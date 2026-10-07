// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_buffer.cpp
 * @brief AmsState's filament buffer reading: its traces and its subjects.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The traces and the buffer_* subjects are the singleton's, read by later tests.
struct ResetBuffer {
    AmsState& ams;
    ~ResetBuffer() {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        AmsStateTestAccess::clear_buffer_traces(ams);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState keeps a trace per buffer reading",
                 "[ams][buffer][trace]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetBuffer reset{ams};

    // Unit 1 feeds the toolhead: unit 0 reads +0.8, unit 1 reads -0.4.
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    AmsStateTestAccess::sync_buffer(ams, info, 1000);

    REQUIRE_FALSE(ams.buffer_trace(-1).window(1000).empty());
    CHECK(ams.buffer_trace(-1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(0).window(1000).back().bias == Catch::Approx(0.8f));
    CHECK(ams.buffer_trace(1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(5).size() == 0);

    SECTION("a unit that goes away takes its trace with it") {
        info.units.pop_back();
        info.current_slot = -1;
        AmsStateTestAccess::sync_buffer(ams, info, 2000);
        CHECK(ams.buffer_trace(1).size() == 0);
        CHECK(ams.buffer_trace(-1).window(2000).back().bias == Catch::Approx(0.8f));
    }

    SECTION("clear_backends drops every trace") {
        ams.clear_backends();
        CHECK(ams.buffer_trace(-1).size() == 0);
        CHECK(ams.buffer_trace(0).size() == 0);
    }
}
