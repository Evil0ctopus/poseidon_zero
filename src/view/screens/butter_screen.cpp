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
#include "safe_tools.h"
#include "theme.h"
#include <cstdio>
#include <string>

namespace screen {
namespace {

struct ToolSelectionBinding {
    lv_obj_t* rows[model::kToolsPageItemCount]{};
    int32_t row_count{model::kStandardToolItemCount};
    viewmodel::BaseViewModel* view_model{nullptr};
};

void cleanup_tool_selection(lv_event_t* event) {
    delete static_cast<ToolSelectionBinding*>(lv_event_get_user_data(event));
}

void apply_tool_selection_style(ToolSelectionBinding* binding,
                                int32_t selected,
                                bool scroll_selected) {
    if (!binding || !binding->view_model) {
        return;
    }
    const auto colors = view::palette(binding->view_model->theme_id());
    for (int32_t index = 0; index < binding->row_count; ++index) {
        if (!lv_obj_is_valid(binding->rows[index])) {
            continue;
        }
        lv_obj_set_style_border_color(binding->rows[index],
                                      index == selected ? colors.primary : colors.border,
                                      0);
        lv_obj_set_style_border_width(binding->rows[index],
                                      index == selected ? 2 : 1,
                                      0);
        if (scroll_selected && index == selected) {
            lv_obj_scroll_to_view(binding->rows[index], LV_ANIM_ON);
        }
    }
}

void tool_selection_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* binding = static_cast<ToolSelectionBinding*>(lv_observer_get_user_data(observer));
    if (!binding) {
        return;
    }

    apply_tool_selection_style(binding, lv_subject_get_int(subject), true);
}

void tool_theme_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* binding = static_cast<ToolSelectionBinding*>(lv_observer_get_user_data(observer));
    if (binding && binding->view_model) {
        apply_tool_selection_style(binding,
                                   binding->view_model->tool_selection(),
                                   false);
    }
}

void tool_row_clicked(lv_event_t* event) {
    auto* binding = static_cast<ToolSelectionBinding*>(lv_event_get_user_data(event));
    auto* row = lv_event_get_target(event);
    if (!binding || !binding->view_model) {
        return;
    }

    for (int32_t index = 0; index < binding->row_count; ++index) {
        if (binding->rows[index] == row) {
            binding->view_model->set_tool_selection(index);
            return;
        }
    }
}

} // namespace

ButterScreen::ButterScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

ButterScreen::~ButterScreen() {
    if (refresh_timer_) {
        lv_timer_delete(refresh_timer_);
    }
}

void ButterScreen::refresh_tool_values() {
    const std::string clock = "Clock  " + platform::current_clock_text();
    lv_label_set_text(clock_label_, clock.c_str());
    char uptime[64];
    std::snprintf(uptime,
                  sizeof(uptime),
                  "Uptime  %lld sec",
                  static_cast<long long>(platform::uptime_seconds()));
    lv_label_set_text(uptime_label_, uptime);
}

void ButterScreen::refresh_tool_values_cb(lv_timer_t* timer) {
    auto* screen = static_cast<ButterScreen*>(lv_timer_get_user_data(timer));
    screen->refresh_tool_values();
}

void ButterScreen::build_content(lv_obj_t* content) {
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(content, 1, 0);
    lv_obj_set_style_pad_row(content, 2, 0);

    auto* heading = lv_label_create(content);
    const char* title = "SYSTEM TOOLS";
    constexpr int32_t kMaxToolCount = model::kToolsPageItemCount;
    constexpr int32_t kStandardToolCount = model::kStandardToolItemCount;
    const int32_t tool_count =
        view_model().current_page() == model::AppPage::Tools
            ? kMaxToolCount
            : view_model().current_page() == model::AppPage::Butter
                  ? model::kSystemToolItemCount
            : kStandardToolCount;
    const char* tools[kMaxToolCount] = {
        "Memory",
        "Wi-Fi",
        "Bluetooth",
        "Ethernet",
        "Storage",
        "Battery",
        "Clock",
        "Diagnostics",
        "Flashlight",
        "Dice / 8-ball",
        "Screen test",
        "Themes",
        "Ambient preview",
        "Layout toggle",
        "Screensaver toggle",
        "Screensaver picker",
        "Sound settings",
    };
    char feature_label_storage[kMaxToolCount][64]{};
    const auto set_feature_labels = [&tools, &feature_label_storage](
                                        const auto& features,
                                        int32_t count) {
        for (int32_t index = 0; index < count; ++index) {
            std::snprintf(feature_label_storage[index],
                          sizeof(feature_label_storage[index]),
                          features[index].available ? "%s" : "%s [OFF]",
                          features[index].name);
            tools[index] = feature_label_storage[index];
        }
    };
    char file_summary[40] = "Files";
    char gateway_summary[64] = "Gateway";
    char clock_summary[64] = "Clock";
    char uptime_summary[64] = "Uptime";

    switch (view_model().current_page()) {
        case model::AppPage::Network:
            title = "NETWORK";
            {
                const auto features = platform::network_features();
                set_feature_labels(features, tool_count);
            }
            break;
        case model::AppPage::Wireless:
            title = "WIRELESS";
            {
                const auto features = platform::wireless_features();
                set_feature_labels(features, tool_count);
            }
            break;
        case model::AppPage::Infrared:
            title = "INFRARED";
            {
                const auto features = platform::infrared_features();
                set_feature_labels(features, tool_count);
            }
            break;
        case model::AppPage::Expansion:
            title = "EXPANSION";
            {
                const auto features = platform::expansion_features();
                set_feature_labels(features, tool_count);
            }
            break;
        case model::AppPage::Tools:
            title = "TOOLS";
            {
                const auto features = platform::tools_features();
                set_feature_labels(features, tool_count);
                const auto home = platform::list_directory("/home");
                const auto media = platform::list_directory("/media");
                std::snprintf(file_summary,
                              sizeof(file_summary),
                              "Files  %zu entries",
                              home.size() + media.size());
                tools[0] = file_summary;
                bool valid = true;
                const double result = platform::calculate_expression("1+1", valid);
                char calculator_summary[64];
                if (valid) {
                    std::snprintf(calculator_summary,
                                  sizeof(calculator_summary),
                                  "Calculator  %.2f",
                                  result);
                } else {
                    std::snprintf(calculator_summary,
                                  sizeof(calculator_summary),
                                  "Calculator  ready");
                }
                static char calculator_storage[64];
                std::snprintf(calculator_storage,
                              sizeof(calculator_storage),
                              "%s",
                              calculator_summary);
                tools[1] = calculator_storage;
                const std::string clock = platform::current_clock_text();
                std::snprintf(clock_summary,
                              sizeof(clock_summary),
                              "Clock  %s",
                              clock.c_str());
                tools[2] = clock_summary;
                const std::string morse = platform::encode_morse("SOS");
                char morse_summary[64];
                std::snprintf(morse_summary,
                              sizeof(morse_summary),
                              "Morse  %s",
                              morse.c_str());
                static char morse_storage[64];
                std::snprintf(morse_storage,
                              sizeof(morse_storage),
                              "%s",
                              morse_summary);
                tools[3] = morse_storage;
                const std::int64_t uptime = platform::uptime_seconds();
                std::snprintf(uptime_summary,
                              sizeof(uptime_summary),
                              "Uptime  %lld sec",
                              static_cast<long long>(uptime));
                tools[4] = uptime_summary;
            }
            break;
        case model::AppPage::Media:
            title = "MEDIA";
            {
                const auto features = platform::media_features();
                set_feature_labels(features, tool_count);
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
    lv_obj_set_width(heading, 292);
    lv_obj_set_style_text_letter_space(heading, 1, 0);
    reactive::bind_theme(heading, view_model().theme_id_subject(), reactive::ThemeRole::Text);

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
        tools[5] = "Battery status";
        tools[6] = "System clock";
        tools[7] = "Display brightness";
        tools[8] = "Themes";
    } else if (view_model().current_page() == model::AppPage::Network) {
        const auto interfaces = platform::list_network_interfaces();
        tools[0] = interfaces.empty() ? "No network interfaces" : "Interfaces  available";
        const auto gateway = platform::default_gateway();
        std::snprintf(gateway_summary,
                      sizeof(gateway_summary),
                      "Gateway  %s",
                      gateway.c_str());
        tools[1] = gateway_summary;
        tools[2] = "Ping  enter host";
        tools[3] = "DNS  enter name";
        tools[4] = "Port scan  disabled";
        tools[5] = "Ethernet status";
        tools[6] = "ARP table";
        tools[7] = "Support report";
    } else if (view_model().current_page() == model::AppPage::Wireless) {
        tools[0] = wifi.present ? "Wi-Fi  available" : "Wi-Fi  unavailable";
        tools[1] = wifi.present ? "Signal  available" : "Signal  unavailable";
        tools[2] = bluetooth.present ? "Bluetooth  available" : "Bluetooth  unavailable";
        tools[3] = "GATT";
        tools[4] = "Tracker scan";
    }
    auto* list = lv_obj_create(content);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 292, 86);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(list, 3, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    reactive::bind_theme(list,
                         view_model().theme_id_subject(),
                         reactive::ThemeRole::Surface);

    lv_obj_t* row_objects[kMaxToolCount] = {};
    for (int32_t index = 0; index < tool_count; ++index) {
        auto* row = lv_button_create(list);
        row_objects[index] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 280, 18);
        lv_obj_set_style_radius(row, 3, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_left(row, 5, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        reactive::bind_theme(row,
                             view_model().theme_id_subject(),
                             reactive::ThemeRole::Button);

        auto* label = lv_label_create(row);
        lv_label_set_text(label, tools[index]);
        const lv_font_t* label_font =
            assets().load_standard_font(12, app::StandardFontWeight::Regular);
        lv_obj_set_style_text_font(label,
                                   label_font ? label_font : &lv_font_montserrat_12,
                                   0);
        lv_obj_set_width(label, 270);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
        reactive::bind_theme(label,
                             view_model().theme_id_subject(),
                             reactive::ThemeRole::Text);
        if (view_model().current_page() == model::AppPage::Tools) {
            if (index == 2) {
                clock_label_ = label;
            } else if (index == 4) {
                uptime_label_ = label;
            }
        }
    }

    auto* binding = new ToolSelectionBinding{};
    binding->view_model = &view_model();
    binding->row_count = tool_count;
    for (int32_t index = 0; index < tool_count; ++index) {
        binding->rows[index] = row_objects[index];
        lv_obj_add_event_cb(row_objects[index], tool_row_clicked, LV_EVENT_CLICKED, binding);
    }
    lv_obj_add_event_cb(content, cleanup_tool_selection, LV_EVENT_DELETE, binding);
    lv_subject_add_observer_obj(view_model().tool_selection_subject(),
                                tool_selection_observer,
                                content,
                                binding);
    lv_subject_add_observer_obj(view_model().theme_id_subject(),
                                tool_theme_observer,
                                content,
                                binding);
    lv_obj_update_layout(list);
    const int32_t selected = view_model().tool_selection();
    apply_tool_selection_style(binding, selected, false);
    if (selected >= 0 && selected < tool_count) {
        lv_obj_scroll_to_view(binding->rows[selected], LV_ANIM_OFF);
    }
    if (clock_label_ && uptime_label_) {
        refresh_timer_ = lv_timer_create(refresh_tool_values_cb, 1000, this);
        if (!refresh_timer_) {
            std::fprintf(stderr, "Unable to start tool value refresh timer\n");
        }
    }
}

} // namespace screen
