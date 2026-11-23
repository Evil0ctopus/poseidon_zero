/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "apple_screen.h"

#include "asset_manager.h"
#include "theme.h"

#include <array>
#include <cstdio>

namespace screen {
namespace {

constexpr std::array<const char*, 7> kCategories = {{
    "Network", "Wireless", "Infrared", "Expansion", "System", "Tools", "Media",
}};

struct MenuBinding {
    std::array<lv_obj_t*, kCategories.size()> rows{};
    std::array<lv_obj_t*, kCategories.size()> labels{};
    lv_obj_t* heading{nullptr};
    lv_obj_t* position{nullptr};
    viewmodel::BaseViewModel* view_model{nullptr};
};

void update_menu(MenuBinding& binding) {
    const auto colors = view::palette(binding.view_model->theme_id());
    const auto selected = binding.view_model->menu_selection();
    const auto accent = binding.view_model->theme_id() == model::ThemeId::Poseidon
                            ? colors.info : colors.primary;
    lv_obj_set_style_text_color(binding.heading, colors.primary, 0);
    lv_obj_set_style_text_color(binding.position, colors.text, 0);
    lv_obj_set_style_text_opa(binding.position, LV_OPA_70, 0);
    char position[12];
    std::snprintf(position, sizeof(position), "%d / %u", selected + 1,
                  static_cast<unsigned>(kCategories.size()));
    lv_label_set_text(binding.position, position);
    for (std::size_t i = 0; i < binding.rows.size(); ++i) {
        const bool active = static_cast<int32_t>(i) == selected;
        lv_obj_set_style_bg_color(binding.rows[i], colors.button, 0);
        lv_obj_set_style_bg_opa(binding.rows[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(binding.rows[i], active ? 1 : 0, 0);
        lv_obj_set_style_border_color(binding.rows[i], accent, 0);
        lv_obj_set_style_text_color(binding.labels[i], colors.text, 0);
        lv_obj_set_style_text_opa(binding.labels[i], active ? LV_OPA_COVER : LV_OPA_70, 0);
    }
}

void menu_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* binding = static_cast<MenuBinding*>(lv_observer_get_user_data(observer));
    if (binding) {
        update_menu(*binding);
    }
}

void menu_clicked(lv_event_t* event) {
    auto* binding = static_cast<MenuBinding*>(lv_event_get_user_data(event));
    if (!binding) {
        return;
    }
    for (std::size_t i = 0; i < binding->rows.size(); ++i) {
        if (binding->rows[i] != lv_event_get_target(event)) {
            continue;
        }
        binding->view_model->set_menu_selection(static_cast<int32_t>(i));
        switch (i) {
            case 0: binding->view_model->show_network_page(); break;
            case 1: binding->view_model->show_wireless_page(); break;
            case 2: binding->view_model->show_infrared_page(); break;
            case 3: binding->view_model->show_expansion_page(); break;
            case 4: binding->view_model->show_butter_page(); break;
            case 5: binding->view_model->show_tools_page(); break;
            case 6: binding->view_model->show_media_page(); break;
        }
        return;
    }
}

void delete_binding(lv_event_t* event) {
    delete static_cast<MenuBinding*>(lv_event_get_user_data(event));
}

} // namespace

AppleScreen::AppleScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void AppleScreen::build_content(lv_obj_t* content) {
    auto* binding = new MenuBinding{};
    binding->view_model = &view_model();
    binding->heading = lv_label_create(content);
    lv_label_set_text(binding->heading, "MAIN MENU");
    lv_obj_set_style_text_font(binding->heading, &lv_font_montserrat_12, 0);
    lv_obj_align(binding->heading, LV_ALIGN_TOP_LEFT, 12, 0);

    binding->position = lv_label_create(content);
    lv_obj_set_style_text_font(binding->position, &lv_font_montserrat_10, 0);
    lv_obj_align(binding->position, LV_ALIGN_TOP_RIGHT, -12, 1);

    for (std::size_t i = 0; i < kCategories.size(); ++i) {
        auto* row = lv_button_create(content);
        lv_group_remove_obj(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 296, 13);
        lv_obj_set_pos(row, 12, 16 + static_cast<int32_t>(i) * 13);
        lv_obj_add_event_cb(row, menu_clicked, LV_EVENT_CLICKED, binding);
        binding->rows[i] = row;

        auto* label = lv_label_create(row);
        lv_label_set_text_fmt(label, "  %s  >", kCategories[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 2, 0);
        binding->labels[i] = label;
    }
    lv_subject_add_observer_obj(view_model().menu_selection_subject(), menu_observer,
                                content, binding);
    lv_subject_add_observer_obj(view_model().theme_id_subject(), menu_observer,
                                content, binding);
    lv_obj_add_event_cb(content, delete_binding, LV_EVENT_DELETE, binding);
    update_menu(*binding);
}

} // namespace screen
