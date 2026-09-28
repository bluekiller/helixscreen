// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_observer_guard.h"
#include "ui_widget_ref.h"

#include "async_lifetime_guard.h"
#include "panel_widget.h"
#include "src/ui/panel_widgets/tile_sizing.h"

#include <memory>

class IMoonrakerAPI;

namespace helix {
class PrinterState;
}

namespace helix {

class LedWidget : public PanelWidget {
  public:
    LedWidget(PrinterState& printer_state, IMoonrakerAPI* api);
    ~LedWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "led";
    }

    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override {
        (void)colspan;
        (void)rowspan;
        sizing_.measure_and_publish(width_px, height_px);
    }

    bool fits_at(int width_px, int height_px) const override {
        return sizing_.fits(width_px, height_px);
    }

    const char** xml_attrs() const override {
        return sizing_.subject_attrs();
    }

    TileSizing* tile_sizing() override {
        return &sizing_;
    }

    // XML event callbacks (public for early registration in register_led_widget)
    static void light_toggle_cb(lv_event_t* e);

  private:
    PrinterState& printer_state_;
    IMoonrakerAPI* api_;

    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef parent_screen_;
    helix::ui::WidgetRef light_icon_;

    bool light_on_ = false;

    ObserverGuard led_version_observer_;
    ObserverGuard led_state_observer_;
    ObserverGuard led_brightness_observer_;

    // MUST stay declared LAST: reverse-declaration destruction makes this the
    // first member torn down, invalidating every captured token before any
    // observer destructs. Without this, queued observer callbacks captured
    // via tok.defer() see token.expired() == false after the observers are
    // already gone and dereference a half-destroyed widget. See temp_stack_widget.h
    // (commit 45abc8c2a, bundle AX3CKAKB).
    helix::AsyncLifetimeGuard lifetime_;

    void handle_light_toggle();
    void update_light_icon();
    void flash_light_icon();
    void bind_led();
    void on_led_state_changed(int state);

    /// Built with the widget so its subjects exist before the manager parses
    /// this tile's component; a binding whose subject is missing at parse time
    /// is dropped permanently.
    TileSizing sizing_{"led", TileSizing::Content{"", "", "Light", false}};
};

} // namespace helix
