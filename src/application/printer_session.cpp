// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "printer_session.h"

#include "ui_keyboard_manager.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_notification.h"
#include "ui_observer_guard.h"
#include "ui_settings_about.h"
#include "ui_toast_manager.h"
#include "ui_update_queue.h"
#include "ui_utils.h"
#include "ui_wizard.h"

#include "active_print_media_manager.h"
#include "ams_state.h"
#include "app_globals.h"
#include "cjk_font_manager.h"
#include "config.h"
#include "display_manager.h"
#include "filament_consumption_tracker.h"
#include "job_queue_state.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"
#include "moonraker_api.h"
#include "moonraker_client.h"
#include "moonraker_manager.h"
#include "page_scroll_auto_inject.h"
#include "panel_factory.h"
#include "post_op_cooldown_manager.h"
#include "power_device_state.h"
#include "print_history_manager.h"
#include "printer_cache_registry.h"
#include "sensor_state.h"
#include "sound_manager.h"
#include "spoolman_active_spool_sync.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "subject_initializer.h"
#include "system/afc_message_dedup.h"
#include "system/crash_history.h"
#include "system/telemetry_manager.h"
#include "system/update_checker.h"
#include "temperature_history_manager.h"
#include "timelapse_state.h"
#include "upgrade_banner.h"
#if HELIX_HAS_PLUGINS
#include "plugin_dir_watcher.h"
#include "plugin_host.h"
#include "plugin_source_app.h"
#endif

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
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

PrinterSession::PrinterSession(Config*& config, AsyncLifetimeGuard& async, Restart restart,
                               lv_obj_t*& screen, std::function<lv_obj_t*()> screen_fn,
                               std::function<IMoonrakerAPI*()> api)
    : m_config(config), m_async(async), m_restart(std::move(restart)), m_screen(screen),
      m_prompter(async, std::move(screen_fn), std::move(api)) {}

PrinterSession::~PrinterSession() = default;

bool PrinterSession::note_hardware_fingerprint(size_t fingerprint) {
    const bool changed = m_first_discovery_complete || fingerprint != m_last_hardware_fingerprint;
    m_last_hardware_fingerprint = fingerprint;
    m_first_discovery_complete = false;
    return changed;
}

void PrinterSession::reset_discovery_session() {
    m_first_discovery_complete = true;
    m_last_hardware_fingerprint = 0;
    m_prompter.reset_for_new_connection();
}

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

// The one ordered teardown behind both soft restart (PrinterSwitch: the process and LVGL
// stay alive, init_printer_state() rebuilds afterwards) and shutdown() (ProcessExit:
// lv_deinit() frees every widget and the process ends). Steps that differ are guarded by
// `exiting` and say why; everything else runs identically in both scopes. Subjects stay
// alive until StaticSubjectRegistry::deinit_all() so ObserverGuards can call
// lv_observer_remove() while destroying.
void PrinterSession::teardown_printer_scope(TeardownScope scope, DisplayManager* exit_display) {
    const bool exiting = scope == TeardownScope::ProcessExit;
    auto destroy_panels = [exiting] {
        if (exiting) {
            StaticPanelRegistry::instance().destroy_all();
        } else {
            helix::ui::destroy_static_panels();
        }
    };

    // A callback armed for the old printer's wizard must not fire against the next one.
    set_wizard_cancel_callback(nullptr);

    // The next printer's discovery is a first discovery with its own prompts to show.
    reset_discovery_session();

    // A switch freezes the UpdateQueue before the disconnect: work the WebSocket thread
    // enqueues from here on is buffered, and update_queue_shutdown() below discards the
    // buffer, so it never runs against the plugins, history managers and AMS backends
    // destroyed in between. Exit needs no freeze; update_queue_shutdown() gates the queue
    // off for good.
    std::optional<helix::ui::UpdateQueue::ScopedFreeze> queue_freeze;
    if (!exiting) {
        queue_freeze.emplace(helix::ui::UpdateQueue::instance(), "teardown_printer_scope");
    }

    // Disconnect the WebSocket client FIRST to stop background threads (mock simulation,
    // WebSocket I/O). Otherwise a notification delivered mid-teardown can trigger new API
    // requests (history fetch, metascan, webcam detection). The client object stays valid
    // for the unregister_method_callback() calls below.
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->disconnect();
    }

    // Clear SoundManager's client ref so the M300 sequencer thread won't call
    // gcode_script() on a dangling pointer (#714). Exit skips host recovery:
    // SoundManager::shutdown() runs below, so re-opening audio hardware would only be
    // torn down again.
    SoundManager::instance().set_moonraker_client(nullptr, /*host_recovery=*/!exiting);

    // Clear app_globals BEFORE destroying managers so destructors (e.g. PrintSelectPanel)
    // never reach destroyed objects.
    set_moonraker_manager(nullptr);
    set_moonraker_api(nullptr);
    set_moonraker_client(nullptr);
    set_job_queue_state(nullptr); // the object itself dies after deinit_all(), below
    set_print_history_manager(nullptr);
    set_temperature_history_manager(nullptr);

    // Deactivate overlays and clear navigation registries
    NavigationManager::instance().shutdown();

    // Detach page-scroll-buttons controllers (gutters + observers) while panel widgets are
    // still alive, before m_panels.reset() / destroy_all() tear down the containers they
    // point at.
    helix::ui::PageScrollAutoInject::instance().shutdown();

    UpdateChecker::instance().stop_auto_check();

    if (exiting) {
        // Process-level singletons: they persist across a printer switch.
        // The banner goes before UpdateChecker so its observers release cleanly (#705).
        UpgradeBanner::instance().shutdown();
        UpdateChecker::instance().shutdown();    // cancels pending checks
        TelemetryManager::instance().shutdown(); // persists queue, joins send thread
        helix::CrashHistory::instance().shutdown();
        AfcMessageDedup::instance().shutdown();
        // Before the client is destroyed: the M300 backend's sender lambda references it
        // and the sequencer thread must be stopped first (#714).
        SoundManager::instance().shutdown();
        PostOpCooldownManager::instance().shutdown(); // cancel pending cooldown timers
    }

    // Unload plugins before destroying what they depend on: plugin closers remove
    // printer-subject observers and Moonraker notify handlers, so they must run while the
    // subjects (deinit_all below) and the Moonraker client (m_moonraker.reset below) are
    // still alive.
#if HELIX_HAS_PLUGINS
    // The watcher and the driver hold host references, and the driver's filelist handler
    // must not outlive the driver it feeds.
    if (m_moonraker && m_moonraker->client()) {
        m_moonraker->client()->unregister_method_callback("notify_filelist_changed", "PluginSync");
        // Before the plugin host goes: the registry's union stops being consulted, so
        // the refresh the unload-time clears schedule shrinks the subscription back to
        // app objects instead of growing it.
        m_moonraker->client()->set_subscription_extras_provider({});
    }
    m_plugin_watcher.reset();
    m_plugin_sync.reset();
    if (m_plugin_host) {
        m_plugin_host->unload_all();
        m_plugin_host.reset();
    }
#endif

    // History managers MUST be reset before moonraker (they use the client for
    // unregistration). JobQueueState is reset AFTER deinit_all() because it owns LVGL
    // subjects that panels still observe: destroying it early frees subject memory while
    // panel ObserverGuards still hold observer pointers into those lists.
    m_history_manager.reset();
    m_temp_history_manager.reset();

    // Unregister the connection-scoped method callbacks whose bodies reach panels or
    // subjects: StaticPanelRegistry::destroy_all() and StaticSubjectRegistry::deinit_all()
    // both run well before the client is released. external_spool_sync additionally
    // dereferences a raw IMoonrakerAPI* that the manager owns, straight from the WebSocket
    // thread.
    if (m_moonraker && m_moonraker->client()) {
        helix::TimelapseState::instance().detach(*m_moonraker->client());
        UpdateChecker::instance().detach(*m_moonraker->client());
        helix::settings::get_about_settings_overlay().detach_print_hours(*m_moonraker->client());
        helix::spoolman_sync::detach(*m_moonraker->client());
    }

    // Unsubscribe power device and sensor state
    if (m_moonraker && m_moonraker->api()) {
        helix::PowerDeviceState::instance().unsubscribe(*m_moonraker->api());
        helix::SensorState::instance().unsubscribe(*m_moonraker->api());
    }

    // Unregister the response handlers and drop the prompt system before moonraker is
    // destroyed.
    m_routing.detach_handlers(m_moonraker ? m_moonraker->client() : nullptr);

    // Stop AMS backend subscriptions BEFORE destroying MoonrakerClient: backends hold
    // SubscriptionGuards with raw client pointers and must unsubscribe while the client's
    // mutex is still alive.
    AmsState::instance().clear_backends();

    // Drain deferred UI callbacks BEFORE destroying panels. observe<int> and
    // observe<const char*> defer via ui_queue_update(), so queued callbacks may hold
    // `this` pointers to living panels; running them after m_panels.reset() is a
    // use-after-free.
    helix::ui::update_queue_shutdown();

    if (!exiting) {
        // Singletons that outlive this printer and hold API/client pointers or observe
        // PrinterState subjects about to be freed. LedAutoState drives LedController, so
        // it goes first.
        helix::led::LedAutoState::instance().deinit();
        helix::led::LedController::instance().deinit();
    }

    // Stop ALL LVGL animations before destroying panels: they hold widget pointers, and
    // completion callbacks fired by lv_anim_delete_all() would dereference freed objects
    // if the panels were already gone.
    lv_anim_delete_all();

    m_panels.reset();
    m_subjects.reset();

    if (exiting && exit_display) {
        // Guard for early exit paths like --help
        exit_display->restore_display_on_shutdown();
    }

    if (!exiting) {
        // The widgets this tracks are destroyed by the tree delete below; lv_deinit() does
        // that on exit.
        ModalStack::instance().clear();
    }

    // Stop the consumption tracker BEFORE destroying overlays. Overlay teardown can free
    // the tracker's PrinterState observer struct, and a later ObserverGuard::reset() then
    // dereferences freed memory (#927). Its self-registration with StaticSubjectRegistry
    // remains as a backstop and is a no-op once the observers are null.
    helix::FilamentConsumptionTracker::instance().stop();

    // Destroy ALL static panel/overlay globals (releases ObserverGuards, deinits local
    // subjects). LVGL must still be initialized so lv_observer_remove() can remove
    // unsubscribe_on_delete_cb from widget event lists. A switch frees the overlay roots
    // the panel destructors hand back (400-800KB each, parented to the screen, nothing
    // else deletes them); exit leaves them for lv_deinit(), because deleting widgets
    // inside this window reopens the crash it exists to avoid.
    destroy_panels();

    if (!exiting) {
        // Release global observer guards that observe subjects about to be freed.
        ui_notification_deinit();
        helix::deinit_active_print_media_manager();
    }

    // Deinit core singleton subjects (PrinterState, AmsState, SettingsManager, ...) BEFORE
    // lv_deinit(). lv_subject_deinit() calls lv_observer_remove() for each observer, which
    // removes unsubscribe_on_delete_cb from widget event lists, so widgets then delete
    // without firing stale unsubscribe callbacks on corrupted linked lists.
    StaticSubjectRegistry::instance().deinit_all();

    // Sweep any panel singleton a deinit callback lazily re-created on its way out (a
    // callback reaching through an auto-creating get_global_*_panel() getter builds a
    // replacement). Destroying it here, while LVGL and spdlog are up, keeps its destructor
    // off the static-destruction path. No-op when nothing resurrected.
    destroy_panels();

    // After deinit_all() so JobQueueState's registered cleanup lambda runs on a live
    // object; before m_moonraker.reset() so client unregistration works.
    m_job_queue_state.reset();

    if (exiting) {
        // Destroy runtime CJK fonts before LVGL shutdown
        helix::system::CjkFontManager::instance().shutdown();
    }

    // Invalidate all ObserverGuards so any reset() in surviving destructors releases
    // instead of calling lv_observer_remove() on freed observer pointers.
    // lv_subject_deinit() (via deinit_all() above) frees each observer it iterates, so
    // without this MoonrakerManager's ObserverGuard members would call
    // lv_observer_remove() on freed memory (lv_observer.c, lv_ll_remove).
    ObserverGuard::invalidate_all(exiting);

    // Tear down GcodeErrorRouter before MoonrakerClient: its dtor unregisters the live and
    // replay callbacks, both of which touch the client. Reset the router BEFORE the
    // presenter (the presenter must outlive it), and AmsErrorBridge, which also holds a
    // presenter reference, before that.
    m_routing.release_routers();

    // Destroy MoonrakerManager (its ObserverGuards now release without touching freed
    // observer memory thanks to invalidate_all() above).
    m_moonraker.reset();

    if (exiting) {
        // The caller finishes the exit: HTTP executors, display, theme manager.
        return;
    }

    KeyboardManager::instance().reset(); // widget pointers dangle after the tree delete

    // Delete the LVGL widget tree (panels already released their references). The display
    // stays alive, so no lv_deinit(). cancel_add_printer_wizard() reaches here inside a
    // queue_update() batch, where a synchronous lv_obj_del() would delete inside
    // UpdateQueue::process_pending() and can corrupt LVGL's global event list (#776/#190/#80);
    // m_app_layout is also the Home widget grid, where a relayout racing teardown could
    // iterate a freed container (#983). safe_delete_subtree() detaches the tree off-screen
    // synchronously (so the immediate init_printer_state() rebuild sees a clean m_screen),
    // forces LV_LAYOUT_NONE, and frees it asynchronously outside the batch.
    if (m_app_layout) {
        helix::ui::safe_delete_subtree(m_app_layout);
        m_app_layout = nullptr;
    }
    m_overlay_panels = {};
}

} // namespace helix
