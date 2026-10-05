// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include <lvgl.h>

namespace helix::ui {

/**
 * @brief The E-stop kept over the navigation rail's nav_estop_slot
 *
 * It lives on the screen, not inside the rail, so it stays above every overlay,
 * modal and keypad backdrop. It is registered as the screen's always-on-top
 * chrome, which is what keeps stale-overlay sweeps from hiding it.
 */
class RailEstop {
  public:
    /// Build the E-stop over @p navbar's nav_estop_slot. No slot, no E-stop.
    void create(lv_obj_t* navbar);

    /// Move the E-stop onto the slot's current position.
    void sync();

    /// The keyboard's top edge in screen coordinates while it is open, or -1
    /// once it closes. Beside a side rail the E-stop moves up the rail column
    /// to clear it and back to its slot after; under a portrait bottom bar the
    /// keyboard covers it.
    void set_keyboard_top(int32_t top);

    /// Delete the E-stop. The next create() builds a fresh one over the new rail.
    void destroy();

    /// The E-stop, or nullptr.
    [[nodiscard]] lv_obj_t* widget() const {
        return estop_;
    }

    /// Hides the E-stop while a screen snapshot is taken, so a dimmed copy is
    /// not baked into the image.
    class ScopedHide {
      public:
        explicit ScopedHide(RailEstop& rail);
        ~ScopedHide();
        ScopedHide(const ScopedHide&) = delete;
        ScopedHide& operator=(const ScopedHide&) = delete;

      private:
        lv_obj_t* hidden_ = nullptr;
    };

  private:
    static void estop_deleted_cb(lv_event_t* e);
    static void navbar_layout_changed_cb(lv_event_t* e);

    lv_obj_t* estop_ = nullptr;
    WidgetRef navbar_;
    int32_t keyboard_top_ = -1;
};

} // namespace helix::ui
