// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lvgl.h"

#include <spdlog/spdlog.h>

#include <exception>

// A handler's exception is logged, not let through LVGL's C dispatch frame.
// Without exceptions (the ESP32 build) the guard is a plain block.
#if defined(__cpp_exceptions)
#define HELIX_TRAMPOLINE_GUARD_BEGIN try {
#define HELIX_TRAMPOLINE_GUARD_END(PanelClass, name)                                               \
    }                                                                                              \
    catch (const std::exception& ex) {                                                             \
        spdlog::error("[" #PanelClass "] Exception in on_" #name ": {}", ex.what());               \
    }                                                                                              \
    catch (...) {                                                                                  \
        spdlog::error("[" #PanelClass "] Unknown exception in on_" #name);                         \
    }
#else
#define HELIX_TRAMPOLINE_GUARD_BEGIN {
#define HELIX_TRAMPOLINE_GUARD_END(PanelClass, name) }
#endif

/**
 * @file ui_event_trampoline.h
 * @brief Macros to reduce boilerplate for LVGL event callback trampolines
 *
 * These macros define static member function trampolines that delegate to instance methods.
 * They are designed for out-of-class definitions of static member functions declared in headers.
 */

/**
 * @brief Define a static member function trampoline that delegates to an instance method
 *
 * Reduces the repetitive pattern:
 *   void Class::callback(lv_event_t* e) {
 *       auto* self = static_cast<Class*>(lv_event_get_user_data(e));
 *       if (self) { self->handler(e); }
 *   }
 *
 * To a single line:
 *   DEFINE_EVENT_TRAMPOLINE(Class, callback, handler)
 *
 * The handler method receives lv_event_t* as parameter.
 *
 * @param ClassName The class type to cast to
 * @param callback_name Name for the static callback function (will be prefixed with ClassName::)
 * @param handler_method Instance method to call (must take lv_event_t*)
 */
#define DEFINE_EVENT_TRAMPOLINE(ClassName, callback_name, handler_method)                          \
    void ClassName::callback_name(lv_event_t* e) {                                                 \
        auto* self = static_cast<ClassName*>(lv_event_get_user_data(e));                           \
        if (self) {                                                                                \
            self->handler_method(e);                                                               \
        }                                                                                          \
    }

/**
 * @brief Variant for handlers that don't need the event parameter
 *
 * For simple handlers like handle_click() that don't use the event:
 *   DEFINE_EVENT_TRAMPOLINE_SIMPLE(Class, on_click, handle_click)
 *
 * @param ClassName The class type to cast to
 * @param callback_name Name for the static callback function (will be prefixed with ClassName::)
 * @param handler_method Instance method to call (takes no parameters)
 */
#define DEFINE_EVENT_TRAMPOLINE_SIMPLE(ClassName, callback_name, handler_method)                   \
    void ClassName::callback_name(lv_event_t* e) {                                                 \
        auto* self = static_cast<ClassName*>(lv_event_get_user_data(e));                           \
        if (self) {                                                                                \
            self->handler_method();                                                                \
        }                                                                                          \
    }

// =============================================================================
// PANEL TRAMPOLINE MACROS (with exception safety)
// =============================================================================
// These macros combine the trampoline pattern with LVGL_SAFE_EVENT_CB_BEGIN/END
// for use in panel classes. They reduce the common 5-line pattern to a single line.

/**
 * @brief Define a panel trampoline for XML event callbacks using global accessor
 *
 * Replaces the repetitive pattern:
 *   void PanelClass::on_foo_clicked(lv_event_t* e) {
 *       LVGL_SAFE_EVENT_CB_BEGIN("[PanelClass] on_foo_clicked");
 *       (void)e;
 *       get_global_panel().handle_foo_clicked();
 *       LVGL_SAFE_EVENT_CB_END();
 *   }
 *
 * With a single line:
 *   PANEL_TRAMPOLINE(PanelClass, get_global_panel, foo_clicked)
 *
 * Naming convention: callback is on_<name>, handler is handle_<name>
 *
 * @param PanelClass The panel class type
 * @param getter_func Global accessor function that returns PanelClass&
 * @param name Base name without on_/handle_ prefix (e.g., "foo_clicked")
 */
#define PANEL_TRAMPOLINE(PanelClass, getter_func, name)                                            \
    void PanelClass::on_##name(lv_event_t* e) {                                                    \
        HELIX_TRAMPOLINE_GUARD_BEGIN(void) e;                                                      \
        getter_func().handle_##name();                                                             \
        HELIX_TRAMPOLINE_GUARD_END(PanelClass, name)                                               \
    }

/**
 * @brief Define a panel trampoline using user_data for instance lookup
 *
 * Replaces the pattern used for modal/dialog callbacks:
 *   void PanelClass::on_foo_confirm(lv_event_t* e) {
 *       LVGL_SAFE_EVENT_CB_BEGIN("[PanelClass] on_foo_confirm");
 *       auto* self = static_cast<PanelClass*>(lv_event_get_user_data(e));
 *       if (self) { self->handle_foo_confirm(); }
 *       LVGL_SAFE_EVENT_CB_END();
 *   }
 *
 * With a single line:
 *   PANEL_TRAMPOLINE_USERDATA(PanelClass, foo_confirm)
 *
 * @param PanelClass The panel class type
 * @param name Base name without on_/handle_ prefix
 */
#define PANEL_TRAMPOLINE_USERDATA(PanelClass, name)                                                \
    void PanelClass::on_##name(lv_event_t* e) {                                                    \
        HELIX_TRAMPOLINE_GUARD_BEGIN                                                               \
        if (!e)                                                                                    \
            return;                                                                                \
        auto* self = static_cast<PanelClass*>(lv_event_get_user_data(e));                          \
        if (self) {                                                                                \
            self->handle_##name();                                                                 \
        }                                                                                          \
        HELIX_TRAMPOLINE_GUARD_END(PanelClass, name)                                               \
    }
