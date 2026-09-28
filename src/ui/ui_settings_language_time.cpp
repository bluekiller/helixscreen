// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_language_time.cpp
 * @brief Implementation of LanguageTimeSettingsOverlay
 */

#include "ui_settings_language_time.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "display_settings_manager.h"
#include "static_panel_registry.h"
#include "system_settings_manager.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<LanguageTimeSettingsOverlay> g_language_time_settings_overlay;

LanguageTimeSettingsOverlay& get_language_time_settings_overlay() {
    if (!g_language_time_settings_overlay) {
        g_language_time_settings_overlay = std::make_unique<LanguageTimeSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "LanguageTimeSettingsOverlay", []() { g_language_time_settings_overlay.reset(); });
    }
    return *g_language_time_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

LanguageTimeSettingsOverlay::LanguageTimeSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

LanguageTimeSettingsOverlay::~LanguageTimeSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void LanguageTimeSettingsOverlay::init_subjects() {
    // Every bound subject is owned by a settings manager and registered globally
    // at startup.
    subjects_initialized_ = true;
}

void LanguageTimeSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_language_changed", on_language_changed},
        {"on_timezone_changed", on_timezone_changed},
        {"on_time_format_changed", on_time_format_changed},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* LanguageTimeSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_language_time_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void LanguageTimeSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void LanguageTimeSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_language_dropdown();
    init_timezone_dropdown();
    init_time_format_dropdown();
}

// ============================================================================
// INIT METHODS
// ============================================================================

void LanguageTimeSettingsOverlay::init_language_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_language");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, SystemSettingsManager::get_language_options());
        int index = SystemSettingsManager::instance().get_language_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Language dropdown initialized (index={})", get_name(), index);
    }
}

void LanguageTimeSettingsOverlay::init_timezone_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_timezone");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        std::string options = DisplaySettingsManager::get_timezone_options();
        lv_dropdown_set_options(dropdown, options.c_str());
        int index = DisplaySettingsManager::instance().get_timezone_index();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(index));
        spdlog::trace("[{}] Timezone dropdown initialized (index={}, tz={})", get_name(), index,
                      DisplaySettingsManager::instance().get_timezone());
    }
}

void LanguageTimeSettingsOverlay::init_time_format_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_time_format");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto current_format = DisplaySettingsManager::instance().get_time_format();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current_format));
        spdlog::trace("[{}] Time format dropdown initialized (format={})", get_name(),
                      static_cast<int>(current_format));
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void LanguageTimeSettingsOverlay::handle_language_changed(int index) {
    std::string lang_code = SystemSettingsManager::language_index_to_code(index);
    spdlog::info("[{}] Language changed: index {} ({})", get_name(), index, lang_code);
    SystemSettingsManager::instance().set_language_by_index(index);
}

void LanguageTimeSettingsOverlay::handle_timezone_changed(int index) {
    spdlog::info("[{}] Timezone changed to index {}", get_name(), index);
    DisplaySettingsManager::instance().set_timezone_by_index(index);
}

void LanguageTimeSettingsOverlay::handle_time_format_changed(int index) {
    auto format = static_cast<TimeFormat>(index);
    spdlog::info("[{}] Time format changed: {} ({})", get_name(), index,
                 index == 0 ? "12 Hour" : "24 Hour");
    DisplaySettingsManager::instance().set_time_format(format);
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void LanguageTimeSettingsOverlay::on_language_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanguageTimeSettingsOverlay] on_language_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_language_time_settings_overlay().handle_language_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LanguageTimeSettingsOverlay::on_timezone_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanguageTimeSettingsOverlay] on_timezone_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_language_time_settings_overlay().handle_timezone_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void LanguageTimeSettingsOverlay::on_time_format_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[LanguageTimeSettingsOverlay] on_time_format_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_language_time_settings_overlay().handle_time_format_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
