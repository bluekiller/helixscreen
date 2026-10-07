// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_buffer_widget.cpp
 * @brief The Filament Buffer home widget: its registry row, its gate, and
 *        what it shows at 1x1 and 2x1.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "ams_state.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/filament_buffer_widget.h"

#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCell = GridLayout::TRACKS_PER_CELL;

bool hidden(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
std::string text(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}
} // namespace

TEST_CASE("filament_buffer is one cell, growable to two wide", "[widget_size][filament_buffer]") {
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);
    CHECK(def->colspan == 1 * kCell);
    CHECK(def->rowspan == 1 * kCell);
    CHECK(def->effective_min_colspan() == 1 * kCell);
    CHECK(def->effective_max_colspan() == 2 * kCell);
    CHECK(def->effective_max_rowspan() == 1 * kCell);
    REQUIRE(def->hardware_gate_subject != nullptr);
    CHECK(std::string_view(def->hardware_gate_subject) == "buffer_present");
    CHECK_FALSE(def->default_enabled);
}

TEST_CASE_METHOD(LVGLUITestFixture, "filament_buffer: what each size and reading shows",
                 "[filament_buffer]") {
    PanelWidgetManager::instance().init_widget_subjects();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<FilamentBufferWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);

    AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}), 0);

    SECTION("1x1: slider, label and number, no trace") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        CHECK_FALSE(hidden(h.child("buffer_graphics")));
        CHECK(hidden(h.child("buffer_trace")));
        CHECK(hidden(h.child("buffer_lean")));
        CHECK(text(h.child("buffer_label")) == "FPS");
        CHECK(text(h.child("buffer_value_short")) == "71%");
    }

    SECTION("2x1: the trace and the lean in words") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        CHECK_FALSE(hidden(h.child("buffer_trace")));
        CHECK_FALSE(hidden(h.child("buffer_lean")));
        CHECK(text(h.child("buffer_lean")) == "Running loose");
    }

    SECTION("no set point: the number alone") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}, -1.0f), 0);
        CHECK(hidden(h.child("buffer_graphics")));
        CHECK(hidden(h.child("buffer_label")));
        CHECK(text(h.child("buffer_value")) == "Pressure: 71%");
    }

    SECTION("1x1 with a set point: the short number is the one showing") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        CHECK_FALSE(hidden(h.child("buffer_value_short")));
        CHECK(text(h.child("buffer_value_short")) == "71%");
        CHECK(hidden(h.child("buffer_value")));
    }

    SECTION("1x1 with no set point: the short number alone") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}, -1.0f), 0);
        CHECK(hidden(h.child("buffer_graphics")));
        CHECK_FALSE(hidden(h.child("buffer_label")));
        CHECK(text(h.child("buffer_label")) == "FPS");
        CHECK_FALSE(hidden(h.child("buffer_value_short")));
        CHECK(text(h.child("buffer_value_short")) == "71%");
        CHECK(hidden(h.child("buffer_value")));
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
