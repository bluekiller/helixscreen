// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

namespace helix {

/// Installs the check every connect below, and Change Host's Test Connection, makes after its
/// disconnect. The K-Touch waits there for the stopped WebSocket task's internal stack. With
/// none installed, connects go ahead.
void set_connect_gate(std::function<bool()> gate);

/// True when no gate is installed or the installed one allows a connect now.
bool connect_gate_open();

/// Reconnects the live client to the host and port the active printer's config names.
/// Main thread only. False when there is no client or manager, the gate is closed, or the
/// connect could not start.
bool reconnect_active_printer();

/// Points the live connection at the active printer as a different printer than the one it
/// served: disconnects, applies the old printer's queued notifications, drops its AMS
/// backends (discovery builds them only when none exist), shows the new printer's name, then
/// reconnects. False when the gate is closed or the connect could not start. Main thread only.
bool retarget_printer_connection();

} // namespace helix
