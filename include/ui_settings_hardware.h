// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

/**
 * @class HardwareSettingsOverlay
 * @brief Overlay for hardware and device settings
 *
 * Provides action rows for managing printers, AMS, fans, sensors,
 * LEDs, power devices, spoolman, and macro buttons. Each row opens
 * an existing overlay for the respective feature.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 */
class HardwareSettingsOverlay : public OverlayBase {
  public:
    HardwareSettingsOverlay();
    ~HardwareSettingsOverlay() override;

    // === OverlayBase Interface ===

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Devices";
    }

    void on_activate() override;

    // === UI Creation ===

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

  private:
    // === Static Callbacks ===

    static void on_camera_view_clicked(lv_event_t* e);
    static void on_ams_settings_clicked(lv_event_t* e);
    static void on_fans_settings_clicked(lv_event_t* e);
    static void on_filament_sensors_clicked(lv_event_t* e);
    static void on_led_settings_clicked(lv_event_t* e);
    static void on_power_devices_clicked(lv_event_t* e);
    static void on_spoolman_settings_clicked(lv_event_t* e);
    static void on_macro_buttons_clicked(lv_event_t* e);
};

/**
 * @brief Global instance accessor
 *
 * Creates the overlay on first access and registers it for cleanup
 * with StaticPanelRegistry.
 *
 * @return Reference to singleton HardwareSettingsOverlay
 */
HardwareSettingsOverlay& get_hardware_settings_overlay();

/**
 * @brief Bind the Hardware Health row to live validation state
 *
 * Points the row's label at the issue-count summary and its icon colour at
 * hardware_status_level. The row lives inside settings_hardware_overlay, so
 * only something holding that tree can reach the widgets by name.
 *
 * @param overlay_root A settings_hardware_overlay tree
 */
void bind_hardware_health_row(lv_obj_t* overlay_root);

} // namespace helix::settings
