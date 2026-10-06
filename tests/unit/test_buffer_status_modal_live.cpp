// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_status_modal_live.cpp
 * @brief The Buffer Status modal follows the backend while it is open.
 */

#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/registered_backend.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "buffer_status_modal.h"
#include "helix-xml/src/xml/lv_xml.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::string subject_text(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_string(s);
}

int subject_int(const char* name) {
    lv_subject_t* s = lv_xml_get_subject(nullptr, name);
    REQUIRE(s != nullptr);
    return lv_subject_get_int(s);
}

void set_pressure(AmsBackendMock& mock, float pressure) {
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    mock.set_unit_buffer_health(0, fps);
}

/// What a backend event does: sync, then announce it.
void land_backend_update() {
    AmsState::instance().sync_from_backend();
    AmsState::instance().bump_data_revision();
    ui::UpdateQueue::instance().drain();
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "Buffer Status modal follows the backend while open",
                 "[modals][buffer_status][live]") {
    AmsState::instance().init_subjects(true);
    test::RegisteredBackend<AmsBackendMock> mock(4);
    // Neither Happy Hare nor AFC: the modal's own filament-pressure view.
    mock->set_tool_changer_mode(true);
    set_pressure(*mock, 0.32f);
    land_backend_update();

    BufferStatusModal modal;
    REQUIRE(modal.show(test_screen()));
    ui::UpdateQueue::instance().drain();

    REQUIRE(subject_int("buf_type") == 3);
    CHECK(subject_text("buf_pressure") == "Pressure: 32% (target 50%)");
    CHECK(subject_text("buf_description") == "Filament is pulling tight");

    SECTION("a new reading lands in the open modal") {
        set_pressure(*mock, 0.71f);
        land_backend_update();
        CHECK(subject_text("buf_pressure") == "Pressure: 71% (target 50%)");
        CHECK(subject_text("buf_description") == "Filament is loose");
    }

    SECTION("the backend vanishing falls back to the unsupported message") {
        AmsState::instance().clear_backends();
        ui::UpdateQueue::instance().drain();
        CHECK(subject_int("buf_type") == 0);
        CHECK_FALSE(subject_text("buf_unsupported").empty());
    }

    modal.hide();
    ui::UpdateQueue::instance().drain();
}
