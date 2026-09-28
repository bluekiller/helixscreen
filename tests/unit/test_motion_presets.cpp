// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "motion_presets.h"

#include <iterator>

#include "../catch_amalgamated.hpp"

using helix::AxisBounds;
using helix::circular_bed_kinematics;
using helix::motion_preset_target;
using helix::MotionPreset;

namespace {

AxisBounds known_bounds(float x_min, float x_max, float y_min, float y_max) {
    AxisBounds b;
    b.x_min = x_min;
    b.x_max = x_max;
    b.y_min = y_min;
    b.y_max = y_max;
    b.has_x = true;
    b.has_y = true;
    b.has_z = true;
    return b;
}

struct PresetPoint {
    MotionPreset preset;
    double x;
    double y;
};

/// Assert every preset lands on its exact position and never commands Z.
void check_grid(const PresetPoint* points, size_t count, const AxisBounds& bounds, bool circular) {
    for (size_t i = 0; i < count; ++i) {
        const auto target = motion_preset_target(points[i].preset, bounds, circular);
        CAPTURE(i, circular);
        REQUIRE(target.has_value());
        CHECK(target->x == Catch::Approx(points[i].x).margin(1e-4));
        CHECK(target->y == Catch::Approx(points[i].y).margin(1e-4));
        CHECK_FALSE(target->z.has_value());
    }
}

constexpr double CORNER_REACH_100 = 63.63961030678928; // 90 * sqrt(0.5) on r=100

} // namespace

TEST_CASE("motion presets cover a 235x235 bed", "[motion][presets]") {
    const auto bounds = known_bounds(0, 235, 0, 235);
    // Edge presets sit 10% of the 235 mm span (23.5 mm) in from the edge.
    const PresetPoint grid[] = {
        {MotionPreset::RearLeft, 23.5, 211.5},   {MotionPreset::Rear, 117.5, 211.5},
        {MotionPreset::RearRight, 211.5, 211.5}, {MotionPreset::Left, 23.5, 117.5},
        {MotionPreset::Center, 117.5, 117.5},    {MotionPreset::Right, 211.5, 117.5},
        {MotionPreset::FrontLeft, 23.5, 23.5},   {MotionPreset::Front, 117.5, 23.5},
        {MotionPreset::FrontRight, 211.5, 23.5},
    };
    check_grid(grid, std::size(grid), bounds, false);
}

TEST_CASE("motion presets on a centre-origin bed", "[motion][presets]") {
    const auto bounds = known_bounds(-100, 100, -100, 100);
    // 10% of the 200 mm span is 20 mm in from each edge.
    const PresetPoint grid[] = {
        {MotionPreset::RearLeft, -80, 80},   {MotionPreset::Rear, 0, 80},
        {MotionPreset::RearRight, 80, 80},   {MotionPreset::Left, -80, 0},
        {MotionPreset::Center, 0, 0},        {MotionPreset::Right, 80, 0},
        {MotionPreset::FrontLeft, -80, -80}, {MotionPreset::Front, 0, -80},
        {MotionPreset::FrontRight, 80, -80},
    };
    check_grid(grid, std::size(grid), bounds, false);
}

TEST_CASE("motion presets on a circular bed sit on the inscribed circle", "[motion][presets]") {
    const auto bounds = known_bounds(-100, 100, -100, 100);
    // Rim presets at 90% of the 100 mm radius; diagonals at 45 degrees, not
    // on the bounding square's corners where the round bed has ended.
    const PresetPoint grid[] = {
        {MotionPreset::RearLeft, -CORNER_REACH_100, CORNER_REACH_100},
        {MotionPreset::Rear, 0, 90},
        {MotionPreset::RearRight, CORNER_REACH_100, CORNER_REACH_100},
        {MotionPreset::Left, -90, 0},
        {MotionPreset::Center, 0, 0},
        {MotionPreset::Right, 90, 0},
        {MotionPreset::FrontLeft, -CORNER_REACH_100, -CORNER_REACH_100},
        {MotionPreset::Front, 0, -90},
        {MotionPreset::FrontRight, CORNER_REACH_100, -CORNER_REACH_100},
    };
    check_grid(grid, std::size(grid), bounds, true);

    // The circular flag is the only difference: the same bounds on a
    // rectangular reading put Front at the inset row, not at 90% radius.
    const auto front = motion_preset_target(MotionPreset::Front, bounds, false);
    REQUIRE(front.has_value());
    CHECK(front->y == Catch::Approx(-80.0).margin(1e-4));
}

TEST_CASE("motion presets refuse unknown or degenerate bounds", "[motion][presets]") {
    AxisBounds unknown_x = known_bounds(0, 235, 0, 235);
    unknown_x.has_x = false;
    CHECK_FALSE(motion_preset_target(MotionPreset::Center, unknown_x, false).has_value());
    CHECK_FALSE(motion_preset_target(MotionPreset::RearRight, unknown_x, true).has_value());

    AxisBounds unknown_y = known_bounds(0, 235, 0, 235);
    unknown_y.has_y = false;
    CHECK_FALSE(motion_preset_target(MotionPreset::Front, unknown_y, false).has_value());

    AxisBounds degenerate = known_bounds(100, 100, 0, 235);
    CHECK_FALSE(motion_preset_target(MotionPreset::Center, degenerate, false).has_value());

    AxisBounds inverted = known_bounds(200, 100, 0, 235);
    CHECK_FALSE(motion_preset_target(MotionPreset::Center, inverted, true).has_value());
}

TEST_CASE("circular_bed_kinematics names the round-bed machines", "[motion][presets]") {
    CHECK(circular_bed_kinematics("delta"));
    CHECK(circular_bed_kinematics("rotary_delta"));
    CHECK_FALSE(circular_bed_kinematics("cartesian"));
    CHECK_FALSE(circular_bed_kinematics("corexy"));
    CHECK_FALSE(circular_bed_kinematics(""));
}

TEST_CASE("motion presets aim at the plate, not axis overtravel", "[motion][presets]") {
    // Snapmaker U1: Y travels to 335 but the plate ends at 270; the tool docks
    // fill the rest. homing_origin (-0.088928, -0.016043) shifts G-code space.
    const AxisBounds machine = known_bounds(0, 271, 0, 335);
    const AxisBounds gcode = known_bounds(0.088928f, 271.088928f, 0.016043f, 335.016043f);
    BuildVolume vol;
    vol.plate_x_min = 3;
    vol.plate_x_max = 267;
    vol.plate_y_min = 3;
    vol.plate_y_max = 267;

    const AxisBounds area = helix::preset_area(machine, gcode, vol);
    CHECK(area.x_min == Catch::Approx(3.088928).margin(1e-4));
    CHECK(area.x_max == Catch::Approx(267.088928).margin(1e-4));
    CHECK(area.y_min == Catch::Approx(3.016043).margin(1e-4));
    CHECK(area.y_max == Catch::Approx(267.016043).margin(1e-4));

    const auto rear_left = motion_preset_target(MotionPreset::RearLeft, area, false);
    REQUIRE(rear_left.has_value());
    CHECK(*rear_left->x == Catch::Approx(29.488928).margin(1e-4));
    CHECK(*rear_left->y == Catch::Approx(240.616043).margin(1e-4));
}

TEST_CASE("preset area falls back to travel and never exceeds it", "[motion][presets]") {
    const AxisBounds travel = known_bounds(0, 235, 0, 235);

    SECTION("no plate declared") {
        const AxisBounds area = helix::preset_area(travel, travel, BuildVolume{});
        CHECK(area.x_min == 0.0f);
        CHECK(area.x_max == 235.0f);
        CHECK(area.y_max == 235.0f);
    }

    SECTION("plate wider than travel is clipped to it") {
        BuildVolume vol;
        vol.plate_x_min = -10;
        vol.plate_x_max = 250;
        vol.plate_y_min = 5;
        vol.plate_y_max = 230;
        const AxisBounds area = helix::preset_area(travel, travel, vol);
        CHECK(area.x_min == 0.0f);
        CHECK(area.x_max == 235.0f);
        CHECK(area.y_min == 5.0f);
        CHECK(area.y_max == 230.0f);
    }

    SECTION("unknown travel stays unknown") {
        AxisBounds unknown = travel;
        unknown.has_y = false;
        BuildVolume vol;
        vol.plate_x_min = 3;
        vol.plate_x_max = 200;
        vol.plate_y_min = 3;
        vol.plate_y_max = 200;
        CHECK_FALSE(helix::preset_area(unknown, unknown, vol).has_y);
    }
}

TEST_CASE("plate_rear_park centres in X and sits inside the rear of the plate",
          "[motion][presets]") {
    const auto park = helix::plate_rear_park(known_bounds(0, 235, 0, 235));
    REQUIRE(park.has_value());
    CHECK(*park->x == Catch::Approx(117.5));
    CHECK(*park->y == Catch::Approx(225.0));
    CHECK_FALSE(park->z.has_value());

    // Never past the plate, even when it is shallower than the margin allows.
    const auto shallow = helix::plate_rear_park(known_bounds(0, 100, 50, 60));
    REQUIRE(shallow.has_value());
    CHECK(*shallow->y == Catch::Approx(55.0));

    AxisBounds unknown = known_bounds(0, 235, 0, 235);
    unknown.has_y = false;
    CHECK_FALSE(helix::plate_rear_park(unknown).has_value());
}
