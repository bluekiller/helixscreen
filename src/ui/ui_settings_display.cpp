// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display.cpp
 * @brief Implementation of DisplaySettingsOverlay
 */

#include "ui_settings_display.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_settings.h" // get_global_settings_panel() owns the restart prompt
#include "ui_toast_manager.h"

#include "display_manager.h"
#include "display_metrics.h"
#include "display_settings_manager.h"
#include "format_utils.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <memory>
#include <string>

namespace helix::settings {

namespace {

/// Explicit UI scale steps offered below Automatic, in dropdown order. 100% is
/// a real choice rather than a synonym for Automatic: it pins a high-DPI panel
/// back to authored sizing. The top of the range is
/// DisplayMetrics::kMaxScaleSettingPercent, so the dropdown cannot ask for a
/// scale the curve itself would refuse.
constexpr int kUiScalePercents[] = {100, 125, 150, 175, 200};
static_assert(kUiScalePercents[0] == helix::DisplayMetrics::kMinScaleSettingPercent);
static_assert(kUiScalePercents[std::size(kUiScalePercents) - 1] ==
              helix::DisplayMetrics::kMaxScaleSettingPercent);

} // namespace

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<DisplaySettingsOverlay> g_display_settings_overlay;

DisplaySettingsOverlay& get_display_settings_overlay() {
    if (!g_display_settings_overlay) {
        g_display_settings_overlay = std::make_unique<DisplaySettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "DisplaySettingsOverlay", []() { g_display_settings_overlay.reset(); });
    }
    return *g_display_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

DisplaySettingsOverlay::DisplaySettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

DisplaySettingsOverlay::~DisplaySettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void DisplaySettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Brightness value subject for label binding
    snprintf(brightness_value_buf_, sizeof(brightness_value_buf_), "100%%");
    UI_MANAGED_SUBJECT_STRING(brightness_value_subject_, brightness_value_buf_,
                              brightness_value_buf_, "brightness_value", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void DisplaySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_display_rotation_changed", on_display_rotation_changed},
        {"on_brightness_changed", on_brightness_changed},
        {"on_brightness_commit", on_brightness_commit},
        {"on_ui_scale_changed", on_ui_scale_changed},
        {"on_dim_changed", on_dim_changed},
        {"on_sleep_changed", on_sleep_changed},
        {"on_sleep_while_printing_changed", on_sleep_while_printing_changed},
#ifdef HELIX_ENABLE_SCREENSAVER
        {"on_screensaver_changed", on_screensaver_changed},
        {"on_test_screensaver", on_test_screensaver},
#else
        {"on_screensaver_changed", [](lv_event_t*) {}},
        {"on_test_screensaver", [](lv_event_t*) {}},
#endif
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* DisplaySettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_display_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void DisplaySettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void DisplaySettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_display_rotation_dropdown();
    init_brightness_controls();
    init_dim_dropdown();
    init_sleep_dropdown();
    init_sleep_while_printing_toggle();
    init_ui_scale_dropdown();

#ifdef HELIX_ENABLE_SCREENSAVER
    init_screensaver_dropdown();
#else
    lv_obj_t* ss_row = lv_obj_find_by_name(overlay_root_, "row_screensaver");
    if (ss_row) {
        lv_obj_add_flag(ss_row, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

// ============================================================================
// INIT METHODS
// ============================================================================

void DisplaySettingsOverlay::init_display_rotation_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_display_rotation");
    lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    if (dropdown) {
        int degrees = DisplaySettingsManager::instance().get_display_rotation();
        int index = DisplaySettingsManager::rotation_degrees_to_index(degrees);
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));

        spdlog::debug("[{}] Screen rotation dropdown initialized to index {} ({}°)", get_name(),
                      index, degrees);
    }
}

void DisplaySettingsOverlay::init_brightness_controls() {
    if (!overlay_root_)
        return;

    lv_obj_t* brightness_slider = lv_obj_find_by_name(overlay_root_, "brightness_slider");
    if (brightness_slider) {
        int brightness = DisplaySettingsManager::instance().get_brightness();
        lv_slider_set_value(brightness_slider, brightness, LV_ANIM_OFF);

        helix::format::format_percent(brightness, brightness_value_buf_,
                                      sizeof(brightness_value_buf_));
        lv_subject_copy_string(&brightness_value_subject_, brightness_value_buf_);

        spdlog::debug("[{}] Brightness initialized to {}%", get_name(), brightness);
    }
}

void DisplaySettingsOverlay::init_dim_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* dim_row = lv_obj_find_by_name(overlay_root_, "row_display_dim");
    lv_obj_t* dim_dropdown = dim_row ? lv_obj_find_by_name(dim_row, "dropdown") : nullptr;
    if (dim_dropdown) {
        int current_sec = DisplaySettingsManager::instance().get_display_dim_sec();
        int index = DisplaySettingsManager::dim_seconds_to_index(current_sec);
        lv_dropdown_set_selected(dim_dropdown, index);

        spdlog::debug("[{}] Dim dropdown initialized to index {} ({}s)", get_name(), index,
                      current_sec);
    }
}

void DisplaySettingsOverlay::init_sleep_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* sleep_row = lv_obj_find_by_name(overlay_root_, "row_display_sleep");
    lv_obj_t* sleep_dropdown = sleep_row ? lv_obj_find_by_name(sleep_row, "dropdown") : nullptr;
    if (sleep_dropdown) {
        int current_sec = DisplaySettingsManager::instance().get_display_sleep_sec();
        int index = DisplaySettingsManager::sleep_seconds_to_index(current_sec);
        lv_dropdown_set_selected(sleep_dropdown, index);

        spdlog::debug("[{}] Sleep dropdown initialized to index {} ({}s)", get_name(), index,
                      current_sec);
    }
}

void DisplaySettingsOverlay::init_sleep_while_printing_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_sleep_while_printing");
    if (!row)
        return;

    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    if (toggle) {
        if (DisplaySettingsManager::instance().get_sleep_while_printing()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        spdlog::trace("[{}] Sleep while printing toggle initialized", get_name());
    }
}

void DisplaySettingsOverlay::init_ui_scale_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_ui_scale");
    lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    if (!dropdown)
        return;

    // The Automatic row carries the percentage the panel's DPI resolved to, so
    // a user on a phone-class screen can see what it chose before deciding to
    // override it. Every shipping printer sits in the DPI deadband and reads
    // "Automatic (100%)".
    const int auto_percent =
        static_cast<int>(std::lround(helix::DisplayMetrics::auto_scale() * 100.0));
    std::string options =
        std::string(lv_tr("Automatic")) + " (" + std::to_string(auto_percent) + "%)";
    for (int percent : kUiScalePercents) {
        options += "\n" + std::to_string(percent) + "%";
    }
    lv_dropdown_set_options(dropdown, options.c_str());

    // Index 0 is Automatic; the explicit steps follow in kUiScalePercents
    // order. A stored value that is not one of the offered steps (hand-edited,
    // or a step we later drop) selects Automatic, which is what it will
    // actually behave as on the next start.
    const int stored = DisplaySettingsManager::instance().get_ui_scale_percent();
    uint32_t selected = 0;
    for (size_t i = 0; i < std::size(kUiScalePercents); ++i) {
        if (kUiScalePercents[i] == stored) {
            selected = static_cast<uint32_t>(i + 1);
            break;
        }
    }
    lv_dropdown_set_selected(dropdown, selected);

    spdlog::debug("[{}] UI scale dropdown initialized: stored={} auto={}%", get_name(), stored,
                  auto_percent);
}

#ifdef HELIX_ENABLE_SCREENSAVER
void DisplaySettingsOverlay::init_screensaver_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* ss_row = lv_obj_find_by_name(overlay_root_, "row_screensaver");
    lv_obj_t* ss_dropdown = ss_row ? lv_obj_find_by_name(ss_row, "dropdown") : nullptr;
    if (ss_dropdown) {
        int current_type = DisplaySettingsManager::instance().get_screensaver_type();
        lv_dropdown_set_selected(ss_dropdown, current_type);

        spdlog::debug("[{}] Screensaver dropdown initialized to type {}", get_name(), current_type);
    }
}
#endif

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void DisplaySettingsOverlay::handle_display_rotation_changed(int index) {
    int degrees = DisplaySettingsManager::index_to_rotation_degrees(index);
    spdlog::info("[{}] Screen rotation changed: index {} = {}°", get_name(), index, degrees);

    // The rotation is read once, by DisplayManager at startup, and LVGL screens
    // never re-rotate afterwards. Only prompt when the applied value actually
    // moved - re-picking the current one needs no restart.
    if (DisplaySettingsManager::instance().set_display_rotation(degrees)) {
        get_global_settings_panel().show_restart_prompt();
    }
}

void DisplaySettingsOverlay::handle_brightness_changed(int value) {
    // Per drag tick: apply to the backlight and update the readout, but do NOT
    // persist. handle_brightness_commit() saves once, on release.
    DisplaySettingsManager::instance().preview_brightness(value);

    helix::format::format_percent(value, brightness_value_buf_, sizeof(brightness_value_buf_));
    lv_subject_copy_string(&brightness_value_subject_, brightness_value_buf_);
}

void DisplaySettingsOverlay::handle_brightness_commit(int value) {
    spdlog::info("[{}] Brightness committed: {}%", get_name(), value);
    DisplaySettingsManager::instance().set_brightness(value);
}

void DisplaySettingsOverlay::handle_ui_scale_changed(int index) {
    const int percent = (index >= 1 && index <= static_cast<int>(std::size(kUiScalePercents)))
                            ? kUiScalePercents[index - 1]
                            : helix::DisplayMetrics::kScaleSettingAutomatic;
    DisplaySettingsManager::instance().set_ui_scale_percent(percent);

    // Say it rather than fake it. Applying live would need every screen to
    // re-tier, and AssetManager only ever registers font tiers upward
    // (register_fonts_for_tier() early-returns on a lower tier), so a downward
    // change cannot un-register the faces it already handed out.
    ToastManager::instance().show(ToastSeverity::INFO, lv_tr("UI scale applies after restart"));
}

void DisplaySettingsOverlay::handle_dim_changed(int index) {
    int seconds = DisplaySettingsManager::index_to_dim_seconds(index);
    spdlog::info("[{}] Display dim changed: index {} = {}s", get_name(), index, seconds);
    DisplaySettingsManager::instance().set_display_dim_sec(seconds);
}

void DisplaySettingsOverlay::handle_sleep_changed(int index) {
    int seconds = DisplaySettingsManager::index_to_sleep_seconds(index);
    spdlog::info("[{}] Display sleep changed: index {} = {}s", get_name(), index, seconds);
    DisplaySettingsManager::instance().set_display_sleep_sec(seconds);
}

void DisplaySettingsOverlay::handle_sleep_while_printing_changed(bool enabled) {
    spdlog::info("[{}] Sleep while printing toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_sleep_while_printing(enabled);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void DisplaySettingsOverlay::on_display_rotation_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_display_rotation_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_settings_overlay().handle_display_rotation_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_brightness_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_brightness_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_display_settings_overlay().handle_brightness_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_brightness_commit(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_brightness_commit");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_display_settings_overlay().handle_brightness_commit(value);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_ui_scale_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_ui_scale_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_settings_overlay().handle_ui_scale_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_dim_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_dim_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_settings_overlay().handle_dim_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_sleep_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_sleep_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_display_settings_overlay().handle_sleep_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_sleep_while_printing_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_sleep_while_printing_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_display_settings_overlay().handle_sleep_while_printing_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

#ifdef HELIX_ENABLE_SCREENSAVER
void DisplaySettingsOverlay::on_screensaver_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_screensaver_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = lv_dropdown_get_selected(dropdown);
    spdlog::info("[DisplaySettingsOverlay] Screensaver changed to type {}", index);
    DisplaySettingsManager::instance().set_screensaver_type(index);
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::on_test_screensaver(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[DisplaySettingsOverlay] on_test_screensaver");
    get_display_settings_overlay().handle_test_screensaver();
    LVGL_SAFE_EVENT_CB_END();
}

void DisplaySettingsOverlay::handle_test_screensaver() {
    int type = DisplaySettingsManager::instance().get_screensaver_type();
    if (type <= 0) {
        return; // "Off" — button should have been hidden, but guard anyway
    }
    auto* dm = DisplayManager::instance();
    if (!dm) {
        spdlog::warn("[{}] DisplayManager not available, cannot preview screensaver", get_name());
        return;
    }
    spdlog::info("[{}] User-initiated screensaver preview (type {})", get_name(), type);
    dm->preview_screensaver(type);
}
#endif

} // namespace helix::settings
