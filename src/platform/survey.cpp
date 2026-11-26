#include "survey.h"

#include "bluetooth_gatt.h"
#include "survey_file.h"
#include "wifi_scan.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

constexpr std::size_t kScanResultLimit = 40;
constexpr std::array<const char*, 4> kCameraNameHints{
    "camera", "flock", "bodycam", "body-cam"};

std::string lower_ascii(std::string name) {
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return name;
}

bool camera_name_hint(const std::string& value) {
    const auto name = lower_ascii(value);
    return std::any_of(kCameraNameHints.begin(), kCameraNameHints.end(),
                       [&name](const char* word) { return name.find(word) != std::string::npos; });
}

std::vector<std::string> load_user_indicators(const std::filesystem::path& directory,
                                              std::string& error) {
    std::vector<std::string> result;
    const auto path = directory / "name-indicators.txt";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return result;
    if (ec || !std::filesystem::is_regular_file(status)) {
        error = ec ? "Indicator file: " + ec.message() :
                     "Indicator file must be a regular file, not a link";
        return result;
    }
    std::string contents;
#if defined(_WIN32)
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Cannot open indicator file";
        return result;
    }
    char ch;
    while (input.get(ch)) {
        if (contents.size() == 4096) {
            error = "Indicator file exceeds 4096 bytes";
            return {};
        }
        contents += ch;
    }
    if (!input.eof()) {
        error = "Cannot read indicator file";
        return {};
    }
#else
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        error = std::string("Cannot open indicator file: ") + std::strerror(errno);
        return {};
    }
    struct stat statbuf{};
    if (fstat(fd, &statbuf) != 0 || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size < 0 || statbuf.st_size > 4096) {
        close(fd);
        error = "Indicator file must be a regular file of at most 4096 bytes";
        return {};
    }
    std::array<char, 4097> buffer{};
    for (;;) {
        const auto count = read(fd, buffer.data(), buffer.size() - contents.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            error = std::string("Cannot read indicator file: ") + std::strerror(errno);
            break;
        }
        if (count == 0) break;
        contents.append(buffer.data(), static_cast<std::size_t>(count));
        if (contents.size() > 4096) {
            error = "Indicator file exceeds 4096 bytes";
            break;
        }
    }
    if (close(fd) != 0 && error.empty())
        error = std::string("Cannot close indicator file: ") + std::strerror(errno);
    if (!error.empty()) return {};
#endif
    std::size_t start = 0;
    while (start < contents.size()) {
        const auto end = contents.find('\n', start);
        std::string line = contents.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() != '#') {
            if (line.size() < 3 || line.size() > 32 || result.size() == 32) {
                error = "Indicator file: use 3-32 characters, up to 32 patterns";
                return {};
            }
            for (char& c : line) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_')) {
                    error = "Indicator file: only letters, digits, - and _ are allowed";
                    return {};
                }
            }
            if (std::find(result.begin(), result.end(), line) != result.end()) {
                error = "Indicator file: duplicate pattern " + line;
                return {};
            }
            result.push_back(std::move(line));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}

std::string name_indicators(std::string name, const std::vector<std::string>& user_indicators) {
    name = lower_ascii(std::move(name));
    std::string result;
    const auto add_word = [&name, &result](const char* word) {
        if (name.find(word) == std::string::npos) return;
        if (!result.empty()) result += ';';
        result += "name:";
        result += word;
    };
    for (const char* word : kCameraNameHints) add_word(word);
    for (const char* word : {"drone", "tracker", "airtag",
                             "smarttag", "skimmer", "flipper", "cardputer"}) {
        add_word(word);
    }
    for (const auto& word : user_indicators) {
        if (name.find(word) == std::string::npos) continue;
        if (!result.empty()) result += ';';
        result += "user-name:" + word;
    }
    return result;
}

class JoiningThread {
public:
    JoiningThread(std::atomic_bool& stop, std::thread& thread) : stop_(stop), thread_(thread) {}
    ~JoiningThread() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
private:
    std::atomic_bool& stop_;
    std::thread& thread_;
};

} // namespace

SurveillanceScanResult scan_surveillance_hints(const std::atomic_bool& cancelled) {
    SurveillanceScanResult result;
    const auto wifi = scan_wifi(cancelled);
    if (!wifi.error.empty()) {
        result.error = "Wi-Fi: " + wifi.error;
        return result;
    }
    if (cancelled) return result;
    if (wifi.scan_started_boottime < 0) {
        result.error = "Wi-Fi scan time unavailable";
        return result;
    }
    result.results_may_be_truncated = wifi.networks.size() >= kScanResultLimit;
    for (const auto& network : wifi.networks) {
        if (!wifi_seen_during_scan(wifi, network)) {
            ++result.stale_wifi;
        } else if (camera_name_hint(network.name)) {
            result.hints.push_back({"Wi-Fi", network.name, network.address});
        }
    }
    if (cancelled) return result;
    const auto bluetooth = scan_ble_devices(cancelled);
    if (!bluetooth.error.empty()) {
        result.hints.clear();
        result.error = "BLE: " + bluetooth.error;
        return result;
    }
    if (cancelled) return result;
    result.results_may_be_truncated |= bluetooth.devices.size() >= kScanResultLimit;
    for (const auto& device : bluetooth.devices) {
        if (camera_name_hint(device.name))
            result.hints.push_back({"BLE", device.name, device.address});
    }
    return result;
}

std::unique_ptr<SurveySession> SurveySession::start(std::filesystem::path directory,
                                                    std::string gps_device,
                                                    std::string& error) {
    error.clear();
#if USE_DESKTOP
    (void)directory;
    (void)gps_device;
    error = "Wireless survey is unavailable in the desktop simulator";
    return nullptr;
#else
    auto session = std::unique_ptr<SurveySession>(new SurveySession);
    try {
        session->thread_ = std::thread(&SurveySession::run, session.get(),
                                       std::move(directory), std::move(gps_device));
    } catch (const std::system_error& failure) {
        error = "Cannot start survey: " + failure.code().message();
        return nullptr;
    }
    return session;
#endif
}

SurveySession::~SurveySession() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void SurveySession::stop() { cancelled_ = true; }

SurveyStatus SurveySession::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void SurveySession::run(std::filesystem::path directory, std::string gps_device) {
    std::string error;
    std::unique_ptr<SurveyFile> file;
    std::atomic_bool gps_stop{false};
    std::thread gps_thread;
    JoiningThread join_gps(gps_stop, gps_thread);
    try {
        file = SurveyFile::create(directory, error);
        std::vector<std::string> user_indicators;
        if (file) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.file_name = file->path().filename().string();
        }
        if (file) user_indicators = load_user_indicators(directory, error);
        if (file && error.empty() && !gps_device.empty()) {
            gps_thread = std::thread([this, &gps_stop, gps_device] {
                try {
                    std::string gps_error;
                    auto receiver = GnssReceiver::open(gps_device, gps_error);
                    if (!receiver) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        status_.gps_status = gps_error;
                        return;
                    }
                    while (!gps_stop) {
                        GnssFix fix;
                        bool available = false;
                        if (!receiver->poll(fix, available, gps_error)) {
                            std::lock_guard<std::mutex> lock(mutex_);
                            status_.gps_status = gps_error;
                            has_fix_ = false;
                            return;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            has_fix_ = available;
                            if (available) fix_ = fix;
                            status_.gps_status = available ? "GPS fix" : "GPS no current fix";
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    }
                } catch (const std::exception& failure) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    has_fix_ = false;
                    status_.gps_status = std::string("GPS failed: ") + failure.what();
                }
            });
        } else if (file && error.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.gps_status = "GPS device not configured";
        }
        auto record = [this, &file, &error](SurveyObservation observation) {
            observation.recorded_at = std::chrono::system_clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (has_fix_ && gnss_fix_is_fresh(fix_, observation.recorded_at))
                    observation.location = fix_;
            }
            if (!file->append(observation, error)) return false;
            std::lock_guard<std::mutex> lock(mutex_);
            if (observation.location) ++status_.gps_tagged;
            else ++status_.gps_untagged;
            if (!observation.indicators.empty()) ++status_.indicator_count;
            if (survey_watchlist_match(observation.indicators))
                ++status_.watchlist_rows;
            return true;
        };
        while (file && error.empty() && !cancelled_) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.phase = "Scanning Wi-Fi";
            }
            const auto wifi = scan_wifi(cancelled_);
            if (cancelled_) break;
            if (!wifi.error.empty()) {
                error = "Wi-Fi: " + wifi.error;
                break;
            }
            if (wifi.scan_started_boottime < 0) {
                error = "Wi-Fi scan time unavailable; cannot exclude stale results";
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.results_may_be_truncated |= wifi.networks.size() >= kScanResultLimit;
            }
            for (const auto& network : wifi.networks) {
                if (cancelled_) break;
                if (!wifi_seen_during_scan(wifi, network)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++status_.wifi_skipped;
                    continue;
                }
                SurveyObservation observation;
                observation.source = "wifi";
                observation.name = network.name;
                observation.address = network.address;
                observation.signal = std::to_string(network.strength);
                observation.signal_unit = "percent";
                if (network.frequency_mhz != 0)
                    observation.frequency_mhz = std::to_string(network.frequency_mhz);
                const int channel = wifi_channel(network.frequency_mhz);
                if (channel >= 0) observation.channel = std::to_string(channel);
                observation.security = network.secured ? "secured" : "open";
                observation.freshness = "seen_during_scan";
                observation.scan_may_be_limited = wifi.networks.size() >= kScanResultLimit;
                observation.indicators = name_indicators(network.name, user_indicators);
                if (!record(std::move(observation))) break;
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.wifi_count;
            }
            if (!error.empty() || cancelled_) break;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.phase = "Scanning BLE";
            }
            const auto ble = scan_ble_devices(cancelled_);
            if (cancelled_) break;
            if (!ble.error.empty()) {
                error = "BLE: " + ble.error;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.results_may_be_truncated |= ble.devices.size() >= kScanResultLimit;
            }
            for (const auto& device : ble.devices) {
                if (cancelled_) break;
                SurveyObservation observation;
                observation.source = "ble";
                observation.name = device.name;
                observation.address = device.address;
                observation.freshness = "discovery_result";
                observation.scan_may_be_limited = ble.devices.size() >= kScanResultLimit;
                observation.indicators = name_indicators(device.name, user_indicators);
                if (device.remote_id_indicator) {
                    if (!observation.indicators.empty()) observation.indicators += ';';
                    observation.indicators += "ble:remote_id_fffa";
                    observation.remote_id = device.remote_id;
                }
                if (device.tracker_indicator) {
                    if (!observation.indicators.empty()) observation.indicators += ';';
                    observation.indicators += "ble:tracker_fcb2";
                }
                if (!record(std::move(observation))) break;
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.ble_count;
            }
        }
    } catch (const std::exception& failure) {
        error = std::string("Survey failed: ") + failure.what();
    }
    if (file) {
        std::string close_error;
        if (!file->close(close_error)) {
            if (!error.empty()) error += "; ";
            error += close_error;
        }
    }
    gps_stop = true;
    if (gps_thread.joinable()) gps_thread.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.error = error;
        status_.phase = error.empty() ? "Stopped" : "Error";
        status_.complete = true;
    }
}

} // namespace platform
