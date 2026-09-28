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

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class ConnectionSettingsOverlay : public OverlayBase {
  public:
    ConnectionSettingsOverlay();
    ~ConnectionSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Connection";
    }

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }
};

ConnectionSettingsOverlay& get_connection_settings_overlay();

} // namespace helix::settings
