/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "theme.h"

#include "ui_const.h"

#include <array>
#include <cstdint>

namespace view {
namespace {

struct ThemeDefinition {
    const char* name;
    uint16_t background;
    uint16_t text;
    uint16_t primary;
    uint16_t info;
    uint16_t warning;
    uint16_t error;
    uint16_t success;
    uint16_t muted;
    uint16_t button;
    uint16_t bar;
    uint16_t surface;
    uint16_t border;
};

constexpr std::array<ThemeDefinition, 11> kThemes = {{
    {"POSEIDON", 0x0864, 0xEFFF, 0x4E9F, 0xAC5C, 0xFE68, 0xF98C, 0x4ED6, 0x9D36,
     0x1147, 0x10A6, 0x10A6, 0x2969},
    {"MATRIX", 0x0000, 0x07E0, 0x07E0, 0x9FE0, 0xFFE0, 0xF800, 0x07E0, 0x05A0,
     0x0240, 0x0220, 0x0080, 0x0440},
    {"E-INK", 0xFFFF, 0x0000, 0x0000, 0x4208, 0x0000, 0x0000, 0x4208, 0x630C,
     0xC618, 0xDEFB, 0xC618, 0x8410},
    {"SYNTHWAVE", 0x1083, 0xFC9F, 0xFB1F, 0x07FF, 0xFFEF, 0xF99F, 0x07F8, 0x9C9C,
     0x381F, 0x2826, 0x180C, 0xC81F},
    {"PHANTOM", 0x0822, 0xDF7F, 0xB37F, 0x801F, 0xFE60, 0xC000, 0x07E4, 0x9C93,
     0x300A, 0x2007, 0x0820, 0x801F},
    {"BLOOD", 0x0000, 0xCE59, 0xE000, 0x8000, 0xFD00, 0xF800, 0x05C0, 0x9CD3,
     0x4000, 0x2000, 0x1000, 0xE000},
    {"AMBER CRT", 0x0000, 0xFD20, 0xFD20, 0xFA60, 0xFFE0, 0xF800, 0xFD20, 0x8300,
     0x3180, 0x2900, 0x1080, 0xFA60},
    {"NIGHT CITY", 0x0000, 0xFFE0, 0xFFE0, 0x07FF, 0xFC00, 0xF800, 0x07E0, 0x7BE0,
     0x3200, 0x2100, 0x0000, 0x07FF},
    {"SOLARIS", 0x0841, 0xFFE8, 0xFDC0, 0xFA60, 0xFEA0, 0xF800, 0x07E0, 0x9480,
     0x39E0, 0x2100, 0x1080, 0xFA60},
    {"NORDIC FROST", 0x0865, 0xE7FF, 0x3FFF, 0x9E7F, 0xFFE0, 0xF988, 0x07F8, 0x6C78,
     0x11B0, 0x0908, 0x0084, 0x3FFF},
    {"GHOST PURPLE", 0x0000, 0xF3FF, 0xBA1F, 0xE03F, 0xFFC0, 0xF808, 0x07E8, 0x6198,
     0x2810, 0x1808, 0x0800, 0xBA1F},
}};

lv_color_t from_rgb565(uint16_t color) {
    const uint8_t red5 = static_cast<uint8_t>((color >> 11) & 0x1f);
    const uint8_t green6 = static_cast<uint8_t>((color >> 5) & 0x3f);
    const uint8_t blue5 = static_cast<uint8_t>(color & 0x1f);
    const uint8_t red = static_cast<uint8_t>((red5 << 3) | (red5 >> 2));
    const uint8_t green = static_cast<uint8_t>((green6 << 2) | (green6 >> 4));
    const uint8_t blue = static_cast<uint8_t>((blue5 << 3) | (blue5 >> 2));
    return lv_color_make(red, green, blue);
}

ThemePalette zero_light_palette() {
    return {
        lv_color_hex(color::light::kBody),
        lv_color_hex(color::light::kCard),
        lv_color_hex(color::light::kAction),
        lv_color_hex(color::light::kButton),
        lv_color_hex(color::light::kBorder),
        lv_color_hex(color::light::kTextPrimary),
        lv_color_hex(color::light::kTextDisabled),
        lv_color_hex(color::light::kPrimary),
        lv_color_hex(color::light::kInfo),
        lv_color_hex(color::light::kSuccess),
        lv_color_hex(color::light::kWarning),
        lv_color_hex(color::light::kError),
    };
}

} // namespace

ThemePalette palette(model::ThemeId id) {
    if (id == model::ThemeId::ZeroLight) {
        return zero_light_palette();
    }

    const int32_t index = static_cast<int32_t>(id);
    const auto& theme = kThemes[index >= 0 && index < static_cast<int32_t>(kThemes.size())
                                    ? static_cast<std::size_t>(index)
                                    : 0U];
    return {
        from_rgb565(theme.background),
        from_rgb565(theme.surface),
        from_rgb565(theme.bar),
        from_rgb565(theme.button),
        from_rgb565(theme.border),
        from_rgb565(theme.text),
        from_rgb565(theme.muted),
        from_rgb565(theme.primary),
        from_rgb565(theme.info),
        from_rgb565(theme.success),
        from_rgb565(theme.warning),
        from_rgb565(theme.error),
    };
}

ThemePalette palette(bool dark_mode) {
    return palette(dark_mode ? model::ThemeId::Poseidon : model::ThemeId::ZeroLight);
}

const char* theme_name(model::ThemeId id) {
    if (id == model::ThemeId::ZeroLight) {
        return "ZERO LIGHT";
    }
    const int32_t index = static_cast<int32_t>(id);
    return index >= 0 && index < static_cast<int32_t>(kThemes.size())
               ? kThemes[static_cast<std::size_t>(index)].name
               : kThemes[0].name;
}

bool theme_is_dark(model::ThemeId id) {
    return id != model::ThemeId::EInk && id != model::ThemeId::ZeroLight;
}

void apply_lvgl_theme(lv_display_t* display, model::ThemeId id, const lv_font_t* font) {
    if (!display) {
        return;
    }

    const auto colors = palette(id);
    lv_theme_t* lv_theme = lv_theme_default_init(display,
                                                 colors.primary,
                                                 colors.info,
                                                 theme_is_dark(id),
                                                 font);
    lv_display_set_theme(display, lv_theme);
}

} // namespace view
