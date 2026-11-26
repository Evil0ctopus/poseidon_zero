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
    config.grid_layout = true;
    config.screensaver_enabled = true;
    config.screensaver_style = 4;
    config.sound_enabled = true;
    config.sound_volume = 70;
    config.large_text = true;
    config.reduced_motion = true;
    config.screensaver_palette = 2;
    config.favorites = {"6:ADS-B receiver", "3:Wardrive", "1:Hardware readiness"};
    config.accessory_profiles = 21;

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
        loaded.grid_layout != config.grid_layout ||
        loaded.screensaver_enabled != config.screensaver_enabled ||
        loaded.screensaver_style != config.screensaver_style ||
        loaded.sound_enabled != config.sound_enabled || loaded.sound_volume != config.sound_volume ||
        loaded.large_text != config.large_text ||
        loaded.reduced_motion != config.reduced_motion ||
        loaded.screensaver_enabled != config.screensaver_enabled ||
        loaded.screensaver_palette != config.screensaver_palette ||
        loaded.favorites != config.favorites ||
        loaded.accessory_profiles != config.accessory_profiles) {
        std::cerr << "Saved application settings did not round-trip\n";
        return 1;
    }

    auto without_favorites = config;
    without_favorites.favorites.clear();
    app::ApplicationConfig reloaded;
    if (!app::save_application_config(temporary.path.string(), without_favorites, error) ||
        !app::load_application_config(temporary.path.string(), reloaded, error) ||
        !reloaded.favorites.empty() || reloaded.theme_id != config.theme_id ||
        reloaded.accessory_profiles != config.accessory_profiles ||
        !app::save_application_config(temporary.path.string(), config, error)) {
        std::cerr << "Removing favorites did not persist safely: " << error << '\n';
        return 1;
    }

    const auto backup_directory = std::filesystem::temp_directory_path() /
        ("poseidon-settings-backup-test-" + std::to_string(unique_part));
    const auto backup = app::backup_application_settings(backup_directory, config);
    if (!backup.error.empty()) {
        std::cerr << "Could not back up settings: " << backup.error << '\n';
        return 1;
    }
    app::ApplicationConfig restored;
    if (!app::load_application_config(backup.path.string(), restored, error) ||
        restored.theme_id != config.theme_id || !restored.theme_id_set ||
        restored.dark_mode != config.dark_mode || restored.clock_24h != config.clock_24h ||
        restored.spacious_layout != config.spacious_layout ||
        restored.grid_layout != config.grid_layout ||
        restored.screensaver_enabled != config.screensaver_enabled ||
        restored.screensaver_style != config.screensaver_style ||
        restored.sound_enabled != config.sound_enabled ||
        restored.sound_volume != config.sound_volume ||
        restored.large_text != config.large_text || restored.reduced_motion != config.reduced_motion ||
        restored.screensaver_palette != config.screensaver_palette ||
        restored.favorites != config.favorites ||
        restored.accessory_profiles != config.accessory_profiles) {
        std::cerr << "Settings backup did not round-trip\n";
        return 1;
    }
    auto invalid_backup = config;
    invalid_backup.sound_volume = 101;
    if (app::backup_application_settings(backup_directory, invalid_backup).error.empty()) {
        std::cerr << "Invalid settings backup accepted\n";
        return 1;
    }
    invalid_backup = config;
    invalid_backup.favorites.push_back(config.favorites.front());
    if (app::backup_application_settings(backup_directory, invalid_backup).error.empty()) {
        std::cerr << "Duplicate favorites accepted\n";
        return 1;
    }
    invalid_backup = config;
    invalid_backup.accessory_profiles = app::kAccessoryProfileMask + 1;
    if (app::backup_application_settings(backup_directory, invalid_backup).error.empty()) {
        std::cerr << "Invalid accessory profiles accepted\n";
        return 1;
    }
    std::error_code cleanup_error;
    const auto backup_link = backup_directory.parent_path() /
        ("poseidon-settings-backup-link-" + std::to_string(unique_part));
    std::filesystem::create_directory_symlink(backup_directory, backup_link, cleanup_error);
    if (!cleanup_error) {
        if (app::backup_application_settings(backup_link, config).error.empty()) {
            std::cerr << "Settings backup followed a directory link\n";
            return 1;
        }
        std::filesystem::remove(backup_link, cleanup_error);
        if (cleanup_error) return 1;
    }
#if !defined(_WIN32)
    else {
        std::cerr << "Could not create settings backup directory link\n";
        return 1;
    }
    std::filesystem::permissions(backup_directory, std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::add, cleanup_error);
    if (cleanup_error || app::backup_application_settings(backup_directory, config).error.empty()) {
        std::cerr << "Settings backup accepted a non-private directory\n";
        return 1;
    }
    std::filesystem::permissions(backup_directory, std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::remove, cleanup_error);
    if (cleanup_error) return 1;
#else
    cleanup_error.clear();
#endif
    std::filesystem::remove(backup.path, cleanup_error);
    if (cleanup_error || !std::filesystem::remove(backup_directory, cleanup_error) || cleanup_error) {
        std::cerr << "Settings backup cleanup failed\n";
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
    if (legacy.grid_layout) {
        std::cerr << "Configs without grid_layout should default to list layout\n";
        return 1;
    }
    if (legacy.screensaver_enabled || legacy.screensaver_palette != 0 ||
        legacy.screensaver_style != 2) {
        std::cerr << "Legacy configs should default to a disabled screensaver\n";
        return 1;
    }
    if (legacy.sound_enabled || legacy.sound_volume != 50) {
        std::cerr << "Legacy configs should default to muted UI sounds\n";
        return 1;
    }
    if (!legacy.favorites.empty() || legacy.accessory_profiles != 0) {
        std::cerr << "Legacy configs should have no favorites or accessory profiles\n";
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
        !sectioned.spacious_layout || !sectioned.grid_layout ||
        sectioned.screensaver_palette != 2 || sectioned.screensaver_style != 4) {
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
    for (const char* favorites : {"0:Unknown", "6:Name|", "6:Name|6:Name",
                                  "1:A|1:B|1:C|1:D|1:E|1:F|1:G|1:H|1:I"}) {
        {
            std::ofstream output(temporary.path, std::ios::trunc);
            output << "[application]\nfavorites=" << favorites << '\n';
        }
        app::ApplicationConfig invalid;
        if (app::load_application_config(temporary.path.string(), invalid, error) ||
            error.find("favorites") == std::string::npos) {
            std::cerr << "Invalid favorites accepted: " << favorites << '\n';
            return 1;
        }
    }
    for (const char* profiles : {"32", "-1", "abc", ""}) {
        {
            std::ofstream output(temporary.path, std::ios::trunc);
            output << "[application]\naccessory_profiles=" << profiles << '\n';
        }
        app::ApplicationConfig invalid;
        if (app::load_application_config(temporary.path.string(), invalid, error) ||
            error.find("accessory_profiles") == std::string::npos) {
            std::cerr << "Invalid accessory profiles accepted: " << profiles << '\n';
            return 1;
        }
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
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid clock format settings should be rejected\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nspacious_layout=maybe\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid tool layout settings should be rejected\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\ngrid_layout=maybe\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid layout settings should be rejected\n";
        return 1;
    }

    {
        std::ofstream output(temporary.path, std::ios::trunc);
        output << "[application]\nscreensaver_style=5\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Invalid screensaver styles should be rejected\n";
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
        output << "[application]\nsound_volume=101\n";
    }
    if (app::load_application_config(temporary.path.string(), loaded, error)) {
        std::cerr << "Out-of-range sound volume should be rejected\n";
        return 1;
    }

    return 0;
}
