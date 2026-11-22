/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "lvgl.h"
#include "../model/base_model.h"

namespace view {

constexpr int kScreenWidth    = 320;
constexpr int kScreenHeight   = 170;
constexpr int kTitleBarHeight = 24;
constexpr int kNavBarHeight   = 28;

struct ThemePalette {
    lv_color_t background;
    lv_color_t surface;
    lv_color_t bar;
    lv_color_t button;
    lv_color_t border;
    lv_color_t text;
    lv_color_t text_disabled;
    lv_color_t primary;
    lv_color_t info;
    lv_color_t success;
    lv_color_t warning;
    lv_color_t error;
};

ThemePalette palette(model::ThemeId id);
ThemePalette palette(bool dark_mode);
const char* theme_name(model::ThemeId id);
bool theme_is_dark(model::ThemeId id);
void apply_lvgl_theme(lv_display_t* display,
                      model::ThemeId id,
                      const lv_font_t* font = LV_FONT_DEFAULT);

} // namespace view
