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

#include <memory>

namespace helix::ui {

namespace {

ObserverGuard s_language_observer;
bool s_initialized = false;
/// The language subjects the observer was registered on. Settings subjects can
/// be torn down and rebuilt (the test fixtures do), and an observer on the old
/// ones would never fire again.
std::weak_ptr<bool> s_bound_to;

} // namespace

void init_language_refresh() {
    const auto bound = s_bound_to.lock();
    if (s_initialized && bound && *bound) {
        return;
    }
    s_bound_to = SystemSettingsManager::instance().get_subjects_lifetime();
    s_language_observer = observe_language_change(&ToolState::instance(), [](ToolState* tools) {
        tools->refresh_display_labels();
        get_printer_state().refresh_extruder_display_names();
        refresh_ams_tool_text();
        // AmsState formats its status texts (clog meter, dryer, loaded
        // lane) as it syncs from the backend.
        AmsState::instance().sync_from_backend();
    });
    if (s_initialized) {
        return; // Re-armed; the deinit below is already registered.
    }
    s_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("LanguageRefresh", []() {
        s_language_observer.reset();
        s_bound_to.reset();
        s_initialized = false;
    });
}

} // namespace helix::ui
