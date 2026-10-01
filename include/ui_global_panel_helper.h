// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "exception_policy.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>

/**
 * @brief Define overlay global storage with strict getter (requires init)
 *
 * Unlike helix::lazy_global, which constructs on first use, this requires an
 * explicit init. Use for overlays that need constructor arguments.
 *
 * @code
 * // In .cpp file:
 * DEFINE_GLOBAL_OVERLAY_STORAGE(FanControlOverlay, g_fan_control, get_fan_control_overlay)
 *
 * void init_fan_control_overlay(PrinterState& state) {
 *     INIT_GLOBAL_OVERLAY(FanControlOverlay, g_fan_control, state);
 * }
 * @endcode
 */
#define DEFINE_GLOBAL_OVERLAY_STORAGE(OverlayType, global_var, getter_func)                        \
    static std::unique_ptr<OverlayType> global_var;                                                \
    OverlayType& getter_func() {                                                                   \
        if (!global_var) {                                                                         \
            spdlog::error("[" #OverlayType "] Called before initialization!");                     \
            helix::throw_or_abort(std::runtime_error(#OverlayType " not initialized"));            \
        }                                                                                          \
        return *global_var;                                                                        \
    }

/**
 * @brief Initialize overlay in init function body
 *
 * Use inside your init function to create the overlay instance.
 * Handles double-init warning and registry cleanup registration.
 */
#define INIT_GLOBAL_OVERLAY(OverlayType, global_var, ...)                                          \
    do {                                                                                           \
        if (global_var) {                                                                          \
            spdlog::warn("[" #OverlayType "] Already initialized, skipping");                      \
            return;                                                                                \
        }                                                                                          \
        global_var = std::make_unique<OverlayType>(__VA_ARGS__);                                   \
        StaticPanelRegistry::instance().register_destroy(#OverlayType,                             \
                                                         []() { global_var.reset(); });            \
        spdlog::debug("[" #OverlayType "] Global instance initialized");                           \
    } while (0)
