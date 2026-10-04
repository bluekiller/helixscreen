// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "hardware_setup_prompter.h"

#include <functional>
#include <string>

class ApplicationTestAccess; // NAMESPACE_OK: test accessor, declared at global scope

namespace helix {
class Config;

/// The state machine behind switching to another printer, adding one through the wizard, and
/// backing out of that wizard. It decides what the config says and in which order the restart
/// steps run; the steps themselves arrive as hooks.
class PrinterSession {
  public:
    /// The restart work the session sequences. Teardown destroys the current printer's
    /// scope, rebuild creates the next one, land_home navigates to the home panel.
    struct Restart {
        std::function<void()> teardown;
        std::function<void()> rebuild;
        std::function<void()> land_home;
    };

    /// `config` is read through the reference because it is assigned after construction.
    /// `screen` and `api` are read at use by the hardware-setup prompter, since both change
    /// across a printer switch.
    PrinterSession(Config*& config, AsyncLifetimeGuard& async, Restart restart,
                   std::function<lv_obj_t*()> screen, std::function<IMoonrakerAPI*()> api);

    PrinterSession(const PrinterSession&) = delete;
    PrinterSession& operator=(const PrinterSession&) = delete;

    /// Makes `printer_id` the active printer and restarts onto it. Ignored while a restart is
    /// running; an unknown id changes nothing.
    void switch_printer(const std::string& printer_id);

    /// Creates an empty printer entry, makes it active and restarts into its setup wizard.
    void add_printer_via_wizard();

    /// Abandons the printer the wizard was adding, restores the previous one and restarts
    /// onto it. The restart is deferred past the click handler that called this.
    void cancel_add_printer_wizard();

    /// The printer to restore if the add-printer wizard is cancelled; empty when no such
    /// wizard is running.
    [[nodiscard]] const std::string& wizard_previous_printer_id() const {
        return m_wizard_previous_printer_id;
    }

    /// The add-printer wizard finished: there is nothing left to cancel back to.
    void clear_wizard_previous_printer_id() {
        m_wizard_previous_printer_id.clear();
    }

    /// The hardware prompts a discovery pass can raise, and their once-per-session guards.
    HardwareSetupPrompter& prompter() {
        return m_prompter;
    }

    /// Records a discovery's hardware fingerprint; true when the hardware shape differs from
    /// the previous discovery of this printer session (always true for the first one). When
    /// a reconnect's fingerprint matches, the expensive user-facing side effects (LED chip
    /// population, hardware validation toasts, targeted reconfig wizard, telemetry
    /// snapshots) are skipped and only the subject-restoring work runs.
    bool note_hardware_fingerprint(size_t fingerprint);

    /// Re-arms the per-printer discovery state: the fingerprint comparison and the
    /// once-per-connection prompt guards. Runs when a printer scope is torn down, so the
    /// next printer's first discovery runs the full pipeline.
    void reset_discovery_session();

  private:
    friend class ::ApplicationTestAccess;

    Config*& m_config;
    AsyncLifetimeGuard& m_async;
    Restart m_restart;

    /// Every restart path is a teardown plus a rebuild that includes a Moonraker connect and
    /// can throw; while this is set, a second switch or add is a no-op.
    bool m_soft_restart_in_progress = false;

    std::string m_wizard_previous_printer_id;

    HardwareSetupPrompter m_prompter;
    size_t m_last_hardware_fingerprint = 0;
    bool m_first_discovery_complete = true;
};

} // namespace helix
