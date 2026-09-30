// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2024-2025 Brad Barnett, 2025 Peter Welch Brown

/**
 * @file ui_lazy_panel_helper.h
 * @brief Template helper for lazy panel creation and navigation
 *
 * lazy_create_and_push_overlay() forwards to OverlayBase::show();
 * lazy_push_overlay() pushes a bare widget with no lifecycle object.
 */

#pragma once

#include "ui_nav_manager.h"
#include "ui_toast_manager.h"

#include "overlay_base.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

/**
 * @brief Open a global overlay through OverlayBase::show()
 *
 * The overlay object owns its root; @p cached_panel is written with it after
 * the push for callers that still read their copy, and is never read here.
 * Whether the tree is freed on close is the overlay's destroy_on_close().
 *
 * @param getter Returns the global overlay instance
 * @param cached_panel Caller's copy of the root, overwritten on return
 * @param parent_screen Screen to build on
 * @param panel_display_name Human-readable name for logging
 * @param caller_name Name of the calling panel (for logging)
 * @return true if the overlay was pushed
 */
template <typename PanelType, typename Getter>
bool lazy_create_and_push_overlay(Getter getter, lv_obj_t*& cached_panel, lv_obj_t* parent_screen,
                                  const char* panel_display_name, const char* caller_name) {
    spdlog::debug("[{}] {} clicked - opening panel", caller_name, panel_display_name);
    PanelType& panel = getter();
    // Qualified: some overlays declare a show() of their own that hides this one.
    bool ok = panel.OverlayBase::show(parent_screen);
    cached_panel = panel.get_root();
    return ok;
}

/**
 * @brief Simple lazy overlay creation and push
 *
 * A simpler version of lazy_create_and_push_overlay for overlays that don't
 * follow the full global-panel pattern. Use this when you have a custom
 * creation function that returns an lv_obj_t*.
 *
 * Pattern it replaces:
 * @code
 * if (!overlay_cache_) {
 *     overlay_cache_ = create_overlay(parent);
 *     if (!overlay_cache_) {
 *         spdlog::error("[Panel] Failed to create overlay");
 *         return;
 *     }
 * }
 * NavigationManager::instance().push_overlay(overlay_cache_);
 * @endcode
 *
 * @tparam CreateFunc Callable that takes (lv_obj_t* parent) and returns lv_obj_t*
 *
 * @param cache Reference to the cached lv_obj_t* pointer
 * @param create_func Function that creates the overlay, returns nullptr on failure
 * @param parent Parent screen for overlay creation
 * @param error_msg Error message for toast notification (default: "Failed to create overlay")
 *
 * @return true if overlay was pushed, false on failure
 *
 * Example:
 * @code
 * void MyPanel::handle_settings_clicked() {
 *     lazy_push_overlay(settings_overlay_, [this](lv_obj_t* p) {
 *         auto* overlay = static_cast<lv_obj_t*>(lv_xml_create(p, "settings_panel", nullptr));
 *         if (overlay) setup_settings(overlay);
 *         return overlay;
 *     }, parent_screen_, "Failed to load settings");
 * }
 * @endcode
 */
template <typename CreateFunc>
bool lazy_push_overlay(lv_obj_t*& cache, CreateFunc create_func, lv_obj_t* parent,
                       const char* error_msg = "Failed to create overlay") {
    if (!cache && parent) {
        cache = create_func(parent);
        if (!cache) {
            spdlog::error("{}", error_msg);
            ToastManager::instance().show(ToastSeverity::ERROR, error_msg, 2000);
            return false;
        }
    }

    if (cache) {
        // Register before pushing, mirroring lazy_create_and_push_overlay(). The
        // caller supplies a bare widget with no lifecycle object, so a null
        // lifecycle is the correct registration — it marks the overlay as
        // intentionally lifecycle-less rather than accidentally unregistered.
        NavigationManager::instance().register_overlay_instance(cache, nullptr);
        NavigationManager::instance().push_overlay(cache);
        return true;
    }

    return false;
}

} // namespace helix::ui
