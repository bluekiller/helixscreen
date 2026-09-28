// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sound.h
 * @brief Sound settings overlay - master toggle, volume, UI sounds, theme, output device
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see AudioSettingsManager for persistence
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class SoundSettingsOverlay : public OverlayBase {
  public:
    SoundSettingsOverlay();
    ~SoundSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Sound";
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

    void handle_sounds_changed(bool enabled);
    void handle_volume_changed(int value);
    void handle_volume_commit(int value);
    void handle_ui_sounds_changed(bool enabled);
    void handle_sound_theme_changed(int index);
    void handle_audio_device_changed(int index);
    void handle_preview_sounds();
    void handle_test_tracker();

  private:
    void init_sounds_toggle();
    void init_volume_slider();
    void init_sound_theme_dropdown();
    void init_audio_device_dropdown();

    /// Volume value label text
    char volume_value_buf_[8]; // e.g., "100%"

    static void on_sounds_changed(lv_event_t* e);
    static void on_volume_changed(lv_event_t* e);
    static void on_volume_commit(lv_event_t* e);
    static void on_volume_released(lv_event_t* e);
    static void on_ui_sounds_changed(lv_event_t* e);
    static void on_sound_theme_changed(lv_event_t* e);
    static void on_audio_device_changed(lv_event_t* e);
    static void on_preview_sounds(lv_event_t* e);
    static void on_test_tracker(lv_event_t* e);
};

/// Singleton accessor; registers the overlay with StaticPanelRegistry on first use.
SoundSettingsOverlay& get_sound_settings_overlay();

} // namespace helix::settings
