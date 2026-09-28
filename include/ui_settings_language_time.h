// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_language_time.h
 * @brief Language & Time settings overlay - language, timezone, time format
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SystemSettingsManager for language persistence
 * @see DisplaySettingsManager for timezone and time format persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class LanguageTimeSettingsOverlay : public OverlayBase {
  public:
    LanguageTimeSettingsOverlay();
    ~LanguageTimeSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Language & Time";
    }

    void on_activate() override;

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    //
    // === Event Handlers (public for static callbacks) ===
    //

    void handle_language_changed(int index);
    void handle_timezone_changed(int index);
    void handle_time_format_changed(int index);

  private:
    void init_language_dropdown();
    void init_timezone_dropdown();
    void init_time_format_dropdown();

    static void on_language_changed(lv_event_t* e);
    static void on_timezone_changed(lv_event_t* e);
    static void on_time_format_changed(lv_event_t* e);
};

/// Singleton accessor; registers the overlay with StaticPanelRegistry on first use.
LanguageTimeSettingsOverlay& get_language_time_settings_overlay();

} // namespace helix::settings
