// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_detail_layout.h"

#include "../catch_amalgamated.hpp"

using helix::ui::decide_detail_portrait_preview;

namespace {
// 480x800 portrait numbers: preview 452 wide -> base 282 (16:10).
constexpr int W = 452;
constexpr int BASE = W * 10 / 16;
constexpr int TILE = 48;
constexpr int GAP = 6;
constexpr int GRID_TOP = 120; // tile grid starts 120px into the scroll content

// Where the visible edge falls inside the scroll content for a given preview height.
int edge_for(int avail_h, int preview_h) {
    return avail_h - preview_h;
}
bool mid_tile(int edge) {
    if (edge < GRID_TOP) {
        return false;
    }
    const int in_row = (edge - GRID_TOP) % (TILE + GAP);
    return in_row >= TILE / 4 && in_row <= TILE - TILE / 4;
}
} // namespace

TEST_CASE("portrait preview: content fits -> 16:10 base, no nudge", "[print_detail_layout]") {
    const int avail = BASE + 300;
    CHECK(decide_detail_portrait_preview(W, avail, 250, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: overflow with edge in a grid gap is nudged mid-tile",
          "[print_detail_layout]") {
    // Put the edge exactly at the end of row 1 (inside the gap).
    const int edge = GRID_TOP + TILE + 2;
    const int avail = BASE + edge;
    const int h = decide_detail_portrait_preview(W, avail, 1000, GRID_TOP, TILE, GAP);
    CHECK(h < BASE);
    CHECK(BASE - h <= GAP + TILE / 2);
    CHECK(mid_tile(edge_for(avail, h)));
}

TEST_CASE("portrait preview: overflow with edge in a tile's outer quarter is nudged",
          "[print_detail_layout]") {
    const int edge = GRID_TOP + TILE - 3; // bottom outer quarter of row 1
    const int avail = BASE + edge;
    const int h = decide_detail_portrait_preview(W, avail, 1000, GRID_TOP, TILE, GAP);
    CHECK(BASE - h <= GAP + TILE / 2);
    CHECK(mid_tile(edge_for(avail, h)));
}

TEST_CASE("portrait preview: overflow with edge in a tile's top outer quarter is nudged",
          "[print_detail_layout]") {
    const int edge = GRID_TOP + 2; // top outer quarter of row 1
    const int avail = BASE + edge;
    const int h = decide_detail_portrait_preview(W, avail, 1000, GRID_TOP, TILE, GAP);
    CHECK(h < BASE);
    CHECK(BASE - h <= GAP + TILE / 2);
    CHECK(mid_tile(edge_for(avail, h)));
}

TEST_CASE("portrait preview: overflow with edge already mid-tile is left alone",
          "[print_detail_layout]") {
    const int edge = GRID_TOP + TILE / 2;
    CHECK(decide_detail_portrait_preview(W, BASE + edge, 1000, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: edge above the tile grid is left alone", "[print_detail_layout]") {
    const int edge = GRID_TOP - 10;
    CHECK(decide_detail_portrait_preview(W, BASE + edge, 1000, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: shrink stops at the width / 3 floor", "[print_detail_layout]") {
    // 80 wide: base 50, floor 26. An edge on the first pixel past a tile's
    // middle half wants a 29px shrink (to 21), past the floor.
    const int w = 80;
    const int base = w * 10 / 16;
    const int floor = w / 3;
    const int in_row = TILE - TILE / 4 + 1;
    const int shift = TILE + GAP + TILE / 4 - in_row;
    REQUIRE(base - shift < floor); // the nudge alone undershoots the floor
    const int avail = base + GRID_TOP + in_row;
    CHECK(decide_detail_portrait_preview(w, avail, 1000, GRID_TOP, TILE, GAP) == floor);
}

TEST_CASE("portrait preview: never below width / 3", "[print_detail_layout]") {
    // 272x480: 256 wide, base 160, floor 85. An edge in a gap right at the floor.
    const int w = 256;
    const int floor = w / 3;
    const int h =
        decide_detail_portrait_preview(w, 160 + GRID_TOP + TILE + 1, 1000, GRID_TOP, TILE, GAP);
    CHECK(h >= floor);
    CHECK(decide_detail_portrait_preview(w, 0, 1000, GRID_TOP, TILE, GAP) >= floor);
}
