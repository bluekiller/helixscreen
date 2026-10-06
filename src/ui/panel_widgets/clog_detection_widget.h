// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_widget_ref.h"

#include "panel_widget.h"

#include <memory>
#include <string>

class ClogDetectionConfigModal;

namespace helix {
namespace ui {
class UiClogBar;
} // namespace ui

/// Panel widget for filament health monitoring on the home panel.
///
/// Shows the clog meter as a horizontal bar, whichever source AmsState picked
/// (FlowGuard, encoder, AFC buffer or filament pressure). The widget is
/// authored wide and short, which is the shape a horizontal scale wants, and it
/// lets both ends carry a label, so a symmetrical reading says which fault it
/// is leaning toward. UiClogMeter's arc is what the AMS sidebar and loaded card
/// use.
class ClogDetectionWidget : public PanelWidget {
  public:
    ClogDetectionWidget() = default;
    ~ClogDetectionWidget() override;

    void set_config(const nlohmann::json& config) override;
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "clog_detection";
    }

    bool has_edit_configure() const override {
        return true;
    }
    bool on_edit_configure() override;

  private:
    void apply_config();

    nlohmann::json config_;
    helix::ui::WidgetRef widget_obj_;
    helix::ui::WidgetRef clog_page_;
    std::unique_ptr<ui::UiClogBar> clog_bar_;
    std::unique_ptr<ClogDetectionConfigModal> config_modal_;
};

} // namespace helix
