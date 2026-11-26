/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>

namespace model {

inline constexpr int32_t kStandardToolItemCount = 8;
inline constexpr int32_t kSystemToolItemCount = 12;
inline constexpr std::array<const char*, kSystemToolItemCount> kSystemToolNames = {{
    "Memory", "Wi-Fi", "Bluetooth", "Ethernet", "Storage", "Battery",
    "System clock", "Display brightness", "Themes", "Clock format",
    "Authorized lab mode", "Hardware readiness"
}};
inline constexpr int32_t kNetworkToolItemCount = 12;
inline constexpr int32_t kToolsPageItemCount = 22;
inline constexpr int32_t kWirelessReferenceItemCount = 20;
inline constexpr int32_t kWirelessToolItemCount =
    kWirelessReferenceItemCount + kStandardToolItemCount;
inline constexpr int32_t kMaxToolItemCount = kWirelessToolItemCount;
inline constexpr int32_t kScreensaverPaletteCount = 3;
inline constexpr uint32_t kScreensaverIdleMs = 120000;

struct WirelessReferenceItem {
    const char* label;
    char shortcut;
    const char* description;
};

extern const std::array<WirelessReferenceItem, kWirelessReferenceItemCount>
    kWirelessReferenceItems;
int32_t wireless_reference_shortcut(uint32_t key);

enum class ThemeId : int32_t {
    Poseidon = 0,
    Matrix,
    EInk,
    Synthwave,
    Phantom,
    Blood,
    AmberCrt,
    NightCity,
    Solaris,
    NordicFrost,
    GhostPurple,
    ZeroLight,
};

inline constexpr int32_t kThemeCount = 12;

enum class AppPage {
    Apple  = 0,
    Butter = 1,
    Network = 2,
    Wireless = 3,
    Infrared = 4,
    Expansion = 5,
    Tools = 6,
    Media = 7,
};

constexpr int32_t tool_item_count(AppPage page) {
    return page == AppPage::Tools ? kToolsPageItemCount
           : page == AppPage::Butter ? kSystemToolItemCount
           : page == AppPage::Network ? kNetworkToolItemCount
           : page == AppPage::Wireless ? kWirelessToolItemCount
                                       : kStandardToolItemCount;
}

class BaseModel {
public:
    const char* app_title() const;
    const char* greeting() const;

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();
    ThemeId theme_id() const;
    void set_theme_id(ThemeId id);
    bool clock_24h() const;
    void set_clock_24h(bool enabled);
    bool spacious_layout() const;
    void set_spacious_layout(bool enabled);
    bool screensaver_enabled() const;
    void set_screensaver_enabled(bool enabled);
    int32_t screensaver_palette() const;
    void set_screensaver_palette(int32_t palette);
    bool screensaver_due(uint32_t inactive_ms) const;
    bool grid_layout() const;
    void set_grid_layout(bool enabled);
    bool authorized_lab_mode() const;
    void set_authorized_lab_mode(bool enabled);

    AppPage current_page() const;
    void set_current_page(AppPage page);
    void toggle_page();

    int32_t menu_selection() const;
    void set_menu_selection(int32_t index);
    void move_menu_selection(int32_t delta);
    int32_t tool_selection() const;
    void set_tool_selection(int32_t index);
    void move_tool_selection(int32_t delta);

private:
    ThemeId theme_id_ = ThemeId::ZeroLight;
    bool clock_24h_{true};
    bool spacious_layout_{false};
    bool screensaver_enabled_{false};
    int32_t screensaver_palette_{0};
    bool grid_layout_{false};
    bool authorized_lab_mode_{false};
    AppPage current_page_ = AppPage::Apple;
    int32_t menu_selection_ = 0;
    int32_t tool_selection_ = 0;
};

} // namespace model
