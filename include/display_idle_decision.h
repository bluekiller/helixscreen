// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace helix {

/// Where the idle state machine currently sits. Sleeping outranks Dimmed.
enum class IdleState { Awake, Dimmed, Sleeping };

/// What the tick should do about it.
enum class IdleAction {
    None,
    Wake,       ///< Dimmed or sleeping, and the user is back (or the host resumed).
    Dim,        ///< Awake -> Dimmed by lowering the backlight only.
    StartSaver, ///< Awake -> Dimmed by starting the screensaver.
    Sleep,      ///< Awake or Dimmed -> Sleeping.
};

struct IdleInputs {
    IdleState state = IdleState::Awake;
    uint32_t inactive_ms = 0;
    int dim_timeout_sec = 0;   ///< 0 = never dim
    int sleep_timeout_sec = 0; ///< 0 = never sleep
    /// Sleep-while-printing is off and a job holds the machine: entering sleep or dim is
    /// refused, but waking is not.
    bool inhibit_entry = false;
    bool can_dim = false;
    bool has_screensaver = false;
    bool saver_running = false;
    bool wake_requested = false; ///< The input wrapper saw a press while asleep or dimmed.
    bool host_resumed = false;   ///< The host powered the panel back on (Android).
    bool is_preview = false;     ///< The running screensaver is a settings preview.
    uint32_t preview_elapsed_ms = 0;
};

struct IdleDecision {
    IdleAction action = IdleAction::None;
};

/// Activity means input within this many milliseconds.
constexpr uint32_t IDLE_ACTIVITY_WINDOW_MS = 500;
/// A preview ignores activity this long, so the click that launched it cannot close it.
constexpr uint32_t PREVIEW_GRACE_MS = 750;

inline IdleDecision decide_idle(const IdleInputs& in) {
    const uint32_t dim_timeout_ms =
        (in.dim_timeout_sec > 0) ? static_cast<uint32_t>(in.dim_timeout_sec) * 1000U : UINT32_MAX;
    const uint32_t sleep_timeout_ms = (in.sleep_timeout_sec > 0)
                                          ? static_cast<uint32_t>(in.sleep_timeout_sec) * 1000U
                                          : UINT32_MAX;
    const bool activity = in.inactive_ms < IDLE_ACTIVITY_WINDOW_MS;

    if (in.state == IdleState::Sleeping) {
        if (in.wake_requested || activity || in.host_resumed) {
            return {IdleAction::Wake};
        }
        return {IdleAction::None};
    }

    if (in.state == IdleState::Dimmed) {
        bool dismiss_on_activity = activity;
        if (in.is_preview && in.preview_elapsed_ms < PREVIEW_GRACE_MS) {
            dismiss_on_activity = false;
        }
        if (in.wake_requested || dismiss_on_activity) {
            return {IdleAction::Wake};
        }
        if (!in.inhibit_entry && in.sleep_timeout_sec > 0 && in.inactive_ms >= sleep_timeout_ms) {
            return {IdleAction::Sleep};
        }
        return {IdleAction::None};
    }

    if (in.inhibit_entry) {
        return {IdleAction::None};
    }
    if (in.sleep_timeout_sec > 0 && in.inactive_ms >= sleep_timeout_ms) {
        return {IdleAction::Sleep};
    }
    if (in.dim_timeout_sec > 0 && in.inactive_ms >= dim_timeout_ms &&
        (in.can_dim || in.has_screensaver)) {
        return {(!in.saver_running && in.has_screensaver) ? IdleAction::StartSaver
                                                          : IdleAction::Dim};
    }
    return {IdleAction::None};
}

} // namespace helix
