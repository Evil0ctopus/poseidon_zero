/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "application_config.h"

#include "base_model.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace app {
namespace {

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) {
        return !std::isspace(character);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool parse_bool(const std::string& value, bool& result) {
    const auto normalized = lowercase(trim(value));
    if (normalized == "yes" || normalized == "true" || normalized == "1") {
        result = true;
        return true;
    }
    if (normalized == "no" || normalized == "false" || normalized == "0") {
        result = false;
        return true;
    }
    return false;
}

bool is_section(const std::string& line, const std::string& expected) {
    const auto normalized = trim(line);
    return normalized.size() >= 2 && normalized.front() == '[' && normalized.back() == ']' &&
           lowercase(trim(normalized.substr(1, normalized.size() - 2))) == expected;
}

bool is_key(const std::string& line, const std::string& expected) {
    const auto separator = line.find('=');
    return separator != std::string::npos &&
           lowercase(trim(line.substr(0, separator))) == expected;
}

} // namespace

bool load_application_config(const std::string& path,
                             ApplicationConfig& config,
                             std::string& error) {
    error.clear();
    std::ifstream input(path);
    if (!input) {
        error = "could not open file";
        return false;
    }

    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = lowercase(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const auto key = lowercase(trim(line.substr(0, separator)));
        if (section == "application" && key == "dark_mode") {
            if (!parse_bool(line.substr(separator + 1), config.dark_mode)) {
                error = "invalid dark_mode value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "theme_id") {
            const std::string value = trim(line.substr(separator + 1));
            int32_t theme_id = 0;
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), theme_id);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                theme_id < 0 || theme_id >= model::kThemeCount) {
                error = "invalid theme_id value at line " + std::to_string(line_number);
                return false;
            }
            config.theme_id = theme_id;
            config.theme_id_set = true;
        }
        if (section == "application" && key == "clock_24h") {
            if (!parse_bool(line.substr(separator + 1), config.clock_24h)) {
                error = "invalid clock_24h value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "grid_layout") {
            if (!parse_bool(line.substr(separator + 1), config.grid_layout)) {
                error = "invalid grid_layout value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "screensaver_enabled") {
            if (!parse_bool(line.substr(separator + 1), config.screensaver_enabled)) {
                error = "invalid screensaver_enabled value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "screensaver_style") {
            const std::string value = trim(line.substr(separator + 1));
            int32_t style = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), style);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                style < 0 || style > 1) {
                error = "invalid screensaver_style value at line " + std::to_string(line_number);
                return false;
            }
            config.screensaver_style = style;
        }
        if (section == "application" && key == "sound_enabled") {
            if (!parse_bool(line.substr(separator + 1), config.sound_enabled)) {
                error = "invalid sound_enabled value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "sound_volume") {
            const std::string value = trim(line.substr(separator + 1));
            int32_t volume = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), volume);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                volume < 0 || volume > 100) {
                error = "invalid sound_volume value at line " + std::to_string(line_number);
                return false;
            }
            config.sound_volume = volume;
        }
        if (section == "application" && key == "large_text") {
            if (!parse_bool(line.substr(separator + 1), config.large_text)) {
                error = "invalid large_text value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "reduced_motion") {
            if (!parse_bool(line.substr(separator + 1), config.reduced_motion)) {
                error = "invalid reduced_motion value at line " + std::to_string(line_number);
                return false;
            }
        }
    }

    return true;
}

bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error) {
    error.clear();
    if (config.theme_id < 0 || config.theme_id >= model::kThemeCount) {
        error = "theme_id is outside the supported range";
        return false;
    }
    if (config.screensaver_style < 0 || config.screensaver_style > 1) {
        error = "screensaver_style is outside the supported range";
        return false;
    }

    if (config.sound_volume < 0 || config.sound_volume > 100) {
        error = "sound_volume is outside the supported range";
        return false;
    }

    const std::filesystem::path config_path(path);
    const auto parent = config_path.parent_path();
    if (!parent.empty()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            error = "could not create parent directory: " + filesystem_error.message();
            return false;
        }
    }

    std::vector<std::string> lines;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }

    std::vector<std::string> updated;
    bool in_application_section = false;
    bool found_application_section = false;
    bool wrote_dark_mode = false;
    bool wrote_theme_id = false;
    bool wrote_clock_24h = false;
    bool wrote_grid_layout = false;
    bool wrote_screensaver_enabled = false;
    bool wrote_screensaver_style = false;
    bool wrote_sound_enabled = false;
    bool wrote_sound_volume = false;
    bool wrote_large_text = false;
    bool wrote_reduced_motion = false;
    const std::string dark_mode_line = config.dark_mode ? "dark_mode=yes" : "dark_mode=no";
    const std::string theme_id_line = "theme_id=" + std::to_string(config.theme_id);
    const std::string clock_24h_line = config.clock_24h ? "clock_24h=yes" : "clock_24h=no";
    const std::string grid_layout_line = config.grid_layout ? "grid_layout=yes" : "grid_layout=no";
    const std::string screensaver_enabled_line =
        config.screensaver_enabled ? "screensaver_enabled=yes" : "screensaver_enabled=no";
    const std::string screensaver_style_line =
        "screensaver_style=" + std::to_string(config.screensaver_style);
    const std::string sound_enabled_line = config.sound_enabled ? "sound_enabled=yes" : "sound_enabled=no";
    const std::string sound_volume_line = "sound_volume=" + std::to_string(config.sound_volume);
    const std::string large_text_line = config.large_text ? "large_text=yes" : "large_text=no";
    const std::string reduced_motion_line =
        config.reduced_motion ? "reduced_motion=yes" : "reduced_motion=no";

    for (const auto& current_line : lines) {
        const auto normalized = trim(current_line);
        const bool section_header = normalized.size() >= 2 && normalized.front() == '[' &&
                                    normalized.back() == ']';
        if (section_header) {
            if (in_application_section && !wrote_dark_mode) {
                updated.push_back(dark_mode_line);
                wrote_dark_mode = true;
            }
            if (in_application_section && !wrote_theme_id) {
                updated.push_back(theme_id_line);
                wrote_theme_id = true;
            }
            if (in_application_section && !wrote_clock_24h) {
                updated.push_back(clock_24h_line);
                wrote_clock_24h = true;
            }
            if (in_application_section && !wrote_grid_layout) {
                updated.push_back(grid_layout_line);
                wrote_grid_layout = true;
            }
            if (in_application_section && !wrote_screensaver_enabled) {
                updated.push_back(screensaver_enabled_line);
                wrote_screensaver_enabled = true;
            }
            if (in_application_section && !wrote_screensaver_style) {
                updated.push_back(screensaver_style_line);
                wrote_screensaver_style = true;
            }
            if (in_application_section && !wrote_sound_enabled) {
                updated.push_back(sound_enabled_line);
                wrote_sound_enabled = true;
            }
            if (in_application_section && !wrote_sound_volume) {
                updated.push_back(sound_volume_line);
                wrote_sound_volume = true;
            }
            if (in_application_section && !wrote_large_text) {
                updated.push_back(large_text_line);
                wrote_large_text = true;
            }
            if (in_application_section && !wrote_reduced_motion) {
                updated.push_back(reduced_motion_line);
                wrote_reduced_motion = true;
            }
            in_application_section = is_section(current_line, "application");
            found_application_section = found_application_section || in_application_section;
        }

        if (in_application_section && is_key(current_line, "dark_mode")) {
            if (!wrote_dark_mode) {
                updated.push_back(dark_mode_line);
                wrote_dark_mode = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "theme_id")) {
            if (!wrote_theme_id) {
                updated.push_back(theme_id_line);
                wrote_theme_id = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "clock_24h")) {
            if (!wrote_clock_24h) {
                updated.push_back(clock_24h_line);
                wrote_clock_24h = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "grid_layout")) {
            if (!wrote_grid_layout) {
                updated.push_back(grid_layout_line);
                wrote_grid_layout = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "screensaver_enabled")) {
            if (!wrote_screensaver_enabled) {
                updated.push_back(screensaver_enabled_line);
                wrote_screensaver_enabled = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "screensaver_style")) {
            if (!wrote_screensaver_style) {
                updated.push_back(screensaver_style_line);
                wrote_screensaver_style = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "sound_enabled")) {
            if (!wrote_sound_enabled) {
                updated.push_back(sound_enabled_line);
                wrote_sound_enabled = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "sound_volume")) {
            if (!wrote_sound_volume) {
                updated.push_back(sound_volume_line);
                wrote_sound_volume = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "large_text")) {
            if (!wrote_large_text) {
                updated.push_back(large_text_line);
                wrote_large_text = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "reduced_motion")) {
            if (!wrote_reduced_motion) {
                updated.push_back(reduced_motion_line);
                wrote_reduced_motion = true;
            }
            continue;
        }
        updated.push_back(current_line);
    }

    if (!wrote_dark_mode) {
        if (!updated.empty() && !updated.back().empty()) {
            updated.emplace_back();
        }
        if (!found_application_section) {
            updated.emplace_back("[application]");
        }
        updated.push_back(dark_mode_line);
    }
    if (!wrote_theme_id) {
        updated.push_back(theme_id_line);
    }
    if (!wrote_clock_24h) {
        updated.push_back(clock_24h_line);
    }
    if (!wrote_grid_layout) {
        updated.push_back(grid_layout_line);
    }
    if (!wrote_screensaver_enabled) {
        updated.push_back(screensaver_enabled_line);
    }
    if (!wrote_screensaver_style) {
        updated.push_back(screensaver_style_line);
    }
    if (!wrote_sound_enabled) {
        updated.push_back(sound_enabled_line);
    }
    if (!wrote_sound_volume) {
        updated.push_back(sound_volume_line);
    }
    if (!wrote_large_text) {
        updated.push_back(large_text_line);
    }
    if (!wrote_reduced_motion) {
        updated.push_back(reduced_motion_line);
    }

    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "could not open file for writing";
        return false;
    }
    for (const auto& updated_line : updated) {
        output << updated_line << '\n';
    }
    if (!output) {
        error = "failed while writing file";
        return false;
    }

    return true;
}

} // namespace app
