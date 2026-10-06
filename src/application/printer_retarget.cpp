// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_retarget.h"

#include "ui_emergency_stop.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "moonraker_manager.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix {

namespace {

/// The disconnect looks exactly like an unexpected drop; suppress the recovery dialog so an
/// intentional one doesn't raise it.
IMoonrakerClient* disconnect_for_retarget() {
    IMoonrakerClient* client = get_moonraker_client();
    if (!client || !get_moonraker_manager()) {
        spdlog::error("[PrinterRetarget] Cannot reconnect - client or manager unavailable");
        return nullptr;
    }
    EmergencyStopOverlay::instance().suppress_recovery_dialog(RecoverySuppression::SHORT);
    client->disconnect();
    return client;
}

void connect_active_printer() {
    Config* config = Config::get_instance();
    const std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
    const int port = config->get<int>(config->df() + "moonraker_port", 7125);

    const std::string ws_url = "ws://" + host + ":" + std::to_string(port) + "/websocket";
    const std::string http_url = "http://" + host + ":" + std::to_string(port);

    spdlog::info("[PrinterRetarget] Connecting to {}:{}", host, port);
    get_moonraker_manager()->connect(ws_url, http_url);
}

} // namespace

bool reconnect_active_printer() {
    if (!disconnect_for_retarget()) {
        return false;
    }
    connect_active_printer();
    return true;
}

bool retarget_printer_connection(const std::function<bool()>& before_connect) {
    if (!disconnect_for_retarget()) {
        return false;
    }

    // The old printer's queued frames apply now rather than on top of the new printer.
    get_moonraker_manager()->process_notifications();

    AmsState::instance().clear_backends();
    get_printer_state().set_active_printer_name(Config::get_instance()->get_active_printer_name());

    if (before_connect && !before_connect()) {
        spdlog::warn("[PrinterRetarget] Connect vetoed; staying disconnected");
        return false;
    }
    connect_active_printer();
    return true;
}

} // namespace helix
