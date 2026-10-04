// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_virtual_list.h"

#include "../catch_amalgamated.hpp"

using helix::ui::compute_window;
using helix::ui::VirtualWindow;

namespace {
struct Case {
    const char* name;
    int scroll_y, viewport_h, stride, total, overscan;
    int first, last, leading, trailing;
};
} // namespace

TEST_CASE("compute_window table", "[virtual_list][window]") {
    // stride 50 throughout unless the case is about the stride itself
    const Case cases[] = {
        {"empty list", 0, 200, 50, 0, 2, 0, 0, 0, 0},
        {"negative total", 0, 200, 50, -3, 2, 0, 0, 0, 0},
        {"top, overscan clamped at start", 0, 200, 50, 100, 2, 0, 7, 0, 93 * 50},
        {"middle, overscan at both ends", 1000, 200, 50, 100, 2, 18, 27, 18 * 50, 73 * 50},
        {"bottom, overscan clamped at end", 4800, 200, 50, 100, 2, 94, 100, 94 * 50, 0},
        {"partial last row still counts", 4770, 200, 50, 100, 0, 95, 100, 95 * 50, 0},
        {"partial first row is included", 75, 100, 50, 100, 0, 1, 4, 50, 96 * 50},
        {"list shorter than viewport", 0, 1000, 50, 3, 2, 0, 3, 0, 0},
        {"scroll beyond end keeps the last row", 99999, 200, 50, 10, 1, 9, 10, 9 * 50, 0},
        {"negative scroll acts as zero", -300, 200, 50, 100, 1, 0, 6, 0, 94 * 50},
        {"zero stride does not divide by zero", 10, 5, 0, 4, 0, 4 - 1, 4, 3, 0},
        {"zero viewport", 100, 0, 50, 100, 0, 2, 3, 100, 97 * 50},
        {"single row", 0, 200, 50, 1, 2, 0, 1, 0, 0},
    };

    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.name) {
            VirtualWindow w =
                compute_window(c.scroll_y, c.viewport_h, c.stride, c.total, c.overscan);
            CHECK(w.first == c.first);
            CHECK(w.last == c.last);
            CHECK(w.leading_px == c.leading);
            CHECK(w.trailing_px == c.trailing);
        }
    }
}

TEST_CASE("compute_window: any non-empty list yields a non-empty window",
          "[virtual_list][window]") {
    for (int total : {1, 2, 7, 100}) {
        for (int scroll : {-50, 0, 1, 333, 100000}) {
            for (int stride : {0, 1, 44, 57}) {
                VirtualWindow w = compute_window(scroll, 240, stride, total, 2);
                CHECK(w.first >= 0);
                CHECK(w.first < w.last);
                CHECK(w.last <= total);
            }
        }
    }
}
