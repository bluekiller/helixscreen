// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.cpp
 * @brief Implementation of PrintingSettingsOverlay
 */

#include "ui_settings_printing.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_overlay_timelapse_settings.h"
#include "ui_settings_machine_limits.h"
#include "ui_settings_material_temps.h"
#include "ui_settings_motion.h"

#include "app_globals.h"
#include "moonraker_manager.h"
#include "post_op_cooldown_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<PrintingSettingsOverlay> g_printing_settings_overlay;

PrintingSettingsOverlay& get_printing_settings_overlay() {
    if (!g_printing_settings_overlay) {
        g_printing_settings_overlay = std::make_unique<PrintingSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "PrintingSettingsOverlay", []() { g_printing_settings_overlay.reset(); });
    }
    return *g_printing_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

PrintingSettingsOverlay::PrintingSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

PrintingSettingsOverlay::~PrintingSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void PrintingSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void PrintingSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_enclosure_style_changed", on_enclosure_style_changed},
        {"on_machine_limits_clicked", on_machine_limits_clicked},
        {"on_motion_settings_clicked", on_motion_settings_clicked},
        {"on_material_temps_clicked", on_material_temps_clicked},
        {"on_allow_cold_extrude_changed", on_allow_cold_extrude_changed},
        {"on_filament_auto_cooldown_changed", on_filament_auto_cooldown_changed},
        // on_retraction_row_clicked is registered by RetractionSettingsOverlay
        // on_timelapse_settings_clicked is registered by SettingsPanel
        {"on_timelapse_settings_clicked", on_timelapse_settings_clicked},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* PrintingSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_printing_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void PrintingSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void PrintingSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
}

// ============================================================================
// EVENT HANDLER IMPLEMENTATIONS
// ============================================================================

void PrintingSettingsOverlay::handle_machine_limits_clicked() {
    spdlog::debug("[{}] Machine Limits clicked", get_name());

    auto& overlay = helix::settings::get_machine_limits_overlay();
    overlay.set_api(get_moonraker_api());
    overlay.show(parent_screen_);
}

void PrintingSettingsOverlay::handle_material_temps_clicked() {
    spdlog::debug("[{}] Material Temperatures clicked", get_name());

    auto& overlay = helix::settings::get_material_temps_overlay();
    overlay.show(parent_screen_);
}

void PrintingSettingsOverlay::handle_allow_cold_extrude_changed(bool enabled) {
    spdlog::info("[{}] Allow cold load/unload toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SafetySettingsManager::instance().set_allow_cold_extrude(enabled);
}

void PrintingSettingsOverlay::handle_filament_auto_cooldown_changed(bool enabled) {
    spdlog::info("[{}] Post-op nozzle cooldown toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_filament_auto_cooldown(enabled);
    // Turning it off mid-countdown should take effect now, not in two minutes.
    if (!enabled) {
        PostOpCooldownManager::instance().cancel();
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void PrintingSettingsOverlay::on_enclosure_style_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_enclosure_style_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    SettingsManager::instance().set_enclosure_style(
        static_cast<helix::bed_drying::EnclosureStyle>(index));
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_machine_limits_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_machine_limits_clicked");
    get_printing_settings_overlay().handle_machine_limits_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_motion_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_motion_settings_clicked");
    helix::settings::show_motion_settings_overlay();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_retraction_row_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_retraction_row_clicked");
    // Delegates to the global retraction settings overlay (same as settings panel)
    // RetractionSettingsOverlay registers its own callback; this is a fallback
    spdlog::debug("[PrintingSettingsOverlay] Retraction row clicked (fallback)");
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_material_temps_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_material_temps_clicked");
    get_printing_settings_overlay().handle_material_temps_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_allow_cold_extrude_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_allow_cold_extrude_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_printing_settings_overlay().handle_allow_cold_extrude_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_filament_auto_cooldown_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_filament_auto_cooldown_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_printing_settings_overlay().handle_filament_auto_cooldown_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void PrintingSettingsOverlay::on_timelapse_settings_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[PrintingSettingsOverlay] on_timelapse_settings_clicked");
    open_timelapse_settings();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
