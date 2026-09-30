// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_gcode_ghost_mode.cpp
 * @brief Pins GhostRenderMode's wire values and default.
 *
 * ui_gcode_viewer_set_ghost_mode(lv_obj_t*, int) is a C-style boundary
 * hard-coding 0=Dimmed and 1=Stipple, so renumbering the enum silently changes
 * what that API means without touching a line of its own code.
 */

#include "gcode_ghost_mode.h"

#include <type_traits>

#include "../catch_amalgamated.hpp"

using helix::gcode::DEFAULT_GHOST_RENDER_MODE;
using helix::gcode::GhostRenderMode;

TEST_CASE("GhostRenderMode wire values are pinned", "[gcode][ghost][ghost_mode]") {
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<GhostRenderMode>, uint8_t>);
    // ui_gcode_viewer_set_ghost_mode() maps a raw int onto these; renumbering
    // them re-points that API at the wrong mode.
    CHECK(static_cast<uint8_t>(GhostRenderMode::Dimmed) == 0);
    CHECK(static_cast<uint8_t>(GhostRenderMode::Stipple) == 1);
}

TEST_CASE("Ghost rendering defaults to Stipple", "[gcode][ghost][ghost_mode]") {
    CHECK(DEFAULT_GHOST_RENDER_MODE == GhostRenderMode::Stipple);
}
