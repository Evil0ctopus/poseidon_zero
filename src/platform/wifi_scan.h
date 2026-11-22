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
};

struct WifiScanResult {
    std::vector<WifiScanNetwork> networks;
    std::string error;
};

WifiScanResult scan_wifi(const std::atomic_bool& cancelled);
int wifi_channel(uint32_t frequency_mhz);

} // namespace platform
