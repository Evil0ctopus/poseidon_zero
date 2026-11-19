/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "butter_screen.h"

#include "asset_manager.h"
#include "bindings.h"
#include "capabilities.h"
#include "memory_status.h"
#include "linux_network.h"
#include "theme.h"
#include "ui_const.h"
#include <cstdio>
#include <string>

namespace screen {

ButterScreen::ButterScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void ButterScreen::build_content(lv_obj_t* content) {
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(content, 5, 0);
    lv_obj_set_style_pad_row(content, 3, 0);

    auto* heading = lv_label_create(content);
    const char* title = "SYSTEM TOOLS";
    const char* tools[5] = {
        "Display",
        "Memory",
        "Storage",
        "Network",
        "Diagnostics",
    };

    switch (view_model().current_page()) {
        case model::AppPage::Network:
            title = "NETWORK";
            tools[0] = "Interfaces";
            tools[1] = "Host discovery";
            tools[2] = "Ping";
            tools[3] = "DNS lookup";
            tools[4] = "Port scan";
            break;
        case model::AppPage::Wireless:
            title = "WIRELESS";
            tools[0] = "Wi-Fi scan";
            tools[1] = "Signal status";
            tools[2] = "BLE devices";
            tools[3] = "GATT tools";
            tools[4] = "Capability gate";
            break;
        case model::AppPage::Infrared:
            title = "INFRARED";
            tools[0] = "Transmit";
            tools[1] = "Receive";
            tools[2] = "Record";
            tools[3] = "Replay";
            tools[4] = "Safety gate";
            break;
        case model::AppPage::Expansion:
            title = "EXPANSION";
            tools[0] = "Grove I2C";
            tools[1] = "UART";
            tools[2] = "SPI";
            tools[3] = "GPIO";
            tools[4] = "LoRa / radio hats";
            break;
        default:
            break;
    }

    lv_label_set_text(heading, title);
    const lv_font_t* heading_font = assets().load_standard_font(15);
    lv_obj_set_style_text_font(heading,
                               heading_font ? heading_font : &lv_font_montserrat_14,
                               0);
    reactive::bind_theme(heading, view_model().dark_mode_subject(), reactive::ThemeRole::Text);

    const auto memory = platform::read_memory_status();
    const auto wifi = platform::read_capability(platform::Capability::Wifi);
    const auto bluetooth = platform::read_capability(platform::Capability::Bluetooth);
    const auto ethernet = platform::read_capability(platform::Capability::Ethernet);
    const auto audio = platform::read_capability(platform::Capability::Audio);

    char memory_text[64];
    if (memory.mem_available_kib >= 0 && memory.mem_total_kib >= 0) {
        std::snprintf(memory_text,
                      sizeof(memory_text),
                      "Memory  %lld/%lld MB",
                      static_cast<long long>(memory.mem_available_kib / 1024),
                      static_cast<long long>(memory.mem_total_kib / 1024));
    } else {
        std::snprintf(memory_text, sizeof(memory_text), "Memory  unavailable");
    }

    if (view_model().current_page() == model::AppPage::Butter) {
        tools[0] = memory_text;
        tools[1] = wifi.present ? "Wi-Fi  available" : "Wi-Fi  unavailable";
        tools[2] = bluetooth.present ? "Bluetooth  available" : "Bluetooth  unavailable";
        tools[3] = ethernet.present ? "Ethernet  available" : "Ethernet  unavailable";
        tools[4] = audio.present ? "Audio  available" : "Audio  unavailable";
    } else if (view_model().current_page() == model::AppPage::Network) {
        const auto interfaces = platform::list_network_interfaces();
        tools[0] = interfaces.empty() ? "No network interfaces" : "Interfaces  available";
        tools[1] = "Host discovery  /24";
        tools[2] = "Ping  host";
        tools[3] = "DNS  lookup";
        tools[4] = "Port  TCP scan";
    } else if (view_model().current_page() == model::AppPage::Wireless) {
        const auto networks = platform::scan_wifi_networks();
        char wifi_count[32];
        std::snprintf(wifi_count, sizeof(wifi_count), "Wi-Fi  %zu networks", networks.size());
        tools[0] = wifi_count;
        tools[1] = wifi.present ? "Signal  available" : "Signal  unavailable";
        tools[2] = bluetooth.present ? "BLE  available" : "BLE  unavailable";
        tools[3] = "GATT  safe tools";
        tools[4] = "Radio  capability gate";
    }
    for (const char* tool : tools) {
        auto* row = lv_button_create(content);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 270, 18);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_left(row, 7, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        reactive::bind_theme(row, view_model().dark_mode_subject(), reactive::ThemeRole::Button);

        auto* label = lv_label_create(row);
        lv_label_set_text(label, tool);
        const lv_font_t* label_font =
            assets().load_standard_font(12, app::StandardFontWeight::Regular);
        lv_obj_set_style_text_font(label,
                                   label_font ? label_font : &lv_font_montserrat_12,
                                   0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        reactive::bind_theme(label, view_model().dark_mode_subject(), reactive::ThemeRole::Text);
    }
}

} // namespace screen
