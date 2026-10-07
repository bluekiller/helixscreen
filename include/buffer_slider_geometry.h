// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "buffer_reading.h"

#include <cstdint>
#include <vector>

namespace helix::ui {

/// Pixel layout of the upright buffer slider in a box `height` px tall, y down
/// from its top. Loose is up and tight is down, as filament flows top to bottom
/// on the path canvas. A zero height lays out nothing.
struct BufferSliderGeometry {
    int block_y = 0; ///< Top edge of the block riding the strand
    int block_h = 0;
    int target_y = 0; ///< Dashed target window: the band under kPressureWarningPct
    int target_h = 0;
    int danger_top_h = 0;    ///< Loose end stop, from y = 0
    int danger_bottom_y = 0; ///< Tight end stop, down to y = height
    int danger_bottom_h = 0;
};

/// Centre y of the block for @p bias (-1 tight .. +1 loose, clamped; NaN reads
/// as 0). The slider and its trace both place a reading with this, so the
/// trace lines up with the block it scrolls out of.
int buffer_slider_y(float bias, int height);

BufferSliderGeometry buffer_slider_geometry(float bias, int height);

struct BufferTraceXY {
    int x;
    int y;
};

/// The trace as polylines in a box width x height: newest at x = 0 beside the
/// slider, older readings further right, one polyline per run of valid points.
/// Each reading holds as a step until the next.
std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height);

} // namespace helix::ui
