// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_retarget.h"

#include "ui_emergency_stop.h"

#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "i_moonraker_client.h"
#include "moonraker_manager.h"
#include "print_history_manager.h"
#include "printer_state.h"

#include <spdlog/spdlog.h>

#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

std::function<bool()>& connect_gate() {
    static std::function<bool()> gate;
    return gate;
}

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

/// False when the transport could not start, e.g. no internal RAM for its task.
bool connect_active_printer() {
    if (!connect_gate_open()) {
        spdlog::warn("[PrinterRetarget] Connect gate closed; staying disconnected");
        return false;
    }
    Config* config = Config::get_instance();
    const std::string host = config->get<std::string>(config->df() + "moonraker_host", "");
    const int port = config->get<int>(config->df() + "moonraker_port", 7125);

    const std::string ws_url = active_printer_ws_url();
    const std::string http_url = "http://" + host + ":" + std::to_string(port);

    spdlog::info("[PrinterRetarget] Connecting to {}:{}", host, port);
    if (get_moonraker_manager()->connect(ws_url, http_url) != 0) {
        spdlog::error("[PrinterRetarget] Connecting to {}:{} could not start", host, port);
        return false;
    }
    return true;
}

/// The new printer's first status replaces the live values; an unreachable one never sends
/// any, so the previous printer's job, message, temperatures and history are cleared here
/// rather than shown as the new printer's.
void forget_previous_printer() {
    PrinterState& ps = get_printer_state();
    nlohmann::json neutral = {
        {"print_stats", {{"state", "standby"}, {"filename", ""}, {"message", ""}}},
        {"display_status", {{"message", nullptr}, {"progress", 0.0}}},
        {"heater_bed", {{"temperature", 0.0}, {"target", 0.0}}}};
    for (const auto& [name, info] : ps.temperature_state().extruders()) {
        neutral[name] = {{"temperature", 0.0}, {"target", 0.0}};
    }
    ps.update_from_status(neutral);

    if (PrintHistoryManager* history = get_print_history_manager()) {
        history->forget_printer();
    }
}

} // namespace

std::string active_printer_ws_url() {
    Config* config = Config::get_instance();
    return "ws://" + config->get<std::string>(config->df() + "moonraker_host", "") + ":" +
           std::to_string(config->get<int>(config->df() + "moonraker_port", 7125)) + "/websocket";
}

void set_connect_gate(std::function<bool()> gate) {
    connect_gate() = std::move(gate);
}

bool connect_gate_open() {
    return !connect_gate() || connect_gate()();
}

bool reconnect_active_printer() {
    if (!disconnect_for_retarget()) {
        return false;
    }
    return connect_active_printer();
}

bool retarget_printer_connection() {
    if (!disconnect_for_retarget()) {
        return false;
    }

    // The old printer's queued frames apply now rather than on top of the new printer.
    get_moonraker_manager()->process_notifications();

    AmsState::instance().clear_backends();
    forget_previous_printer();
    get_printer_state().set_active_printer_name(Config::get_instance()->get_active_printer_name());

    return connect_active_printer();
}

} // namespace helix
