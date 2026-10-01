// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_safety.h
 * @brief Safety & Alerts overlay - e-stop, cancel escalation, completion alerts
 *
 * This overlay allows users to configure:
 * - E-Stop confirmation toggle
 * - Cancel escalation toggle and timeout
 * - Print completion alert mode
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SafetySettingsManager for persistence
 * @see AudioSettingsManager for completion alert persistence
 */

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

/**
 * @class SafetySettingsOverlay
 * @brief Overlay for configuring safety and notification settings
 *
 * Every row callback writes straight through its settings manager; the rows
 * that are not yet bound to their subject are re-synced on activate.
 */
class SafetySettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "Safety & Alerts";
    }
    const char* xml_component() const override {
        return "settings_safety_overlay";
    }

    void register_callbacks() override;
    void on_activate() override;

  private:
    void init_estop_toggle();
    void init_completion_alert_dropdown();
};

inline SafetySettingsOverlay& get_safety_settings_overlay() {
    return lazy_global<SafetySettingsOverlay>("SafetySettingsOverlay");
}

} // namespace helix::settings
