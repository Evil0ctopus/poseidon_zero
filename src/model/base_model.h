/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstdint>

namespace model {

enum class AppPage {
    Apple  = 0,
    Butter = 1,
};

class BaseModel {
public:
    const char* app_title() const;
    const char* greeting() const;

    bool dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    AppPage current_page() const;
    void set_current_page(AppPage page);
    void toggle_page();

    int32_t menu_selection() const;
    void set_menu_selection(int32_t index);
    void move_menu_selection(int32_t delta);

private:
    bool dark_mode_ = false;
    AppPage current_page_ = AppPage::Apple;
    int32_t menu_selection_ = 0;
};

} // namespace model
