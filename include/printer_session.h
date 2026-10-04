// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "gcode_response_routing.h"
#include "hardware_setup_prompter.h"
#include "lvgl/lvgl.h"

#include <functional>
#include <memory>
#include <set>
#include <string>

class ApplicationTestAccess;     // NAMESPACE_OK: test accessor, declared at global scope
class DisplayManager;            // NAMESPACE_OK: declared at global scope
class JobQueueState;             // NAMESPACE_OK: declared at global scope
class MoonrakerManager;          // NAMESPACE_OK: declared at global scope
class PrintHistoryManager;       // NAMESPACE_OK: declared at global scope
class SubjectInitializer;        // NAMESPACE_OK: declared at global scope
class TemperatureHistoryManager; // NAMESPACE_OK: declared at global scope

#if HELIX_HAS_PLUGINS
namespace helix::plugin {
class PluginHost;
class PluginDirWatcher;
class PluginSyncDriver;
} // namespace helix::plugin
#endif

namespace helix {
class Config;
class PanelFactory;

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
    PrinterSession(Config*& config, AsyncLifetimeGuard& async, Restart restart, lv_obj_t*& screen,
                   std::function<lv_obj_t*()> screen_fn, std::function<IMoonrakerAPI*()> api);
    ~PrinterSession();

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

    /// What survives the teardown: a printer switch keeps the process and LVGL alive,
    /// ProcessExit ends both.
    enum class TeardownScope { PrinterSwitch, ProcessExit };

    /// The one ordered teardown behind both a printer switch and process exit. On exit,
    /// `exit_display` (null for an early exit that never built one) gets its display restored
    /// at the point the framebuffer is no longer being drawn to; the caller finishes the exit
    /// by stopping the HTTP executors and destroying the display.
    void teardown_printer_scope(TeardownScope scope, DisplayManager* exit_display = nullptr);

    // The per-printer objects, owned here so one teardown destroys them in order.
    std::unique_ptr<MoonrakerManager>& moonraker() {
        return m_moonraker;
    }
    std::unique_ptr<JobQueueState>& job_queue_state() {
        return m_job_queue_state;
    }
    std::unique_ptr<PrintHistoryManager>& history_manager() {
        return m_history_manager;
    }
    std::unique_ptr<TemperatureHistoryManager>& temp_history_manager() {
        return m_temp_history_manager;
    }
    std::unique_ptr<PanelFactory>& panels() {
        return m_panels;
    }
    std::unique_ptr<SubjectInitializer>& subjects() {
        return m_subjects;
    }
    GcodeResponseRouting& routing() {
        return m_routing;
    }
    lv_obj_t*& app_layout() {
        return m_app_layout;
    }
#if HELIX_HAS_PLUGINS
    std::unique_ptr<plugin::PluginHost>& plugin_host() {
        return m_plugin_host;
    }
    std::unique_ptr<plugin::PluginDirWatcher>& plugin_watcher() {
        return m_plugin_watcher;
    }
    std::unique_ptr<plugin::PluginSyncDriver>& plugin_sync() {
        return m_plugin_sync;
    }
    std::set<std::string>& known_plugin_ids() {
        return m_known_plugin_ids;
    }
#endif

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

    std::unique_ptr<MoonrakerManager> m_moonraker;
    std::unique_ptr<JobQueueState> m_job_queue_state;
    std::unique_ptr<PrintHistoryManager> m_history_manager;
    std::unique_ptr<TemperatureHistoryManager> m_temp_history_manager;
    std::unique_ptr<PanelFactory> m_panels;
    std::unique_ptr<SubjectInitializer> m_subjects;
#if HELIX_HAS_PLUGINS
    std::unique_ptr<plugin::PluginHost> m_plugin_host;
    /// Hot-reloads plugins from HELIX_PLUGIN_DIR while it is the source (no sync driver runs
    /// then). Holds a reference to m_plugin_host, so it must be reset before the host at
    /// every teardown.
    std::unique_ptr<plugin::PluginDirWatcher> m_plugin_watcher;
    /// Syncs the Moonraker plugin folder into the host's cache dir. Holds a reference to
    /// m_plugin_host, so it must be reset before the host at every teardown, and rebuilt
    /// with it on a printer switch.
    std::unique_ptr<plugin::PluginSyncDriver> m_plugin_sync;
    /// Plugin ids an earlier load or sync already showed; a sync finding an id outside this
    /// set toasts "new plugin available".
    std::set<std::string> m_known_plugin_ids;
#endif
    GcodeResponseRouting m_routing;

    lv_obj_t*& m_screen;
    lv_obj_t* m_app_layout = nullptr;
    struct OverlayPanels {
        lv_obj_t* motion = nullptr;
        lv_obj_t* nozzle_temp = nullptr;
        lv_obj_t* bed_temp = nullptr;
        lv_obj_t* print_status = nullptr;
        lv_obj_t* ams = nullptr;
        lv_obj_t* bed_mesh = nullptr;
    } m_overlay_panels;

    HardwareSetupPrompter m_prompter;
    size_t m_last_hardware_fingerprint = 0;
    bool m_first_discovery_complete = true;
};

} // namespace helix
