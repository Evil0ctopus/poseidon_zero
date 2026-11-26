#include "esp32_monitor_serial.h"

#include <charconv>
#include <cstring>

#if !USE_DESKTOP
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace platform {

bool parse_esp32_monitor_event(const std::string& line, WirelessEventKind kind,
                               WirelessEvent& event, bool hopping) {
    std::string payload = line;
    unsigned channel = 0;
    if (hopping) {
        const auto separator = payload.rfind(' ');
        if (separator == std::string::npos) return false;
        const auto value = payload.substr(separator + 1);
        const auto result = std::from_chars(value.data(), value.data() + value.size(), channel);
        if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            (channel != 1 && channel != 6 && channel != 11)) return false;
        payload.resize(separator);
    }
    if (kind == WirelessEventKind::ProbeRequest) {
        if (payload != "P") return false;
        event = {};
        event.channel = static_cast<uint8_t>(channel);
        return true;
    }
    if (payload.size() < 21 || payload.compare(0, 2, "D ") != 0 || payload[19] != ' ') return false;
    for (std::size_t i = 0; i < 17; ++i) {
        const char c = payload[2 + i];
        if (i % 3 == 2) {
            if (c != ':') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    WirelessEvent parsed;
    parsed.bssid = payload.substr(2, 17);
    parsed.channel = static_cast<uint8_t>(channel);
    const auto detail = payload.substr(20);
    if (detail == "protected") {
        parsed.protected_frame = true;
    } else {
        unsigned value = 0;
        const auto result = std::from_chars(detail.data(), detail.data() + detail.size(), value);
        if (detail.empty() || result.ec != std::errc{} ||
            result.ptr != detail.data() + detail.size() || value > 65535) return false;
        parsed.reason = static_cast<uint16_t>(value);
    }
    event = std::move(parsed);
    return true;
}

#if !USE_DESKTOP
struct Esp32MonitorSerial::SerialSettings {
    termios previous{};
};
#endif

Esp32MonitorSerial::Esp32MonitorSerial(std::atomic_bool& cancelled, WirelessEventKind kind)
    : cancelled_(cancelled), kind_(kind) {}

std::unique_ptr<Esp32MonitorSerial> Esp32MonitorSerial::open(
    const std::string& path, std::atomic_bool& cancelled, WirelessEventKind kind,
    std::string& error, unsigned channel) {
    error.clear();
    if (channel > 11) {
        error = "ESP32 channel must be 1-11 or hop";
        return nullptr;
    }
#if USE_DESKTOP
    (void)path;
    (void)cancelled;
    (void)kind;
    (void)channel;
    error = "ESP32 monitor unavailable in desktop simulator";
    return nullptr;
#else
    if (path.compare(0, 5, "/dev/") != 0) {
        error = "Set POSEIDON_ESP32_MONITOR_DEVICE to the verified USB serial device";
        return nullptr;
    }
    std::error_code canonical_error;
    const auto canonical = std::filesystem::canonical(path, canonical_error);
    const auto name = canonical.filename().string();
    const auto numbered = [&name](const std::string& prefix) {
        if (name.compare(0, prefix.size(), prefix) != 0 || name.size() == prefix.size())
            return false;
        for (std::size_t i = prefix.size(); i < name.size(); ++i)
            if (name[i] < '0' || name[i] > '9') return false;
        return true;
    };
    bool valid_path = !canonical_error && canonical.parent_path() == "/dev" &&
                      (numbered("ttyUSB") || numbered("ttyACM"));
#if defined(POSEIDON_TEST_ESP32)
    valid_path = valid_path || (!canonical_error && canonical.parent_path() == "/dev/pts");
#endif
    struct stat target{};
    if (!valid_path || stat(canonical.c_str(), &target) != 0 || !S_ISCHR(target.st_mode)) {
        error = "ESP32 path must resolve to a USB serial device";
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
        std::string console_name;
        input >> console_name;
        struct stat console{};
        if (!console_name.empty() && stat(("/dev/" + console_name).c_str(), &console) == 0 &&
            S_ISCHR(console.st_mode) && console.st_rdev == target.st_rdev) {
            error = "ESP32 serial device is a system console";
            return nullptr;
        }
    }
    if (consoles.bad()) {
        error = "Cannot read Linux console ownership";
        return nullptr;
    }
    auto serial = std::unique_ptr<Esp32MonitorSerial>(new Esp32MonitorSerial(cancelled, kind));
    serial->channel_ = channel;
    serial->fd_ = ::open(canonical.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (serial->fd_ < 0) {
        error = "Cannot open ESP32 serial: " + std::string(std::strerror(errno));
        return nullptr;
    }
    struct stat opened{};
    if (fstat(serial->fd_, &opened) != 0 || !S_ISCHR(opened.st_mode) ||
        opened.st_rdev != target.st_rdev) {
        error = "ESP32 serial device changed while opening";
        return nullptr;
    }
    if (flock(serial->fd_, LOCK_EX | LOCK_NB) != 0 || ioctl(serial->fd_, TIOCEXCL) != 0) {
        error = "Cannot reserve ESP32 serial: " + std::string(std::strerror(errno));
        return nullptr;
    }
    serial->exclusive_ = true;
    serial->settings_ = std::make_unique<SerialSettings>();
    if (tcgetattr(serial->fd_, &serial->settings_->previous) != 0) {
        error = "Cannot read ESP32 serial settings: " + std::string(std::strerror(errno));
        serial->settings_.reset();
        return nullptr;
    }
    termios options = serial->settings_->previous;
    cfmakeraw(&options);
    options.c_cflag |= CLOCAL | CREAD;
    options.c_cflag &= ~(CSTOPB | PARENB | CSIZE | CRTSCTS);
    options.c_cflag |= CS8;
    if (cfsetispeed(&options, B115200) != 0 || cfsetospeed(&options, B115200) != 0 ||
        tcsetattr(serial->fd_, TCSANOW, &options) != 0) {
        error = "Cannot configure ESP32 serial: " + std::string(std::strerror(errno));
        return nullptr;
    }
    if (!serial->send("HELLO\n", error) ||
        !serial->wait_for("POSEIDON-C5 1", 6000, true, error)) return nullptr;
    if (cancelled) {
        error = "Monitor cancelled";
        return nullptr;
    }
    serial->started_ = true;
    std::string start = kind == WirelessEventKind::Deauthentication ? "START D" : "START P";
    if (channel != 1) start += channel == 0 ? " H" : " " + std::to_string(channel);
    start += '\n';
    const std::string expected = channel == 0 ? "ACTIVE H" : "ACTIVE " + std::to_string(channel);
    if (!serial->send(start.c_str(), error) ||
        !serial->wait_for(expected.c_str(), 5000, false, error)) {
        std::string stop_error;
        if (!serial->stop(stop_error)) error += "; " + stop_error;
        return nullptr;
    }
    serial->last_ping_ = serial->last_pong_ = std::chrono::steady_clock::now();
    return serial;
#endif
}

Esp32MonitorSerial::~Esp32MonitorSerial() {
#if !USE_DESKTOP
    if (fd_ >= 0) {
        if (settings_) tcsetattr(fd_, TCSANOW, &settings_->previous);
        if (exclusive_) ioctl(fd_, TIOCNXCL);
        close(fd_);
    }
#endif
}

bool Esp32MonitorSerial::send(const char* command, std::string& error) {
#if USE_DESKTOP
    (void)command;
    error = "ESP32 monitor unavailable in desktop simulator";
    return false;
#else
    std::size_t offset = 0;
    const std::size_t length = std::strlen(command);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (offset < length && std::chrono::steady_clock::now() < deadline) {
        const auto n = write(fd_, command + offset, length - offset);
        if (n > 0) offset += static_cast<std::size_t>(n);
        else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            error = "ESP32 serial write: " + std::string(std::strerror(errno));
            return false;
        } else {
            pollfd ready{fd_, POLLOUT, 0};
            const int result = poll(&ready, 1, 100);
            if (result < 0 && errno != EINTR) {
                error = "ESP32 serial poll: " + std::string(std::strerror(errno));
                return false;
            }
        }
    }
    if (offset != length) {
        error = "ESP32 serial write timed out";
        return false;
    }
    return true;
#endif
}

bool Esp32MonitorSerial::read_line(std::string& line, int timeout_ms, std::string& error) {
    line.clear();
#if USE_DESKTOP
    (void)timeout_ms;
    error = "ESP32 monitor unavailable in desktop simulator";
    return false;
#else
    const auto end = buffer_.find('\n');
    if (end != std::string::npos) {
        line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 96) {
            error = "ESP32 sent an oversized line";
            return false;
        }
        return true;
    }
    if (buffer_.size() > 96) {
        error = "ESP32 sent an oversized line";
        return false;
    }
    pollfd ready{fd_, POLLIN, 0};
    const int result = poll(&ready, 1, timeout_ms);
    if (result < 0) {
        if (errno == EINTR) return true;
        error = "ESP32 serial poll: " + std::string(std::strerror(errno));
        return false;
    }
    if (result == 0) return true;
    if (ready.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        error = "ESP32 serial disconnected";
        return false;
    }
    char bytes[256];
    const auto n = read(fd_, bytes, sizeof(bytes));
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return true;
        error = "ESP32 serial read: " + std::string(std::strerror(errno));
        return false;
    }
    if (n == 0) {
        error = "ESP32 serial disconnected";
        return false;
    }
    buffer_.append(bytes, static_cast<std::size_t>(n));
    return read_line(line, 0, error);
#endif
}

bool Esp32MonitorSerial::wait_for(const char* expected, int timeout_ms, bool retry_hello,
                                 std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto next_hello = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::string loss;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled_ && std::strcmp(expected, "STOPPED") != 0) {
            error = "Monitor cancelled";
            return false;
        }
        if (retry_hello && std::chrono::steady_clock::now() >= next_hello) {
            if (!send("HELLO\n", error)) return false;
            next_hello = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
        std::string line;
        if (!read_line(line, 200, error)) return false;
        if (line == expected) {
            if (!loss.empty()) {
                error = loss;
                return false;
            }
            return true;
        }
        if (line.compare(0, 5, "DROP ") == 0) loss = "ESP32 dropped events: " + line.substr(5);
        if (line.compare(0, 6, "ERROR ") == 0) {
            error = "ESP32 " + line;
            return false;
        }
    }
    error = std::string("ESP32 did not respond: ") + expected;
    return false;
}

bool Esp32MonitorSerial::read_event(WirelessEvent& event, bool& received, std::string& error) {
    received = false;
    if (std::chrono::steady_clock::now() - last_pong_ >= std::chrono::seconds(4)) {
        error = "ESP32 heartbeat timed out";
        return false;
    }
    if (std::chrono::steady_clock::now() - last_ping_ >= std::chrono::seconds(1)) {
        if (!send("PING\n", error)) return false;
        last_ping_ = std::chrono::steady_clock::now();
    }
    std::string line;
    if (!read_line(line, 200, error)) return false;
    if (line == "PONG") {
        last_pong_ = std::chrono::steady_clock::now();
        return true;
    }
    if (line.empty() || line == "POSEIDON-C5 1") return true;
    if (line.compare(0, 5, "DROP ") == 0) {
        error = "ESP32 dropped events: " + line.substr(5);
        return false;
    }
    if (line.compare(0, 6, "ERROR ") == 0 || line.compare(0, 7, "STOPPED") == 0) {
        error = "ESP32 " + line;
        return false;
    }
    if (!parse_esp32_monitor_event(line, kind_, event, channel_ == 0)) {
        error = "Invalid ESP32 monitor record";
        return false;
    }
    if (channel_ != 0) event.channel = static_cast<uint8_t>(channel_);
    received = true;
    return true;
}

bool Esp32MonitorSerial::stop(std::string& error) {
    bool ok = true;
    if (started_) {
        started_ = false;
        if (!send("STOP\n", error) || !wait_for("STOPPED", 2000, false, error)) {
            error += " (receiver has a heartbeat timeout)";
            ok = false;
        }
    }
#if !USE_DESKTOP
    const auto report = [&](const char* operation) {
        if (!error.empty()) error += "; ";
        error += std::string(operation) + ": " + std::strerror(errno);
        ok = false;
    };
    if (fd_ >= 0) {
        if (settings_ && tcsetattr(fd_, TCSANOW, &settings_->previous) != 0)
            report("Restore serial settings");
        settings_.reset();
        if (exclusive_ && ioctl(fd_, TIOCNXCL) != 0) report("Release serial device");
        exclusive_ = false;
        if (close(fd_) != 0) report("Close serial device");
        fd_ = -1;
    }
#endif
    return ok;
}

} // namespace platform
