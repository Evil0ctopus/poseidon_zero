#include "capabilities.h"

#include <filesystem>
#include <fstream>
#include <string>

#if USE_DESKTOP
#include "device_status.h"
#endif

namespace platform {
namespace {

bool sysfs_exists(const char* path) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

bool input_device_exists(const char* name) {
    std::error_code error;
    std::filesystem::path root("/sys/class/input");
    if (!std::filesystem::exists(root, error)) {
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        std::ifstream input(entry.path() / "name");
        std::string detected;
        if (std::getline(input, detected) && detected.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool i2c_device_present(const char* address) {
    std::error_code error;
    std::filesystem::path root("/sys/bus/i2c/devices");
    if (!std::filesystem::exists(root, error)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.path().filename().string().find(address) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

CapabilityStatus read_capability(Capability capability) {
#if USE_DESKTOP
    switch (capability) {
        case Capability::Wifi:
            return {true, true, "Desktop emulator"};
        case Capability::Bluetooth:
            return {true, false, "Desktop emulator"};
        case Capability::Ethernet:
            return {true, true, "Desktop emulator"};
        case Capability::Battery:
            return {false, false, "No battery on desktop"};
        case Capability::Camera:
            return {false, false, "Simulated only"};
        case Capability::Imu:
            return {false, false, "Simulated only"};
        case Capability::Ir:
            return {false, false, "Simulated only"};
        case Capability::Rtc:
            return {true, true, "Desktop clock"};
        case Capability::Audio:
            return {true, true, "Desktop audio"};
        case Capability::Microphone:
            return {true, false, "Permission not declared"};
        case Capability::Grove:
            return {true, false, "Runtime detection required"};
        case Capability::Expansion:
            return {true, false, "Runtime detection required"};
        case Capability::Usb:
            return {true, true, "Desktop USB unavailable"};
        case Capability::ExternalDisplay:
            return {true, true, "SDL2 simulator"};
    }
#endif

    switch (capability) {
        case Capability::Wifi:
            return {sysfs_exists("/sys/class/net"), sysfs_exists("/proc/net/wireless"), "Linux Wi-Fi"};
        case Capability::Bluetooth:
            return {sysfs_exists("/sys/class/bluetooth"), sysfs_exists("/sys/class/bluetooth"), "BlueZ"};
        case Capability::Ethernet:
            return {sysfs_exists("/sys/class/net"), sysfs_exists("/sys/class/net"), "USB 10/100 Ethernet"};
        case Capability::Battery:
            return {sysfs_exists("/sys/class/power_supply"), sysfs_exists("/sys/class/power_supply"), "BQ27220"};
        case Capability::Camera:
            return {sysfs_exists("/dev/video0"), sysfs_exists("/dev/video0"), "IMX219 / USB camera"};
        case Capability::Imu:
            return {i2c_device_present("68"), i2c_device_present("68"), "BMI270"};
        case Capability::Ir:
            return {input_device_exists("ir"), input_device_exists("ir"), "IR TX/RX"};
        case Capability::Rtc:
            return {sysfs_exists("/dev/rtc0"), sysfs_exists("/dev/rtc0"), "RX8130CE"};
        case Capability::Audio:
            return {sysfs_exists("/proc/asound"), sysfs_exists("/proc/asound"), "ES8389"};
        case Capability::Microphone:
            return {sysfs_exists("/proc/asound"), false, "MEMS microphone"};
        case Capability::Grove:
            return {true, false, "HY2.0-4P"};
        case Capability::Expansion:
            return {true, false, "EXT 2.54-14P"};
        case Capability::Usb:
            return {sysfs_exists("/sys/bus/usb"), sysfs_exists("/sys/bus/usb"), "USB 2.0 hub"};
        case Capability::ExternalDisplay:
            return {sysfs_exists("/sys/class/drm"), false, "HDMI / DRM"};
    }

    return {};
}

} // namespace platform
