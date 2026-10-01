// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_panel_settings.h"

#include "ui_ams_device_operations_overlay.h"
#include "ui_callback_helpers.h"
#include "ui_change_host_modal.h"
#include "ui_debug_bundle_modal.h"
#include "ui_emergency_stop.h"
#include "ui_event_safety.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#if HELIX_HAS_PLUGINS
#include "plugins_overlay.h"
#endif
#include "ui_nav_manager.h"
#include "ui_overlay_network_settings.h"
#include "ui_overlay_performance.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_panel_history_dashboard.h"
#include "ui_panel_memory_stats.h"
#include "ui_panel_power.h"
#include "ui_printer_list_overlay.h"
#include "ui_settings_about.h"
#include "ui_settings_appearance.h"
#include "ui_settings_connection.h"
#include "ui_settings_display.h"
#include "ui_settings_hardware.h"
#include "ui_settings_hardware_health.h"
#include "ui_settings_help.h"
#include "ui_settings_language_time.h"
#include "ui_settings_printing.h"
#include "ui_settings_safety.h"
#include "ui_settings_system.h"
#include "ui_settings_touch.h"
#include "ui_settings_updates.h"
#if HELIX_HAS_LABEL_PRINTER
#include "ui_settings_label_printer.h"
#endif
#include "ui_settings_fans.h"
#include "ui_settings_led.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_material_temps.h"
#include "ui_settings_security.h"
#include "ui_settings_sensors.h"
#include "ui_settings_sound.h"
#include "ui_settings_telemetry_data.h"
#include "ui_severity_card.h"
#include "ui_snake_game.h"
#include "ui_spoolman_overlay.h"
#include "ui_toast_manager.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard_hardware_selector.h"

#include "app_globals.h"
#include "audio_settings_manager.h"
#include "config.h"
#include "device_display_name.h"
#include "display_manager.h"
#include "display_settings_manager.h"
#include "ethernet_manager.h"
#include "filament_sensor_manager.h"
#include "format_utils.h"
#include "hardware_validator.h"
#include "helix_version.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "input_settings_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_manager.h"
#include "page_scroll_auto_inject.h"
#include "platform_info.h"
#include "printer_hardware.h"
#include "printer_state.h"
#include "runtime_config.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "settings_root_status.h"
#include "sound_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"
#include "theme_manager.h"
#include "ui/ui_lazy_panel_helper.h"
#include "wifi_manager.h"
#include "wizard_config_paths.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

using namespace helix;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

SettingsPanel::SettingsPanel(PrinterState& printer_state, IMoonrakerAPI* api)
    : PanelBase(printer_state, api) {
    spdlog::trace("[{}] Constructor", get_name());
}

SettingsPanel::~SettingsPanel() {
    // Applying [L041]: deinit_subjects() as first line in destructor
    deinit_subjects();

    // Note: Klipper/Moonraker/OS version observers bound declaratively in XML
    if (lv_is_initialized()) {
        // Unregister overlay callbacks to prevent dangling 'this' in callbacks
        auto& nav = NavigationManager::instance();
        if (factory_reset_dialog_) {
            nav.unregister_overlay_close_callback(factory_reset_dialog_);
        }
    }
    // Note: Don't log here - spdlog may be destroyed during static destruction
}

// ============================================================================
// PANELBASE IMPLEMENTATION
// ============================================================================

// Static callback for XML event_cb (registered with lv_xml_register_event_cb)
static void on_completion_alert_dropdown_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    auto mode = static_cast<CompletionAlertMode>(index);
    spdlog::info("[SettingsPanel] Completion alert changed: {} ({})", index,
                 index == 0 ? "Off" : (index == 1 ? "Notification" : "Alert"));
    AudioSettingsManager::instance().set_completion_alert_mode(mode);
}

// Static callback for cancel escalation timeout dropdown
static void on_cancel_escalation_timeout_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    static constexpr int TIMEOUT_VALUES[] = {15, 30, 60, 120};
    int seconds = TIMEOUT_VALUES[std::max(0, std::min(3, index))];
    spdlog::info("[SettingsPanel] Cancel escalation timeout changed: {}s (index {})", seconds,
                 index);
    SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(seconds);
}

// Static callback for log level dropdown
static void on_log_level_changed(lv_event_t* e) {
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    spdlog::info("[SettingsPanel] Log level changed: index {}", index);
    SystemSettingsManager::instance().set_log_level_by_index(index);
}

// Touch & input setting callbacks (Settings → Touch & Input).
// The slider rows nest as: row > slider_container > slider, so the row is
// the slider's grandparent. Used by both drag-time syncs (here) and the
// activation-time refresh in TouchSettingsOverlay::init_input_sliders.
static void sync_slider_value_label(lv_obj_t* slider, int value) {
    lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(slider));
    if (!row)
        return;
    if (lv_obj_t* value_label = lv_obj_find_by_name(row, "value_label")) {
        lv_label_set_text_fmt(value_label, "%d", value);
    }
}

static void on_debug_touches_changed(lv_event_t* e) {
    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    InputSettingsManager::instance().set_debug_touches(lv_obj_has_state(toggle, LV_STATE_CHECKED));
}

static void on_scroll_limit_changed(lv_event_t* e) {
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    sync_slider_value_label(slider, value);
    InputSettingsManager::instance().set_scroll_limit(value);
    get_global_settings_panel().show_restart_prompt();
}

static void on_long_press_time_changed(lv_event_t* e) {
    lv_obj_t* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    sync_slider_value_label(slider, value);
    InputSettingsManager::instance().set_long_press_time(value);
    // No restart prompt — set_long_press_time live-applies via lv_indev_set_long_press_time.
}

static void on_home_edit_mode_changed(lv_event_t* e) {
    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    InputSettingsManager::instance().set_home_edit_mode_enabled(
        lv_obj_has_state(toggle, LV_STATE_CHECKED));
    // No restart prompt — should_suppress_edit_mode checks this live.
}

static void on_scroll_guard_changed(lv_event_t* e) {
    lv_obj_t* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    InputSettingsManager::instance().set_scroll_guard(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    get_global_settings_panel().show_restart_prompt();
}

static void on_system_keyboard_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_system_keyboard_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[SettingsPanel] System keyboard toggled: {}", enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_use_system_keyboard(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_hide_keyboard_with_hardware_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_hide_keyboard_with_hardware_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[SettingsPanel] Hide keyboard with hardware keyboard toggled: {}",
                 enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_hide_keyboard_with_hardware(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_keep_navbar_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_keep_navbar_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[SettingsPanel] Keep navbar toggled: {}", enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_keep_navbar_visible(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

static void on_page_scroll_buttons_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_page_scroll_buttons_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    spdlog::info("[SettingsPanel] Page scroll buttons toggled: {}", enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_page_scroll_buttons(enabled);
    // Apply immediately to the current screen — this callback is the authoritative
    // user-toggle signal (a subject observer can't be used; see PageScrollAutoInject::init).
    helix::ui::PageScrollAutoInject::instance().on_setting_toggled(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

// Note: Sensors overlay callbacks are now in SensorSettingsOverlay class
// See ui_settings_sensors.cpp
// Note: Macro Buttons overlay callbacks are now in MacroButtonsOverlay class
// See ui_settings_macro_buttons.cpp

// ============================================================================
// MODAL DIALOG STATIC CALLBACKS (XML event_cb)
// ============================================================================

static void on_factory_reset_confirm(lv_event_t* e) {
    (void)e;
    spdlog::info("[SettingsPanel] User confirmed factory reset");
    auto& panel = get_global_settings_panel();
    panel.perform_factory_reset();
}

static void on_factory_reset_cancel(lv_event_t* e) {
    (void)e;
    spdlog::info("[SettingsPanel] User cancelled factory reset");
    auto& panel = get_global_settings_panel();
    if (panel.factory_reset_dialog_) {
        NavigationManager::instance().go_back(); // Animation + callback will handle cleanup
    }
}

void SettingsPanel::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // Note: LED config loading moved to MoonrakerManager::create_api() for centralized init

    // Initialize info row subjects that remain in SettingsPanel
    UI_MANAGED_SUBJECT_STRING(printer_host_value_subject_, printer_host_value_buf_, "\xe2\x80\x94",
                              "printer_host_value", subjects_);

    // LED chip selection (no subject needed - chips handle their own state)

    // Initialize visibility subjects (controls which settings are shown)
    // Touch calibration: show on touch displays (non-SDL) OR in test mode (for testing on desktop)
#ifdef HELIX_DISPLAY_SDL
    bool show_touch_cal = get_runtime_config()->is_test_mode();
#else
    // supports_ (any real touch panel), NOT needs_ (auto-fire the first-run
    // wizard). The auto-fire heuristic keys off controller name and ABS range,
    // and neither can see a touch panel mounted 90° from the display — so
    // gating the manual entry point on it left those users with no way in
    // (prestonbrown/helixscreen#1259).
    DisplayManager* dm = DisplayManager::instance();
    bool show_touch_cal = dm && dm->supports_touch_calibration();
#endif
    UI_MANAGED_SUBJECT_INT(show_touch_calibration_subject_, show_touch_cal ? 1 : 0,
                           "show_touch_calibration", subjects_);

    // Note: show_beta_features subject is initialized globally in app_globals.cpp

    // Platform visibility subjects — hidden on Android where OS manages these
    bool on_android = helix::is_android_platform();

    // Task 13: un-hidden on ESP32 now that WifiBackend over esp_wifi
    // (wifi_backend_esp.cpp) backs wifi_manager.h for real — Ethernet stays
    // out of scope (ethernet_manager.h still resolves to the
    // helixapp_platform_stubs.cpp seam; no ESP32 wired-network HIL exists).
    bool show_network_settings = !on_android;
    UI_MANAGED_SUBJECT_INT(show_network_settings_subject_, show_network_settings ? 1 : 0,
                           "show_network_settings", subjects_);

    // Update checker runs on all platforms — on Android, "Install Update"
    // redirects to the Play Store instead of self-updating.
    //
    // Checking and installing are gated SEPARATELY. Only a firmware opt-out hides
    // the "Check for Updates" row, because checking is a network fetch that a
    // read-only install tree cannot fail; an install tree we cannot write hides
    // only "Install Update" and adds a notice saying so. Gating both on one
    // predicate is what made a false negative unrecoverable — the whole section
    // disappeared, so nothing could tell the user an update existed or what to do
    // about it.
    bool externally_managed = updates_externally_managed();
    bool install_suppressed = update_install_suppressed();
    UI_MANAGED_SUBJECT_INT(show_update_settings_subject_, update_checks_suppressed() ? 0 : 1,
                           "show_update_settings", subjects_);

    UI_MANAGED_SUBJECT_INT(updates_firmware_managed_subject_, externally_managed ? 1 : 0,
                           "updates_firmware_managed", subjects_);

    UI_MANAGED_SUBJECT_INT(updates_unavailable_subject_,
                           (install_suppressed && !externally_managed) ? 1 : 0,
                           "updates_unavailable", subjects_);

    // 0 until Application::init_plugins loads at least one plugin, so the row
    // stays hidden until there is something to show
    UI_MANAGED_SUBJECT_INT(plugins_available_subject_, 0, "settings_plugins_available", subjects_);

    // Touch calibration status, filled by refresh_status_lines().
    UI_MANAGED_SUBJECT_STRING(touch_cal_status_subject_, touch_cal_status_buf_, "",
                              "touch_cal_status", subjects_);

    // Live status line under each stateful root row; refresh_status_lines()
    // fills these in, first from setup() and then on every return to the root.
    UI_MANAGED_SUBJECT_STRING(settings_status_display_subject_, settings_status_display_buf_, "",
                              "settings_status_display", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_appearance_subject_, settings_status_appearance_buf_,
                              "", "settings_status_appearance", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_sound_subject_, settings_status_sound_buf_, "",
                              "settings_status_sound", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_devices_subject_, settings_status_devices_buf_, "",
                              "settings_status_devices", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_connection_subject_, settings_status_connection_buf_,
                              "", "settings_status_connection", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_language_time_subject_,
                              settings_status_language_time_buf_, "",
                              "settings_status_language_time", subjects_);
    UI_MANAGED_SUBJECT_STRING(settings_status_updates_subject_, settings_status_updates_buf_, "",
                              "settings_status_updates", subjects_);

    // Register XML event callbacks for dropdowns, toggles, and action rows
    register_xml_callbacks({
        // Dropdowns
        {"on_completion_alert_changed", on_completion_alert_dropdown_changed},
        {"on_log_level_changed", on_log_level_changed},
        {"on_debug_touches_changed", on_debug_touches_changed},
        {"on_scroll_limit_changed", on_scroll_limit_changed},
        {"on_long_press_time_changed", on_long_press_time_changed},
        {"on_home_edit_mode_changed", on_home_edit_mode_changed},
        {"on_scroll_guard_changed", on_scroll_guard_changed},
        {"on_system_keyboard_changed", on_system_keyboard_changed},
        {"on_hide_keyboard_with_hardware_changed", on_hide_keyboard_with_hardware_changed},
        {"on_keep_navbar_changed", on_keep_navbar_changed},
        {"on_page_scroll_buttons_changed", on_page_scroll_buttons_changed},

        // Toggle switches
        {"on_led_settings_clicked", on_led_settings_clicked},
        // Note: on_retraction_row_clicked is registered by RetractionSettingsOverlay
        {"on_security_clicked", on_security_clicked},
        {"on_estop_confirm_changed", on_estop_confirm_changed},
        {"on_cancel_escalation_changed", on_cancel_escalation_changed},
        {"on_cancel_escalation_timeout_changed", on_cancel_escalation_timeout_changed},
        {"on_telemetry_changed", SettingsPanel::on_telemetry_changed},
        {"on_telemetry_view_data", SettingsPanel::on_telemetry_view_data},

        // Action rows
        {"on_printers_clicked", on_printers_clicked},
        // Note: on_printer_image_clicked moved to PrinterManagerOverlay
        {"on_filament_sensors_clicked", on_filament_sensors_clicked},
        {"on_fans_settings_clicked", on_fans_settings_clicked},
        {"on_timelapse_settings_clicked", on_timelapse_settings_clicked},
    });

    // Category navigation callbacks (open sub-panel overlays from top-level)
    register_xml_callbacks({
        {"on_display_clicked", on_display_clicked},
        {"on_appearance_clicked", on_appearance_clicked},
        {"on_sound_clicked", on_sound_clicked},
        {"on_language_time_clicked", on_language_time_clicked},
        {"on_printing_clicked", on_printing_clicked},
        {"on_devices_clicked", on_devices_clicked},
        {"on_safety_clicked", on_safety_clicked},
        {"on_system_clicked", on_system_clicked},
        {"on_help_clicked", on_help_clicked},
        {"on_touch_input_clicked", on_touch_input_clicked},
        {"on_connection_clicked", on_connection_clicked},
        {"on_updates_clicked", on_updates_clicked},
        {"on_plugins_clicked", on_plugins_clicked},
    });

    // Register sub-panel overlay callbacks (must happen before XML parsing)
    helix::settings::get_display_settings_overlay().register_callbacks();
    helix::settings::get_appearance_settings_overlay().register_callbacks();
    helix::settings::get_sound_settings_overlay().register_callbacks();
    helix::settings::get_language_time_settings_overlay().register_callbacks();
    helix::settings::get_printing_settings_overlay().register_callbacks();
    helix::settings::get_hardware_settings_overlay().register_callbacks();
    helix::settings::get_safety_settings_overlay().register_callbacks();
    helix::settings::get_system_settings_overlay().register_callbacks();
    helix::settings::get_help_settings_overlay().register_callbacks();
    helix::settings::get_touch_settings_overlay().register_callbacks();
    helix::settings::get_connection_settings_overlay().register_callbacks();
    helix::settings::get_updates_settings_overlay().register_callbacks();

    // Note: Sensors overlay callbacks are now handled by SensorSettingsOverlay
    // See ui_settings_sensors.h
    helix::settings::get_sensor_settings_overlay().register_callbacks();

    // Note: Fan Settings overlay callbacks are now handled by FanSettingsOverlay
    helix::settings::get_fan_settings_overlay().register_callbacks();

    // Settings action rows and overlay navigation callbacks
    register_xml_callbacks({
        {"on_ams_settings_clicked", on_ams_settings_clicked},
        {"on_spoolman_settings_clicked", on_spoolman_settings_clicked},
        {"on_macro_buttons_clicked", on_macro_buttons_clicked},
        {"on_machine_limits_clicked", on_machine_limits_clicked},
        {"on_network_clicked", on_network_clicked},
        {"on_power_devices_clicked", on_power_devices_clicked},
        {"on_factory_reset_clicked", on_factory_reset_clicked},
        {"on_hardware_health_clicked", on_hardware_health_clicked},
        {"on_system_performance_clicked", on_system_performance_clicked},

        // Overlay callbacks
        {"on_restart_later_clicked", on_restart_later_clicked},
        {"on_restart_now_clicked", on_restart_now_clicked},

        // Modal dialog callbacks
        {"on_factory_reset_confirm", on_factory_reset_confirm},
        {"on_factory_reset_cancel", on_factory_reset_cancel},
        {"on_header_back_clicked", on_header_back_clicked},
        // Note: on_brightness_changed is now handled by DisplaySettingsOverlay
    });

    // Note: BedMeshPanel subjects are initialized in main.cpp during startup

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SettingsPanel::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[{}] Deinitializing subjects", get_name());

    // Expire any in-flight Ethernet probe first: get_info_async()'s deferred
    // write targets settings_status_connection_subject_ below, which
    // subjects_.deinit_all() is about to tear down.
    lifetime_.invalidate();

    // Deinit all subjects via SubjectManager (handles 7 string subjects)
    subjects_.deinit_all();

    subjects_initialized_ = false;
    spdlog::debug("[{}] Subjects deinitialized", get_name());
}

void SettingsPanel::setup(lv_obj_t* panel, lv_obj_t* parent_screen) {
    // Call base class to store panel_ and parent_screen_
    PanelBase::setup(panel, parent_screen);

    if (!panel_) {
        spdlog::error("[{}] NULL panel", get_name());
        return;
    }

    populate_info_rows();

    spdlog::debug("[{}] Setup complete", get_name());
}

void SettingsPanel::on_activate() {
    PanelBase::on_activate();
    refresh_status_lines();
}

// ============================================================================
// SETUP HELPERS
// ============================================================================

void SettingsPanel::populate_info_rows() {
    // Printer host description: bound declaratively in settings_connection_overlay.xml
    // via bind_description="printer_host_value". Seed the subject from config so the
    // first paint shows the current host:port (otherwise it's the em-dash default
    // until ChangeHostModal fires its completion callback).
    Config* config = Config::get_instance();

    std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
    if (!host.empty()) {
        int port = config->get<int>(config->df() + "moonraker_port", 7125);
        std::string host_display = host + ":" + std::to_string(port);
        lv_subject_copy_string(&printer_host_value_subject_, host_display.c_str());
    }
}

namespace {
// A subject owned by an overlay not yet created (e.g. update_new_version,
// registered by the Updates overlay) may not exist; the caller's fallback
// stands in for it, matching the formatter's neutral input.
int status_int_subject(const char* name, int fallback) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    return s ? lv_subject_get_int(s) : fallback;
}
std::string status_string_subject(const char* name, const char* fallback) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    return s ? std::string(lv_subject_get_string(s)) : std::string(fallback);
}
} // namespace

void SettingsPanel::refresh_status_lines() {
    using namespace helix::settings::status;

    // Formatted here rather than once at init, so it is in the language of the
    // latest return to the settings root.
    Config* config = Config::get_instance();
    const bool is_calibrated = config->get<bool>(config->df() + "input/calibration/valid", false);
    lv_subject_copy_string(&touch_cal_status_subject_,
                           is_calibrated ? lv_tr("Calibrated") : lv_tr("Not calibrated"));

    lv_subject_copy_string(&settings_status_display_subject_,
                           display(status_int_subject("settings_brightness", 0),
                                   status_int_subject("settings_display_sleep", 0),
                                   status_int_subject("settings_has_dimming", 0) != 0)
                               .c_str());

    lv_subject_copy_string(&settings_status_appearance_subject_,
                           appearance(status_int_subject("settings_dark_mode", 0) != 0,
                                      theme_manager_get_active_theme().name)
                               .c_str());

    lv_subject_copy_string(&settings_status_sound_subject_,
                           sound(status_int_subject("settings_sounds_enabled", 0) != 0,
                                 status_int_subject("settings_volume", 0))
                               .c_str());

    lv_subject_copy_string(
        &settings_status_devices_subject_,
        devices(lv_subject_get_int(get_printer_state().get_hardware_status_level_subject()))
            .c_str());

    if (helix::is_android_platform()) {
        // Android manages Wi-Fi and Ethernet itself — both backends compile to
        // nullptr there (wifi_backend.cpp, ethernet_backend.cpp under
        // __ANDROID__) — so probing either just logs errors/warnings for
        // nothing. Show the printer host instead, the same value
        // printer_host_value already carries.
        lv_subject_copy_string(&settings_status_connection_subject_,
                               lv_subject_get_string(&printer_host_value_subject_));
    } else {
        // Both link probes block (a wpa_supplicant control round trip; sysfs
        // scans or a netd socket round trip for Ethernet), so neither runs on
        // this thread. Show the last resolved states now, then refresh each as
        // its probe lands.
        render_connection_status();

        if (!ethernet_manager_) {
            ethernet_manager_ = std::make_unique<EthernetManager>();
        }
        const uint32_t seq = ++connection_probe_seq_;
        auto tok = lifetime_.token();
        ethernet_manager_->get_info_async([this, tok, seq](const EthernetInfo& info) {
            const bool ethernet_up = info.connected;
            tok.defer("SettingsPanel::apply_ethernet_status", [this, seq, ethernet_up]() {
                if (seq != connection_probe_seq_) {
                    return;
                }
                last_ethernet_up_ = ethernet_up;
                render_connection_status();
            });
        });
        get_wifi_manager()->get_status_async(
            tok, [this, seq](const WifiBackend::ConnectionStatus& status) {
                if (seq != connection_probe_seq_) {
                    return;
                }
                last_wifi_connected_ = status.connected;
                last_wifi_ssid_ = status.ssid;
                render_connection_status();
            });
    }

    lv_subject_copy_string(
        &settings_status_language_time_subject_,
        language_time(SystemSettingsManager::instance().get_language_display_name(),
                      status_int_subject("settings_time_format", 0))
            .c_str());

    lv_subject_copy_string(&settings_status_updates_subject_,
                           updates(status_int_subject("update_status", 0),
                                   status_string_subject("update_new_version", ""), helix_version(),
                                   lv_subject_get_int(&updates_firmware_managed_subject_) != 0)
                               .c_str());
}

void SettingsPanel::render_connection_status() {
    lv_subject_copy_string(&settings_status_connection_subject_,
                           helix::settings::status::connection(
                               last_ethernet_up_, last_wifi_connected_, last_wifi_ssid_)
                               .c_str());
}

void SettingsPanel::populate_led_chips() {
    // LED chip selection has been moved to LedSettingsOverlay.
    // This method is kept as a no-op stub for callers that haven't been updated yet.
    spdlog::trace("[{}] populate_led_chips() is now handled by LedSettingsOverlay", get_name());
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SettingsPanel::handle_estop_confirm_changed(bool enabled) {
    spdlog::info("[{}] E-Stop confirmation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_estop_require_confirmation(enabled);
    // Update EmergencyStopOverlay immediately
    EmergencyStopOverlay::instance().set_require_confirmation(enabled);
}

void SettingsPanel::handle_cancel_escalation_changed(bool enabled) {
    spdlog::info("[{}] Cancel escalation toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_cancel_escalation_enabled(enabled);
}

void SettingsPanel::handle_telemetry_changed(bool enabled) {
    spdlog::info("[{}] Telemetry toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SystemSettingsManager::instance().set_telemetry_enabled(enabled);
    if (enabled) {
        ToastManager::instance().show(
            ToastSeverity::SUCCESS,
            lv_tr("Thanks! TOTALLY anonymous usage data helps improve HelixScreen."), 4000);
    }
}

void SettingsPanel::handle_telemetry_view_data_clicked() {
    spdlog::debug("[{}] View Telemetry Data clicked - delegating to TelemetryDataOverlay",
                  get_name());

    auto& overlay = helix::settings::get_telemetry_data_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::show_restart_prompt() {
    // Already showing
    if (restart_prompt_dialog_) {
        return;
    }

    restart_prompt_dialog_ = helix::ui::modal_show("restart_prompt_dialog");
    if (restart_prompt_dialog_) {
        spdlog::debug("[{}] Restart prompt dialog shown via Modal system", get_name());
        // Clear pending flag so we don't show again until next change
        InputSettingsManager::instance().clear_restart_pending();
    }
}

void SettingsPanel::handle_debug_bundle_clicked() {
    spdlog::info("[SettingsPanel] Upload Debug Bundle clicked");
    DebugBundleModal::show_owned();
}

void SettingsPanel::handle_discord_clicked() {
    spdlog::info("[SettingsPanel] Discord clicked");
    helix::ui::InfoQrModal::show_owned({
        .icon = "message",
        .title = "Discord Community",
        .message = lv_tr("Join the HelixScreen community on Discord for discussion, "
                         "tips, troubleshooting help, and feature requests."),
        .url = "https://discord.gg/RZCT2StKhr",
        .url_text = "discord.gg/RZCT2StKhr",
    });
}

void SettingsPanel::handle_docs_clicked() {
    spdlog::info("[SettingsPanel] Documentation clicked");
    helix::ui::InfoQrModal::show_owned({
        .icon = "book",
        .title = lv_tr("Documentation"),
        .message = lv_tr("Browse guides, configuration references, and troubleshooting "
                         "resources for HelixScreen."),
        .url = "https://helixscreen.org/docs/guide/getting-started/",
        .url_text = "helixscreen.org/docs",
    });
}

void SettingsPanel::handle_security_settings_clicked() {
    spdlog::debug("[{}] Security clicked - delegating to SecuritySettingsOverlay", get_name());

    auto& overlay = helix::settings::get_security_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_led_settings_clicked() {
    spdlog::debug("[{}] LED Settings clicked - delegating to LedSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_led_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_printers_clicked() {
    spdlog::debug("[{}] Printers clicked - opening Printer List", get_name());

    auto& overlay = helix::ui::get_printer_list_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_filament_sensors_clicked() {
    spdlog::debug("[{}] Sensors clicked - delegating to SensorSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_sensor_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_fans_settings_clicked() {
    spdlog::debug("[{}] Fans clicked - delegating to FanSettingsOverlay", get_name());

    auto& overlay = helix::settings::get_fan_settings_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_ams_settings_clicked() {
    spdlog::debug("[{}] AMS Settings clicked - opening Device Operations", get_name());

    auto& overlay = helix::ui::get_ams_device_operations_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_spoolman_settings_clicked() {
    spdlog::debug("[{}] Spoolman Settings clicked - opening Spoolman overlay", get_name());

    auto& overlay = helix::ui::get_spoolman_overlay();
    if (!overlay.are_subjects_initialized()) {
        overlay.init_subjects();
        overlay.register_callbacks();
    }
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        overlay.set_api(api);
    }
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_macro_buttons_clicked() {
    spdlog::debug("[{}] Macro Buttons clicked - delegating to MacroButtonsOverlay", get_name());

    auto& overlay = helix::settings::get_macro_buttons_overlay();
    overlay.show(parent_screen_);
}

// Note: populate_macro_dropdowns() moved to MacroButtonsOverlay::populate_dropdowns()
// See ui_settings_macro_buttons.cpp
// Note: populate_sensor_list() moved to SensorSettingsOverlay::populate_switch_sensors()
// See ui_settings_sensors.cpp

void SettingsPanel::handle_machine_limits_clicked() {
    spdlog::debug("[{}] Machine Limits clicked - delegating to MachineLimitsOverlay", get_name());

    auto& overlay = helix::settings::get_machine_limits_overlay();
    overlay.set_api(api_);
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_material_temps_clicked() {
    spdlog::debug("[{}] Material Temperatures clicked", get_name());

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(parent_screen_);
}

void SettingsPanel::handle_change_host_clicked() {
    spdlog::debug("[{}] Change Host clicked", get_name());

    // Ownership and the reconnect sequence live in show_change_host_modal();
    // this panel contributes only its own host label refresh. The connection-
    // failed prompt reaches the same modal, and duplicating the reconnect here
    // is how the two would drift.
    helix::ui::show_change_host_modal([this](bool changed) {
        if (!changed) {
            return;
        }
        Config* config = Config::get_instance();
        const std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
        const int port = config->get<int>(config->df() + "moonraker_port", 7125);
        const std::string host_display = host + ":" + std::to_string(port);
        lv_subject_copy_string(&printer_host_value_subject_, host_display.c_str());
    });
}

void SettingsPanel::handle_network_clicked() {
    spdlog::debug("[{}] Network Settings clicked", get_name());

    auto& overlay = get_network_settings_overlay();

    if (!overlay.is_created()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    overlay.show();
}

void SettingsPanel::handle_power_devices_clicked() {
    spdlog::debug("[{}] Power Devices clicked", get_name());

    auto& panel = get_global_power_panel();
    lv_obj_t* overlay = panel.get_or_create_overlay(parent_screen_);
    if (overlay) {
        NavigationManager::instance().push_overlay(overlay);
    } else {
        spdlog::error("[{}] Failed to open Power panel", get_name());
    }
}

void SettingsPanel::handle_touch_calibration_clicked() {
    DisplayManager* dm = DisplayManager::instance();
    if (dm && !dm->supports_touch_calibration()) {
        spdlog::debug("[{}] No calibratable touch device", get_name());
        return;
    }

    spdlog::debug("[{}] Touch Calibration clicked", get_name());

    auto& overlay = helix::ui::get_touch_calibration_overlay();

    if (!overlay.is_created()) {
        overlay.init_subjects();
        overlay.register_callbacks();
        overlay.create(parent_screen_);
    }

    overlay.show([this](bool success) {
        if (success) {
            // Update status when calibration completes successfully
            lv_subject_copy_string(&touch_cal_status_subject_, lv_tr("Calibrated"));
            spdlog::info("[{}] Touch calibration completed - updated status", get_name());
        }
    });
}

void SettingsPanel::handle_restart_helix_clicked() {
    spdlog::info("[SettingsPanel] Restart HelixScreen requested");
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("Restarting HelixScreen..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::queue_update("SettingsPanel::restart", []() {
        spdlog::info("[SettingsPanel] Initiating restart...");
        app_request_restart_service();
    });
}

void SettingsPanel::handle_factory_reset_clicked() {
    spdlog::debug("[{}] Factory Reset clicked - showing confirmation dialog", get_name());

    // Create dialog on first use (lazy initialization)
    if (!factory_reset_dialog_ && parent_screen_) {
        spdlog::debug("[{}] Creating factory reset dialog...", get_name());

        // Create self-contained factory_reset_modal component
        // Callbacks are already wired via XML event_cb elements
        factory_reset_dialog_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "factory_reset_modal", nullptr));

        if (factory_reset_dialog_) {
            // Start hidden
            lv_obj_add_flag(factory_reset_dialog_, LV_OBJ_FLAG_HIDDEN);

            // Register as a function-based (nullptr-lifecycle) overlay so
            // crash crumbs show "anon" instead of "unreg".
            NavigationManager::instance().register_overlay_instance(factory_reset_dialog_, nullptr);

            // Register close callback to delete dialog when animation completes.
            // Must use safe_delete_deferred — this lambda runs inside
            // UpdateQueue::process_pending(), and synchronous deletion
            // during a batch corrupts LVGL's event linked list (#356, #491).
            NavigationManager::instance().register_overlay_close_callback(
                factory_reset_dialog_,
                [this]() { helix::ui::safe_delete_deferred(factory_reset_dialog_); });

            spdlog::info("[{}] Factory reset dialog created", get_name());
        } else {
            spdlog::error("[{}] Failed to create factory reset dialog", get_name());
            return;
        }
    }

    // Show the dialog via navigation stack
    if (factory_reset_dialog_) {
        NavigationManager::instance().push_overlay(factory_reset_dialog_);
    }
}

void SettingsPanel::perform_factory_reset() {
    spdlog::warn("[{}] Performing factory reset - resetting config!", get_name());

    // Get config instance and reset
    Config* config = Config::get_instance();
    config->reset_to_defaults();
    config->save();
    spdlog::info("[{}] Config reset to defaults", get_name());

    // Hide the dialog - animation + callback will handle cleanup
    if (factory_reset_dialog_) {
        NavigationManager::instance().go_back();
    }

    // Show confirmation toast and restart
    ToastManager::instance().show(ToastSeverity::INFO,
                                  lv_tr("Settings reset to defaults. Restarting..."), 1500);

    // Schedule restart after brief delay to let toast display
    helix::ui::queue_update("SettingsPanel::factory_reset_restart", []() {
        spdlog::info("[SettingsPanel] Restarting after factory reset...");
        app_request_restart_service();
    });
}

void SettingsPanel::handle_hardware_health_clicked() {
    spdlog::debug("[{}] Hardware Health clicked - delegating to HardwareHealthOverlay", get_name());

    auto& overlay = helix::settings::get_hardware_health_overlay();
    overlay.set_printer_state(&printer_state_);
    overlay.show(parent_screen_);
}

// Note: populate_hardware_issues() moved to HardwareHealthOverlay
// See ui_settings_hardware_health.cpp

// Note: handle_hardware_action() and related methods moved to HardwareHealthOverlay
// See ui_settings_hardware_health.cpp

// ============================================================================
// CATEGORY NAVIGATION CALLBACKS (open sub-panel overlays)
// ============================================================================

void SettingsPanel::on_display_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_display_clicked");
    auto& overlay = helix::settings::get_display_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_appearance_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_appearance_clicked");
    auto& overlay = helix::settings::get_appearance_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_sound_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_sound_clicked");
    auto& overlay = helix::settings::get_sound_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_language_time_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_language_time_clicked");
    auto& overlay = helix::settings::get_language_time_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_printing_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_printing_clicked");
    auto& overlay = helix::settings::get_printing_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_devices_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_devices_clicked");
    auto& overlay = helix::settings::get_hardware_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_safety_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_safety_clicked");
    auto& overlay = helix::settings::get_safety_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_system_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_system_clicked");
    auto& overlay = helix::settings::get_system_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_help_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_help_clicked");
    auto& overlay = helix::settings::get_help_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_touch_input_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_touch_input_clicked");
    auto& overlay = helix::settings::get_touch_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_connection_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_connection_clicked");
    auto& overlay = helix::settings::get_connection_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_updates_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_updates_clicked");
    auto& overlay = helix::settings::get_updates_settings_overlay();
    overlay.show(get_global_settings_panel().parent_screen_);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_plugins_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_plugins_clicked");
#if HELIX_HAS_PLUGINS
    helix::plugin::show_plugins_overlay(get_global_settings_panel().parent_screen_,
                                        "[SettingsPanel]");
#else
    // No plugin host on this build; the row stays hidden (subject never set).
#endif
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// STATIC TRAMPOLINES (XML event_cb pattern - use global singleton)
// ============================================================================

void SettingsPanel::on_estop_confirm_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_estop_confirm_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_estop_confirm_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_cancel_escalation_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_cancel_escalation_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_cancel_escalation_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_debug_bundle_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_debug_bundle_clicked");
    get_global_settings_panel().handle_debug_bundle_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_discord_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_discord_clicked");
    get_global_settings_panel().handle_discord_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_docs_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_docs_clicked");
    get_global_settings_panel().handle_docs_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_telemetry_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_telemetry_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_global_settings_panel().handle_telemetry_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_telemetry_view_data(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_telemetry_view_data");
    get_global_settings_panel().handle_telemetry_view_data_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_security_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_security_clicked");
    get_global_settings_panel().handle_security_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_led_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_led_settings_clicked");
    get_global_settings_panel().handle_led_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_timelapse_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_timelapse_settings_clicked");
    open_timelapse_settings();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_printers_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_printers_clicked");
    get_global_settings_panel().handle_printers_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_filament_sensors_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_filament_sensors_clicked");
    get_global_settings_panel().handle_filament_sensors_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_fans_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_fans_settings_clicked");
    get_global_settings_panel().handle_fans_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_ams_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_ams_settings_clicked");
    get_global_settings_panel().handle_ams_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_spoolman_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_spoolman_settings_clicked");
    get_global_settings_panel().handle_spoolman_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_macro_buttons_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_macro_buttons_clicked");
    get_global_settings_panel().handle_macro_buttons_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_machine_limits_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_machine_limits_clicked");
    get_global_settings_panel().handle_machine_limits_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_material_temps_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_material_temps_clicked");
    get_global_settings_panel().handle_material_temps_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_change_host_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_change_host_clicked");
    get_global_settings_panel().handle_change_host_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_network_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_network_clicked");
    get_global_settings_panel().handle_network_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_power_devices_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_power_devices_clicked");
    get_global_settings_panel().handle_power_devices_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_touch_calibration_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_touch_calibration_clicked");
    get_global_settings_panel().handle_touch_calibration_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_factory_reset_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_factory_reset_clicked");
    get_global_settings_panel().handle_factory_reset_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_hardware_health_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_hardware_health_clicked");
    get_global_settings_panel().handle_hardware_health_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_system_performance_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_system_performance_clicked");
    get_global_settings_panel().handle_performance_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::handle_performance_clicked() {
    spdlog::debug("[{}] Performance clicked - opening overlay", get_name());

    auto* overlay = helix::ui::UiOverlayPerformance::instance().create(lv_screen_active());
    if (!overlay) {
        spdlog::error("[{}] Failed to create Performance overlay", get_name());
        return;
    }

    // UiOverlayPerformance carries no IPanelLifecycle, so it registers with a null
    // lifecycle: that is what separates an intentional lifecycle-less overlay from a
    // caller who forgot to register. Without it the push is recorded as "unreg" in
    // panel telemetry and crash breadcrumbs, and strict mode aborts.
    NavigationManager::instance().register_overlay_instance(overlay, nullptr);
    NavigationManager::instance().push_overlay(overlay);
}

void SettingsPanel::on_restart_helix_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_helix_settings_clicked");
    get_global_settings_panel().handle_restart_helix_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_about_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_about_clicked");
    get_global_settings_panel().handle_about_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::handle_about_clicked() {
    spdlog::debug("[{}] About clicked - opening AboutSettingsOverlay", get_name());
    auto& overlay = helix::settings::get_about_settings_overlay();
    overlay.show(parent_screen_);
}

// ============================================================================
// STATIC TRAMPOLINES - OVERLAYS
// ============================================================================

// Note: Machine limits overlay callbacks are now in MachineLimitsOverlay class
// See ui_settings_machine_limits.cpp

void SettingsPanel::on_restart_later_clicked(lv_event_t* /* e */) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_later_clicked");
    auto& panel = get_global_settings_panel();
    if (panel.restart_prompt_dialog_) {
        helix::ui::modal_hide(panel.restart_prompt_dialog_);
        panel.restart_prompt_dialog_ = nullptr;
    }
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_restart_now_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_restart_now_clicked");
    spdlog::info("[SettingsPanel] User requested restart (input settings changed)");
    app_request_restart_service();
    LVGL_SAFE_EVENT_CB_END();
}

void SettingsPanel::on_header_back_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SettingsPanel] on_header_back_clicked");
    NavigationManager::instance().go_back();
    LVGL_SAFE_EVENT_CB_END();
}

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<SettingsPanel> g_settings_panel;

SettingsPanel& get_global_settings_panel() {
    if (!g_settings_panel) {
        g_settings_panel = std::make_unique<SettingsPanel>(get_printer_state(), nullptr);
        StaticPanelRegistry::instance().register_destroy("SettingsPanel",
                                                         []() { g_settings_panel.reset(); });
    }
    return *g_settings_panel;
}

// Register callbacks BEFORE settings_panel.xml registration per [L013]
void register_settings_panel_callbacks() {
    spdlog::trace("[SettingsPanel] Registering XML callbacks for settings_panel.xml");

    register_xml_callbacks({
        // Toggle callbacks used in settings_panel.xml
        {"on_led_settings_clicked", SettingsPanel::on_led_settings_clicked},
        {"on_timelapse_settings_clicked", SettingsPanel::on_timelapse_settings_clicked},
        {"on_security_clicked", SettingsPanel::on_security_clicked},
        {"on_estop_confirm_changed", SettingsPanel::on_estop_confirm_changed},
        {"on_cancel_escalation_changed", SettingsPanel::on_cancel_escalation_changed},
        {"on_cancel_escalation_timeout_changed", on_cancel_escalation_timeout_changed},
        {"on_telemetry_changed", SettingsPanel::on_telemetry_changed},
        {"on_telemetry_view_data", SettingsPanel::on_telemetry_view_data},
        {"on_log_level_changed", on_log_level_changed},
        {"on_debug_touches_changed", on_debug_touches_changed},
        {"on_scroll_limit_changed", on_scroll_limit_changed},
        {"on_long_press_time_changed", on_long_press_time_changed},
        {"on_home_edit_mode_changed", on_home_edit_mode_changed},
        {"on_scroll_guard_changed", on_scroll_guard_changed},
        // Action row callbacks used in settings_panel.xml
        {"on_printers_clicked", SettingsPanel::on_printers_clicked},
        {"on_filament_sensors_clicked", SettingsPanel::on_filament_sensors_clicked},
        {"on_fans_settings_clicked", SettingsPanel::on_fans_settings_clicked},
        {"on_macro_buttons_clicked", SettingsPanel::on_macro_buttons_clicked},
        {"on_machine_limits_clicked", SettingsPanel::on_machine_limits_clicked},
        {"on_material_temps_clicked", SettingsPanel::on_material_temps_clicked},
        {"on_network_clicked", SettingsPanel::on_network_clicked},
        {"on_power_devices_clicked", SettingsPanel::on_power_devices_clicked},
        {"on_touch_calibration_clicked", SettingsPanel::on_touch_calibration_clicked},
        {"on_factory_reset_clicked", SettingsPanel::on_factory_reset_clicked},
        {"on_hardware_health_clicked", SettingsPanel::on_hardware_health_clicked},
        {"on_restart_helix_settings_clicked", SettingsPanel::on_restart_helix_settings_clicked},
        {"on_about_clicked", SettingsPanel::on_about_clicked},
        {"on_change_host_clicked", SettingsPanel::on_change_host_clicked},
        // Help & Support callbacks
        {"on_debug_bundle_clicked", SettingsPanel::on_debug_bundle_clicked},
        {"on_discord_clicked", SettingsPanel::on_discord_clicked},
        {"on_docs_clicked", SettingsPanel::on_docs_clicked},
        // Category navigation callbacks (open sub-panel overlays)
        {"on_display_clicked", SettingsPanel::on_display_clicked},
        {"on_appearance_clicked", SettingsPanel::on_appearance_clicked},
        {"on_sound_clicked", SettingsPanel::on_sound_clicked},
        {"on_language_time_clicked", SettingsPanel::on_language_time_clicked},
        {"on_printing_clicked", SettingsPanel::on_printing_clicked},
        {"on_devices_clicked", SettingsPanel::on_devices_clicked},
        {"on_safety_clicked", SettingsPanel::on_safety_clicked},
        {"on_system_clicked", SettingsPanel::on_system_clicked},
        {"on_help_clicked", SettingsPanel::on_help_clicked},
        {"on_touch_input_clicked", SettingsPanel::on_touch_input_clicked},
        {"on_connection_clicked", SettingsPanel::on_connection_clicked},
        {"on_updates_clicked", SettingsPanel::on_updates_clicked},
    });
}
