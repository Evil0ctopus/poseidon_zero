/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "adsb.h"
#include "adsb_receiver.h"
#include "asset_manager.h"
#include "backlight.h"
#include "bluetooth_gatt.h"
#include "camera_capture.h"
#include "capabilities.h"
#include "device_status.h"
#include "wireless_event_monitor.h"
#include "help_popup.h"
#include "logger.h"
#include "linux_input.h"
#include "memory_status.h"
#include "microphone.h"
#include "screenshot_service.h"
#include "safe_tools.h"
#include "screen_manager.h"
#include "storage_status.h"
#include "survey.h"
#include "survey_file.h"
#include "survey_qr.h"
#include "base_viewmodel.h"
#include "tool_status.h"
#include "ui_sound.h"
#include "unique_file.h"
#include "usb_keyboard_simulator.h"
#include "wifi_scan.h"
#include "ir_receive.h"
#include "linux_network.h"
#include "lab_honeypot.h"
#include "network_inventory.h"
#include "evidence_export.h"
#include "local_script.h"
#include "port_scan.h"
#include "readiness.h"
#include "feature_catalog.h"
#include "expansion_inventory.h"
#include "gallery_image.h"
#include "theme.h"
#include "toast.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
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
#include <numeric>
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

std::string adsb_velocity_text(const platform::AdsbFrame& frame) {
    std::string text;
    if (frame.speed_available) {
        const char* label = frame.speed_type == platform::AdsbSpeedType::Ground ? "GS" :
                            frame.speed_type == platform::AdsbSpeedType::TrueAirspeed ? "TAS" : "IAS";
        text += "\n" + std::string(label) + ": " + std::to_string(std::lround(frame.speed_knots)) + " kt";
    }
    if (frame.direction_available) {
        const char* label = frame.speed_type == platform::AdsbSpeedType::Ground ? "Track" : "Mag hdg";
        text += frame.speed_available ? "  " : "\n";
        text += std::string(label) + ": " + std::to_string(std::lround(frame.direction_degrees) % 360) + " deg";
    }
    if (frame.vertical_rate_available)
        text += std::string("\n") + (frame.vertical_rate_barometric ? "Baro" : "GNSS") +
                " rate: " + std::to_string(frame.vertical_rate_fpm) + " ft/min";
    return text;
}

std::string adsb_status_text(const platform::AdsbFrame& frame) {
    if (!frame.emergency_status_available) return {};
    static constexpr const char* labels[] = {
        "No emergency", "General emergency", "Medical priority", "Minimum fuel",
        "No communications", "Unlawful interference", "Downed aircraft", "Reserved"
    };
    if (frame.emergency_status >= sizeof(labels) / sizeof(labels[0]))
        return "\nStatus: invalid";
    std::string text = "\nReported: " + std::string(labels[frame.emergency_status]);
    text += frame.squawk.empty() ? "\nSquawk unavailable" : "\nSquawk: " + frame.squawk;
    return text;
}

std::string adsb_category_text(const platform::AdsbFrame& frame) {
    static constexpr const char* categories[][8] = {
        {"", "Emergency vehicle", "Service vehicle", "Point obstacle", "Obstacle cluster",
         "Line obstacle", "Reserved", "Reserved"},
        {"", "Glider", "Lighter than air", "Parachutist", "Ultralight",
         "Reserved", "Unmanned aircraft", "Space vehicle"},
        {"", "Light aircraft", "Small aircraft", "Large aircraft", "High vortex aircraft",
         "Heavy aircraft", "High performance", "Rotorcraft"}
    };
    if (frame.type_code < 2 || frame.type_code > 4 ||
        frame.emitter_category <= 0 || frame.emitter_category > 7)
        return {};
    return "\nCategory: " + std::string(categories[frame.type_code - 2][frame.emitter_category]);
}

std::string adsb_frame_details(const platform::AdsbFrame& frame) {
    std::string text;
    if (!frame.callsign.empty()) text += "\nCallsign: " + frame.callsign;
    text += adsb_category_text(frame);
    if (frame.altitude_available)
        text += "\nBaro: " + std::to_string(frame.altitude_feet) + " ft";
    text += adsb_velocity_text(frame);
    text += adsb_status_text(frame);
    if (frame.position_available) {
        char coordinates[64];
        const int length = std::snprintf(coordinates, sizeof(coordinates), "%.4f, %.4f",
                                         frame.latitude, frame.longitude);
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(coordinates))
            text += "\nReported: " + std::string(coordinates);
    }
    return text;
}

const char* favorite_tool_name(model::AppPage page, int32_t index) {
    if (index < 0 || index >= model::tool_item_count(page)) return nullptr;
    switch (page) {
        case model::AppPage::Butter: return model::kSystemToolNames[index];
        case model::AppPage::Network: return platform::network_features()[index].name;
        case model::AppPage::Wireless:
            return index < model::kWirelessReferenceItemCount
                ? model::kWirelessReferenceItems[index].label
                : platform::wireless_features()[index - model::kWirelessReferenceItemCount].name;
        case model::AppPage::Infrared: return platform::infrared_features()[index].name;
        case model::AppPage::Expansion: return platform::expansion_features()[index].name;
        case model::AppPage::Tools: return platform::tools_features()[index].name;
        case model::AppPage::Media: return platform::media_features()[index].name;
        default: return nullptr;
    }
}

std::string favorite_id(model::AppPage page, int32_t index) {
    const char* name = favorite_tool_name(page, index);
    if (!name) return {};
    return std::to_string(static_cast<int>(page)) + ":" + name;
}

const char* favorite_page_name(char page) {
    switch (page) {
        case '1': return "System";
        case '2': return "Network";
        case '3': return "Wireless";
        case '4': return "Infrared";
        case '5': return "Expansion";
        case '6': return "Tools";
        case '7': return "Media";
        default: return "Unknown";
    }
}

struct AccessoryProfile {
    const char* name;
    const char* guidance;
};

constexpr std::array<AccessoryProfile, 5> kAccessoryProfiles = {{
    {"GPS Cap", "GNSS serial path required"},
    {"Wireless-Tag ESP32-C5", "2.4 GHz; firmware unverified"},
    {"Monster C5", "2.4 GHz; firmware unverified"},
    {"RTL-SDR", "ADS-B needs rtl_adsb"},
    {"nRF24", "No supported radio driver"}
}};
static_assert(kAccessoryProfileMask == (uint32_t{1} << kAccessoryProfiles.size()) - 1);

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
          assets_(assets),
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
        if (survey_session_) survey_session_->stop();
        survey_session_.reset();
        if (surveillance_task_) surveillance_task_->cancelled = true;
        if (surveillance_thread_.joinable()) surveillance_thread_.join();
        if (lab_listener_) lab_listener_->stop();
        lab_listener_.reset();
        if (wireless_event_monitor_) wireless_event_monitor_->stop();
        wireless_event_monitor_.reset();
        if (lab_export_thread_.joinable()) lab_export_thread_.join();
        if (security_log_thread_.joinable()) security_log_thread_.join();
        if (survey_file_task_) survey_file_task_->cancelled = true;
        if (survey_file_thread_.joinable()) survey_file_thread_.join();
        if (note_task_) note_task_->cancelled = true;
        if (note_thread_.joinable()) note_thread_.join();
        if (settings_backup_thread_.joinable()) settings_backup_thread_.join();
        if (port_task_) port_task_->progress.cancelled = true;
        if (port_thread_.joinable()) port_thread_.join();
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
        adsb_receiver_.reset();
        if (readiness_thread_.joinable()) readiness_thread_.join();
        if (script_task_) script_task_->cancelled = true;
        if (script_thread_.joinable()) script_thread_.join();
        if (file_task_) {
            file_task_->cancelled = true;
        }
        if (file_thread_.joinable()) {
            file_thread_.join();
        }
        if (file_hash_task_) file_hash_task_->cancelled = true;
        if (file_hash_thread_.joinable()) file_hash_thread_.join();
        if (gallery_task_) {
            gallery_task_->cancelled = true;
        }
        if (gallery_thread_.joinable()) {
            gallery_thread_.join();
        }
        close_visual_tool();
    }

    void update() {
        update_adsb_receiver();
        update_readiness();
        update_network_query();
        update_script();
        update_gatt();
        update_wifi();
        update_survey();
        update_field_notes();
        update_settings_backup();
        update_surveillance();
        update_lab_listener();
        update_wireless_event_monitor();
        update_security_logs();
        update_port_scan();
        update_ir();
        update_file_browser();
        update_file_hash();
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
                 survey_stage_ == SurveyStage::None &&
                 surveillance_stage_ == SurveillanceStage::None &&
                 lab_listener_stage_ == LabListenerStage::None &&
                 wireless_event_stage_ == WirelessEventStage::None &&
                 security_log_stage_ == SecurityLogStage::None &&
                 !ap_signal_test_active_ &&
                 port_stage_ == PortStage::None && !port_task_ &&
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
        Adsb,
        Ping,
        Dns,
        Https,
    };

    enum class LabListenerStage { None, Address, Confirm, Running };
    enum class WirelessEventStage { None, Interface, Channel, Confirm, Running };
    enum class SecurityLogStage { None, Working, List, ConfirmDelete, ConfirmExport, ConfirmDeleteAll, Notice };
    enum class SecurityLogAction { List, Delete, Export, ListExports, DeleteExport, DeleteAllExports };

    struct ReadinessTask {
        std::atomic_bool complete{false};
        platform::ReadinessReport report;
        std::string error;
    };

    struct SecurityLogTask {
        std::atomic_bool complete{false};
        SecurityLogAction action{SecurityLogAction::List};
        platform::SecurityLogList logs;
        platform::EvidenceFileList exports;
        std::string error;
    };

    struct LabExportTask {
        std::atomic_bool complete{false};
        bool deleting{false};
        platform::EvidenceExportResult result;
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
        SurveyPlot,
        SurveyMap,
        SurveyQr,
    };

    enum class FileMode {
        Files,
        Gallery,
        Music,
        AlbumArt,
        I2c,
        Scripts,
    };

    enum class ScriptStage { None, Loading, Preview, Confirm, Running, Result };
    struct ScriptTask {
        std::atomic_bool cancelled{false};
        std::atomic_bool complete{false};
        bool loading{false};
        platform::LocalScript script;
        platform::ScriptResult result;
    };

    struct NetworkQueryState {
        std::mutex mutex;
        std::string host;
        std::string result;
        bool dns{false};
        bool https{false};
        bool internet{false};
        bool connected{false};
        bool complete{false};
    };

    struct FileQueryState {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::filesystem::path directory;
        std::size_t offset{0};
        bool create_directory{false};
        platform::DirectoryResult result;
        bool complete{false};
    };

    struct FileHashTask {
        std::atomic_bool cancelled{false};
        std::atomic_bool complete{false};
        platform::FileHashResult result;
    };

    struct GalleryQueryState {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        platform::GalleryImage result;
        bool complete{false};
        bool camera{false};
        bool saving{false};
        bool survey_map{false};
        platform::SurveyMapTile map_tile;
        std::string map_attribution;
        std::vector<uint32_t> map_attribution_glyphs;
        std::vector<uint8_t> jpeg;
        platform::CameraSaveResult save_result;
    };

    enum class GattStage {
        None,
        ConfirmTracker,
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
        bool connecting{false};
        std::string connection_error;
        platform::WifiScanResult result;
    };

    enum class SurveillanceStage { None, Confirm, Scanning, Results };
    struct SurveillanceTask {
        std::atomic_bool cancelled{false};
        std::atomic_bool complete{false};
        platform::SurveillanceScanResult result;
    };

    enum class WifiStage { Results, SecurityAdvice, ChooseConnection, Password, Confirm, Connecting, Notice };

    enum class SurveyStage { None, Home, Running, Stopping, Sessions, Timeline, MapNotice, QrNotice, ConfirmQr, ConfirmOne, ConfirmExport,
                             ConfirmGpx, ConfirmAll, FileOperation, Notice };
    enum class SurveyFileAction { List, DeleteOne, DeleteAll, Export, Timeline, Gpx };
    struct SurveyFileTask {
        std::atomic_bool complete{false};
        std::atomic_bool cancelled{false};
        SurveyFileAction action{SurveyFileAction::List};
        platform::SurveyFiles files;
        platform::SurveyTimeline timeline;
        platform::SurveyGpxExport gpx;
        platform::EvidenceExportResult evidence;
        std::string error;
    };

    enum class NoteStage { None, List, Edit, Viewing, ConfirmDelete, Working, Notice };
    enum class NoteAction { List, Save, Read, Delete };
    struct NoteTask {
        std::atomic_bool complete{false};
        std::atomic_bool cancelled{false};
        NoteAction action{NoteAction::List};
        platform::SurveyFiles files;
        platform::FieldNote note;
        std::string error;
    };

    enum class SettingsBackupStage { None, Confirm, Working, Notice };
    struct SettingsBackupTask {
        std::atomic_bool complete{false};
        SettingsBackup result;
    };

    enum class PortStage { None, InventoryHome, InventoryHistory, InventoryRestoring, InventoryListing,
                           InventoryLoading, InventoryDeleting, InventoryDeleteConfirm,
                           InventoryExportConfirm, InventoryExporting,
                           InventoryComparison, InventoryPorts, InventoryExclusions,
                           InventoryLabel, InventoryUpdateNote, InventoryReminder, InventoryPolicy,
                           RouterConfirm, RouterLoading,
                           NmapChoice, NmapConfirm, NmapRunning, NmapDetails,
                           Target, Protocol, Ports, Custom, DnsConsent,
                           Resolving, Review, ReviewPorts, Scanning, Results, Saving, Notice };
    enum class InventoryAction { Restore, List, Load, Delete, Export };
    struct PortTask {
        platform::PortScanProgress progress;
        std::atomic_bool complete{false};
        platform::PortTargets targets;
        platform::PortScanResult result;
        platform::PortReportResult report;
        platform::NetworkInventory inventory;
        platform::InventoryHistory history;
        platform::InventoryBaseline baseline;
        platform::NmapResult nmap;
        platform::RouterClients router_clients;
        platform::EvidenceExportResult evidence;
        InventoryAction inventory_action{InventoryAction::List};
        bool inventory_select_baseline{false};
        bool inventory_deleted{false};
        std::string inventory_file_error;
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
                    const auto error = platform::ensure_private_directory(directory);
                    if (!error.empty()) {
                        result.error = "Recording folder: " + error;
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
                    const auto error = platform::ensure_private_directory(directory);
                    if (!error.empty()) {
                        result.error = "IR folder: " + error;
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
            if (ir_result_.truncated) message += "\nIncomplete; cannot save";
        }
        if (!ir_notice_.empty()) message += "\n" + file_display_text(ir_notice_, 30);
        message += ir_learning_
                       ? "\nUP/DOWN  ENTER receive  S save\nESC back"
                       : "\nUP/DOWN select  ENTER receive\nESC back";
        toast_->show_persistent(message, ir_result_.error.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    void show_ir_receiver_check() {
        const auto status = platform::inspect_ir_receiver();
        if (status.available) {
            toast_->show("IR receiver ready\n" + status.device_path +
                             "\nNo signal was captured or sent",
                         view::widgets::ToastTone::Default, 4200);
        } else {
            toast_->show("IR receiver unavailable\n" + status.error,
                         view::widgets::ToastTone::Warning, 4200);
        }
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

    void show_port_prompt(const std::string& notice = {}) {
        if (!notice.empty()) {
            port_return_stage_ = port_stage_;
            port_stage_ = PortStage::Notice;
            port_notice_ = file_display_text(notice, 512);
            port_notice_page_ = 0;
        }
        std::string text;
        switch (port_stage_) {
            case PortStage::InventoryHome:
                text = "Host discovery\n1 New scan  2 History\n";
                text += inventory_baseline_name_.empty() ? "No baseline selected" : "Baseline selected";
                text += "\nESC close";
                break;
            case PortStage::InventoryPorts:
                text = "Discovery TCP ports\n1 Default 22,80,443,445\n2 Custom (up to 8)\nESC returns";
                break;
            case PortStage::RouterConfirm:
                text = "Import local router CSV?\n~/poseidon-reports/\nrouter-clients.csv\nENTER import  ESC cancel";
                break;
            case PortStage::RouterLoading:
                text = "Reading router export...";
                break;
            case PortStage::InventoryExclusions: {
                const auto shown = inventory_exclusions_.size() > 35
                    ? "..." + inventory_exclusions_.substr(inventory_exclusions_.size() - 35)
                    : inventory_exclusions_;
                text = "Exclude IPs (comma-separated)\n" + shown +
                       "\nENTER review  ESC returns";
                break;
            }
            case PortStage::InventoryLabel:
            case PortStage::InventoryUpdateNote:
            case PortStage::InventoryReminder: {
                const auto shown = inventory_edit_buffer_.size() > 35
                    ? "..." + inventory_edit_buffer_.substr(inventory_edit_buffer_.size() - 35)
                    : inventory_edit_buffer_;
                text = (port_stage_ == PortStage::InventoryLabel ? "User label\n" :
                        port_stage_ == PortStage::InventoryUpdateNote ? "Manual update note\n" :
                        "Reminder date YYYY-MM-DD\n") + shown + "\nENTER apply  ESC cancel";
                break;
            }
            case PortStage::InventoryPolicy: {
                const auto shown = inventory_policy_text_.size() > 35
                    ? "..." + inventory_policy_text_.substr(inventory_policy_text_.size() - 35)
                    : inventory_policy_text_;
                text = "Restricted TCP ports (max 32)\n" + shown +
                       "\nENTER review  ESC returns";
                break;
            }
            case PortStage::InventoryHistory:
                if (inventory_history_.empty()) {
                    text = "No discovery reports\nESC returns";
                } else {
                    const auto& name = inventory_history_[inventory_history_selection_];
                    text = "History " + std::to_string(inventory_history_selection_ + 1) + "/" +
                           std::to_string(inventory_history_.size()) + "\n" +
                           name.substr(std::string("poseidon-host-discovery-").size()) +
                           "\nLEFT/RIGHT ENTER view\nB baseline  D delete  X export";
                }
                break;
            case PortStage::InventoryListing:
            case PortStage::InventoryRestoring:
            case PortStage::InventoryLoading:
            case PortStage::InventoryDeleting:
            case PortStage::InventoryExporting:
                text = "Working with reports...";
                break;
            case PortStage::InventoryDeleteConfirm:
                text = "Delete selected report?\nThis cannot be undone.\nENTER delete  ESC cancel";
                break;
            case PortStage::InventoryExportConfirm:
                text = "Copy selected report?\nExport contains sensitive data.\n"
                       "ENTER copy + SHA-256  ESC";
                break;
            case PortStage::InventoryComparison: {
                const auto comparison = platform::compare_network_inventories(
                    inventory_baseline_, inventory_result_);
                if (!comparison.comparable) {
                    text = "Cannot compare: scan targets\nor ports differ.\nESC returns";
                } else {
                    text = (inventory_baseline_.scan.cancelled || inventory_result_.scan.cancelled ||
                            !inventory_baseline_.scan.error.empty() || !inventory_result_.scan.error.empty() ||
                            inventory_baseline_.scan.truncated || inventory_result_.scan.truncated ||
                            inventory_baseline_.scan.completed != inventory_baseline_.scan.total ||
                            inventory_result_.scan.completed != inventory_result_.scan.total
                                ? "Partial comparison\n" : "Compared to baseline\n");
                    text += "New responses: " +
                        std::to_string(comparison.newly_responding) +
                        "  not seen: " + std::to_string(comparison.not_seen) +
                        "\nMAC differences: " + std::to_string(comparison.mac_changed) +
                        "\nNewly observed open: " + std::to_string(comparison.newly_open_ports) +
                        "  svc diff: " + std::to_string(comparison.service_changes) +
                        "\nD details  ESC returns";
                }
                break;
            }
            case PortStage::NmapChoice:
                text = "Nmap inspection\n1 Services  2 Services + OS\nOS uses raw packets/privilege\nESC returns";
                break;
            case PortStage::NmapConfirm:
                text = "Inspect authorized host?\n" +
                       inventory_result_.hosts[port_selection_ - 1].address +
                       "\nTCP " + platform::format_port_list(inventory_result_.scan.ports) +
                       (nmap_os_ ? "\nPlus OS fingerprint packets" : "\nService probes send data") +
                       "\nENTER confirm  ESC cancel";
                break;
            case PortStage::NmapRunning:
                text = "Nmap inspecting host...\nESC cancels";
                break;
            case PortStage::NmapDetails: {
                constexpr std::size_t width = 90;
                const auto pages = std::max<std::size_t>(1, (nmap_detail_.size() + width - 1) / width);
                text = inventory_detail_title_ + " " + std::to_string(nmap_detail_page_ + 1) + "/" +
                       std::to_string(pages) + "\n" +
                       nmap_detail_.substr(nmap_detail_page_ * width, width) +
                       "\nLEFT/RIGHT  ESC returns";
                break;
            }
            case PortStage::Target: {
                const auto shown = port_target_.size() > 35
                    ? "..." + port_target_.substr(port_target_.size() - 35) : port_target_;
                text += (inventory_mode_ ? "Discovery IP or CIDR (max 256)\n" :
                                          "Target IP, CIDR or hostname\n") +
                        shown + "\nENTER next  ESC close";
                break;
            }
            case PortStage::Protocol:
                text += "Protocol: 1 TCP  2 UDP\n3 both  ESC close";
                break;
            case PortStage::Ports:
                text += "Ports: ENTER/1 common\n2 all  3 custom  ESC close";
                break;
            case PortStage::Custom: {
                const auto shown = port_spec_.size() > 35
                    ? "..." + port_spec_.substr(port_spec_.size() - 35) : port_spec_;
                text += "Ports: 80,443,1000-1010\n" + shown + "\nENTER next  ESC close";
                break;
            }
            case PortStage::DnsConsent:
                text += "DNS may contact a server.\nResolve only authorized targets?\nENTER yes  ESC cancel";
                break;
            case PortStage::Resolving:
                text += "Resolving target...\nESC cancels";
                break;
            case PortStage::Review:
                if (inventory_mode_) {
                    text += "Target " + std::to_string(port_selection_ + 1) + "/" +
                        std::to_string(port_targets_.addresses.size()) + "\n" +
                        port_targets_.addresses[port_selection_] +
                        "\nTCP " + std::to_string(port_ports_.size()) + " ports  " +
                        std::to_string(port_targets_.addresses.size() * port_ports_.size()) +
                        " probes\nLEFT/RIGHT targets  P ports\nENTER: authorized scan";
                    break;
                }
                text += "Target " + std::to_string(port_selection_ + 1) + "/" +
                    std::to_string(port_targets_.addresses.size()) + "\n" +
                    port_targets_.addresses[port_selection_] + "\n" +
                    (port_tcp_ ? (port_udp_ ? "TCP+UDP" : "TCP") : "UDP") +
                    "  " + std::to_string(port_ports_.size()) + " ports  " +
                    std::to_string(static_cast<uint64_t>(port_targets_.addresses.size()) *
                                   port_ports_.size() * (port_tcp_ && port_udp_ ? 2 : 1)) +
                    " probes\nLEFT/RIGHT targets  P ports\nENTER: authorized scan";
                break;
            case PortStage::ReviewPorts: {
                constexpr std::size_t width = 32;
                const auto pages = (port_spec_.size() + width - 1) / width;
                text += "Ports " + std::to_string(port_selection_ + 1) + "/" +
                    std::to_string(pages) + "\n" +
                    port_spec_.substr(port_selection_ * width, width) +
                    "\nLEFT/RIGHT  ESC returns";
                break;
            }
            case PortStage::Scanning:
                text += "Scanning " + std::to_string(port_task_->progress.completed.load()) + "/" +
                    std::to_string(port_task_->progress.total.load()) + "\nESC cancels";
                break;
            case PortStage::Saving:
                text += "Saving report...";
                break;
            case PortStage::Results:
                if (inventory_mode_) {
                    if (port_selection_ == 0) {
                        text += !port_result_.error.empty() ? "Failed/partial (E details)" :
                                port_result_.cancelled ? "Cancelled (partial)" : "Discovery complete";
                        const auto responding = std::count_if(
                            inventory_result_.hosts.begin(), inventory_result_.hosts.end(),
                            [](const platform::InventoryHost& host) { return host.responded; });
                        const auto imported = std::count_if(
                            inventory_result_.hosts.begin(), inventory_result_.hosts.end(),
                            [](const platform::InventoryHost& host) { return !host.responded && host.router_listed; });
                        text += "\nResponding: " + std::to_string(responding) +
                                "  router only: " + std::to_string(imported);
                        if (port_result_.truncated) text += "\nResults truncated";
                        text += "\nM map  T router";
                        if (!inventory_result_.arp_warning.empty()) text += "  A ARP";
                        if (!inventory_baseline_name_.empty()) text += "  B compare";
                        const auto today = platform::inventory_local_date();
                        const auto reminders = std::count_if(
                            inventory_result_.hosts.begin(), inventory_result_.hosts.end(),
                            [&](const platform::InventoryHost& host) {
                                return !host.update_due.empty() && !today.empty() && host.update_due <= today;
                            });
                        const bool has_reminders = std::any_of(
                            inventory_result_.hosts.begin(), inventory_result_.hosts.end(),
                            [](const platform::InventoryHost& host) { return !host.update_due.empty(); });
                        if (has_reminders) text += today.empty() ? "\nReminder clock unavailable" :
                                                    "\nReminders due: " + std::to_string(reminders);
                    } else {
                        const auto& host = inventory_result_.hosts[port_selection_ - 1];
                        text += std::string(host.responded ? "Responding " :
                                            host.router_listed ? "Router only " : "No current evidence ") +
                                std::to_string(port_selection_) + "/" +
                                std::to_string(inventory_result_.hosts.size()) + "\n" +
                                host.address + "\nCached MAC: " +
                                (host.cached_mac.empty() ? "unknown" : host.cached_mac) +
                                "\nTCP open: " +
                                (host.open_ports.empty() ? "none observed" :
                                 platform::format_port_list(host.open_ports));
                        text += "\nI inspect V info Y policy\nL label U note R due G tips";
                    }
                    text += "\nLEFT/RIGHT  S save  H history";
                } else if (port_selection_ == 0) {
                    text += !port_result_.error.empty() ? "Scan failed (E details)" :
                            port_result_.cancelled ? "Cancelled (partial)" : "Scan complete";
                    text += "\nOpen " + std::to_string(port_result_.open) +
                            "  closed " + std::to_string(port_result_.closed) +
                            "\nFiltered " + std::to_string(port_result_.filtered) +
                            "  open/filter " + std::to_string(port_result_.open_or_filtered) +
                            "\nUnreachable " + std::to_string(port_result_.unreachable) +
                            "  errors " + std::to_string(port_result_.errors);
                    if (port_result_.truncated) text += "\nResults truncated";
                } else {
                    const auto& entry = port_result_.entries[port_selection_ - 1];
                    text += "Result " + std::to_string(port_selection_) + "/" +
                        std::to_string(port_result_.entries.size()) + "\n" + entry.address + "\n" +
                        (entry.protocol == platform::PortProtocol::Tcp ? "TCP " : "UDP ") +
                        std::to_string(entry.port) + "  " + platform::port_state_name(entry.state);
                }
                if (!inventory_mode_) text += "\nLEFT/RIGHT browse  S save  ESC";
                break;
            case PortStage::Notice: {
                constexpr std::size_t width = 90;
                const auto pages = (port_notice_.size() + width - 1) / width;
                text += "Notice " + std::to_string(port_notice_page_ + 1) + "/" +
                    std::to_string(pages) + "\n" +
                    port_notice_.substr(port_notice_page_ * width, width) +
                    "\nLEFT/RIGHT  ENTER close";
                break;
            }
            case PortStage::None: return;
        }
        toast_->show_persistent(text);
    }

    void start_port_scan(bool inventory = false) {
        if (port_task_) return;
        if (inventory && USE_DESKTOP) {
            toast_->show("Host discovery requires a supported device",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        inventory_mode_ = inventory;
        port_stage_ = inventory ? PortStage::InventoryHome : PortStage::Target;
        port_target_.clear();
        port_spec_.clear();
        port_result_ = {};
        inventory_result_ = {};
        if (inventory) {
            inventory_baseline_name_.clear();
            inventory_baseline_ = {};
            start_inventory_file_action(InventoryAction::Restore);
        } else {
            show_port_prompt();
        }
    }

    void start_inventory_file_action(InventoryAction action, bool select_baseline = false) {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
        if (!home || !*home) {
            show_port_prompt("Home folder unavailable");
            return;
        }
        auto task = std::make_shared<PortTask>();
        task->inventory_action = action;
        task->inventory_select_baseline = select_baseline;
        const auto name = action == InventoryAction::List || action == InventoryAction::Restore ? std::string{} :
                          inventory_history_[inventory_history_selection_];
        port_task_ = task;
        port_stage_ = action == InventoryAction::Restore ? PortStage::InventoryRestoring :
                      action == InventoryAction::List ? PortStage::InventoryListing :
                      action == InventoryAction::Load ? PortStage::InventoryLoading :
                      action == InventoryAction::Export ? PortStage::InventoryExporting :
                                                        PortStage::InventoryDeleting;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, name,
                                        directory = std::filesystem::u8path(home) / "poseidon-reports",
                                        destination = std::filesystem::u8path(home) / "poseidon-evidence"] {
                try {
                    if (task->inventory_action == InventoryAction::Restore)
                        task->baseline = platform::load_inventory_baseline(directory);
                    else if (task->inventory_action == InventoryAction::List)
                        task->history = platform::list_network_inventories(directory);
                    else if (task->inventory_action == InventoryAction::Load) {
                        task->inventory = platform::load_network_inventory(directory, name);
                        if (task->inventory_select_baseline && task->inventory.scan.error.empty() &&
                            !task->inventory.scan.targets.empty())
                            task->inventory_file_error = platform::save_inventory_baseline(directory, name);
                    } else if (task->inventory_action == InventoryAction::Export)
                        task->evidence = platform::export_evidence(
                            directory, name, destination, platform::EvidenceSource::Inventory);
                    else {
                        task->inventory_file_error = platform::delete_network_inventory(directory, name);
                        task->inventory_deleted = task->inventory_file_error.empty();
                        if (task->inventory_deleted)
                            task->inventory_file_error = platform::clear_inventory_baseline(directory, name);
                    }
                } catch (const std::exception& error) {
                    task->inventory_file_error = std::string("Report operation failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = action == InventoryAction::Restore ? PortStage::InventoryHome : PortStage::InventoryHistory;
            show_port_prompt("Cannot start report task: " + error.code().message());
        }
    }

    void start_nmap_inspection() {
        const auto host = inventory_result_.hosts[port_selection_ - 1].address;
        const auto ports = inventory_result_.scan.ports;
        auto task = std::make_shared<PortTask>();
        port_task_ = task;
        port_stage_ = PortStage::NmapRunning;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, host, ports, os = nmap_os_] {
                try {
                    task->nmap = platform::run_nmap_inspection(host, ports, os,
                                                               task->progress.cancelled);
                } catch (const std::exception& error) {
                    task->nmap.error = std::string("Nmap inspection failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = PortStage::NmapConfirm;
            show_port_prompt("Cannot start Nmap: " + error.code().message());
        }
    }

    void start_router_import() {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
        if (!home || !*home) { show_port_prompt("Home folder unavailable"); return; }
        auto task = std::make_shared<PortTask>();
        port_task_ = task;
        port_stage_ = PortStage::RouterLoading;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, directory = std::filesystem::u8path(home) / "poseidon-reports"] {
                try {
                    task->router_clients = platform::load_router_clients(directory);
                } catch (const std::exception& error) {
                    task->router_clients.error = std::string("Router import failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = PortStage::RouterConfirm;
            show_port_prompt("Cannot start import: " + error.code().message());
        }
    }

    void show_nmap_details() {
        const auto& host = inventory_result_.hosts[port_selection_ - 1];
        inventory_detail_title_ = "Nmap evidence";
        nmap_detail_ = "Service/OS results are estimates, not patch status. ";
        if (!host.responded) nmap_detail_ += "Not observed responding in discovery scan. ";
        if (!host.label.empty()) nmap_detail_ += "Label: " + file_display_text(host.label, 80) + ". ";
        if (!host.update_note.empty())
            nmap_detail_ += "Update note: " + file_display_text(host.update_note, 80) + ". ";
        if (!host.update_due.empty()) {
            const auto today = platform::inventory_local_date();
            nmap_detail_ += "Reminder: " + host.update_due +
                (today.empty() ? " (clock unavailable). " :
                 host.update_due <= today ? " (due). " : " (upcoming). ");
        }
        if (!host.router_name.empty() || !host.router_mac.empty())
            nmap_detail_ += "Imported router record (unverified): " +
                            file_display_text(host.router_name, 80) + " " + host.router_mac + ". ";
        if (platform::inventory_mac_conflict(host))
            nmap_detail_ += "Warning: router and cached MAC differ. ";
        if (host.services.empty()) nmap_detail_ += "No service details recorded. ";
        for (const auto& service : host.services) {
            nmap_detail_ += "TCP " + std::to_string(service.port) + " " +
                            file_display_text(service.service, 80) + " " +
                            file_display_text(service.version, 80) + ". ";
        }
        nmap_detail_ += host.os_guess.empty() ? "OS unknown." :
                        "OS guess: " + file_display_text(host.os_guess, 80);
        nmap_detail_page_ = 0;
        port_stage_ = PortStage::NmapDetails;
        show_port_prompt();
    }

    void show_inventory_map() {
        inventory_detail_title_ = "Observation map";
        nmap_detail_ = "[This scanner]\n? Network path unknown\n";
        if (inventory_result_.hosts.empty()) nmap_detail_ += "No responses observed.";
        for (const auto& host : inventory_result_.hosts)
            nmap_detail_ += "? " + host.address +
                            (host.responded ? "\n" : " (not observed)\n");
        nmap_detail_ += "Physical links are unknown.";
        nmap_detail_page_ = 0;
        port_stage_ = PortStage::NmapDetails;
        show_port_prompt();
    }

    void show_inventory_policy(const std::vector<uint16_t>& restricted_ports) {
        const auto& host = inventory_result_.hosts[port_selection_ - 1];
        const auto review = platform::review_inventory_policy(inventory_result_, host, restricted_ports);
        inventory_detail_title_ = "TCP policy review";
        nmap_detail_ = host.address + ". Restricted: " + platform::format_port_list(restricted_ports) +
                       ". Observed open: " +
                       (review.observed_open.empty() ? "none" : platform::format_port_list(review.observed_open)) +
                       ". Not in scan plan: " +
                       (review.not_in_scan.empty() ? "none" : platform::format_port_list(review.not_in_scan)) +
                       ". Other ports may be filtered or unprobed. No proof of safety or internet exposure. "
                       "No new scan performed.";
        if (!host.responded) nmap_detail_ += " Host was not observed responding.";
        nmap_detail_page_ = 0;
        port_stage_ = PortStage::NmapDetails;
        show_port_prompt();
    }

    void show_inventory_guidance() {
        const auto& host = inventory_result_.hosts[port_selection_ - 1];
        const auto hints = platform::inventory_port_guidance(host);
        inventory_detail_title_ = "Offline port tips";
        nmap_detail_ = "Ports do not identify services or vulnerabilities. ";
        if (hints.empty()) nmap_detail_ += "No tips for observed open ports. ";
        for (const auto& hint : hints) nmap_detail_ += hint + " ";
        nmap_detail_ += "Filtered or unscanned ports remain unknown; no new scan was performed.";
        nmap_detail_page_ = 0;
        port_stage_ = PortStage::NmapDetails;
        show_port_prompt();
    }

    void start_port_resolution() {
        auto task = std::make_shared<PortTask>();
        port_task_ = task;
        port_stage_ = PortStage::Resolving;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, target = port_target_] {
                try {
                    task->targets = platform::resolve_port_targets(target, task->progress.cancelled);
                } catch (const std::exception& error) {
                    task->targets.error = std::string("Resolution failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = PortStage::Target;
            show_port_prompt("Cannot start resolver: " + error.code().message());
        }
    }

    void run_port_scan() {
        auto task = std::make_shared<PortTask>();
        port_task_ = task;
        port_stage_ = PortStage::Scanning;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, targets = port_targets_, ports = port_ports_,
                                        tcp = port_tcp_, udp = port_udp_, inventory = inventory_mode_] {
                try {
                    task->result = platform::scan_ports(targets, ports, tcp, udp, task->progress);
                    if (inventory)
                        task->inventory = platform::make_network_inventory(
                            task->result, platform::arp_entries());
                } catch (const std::exception& error) {
                    task->result.error = std::string("Scan failed: ") + error.what();
                    if (inventory) {
                        task->result.targets = targets.addresses;
                        task->result.ports = ports;
                        task->result.tcp = tcp;
                        task->inventory.scan = task->result;
                    }
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = PortStage::Review;
            show_port_prompt("Cannot start scan: " + error.code().message());
        }
    }

    void save_port_scan() {
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
        if (!home || !*home) {
            show_port_prompt("Home folder unavailable");
            return;
        }
        if (inventory_mode_ && !inventory_baseline_name_.empty()) update_inventory_comparison();
        auto task = std::make_shared<PortTask>();
        port_task_ = task;
        port_stage_ = PortStage::Saving;
        show_port_prompt();
        try {
            port_thread_ = std::thread([task, result = port_result_, inventory = inventory_result_,
                                        inventory_mode = inventory_mode_,
                                        directory = std::filesystem::u8path(home) / "poseidon-reports"] {
                try {
                    task->report = inventory_mode
                        ? platform::save_network_inventory(directory, inventory)
                        : platform::save_port_report(directory, result);
                } catch (const std::exception& error) {
                    task->report.error = std::string("Save failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            port_task_.reset();
            port_stage_ = PortStage::Results;
            show_port_prompt("Cannot start save: " + error.code().message());
        }
    }

    void update_inventory_comparison() {
        const auto comparison = platform::compare_network_inventories(inventory_baseline_, inventory_result_);
        const bool partial = inventory_baseline_.scan.cancelled || inventory_result_.scan.cancelled ||
            !inventory_baseline_.scan.error.empty() || !inventory_result_.scan.error.empty() ||
            inventory_baseline_.scan.truncated || inventory_result_.scan.truncated ||
            inventory_baseline_.scan.completed != inventory_baseline_.scan.total ||
            inventory_result_.scan.completed != inventory_result_.scan.total;
        inventory_result_.comparison_baseline = inventory_baseline_name_;
        inventory_result_.comparison_note = comparison.comparable
            ? (partial ? "Partial: new " : "New ") + std::to_string(comparison.newly_responding) +
              ", not seen " + std::to_string(comparison.not_seen) +
              ", MAC " + std::to_string(comparison.mac_changed) +
              ", TCP " + std::to_string(comparison.newly_open_ports) +
              ", service " + std::to_string(comparison.service_changes)
            : "Baseline targets or ports differ; no comparison";
    }

    void update_port_scan() {
        const auto task = port_task_;
        if (!task) return;
        if (!task->complete) {
            const auto now = std::chrono::steady_clock::now();
            if (port_stage_ == PortStage::Scanning && now >= port_next_update_) {
                port_next_update_ = now + std::chrono::milliseconds(250);
                show_port_prompt();
            }
            return;
        }
        if (port_thread_.joinable()) port_thread_.join();
        port_task_.reset();
        if (port_stage_ == PortStage::RouterLoading) {
            port_stage_ = PortStage::Results;
            if (!task->router_clients.error.empty()) show_port_prompt(task->router_clients.error);
            else {
                const auto imported = platform::add_router_evidence(inventory_result_, task->router_clients);
                if (!imported.error.empty()) show_port_prompt(imported.error);
                else {
                    port_selection_ = 0;
                    show_port_prompt("Router records: " + std::to_string(imported.matched) +
                        " matched, " + std::to_string(imported.imported_only) +
                        " not observed, " + std::to_string(imported.retained_without_record) +
                        " retained without current record. Records may be stale. Press S to save.");
                }
            }
        } else if (port_stage_ == PortStage::NmapRunning) {
            port_stage_ = PortStage::Results;
            if (task->nmap.cancelled) show_port_prompt("Nmap inspection cancelled");
            else if (!task->nmap.error.empty()) show_port_prompt(task->nmap.error);
            else {
                auto& host = inventory_result_.hosts[port_selection_ - 1];
                host.services = std::move(task->nmap.services);
                host.os_guess = std::move(task->nmap.os_guess);
                show_nmap_details();
                if (!task->nmap.warning.empty()) show_port_prompt(task->nmap.warning);
            }
        } else if (port_stage_ == PortStage::InventoryRestoring) {
            port_stage_ = PortStage::InventoryHome;
            if (!task->inventory_file_error.empty()) show_port_prompt(task->inventory_file_error);
            else if (!task->baseline.error.empty()) show_port_prompt(task->baseline.error);
            else {
                inventory_baseline_name_ = std::move(task->baseline.name);
                inventory_baseline_ = std::move(task->baseline.inventory);
                show_port_prompt();
            }
        } else if (port_stage_ == PortStage::InventoryListing) {
            port_stage_ = PortStage::InventoryHistory;
            if (!task->inventory_file_error.empty()) show_port_prompt(task->inventory_file_error);
            else if (!task->history.error.empty()) show_port_prompt(task->history.error);
            else {
                inventory_history_ = std::move(task->history.names);
                inventory_history_selection_ = 0;
                if (task->history.truncated)
                    show_port_prompt("More than 256 reports; showing a subset. Delete reports to see the rest.");
                else show_port_prompt();
            }
        } else if (port_stage_ == PortStage::InventoryLoading) {
            port_stage_ = PortStage::InventoryHistory;
            if (!task->inventory_file_error.empty()) show_port_prompt(task->inventory_file_error);
            else if (task->inventory.scan.targets.empty())
                show_port_prompt(task->inventory.scan.error.empty() ?
                                 "Invalid discovery report" : task->inventory.scan.error);
            else if (task->inventory_select_baseline) {
                inventory_baseline_ = std::move(task->inventory);
                inventory_baseline_name_ = inventory_history_[inventory_history_selection_];
                port_stage_ = PortStage::InventoryHome;
                show_port_prompt("Baseline saved");
            } else {
                inventory_result_ = std::move(task->inventory);
                port_result_ = inventory_result_.scan;
                port_selection_ = 0;
                port_stage_ = PortStage::Results;
                show_port_prompt("Saved report, not a live scan");
            }
        } else if (port_stage_ == PortStage::InventoryDeleting) {
            port_stage_ = PortStage::InventoryHistory;
            if (task->inventory_deleted &&
                inventory_baseline_name_ == inventory_history_[inventory_history_selection_]) {
                inventory_baseline_name_.clear();
                inventory_baseline_ = {};
            }
            if (!task->inventory_file_error.empty()) {
                if (task->inventory_deleted) {
                    inventory_history_.erase(inventory_history_.begin() + inventory_history_selection_);
                    if (inventory_history_selection_ >= inventory_history_.size() && inventory_history_selection_ > 0)
                        --inventory_history_selection_;
                    show_port_prompt("Report deleted; baseline cleanup: " + task->inventory_file_error);
                } else {
                    show_port_prompt(task->inventory_file_error);
                }
            }
            else {
                start_inventory_file_action(InventoryAction::List);
            }
        } else if (port_stage_ == PortStage::InventoryExporting) {
            port_stage_ = PortStage::InventoryHistory;
            if (!task->inventory_file_error.empty()) show_port_prompt(task->inventory_file_error);
            else if (!task->evidence.error.empty()) show_port_prompt(task->evidence.error);
            else show_port_prompt("Evidence copy and SHA-256 manifest saved in ~/poseidon-evidence. " +
                                  task->evidence.manifest_path.filename().u8string());
        } else if (port_stage_ == PortStage::Resolving) {
            if (task->progress.cancelled) {
                port_stage_ = PortStage::Target;
                show_port_prompt("Resolution cancelled");
            } else if (!task->targets.error.empty() || task->targets.addresses.empty()) {
                port_stage_ = PortStage::Target;
                show_port_prompt(task->targets.error.empty() ? "No target addresses" : task->targets.error);
            } else {
                port_targets_ = std::move(task->targets);
                port_selection_ = 0;
                port_stage_ = PortStage::Review;
                show_port_prompt();
            }
        } else if (port_stage_ == PortStage::Scanning) {
            port_result_ = std::move(task->result);
            if (inventory_mode_) inventory_result_ = std::move(task->inventory);
            port_selection_ = 0;
            port_stage_ = PortStage::Results;
            if (inventory_mode_ && !inventory_baseline_name_.empty()) {
                update_inventory_comparison();
                const auto changes = platform::compare_network_inventories(
                    inventory_baseline_, inventory_result_);
                const bool changed = changes.newly_responding || changes.not_seen ||
                    changes.mac_changed || changes.newly_open_ports || changes.service_changes;
                show_port_prompt(std::string(changes.comparable && changed ? "Change alert: " : "") +
                                 inventory_result_.comparison_note +
                                 ". Not seen is unknown.");
            } else {
                show_port_prompt();
            }
        } else if (port_stage_ == PortStage::Saving) {
            port_stage_ = PortStage::Results;
            show_port_prompt(task->report.error.empty()
                ? "Saved: " + task->report.path.filename().u8string() : task->report.error);
        }
    }

    bool handle_port_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (port_stage_ == PortStage::Notice) {
            const auto pages = (port_notice_.size() + 89) / 90;
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                port_stage_ = port_return_stage_;
                show_port_prompt();
            } else if ((key == LV_KEY_LEFT || key == 'z') && port_notice_page_ > 0) {
                --port_notice_page_;
                show_port_prompt();
            } else if ((key == LV_KEY_RIGHT || key == 'c') && port_notice_page_ + 1 < pages) {
                ++port_notice_page_;
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::InventoryRestoring || port_stage_ == PortStage::InventoryListing || port_stage_ == PortStage::InventoryLoading ||
            port_stage_ == PortStage::InventoryDeleting || port_stage_ == PortStage::InventoryExporting ||
            port_stage_ == PortStage::RouterLoading) return true;
        if (port_stage_ == PortStage::RouterConfirm) {
            if (key == LV_KEY_ENTER) start_router_import();
            else if (key == LV_KEY_ESC) { port_stage_ = PortStage::Results; show_port_prompt(); }
            return true;
        }
        if (port_stage_ == PortStage::InventoryHome) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::None; toast_->hide(); }
            else if (key == '1' || key == LV_KEY_ENTER) {
                port_stage_ = PortStage::Target;
                port_target_.clear();
                show_port_prompt();
            } else if (key == '2' || key == 'h' || key == 'H')
                start_inventory_file_action(InventoryAction::List);
            return true;
        }
        if (port_stage_ == PortStage::InventoryPorts) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::Target; show_port_prompt(); }
            else if (key == '1' || key == LV_KEY_ENTER) {
                port_ports_ = {22, 80, 443, 445};
                port_spec_ = platform::format_port_list(port_ports_);
                inventory_exclusions_.clear();
                port_stage_ = PortStage::InventoryExclusions;
                show_port_prompt();
            } else if (key == '2') {
                port_spec_.clear();
                port_stage_ = PortStage::Custom;
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::InventoryExclusions) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::InventoryPorts; show_port_prompt(); }
            else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!inventory_exclusions_.empty()) inventory_exclusions_.pop_back();
                show_port_prompt();
            } else if (key == LV_KEY_ENTER) {
                const auto selected = platform::exclude_inventory_targets(
                    platform::numeric_port_targets(port_target_), inventory_exclusions_);
                if (!selected.error.empty()) show_port_prompt(selected.error);
                else {
                    port_targets_ = selected;
                    port_selection_ = 0;
                    port_stage_ = PortStage::Review;
                    show_port_prompt();
                }
            } else if (key >= 32 && key <= 126 && inventory_exclusions_.size() < 253) {
                inventory_exclusions_.push_back(static_cast<char>(key));
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::InventoryLabel || port_stage_ == PortStage::InventoryUpdateNote ||
            port_stage_ == PortStage::InventoryReminder) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::Results; show_port_prompt(); }
            else if (key == LV_KEY_ENTER) {
                auto& host = inventory_result_.hosts[port_selection_ - 1];
                if (port_stage_ == PortStage::InventoryReminder && !inventory_edit_buffer_.empty() &&
                    !platform::valid_inventory_date(inventory_edit_buffer_)) {
                    show_port_prompt("Use a valid YYYY-MM-DD date, or clear it");
                    return true;
                }
                if (port_stage_ == PortStage::InventoryPolicy) {
                    if (key == LV_KEY_ESC) {
                        port_stage_ = PortStage::Results;
                        show_port_prompt();
                    } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                        if (!inventory_policy_text_.empty()) inventory_policy_text_.pop_back();
                        show_port_prompt();
                    } else if (key == LV_KEY_ENTER) {
                        const auto parsed = platform::parse_port_list(inventory_policy_text_);
                        if (!parsed.error.empty()) show_port_prompt(parsed.error);
                        else if (parsed.ports.size() > 32) show_port_prompt("Policy supports at most 32 ports");
                        else show_inventory_policy(parsed.ports);
                    } else if (((key >= '0' && key <= '9') || key == ',' || key == '-') &&
                               inventory_policy_text_.size() < 80) {
                        inventory_policy_text_.push_back(static_cast<char>(key));
                        show_port_prompt();
                    }
                    return true;
                }
                if (port_stage_ == PortStage::InventoryLabel) host.label = inventory_edit_buffer_;
                else if (port_stage_ == PortStage::InventoryUpdateNote) host.update_note = inventory_edit_buffer_;
                else host.update_due = inventory_edit_buffer_;
                port_stage_ = PortStage::Results;
                show_port_prompt("Applied. Press S to save a report.");
            } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!inventory_edit_buffer_.empty()) inventory_edit_buffer_.pop_back();
                show_port_prompt();
            } else if (key >= 32 && key <= 126 && inventory_edit_buffer_.size() < 64) {
                inventory_edit_buffer_.push_back(static_cast<char>(key));
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::InventoryHistory) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::InventoryHome; show_port_prompt(); }
            else if ((key == LV_KEY_LEFT || key == 'z') && inventory_history_selection_ > 0) {
                --inventory_history_selection_;
                show_port_prompt();
            } else if ((key == LV_KEY_RIGHT || key == 'c') &&
                       inventory_history_selection_ + 1 < inventory_history_.size()) {
                ++inventory_history_selection_;
                show_port_prompt();
            } else if (!inventory_history_.empty() && key == LV_KEY_ENTER)
                start_inventory_file_action(InventoryAction::Load);
            else if (!inventory_history_.empty() && (key == 'b' || key == 'B'))
                start_inventory_file_action(InventoryAction::Load, true);
            else if (!inventory_history_.empty() && (key == 'd' || key == 'D')) {
                port_stage_ = PortStage::InventoryDeleteConfirm;
                show_port_prompt();
            } else if (!inventory_history_.empty() && (key == 'x' || key == 'X')) {
                port_stage_ = PortStage::InventoryExportConfirm;
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::InventoryDeleteConfirm) {
            if (key == LV_KEY_ENTER) start_inventory_file_action(InventoryAction::Delete);
            else if (key == LV_KEY_ESC) { port_stage_ = PortStage::InventoryHistory; show_port_prompt(); }
            return true;
        }
        if (port_stage_ == PortStage::InventoryExportConfirm) {
            if (key == LV_KEY_ENTER) start_inventory_file_action(InventoryAction::Export);
            else if (key == LV_KEY_ESC) { port_stage_ = PortStage::InventoryHistory; show_port_prompt(); }
            return true;
        }
        if (port_stage_ == PortStage::InventoryComparison) {
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                port_stage_ = PortStage::Results;
                show_port_prompt();
            } else if (key == 'd' || key == 'D') {
                const auto comparison = platform::compare_network_inventories(
                    inventory_baseline_, inventory_result_);
                if (!comparison.comparable) return true;
                inventory_detail_title_ = "Observed changes";
                nmap_detail_.clear();
                for (const auto& line : comparison.details)
                    nmap_detail_ += file_display_text(line, 120) + "\n";
                if (comparison.details.empty()) nmap_detail_ = "No differences observed.";
                if (comparison.details_truncated) nmap_detail_ += "Further details omitted.";
                nmap_detail_page_ = 0;
                port_stage_ = PortStage::NmapDetails;
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::NmapRunning) {
            if (key == LV_KEY_ESC) port_task_->progress.cancelled = true;
            return true;
        }
        if (port_stage_ == PortStage::NmapChoice) {
            if (key == LV_KEY_ESC) { port_stage_ = PortStage::Results; show_port_prompt(); }
            else if (key == '1' || key == '2') {
                nmap_os_ = key == '2';
                port_stage_ = PortStage::NmapConfirm;
                show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::NmapConfirm) {
            if (key == LV_KEY_ENTER) start_nmap_inspection();
            else if (key == LV_KEY_ESC) { port_stage_ = PortStage::NmapChoice; show_port_prompt(); }
            return true;
        }
        if (port_stage_ == PortStage::NmapDetails) {
            const auto pages = std::max<std::size_t>(1, (nmap_detail_.size() + 89) / 90);
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                port_stage_ = PortStage::Results;
                show_port_prompt();
            } else if ((key == LV_KEY_LEFT || key == 'z') && nmap_detail_page_ > 0) {
                --nmap_detail_page_; show_port_prompt();
            } else if ((key == LV_KEY_RIGHT || key == 'c') && nmap_detail_page_ + 1 < pages) {
                ++nmap_detail_page_; show_port_prompt();
            }
            return true;
        }
        if (port_stage_ == PortStage::Resolving || port_stage_ == PortStage::Scanning) {
            if (key == LV_KEY_ESC) port_task_->progress.cancelled = true;
            return true;
        }
        if (port_stage_ == PortStage::Saving) return true;
        if (port_stage_ == PortStage::ReviewPorts && key == LV_KEY_ESC) {
            port_stage_ = PortStage::Review;
            port_selection_ = 0;
            show_port_prompt();
            return true;
        }
        if (key == LV_KEY_ESC) {
            if (inventory_mode_) { port_stage_ = PortStage::InventoryHome; show_port_prompt(); }
            else { port_stage_ = PortStage::None; toast_->hide(); }
            return true;
        }
        if (port_stage_ == PortStage::Target || port_stage_ == PortStage::Custom) {
            auto& input = port_stage_ == PortStage::Target ? port_target_ : port_spec_;
            const auto limit = port_stage_ == PortStage::Target ? 253U : 4096U;
            if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!input.empty()) input.pop_back();
                show_port_prompt();
            } else if (key == LV_KEY_ENTER) {
                if (port_stage_ == PortStage::Target) {
                    if (input.empty()) show_port_prompt("Enter a target");
                    else if (inventory_mode_) {
                        port_targets_ = platform::numeric_port_targets(input);
                        if (!port_targets_.error.empty()) {
                            show_port_prompt(port_targets_.error);
                        } else {
                            port_tcp_ = true;
                            port_udp_ = false;
                            port_stage_ = PortStage::InventoryPorts;
                            show_port_prompt();
                        }
                    }
                    else { port_stage_ = PortStage::Protocol; show_port_prompt(); }
                } else {
                    auto parsed = platform::parse_port_list(input);
                    if (!parsed.error.empty()) show_port_prompt(parsed.error);
                    else if (inventory_mode_ && parsed.ports.size() > 8)
                        show_port_prompt("Discovery supports at most 8 ports");
                    else {
                        port_ports_ = std::move(parsed.ports);
                        port_spec_ = platform::format_port_list(port_ports_);
                        if (inventory_mode_) {
                            inventory_exclusions_.clear();
                            port_stage_ = PortStage::InventoryExclusions;
                        } else {
                            port_stage_ = PortStage::DnsConsent;
                        }
                        show_port_prompt();
                    }
                }
            } else if (key >= 32 && key <= 126 && input.size() < limit) {
                input.push_back(static_cast<char>(key));
                show_port_prompt();
            }
        } else if (port_stage_ == PortStage::Protocol && key >= '1' && key <= '3') {
            port_tcp_ = key != '2';
            port_udp_ = key != '1';
            port_stage_ = PortStage::Ports;
            show_port_prompt();
        } else if (port_stage_ == PortStage::Ports &&
                   (key == LV_KEY_ENTER || (key >= '1' && key <= '3'))) {
            if (key == '3') {
                port_spec_.clear();
                port_stage_ = PortStage::Custom;
            } else {
                port_ports_ = key == '2' ? platform::parse_port_list("1-65535").ports
                                         : platform::common_ports().ports;
                port_spec_ = platform::format_port_list(port_ports_);
                port_stage_ = PortStage::DnsConsent;
            }
            show_port_prompt();
        } else if (port_stage_ == PortStage::DnsConsent && key == LV_KEY_ENTER) {
            start_port_resolution();
        } else if (port_stage_ == PortStage::Review || port_stage_ == PortStage::ReviewPorts ||
                   port_stage_ == PortStage::Results) {
            const std::size_t count = port_stage_ == PortStage::Review
                ? port_targets_.addresses.size() : port_stage_ == PortStage::ReviewPorts
                ? (port_spec_.size() + 31) / 32 : inventory_mode_
                ? inventory_result_.hosts.size() + 1 : port_result_.entries.size() + 1;
            if ((key == LV_KEY_LEFT || key == 'z') && port_selection_ > 0) {
                --port_selection_;
                show_port_prompt();
            } else if ((key == LV_KEY_RIGHT || key == 'c') && port_selection_ + 1 < count) {
                ++port_selection_;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Review && key == LV_KEY_ENTER) {
                run_port_scan();
            } else if (port_stage_ == PortStage::Review && (key == 'p' || key == 'P')) {
                port_stage_ = PortStage::ReviewPorts;
                port_selection_ = 0;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && (key == 's' || key == 'S')) {
                save_port_scan();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       (key == 'h' || key == 'H')) {
                start_inventory_file_action(InventoryAction::List);
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       port_selection_ > 0 && (key == 'i' || key == 'I')) {
                port_stage_ = PortStage::NmapChoice;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       port_selection_ > 0 && (key == 'v' || key == 'V')) {
                show_nmap_details();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       port_selection_ > 0 && (key == 'y' || key == 'Y')) {
                port_stage_ = PortStage::InventoryPolicy;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       port_selection_ > 0 && (key == 'g' || key == 'G')) {
                show_inventory_guidance();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       port_selection_ > 0 && (key == 'l' || key == 'L' || key == 'u' || key == 'U' ||
                                               key == 'r' || key == 'R')) {
                const auto& host = inventory_result_.hosts[port_selection_ - 1];
                const bool label = key == 'l' || key == 'L';
                const bool reminder = key == 'r' || key == 'R';
                inventory_edit_buffer_ = label ? host.label : reminder ? host.update_due : host.update_note;
                port_stage_ = label ? PortStage::InventoryLabel : reminder
                    ? PortStage::InventoryReminder : PortStage::InventoryUpdateNote;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       (key == 'm' || key == 'M')) {
                show_inventory_map();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       (key == 't' || key == 'T')) {
                port_stage_ = PortStage::RouterConfirm;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       !inventory_baseline_name_.empty() && (key == 'b' || key == 'B')) {
                port_stage_ = PortStage::InventoryComparison;
                show_port_prompt();
            } else if (port_stage_ == PortStage::Results && (key == 'e' || key == 'E') &&
                       !port_result_.error.empty()) {
                show_port_prompt(port_result_.error);
            } else if (port_stage_ == PortStage::Results && inventory_mode_ &&
                       (key == 'a' || key == 'A') && !inventory_result_.arp_warning.empty()) {
                show_port_prompt(inventory_result_.arp_warning);
            }
        }
        return true;
    }

    void start_ap_signal_test() {
        ap_signal_test_active_ = true;
        ap_signal_samples_.clear();
        show_ap_signal_test();
    }

    void show_lab_listener() {
        std::string text;
        if (lab_listener_stage_ == LabListenerStage::Address) {
            text = "Lab listener IPv4:\n" + lab_listener_address_ +
                   "\nPrivate local IP; TCP 8088\nENTER review  ESC cancel";
        } else if (lab_listener_stage_ == LabListenerStage::Confirm) {
            text = "Bind " + lab_listener_address_ + ":8088\n"
                   "Ensure lab is isolated.\nNot isolated automatically!\n"
                   "ENTER start  ESC cancel";
        } else if (lab_listener_) {
            const auto status = lab_listener_->status();
            if (lab_export_task_) {
                text = lab_export_task_->deleting ? "Deleting log..." : "Copying log and hashing...";
            } else if (lab_delete_confirm_) {
                text = "Delete original log?\nExports are not deleted.\nENTER delete  ESC cancel";
            } else if (lab_export_confirm_) {
                text = "Copy lab log?\nContains peer IP addresses.\n"
                       "ENTER copy + SHA-256  ESC";
            } else if (!lab_listener_notice_.empty()) {
                text = lab_listener_notice_ + "\nENTER/ESC back";
            } else if (lab_listener_log_info_) {
                text = "Lab listener log\n" + status.file_name +
                       "\n~/poseidon-lab\nL back  ESC " +
                       (status.complete ? "close" : "stop");
            } else if (!status.error.empty()) {
                text = "Lab listener error\n" + status.error +
                       (status.file_name.empty() || lab_log_deleted_ ? "\nESC close" :
                        "\nL log  X export  D delete\nESC close");
            } else if (status.complete) {
                text = "Listener stopped\nConnections: " + std::to_string(status.connections) +
                       (lab_log_deleted_ ? "\nLog deleted  ESC close" :
                        status.file_name.empty() ? "\nNo log created\nESC close" :
                        "\nL log  X export  D delete\nESC close");
            } else if (lab_listener_stop_requested_) {
                text = "Stopping listener...";
            } else if (status.listening) {
                text = "Listening " + lab_listener_address_ + ":8088\nConnections: " +
                       std::to_string(status.connections) +
                       (status.last_peer.empty() ? "" : "\nLast: " + status.last_peer) +
                       "\n5 min/100 max  ESC stop";
            } else {
                text = "Starting lab listener...\nESC stop";
            }
        }
        if (text != lab_listener_display_) {
            lab_listener_display_ = text;
            toast_->show_persistent(text);
        }
    }

    void update_lab_listener() {
        if (lab_listener_stage_ != LabListenerStage::Running) return;
        if (lab_export_task_ && lab_export_task_->complete) {
            if (lab_export_thread_.joinable()) lab_export_thread_.join();
            const bool deleting = lab_export_task_->deleting;
            const auto result = std::move(lab_export_task_->result);
            lab_export_task_.reset();
            if (deleting && result.error.empty()) lab_log_deleted_ = true;
            lab_listener_notice_ = result.error.empty()
                ? (deleting ? "Log deleted\nExports remain" :
                              "Copy + SHA-256 saved\n~/poseidon-evidence")
                : (deleting ? "Delete failed\n" : "Export failed\n") + result.error;
        }
        show_lab_listener();
    }

    void start_security_log_action(const char* directory, const std::string& filename,
                                   platform::EvidenceSource source, bool deleting,
                                   std::string& notice) {
        const char* home = std::getenv("HOME");
        if (!home || !*home || !std::filesystem::u8path(home).is_absolute()) {
            notice = "Home folder unavailable";
            return;
        }
        auto task = std::make_shared<LabExportTask>();
        task->deleting = deleting;
        const auto root = std::filesystem::u8path(home);
        const auto source_directory = root / directory;
        lab_export_task_ = task;
        try {
            lab_export_thread_ = std::thread([task, root, source_directory, filename, source] {
                try {
                    if (task->deleting) {
                        task->result.error = platform::delete_security_log(
                            source_directory, filename, source);
                    } else {
                        task->result = platform::export_evidence(
                            source_directory, filename, root / "poseidon-evidence", source);
                    }
                } catch (const std::exception& failure) {
                    task->result.error = failure.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& failure) {
            lab_export_task_.reset();
            notice = "Cannot start log operation: " + failure.code().message();
        }
    }

    bool handle_lab_listener_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (lab_listener_stage_ == LabListenerStage::Running) {
            const auto status = lab_listener_->status();
            if (lab_export_task_) return true;
            if (lab_delete_confirm_) {
                if (key == LV_KEY_ENTER) {
                    lab_delete_confirm_ = false;
                    start_security_log_action("poseidon-lab", status.file_name,
                                              platform::EvidenceSource::LabListener, true,
                                              lab_listener_notice_);
                } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                    lab_delete_confirm_ = false;
                }
                show_lab_listener();
            } else if (lab_export_confirm_) {
                if (key == LV_KEY_ENTER) {
                    lab_export_confirm_ = false;
                    start_security_log_action("poseidon-lab", status.file_name,
                                              platform::EvidenceSource::LabListener, false,
                                              lab_listener_notice_);
                } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                    lab_export_confirm_ = false;
                }
                show_lab_listener();
            } else if (!lab_listener_notice_.empty()) {
                if (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                    lab_listener_notice_.clear();
                    show_lab_listener();
                }
            } else if ((key == 'd' || key == 'D') && status.complete &&
                       !status.file_name.empty() && !lab_log_deleted_) {
                lab_listener_log_info_ = false;
                lab_delete_confirm_ = true;
                show_lab_listener();
            } else if ((key == 'x' || key == 'X') && status.complete &&
                       !status.file_name.empty() && !lab_log_deleted_) {
                lab_listener_log_info_ = false;
                lab_export_confirm_ = true;
                show_lab_listener();
            } else if ((key == 'l' || key == 'L') && !status.file_name.empty() &&
                       !lab_log_deleted_) {
                lab_listener_log_info_ = !lab_listener_log_info_;
                show_lab_listener();
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                if (!status.complete) {
                    lab_listener_->stop();
                    lab_listener_stop_requested_ = true;
                    show_lab_listener();
                } else {
                    lab_listener_.reset();
                    lab_listener_stage_ = LabListenerStage::None;
                    lab_listener_display_.clear();
                    toast_->hide();
                }
            }
            return true;
        }
        if (key == LV_KEY_ESC) {
            lab_listener_stage_ = LabListenerStage::None;
            lab_listener_display_.clear();
            toast_->hide();
        } else if (lab_listener_stage_ == LabListenerStage::Confirm) {
            if (key == LV_KEY_ENTER) {
                const char* home = std::getenv("HOME");
                if (!home || !*home || !std::filesystem::u8path(home).is_absolute()) {
                    lab_listener_stage_ = LabListenerStage::None;
                    toast_->show("Home folder unavailable", view::widgets::ToastTone::Error, 4000);
                    return true;
                }
                std::string error;
                lab_listener_ = platform::LabHoneypot::start(
                    lab_listener_address_, std::filesystem::u8path(home) / "poseidon-lab", error);
                if (!lab_listener_) {
                    lab_listener_stage_ = LabListenerStage::None;
                    toast_->show(error, view::widgets::ToastTone::Error, 4000);
                    return true;
                }
                lab_listener_stage_ = LabListenerStage::Running;
                lab_listener_stop_requested_ = false;
                lab_listener_log_info_ = false;
                lab_export_confirm_ = false;
                lab_delete_confirm_ = false;
                lab_log_deleted_ = false;
                lab_listener_notice_.clear();
                show_lab_listener();
            }
        } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (!lab_listener_address_.empty()) lab_listener_address_.pop_back();
            show_lab_listener();
        } else if (key == LV_KEY_ENTER) {
            if (!platform::valid_lab_honeypot_address(lab_listener_address_)) {
                toast_->show("Enter a private or\nloopback IPv4 address",
                             view::widgets::ToastTone::Warning, 3000);
                lab_listener_display_.clear();
            } else {
                lab_listener_stage_ = LabListenerStage::Confirm;
                show_lab_listener();
            }
        } else if (((key >= '0' && key <= '9') || key == '.') &&
                   lab_listener_address_.size() < 15) {
            lab_listener_address_ += static_cast<char>(key);
            show_lab_listener();
        }
        return true;
    }

    void show_wireless_event_monitor() {
        std::string text;
        const bool probe = wireless_event_kind_ == platform::WirelessEventKind::ProbeRequest;
        if (wireless_event_stage_ == WirelessEventStage::Interface) {
            text = "Interface or esp32:\n" + wireless_event_interface_ +
                   (wireless_event_setup_error_.empty()
                        ? "\nRadiotap, or configured ESP32"
                        : "\n" + wireless_event_setup_error_) +
                   "\nENTER review  ESC cancel";
        } else if (wireless_event_stage_ == WirelessEventStage::Channel) {
            text = "ESP32 2.4 GHz channel 1-11\n" +
                   (wireless_event_channel_text_.empty() ? std::string("Default: 1") : wireless_event_channel_text_) +
                   (wireless_event_setup_error_.empty() ? "" : "\n" + wireless_event_setup_error_) +
                   "\nH hop 1/6/11  ENTER next\nESC back";
        } else if (wireless_event_stage_ == WirelessEventStage::Confirm) {
            text = std::string(probe ? "Count probe headers\n" : "Receive deauth headers\n") +
                   wireless_event_interface_ +
                   (wireless_event_interface_ == "esp32"
                        ? (wireless_event_channel_ == 0 ? "\nESP32: hop 1/6/11, no TX."
                            : "\nESP32: 2.4 GHz ch " + std::to_string(wireless_event_channel_) + ", no TX.")
                        : "\nNo channel changes or TX.") +
                   "\nENTER start  ESC cancel";
        } else if (wireless_event_monitor_) {
            const auto status = wireless_event_monitor_->status();
            if (lab_export_task_) {
                text = lab_export_task_->deleting ? "Deleting log..." : "Copying log and hashing...";
            } else if (wireless_event_delete_confirm_) {
                text = "Delete original log?\nExports are not deleted.\nENTER delete  ESC cancel";
            } else if (wireless_event_export_confirm_) {
                text = probe ? "Copy probe log?\nContains event times.\nENTER copy + SHA-256  ESC"
                             : "Copy deauth log?\nContains AP identifiers.\nENTER copy + SHA-256  ESC";
            } else if (!wireless_event_notice_.empty()) {
                text = wireless_event_notice_ + "\nENTER/ESC back";
            } else if (wireless_event_log_info_) {
                text = std::string(probe ? "Probe log\n" : "Deauth log\n") + status.file_name +
                       "\n~/poseidon-wireless-monitor\nL back  ESC " +
                       (status.complete ? "close" : "stop");
            } else if (!status.error.empty()) {
                text = "Wireless monitor error\n" + status.error +
                       (status.file_name.empty() || wireless_event_log_deleted_ ? "\nESC close" :
                        "\nL log  X export  D delete\nESC close");
            } else if (status.complete) {
                text = "Monitor stopped\nEvents: " + std::to_string(status.events) +
                       (wireless_event_log_deleted_ ? "\nLog deleted  ESC close" :
                        status.file_name.empty() ? "\nNo log created\nESC close" :
                        "\nL log  X export  D delete\nESC close");
            } else if (wireless_event_stop_requested_) {
                text = "Stopping monitor...";
            } else if (status.listening) {
                text = std::string(probe ? "Probe requests: " : "Deauth events: ") +
                       std::to_string(status.events) +
                       (status.last_bssid.empty() ? "" : "\nAP: " + status.last_bssid) +
                       "\nUnverified; incomplete\n5 min/1000 max  ESC stop";
            } else {
                text = "Starting monitor...\nESC stop";
            }
        }
        if (text != wireless_event_display_) {
            wireless_event_display_ = text;
            toast_->show_persistent(text);
        }
    }

    void update_wireless_event_monitor() {
        if (wireless_event_stage_ != WirelessEventStage::Running) return;
        if (lab_export_task_ && lab_export_task_->complete) {
            if (lab_export_thread_.joinable()) lab_export_thread_.join();
            const bool deleting = lab_export_task_->deleting;
            const auto result = std::move(lab_export_task_->result);
            lab_export_task_.reset();
            if (deleting && result.error.empty()) wireless_event_log_deleted_ = true;
            wireless_event_notice_ = result.error.empty()
                ? (deleting ? "Log deleted\nExports remain" :
                              "Copy + SHA-256 saved\n~/poseidon-evidence")
                : (deleting ? "Delete failed\n" : "Export failed\n") + result.error;
        }
        show_wireless_event_monitor();
    }

    bool handle_wireless_event_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (wireless_event_stage_ == WirelessEventStage::Interface) {
            if (key == LV_KEY_ESC) {
                wireless_event_stage_ = WirelessEventStage::None;
                wireless_event_display_.clear();
                toast_->hide();
            } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!wireless_event_interface_.empty()) wireless_event_interface_.pop_back();
                wireless_event_setup_error_.clear();
                show_wireless_event_monitor();
            } else if (key == LV_KEY_ENTER) {
                if (!platform::valid_monitor_interface_name(wireless_event_interface_)) {
                    wireless_event_setup_error_ = "Invalid interface name";
                } else {
                    wireless_event_setup_error_.clear();
                    wireless_event_stage_ = wireless_event_interface_ == "esp32"
                        ? WirelessEventStage::Channel : WirelessEventStage::Confirm;
                    wireless_event_channel_ = 1;
                    wireless_event_channel_text_.clear();
                }
                show_wireless_event_monitor();
            } else if (((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
                        (key >= '0' && key <= '9') || key == '_' || key == '-' || key == '.') &&
                       wireless_event_interface_.size() < 15) {
                wireless_event_interface_ += static_cast<char>(key);
                wireless_event_setup_error_.clear();
                show_wireless_event_monitor();
            }
            return true;
        }
        if (wireless_event_stage_ == WirelessEventStage::Channel) {
            if (key == LV_KEY_ESC) {
                wireless_event_stage_ = WirelessEventStage::Interface;
                wireless_event_setup_error_.clear();
            } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!wireless_event_channel_text_.empty()) wireless_event_channel_text_.pop_back();
                wireless_event_setup_error_.clear();
            } else if (key == 'h' || key == 'H') {
                wireless_event_channel_ = 0;
                wireless_event_stage_ = WirelessEventStage::Confirm;
            } else if (key == LV_KEY_ENTER) {
                unsigned value = 1;
                const auto& input = wireless_event_channel_text_;
                if (!input.empty()) {
                    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
                    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) value = 0;
                }
                if (value < 1 || value > 11) wireless_event_setup_error_ = "Use channels 1-11";
                else {
                    wireless_event_channel_ = value;
                    wireless_event_stage_ = WirelessEventStage::Confirm;
                    wireless_event_setup_error_.clear();
                }
            } else if (key >= '0' && key <= '9' && wireless_event_channel_text_.size() < 2) {
                wireless_event_channel_text_ += static_cast<char>(key);
                wireless_event_setup_error_.clear();
            }
            show_wireless_event_monitor();
            return true;
        }
        if (wireless_event_stage_ == WirelessEventStage::Confirm) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wireless_event_stage_ = wireless_event_interface_ == "esp32"
                    ? WirelessEventStage::Channel : WirelessEventStage::Interface;
                show_wireless_event_monitor();
            } else if (key == LV_KEY_ENTER) {
                const char* home = std::getenv("HOME");
                if (!home || !*home || !std::filesystem::u8path(home).is_absolute()) {
                    wireless_event_stage_ = WirelessEventStage::None;
                    toast_->show("Home folder unavailable", view::widgets::ToastTone::Error, 4000);
                    return true;
                }
                std::string error;
                wireless_event_monitor_ = platform::WirelessEventMonitor::start(
                    wireless_event_interface_, std::filesystem::u8path(home) / "poseidon-wireless-monitor",
                    wireless_event_kind_, error, wireless_event_channel_);
                if (!wireless_event_monitor_) {
                    wireless_event_stage_ = WirelessEventStage::None;
                    toast_->show(error, view::widgets::ToastTone::Error, 4000);
                    return true;
                }
                wireless_event_stage_ = WirelessEventStage::Running;
                wireless_event_stop_requested_ = false;
                wireless_event_log_info_ = false;
                wireless_event_export_confirm_ = false;
                wireless_event_delete_confirm_ = false;
                wireless_event_log_deleted_ = false;
                wireless_event_notice_.clear();
                show_wireless_event_monitor();
            }
            return true;
        }
        const auto status = wireless_event_monitor_->status();
        if (lab_export_task_) return true;
        if (wireless_event_delete_confirm_) {
            if (key == LV_KEY_ENTER) {
                wireless_event_delete_confirm_ = false;
                start_security_log_action("poseidon-wireless-monitor", status.file_name,
                    wireless_event_kind_ == platform::WirelessEventKind::ProbeRequest
                        ? platform::EvidenceSource::ProbeMonitor
                        : platform::EvidenceSource::DeauthMonitor,
                    true, wireless_event_notice_);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wireless_event_delete_confirm_ = false;
            }
            show_wireless_event_monitor();
        } else if (wireless_event_export_confirm_) {
            if (key == LV_KEY_ENTER) {
                wireless_event_export_confirm_ = false;
                start_security_log_action("poseidon-wireless-monitor", status.file_name,
                                          wireless_event_kind_ == platform::WirelessEventKind::ProbeRequest
                                              ? platform::EvidenceSource::ProbeMonitor
                                              : platform::EvidenceSource::DeauthMonitor,
                                          false, wireless_event_notice_);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wireless_event_export_confirm_ = false;
            }
            show_wireless_event_monitor();
        } else if (!wireless_event_notice_.empty()) {
            if (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wireless_event_notice_.clear();
                show_wireless_event_monitor();
            }
        } else if ((key == 'd' || key == 'D') && status.complete && !status.file_name.empty() &&
                   !wireless_event_log_deleted_) {
            wireless_event_log_info_ = false;
            wireless_event_delete_confirm_ = true;
            show_wireless_event_monitor();
        } else if ((key == 'x' || key == 'X') && status.complete && !status.file_name.empty() &&
                   !wireless_event_log_deleted_) {
            wireless_event_log_info_ = false;
            wireless_event_export_confirm_ = true;
            show_wireless_event_monitor();
        } else if ((key == 'l' || key == 'L') && !status.file_name.empty() &&
                   !wireless_event_log_deleted_) {
            wireless_event_log_info_ = !wireless_event_log_info_;
            show_wireless_event_monitor();
        } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (!status.complete) {
                wireless_event_monitor_->stop();
                wireless_event_stop_requested_ = true;
                show_wireless_event_monitor();
            } else {
                wireless_event_monitor_.reset();
                wireless_event_stage_ = WirelessEventStage::None;
                wireless_event_display_.clear();
                toast_->hide();
            }
        }
        return true;
    }

    void show_adsb_receiver() {
        if (adsb_receiver_confirm_) {
            toast_->show_persistent(std::string("ADS-B 1090 MHz: check bias\n") +
                (adsb_receiver_record_ ? "L LOG ON " : "L LOG OFF ") +
                (adsb_receiver_gps_ ? "G GPS ON\n" : "G GPS OFF\n") +
                "LOG: aircraft IDs/position\nGPS: your location\nENTER start  ESC cancel");
            return;
        }
        if (!adsb_receiver_) return;
        const auto status = adsb_receiver_->status();
        std::string text;
        if (!status.error.empty()) text = "ADS-B error\n" + file_display_text(status.error, 100);
        else if (status.complete) text = "ADS-B stopped";
        else if (adsb_receiver_stopping_) text = "Stopping ADS-B receiver...";
        else if (!status.running) text = "Starting ADS-B receiver...";
        else if (status.valid_frames == 0) text = "Running; no valid frames yet";
        else if (adsb_aircraft_view_) {
            const auto& recent = status.recent_aircraft;
            auto selected = std::find_if(recent.begin(), recent.end(), [&](const auto& frame) {
                return frame.aircraft_address == adsb_aircraft_selected_;
            });
            if (selected == recent.end()) {
                selected = recent.begin();
                adsb_aircraft_selected_ = selected->aircraft_address;
            }
            const auto& frame = *selected;
            text = "Recent aircraft " + std::to_string(selected - recent.begin() + 1) +
                   "/" + std::to_string(recent.size()) +
                   "\n" + frame.aircraft_address + "  TC " + std::to_string(frame.type_code) +
                   adsb_frame_details(frame);
            text += "\nLast frame; unverified\nZ/C select  A live\nESC stop";
            if (text != adsb_receiver_display_) {
                adsb_receiver_display_ = text;
                toast_->show_persistent(text);
            }
            return;
        }
        else {
            text = "ADS-B " + status.latest.aircraft_address +
                   "  TC " + std::to_string(status.latest.type_code) +
                   adsb_frame_details(status.latest);
        }
        text += "\nValid: " + std::to_string(status.valid_frames) +
                "  Rejected: " + std::to_string(status.rejected_frames) +
                (status.gps_status.empty() ? "" : "\n" + status.gps_status) +
                (status.log_filename.empty() ? "" : "\nLog saved: " + status.log_filename) +
                "\nUnverified  A aircraft\nESC " + (status.complete ? "close" : "stop");
        if (text != adsb_receiver_display_) {
            adsb_receiver_display_ = text;
            toast_->show_persistent(text);
        }
    }

    void update_adsb_receiver() {
        if (adsb_receiver_active_ && !adsb_receiver_confirm_) show_adsb_receiver();
    }

    bool handle_adsb_receiver_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (adsb_receiver_confirm_) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                adsb_receiver_active_ = false;
                adsb_receiver_confirm_ = false;
                toast_->hide();
            } else if (key == LV_KEY_ENTER) {
                std::string error;
                std::filesystem::path log_home;
                std::string gps_device;
                if (adsb_receiver_record_) {
                    const char* home = std::getenv("HOME");
                    if (!home || !std::filesystem::path(home).is_absolute()) {
                        toast_->show_persistent("Log requires absolute HOME\nESC cancel");
                        return true;
                    }
                    log_home = std::filesystem::path(home);
                }
                if (adsb_receiver_gps_) {
                    const char* gps = std::getenv("POSEIDON_GPS_DEVICE");
                    if (!gps || !*gps) {
                        toast_->show_persistent("GPS device not configured\nESC cancel");
                        return true;
                    }
                    gps_device = gps;
                }
                adsb_receiver_ = platform::AdsbReceiver::start(error, log_home, gps_device);
                adsb_receiver_confirm_ = false;
                if (!adsb_receiver_) {
                    adsb_receiver_active_ = false;
                    toast_->show(error, view::widgets::ToastTone::Error, 5000);
                } else {
                    adsb_receiver_display_.clear();
                    adsb_receiver_stopping_ = false;
                    adsb_aircraft_view_ = false;
                    adsb_aircraft_selected_.clear();
                    show_adsb_receiver();
                }
            } else if (key == 'l' || key == 'L') {
                adsb_receiver_record_ = !adsb_receiver_record_;
                if (!adsb_receiver_record_) adsb_receiver_gps_ = false;
                show_adsb_receiver();
            } else if (key == 'g' || key == 'G') {
                if (!adsb_receiver_record_) {
                    toast_->show_persistent("Enable logging before GPS\nL log  ESC cancel");
                    return true;
                }
                adsb_receiver_gps_ = !adsb_receiver_gps_;
                show_adsb_receiver();
            }
        } else if ((key == 'a' || key == 'A') && adsb_receiver_) {
            const auto status = adsb_receiver_->status();
            if (status.running && !adsb_receiver_stopping_ && !status.recent_aircraft.empty()) {
                adsb_aircraft_view_ = !adsb_aircraft_view_;
                show_adsb_receiver();
            }
        } else if (adsb_aircraft_view_ && adsb_receiver_ &&
                   (key == LV_KEY_UP || key == LV_KEY_DOWN || key == 'z' || key == 'c')) {
            const auto status = adsb_receiver_->status();
            const auto& recent = status.recent_aircraft;
            if (!recent.empty()) {
                const auto selected = std::find_if(recent.begin(), recent.end(), [&](const auto& frame) {
                    return frame.aircraft_address == adsb_aircraft_selected_;
                });
                std::size_t index = selected == recent.end() ? 0 :
                    static_cast<std::size_t>(selected - recent.begin());
                if ((key == LV_KEY_UP || key == 'z') && index > 0) --index;
                else if ((key == LV_KEY_DOWN || key == 'c') && index + 1 < recent.size()) ++index;
                adsb_aircraft_selected_ = recent[index].aircraft_address;
                show_adsb_receiver();
            }
        } else if ((key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) && adsb_receiver_) {
            if (adsb_receiver_->status().complete) {
                adsb_receiver_.reset();
                adsb_receiver_active_ = false;
                toast_->hide();
            } else {
                adsb_receiver_->stop();
                adsb_receiver_stopping_ = true;
                toast_->show_persistent("Stopping ADS-B receiver...");
            }
        }
        return true;
    }

    void show_favorites() {
        const auto& favorites = theme_persistence_.config.favorites;
        std::string text = "Favorites";
        if (favorites.empty()) {
            text += "\nNo saved favorites";
        } else {
            if (favorite_selection_ >= favorites.size()) favorite_selection_ = favorites.size() - 1;
            const auto& item = favorites[favorite_selection_];
            text += " " + std::to_string(favorite_selection_ + 1) + "/" +
                    std::to_string(favorites.size()) + "\n" +
                    favorite_page_name(item[0]) + ": " + item.substr(2);
        }
        if (!favorite_notice_.empty()) text += "\n" + favorite_notice_;
        text += "\nS save selected  D remove\nZ/C select  ENTER go\nESC close";
        toast_->show_persistent(text);
    }

    bool save_favorites(std::vector<std::string> favorites) {
        auto config = theme_persistence_.config;
        config.favorites = std::move(favorites);
        std::string error;
        if (!save_application_config(theme_persistence_.config_path, config, error)) {
            LOG_WARN("failed to save favorites {}: {}", theme_persistence_.config_path, error);
            favorite_notice_ = "Save failed: " + file_display_text(error, 55);
            return false;
        }
        theme_persistence_.config = std::move(config);
        return true;
    }

    bool handle_favorites_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        auto favorites = theme_persistence_.config.favorites;
        favorite_notice_.clear();
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE || key == '0') {
            favorites_active_ = false;
            toast_->hide();
            return true;
        }
        if ((key == LV_KEY_UP || key == 'z') && favorite_selection_ > 0) {
            --favorite_selection_;
        } else if ((key == LV_KEY_DOWN || key == 'c') &&
                   favorite_selection_ + 1 < favorites.size()) {
            ++favorite_selection_;
        } else if (key == 's' || key == 'S') {
            const auto id = favorite_id(view_model_.current_page(), view_model_.tool_selection());
            if (id.empty()) favorite_notice_ = "Select a tool first";
            else if (std::find(favorites.begin(), favorites.end(), id) != favorites.end())
                favorite_notice_ = "Already saved";
            else if (favorites.size() >= kMaxFavorites)
                favorite_notice_ = "Favorites full";
            else {
                favorites.push_back(id);
                if (save_favorites(favorites)) {
                    favorite_selection_ = favorites.size() - 1;
                    favorite_notice_ = "Saved";
                }
            }
        } else if ((key == 'd' || key == 'D') && !favorites.empty()) {
            favorites.erase(favorites.begin() + favorite_selection_);
            if (save_favorites(favorites)) {
                if (favorite_selection_ >= favorites.size() && favorite_selection_ > 0)
                    --favorite_selection_;
                favorite_notice_ = "Removed";
            }
        } else if (key == LV_KEY_ENTER && !favorites.empty()) {
            const auto& id = favorites[favorite_selection_];
            const auto page = static_cast<model::AppPage>(id[0] - '0');
            int32_t index = 0;
            for (; index < model::tool_item_count(page); ++index) {
                if (favorite_id(page, index) == id) break;
            }
            if (index == model::tool_item_count(page)) {
                favorite_notice_ = "Tool no longer available";
            } else {
                favorites_active_ = false;
                toast_->hide();
                switch (page) {
                    case model::AppPage::Butter: view_model_.show_butter_page(); break;
                    case model::AppPage::Network: view_model_.show_network_page(); break;
                    case model::AppPage::Wireless: view_model_.show_wireless_page(); break;
                    case model::AppPage::Infrared: view_model_.show_infrared_page(); break;
                    case model::AppPage::Expansion: view_model_.show_expansion_page(); break;
                    case model::AppPage::Tools: view_model_.show_tools_page(); break;
                    case model::AppPage::Media: view_model_.show_media_page(); break;
                    default: break;
                }
                view_model_.set_tool_selection(index);
                return true;
            }
        }
        show_favorites();
        return true;
    }

    void show_readiness() {
        if (accessory_profiles_active_) {
            const auto& profile = kAccessoryProfiles[accessory_profile_selection_];
            const bool saved = (theme_persistence_.config.accessory_profiles &
                                (uint32_t{1} << accessory_profile_selection_)) != 0;
            std::string text = "Accessory " + std::to_string(accessory_profile_selection_ + 1) +
                               "/" + std::to_string(kAccessoryProfiles.size()) +
                               (saved ? " SAVED\n" : " not saved\n") +
                               profile.name + "\n" + profile.guidance +
                               "\nNo detection or setup";
            if (!accessory_profile_notice_.empty()) text += "\n" + accessory_profile_notice_;
            text += "\nS toggle Z/C browse\nESC back";
            toast_->show_persistent(text);
        } else if (readiness_task_) {
            toast_->show_persistent("Hardware readiness\nChecking local status...\nESC close");
        } else if (!readiness_error_.empty()) {
            toast_->show_persistent("Readiness failed\n" + readiness_error_ +
                                    "\nR retry P profiles ESC");
        } else {
            toast_->show_persistent("Hardware readiness " + std::to_string(readiness_page_ + 1) +
                "/4\n" + readiness_report_.pages[readiness_page_] +
                "\nZ/C page R refresh\nP profiles ESC close");
        }
    }

    void start_readiness() {
        readiness_active_ = true;
        if (readiness_task_) {
            show_readiness();
            return;
        }
        const char* home = std::getenv("HOME");
#if defined(_WIN32)
        if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
        const char* gps = std::getenv("POSEIDON_GPS_DEVICE");
        const char* esp = std::getenv("POSEIDON_ESP32_MONITOR_DEVICE");
        auto task = std::make_shared<ReadinessTask>();
        readiness_task_ = task;
        readiness_error_.clear();
        show_readiness();
        try {
            readiness_thread_ = std::thread([task, home_path = std::string(home ? home : ""),
                                             gps_path = std::string(gps ? gps : ""),
                                             esp_path = std::string(esp ? esp : "")] {
                try {
                    task->report = platform::read_device_readiness(
                        std::filesystem::u8path(home_path), gps_path, esp_path);
                } catch (const std::exception& error) {
                    task->error = error.what();
                    if (task->error.empty()) task->error = "Unknown readiness error";
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            readiness_task_.reset();
            readiness_error_ = "Cannot start: " + error.code().message();
            show_readiness();
        }
    }

    void update_readiness() {
        if (!readiness_task_ || !readiness_task_->complete) return;
        if (readiness_thread_.joinable()) readiness_thread_.join();
        readiness_report_ = std::move(readiness_task_->report);
        readiness_error_ = std::move(readiness_task_->error);
        readiness_task_.reset();
        if (readiness_active_) show_readiness();
    }

    bool handle_readiness_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (accessory_profiles_active_) {
            accessory_profile_notice_.clear();
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                accessory_profiles_active_ = false;
            } else if ((key == LV_KEY_LEFT || key == 'z') && accessory_profile_selection_ > 0) {
                --accessory_profile_selection_;
            } else if ((key == LV_KEY_RIGHT || key == 'c') &&
                       accessory_profile_selection_ + 1 < kAccessoryProfiles.size()) {
                ++accessory_profile_selection_;
            } else if (key == 's' || key == 'S') {
                auto config = theme_persistence_.config;
                config.accessory_profiles ^= uint32_t{1} << accessory_profile_selection_;
                std::string error;
                if (!save_application_config(theme_persistence_.config_path, config, error)) {
                    LOG_WARN("failed to save accessory profiles {}: {}",
                             theme_persistence_.config_path, error);
                    accessory_profile_notice_ = "Save failed: " + file_display_text(error, 48);
                } else {
                    theme_persistence_.config = std::move(config);
                    accessory_profile_notice_ = "Preference saved";
                }
            }
            show_readiness();
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            readiness_active_ = false;
            toast_->hide();
        } else if (!readiness_task_ && (key == 'p' || key == 'P')) {
            accessory_profiles_active_ = true;
            accessory_profile_selection_ = 0;
            accessory_profile_notice_.clear();
            show_readiness();
        } else if (!readiness_task_ && (key == 'r' || key == 'R')) {
            start_readiness();
        } else if (!readiness_task_ && readiness_error_.empty()) {
            if ((key == LV_KEY_LEFT || key == 'z') && readiness_page_ > 0) {
                --readiness_page_;
                show_readiness();
            } else if ((key == LV_KEY_RIGHT || key == 'c') && readiness_page_ < 3) {
                ++readiness_page_;
                show_readiness();
            }
        }
        return true;
    }

    void show_incident_checklist() {
        if (settings_backup_stage_ == SettingsBackupStage::Confirm) {
            toast_->show_persistent("Back up app UI settings?\nNo surveys, notes or secrets\n"
                                    "ENTER save  ESC cancel");
            return;
        }
        if (settings_backup_stage_ == SettingsBackupStage::Working) {
            toast_->show_persistent("Saving settings backup...\nPlease wait");
            return;
        }
        if (settings_backup_stage_ == SettingsBackupStage::Notice) {
            toast_->show_persistent(settings_backup_notice_ + "\nENTER/ESC back");
            return;
        }
        static constexpr const char* steps[] = {
            "1/6 Safety first\nIf harm is ongoing, isolate\nthrough your administrator.",
            "2/6 Preserve evidence\nRecord time and timezone.\nKeep original logs unchanged.",
            "3/6 Document scope\nNote affected devices and\nwhat you observed, not guesses.",
            "4/6 Contain safely\nUse your router or admin tools.\nDo not retaliate or probe others.",
            "5/6 Recover\nReview accounts and sessions.\nUpdate and verify devices.",
            "6/6 Escalate\nContact your security team or\nauthorities where appropriate."
        };
        static constexpr const char* guide[] = {
            "1/6 GPS\nA fix records your location,\nnot the transmitter location.",
            "2/6 Wi-Fi and BLE\nDiscovery may send requests.\nNames and addresses can be faked.",
            "3/6 Coverage\nNo match does not prove absence.\nHopping misses other channels.",
            "4/6 ESP32-C5\nCompanion firmware: 2.4 GHz,\nchannels 1-11. Hardware unverified.",
            "5/6 ADS-B\nPositions are broadcast reports,\nnot authenticated locations.",
            "6/6 Privacy and law\nCheck local rules and authority.\nDeletion does not erase copies."
        };
        const std::string text = (field_guide_active_ ? guide : steps)[incident_checklist_page_];
        toast_->show_persistent(text + (field_guide_active_
            ? "\nR checklist N notes B backup\nLEFT/RIGHT ESC"
            : "\nR guide N notes B backup\nLEFT/RIGHT ESC"));
    }

    void start_settings_backup() {
        const char* home = std::getenv("HOME");
        if (!home || !std::filesystem::path(home).is_absolute()) {
            settings_backup_notice_ = "HOME is not an absolute path";
            settings_backup_stage_ = SettingsBackupStage::Notice;
            show_incident_checklist();
            return;
        }
        auto task = std::make_shared<SettingsBackupTask>();
        settings_backup_task_ = task;
        settings_backup_stage_ = SettingsBackupStage::Working;
        show_incident_checklist();
        try {
            settings_backup_thread_ = std::thread(
                [task, directory = std::filesystem::path(home) / "poseidon-evidence",
                 config = theme_persistence_.config] {
                    try {
                        task->result = backup_application_settings(directory, config);
                    } catch (const std::exception& failure) {
                        task->result.error = std::string("Settings backup: ") + failure.what();
                    }
                    task->complete = true;
                });
        } catch (const std::system_error& failure) {
            settings_backup_task_.reset();
            settings_backup_notice_ = "Cannot start backup: " + failure.code().message();
            settings_backup_stage_ = SettingsBackupStage::Notice;
            show_incident_checklist();
        }
    }

    void update_settings_backup() {
        if (!settings_backup_task_ || !settings_backup_task_->complete) return;
        if (settings_backup_thread_.joinable()) settings_backup_thread_.join();
        const auto task = settings_backup_task_;
        settings_backup_task_.reset();
        settings_backup_notice_ = task->result.error.empty()
            ? "Settings saved in\n~/poseidon-evidence\n" + task->result.path.filename().u8string()
            : task->result.error;
        settings_backup_stage_ = SettingsBackupStage::Notice;
        if (incident_checklist_active_) show_incident_checklist();
    }

    bool handle_incident_checklist_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (settings_backup_stage_ == SettingsBackupStage::Confirm) {
            if (key == LV_KEY_ENTER) start_settings_backup();
            else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                settings_backup_stage_ = SettingsBackupStage::None;
                show_incident_checklist();
            }
            return true;
        }
        if (settings_backup_stage_ == SettingsBackupStage::Working) return true;
        if (settings_backup_stage_ == SettingsBackupStage::Notice) {
            if (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                settings_backup_stage_ = SettingsBackupStage::None;
                show_incident_checklist();
            }
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            incident_checklist_active_ = false;
            toast_->hide();
        } else if (key == 'n' || key == 'N') {
            start_field_note_action(NoteAction::List);
        } else if (key == 'b' || key == 'B') {
            settings_backup_stage_ = SettingsBackupStage::Confirm;
            show_incident_checklist();
        } else if (key == 'r' || key == 'R') {
            field_guide_active_ = !field_guide_active_;
            incident_checklist_page_ = 0;
            show_incident_checklist();
        } else if ((key == LV_KEY_LEFT || key == 'z') && incident_checklist_page_ > 0) {
            --incident_checklist_page_;
            show_incident_checklist();
        } else if ((key == LV_KEY_RIGHT || key == 'c') && incident_checklist_page_ < 5) {
            ++incident_checklist_page_;
            show_incident_checklist();
        }
        return true;
    }

    void show_field_notes() {
        std::string message;
        switch (note_stage_) {
            case NoteStage::List:
                if (note_files_.empty()) message = "Field notes\nNo notes saved\nN new  ESC back";
                else message = "Field note " + std::to_string(note_selection_ + 1) + "/" +
                    std::to_string(note_files_.size()) + "\n" +
                    file_display_text(note_files_[note_selection_], 28) +
                    "\nUP/DOWN ENTER read N new\nD delete  ESC back";
                break;
            case NoteStage::Edit: {
                const auto shown = note_buffer_.size() > 55
                    ? "..." + note_buffer_.substr(note_buffer_.size() - 55) : note_buffer_;
                message = "New note " + std::to_string(note_buffer_.size()) + "/160\n" +
                          (shown.empty() ? "_" : shown) +
                          "\nENTER save  ESC cancel";
                break;
            }
            case NoteStage::Viewing: {
                constexpr std::size_t width = 60;
                const auto pages = std::max<std::size_t>(1, (note_view_.text.size() + width - 1) / width);
                message = "Note " + std::to_string(note_page_ + 1) + "/" + std::to_string(pages) +
                          "\n" + file_display_text(note_view_.recorded_utc, 25) + "\n" +
                          file_display_text(note_view_.text.substr(note_page_ * width, width), width) +
                          "\nLEFT/RIGHT page  ESC back";
                break;
            }
            case NoteStage::ConfirmDelete:
                message = "Delete selected note?\nCopies are not deleted\nENTER delete  ESC cancel";
                break;
            case NoteStage::Working:
                message = "Working with field notes...\n" +
                    std::string(note_task_ && note_task_->action == NoteAction::Read ? "ESC cancels read" : "Please wait");
                break;
            case NoteStage::Notice:
                message = file_display_text(note_notice_, 120) + "\nENTER/ESC back";
                break;
            case NoteStage::None:
                return;
        }
        toast_->show_persistent(message, note_stage_ == NoteStage::Notice
            ? view::widgets::ToastTone::Warning : view::widgets::ToastTone::Default);
    }

    void start_field_note_action(NoteAction action) {
        const char* home = std::getenv("HOME");
        if (!home || !std::filesystem::path(home).is_absolute()) {
            note_notice_ = "HOME is not an absolute path";
            note_notice_return_ = action == NoteAction::Save ? NoteStage::Edit : NoteStage::None;
            note_stage_ = NoteStage::Notice;
            show_field_notes();
            return;
        }
        const auto directory = std::filesystem::path(home) / "poseidon-field-notes";
        const std::string name = action == NoteAction::Read || action == NoteAction::Delete
            ? note_files_[note_selection_] : "";
        auto task = std::make_shared<NoteTask>();
        task->action = action;
        note_task_ = task;
        note_stage_ = NoteStage::Working;
        show_field_notes();
        try {
            note_thread_ = std::thread([task, directory, name, text = note_buffer_] {
                try {
                    if (task->action == NoteAction::List) {
                        task->files = platform::list_survey_files(directory);
                        task->error = task->files.error;
                    } else if (task->action == NoteAction::Save) {
                        task->note = platform::save_field_note(directory, text);
                        task->error = task->note.error;
                    } else if (task->action == NoteAction::Read) {
                        task->note = platform::read_field_note(directory, name, task->cancelled);
                        task->error = task->note.error;
                    } else {
                        task->error = platform::delete_survey_file(directory, name);
                    }
                } catch (const std::exception& failure) {
                    task->error = std::string("Field notes: ") + failure.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& failure) {
            note_task_.reset();
            note_notice_ = "Cannot start note action: " + failure.code().message();
            note_notice_return_ = action == NoteAction::Save ? NoteStage::Edit : NoteStage::None;
            note_stage_ = NoteStage::Notice;
            show_field_notes();
        }
    }

    void update_field_notes() {
        if (!note_task_ || !note_task_->complete) return;
        if (note_thread_.joinable()) note_thread_.join();
        const auto task = note_task_;
        note_task_.reset();
        if (!task->error.empty() || (task->action == NoteAction::Read && task->cancelled)) {
            note_notice_ = task->error.empty() ? "Note read cancelled" : task->error;
            note_notice_return_ = task->action == NoteAction::Save ? NoteStage::Edit :
                                  task->action == NoteAction::List ? NoteStage::None : NoteStage::List;
            note_stage_ = NoteStage::Notice;
        } else if (task->action == NoteAction::List) {
            note_files_ = task->files.names;
            note_selection_ = 0;
            note_stage_ = NoteStage::List;
        } else if (task->action == NoteAction::Read) {
            note_view_ = task->note;
            note_page_ = 0;
            note_stage_ = NoteStage::Viewing;
        } else {
            note_notice_ = task->action == NoteAction::Save ? "Note saved" : "Note deleted; copies remain";
            if (task->action == NoteAction::Save) note_buffer_.clear();
            note_notice_return_ = NoteStage::List;
            note_stage_ = NoteStage::Notice;
        }
        show_field_notes();
    }

    bool handle_field_notes_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (note_stage_ == NoteStage::List) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                note_stage_ = NoteStage::None;
                note_buffer_.clear();
                note_files_.clear();
                show_incident_checklist();
                return true;
            } else if (key == 'n' || key == 'N') {
                note_buffer_.clear();
                note_stage_ = NoteStage::Edit;
            } else if (key == LV_KEY_ENTER && !note_files_.empty()) {
                start_field_note_action(NoteAction::Read);
                return true;
            } else if ((key == 'd' || key == 'D') && !note_files_.empty()) {
                note_stage_ = NoteStage::ConfirmDelete;
            } else if ((key == LV_KEY_UP || key == 'z') && note_selection_ > 0) {
                --note_selection_;
            } else if ((key == LV_KEY_DOWN || key == 'c') && note_selection_ + 1 < note_files_.size()) {
                ++note_selection_;
            }
        } else if (note_stage_ == NoteStage::Edit) {
            if (key == LV_KEY_ESC) {
                note_buffer_.clear();
                note_stage_ = NoteStage::List;
            }
            else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!note_buffer_.empty()) note_buffer_.pop_back();
            } else if (key == LV_KEY_ENTER) {
                start_field_note_action(NoteAction::Save);
                return true;
            } else if (key == '#') {
                note_notice_ = "Character # is not supported in notes";
                note_notice_return_ = NoteStage::Edit;
                note_stage_ = NoteStage::Notice;
            } else if (key >= 32 && key <= 126) {
                if (note_buffer_.size() < 160) note_buffer_.push_back(static_cast<char>(key));
                else {
                    note_notice_ = "Note limit is 160 characters";
                    note_notice_return_ = NoteStage::Edit;
                    note_stage_ = NoteStage::Notice;
                }
            }
        } else if (note_stage_ == NoteStage::Viewing) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                note_view_ = {};
                note_stage_ = NoteStage::List;
            } else if ((key == LV_KEY_LEFT || key == 'z') && note_page_ > 0) --note_page_;
            else if ((key == LV_KEY_RIGHT || key == 'c') && (note_page_ + 1) * 60 < note_view_.text.size()) ++note_page_;
        } else if (note_stage_ == NoteStage::ConfirmDelete) {
            if (key == LV_KEY_ENTER) {
                start_field_note_action(NoteAction::Delete);
                return true;
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) note_stage_ = NoteStage::List;
        } else if (note_stage_ == NoteStage::Working) {
            if (note_task_ && note_task_->action == NoteAction::Read &&
                (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) note_task_->cancelled = true;
        } else if (note_stage_ == NoteStage::Notice &&
                   (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            if (note_notice_return_ == NoteStage::List) {
                start_field_note_action(NoteAction::List);
                return true;
            }
            note_stage_ = note_notice_return_;
            if (note_stage_ == NoteStage::None) {
                show_incident_checklist();
                return true;
            }
        }
        show_field_notes();
        return true;
    }

    void show_ap_signal_test() {
        const auto device = platform::read_device_status();
        std::string message = "AP signal test\n";
        if (!device.wifi_present) {
            message += "Wi-Fi unavailable";
        } else if (!device.wifi_connected) {
            message += "Connect to an AP you own";
        } else if (device.wifi_strength_percent < 0) {
            message += "Signal measurement unavailable";
        } else {
            message += "Current link " +
                       std::to_string(device.wifi_strength_percent) + "%";
        }

        if (!ap_signal_samples_.empty()) {
            const auto bounds = std::minmax_element(ap_signal_samples_.begin(),
                                                    ap_signal_samples_.end());
            const int total = std::accumulate(ap_signal_samples_.begin(),
                                              ap_signal_samples_.end(), 0);
            message += "\n" + std::to_string(ap_signal_samples_.size()) +
                       "/10 samples  min/avg/max " +
                       std::to_string(*bounds.first) + "/" +
                       std::to_string(total / static_cast<int>(ap_signal_samples_.size())) +
                       "/" + std::to_string(*bounds.second) + "%";
        } else {
            message += "\nNo samples yet";
        }
        message += ap_signal_samples_.size() < 10
            ? "\nENTER sample  R reset\nESC back"
            : "\nLimit reached  R reset\nESC back";
        toast_->show_persistent(
            message, device.wifi_connected ? view::widgets::ToastTone::Default
                                           : view::widgets::ToastTone::Warning);
    }

    void show_wifi_channel_survey() {
        std::string message;
        if (!wifi_result_.error.empty()) {
            message = "Channel survey error\n" + wifi_result_.error;
        } else if (wifi_result_.networks.empty()) {
            message = "No visible Wi-Fi APs";
        } else {
            message = "Top 3 visible AP channels\n";
            const auto summary = platform::summarize_wifi_channels(wifi_result_.networks);
            if (summary.empty()) {
                message += "No known channel data";
            } else {
                const auto shown = std::min<std::size_t>(summary.size(), 3);
                for (std::size_t index = 0; index < shown; ++index) {
                    if (index > 0) message += "\n";
                    const auto& channel = summary[index];
                    message += channel.band + " ch " + std::to_string(channel.channel) +
                               ": " + std::to_string(channel.access_points) + " APs";
                }
            }
        }
        message += "\nENTER/R rescan  ESC back";
        toast_->show_persistent(
            message, wifi_result_.error.empty() ? view::widgets::ToastTone::Default
                                                : view::widgets::ToastTone::Warning);
    }

    bool handle_ap_signal_test_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            ap_signal_test_active_ = false;
            ap_signal_samples_.clear();
            toast_->hide();
        } else if (key == 'r' || key == 'R') {
            ap_signal_samples_.clear();
            show_ap_signal_test();
        } else if (key == LV_KEY_ENTER && ap_signal_samples_.size() < 10) {
            const auto device = platform::read_device_status();
            if (device.wifi_present && device.wifi_connected &&
                device.wifi_strength_percent >= 0) {
                ap_signal_samples_.push_back(device.wifi_strength_percent);
            }
            show_ap_signal_test();
        }
        return true;
    }

    void show_security_logs() {
        std::string message;
        switch (security_log_stage_) {
            case SecurityLogStage::Working:
                message = "Working with security logs...\nPlease wait";
                break;
            case SecurityLogStage::List:
                if (security_log_export_mode_) {
                    if (security_export_files_.empty()) {
                        message = "No evidence exports\nE logs  ESC close";
                    } else {
                        message = "Evidence file " + std::to_string(security_log_selection_ + 1) +
                                  "/" + std::to_string(security_export_files_.size()) + "\n" +
                                  security_export_files_[security_log_selection_] +
                                  "\nUP/DOWN D delete A delete all\nE logs  ESC close";
                    }
                } else if (security_logs_.empty()) {
                    message = "No saved security logs\nE exports  ESC close";
                } else {
                    message = "Security log " + std::to_string(security_log_selection_ + 1) +
                              "/" + std::to_string(security_logs_.size()) + "\n" +
                              security_logs_[security_log_selection_].filename +
                              "\nUP/DOWN  D delete  X export\nE exports  ESC close";
                }
                break;
            case SecurityLogStage::ConfirmDelete:
                message = security_log_export_mode_
                    ? "Delete this export file?\nIts paired file may remain.\nENTER delete  ESC cancel"
                    : "Delete original log?\n" + security_logs_[security_log_selection_].filename +
                      "\nExports are not deleted.\nENTER delete  ESC cancel";
                break;
            case SecurityLogStage::ConfirmDeleteAll:
                message = "Delete ALL evidence exports?\nOriginals and backups remain.\nENTER delete  ESC cancel";
                break;
            case SecurityLogStage::ConfirmExport:
                message = "Copy selected security log?\nMay contain identifiers.\n"
                          "ENTER copy + SHA-256  ESC";
                break;
            case SecurityLogStage::Notice:
                message = security_log_notice_ + "\nENTER refresh  ESC close";
                break;
            case SecurityLogStage::None:
                return;
        }
        toast_->show_persistent(message, security_log_stage_ == SecurityLogStage::Notice
                                             ? view::widgets::ToastTone::Warning
                                             : view::widgets::ToastTone::Default);
    }

    void start_security_log_history_action(SecurityLogAction action) {
        const char* home = std::getenv("HOME");
        if (!home || !std::filesystem::u8path(home).is_absolute()) {
            security_log_notice_ = "HOME is not an absolute path";
            security_log_stage_ = SecurityLogStage::Notice;
            show_security_logs();
            return;
        }
        const auto root = std::filesystem::u8path(home);
        platform::SecurityLogEntry selected;
        if (action == SecurityLogAction::Delete || action == SecurityLogAction::Export)
            selected = security_logs_[security_log_selection_];
        const std::string export_name = action == SecurityLogAction::DeleteExport
            ? security_export_files_[security_log_selection_] : "";
        if (action == SecurityLogAction::List) security_log_export_mode_ = false;
        if (action == SecurityLogAction::ListExports) security_log_export_mode_ = true;
        auto task = std::make_shared<SecurityLogTask>();
        task->action = action;
        security_log_task_ = task;
        security_log_stage_ = SecurityLogStage::Working;
        show_security_logs();
        try {
            security_log_thread_ = std::thread([task, root, selected, export_name] {
                try {
                    const auto directory = platform::security_log_directory(root, selected.source);
                    if (task->action == SecurityLogAction::Delete) {
                        task->error = platform::delete_security_log(
                            directory, selected.filename, selected.source);
                    } else if (task->action == SecurityLogAction::Export) {
                        task->error = platform::export_evidence(
                            directory, selected.filename, root / "poseidon-evidence",
                            selected.source).error;
                    } else if (task->action == SecurityLogAction::List) {
                        task->logs = platform::list_security_logs(root);
                        task->error = task->logs.error;
                    } else if (task->action == SecurityLogAction::ListExports) {
                        task->exports = platform::list_evidence_files(root / "poseidon-evidence");
                        task->error = task->exports.error;
                    } else if (task->action == SecurityLogAction::DeleteExport) {
                        task->error = platform::delete_evidence_file(root / "poseidon-evidence", export_name);
                    } else {
                        task->error = platform::delete_all_evidence_files(root / "poseidon-evidence");
                    }
                } catch (const std::exception& failure) {
                    task->error = std::string("Security logs: ") + failure.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& failure) {
            security_log_task_.reset();
            security_log_notice_ = "Cannot start log action: " + failure.code().message();
            security_log_stage_ = SecurityLogStage::Notice;
            show_security_logs();
        }
    }

    void update_security_logs() {
        if (!security_log_task_ || !security_log_task_->complete) return;
        if (security_log_thread_.joinable()) security_log_thread_.join();
        const auto task = security_log_task_;
        security_log_task_.reset();
        if (!task->error.empty()) {
            security_log_notice_ = task->error;
            security_log_stage_ = SecurityLogStage::Notice;
        } else if (task->action == SecurityLogAction::List) {
            security_logs_ = std::move(task->logs.entries);
            security_log_selection_ = 0;
            security_log_stage_ = SecurityLogStage::List;
        } else if (task->action == SecurityLogAction::ListExports) {
            security_export_files_ = std::move(task->exports.names);
            security_log_selection_ = 0;
            security_log_stage_ = SecurityLogStage::List;
        } else {
            security_log_notice_ = task->action == SecurityLogAction::Delete ? "Original deleted\nExports remain" :
                task->action == SecurityLogAction::Export ? "Copy + SHA-256 saved\n~/poseidon-evidence" :
                task->action == SecurityLogAction::DeleteExport ? "Export file deleted\nOther files remain" :
                "Evidence exports deleted\nOriginals and backups remain";
            security_log_stage_ = SecurityLogStage::Notice;
        }
        show_security_logs();
    }

    bool handle_security_logs_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (security_log_stage_ == SecurityLogStage::List) {
            const auto count = security_log_export_mode_ ? security_export_files_.size() : security_logs_.size();
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                security_log_stage_ = SecurityLogStage::None;
                toast_->hide();
                return true;
            } else if ((key == LV_KEY_UP || key == 'z') && security_log_selection_ > 0) {
                --security_log_selection_;
            } else if ((key == LV_KEY_DOWN || key == 'c') &&
                       security_log_selection_ + 1 < count) {
                ++security_log_selection_;
            } else if (key == 'e' || key == 'E') {
                start_security_log_history_action(security_log_export_mode_
                    ? SecurityLogAction::List : SecurityLogAction::ListExports);
                return true;
            } else if ((key == 'd' || key == 'D') && count) {
                security_log_stage_ = SecurityLogStage::ConfirmDelete;
            } else if ((key == 'a' || key == 'A') && security_log_export_mode_ && count) {
                security_log_stage_ = SecurityLogStage::ConfirmDeleteAll;
            } else if ((key == 'x' || key == 'X') && !security_log_export_mode_ && count) {
                security_log_stage_ = SecurityLogStage::ConfirmExport;
            }
            show_security_logs();
        } else if (security_log_stage_ == SecurityLogStage::ConfirmDelete ||
                   security_log_stage_ == SecurityLogStage::ConfirmExport ||
                   security_log_stage_ == SecurityLogStage::ConfirmDeleteAll) {
            if (key == LV_KEY_ENTER) {
                const auto action = security_log_stage_ == SecurityLogStage::ConfirmDeleteAll
                    ? SecurityLogAction::DeleteAllExports :
                    security_log_stage_ == SecurityLogStage::ConfirmExport ? SecurityLogAction::Export :
                    security_log_export_mode_ ? SecurityLogAction::DeleteExport : SecurityLogAction::Delete;
                start_security_log_history_action(action);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                security_log_stage_ = SecurityLogStage::List;
                show_security_logs();
            }
        } else if (security_log_stage_ == SecurityLogStage::Notice) {
            if (key == LV_KEY_ENTER) {
                start_security_log_history_action(security_log_export_mode_
                    ? SecurityLogAction::ListExports : SecurityLogAction::List);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                security_log_stage_ = SecurityLogStage::None;
                toast_->hide();
            }
        }
        return true;
    }

    void show_surveillance() {
        std::string text;
        if (surveillance_stage_ == SurveillanceStage::Confirm) {
            text = "Wi-Fi/BLE scans may transmit.\nCamera name hints only.\n"
                   "Unverified and incomplete.\nENTER scan  ESC cancel";
        } else if (surveillance_stage_ == SurveillanceStage::Scanning) {
            text = "Scanning Wi-Fi and BLE...\nESC cancels";
        } else if (surveillance_stage_ == SurveillanceStage::Results) {
            if (!surveillance_result_.error.empty()) {
                text = "Camera hint scan error\n" + surveillance_result_.error +
                       "\nENTER retry  ESC close";
            } else if (surveillance_result_.hints.empty()) {
                text = "No matching camera names\nNot proof none are present" +
                       std::string(surveillance_result_.results_may_be_truncated
                                       ? "\nScan limited" : "") +
                       "\nENTER rescan  ESC close";
            } else {
                const auto& hint = surveillance_result_.hints[surveillance_selection_];
                text = hint.transport + " name hint " +
                       std::to_string(surveillance_selection_ + 1) + "/" +
                       std::to_string(surveillance_result_.hints.size()) + "\n" +
                       file_display_text(hint.name, 28) + "\n" + hint.address +
                       (surveillance_result_.results_may_be_truncated
                            ? "\nUnverified; scan limited"
                            : surveillance_result_.stale_wifi
                                ? "\nUnverified; stale APs skipped" : "\nUnverified name only") +
                       "\nUP/DOWN  R rescan  ESC";
            }
        } else {
            return;
        }
        toast_->show_persistent(text, surveillance_result_.error.empty()
            ? view::widgets::ToastTone::Default : view::widgets::ToastTone::Warning);
    }

    void start_surveillance_scan() {
        if (surveillance_task_ || wifi_task_ || gatt_task_ || survey_session_) {
            surveillance_result_ = {};
            surveillance_result_.error = "Previous scan is still stopping";
            surveillance_stage_ = SurveillanceStage::Results;
            show_surveillance();
            return;
        }
        auto task = std::make_shared<SurveillanceTask>();
        surveillance_task_ = task;
        surveillance_stage_ = SurveillanceStage::Scanning;
        surveillance_result_ = {};
        show_surveillance();
        try {
            surveillance_thread_ = std::thread([task] {
                try {
                    task->result = platform::scan_surveillance_hints(task->cancelled);
                } catch (const std::exception& failure) {
                    task->result.error = std::string("Scan failed: ") + failure.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& failure) {
            surveillance_task_.reset();
            surveillance_result_.error = "Cannot start scan: " + failure.code().message();
            surveillance_stage_ = SurveillanceStage::Results;
            show_surveillance();
        }
    }

    void update_surveillance() {
        if (!surveillance_task_ || !surveillance_task_->complete) return;
        if (surveillance_thread_.joinable()) surveillance_thread_.join();
        const auto task = surveillance_task_;
        surveillance_task_.reset();
        if (task->cancelled) {
            if (!task->result.error.empty()) {
                LOG_WARN("Camera hint scan cleanup failed: {}", task->result.error);
                toast_->show("Scan cleanup failed\n" + task->result.error,
                             view::widgets::ToastTone::Warning, 5200);
            }
            return;
        }
        surveillance_result_ = std::move(task->result);
        surveillance_selection_ = 0;
        surveillance_stage_ = SurveillanceStage::Results;
        show_surveillance();
    }

    bool handle_surveillance_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            if (surveillance_task_) surveillance_task_->cancelled = true;
            surveillance_stage_ = SurveillanceStage::None;
            toast_->hide();
        } else if (surveillance_stage_ == SurveillanceStage::Confirm && key == LV_KEY_ENTER) {
            start_surveillance_scan();
        } else if (surveillance_stage_ == SurveillanceStage::Results) {
            if ((key == LV_KEY_UP || key == 'z') && surveillance_selection_ > 0) {
                --surveillance_selection_;
                show_surveillance();
            } else if ((key == LV_KEY_DOWN || key == 'c') &&
                       surveillance_selection_ + 1 < surveillance_result_.hints.size()) {
                ++surveillance_selection_;
                show_surveillance();
            } else if (key == 'r' || key == 'R' ||
                       (key == LV_KEY_ENTER && (surveillance_result_.hints.empty() ||
                                                !surveillance_result_.error.empty()))) {
                start_surveillance_scan();
            }
        }
        return true;
    }

    bool survey_directory(std::filesystem::path& directory, std::string& error) {
        const char* home = std::getenv("HOME");
        if (!home || !std::filesystem::path(home).is_absolute()) {
            error = "HOME is not an absolute path";
            return false;
        }
        directory = std::filesystem::path(home) / "poseidon-surveys";
        return true;
    }

    void show_survey() {
        std::string message;
        switch (survey_stage_) {
            case SurveyStage::Home:
                message = "Wireless survey\nWi-Fi/BLE scans may transmit\n"
                          "ENTER start  S sessions\nESC back";
                break;
            case SurveyStage::Running:
            case SurveyStage::Stopping: {
                const auto status = survey_session_->status();
                if (survey_quality_ && survey_stage_ == SurveyStage::Running) {
                    message = "Survey row quality\nGPS tagged " + std::to_string(status.gps_tagged) +
                              "  missing " + std::to_string(status.gps_untagged) +
                              "\nStale Wi-Fi skipped " + std::to_string(status.wifi_skipped) +
                              (status.results_may_be_truncated ? "\nResults may be limited" :
                                                               "\nCoverage not guaranteed") +
                              "\nQ back  ESC stop";
                } else {
                    message = status.phase + "\nWi-Fi " + std::to_string(status.wifi_count) +
                              " BLE " + std::to_string(status.ble_count) + "\n" +
                              "Hints " + std::to_string(status.indicator_count) +
                              "  watch rows " + std::to_string(status.watchlist_rows) +
                              "\nHints unverified\n" +
                              status.gps_status +
                              (status.results_may_be_truncated ? "\nResults may be limited" : "") +
                              (survey_stage_ == SurveyStage::Stopping ? "\nStopping..."
                                                                      : "\nQ quality  ESC stop");
                }
                break;
            }
            case SurveyStage::Sessions:
                if (survey_files_.empty()) {
                    message = "No saved surveys\nESC back";
                } else {
                    message = "Survey " + std::to_string(survey_selection_ + 1) + "/" +
                              std::to_string(survey_files_.size()) + "\n" +
                              survey_files_[survey_selection_] +
                              "\nUP/DOWN T timeline D delete\nX export  A delete all  ESC";
                }
                break;
            case SurveyStage::Timeline:
                if (survey_watch_filter_ && !survey_timeline_.indicators_available) {
                    message = "Watch filter unavailable\nNo indicator column in file\nW timeline P plot G GPX ESC";
                } else if (survey_watch_filter_ && survey_watch_indices_.empty()) {
                    message = "No watch matches in latest\n" +
                              std::to_string(survey_timeline_.entries.size()) +
                              " rows; not proof of absence\nW timeline P plot G GPX ESC";
                } else if (survey_timeline_.entries.empty()) {
                    message = "Survey timeline\nNo observation rows\nW filter P plot G GPX ESC";
                } else {
                    const auto index = survey_watch_filter_ ? survey_watch_indices_[survey_timeline_selection_]
                                                            : survey_timeline_selection_;
                    const auto& entry = survey_timeline_.entries[index];
                    const auto visible = survey_watch_filter_ ? survey_watch_indices_.size()
                                                              : survey_timeline_.entries.size();
                    message = std::string(survey_watch_filter_ ? "Watch " : "Timeline ") +
                              std::to_string(survey_timeline_selection_ + 1) + "/" +
                              std::to_string(visible) +
                              (survey_timeline_.total_rows > survey_timeline_.entries.size() ? " (latest)" : "") +
                              "\n" + file_display_text(entry.recorded_utc, 25) +
                              "\n" + file_display_text(entry.source, 8) + " " +
                              file_display_text(entry.name, 20) +
                              (entry.gps_latitude.empty() ? "\nObserver GPS unavailable" :
                               "\nObserver GPS " + file_display_text(entry.gps_latitude, 13) + "," +
                               file_display_text(entry.gps_longitude, 14)) +
                              "\nZ/C W filter P plot\nG GPX M map Q QR ESC";
                }
                break;
            case SurveyStage::MapNotice:
                message = survey_notice_ + "\nZ/C zoom ENTER/ESC back";
                break;
            case SurveyStage::QrNotice:
                message = survey_notice_ + "\nENTER/ESC back";
                break;
            case SurveyStage::ConfirmQr:
                message = "Show receiver GPS in QR?\nAnyone nearby can scan it.\n"
                          "No names or radio IDs.\nENTER show  ESC cancel";
                break;
            case SurveyStage::ConfirmOne:
                message = "Delete selected survey?\n" + survey_files_[survey_selection_] +
                          "\nENTER delete  ESC cancel";
                break;
            case SurveyStage::ConfirmExport:
                message = "Copy selected survey?\nMay contain GPS and identifiers.\n"
                          "ENTER copy + SHA-256  ESC";
                break;
            case SurveyStage::ConfirmGpx:
                message = "Export latest 128 rows GPS?\nReceiver points; all rows\n"
                          "Separate file; ENTER yes ESC no";
                break;
            case SurveyStage::ConfirmAll:
                message = "Delete ALL survey sessions?\nExports are not deleted\n"
                          "ENTER delete  ESC cancel";
                break;
            case SurveyStage::FileOperation:
                message = survey_file_task_ && survey_file_task_->action == SurveyFileAction::Timeline
                              ? "Reading survey timeline...\nESC cancels"
                              : "Working with survey files...\nPlease wait";
                break;
            case SurveyStage::Notice:
                message = survey_notice_ + "\nENTER/ESC back";
                break;
            case SurveyStage::None:
                return;
        }
        toast_->show_persistent(message, survey_stage_ == SurveyStage::Notice
                                             ? view::widgets::ToastTone::Warning
                                             : view::widgets::ToastTone::Default);
    }

    void start_survey_map(uint32_t zoom) {
        survey_map_zoom_ = zoom;
        const auto visible = survey_watch_filter_ ? survey_watch_indices_.size()
                                                   : survey_timeline_.entries.size();
        if (visible == 0) {
            survey_notice_ = "Select a timeline row first";
            survey_stage_ = SurveyStage::MapNotice;
            show_survey();
            return;
        }
        const auto index = survey_watch_filter_ ? survey_watch_indices_[survey_timeline_selection_]
                                                : survey_timeline_selection_;
        const auto tile = platform::survey_map_tile(survey_timeline_.entries[index], zoom);
        const char* home = std::getenv("HOME");
        if (!tile.error.empty() || !home || !std::filesystem::path(home).is_absolute()) {
            survey_notice_ = !tile.error.empty() ? tile.error : "HOME is not an absolute path";
            survey_stage_ = SurveyStage::MapNotice;
            show_survey();
            return;
        }
        if (gallery_task_) {
            survey_notice_ = "Previous image read is stopping";
            survey_stage_ = SurveyStage::MapNotice;
            show_survey();
            return;
        }
        const auto root = std::filesystem::path(home) / "poseidon-maps";
        const auto path = root /
                          std::to_string(tile.zoom) / std::to_string(tile.x) /
                          (std::to_string(tile.y) + ".png");
        auto task = std::make_shared<GalleryQueryState>();
        task->survey_map = true;
        task->map_tile = tile;
        gallery_task_ = task;
        try {
            gallery_thread_ = std::thread([task, path, root] {
                platform::GalleryImage image;
                platform::SurveyMapAttribution attribution;
                try {
                    attribution = platform::read_survey_map_attribution(root / "attribution.txt");
                    if (!attribution.error.empty()) {
                        image.error = attribution.error;
                    } else {
                        image = platform::read_gallery_image(path, &task->cancelled);
                        if (image.error.empty() && (image.width != 256 || image.height != 256))
                            image.error = "Map tile must be 256x256";
                    }
                } catch (const std::exception& error) {
                    image.error = std::string("Map read failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = std::move(image);
                task->map_attribution = std::move(attribution.text);
                task->map_attribution_glyphs = std::move(attribution.glyphs);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            gallery_task_.reset();
            survey_notice_ = "Cannot start map read: " + error.code().message();
            survey_stage_ = SurveyStage::MapNotice;
            show_survey();
            return;
        }
        gallery_loading_ = true;
        toast_->show_persistent("Opening offline map tile\nESC cancels");
    }

    void show_survey_qr() {
        const auto visible = survey_watch_filter_ ? survey_watch_indices_.size()
                                                  : survey_timeline_.entries.size();
        if (!visible) {
            survey_notice_ = "Select a timeline row first";
            survey_stage_ = SurveyStage::QrNotice;
            show_survey();
            return;
        }
        const auto index = survey_watch_filter_ ? survey_watch_indices_[survey_timeline_selection_]
                                                : survey_timeline_selection_;
        auto qr = make_survey_qr(survey_timeline_.entries[index]);
        if (!qr.error.empty()) {
            survey_notice_ = qr.error;
            survey_stage_ = SurveyStage::QrNotice;
            show_survey();
            return;
        }
        gallery_pixels_ = std::move(qr.pixels);
        gallery_image_ = {};
        gallery_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        gallery_image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
        gallery_image_.header.w = qr.width;
        gallery_image_.header.h = qr.width;
        gallery_image_.header.stride = qr.width * 4U;
        gallery_image_.data_size = static_cast<uint32_t>(gallery_pixels_.size());
        gallery_image_.data = gallery_pixels_.data();
        survey_stage_ = SurveyStage::Timeline;
        visual_tool_mode_ = VisualToolMode::SurveyQr;
        if (!create_visual_overlay()) {
            close_visual_tool();
            survey_notice_ = "Could not open GPS QR";
            survey_stage_ = SurveyStage::QrNotice;
            show_survey();
            return;
        }
        auto* image = lv_image_create(overlay_);
        if (!image) {
            close_visual_tool();
            survey_notice_ = "Could not create GPS QR";
            survey_stage_ = SurveyStage::QrNotice;
            show_survey();
            return;
        }
        lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
        lv_image_set_src(image, &gallery_image_);
        lv_obj_align(image, LV_ALIGN_LEFT_MID, 4, 0);
        lv_label_set_text(screen_test_label_,
                          "Receiver GPS\nCoordinates only\nNot transmitter\nlocation\n\nESC back");
        lv_obj_set_style_text_color(screen_test_label_, lv_color_white(), 0);
        lv_obj_align(screen_test_label_, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_move_foreground(screen_test_label_);
    }

    void start_survey() {
#if USE_DESKTOP
        survey_notice_ = "Survey requires a supported device";
        survey_stage_ = SurveyStage::Notice;
        show_survey();
        return;
#endif
        if (wifi_task_ || gatt_task_ || surveillance_task_) {
            survey_notice_ = "Previous scan is still stopping";
            survey_stage_ = SurveyStage::Notice;
            show_survey();
            return;
        }
        std::filesystem::path directory;
        std::string error;
        if (!survey_directory(directory, error)) {
            survey_notice_ = error;
            survey_stage_ = SurveyStage::Notice;
            show_survey();
            return;
        }
        const char* gps = std::getenv("POSEIDON_GPS_DEVICE");
        survey_session_ = platform::SurveySession::start(directory, gps ? gps : "", error);
        if (!survey_session_) {
            survey_notice_ = error;
            survey_stage_ = SurveyStage::Notice;
        } else {
            survey_stage_ = SurveyStage::Running;
            survey_quality_ = false;
            survey_next_update_ = {};
        }
        show_survey();
    }

    void start_survey_file_action(SurveyFileAction action) {
        std::filesystem::path directory;
        std::string error;
        if (!survey_directory(directory, error)) {
            survey_notice_ = error;
            survey_stage_ = SurveyStage::Notice;
            show_survey();
            return;
        }
        const std::string name = action == SurveyFileAction::DeleteOne || action == SurveyFileAction::Export ||
                                         action == SurveyFileAction::Timeline
                                     ? survey_files_[survey_selection_] : "";
        auto task = std::make_shared<SurveyFileTask>();
        task->action = action;
        if (action == SurveyFileAction::Gpx) task->timeline = survey_timeline_;
        survey_file_task_ = task;
        survey_stage_ = SurveyStage::FileOperation;
        show_survey();
        try {
            survey_file_thread_ = std::thread([task, directory, name] {
                try {
                    if (task->action == SurveyFileAction::DeleteOne)
                        task->error = platform::delete_survey_file(directory, name);
                    else if (task->action == SurveyFileAction::DeleteAll)
                        task->error = platform::delete_all_survey_files(directory);
                    else if (task->action == SurveyFileAction::Export) {
                        task->evidence = platform::export_evidence(
                            directory, name, directory.parent_path() / "poseidon-evidence",
                            platform::EvidenceSource::Survey);
                        task->error = task->evidence.error;
                    } else if (task->action == SurveyFileAction::Timeline) {
                        task->timeline = platform::read_survey_timeline(directory, name, task->cancelled);
                        task->error = task->timeline.error;
                    } else if (task->action == SurveyFileAction::Gpx) {
                        task->gpx = platform::export_survey_gpx(
                            directory.parent_path() / "poseidon-evidence", task->timeline);
                        task->error = task->gpx.error;
                    }
                    if (task->error.empty() && task->action != SurveyFileAction::Timeline &&
                        task->action != SurveyFileAction::Gpx) {
                        task->files = platform::list_survey_files(directory);
                        task->error = task->files.error;
                    }
                } catch (const std::exception& failure) {
                    task->error = std::string("Survey files: ") + failure.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& failure) {
            survey_file_task_.reset();
            survey_notice_ = "Cannot start file action: " + failure.code().message();
            survey_stage_ = SurveyStage::Notice;
            show_survey();
        }
    }

    void update_survey() {
        if (survey_file_task_ && survey_file_task_->complete) {
            if (survey_file_thread_.joinable()) survey_file_thread_.join();
            const auto task = survey_file_task_;
            survey_file_task_.reset();
            if (!task->error.empty() ||
                (task->action == SurveyFileAction::Timeline && task->cancelled)) {
                survey_notice_ = task->error.empty() ? "Timeline cancelled" : task->error;
                survey_stage_ = SurveyStage::Notice;
            } else if (task->action == SurveyFileAction::Export) {
                survey_notice_ = "Evidence saved in\n~/poseidon-evidence\n" +
                                 task->evidence.manifest_path.filename().u8string();
                survey_stage_ = SurveyStage::Notice;
            } else if (task->action == SurveyFileAction::Gpx) {
                survey_notice_ = "GPS export: " + std::to_string(task->gpx.points) +
                                 " points\n~/poseidon-evidence\n" +
                                 task->gpx.path.filename().u8string();
                survey_stage_ = SurveyStage::Notice;
            } else if (task->action == SurveyFileAction::Timeline) {
                survey_timeline_ = std::move(task->timeline);
                survey_watch_filter_ = false;
                survey_watch_indices_.clear();
                for (std::size_t i = 0; i < survey_timeline_.entries.size(); ++i)
                    if (platform::survey_watchlist_match(survey_timeline_.entries[i].indicators))
                        survey_watch_indices_.push_back(i);
                survey_timeline_selection_ = survey_timeline_.entries.empty()
                    ? 0 : survey_timeline_.entries.size() - 1;
                survey_stage_ = SurveyStage::Timeline;
            } else {
                survey_files_ = task->files.names;
                survey_selection_ = 0;
                survey_stage_ = SurveyStage::Sessions;
            }
            show_survey();
        }
        if (!survey_session_) return;
        const auto status = survey_session_->status();
        if (status.complete) {
            survey_session_.reset();
            survey_notice_ = status.error.empty() ? "Survey saved\n" + status.file_name
                                                  : "Survey stopped: " + status.error;
            if (status.results_may_be_truncated) survey_notice_ += "\nResults may be limited";
            if (status.wifi_skipped) survey_notice_ += "\nStale/unknown APs skipped";
            if (status.gps_untagged) survey_notice_ += "\nRows without GPS: " + std::to_string(status.gps_untagged);
            survey_stage_ = SurveyStage::Notice;
            show_survey();
        } else if (std::chrono::steady_clock::now() >= survey_next_update_) {
            show_survey();
            survey_next_update_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
    }

    bool handle_survey_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (survey_stage_ == SurveyStage::Running) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_session_->stop();
                survey_stage_ = SurveyStage::Stopping;
                show_survey();
            } else if (key == 'q' || key == 'Q') {
                survey_quality_ = !survey_quality_;
                show_survey();
            }
        } else if (survey_stage_ == SurveyStage::Home) {
            if (key == LV_KEY_ENTER) start_survey();
            else if (key == 's' || key == 'S') start_survey_file_action(SurveyFileAction::List);
            else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_stage_ = SurveyStage::None;
                toast_->hide();
            }
        } else if (survey_stage_ == SurveyStage::Sessions) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_stage_ = SurveyStage::Home;
            } else if ((key == LV_KEY_UP || key == 'z') && survey_selection_ > 0) {
                --survey_selection_;
            } else if ((key == LV_KEY_DOWN || key == 'c') &&
                       survey_selection_ + 1 < survey_files_.size()) {
                ++survey_selection_;
            } else if ((key == 'd' || key == 'D') && !survey_files_.empty()) {
                survey_stage_ = SurveyStage::ConfirmOne;
            } else if ((key == 'x' || key == 'X') && !survey_files_.empty()) {
                survey_stage_ = SurveyStage::ConfirmExport;
            } else if ((key == 't' || key == 'T') && !survey_files_.empty()) {
                start_survey_file_action(SurveyFileAction::Timeline);
            } else if ((key == 'a' || key == 'A') && !survey_files_.empty()) {
                survey_stage_ = SurveyStage::ConfirmAll;
            }
            show_survey();
        } else if (survey_stage_ == SurveyStage::Timeline) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_timeline_ = {};
                survey_watch_indices_.clear();
                survey_watch_filter_ = false;
                survey_stage_ = SurveyStage::Sessions;
            } else if (key == 'p' || key == 'P') {
                show_survey_plot();
                return true;
            } else if (key == 'm' || key == 'M') {
                start_survey_map(14);
                return true;
            } else if (key == 'q' || key == 'Q') {
                survey_stage_ = SurveyStage::ConfirmQr;
            } else if (key == 'g' || key == 'G') {
                survey_stage_ = SurveyStage::ConfirmGpx;
            } else if (key == 'w' || key == 'W') {
                survey_watch_filter_ = !survey_watch_filter_;
                const auto visible = survey_watch_filter_ ? survey_watch_indices_.size()
                                                          : survey_timeline_.entries.size();
                survey_timeline_selection_ = visible ? visible - 1 : 0;
            } else if ((key == LV_KEY_UP || key == 'z') && survey_timeline_selection_ > 0) {
                --survey_timeline_selection_;
            } else if ((key == LV_KEY_DOWN || key == 'c') &&
                       survey_timeline_selection_ + 1 <
                           (survey_watch_filter_ ? survey_watch_indices_.size()
                                                 : survey_timeline_.entries.size())) {
                ++survey_timeline_selection_;
            }
            show_survey();
        } else if (survey_stage_ == SurveyStage::ConfirmQr) {
            if (key == LV_KEY_ENTER) show_survey_qr();
            else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_stage_ = SurveyStage::Timeline;
                show_survey();
            }
        } else if (survey_stage_ == SurveyStage::QrNotice &&
                   (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            survey_stage_ = SurveyStage::Timeline;
            show_survey();
        } else if (survey_stage_ == SurveyStage::MapNotice &&
                   (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            survey_stage_ = SurveyStage::Timeline;
            show_survey();
        } else if (survey_stage_ == SurveyStage::MapNotice &&
                   (key == LV_KEY_LEFT || key == 'z') && survey_map_zoom_ > 0) {
            survey_stage_ = SurveyStage::Timeline;
            start_survey_map(survey_map_zoom_ - 1);
        } else if (survey_stage_ == SurveyStage::MapNotice &&
                   (key == LV_KEY_RIGHT || key == 'c') && survey_map_zoom_ < 18) {
            survey_stage_ = SurveyStage::Timeline;
            start_survey_map(survey_map_zoom_ + 1);
        } else if (survey_stage_ == SurveyStage::FileOperation && survey_file_task_ &&
                   survey_file_task_->action == SurveyFileAction::Timeline &&
                   (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            survey_file_task_->cancelled = true;
            toast_->show_persistent("Cancelling timeline...");
        } else if (survey_stage_ == SurveyStage::ConfirmGpx) {
            if (key == LV_KEY_ENTER) start_survey_file_action(SurveyFileAction::Gpx);
            else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_stage_ = SurveyStage::Timeline;
                show_survey();
            }
        } else if (survey_stage_ == SurveyStage::ConfirmOne ||
                   survey_stage_ == SurveyStage::ConfirmExport ||
                   survey_stage_ == SurveyStage::ConfirmAll) {
            if (key == LV_KEY_ENTER) {
                start_survey_file_action(survey_stage_ == SurveyStage::ConfirmOne
                    ? SurveyFileAction::DeleteOne : survey_stage_ == SurveyStage::ConfirmExport
                    ? SurveyFileAction::Export : SurveyFileAction::DeleteAll);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                survey_stage_ = SurveyStage::Sessions;
                show_survey();
            }
        } else if (survey_stage_ == SurveyStage::Notice &&
                   (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
            survey_stage_ = SurveyStage::Home;
            show_survey();
        }
        return true;
    }

    void start_wifi_scan(bool channel_survey = false) {
        if (wifi_task_ || surveillance_task_) {
            toast_->show("Previous Wi-Fi scan is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        wifi_channel_survey_active_ = channel_survey;
        auto task = std::make_shared<WifiTask>();
        wifi_task_ = task;
        wifi_active_ = true;
        wifi_scanning_ = true;
        wifi_stage_ = WifiStage::Results;
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
            if (wifi_monitor_active_) {
                show_wifi_monitor();
            } else if (wifi_channel_survey_active_) {
                show_wifi_channel_survey();
            } else {
                show_wifi();
            }
        }
    }

    void show_wifi() {
        std::string message;
        if (!wifi_result_.error.empty()) {
            message += "Wi-Fi error\n" + wifi_result_.error;
        } else if (wifi_result_.networks.empty()) {
            message += "No Wi-Fi networks found";
        } else {
            const auto& network = wifi_result_.networks[wifi_selection_];
            const int channel = platform::wifi_channel(network.frequency_mhz);
            message += "Wi-Fi " + std::to_string(wifi_selection_ + 1) + "/" +
                      std::to_string(wifi_result_.networks.size()) + "\n" +
                      network.name + "\n" + network.address + "\n" +
                      std::to_string(network.strength) + "%  " +
                      (network.secured ? "Secured" : "Open") +
                      (channel >= 0 ? "  Ch " + std::to_string(channel) : "");
        }
        message += wifi_result_.networks.empty()
            ? "\nENTER rescan  ESC back"
            : "\nUP/DOWN  ENTER connect\nH advice  R rescan  ESC";
        toast_->show_persistent(message, wifi_result_.error.empty()
                                             ? view::widgets::ToastTone::Default
                                             : view::widgets::ToastTone::Warning);
    }

    void show_wifi_monitor() {
        std::string message;
        if (!wifi_result_.error.empty()) {
            message = "AP monitor error\n" + wifi_result_.error + "\nR retry";
        } else if (!wifi_monitor_has_baseline_ && !wifi_monitor_comparison_.error.empty()) {
            message = "AP baseline unavailable\n" + wifi_monitor_comparison_.error;
        } else if (!wifi_monitor_has_baseline_) {
            message = "No fresh AP baseline\nR retries; results may be limited";
        } else if (!wifi_monitor_compared_) {
            message = "AP baseline: " + std::to_string(wifi_monitor_comparison_.baseline_count) +
                      "\nR compares another scan";
        } else if (!wifi_monitor_comparison_.error.empty()) {
            message = "Comparison unavailable\n" + wifi_monitor_comparison_.error + "\nR retry";
        } else {
            message = "AP changes (unverified)\nNew " +
                      std::to_string(wifi_monitor_comparison_.new_addresses) +
                      "  changed " + std::to_string(wifi_monitor_comparison_.changed) +
                      "\nNot seen " + std::to_string(wifi_monitor_comparison_.not_seen) +
                      " (unknown)\nD details  R rescan";
        }
        message += "\nLimited scan; no deauth  ESC";
        toast_->show_persistent(message, wifi_result_.error.empty()
            ? view::widgets::ToastTone::Default : view::widgets::ToastTone::Warning);
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
        if (task->connecting) {
            if (!wifi_active_) return;
            wifi_stage_ = WifiStage::Notice;
            if (task->connection_error.empty()) {
                toast_->show_persistent("Connection requested\nCheck Wi-Fi status for result\nENTER/ESC back");
            } else {
                toast_->show_persistent(task->connection_error + "\nENTER/ESC back",
                                        view::widgets::ToastTone::Warning);
            }
            return;
        }
        if (task->cancelled) return;
        wifi_result_ = std::move(task->result);
        wifi_selection_ = 0;
        if (wifi_monitor_active_) {
            if (wifi_result_.error.empty()) {
                if (!wifi_monitor_has_baseline_) {
                    wifi_monitor_comparison_ =
                        platform::compare_wifi_observations(wifi_result_, wifi_result_);
                    if (wifi_monitor_comparison_.error.empty() &&
                        wifi_monitor_comparison_.baseline_count > 0) {
                        wifi_monitor_baseline_ = wifi_result_;
                        wifi_monitor_has_baseline_ = true;
                    }
                } else {
                    wifi_monitor_comparison_ =
                        platform::compare_wifi_observations(wifi_monitor_baseline_, wifi_result_);
                    wifi_monitor_compared_ = true;
                }
            }
            show_wifi_monitor();
        } else if (wifi_channel_survey_active_) {
            show_wifi_channel_survey();
        } else {
            show_wifi();
        }
    }

    bool handle_wifi_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (wifi_monitor_active_) {
            if (wifi_monitor_details_) {
                const auto pages = std::max<std::size_t>(1, (wifi_monitor_detail_.size() + 89) / 90);
                if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                    wifi_monitor_details_ = false;
                    show_wifi_monitor();
                } else if ((key == LV_KEY_LEFT || key == 'z') && wifi_monitor_page_ > 0) {
                    --wifi_monitor_page_;
                } else if ((key == LV_KEY_RIGHT || key == 'c') && wifi_monitor_page_ + 1 < pages) {
                    ++wifi_monitor_page_;
                } else return true;
                if (wifi_monitor_details_)
                    toast_->show_persistent("AP details " + std::to_string(wifi_monitor_page_ + 1) +
                        "/" + std::to_string(pages) + "\n" +
                        wifi_monitor_detail_.substr(wifi_monitor_page_ * 90, 90) +
                        "\nLEFT/RIGHT  ESC");
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                if (wifi_task_) wifi_task_->cancelled = true;
                wifi_monitor_active_ = false;
                wifi_active_ = false;
                toast_->hide();
            } else if (!wifi_scanning_ && (key == 'r' || key == 'R')) {
                start_wifi_scan();
            } else if (!wifi_scanning_ && wifi_monitor_compared_ && wifi_result_.error.empty() &&
                       wifi_monitor_comparison_.error.empty() && (key == 'd' || key == 'D')) {
                wifi_monitor_detail_ = "Observations can be stale, spoofed, or incomplete. ";
                for (const auto& detail : wifi_monitor_comparison_.details)
                    wifi_monitor_detail_ += detail + ". ";
                wifi_monitor_detail_ += "Skipped stale, ambiguous, or duplicate entries: " +
                                        std::to_string(wifi_monitor_comparison_.skipped) +
                                        ". Scans may be limited to 40 APs.";
                wifi_monitor_details_ = true;
                wifi_monitor_page_ = 0;
                toast_->show_persistent("AP details 1/" +
                    std::to_string((wifi_monitor_detail_.size() + 89) / 90) + "\n" +
                    wifi_monitor_detail_.substr(0, 90) + "\nLEFT/RIGHT  ESC");
            }
            return true;
        }
        if (wifi_channel_survey_active_) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                if (wifi_task_) wifi_task_->cancelled = true;
                wifi_active_ = false;
                wifi_channel_survey_active_ = false;
                toast_->hide();
            } else if (!wifi_scanning_ &&
                       (key == LV_KEY_ENTER || key == 'r' || key == 'R')) {
                start_wifi_scan(true);
            }
            return true;
        }
        if (wifi_stage_ == WifiStage::Connecting) return true;
        if (wifi_stage_ == WifiStage::SecurityAdvice) {
            if (key == LV_KEY_ENTER || key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                wifi_stage_ = WifiStage::Results;
                show_wifi();
            }
            return true;
        }
        if (wifi_stage_ == WifiStage::Notice) {
            if (key == LV_KEY_ENTER || key == LV_KEY_ESC) {
                wifi_stage_ = WifiStage::Results;
                show_wifi();
            }
            return true;
        }
        if (wifi_stage_ == WifiStage::Password) {
            if (key == LV_KEY_ESC) {
                std::fill(wifi_password_.begin(), wifi_password_.end(), '\0');
                wifi_password_.clear();
                wifi_stage_ = WifiStage::Results;
                show_wifi();
            } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
                if (!wifi_password_.empty()) {
                    wifi_password_.back() = '\0';
                    wifi_password_.pop_back();
                }
                show_wifi_password();
            } else if (key == LV_KEY_ENTER) {
                const auto security = platform::wifi_security(wifi_result_.networks[wifi_selection_]);
                if (!platform::valid_wifi_password(security, wifi_password_)) {
                    show_wifi_password("Invalid password");
                } else {
                    show_wifi_confirm();
                }
            } else if (key >= 32 && key <= 126 && wifi_password_.size() < 64) {
                wifi_password_.push_back(static_cast<char>(key));
                show_wifi_password();
            }
            return true;
        }
        if (wifi_stage_ == WifiStage::ChooseConnection) {
            if (key == LV_KEY_ESC) {
                wifi_stage_ = WifiStage::Results;
                show_wifi();
            } else if (key == 's' || key == 'S') {
                wifi_connect_mode_ = platform::WifiConnectMode::Saved;
                show_wifi_confirm();
            } else if (key == 'n' || key == 'N') {
                wifi_connect_mode_ = platform::WifiConnectMode::New;
                const auto security = platform::wifi_security(wifi_result_.networks[wifi_selection_]);
                if (security == platform::WifiSecurity::Unsupported) {
                    toast_->show_persistent("Security not supported for new profiles\nS saved  ESC back",
                                            view::widgets::ToastTone::Warning);
                } else if (security == platform::WifiSecurity::Open) {
                    show_wifi_confirm();
                } else {
                    wifi_stage_ = WifiStage::Password;
                    show_wifi_password();
                }
            }
            return true;
        }
        if (wifi_stage_ == WifiStage::Confirm) {
            if (key == LV_KEY_ESC) {
                std::fill(wifi_password_.begin(), wifi_password_.end(), '\0');
                wifi_password_.clear();
                wifi_stage_ = WifiStage::Results;
                show_wifi();
            } else if (key == LV_KEY_ENTER) {
                request_wifi_connection();
            }
            return true;
        }
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
            } else if ((key == 'r' || key == 'R') ||
                       (key == LV_KEY_ENTER && wifi_result_.networks.empty())) {
                start_wifi_scan();
            } else if ((key == 'h' || key == 'H') && !wifi_result_.networks.empty() &&
                       wifi_result_.error.empty()) {
                wifi_stage_ = WifiStage::SecurityAdvice;
                toast_->show_persistent(platform::wifi_hardening_advice(wifi_result_.networks[wifi_selection_]) +
                    "\nAdvertised, not verified\nOnly change your own AP\nENTER/ESC back");
            } else if (key == LV_KEY_ENTER && !wifi_result_.networks.empty()) {
                wifi_stage_ = WifiStage::ChooseConnection;
                toast_->show_persistent("Connect to " + wifi_result_.networks[wifi_selection_].name +
                                        "\nS saved profile  N new profile\nESC back");
            }
        }
        return true;
    }

    void show_wifi_password(const std::string& notice = {}) {
        std::string message = notice.empty() ? "" : notice + "\n";
        message += "Wi-Fi password: " + std::string(wifi_password_.size(), '*') +
                   "\nENTER next  ESC back";
        toast_->show_persistent(message, notice.empty() ? view::widgets::ToastTone::Default
                                                       : view::widgets::ToastTone::Warning);
    }

    void show_wifi_confirm() {
        wifi_stage_ = WifiStage::Confirm;
        toast_->show_persistent("Switch Wi-Fi to " + wifi_result_.networks[wifi_selection_].name +
            (wifi_connect_mode_ == platform::WifiConnectMode::New
                ? "\nNew profile saved by NetworkManager" : "\nUse saved profile") +
            "\nENTER connect  ESC cancel");
    }

    void request_wifi_connection() {
        auto task = std::make_shared<WifiTask>();
        task->connecting = true;
        wifi_task_ = task;
        wifi_stage_ = WifiStage::Connecting;
        const auto network = wifi_result_.networks[wifi_selection_];
        const auto mode = wifi_connect_mode_;
        std::string password = std::move(wifi_password_);
        wifi_password_.clear();
        toast_->show_persistent("Requesting Wi-Fi connection...\nPlease wait");
        try {
            wifi_thread_ = std::thread([task, network, mode, password = std::move(password)]() mutable {
                std::string error;
                try {
                    error = platform::request_wifi_connection(network, mode, password);
                } catch (const std::exception& exception) {
                    error = std::string("Connection request failed: ") + exception.what();
                }
                std::fill(password.begin(), password.end(), '\0');
                std::lock_guard<std::mutex> lock(task->mutex);
                task->connection_error = std::move(error);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            std::fill(password.begin(), password.end(), '\0');
            wifi_task_.reset();
            wifi_stage_ = WifiStage::Notice;
            toast_->show_persistent("Could not start connection: " + error.code().message() +
                                        "\nENTER/ESC back",
                                    view::widgets::ToastTone::Error);
        }
    }

    void start_gatt_scan(bool tracker_mode = false) {
        if (gatt_task_ || surveillance_task_) {
            toast_->show("Previous Bluetooth operation is stopping",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        gatt_tracker_mode_ = tracker_mode;
        auto task = std::make_shared<GattTask>();
        task->scan = true;
        gatt_task_ = task;
        gatt_stage_ = GattStage::Scanning;
        toast_->show_persistent(tracker_mode ? "Scanning BLE tracker hints...\nESC cancels"
                                           : "Scanning nearby BLE devices...\nESC cancels");
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
        if (gatt_stage_ == GattStage::ConfirmTracker) {
            message = "BLE scan may transmit.\nFCB2 hints only; unverified.\n"
                      "ENTER scan  ESC cancel";
        } else if (gatt_stage_ == GattStage::Devices && !gatt_devices_.empty()) {
            const auto& device = gatt_devices_[gatt_device_index_];
            message = std::string(gatt_tracker_mode_ ? "FCB2 unverified " : "BLE ") +
                      std::to_string(gatt_device_index_ + 1) + "/" +
                      std::to_string(gatt_devices_.size()) + "\n" +
                      (device.name.empty() ? "Unnamed device" : device.name) + "\n" +
                      device.address +
                      (gatt_tracker_mode_ ? "\nUP/DOWN  R rescan  ESC"
                                          : "\nUP/DOWN select  ENTER open\nESC back");
        } else if (gatt_stage_ == GattStage::Entries && !gatt_entries_.empty()) {
            const auto& entry = gatt_entries_[gatt_entry_index_];
            message = std::string(entry.characteristic ? "Characteristic " : "Service ") +
                      std::to_string(gatt_entry_index_ + 1) + "/" +
                      std::to_string(gatt_entries_.size()) + "\n" + entry.uuid +
                      "\nUP/DOWN browse  ESC back";
        } else if (gatt_stage_ == GattStage::Empty) {
            message = gatt_tracker_mode_
                          ? "No FCB2 hints found\nNot proof no trackers\nENTER rescan  ESC back"
                          : gatt_devices_.empty()
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
            gatt_devices_ = gatt_tracker_mode_
                ? platform::tracker_indicator_devices(std::move(task->scan_result.devices))
                : std::move(task->scan_result.devices);
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
        if (gatt_stage_ == GattStage::ConfirmTracker) {
            if (key == LV_KEY_ENTER) {
                start_gatt_scan(true);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                gatt_stage_ = GattStage::None;
                toast_->hide();
            }
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
        } else if (gatt_tracker_mode_ && (key == 'r' || key == 'R') &&
                   (gatt_stage_ == GattStage::Devices || gatt_stage_ == GattStage::Empty ||
                    gatt_stage_ == GattStage::Error)) {
            start_gatt_scan(true);
        } else if (key == LV_KEY_ENTER) {
            if (gatt_stage_ == GattStage::Devices) {
                if (!gatt_tracker_mode_) start_gatt_browse();
            } else if (gatt_stage_ == GattStage::Error ||
                       (gatt_stage_ == GattStage::Empty && gatt_devices_.empty())) {
                start_gatt_scan(gatt_tracker_mode_);
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
        else if (text_tool_mode_ == TextToolMode::Adsb) {
            message += "ADS-B DF17 hex (28 digits)\n";
            message += text_tool_buffer_.empty() ? "_" : text_tool_buffer_;
            message += "\nENTER decode  ESC cancel";
        }
        else {
            message += text_tool_mode_ == TextToolMode::Ping ? "Ping host: "
                       : text_tool_mode_ == TextToolMode::Https ? "HTTPS host: " : "DNS name: ";
            message += text_tool_buffer_.empty() ? "_" : text_tool_buffer_;
            message += "\nEnter=run  Esc=cancel";
        }
        toast_->show_persistent(message, tone);
    }

    void start_text_tool(TextToolMode mode) {
        text_tool_mode_ = mode;
        https_confirm_ = false;
        text_tool_buffer_.clear();
        show_text_tool_prompt();
    }

    void cancel_text_tool() {
        text_tool_mode_ = TextToolMode::None;
        https_confirm_ = false;
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

    void show_expansion_inventory() {
        const auto inventory = platform::read_expansion_bus_inventory();
        std::string message = "EXT bus nodes (read-only)";
        if (!inventory.error.empty()) {
            message += "\n" + file_display_text(inventory.error, 38);
        } else {
            const auto summarize = [](const std::vector<std::string>& nodes) {
                if (nodes.empty()) {
                    return std::string("none");
                }
                std::string summary = std::filesystem::path(nodes.front()).filename().string();
                if (nodes.size() > 1) {
                    summary += " +" + std::to_string(nodes.size() - 1);
                }
                return summary;
            };
            message += "\nSPI: " + summarize(inventory.spi_nodes);
            message += "\nGPIO: " + summarize(inventory.gpio_chips);
            message += "\nNodes only; hats not identified";
        }
        toast_->show(message, inventory.error.empty()
                                  ? view::widgets::ToastTone::Default
                                  : view::widgets::ToastTone::Warning,
                     5200);
    }

    void show_cap_nfc_readiness() {
        const auto probe = platform::read_cap_nfc_identity();
        if (!probe.detected) {
            const std::string detail = probe.error.empty()
                                           ? "No valid chip identity"
                                           : file_display_text(probe.error, 38);
            toast_->show("NFC readiness unavailable\n" + detail,
                         view::widgets::ToastTone::Warning,
                         5200);
            return;
        }

        char identity[5]{};
        std::snprintf(identity, sizeof(identity), "0x%02X", probe.identity.value);
        toast_->show("ST25R3916 detected\nIdentity " + std::string(identity) +
                         " / Rev " + std::to_string(probe.identity.revision) +
                         "\nNo RF or card scan",
                     view::widgets::ToastTone::Success,
                     5200);
    }

    static std::string script_line(const std::string& text, std::size_t offset) {
        std::string line;
        const auto end = std::min(text.size(), offset + 6);
        for (auto index = offset; index < end; ++index) {
            const auto ch = static_cast<unsigned char>(text[index]);
            if (ch == '#') line += "\\x23";
            else if (ch == '\\') line += "\\\\";
            else if (ch == '\n') line += "\\n";
            else if (ch == '\r') line += "\\r";
            else if (ch == '\t') line += "\\t";
            else line += ch >= 32 && ch < 127 ? static_cast<char>(ch) : '?';
        }
        return line;
    }

    void show_script_page() {
        const bool preview = script_stage_ == ScriptStage::Preview;
        const auto& text = preview ? script_.text : script_output_;
        const std::size_t pages = std::max<std::size_t>(1, (text.size() + 11) / 12);
        const auto offset = script_page_ * 12;
        std::string message = preview ? "Script preview " : "Script result ";
        message += std::to_string(script_page_ + 1) + "/" + std::to_string(pages);
        message += "\n" + script_line(text, offset) + "\n" + script_line(text, offset + 6);
        message += preview ? "\nN/P pages  ENTER review\nESC back"
                           : "\nN/P pages  ESC back";
        toast_->show_persistent(message);
    }

    void start_script_load(const std::string& filename) {
        if (script_task_) return;
        auto task = std::make_shared<ScriptTask>();
        task->loading = true;
        script_task_ = task;
        script_stage_ = ScriptStage::Loading;
        const auto directory = file_directory_;
        toast_->show_persistent("Reading script...\nESC cancels");
        try {
            script_thread_ = std::thread([task, directory, filename] {
                try {
                    task->script = platform::read_local_script(directory, filename);
                } catch (const std::exception& error) {
                    task->script.error = std::string("Script read failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            script_task_.reset();
            script_stage_ = ScriptStage::None;
            file_notice_ = "Cannot start script read: " + error.code().message();
            show_file_browser();
        }
    }

    void start_script_run() {
        auto task = std::make_shared<ScriptTask>();
        task->script = script_;
        script_task_ = task;
        script_stage_ = ScriptStage::Running;
        toast_->show_persistent("Running local script...\nESC requests stop\nTimeout: 60 seconds");
        try {
            script_thread_ = std::thread([task] {
                try {
                    task->result = platform::run_local_script(task->script, task->cancelled);
                } catch (const std::exception& error) {
                    task->result.error = std::string("Script failed: ") + error.what();
                }
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            script_task_.reset();
            script_stage_ = ScriptStage::None;
            file_notice_ = "Cannot start script: " + error.code().message();
            show_file_browser();
        }
    }

    void update_script() {
        const auto task = script_task_;
        if (!task || !task->complete) return;
        if (script_thread_.joinable()) script_thread_.join();
        script_task_.reset();
        if (task->cancelled && task->loading) {
            script_stage_ = ScriptStage::None;
            show_file_browser();
            return;
        }
        if (task->loading) {
            if (!task->script.error.empty()) {
                script_stage_ = ScriptStage::None;
                file_notice_ = task->script.error;
                show_file_browser();
                return;
            }
            script_ = std::move(task->script);
            script_stage_ = ScriptStage::Preview;
        } else {
            script_output_ = task->result.error.empty()
                ? "Exit " + std::to_string(task->result.exit_code)
                : task->result.error;
            script_output_ += "\n" + task->result.output;
            if (task->result.truncated) script_output_ += "\n[output truncated]";
            script_stage_ = ScriptStage::Result;
        }
        script_page_ = 0;
        show_script_page();
    }

    bool handle_script_key(uint32_t key, bool long_pressed) {
        if (long_pressed) return true;
        if (script_stage_ == ScriptStage::Loading || script_stage_ == ScriptStage::Running) {
            if (key == LV_KEY_ESC && script_task_) {
                script_task_->cancelled = true;
                toast_->show_persistent("Stopping...\nPlease wait");
            }
            return true;
        }
        if (key == LV_KEY_ESC) {
            if (script_stage_ == ScriptStage::Confirm) {
                script_stage_ = ScriptStage::Preview;
                show_script_page();
            } else {
                script_stage_ = ScriptStage::None;
                script_ = {};
                show_file_browser();
            }
            return true;
        }
        if (script_stage_ == ScriptStage::Confirm) {
            if (key == LV_KEY_ENTER) start_script_run();
            return true;
        }
        const auto& text = script_stage_ == ScriptStage::Preview ? script_.text : script_output_;
        const std::size_t pages = std::max<std::size_t>(1, (text.size() + 11) / 12);
        if ((key == 'n' || key == 'N' || key == LV_KEY_DOWN) && script_page_ + 1 < pages)
            ++script_page_;
        else if ((key == 'p' || key == 'P' || key == LV_KEY_UP) && script_page_ > 0)
            --script_page_;
        else if (key == LV_KEY_ENTER && script_stage_ == ScriptStage::Preview) {
            if (script_page_ + 1 != pages) {
                toast_->show_persistent("Review all pages first\nN next  ESC back",
                                        view::widgets::ToastTone::Warning);
                return true;
            }
            script_stage_ = ScriptStage::Confirm;
            toast_->show_persistent("Run trusted local script?\nApp-user access; no sandbox\nFiles/network may change\nENTER run  ESC back",
                                    view::widgets::ToastTone::Warning);
            return true;
        }
        show_script_page();
        return true;
    }

    void show_file_browser() {
        if (file_mode_ == FileMode::I2c) {
            std::string message = "I2C buses (read-only)";
            if (!file_notice_.empty()) {
                message += "\n" + file_display_text(file_notice_, 30);
            } else if (file_entries_.empty()) {
                message += "\nNo buses exposed";
            } else {
                message += "\nBus " + std::to_string(file_offset_ + file_selection_ + 1) +
                           (file_truncated_ ? "+" : "") + ": " +
                           file_display_text(file_entries_[file_selection_].name, 20);
            }
            message += "\nNo device probing or control\nUP/DOWN select  P/N page\nESC back";
            toast_->show_persistent(message, file_notice_.empty()
                                                 ? view::widgets::ToastTone::Default
                                                 : view::widgets::ToastTone::Warning);
            return;
        }
        std::string folder = file_directory_.filename().u8string();
        if (folder.empty()) {
            folder = file_directory_.root_path().u8string();
        }
        const char* title = file_mode_ == FileMode::Gallery ? "Gallery: "
                            : file_mode_ == FileMode::AlbumArt ? "Album art: "
                            : file_mode_ == FileMode::Music ? "Music: "
                            : file_mode_ == FileMode::Scripts ? "Scripts: " : "Files: ";
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
                   : file_mode_ == FileMode::Files ? "\nENTER open  H hash file\n"
                                                  : "\nUP/DOWN select  ENTER open\n";
        message += file_mode_ == FileMode::Scripts ? "P/N page  ESC exit"
                                                  : "P/N page  BACK up  ESC exit";
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
        task->create_directory = file_mode_ == FileMode::Scripts;
        file_task_ = task;
        try {
            file_thread_ = std::thread([task] {
                platform::DirectoryResult result;
                try {
                    std::error_code error;
                    if (task->create_directory)
                        std::filesystem::create_directories(task->directory, error);
                    if (error) result.error = "Cannot create scripts folder: " + error.message();
                    else result = platform::read_directory(task->directory.u8string(),
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

    void start_file_hash(const std::filesystem::path& path) {
        auto task = std::make_shared<FileHashTask>();
        file_hash_task_ = task;
        try {
            file_hash_thread_ = std::thread([task, path] {
                task->result = platform::hash_local_file(path, task->cancelled);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            file_hash_task_.reset();
            file_notice_ = "Cannot start hash: " + error.code().message();
            show_file_browser();
            return;
        }
        toast_->show_persistent("Hashing file (max 10 MiB)\nESC cancels");
    }

    void update_file_hash() {
        const auto task = file_hash_task_;
        if (!task || !task->complete) return;
        if (file_hash_thread_.joinable()) file_hash_thread_.join();
        file_hash_task_.reset();
        if (!file_browser_active_) return;
        if (task->cancelled || !task->result.error.empty()) {
            file_notice_ = task->cancelled ? "Hash cancelled" : task->result.error;
            show_file_browser();
            return;
        }
        file_hash_showing_ = true;
        const auto& digest = task->result.sha256;
        toast_->show_persistent("SHA-256\n" + digest.substr(0, 32) + "\n" +
                                digest.substr(32) + "\nNot proof of authenticity\nENTER/ESC back");
    }

    void start_file_browser(FileMode mode = FileMode::Files) {
        if (mode == FileMode::I2c) {
#if USE_DESKTOP
            toast_->show("I2C bus listing is unavailable\nin the desktop simulator",
                         view::widgets::ToastTone::Warning, 4200);
#else
            file_mode_ = mode;
            open_file_directory("/sys/class/i2c-dev");
#endif
            return;
        }
#if USE_DESKTOP
        if (mode == FileMode::Scripts) {
            toast_->show("Local scripts are unavailable\nin the desktop simulator",
                         view::widgets::ToastTone::Warning, 4200);
            return;
        }
#endif
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
            auto directory = std::filesystem::u8path(home);
            if (mode == FileMode::Scripts) directory /= "poseidon-scripts";
            open_file_directory(directory);
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
        if (file_hash_task_) {
            if (key == LV_KEY_ESC) {
                file_hash_task_->cancelled = true;
                toast_->show_persistent("Cancelling hash...");
            }
            return true;
        }
        if (file_hash_showing_) {
            if (key == LV_KEY_ESC || key == LV_KEY_ENTER) {
                file_hash_showing_ = false;
                show_file_browser();
            }
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
        if ((key == 'h' || key == 'H') && file_mode_ == FileMode::Files &&
            !file_entries_.empty()) {
            const auto& entry = file_entries_[file_selection_];
            if (entry.directory) {
                file_notice_ = "Select a regular file";
                show_file_browser();
            } else {
                start_file_hash(file_directory_ / std::filesystem::u8path(entry.name));
            }
            return true;
        }
        if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
            if (file_mode_ == FileMode::I2c || file_mode_ == FileMode::Scripts) return true;
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
            if (file_mode_ == FileMode::I2c) return true;
            const auto& entry = file_entries_[file_selection_];
            const auto path = file_directory_ / std::filesystem::u8path(entry.name);
            if (file_mode_ == FileMode::Scripts) {
                if (entry.directory) {
                    file_notice_ = "Select a .sh or .py file";
                    show_file_browser();
                } else {
                    start_script_load(entry.name);
                }
            } else if (entry.directory) {
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
                    result = platform::capture_camera(task->cancelled, &task->jpeg);
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

    void save_camera_photo() {
#if USE_DESKTOP && defined(_WIN32)
        const wchar_t* home = _wgetenv(L"HOME");
        if (!home || home[0] == L'\0') home = _wgetenv(L"USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (!home || home[0] == 0 || !std::filesystem::path(home).is_absolute()) {
            toast_->show("An absolute home directory is required",
                         view::widgets::ToastTone::Error, 4200);
            return;
        }
        const auto directory = std::filesystem::path(home) / "poseidon-photos";
        auto task = std::make_shared<GalleryQueryState>();
        task->camera = true;
        task->saving = true;
        task->jpeg = std::move(camera_jpeg_);
        gallery_task_ = task;
        toast_->show_persistent("Saving photo...\nPlease wait");
        try {
            gallery_thread_ = std::thread([task, directory] {
                platform::CameraSaveResult result;
                try {
                    const auto error = platform::ensure_private_directory(directory);
                    if (!error.empty()) result.error = "Photo folder: " + error;
                    else result = platform::save_camera_jpeg(directory, task->jpeg);
                } catch (const std::exception& error) {
                    result.error = std::string("Photo save failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(task->mutex);
                task->save_result = std::move(result);
                task->complete = true;
            });
        } catch (const std::system_error& error) {
            camera_jpeg_ = std::move(task->jpeg);
            gallery_task_.reset();
            toast_->show("Could not save: " + error.code().message(),
                         view::widgets::ToastTone::Error, 4200);
        }
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
        if (task->saving) {
            camera_jpeg_ = std::move(task->jpeg);
            if (!task->save_result.error.empty()) {
                toast_->show(task->save_result.error, view::widgets::ToastTone::Error, 5000);
            } else {
                toast_->show("Saved in poseidon-photos:\n" +
                                 file_display_text(task->save_result.path.filename().u8string(), 28),
                             view::widgets::ToastTone::Default, 5000);
            }
            return;
        }
        if (task->cancelled || !gallery_loading_) {
            return;
        }
        gallery_loading_ = false;
        if (!task->result.error.empty()) {
            if (task->survey_map) {
                survey_notice_ = "Map " + std::to_string(task->map_tile.zoom) + "/" +
                                 std::to_string(task->map_tile.x) + "/" +
                                 std::to_string(task->map_tile.y) + "\n" +
                                 file_display_text(task->result.error, 80);
                survey_stage_ = SurveyStage::MapNotice;
                show_survey();
            } else if (task->camera) {
                toast_->show("Camera: " + task->result.error,
                             view::widgets::ToastTone::Error, 5000);
            } else {
                file_notice_ = task->result.error;
                show_file_browser();
            }
            return;
        }
        gallery_camera_ = task->camera;
        camera_jpeg_ = std::move(task->jpeg);
        gallery_pixels_ = std::move(task->result.pixels);
        gallery_image_ = {};
        gallery_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        gallery_image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
        gallery_image_.header.w = task->result.width;
        gallery_image_.header.h = task->result.height;
        gallery_image_.header.stride = task->result.width * 4U;
        gallery_image_.data_size = static_cast<uint32_t>(gallery_pixels_.size());
        gallery_image_.data = gallery_pixels_.data();
        visual_tool_mode_ = task->survey_map ? VisualToolMode::SurveyMap : VisualToolMode::Gallery;
        if (!create_visual_overlay()) {
            close_visual_tool();
            if (task->survey_map) {
                survey_notice_ = "Could not open offline map";
                survey_stage_ = SurveyStage::MapNotice;
                show_survey();
            } else if (task->camera) {
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
            if (task->survey_map) {
                survey_notice_ = "Could not create map image";
                survey_stage_ = SurveyStage::MapNotice;
                show_survey();
            } else if (task->camera) {
                toast_->show("Could not create image viewer", view::widgets::ToastTone::Error, 4200);
            } else {
                file_notice_ = "Could not create image viewer";
                show_file_browser();
            }
            return;
        }
        lv_image_set_src(image, &gallery_image_);
        if (task->survey_map) {
            const lv_font_t* font = assets_.load_standard_font(10);
            if (!font) font = &lv_font_montserrat_10;
            for (const auto codepoint : task->map_attribution_glyphs) {
                lv_font_glyph_dsc_t glyph{};
                if (!lv_font_get_glyph_dsc(font, &glyph, codepoint, 0) || glyph.is_placeholder) {
                    close_visual_tool();
                    survey_notice_ = "Attribution font missing a glyph";
                    survey_stage_ = SurveyStage::MapNotice;
                    show_survey();
                    return;
                }
            }
            auto* marker = lv_obj_create(overlay_);
            if (!marker) {
                close_visual_tool();
                survey_notice_ = "Could not create map marker";
                survey_stage_ = SurveyStage::MapNotice;
                show_survey();
                return;
            }
            lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
            lv_image_set_pivot(image, 0, 0);
            lv_image_set_scale(image, 128);
            lv_obj_set_pos(image, 5, 21);
            lv_obj_remove_style_all(marker);
            lv_obj_set_size(marker, 5, 5);
            lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(marker, lv_color_hex(0xff0000), 0);
            lv_obj_set_pos(marker,
                           5 + std::clamp(static_cast<int>(task->map_tile.pixel_x / 2) - 2, 0, 123),
                           21 + std::clamp(static_cast<int>(task->map_tile.pixel_y / 2) - 2, 0, 123));
            const std::string label = "Offline map z" + std::to_string(task->map_tile.zoom) +
                                      "\nReceiver GPS; map unverified\n" + task->map_attribution +
                                      "\nZ/C zoom  ESC back";
            lv_label_set_text(screen_test_label_, label.c_str());
            lv_obj_set_style_text_color(screen_test_label_, lv_color_white(), 0);
            lv_obj_set_style_text_font(screen_test_label_, font, 0);
            lv_obj_set_width(screen_test_label_, 175);
            lv_label_set_long_mode(screen_test_label_, LV_LABEL_LONG_WRAP);
            lv_obj_align(screen_test_label_, LV_ALIGN_RIGHT_MID, -4, 0);
            lv_obj_update_layout(overlay_);
            if (lv_obj_get_height(screen_test_label_) > 160) {
                close_visual_tool();
                survey_notice_ = "Attribution does not fit screen";
                survey_stage_ = SurveyStage::MapNotice;
                show_survey();
                return;
            }
            lv_obj_move_foreground(marker);
            lv_obj_move_foreground(screen_test_label_);
            survey_map_zoom_ = task->map_tile.zoom;
            return;
        }
        const uint32_t scale = std::min({256U,
                                         320U * 256U / task->result.width,
                                         150U * 256U / task->result.height});
        lv_image_set_scale(image, scale);
        lv_obj_center(image);
        lv_label_set_text(screen_test_label_, task->camera ? "S: save  ESC: close" : "ESC: back");
        lv_obj_set_style_text_color(screen_test_label_, lv_color_white(), 0);
        lv_obj_align(screen_test_label_, LV_ALIGN_BOTTOM_MID, 0, -2);
        lv_obj_move_foreground(screen_test_label_);
    }

    void start_network_query(const std::string& host, bool dns, bool https = false) {
        if (network_query_) {
            toast_->show("A network check is already running",
                         view::widgets::ToastTone::Warning,
                         3200);
            return;
        }

        auto state = std::make_shared<NetworkQueryState>();
        state->host = host;
        state->dns = dns;
        state->https = https;
        network_query_ = state;
        toast_->show_persistent(https ? "Checking HTTPS trust...\nTimeout: 10 seconds"
                                     : dns ? "Resolving hostname..." : "Checking host...");
        try {
            network_thread_ = std::thread([state] {
                std::string output;
                bool succeeded = false;
                try {
                    if (state->https) {
                        auto result = platform::check_https_certificate(state->host);
                        succeeded = result.verified;
                        output = std::move(result.detail);
                    } else if (state->dns) {
                        auto result = platform::dns_lookup(state->host);
                        succeeded = result.error.empty();
                        output = succeeded ? std::move(result.output) : std::move(result.error);
                    } else {
                        output = platform::ping_host(state->host);
                    }
                }
                catch (const std::exception& error) {
                    output = std::string("Network check failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = output;
                state->connected = succeeded;
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

    void start_internet_check() {
        if (network_query_) {
            toast_->show("A network check is already running",
                         view::widgets::ToastTone::Warning, 3200);
            return;
        }
        auto state = std::make_shared<NetworkQueryState>();
        state->internet = true;
        network_query_ = state;
        toast_->show_persistent("Checking Cloudflare HTTPS...\nTimeout: 10 seconds");
        try {
            network_thread_ = std::thread([state] {
                platform::InternetCheckResult result;
                try {
                    result = platform::check_internet();
                } catch (const std::exception& error) {
                    result.detail = std::string("Internet check failed: ") + error.what();
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->connected = result.connected;
                state->result = std::move(result.detail);
                state->complete = true;
            });
        } catch (const std::system_error& error) {
            network_query_.reset();
            toast_->show("Could not start internet check: " + error.code().message(),
                         view::widgets::ToastTone::Error, 4200);
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

        if (state->https) {
            toast_->show("HTTPS trust\n" + (result.empty() ? "No result" : result),
                         state->connected ? view::widgets::ToastTone::Success
                                          : view::widgets::ToastTone::Warning,
                         5200);
            return;
        }
        if (state->internet) {
            toast_->show("Internet check\n" + (result.empty() ? "No result" : result),
                         state->connected ? view::widgets::ToastTone::Success
                                          : view::widgets::ToastTone::Warning,
                         5200);
            return;
        }

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
            (state->dns ? state->connected
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
        std::string message = interfaces_ethernet_only_ ? (USE_DESKTOP ? "Host Ethernet " : "Ethernet ")
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
            toast_->show(ethernet_only ? (USE_DESKTOP ? "No host Ethernet adapters found"
                                                      : "No Ethernet adapter detected")
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
                   "\nUP/DOWN select Y compare ESC";
        toast_->show_persistent(message);
    }

    void compare_selected_arp_entry() {
        platform::ArpResult baseline;
        baseline.entries = arp_entries_;
        baseline.truncated = arp_truncated_;
        const auto comparison = platform::compare_arp_entry(
            baseline, arp_entries_[arp_selection_], platform::arp_entries());
        std::string message = "Cached ARP comparison\n";
        if (comparison.state == platform::ArpComparisonState::Same) {
            message += "MAC unchanged in snapshots\nNot proof of safety";
        } else if (comparison.state == platform::ArpComparisonState::Changed) {
            message += "MAC changed: " + file_display_text(comparison.current_mac, 17) +
                       "\nNot proof of an attack";
        } else {
            message += "Unknown: " + file_display_text(comparison.detail, 48);
        }
        message += "\nNo probes sent\nESC back";
        arp_compare_active_ = true;
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
        if (arp_compare_active_) {
            if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                arp_compare_active_ = false;
                show_selected_arp_entry();
            }
            return true;
        }
        if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
            arp_active_ = false;
            arp_entries_.clear();
            toast_->hide();
        } else if (key == 'y' || key == 'Y') {
            compare_selected_arp_entry();
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
        if (visual_tool_mode_ == VisualToolMode::Gallery ||
            visual_tool_mode_ == VisualToolMode::SurveyMap ||
            visual_tool_mode_ == VisualToolMode::SurveyQr) {
            lv_image_cache_drop(&gallery_image_);
            gallery_pixels_.clear();
            camera_jpeg_.clear();
        }
        visual_tool_mode_ = VisualToolMode::None;
    }

    void show_survey_plot() {
        const auto plot = platform::make_survey_plot(survey_timeline_);
        if (!plot.error.empty() || plot.gps_rows == 0) {
            toast_->show(plot.error.empty() ? "No receiver GPS in latest rows" : plot.error,
                         view::widgets::ToastTone::Warning, 4200);
            return;
        }
        visual_tool_mode_ = VisualToolMode::SurveyPlot;
        if (!create_visual_overlay()) {
            close_visual_tool();
            toast_->show("Could not open GPS plot", view::widgets::ToastTone::Error, 4200);
            return;
        }
        const auto colors = view::palette(view_model_.theme_id());
        lv_obj_set_style_bg_color(overlay_, colors.background, 0);
        const std::string title = "Receiver GPS " + std::to_string(plot.gps_rows) + " rows  N up";
        lv_label_set_text(screen_test_label_, title.c_str());
        lv_obj_set_style_text_color(screen_test_label_, colors.text, 0);
        lv_obj_align(screen_test_label_, LV_ALIGN_TOP_MID, 0, 5);
        auto* panel = lv_obj_create(overlay_);
        auto* footer = lv_label_create(overlay_);
        if (!panel || !footer) {
            close_visual_tool();
            toast_->show("Could not create GPS plot", view::widgets::ToastTone::Error, 4200);
            return;
        }
        lv_obj_remove_style_all(panel);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(panel, 296, 105);
        lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_set_style_bg_color(panel, colors.surface, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        for (std::size_t y = 0; y < platform::kSurveyPlotHeight; ++y) {
            for (std::size_t x = 0; x < platform::kSurveyPlotWidth; ++x) {
                if (!plot.cells[y * platform::kSurveyPlotWidth + x]) continue;
                auto* dot = lv_obj_create(panel);
                if (!dot) {
                    close_visual_tool();
                    toast_->show("Could not draw GPS plot", view::widgets::ToastTone::Error, 4200);
                    return;
                }
                lv_obj_remove_style_all(dot);
                lv_obj_set_size(dot, 6, 6);
                lv_obj_set_pos(dot, static_cast<int32_t>(12 + x * 14),
                               static_cast<int32_t>(11 + y * 11));
                lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(dot, colors.primary, 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            }
        }
        lv_label_set_text(footer, "Auto-scaled; no map   ESC back");
        lv_obj_set_style_text_color(footer, colors.text, 0);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -6);
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
        if (text_tool_mode_ == TextToolMode::Https && https_confirm_) {
            if (key == LV_KEY_ENTER) {
                const std::string host = text_tool_buffer_;
                cancel_text_tool();
                start_network_query(host, false, true);
            } else if (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE) {
                https_confirm_ = false;
                show_text_tool_prompt();
            }
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

            if (text_tool_mode_ == TextToolMode::Adsb) {
                const auto frame = platform::decode_adsb_frame(text_tool_buffer_);
                if (!frame.error.empty()) {
                    show_text_tool_prompt(view::widgets::ToastTone::Error, frame.error);
                    return true;
                }
                std::string message = "DF17: " + frame.aircraft_address +
                                      "  TC " + std::to_string(frame.type_code);
                if (!frame.callsign.empty()) message += "\nCallsign: " + frame.callsign;
                message += adsb_category_text(frame);
                if (frame.altitude_available) message += "\nBaro: " + std::to_string(frame.altitude_feet) + " ft";
                message += adsb_velocity_text(frame);
                message += adsb_status_text(frame);
                message += "\nCRC OK; identity unverified";
                cancel_text_tool();
                toast_->show(message, view::widgets::ToastTone::Default, 9000);
                return true;
            }

            if (text_tool_mode_ == TextToolMode::Https) {
                if (!platform::valid_https_host(text_tool_buffer_)) {
                    show_text_tool_prompt(view::widgets::ToastTone::Error,
                                          "Invalid host; max 44 chars");
                    return true;
                }
                https_confirm_ = true;
                std::string target = text_tool_buffer_.substr(0, 22);
                if (text_tool_buffer_.size() > 22)
                    target += "\n" + text_tool_buffer_.substr(22);
                toast_->show_persistent("HTTPS HEAD to:\n" + target +
                    "\nDNS and request may be sent\nENTER send  ESC edit");
                return true;
            }
            if (text_tool_mode_ != TextToolMode::Ping && text_tool_mode_ != TextToolMode::Dns) {
                show_text_tool_prompt(view::widgets::ToastTone::Error, "Unsupported input mode");
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
        const bool adsb_input = (key >= '0' && key <= '9') ||
                                (key >= 'a' && key <= 'f') || (key >= 'A' && key <= 'F');
        const bool allowed =
            text_tool_mode_ == TextToolMode::Calculator
                ? calculator_input
                : text_tool_mode_ == TextToolMode::Morse ? morse_input
                : text_tool_mode_ == TextToolMode::Adsb ? adsb_input : network_input;
        if (allowed) {
            const std::size_t limit =
                text_tool_mode_ == TextToolMode::Calculator
                    ? 64
                    : text_tool_mode_ == TextToolMode::Morse ? 48
                    : text_tool_mode_ == TextToolMode::Adsb ? 28
                    : text_tool_mode_ == TextToolMode::Https ? 44 : 253;
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
                if (gallery_task_->survey_map) {
                    show_survey();
                } else if (gallery_task_->camera) {
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
        if (visual_tool_mode_ == VisualToolMode::SurveyMap) {
            if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
                close_visual_tool();
                show_survey();
            } else if (!long_pressed &&
                       ((key == LV_KEY_LEFT || key == 'z') && survey_map_zoom_ > 0)) {
                const auto zoom = survey_map_zoom_ - 1;
                close_visual_tool();
                start_survey_map(zoom);
            } else if (!long_pressed &&
                       ((key == LV_KEY_RIGHT || key == 'c') && survey_map_zoom_ < 18)) {
                const auto zoom = survey_map_zoom_ + 1;
                close_visual_tool();
                start_survey_map(zoom);
            }
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::SurveyPlot ||
            visual_tool_mode_ == VisualToolMode::SurveyQr) {
            if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
                close_visual_tool();
                show_survey();
            }
            return true;
        }
        if (visual_tool_mode_ == VisualToolMode::Gallery) {
            if (gallery_camera_ && gallery_task_ && gallery_task_->saving) {
                return true;
            }
            if (!long_pressed && gallery_camera_ && (key == 's' || key == 'S')) {
                save_camera_photo();
                return true;
            }
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
        if (script_stage_ != ScriptStage::None) {
            return handle_script_key(key, long_pressed);
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
        if (lab_listener_stage_ != LabListenerStage::None) {
            return handle_lab_listener_key(key, long_pressed);
        }
        if (wireless_event_stage_ != WirelessEventStage::None) {
            return handle_wireless_event_key(key, long_pressed);
        }
        if (surveillance_stage_ != SurveillanceStage::None && key != platform::kKeyPrintScreen) {
            return handle_surveillance_key(key, long_pressed);
        }
        if (lab_confirm_active_) {
            if (!long_pressed && key == LV_KEY_ENTER) {
                lab_confirm_active_ = false;
                view_model_.set_authorized_lab_mode(true);
                toast_->show("Lab mode on for this session\nUnavailable tools remain disabled",
                             view::widgets::ToastTone::Warning, 4200);
            } else if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
                lab_confirm_active_ = false;
                toast_->hide();
            }
            return true;
        }
        if (wifi_monitor_confirm_) {
            if (!long_pressed && key == LV_KEY_ENTER) {
                wifi_monitor_confirm_ = false;
                wifi_monitor_active_ = true;
                wifi_monitor_has_baseline_ = false;
                wifi_monitor_compared_ = false;
                wifi_monitor_details_ = false;
                wifi_monitor_baseline_ = {};
                wifi_monitor_comparison_ = {};
                start_wifi_scan();
            } else if (!long_pressed && (key == LV_KEY_ESC || key == LV_KEY_BACKSPACE)) {
                wifi_monitor_confirm_ = false;
                toast_->hide();
            }
            return true;
        }
        if (adsb_receiver_active_ && key != platform::kKeyPrintScreen) {
            return handle_adsb_receiver_key(key, long_pressed);
        }
        if (readiness_active_ && key != platform::kKeyPrintScreen) {
            return handle_readiness_key(key, long_pressed);
        }
        if (note_stage_ != NoteStage::None && key != platform::kKeyPrintScreen) {
            return handle_field_notes_key(key, long_pressed);
        }
        if (incident_checklist_active_) {
            return handle_incident_checklist_key(key, long_pressed);
        }
        if (security_log_stage_ != SecurityLogStage::None && key != platform::kKeyPrintScreen) {
            return handle_security_logs_key(key, long_pressed);
        }
        if (port_stage_ != PortStage::None && key != platform::kKeyPrintScreen) {
            return handle_port_key(key, long_pressed);
        }
        if (ap_signal_test_active_ && key != platform::kKeyPrintScreen) {
            return handle_ap_signal_test_key(key, long_pressed);
        }
        if (survey_stage_ != SurveyStage::None && key != platform::kKeyPrintScreen) {
            if (visual_tool_mode_ == VisualToolMode::SurveyPlot ||
                visual_tool_mode_ == VisualToolMode::SurveyMap ||
                visual_tool_mode_ == VisualToolMode::SurveyQr) {
                return handle_visual_tool_key(key, long_pressed);
            }
            return handle_survey_key(key, long_pressed);
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

        if (favorites_active_) return handle_favorites_key(key, long_pressed);
        if (key == '0' && !long_pressed) {
            favorites_active_ = true;
            favorite_selection_ = 0;
            favorite_notice_.clear();
            show_favorites();
            return true;
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
                            case 17:
                                start_file_browser(FileMode::Scripts);
                                break;
                            case 18:
                                incident_checklist_active_ = true;
                                field_guide_active_ = false;
                                incident_checklist_page_ = 0;
                                show_incident_checklist();
                                break;
                            case 19:
                                start_security_log_history_action(SecurityLogAction::List);
                                break;
                            case 20:
                                start_text_tool(TextToolMode::Adsb);
                                break;
                            case 21:
                                if (USE_DESKTOP) {
                                    toast_->show("ADS-B receiver requires a device",
                                                 view::widgets::ToastTone::Warning, 3200);
                                } else {
                                    adsb_receiver_active_ = true;
                                    adsb_receiver_confirm_ = true;
                                    adsb_receiver_record_ = false;
                                    adsb_receiver_gps_ = false;
                                    show_adsb_receiver();
                                }
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
                        } else if (view_model_.tool_selection() == 10) {
                            if (view_model_.authorized_lab_mode()) {
                                view_model_.set_authorized_lab_mode(false);
                                toast_->show("Authorized lab mode off",
                                             view::widgets::ToastTone::Default, 3200);
                            } else {
                                lab_confirm_active_ = true;
                                toast_->show_persistent(
                                    "Authorized lab only\nCheck for nearby devices.\n"
                                    "Radio may reach beyond lab.\nENTER enable  ESC cancel",
                                    view::widgets::ToastTone::Warning);
                            }
                        } else if (view_model_.tool_selection() == 11) {
                            readiness_page_ = 0;
                            start_readiness();
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
                                start_port_scan();
                                break;
                            case 5:
                                show_network_interfaces(true);
                                break;
                            case 6:
                                show_arp_table();
                                break;
                            case 7:
                                save_diagnostics();
                                break;
                            case 8:
                                start_internet_check();
                                break;
                            case 9:
                                start_port_scan(true);
                                break;
                            case 10:
                                if (USE_DESKTOP) {
                                    toast_->show("Lab listener requires a supported device",
                                                 view::widgets::ToastTone::Warning, 3200);
                                } else if (!view_model_.authorized_lab_mode()) {
                                    toast_->show("Enable Authorized lab mode in System",
                                                 view::widgets::ToastTone::Warning, 3200);
                                } else {
                                    lab_listener_address_.clear();
                                    lab_listener_display_.clear();
                                    lab_listener_stage_ = LabListenerStage::Address;
                                    show_lab_listener();
                                }
                                break;
                            case 11:
                                if (USE_DESKTOP) {
                                    toast_->show("HTTPS trust requires a supported device",
                                                 view::widgets::ToastTone::Warning, 3200);
                                } else {
                                    start_text_tool(TextToolMode::Https);
                                }
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
                        if (selection == 5 || selection == 12) {
                            if (USE_DESKTOP) {
                                toast_->show("Wireless monitor requires a supported device",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else {
                                const char* iface = std::getenv("POSEIDON_WIFI_MONITOR_INTERFACE");
                                wireless_event_kind_ = selection == 5
                                    ? platform::WirelessEventKind::Deauthentication
                                    : platform::WirelessEventKind::ProbeRequest;
                                wireless_event_interface_.clear();
                                wireless_event_setup_error_.clear();
                                if (iface && *iface) {
                                    if (platform::valid_monitor_interface_name(iface))
                                        wireless_event_interface_ = iface;
                                    else
                                        wireless_event_setup_error_ = "Invalid configured interface";
                                }
                                wireless_event_display_.clear();
                                wireless_event_stage_ = WirelessEventStage::Interface;
                                show_wireless_event_monitor();
                            }
                            return true;
                        }
                        if (selection == 9) {
                            if (USE_DESKTOP) {
                                toast_->show("AP signal test requires a supported device",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else {
                                start_ap_signal_test();
                            }
                            return true;
                        }
                        if (selection == 14) {
                            if (USE_DESKTOP) {
                                toast_->show("Channel survey requires a supported device",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else {
                                start_wifi_scan(true);
                            }
                            return true;
                        }
                        if (selection == 15) {
                            survey_stage_ = SurveyStage::Home;
                            show_survey();
                            return true;
                        }
                        if (selection == 16) {
                            start_wifi_scan();
                            return true;
                        }
                        if (selection == 17) {
                            if (USE_DESKTOP) {
                                toast_->show("AP monitor requires a supported device",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else if (wifi_task_) {
                                toast_->show("Previous Wi-Fi scan is stopping",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else {
                                wifi_monitor_confirm_ = true;
                                toast_->show_persistent(
                                    "AP monitor requests Wi-Fi scans.\n"
                                    "Scans may transmit requests.\nENTER start  ESC cancel");
                            }
                            return true;
                        }
                        if (selection == 19) {
                            if (USE_DESKTOP) {
                                toast_->show("Camera hints require a supported device",
                                             view::widgets::ToastTone::Warning, 3200);
                            } else {
                                surveillance_result_ = {};
                                surveillance_stage_ = SurveillanceStage::Confirm;
                                show_surveillance();
                            }
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
                            case 4:
                                if (USE_DESKTOP) {
                                    toast_->show("Tracker scan requires a supported device",
                                                 view::widgets::ToastTone::Warning, 3200);
                                } else {
                                    gatt_tracker_mode_ = true;
                                    gatt_stage_ = GattStage::ConfirmTracker;
                                    show_gatt();
                                }
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
                        } else if (view_model_.tool_selection() == 4) {
                            show_ir_receiver_check();
                        } else {
                            show_feature_status(platform::infrared_features(),
                                                view_model_.tool_selection());
                        }
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Expansion) {
                        if (view_model_.tool_selection() == 0) {
                            start_file_browser(FileMode::I2c);
                            return true;
                        }
                        if (view_model_.tool_selection() == 2) {
                            show_expansion_inventory();
                            return true;
                        }
#if USE_DESKTOP
                        if (view_model_.tool_selection() == 7) {
                            usb_sim_active_ = true;
                            usb_sim_connected_ = false;
                            usb_sim_confirm_ = false;
                            usb_sim_text_.clear();
                            show_usb_sim_prompt();
                            return true;
                        }
#else
                        if (view_model_.tool_selection() == 7) {
                            show_cap_nfc_readiness();
                            return true;
                        }
#endif
                        show_feature_status(platform::expansion_features(),
                                            view_model_.tool_selection());
                        return true;
                    }
                    if (view_model_.current_page() == model::AppPage::Media) {
                        if (view_model_.tool_selection() == 0) {
#if USE_DESKTOP
                            toast_->show("Camera is unavailable\nin the desktop simulator",
                                         view::widgets::ToastTone::Warning, 4200);
#else
                            camera_confirm_ = true;
                            toast_->show_persistent("Camera snapshot\nImage stays in memory\nENTER capture  ESC back");
#endif
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
    AssetManager& assets_;
    ThemePersistence& theme_persistence_;
    BootSplash& boot_splash_;
    platform::UiSound ui_sound_;
    std::unique_ptr<view::widgets::Toast> toast_;
    std::unique_ptr<view::widgets::HelpPopup> help_popup_;
    std::string text_tool_buffer_;
    TextToolMode text_tool_mode_{TextToolMode::None};
    bool https_confirm_{false};
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
    std::shared_ptr<FileHashTask> file_hash_task_;
    std::thread file_hash_thread_;
    bool file_hash_showing_{false};
    ScriptStage script_stage_{ScriptStage::None};
    std::shared_ptr<ScriptTask> script_task_;
    std::thread script_thread_;
    platform::LocalScript script_;
    std::string script_output_;
    std::size_t script_page_{0};
    bool gallery_loading_{false};
    bool gallery_camera_{false};
    uint32_t survey_map_zoom_{14};
    bool camera_confirm_{false};
    std::shared_ptr<GalleryQueryState> gallery_task_;
    std::thread gallery_thread_;
    std::vector<uint8_t> gallery_pixels_;
    std::vector<uint8_t> camera_jpeg_;
    lv_image_dsc_t gallery_image_{};
    std::shared_ptr<NetworkQueryState> network_query_;
    std::thread network_thread_;
    bool interfaces_active_{false};
    bool interfaces_ethernet_only_{false};
    std::vector<platform::NetworkInterface> interfaces_;
    std::size_t interface_selection_{0};
    bool arp_active_{false};
    bool arp_compare_active_{false};
    bool arp_truncated_{false};
    std::vector<platform::ArpEntry> arp_entries_;
    std::size_t arp_selection_{0};
    PortStage port_stage_{PortStage::None};
    PortStage port_return_stage_{PortStage::None};
    std::string port_notice_;
    std::size_t port_notice_page_{0};
    std::shared_ptr<PortTask> port_task_;
    std::thread port_thread_;
    std::string port_target_;
    std::string port_spec_;
    std::string inventory_policy_text_;
    platform::PortTargets port_targets_;
    std::vector<uint16_t> port_ports_;
    platform::PortScanResult port_result_;
    platform::NetworkInventory inventory_result_;
    platform::NetworkInventory inventory_baseline_;
    std::string inventory_baseline_name_;
    std::string inventory_exclusions_;
    std::string inventory_edit_buffer_;
    std::vector<std::string> inventory_history_;
    std::size_t inventory_history_selection_{0};
    bool nmap_os_{false};
    std::string nmap_detail_;
    std::string inventory_detail_title_;
    std::size_t nmap_detail_page_{0};
    bool inventory_mode_{false};
    std::size_t port_selection_{0};
    bool port_tcp_{true};
    bool port_udp_{false};
    std::chrono::steady_clock::time_point port_next_update_{};
    std::shared_ptr<WifiTask> wifi_task_;
    std::thread wifi_thread_;
    SurveyStage survey_stage_{SurveyStage::None};
    bool survey_quality_{false};
    std::unique_ptr<platform::SurveySession> survey_session_;
    std::shared_ptr<SurveyFileTask> survey_file_task_;
    std::thread survey_file_thread_;
    std::vector<std::string> survey_files_;
    platform::SurveyTimeline survey_timeline_;
    std::size_t survey_timeline_selection_{0};
    std::vector<std::size_t> survey_watch_indices_;
    bool survey_watch_filter_{false};
    std::size_t survey_selection_{0};
    std::string survey_notice_;
    std::chrono::steady_clock::time_point survey_next_update_{};
    SurveillanceStage surveillance_stage_{SurveillanceStage::None};
    std::shared_ptr<SurveillanceTask> surveillance_task_;
    std::thread surveillance_thread_;
    platform::SurveillanceScanResult surveillance_result_;
    std::size_t surveillance_selection_{0};
    platform::WifiScanResult wifi_result_;
    std::size_t wifi_selection_{0};
    bool wifi_active_{false};
    bool wifi_scanning_{false};
    bool wifi_channel_survey_active_{false};
    bool wifi_monitor_confirm_{false};
    bool wifi_monitor_active_{false};
    bool wifi_monitor_has_baseline_{false};
    bool wifi_monitor_compared_{false};
    bool wifi_monitor_details_{false};
    platform::WifiScanResult wifi_monitor_baseline_;
    platform::WifiMonitorComparison wifi_monitor_comparison_;
    std::string wifi_monitor_detail_;
    std::size_t wifi_monitor_page_{0};
    bool lab_confirm_active_{false};
    LabListenerStage lab_listener_stage_{LabListenerStage::None};
    std::unique_ptr<platform::LabHoneypot> lab_listener_;
    std::string lab_listener_address_;
    std::string lab_listener_display_;
    bool lab_listener_stop_requested_{false};
    bool lab_listener_log_info_{false};
    bool lab_export_confirm_{false};
    bool lab_delete_confirm_{false};
    bool lab_log_deleted_{false};
    std::shared_ptr<LabExportTask> lab_export_task_;
    std::thread lab_export_thread_;
    std::string lab_listener_notice_;
    WirelessEventStage wireless_event_stage_{WirelessEventStage::None};
    std::unique_ptr<platform::WirelessEventMonitor> wireless_event_monitor_;
    platform::WirelessEventKind wireless_event_kind_{platform::WirelessEventKind::Deauthentication};
    std::string wireless_event_interface_;
    std::string wireless_event_channel_text_;
    unsigned wireless_event_channel_{1};
    std::string wireless_event_setup_error_;
    std::string wireless_event_display_;
    std::string wireless_event_notice_;
    bool wireless_event_stop_requested_{false};
    bool wireless_event_log_info_{false};
    bool wireless_event_export_confirm_{false};
    bool wireless_event_delete_confirm_{false};
    bool wireless_event_log_deleted_{false};
    SecurityLogStage security_log_stage_{SecurityLogStage::None};
    std::shared_ptr<SecurityLogTask> security_log_task_;
    std::thread security_log_thread_;
    std::vector<platform::SecurityLogEntry> security_logs_;
    std::vector<std::string> security_export_files_;
    bool security_log_export_mode_{false};
    std::size_t security_log_selection_{0};
    std::string security_log_notice_;
    bool incident_checklist_active_{false};
    bool field_guide_active_{false};
    std::size_t incident_checklist_page_{0};
    SettingsBackupStage settings_backup_stage_{SettingsBackupStage::None};
    std::shared_ptr<SettingsBackupTask> settings_backup_task_;
    std::thread settings_backup_thread_;
    std::string settings_backup_notice_;
    NoteStage note_stage_{NoteStage::None};
    NoteStage note_notice_return_{NoteStage::List};
    std::shared_ptr<NoteTask> note_task_;
    std::thread note_thread_;
    std::vector<std::string> note_files_;
    std::size_t note_selection_{0};
    std::size_t note_page_{0};
    std::string note_buffer_;
    std::string note_notice_;
    platform::FieldNote note_view_;
    bool readiness_active_{false};
    std::size_t readiness_page_{0};
    platform::ReadinessReport readiness_report_;
    std::string readiness_error_;
    std::shared_ptr<ReadinessTask> readiness_task_;
    std::thread readiness_thread_;
    bool adsb_receiver_active_{false};
    bool adsb_receiver_confirm_{false};
    bool adsb_receiver_record_{false};
    bool adsb_receiver_gps_{false};
    bool adsb_receiver_stopping_{false};
    bool adsb_aircraft_view_{false};
    std::string adsb_aircraft_selected_;
    bool favorites_active_{false};
    std::size_t favorite_selection_{0};
    std::string favorite_notice_;
    bool accessory_profiles_active_{false};
    std::size_t accessory_profile_selection_{0};
    std::string accessory_profile_notice_;
    std::string adsb_receiver_display_;
    std::unique_ptr<platform::AdsbReceiver> adsb_receiver_;
    bool ap_signal_test_active_{false};
    std::vector<int> ap_signal_samples_;
    WifiStage wifi_stage_{WifiStage::Results};
    platform::WifiConnectMode wifi_connect_mode_{platform::WifiConnectMode::Saved};
    std::string wifi_password_;
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
    bool gatt_tracker_mode_{false};
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
