// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_system.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_panel_settings.h"

#include "config.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SystemSettingsOverlay> g_system_settings_overlay;

SystemSettingsOverlay& get_system_settings_overlay() {
    if (!g_system_settings_overlay) {
        g_system_settings_overlay = std::make_unique<SystemSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SystemSettingsOverlay", []() { g_system_settings_overlay.reset(); });
    }
    return *g_system_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SystemSettingsOverlay::SystemSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SystemSettingsOverlay::~SystemSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SystemSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // All subjects used by the system overlay XML are registered at startup
    // (perf_available by PerformanceState, settings_telemetry_enabled by
    // SystemSettingsManager).
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void SystemSettingsOverlay::register_callbacks() {
    // All callbacks used in settings_system_overlay.xml are already registered
    // globally by SettingsPanel in init_subjects() and register_settings_panel_callbacks().
    // No additional registrations needed here.
    spdlog::debug("[{}] Callbacks registered (delegated to SettingsPanel)", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SystemSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_system_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SystemSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void SystemSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_telemetry_toggle();
    init_log_level_dropdown();
}

// ============================================================================
// INTERNAL INITIALIZATION
// ============================================================================

void SystemSettingsOverlay::init_telemetry_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* telemetry_row = lv_obj_find_by_name(overlay_root_, "row_telemetry");
    if (telemetry_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(telemetry_row, "toggle");
        if (toggle) {
            if (SystemSettingsManager::instance().get_telemetry_enabled()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] Telemetry toggle initialized", get_name());
        }
    }
}

void SystemSettingsOverlay::init_log_level_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_log_level");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        // Options are populated + translated by the XML options_tag on row_log_level.
        int index = SystemSettingsManager::instance().get_log_level_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Log level dropdown initialized (index={})", get_name(), index);
    }
}

} // namespace helix::settings
