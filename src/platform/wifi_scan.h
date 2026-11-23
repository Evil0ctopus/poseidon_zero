#pragma once

#include <atomic>
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
};

struct WifiScanResult {
    std::vector<WifiScanNetwork> networks;
    std::string error;
};

enum class WifiConnectMode { Saved, New };
enum class WifiSecurity { Open, WpaPsk, Sae, Unsupported };

WifiScanResult scan_wifi(const std::atomic_bool& cancelled);
int wifi_channel(uint32_t frequency_mhz);
WifiSecurity wifi_security(const WifiScanNetwork& network);
bool valid_wifi_password(WifiSecurity security, const std::string& password);
std::string request_wifi_connection(const WifiScanNetwork& network, WifiConnectMode mode,
                                    const std::string& password);

} // namespace platform
