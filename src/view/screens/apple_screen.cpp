/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"
#include "ui_const.h"

namespace screen {
namespace {

struct MenuSelectionBinding {
    lv_obj_t* rows[5];
};

void cleanup_menu_selection(lv_event_t* event) {
    delete static_cast<MenuSelectionBinding*>(lv_event_get_user_data(event));
}

void menu_selection_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_observer_get_user_data(observer));
    if (!binding) {
        return;
    }

    const int32_t selected = lv_subject_get_int(subject);
    for (int32_t index = 0; index < 5; ++index) {
        if (!lv_obj_is_valid(binding->rows[index])) {
            continue;
        }
        lv_obj_set_style_border_color(
            binding->rows[index],
            index == selected ? lv_color_hex(view::color::dark::kPrimary)
                              : lv_color_hex(view::color::dark::kBorder),
            0);
    }
}

struct AppleTextFontBinding {
    lv_font_t* regular;
    lv_font_t* bold;
};

constexpr const char* kIconKbFn          = "\uE080";
constexpr const char* kIconKbH           = "\uE084";
constexpr const char* kIconKbJ           = "\uE08C";
constexpr const char* kIconKbPrintScreen = "\uE0B1";
constexpr const char* kIconKbHelp        = "\uEA28";

void cleanup_font_binding(lv_event_t* event) {
    delete static_cast<AppleTextFontBinding*>(lv_event_get_user_data(event));
}

void apple_text_font_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* label = lv_observer_get_target_obj(observer);
    auto* fonts = static_cast<AppleTextFontBinding*>(lv_observer_get_user_data(observer));
    if (!label || !fonts) {
        return;
    }

    auto* font = lv_subject_get_int(subject) ? fonts->bold : fonts->regular;
    lv_obj_set_style_text_font(label, font ? font : &lv_font_montserrat_20, 0);
}

void info_visible_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* label = lv_observer_get_target_obj(observer);
    if (!label) {
        return;
    }

    if (lv_subject_get_int(subject)) {
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

AppleScreen::AppleScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AppleScreen::build_content(lv_obj_t* content) {
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(content, 5, 0);
    lv_obj_set_style_pad_row(content, 3, 0);

    const lv_font_t* title_font = assets().load_standard_font(15);
    const lv_font_t* label_font = assets().load_standard_font(12, app::StandardFontWeight::Regular);
    const lv_font_t* key_font = assets().load_standard_font(11, app::StandardFontWeight::Regular);

    auto* heading = lv_label_create(content);
    lv_label_set_text(heading, "AUTHORIZED OPERATIONS");
    lv_obj_set_style_text_font(heading, title_font ? title_font : &lv_font_montserrat_14, 0);
    reactive::bind_theme(heading, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    const char* categories[] = {
        "1  NETWORK",
        "2  WIRELESS",
        "3  INFRARED",
        "4  EXPANSION",
        "5  SYSTEM",
    };
    lv_obj_t* row_objects[5] = {};
    for (int32_t index = 0; index < 5; ++index) {
        auto* row = lv_button_create(content);
        row_objects[index] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 270, 18);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_left(row, 7, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        reactive::bind_theme(row, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

        auto* label = lv_label_create(row);
        lv_label_set_text(label, categories[index]);
        lv_obj_set_style_text_font(label, label_font ? label_font : &lv_font_montserrat_12, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        reactive::bind_theme(label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

        auto* shortcut = lv_label_create(row);
        lv_label_set_text(shortcut, "ENTER");
        lv_obj_set_style_text_font(shortcut, key_font ? key_font : &lv_font_montserrat_10, 0);
        lv_obj_align(shortcut, LV_ALIGN_RIGHT_MID, -5, 0);
        reactive::bind_theme(shortcut, view_model().dark_mode_subject(), reactive::ThemeRole::TextDisabled);
    }

    const int32_t selected = view_model().menu_selection();
    for (int32_t index = 0; index < 5; ++index) {
        lv_obj_set_style_border_color(
            row_objects[index],
            index == selected ? lv_color_hex(view::color::dark::kPrimary)
                              : lv_color_hex(view::color::dark::kBorder),
            0);
    }

    auto* binding = new MenuSelectionBinding{};
    for (int32_t index = 0; index < 5; ++index) {
        binding->rows[index] = row_objects[index];
    }
    lv_obj_add_event_cb(content, cleanup_menu_selection, LV_EVENT_DELETE, binding);
    lv_subject_add_observer_obj(view_model().menu_selection_subject(),
                                menu_selection_observer,
                                content,
                                binding);
}

} // namespace screen
