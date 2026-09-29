// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "panel_widget.h"
#include "src/ui/panel_widgets/tile_sizing.h"

namespace helix {

/// The measured filament diameter on a centred-icon tile.
class WidthSensorWidget : public PanelWidget {
  public:
    WidthSensorWidget() = default;
    ~WidthSensorWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    const char* id() const override {
        return "width_sensor";
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

  private:
    helix::ui::WidgetRef widget_obj_;
    /// Built with the widget so its subjects exist before the manager parses
    /// this tile's component. The reading is budgeted at its widest.
    TileSizing sizing_{"width_sensor", TileSizing::Content{"8.88 mm", "8.88 mm", "Width", true}};
};

} // namespace helix
