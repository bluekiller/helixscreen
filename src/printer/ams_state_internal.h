// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers shared by the files AmsState's definitions are split across. Not a
// public header: nothing outside src/printer/ams_state*.cpp includes it.

#pragma once

#include "ams_types.h"

namespace helix::ams_state_detail {

/// Everything AmsState holds outside the registry, RunoutGrace and its atomics
/// is main-thread state with no lock; an off-main caller is a bug.
void assert_main_thread();

/// True once the singleton is being destroyed. Work queued to the main thread
/// checks it before touching AmsState.
bool shutting_down();

/// The error state a lane bar's status line draws from: the same derivation
/// both current consumers (AMS overview mini bars, mini status) compute from
/// SlotInfo. has_error covers a carried SlotError AND a BLOCKED lane;
/// severity falls back to INFO when no error object is carried.
inline void slot_error_state(const SlotInfo& slot, bool& has_error, int& severity) {
    has_error = (slot.status == SlotStatus::BLOCKED || slot.error.has_value());
    severity =
        static_cast<int>(slot.error.has_value() ? slot.error->severity : SlotError::Severity::INFO);
}

} // namespace helix::ams_state_detail
