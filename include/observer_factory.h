// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file observer_factory.h
 * @brief Factory functions for creating type-safe LVGL observers with RAII cleanup
 *
 * Provides template-based observer creation that eliminates boilerplate callback code.
 * All observers return ObserverGuard for automatic cleanup.
 *
 * Each observer takes a lambda handler and comes in a deferred form (observe_int_sync,
 * observe_string), an immediate form (*_immediate) and, for ints, a value-now /
 * update-later form (observe_int_async). All of them forward to one type-erased
 * detail::observe_core() per value type. Domain helpers wrap them for print state and
 * language changes.
 */

#pragma once

#include "ui_observer_guard.h"
#include "ui_update_queue.h"

#include "connection_state.h"
#include "lvgl/lvgl.h"
#include "print_lifecycle_state.h" // PrintState, for observe_print_lifecycle
#include "printer_state.h"         // PrintJobState
#include "system_settings_manager.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace helix::ui {

/// How an observer's handler runs relative to the subject notification.
enum class Dispatch : uint8_t {
    Deferred,  ///< From the UpdateQueue after the notification (observe_int_sync, observe_string)
    Immediate, ///< Inside lv_subject notify (the *_immediate forms)
};

namespace detail {

// The observer machinery is compiled once per value type in src/ui/observer_factory.cpp.
// Each call site only binds its owner into a std::function; see observe_core() there for
// the context layout and the teardown invariant it depends on.

/// @p then_deferred, when set, is queued after every Immediate call (observe_int_async).
ObserverGuard observe_core(lv_subject_t* subject, const void* owner, std::function<void(int)> fn,
                           Dispatch dispatch, const SubjectLifetime& lifetime,
                           std::function<void()> then_deferred = nullptr);
ObserverGuard observe_core(lv_subject_t* subject, const void* owner,
                           std::function<void(const char*)> fn, Dispatch dispatch,
                           const SubjectLifetime& lifetime);

/// One handler instance serves every notification, so a `mutable` handler keeps its state.
template <typename V, typename Panel, typename Handler>
std::function<void(V)> bind_owner(Panel* panel, Handler&& handler) {
    return [panel, h = std::forward<Handler>(handler)](V value) mutable { h(panel, value); };
}

} // namespace detail

/**
 * @brief Create deferred int observer with custom lambda handler
 *
 * The handler is deferred via helix::ui::queue_update() to run after the current
 * subject notification completes. This prevents re-entrant observer
 * destruction crashes (issue #82). Safe default for all observer callbacks.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable type: void(Panel*, int)
 * @param subject LVGL subject to observe
 * @param panel Panel instance
 * @param handler Lambda called with panel and int value
 * @param lifetime Death signal for @p subject. Required: pass the subject
 *        owner's get_subjects_lifetime(), or subject_never_freed() only when
 *        the subject genuinely cannot be freed before process exit.
 * @return ObserverGuard for RAII cleanup; empty (with a warning) when @p subject
 *         or @p panel is null
 */
template <typename Panel, typename Handler>
ObserverGuard observe_int_sync(lv_subject_t* subject, Panel* panel, Handler&& handler,
                               const SubjectLifetime& lifetime) {
    return detail::observe_core(subject, panel,
                                detail::bind_owner<int>(panel, std::forward<Handler>(handler)),
                                Dispatch::Deferred, lifetime);
}

/**
 * @brief Create immediate (non-deferred) int observer with custom lambda handler
 *
 * The handler is called directly in the observer callback with no deferral.
 * Use ONLY when you are certain the callback will NOT modify observer lifecycle
 * (no observer reassignment, no widget destruction, no ObserverGuard mutation).
 * Prefer observe_int_sync() in all other cases.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable type: void(Panel*, int)
 */
template <typename Panel, typename Handler>
ObserverGuard observe_int_immediate(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                    const SubjectLifetime& lifetime) {
    return detail::observe_core(subject, panel,
                                detail::bind_owner<int>(panel, std::forward<Handler>(handler)),
                                Dispatch::Immediate, lifetime);
}

/**
 * @brief Create async int observer with value and update handlers
 *
 * Value handler is called synchronously, update handler via ui_queue_update().
 *
 * @tparam Panel Panel class type
 * @tparam ValueHandler Callable: void(Panel*, int)
 * @tparam UpdateHandler Callable: void(Panel*)
 */
template <typename Panel, typename ValueHandler, typename UpdateHandler>
ObserverGuard observe_int_async(lv_subject_t* subject, Panel* panel, ValueHandler&& value_handler,
                                UpdateHandler&& update_handler, const SubjectLifetime& lifetime) {
    return detail::observe_core(
        subject, panel, detail::bind_owner<int>(panel, std::forward<ValueHandler>(value_handler)),
        Dispatch::Immediate, lifetime,
        [panel, u = std::forward<UpdateHandler>(update_handler)]() mutable { u(panel); });
}

/**
 * @brief Create deferred string observer with custom lambda handler
 *
 * The handler is deferred via helix::ui::queue_update() to run after the current
 * subject notification completes. String value is copied to ensure validity.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, const char*)
 */
template <typename Panel, typename Handler>
ObserverGuard observe_string(lv_subject_t* subject, Panel* panel, Handler&& handler,
                             const SubjectLifetime& lifetime) {
    return detail::observe_core(
        subject, panel, detail::bind_owner<const char*>(panel, std::forward<Handler>(handler)),
        Dispatch::Deferred, lifetime);
}

/**
 * @brief Create immediate (non-deferred) string observer
 *
 * Use ONLY when the callback will NOT modify observer lifecycle.
 * Prefer observe_string() in all other cases.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, const char*)
 */
template <typename Panel, typename Handler>
ObserverGuard observe_string_immediate(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                       const SubjectLifetime& lifetime) {
    return detail::observe_core(
        subject, panel, detail::bind_owner<const char*>(panel, std::forward<Handler>(handler)),
        Dispatch::Immediate, lifetime);
}

// ============================================================================
// Domain-Specific Observer Helpers
// ============================================================================

/**
 * @brief Re-render C++-formatted text after every language switch
 *
 * XML re-translates what it bound through translation_tag. Text C++ produced
 * with lv_tr() was translated once, when it was set, and stays in the old
 * language until the owner formats it again: this is the trigger for that.
 * The handler runs deferred, so the new translation pack is already active,
 * and only on a real switch (not on registration).
 *
 * @tparam Panel Owner class type
 * @tparam OnChange Callable: void(Panel*)
 */
template <typename Panel, typename OnChange>
ObserverGuard observe_language_change(Panel* panel, OnChange&& on_change) {
    auto& settings = SystemSettingsManager::instance();
    lv_subject_t* language = settings.subject_language();
    // Held by pointer so the non-mutable handler can update it.
    auto shown = std::make_shared<int>(lv_subject_get_int(language));
    return observe_int_sync<Panel>(
        language, panel,
        [shown, on_change = std::forward<OnChange>(on_change)](Panel* p, int index) {
            if (index == *shown) {
                return;
            }
            *shown = index;
            on_change(p);
        },
        settings.get_subjects_lifetime());
}

/**
 * @brief Create print state observer with typed PrintJobState
 *
 * Common pattern used in 4+ files to react to print state changes.
 * Automatically casts the int subject value to PrintJobState enum.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, PrintJobState)
 * @param subject The RAW wire subject, `print_state_enum` — nothing else. This
 *        factory hard-casts to PrintJobState, and PrintState does not share its
 *        numbering past index 0, so handing it `print_lifecycle` compiles, runs,
 *        and answers a different question. Use observe_print_lifecycle() for
 *        that subject.
 * @param panel Panel instance
 * @param handler Lambda called with panel and typed PrintJobState
 * @param lifetime Death signal for @p subject. Required whenever the observing
 *        object can outlive the subject's owner — see observe_int_sync().
 *        print_state_enum belongs to PrinterState, so every caller that is not
 *        itself owned by PrinterState wants one.
 * @return ObserverGuard for RAII cleanup
 */
template <typename Panel, typename Handler>
ObserverGuard observe_print_state(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                  const SubjectLifetime& lifetime) {
    return observe_int_sync<Panel>(
        subject, panel,
        [handler = std::forward<Handler>(handler)](Panel* p, int state_int) {
            handler(p, static_cast<PrintJobState>(state_int));
        },
        lifetime);
}

/**
 * @brief observe_print_state(), but firing SYNCHRONOUSLY like observe_int_immediate().
 *
 * Typing and dispatch mode are orthogonal, and a family that covers only one
 * dispatch mode is a trap: swapping observe_int_immediate() for
 * observe_print_state() to gain the typing silently moves the handler onto the
 * UpdateQueue. AbortManager's cancel detection reads the resulting state in the
 * same turn, so deferring it broke two tests the moment that swap was tried.
 *
 * Prefer the deferred observe_print_state() unless the caller genuinely needs
 * the value before returning.
 */
template <typename Panel, typename Handler>
ObserverGuard observe_print_state_immediate(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                            const SubjectLifetime& lifetime) {
    return observe_int_immediate<Panel>(
        subject, panel,
        [handler = std::forward<Handler>(handler)](Panel* p, int state_int) {
            handler(p, static_cast<PrintJobState>(state_int));
        },
        lifetime);
}

/**
 * @brief Create a print lifecycle observer with typed PrintState
 *
 * The derived-lifecycle sibling of observe_print_state(). Consumers asking a
 * capability question — "does a job own the machine right now?" — want this one,
 * because it is the only axis that can express a start the app has committed to
 * but the printer has not reported yet.
 *
 * @tparam Panel Panel class type
 * @tparam Handler Callable: void(Panel*, PrintState)
 * @param subject The derived lifecycle subject, `print_lifecycle` — nothing else.
 *        See the warning on observe_print_state(): the two enums do not share
 *        numbering, and passing the wrong subject is silent.
 * @param panel Panel instance
 * @param handler Lambda called with panel and typed PrintState
 * @param lifetime Death signal for @p subject. Same rule as observe_int_sync():
 *        print_lifecycle belongs to PrinterState, so every caller not owned by
 *        PrinterState wants one.
 * @return ObserverGuard for RAII cleanup
 */
template <typename Panel, typename Handler>
ObserverGuard observe_print_lifecycle(lv_subject_t* subject, Panel* panel, Handler&& handler,
                                      const SubjectLifetime& lifetime) {
    return observe_int_sync<Panel>(
        subject, panel,
        [handler = std::forward<Handler>(handler)](Panel* p, int state_int) {
            handler(p, static_cast<PrintState>(state_int));
        },
        lifetime);
}

} // namespace helix::ui
