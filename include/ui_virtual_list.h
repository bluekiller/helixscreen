// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <lvgl.h>

namespace helix::ui {

/// Rows [first, last) a virtual list keeps materialised, and the spacer heights that stand in
/// for the rows outside it.
struct VirtualWindow {
    int first = 0;
    int last = 0;
    int leading_px = 0;
    int trailing_px = 0;
};

/// Which rows to materialise for a list scrolled to `scroll_y` in a `viewport_h` tall viewport.
/// `row_stride` is row height plus gap; `overscan` is the extra rows kept above and below.
///
/// A non-empty list always yields a non-empty window, even when scroll_y is past the end (a
/// shrunk list whose scroll position has not been clamped yet) or negative (overscroll). A
/// non-positive stride is treated as 1 px so a not-yet-measured row cannot divide by zero.
inline VirtualWindow compute_window(int scroll_y, int viewport_h, int row_stride, int total_rows,
                                    int overscan) {
    if (total_rows <= 0) {
        return {};
    }
    const int stride = std::max(1, row_stride);
    scroll_y = std::max(0, scroll_y);
    viewport_h = std::max(0, viewport_h);

    VirtualWindow w;
    w.first = std::min(std::max(0, scroll_y / stride - overscan), total_rows - 1);
    w.last = std::min(total_rows, (scroll_y + viewport_h) / stride + 1 + overscan);
    w.leading_px = w.first * stride;
    w.trailing_px = (total_rows - w.last) * stride;
    return w;
}

/// Apply `w`'s spacer heights, touching LVGL only when a height changed (`last_*` cache the
/// previous values), and keep the leading spacer first and the trailing spacer last among
/// the container's children.
inline void sync_list_spacers(lv_obj_t* container, lv_obj_t* leading, lv_obj_t* trailing,
                              const VirtualWindow& w, int& last_leading, int& last_trailing) {
    if (leading) {
        if (w.leading_px != last_leading) {
            lv_obj_set_height(leading, w.leading_px);
            last_leading = w.leading_px;
        }
        if (lv_obj_get_index(leading) != 0) {
            lv_obj_move_to_index(leading, 0);
        }
    }
    if (trailing) {
        if (w.trailing_px != last_trailing) {
            lv_obj_set_height(trailing, w.trailing_px);
            last_trailing = w.trailing_px;
        }
        int32_t last_index = static_cast<int32_t>(lv_obj_get_child_count(container)) - 1;
        if (lv_obj_get_index(trailing) != last_index) {
            lv_obj_move_to_index(trailing, last_index);
        }
    }
}

} // namespace helix::ui
