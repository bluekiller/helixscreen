// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_session.h"

#include "ui_nav_manager.h"
#include "ui_toast_manager.h"
#include "ui_wizard.h"

#include "app_globals.h"
#include "config.h"
#include "printer_cache_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>

#include "hv/json.hpp"

namespace helix {

namespace {

/// RAII latch for PrinterSession::m_soft_restart_in_progress.
///
/// Clearing the re-entrancy flag by hand on each exit path leaves it stuck true whenever an
/// exit is not the one that was hand-coded, and a stuck flag makes every later printer
/// switch or add a silent no-op until the process restarts.
class SoftRestartLatch {
  public:
    explicit SoftRestartLatch(bool& flag) : m_flag(flag) {
        m_flag = true;
    }
    ~SoftRestartLatch() {
        m_flag = false;
    }

    SoftRestartLatch(const SoftRestartLatch&) = delete;
    SoftRestartLatch& operator=(const SoftRestartLatch&) = delete;
    SoftRestartLatch(SoftRestartLatch&&) = delete;
    SoftRestartLatch& operator=(SoftRestartLatch&&) = delete;

  private:
    bool& m_flag;
};

} // namespace

PrinterSession::PrinterSession(Config*& config, AsyncLifetimeGuard& async, Restart restart)
    : m_config(config), m_async(async), m_restart(std::move(restart)) {}

void PrinterSession::switch_printer(const std::string& printer_id) {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[PrinterSession] Ignoring switch_printer during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

    spdlog::info("[PrinterSession] Switching to printer '{}'...", printer_id);

    // Validate printer exists in config
    if (!m_config->set_active_printer(printer_id)) {
        spdlog::error("[PrinterSession] Failed to switch — unknown printer '{}'", printer_id);
        return;
    }
    m_config->save();

    // Per-printer state lives at /printers/<active>/… and is reached via Config::df().
    // The active printer just changed, so df() now points at the new printer — fire every
    // registered per-printer cache invalidator BEFORE teardown, while df() is already
    // correct, so nothing keeps serving the previous printer's values. PanelWidgetManager's
    // cached layouts (#804) were the first instance of this; the registry is what stops the
    // next one from being a latent bug nobody remembers to wire up.
    PrinterCacheRegistry::instance().invalidate_all();

    m_restart.teardown();
    m_restart.rebuild();

    m_restart.land_home();

    // Show toast with the new printer name
    std::string printer_name =
        m_config->get<std::string>(m_config->df() + "printer_name", printer_id);
    std::string toast_msg = fmt::format(fmt::runtime(lv_tr("Connected to {}")), printer_name);
    ToastManager::instance().show(ToastSeverity::INFO, toast_msg.c_str());

    spdlog::info("[PrinterSession] Switched to printer '{}'", printer_id);
}

void PrinterSession::add_printer_via_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn("[PrinterSession] Ignoring add_printer_via_wizard during active soft restart");
        return;
    }
    SoftRestartLatch soft_restart(m_soft_restart_in_progress);

    // Generate a unique ID for the new printer entry (loop to avoid collisions after deletes)
    auto existing_ids = m_config->get_printer_ids();
    int counter = static_cast<int>(existing_ids.size()) + 1;
    std::string new_id;
    do {
        new_id = "printer-" + std::to_string(counter++);
    } while (std::find(existing_ids.begin(), existing_ids.end(), new_id) != existing_ids.end());
    std::string previous_id = m_config->get_active_printer_id();

    // Create empty printer entry with wizard_completed=false so is_wizard_required()
    // returns true (without this, root-level wizard_completed fallback blocks the wizard)
    nlohmann::json printer_data = {{"wizard_completed", false}};
    m_config->add_printer(new_id, printer_data);
    m_config->set_active_printer(new_id);
    m_config->save();

    // Store previous ID so wizard cancellation can recover
    m_wizard_previous_printer_id = previous_id;

    spdlog::info("[PrinterSession] Adding new printer '{}' via wizard (previous: '{}')", new_id,
                 previous_id);

    // Same active-printer change as switch_printer(): Config::df() has moved to the new
    // entry, so every per-printer cache must be dropped before teardown.
    PrinterCacheRegistry::instance().invalidate_all();

    // The rebuild runs the wizard itself when is_wizard_required() returns true (it does for
    // the new empty entry), so the wizard must not be launched again here.
    m_restart.teardown();

    // Registered after the teardown (which clears it) and before the rebuild (which runs the
    // wizard).
    set_wizard_cancel_callback([this]() { cancel_add_printer_wizard(); });

    m_restart.rebuild();
}

void PrinterSession::cancel_add_printer_wizard() {
    if (m_soft_restart_in_progress) {
        spdlog::warn(
            "[PrinterSession] Ignoring cancel_add_printer_wizard during active soft restart");
        return;
    }

    if (m_wizard_previous_printer_id.empty()) {
        spdlog::debug("[PrinterSession] No add-printer recovery state — ignoring cancel");
        return;
    }

    std::string failed_id = m_config->get_active_printer_id();
    std::string restore_id = m_wizard_previous_printer_id;
    spdlog::info("[PrinterSession] Cancelling add-printer wizard — removing '{}', restoring '{}'",
                 failed_id, restore_id);

    m_config->remove_printer(failed_id);
    m_config->set_active_printer(restore_id);
    m_config->save();
    m_wizard_previous_printer_id.clear();

    // Defer wizard teardown + soft restart — we're called from a wizard button click handler,
    // so the wizard_container must survive until the event callback returns.
    m_async.defer("PrinterSession::cancel_add_printer_wizard", [this]() {
        SoftRestartLatch soft_restart(m_soft_restart_in_progress);

        set_wizard_active(false);
        ui_wizard_deinit_subjects();

        // set_active_printer() above restored the previous printer, so Config::df() moved
        // again — drop every per-printer cache before teardown.
        PrinterCacheRegistry::instance().invalidate_all();

        m_restart.teardown();
        m_restart.rebuild();
        m_restart.land_home();
    });
}

} // namespace helix
