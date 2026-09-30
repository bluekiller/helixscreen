// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_safety.h"

#include "ui_callback_helpers.h"
#include "ui_emergency_stop.h"

#include "audio_settings_manager.h"
#include "safety_settings_manager.h"
#include "settings_manager.h"
#include "ui/ui_widget_helpers.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::settings {

using helix::ui::event_checked;
using helix::ui::event_selected;

void SafetySettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_estop_confirm_changed",
         [](lv_event_t* e) {
             bool on = event_checked(e);
             SafetySettingsManager::instance().set_estop_require_confirmation(on);
             EmergencyStopOverlay::instance().set_require_confirmation(on);
         }},
        {"on_cancel_escalation_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_cancel_escalation_enabled(event_checked(e));
         }},
        {"on_cancel_escalation_timeout_changed",
         [](lv_event_t* e) {
             static constexpr int TIMEOUT_SECONDS[] = {15, 30, 60, 120};
             int index = std::clamp(event_selected(e), 0, 3);
             SafetySettingsManager::instance().set_cancel_escalation_timeout_seconds(
                 TIMEOUT_SECONDS[index]);
         }},
        {"on_completion_alert_changed",
         [](lv_event_t* e) {
             AudioSettingsManager::instance().set_completion_alert_mode(
                 static_cast<CompletionAlertMode>(event_selected(e)));
         }},
        {"on_min_toast_severity_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_min_toast_severity(event_selected(e));
         }},
        {"on_macro_confirm_changed",
         [](lv_event_t* e) {
             SafetySettingsManager::instance().set_macro_require_confirmation(event_checked(e));
         }},
        {"on_detection_enabled_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_detection_enabled(event_checked(e));
         }},
        {"on_detection_pause_changed",
         [](lv_event_t* e) {
             SettingsManager::instance().set_detection_pause_on_detect(event_checked(e));
         }},
    });
}

void SafetySettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_estop_toggle();
    init_completion_alert_dropdown();
}

void SafetySettingsOverlay::init_estop_toggle() {
    auto& safety_settings = SafetySettingsManager::instance();

    lv_obj_t* estop_row = lv_obj_find_by_name(overlay_root_, "row_estop_confirm");
    if (estop_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(estop_row, "toggle");
        if (toggle) {
            if (safety_settings.get_estop_require_confirmation()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] E-Stop confirmation toggle initialized", get_name());
        }
    }
}

void SafetySettingsOverlay::init_completion_alert_dropdown() {
    lv_obj_t* row = helix::ui::find_required(overlay_root_, "row_completion_alert", get_name());
    if (lv_obj_t* dropdown = helix::ui::find_required(row, "dropdown", get_name())) {
        auto mode = AudioSettingsManager::instance().get_completion_alert_mode();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(mode));
    }
}

} // namespace helix::settings
