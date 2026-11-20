/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace model {

inline constexpr int32_t kStandardToolItemCount = 8;
inline constexpr int32_t kSystemToolItemCount = 10;
inline constexpr int32_t kToolsPageItemCount = 17;

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
    bool grid_layout() const;
    void set_grid_layout(bool enabled);

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
    bool grid_layout_{false};
    AppPage current_page_ = AppPage::Apple;
    int32_t menu_selection_ = 0;
    int32_t tool_selection_ = 0;
};

} // namespace model
