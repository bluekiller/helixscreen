// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_display.h
 * @brief Display settings overlay - brightness, dim, sleep, screensaver, rotation, UI scale
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see DisplaySettingsManager for persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"
#include "subject_managed_panel.h"

namespace helix::settings {

class DisplaySettingsOverlay : public OverlayBase {
  public:
    DisplaySettingsOverlay();
    ~DisplaySettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Display";
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

    void handle_display_rotation_changed(int index);
    void handle_brightness_changed(int value);
    void handle_brightness_commit(int value);
    void handle_ui_scale_changed(int index);
    void handle_dim_changed(int index);
    void handle_sleep_changed(int index);
    void handle_sleep_while_printing_changed(bool enabled);

  private:
    void init_display_rotation_dropdown();
    void init_brightness_controls();
    void init_dim_dropdown();
    void init_sleep_dropdown();
    void init_sleep_while_printing_toggle();
    void init_ui_scale_dropdown();

#ifdef HELIX_ENABLE_SCREENSAVER
    void init_screensaver_dropdown();
    void handle_test_screensaver();
#endif

    /// SubjectManager, declared ahead of the subjects it owns so it tears down
    /// after them (names withdraw before storage dies).
    SubjectManager subjects_;

    /// Subject for brightness value label binding
    lv_subject_t brightness_value_subject_;
    char brightness_value_buf_[8]; // e.g., "100%"

    static void on_display_rotation_changed(lv_event_t* e);
    static void on_brightness_changed(lv_event_t* e);
    static void on_brightness_commit(lv_event_t* e);
    static void on_ui_scale_changed(lv_event_t* e);
    static void on_dim_changed(lv_event_t* e);
    static void on_sleep_changed(lv_event_t* e);
    static void on_sleep_while_printing_changed(lv_event_t* e);
#ifdef HELIX_ENABLE_SCREENSAVER
    static void on_screensaver_changed(lv_event_t* e);
    static void on_test_screensaver(lv_event_t* e);
#endif
};

/// Singleton accessor; registers the overlay with StaticPanelRegistry on first use.
DisplaySettingsOverlay& get_display_settings_overlay();

} // namespace helix::settings
