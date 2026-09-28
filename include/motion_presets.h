// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "axis_move.h"
#include "printer_detector.h"
#include "printer_motion_state.h"
#include "printer_state.h"

#include <optional>

namespace helix {

/// The nine fixed bed positions the Move tab offers as a 3x3 grid. Rear is
/// +Y, Front is -Y, Left is -X, Right is +X, all in G-code space.
enum class MotionPreset {
    RearLeft,
    Rear,
    RearRight,
    Left,
    Center,
    Right,
    FrontLeft,
    Front,
    FrontRight,
};

/**
 * @brief The area Move-tab presets aim at, in G-code space
 *
 * The plate the config declares ([bed_mesh] mesh_min/max) when there is one,
 * clipped to travel; otherwise the whole travel. Axis travel can run past the
 * plate into tool docks or a purge area, which a preset must never target.
 *
 * @param machine_travel toolhead axis limits (PrinterState::get_axis_bounds())
 * @param gcode_travel the same limits in G-code space
 *        (PrinterState::get_gcode_axis_bounds())
 * @param volume build volume holding the declared plate (machine space)
 */
AxisBounds preset_area(const AxisBounds& machine_travel, const AxisBounds& gcode_travel,
                       const BuildVolume& volume);

/**
 * @brief Bed position a Move-tab preset names, in G-code millimetres
 *
 * X and Y only; a preset never commands Z. On a rectangular bed, edge
 * presets sit 10% of that axis's span in from the edge and Center is the
 * midpoint. On a circular bed (delta), the eight rim presets sit at 90% of
 * the inscribed radius in their direction - a round bed has no corners, so
 * the diagonal presets sit at 45 degrees instead.
 *
 * @param gcode_bounds The area to place presets in, G-code space
 *        (preset_area()).
 * @param circular_bed True on a delta/rotary_delta machine (see
 *        circular_bed_kinematics() in printer_state.h).
 * @return nullopt when either axis's bounds are unknown or degenerate; the
 *         caller must skip the move rather than aim at fabricated
 *         coordinates.
 */
std::optional<AxisTarget> motion_preset_target(MotionPreset preset, const AxisBounds& gcode_bounds,
                                               bool circular_bed);

/// How far inside the rear edge of the plate a park position sits.
inline constexpr double PARK_REAR_MARGIN_MM = 10.0;

/**
 * @brief Where the toolhead parks when nothing else says: over the rear of
 *        the plate, centred in X, in G-code millimetres
 *
 * X and Y only. It never leaves the plate: PARK_REAR_MARGIN_MM inside the rear
 * edge, or the plate's centre line on a plate shallower than twice that.
 *
 * @param area The plate in G-code space (preset_area()).
 * @return nullopt when either axis's bounds are unknown or degenerate.
 */
std::optional<AxisTarget> plate_rear_park(const AxisBounds& area);

} // namespace helix
