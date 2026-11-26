#include "gnss.h"

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::string checksum(const std::string& body) {
    constexpr char digits[] = "0123456789ABCDEF";
    unsigned value = 0;
    for (unsigned char c : body) value ^= c;
    return "$" + body + "*" + digits[value >> 4] + digits[value & 15] + "\r\n";
}

bool send_sentence(int fd, const std::string& sentence) {
    return write(fd, sentence.data(), sentence.size()) == static_cast<ssize_t>(sentence.size());
}

} // namespace

int main() {
    int master = -1;
    int slave = -1;
    char name[128]{};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0) {
        std::cerr << "Cannot create test pseudo-terminal\n";
        return 1;
    }
    termios options{};
    if (tcgetattr(slave, &options) != 0) return 1;
    cfmakeraw(&options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);
    options.c_lflag |= ECHO;
    if (tcsetattr(slave, TCSANOW, &options) != 0) return 1;
    std::string error;
    auto invalid_receiver = platform::GnssReceiver::open(name, error);
    if (invalid_receiver || error.empty()) {
        std::cerr << "Serial echo was not rejected\n";
        return 1;
    }
    options.c_lflag &= ~ECHO;
    if (tcsetattr(slave, TCSANOW, &options) != 0) return 1;
    close(slave);

    auto receiver = platform::GnssReceiver::open(name, error);
    if (!receiver) {
        std::cerr << "Cannot open test GNSS: " << error << '\n';
        close(master);
        return 1;
    }
    const auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char date_time[32]{};
    std::strftime(date_time, sizeof(date_time), "%H%M%S,%d%m%y", &utc);
    const std::string time_and_date(date_time);
    const auto separator = time_and_date.find(',');
    const std::string body = "GNRMC," + time_and_date.substr(0, separator) +
        ",A,3853.8000,N,09217.4000,W,0,0," + time_and_date.substr(separator + 1) + ",,,A";
    bool ok = send_sentence(master, checksum(body));
    platform::GnssFix fix;
    bool has_fix = false;
    for (int attempt = 0; ok && !has_fix && attempt < 20; ++attempt) {
        ok = receiver->poll(fix, has_fix, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ok || !has_fix || fix.mode != "A") {
        std::cerr << "Valid serial fix not received: " << error << '\n';
        ok = false;
    }
    const auto no_fix = checksum("GNRMC,000000,V,,,,,,,010126");
    bool no_fix_received = false;
    if (send_sentence(master, no_fix)) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (!receiver->poll(fix, has_fix, error)) break;
            if (!has_fix) {
                no_fix_received = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!no_fix_received) {
        std::cerr << "No-fix sentence did not clear the fix\n";
        ok = false;
    }
    std::string corrupt;
    for (int i = 0; i < 20; ++i) corrupt += "$GNRMC,broken*XX\r\n";
    bool invalid_reported = false;
    if (send_sentence(master, corrupt)) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (!receiver->poll(fix, has_fix, error)) {
                invalid_reported = !error.empty();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!invalid_reported) {
        std::cerr << "Repeated malformed input was not reported\n";
        ok = false;
    }
    receiver.reset();
    close(master);
    return ok ? 0 : 1;
}
