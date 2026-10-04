// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_response_routing.h"

#include "ui_toast_manager.h"
#include "ui_update_queue.h"

#include "action_prompt_manager.h"
#include "action_prompt_modal.h"
#include "ams_error_bridge.h"
#include "ams_state.h"
#include "app_globals.h"
#include "async_lifetime_guard.h"
#include "gcode_error_router.h"
#include "gcode_narration_router.h"
#include "gcode_response_lines.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "lan_client_auth_router.h"
#include "moonraker_error.h"
#include "printer_state.h"
#include "recovery_modal_presenter.h"

#include <spdlog/spdlog.h>

namespace helix {

GcodeResponseRouting::GcodeResponseRouting() = default;
GcodeResponseRouting::~GcodeResponseRouting() = default;

void GcodeResponseRouting::attach(IMoonrakerClient* client, IMoonrakerAPI* api,
                                  AsyncLifetimeGuard& async) {
    m_async = &async;

    // Create ActionPromptManager and register global instance for cross-TU access
    m_action_prompt_manager = std::make_unique<ActionPromptManager>();
    ActionPromptManager::set_instance(m_action_prompt_manager.get());

    m_action_prompt_modal = std::make_unique<ui::ActionPromptModal>();

    // Set up gcode callback to send button commands via API
    if (api) {
        m_action_prompt_modal->set_gcode_callback([api](const std::string& gcode) {
            spdlog::info("[ActionPrompt] Sending gcode: {}", gcode);
            // Action-prompt buttons run firmware macros (IFS load/unload, color
            // changes, tool changes) that routinely exceed the 60s default
            // request timeout: heat + cut + multi-stage feed/retract/purge can
            // take minutes. The macro timeout keeps a slow-but-progressing
            // operation from being reported as failed or stalled.
            api->execute_gcode(
                gcode, []() { spdlog::debug("[ActionPrompt] Gcode executed successfully"); },
                [gcode](const MoonrakerError& err) {
                    spdlog::error("[ActionPrompt] Gcode execution failed: {}", err.message);
                    // The modal already closed on the button press, and this
                    // error_cb marks the call caller-handled so the `!!`
                    // GcodeError toast is suppressed for the same failure.
                    // Without this the user sees nothing at all.
                    ui::report_action_prompt_gcode_failure(err.user_message());
                },
                IMoonrakerAPI::MACRO_TIMEOUT_MS);
        });
    }

    // Wire on_show callback to display modal (uses ui_queue_update() for thread safety)
    m_action_prompt_manager->set_on_show([this](const PromptData& data) {
        spdlog::info("[ActionPrompt] Showing prompt: {}", data.title);
        // WebSocket callbacks run on background thread - must use ui_queue_update
        m_async->defer("GcodeResponseRouting::action_prompt_show", [this, data]() {
            lv_obj_t* screen = lv_screen_active();
            if (m_action_prompt_modal && screen) {
                m_action_prompt_modal->show_prompt(screen, data);
            }
        });
    });

    // Wire on_close callback to hide modal
    m_action_prompt_manager->set_on_close([this]() {
        spdlog::info("[ActionPrompt] Closing prompt");
        m_async->defer("GcodeResponseRouting::action_prompt_close", [this]() {
            if (m_action_prompt_modal) {
                m_action_prompt_modal->hide();
            }
        });
    });

    // Wire on_notify callback for standalone notifications (action:notify)
    m_action_prompt_manager->set_on_notify([](const std::string& message) {
        spdlog::info("[ActionPrompt] Notification: {}", message);
        ui::queue_update([message]() {
            ToastManager::instance().show(ToastSeverity::INFO, message.c_str(), 5000);
        });
    });

    // Allow mock AMS backends to inject action_prompt lines (e.g., calibration wizard)
    auto* prompt_mgr = m_action_prompt_manager.get();
    AmsState::instance().set_gcode_response_callback(
        [prompt_mgr](const std::string& line) { prompt_mgr->process_line(line); });

    // Every line of G-code console output arrives through notify_gcode_response
    client->register_method_callback(
        "notify_gcode_response", "action_prompt_manager", [this](const nlohmann::json& msg) {
            for_each_gcode_response_line(msg, [this](const std::string& line) {
                m_action_prompt_manager->process_line(line);
            });
        });

    // Recovery modal presenter: source-agnostic owner of the CRITICAL recovery
    // modal (AFC jam, CFS key840, etc.). Created before GcodeErrorRouter so the
    // presenter outlives the router if both are reset in the wrong order.
    // Rebuilt with each printer scope; the modal does not outlive it.
    m_recovery_presenter = std::make_unique<ui::RecoveryModalPresenter>(api);

    // Klipper `!!` / `Error:` lines flow through GcodeErrorRouter, which
    // also replays the most recent gcode_store error when the WS (re)connects
    // (catches errors that fired while HelixScreen was offline). Its dtor
    // unregisters callbacks, so it must die before the MoonrakerClient.
    m_gcode_error_router = std::make_unique<GcodeErrorRouter>(api, client, *m_recovery_presenter);

    // Narration router: maps `//` toolchange narration to the active AMS
    // backend's step model and drives the toolchange_step subject. Separate
    // handler key from the error router; ignores `!!` / `Error:` lines.
    m_gcode_narration_router = std::make_unique<GcodeNarrationRouter>(api, client);

    // Firmware-brokered LAN pairing: on machines whose firmware asks the
    // printer's own screen to approve a slicer or phone app before letting it
    // in, HelixScreen is that screen. Inert on firmwares that never send one;
    // the notification is its own capability probe.
    m_lan_client_auth_router = std::make_unique<LanClientAuthRouter>(client);

    // AMS error bridge: observes AmsState's action subject and surfaces
    // AmsAction::ERROR from STATUS-driven backends (IFS, QIDI, etc.) via the
    // recovery modal. Complements GcodeErrorRouter which handles `!!` lines.
    m_ams_error_bridge = std::make_unique<AmsErrorBridge>(*m_recovery_presenter);
    m_ams_error_bridge->start();

    // Layer tracking fallback: some slicers don't emit SET_PRINT_STATS_INFO, so
    // print_stats.info never updates current_layer. The response lines carry it.
    client->register_method_callback(
        "notify_gcode_response", "layer_tracker", [](const nlohmann::json& msg) {
            // RAW_PRINT_STATE_OK: layer tracking. There are no layers during a
            // preparing window, and admitting one would derive a layer number
            // from the pre-print block's own Z moves.
            auto job_state = get_printer_state().get_print_job_state();
            if (job_state != PrintJobState::PRINTING && job_state != PrintJobState::PAUSED) {
                return;
            }

            for_each_gcode_response_line(msg, [](const std::string& line) {
                const auto parsed = parse_layer_line(line);
                if (parsed.current >= 0) {
                    spdlog::debug("[LayerTracker] Layer {} from gcode response: {}", parsed.current,
                                  line);
                    get_printer_state().set_print_layer_current(parsed.current);
                }
                if (parsed.total >= 0) {
                    spdlog::debug("[LayerTracker] Total layers {} from gcode response",
                                  parsed.total);
                    get_printer_state().set_print_layer_total(parsed.total);
                }
            });
        });

    spdlog::debug("[GcodeResponseRouting] Action prompt system initialized");
}

void GcodeResponseRouting::detach_handlers(IMoonrakerClient* client) {
    if (client) {
        client->unregister_method_callback("notify_gcode_response", "layer_tracker");
        if (m_action_prompt_manager) {
            client->unregister_method_callback("notify_gcode_response", "action_prompt_manager");
        }
    }
    // AmsState outlives the bundle: its mock gcode injection callback would dangle.
    AmsState::instance().set_gcode_response_callback(nullptr);
    m_action_prompt_modal.reset();
    ActionPromptManager::set_instance(nullptr);
    m_action_prompt_manager.reset();
}

void GcodeResponseRouting::release_routers() {
    m_gcode_narration_router.reset();
    m_lan_client_auth_router.reset();
    m_gcode_error_router.reset();
    m_ams_error_bridge.reset();
    m_recovery_presenter.reset();
}

} // namespace helix
