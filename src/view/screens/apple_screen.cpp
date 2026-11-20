/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace screen {
namespace {

constexpr std::size_t kCategoryCount = 7;
constexpr std::array<const char*, kCategoryCount> kTabLabels = {
    "NET", "WIFI", "IR", "I/O", "SYS", "TOOL", "MED"};
constexpr std::array<const char*, kCategoryCount> kCategoryNames = {
    "NETWORK",
    "WIRELESS",
    "INFRARED",
    "EXPANSION",
    "SYSTEM",
    "TOOLS",
    "MEDIA",
};
constexpr std::array<const char*, kCategoryCount> kCategoryDetails = {
    "Local network diagnostics",
    "Wireless status and tools",
    "Infrared utilities",
    "Ports and connected modules",
    "Device health and settings",
    "Utilities, Morse, themes",
    "Capture and media",
};
constexpr std::array<const char*, kCategoryCount> kCategoryNumbers = {
    "01", "02", "03", "04", "05", "06", "07"};

struct MenuSelectionBinding {
    lv_obj_t* tabview{nullptr};
    lv_obj_t* cards[kCategoryCount]{};
    lv_obj_t* accents[kCategoryCount]{};
    lv_obj_t* badges[kCategoryCount]{};
    lv_obj_t* number_labels[kCategoryCount]{};
    lv_obj_t* titles[kCategoryCount]{};
    lv_obj_t* details[kCategoryCount]{};
    lv_obj_t* open_buttons[kCategoryCount]{};
    lv_obj_t* hint_labels[kCategoryCount]{};
    lv_obj_t* position_labels[kCategoryCount]{};
    viewmodel::BaseViewModel* view_model{nullptr};
};

void cleanup_menu_selection(lv_event_t* event) {
    delete static_cast<MenuSelectionBinding*>(lv_event_get_user_data(event));
}

void detach_menu_selection(lv_event_t* event) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_event_get_user_data(event));
    if (binding) {
        binding->tabview = nullptr;
    }
}

void open_category(viewmodel::BaseViewModel* view_model, uint32_t index) {
    if (!view_model || index >= kCategoryCount) {
        return;
    }

    switch (index) {
        case 0:
            view_model->show_network_page();
            break;
        case 1:
            view_model->show_wireless_page();
            break;
        case 2:
            view_model->show_infrared_page();
            break;
        case 3:
            view_model->show_expansion_page();
            break;
        case 4:
            view_model->show_butter_page();
            break;
        case 5:
            view_model->show_tools_page();
            break;
        case 6:
            view_model->show_media_page();
            break;
        default:
            break;
    }
}

void open_selected_category(lv_event_t* event) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_event_get_user_data(event));
    if (binding && binding->tabview && binding->view_model) {
        const auto selected = lv_tabview_get_tab_active(binding->tabview);
        binding->view_model->set_menu_selection(static_cast<int32_t>(selected));
        open_category(binding->view_model, selected);
    }
}

void tabview_value_changed(lv_event_t* event) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_event_get_user_data(event));
    auto* indev = lv_indev_active();
    if (!binding || !binding->tabview || !binding->view_model ||
        binding->view_model->current_page() != model::AppPage::Apple ||
        !indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    const auto selected = lv_tabview_get_tab_active(binding->tabview);
    if (selected != static_cast<uint32_t>(binding->view_model->menu_selection())) {
        binding->view_model->set_menu_selection(static_cast<int32_t>(selected));
    }
}

void apply_menu_theme(MenuSelectionBinding* binding, int32_t selected) {
    if (!binding || !binding->tabview || !binding->view_model) {
        return;
    }

    const auto colors = view::palette(binding->view_model->theme_id());
    const int32_t active = std::clamp(selected, 0, static_cast<int32_t>(kCategoryCount - 1));
    for (int32_t index = 0; index < static_cast<int32_t>(kCategoryCount); ++index) {
        const bool is_selected = index == active;
        auto* tab_button = lv_tabview_get_tab_button(binding->tabview, index);
        if (tab_button) {
            lv_obj_set_style_radius(tab_button, 7, 0);
            lv_obj_set_style_pad_all(tab_button, 0, 0);
            lv_obj_set_style_bg_opa(tab_button, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_color(tab_button, colors.button, LV_PART_MAIN | LV_STATE_CHECKED);
            lv_obj_set_style_bg_opa(tab_button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_CHECKED);
            lv_obj_set_style_border_width(tab_button, is_selected ? 1 : 0, 0);
            lv_obj_set_style_border_color(tab_button,
                                          is_selected ? colors.primary : colors.border,
                                          0);
            auto* tab_label = lv_obj_get_child_by_type(tab_button, 0, &lv_label_class);
            if (tab_label) {
                lv_obj_set_style_text_color(tab_label,
                                            is_selected ? colors.primary : colors.text_disabled,
                                            0);
            }
        }

        if (binding->cards[index]) {
            lv_obj_set_style_bg_color(binding->cards[index], colors.surface, 0);
            lv_obj_set_style_border_color(binding->cards[index],
                                          is_selected ? colors.primary : colors.border,
                                          0);
            lv_obj_set_style_shadow_color(binding->cards[index],
                                          is_selected ? colors.primary : colors.background,
                                          0);
        }
        if (binding->accents[index]) {
            lv_obj_set_style_bg_color(binding->accents[index], colors.primary, 0);
            lv_obj_set_style_bg_opa(binding->accents[index],
                                    is_selected ? LV_OPA_COVER : LV_OPA_50,
                                    0);
        }
        if (binding->badges[index]) {
            lv_obj_set_style_bg_color(binding->badges[index], colors.button, 0);
            lv_obj_set_style_border_color(binding->badges[index], colors.primary, 0);
        }
        if (binding->number_labels[index]) {
            lv_obj_set_style_text_color(binding->number_labels[index], colors.primary, 0);
        }
        if (binding->titles[index]) {
            lv_obj_set_style_text_color(binding->titles[index], colors.text, 0);
        }
        if (binding->details[index]) {
            lv_obj_set_style_text_color(binding->details[index], colors.text_disabled, 0);
        }
        if (binding->open_buttons[index]) {
            lv_obj_set_style_bg_color(binding->open_buttons[index], colors.button, 0);
            lv_obj_set_style_border_color(binding->open_buttons[index], colors.primary, 0);
            auto* label = lv_obj_get_child_by_type(binding->open_buttons[index],
                                                   0,
                                                   &lv_label_class);
            if (label) {
                lv_obj_set_style_text_color(label, colors.primary, 0);
            }
        }
        if (binding->hint_labels[index]) {
            lv_obj_set_style_text_color(binding->hint_labels[index],
                                        colors.text_disabled,
                                        0);
        }
        if (binding->position_labels[index]) {
            lv_obj_set_style_text_color(binding->position_labels[index],
                                        is_selected ? colors.primary : colors.text_disabled,
                                        0);
        }
    }
}

void menu_selection_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_observer_get_user_data(observer));
    if (!binding || !binding->tabview) {
        return;
    }

    const int32_t selected = lv_subject_get_int(subject);
    if (selected < 0 || selected >= static_cast<int32_t>(kCategoryCount)) {
        return;
    }
    if (lv_tabview_get_tab_active(binding->tabview) != static_cast<uint32_t>(selected)) {
        lv_tabview_set_active(binding->tabview, static_cast<uint32_t>(selected), LV_ANIM_ON);
    }
    apply_menu_theme(binding, selected);
}

void menu_theme_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* binding = static_cast<MenuSelectionBinding*>(lv_observer_get_user_data(observer));
    if (binding && binding->view_model) {
        apply_menu_theme(binding, binding->view_model->menu_selection());
    }
}

} // namespace

AppleScreen::AppleScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AppleScreen::build_content(lv_obj_t* content) {
    auto* tabview = lv_tabview_create(content);
    lv_obj_set_size(tabview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(tabview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabview, 0, 0);
    lv_obj_set_style_pad_all(tabview, 0, 0);
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, 23);

    auto* tab_bar = lv_tabview_get_tab_bar(tabview);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_left(tab_bar, 4, 0);
    lv_obj_set_style_pad_right(tab_bar, 4, 0);
    lv_obj_set_style_pad_top(tab_bar, 1, 0);
    lv_obj_set_style_pad_bottom(tab_bar, 2, 0);
    lv_obj_set_style_pad_column(tab_bar, 2, 0);

    auto* tab_content = lv_tabview_get_content(tabview);
    lv_obj_set_style_bg_opa(tab_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tab_content, 0, 0);
    lv_obj_set_style_pad_all(tab_content, 0, 0);
    lv_obj_set_style_pad_row(tab_content, 0, 0);

    auto* binding = new MenuSelectionBinding{};
    binding->tabview = tabview;
    binding->view_model = &view_model();

    const lv_font_t* tab_font = assets().load_standard_font(10);
    const lv_font_t* title_font = assets().load_standard_font(16);
    const lv_font_t* detail_font = assets().load_standard_font(10);
    const lv_font_t* button_font = assets().load_standard_font(10);

    for (std::size_t index = 0; index < kCategoryCount; ++index) {
        auto* page = lv_tabview_add_tab(tabview, kTabLabels[index]);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

        auto* card = lv_obj_create(page);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(94), 64);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 3);
        lv_obj_set_style_radius(card, 11, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_shadow_width(card, 5, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        binding->cards[index] = card;

        auto* accent = lv_obj_create(card);
        lv_obj_remove_style_all(accent);
        lv_obj_set_size(accent, 4, 48);
        lv_obj_align(accent, LV_ALIGN_LEFT_MID, 7, 0);
        lv_obj_set_style_radius(accent, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(accent, LV_OBJ_FLAG_CLICKABLE);
        binding->accents[index] = accent;

        auto* badge = lv_obj_create(card);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 32, 32);
        lv_obj_align(badge, LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 1, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
        binding->badges[index] = badge;

        auto* number = lv_label_create(badge);
        lv_label_set_text(number, kCategoryNumbers[index]);
        lv_obj_set_style_text_font(number, detail_font ? detail_font : &lv_font_montserrat_10, 0);
        lv_obj_center(number);
        binding->number_labels[index] = number;

        auto* title = lv_label_create(card);
        lv_label_set_text(title, kCategoryNames[index]);
        lv_obj_set_width(title, 170);
        lv_obj_set_style_text_font(title, title_font ? title_font : &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 66, -10);
        binding->titles[index] = title;

        auto* detail = lv_label_create(card);
        lv_label_set_text(detail, kCategoryDetails[index]);
        lv_obj_set_width(detail, 170);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(detail, detail_font ? detail_font : &lv_font_montserrat_10, 0);
        lv_obj_align(detail, LV_ALIGN_LEFT_MID, 66, 10);
        binding->details[index] = detail;

        auto* open_button = lv_button_create(card);
        lv_group_remove_obj(open_button);
        lv_obj_remove_style_all(open_button);
        lv_obj_set_size(open_button, 56, 23);
        lv_obj_align(open_button, LV_ALIGN_RIGHT_MID, -7, 0);
        lv_obj_set_style_radius(open_button, 7, 0);
        lv_obj_set_style_bg_opa(open_button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(open_button, 1, 0);
        lv_obj_set_style_pad_all(open_button, 2, 0);
        binding->open_buttons[index] = open_button;
        lv_obj_add_event_cb(open_button,
                            open_selected_category,
                            LV_EVENT_CLICKED,
                            binding);

        auto* open_label = lv_label_create(open_button);
        lv_label_set_text(open_label, "OPEN");
        lv_obj_set_style_text_font(open_label,
                                   button_font ? button_font : &lv_font_montserrat_10,
                                   0);
        lv_obj_center(open_label);

        auto* hint = lv_label_create(page);
        lv_label_set_text(hint, "SWIPE OR ARROWS TO BROWSE");
        lv_obj_set_style_text_font(hint,
                                   detail_font ? detail_font : &lv_font_montserrat_10,
                                   0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 12, -3);
        binding->hint_labels[index] = hint;

        auto* position = lv_label_create(page);
        char position_text[8]{};
        std::snprintf(position_text,
                      sizeof(position_text),
                      "%02u / %02u",
                      static_cast<unsigned>(index + 1),
                      static_cast<unsigned>(kCategoryCount));
        lv_label_set_text(position, position_text);
        lv_obj_set_style_text_font(position,
                                   detail_font ? detail_font : &lv_font_montserrat_10,
                                   0);
        lv_obj_align(position, LV_ALIGN_BOTTOM_RIGHT, -12, -3);
        binding->position_labels[index] = position;

        auto* tab_button = lv_tabview_get_tab_button(tabview,
                                                     static_cast<int32_t>(index));
        if (tab_button) {
            lv_group_remove_obj(tab_button);
            if (tab_font) {
                auto* tab_label = lv_obj_get_child_by_type(tab_button, 0, &lv_label_class);
                if (tab_label) {
                    lv_obj_set_style_text_font(tab_label, tab_font, 0);
                }
            }
        }
    }

    lv_tabview_set_active(tabview,
                          static_cast<uint32_t>(view_model().menu_selection()),
                          LV_ANIM_OFF);
    apply_menu_theme(binding, view_model().menu_selection());

    lv_obj_add_event_cb(tabview,
                        tabview_value_changed,
                        LV_EVENT_VALUE_CHANGED,
                        binding);
    lv_obj_add_event_cb(tabview,
                        detach_menu_selection,
                        LV_EVENT_DELETE,
                        binding);
    lv_obj_add_event_cb(content, cleanup_menu_selection, LV_EVENT_DELETE, binding);
    lv_subject_add_observer_obj(view_model().menu_selection_subject(),
                                menu_selection_observer,
                                content,
                                binding);
    lv_subject_add_observer_obj(view_model().theme_id_subject(),
                                menu_theme_observer,
                                content,
                                binding);
}

} // namespace screen
