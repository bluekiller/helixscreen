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
 * update-later form (observe_int_async). Domain helpers wrap them for print state and
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

#include <memory>
#include <string>
#include <type_traits>

namespace helix::ui {

// ============================================================================
// Lambda-based API
// ============================================================================

namespace detail {

/**
 * @brief Context for lambda-based observers
 *
 * The `alive` token lets deferred lambdas (queued via queue_update) detect
 * when the observer has been destroyed between queueing and execution.
 * Without this, the copied panel pointer in the deferred lambda becomes
 * dangling if the panel is destroyed during a widget rebuild.
 * See issue #174: SEGV in LedWidget::update_light_icon() via stale pointer.
 */
template <typename Panel, typename Handler> struct LambdaObserverContext {
    Panel* panel;
    Handler handler;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

/**
 * @brief Context for async lambda observers with update handler
 */
template <typename Panel, typename ValueHandler, typename UpdateHandler>
struct AsyncLambdaObserverContext {
    Panel* panel;
    ValueHandler value_handler;
    UpdateHandler update_handler;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

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
 * @return ObserverGuard for RAII cleanup
 */
template <typename Panel, typename Handler>
ObserverGuard observe_int_sync(lv_subject_t* subject, Panel* panel, Handler&& handler,
                               const SubjectLifetime& lifetime) {
    if (!subject || !panel) {
        return ObserverGuard();
    }

    using DecayedHandler = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, DecayedHandler>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, DecayedHandler>*>(
                lv_observer_get_user_data(obs));
            // LOAD-BEARING INVARIANT: this synchronous body has NO defense
            // against a freed `c`. The `if (c && c->panel)` check is not one —
            // freed-but-not-yet-reused heap still reads a plausible non-null
            // panel, so we fall through and copy c->handler (a shared_ptr
            // refcount bump) on freed memory → SIGSEGV in __aarch64_ldadd4 (the
            // crash in bundles 449TVQ82/X3RA4252). The weak_alive/lifetime
            // guards below protect only the DEFERRED lambda, not this copy.
            // Safety therefore depends entirely on ObserverGuard::reset()
            // removing this observer from the subject BEFORE freeing `c` (its
            // `delete ctx` cleanup). reset() guarantees that ordering for any
            // observer on a live subject; the per-creation invalidation epoch
            // (ui_observer_guard.h) ensures an observer created during a
            // printer-state reinit window is still removed rather than orphaned.
            if (c && c->panel) {
                int value = lv_subject_get_int(subj);
                // Copy handler and panel pointer so the deferred lambda is
                // self-contained and safe even if the observer context is
                // destroyed before execution (the exact crash in issue #82).
                // The weak alive token detects when the observer (and thus
                // the panel) has been destroyed between queueing and
                // execution (issue #174).
                auto handler_copy = c->handler;
                auto* panel_ptr = c->panel;
                std::weak_ptr<bool> weak_alive = c->alive;
                helix::ui::queue_update("observe_int_sync::apply",
                                        [handler_copy, panel_ptr, value, weak_alive]() {
                                            if (weak_alive.expired())
                                                return;
                                            handler_copy(panel_ptr, value);
                                        });
            }
        },
        ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
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
    if (!subject || !panel) {
        return ObserverGuard();
    }

    using DecayedHandler = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, DecayedHandler>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, DecayedHandler>*>(
                lv_observer_get_user_data(obs));
            if (c && c->panel) {
                int value = lv_subject_get_int(subj);
                c->handler(c->panel, value);
            }
        },
        ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
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
    if (!subject || !panel) {
        return ObserverGuard();
    }

    using DecayedValueHandler = std::decay_t<ValueHandler>;
    using DecayedUpdateHandler = std::decay_t<UpdateHandler>;
    auto* ctx =
        new detail::AsyncLambdaObserverContext<Panel, DecayedValueHandler, DecayedUpdateHandler>{
            panel, std::forward<ValueHandler>(value_handler),
            std::forward<UpdateHandler>(update_handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::AsyncLambdaObserverContext<Panel, DecayedValueHandler,
                                                                     DecayedUpdateHandler>*>(
                lv_observer_get_user_data(obs));
            if (c && c->panel) {
                int value = lv_subject_get_int(subj);
                c->value_handler(c->panel, value);

                // Schedule async update with alive guard to prevent
                // use-after-free if ObserverGuard is destroyed before execution
                auto* panel_ptr = c->panel;
                auto update_copy = c->update_handler;
                std::weak_ptr<bool> weak_alive = c->alive;
                helix::ui::queue_update([panel_ptr, update_copy, weak_alive]() {
                    if (weak_alive.expired())
                        return;
                    update_copy(panel_ptr);
                });
            }
        },
        ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
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
    if (!subject || !panel) {
        return ObserverGuard();
    }

    using DecayedHandler = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, DecayedHandler>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, DecayedHandler>*>(
                lv_observer_get_user_data(obs));
            if (c && c->panel) {
                const char* str = lv_subject_get_string(subj);
                std::string str_copy = str ? str : "";
                auto handler_copy = c->handler;
                auto* panel_ptr = c->panel;
                std::weak_ptr<bool> weak_alive = c->alive;
                helix::ui::queue_update("observe_string_sync::apply",
                                        [handler_copy, panel_ptr, str_copy, weak_alive]() {
                                            if (weak_alive.expired())
                                                return;
                                            handler_copy(panel_ptr, str_copy.c_str());
                                        });
            }
        },
        ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
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
    if (!subject || !panel) {
        return ObserverGuard();
    }

    using DecayedHandler = std::decay_t<Handler>;
    auto* ctx = new detail::LambdaObserverContext<Panel, DecayedHandler>{
        panel, std::forward<Handler>(handler)};

    ObserverGuard guard(
        subject,
        [](lv_observer_t* obs, lv_subject_t* subj) {
            auto* c = static_cast<detail::LambdaObserverContext<Panel, DecayedHandler>*>(
                lv_observer_get_user_data(obs));
            if (c && c->panel) {
                const char* str = lv_subject_get_string(subj);
                if (!str)
                    str = "";
                c->handler(c->panel, str);
            }
        },
        ctx, [ctx]() { delete ctx; });
    if (lifetime) {
        guard.set_alive_token(lifetime);
    }
    return guard;
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
    // Shared, because the factory copies the handler for each notification.
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
