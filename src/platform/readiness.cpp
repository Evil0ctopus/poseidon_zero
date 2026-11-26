#include "readiness.h"

#include "adsb_receiver.h"
#include "device_status.h"

#include <system_error>

#if !USE_DESKTOP
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform {
namespace {

std::string serial_status(const char* label, const std::string& path, bool write_required) {
#if USE_DESKTOP
    (void)path;
    (void)write_required;
    return std::string(label) + ": simulator only";
#else
    if (path.empty()) return std::string(label) + ": not configured";
    if (path.compare(0, 5, "/dev/") != 0)
        return std::string(label) + ": invalid device path";
    struct stat status{};
    if (stat(path.c_str(), &status) != 0)
        return std::string(label) + ": " + std::strerror(errno);
    if (!S_ISCHR(status.st_mode)) return std::string(label) + ": not a character device";
    if (access(path.c_str(), R_OK | (write_required ? W_OK : 0)) != 0)
        return std::string(label) + ": " + std::strerror(errno);
    return std::string(label) + ": serial accessible";
#endif
}

std::string bluetooth_status() {
#if USE_DESKTOP
    return "Bluetooth: simulator only";
#else
    std::error_code error;
    std::filesystem::directory_iterator it("/sys/class/bluetooth", error);
    if (error) return "Bluetooth: detection unavailable";
    if (it == std::filesystem::directory_iterator{}) return "Bluetooth: no adapter found";
    return "Bluetooth: adapter listed";
#endif
}

std::string executable_status(const std::filesystem::path& path, const char* name) {
#if USE_DESKTOP
    (void)path;
    return std::string(name) + ": simulator only";
#else
    struct stat status{};
    if (stat(path.c_str(), &status) != 0)
        return errno == ENOENT ? std::string(name) + ": not found"
                              : std::string(name) + ": " + std::strerror(errno);
    if (!S_ISREG(status.st_mode)) return std::string(name) + ": not a regular file";
    if (access(path.c_str(), X_OK) != 0)
        return std::string(name) + ": " + std::strerror(errno);
    return std::string(name) + ": executable found";
#endif
}

} // namespace

std::string readiness_storage_status(std::uintmax_t available_bytes) {
    constexpr std::uintmax_t mib = 1024 * 1024;
    return "Home free: " + std::to_string(available_bytes / mib) + " MiB" +
           (available_bytes < 100 * mib ? " LOW" : "");
}

std::string readiness_battery_status(int percent) {
    if (percent < 0 || percent > 100) return "Battery: unavailable";
    return "Battery: " + std::to_string(percent) + "%" + (percent <= 15 ? " LOW" : "");
}

std::string readiness_adsb_tool_status(const std::filesystem::path& path) {
    return executable_status(path, "rtl_adsb");
}

std::string readiness_rtl_power_status(const std::filesystem::path& path) {
    return executable_status(path, "rtl_power");
}

ReadinessReport read_device_readiness(const std::filesystem::path& home,
                                      const std::string& gps_device,
                                      const std::string& esp32_device) {
    ReadinessReport report;
    std::error_code error;
    if (home.empty() || !home.is_absolute()) {
        report.pages[0] = "Storage: home unavailable";
    } else if (!std::filesystem::is_directory(home, error)) {
        report.pages[0] = error ? "Storage: " + error.message() : "Storage: home is not a directory";
    } else {
        const auto space = std::filesystem::space(home, error);
        report.pages[0] = error ? "Storage: " + error.message() :
            readiness_storage_status(space.available);
    }
#if USE_DESKTOP
    report.pages[0] += "\nBattery: device not checked";
    report.pages[1] = "Wi-Fi: simulator only\n" + bluetooth_status();
#else
    const auto device = read_device_status();
    report.pages[0] += "\n" + readiness_battery_status(
        device.battery_present ? device.battery_percent : -1);
    report.pages[1] = std::string(device.wifi_present ? "Wi-Fi: interface listed" :
                                                       "Wi-Fi: not confirmed") +
                      "\n" + bluetooth_status();
#endif
    report.pages[1] += "\nNo scans or connections";
    report.pages[2] = serial_status("GPS", gps_device, false) +
                      "\nFix and ownership untested";
    report.pages[3] = serial_status("ESP32", esp32_device, true) +
                      "\n" + readiness_adsb_tool_status(kAdsbReceiverExecutable) +
                      "\n" + readiness_rtl_power_status(kRtlPowerExecutable) +
                      "\nSDR hardware/firmware unverified";
    return report;
}

} // namespace platform
