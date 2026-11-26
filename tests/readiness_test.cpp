#include "readiness.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    constexpr std::uintmax_t mib = 1024 * 1024;
    if (platform::readiness_storage_status(100 * mib - 1).find("LOW") == std::string::npos ||
        platform::readiness_storage_status(100 * mib).find("LOW") != std::string::npos ||
        platform::readiness_battery_status(15).find("LOW") == std::string::npos ||
        platform::readiness_battery_status(16).find("LOW") != std::string::npos ||
        platform::readiness_battery_status(-1) != "Battery: unavailable" ||
        platform::readiness_battery_status(101) != "Battery: unavailable") {
        std::cerr << "Readiness capacity warnings are incorrect\n";
        return 1;
    }
    const auto home = std::filesystem::temp_directory_path();
    const auto empty = platform::read_device_readiness(home, "", "");
    if (empty.pages[0].find("Home free:") == std::string::npos ||
        empty.pages[1].find("No scans or connections") == std::string::npos ||
        empty.pages[3].find("SDR hardware/firmware unverified") == std::string::npos) {
        std::cerr << "Readiness report missing safety or storage information\n";
        return 1;
    }
#if !USE_DESKTOP
    if (empty.pages[2].find("GPS: not configured") == std::string::npos ||
        empty.pages[3].find("ESP32: not configured") == std::string::npos) return 1;
    const auto serial = platform::read_device_readiness(home, "/dev/null", "/dev/null");
    if (serial.pages[2].find("serial accessible") == std::string::npos ||
        serial.pages[3].find("SDR hardware/firmware unverified") == std::string::npos) return 1;
    const auto invalid = platform::read_device_readiness({}, "bad", "/dev/does-not-exist");
    if (invalid.pages[0].find("home unavailable") == std::string::npos ||
        invalid.pages[2].find("invalid device path") == std::string::npos ||
        invalid.pages[3].find("ESP32:") == std::string::npos ||
        invalid.pages[3].find("serial accessible") != std::string::npos) return 1;
    const auto not_devices = platform::read_device_readiness(home, "/dev", "/dev/");
    if (not_devices.pages[2].find("invalid device path") == std::string::npos ||
        not_devices.pages[3].find("not a character device") == std::string::npos) return 1;
    const auto tool_dir = home / ("poseidon-readiness-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code error;
    if (!std::filesystem::create_directory(tool_dir, error) || error) return 1;
    const auto tool = tool_dir / "rtl_adsb";
    const auto rtl_power = tool_dir / "rtl_power";
    if (platform::readiness_adsb_tool_status(tool) != "rtl_adsb: not found" ||
        platform::readiness_adsb_tool_status(tool_dir) != "rtl_adsb: not a regular file" ||
        platform::readiness_rtl_power_status(rtl_power) != "rtl_power: not found" ||
        platform::readiness_rtl_power_status(tool_dir) != "rtl_power: not a regular file") return 1;
    { std::ofstream file(tool); file << "fixture"; if (!file) return 1; }
    std::filesystem::permissions(tool, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, error);
    if (error || platform::readiness_adsb_tool_status(tool) == "rtl_adsb: executable found") return 1;
    std::filesystem::permissions(tool, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add, error);
    if (error || platform::readiness_adsb_tool_status(tool) != "rtl_adsb: executable found") return 1;
    std::filesystem::remove(tool, error);
    if (error) return 1;
    { std::ofstream file(rtl_power); file << "fixture"; if (!file) return 1; }
    std::filesystem::permissions(rtl_power, std::filesystem::perms::owner_read,
                                 std::filesystem::perm_options::replace, error);
    if (error || platform::readiness_rtl_power_status(rtl_power) == "rtl_power: executable found")
        return 1;
    std::filesystem::permissions(rtl_power, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add, error);
    if (error || platform::readiness_rtl_power_status(rtl_power) != "rtl_power: executable found")
        return 1;
    std::filesystem::remove(rtl_power, error);
    if (error || !std::filesystem::remove(tool_dir, error) || error) return 1;
#else
    if (empty.pages[2].find("simulator only") == std::string::npos ||
        empty.pages[3].find("simulator only") == std::string::npos ||
        platform::readiness_adsb_tool_status("ignored") != "rtl_adsb: simulator only" ||
        platform::readiness_rtl_power_status("ignored") != "rtl_power: simulator only") return 1;
#endif
    return 0;
}
