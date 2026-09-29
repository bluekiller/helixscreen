// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_language_refresh.h"

#include "ui_ams_tool_text.h"

#include "ams_state.h"
#include "app_globals.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "static_subject_registry.h"
#include "system_settings_manager.h"
#include "tool_state.h"

namespace helix::ui {

namespace {

ObserverGuard s_language_observer;
bool s_deinit_registered = false;

} // namespace

void init_language_refresh() {
    // Replacing the guard drops any observer left on settings subjects that were
    // since torn down and rebuilt, so calling this again re-arms it.
    s_language_observer = observe_language_change(&ToolState::instance(), [](ToolState* tools) {
        tools->refresh_display_labels();
        get_printer_state().refresh_extruder_display_names();
        get_printer_state().refresh_fan_display_names();
        refresh_ams_tool_text();
        // AmsState formats its status texts (clog meter, dryer, loaded
        // lane) as it syncs from the backend.
        AmsState::instance().sync_from_backend();
    });
    if (s_deinit_registered) {
        return;
    }
    s_deinit_registered = true;
    StaticSubjectRegistry::instance().register_deinit("LanguageRefresh", []() {
        s_language_observer.reset();
        s_deinit_registered = false;
    });
}

} // namespace helix::ui
