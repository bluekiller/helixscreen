// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_touch.h
 * @brief Touch & Input settings overlay — calibration, debug viz, scroll feel
 *
 * Reached from Settings → Touch & Input. Groups everything that affects how the
 * screen reads finger input, plus the Scroll Buttons, System Keyboard and Keep
 * Navigation Bar rows.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */

#pragma once

#include "overlay_base.h"
#include "static_panel_registry.h"

namespace helix::settings {

class TouchSettingsOverlay : public OverlayBase {
  public:
    const char* get_name() const override {
        return "TouchInput";
    }
    const char* xml_component() const override {
        return "settings_touch_overlay";
    }

    // Row callbacks are registered globally by SettingsPanel so the top-level
    // Touch Calibration entry can share them; the bound subjects are owned by
    // InputSettingsManager.
    void on_activate() override;

  private:
    void init_input_sliders();
};

inline TouchSettingsOverlay& get_touch_settings_overlay() {
    return lazy_global<TouchSettingsOverlay>("TouchSettingsOverlay");
}

} // namespace helix::settings
