/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "application_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

struct TemporaryConfig {
    std::filesystem::path path;

    ~TemporaryConfig() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

} // namespace

int main() {
    const auto unique_part =
        std::chrono::steady_clock::now().time_since_epoch().count();
    TemporaryConfig temporary{
        std::filesystem::temp_directory_path() /
        ("poseidon-zero-config-" + std::to_string(unique_part) + ".ini")};

    app::ApplicationConfig config;
    config.dark_mode = false;
    config.theme_id = 11;
    config.theme_id_set = true;
    config.clock_24h = false;
    config.spacious_layout = true;
    config.large_text = true;
    config.reduced_motion = true;
    config.screensaver_enabled = true;
    config.screensaver_palette = 2;

    std::string error;
    if (!app::save_application_config(temporary.path.string(), config, error)) {
        std::cerr << "Could not save theme config: " << error << '\n';
        return 1;
    }

    app::ApplicationConfig loaded;
    if (!app::load_application_config(temporary.path.string(), loaded, error) ||
        !loaded.theme_id_set || loaded.theme_id != config.theme_id ||
        loaded.dark_mode != config.dark_mode || loaded.clock_24h != config.clock_24h ||
        loaded.spacious_layout != config.spacious_layout ||
        loaded.large_text != config.large_text ||
        loaded.reduced_motion != config.reduced_motion ||
        loaded.screensaver_enabled != config.screensaver_enabled ||
        loaded.screensaver_palette != config.screensaver_palette) {
        std::cerr << "Saved application settings did not round-trip\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\ndark_mode=yes\n";
    }
    app::ApplicationConfig legacy;
    if (!app::load_application_config(temporary.path.string(), legacy, error) ||
        legacy.theme_id_set || !legacy.dark_mode) {
        std::cerr << "Legacy dark_mode configs should remain supported\n";
        return 1;
    }
    if (!legacy.clock_24h) {
        std::cerr << "Configs without clock_24h should default to 24-hour time\n";
        return 1;
    }
    if (legacy.spacious_layout) {
        std::cerr << "Legacy configs should use the compact tool layout\n";
        return 1;
    }
    if (legacy.screensaver_enabled || legacy.screensaver_palette != 0) {
        std::cerr << "Legacy configs should default to a disabled screensaver\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\ndark_mode=yes\n[other]\nvalue=unchanged\n";
    }
    if (!app::save_application_config(temporary.path.string(), config, error)) {
        std::cerr << "Could not save settings before another section: " << error << '\n';
        return 1;
    }
    app::ApplicationConfig sectioned;
    if (!app::load_application_config(temporary.path.string(), sectioned, error) ||
        !sectioned.spacious_layout || sectioned.screensaver_palette != 2) {
        std::cerr << "Settings must stay in the application section\n";
        return 1;
    }
    {
        std::ifstream input(temporary.path);
        const std::string contents((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        if (contents.find("[other]\nvalue=unchanged\n") == std::string::npos) {
            std::cerr << "Saving settings must preserve unrelated sections\n";
            return 1;
        }
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nscreensaver_enabled=yes\nscreensaver_palette=3\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Out-of-range screensaver palettes should be rejected\n";
        return 1;
    }
    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nscreensaver_enabled=maybe\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid screensaver settings should be rejected\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\ntheme_id=12\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Out-of-range theme IDs should be rejected\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nclock_24h=maybe\n";
    }
    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nspacious_layout=maybe\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid tool layout settings should be rejected\n";
        return 1;
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid clock format settings should be rejected\n";
        return 1;
    }

    return 0;
}
