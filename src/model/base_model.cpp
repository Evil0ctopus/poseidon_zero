/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "base_model.h"

#include <algorithm>

namespace model {

const char* BaseModel::app_title() const {
    return "POSEIDON";
}

const char* BaseModel::greeting() const {
    return "CardputerZero";
}

bool BaseModel::dark_mode() const {
    return dark_mode_;
}

void BaseModel::set_dark_mode(bool enabled) {
    dark_mode_ = enabled;
}

void BaseModel::toggle_dark_mode() {
    dark_mode_ = !dark_mode_;
}

AppPage BaseModel::current_page() const {
    return current_page_;
}


void BaseModel::set_current_page(AppPage page) {
    current_page_ = page;
}

void BaseModel::toggle_page() {
    current_page_ = current_page_ == AppPage::Apple ? AppPage::Butter : AppPage::Apple;
}


int32_t BaseModel::menu_selection() const {
    return menu_selection_;
}

void BaseModel::set_menu_selection(int32_t index) {
    menu_selection_ = std::clamp(index, int32_t{0}, int32_t{4});
}

void BaseModel::move_menu_selection(int32_t delta) {
    set_menu_selection(menu_selection_ + delta);
}

} // namespace model
