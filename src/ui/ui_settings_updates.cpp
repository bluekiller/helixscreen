// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_updates.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_info_qr_modal.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_toast_manager.h"

#include "helix_version.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "platform_info.h"
#include "static_panel_registry.h"
#include "system/config_trust.h"
#include "system/update_checker.h"
#include "system_settings_manager.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
#include <SDL.h>
#endif
#include <memory>

namespace helix::settings {

static std::unique_ptr<UpdatesSettingsOverlay> g_updates_settings_overlay;

UpdatesSettingsOverlay& get_updates_settings_overlay() {
    if (!g_updates_settings_overlay) {
        g_updates_settings_overlay = std::make_unique<UpdatesSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "UpdatesSettingsOverlay", []() { g_updates_settings_overlay.reset(); });
    }
    return *g_updates_settings_overlay;
}

UpdatesSettingsOverlay::UpdatesSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

UpdatesSettingsOverlay::~UpdatesSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

void UpdatesSettingsOverlay::init_subjects() {
    // update_status and update_version_text belong to UpdateChecker; the
    // show_update_settings / updates_* gates belong to SettingsPanel.
    subjects_initialized_ = true;
}

void UpdatesSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_about_update_channel_changed", on_about_update_channel_changed},
        {"on_about_check_updates_clicked", on_about_check_updates_clicked},
        {"on_about_install_update_clicked", on_about_install_update_clicked},
        {"on_about_updates_unavailable_clicked", on_about_updates_unavailable_clicked},
        {"on_update_download_start", on_about_update_download_start},
        {"on_update_download_cancel", on_about_update_download_cancel},
        {"on_update_download_dismiss", on_about_update_download_dismiss},
    });
}

lv_obj_t* UpdatesSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        return overlay_root_;
    }
    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_updates_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void UpdatesSettingsOverlay::show(lv_obj_t* parent_screen) {
    parent_screen_ = parent_screen;
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }
    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }
    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

void UpdatesSettingsOverlay::on_activate() {
    OverlayBase::on_activate();
    sync_update_channel_rows(overlay_root_,
                             static_cast<int>(UpdateChecker::instance().get_channel()));
}

void UpdatesSettingsOverlay::show_update_download_modal(bool start_immediately) {
#ifdef __ANDROID__
    // On Android, we never download/install tarballs — Play Store is the update
    // channel. Route all install intents (Install Update row and the in-app
    // "New Version Available" notification) to the store listing.
    if (helix::is_android_platform()) {
        spdlog::info("[UpdatesSettings] Opening Play Store for update");
        int result = SDL_OpenURL("market://details?id=org.helixscreen.app");
        if (result != 0) {
            spdlog::warn("[UpdatesSettings] market:// failed, trying web URL: {}", SDL_GetError());
            SDL_OpenURL("https://play.google.com/store/apps/details?id=org.helixscreen.app");
        }
        (void)start_immediately;
        return;
    }
#endif

    // Ensure callbacks are registered (modal may be shown before the overlay)
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Backdrop-tap and ESC dismissal destroy the modal widget via Modal::hide
    // directly, bypassing hide_update_download_modal().  That leaves our
    // pointer dangling and a second "Install Update" tap becomes a no-op.
    // Re-validate before reusing.
    if (update_download_modal_ && !lv_obj_is_valid(update_download_modal_)) {
        update_download_modal_ = nullptr;
    }

    if (!update_download_modal_) {
        // Clear any stale Error/Complete status carried over from a prior
        // download attempt — otherwise the modal briefly flashes that
        // content before the status update below takes effect.
        UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0,
                                                         "");
        update_download_modal_ = helix::ui::modal_show("update_download_modal");
    }

    if (start_immediately) {
        // User already confirmed on the "New Version Available" notification —
        // skip the redundant Confirming state and begin the download directly.
        UpdateChecker::instance().start_download();
        return;
    }

    // Set to Confirming state with version info
    auto info = UpdateChecker::instance().get_cached_update();
    std::string text = info ? fmt::format(lv_tr("Download v{}?"), info->version)
                            : std::string(lv_tr("Download update?"));
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Confirming, 0,
                                                     text);
}

void UpdatesSettingsOverlay::hide_update_download_modal() {
    if (update_download_modal_) {
        helix::ui::modal_hide(update_download_modal_);
        update_download_modal_ = nullptr;
    }
    // Reset download state
    UpdateChecker::instance().report_download_status(UpdateChecker::DownloadStatus::Idle, 0, "");
}

void UpdatesSettingsOverlay::sync_update_channel_rows(lv_obj_t* root, int effective_channel) {
    if (!root || effective_channel < 0) {
        return;
    }

    const auto selected = static_cast<uint32_t>(effective_channel);

    for (const char* row_name : {"row_update_channel", "row_update_channel_dev"}) {
        lv_obj_t* row = lv_obj_find_by_name(root, row_name);
        if (!row) {
            continue;
        }
        lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
        if (!dropdown) {
            continue;
        }
        // A row too short for this channel is the one that is hidden right now.
        // Skipping it rather than letting LVGL clamp keeps Dev's index 2 from
        // rendering as Beta on the two-entry row if it ever became visible.
        if (selected < lv_dropdown_get_option_count(dropdown)) {
            lv_dropdown_set_selected(dropdown, selected);
        }
    }
}

void UpdatesSettingsOverlay::on_about_update_channel_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_update_channel_changed");
    lv_obj_t* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));

    bool rejected = false;
    if (index == 2) {
        std::string dev_url = helix::config_trust::read_update_urls().dev_url;
        if (dev_url.empty()) {
            spdlog::warn("[UpdatesSettings] Dev channel selected but no dev_url configured");
            int current = SystemSettingsManager::instance().get_update_channel();
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(current));
            ToastManager::instance().show(ToastSeverity::WARNING,
                                          lv_tr("Dev channel requires dev_url in update_urls.json"),
                                          3000);
            rejected = true;
        }
    }

    if (!rejected) {
        spdlog::info("[UpdatesSettings] Update channel changed: {} ({})", index,
                     index == 0 ? "Stable" : (index == 1 ? "Beta" : "Dev"));
        SystemSettingsManager::instance().set_update_channel(index);
        // Drops the previous channel's cached verdict, re-snapshots the config
        // for the debug bundle's off-thread reader, and starts a fresh check.
        // Without the re-check the row keeps showing whatever the old channel
        // offered, including when the new channel is BEHIND this install and
        // the only way forward is an explicit switch back.
        UpdateChecker::instance().on_channel_changed();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_check_updates_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_check_updates_clicked");
    spdlog::info("[UpdatesSettings] Check for updates requested");
    UpdateChecker::instance().check_for_updates();
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_updates_unavailable_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_updates_unavailable_clicked");
    spdlog::info("[UpdatesSettings] Updates-unavailable notice tapped");

    // Reached only when self_update_supported() is false and updates are not
    // firmware-managed: this box can see that a new version exists but cannot
    // apply one itself. The command is the whole payload — without it the row
    // states a problem and offers nothing, which is what made the suppressed
    // state a dead end. The QR points at the docs for the longer story.
    helix::ui::InfoQrModal::show_owned({
        .icon = "console",
        .title = lv_tr("Update from a Terminal"),
        // No command in here on purpose. The one-liner is not portable across the
        // platforms this runs on — BusyBox firmwares (K1, K2, AD5M, CC1) ship ash
        // with no bash, and several have wget but no curl — so any single literal
        // would be wrong somewhere, baked into a binary, and only fixable by the
        // release the user cannot install. The docs can say the right thing per
        // platform and can be corrected without shipping anything.
        .message = lv_tr("Run the HelixScreen installer with --update from a "
                         "terminal on this printer. Scan for the command for "
                         "your platform."),
        .url = "https://helixscreen.org/docs/guide/getting-started/",
        .url_text = "helixscreen.org/docs",
    });
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_install_update_clicked(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_install_update_clicked");
    spdlog::info("[UpdatesSettings] Install update requested");

    // Moving backward is never what someone means by "install update", so it is
    // never the one-tap path. Settings written by the newer build are not
    // migrated back either — the older build reads what it recognizes and
    // leaves the rest alone.
    // Single if/else, no early return: BEGIN/END are a try/catch pair, so a
    // return between them leaves the block unclosed.
    auto cached = UpdateChecker::instance().get_cached_update();
    if (cached && cached->is_downgrade) {
        spdlog::info("[UpdatesSettings] Install target v{} is older than installed v{}, confirming",
                     cached->version, HELIX_VERSION);
        std::string msg =
            fmt::format(lv_tr("This channel offers v{}, older than the installed v{}. "
                              "Anything added since then will be removed."),
                        cached->version, HELIX_VERSION);
        helix::ui::modal_confirm(
            lv_tr("Install Older Version?"), msg.c_str(), ModalSeverity::Warning, lv_tr("Install"),
            [] { get_updates_settings_overlay().show_update_download_modal(); });
    } else {
        get_updates_settings_overlay().show_update_download_modal();
    }
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_update_download_start(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_update_download_start");
    spdlog::info("[UpdatesSettings] Starting update download");
    UpdateChecker::instance().start_download();
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_update_download_cancel(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_update_download_cancel");
    spdlog::info("[UpdatesSettings] Download cancelled by user");
    UpdateChecker::instance().cancel_download();
    get_updates_settings_overlay().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

void UpdatesSettingsOverlay::on_about_update_download_dismiss(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[UpdatesSettings] on_about_update_download_dismiss");
    get_updates_settings_overlay().hide_update_download_modal();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
