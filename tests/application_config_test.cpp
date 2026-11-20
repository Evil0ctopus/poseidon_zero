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
    config.large_text = true;
    config.reduced_motion = true;

    std::string error;
    if (!app::save_application_config(temporary.path.string(), config, error)) {
        std::cerr << "Could not save theme config: " << error << '\n';
        return 1;
    }

    app::ApplicationConfig loaded;
    if (!app::load_application_config(temporary.path.string(), loaded, error) ||
        !loaded.theme_id_set || loaded.theme_id != config.theme_id ||
        loaded.dark_mode != config.dark_mode || loaded.large_text != config.large_text ||
        loaded.reduced_motion != config.reduced_motion) {
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

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\ntheme_id=12\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Out-of-range theme IDs should be rejected\n";
        return 1;
    }

    return 0;
}
