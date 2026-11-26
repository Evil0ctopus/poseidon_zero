/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "help_popup.h"

#include "asset_manager.h"
#include "base_viewmodel.h"
#include "theme.h"

#include <array>
#include <cstddef>

namespace view::widgets {
namespace {

struct HelpRow {
    const char* key;
    const char* action;
};

constexpr std::array<HelpRow, 5> kAppleRows = {{
    {"ARROWS", "Move"},
    {"ENTER", "Open"},
    {"4 / ESC", "Hold to exit"},
    {"5 / 6", "Text / theme"},
    {"7 / 8", "Info / system"},
}};

constexpr std::array<HelpRow, 5> kMenuRows = {{
    {"UP / DOWN", "Move"},
    {"Z / C", "Prev / next"},
    {"ENTER", "Open"},
    {"ESC", "Back"},
    {"0", "Favorites"},
}};

constexpr std::array<HelpRow, 5> kWirelessRows = {{
    {"UP / DOWN", "Move"},
    {"Z / C", "Prev / next"},
    {"LETTER", "Jump"},
    {"TAB", "Switch lists"},
    {"ENTER", "Open"},
}};

} // namespace

HelpPopup::HelpPopup(lv_obj_t* parent,
                     viewmodel::BaseViewModel& view_model,
                     app::AssetManager& assets)
    : BaseWidgets(parent), view_model_(view_model), assets_(assets) {}

void HelpPopup::build() {
    if (core_obj_ || !parent_) {
        return;
    }

    core_obj_ = lv_obj_create(parent_);
    lv_obj_remove_style_all(core_obj_);
    lv_obj_set_size(core_obj_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(core_obj_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(core_obj_, LV_OPA_50, 0);
    lv_obj_add_flag(core_obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(core_obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(core_obj_, backdrop_clicked_cb, LV_EVENT_CLICKED, this);

    panel_ = lv_obj_create(core_obj_);
    lv_obj_remove_style_all(panel_);
    lv_obj_set_size(panel_, 320, 170);
    lv_obj_center(panel_);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel_, 0, 0);
    lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_CLICKABLE);

    title_ = lv_label_create(panel_);
    lv_label_set_text(title_, "HELP");
    auto* title_font = assets_.load_standard_font(15);
    lv_obj_set_style_text_font(title_, title_font ? title_font : &lv_font_montserrat_14, 0);
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 12, 9);

    page_badge_ = lv_label_create(panel_);
    auto* badge_font = assets_.load_standard_font(11);
    lv_obj_set_style_text_font(page_badge_, badge_font ? badge_font : &lv_font_montserrat_12, 0);
    lv_obj_align(page_badge_, LV_ALIGN_TOP_RIGHT, -12, 10);

    rule_ = lv_obj_create(panel_);
    lv_obj_remove_style_all(rule_);
    lv_obj_set_size(rule_, 296, 1);
    lv_obj_align(rule_, LV_ALIGN_TOP_MID, 0, 29);
    lv_obj_set_style_bg_opa(rule_, LV_OPA_70, 0);

    keys_heading_ = lv_label_create(panel_);
    lv_label_set_text(keys_heading_, "KEYS");
    lv_obj_set_style_text_font(keys_heading_, &lv_font_montserrat_10, 0);
    lv_obj_align(keys_heading_, LV_ALIGN_TOP_LEFT, 12, 36);

    legend_heading_ = lv_label_create(panel_);
    lv_label_set_text(legend_heading_, "LEGEND");
    lv_obj_set_style_text_font(legend_heading_, &lv_font_montserrat_10, 0);
    lv_obj_align(legend_heading_, LV_ALIGN_TOP_LEFT, 170, 36);

    legend_ = lv_label_create(panel_);
    lv_label_set_text(legend_, "Available   Working\n[OFF]       Unavailable\n\n? / ESC  Close");
    lv_obj_set_style_text_font(legend_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_line_space(legend_, 3, 0);
    lv_obj_align(legend_, LV_ALIGN_TOP_LEFT, 170, 52);

    auto* rows = lv_obj_create(panel_);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, 152, 86);
    lv_obj_align(rows, LV_ALIGN_TOP_LEFT, 12, 51);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(rows, 1, 0);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);

    auto* key_font = assets_.load_standard_font(11);
    auto* action_font = assets_.load_standard_font(12, app::StandardFontWeight::Regular);
    for (auto& widgets : rows_) {
        widgets.row = lv_obj_create(rows);
        lv_obj_remove_style_all(widgets.row);
        lv_obj_set_size(widgets.row, LV_PCT(100), 16);
        lv_obj_clear_flag(widgets.row, LV_OBJ_FLAG_SCROLLABLE);

        widgets.key = lv_label_create(widgets.row);
        lv_obj_set_width(widgets.key, 76);
        lv_label_set_long_mode(widgets.key, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(widgets.key, key_font ? key_font : &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_align(widgets.key, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(widgets.key, LV_ALIGN_LEFT_MID, 0, 0);

        widgets.action = lv_label_create(widgets.row);
        lv_obj_set_width(widgets.action, 76);
        lv_label_set_long_mode(widgets.action, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(widgets.action,
                                   action_font ? action_font : &lv_font_montserrat_12,
                                   0);
        lv_obj_align(widgets.action, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    footer_ = lv_label_create(panel_);
    lv_label_set_text(footer_, "Local tools may use network. Authorized use only.");
    lv_label_set_long_mode(footer_, LV_LABEL_LONG_CLIP);
    auto* footer_font = assets_.load_standard_font(10);
    lv_obj_set_style_text_font(footer_, footer_font ? footer_font : &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(footer_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_size(footer_, LV_SIZE_CONTENT, 12);
    lv_obj_align(footer_, LV_ALIGN_BOTTOM_LEFT, 12, -10);

    lv_subject_add_observer_obj(view_model_.theme_id_subject(),
                                theme_observer_cb,
                                core_obj_,
                                this);
    apply_theme();
    hide();
}

void HelpPopup::show(model::AppPage page) {
    if (!core_obj_) {
        build();
    }
    if (!core_obj_) {
        return;
    }
    update_rows(page);
    apply_theme();
    lv_obj_move_foreground(core_obj_);
    lv_obj_remove_flag(core_obj_, LV_OBJ_FLAG_HIDDEN);
}

void HelpPopup::hide() {
    if (core_obj_ && lv_obj_is_valid(core_obj_)) {
        lv_obj_add_flag(core_obj_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool HelpPopup::visible() const {
    return core_obj_ && lv_obj_is_valid(core_obj_) &&
           !lv_obj_has_flag(core_obj_, LV_OBJ_FLAG_HIDDEN);
}

void HelpPopup::update_rows(model::AppPage page) {
    const bool home = page == model::AppPage::Apple;
    lv_label_set_text(page_badge_, "? / ENTER / ESC to close");
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const auto& help_rows = home ? kAppleRows
                                : page == model::AppPage::Wireless ? kWirelessRows : kMenuRows;
        const bool row_visible = i < help_rows.size();
        if (!row_visible) {
            lv_obj_add_flag(rows_[i].row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(rows_[i].row, LV_OBJ_FLAG_HIDDEN);
        const auto& row = help_rows[i];
        lv_label_set_text(rows_[i].key, row.key);
        lv_label_set_text(rows_[i].action, row.action);
    }
    lv_label_set_text(footer_, page == model::AppPage::Wireless
                                   ? "ESC back; Shift+C/X/Z jump; authorized use."
                                   : "Local tools may use network. Authorized use only.");
}

void HelpPopup::apply_theme() {
    if (!panel_) {
        return;
    }
    const auto colors = view::palette(view_model_.theme_id());
    lv_obj_set_style_bg_color(panel_, colors.background, 0);
    lv_obj_set_style_text_color(title_, colors.primary, 0);
    lv_obj_set_style_bg_color(rule_, colors.info, 0);
    lv_obj_set_style_text_color(page_badge_, colors.text, 0);
    lv_obj_set_style_text_opa(page_badge_, LV_OPA_70, 0);
    lv_obj_set_style_text_color(keys_heading_, colors.primary, 0);
    lv_obj_set_style_text_color(legend_heading_, colors.success, 0);
    lv_obj_set_style_text_color(legend_, colors.text, 0);
    lv_obj_set_style_text_color(footer_, colors.success, 0);
    for (auto& row : rows_) {
        lv_obj_set_style_text_color(row.key, colors.primary, 0);
        lv_obj_set_style_text_color(row.action, colors.text, 0);
    }
}

void HelpPopup::backdrop_clicked_cb(lv_event_t* event) {
    auto* popup = static_cast<HelpPopup*>(lv_event_get_user_data(event));
    if (popup && lv_event_get_target(event) == popup->core_obj_) {
        popup->hide();
    }
}

void HelpPopup::theme_observer_cb(lv_observer_t* observer, lv_subject_t*) {
    auto* popup = static_cast<HelpPopup*>(lv_observer_get_user_data(observer));
    if (popup) {
        popup->apply_theme();
    }
}

} // namespace view::widgets
