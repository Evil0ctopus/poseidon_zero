/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <algorithm>

namespace model {

const std::array<WirelessReferenceItem, kWirelessReferenceItemCount> kWirelessReferenceItems = {{
    {"Scan", 's', "Scan nearby access points"},
    {"Clients", 'l', "Discover wireless clients"},
    {"AP Clients", 'o', "List access point clients"},
    {"Deauth", 'd', "Deauthentication tool"},
    {"Deauth all", 'x', "Broadcast deauthentication tool"},
    {"Deauth det.", 'e', "Detect deauthentication activity"},
    {"AP Clone", 'c', "Access point clone"},
    {"Portal", 'p', "Captive portal"},
    {"Evil Twin", 'i', "Rogue access point"},
    {"AP Signal Test", 't', "Test access point signal"},
    {"Karma", 'k', "Probe response tool"},
    {"Beacon spam", 'b', "Beacon broadcast tool"},
    {"Probe sniff", 'r', "Observe probe requests"},
    {"PMKID cap", 'm', "Capture authentication data"},
    {"Spectrum", 'g', "View wireless spectrum"},
    {"Wardrive", 'w', "Map wireless networks"},
    {"Connect", 'n', "Connect to a wireless network"},
    {"DefMon", 'y', "Wireless security monitor"},
    {"Cable Guard", 'u', "Inspect for wireless cable implants"},
    {"Surveil", 'v', "Surveillance device detector"},
    {"CIW Zeroclick", 'z', "SSID injection demonstration"},
}};

int32_t wireless_reference_shortcut(uint32_t key) {
    if (key == 'c' || key == 'x' || key == 'z') {
        return -1;
    }
    if (key >= 'A' && key <= 'Z') {
        key += 'a' - 'A';
    }
    for (std::size_t index = 0; index < kWirelessReferenceItems.size(); ++index) {
        if (key == static_cast<uint32_t>(kWirelessReferenceItems[index].shortcut)) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

const char* BaseModel::app_title() const {
    return "POSEIDON";
}

const char* BaseModel::greeting() const {
    return "CardputerZero";
}

bool BaseModel::dark_mode() const {
    return theme_id_ != ThemeId::EInk && theme_id_ != ThemeId::ZeroLight;
}

void BaseModel::set_dark_mode(bool enabled) {
    theme_id_ = enabled ? ThemeId::Poseidon : ThemeId::ZeroLight;
}

void BaseModel::toggle_dark_mode() {
    set_dark_mode(!dark_mode());
}

ThemeId BaseModel::theme_id() const {
    return theme_id_;
}

void BaseModel::set_theme_id(ThemeId id) {
    const int32_t value = static_cast<int32_t>(id);
    theme_id_ = value >= 0 && value < kThemeCount ? id : ThemeId::ZeroLight;
}

bool BaseModel::clock_24h() const {
    return clock_24h_;
}

void BaseModel::set_clock_24h(bool enabled) {
    clock_24h_ = enabled;
}

bool BaseModel::spacious_layout() const {
    return spacious_layout_;
}

void BaseModel::set_spacious_layout(bool enabled) {
    spacious_layout_ = enabled;
}

bool BaseModel::screensaver_enabled() const {
    return screensaver_enabled_;
}

void BaseModel::set_screensaver_enabled(bool enabled) {
    screensaver_enabled_ = enabled;
}

int32_t BaseModel::screensaver_palette() const {
    return screensaver_palette_;
}

void BaseModel::set_screensaver_palette(int32_t palette) {
    if (palette >= 0 && palette < kScreensaverPaletteCount) {
        screensaver_palette_ = palette;
    }
}

bool BaseModel::screensaver_due(uint32_t inactive_ms) const {
    return screensaver_enabled_ && inactive_ms >= kScreensaverIdleMs;
}

bool BaseModel::grid_layout() const {
    return grid_layout_;
}

void BaseModel::set_grid_layout(bool enabled) {
    grid_layout_ = enabled;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}


void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
    set_tool_selection(tool_selection_);
}

void BaseModel::toggle_page() {
    current_page_ = current_page_ == AppPage::Apple ? AppPage::Butter : AppPage::Apple;
}


int32_t BaseModel::menu_selection() const {
    return menu_selection_;
}

void BaseModel::set_menu_selection(int32_t index) {
    menu_selection_ = std::clamp(index, int32_t{0}, int32_t{6});
}

void BaseModel::move_menu_selection(int32_t delta) {
    set_menu_selection(menu_selection_ + delta);
}

int32_t BaseModel::tool_selection() const {
    return tool_selection_;
}

void BaseModel::set_tool_selection(int32_t index) {
    const int32_t max_selection = tool_item_count(current_page_) - 1;
    tool_selection_ = std::clamp(index, int32_t{0}, max_selection);
}

void BaseModel::move_tool_selection(int32_t delta) {
    set_tool_selection(tool_selection_ + delta);
}

} // namespace model
