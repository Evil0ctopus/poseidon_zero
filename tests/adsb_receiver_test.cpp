#include "adsb_receiver.h"
#include "evidence_export.h"

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace {
std::string checksum(const std::string& body) {
    constexpr char digits[] = "0123456789ABCDEF";
    unsigned value = 0;
    for (unsigned char c : body) value ^= c;
    return "$" + body + "*" + digits[value >> 4] + digits[value & 15] + "\r\n";
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    std::string error;
    auto receiver = platform::AdsbReceiver::start_fixture(argv[1], error);
    if (!receiver) return 1;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    platform::AdsbReceiverStatus status;
    do {
        status = receiver->status();
        if (status.valid_frames && status.rejected_frames) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.running || status.valid_frames != 2 || status.rejected_frames != 1 ||
        status.latest.aircraft_address != "40621D" || status.latest.altitude_feet != 38000 ||
        !status.latest.position_available || std::abs(status.latest.latitude - 52.26578) > 0.0001 ||
        std::abs(status.latest.longitude - 3.93891) > 0.0001) {
        std::cerr << "Fixture frames were not handled correctly: " << status.error << '\n';
        return 1;
    }
    receiver->stop();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.complete || !status.error.empty() || !status.log_filename.empty()) {
        std::cerr << "Receiver did not stop cleanly: " << status.error << '\n';
        return 1;
    }
    receiver.reset();
    setenv("POSEIDON_ADSB_TEST_MODE", "tracks", 1);
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.valid_frames == 34 && status.rejected_frames == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    unsetenv("POSEIDON_ADSB_TEST_MODE");
    if (status.valid_frames != 34 || status.rejected_frames != 1 ||
        status.recent_aircraft.size() != platform::kAdsbRecentAircraftLimit ||
        status.recent_aircraft.front().aircraft_address != "000002" ||
        status.recent_aircraft[1].aircraft_address != "000021" ||
        status.recent_aircraft.back().aircraft_address != "000003" ||
        status.latest.aircraft_address != "000002" || !status.log_filename.empty()) {
        std::cerr << "Recent aircraft ordering, eviction or rejection failed: " << status.error << '\n';
        return 1;
    }
    receiver->stop();
    receiver.reset();
    const auto home = std::filesystem::temp_directory_path() /
        ("poseidon-adsb-fixture-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!std::filesystem::create_directory(home)) return 1;
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error, home);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.valid_frames == 2 && status.rejected_frames == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    receiver->stop();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.complete || !status.error.empty() || status.log_filename.empty()) {
        std::cerr << "ADS-B log session failed: " << status.error << '\n';
        return 1;
    }
    receiver.reset();
    const auto directory = home / "poseidon-adsb";
    std::ifstream log(directory / status.log_filename, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    log.close();
    const auto logs = platform::list_security_logs(home);
    if (contents.find("receipt_unix_ms,aircraft_address,callsign,type,altitude_ft,") != 0 ||
        contents.find("40621D") == std::string::npos ||
        contents.find("52.265780") == std::string::npos ||
        contents.find("*8D") != std::string::npos ||
        std::count(contents.begin(), contents.end(), '\n') != 3 ||
        !logs.error.empty() || logs.entries.size() != 1 ||
        logs.entries[0].source != platform::EvidenceSource::Adsb ||
        logs.entries[0].filename != status.log_filename ||
        !platform::delete_security_log(directory, status.log_filename,
                                       platform::EvidenceSource::Adsb).empty() ||
        std::filesystem::exists(directory / status.log_filename)) {
        std::cerr << "ADS-B log content, listing or deletion failed\n";
        return 1;
    }
    setenv("POSEIDON_ADSB_TEST_MODE", "flood", 1);
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error, home);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    unsetenv("POSEIDON_ADSB_TEST_MODE");
    if (!status.complete || status.error.find("log limit reached") == std::string::npos ||
        status.log_filename.empty() ||
        std::filesystem::file_size(directory / status.log_filename) > 1024 * 1024) {
        std::cerr << "ADS-B log limit was not enforced: " << status.error << '\n';
        return 1;
    }
    receiver.reset();
    if (!platform::delete_security_log(directory, status.log_filename,
                                       platform::EvidenceSource::Adsb).empty()) return 1;
    for (bool stale : {false, true}) {
        int master = -1, slave = -1;
        char device[128]{};
        if (openpty(&master, &slave, device, nullptr, nullptr) != 0) return 1;
        termios options{};
        if (tcgetattr(slave, &options) != 0) return 1;
        cfmakeraw(&options);
        cfsetispeed(&options, B115200);
        cfsetospeed(&options, B115200);
        if (tcsetattr(slave, TCSANOW, &options) != 0) return 1;
        close(slave);
        setenv("POSEIDON_ADSB_TEST_MODE", "gps", 1);
        receiver = platform::AdsbReceiver::start_fixture(argv[1], error, home, device);
        if (!receiver) return 1;
        until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            status = receiver->status();
            if (status.running || status.complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < until);
        if (!status.running) {
            std::cerr << "GPS fixture failed to start: " << status.error << '\n';
            return 1;
        }
        const auto now = std::time(nullptr) - (stale ? 20 : 0);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char date_time[32]{};
        std::strftime(date_time, sizeof(date_time), "%H%M%S,%d%m%y", &utc);
        const std::string time_and_date(date_time);
        const auto separator = time_and_date.find(',');
        const auto sentence = checksum("GNRMC," + time_and_date.substr(0, separator) +
            ",A,3853.8000,N,09217.4000,W,0,0," + time_and_date.substr(separator + 1) + ",,,A");
        if (write(master, sentence.data(), sentence.size()) != static_cast<ssize_t>(sentence.size())) return 1;
        until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            status = receiver->status();
            if (status.valid_frames || status.complete) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < until);
        receiver->stop();
        receiver.reset();
        close(master);
        unsetenv("POSEIDON_ADSB_TEST_MODE");
        std::ifstream gps_log(directory / status.log_filename, std::ios::binary);
        const std::string gps_contents{std::istreambuf_iterator<char>(gps_log), {}};
        gps_log.close();
        const bool coordinates_present =
            gps_contents.find(",38.8966667,-92.2900000,") != std::string::npos;
        if (status.valid_frames != 1 ||
            status.gps_status != (stale ? "GPS no current fix" : "GPS fix") ||
            coordinates_present == stale ||
            !platform::delete_security_log(directory, status.log_filename,
                                           platform::EvidenceSource::Adsb).empty()) {
            std::cerr << "ADS-B receiver GPS tagging failed: " << status.error << '\n';
            return 1;
        }
    }
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error, home, "/dev/does-not-exist");
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.complete || status.error.find("ADS-B GPS:") == std::string::npos ||
        !status.log_filename.empty() || !platform::list_security_logs(home).entries.empty()) return 1;
    receiver.reset();
    receiver = platform::AdsbReceiver::start_fixture("/nonexistent/rtl_adsb", error, home);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.complete || status.error.find("Cannot start rtl_adsb") == std::string::npos ||
        !status.log_filename.empty() || !platform::list_security_logs(home).entries.empty()) {
        std::cerr << "Failed receiver left an ADS-B log\n";
        return 1;
    }
    receiver.reset();
    if (platform::AdsbReceiver::start_fixture(argv[1], error, "relative-home") || error.empty())
        return 1;
    if (platform::AdsbReceiver::start_fixture(argv[1], error, {}, "/dev/null") || error.empty())
        return 1;
    std::filesystem::remove(directory);
    std::filesystem::remove(home);
    for (const char* mode : {"emergency", "category"}) {
        setenv("POSEIDON_ADSB_TEST_MODE", mode, 1);
        receiver = platform::AdsbReceiver::start_fixture(argv[1], error);
        if (!receiver) return 1;
        until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            status = receiver->status();
            if (status.valid_frames) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < until);
        const bool emergency = std::string(mode) == "emergency";
        if (status.valid_frames != 1 || !status.latest.error.empty() ||
            (emergency && (!status.latest.emergency_status_available ||
                           status.latest.emergency_status != 1 || status.latest.squawk != "7700")) ||
            (!emergency && (status.latest.type_code != 3 || status.latest.emitter_category != 6))) {
            std::cerr << "Receiver did not expose " << mode << " frame correctly\n";
            return 1;
        }
        receiver->stop();
        receiver.reset();
    }
    unsetenv("POSEIDON_ADSB_TEST_MODE");
    setenv("POSEIDON_ADSB_TEST_MODE", "stubborn", 1);
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.valid_frames) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.valid_frames) return 1;
    receiver->stop();
    until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    unsetenv("POSEIDON_ADSB_TEST_MODE");
    if (!status.complete || !status.error.empty()) {
        std::cerr << "Stubborn receiver was not terminated: " << status.error << '\n';
        return 1;
    }
    receiver.reset();
    setenv("POSEIDON_ADSB_TEST_MODE", "exit", 1);
    receiver = platform::AdsbReceiver::start_fixture(argv[1], error);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    unsetenv("POSEIDON_ADSB_TEST_MODE");
    if (!status.complete || status.error.find("fixture unavailable") == std::string::npos) {
        std::cerr << "Unexpected exit was not reported: " << status.error << '\n';
        return 1;
    }
    receiver.reset();
    receiver = platform::AdsbReceiver::start_fixture("/nonexistent/rtl_adsb", error);
    if (!receiver) return 1;
    until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
        status = receiver->status();
        if (status.complete) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    if (!status.complete || status.error.find("Cannot start rtl_adsb") == std::string::npos) return 1;
    return 0;
}
