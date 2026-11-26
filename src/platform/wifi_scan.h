#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace platform {

struct WifiScanNetwork {
    std::string name;
    std::string address;
    int strength{-1};
    bool secured{false};
    uint32_t frequency_mhz{0};
    std::string device_path;
    std::string access_point_path;
    uint32_t wpa_flags{0};
    uint32_t rsn_flags{0};
    int32_t last_seen_boottime{-1};
};

struct WifiScanResult {
    std::vector<WifiScanNetwork> networks;
    std::string error;
    int64_t scan_started_boottime{-1};
};

struct WifiChannelCount {
    std::string band;
    int channel{0};
    std::size_t access_points{0};
};

struct WifiMonitorComparison {
    std::size_t baseline_count{0};
    std::size_t current_count{0};
    std::size_t new_addresses{0};
    std::size_t changed{0};
    std::size_t not_seen{0};
    std::size_t skipped{0};
    std::vector<std::string> details;
    std::string error;
};

enum class WifiConnectMode { Saved, New };
enum class WifiSecurity { Open, WpaPsk, Sae, Unsupported };

WifiScanResult scan_wifi(const std::atomic_bool& cancelled);
int wifi_channel(uint32_t frequency_mhz);
bool wifi_seen_during_scan(const WifiScanResult& result, const WifiScanNetwork& network);
std::vector<WifiChannelCount> summarize_wifi_channels(
    const std::vector<WifiScanNetwork>& networks);
WifiMonitorComparison compare_wifi_observations(const WifiScanResult& baseline,
                                                const WifiScanResult& current);
WifiSecurity wifi_security(const WifiScanNetwork& network);
std::string wifi_hardening_advice(const WifiScanNetwork& network);
bool valid_wifi_password(WifiSecurity security, const std::string& password);
std::string request_wifi_connection(const WifiScanNetwork& network, WifiConnectMode mode,
                                    const std::string& password);

} // namespace platform
