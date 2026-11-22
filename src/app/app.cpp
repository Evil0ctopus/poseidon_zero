/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "backlight.h"
#include "bluetooth_gatt.h"
#include "camera_capture.h"
#include "capabilities.h"
#include "device_status.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "memory_status.h"
#include "microphone.h"
#include "screenshot_service.h"
#include "safe_tools.h"
#include "screen_manager.h"
#include "storage_status.h"
#include "base_viewmodel.h"
#include "tool_status.h"
#include "ui_sound.h"
#include "usb_keyboard_simulator.h"
#include "wifi_scan.h"
#include "ir_receive.h"
#include "linux_network.h"
#include "feature_catalog.h"
#include "gallery_image.h"
#include "theme.h"
#include "toast.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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

struct ThemePersistence {
    std::string config_path;
    ApplicationConfig config;
    int32_t last_theme_id;
    bool previewing{false};
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

bool persist_theme(ThemePersistence& persistence, int32_t theme_id) {
    if (theme_id < 0 || theme_id >= model::kThemeCount) {
        return false;
    }
    if (theme_id == persistence.last_theme_id) {
        return true;
    }
    persistence.config.theme_id = theme_id;
    persistence.config.theme_id_set = true;
    persistence.config.dark_mode =
        view::theme_is_dark(static_cast<model::ThemeId>(theme_id));
    std::string error;
    if (!save_application_config(persistence.config_path, persistence.config, error)) {
        LOG_WARN("failed to save config {}: {}", persistence.config_path, error);
        return false;
    }
    persistence.last_theme_id = theme_id;
    LOG_INFO("saved config: {} (theme_id={})", persistence.config_path, theme_id);
    return true;
}

bool persist_clock_format(ThemePersistence& persistence, bool clock_24h) {
    persistence.config.clock_24h = clock_24h;
    std::string error;
    if (!save_application_config(persistence.config_path, persistence.config, error)) {
        LOG_WARN("failed to save clock format {}: {}", persistence.config_path, error);
        return false;
    }
    LOG_INFO("saved clock format: {} (clock_24h={})",
             persistence.config_path,
             clock_24h);
    return true;
}

void persist_theme_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<ThemePersistence*>(lv_observer_get_user_data(observer));
    if (!persistence || persistence->previewing) {
        return;
    }
    persist_theme(*persistence, lv_subject_get_int(subject));
}

class BootSplash {
public:
    explicit BootSplash(AssetManager& assets) : assets_(assets) {}

    ~BootSplash() { finish(); }

    void start(bool reduced_motion) {
        reduced_motion_ = reduced_motion;
        const auto path = assets_.resolve("images/poseidon-boot.png");
        if (path.empty()) {
            LOG_WARN("boot logo asset not found");
        } else {
            auto image = platform::read_gallery_image(path);
            if (!image.error.empty()) {
                LOG_WARN("failed to load boot logo: {}", image.error);
            } else {
                pixels_ = std::move(image.pixels);
                image_.header.magic = LV_IMAGE_HEADER_MAGIC;
                image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
                image_.header.w = image.width;
                image_.header.h = image.height;
                image_.header.stride = image.width * 4U;
                image_.data_size = static_cast<uint32_t>(pixels_.size());
                image_.data = pixels_.data();
            }
        }

        root_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, view::kScreenWidth, view::kScreenHeight);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x050308), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

        status_ = lv_label_create(root_);
        lv_label_set_text(status_, "POSEIDON\n\nInitializing display\nLoading interface");
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(0x5ee6f5), 0);
        lv_obj_align(status_, LV_ALIGN_TOP_LEFT, 12, 10);

        if (!pixels_.empty()) {
            logo_ = lv_image_create(root_);
            lv_image_set_src(logo_, &image_);
            lv_obj_align(logo_, LV_ALIGN_TOP_MID, 0, 7);
            lv_obj_add_flag(logo_, LV_OBJ_FLAG_HIDDEN);
        }
        footer_ = lv_label_create(root_);
        lv_label_set_text(footer_, "EDU // AUTHORIZED TESTING ONLY");
        lv_obj_set_style_text_font(footer_, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(footer_, lv_color_hex(0x7bf6b0), 0);
        lv_obj_align(footer_, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        for (auto& bar : bars_) {
            bar = lv_obj_create(root_);
            lv_obj_remove_style_all(bar);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xd946ef), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_70, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        }
        started_ = lv_tick_get();
    }

    bool active() const { return root_ != nullptr; }

    void update() {
        if (!root_) {
            return;
        }
        const uint32_t elapsed = lv_tick_elaps(started_);
        const uint32_t duration = reduced_motion_ ? 1500U : 3200U;
        if (elapsed >= duration) {
            finish();
            return;
        }
        if (elapsed < duration / 2U) {
            return;
        }
        lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
        if (logo_) {
            lv_obj_remove_flag(logo_, LV_OBJ_FLAG_HIDDEN);
            const auto opacity = reduced_motion_ ? 255U :
                std::min(255U, (elapsed - duration / 2U) * 255U / 400U);
            lv_obj_set_style_image_opa(logo_, static_cast<lv_opa_t>(opacity), 0);
        }
        lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        for (std::size_t i = 0; i < bars_.size(); ++i) {
            const double distance = std::abs(static_cast<double>(i) - 23.5) / 23.5;
            const double phase = reduced_motion_ ? 0.0 : elapsed / 125.0;
            const int height = 2 + static_cast<int>((1.0 - distance) *
                (std::sin(phase + i * 0.5) * 0.5 + 0.5) * 19.0);
            lv_obj_set_pos(bars_[i], 8 + static_cast<int>(i) * 6, 148 - height);
            lv_obj_set_size(bars_[i], 4, height);
            lv_obj_remove_flag(bars_[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_move_foreground(footer_);
    }

    void finish() {
        if (root_) {
            lv_obj_delete(root_);
            root_ = nullptr;
        }
    }

private:
    AssetManager& assets_;
    lv_obj_t* root_{nullptr};
    lv_obj_t* status_{nullptr};
    lv_obj_t* logo_{nullptr};
    lv_obj_t* footer_{nullptr};
    std::array<lv_obj_t*, 48> bars_{};
    std::vector<uint8_t> pixels_;
    lv_image_dsc_t image_{};
    uint32_t started_{0};
    bool reduced_motion_{false};
};

class ShortcutController {
public:
    ShortcutController(viewmodel::BaseViewModel& view_model,
                       AssetManager& assets,
                       ThemePersistence& theme_persistence,
                       BootSplash& boot_splash)
        : view_model_(view_model),
          theme_persistence_(theme_persistence),
          boot_splash_(boot_splash),
          ui_sound_(assets.resolve("audio/click.wav").string()),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)),
          help_popup_(std::make_unique<view::widgets::HelpPopup>(lv_layer_top(), view_model, assets)) {
        toast_->build();
        help_popup_->build();
        platform::set_global_key_listener(global_key_listener, this);
        platform::set_key_release_listener(key_release_listener, this);
        for (auto* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER) {
                lv_indev_add_event_cb(input, pointer_sound_listener, LV_EVENT_PRESSED, this);
            }
        }
    }

    ~ShortcutController() {
        platform::clear_key_release_listener(key_release_listener, this);
        platform::clear_global_key_listener(global_key_listener, this);
        if (microphone_task_) {
            microphone_task_->cancelled = true;
        }
        if (microphone_thread_.joinable()) {
            microphone_thread_.join();
        }
        if (ir_task_) {
            ir_task_->cancelled = true;
        }
        if (ir_thread_.joinable()) {
            ir_thread_.join();
        }
        if (wifi_task_) {
            wifi_task_->cancelled = true;
        }
        if (wifi_thread_.joinable()) {
            wifi_thread_.join();
        }
        if (gatt_task_) {
            gatt_task_->cancelled = true;
        }
        if (gatt_thread_.joinable()) {
            gatt_thread_.join();
        }
        for (auto* input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            lv_indev_remove_event_cb_with_user_data(input, pointer_sound_listener, this);
        }
        if (network_thread_.joinable()) {
            network_thread_.join();
        }
        if (file_task_) {
            file_task_->cancelled = true;
        }
        if (file_thread_.joinable()) {
            file_thread_.join();
        }
        if (gallery_task_) {
            gallery_task_->cancelled = true;
        }
        if (gallery_thread_.joinable()) {
            gallery_thread_.join();
        }
        close_visual_tool();
    }

    void update() {
        update_network_query();
        update_gatt();
        update_wifi();
        update_ir();
        update_file_browser();
        update_gallery_image();
        update_music();
        update_microphone();
        if (piano_active_) {
            const auto status = ui_sound_.tone_status();
            if (status.state == platform::MusicState::Error && status.error != piano_error_) {
                piano_error_ = status.error;
                show_piano();
            }
        }
        if (!music_active_ && !microphone_active_) {
            const auto sound_error = ui_sound_.take_error();
            if (!sound_error.empty()) {
                LOG_WARN("{}", sound_error);
                toast_->show(sound_error, view::widgets::ToastTone::Error, 4200);
            }
        }
        if (visual_tool_mode_ == VisualToolMode::Ambient ||
            visual_tool_mode_ == VisualToolMode::Screensaver) {
            update_ambient();
        } else if (visual_tool_mode_ == VisualToolMode::AmbientPreview) {
            update_ambient_preview();
        } else if (visual_tool_mode_ == VisualToolMode::ScreensaverAmbient) {
            update_ambient_preview();
        } else if (visual_tool_mode_ == VisualToolMode::ScreensaverClock) {
            update_screensaver_clock();
        }
        if (visual_tool_mode_ == VisualToolMode::None &&
                 !screensaver_picker_active_ && !theme_picker_active_ &&
                 !sound_picker_active_ && !usb_sim_active_ &&
                 !file_browser_active_ && !interfaces_active_ && !arp_active_ &&
                 !chance_tool_active_ && !brightness_adjustment_ &&
                 text_tool_mode_ == TextToolMode::None &&
                 gatt_stage_ == GattStage::None && !wifi_active_ && !wifi_task_ &&
                 !ir_active_ && !ir_task_ && !network_query_ &&
                 !help_popup_->visible() && !toast_->visible() &&
                 view_model_.screensaver_due(lv_display_get_inactive_time(nullptr))) {
            start_screensaver();
        }
    }

private:
    enum class TextToolMode {
        None,
        Calculator,
        Morse,
        Ping,
        Dns,
    };

    enum class VisualToolMode {
        None,
        Flashlight,
        ScreenTest,
        Ambient,
        AmbientPreview,
        Screensaver,
        ScreensaverAmbient,
        ScreensaverClock,
        Gallery,
    };

    enum class FileMode {
        Files,
        Gallery,
        Music,
        AlbumArt,
    };

    struct NetworkQueryState {
        std::mutex mutex;
        std::string host;
        std::string result;
        bool dns{false};
        bool complete{false};
    };

    struct FileQueryState {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::filesystem::path directory;
        std::size_t offset{0};
        platform::DirectoryResult result;
        bool complete{false};
    };

    struct GalleryQueryState {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        platform::GalleryImage result;
        bool complete{false};
        bool camera{false};
    };

    enum class GattStage {
        None,
        Scanning,
        Devices,
        Connecting,
        Entries,
        Empty,
        Error,
    };

    struct GattTask {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool complete{false};
        bool scan{false};
        platform::BluetoothScanResult scan_result;
        platform::GattBrowseResult browse_result;
    };

    struct WifiTask {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool complete{false};
        platform::WifiScanResult result;
    };

    struct IrTask {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool complete{false};
        bool saving{false};
        platform::IrReceiveResult result;
        platform::IrSaveResult save_result;
    };

    struct MicrophoneTask {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        bool complete{false};
        bool recording{false};
        bool saving{false};
        platform::MicrophoneLevel result;
        platform::MicrophoneRecording recording_result;
        platform::RecordingSaveResult save_result;
        std::vector<int16_t> samples;
    };

    void start_microphone_test(bool recording = false) {
        if (microphone_task_) {
            toast_->show("Previous microphone test is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        microphone_active_ = true;
        microphone_confirm_ = true;
        microphone_recording_mode_ = recording;
        microphone_ready_ = false;
        microphone_samples_.clear();
        ui_sound_.play(0);
        toast_->show_persistent(recording
            ? "Recorder: 5 seconds\nAudio stays in memory\nENTER start  ESC back"
            : "Microphone level test\n5 seconds; no audio saved\nENTER start  ESC back");
    }

    void run_microphone_test() {
        auto task = std::make_shared<MicrophoneTask>();
        task->recording = microphone_recording_mode_;
        microphone_task_ = task;
        microphone_confirm_ = false;
        toast_->show_persistent(microphone_recording_mode_
            ? "Microphone active\nRecording for 5 seconds\nESC cancels and discards"
            : "Microphone active\nListening for 5 seconds\nNo audio saved  ESC cancel");
        try {
            microphone_thread_ = std::thread([task] {
                platform::MicrophoneLevel result;
                platform::MicrophoneRecording recording;
                try {
                    if (task->recording)
                        recording = platform::record_microphone(task->cancelled,
                                                                 std::chrono::seconds(5));
                    else
                        result = platform::measure_microphone(task->cancelled,
                                                              std::chrono::seconds(5));
                } catch (const std::exception& error) {
                    result.error = std::string("Microphone test failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->recording_result = std::move(recording);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            microphone_task_.reset();
            toast_->show_persistent("Microphone test failed\n" + error.code().message() +
                                        "\nESC back", view::widgets::ToastTone::Error);
        }
    }

    void save_microphone_recording() {
#if USE_DESKTOP && defined(_WIN32)
        const wchar_t* home = _wgetenv(L"HOME");
        if (!home || home[0] == L'\0') home = _wgetenv(L"USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (!home || home[0] == 0) {
            toast_->show_persistent("Home directory unavailable\nENTER retry  ESC discard",
                                    view::widgets::ToastTone::Error);
            return;
        }
        const std::filesystem::path home_directory(home);
        if (!home_directory.is_absolute()) {
            toast_->show_persistent("Home path must be absolute\nENTER retry  ESC discard",
                                    view::widgets::ToastTone::Error);
            return;
        }
        const auto directory = home_directory / "poseidon-recordings";
        auto task = std::make_shared<MicrophoneTask>();
        task->saving = true;
        task->samples = std::move(microphone_samples_);
        microphone_task_ = task;
        toast_->show_persistent("Saving recording...\nPlease wait");
        try {
            microphone_thread_ = std::thread([task, directory] {
                platform::RecordingSaveResult result;
                try {
                    std::error_code error;
                    std::filesystem::create_directory(directory, error);
                    if (error) {
                        result.error = "Recording folder: " + error.message();
                    } else {
                        result = platform::save_recording(directory, task->samples);
                    }
                } catch (const std::exception& error) {
                    result.error = std::string("Recording save failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->save_result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            microphone_samples_ = std::move(task->samples);
            microphone_task_.reset();
            toast_->show_persistent("Could not save: " + error.code().message() +
                                        "\nENTER retry  ESC discard", view::widgets::ToastTone::Error);
        }
    }

    void update_microphone() {
        const auto task = microphone_task_;
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) return;
        }
        if (microphone_thread_.joinable()) microphone_thread_.join();
        microphone_task_.reset();
        if (task->cancelled) {
            microphone_active_ = false;
            microphone_samples_.clear();
            toast_->hide();
            return;
        }
        if (!microphone_active_) return;
        if (task->saving) {
            if (!task->save_result.error.empty()) {
                microphone_samples_ = std::move(task->samples);
                toast_->show_persistent(task->save_result.error + "\nENTER retry  ESC discard",
                                        view::widgets::ToastTone::Error);
            } else {
                microphone_ready_ = false;
                toast_->show_persistent("Saved in poseidon-recordings:\n" +
                    file_display_text(task->save_result.path.filename().u8string(), 28) +
                    "\nESC back");
            }
            return;
        }
        const auto& result = task->result;
        const auto& error = result.error.empty() ? task->recording_result.error : result.error;
        if (!error.empty()) {
            toast_->show_persistent(error + "\nESC back", view::widgets::ToastTone::Error);
        } else if (task->recording) {
            microphone_samples_ = std::move(task->recording_result.samples);
            microphone_ready_ = true;
            toast_->show_persistent("Recording complete\nENTER saves in home/\nposeidon-recordings\nESC discards");
        } else {
            toast_->show_persistent("Microphone test complete\nPeak level: " +
                                    std::to_string(result.peak_percent) +
                                    "%\nNo audio saved  ESC back");
        }
    }

    bool handle_microphone_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (microphone_task_ && microphone_task_->saving) return true;
            if (microphone_task_) {
                microphone_task_->cancelled = true;
                toast_->show_persistent("Stopping microphone...\nPlease wait");
                return true;
            }
            microphone_active_ = false;
            microphone_confirm_ = false;
            microphone_ready_ = false;
            microphone_samples_.clear();
            toast_->hide();
        } else if (key == LV_KEY_ENTER && microphone_confirm_) {
            run_microphone_test();
        } else if (key == LV_KEY_ENTER && microphone_ready_ && !microphone_task_) {
            save_microphone_recording();
        }
        return true;
    }

    void start_ir_receive() {
        if (ir_task_) {
            toast_->show("Previous IR receive is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        auto task = std::make_shared<IrTask>();
        ir_task_ = task;
        ir_active_ = true;
        ir_receiving_ = true;
        ir_notice_.clear();
        toast_->show_persistent("Waiting for infrared signal...\nESC cancels");
        try {
            ir_thread_ = std::thread([task] {
                platform::IrReceiveResult result;
                try {
                    result = platform::receive_ir(task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("IR receive failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            ir_task_.reset();
            ir_receiving_ = false;
            ir_result_ = {};
            ir_result_.error = "Could not start receiver: " + error.code().message();
            show_ir();
        }
    }

    void start_ir_save() {
        if (ir_task_) return;
        if (ir_result_.samples.empty() || ir_result_.truncated || !ir_result_.error.empty()) {
            ir_notice_ = "Only complete captures can be saved";
            show_ir();
            return;
        }
#if USE_DESKTOP && defined(_WIN32)
        const wchar_t* home = _wgetenv(L"HOME");
        if (!home || home[0] == L'\0') home = _wgetenv(L"USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (!home || home[0] == 0 || !std::filesystem::path(home).is_absolute()) {
            ir_notice_ = "Absolute home path unavailable";
            show_ir();
            return;
        }
        const auto directory = std::filesystem::path(home) / "poseidon-ir";
        auto task = std::make_shared<IrTask>();
        task->saving = true;
        task->result = ir_result_;
        ir_task_ = task;
        toast_->show_persistent("Saving IR timings...\nPlease wait");
        try {
            ir_thread_ = std::thread([task, directory] {
                platform::IrSaveResult result;
                try {
                    std::error_code error;
                    std::filesystem::create_directory(directory, error);
                    if (error) {
                        result.error = "IR folder: " + error.message();
                    } else {
                        result = platform::save_ir_capture(directory, task->result);
                    }
                } catch (const std::exception& error) {
                    result.error = std::string("IR save failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->save_result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            ir_task_.reset();
            ir_notice_ = "Could not save: " + error.code().message();
            show_ir();
        }
    }

    void show_ir() {
        std::string message;
        if (!ir_result_.error.empty()) {
            message = "IR error\n" + ir_result_.error;
        } else if (ir_result_.samples.empty()) {
            message = "No infrared signal received";
        } else {
            const auto& sample = ir_result_.samples[ir_selection_];
            message = "IR sample " + std::to_string(ir_selection_ + 1) + "/" +
                      std::to_string(ir_result_.samples.size()) +
                      (ir_result_.truncated ? "+" : "") + "\n" +
                      (sample.pulse ? "Pulse " : "Space ") +
                      std::to_string(sample.duration_us) + " us";
        }
        if (!ir_notice_.empty()) message += "\n" + file_display_text(ir_notice_, 30);
        message += ir_learning_
                       ? "\nUP/DOWN  ENTER receive  S save\nESC back"
                       : "\nUP/DOWN select  ENTER receive\nESC back";
        toast_->show_persistent(message, ir_result_.error.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    void update_ir() {
        const auto task = ir_task_;
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) return;
        }
        if (ir_thread_.joinable()) ir_thread_.join();
        ir_task_.reset();
        ir_receiving_ = false;
        if (task->saving) {
            ir_notice_ = task->save_result.error.empty()
                             ? "Saved in home/poseidon-ir"
                             : task->save_result.error;
            if (ir_active_) show_ir();
            return;
        }
        if (task->cancelled) return;
        ir_result_ = std::move(task->result);
        ir_selection_ = 0;
        show_ir();
    }

    bool handle_ir_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (ir_task_ && ir_task_->saving) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (ir_task_) ir_task_->cancelled = true;
            ir_active_ = false;
            toast_->hide();
        } else if (!ir_receiving_) {
            const bool previous = key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z';
            const bool next = key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c';
            if (previous && ir_selection_ > 0) {
                --ir_selection_;
                show_ir();
            } else if (next && ir_selection_ + 1 < ir_result_.samples.size()) {
                ++ir_selection_;
                show_ir();
            } else if (key == LV_KEY_ENTER) {
                start_ir_receive();
            } else if (ir_learning_ && (key == 's' || key == 'S')) {
                start_ir_save();
            }
        }
        return true;
    }

    void start_wifi_scan() {
        if (wifi_task_) {
            toast_->show("Previous Wi-Fi scan is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        auto task = std::make_shared<WifiTask>();
        wifi_task_ = task;
        wifi_active_ = true;
        wifi_scanning_ = true;
        toast_->show_persistent("Scanning nearby Wi-Fi networks...\nESC cancels");
        try {
            wifi_thread_ = std::thread([task] {
                platform::WifiScanResult result;
                try {
                    result = platform::scan_wifi(task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("Wi-Fi scan failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            wifi_task_.reset();
            wifi_scanning_ = false;
            wifi_result_ = {};
            wifi_result_.error = "Could not start scan: " + error.code().message();
            show_wifi();
        }
    }

    void show_wifi() {
        std::string message;
        if (!wifi_result_.error.empty()) {
            message = "Wi-Fi error\n" + wifi_result_.error;
        } else if (wifi_result_.networks.empty()) {
            message = "No Wi-Fi networks found";
        } else {
            const auto& network = wifi_result_.networks[wifi_selection_];
            const int channel = platform::wifi_channel(network.frequency_mhz);
            message = "Wi-Fi " + std::to_string(wifi_selection_ + 1) + "/" +
                      std::to_string(wifi_result_.networks.size()) + "\n" +
                      network.name + "\n" + network.address + "\n" +
                      std::to_string(network.strength) + "%  " +
                      (network.secured ? "Secured" : "Open") +
                      (channel >= 0 ? "  Ch " + std::to_string(channel) : "");
        }
        message += "\nUP/DOWN select  ENTER rescan\nESC back";
        toast_->show_persistent(message, wifi_result_.error.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    void update_wifi() {
        const auto task = wifi_task_;
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) return;
        }
        if (wifi_thread_.joinable()) wifi_thread_.join();
        wifi_task_.reset();
        wifi_scanning_ = false;
        if (task->cancelled) return;
        wifi_result_ = std::move(task->result);
        wifi_selection_ = 0;
        show_wifi();
    }

    bool handle_wifi_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (wifi_task_) wifi_task_->cancelled = true;
            wifi_active_ = false;
            toast_->hide();
        } else if (!wifi_scanning_) {
            const bool previous = key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z';
            const bool next = key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c';
            if (previous && wifi_selection_ > 0) {
                --wifi_selection_;
                show_wifi();
            } else if (next && wifi_selection_ + 1 < wifi_result_.networks.size()) {
                ++wifi_selection_;
                show_wifi();
            } else if (key == LV_KEY_ENTER) {
                start_wifi_scan();
            }
        }
        return true;
    }

    void start_gatt_scan() {
        if (gatt_task_) {
            toast_->show("Previous Bluetooth operation is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        auto task = std::make_shared<GattTask>();
        task->scan = true;
        gatt_task_ = task;
        gatt_stage_ = GattStage::Scanning;
        toast_->show_persistent("Scanning nearby BLE devices...\nESC cancels");
        try {
            gatt_thread_ = std::thread([task] {
                platform::BluetoothScanResult result;
                try {
                    result = platform::scan_ble_devices(task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("Bluetooth scan failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->scan_result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            gatt_task_.reset();
            gatt_stage_ = GattStage::Error;
            gatt_error_ = "Could not start scan: " + error.code().message();
            show_gatt();
        }
    }

    void start_gatt_browse() {
        if (gatt_task_ || gatt_device_index_ >= gatt_devices_.size()) {
            toast_->show("Bluetooth device is unavailable",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        const auto device = gatt_devices_[gatt_device_index_];
        auto task = std::make_shared<GattTask>();
        gatt_task_ = task;
        gatt_stage_ = GattStage::Connecting;
        toast_->show_persistent("Connecting to selected device...\nESC cancels");
        try {
            gatt_thread_ = std::thread([task, device] {
                platform::GattBrowseResult result;
                try {
                    result = platform::browse_gatt(device, task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("GATT browse failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->browse_result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            gatt_task_.reset();
            gatt_stage_ = GattStage::Error;
            gatt_error_ = "Could not start connection: " + error.code().message();
            show_gatt();
        }
    }

    void show_gatt() {
        std::string message;
        if (gatt_stage_ == GattStage::Devices && !gatt_devices_.empty()) {
            const auto& device = gatt_devices_[gatt_device_index_];
            message = "BLE " + std::to_string(gatt_device_index_ + 1) + "/" +
                      std::to_string(gatt_devices_.size()) + "\n" +
                      (device.name.empty() ? "Unnamed device" : device.name) + "\n" +
                      device.address + "\nUP/DOWN select  ENTER open\nESC back";
        } else if (gatt_stage_ == GattStage::Entries && !gatt_entries_.empty()) {
            const auto& entry = gatt_entries_[gatt_entry_index_];
            message = std::string(entry.characteristic ? "Characteristic " : "Service ") +
                      std::to_string(gatt_entry_index_ + 1) + "/" +
                      std::to_string(gatt_entries_.size()) + "\n" + entry.uuid +
                      "\nUP/DOWN browse  ESC back";
        } else if (gatt_stage_ == GattStage::Empty) {
            message = gatt_devices_.empty()
                          ? "No BLE devices found\nMake the device advertise\nENTER rescan  ESC back"
                          : "No GATT services found\nESC back";
        } else if (gatt_stage_ == GattStage::Error) {
            message = "Bluetooth error\n" + gatt_error_ + "\nENTER rescan  ESC back";
        } else {
            return;
        }
        toast_->show_persistent(message, gatt_stage_ == GattStage::Error
                                             ? view::widgets::ToastTone::Warning
                                             : view::widgets::ToastTone::Default);
    }

    void update_gatt() {
        const auto task = gatt_task_;
        if (!task) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) {
                return;
            }
        }
        if (gatt_thread_.joinable()) {
            gatt_thread_.join();
        }
        gatt_task_.reset();
        if (task->cancelled) {
            const auto& error = task->scan ? task->scan_result.error : task->browse_result.error;
            if (!error.empty()) {
                LOG_WARN("Bluetooth cleanup failed: {}", error);
                toast_->show("Bluetooth cleanup failed\n" + error,
                             view::widgets::ToastTone::Warning, 5200);
            }
            return;
        }
        if (task->scan) {
            gatt_error_ = task->scan_result.error;
            gatt_devices_ = std::move(task->scan_result.devices);
            gatt_device_index_ = 0;
            gatt_stage_ = !gatt_error_.empty() ? GattStage::Error
                          : gatt_devices_.empty() ? GattStage::Empty
                                                  : GattStage::Devices;
        } else {
            gatt_error_ = task->browse_result.error;
            gatt_entries_ = std::move(task->browse_result.entries);
            gatt_entry_index_ = 0;
            gatt_stage_ = !gatt_error_.empty() ? GattStage::Error
                          : gatt_entries_.empty() ? GattStage::Empty
                                                  : GattStage::Entries;
        }
        show_gatt();
    }

    bool handle_gatt_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (gatt_task_) {
                gatt_task_->cancelled = true;
            }
            if (gatt_stage_ == GattStage::Entries ||
                (gatt_stage_ == GattStage::Empty && !gatt_devices_.empty())) {
                gatt_stage_ = GattStage::Devices;
                show_gatt();
            } else {
                gatt_stage_ = GattStage::None;
                toast_->hide();
            }
            return true;
        }
        const bool previous = key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z';
        const bool next = key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c';
        if (previous || next) {
            std::size_t* index = nullptr;
            std::size_t count = 0;
            if (gatt_stage_ == GattStage::Devices) {
                index = &gatt_device_index_;
                count = gatt_devices_.size();
            } else if (gatt_stage_ == GattStage::Entries) {
                index = &gatt_entry_index_;
                count = gatt_entries_.size();
            }
            if (index && count) {
                if (previous && *index > 0) {
                    --*index;
                } else if (next && *index + 1 < count) {
                    ++*index;
                }
                show_gatt();
            }
        } else if (key == LV_KEY_ENTER) {
            if (gatt_stage_ == GattStage::Devices) {
                start_gatt_browse();
            } else if (gatt_stage_ == GattStage::Error ||
                       (gatt_stage_ == GattStage::Empty && gatt_devices_.empty())) {
                start_gatt_scan();
            }
        }
        return true;
    }

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

    void show_brightness_prompt() {
        char message[128]{};
        std::snprintf(message,
                      sizeof(message),
                      "Display brightness: %d%%\nZ/Left darker\nC/Right brighter\nEnter/ESC done",
                      brightness_percent_);
        toast_->show_persistent(message);
    }

    void start_brightness_adjustment() {
        std::string error;
        if (!platform::read_brightness_percent(brightness_percent_, error)) {
            toast_->show("Brightness unavailable\n" + error,
                         view::widgets::ToastTone::Warning,
                         4200);
            return;
        }

        brightness_adjustment_ = true;
        show_brightness_prompt();
    }

    void adjust_brightness(int32_t delta) {
        const int32_t next = std::clamp(brightness_percent_ + delta, 5, 100);
        if (next == brightness_percent_) {
            show_brightness_prompt();
            return;
        }

        std::string error;
        if (!platform::write_brightness_percent(next, error)) {
            brightness_adjustment_ = false;
            toast_->show("Could not change brightness\n" + error,
                         view::widgets::ToastTone::Error,
                         4200);
            return;
        }

        brightness_percent_ = next;
        show_brightness_prompt();
    }

    bool handle_brightness_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_LEFT || key == 'z') {
            adjust_brightness(-5);
        }
        else if (key == LV_KEY_RIGHT || key == 'c') {
            adjust_brightness(5);
        }
        else if (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE ||
                 key == platform::kKeyHelp) {
            brightness_adjustment_ = false;
            char message[48]{};
            std::snprintf(message, sizeof(message), "Brightness set to %d%%", brightness_percent_);
            toast_->show(message, view::widgets::ToastTone::Success, 3200);
        }
        return true;
    }

    static std::string file_display_text(const std::string& value, std::size_t limit) {
        std::string result;
        std::size_t characters = 0;
        for (const unsigned char character : value) {
            if ((character & 0xc0) != 0x80) {
                if (characters == limit) {
                    result += "...";
                    break;
                }
                ++characters;
            }
            result += character < 0x20 || character == 0x7f || character == '#'
                          ? '?'
                          : static_cast<char>(character);
        }
        return result;
    }

    void show_file_browser() {
        std::string folder = file_directory_.filename().u8string();
        if (folder.empty()) {
            folder = file_directory_.root_path().u8string();
        }
        const char* title = file_mode_ == FileMode::Gallery ? "Gallery: "
                            : file_mode_ == FileMode::AlbumArt ? "Album art: "
                            : file_mode_ == FileMode::Music ? "Music: " : "Files: ";
        std::string message = std::string(title) +
                              file_display_text(folder, 20);
        if (!file_notice_.empty()) {
            message += "\n" + file_display_text(file_notice_, 30);
        }
        else if (file_entries_.empty()) {
            message += "\nEmpty directory";
        }
        else {
            const auto& entry = file_entries_[file_selection_];
            message += "\nItem " + std::to_string(file_offset_ + file_selection_ + 1) +
                       (file_truncated_ ? "+" : "") +
                       (entry.directory ? " DIR " : " FILE ") +
                       file_display_text(entry.name, 22);
            if (!entry.directory) {
                message += entry.size >= 0 ? "\n" + std::to_string(entry.size) + " bytes"
                                           : "\nSize unavailable";
            }
        }
        message += file_mode_ == FileMode::Gallery ? "\nUP/DOWN select  ENTER view\n"
                   : file_mode_ == FileMode::AlbumArt ? "\nSelect track  ENTER cover\n"
                   : file_mode_ == FileMode::Music ? "\nUP/DOWN select  ENTER play\n"
                                                  : "\nUP/DOWN select  ENTER open\n";
        message += "P/N page  BACK up  ESC exit";
        toast_->show_persistent(message, file_notice_.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    bool open_file_directory(const std::filesystem::path& directory, std::size_t offset = 0) {
        if (file_task_) {
            file_notice_ = "Previous file read is stopping";
            if (file_browser_active_) {
                show_file_browser();
            } else {
                toast_->show("Files\n" + file_notice_, view::widgets::ToastTone::Warning, 4200);
            }
            return false;
        }
        auto task = std::make_shared<FileQueryState>();
        task->directory = directory;
        task->offset = offset;
        file_task_ = task;
        try {
            file_thread_ = std::thread([task] {
                platform::DirectoryResult result;
                try {
                    result = platform::read_directory(task->directory.u8string(),
                                                      task->offset, &task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("Directory read failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            file_task_.reset();
            file_notice_ = "Could not start file read: " + error.code().message();
            if (file_browser_active_) {
                show_file_browser();
            } else {
                toast_->show("Files\n" + file_notice_, view::widgets::ToastTone::Error, 4200);
            }
            return false;
        }
        if (!file_browser_active_) {
            file_directory_ = directory;
            file_entries_.clear();
            file_selection_ = 0;
            file_offset_ = 0;
            file_truncated_ = false;
        }
        file_browser_active_ = true;
        toast_->show_persistent("Opening directory...\nESC cancels");
        return true;
    }

    void update_file_browser() {
        const auto task = file_task_;
        if (!task) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) {
                return;
            }
        }
        if (file_thread_.joinable()) {
            file_thread_.join();
        }
        file_task_.reset();
        if (task->cancelled || !file_browser_active_) {
            return;
        }
        if (!task->result.error.empty()) {
            file_notice_ = task->result.error;
        } else {
            file_directory_ = task->directory;
            file_entries_ = std::move(task->result.entries);
            file_truncated_ = task->result.truncated;
            file_offset_ = task->offset;
            file_selection_ = 0;
            file_notice_.clear();
        }
        show_file_browser();
    }

    void start_file_browser(FileMode mode = FileMode::Files) {
#if USE_DESKTOP && defined(_WIN32)
        const wchar_t* home = _wgetenv(L"HOME");
#else
        const char* home = std::getenv("HOME");
#endif
#if USE_DESKTOP && defined(_WIN32)
        if (!home || home[0] == '\0') {
            home = _wgetenv(L"USERPROFILE");
        }
#endif
        if (!home || home[0] == '\0') {
            toast_->show("Files\nHome directory is not configured",
                         view::widgets::ToastTone::Error, 4200);
            return;
        }
        try {
            file_mode_ = mode;
#if USE_DESKTOP && defined(_WIN32)
            open_file_directory(std::filesystem::path(home));
#else
            open_file_directory(std::filesystem::u8path(home));
#endif
        } catch (const std::filesystem::filesystem_error& error) {
            toast_->show("Files\nInvalid home path: " + std::string(error.what()),
                         view::widgets::ToastTone::Error, 4200);
        } catch (const std::range_error& error) {
            toast_->show("Files\nInvalid home path: " + std::string(error.what()),
                         view::widgets::ToastTone::Error, 4200);
        }
    }

    bool handle_file_browser_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC) {
            file_browser_active_ = false;
            if (file_task_) {
                file_task_->cancelled = true;
            }
            toast_->hide();
            return true;
        }
        if (file_task_) {
            return true;
        }
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            const auto parent = file_directory_.parent_path();
            if (!parent.empty() && parent != file_directory_) {
                open_file_directory(parent);
            }
            return true;
        }
        if ((key == 'n' || key == 'N') && file_truncated_) {
            open_file_directory(file_directory_, file_offset_ + platform::kDirectoryPageSize);
            return true;
        }
        if ((key == 'p' || key == 'P') && file_offset_ >= platform::kDirectoryPageSize) {
            open_file_directory(file_directory_, file_offset_ - platform::kDirectoryPageSize);
            return true;
        }
        if (key == LV_KEY_UP || key == 'f' || key == 'z') {
            if (file_selection_ > 0) {
                --file_selection_;
            }
        }
        else if (key == LV_KEY_DOWN || key == 'x' || key == 'c') {
            if (file_selection_ + 1 < file_entries_.size()) {
                ++file_selection_;
            }
        }
        else if (key == LV_KEY_ENTER && !file_entries_.empty()) {
            const auto& entry = file_entries_[file_selection_];
            const auto path = file_directory_ / std::filesystem::u8path(entry.name);
            if (entry.directory) {
                open_file_directory(path);
            } else if (file_mode_ == FileMode::Gallery) {
                start_gallery_image(path);
            } else if (file_mode_ == FileMode::AlbumArt) {
                start_gallery_image(path, true);
            } else if (file_mode_ == FileMode::Music) {
                start_music(path);
            }
            return true;
        }
        file_notice_.clear();
        show_file_browser();
        return true;
    }

    void start_music(const std::filesystem::path& path) {
        music_name_ = file_display_text(path.filename().u8string(), 24);
        music_active_ = true;
        music_state_ = platform::MusicState::Stopped;
        music_error_.clear();
        ui_sound_.play_music(path, 50);
        update_music();
    }

    void update_music() {
        if (!music_active_) return;
        const auto status = ui_sound_.music_status();
        if (status.state == music_state_ && status.error == music_error_) return;
        music_state_ = status.state;
        music_error_ = status.error;
        std::string message = "Music\n" + music_name_ + "\n";
        switch (music_state_) {
            case platform::MusicState::Loading: message += "Opening audio..."; break;
            case platform::MusicState::Playing: message += "Playing at 50% volume"; break;
            case platform::MusicState::Finished: message += "Playback finished"; break;
            case platform::MusicState::Error: message += music_error_; break;
            case platform::MusicState::Stopped: message += "Playback stopped"; break;
        }
        message += "\nESC stop / back";
        toast_->show_persistent(message, music_state_ == platform::MusicState::Error
                                             ? view::widgets::ToastTone::Warning
                                             : view::widgets::ToastTone::Default);
    }

    bool handle_music_key(uint32_t key, bool long_pressed) {
        if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            ui_sound_.stop_music();
            music_active_ = false;
            show_file_browser();
        }
        return true;
    }

    void show_piano() {
        std::string message = "Piano: A S D F G H J K\nC D E F G A B C";
        if (!piano_error_.empty()) message += "\n" + piano_error_;
        message += "\nESC back";
        toast_->show_persistent(message, piano_error_.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    bool handle_piano_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            piano_active_ = false;
            ui_sound_.stop_tone();
            toast_->hide();
            return true;
        }
        static constexpr char keys[] = "asdfghjk";
        static constexpr double frequencies[] = {
            261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25
        };
        if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
        for (std::size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); ++i) {
            if (key == static_cast<uint32_t>(keys[i])) {
                piano_error_.clear();
                ui_sound_.play_tone(frequencies[i], 50);
                show_piano();
                break;
            }
        }
        return true;
    }

    void start_gallery_image(const std::filesystem::path& path, bool album_art = false) {
        if (gallery_task_) {
            file_notice_ = "Previous image read is stopping";
            show_file_browser();
            return;
        }
        auto task = std::make_shared<GalleryQueryState>();
        gallery_task_ = task;
        try {
            gallery_thread_ = std::thread([task, path, album_art] {
                platform::GalleryImage result;
                try {
                    result = album_art ? platform::read_album_art(path, &task->cancelled)
                                       : platform::read_gallery_image(path, &task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("Image read failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            gallery_task_.reset();
            file_notice_ = "Could not start image read: " + error.code().message();
            show_file_browser();
            return;
        }
        gallery_loading_ = true;
        toast_->show_persistent("Opening image...\nESC cancels");
    }

    void start_camera_capture() {
        if (gallery_task_) {
            camera_confirm_ = false;
            toast_->show("Previous image task is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        auto task = std::make_shared<GalleryQueryState>();
        task->camera = true;
        gallery_task_ = task;
        camera_confirm_ = false;
        try {
            gallery_thread_ = std::thread([task] {
                platform::GalleryImage result;
                try {
                    result = platform::capture_camera(task->cancelled);
                } catch (const std::exception& error) {
                    result.error = std::string("Camera capture failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            gallery_task_.reset();
            toast_->show("Could not start camera: " + error.code().message(),
                         view::widgets::ToastTone::Error, 4200);
            return;
        }
        gallery_loading_ = true;
        toast_->show_persistent("Camera active\nCapturing one snapshot\nESC cancels; no image saved");
    }

    void update_gallery_image() {
        const auto task = gallery_task_;
        if (!task) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (!task->complete) {
                return;
            }
        }
        if (gallery_thread_.joinable()) {
            gallery_thread_.join();
        }
        gallery_task_.reset();
        if (task->cancelled || !gallery_loading_) {
            return;
        }
        gallery_loading_ = false;
        if (!task->result.error.empty()) {
            if (task->camera) {
                toast_->show("Camera: " + task->result.error,
                             view::widgets::ToastTone::Error, 5000);
            } else {
                file_notice_ = task->result.error;
                show_file_browser();
            }
            return;
        }
        gallery_camera_ = task->camera;
        gallery_pixels_ = std::move(task->result.pixels);
        gallery_image_ = {};
        gallery_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        gallery_image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
        gallery_image_.header.w = task->result.width;
        gallery_image_.header.h = task->result.height;
        gallery_image_.header.stride = task->result.width * 4U;
        gallery_image_.data_size = static_cast<uint32_t>(gallery_pixels_.size());
        gallery_image_.data = gallery_pixels_.data();
        visual_tool_mode_ = VisualToolMode::Gallery;
        if (!create_visual_overlay()) {
            close_visual_tool();
            if (task->camera) {
                toast_->show("Could not open image viewer", view::widgets::ToastTone::Error, 4200);
            } else {
                file_notice_ = "Could not open image viewer";
                show_file_browser();
            }
            return;
        }
        lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
        auto* image = lv_image_create(overlay_);
        if (!image) {
            close_visual_tool();
            if (task->camera) {
                toast_->show("Could not create image viewer", view::widgets::ToastTone::Error, 4200);
            } else {
                file_notice_ = "Could not create image viewer";
                show_file_browser();
            }
            return;
        }
        lv_image_set_src(image, &gallery_image_);
        const uint32_t scale = std::min({256U,
                                         320U * 256U / task->result.width,
                                         150U * 256U / task->result.height});
        lv_image_set_scale(image, scale);
        lv_obj_center(image);
        lv_label_set_text(screen_test_label_, "ESC: back");
        lv_obj_set_style_text_color(screen_test_label_, lv_color_white(), 0);
        lv_obj_align(screen_test_label_, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_move_foreground(screen_test_label_);
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
               << "Network interface count: ";
        if (interfaces.error.empty()) {
            report << interfaces.interfaces.size() << "\n";
        } else {
            report << "unavailable (" << interfaces.error << ")\n";
        }
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
                                 6000);
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
                const std::string clock =
                    platform::current_clock_text(view_model_.clock_24h());
                toast_->show(clock.empty() ? "System clock unavailable"
                                           : "System time\n" + clock,
                             clock.empty() ? view::widgets::ToastTone::Warning
                                           : view::widgets::ToastTone::Default,
                             4200);
                return;
            }
            case 7:
                start_brightness_adjustment();
                return;
            default:
                return;
        }
    }

    void toggle_clock_format() {
        const bool clock_24h = !view_model_.clock_24h();
        view_model_.set_clock_24h(clock_24h);
        if (!persist_clock_format(theme_persistence_, clock_24h)) {
            toast_->show("Clock format changed but could not be saved",
                         view::widgets::ToastTone::Error,
                         4200);
            return;
        }

        toast_->show(std::string("Clock format saved\n") +
                         (clock_24h ? "24-hour" : "12-hour"),
                     view::widgets::ToastTone::Success,
                     3200);
    }

    void toggle_layout() {
        auto config = theme_persistence_.config;
        const int current = config.grid_layout ? 2 : config.spacious_layout ? 1 : 0;
        const int next = (current + 1) % 3;
        config.spacious_layout = next == 1;
        config.grid_layout = next == 2;
        std::string error;
        if (!save_application_config(theme_persistence_.config_path, config, error)) {
            LOG_WARN("failed to save layout {}: {}", theme_persistence_.config_path, error);
            toast_->show("Layout changed but could not be saved",
                         view::widgets::ToastTone::Error, 4200);
            return;
        }
        theme_persistence_.config = config;
        view_model_.set_spacious_layout(config.spacious_layout);
        view_model_.set_grid_layout(config.grid_layout);
        static constexpr const char* kNames[] = {"Compact list", "Spacious list", "Two-column grid"};
        toast_->show(std::string("Layout saved\n") + kNames[next],
                     view::widgets::ToastTone::Success, 3200);
    }

    void show_sound_picker() {
        toast_->show_persistent(std::string("UI sounds: ") +
                                (sound_selection_enabled_ ? "ON" : "MUTED") +
                                "  " + std::to_string(sound_selection_volume_) + "%" +
                                "\nArrows volume  M mute\nENTER save  ESC cancel");
    }

    void start_sound_picker() {
        sound_selection_enabled_ = theme_persistence_.config.sound_enabled;
        sound_selection_volume_ = theme_persistence_.config.sound_volume;
        sound_picker_active_ = true;
        show_sound_picker();
    }

    bool handle_sound_picker_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC) {
            sound_picker_active_ = false;
            toast_->hide();
        } else if (key == 'm' || key == 'M') {
            sound_selection_enabled_ = !sound_selection_enabled_;
            show_sound_picker();
        } else if (key == LV_KEY_UP || key == LV_KEY_RIGHT || key == 'f' || key == 'c') {
            sound_selection_volume_ = std::min(100, sound_selection_volume_ + 10);
            show_sound_picker();
        } else if (key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == 'x' || key == 'z') {
            sound_selection_volume_ = std::max(0, sound_selection_volume_ - 10);
            show_sound_picker();
        } else if (key == LV_KEY_ENTER) {
            sound_picker_active_ = false;
            auto& config = theme_persistence_.config;
            config.sound_enabled = sound_selection_enabled_;
            config.sound_volume = sound_selection_volume_;
            std::string error;
            if (!save_application_config(theme_persistence_.config_path, config, error)) {
                LOG_WARN("failed to save sound settings: {}", error);
                toast_->show("Sound settings changed but could not be saved",
                             view::widgets::ToastTone::Error, 4200);
            } else {
                toast_->show("Sound settings saved", view::widgets::ToastTone::Success, 3200);
            }
            if (config.sound_enabled) {
                ui_sound_.retry();
            }
            ui_sound_.play(config.sound_enabled ? config.sound_volume : 0);
        }
        return true;
    }

    void show_usb_sim_prompt(const std::string& notice = {}) {
        std::string text = notice.empty() ? "" : notice + "\n";
        if (!usb_sim_connected_) {
            text += "USB KEYBOARD SIMULATION\nENTER connect  ESC cancel\nNo real host is connected";
        } else if (usb_sim_confirm_) {
            text += "SIMULATED SEND " + std::to_string(usb_sim_text_.size()) +
                    " chars?\nENTER confirm  ESC edit\nNo real input will be sent";
        } else {
            text += "SIMULATED keyboard\n" + usb_sim_text_ +
                    "\nENTER review  ESC disconnect";
        }
        toast_->show_persistent(text);
    }

    bool handle_usb_sim_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC) {
            if (usb_sim_confirm_) {
                usb_sim_confirm_ = false;
                show_usb_sim_prompt();
            } else {
                usb_sim_active_ = false;
                usb_sim_connected_ = false;
                usb_sim_text_.clear();
                toast_->hide();
            }
            return true;
        }
        if (key == LV_KEY_ENTER) {
            if (!usb_sim_connected_) {
                usb_sim_connected_ = true;
                show_usb_sim_prompt("SIMULATED connection");
            } else if (!usb_sim_confirm_) {
                std::vector<platform::KeyboardReport> reports;
                std::string error;
                if (platform::make_simulated_keyboard_reports(usb_sim_text_, reports, error)) {
                    usb_sim_confirm_ = true;
                    show_usb_sim_prompt();
                } else {
                    show_usb_sim_prompt(error);
                }
            } else {
                std::vector<platform::KeyboardReport> reports;
                std::string error;
                if (!platform::make_simulated_keyboard_reports(usb_sim_text_, reports, error)) {
                    usb_sim_confirm_ = false;
                    show_usb_sim_prompt(error);
                } else {
                    usb_sim_text_.clear();
                    usb_sim_confirm_ = false;
                    show_usb_sim_prompt("SIMULATED " + std::to_string(reports.size()) +
                                        " reports; no input sent");
                }
            }
            return true;
        }
        if (!usb_sim_connected_ || usb_sim_confirm_) return true;
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (!usb_sim_text_.empty()) usb_sim_text_.pop_back();
        } else if (key >= 32 && key <= 126) {
            if (usb_sim_text_.size() == 64) {
                show_usb_sim_prompt("Input limit reached");
                return true;
            }
            usb_sim_text_.push_back(static_cast<char>(key));
        } else {
            show_usb_sim_prompt("Unsupported key");
            return true;
        }
        show_usb_sim_prompt();
        return true;
    }

    void show_selected_interface() {
        const auto& item = interfaces_[interface_selection_];
        std::string message = interfaces_ethernet_only_ ? "Host Ethernet "
                              : USE_DESKTOP ? "Host interface " : "Interface ";
        message += std::to_string(interface_selection_ + 1) + "/" +
                   std::to_string(interfaces_.size());
        message += "\n" + file_display_text(item.name, 24);
        message += !item.state_known ? "\nState unknown"
                   : item.connected ? "\nUp" : "\nDown";
        if (!item.address.empty()) message += "  " + item.address;
        message += "\nUP/DOWN select  ESC back";
        toast_->show_persistent(message);
    }

    void show_network_interfaces(bool ethernet_only = false) {
        auto interfaces = platform::list_network_interfaces();
        if (!interfaces.error.empty()) {
            toast_->show(std::string(ethernet_only ? "Ethernet\n" : "Interfaces\n") +
                             interfaces.error,
                         view::widgets::ToastTone::Error, 5200);
            return;
        }
        if (ethernet_only) {
            interfaces = platform::filter_ethernet_interfaces(std::move(interfaces));
        }
        if (interfaces.interfaces.empty()) {
            toast_->show(ethernet_only ? "No host Ethernet adapters found"
                                       : "No network interfaces found",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        interfaces_ = std::move(interfaces.interfaces);
        interfaces_ethernet_only_ = ethernet_only;
        interface_selection_ = 0;
        interfaces_active_ = true;
        show_selected_interface();
    }

    bool handle_interface_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            interfaces_active_ = false;
            interfaces_.clear();
            toast_->hide();
        } else if (key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z') {
            if (interface_selection_ > 0) {
                --interface_selection_;
            }
            show_selected_interface();
        } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c') {
            if (interface_selection_ + 1 < interfaces_.size()) {
                ++interface_selection_;
            }
            show_selected_interface();
        }
        return true;
    }

    void show_selected_arp_entry() {
        const auto& item = arp_entries_[arp_selection_];
        std::string message = USE_DESKTOP ? "Host ARP " : "ARP ";
        message += std::to_string(arp_selection_ + 1) + "/" +
                   std::to_string(arp_entries_.size()) + (arp_truncated_ ? "+" : "");
        message += "\n" + file_display_text(item.address, 24) +
                   "\n" + file_display_text(item.hardware_address, 24) +
                   "\n" + file_display_text(item.interface_name, 24) +
                   "\nUP/DOWN select  ESC back";
        toast_->show_persistent(message);
    }

    void show_arp_table() {
        auto result = platform::arp_entries();
        if (!result.error.empty()) {
            toast_->show("ARP table\n" + result.error,
                         view::widgets::ToastTone::Error, 5200);
            return;
        }
        if (result.entries.empty()) {
            toast_->show("Local ARP table is empty",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }
        arp_entries_ = std::move(result.entries);
        arp_truncated_ = result.truncated;
        arp_selection_ = 0;
        arp_active_ = true;
        show_selected_arp_entry();
    }

    bool handle_arp_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            arp_active_ = false;
            arp_entries_.clear();
            toast_->hide();
        } else if (key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z') {
            if (arp_selection_ > 0) {
                --arp_selection_;
            }
            show_selected_arp_entry();
        } else if (key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c') {
            if (arp_selection_ + 1 < arp_entries_.size()) {
                ++arp_selection_;
            }
            show_selected_arp_entry();
        }
        return true;
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

    void show_identity_and_connection() {
        const auto device = platform::read_device_status();
        const std::string gateway = platform::default_gateway();
        const bool connected = device.wifi_connected || device.ethernet_connected;
        const std::string message =
            platform::local_identity_text() +
            std::string("\nWi-Fi ") + (device.wifi_connected ? "connected" : "not connected") +
            "\nEthernet " + (device.ethernet_connected ? "connected" : "not connected") +
            (USE_DESKTOP ? "\nHost gateway " : "\nGateway ") + gateway;
        toast_->show(message,
                     connected ? view::widgets::ToastTone::Default
                               : view::widgets::ToastTone::Warning,
                     6000);
    }

    void show_chance_prompt() {
        static constexpr const char* kModes[] = {"DICE 2d6", "COIN", "8-BALL"};
        const std::string message =
            std::string(kModes[chance_mode_]) +
            "\nENTER=roll  UP/DOWN=mode\nESC=back";
        toast_->show_persistent(message);
    }

    void show_theme_prompt() {
        const auto id = static_cast<model::ThemeId>(theme_selection_);
        toast_->show_persistent(std::string("Theme: ") + view::theme_name(id) +
                                "\nArrows / Z-C preview\nENTER save  ESC cancel");
    }

    void start_theme_picker() {
        original_theme_selection_ = static_cast<int32_t>(view_model_.theme_id());
        theme_selection_ = original_theme_selection_;
        theme_picker_active_ = true;
        theme_persistence_.previewing = true;
        show_theme_prompt();
    }

    bool handle_theme_picker_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC) {
            view_model_.set_theme_id(
                static_cast<model::ThemeId>(original_theme_selection_));
            theme_persistence_.previewing = false;
            theme_picker_active_ = false;
            toast_->hide();
            return true;
        }
        if (key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z' ||
            key == LV_KEY_DOWN || key == LV_KEY_RIGHT || key == 'x' || key == 'c') {
            const bool previous =
                key == LV_KEY_UP || key == LV_KEY_LEFT || key == 'f' || key == 'z';
            const int32_t delta = previous ? -1 : 1;
            theme_selection_ =
                (theme_selection_ + delta + model::kThemeCount) % model::kThemeCount;
            view_model_.set_theme_id(static_cast<model::ThemeId>(theme_selection_));
            show_theme_prompt();
            return true;
        }
        if (key == LV_KEY_ENTER) {
            theme_picker_active_ = false;
            theme_persistence_.previewing = false;
            if (persist_theme(theme_persistence_, theme_selection_)) {
                toast_->show(std::string("Theme saved: ") +
                                 view::theme_name(static_cast<model::ThemeId>(theme_selection_)),
                             view::widgets::ToastTone::Success,
                             3200);
            } else {
                toast_->show("Theme selected but could not be saved",
                             view::widgets::ToastTone::Error,
                             4200);
            }
            return true;
        }
        return true;
    }

    void start_chance_tool() {
        chance_mode_ = 0;
        chance_tool_active_ = true;
        toast_->hide();
        show_chance_prompt();
    }

    bool handle_chance_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC) {
            chance_tool_active_ = false;
            toast_->hide();
            return true;
        }
        if (key == LV_KEY_UP || key == LV_KEY_DOWN || key == 'm' || key == 'M') {
            chance_mode_ = (chance_mode_ + (key == LV_KEY_UP ? 2 : 1)) % 3;
            show_chance_prompt();
            return true;
        }
        if (key != LV_KEY_ENTER && key != ' ') {
            return true;
        }

        std::string result;
        if (chance_mode_ == 0) {
            std::uniform_int_distribution<int> die(1, 6);
            const int first = die(random_engine_);
            const int second = die(random_engine_);
            result = "Dice\n" + std::to_string(first) + " + " +
                     std::to_string(second) + " = " +
                     std::to_string(first + second);
        }
        else if (chance_mode_ == 1) {
            std::uniform_int_distribution<int> coin(0, 1);
            result = coin(random_engine_) ? "Coin\nHEADS" : "Coin\nTAILS";
        }
        else {
            static constexpr const char* kAnswers[] = {
                "Yes", "Absolutely", "No", "Doubtful", "Ask again later",
                "Signs point to yes", "Very doubtful", "Without a doubt",
                "My reply is no", "As I see it, yes", "Cannot predict",
                "Outlook good", "Outlook not so good"
            };
            constexpr std::size_t kAnswerCount = sizeof(kAnswers) / sizeof(kAnswers[0]);
            std::uniform_int_distribution<std::size_t> answer(0, kAnswerCount - 1);
            result = std::string("8-ball\n") + kAnswers[answer(random_engine_)];
        }
        toast_->show_persistent(result + "\nENTER=roll  UP/DOWN=mode\nESC=back");
        return true;
    }

    bool create_visual_overlay() {
        toast_->hide();
        help_popup_->hide();
        overlay_ = lv_obj_create(lv_screen_active());
        if (!overlay_) {
            return false;
        }
        lv_obj_remove_style_all(overlay_);
        lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
        lv_obj_center(overlay_);
        lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
        screen_test_label_ = lv_label_create(overlay_);
        if (!screen_test_label_) {
            close_visual_tool();
            return false;
        }
        lv_obj_center(screen_test_label_);
        return true;
    }

    void close_visual_tool() {
        if (overlay_ && lv_obj_is_valid(overlay_)) {
            lv_obj_delete(overlay_);
        }
        overlay_ = nullptr;
        screen_test_label_ = nullptr;
        if (visual_tool_mode_ == VisualToolMode::Gallery) {
            lv_image_cache_drop(&gallery_image_);
            gallery_pixels_.clear();
        }
        visual_tool_mode_ = VisualToolMode::None;
    }

    void start_flashlight() {
        visual_tool_mode_ = VisualToolMode::Flashlight;
        if (!create_visual_overlay()) {
            visual_tool_mode_ = VisualToolMode::None;
            toast_->show("Flashlight could not open",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        lv_obj_set_style_bg_color(overlay_, lv_color_white(), 0);
        lv_label_set_text(screen_test_label_, "FLASHLIGHT\nPress any key to exit");
        lv_obj_set_style_text_color(screen_test_label_, lv_color_black(), 0);
    }

    void update_ambient_preview() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ambient_started_).count();
        const int phase = static_cast<int>((elapsed / 25) % 200);
        const int brightness = phase <= 100 ? phase : 200 - phase;
        const auto colors = view::palette(view_model_.theme_id());
        lv_obj_set_style_bg_color(
            overlay_, lv_color_mix(colors.primary, colors.background,
                                   static_cast<uint8_t>(brightness * 128 / 100)), 0);
    }

    void start_ambient_preview() {
        visual_tool_mode_ = VisualToolMode::AmbientPreview;
        if (!create_visual_overlay()) {
            visual_tool_mode_ = VisualToolMode::None;
            toast_->show("Ambient preview could not open",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        ambient_started_ = std::chrono::steady_clock::now();
        lv_label_set_text(screen_test_label_, "AMBIENT\nPress any key to exit");
        lv_obj_set_style_text_color(screen_test_label_,
                                    view::palette(view_model_.theme_id()).text, 0);
        update_ambient_preview();
    }

    void update_screensaver_clock() {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - ambient_started_).count();
        if (elapsed == screensaver_clock_second_) return;
        screensaver_clock_second_ = elapsed;
        const std::string clock = platform::current_clock_text(view_model_.clock_24h());
        lv_label_set_text(screen_test_label_, clock.empty() ? "Clock unavailable" : clock.c_str());
        const int x = static_cast<int>((elapsed / 3) % 3) - 1;
        const int y = static_cast<int>((elapsed / 9) % 3) - 1;
        lv_obj_align(screen_test_label_, LV_ALIGN_CENTER, x * 90, y * 50);
    }

    void update_screen_test() {
        static constexpr uint32_t kColors[] = {
            0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000, 0x00FFFF, 0xFFBF00
        };
        static constexpr const char* kNames[] = {
            "RED", "GREEN", "BLUE", "WHITE", "BLACK", "CYAN", "AMBER"
        };
        const uint32_t color = kColors[screen_test_index_];
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(color), 0);
        lv_obj_set_style_text_color(screen_test_label_,
                                    screen_test_index_ == 1 ||
                                            screen_test_index_ == 3 ||
                                            screen_test_index_ == 5 ||
                                            screen_test_index_ == 6
                                        ? lv_color_black()
                                        : lv_color_white(),
                                    0);
        char message[64]{};
        std::snprintf(message,
                      sizeof(message),
                      "%s  %d/7\nAny key=next  ESC=exit",
                      kNames[screen_test_index_],
                      screen_test_index_ + 1);
        lv_label_set_text(screen_test_label_, message);
    }

    void start_screen_test() {
        visual_tool_mode_ = VisualToolMode::ScreenTest;
        screen_test_index_ = 0;
        if (!create_visual_overlay()) {
            visual_tool_mode_ = VisualToolMode::None;
            toast_->show("Screen test could not open",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
        update_screen_test();
    }

    void update_ambient() {
        static constexpr uint32_t kPalettes[][3] = {
            {0x152B54, 0x25466A, 0x25335F},
            {0x4A2028, 0x623720, 0x51304C},
            {0x173E37, 0x24503C, 0x1B3B50},
        };
        const auto now = std::chrono::steady_clock::now();
        if (now < ambient_next_update_) {
            return;
        }
        ambient_next_update_ = now + std::chrono::milliseconds(100);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ambient_started_).count();
        constexpr int64_t kTransitionMs = 5000;
        const int64_t phase = theme_persistence_.config.reduced_motion ? 0 : elapsed;
        const std::size_t first = static_cast<std::size_t>((phase / kTransitionMs) % 3);
        const std::size_t second = (first + 1) % 3;
        const uint32_t from = kPalettes[ambient_palette_][first];
        const uint32_t to = kPalettes[ambient_palette_][second];
        const int64_t progress = phase % kTransitionMs;
        uint32_t color = 0;
        for (int shift : {16, 8, 0}) {
            const int64_t a = (from >> shift) & 0xff;
            const int64_t b = (to >> shift) & 0xff;
            const auto channel = static_cast<uint32_t>(
                (a * (kTransitionMs - progress) + b * progress) / kTransitionMs);
            color |= channel << shift;
        }
        lv_obj_set_style_bg_color(overlay_, lv_color_hex(color), 0);
    }

    void show_ambient_palette() {
        static constexpr const char* kNames[] = {"Ocean", "Sunset", "Forest"};
        char message[96]{};
        std::snprintf(message, sizeof(message),
                      "AMBIENT: %s\nLEFT/RIGHT=palette  ESC=exit",
                      kNames[ambient_palette_]);
        lv_label_set_text(screen_test_label_, message);
        ambient_started_ = std::chrono::steady_clock::now();
        ambient_next_update_ = ambient_started_;
        update_ambient();
    }

    void start_ambient() {
        visual_tool_mode_ = VisualToolMode::Ambient;
        ambient_palette_ = 0;
        if (!create_visual_overlay()) {
            visual_tool_mode_ = VisualToolMode::None;
            toast_->show("Ambient preview could not open",
                         view::widgets::ToastTone::Error, 3200);
            return;
        }
        lv_obj_set_style_text_color(screen_test_label_, lv_color_white(), 0);
        lv_obj_set_style_text_align(screen_test_label_, LV_TEXT_ALIGN_CENTER, 0);
        show_ambient_palette();
    }

    bool save_screensaver_settings(bool enabled, int32_t palette) {
        auto config = theme_persistence_.config;
        config.screensaver_enabled = enabled;
        config.screensaver_palette = palette;
        std::string error;
        if (!save_application_config(theme_persistence_.config_path, config, error)) {
            LOG_WARN("failed to save screensaver settings {}: {}",
                     theme_persistence_.config_path, error);
            toast_->show("Could not save screensaver settings\n" + error,
                         view::widgets::ToastTone::Error, 4200);
            return false;
        }
        theme_persistence_.config = config;
        view_model_.set_screensaver_enabled(enabled);
        view_model_.set_screensaver_palette(palette);
        lv_display_trigger_activity(nullptr);
        return true;
    }

    void toggle_screensaver() {
        const bool enabled = !view_model_.screensaver_enabled();
        if (save_screensaver_settings(enabled, view_model_.screensaver_palette())) {
            toast_->show(enabled ? "Screensaver enabled\nStarts after 2 minutes"
                                 : "Screensaver disabled",
                         view::widgets::ToastTone::Success, 3200);
        }
    }

    void show_screensaver_picker() {
        static constexpr const char* kNames[] = {
            "Theme colors", "Moving clock", "Ocean", "Sunset", "Forest"};
        toast_->show_persistent(std::string("Screensaver: ") + kNames[screensaver_selection_] +
                                "\nArrows choose\nENTER=save  ESC=cancel");
    }

    void start_screensaver_picker() {
        screensaver_selection_ = std::clamp(theme_persistence_.config.screensaver_style, 0, 4);
        screensaver_picker_active_ = true;
        show_screensaver_picker();
    }

    bool handle_screensaver_picker_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (key == LV_KEY_ESC) {
            screensaver_picker_active_ = false;
            toast_->hide();
        }
        else if (key == LV_KEY_ENTER) {
            screensaver_picker_active_ = false;
            auto config = theme_persistence_.config;
            config.screensaver_style = screensaver_selection_;
            if (screensaver_selection_ >= 2) {
                config.screensaver_palette = screensaver_selection_ - 2;
            }
            std::string error;
            if (!save_application_config(theme_persistence_.config_path, config, error)) {
                LOG_WARN("failed to save screensaver style: {}", error);
                toast_->show("Could not save screensaver style\n" + error,
                             view::widgets::ToastTone::Error, 4200);
            } else {
                theme_persistence_.config = config;
                view_model_.set_screensaver_palette(config.screensaver_palette);
                toast_->show("Screensaver style saved",
                             view::widgets::ToastTone::Success, 3200);
            }
        }
        else if (key == LV_KEY_LEFT || key == LV_KEY_UP || key == 'z' ||
                 key == LV_KEY_RIGHT || key == LV_KEY_DOWN || key == 'c' ||
                 key == 'f' || key == 'x') {
            screensaver_selection_ = (screensaver_selection_ +
                (key == LV_KEY_LEFT || key == LV_KEY_UP || key == 'z' || key == 'f' ? 4 : 1)) % 5;
            show_screensaver_picker();
        }
        return true;
    }

    void start_screensaver() {
        const int32_t style = theme_persistence_.config.screensaver_style;
        visual_tool_mode_ = style == 0 ? VisualToolMode::ScreensaverAmbient
                             : style == 1 ? VisualToolMode::ScreensaverClock
                                          : VisualToolMode::Screensaver;
        ambient_palette_ = style >= 2 ? style - 2 : view_model_.screensaver_palette();
        if (!create_visual_overlay()) {
            visual_tool_mode_ = VisualToolMode::None;
            lv_display_trigger_activity(nullptr);
            toast_->show("Screensaver could not open",
                         view::widgets::ToastTone::Error, 3200);
            return;
        }
        lv_obj_add_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(overlay_, screensaver_pressed, LV_EVENT_PRESSED, this);
        ambient_started_ = std::chrono::steady_clock::now();
        ambient_next_update_ = ambient_started_;
        screensaver_clock_second_ = -1;
        const auto colors = view::palette(view_model_.theme_id());
        lv_obj_set_style_bg_color(overlay_, colors.background, 0);
        lv_obj_set_style_text_color(screen_test_label_, colors.text, 0);
        if (visual_tool_mode_ == VisualToolMode::ScreensaverAmbient) {
            lv_label_set_text(screen_test_label_, "");
            update_ambient_preview();
        } else if (visual_tool_mode_ == VisualToolMode::ScreensaverClock) {
            update_screensaver_clock();
        } else {
            lv_label_set_text(screen_test_label_, "Press any key to wake");
            update_ambient();
        }
    }

    static void screensaver_pressed(lv_event_t* event) {
        auto* controller = static_cast<ShortcutController*>(lv_event_get_user_data(event));
        if (controller &&
            (controller->visual_tool_mode_ == VisualToolMode::Screensaver ||
             controller->visual_tool_mode_ == VisualToolMode::ScreensaverAmbient ||
             controller->visual_tool_mode_ == VisualToolMode::ScreensaverClock)) {
            controller->close_visual_tool();
            lv_display_trigger_activity(nullptr);
        }
    }

    bool handle_visual_tool_key(uint32_t key, bool long_pressed) {
        if (long_pressed) {
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::Screensaver ||
            visual_tool_mode_ == VisualToolMode::ScreensaverAmbient ||
            visual_tool_mode_ == VisualToolMode::ScreensaverClock ||
            visual_tool_mode_ == VisualToolMode::Flashlight || key == LV_KEY_ESC) {
            close_visual_tool();
            lv_display_trigger_activity(nullptr);
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::Ambient) {
            if (key == LV_KEY_LEFT || key == 'z' || key == LV_KEY_RIGHT || key == 'c') {
                ambient_palette_ = (ambient_palette_ +
                    (key == LV_KEY_LEFT || key == 'z' ? 2 : 1)) % 3;
                show_ambient_palette();
            }
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::AmbientPreview) {
            close_visual_tool();
            lv_display_trigger_activity(nullptr);
            return true;
        }
        screen_test_index_ = (screen_test_index_ + 1) % 7;
        update_screen_test();
        return true;
    }

    template <typename Features>
    void show_feature_status(const Features& features, int32_t selection) {
        if (selection < 0 ||
            static_cast<std::size_t>(selection) >= features.size()) {
            toast_->show("Feature selection is out of range",
                         view::widgets::ToastTone::Error,
                         3200);
            return;
        }
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
        if (boot_splash_.active()) {
            if (!long_pressed) {
                boot_splash_.finish();
            }
            return true;
        }
        if (help_popup_->visible()) {
            if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_ENTER ||
                                  key == platform::kKeyHelp || key == '?')) {
                help_popup_->hide();
            }
            return true;
        }
        if (gallery_loading_) {
            if (!long_pressed && key == LV_KEY_ESC) {
                gallery_task_->cancelled = true;
                gallery_loading_ = false;
                if (gallery_task_->camera) {
                    toast_->hide();
                } else {
                    show_file_browser();
                }
            }
            return true;
        }
        if (camera_confirm_) {
            if (!long_pressed && key == LV_KEY_ENTER) {
                start_camera_capture();
            } else if (!long_pressed && key == LV_KEY_ESC) {
                camera_confirm_ = false;
                toast_->hide();
            }
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::Gallery) {
            if (!long_pressed && key == LV_KEY_ESC) {
                close_visual_tool();
                if (!gallery_camera_) show_file_browser();
                gallery_camera_ = false;
            }
            return true;
        }
        if (music_active_) {
            return handle_music_key(key, long_pressed);
        }
        if (piano_active_) {
            return handle_piano_key(key, long_pressed);
        }
        if (microphone_active_) {
            return handle_microphone_key(key, long_pressed);
        }
        if (file_browser_active_) {
            return handle_file_browser_key(key, long_pressed);
        }
        if (interfaces_active_) {
            return handle_interface_key(key, long_pressed);
        }
        if (arp_active_) {
            return handle_arp_key(key, long_pressed);
        }
        if (wifi_active_) {
            return handle_wifi_key(key, long_pressed);
        }
        if (ir_active_) {
            return handle_ir_key(key, long_pressed);
        }
        if (usb_sim_active_) {
            return handle_usb_sim_key(key, long_pressed);
        }
        if (sound_picker_active_) {
            return handle_sound_picker_key(key, long_pressed);
        }
        if (screensaver_picker_active_) {
            return handle_screensaver_picker_key(key, long_pressed);
        }
        if (gatt_stage_ != GattStage::None && key != platform::kKeyPrintScreen) {
            return handle_gatt_key(key, long_pressed);
        }
        if (theme_picker_active_) {
            return handle_theme_picker_key(key, long_pressed);
        }
        if (chance_tool_active_) {
            return handle_chance_key(key, long_pressed);
        }
        if (visual_tool_mode_ != VisualToolMode::None) {
            return handle_visual_tool_key(key, long_pressed);
        }
        if (brightness_adjustment_ && key != platform::kKeyPrintScreen) {
            return handle_brightness_key(key, long_pressed);
        }

        if (text_tool_mode_ != TextToolMode::None && key == platform::kKeyHelp) {
            cancel_text_tool();
        }
        else if (text_tool_mode_ != TextToolMode::None &&
                 key != platform::kKeyPrintScreen) {
            return handle_text_tool_key(key, long_pressed);
        }

        if (view_model_.current_page() == model::AppPage::Apple) {
            if (key == LV_KEY_UP || key == 'f' || key == LV_KEY_LEFT || key == 'z') {
                if (!long_pressed) {
                    view_model_.move_menu_selection(-1);
                }
                return true;
            }
            if (key == LV_KEY_DOWN || key == 'x' || key == LV_KEY_RIGHT || key == 'c') {
                if (!long_pressed) {
                    view_model_.move_menu_selection(1);
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
            if (view_model_.current_page() == model::AppPage::Wireless) {
                if (key == LV_KEY_NEXT || key == LV_KEY_PREV) {
                    if (!long_pressed) {
                        const int32_t selection = view_model_.tool_selection();
                        view_model_.set_tool_selection(
                            selection < model::kWirelessReferenceItemCount
                                ? model::kWirelessReferenceItemCount : 0);
                    }
                    return true;
                }
                const int32_t reference_index = model::wireless_reference_shortcut(key);
                if (reference_index >= 0) {
                    if (!long_pressed) {
                        view_model_.set_tool_selection(reference_index);
                    }
                    return true;
                }
            }
            if (key == LV_KEY_UP || key == 'f') {
                if (!long_pressed) {
                    if (!view_model_.grid_layout() || view_model_.tool_selection() >= 2) {
                        view_model_.move_tool_selection(view_model_.grid_layout() ? -2 : -1);
                    }
                }
                return true;
            }
            if (key == LV_KEY_DOWN || key == 'x') {
                if (!long_pressed) {
                    const int32_t selection = view_model_.tool_selection();
                    const int32_t count = model::tool_item_count(view_model_.current_page());
                    if (!view_model_.grid_layout() || selection + 2 < count) {
                        view_model_.move_tool_selection(view_model_.grid_layout() ? 2 : 1);
                    }
                }
                return true;
            }
            if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == 'z' || key == 'c') {
                if (!long_pressed) {
                    const int32_t delta = key == LV_KEY_LEFT || key == 'z' ? -1 : 1;
                    const int32_t selection = view_model_.tool_selection();
                    if (!view_model_.grid_layout() ||
                        (delta < 0 && selection % 2 == 1) ||
                        (delta > 0 && selection % 2 == 0)) {
                        view_model_.move_tool_selection(delta);
                    }
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
                                start_file_browser();
                                break;
                            case 1:
                                start_text_tool(TextToolMode::Calculator);
                                break;
                            case 2: {
                                const std::string clock =
                                    platform::current_clock_text(view_model_.clock_24h());
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
                                show_identity_and_connection();
                                break;
                            case 8:
                                start_flashlight();
                                break;
                            case 9:
                                start_chance_tool();
                                break;
                            case 10:
                                start_screen_test();
                                break;
                            case 11:
                                start_theme_picker();
                                break;
                            case 12:
                                start_ambient();
                                break;
                            case 13:
                                toggle_layout();
                                break;
                            case 14:
                                toggle_screensaver();
                                break;
                            case 15:
                                start_screensaver_picker();
                                break;
                            case 16:
                                start_sound_picker();
                                break;
                            default:
                                break;
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Butter) {
                        if (view_model_.tool_selection() == 8) {
                            start_theme_picker();
                        } else if (view_model_.tool_selection() == 9) {
                            toggle_clock_format();
                        } else {
                            show_system_status(view_model_.tool_selection());
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Network) {
                        switch (view_model_.tool_selection()) {
                            case 0:
                                show_network_interfaces();
                                break;
                            case 1: {
                                const std::string gateway = platform::default_gateway();
                                toast_->show(std::string(USE_DESKTOP ? "Host gateway\n"
                                                                    : "Default gateway\n") + gateway,
                                             gateway == "No gateway" ||
                                                     gateway.find("Route table") == 0 ||
                                                     gateway == "Gateway data invalid"
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
                                show_feature_status(platform::network_features(),
                                                    view_model_.tool_selection());
                                break;
                            case 5:
                                if (USE_DESKTOP) {
                                    show_network_interfaces(true);
                                } else {
                                    show_system_status(3);
                                }
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
                        const int32_t selection = view_model_.tool_selection();
                        if (selection == 0) {
                            start_wifi_scan();
                            return true;
                        }
                        if (selection < model::kWirelessReferenceItemCount) {
                            toast_->show("Reference tool unavailable\nNo radio action is run",
                                         view::widgets::ToastTone::Warning, 3200);
                            return true;
                        }
                        const int32_t wireless_selection =
                            selection - model::kWirelessReferenceItemCount;
                        switch (wireless_selection) {
                            case 0:
                            case 1:
                                show_system_status(1);
                                break;
                            case 2:
                                show_system_status(2);
                                break;
                            case 3:
                                start_gatt_scan();
                                break;
                            case 5:
                                start_wifi_scan();
                                break;
                            default:
                                show_feature_status(platform::wireless_features(),
                                                    wireless_selection);
                                break;
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Infrared) {
                        if (view_model_.tool_selection() == 0 ||
                            view_model_.tool_selection() == 6) {
                            if (!ir_task_) {
                                ir_learning_ = view_model_.tool_selection() == 6;
                                ir_result_ = {};
                                ir_selection_ = 0;
                            }
                            start_ir_receive();
                        } else {
                            show_feature_status(platform::infrared_features(),
                                                view_model_.tool_selection());
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Expansion) {
#if USE_DESKTOP
                        if (view_model_.tool_selection() == 7) {
                            usb_sim_active_ = true;
                            usb_sim_connected_ = false;
                            usb_sim_confirm_ = false;
                            usb_sim_text_.clear();
                            show_usb_sim_prompt();
                            return true;
                        }
#endif
                        show_feature_status(platform::expansion_features(),
                                            view_model_.tool_selection());
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Media) {
                        if (view_model_.tool_selection() == 0) {
                            camera_confirm_ = true;
                            toast_->show_persistent("Camera snapshot\nImage stays in memory\nENTER capture  ESC back");
                            return true;
                        }
                        if (view_model_.tool_selection() == 1) {
                            start_microphone_test(true);
                            return true;
                        }
                        if (view_model_.tool_selection() == 2) {
                            start_file_browser(FileMode::Music);
                            return true;
                        }
                        if (view_model_.tool_selection() == 3) {
                            piano_active_ = true;
                            piano_error_.clear();
                            show_piano();
                            return true;
                        }
                        if (view_model_.tool_selection() == 4) {
                            start_file_browser(FileMode::AlbumArt);
                            return true;
                        }
                        if (view_model_.tool_selection() == 5) {
                            start_microphone_test();
                            return true;
                        }
                        if (view_model_.tool_selection() == 6) {
                            ui_sound_.retry();
                            ui_sound_.play(50);
                            toast_->show("Speaker test requested\nListen for a click",
                                         view::widgets::ToastTone::Default, 4200);
                            return true;
                        }
                        if (view_model_.tool_selection() == 7) {
                            start_file_browser(FileMode::Gallery);
                        } else {
                            show_feature_status(platform::media_features(),
                                                view_model_.tool_selection());
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
                                             5200);
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

        if (key == platform::kKeyHelp || key == '?') {
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

    static void pointer_sound_listener(lv_event_t* event) {
        auto* controller = static_cast<ShortcutController*>(lv_event_get_user_data(event));
        if (controller && !controller->music_active_ && !controller->piano_active_ &&
            !controller->microphone_active_ &&
            controller->theme_persistence_.config.sound_enabled) {
            controller->ui_sound_.play(controller->theme_persistence_.config.sound_volume);
        }
    }

    static bool global_key_listener(uint32_t key,
                                    const char*,
                                    bool long_pressed,
                                    void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        if (controller && !controller->music_active_ && !controller->piano_active_ &&
            !controller->microphone_active_ && !long_pressed &&
            controller->theme_persistence_.config.sound_enabled) {
            controller->ui_sound_.play(controller->theme_persistence_.config.sound_volume);
        }
        return controller && controller->handle_key(key, long_pressed);
    }

    static void key_release_listener(uint32_t key, const char*, void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        if (controller) {
            controller->handle_release(key);
        }
    }

    viewmodel::BaseViewModel& view_model_;
    ThemePersistence& theme_persistence_;
    BootSplash& boot_splash_;
    platform::UiSound ui_sound_;
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::string text_tool_buffer_;
    TextToolMode text_tool_mode_{TextToolMode::None};
    bool file_browser_active_{false};
    FileMode file_mode_{FileMode::Files};
    bool music_active_{false};
    bool piano_active_{false};
    std::string piano_error_;
    bool microphone_active_{false};
    bool microphone_confirm_{false};
    bool microphone_recording_mode_{false};
    bool microphone_ready_{false};
    std::vector<int16_t> microphone_samples_;
    std::shared_ptr<MicrophoneTask> microphone_task_;
    std::thread microphone_thread_;
    platform::MusicState music_state_{platform::MusicState::Stopped};
    std::string music_name_;
    std::string music_error_;
    std::filesystem::path file_directory_;
    std::vector<platform::FileEntry> file_entries_;
    std::size_t file_selection_{0};
    std::size_t file_offset_{0};
    bool file_truncated_{false};
    std::string file_notice_;
    std::shared_ptr<FileQueryState> file_task_;
    std::thread file_thread_;
    bool gallery_loading_{false};
    bool gallery_camera_{false};
    bool camera_confirm_{false};
    std::shared_ptr<GalleryQueryState> gallery_task_;
    std::thread gallery_thread_;
    std::vector<uint8_t> gallery_pixels_;
    lv_image_dsc_t gallery_image_{};
    std::shared_ptr<NetworkQueryState> network_query_;
    std::thread network_thread_;
    bool interfaces_active_{false};
    bool interfaces_ethernet_only_{false};
    std::vector<platform::NetworkInterface> interfaces_;
    std::size_t interface_selection_{0};
    bool arp_active_{false};
    bool arp_truncated_{false};
    std::vector<platform::ArpEntry> arp_entries_;
    std::size_t arp_selection_{0};
    std::shared_ptr<WifiTask> wifi_task_;
    std::thread wifi_thread_;
    platform::WifiScanResult wifi_result_;
    std::size_t wifi_selection_{0};
    bool wifi_active_{false};
    bool wifi_scanning_{false};
    std::shared_ptr<IrTask> ir_task_;
    std::thread ir_thread_;
    platform::IrReceiveResult ir_result_;
    std::size_t ir_selection_{0};
    bool ir_active_{false};
    bool ir_receiving_{false};
    bool ir_learning_{false};
    std::string ir_notice_;
    std::shared_ptr<GattTask> gatt_task_;
    std::thread gatt_thread_;
    GattStage gatt_stage_{GattStage::None};
    std::vector<platform::BluetoothDevice> gatt_devices_;
    std::vector<platform::GattEntry> gatt_entries_;
    std::size_t gatt_device_index_{0};
    std::size_t gatt_entry_index_{0};
    std::string gatt_error_;
    std::chrono::steady_clock::time_point stopwatch_started_{};
    std::chrono::steady_clock::duration stopwatch_elapsed_{};
    bool stopwatch_running_{false};
    bool brightness_adjustment_{false};
    int32_t brightness_percent_{0};
    uint32_t exit_key_pressed_{0};
    bool exit_armed_{false};
    bool chance_tool_active_{false};
    int32_t chance_mode_{0};
    std::mt19937 random_engine_{
        static_cast<std::mt19937::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    VisualToolMode visual_tool_mode_{VisualToolMode::None};
    lv_obj_t* overlay_{nullptr};
    lv_obj_t* screen_test_label_{nullptr};
    int32_t screen_test_index_{0};
    int32_t ambient_palette_{0};
    std::chrono::steady_clock::time_point ambient_started_{};
    std::chrono::steady_clock::time_point ambient_next_update_{};
    bool screensaver_picker_active_{false};
    int32_t screensaver_selection_{0};
    int64_t screensaver_clock_second_{-1};
    bool sound_picker_active_{false};
    bool sound_selection_enabled_{false};
    int32_t sound_selection_volume_{50};
    bool usb_sim_active_{false};
    bool usb_sim_connected_{false};
    bool usb_sim_confirm_{false};
    std::string usb_sim_text_;
    bool theme_picker_active_{false};
    int32_t theme_selection_{0};
    int32_t original_theme_selection_{0};
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
        if (config.theme_id_set) {
            view_model.set_theme_id(static_cast<model::ThemeId>(config.theme_id));
        } else {
            view_model.set_dark_mode(config.dark_mode);
        }
        view_model.set_clock_24h(config.clock_24h);
        view_model.set_spacious_layout(config.spacious_layout);
        view_model.set_screensaver_enabled(config.screensaver_enabled);
        view_model.set_screensaver_palette(config.screensaver_palette);
        view_model.set_grid_layout(config.grid_layout);
        LOG_INFO("loaded config: {} (theme={})",
                 loaded_config_path,
                 view::theme_name(view_model.theme_id()));
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
                           view_model.theme_id(),
                           standard_font ? standard_font : LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    ScreenManager screen_manager(view_model, assets);
    screen_manager.start();
    ThemePersistence theme_persistence{
        user_config_path, config, static_cast<int32_t>(view_model.theme_id())};
    theme_persistence.config.theme_id = static_cast<int32_t>(view_model.theme_id());
    theme_persistence.config.theme_id_set = true;
    theme_persistence.config.dark_mode = view_model.is_dark_mode();
    BootSplash boot_splash(assets);
    ShortcutController shortcuts(view_model, assets, theme_persistence, boot_splash);
    if (!store_capture_mode) {
        boot_splash.start(config.reduced_motion);
    }

    auto* dark_mode_observer =
        store_capture_mode
            ? nullptr
            : lv_subject_add_observer(view_model.theme_id_subject(),
                                      persist_theme_observer,
                                      &theme_persistence);

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
        boot_splash.update();
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
