/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "application_config.h"

#include "base_model.h"
#include "unique_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstring>
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

bool valid_favorites(const std::vector<std::string>& favorites) {
    if (favorites.size() > kMaxFavorites) return false;
    for (std::size_t i = 0; i < favorites.size(); ++i) {
        const auto& favorite = favorites[i];
        if (favorite.size() < 3 || favorite.size() > 80 ||
            favorite[0] < '1' || favorite[0] > '7' || favorite[1] != ':')
            return false;
        for (std::size_t j = 2; j < favorite.size(); ++j) {
            const auto c = static_cast<unsigned char>(favorite[j]);
            if (c < 32 || c > 126 || c == '|' || c == '=') return false;
        }
        if (std::find(favorites.begin(), favorites.begin() + i, favorite) != favorites.begin() + i)
            return false;
    }
    return true;
}

bool validate_config(const ApplicationConfig& config, std::string& error) {
    if (config.accessory_profiles > kAccessoryProfileMask) {
        error = "invalid accessory profiles";
        return false;
    }
    if (!valid_favorites(config.favorites)) {
        error = "invalid favorites";
        return false;
    }
    if (config.theme_id < 0 || config.theme_id >= model::kThemeCount) {
        error = "theme_id is outside the supported range";
        return false;
    }
    if (config.screensaver_palette < 0 ||
        config.screensaver_palette >= model::kScreensaverPaletteCount) {
        error = "screensaver_palette is outside the supported range";
        return false;
    }
    if (config.screensaver_style < 0 || config.screensaver_style > 4) {
        error = "screensaver_style is outside the supported range";
        return false;
    }
    if (config.sound_volume < 0 || config.sound_volume > 100) {
        error = "sound_volume is outside the supported range";
        return false;
    }
    return true;
}

std::array<std::string, 14> config_lines(const ApplicationConfig& config) {
    std::string favorites = "favorites=";
    for (const auto& favorite : config.favorites) {
        if (favorites.size() > 10) favorites += '|';
        favorites += favorite;
    }
    return {{
        config.dark_mode ? "dark_mode=yes" : "dark_mode=no",
        "theme_id=" + std::to_string(config.theme_id),
        config.clock_24h ? "clock_24h=yes" : "clock_24h=no",
        config.spacious_layout ? "spacious_layout=yes" : "spacious_layout=no",
        config.grid_layout ? "grid_layout=yes" : "grid_layout=no",
        "screensaver_style=" + std::to_string(config.screensaver_style),
        config.sound_enabled ? "sound_enabled=yes" : "sound_enabled=no",
        "sound_volume=" + std::to_string(config.sound_volume),
        config.large_text ? "large_text=yes" : "large_text=no",
        config.reduced_motion ? "reduced_motion=yes" : "reduced_motion=no",
        config.screensaver_enabled ? "screensaver_enabled=yes" : "screensaver_enabled=no",
        "screensaver_palette=" + std::to_string(config.screensaver_palette),
        favorites,
        "accessory_profiles=" + std::to_string(config.accessory_profiles)
    }};
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
    bool screensaver_style_set = false;
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
        if (section == "application" && key == "accessory_profiles") {
            const std::string value = trim(line.substr(separator + 1));
            uint32_t profiles = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), profiles);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                profiles > kAccessoryProfileMask) {
                error = "invalid accessory_profiles at line " + std::to_string(line_number);
                return false;
            }
            config.accessory_profiles = profiles;
        }
        if (section == "application" && key == "favorites") {
            const std::string value = trim(line.substr(separator + 1));
            std::vector<std::string> favorites;
            if (!value.empty()) {
                std::size_t start = 0;
                for (;;) {
                    const auto end = value.find('|', start);
                    favorites.push_back(value.substr(start, end - start));
                    if (end == std::string::npos) break;
                    start = end + 1;
                    if (favorites.size() > kMaxFavorites) break;
                }
            }
            if (!valid_favorites(favorites)) {
                error = "invalid favorites at line " + std::to_string(line_number);
                return false;
            }
            config.favorites = std::move(favorites);
        }
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
        if (section == "application" && key == "spacious_layout") {
            if (!parse_bool(line.substr(separator + 1), config.spacious_layout)) {
                error = "invalid spacious_layout value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "grid_layout") {
            if (!parse_bool(line.substr(separator + 1), config.grid_layout)) {
                error = "invalid grid_layout value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "screensaver_style") {
            const std::string value = trim(line.substr(separator + 1));
            int32_t style = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), style);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                style < 0 || style > 4) {
                error = "invalid screensaver_style value at line " + std::to_string(line_number);
                return false;
            }
            config.screensaver_style = style;
            screensaver_style_set = true;
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
        if (section == "application" && key == "screensaver_enabled") {
            if (!parse_bool(line.substr(separator + 1), config.screensaver_enabled)) {
                error = "invalid screensaver_enabled value at line " + std::to_string(line_number);
                return false;
            }
        }
        if (section == "application" && key == "screensaver_palette") {
            const std::string value = trim(line.substr(separator + 1));
            int32_t palette = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), palette);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                palette < 0 || palette >= model::kScreensaverPaletteCount) {
                error = "invalid screensaver_palette value at line " + std::to_string(line_number);
                return false;
            }
            config.screensaver_palette = palette;
        }
    }

    if (!screensaver_style_set) {
        config.screensaver_style = config.screensaver_palette + 2;
    }
    return true;
}

bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error) {
    error.clear();
    if (!validate_config(config, error)) return false;

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
    bool wrote_spacious_layout = false;
    bool wrote_grid_layout = false;
    bool wrote_screensaver_style = false;
    bool wrote_sound_enabled = false;
    bool wrote_sound_volume = false;
    bool wrote_large_text = false;
    bool wrote_reduced_motion = false;
    bool wrote_screensaver_enabled = false;
    bool wrote_screensaver_palette = false;
    bool wrote_favorites = false;
    bool wrote_accessory_profiles = false;
    const auto settings = config_lines(config);
    const auto& dark_mode_line = settings[0];
    const auto& theme_id_line = settings[1];
    const auto& clock_24h_line = settings[2];
    const auto& spacious_layout_line = settings[3];
    const auto& grid_layout_line = settings[4];
    const auto& screensaver_style_line = settings[5];
    const auto& sound_enabled_line = settings[6];
    const auto& sound_volume_line = settings[7];
    const auto& large_text_line = settings[8];
    const auto& reduced_motion_line = settings[9];
    const auto& screensaver_enabled_line = settings[10];
    const auto& screensaver_palette_line = settings[11];
    const auto& favorites_line = settings[12];
    const auto& accessory_profiles_line = settings[13];

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
            if (in_application_section && !wrote_spacious_layout) {
                updated.push_back(spacious_layout_line);
                wrote_spacious_layout = true;
            }
            if (in_application_section && !wrote_grid_layout) {
                updated.push_back(grid_layout_line);
                wrote_grid_layout = true;
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
            if (in_application_section && !wrote_screensaver_enabled) {
                updated.push_back(screensaver_enabled_line);
                wrote_screensaver_enabled = true;
            }
            if (in_application_section && !wrote_screensaver_palette) {
                updated.push_back(screensaver_palette_line);
                wrote_screensaver_palette = true;
            }
            if (in_application_section && !wrote_favorites) {
                updated.push_back(favorites_line);
                wrote_favorites = true;
            }
            if (in_application_section && !wrote_accessory_profiles) {
                updated.push_back(accessory_profiles_line);
                wrote_accessory_profiles = true;
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
        if (in_application_section && is_key(current_line, "spacious_layout")) {
            if (!wrote_spacious_layout) {
                updated.push_back(spacious_layout_line);
                wrote_spacious_layout = true;
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
        if (in_application_section && is_key(current_line, "screensaver_enabled")) {
            if (!wrote_screensaver_enabled) {
                updated.push_back(screensaver_enabled_line);
                wrote_screensaver_enabled = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "screensaver_palette")) {
            if (!wrote_screensaver_palette) {
                updated.push_back(screensaver_palette_line);
                wrote_screensaver_palette = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "favorites")) {
            if (!wrote_favorites) {
                updated.push_back(favorites_line);
                wrote_favorites = true;
            }
            continue;
        }
        if (in_application_section && is_key(current_line, "accessory_profiles")) {
            if (!wrote_accessory_profiles) {
                updated.push_back(accessory_profiles_line);
                wrote_accessory_profiles = true;
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
    if (!wrote_spacious_layout) {
        updated.push_back(spacious_layout_line);
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
    if (!wrote_screensaver_enabled) {
        updated.push_back(screensaver_enabled_line);
    }
    if (!wrote_screensaver_palette) {
        updated.push_back(screensaver_palette_line);
    }
    if (!wrote_favorites) {
        updated.push_back(favorites_line);
    }
    if (!wrote_accessory_profiles) {
        updated.push_back(accessory_profiles_line);
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

SettingsBackup backup_application_settings(const std::filesystem::path& directory,
                                            const ApplicationConfig& config) {
    SettingsBackup result;
    if (!validate_config(config, result.error)) return result;
    result.error = platform::ensure_confidential_directory(directory);
    if (!result.error.empty()) return result;
    auto file = platform::open_unique_file(directory, "settings-backup-", ".ini");
    if (!file.error.empty()) { result.error = file.error; return result; }
    std::string contents = "[application]\n";
    for (const auto& line : config_lines(config)) contents += line + '\n';
    errno = 0;
    if (std::fwrite(contents.data(), 1, contents.size(), file.stream.get()) != contents.size() ||
        std::fflush(file.stream.get()) != 0)
        result.error = std::string("Cannot write settings backup: ") +
                       (errno ? std::strerror(errno) : "I/O error");
    errno = 0;
    if (std::fclose(file.stream.release()) != 0) {
        if (!result.error.empty()) result.error += "; ";
        result.error += std::string("Cannot close settings backup: ") +
                        (errno ? std::strerror(errno) : "I/O error");
    }
    if (!result.error.empty()) {
        result.error += platform::remove_incomplete_file(file.path);
        return result;
    }
    result.path = file.path;
    return result;
}

} // namespace app
