// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_printing.h
 * @brief Printing Settings overlay - machine limits, retraction, filament temps and
 *        cooldown behaviour, timelapse
 *
 * This overlay allows users to configure:
 * - Machine velocity/acceleration limits
 * - Firmware retraction settings (when available)
 * - Enclosure marking
 * - Material temperature presets
 * - Cold extrude / post-op nozzle cooldown behaviour
 * - Timelapse recording (when available)
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see SettingsManager for persistence
 * @see DisplaySettingsManager for G-code render mode
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class PrintingSettingsOverlay
 * @brief Overlay for configuring printing-related settings
 *
 * ## Usage:
 *
 * @code
 * auto& overlay = helix::settings::get_printing_settings_overlay();
 * overlay.show(parent_screen);
 * @endcode
 */
class PrintingSettingsOverlay : public OverlayBase {
  public:
    PrintingSettingsOverlay();
    ~PrintingSettingsOverlay() override;

    //
    // === OverlayBase Interface ===
    //

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Printing Settings";
    }

    void on_activate() override;

    //
    // === UI Creation ===
    //

    lv_obj_t* create(lv_obj_t* parent) override;

    /**
     * @brief Show the overlay (lazy-creates if needed)
     * @param parent_screen The parent screen for overlay creation
     */
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    //
    // === Event Handlers (public for static callbacks) ===
    //

    void handle_machine_limits_clicked();
    void handle_material_temps_clicked();
    void handle_allow_cold_extrude_changed(bool enabled);
    void handle_filament_auto_cooldown_changed(bool enabled);

  private:
    //
    // === Static Callbacks ===
    //

    static void on_enclosure_style_changed(lv_event_t* e);
    static void on_machine_limits_clicked(lv_event_t* e);
    static void on_motion_settings_clicked(lv_event_t* e);
    static void on_retraction_row_clicked(lv_event_t* e);
    static void on_material_temps_clicked(lv_event_t* e);
    static void on_allow_cold_extrude_changed(lv_event_t* e);
    static void on_filament_auto_cooldown_changed(lv_event_t* e);
    static void on_timelapse_settings_clicked(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton PrintingSettingsOverlay
 */
PrintingSettingsOverlay& get_printing_settings_overlay();

} // namespace helix::settings
