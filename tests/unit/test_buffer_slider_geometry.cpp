// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_slider_geometry.cpp
 * @brief Where the buffer slider's block, target window and end stops land,
 *        and how its trace is laid out. Pure, no LVGL.
 */

#include "buffer_slider_geometry.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

TEST_CASE("buffer_slider_geometry: loose up, tight down", "[buffer][slider][geometry]") {
    // h = 100: block 12 px, 88 px of travel, centre y = 6 + (1 - bias) * 44.
    CHECK(buffer_slider_geometry(0.0f, 100).block_y == 44);
    CHECK(buffer_slider_geometry(1.0f, 100).block_y == 0);
    CHECK(buffer_slider_geometry(-1.0f, 100).block_y == 88);
    CHECK(buffer_slider_geometry(0.0f, 100).block_h == 12);
}

TEST_CASE("buffer_slider_geometry: the target window and end stops follow the bands",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, 100);
    CHECK(g.target_y == 37);        // y of +0.3
    CHECK(g.target_h == 26);        // down to y of -0.3
    CHECK(g.danger_top_h == 19);    // y of +0.7
    CHECK(g.danger_bottom_y == 81); // y of -0.7
    CHECK(g.danger_bottom_h == 19);
}

TEST_CASE("buffer_slider_geometry: out-of-range bias stays in the housing",
          "[buffer][slider][geometry]") {
    CHECK(buffer_slider_geometry(3.0f, 100).block_y == 0);
    CHECK(buffer_slider_geometry(-3.0f, 100).block_y == 88);
    CHECK(buffer_slider_geometry(std::nanf(""), 100).block_y == 44);
}

TEST_CASE("buffer_slider_geometry: small and empty boxes", "[buffer][slider][geometry]") {
    const auto small = buffer_slider_geometry(0.0f, 20);
    CHECK(small.block_h == 4);
    CHECK(small.block_y == 8);
    const auto none = buffer_slider_geometry(0.5f, 0);
    CHECK(none.block_h == 0);
    CHECK(none.target_h == 0);
}

TEST_CASE("buffer_trace_polylines: newest beside the slider, each reading a step",
          "[buffer][trace][geometry]") {
    // now = 100 s, 120 px wide: 60 s of history is 120 px, 2 px per second.
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.5f, true}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    const auto& l = lines[0];
    REQUIRE(l.size() == 4);
    CHECK(l[0].x == 0); // +0.5 from now ...
    CHECK(l[0].y == 28);
    CHECK(l[1].x == 60); // ... back to 70 s
    CHECK(l[1].y == 28);
    CHECK(l[2].x == 60); // step to 0.0 ...
    CHECK(l[2].y == 50);
    CHECK(l[3].x == 120); // ... held back to 40 s, the window's start
    CHECK(l[3].y == 50);
    // The trace and the block place a reading at the same height.
    CHECK(l[0].y == buffer_slider_y(0.5f, 100));
}

TEST_CASE("buffer_trace_polylines: a gap breaks the line", "[buffer][trace][geometry]") {
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.0f, false}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].front().x == 60);
    CHECK(lines[0].back().x == 120);
}

TEST_CASE("buffer_trace_polylines: nothing to draw", "[buffer][trace][geometry]") {
    CHECK(buffer_trace_polylines({}, 100000, 120, 100).empty());
    CHECK(buffer_trace_polylines({{0, 0.0f, true}}, 100000, 0, 100).empty());
}
