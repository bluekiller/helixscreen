// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_clog_meter_pressure_source.cpp
 * @brief Filament pressure (sync-feedback bias) as a clog-meter source: when
 *        AmsState picks it, and what it publishes.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "clog_meter_geometry.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterMode;

namespace {

constexpr int kPressure = static_cast<int>(ClogMeterMode::Pressure);

int mode(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_mode_subject());
}
int value(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_value_subject());
}
std::string text(lv_subject_t* s) {
    return lv_subject_get_string(s);
}
/// What clog_bar_body's end labels bind to.
int symmetrical(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_symmetrical_subject());
}

AmsSystemInfo fps_info(float pressure) {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    unit.buffer_health = fps;
    info.units.push_back(unit);
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

/// The meter subjects and the override are the singleton's, read by later tests.
struct ResetMeter {
    AmsState& ams;
    ~ResetMeter() {
        ams.set_source_override(0);
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: Happy Hare sync feedback is a pressure source",
                 "[ams][clog][pressure]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    AmsSystemInfo info;
    info.sync_feedback_bias = -0.45f;
    AmsStateTestAccess::sync_clog_meter(ams, info);

    CHECK(mode(ams) == kPressure);
    CHECK(value(ams) == -45);
    CHECK(text(ams.get_clog_meter_mode_text_subject()) == "Sync");
    CHECK(text(ams.get_clog_meter_center_text_subject()) == "-45%");
    CHECK(text(ams.get_clog_meter_label_left_subject()) == "TIGHT");
    CHECK(text(ams.get_clog_meter_label_right_subject()) == "LOOSE");
    CHECK(symmetrical(ams) == 1);
    CHECK(lv_subject_get_int(ams.get_clog_meter_status_subject()) ==
          static_cast<int>(helix::ui::ClogMeterStatus::Warning));
    CHECK(lv_subject_get_int(ams.get_clog_meter_warning_subject()) == 0);

    SECTION("near the end stop it reads as a fault") {
        info.sync_feedback_bias = 0.8f;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(lv_subject_get_int(ams.get_clog_meter_warning_subject()) == 1);
        CHECK(lv_subject_get_int(ams.get_clog_meter_status_subject()) ==
              static_cast<int>(helix::ui::ClogMeterStatus::Fault));
    }

    SECTION("no bias, no source") {
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
        CHECK(mode(ams) == 0);
        CHECK(symmetrical(ams) == 0);
    }
}

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: a filament pressure sensor reads its pressure",
                 "[ams][clog][pressure][fps]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));

    CHECK(mode(ams) == kPressure);
    CHECK(value(ams) == -36); // (0.32 - 0.5) / 0.5
    CHECK(text(ams.get_clog_meter_mode_text_subject()) == "FPS");
    CHECK(text(ams.get_clog_meter_center_text_subject()) == "32%");
}

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: a clog detector outranks pressure unless chosen",
                 "[ams][clog][pressure]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    AmsSystemInfo info;
    info.sync_feedback_bias = 0.15f;
    info.encoder_info.enabled = true;
    AmsStateTestAccess::sync_clog_meter(ams, info);
    CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));

    ams.set_source_override(4);
    AmsStateTestAccess::sync_clog_meter(ams, info);
    CHECK(mode(ams) == kPressure);
    CHECK(value(ams) == 15);

    SECTION("a forced pressure source with no bias shows nothing") {
        info.sync_feedback_bias = -2.0f;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == 0);
    }
}
