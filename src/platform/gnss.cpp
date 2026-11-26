#include "gnss.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>

#if !USE_DESKTOP
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool digits(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) if (c < '0' || c > '9') return false;
    return true;
}

int integer(std::string_view text) {
    int value = 0;
    for (char c : text) value = value * 10 + c - '0';
    return value;
}

bool coordinate(std::string_view value, std::string_view hemisphere,
                int degree_digits, int max_degree, double& result) {
    const auto dot = value.find('.');
    const auto whole = value.substr(0, dot);
    if (whole.size() != static_cast<std::size_t>(degree_digits + 2) ||
        !digits(whole) || (dot != std::string_view::npos && !digits(value.substr(dot + 1))) ||
        value.size() > 20 || hemisphere.size() != 1) return false;
    const int degrees = integer(whole.substr(0, degree_digits));
    double minutes = integer(whole.substr(degree_digits));
    double place = 0.1;
    if (dot != std::string_view::npos) {
        for (char c : value.substr(dot + 1)) {
            minutes += (c - '0') * place;
            place *= 0.1;
        }
    }
    if (minutes >= 60 || degrees > max_degree ||
        (degrees == max_degree && minutes != 0)) return false;
    const char positive = degree_digits == 2 ? 'N' : 'E';
    const char negative = degree_digits == 2 ? 'S' : 'W';
    if (hemisphere[0] != positive && hemisphere[0] != negative) return false;
    result = (degrees + minutes / 60.0) * (hemisphere[0] == negative ? -1 : 1);
    return true;
}

// Days since the Unix epoch, using the proleptic Gregorian calendar.
std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned shifted_month = month > 2 ? month - 3 : month + 9;
    const unsigned doy = (153 * shifted_month + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

bool parse_utc(std::string_view time, std::string_view date,
               std::chrono::system_clock::time_point& result) {
    if (date.size() != 6 || !digits(date) || time.size() < 6 ||
        !digits(time.substr(0, 6)) || time.size() > 16) return false;
    if (time.size() > 6 && (time[6] != '.' || !digits(time.substr(7)))) return false;
    const int hour = integer(time.substr(0, 2));
    const int minute = integer(time.substr(2, 2));
    const int second = integer(time.substr(4, 2));
    const int day = integer(date.substr(0, 2));
    const int month = integer(date.substr(2, 2));
    const int yy = integer(date.substr(4, 2));
    const int year = yy >= 80 ? 1900 + yy : 2000 + yy;
    if (hour > 23 || minute > 59 || second > 59 || month < 1 || month > 12) return false;
    constexpr std::array<int, 12> month_days{{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int limit = month_days[month - 1] + (month == 2 && leap ? 1 : 0);
    if (day < 1 || day > limit) return false;
    int milliseconds = 0;
    if (time.size() > 7) {
        int place = 100;
        for (char c : time.substr(7, 3)) {
            milliseconds += (c - '0') * place;
            place /= 10;
        }
    }
    const auto seconds = days_from_civil(year, static_cast<unsigned>(month),
                                         static_cast<unsigned>(day)) * 86400 +
                         hour * 3600 + minute * 60 + second;
    result = std::chrono::system_clock::time_point{} + std::chrono::seconds(seconds) +
             std::chrono::milliseconds(milliseconds);
    return true;
}

} // namespace

GnssSentenceStatus parse_gnss_sentence(std::string_view sentence, GnssFix& fix) {
    fix = {};
    while (!sentence.empty() && (sentence.back() == '\r' || sentence.back() == '\n'))
        sentence.remove_suffix(1);
    if (sentence.size() < 7 || sentence.size() > 128 || sentence.front() != '$')
        return GnssSentenceStatus::Invalid;
    const auto star = sentence.find('*');
    if (star == std::string_view::npos || star + 3 != sentence.size())
        return GnssSentenceStatus::Invalid;
    const int high = hex_digit(sentence[star + 1]);
    const int low = hex_digit(sentence[star + 2]);
    if (high < 0 || low < 0) return GnssSentenceStatus::Invalid;
    unsigned checksum = 0;
    for (std::size_t i = 1; i < star; ++i) checksum ^= static_cast<unsigned char>(sentence[i]);
    if (checksum != static_cast<unsigned>(high * 16 + low)) return GnssSentenceStatus::Invalid;

    std::array<std::string_view, 20> fields{};
    const auto body = sentence.substr(1, star - 1);
    std::size_t begin = 0, count = 0;
    while (count < fields.size()) {
        const auto end = body.find(',', begin);
        fields[count++] = body.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    if (fields[0].size() != 5 || fields[0].substr(2) != "RMC")
        return GnssSentenceStatus::Other;
    if (count < 10 || (fields[2] != "A" && fields[2] != "V"))
        return GnssSentenceStatus::Invalid;
    if (fields[2] == "V") return GnssSentenceStatus::NoFix;
    if (count > 12 && !fields[12].empty() && fields[12] != "A" && fields[12] != "D" &&
        fields[12] != "F" && fields[12] != "R") return GnssSentenceStatus::NoFix;
    if (!parse_utc(fields[1], fields[9], fix.utc) ||
        !coordinate(fields[3], fields[4], 2, 90, fix.latitude) ||
        !coordinate(fields[5], fields[6], 3, 180, fix.longitude)) {
        fix = {};
        return GnssSentenceStatus::Invalid;
    }
    if (count > 12) fix.mode = std::string(fields[12]);
    return GnssSentenceStatus::Fix;
}

bool gnss_fix_is_fresh(const GnssFix& fix, std::chrono::system_clock::time_point now) {
    const auto age = now - fix.utc;
    return age >= -std::chrono::seconds(2) && age <= std::chrono::seconds(5);
}

GnssReceiver::GnssReceiver(int fd) : fd_(fd) {}

GnssReceiver::~GnssReceiver() {
#if !USE_DESKTOP
    if (fd_ >= 0) {
        ioctl(fd_, TIOCNXCL);
        close(fd_);
    }
#endif
}

std::unique_ptr<GnssReceiver> GnssReceiver::open(const std::string& path, std::string& error) {
    error.clear();
#if USE_DESKTOP
    (void)path;
    error = "GNSS is unavailable in the desktop simulator";
    return nullptr;
#else
    if (path.empty() || path.compare(0, 5, "/dev/") != 0) {
        error = "Set POSEIDON_GPS_DEVICE to the verified GNSS serial device";
        return nullptr;
    }
    struct stat target{};
    if (stat(path.c_str(), &target) != 0 || !S_ISCHR(target.st_mode)) {
        error = "GNSS path is not an accessible character device: " + path;
        return nullptr;
    }
    std::ifstream consoles("/proc/consoles");
    if (!consoles) {
        error = "Cannot check Linux console ownership";
        return nullptr;
    }
    std::string line;
    while (std::getline(consoles, line)) {
        std::istringstream input(line);
        std::string name;
        input >> name;
        struct stat console{};
        if (!name.empty() && stat(("/dev/" + name).c_str(), &console) == 0 &&
            S_ISCHR(console.st_mode) && console.st_rdev == target.st_rdev) {
            error = "GNSS serial device is a system console";
            return nullptr;
        }
    }
    if (consoles.bad()) {
        error = "Cannot read Linux console ownership";
        return nullptr;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "Cannot open GNSS serial device: " + std::string(std::strerror(errno));
        return nullptr;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 || ioctl(fd, TIOCEXCL) != 0) {
        error = "Cannot reserve GNSS serial device: " + std::string(std::strerror(errno));
        close(fd);
        return nullptr;
    }
    termios options{};
    if (tcgetattr(fd, &options) != 0) {
        error = "Cannot read GNSS serial settings: " + std::string(std::strerror(errno));
        ioctl(fd, TIOCNXCL);
        close(fd);
        return nullptr;
    }
    if (cfgetispeed(&options) != B115200 || cfgetospeed(&options) != B115200 ||
        (options.c_cflag & CSIZE) != CS8 ||
        (options.c_cflag & (CSTOPB | PARENB)) != 0 ||
        (options.c_lflag & (ECHO | ECHONL | ICANON | ISIG | IEXTEN)) != 0 ||
        (options.c_iflag & (BRKINT | ICRNL | IGNCR | INLCR | INPCK | ISTRIP |
                            IXON | IXOFF | IXANY)) != 0 ||
        (options.c_oflag & OPOST) != 0) {
        error = "GNSS serial device must already be configured as raw 115200 8N1";
        ioctl(fd, TIOCNXCL);
        close(fd);
        return nullptr;
    }
    return std::unique_ptr<GnssReceiver>(new GnssReceiver(fd));
#endif
}

bool GnssReceiver::poll(GnssFix& fix, bool& has_fix, std::string& error) {
    error.clear();
#if USE_DESKTOP
    (void)fix;
    (void)fd_;
    (void)has_fix_;
    (void)invalid_sentences_;
    has_fix = false;
    error = "GNSS is unavailable in the desktop simulator";
    return false;
#else
    char bytes[512];
    // Bound each poll so a noisy serial device cannot monopolize the worker.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto count = read(fd_, bytes, sizeof(bytes));
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            error = "GNSS read failed: " + std::string(std::strerror(errno));
            has_fix_ = false;
            has_fix = false;
            return false;
        }
        if (count == 0) break;
        for (ssize_t i = 0; i < count; ++i) {
            const char ch = bytes[i];
            if (ch == '$') buffer_.assign(1, ch);
            else if (!buffer_.empty() && buffer_.size() < 128) buffer_.push_back(ch);
            else if (buffer_.size() >= 128) {
                buffer_.clear();
                ++invalid_sentences_;
            }
            if (ch == '\n' && !buffer_.empty()) {
                GnssFix next;
                const auto status = parse_gnss_sentence(buffer_, next);
                if (status == GnssSentenceStatus::Invalid) ++invalid_sentences_;
                else invalid_sentences_ = 0;
                if (status == GnssSentenceStatus::Fix) {
                    fix_ = next;
                    has_fix_ = true;
                } else if (status == GnssSentenceStatus::NoFix) {
                    has_fix_ = false;
                }
                buffer_.clear();
            }
            if (invalid_sentences_ >= 20) {
                error = "GNSS input contains repeated invalid NMEA sentences";
                has_fix_ = false;
                has_fix = false;
                return false;
            }
        }
    }
    has_fix = has_fix_ && gnss_fix_is_fresh(fix_, std::chrono::system_clock::now());
    if (has_fix) fix = fix_;
    return true;
#endif
}

} // namespace platform
