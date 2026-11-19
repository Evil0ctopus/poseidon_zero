/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "screenshot_service.h"
#include "safe_tools.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "tool_status.h"
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "poseidon-zero.conf"
#endif

namespace app {
namespace {

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

struct DarkModePersistence {
    std::string config_path;
    bool last_dark_mode;
};

std::string writable_config_path() {
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "poseidon-zero" / "poseidon-zero.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "poseidon-zero" / "poseidon-zero.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

void persist_dark_mode_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<DarkModePersistence*>(lv_observer_get_user_data(observer));
    if (!persistence) {
        return;
    }
    const bool dark_mode = lv_subject_get_int(subject) != 0;
    if (dark_mode == persistence->last_dark_mode) {
        return;
    }
    persistence->last_dark_mode = dark_mode;

    ApplicationConfig config;
    config.dark_mode = dark_mode;
    std::string error;
    if (save_application_config(persistence->config_path, config, error)) {
        LOG_INFO("saved config: {} (dark_mode={})",
                 persistence->config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to save config {}: {}", persistence->config_path, error);
    }
}

class ShortcutController {
public:
    ShortcutController(viewmodel::BaseViewModel& view_model, AssetManager& assets)
        : view_model_(view_model),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)),
          help_popup_(std::make_unique<view::widgets::HelpPopup>(lv_layer_top(), view_model, assets)) {
        toast_->build();
        help_popup_->build();
        platform::set_global_key_listener(global_key_listener, this);
        platform::set_key_release_listener(key_release_listener, this);
    }

    ~ShortcutController() {
        platform::clear_key_release_listener(key_release_listener, this);
        platform::clear_global_key_listener(global_key_listener, this);
    }

private:
    enum class TextToolMode {
        None,
        Calculator,
        Morse,
    };

    void show_text_tool_prompt(view::widgets::ToastTone tone =
                                   view::widgets::ToastTone::Default,
                               const std::string& notice = {}) {
        std::string message;
        if (!notice.empty()) {
            message = notice + "\n";
        }
        if (text_tool_mode_ == TextToolMode::Calculator) {
            message += "Calc: ";
            message += text_tool_buffer_.empty() ? "_" : text_tool_buffer_;
            message += "\nEnter=solve  Esc=cancel";
        }
        else {
            message += "Morse: text or /code";
            if (!text_tool_buffer_.empty()) {
                message += "\n" + text_tool_buffer_;
            }
            message += "\nEnter=run  Esc=cancel";
        }
        toast_->show_persistent(message, tone);
    }

    void start_text_tool(TextToolMode mode) {
        text_tool_mode_ = mode;
        text_tool_buffer_.clear();
        show_text_tool_prompt();
    }

    void cancel_text_tool() {
        text_tool_mode_ = TextToolMode::None;
        text_tool_buffer_.clear();
        toast_->hide();
    }

    void show_file_summary() {
        const char* home = std::getenv("HOME");
        const std::string home_path = home && home[0] != '\0' ? home : "/home";
        const auto home_entries = platform::list_directory(home_path);
        const auto media_entries = platform::list_directory("/media");
        const std::string message =
            "Files\nHome: " + std::to_string(home_entries.size()) +
            " entries\nMedia: " + std::to_string(media_entries.size()) + " entries";
        toast_->show(message, view::widgets::ToastTone::Default, 5200);
    }

    void show_uptime() {
        const auto seconds = platform::uptime_seconds();
        if (seconds <= 0) {
            toast_->show("System uptime unavailable",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }

        const auto days = seconds / 86400;
        const auto hours = (seconds / 3600) % 24;
        const auto minutes = (seconds / 60) % 60;
        const auto remaining_seconds = seconds % 60;
        char message[64]{};
        std::snprintf(message,
                      sizeof(message),
                      "Uptime %lldd %02lld:%02lld:%02lld",
                      static_cast<long long>(days),
                      static_cast<long long>(hours),
                      static_cast<long long>(minutes),
                      static_cast<long long>(remaining_seconds));
        toast_->show(message, view::widgets::ToastTone::Default, 4200);
    }

    bool handle_text_tool_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC) {
            cancel_text_tool();
            return true;
        }
        if (key == LV_KEY_ENTER) {
            if (text_tool_buffer_.empty()) {
                show_text_tool_prompt(view::widgets::ToastTone::Warning,
                                      "Enter a value");
                return true;
            }

            if (text_tool_mode_ == TextToolMode::Calculator) {
                bool valid = false;
                const double result = platform::calculate_expression(text_tool_buffer_, valid);
                if (!valid) {
                    show_text_tool_prompt(view::widgets::ToastTone::Error,
                                          "Invalid expression");
                    return true;
                }

                char result_text[48]{};
                std::snprintf(result_text, sizeof(result_text), "%.10g", result);
                const std::string message = text_tool_buffer_ + " = " + result_text;
                cancel_text_tool();
                toast_->show(message, view::widgets::ToastTone::Success, 4200);
                return true;
            }

            const bool decode = text_tool_buffer_.front() == '/';
            const std::string result =
                decode ? platform::decode_morse(text_tool_buffer_.substr(1))
                       : platform::encode_morse(text_tool_buffer_);
            if (result.empty()) {
                show_text_tool_prompt(view::widgets::ToastTone::Error,
                                      "No Morse output");
                return true;
            }

            cancel_text_tool();
            toast_->show_highlighted("Morse: ",
                                     result,
                                     "",
                                     view::widgets::ToastTone::Success,
                                     6000,
                                     true);
            return true;
        }
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (!text_tool_buffer_.empty()) {
                text_tool_buffer_.pop_back();
                show_text_tool_prompt();
            }
            return true;
        }

        const bool calculator_input =
            (key >= '0' && key <= '9') || key == '.' || key == '+' || key == '-' ||
            key == '*' || key == '/' || key == '(' || key == ')';
        const bool morse_input =
            (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
            key == ' ' || key == '.' || key == '-' || key == '/';
        const bool allowed = text_tool_mode_ == TextToolMode::Calculator
                                 ? calculator_input
                                 : morse_input;
        if (allowed) {
            const std::size_t limit =
                text_tool_mode_ == TextToolMode::Calculator ? 64 : 48;
            if (text_tool_buffer_.size() >= limit) {
                show_text_tool_prompt(view::widgets::ToastTone::Warning,
                                      "Input limit reached");
                return true;
            }
            text_tool_buffer_.push_back(static_cast<char>(key));
            show_text_tool_prompt();
        }
        return true;
    }

    bool handle_key(uint32_t key, bool long_pressed) {
        if (text_tool_mode_ != TextToolMode::None && key == platform::kKeyHelp) {
            cancel_text_tool();
        }
        else if (text_tool_mode_ != TextToolMode::None &&
                 key != platform::kKeyPrintScreen) {
            return handle_text_tool_key(key, long_pressed);
        }

        if (view_model_.current_page() == model::AppPage::Apple) {
            if (key == LV_KEY_UP || key == 'f') {
                if (!long_pressed) {
                    view_model_.move_menu_selection(-2);
                }
                return true;
            }
            if (key == LV_KEY_DOWN || key == 'x') {
                if (!long_pressed) {
                    view_model_.move_menu_selection(2);
                }
                return true;
            }
            if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
                if (!long_pressed) {
                    const int32_t selection = view_model_.menu_selection();
                    if (key == LV_KEY_LEFT && selection % 2 == 1) {
                        view_model_.set_menu_selection(selection - 1);
                    }
                    else if (key == LV_KEY_RIGHT && selection % 2 == 0 &&
                             selection < 6) {
                        view_model_.set_menu_selection(selection + 1);
                    }
                }
                return true;
            }
            if (key == LV_KEY_ENTER) {
                if (!long_pressed) {
                    const int32_t selection = view_model_.menu_selection();
                    switch (selection) {
                        case 0:
                            view_model_.show_network_page();
                            break;
                        case 1:
                            view_model_.show_wireless_page();
                            break;
                        case 2:
                            view_model_.show_infrared_page();
                            break;
                        case 3:
                            view_model_.show_expansion_page();
                            break;
                        case 4:
                            view_model_.show_butter_page();
                            break;
                        case 5:
                            view_model_.show_tools_page();
                            break;
                        case 6:
                            view_model_.show_media_page();
                            break;
                    }
                }
                return true;
            }
        }

        if (view_model_.current_page() != model::AppPage::Apple) {
            if (key == LV_KEY_UP || key == 'f') {
                if (!long_pressed) {
                    view_model_.move_tool_selection(-1);
                }
                return true;
            }
            if (key == LV_KEY_DOWN || key == 'x') {
                if (!long_pressed) {
                    view_model_.move_tool_selection(1);
                }
                return true;
            }
            if (key == LV_KEY_ENTER) {
                if (!long_pressed) {
                    if (view_model_.current_page() == model::AppPage::Tools) {
                        switch (view_model_.tool_selection()) {
                            case 0:
                                show_file_summary();
                                break;
                            case 1:
                                start_text_tool(TextToolMode::Calculator);
                                break;
                            case 2: {
                                const std::string clock = platform::current_clock_text();
                                toast_->show(clock.empty() ? "System clock unavailable"
                                                           : "System time\n" + clock,
                                             clock.empty()
                                                 ? view::widgets::ToastTone::Warning
                                                 : view::widgets::ToastTone::Default,
                                             4200);
                                break;
                            }
                            case 3:
                                start_text_tool(TextToolMode::Morse);
                                break;
                            case 4:
                                show_uptime();
                                break;
                            default:
                                break;
                        }
                        return true;
                    }
                    toast_->show("This action is not implemented yet",
                                 view::widgets::ToastTone::Warning,
                                 2200);
                }
                return true;
            }
        }

        if (key == platform::kKeyPrintScreen) {
            if (!long_pressed) {
                const auto result = platform::screenshot::capture_active_screen();
                if (result.success) {
                    LOG_INFO("screenshot saved: {}", result.path);
                    toast_->show_highlighted("Saved to ",
                                             result.path,
                                             "",
                                             view::widgets::ToastTone::Success,
                                             5200,
                                             true);
                }
                else {
                    LOG_WARN("screenshot failed: {}", result.error);
                    toast_->show("Screenshot failed: " + result.error,
                                 view::widgets::ToastTone::Error,
                                 3200);
                }
            }
            return true;
        }

        if (key == platform::kKeyHelp) {
            if (!long_pressed) {
                toast_->hide();
                if (help_popup_->visible()) {
                    help_popup_->hide();
                }
                else {
                    help_popup_->show(view_model_.current_page());
                }
            }
            return true;
        }

        if (help_popup_->visible()) {
            if (key == LV_KEY_ESC && !long_pressed) {
                help_popup_->hide();
            }
            return true;
        }

        const bool is_escape = key == LV_KEY_ESC;
        const bool is_home_exit_key =
            key == '4' &&
            ((!long_pressed && view_model_.current_page() == model::AppPage::Apple) ||
             (long_pressed && exit_key_pressed_ == key));
        if (!is_escape && !is_home_exit_key) {
            return false;
        }

        if (long_pressed) {
            if (exit_key_pressed_ == key && exit_armed_) {
                toast_->hide();
                view_model_.request_quit();
            }
        }
        else {
            exit_key_pressed_ = key;
            exit_armed_ = view_model_.current_page() == model::AppPage::Apple;
            if (exit_armed_) {
                toast_->show_persistent_highlighted("Hold ",
                                                    "ESC",
                                                    " to exit",
                                                    view::widgets::ToastTone::Warning);
            }
        }
        return true;
    }

    void handle_release(uint32_t key) {
        if (key != exit_key_pressed_) {
            return;
        }

        if (exit_armed_) {
            toast_->hide();
        } else if (key == LV_KEY_ESC &&
                   view_model_.current_page() != model::AppPage::Apple) {
            view_model_.show_apple_page();
        }
        exit_key_pressed_ = 0;
        exit_armed_ = false;
    }

    static bool global_key_listener(uint32_t key,
                                    const char*,
                                    bool long_pressed,
                                    void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        return controller && controller->handle_key(key, long_pressed);
    }

    static void key_release_listener(uint32_t key, const char*, void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        if (controller) {
            controller->handle_release(key);
        }
    }

    viewmodel::BaseViewModel& view_model_;
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::string text_tool_buffer_;
    TextToolMode text_tool_mode_{TextToolMode::None};
    uint32_t exit_key_pressed_{0};
    bool exit_armed_{false};
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("poseidon-zero");

    lv_init();

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    viewmodel::BaseViewModel view_model;
    const bool store_capture_mode =
        std::getenv("POSEIDON_STORE_CAPTURE") != nullptr;
    const std::string user_config_path = writable_config_path();
    std::string loaded_config_path = APP_CONFIG_FILE;
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error)) {
            loaded_config_path = user_config_path;
        }
    }

    ApplicationConfig config;
    std::string config_error;
    if (load_application_config(loaded_config_path, config, config_error)) {
        view_model.set_dark_mode(config.dark_mode);
        LOG_INFO("loaded config: {} (dark_mode={})",
                 loaded_config_path,
                 config.dark_mode ? "yes" : "no");
    } else {
        LOG_WARN("failed to load config {}: {}; using defaults", loaded_config_path, config_error);
    }
    if (store_capture_mode) {
        view_model.set_dark_mode(false);
    }

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }

    auto* standard_font = assets.load_standard_font(14);
    view::apply_lvgl_theme(display,
                           view_model.is_dark_mode(),
                           standard_font ? standard_font : LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    ScreenManager screen_manager(view_model, assets);
    screen_manager.start();
    ShortcutController shortcuts(view_model, assets);

    DarkModePersistence dark_mode_persistence{user_config_path, view_model.is_dark_mode()};
    auto* dark_mode_observer =
        store_capture_mode
            ? nullptr
            : lv_subject_add_observer(view_model.dark_mode_subject(),
                                      persist_dark_mode_observer,
                                      &dark_mode_persistence);

    bool running = true;
    int exit_code = 0;
    int capture_phase = 0;
    int settled_frames = 0;
    auto* quit_observer = lv_subject_add_observer(view_model.quit_requested_subject(),
                                                  quit_requested_observer,
                                                  &running);

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        lv_timer_handler();
        if (store_capture_mode && ++settled_frames >= 20) {
            const auto capture = platform::screenshot::capture_active_screen();
            if (!capture.success) {
                LOG_ERROR("store screenshot capture failed: {}", capture.error);
                exit_code = 1;
                running = false;
            }
            else if (capture_phase == 0) {
                capture_phase = 1;
                settled_frames = 0;
                view_model.set_dark_mode(true);
            }
            else {
                running = false;
            }
        }
        lv_delay_ms(5);
    }

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    if (dark_mode_observer) {
        lv_observer_remove(dark_mode_observer);
    }

    return exit_code;
}

} // namespace app
