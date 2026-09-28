// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_updates.h
 * @brief Updates settings overlay - update channel, check, install, and the
 *        firmware-managed / cannot-install notices
 *
 * Also owns the update download modal, which the "New Version Available"
 * notification opens without this overlay ever being shown.
 *
 * @pattern Overlay (lazy init)
 * @threading Main thread only
 *
 * @see UpdateChecker for update logic
 */

#pragma once

#include "lvgl/lvgl.h"
#include "overlay_base.h"

namespace helix::settings {

class UpdatesSettingsOverlay : public OverlayBase {
  public:
    UpdatesSettingsOverlay();
    ~UpdatesSettingsOverlay() override;

    void init_subjects() override;
    void register_callbacks() override;

    const char* get_name() const override {
        return "Updates";
    }

    void on_activate() override;

    lv_obj_t* create(lv_obj_t* parent) override;
    void show(lv_obj_t* parent_screen);

    bool is_created() const {
        return overlay_root_ != nullptr;
    }

    /**
     * @brief Point both Update Channel rows at the channel the updater is using
     *
     * settings_updates_overlay.xml carries a Stable/Beta row and a Stable/Beta/Dev
     * row. Neither binds its selection to update_channel, because a row must show
     * the *effective* channel rather than the stored one: Dev needs beta features,
     * so a locked install runs on Stable while /update/channel still reads Dev,
     * and the two-entry row has no index that renders a stored Dev at all.
     *
     * A row with too few options for the value is left alone rather than clamped,
     * since LVGL would render Dev's index 2 as Beta on the two-entry row.
     *
     * Both the tree and the channel are parameters so this is testable without
     * an overlay instance, a Config, or an UpdateChecker.
     *
     * @param root Overlay root to search, or nullptr for a no-op
     * @param effective_channel Channel index to show (UpdateChannel's enumerators)
     */
    static void sync_update_channel_rows(lv_obj_t* root, int effective_channel);

    // When start_immediately is true, skip the Confirming state and begin the
    // download directly: the user already confirmed on the "New Version
    // Available" notification modal.
    void show_update_download_modal(bool start_immediately = false);
    void hide_update_download_modal();

  private:
    lv_obj_t* update_download_modal_ = nullptr;

    static void on_about_update_channel_changed(lv_event_t* e);
    static void on_about_check_updates_clicked(lv_event_t* e);
    static void on_about_install_update_clicked(lv_event_t* e);
    static void on_about_updates_unavailable_clicked(lv_event_t* e);
    static void on_about_update_download_start(lv_event_t* e);
    static void on_about_update_download_cancel(lv_event_t* e);
    static void on_about_update_download_dismiss(lv_event_t* e);
};

UpdatesSettingsOverlay& get_updates_settings_overlay();

} // namespace helix::settings
