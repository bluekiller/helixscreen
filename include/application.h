// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"

#include "async_lifetime_guard.h"
#include "cli_args.h"
#include "gcode_response_routing.h"
#include "invalidation_suppression.h"
#include "lvgl/lvgl.h"
#include "main_loop_handler.h"
#include "printer_session.h"
#include "splash_screen_manager.h"
#include "wizard_step.h" // helix::wizard::StepId
#include "xml_hot_reloader.h"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations
namespace helix {
class Config;
}
#if HELIX_HAS_PLUGINS
namespace helix::plugin {
class PluginHost;
class PluginDirWatcher;
class PluginSyncDriver;
struct SyncResult;
} // namespace helix::plugin
#endif
namespace helix {
class PrinterDiscovery;
} // namespace helix
class DisplayManager;
class SubjectInitializer;
class MoonrakerManager;
namespace helix {
class PanelFactory;
}
class JobQueueState;
class PrintHistoryManager;
class TemperatureHistoryManager;

/**
 * @brief Main application orchestrator
 *
 * Application coordinates all subsystems in the correct order:
 * 1. Parse CLI args and configure runtime settings
 * 2. Initialize display (LVGL, backend, input devices)
 * 3. Register fonts and images
 * 4. Initialize reactive subjects
 * 5. Create UI from XML and wire panels
 * 6. Initialize Moonraker client/API
 * 7. Connect to printer and run main loop
 * 8. Shutdown in reverse order
 *
 * Usage:
 *   Application app;
 *   return app.run(argc, argv);
 */
class Application {
  public:
    Application();
    ~Application();

    // Non-copyable, non-movable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    /**
     * @brief Run the application
     * @param argc Command line argument count
     * @param argv Command line argument array
     * @return Exit code (0 = success)
     */
    int run(int argc, char** argv);

  private:
    /// Allow test-only accessor to reach protected/private members
    friend class ApplicationTestAccess;

    // Initialization phases
    bool parse_args(int argc, char** argv);
    bool init_config();
    bool init_logging();
    bool init_display();
    void run_rotation_probe_and_layout();
    bool init_theme();
    bool init_assets();
    bool register_widgets();
    bool register_xml_components();
    bool init_translations();
    bool init_core_subjects();
    bool init_panel_subjects();
    bool init_ui();
    bool init_moonraker();
    bool connect_moonraker();
    void apply_startup_cli_actions();
    bool run_wizard();
#if HELIX_HAS_PLUGINS
    void init_plugins();
    /// Toasts plugin ids a sync found that no earlier load or sync had shown,
    /// then refreshes the Settings > Plugins row. Runs on the main thread from
    /// the sync driver's completion.
    void on_plugin_sync(const helix::plugin::SyncResult& result);
    /// settings_plugins_available follows "the host exists and holds at least
    /// one plugin", so the row appears only once there is something to show.
    void update_plugins_row_visibility();
#endif

    // Main loop
    int main_loop();
    void handle_keyboard_shortcuts();
    void process_notifications();
    void check_timeouts();

    // Shutdown
    void shutdown();

    // Soft restart (printer switching): the session sequences these two
    void tear_down_printer_state();

    /// What survives the teardown: a printer switch keeps the process and LVGL alive,
    /// ProcessExit ends both.
    enum class TeardownScope { PrinterSwitch, ProcessExit };
    void teardown_printer_scope(TeardownScope scope);

    void init_printer_state();

    // Helper functions
    void ensure_project_root_cwd();
#ifdef HELIX_ENABLE_SCREENSAVER
    void show_screensaver_migration_notice_if_pending();
#endif
    void setup_discovery_callbacks();
    lv_obj_t* create_overlay_panel(lv_obj_t* screen, const char* component_name,
                                   const char* display_name);
    void check_wifi_availability();
    void restore_flush_callback();

    // Owned managers (in initialization order)
    /// Expires the callbacks Application defers to the main thread — the
    /// hardware-role reapply, the two ActionPrompt modal hops off the WebSocket
    /// thread, and the wizard-cancel soft restart. Invalidated explicitly at the
    /// top of shutdown() rather than relying on member destruction order: the
    /// owned subsystems below are what those callbacks touch, and a guard that
    /// only expired via its own destructor would still read as live while those
    /// members were being torn down. Application is a stack local in main() with
    /// no deinit_subjects(), so shutdown()/destruction is the only teardown
    /// point — this is debt migrated to the sanctioned form, not a live
    /// use-after-free (#1165).
    helix::AsyncLifetimeGuard m_async_lifetime;

    std::unique_ptr<DisplayManager> m_display;
    std::unique_ptr<SubjectInitializer> m_subjects;
    std::unique_ptr<MoonrakerManager> m_moonraker;
    std::unique_ptr<JobQueueState> m_job_queue_state;
    std::unique_ptr<PrintHistoryManager> m_history_manager;
    std::unique_ptr<TemperatureHistoryManager> m_temp_history_manager;
    std::unique_ptr<helix::PanelFactory> m_panels;
    std::unique_ptr<helix::XmlHotReloader> m_hot_reloader;
#if HELIX_HAS_PLUGINS
    std::unique_ptr<helix::plugin::PluginHost> m_plugin_host;
    /// Hot-reloads plugins from HELIX_PLUGIN_DIR while it is the source (no
    /// sync driver runs then). Holds a reference to m_plugin_host, so it must
    /// be reset before the host at every teardown.
    std::unique_ptr<helix::plugin::PluginDirWatcher> m_plugin_watcher;
    /// Syncs the Moonraker plugin folder into the host's cache dir. Holds a
    /// reference to m_plugin_host, so it must be reset before the host at
    /// every teardown, and rebuilt with it on a printer switch.
    std::unique_ptr<helix::plugin::PluginSyncDriver> m_plugin_sync;
    /// Plugin ids an earlier load or sync already showed; a sync finding an
    /// id outside this set toasts "new plugin available".
    std::set<std::string> m_known_plugin_ids;
#endif

    /// Action prompts, the error / narration / LAN-pairing routers and layer tracking.
    helix::GcodeResponseRouting m_routing;

    // Configuration
    helix::Config* m_config = nullptr; // Singleton, not owned
    helix::CliArgs m_args;

    // Screen dimensions (0 = auto-detect from display hardware)
    int m_screen_width = 0;
    int m_screen_height = 0;

    // UI objects (not owned, managed by LVGL)
    lv_obj_t* m_screen = nullptr;
    lv_obj_t* m_app_layout = nullptr;

    // Overlay panels (for lifecycle management)
    struct OverlayPanels {
        lv_obj_t* motion = nullptr;
        lv_obj_t* nozzle_temp = nullptr;
        lv_obj_t* bed_temp = nullptr;
        lv_obj_t* print_status = nullptr;
        lv_obj_t* ams = nullptr;
        lv_obj_t* bed_mesh = nullptr;
    } m_overlay_panels;

    // NOTE: Print start collector and observers are kept in main.cpp
    // until the observer pattern is refactored to support capturing lambdas.

    // Periodic timeout checking (Moonraker connection health)
    uint32_t m_last_timeout_check = 0;
    uint32_t m_timeout_check_interval = 2000;

    // Main loop timing handler (screenshot, auto-quit, benchmark)
    helix::application::MainLoopHandler m_loop_handler;

    // Single-instance lock
    bool acquire_instance_lock();
    void release_instance_lock();
    int m_lock_fd = -1;

    // Android lifecycle pause/resume
    void on_enter_background();
    void on_enter_foreground();
    bool m_backgrounded = false;

    // Debounce for force_reconnect: on_enter_foreground and the DisplayManager
    // sleep callback can both fire for the same wake event. Without this, the
    // second call bumps the connection generation and makes the first
    // discovery's subscription stale — leaving the temp overlay dead (#1245).
    std::chrono::steady_clock::time_point m_last_force_reconnect{};

    // State
    bool m_running = false;
    bool m_wizard_active = false;
    bool m_shutdown_complete = false;

    /// Switching to, adding and abandoning printers; sequences tear_down_printer_state() and
    /// init_printer_state().
    helix::PrinterSession m_session;

    // Splash screen lifecycle manager
    helix::application::SplashScreenManager m_splash_manager;

    /// Original LVGL flush callback, saved while splash no-op is active
    lv_display_flush_cb_t m_original_flush_cb = nullptr;

    /// Display invalidation suppressed while the launcher's splash owns the framebuffer
    helix::InvalidationSuppression m_splash_invalidation_suppression;
};
