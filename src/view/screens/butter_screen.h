/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

namespace screen {

class ButterScreen : public BaseScreen {
public:
    ButterScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~ButterScreen() override;

private:
    void build_content(lv_obj_t* content) override;
    void refresh_tool_values();
    static void refresh_tool_values_cb(lv_timer_t* timer);

    lv_obj_t* clock_label_{nullptr};
    lv_obj_t* uptime_label_{nullptr};
    lv_timer_t* refresh_timer_{nullptr};
};

} // namespace screen
