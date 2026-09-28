// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_connection.h"

#include "ui_nav_manager.h"

#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

static std::unique_ptr<ConnectionSettingsOverlay> g_connection_settings_overlay;

ConnectionSettingsOverlay& get_connection_settings_overlay() {
    if (!g_connection_settings_overlay) {
        g_connection_settings_overlay = std::make_unique<ConnectionSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "ConnectionSettingsOverlay", []() { g_connection_settings_overlay.reset(); });
    }
    return *g_connection_settings_overlay;
}

ConnectionSettingsOverlay::ConnectionSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

ConnectionSettingsOverlay::~ConnectionSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

void ConnectionSettingsOverlay::init_subjects() {
    // show_network_settings and printer_host_value are owned by SettingsPanel.
    subjects_initialized_ = true;
}

void ConnectionSettingsOverlay::register_callbacks() {
    // See the file header for where each row callback is registered.
}

lv_obj_t* ConnectionSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_connection_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void ConnectionSettingsOverlay::show(lv_obj_t* parent_screen) {
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

} // namespace helix::settings
