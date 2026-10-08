// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui_panel_ams.h"
#include "ui_panel_ams_overview.h"

namespace helix {

/// What a closed AMS panel keeps running, for the keep-alive tests.
class AmsPanelTestAccess {
  public:
    static bool is_open(const AmsPanel& p) {
        return p.open_;
    }
    static bool has_sidebar(const AmsPanel& p) {
        return p.sidebar_ != nullptr;
    }
    static helix::ui::AmsOperationSidebar* sidebar(AmsPanel& p) {
        return p.sidebar_.get();
    }
    static lv_obj_t* path_canvas(const AmsPanel& p) {
        return p.path_canvas_;
    }
    static bool is_open(const AmsOverviewPanel& p) {
        return p.open_;
    }
    static int units_refreshes(const AmsOverviewPanel& p) {
        return p.units_refreshes_;
    }
};

} // namespace helix
