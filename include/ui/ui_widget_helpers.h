// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file ui_widget_helpers.h
 * @brief Helper macros for LVGL widget lookup with automatic error logging
 *
 * Reduces boilerplate for the common pattern of looking up a widget by name
 * and logging a warning if it's not found. This pattern is repeated 74+ times
 * across the codebase.
 *
 * Pattern replaced:
 * @code
 * auto* btn = lv_obj_find_by_name(root_, "my_button");
 * if (!btn) {
 *     spdlog::warn("[MyPanel] Widget 'my_button' not found");
 * }
 * @endcode
 *
 * New usage:
 * @code
 * lv_obj_t* btn;
 * FIND_WIDGET(btn, root_, "my_button", get_name());
 * @endcode
 */

#pragma once

#include <spdlog/spdlog.h>

// Include LVGL for lv_obj_t and lv_obj_find_by_name
// This ensures we get the correct type declarations
#include "lvgl/lvgl.h"

/**
 * @brief Look up a widget by name and log a warning if not found
 *
 * Assigns the result to @p var after looking up @p name in @p parent.
 * If the widget is not found, logs a warning with the panel name and widget name.
 *
 * @param var     The lv_obj_t* variable to assign to (must be declared)
 * @param parent  The parent object to search in
 * @param name    The widget name to find (string literal)
 * @param panel   The panel/component name for logging (typically get_name())
 *
 * Example:
 * @code
 * lv_obj_t* btn;
 * FIND_WIDGET(btn, panel_, "my_button", get_name());
 * if (btn) {
 *     // Use the button
 * }
 * @endcode
 *
 * @note The variable must be declared before the macro. This allows
 *       the caller to control the variable's scope and type qualifiers.
 */
#define FIND_WIDGET(var, parent, name, panel)                                                      \
    do {                                                                                           \
        (var) = lv_obj_find_by_name((parent), (name));                                             \
        if (!(var)) {                                                                              \
            spdlog::warn("[{}] Widget '{}' not found", (panel), (name));                           \
        }                                                                                          \
    } while (0)

/**
 * @brief Look up a widget by name and log an error if not found
 *
 * Same as FIND_WIDGET but uses error-level logging for critical widgets.
 * Use this for widgets that are required for the panel to function.
 *
 * @param var     The lv_obj_t* variable to assign to (must be declared)
 * @param parent  The parent object to search in
 * @param name    The widget name to find (string literal)
 * @param panel   The panel/component name for logging (typically get_name())
 *
 * Example:
 * @code
 * lv_obj_t* required_content;
 * FIND_WIDGET_REQUIRED(required_content, overlay_root_, "overlay_content", get_name());
 * if (!required_content) {
 *     return;  // Cannot proceed without this widget
 * }
 * @endcode
 */
#define FIND_WIDGET_REQUIRED(var, parent, name, panel)                                             \
    do {                                                                                           \
        (var) = lv_obj_find_by_name((parent), (name));                                             \
        if (!(var)) {                                                                              \
            spdlog::error("[{}] Widget '{}' not found!", (panel), (name));                         \
        }                                                                                          \
    } while (0)

/**
 * @brief Look up a widget silently (no logging on failure)
 *
 * Use this for optional widgets where absence is expected and not an error.
 * The caller can check the result and handle accordingly.
 *
 * @param var     The lv_obj_t* variable to assign to (must be declared)
 * @param parent  The parent object to search in
 * @param name    The widget name to find (string literal)
 *
 * Example:
 * @code
 * lv_obj_t* optional_header;
 * FIND_WIDGET_OPTIONAL(optional_header, root_, "optional_header");
 * // No warning if not found - this widget may not exist in all layouts
 * @endcode
 */
#define FIND_WIDGET_OPTIONAL(var, parent, name) (var) = lv_obj_find_by_name((parent), (name))

namespace helix::ui {

/**
 * @brief Make a breached UI contract fatal
 *
 * On in unit tests and under --test: a breach prints to stderr and aborts, so
 * the run fails where the XML and the C++ disagree. Off (the default) it only
 * logs. Release builds never abort.
 */
void set_strict_ui_checks(bool enabled) noexcept;

/// Log @p message at error, then abort when strict UI checks are on.
void report_ui_contract_breach(const char* message);

/**
 * @brief A widget the component's XML must contain
 *
 * A missing name logs once per (owner, name) at error and returns nullptr; with
 * strict UI checks on it aborts. A null @p root returns nullptr silently, so a
 * lookup nested under a failed one does not report twice.
 * scripts/check_required_names.py checks each literal @p name against every
 * layout variant of the component the calling file creates.
 */
lv_obj_t* find_required(lv_obj_t* root, const char* name, const char* owner);

/// A widget that may legitimately be absent (inside <if>, omitted by a layout
/// variant, or in plugin-supplied XML). Never reports.
inline lv_obj_t* find_optional(lv_obj_t* root, const char* name) {
    return root ? lv_obj_find_by_name(root, name) : nullptr;
}

} // namespace helix::ui

/**
 * @brief Toggle a widget's enabled state with visual feedback
 *
 * Adds/removes LV_STATE_DISABLED and sets main-part opacity to LV_OPA_COVER
 * when enabled or LV_OPA_50 when disabled — the conventional "button greyed
 * out" appearance used across the UI.
 *
 * Safe to call with a null widget (no-op).
 */
inline void ui_set_button_enabled(lv_obj_t* btn, bool enabled) {
    if (!btn)
        return;
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_50, LV_PART_MAIN);
    }
}
