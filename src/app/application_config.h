/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>
#include <string>

namespace app {

struct ApplicationConfig {
    bool dark_mode = false;
    int32_t theme_id = 0;
    bool theme_id_set = false;
    bool clock_24h = true;
    bool spacious_layout = false;
    bool large_text = false;
    bool reduced_motion = false;
    bool screensaver_enabled = false;
    int32_t screensaver_palette = 0;
};

bool load_application_config(const std::string& path,
                             ApplicationConfig& config,
                             std::string& error);
bool save_application_config(const std::string& path,
                             const ApplicationConfig& config,
                             std::string& error);

} // namespace app
