// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace helix {

/// Crash resets in a row, with no healthy session between them, that make boot stop
/// connecting to the active printer (prestonbrown/helixscreen#1750).
constexpr int BOOT_CRASH_FALLBACK_THRESHOLD = 3;

struct BootPrinterChoice {
    int crash_streak = 0;    ///< The streak to persist for the next boot.
    std::string fallback_id; ///< Printer to make active instead; empty keeps the active one.
    bool auto_connect = true;
};

/// Which printer a boot connects to. A crash reset adds one to the streak; a deliberate
/// restart or a power-on leaves it alone, and a healthy session clears it elsewhere. At the
/// threshold the boot falls back to the printer that was active before the last switch, or,
/// with none to go back to, does not connect at all. The streak restarts from zero either
/// way, so the panel tries again only after another full run of crashes.
inline BootPrinterChoice choose_boot_printer(bool crash_reset, int crash_streak,
                                             const std::string& active_id,
                                             const std::string& previous_id,
                                             const std::vector<std::string>& printer_ids) {
    if (!crash_reset) {
        return {crash_streak, {}, true};
    }
    if (crash_streak + 1 < BOOT_CRASH_FALLBACK_THRESHOLD) {
        return {crash_streak + 1, {}, true};
    }
    const bool previous_exists =
        std::find(printer_ids.begin(), printer_ids.end(), previous_id) != printer_ids.end();
    if (!previous_id.empty() && previous_id != active_id && previous_exists) {
        return {0, previous_id, true};
    }
    return {0, {}, false};
}

} // namespace helix
