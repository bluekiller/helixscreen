// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file tile_layout.h
 * @brief Pure sizing decision for a centred-icon home tile.
 *
 * Measured pixels in, a verdict out. No LVGL, no widget, no subjects, so every
 * boundary below is unit-testable without a display. The widget measures in the
 * faces it actually renders and publishes the verdict as subjects; XML binds
 * appearance to those. See docs/devel/PANEL_WIDGET_GUIDE.md.
 */

#include <algorithm>

namespace helix {

/// Rungs on the icon ladder: xs, sm, md, lg, xl. The same index selects the
/// value and label faces the tile pairs with that rung, so one number drives
/// every face on the tile and the ladders cannot drift apart.
inline constexpr int kTileRungs = 5;

enum class TileDirection : int {
    Column = 0, ///< icon above the value
    Row = 1,    ///< icon beside the value
};

enum class TileLabelRung : int {
    None = 0,
    Label = 1,
};

/// Measured content at one rung, in the faces the tile renders at that rung.
///
/// The two value widths are both WORST CASES, never a live reading: a size
/// accepted while the tile reads 95 must still draw 888. `value_full_w` is the
/// widest "current / target" the tile can ever show, `value_current_w` the
/// widest current half alone, which is what it draws once the target is
/// dropped.
struct TileRungMetrics {
    int icon_w = 0;
    int icon_h = 0;
    int value_full_w = 0;
    int value_current_w = 0;
    int value_h = 0;
    int label_w = 0;
    int label_h = 0;
};

struct TileVerdict {
    int icon_rung = 2;
    TileLabelRung label = TileLabelRung::Label;
    TileDirection direction = TileDirection::Column;
    bool show_target = true;
    /// Whether the tile can draw its icon and its widest value at this size.
    /// Edit mode refuses a size where this is false.
    bool fits = true;
};

namespace detail {

/// One candidate presentation: which optional parts are still drawn.
struct TileCandidate {
    bool label;
    bool target;
};

/// Does rung @p m draw this candidate inside @p avail_w x @p avail_h?
inline bool tile_candidate_fits(const TileRungMetrics& m, TileDirection dir, TileCandidate cand,
                                bool has_value, int avail_w, int avail_h, int gap_px) {
    const int value_w = !has_value ? 0 : (cand.target ? m.value_full_w : m.value_current_w);
    const int value_h = has_value ? m.value_h : 0;
    const int label_w = cand.label ? m.label_w : 0;
    const int label_h = cand.label ? m.label_h : 0;

    int need_w = 0;
    int need_h = 0;
    if (dir == TileDirection::Column) {
        need_w = std::max({m.icon_w, value_w, label_w});
        need_h =
            m.icon_h + (has_value ? gap_px + value_h : 0) + (cand.label ? gap_px + label_h : 0);
    } else {
        const int side_w = std::max(value_w, label_w);
        need_w = m.icon_w + (side_w > 0 ? gap_px + side_w : 0);
        const int side_h = value_h + (has_value && cand.label ? gap_px : 0) + label_h;
        need_h = std::max(m.icon_h, side_h);
    }
    return need_w <= avail_w && need_h <= avail_h;
}

} // namespace detail

/**
 * @brief Pick the rung, direction and content a tile can draw at this size
 *
 * Content is surrendered in a fixed order, and only when nothing at any rung
 * can keep it: the target half first, then the label. Within each step the
 * largest rung that fits wins, so a tile grows its glyph rather than its text.
 * Column is preferred and Row is the fallback for a box too short to stack, so
 * direction follows fit rather than aspect ratio.
 *
 * `fits` is false only when even the smallest rung, with everything optional
 * surrendered, cannot draw the icon and the widest value. It is MONOTONIC in
 * both axes by construction: it is a disjunction over a fixed candidate set,
 * each member of which compares a constant need against the available box.
 *
 * @param avail_w        usable width in px (the grid's arithmetic extent)
 * @param avail_h        usable height in px
 * @param gap_px         gap between stacked parts
 * @param rungs          measured metrics, one per rung, smallest first
 * @param has_value      false for a tile that shows only an icon and a label
 * @param labels_enabled the user setting; size may hide a label, never show one
 */
inline TileVerdict decide_tile_layout(int avail_w, int avail_h, int gap_px,
                                      const TileRungMetrics rungs[kTileRungs], bool has_value,
                                      bool labels_enabled) {
    using detail::TileCandidate;

    // Ordered by what the tile gives up, most complete first. A tile that has
    // already surrendered its label has surrendered the target too, so the
    // label-off/target-on pairing is deliberately not offered.
    const TileCandidate with_labels[] = {{true, true}, {true, false}, {false, false}};
    const TileCandidate without_labels[] = {{false, true}, {false, false}};
    const TileCandidate* candidates = labels_enabled ? with_labels : without_labels;
    const int candidate_count = labels_enabled ? 3 : 2;

    const TileDirection directions[] = {TileDirection::Column, TileDirection::Row};

    for (int c = 0; c < candidate_count; ++c) {
        for (TileDirection dir : directions) {
            for (int r = kTileRungs - 1; r >= 0; --r) {
                if (detail::tile_candidate_fits(rungs[r], dir, candidates[c], has_value, avail_w,
                                                avail_h, gap_px)) {
                    TileVerdict v;
                    v.icon_rung = r;
                    v.label = candidates[c].label ? TileLabelRung::Label : TileLabelRung::None;
                    v.direction = dir;
                    v.show_target = has_value && candidates[c].target;
                    v.fits = true;
                    return v;
                }
            }
        }
    }

    // Nothing draws here. Report the most forgiving arrangement so a caller
    // that renders anyway clips as little as possible: the smallest glyph,
    // stacked along whichever axis the box has more of.
    TileVerdict v;
    v.icon_rung = 0;
    v.label = TileLabelRung::None;
    v.direction = avail_h >= avail_w ? TileDirection::Column : TileDirection::Row;
    v.show_target = false;
    v.fits = false;
    return v;
}

} // namespace helix
