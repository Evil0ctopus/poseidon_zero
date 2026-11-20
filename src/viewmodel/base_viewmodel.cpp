/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

namespace viewmodel {
namespace {

int page_to_int(model::AppPage page) {
    return static_cast<int>(page);
}

} // namespace

BaseViewModel::BaseViewModel()
    : title_subject_(model_.app_title()),
      greeting_subject_(model_.greeting()),
      dark_mode_subject_(model_.dark_mode()),
      theme_id_subject_(static_cast<int32_t>(model_.theme_id())),
      current_page_subject_(page_to_int(model_.current_page())),
      counter_subject_(0),
      bold_text_subject_(false),
      info_visible_subject_(false),
      quit_requested_subject_(false),
      menu_selection_subject_(0),
      tool_selection_subject_(0),
      grid_layout_subject_(model_.grid_layout()) {}

BaseViewModel::~BaseViewModel() = default;

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::greeting_subject() {
    return greeting_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::theme_id_subject() {
    return theme_id_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::counter_subject() {
    return counter_subject_.native();
}

lv_subject_t* BaseViewModel::bold_text_subject() {
    return bold_text_subject_.native();
}

lv_subject_t* BaseViewModel::info_visible_subject() {
    return info_visible_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

lv_subject_t* BaseViewModel::menu_selection_subject() {
    return menu_selection_subject_.native();
}

lv_subject_t* BaseViewModel::tool_selection_subject() {
    return tool_selection_subject_.native();
}

lv_subject_t* BaseViewModel::grid_layout_subject() {
    return grid_layout_subject_.native();
}

bool BaseViewModel::is_dark_mode() const {
    return model_.dark_mode();
}

void BaseViewModel::set_dark_mode(bool enabled) {
    model_.set_dark_mode(enabled);
    dark_mode_subject_.set(model_.dark_mode());
    theme_id_subject_.set(static_cast<int32_t>(model_.theme_id()));
}

void BaseViewModel::toggle_dark_mode() {
    model_.toggle_dark_mode();
    dark_mode_subject_.set(model_.dark_mode());
    theme_id_subject_.set(static_cast<int32_t>(model_.theme_id()));
}

model::ThemeId BaseViewModel::theme_id() const {
    return model_.theme_id();
}

void BaseViewModel::set_theme_id(model::ThemeId id) {
    model_.set_theme_id(id);
    dark_mode_subject_.set(model_.dark_mode());
    theme_id_subject_.set(static_cast<int32_t>(model_.theme_id()));
}

bool BaseViewModel::clock_24h() const {
    return model_.clock_24h();
}

void BaseViewModel::set_clock_24h(bool enabled) {
    model_.set_clock_24h(enabled);
}

bool BaseViewModel::grid_layout() const {
    return model_.grid_layout();
}

void BaseViewModel::set_grid_layout(bool enabled) {
    model_.set_grid_layout(enabled);
    grid_layout_subject_.set(enabled);
}

model::AppPage BaseViewModel::current_page() const {
    return model_.current_page();
}

void BaseViewModel::show_apple_page() {
    model_.set_current_page(model::AppPage::Apple);
    publish_all();
}

void BaseViewModel::show_butter_page() {
    model_.set_current_page(model::AppPage::Butter);
    publish_all();
}

void BaseViewModel::show_network_page() {
    model_.set_current_page(model::AppPage::Network);
    publish_all();
}

void BaseViewModel::show_wireless_page() {
    model_.set_current_page(model::AppPage::Wireless);
    publish_all();
}

void BaseViewModel::show_infrared_page() {
    model_.set_current_page(model::AppPage::Infrared);
    publish_all();
}

void BaseViewModel::show_expansion_page() {
    model_.set_current_page(model::AppPage::Expansion);
    publish_all();
}

void BaseViewModel::show_tools_page() {
    model_.set_current_page(model::AppPage::Tools);
    publish_all();
}

void BaseViewModel::show_media_page() {
    model_.set_current_page(model::AppPage::Media);
    publish_all();
}

void BaseViewModel::toggle_page() {
    model_.toggle_page();
    publish_all();
}

void BaseViewModel::increment_counter() {
    counter_subject_.set(counter_subject_.value() + 1);
}

void BaseViewModel::decrement_counter() {
    const auto next = counter_subject_.value() - 1;
    counter_subject_.set(next < 0 ? 0 : next);
}

void BaseViewModel::toggle_bold_text() {
    bold_text_subject_.toggle();
}

void BaseViewModel::toggle_info() {
    info_visible_subject_.toggle();
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

int32_t BaseViewModel::menu_selection() const {
    return model_.menu_selection();
}

void BaseViewModel::set_menu_selection(int32_t index) {
    model_.set_menu_selection(index);
    menu_selection_subject_.set(model_.menu_selection());
}

void BaseViewModel::move_menu_selection(int32_t delta) {
    model_.move_menu_selection(delta);
    menu_selection_subject_.set(model_.menu_selection());
}

int32_t BaseViewModel::tool_selection() const {
    return model_.tool_selection();
}

void BaseViewModel::set_tool_selection(int32_t index) {
    model_.set_tool_selection(index);
    tool_selection_subject_.set(model_.tool_selection());
}

void BaseViewModel::move_tool_selection(int32_t delta) {
    model_.move_tool_selection(delta);
    tool_selection_subject_.set(model_.tool_selection());
}

void BaseViewModel::publish_all() {
    title_subject_.set(model_.app_title());
    greeting_subject_.set(model_.greeting());
    dark_mode_subject_.set(model_.dark_mode());
    theme_id_subject_.set(static_cast<int32_t>(model_.theme_id()));
    tool_selection_subject_.set(model_.tool_selection());
    current_page_subject_.set(page_to_int(model_.current_page()));
}

} // namespace viewmodel
