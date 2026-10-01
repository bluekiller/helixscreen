// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_touch.h"

#include "input_settings_manager.h"

#include <spdlog/spdlog.h>

namespace helix::settings {

void TouchSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    init_input_sliders();
}

// Sliders capture their value at XML construction from the static `value`
// prop, so push the persisted value on every activate. Toggle rows bind to
// subjects directly and need no manual sync.
void TouchSettingsOverlay::init_input_sliders() {
    if (!overlay_root_) {
        return;
    }

    auto& input = helix::InputSettingsManager::instance();

    auto sync_slider = [this](const char* row_name, int value) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, row_name);
        if (!row) {
            return;
        }
        if (lv_obj_t* slider = lv_obj_find_by_name(row, "slider")) {
            lv_slider_set_value(slider, value, LV_ANIM_OFF);
        }
        if (lv_obj_t* value_label = lv_obj_find_by_name(row, "value_label")) {
            lv_label_set_text_fmt(value_label, "%d", value);
        }
    };

    sync_slider("row_scroll_limit", input.get_scroll_limit());
    sync_slider("row_long_press_time", input.get_long_press_time());
}

} // namespace helix::settings
