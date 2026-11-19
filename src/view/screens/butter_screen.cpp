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
#include "storage_status.h"
#include "feature_catalog.h"
#include "tool_status.h"
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
    char file_summary[40] = "Files";
    char gateway_summary[64] = "Gateway";

    switch (view_model().current_page()) {
        case model::AppPage::Network:
            title = "NETWORK";
            {
                const auto features = platform::network_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
            }
            break;
        case model::AppPage::Wireless:
            title = "WIRELESS";
            {
                const auto features = platform::wireless_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
            }
            break;
        case model::AppPage::Infrared:
            title = "INFRARED";
            {
                const auto features = platform::infrared_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
            }
            break;
        case model::AppPage::Expansion:
            title = "EXPANSION";
            {
                const auto features = platform::expansion_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
            }
            break;
        case model::AppPage::Tools:
            title = "TOOLS";
            {
                const auto features = platform::tools_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
                const auto home = platform::list_directory("/home");
                const auto media = platform::list_directory("/media");
                std::snprintf(file_summary,
                              sizeof(file_summary),
                              "Files  %zu entries",
                              home.size() + media.size());
                tools[0] = file_summary;
            }
            break;
        case model::AppPage::Media:
            title = "MEDIA";
            {
                const auto features = platform::media_features();
                for (int32_t index = 0; index < 5; ++index) {
                    tools[index] = features[index].available ? features[index].name : "Unavailable";
                }
            }
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
    const auto storage = platform::read_storage_status();

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
        tools[4] = storage.micro_sd_present ? "Storage  available" : "Storage  no card";
    } else if (view_model().current_page() == model::AppPage::Network) {
        const auto interfaces = platform::list_network_interfaces();
        tools[0] = interfaces.empty() ? "No network interfaces" : "Interfaces  available";
        const auto gateway = platform::default_gateway();
        std::snprintf(gateway_summary,
                      sizeof(gateway_summary),
                      "Gateway  %s",
                      gateway.c_str());
        tools[1] = gateway_summary;
        tools[2] = "Ping  host";
        tools[3] = "DNS  lookup";
        tools[4] = "Port  TCP scan";
    } else if (view_model().current_page() == model::AppPage::Wireless) {
        tools[0] = wifi.present ? "Wi-Fi  available" : "Wi-Fi  unavailable";
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
