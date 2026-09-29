// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "src/ui/panel_widgets/tile_widget.h"

#include "panel_widget_registry.h"

namespace helix {

namespace {

/// What each tile here draws, for measurement. The value strings are worst
/// cases, never a live reading, so a size accepted while a tile reads one thing
/// still draws the widest thing it can ever show.
///
/// Only tiles with no widget class of their own. Every other centred-icon tile
/// owns a TileSizing on its own class, including the three heaters, which share
/// HeaterTempWidget, and power_device and filament, whose classes live outside
/// src/ui/panel_widgets.
struct TileContentRow {
    const char* id;
    const char* widest_value;
    const char* widest_current;
    const char* label;
    bool has_value;
};

constexpr TileContentRow kPureXmlTiles[] = {
    {"notifications", "", "", "Alerts", false},
    {"firmware_restart", "", "", "Restart", false},
};

} // namespace

void register_tile_widgets() {
    for (const auto& row : kPureXmlTiles) {
        // Only tiles nothing else claims. A widget with its own class owns its
        // sizing; registering over it would swap real behaviour for a shell
        // that merely measures, and silently, because the last registration
        // wins.
        const PanelWidgetDef* def = find_widget_def(row.id);
        if (!def || def->factory) {
            continue;
        }
        TileSizing::Content content{row.widest_value, row.widest_current, row.label, row.has_value};
        register_widget_factory(row.id, [content](const std::string& instance_id) {
            return std::make_unique<TileWidget>(instance_id, content);
        });
    }
}

} // namespace helix
