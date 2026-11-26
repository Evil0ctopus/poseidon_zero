#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace platform {

struct ReadinessReport {
    std::array<std::string, 4> pages;
};

std::string readiness_storage_status(std::uintmax_t available_bytes);
std::string readiness_battery_status(int percent);
std::string readiness_adsb_tool_status(const std::filesystem::path& path);
std::string readiness_rtl_power_status(const std::filesystem::path& path);
inline constexpr char kRtlPowerExecutable[] = "/usr/bin/rtl_power";
ReadinessReport read_device_readiness(const std::filesystem::path& home,
                                      const std::string& gps_device,
                                      const std::string& esp32_device);

} // namespace platform
