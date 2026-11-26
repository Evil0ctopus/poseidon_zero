/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace app {

inline constexpr std::size_t kMaxFavorites = 8;
inline constexpr uint32_t kAccessoryProfileMask = 31;

struct ApplicationConfig {
    bool dark_mode = false;
    int32_t theme_id = 0;
    bool theme_id_set = false;
    bool clock_24h = true;
    bool spacious_layout = false;
    bool grid_layout = false;
    bool screensaver_enabled = false;
    int32_t screensaver_style = 0;
    bool sound_enabled = false;
    int32_t sound_volume = 50;
    bool large_text = false;
    bool reduced_motion = false;
    int32_t screensaver_palette = 0;
    std::vector<std::string> favorites;
    uint32_t accessory_profiles = 0;
};

bool load_application_config(const std::string& path,
                             ApplicationConfig& config,
                             std::string& error);
bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error);

struct SettingsBackup {
    std::filesystem::path path;
    std::string error;
};

SettingsBackup backup_application_settings(const std::filesystem::path& directory,
                                            const ApplicationConfig& config);

} // namespace app
