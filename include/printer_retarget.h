// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>

namespace helix {

/// Reconnects the live client to the host and port the active printer's config names.
/// Main thread only. False when there is no client or manager, or the connect could not start.
bool reconnect_active_printer();

/// Points the live connection at the active printer as a different printer than the one it
/// served: disconnects, applies the old printer's queued notifications, drops its AMS
/// backends (discovery builds them only when none exist), shows the new printer's name, then
/// reconnects. `before_connect`, when given, runs after the disconnect and may veto the
/// connect by returning false. False when vetoed or the connect could not start. Main thread
/// only.
bool retarget_printer_connection(const std::function<bool()>& before_connect = nullptr);

} // namespace helix
