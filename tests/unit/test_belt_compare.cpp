// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_tension_types.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

/// Lorentzian peak on a noise floor, 5-135 Hz at Klipper's 3200/4096 Hz spacing.
BeltCurve curve_with_peak(float peak_hz, float height = 1e4f, float floor = 50.0f) {
    BeltCurve c;
    for (float f = 5.0f; f < 135.0f; f += 0.78125f) {
        const float d = (f - peak_hz) / 5.0f;
        c.emplace_back(f, floor + height / (1.0f + d * d));
    }
    return c;
}

} // namespace

TEST_CASE("identical paths are matched", "[belt][compare]") {
    auto c = curve_with_peak(104.0f);
    auto r = compare_belt_paths(c, c);
    REQUIRE(r.valid);
    CHECK(r.peak_a_hz == Catch::Approx(104.0f).margin(0.8f));
    CHECK(r.delta_hz == Catch::Approx(0.0f).margin(0.01f));
    CHECK(r.similarity_percent > 99.0f);
    CHECK(r.verdict == BeltVerdict::MATCHED);
}

TEST_CASE("delta is signed: positive when A peaks higher", "[belt][compare]") {
    auto r = compare_belt_paths(curve_with_peak(110.0f), curve_with_peak(98.0f));
    REQUIRE(r.valid);
    CHECK(r.delta_hz == Catch::Approx(12.0f).margin(0.8f));
    auto flipped = compare_belt_paths(curve_with_peak(98.0f), curve_with_peak(110.0f));
    CHECK(flipped.delta_hz == Catch::Approx(-12.0f).margin(0.8f));
}

TEST_CASE("verdict tier boundaries on delta", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(3.0f, 95.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(-3.0f, 95.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(3.01f, 95.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(8.0f, 95.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(-8.01f, 95.0f) == BeltVerdict::ADJUST);
}

TEST_CASE("verdict tier boundaries on similarity", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(0.0f, 90.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(0.0f, 89.99f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(0.0f, 75.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(0.0f, 74.99f) == BeltVerdict::ADJUST);
}

TEST_CASE("verdict is the worse of the two tiers", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(1.0f, 60.0f) == BeltVerdict::ADJUST);
    CHECK(belt_verdict_for(12.0f, 99.0f) == BeltVerdict::ADJUST);
    CHECK(belt_verdict_for(5.0f, 95.0f) == BeltVerdict::CLOSE);
}

TEST_CASE("a peak below 20 Hz is not a belt peak", "[belt][compare]") {
    BeltCurve low;
    for (float f = 5.0f; f < 19.0f; f += 0.78125f)
        low.emplace_back(f, 1000.0f);
    auto r = compare_belt_paths(low, curve_with_peak(100.0f));
    CHECK_FALSE(r.valid);
    CHECK_FALSE(r.peak_a_found);
    CHECK(r.peak_b_found);
}

TEST_CASE("empty curves are invalid, not a crash", "[belt][compare]") {
    auto r = compare_belt_paths({}, {});
    CHECK_FALSE(r.valid);
}

TEST_CASE("the peak window follows the curve's own end", "[belt][compare]") {
    // A printer configured for max_freq 100: nothing above 100 Hz exists.
    BeltCurve c;
    for (float f = 5.0f; f < 100.0f; f += 0.78125f) {
        const float d = (f - 60.0f) / 5.0f;
        c.emplace_back(f, 50.0f + 1e4f / (1.0f + d * d));
    }
    auto r = compare_belt_paths(c, c);
    REQUIRE(r.valid);
    CHECK(r.peak_a_hz == Catch::Approx(60.0f).margin(0.8f));
}
