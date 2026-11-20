#include "backlight.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef APP_BACKLIGHT_ROOT
#define APP_BACKLIGHT_ROOT "/sys/class/backlight"
#endif

namespace platform {
namespace {

#if !USE_DESKTOP
bool find_backlight_device(std::filesystem::path& device, std::string& error) {
    std::error_code filesystem_error;
    std::filesystem::directory_iterator entry(APP_BACKLIGHT_ROOT, filesystem_error);
    if (filesystem_error) {
        error = "No readable system backlight interface was found";
        return false;
    }

    const std::filesystem::directory_iterator end;
    while (entry != end) {
        const auto candidate = entry->path();
        std::error_code brightness_error;
        std::error_code maximum_error;
        const bool has_brightness =
            std::filesystem::exists(candidate / "brightness", brightness_error);
        const bool has_maximum =
            std::filesystem::exists(candidate / "max_brightness", maximum_error);
        if (!brightness_error && !maximum_error && has_brightness && has_maximum) {
            if (!device.empty()) {
                error = "Multiple backlight devices were found; refusing to guess";
                return false;
            }
            device = candidate;
        }

        entry.increment(filesystem_error);
        if (filesystem_error) {
            error = "Could not enumerate system backlight devices";
            return false;
        }
    }

    if (device.empty()) {
        error = "No usable system backlight device was found";
        return false;
    }
    return true;
}

bool read_levels(const std::filesystem::path& device,
                 int64_t& brightness,
                 int64_t& maximum,
                 std::string& error) {
    std::ifstream brightness_file(device / "brightness");
    std::ifstream maximum_file(device / "max_brightness");
    if (!(brightness_file >> brightness) || !(maximum_file >> maximum)) {
        error = "Could not read the backlight brightness";
        return false;
    }
    if (maximum <= 0 || brightness < 0 || brightness > maximum) {
        error = "The backlight reported an invalid brightness range";
        return false;
    }
    return true;
}
#endif

} // namespace

bool read_brightness_percent(int32_t& percent, std::string& error) {
    error.clear();
#if USE_DESKTOP
    static_cast<void>(percent);
    error = "Backlight control is unavailable in the desktop simulator";
    return false;
#else
    std::filesystem::path device;
    if (!find_backlight_device(device, error)) {
        return false;
    }

    int64_t brightness = 0;
    int64_t maximum = 0;
    if (!read_levels(device, brightness, maximum, error)) {
        return false;
    }

    percent = static_cast<int32_t>(
        (static_cast<long double>(brightness) * 100.0L / maximum) + 0.5L);
    return true;
#endif
}

bool write_brightness_percent(int32_t percent, std::string& error) {
    error.clear();
#if USE_DESKTOP
    static_cast<void>(percent);
    error = "Backlight control is unavailable in the desktop simulator";
    return false;
#else
    std::filesystem::path device;
    if (!find_backlight_device(device, error)) {
        return false;
    }

    int64_t current = 0;
    int64_t maximum = 0;
    if (!read_levels(device, current, maximum, error)) {
        return false;
    }

    const int32_t clamped_percent = std::clamp(percent, 5, 100);
    const int64_t target =
        (maximum / 100) * clamped_percent +
        ((maximum % 100) * clamped_percent + 50) / 100;
    std::ofstream brightness_file(device / "brightness");
    if (!brightness_file) {
        error = "Could not write backlight brightness; check device permissions";
        return false;
    }
    brightness_file << target << '\n';
    brightness_file.flush();
    if (!brightness_file) {
        error = "The backlight rejected the brightness change";
        return false;
    }
    brightness_file.close();
    if (!brightness_file) {
        error = "Could not finish writing backlight brightness";
        return false;
    }

    int64_t actual = 0;
    if (!read_levels(device, actual, maximum, error)) {
        return false;
    }
    if (actual != target) {
        error = "The backlight did not apply the requested brightness";
        return false;
    }
    return true;
#endif
}

} // namespace platform
