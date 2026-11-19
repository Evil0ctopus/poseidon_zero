/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "capabilities.h"
#include "device_status.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "memory_status.h"
#include "screenshot_service.h"
#include "safe_tools.h"
#include "screen_manager.h"
#include "storage_status.h"
#include "base_viewmodel.h"
#include "tool_status.h"
#include "linux_network.h"
#include "feature_catalog.h"
#include "theme.h"
#include "toast.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

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
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
    }

    void update() {
        update_network_query();
    }

private:
    enum class TextToolMode {
        None,
        Calculator,
        Morse,
        Ping,
        Dns,
    };

    struct NetworkQueryState {
        std::mutex mutex;
        std::string host;
        std::string result;
        bool dns{false};
        bool complete{false};
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
        else if (text_tool_mode_ == TextToolMode::Morse) {
            message += "Morse: text or /code";
            if (!text_tool_buffer_.empty()) {
                message += "\n" + text_tool_buffer_;
            }
            message += "\nEnter=run  Esc=cancel";
        }
        else {
            message += text_tool_mode_ == TextToolMode::Ping ? "Ping host: " : "DNS name: ";
            message += text_tool_buffer_.empty() ? "_" : text_tool_buffer_;
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

    void start_network_query(const std::string& host, bool dns) {
        if (network_query_) {
            toast_->show("A network check is already running",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }

        auto state = std::make_shared<NetworkQueryState>();
        state->host = host;
        state->dns = dns;
        network_query_ = state;
        toast_->show_persistent(dns ? "Resolving hostname..." : "Checking host...");
        try {
            network_thread_ = std::thread([state] {
                std::string output;
                try {
                    output = state->dns ? platform::dns_lookup(state->host)
                                        : platform::ping_host(state->host);
                }
                catch (const std::exception& error) {
                    output = std::string("Network check failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = output;
                state->complete = true;
            });
        }
        catch (const std::system_error& error) {
            network_query_.reset();
            toast_->show("Could not start network check: " + error.code().message(),
                         view::widgets::ToastTone::Error,
                         4200);
        }
    }

    void update_network_query() {
        const auto state = network_query_;
        if (!state) {
            return;
        }

        std::string result;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->complete) {
                return;
            }
            result = state->result;
        }
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        network_query_.reset();

        if (result.empty()) {
            toast_->show("Network check returned no result",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }

        const bool succeeded =
            result.find("disabled") == std::string::npos &&
            result.find("Invalid host") == std::string::npos &&
            result.find("Network check failed:") == std::string::npos &&
            (state->dns ? result.find("getent:") == std::string::npos
                        : result.find("bytes from") != std::string::npos);
        std::size_t line_start = 0;
        if (!state->dns) {
            const std::size_t response = result.find("bytes from");
            const std::size_t packet_stats = result.find("packet loss");
            const std::size_t useful = response != std::string::npos ? response : packet_stats;
            if (useful != std::string::npos) {
                const std::size_t previous_newline = result.rfind('\n', useful);
                line_start = previous_newline == std::string::npos ? 0 : previous_newline + 1;
            }
        }
        const std::size_t line_end = result.find('\n', line_start);
        result = result.substr(line_start,
                               line_end == std::string::npos
                                   ? std::string::npos
                                   : line_end - line_start);
        if (result.size() > 84) {
            result.resize(84);
        }
        toast_->show((state->dns ? "DNS\n" : "Ping\n") + result,
                     succeeded ? view::widgets::ToastTone::Success
                               : view::widgets::ToastTone::Warning,
                     5200);
    }

    void show_stopwatch() {
        const auto now = std::chrono::steady_clock::now();
        if (stopwatch_running_) {
            stopwatch_elapsed_ += now - stopwatch_started_;
            stopwatch_running_ = false;
        }
        else {
            stopwatch_started_ = now;
            stopwatch_running_ = true;
            toast_->show_persistent("Stopwatch running\nENTER stops  BACKSPACE resets");
            return;
        }

        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(stopwatch_elapsed_).count();
        const auto hours = elapsed / 3600000;
        const auto minutes = (elapsed / 60000) % 60;
        const auto seconds = (elapsed / 1000) % 60;
        const auto milliseconds = elapsed % 1000;
        char message[80]{};
        std::snprintf(message,
                      sizeof(message),
                      "Stopwatch\n%02lld:%02lld:%02lld.%03lld",
                      static_cast<long long>(hours),
                      static_cast<long long>(minutes),
                      static_cast<long long>(seconds),
                      static_cast<long long>(milliseconds));
        toast_->show(message, view::widgets::ToastTone::Success, 5200);
    }

    void reset_stopwatch() {
        stopwatch_elapsed_ = std::chrono::steady_clock::duration::zero();
        if (stopwatch_running_) {
            stopwatch_started_ = std::chrono::steady_clock::now();
        }
        toast_->show("Stopwatch reset",
                     view::widgets::ToastTone::Default,
                     2200);
    }

    void show_mac_addresses() {
        const std::filesystem::path interfaces_root("/sys/class/net");
        std::error_code error;
        if (!std::filesystem::is_directory(interfaces_root, error) || error) {
            toast_->show("Local MAC addresses are unavailable in this simulator",
                         view::widgets::ToastTone::Warning,
                         4200);
            return;
        }

        std::string message = "Local MAC addresses";
        std::size_t count = 0;
        for (const auto& entry :
             std::filesystem::directory_iterator(interfaces_root, error)) {
            if (error) {
                break;
            }
            if (entry.path().filename() == "lo") {
                continue;
            }
            std::ifstream address_file(entry.path() / "address");
            std::string address;
            if (!std::getline(address_file, address) || address.empty()) {
                continue;
            }
            message += "\n" + entry.path().filename().string() + " " + address;
            if (++count == 3) {
                break;
            }
        }
        if (error) {
            toast_->show("Could not read local interface addresses",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        toast_->show(count ? message : "No local interface MAC addresses found",
                     count ? view::widgets::ToastTone::Default
                           : view::widgets::ToastTone::Warning,
                     5200);
    }

    void save_diagnostics() {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home || home[0] == '\0') {
            home = std::getenv("USERPROFILE");
        }
#endif
        if (!home || home[0] == '\0') {
            toast_->show("Cannot find a home folder for the support report",
                         view::widgets::ToastTone::Error,
                         4200);
            return;
        }

        const std::filesystem::path home_path(home);
        std::error_code filesystem_error;
        if (!std::filesystem::is_directory(home_path, filesystem_error) ||
            filesystem_error) {
            toast_->show("Cannot access the home folder for the support report",
                         view::widgets::ToastTone::Error,
                         4200);
            return;
        }

        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char timestamp[24]{};
        if (std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local) == 0) {
            toast_->show("Could not create a timestamp for the support report",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }

        std::filesystem::path report_path =
            home_path / (std::string("poseidon-diagnostics-") + timestamp + ".txt");
        for (unsigned int suffix = 1;
             std::filesystem::exists(report_path, filesystem_error);
             ++suffix) {
            if (filesystem_error || suffix > 99) {
                toast_->show("Could not choose a support report filename",
                             view::widgets::ToastTone::Error,
                             3200);
                return;
            }
            report_path = home_path /
                          (std::string("poseidon-diagnostics-") + timestamp + "-" +
                           std::to_string(suffix) + ".txt");
        }
        if (filesystem_error) {
            toast_->show("Could not choose a support report filename",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }

        std::ofstream report(report_path);
        if (!report) {
            toast_->show("Could not create the support report",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }

        const auto memory = platform::read_memory_status();
        const auto device = platform::read_device_status();
        const auto storage = platform::read_storage_status();
        const auto interfaces = platform::list_network_interfaces();
        report << "POSEIDON-Zero sanitized support report\n"
               << "Generated: " << timestamp << "\n"
               << "Version: 0.7.0\n"
               << "Uptime seconds: " << platform::uptime_seconds() << "\n"
               << "Memory total KiB: " << memory.mem_total_kib << "\n"
               << "Memory available KiB: " << memory.mem_available_kib << "\n"
               << "Wi-Fi present: " << (device.wifi_present ? "yes" : "no") << "\n"
               << "Wi-Fi connected: " << (device.wifi_connected ? "yes" : "no") << "\n"
               << "Ethernet present: " << (device.ethernet_present ? "yes" : "no") << "\n"
               << "Ethernet connected: "
               << (device.ethernet_connected ? "yes" : "no") << "\n"
               << "Battery present: " << (device.battery_present ? "yes" : "no") << "\n"
               << "Storage detected: " << (storage.micro_sd_present ? "yes" : "no") << "\n"
               << "Network interface count: " << interfaces.size() << "\n";
        report.flush();
        if (!report) {
            toast_->show("Could not write the support report",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        report.close();
        if (!report) {
            toast_->show("Could not finish writing the support report",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        toast_->show_highlighted("Saved to ",
                                 report_path.string(),
                                 "",
                                 view::widgets::ToastTone::Success,
                                 6000,
                                 true);
    }

    void show_system_status(int32_t selection) {
        switch (selection) {
            case 0: {
                const auto memory = platform::read_memory_status();
                if (memory.mem_available_kib < 0 || memory.mem_total_kib < 0) {
                    toast_->show("Memory status unavailable",
                                 view::widgets::ToastTone::Warning,
                                 3200);
                    return;
                }
                char message[80]{};
                std::snprintf(message,
                              sizeof(message),
                              "Memory\n%lld MB available\n%lld MB total",
                              static_cast<long long>(memory.mem_available_kib / 1024),
                              static_cast<long long>(memory.mem_total_kib / 1024));
                toast_->show(message, view::widgets::ToastTone::Default, 4200);
                return;
            }
            case 1: {
                const auto device = platform::read_device_status();
                std::string message = std::string("Wi-Fi\n") +
                                      (device.wifi_present ? "Available" : "Unavailable") +
                                      (device.wifi_connected ? "\nConnected" : "\nNot connected");
                if (device.wifi_strength_percent >= 0) {
                    message += "\nSignal " +
                               std::to_string(device.wifi_strength_percent) + "%";
                }
                toast_->show(message,
                             device.wifi_present ? view::widgets::ToastTone::Default
                                                 : view::widgets::ToastTone::Warning,
                             4200);
                return;
            }
            case 2: {
                const auto capability =
                    platform::read_capability(platform::Capability::Bluetooth);
                const std::string message =
                    std::string("Bluetooth\n") +
                    (capability.present ? "Available" : "Unavailable") +
                    (capability.enabled ? "\nEnabled" : "\nNot active");
                toast_->show(message,
                             capability.present ? view::widgets::ToastTone::Default
                                                : view::widgets::ToastTone::Warning,
                             4200);
                return;
            }
            case 3: {
                const auto device = platform::read_device_status();
                const std::string message =
                    std::string("Ethernet\n") +
                    (device.ethernet_present ? "Available" : "Unavailable") +
                    (device.ethernet_connected ? "\nConnected" : "\nNot connected");
                toast_->show(message,
                             device.ethernet_present ? view::widgets::ToastTone::Default
                                                     : view::widgets::ToastTone::Warning,
                             4200);
                return;
            }
            case 4: {
                const auto storage = platform::read_storage_status();
                if (!storage.micro_sd_present) {
                    toast_->show("Storage\nNo microSD card detected",
                                 view::widgets::ToastTone::Warning,
                                 3200);
                    return;
                }
                if (storage.total_bytes < 0 || storage.free_bytes < 0) {
                    toast_->show("Storage\nCard detected; capacity unavailable",
                                 view::widgets::ToastTone::Warning,
                                 4200);
                    return;
                }
                constexpr int64_t kBytesPerMiB = 1024 * 1024;
                char message[96]{};
                std::snprintf(message,
                              sizeof(message),
                              "Storage\n%lld MB free\n%lld MB total",
                              static_cast<long long>(storage.free_bytes / kBytesPerMiB),
                              static_cast<long long>(storage.total_bytes / kBytesPerMiB));
                toast_->show(message, view::widgets::ToastTone::Default, 4200);
                return;
            }
            case 5: {
                const auto device = platform::read_device_status();
                if (!device.battery_present) {
                    toast_->show("Battery status unavailable",
                                 view::widgets::ToastTone::Warning,
                                 3200);
                    return;
                }
                const std::string message =
                    "Battery\n" + std::to_string(device.battery_percent) + "%\n" +
                    (device.battery_charging ? "Charging" : "Not charging");
                toast_->show(message, view::widgets::ToastTone::Default, 4200);
                return;
            }
            case 6: {
                const std::string clock = platform::current_clock_text();
                toast_->show(clock.empty() ? "System clock unavailable"
                                           : "System time\n" + clock,
                             clock.empty() ? view::widgets::ToastTone::Warning
                                           : view::widgets::ToastTone::Default,
                             4200);
                return;
            }
            case 7:
                save_diagnostics();
                return;
            default:
                return;
        }
    }

    void show_network_interfaces() {
        const auto interfaces = platform::list_network_interfaces();
        if (interfaces.empty()) {
            toast_->show("No network interfaces found",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }

        std::string message = "Interfaces";
        const std::size_t count = std::min<std::size_t>(interfaces.size(), 4);
        for (std::size_t index = 0; index < count; ++index) {
            message += "\n" + interfaces[index].name +
                       (interfaces[index].connected ? "  up" : "  down");
        }
        toast_->show(message, view::widgets::ToastTone::Default, 5200);
    }

    void show_arp_table() {
        const auto entries = platform::arp_entries();
        if (entries.empty()) {
            toast_->show("Local ARP table is empty",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }
        std::string message = "Local ARP entries";
        const std::size_t count = std::min<std::size_t>(entries.size(), 2);
        for (std::size_t index = 0; index < count; ++index) {
            message += "\n" + entries[index];
        }
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

    void show_feature_status(const platform::FeatureList& features, int32_t selection) {
        const auto& feature = features[static_cast<std::size_t>(selection)];
        std::string message = std::string(feature.name) + "\n";
        if (feature.available) {
            message += feature.description;
        }
        else {
            message += std::string("Unavailable: ") + feature.description;
        }
        toast_->show(message,
                     feature.available ? view::widgets::ToastTone::Default
                                       : view::widgets::ToastTone::Warning,
                     5200);
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

            if (text_tool_mode_ == TextToolMode::Morse) {
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

            const bool dns = text_tool_mode_ == TextToolMode::Dns;
            const std::string host = text_tool_buffer_;
            cancel_text_tool();
            start_network_query(host, dns);
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
        const bool network_input =
            (key >= '0' && key <= '9') || (key >= 'a' && key <= 'z') ||
            (key >= 'A' && key <= 'Z') || key == '.' || key == '-';
        const bool allowed =
            text_tool_mode_ == TextToolMode::Calculator
                ? calculator_input
                : text_tool_mode_ == TextToolMode::Morse ? morse_input : network_input;
        if (allowed) {
            const std::size_t limit =
                text_tool_mode_ == TextToolMode::Calculator
                    ? 64
                    : text_tool_mode_ == TextToolMode::Morse ? 48 : 253;
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
                    view_model_.move_tool_selection(-2);
                }
                return true;
            }
            if (key == LV_KEY_DOWN || key == 'x') {
                if (!long_pressed) {
                    view_model_.move_tool_selection(2);
                }
                return true;
            }
            if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
                if (!long_pressed) {
                    view_model_.move_tool_selection(key == LV_KEY_LEFT ? -1 : 1);
                }
                return true;
            }
            if ((key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) &&
                view_model_.current_page() == model::AppPage::Tools &&
                view_model_.tool_selection() == 5 && !long_pressed) {
                reset_stopwatch();
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
                            case 5:
                                show_stopwatch();
                                break;
                            case 6:
                                show_mac_addresses();
                                break;
                            case 7:
                                save_diagnostics();
                                break;
                            default:
                                break;
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Butter) {
                        show_system_status(view_model_.tool_selection());
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Network) {
                        switch (view_model_.tool_selection()) {
                            case 0:
                                show_network_interfaces();
                                break;
                            case 1: {
                                const std::string gateway = platform::default_gateway();
                                toast_->show("Default gateway\n" + gateway,
                                             gateway == "No gateway"
                                                 ? view::widgets::ToastTone::Warning
                                                 : view::widgets::ToastTone::Default,
                                             4200);
                                break;
                            }
                            case 2:
                                start_text_tool(TextToolMode::Ping);
                                break;
                            case 3:
                                start_text_tool(TextToolMode::Dns);
                                break;
                            case 4:
                                toast_->show("Port scanning is disabled",
                                             view::widgets::ToastTone::Warning,
                                             3200);
                                break;
                            case 5:
                                show_system_status(3);
                                break;
                            case 6:
                                show_arp_table();
                                break;
                            case 7:
                                save_diagnostics();
                                break;
                            default:
                                toast_->show("This network action is unavailable",
                                             view::widgets::ToastTone::Warning,
                                             3200);
                                break;
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Wireless) {
                        switch (view_model_.tool_selection()) {
                            case 0:
                            case 1:
                                show_system_status(1);
                                break;
                            case 2:
                                show_system_status(2);
                                break;
                            default:
                                show_feature_status(platform::wireless_features(),
                                                    view_model_.tool_selection());
                                break;
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Infrared) {
                        show_feature_status(platform::infrared_features(),
                                            view_model_.tool_selection());
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Expansion) {
                        show_feature_status(platform::expansion_features(),
                                            view_model_.tool_selection());
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Media) {
                        show_feature_status(platform::media_features(),
                                            view_model_.tool_selection());
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
    std::shared_ptr<NetworkQueryState> network_query_;
    std::thread network_thread_;
    std::chrono::steady_clock::time_point stopwatch_started_{};
    std::chrono::steady_clock::duration stopwatch_elapsed_{};
    bool stopwatch_running_{false};
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
        shortcuts.update();
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
