// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace helix::ui {

/// Portrait preview card height for the print file detail view.
///
/// The preview is 16:10 of its width. When the options below it overflow,
/// the scroll area's visible bottom edge should cut a tile through its middle
/// half, so part of a tile shows under the fade cue: an edge in a grid gap or
/// a tile's outer quarter shrinks the preview until it does. Edges above the
/// tile grid are left alone; the cue alone carries those. Never below width/3,
/// where the preview stops reading as a model.
inline int decide_detail_portrait_preview(int width, int avail_h, int content_h, int grid_top,
                                          int tile_h, int gap) {
    const int base = width * 10 / 16;
    const int min_h = width / 3;
    if (content_h <= avail_h - base) {
        return std::max(base, min_h);
    }
    const int edge = avail_h - base;
    const int pitch = tile_h + gap;
    if (edge < grid_top || pitch <= 0) {
        return std::max(base, min_h);
    }
    const int in_row = (edge - grid_top) % pitch;
    const int lo = tile_h / 4;
    const int hi = tile_h - tile_h / 4;
    if (in_row >= lo && in_row <= hi) {
        return std::max(base, min_h);
    }
    // Shrinking the preview moves the edge down. Past `hi` the next tile's
    // middle half starts at pitch + lo; below `lo` it starts at lo.
    const int shift = in_row < lo ? lo - in_row : pitch + lo - in_row;
    return std::max(base - shift, min_h);
}

} // namespace helix::ui
