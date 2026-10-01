// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_connection.h
 * @brief Connection settings overlay - network, printers, Moonraker host
 *
 * Every row callback (on_network_clicked, on_printers_clicked,
 * on_change_host_clicked) is registered by SettingsPanel, whose parent screen
 * is set at startup, so each opens correctly whichever page was visited first.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

class ConnectionSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Connection";
    }
    const char* xml_component() const override {
        return "settings_connection_overlay";
    }
};

inline ConnectionSettingsOverlay& get_connection_settings_overlay() {
    return lazy_global<ConnectionSettingsOverlay>("ConnectionSettingsOverlay");
}

} // namespace helix::settings
