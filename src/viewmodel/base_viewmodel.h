/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "subjects.h"
#include "base_model.h"

#include "lvgl.h"

namespace viewmodel {

class BaseViewModel {
public:
    BaseViewModel();
    ~BaseViewModel();

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    lv_subject_t* title_subject();
    lv_subject_t* greeting_subject();
    lv_subject_t* dark_mode_subject();
    lv_subject_t* theme_id_subject();
    lv_subject_t* current_page_subject();
    lv_subject_t* counter_subject();
    lv_subject_t* bold_text_subject();
    lv_subject_t* info_visible_subject();
    lv_subject_t* quit_requested_subject();
    lv_subject_t* menu_selection_subject();
    lv_subject_t* tool_selection_subject();
    lv_subject_t* grid_layout_subject();

    bool is_dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();
    model::ThemeId theme_id() const;
    void set_theme_id(model::ThemeId id);
    bool clock_24h() const;
    void set_clock_24h(bool enabled);
    bool grid_layout() const;
    void set_grid_layout(bool enabled);

    model::AppPage current_page() const;
    void show_apple_page();
    void show_butter_page();
    void show_network_page();
    void show_wireless_page();
    void show_infrared_page();
    void show_expansion_page();
    void show_tools_page();
    void show_media_page();
    void toggle_page();
    void increment_counter();
    void decrement_counter();
    void toggle_bold_text();
    void toggle_info();
    void request_quit();
    int32_t menu_selection() const;
    void set_menu_selection(int32_t index);
    void move_menu_selection(int32_t delta);
    int32_t tool_selection() const;
    void set_tool_selection(int32_t index);
    void move_tool_selection(int32_t delta);

private:
    void publish_all();

    model::BaseModel model_;
    reactive::StringSubject<32> title_subject_;
    reactive::StringSubject<32> greeting_subject_;
    reactive::BoolSubject dark_mode_subject_;
    reactive::IntSubject theme_id_subject_;
    reactive::IntSubject current_page_subject_;
    reactive::IntSubject counter_subject_;
    reactive::BoolSubject bold_text_subject_;
    reactive::BoolSubject info_visible_subject_;
    reactive::BoolSubject quit_requested_subject_;
    reactive::IntSubject menu_selection_subject_;
    reactive::IntSubject tool_selection_subject_;
    reactive::BoolSubject grid_layout_subject_;
};

} // namespace viewmodel
