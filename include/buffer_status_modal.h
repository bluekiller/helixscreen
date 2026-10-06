// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_modal.h"
#include "ui_observer_guard.h"

#include "ams_types.h"

namespace helix::ui {
class UiClogBar;
} // namespace helix::ui

/**
 * @brief Read-only modal showing buffer/sync status for Happy Hare, AFC or a
 *        filament pressure sensor (OpenAMS)
 *
 * Live while open: every row is re-read from the backend on each AmsState data
 * revision. The clog bar is the clog meter, whichever source AmsState picked.
 *
 * Subjects are static (shared across instances) because lv_xml_register_subject
 * rejects duplicate names — the first registration wins and the pointer persists.
 * Destroying per-instance subjects would leave the registry with dangling pointers.
 */
class BufferStatusModal : public Modal {
  public:
    BufferStatusModal();
    ~BufferStatusModal() override;

    const char* get_name() const override {
        return "Buffer Status";
    }
    const char* component_name() const override {
        return "buffer_status_modal";
    }

    /// Convenience: create the modal for one unit's buffer and show it. One-shot
    /// and stack-owned - ModalStack frees the instance when its entry goes (#1382).
    static void show_for(int effective_unit);

  protected:
    void on_show() override;

  private:
    friend class TestableBufferStatusModal;

    static void init_subjects();
    void populate(const helix::AmsSystemInfo& info, int effective_unit);
    /// populate() from the active backend's current snapshot.
    void refresh();

    static bool subjects_initialized_;
    /// The clog reading above the columns. Owned here so it is torn down
    /// before Modal::~Modal() frees the dialog tree it points into.
    helix::ui::UiClogBar* clog_bar_ = nullptr;

    // Static subjects + backing buffers (persist across modal instances)
    static lv_subject_t type_subject_;
    /// 1 when the unit has a sync-feedback bias, so the lean description shows.
    static lv_subject_t show_lean_subject_;
    static lv_subject_t show_espooler_subject_;
    static lv_subject_t show_flow_subject_;
    static lv_subject_t show_distance_subject_;

    static lv_subject_t description_subject_;
    static char description_buf_[128];
    static lv_subject_t pressure_subject_;
    static char pressure_buf_[128];
    /// Shown when the filament system reports no buffer/flow data at all, so
    /// the dialog says why instead of rendering an empty box.
    static lv_subject_t unsupported_subject_;
    static char unsupported_buf_[128];
    static lv_subject_t espooler_value_subject_;
    static char espooler_buf_[128];
    static lv_subject_t gear_sync_value_subject_;
    static char gear_sync_buf_[32];
    static lv_subject_t flow_value_subject_;
    static char flow_buf_[32];
    static lv_subject_t afc_state_subject_;
    static char afc_state_buf_[128];
    static lv_subject_t afc_distance_subject_;
    static char afc_distance_buf_[128];

    int effective_unit_ = 0;
    ObserverGuard revision_observer_;
    ObserverGuard backend_observer_;
};
