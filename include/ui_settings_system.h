// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_system.h
 * @brief System settings overlay - security, telemetry, logging, admin
 *
 * This overlay provides access to system administration settings:
 * - Security (PIN lock)
 * - Performance
 * - Telemetry
 * - Log level
 * - Restart / Factory reset
 *
 * Its callbacks are registered by the global SettingsPanel, which owns the
 * complex logic (factory reset dialog, etc.).
 * The telemetry and log-level rows bind to their SystemSettingsManager subjects.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

/**
 * @class SystemSettingsOverlay
 * @brief Overlay for system administration settings
 *
 * @code
 * helix::settings::get_system_settings_overlay().show(parent_screen);
 * @endcode
 */
class SystemSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "System";
    }
    const char* xml_component() const override {
        return "settings_system_overlay";
    }
};

inline SystemSettingsOverlay& get_system_settings_overlay() {
    return lazy_global<SystemSettingsOverlay>("SystemSettingsOverlay");
}

} // namespace helix::settings
