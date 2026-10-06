// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
// TEST_MIRROR_OK: exercises patches/lvgl_scroll_throw_time_based.patch, which has
//                 no HelixScreen header to include

/**
 * @file test_indev_scroll_throw_timing.cpp
 * @brief Scroll momentum is measured in wall time, not in frames.
 *
 * The same flick, read and animated at the nominal 33 ms period and at a slow
 * device's 300 ms, must glide the same distance and stop at about the same
 * time. At the nominal period every step must match LVGL's per-step recurrence
 * exactly, so desktop feel does not move.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/scoped_pointer_indev.h"

#include <cstdlib>

#include "../catch_amalgamated.hpp"

namespace {

constexpr uint32_t NOMINAL_MS = LV_DEF_REFR_PERIOD;
constexpr uint32_t SLOW_MS = 300;
constexpr int SCROLL_THROW = 25;

/// Finger speed of 1 px/ms.
constexpr int px_per_read(uint32_t period_ms) {
    return static_cast<int>(period_ms);
}

struct Glide {
    int32_t at_600ms = 0; ///< glide distance 600 ms after the release read
    int32_t total = 0;    ///< glide distance once momentum has run out
    int32_t vect_ori = 0; ///< throw vector the release sampled
    bool ended = false;   ///< momentum ran out within the 3300 ms
};

/// A 300 ms upward flick read every @p period_ms, then momentum stepped at the
/// same period for 3300 ms (a common multiple of both cadences).
Glide flick(lv_obj_t* screen, uint32_t period_ms) {
    lv_obj_t* list = lv_obj_create(screen);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, TEST_DISPLAY_WIDTH, TEST_DISPLAY_HEIGHT);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_t* content = lv_obj_create(list);
    lv_obj_set_size(content, TEST_DISPLAY_WIDTH, 5000);
    lv_obj_update_layout(screen);

    helix_test::ScopedPointerIndev pointer;
    lv_indev_set_scroll_throw(pointer.indev(), SCROLL_THROW);

    int y = 420;
    pointer.press(400, y);
    for (uint32_t t = 0; t < 300; t += period_ms) {
        lv_tick_inc(period_ms);
        y -= px_per_read(period_ms);
        pointer.move(400, y);
    }
    lv_tick_inc(period_ms);
    const int32_t released_at = lv_obj_get_scroll_y(list);
    pointer.release(400, y);

    Glide g;
    g.vect_ori = pointer.indev()->pointer.scroll_throw_vect_ori.y;
    for (uint32_t t = period_ms; t <= 3300; t += period_ms) {
        lv_tick_inc(period_ms);
        lv_anim_refr_now();
        if (t == 600 || (t < 600 && t + period_ms > 600)) {
            g.at_600ms = lv_obj_get_scroll_y(list) - released_at;
        }
    }
    g.total = lv_obj_get_scroll_y(list) - released_at;
    g.ended = pointer.indev()->pointer.scroll_throw_vect.y == 0;

    lv_obj_delete(list);
    return g;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "Scroll throw glides the same distance at any frame rate",
                 "[indev][scroll_throw]") {
    const Glide nominal = flick(test_screen(), NOMINAL_MS);
    const Glide slow = flick(test_screen(), SLOW_MS);

    INFO("nominal vect " << nominal.vect_ori << " total " << nominal.total << " at 600ms "
                         << nominal.at_600ms);
    INFO("slow    vect " << slow.vect_ori << " total " << slow.total << " at 600ms "
                         << slow.at_600ms);

    REQUIRE(nominal.total > 100);

    SECTION("the sampled throw vector does not grow with the read period") {
        CHECK(std::abs(slow.vect_ori - nominal.vect_ori) <= 2);
    }

    SECTION("the glide covers the same distance") {
        CHECK(std::abs(slow.total - nominal.total) * 10 <= nominal.total);
    }

    SECTION("the glide is over by the same wall time") {
        CHECK(nominal.ended);
        CHECK(slow.ended);
        CHECK(nominal.at_600ms * 100 >= nominal.total * 95);
        CHECK(slow.at_600ms * 100 >= slow.total * 95);
    }
}

TEST_CASE_METHOD(LVGLTestFixture,
                 "Scroll throw at the nominal period matches LVGL's per-step recurrence",
                 "[indev][scroll_throw]") {
    const Glide nominal = flick(test_screen(), NOMINAL_MS);

    // One step per period: decay by scroll_throw percent, then move by the result.
    int32_t v = nominal.vect_ori;
    int32_t expected = 0;
    while (v != 0) {
        v = v * (100 - SCROLL_THROW) / 100;
        expected += v;
    }
    CHECK(nominal.total == -expected);
}
