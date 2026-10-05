// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 356C LLC

/**
 * @file test_wizard_touch_calibration_preview.cpp
 * @brief The wizard previews the mapping 'Next' persists (prestonbrown/helixscreen#1714)
 *
 * lv_evdev scales by the installed ABS range and clamps to the display before
 * the affine runs. On a panel whose declared range under-covers an axis, a full
 * affine installed behind that range can never reach the clamped edge, which is
 * where 'Next' sits. The preview therefore has to install the solved range plus
 * its residual, and the session has to put the declared range back on Back.
 */

#include "ui_wizard_touch_calibration.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/wizard_touch_calibration_test_access.h"
#include "touch_calibration.h"
#include "touch_calibration_panel.h"
#include "touch_calibration_session.h"

#include <algorithm>
#include <cstdlib>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

constexpr int PANEL_W = 480;
constexpr int PANEL_H = 800;
// The kernel declares Y as 0..640 while the digitizer emits 0..799, so lv_evdev
// scales Y up and clamps everything past raw 640 onto the bottom row.
const TouchRangeSettings DECLARED{true, false, 0, PANEL_W - 1, 0, 640};

/// lv_evdev's _evdev_calibrate(): integer scale, unconditional clamp.
int evdev_calibrate(int v, int in_min, int in_max, int out_max) {
    if (in_min != in_max) {
        v = (v - in_min) * out_max / (in_max - in_min);
    }
    return std::max(0, std::min(v, out_max));
}

/// Models the device's two stages: the evdev range (swap, scale, clamp) and the
/// affine on top, each restorable the way the real backend's are.
struct PipelineSink : ICalibrationSink {
    TouchRangeSettings range = DECLARED;
    TouchCalibration stored{};
    bool affine_enabled = true;

    TouchCalibration current_calibration() const override {
        return stored;
    }
    bool apply_calibration(const TouchCalibration& cal) override {
        if (!cal.valid) {
            return false;
        }
        stored = cal;
        affine_enabled = true;
        return true;
    }
    void disable_affine() override {
        affine_enabled = false;
    }
    void enable_affine() override {
        affine_enabled = true;
    }
    void clear_calibration() override {
        stored = TouchCalibration{};
    }
    TouchRangeSettings current_touch_range() const override {
        return range;
    }
    bool apply_touch_range(bool swap, int min_x, int min_y, int max_x, int max_y) override {
        range = TouchRangeSettings{true, swap, min_x, max_x, min_y, max_y};
        return true;
    }

    /// Where a raw digitizer reading lands on screen right now.
    Point map(Point raw) const {
        const int sx = range.swap_axes ? raw.y : raw.x;
        const int sy = range.swap_axes ? raw.x : raw.y;
        Point p{evdev_calibrate(sx, range.min_x, range.max_x, PANEL_W - 1),
                evdev_calibrate(sy, range.min_y, range.max_y, PANEL_H - 1)};
        if (affine_enabled && stored.valid) {
            p = transform_point(stored, p, PANEL_W - 1, PANEL_H - 1);
        }
        return p;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "Wizard preview installs the solved range, Back restores the declared one",
                 "[wizard][touch][calibration][range-fit][1714]") {
    WizardTouchCalibrationStep step;
    step.init_subjects();

    PipelineSink sink;
    WizardTouchCalibrationTestAccess::set_calibration_sink(step, &sink);
    WizardTouchCalibrationTestAccess::session(step).begin_capture(sink);

    TouchCalibrationPanel* panel = WizardTouchCalibrationTestAccess::panel(step);
    panel->set_screen_size(PANEL_W, PANEL_H);
    panel->start();
    // The digitizer emits screen coordinates; the touch LVGL sees went through
    // the declared range with the affine off.
    for (int i = 0; i < 3; i++) {
        const Point raw = panel->get_target_position(i);
        const Point touch = sink.map(raw);
        for (int s = 0; s < 3; s++) {
            panel->add_sample(touch, &raw);
        }
    }
    // The wizard auto-accepts on reaching VERIFY.
    REQUIRE(panel->get_state() == TouchCalibrationPanel::State::COMPLETE);
    const TouchCalibration* cal = panel->get_calibration();
    REQUIRE(cal != nullptr);
    REQUIRE(cal->valid);
    REQUIRE(panel->get_range_fit().valid);

    lv_obj_t* root = lv_obj_create(lv_screen_active());
    WizardTouchCalibrationTestAccess::invoke_calibration_complete(step, cal, root);

    // Before 'Next': the preview already runs the solved range.
    INFO("preview range X(" << sink.range.min_x << ".." << sink.range.max_x << ") Y("
                            << sink.range.min_y << ".." << sink.range.max_y << ")");
    CHECK(std::abs(sink.range.min_x - 0) <= 5);
    CHECK(std::abs(sink.range.max_x - (PANEL_W - 1)) <= 5);
    CHECK(std::abs(sink.range.min_y - 0) <= 5);
    CHECK(std::abs(sink.range.max_y - (PANEL_H - 1)) <= 5);
    // A touch near the bottom edge, where 'Next' sits, reaches it.
    CHECK(sink.map({240, 790}).y > 750);

    // Back out without committing: the declared range is what the device runs.
    WizardTouchCalibrationTestAccess::session(step).restore(sink);
    CHECK(sink.range.max_y == DECLARED.max_y);
    CHECK(sink.range.max_x == DECLARED.max_x);

    lv_obj_delete(root);
}

TEST_CASE_METHOD(LVGLUITestFixture, "Wizard retry re-captures under the declared range",
                 "[wizard][touch][calibration][retry][range-fit][1714]") {
    WizardTouchCalibrationStep step;
    step.init_subjects();

    PipelineSink sink;
    WizardTouchCalibrationTestAccess::set_calibration_sink(step, &sink);
    WizardTouchCalibrationTestAccess::session(step).begin_capture(sink);

    // A previewed candidate re-programmed the range.
    sink.apply_touch_range(false, 0, 0, PANEL_W - 1, PANEL_H - 1);

    WizardTouchCalibrationTestAccess::invoke_retry(step);

    CHECK(sink.range.max_y == DECLARED.max_y);
    CHECK_FALSE(sink.affine_enabled);
}

TEST_CASE("TouchCalibrationSession: a committed range survives restore",
          "[touch-calibration][session][range-fit][1714]") {
    PipelineSink sink;
    TouchCalibrationSession session;
    session.begin_capture(sink);

    sink.apply_touch_range(false, 0, 0, PANEL_W - 1, PANEL_H - 1);
    session.commit();
    session.restore(sink);

    CHECK(sink.range.max_y == PANEL_H - 1);
}
