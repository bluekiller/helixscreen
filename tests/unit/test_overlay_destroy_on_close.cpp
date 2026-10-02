// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_overlay_destroy_on_close.cpp
 * @brief Overlays that free their widget tree on close rebuild it on reopen
 *
 * Run with: ./build/bin/helix-tests "[overlay_destroy_on_close]"
 *
 * Each case opens the real overlay through its caller path, pops it, and
 * checks the tree was deleted and the overlay's own root pointer dropped, then
 * reopens it and pops it again.
 */

#include "ui_cfs_chute_calibration_overlay.h"
#include "ui_nav_manager.h"
#include "ui_overlay_temp_graph.h"
#include "ui_panel_calibration_pa.h"
#include "ui_panel_calibration_pid.h"
#include "ui_panel_calibration_tool_offset.h"
#include "ui_panel_calibration_zoffset.h"
#include "ui_panel_motion.h"
#include "ui_printer_manager_overlay.h"
#include "ui_settings_macro_buttons.h"
#include "ui_settings_motion.h"
#include "ui_theme_editor_overlay.h"
#include "ui_touch_calibration_overlay.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "static_panel_registry.h"

#include <array>
#include <functional>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

void count_delete(lv_event_t* e) {
    ++*static_cast<int*>(lv_event_get_user_data(e));
}

class DestroyOnCloseFixture : public LVGLUITestFixture {
  protected:
    DestroyOnCloseFixture() {
        for (auto& p : panels_)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels_.data());
    }

    ~DestroyOnCloseFixture() override {
        StaticPanelRegistry::instance().destroy_all();
        settle();
    }

    void settle() {
        for (int i = 0; i < 5; ++i) {
            helix::ui::UpdateQueue::instance().drain();
            process_lvgl(50);
        }
    }

    /// Open, pop, check the tree is gone; then reopen and pop again.
    void expect_rebuilt_on_reopen(const std::function<void()>& open,
                                  const std::function<lv_obj_t*()>& root) {
        for (int round = 0; round < 2; ++round) {
            open();
            settle();
            lv_obj_t* opened = root();
            REQUIRE(opened != nullptr);
            int deletes = 0;
            lv_obj_add_event_cb(opened, count_delete, LV_EVENT_DELETE, &deletes);

            NavigationManager::instance().go_back();
            settle();
            CHECK(deletes == 1);
            CHECK(root() == nullptr);
        }
    }

    std::array<lv_obj_t*, UI_PANEL_COUNT> panels_{};
};

} // namespace

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][motion]") {
    auto& p = get_global_motion_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion observers fired after close touch no freed tree",
                 "[overlay_destroy_on_close][motion]") {
    auto& p = get_global_motion_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
    // The homed and position observers outlive the tree and reach for the jog pad.
    lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");
    settle();
    lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "");
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Macro Buttons rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][macro_buttons]") {
    auto& p = helix::settings::get_macro_buttons_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Motion Settings rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][motion_settings]") {
    expect_rebuilt_on_reopen(
        [&] { helix::settings::show_motion_settings_overlay(); },
        [&] { return lv_obj_find_by_name(lv_screen_active(), "jog_speed_xy_slider"); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Theme Editor rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][theme_editor]") {
    auto& p = get_theme_editor_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Temp Graph rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][temp_graph]") {
    auto& p = get_global_temp_graph_overlay();
    expect_rebuilt_on_reopen([&] { p.open(TempGraphOverlay::Mode::Nozzle, lv_screen_active()); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "PA Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][pa_cal]") {
    auto& p = get_global_pa_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Tool Offset Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][tool_offset_cal]") {
    auto& p = get_global_tool_offset_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

#if HELIX_HAS_CFS
TEST_CASE_METHOD(DestroyOnCloseFixture, "CFS Chute Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][cfs_chute]") {
    auto& p = get_cfs_chute_calibration_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}
#endif

TEST_CASE_METHOD(DestroyOnCloseFixture, "PID Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][pid_cal]") {
    auto& p = get_global_pid_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture,
                 "PID Calibration results and ticks after close touch no tree",
                 "[overlay_destroy_on_close][pid_cal]") {
    auto& p = get_global_pid_cal_panel();
    REQUIRE(p.show(lv_screen_active()));
    settle();
    p.arm_eta_timer_for_test();
    REQUIRE(p.eta_timer_for_test() != nullptr);

    NavigationManager::instance().go_back();
    settle();
    CHECK(p.get_root() == nullptr);
    CHECK(p.eta_timer_for_test() == nullptr);

    p.on_calibration_result(false, 0, 0, 0, "late failure");
    p.on_calibration_result(true, 1.0f, 2.0f, 3.0f);
    settle();
    CHECK(p.get_root() == nullptr);

    REQUIRE(p.show(lv_screen_active()));
    settle();
    CHECK(p.get_state() == PIDCalibrationPanel::State::IDLE);
    NavigationManager::instance().go_back();
    settle();
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Z Offset Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][zoffset_cal]") {
    auto& p = get_global_zoffset_cal_panel();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}

TEST_CASE_METHOD(DestroyOnCloseFixture,
                 "Z Offset Calibration results and probe updates after close touch no tree",
                 "[overlay_destroy_on_close][zoffset_cal]") {
    auto& p = get_global_zoffset_cal_panel();
    REQUIRE(p.show(lv_screen_active()));
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    // The probe observers die with the tree: a Klipper-side probe starting
    // while the overlay is closed must not move a closed panel to ADJUSTING.
    auto& ps = get_printer_state();
    lv_subject_set_int(ps.get_manual_probe_active_subject(), 1);
    settle();
    CHECK(p.get_state() == ZOffsetCalibrationPanel::State::IDLE);
    lv_subject_set_int(ps.get_manual_probe_active_subject(), 0);
    settle();

    p.update_z_position(0.2f);
    p.on_calibration_result(true, "");
    p.on_calibration_result(false, "late failure");
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Touch Calibration rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][touch_cal]") {
    auto& p = helix::ui::get_touch_calibration_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active(), nullptr)); },
                             [&] { return p.get_root(); });
    // The lifted capture surface goes back under the root, so nothing is left on the screen.
    CHECK(lv_obj_find_by_name(lv_screen_active(), "touch_capture_overlay") == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Touch Calibration feedback after close touches no tree",
                 "[overlay_destroy_on_close][touch_cal]") {
    auto& p = helix::ui::get_touch_calibration_overlay();
    REQUIRE(p.show(lv_screen_active(), nullptr));
    settle();
    NavigationManager::instance().go_back();
    settle();
    REQUIRE(p.get_root() == nullptr);

    p.on_progress();
    p.on_capture_feedback({10, 10});
    p.on_verify_feedback({10, 10});
    p.handle_screen_touched(nullptr);
    p.handle_screen_released();
    settle();
    CHECK(p.get_root() == nullptr);
}

TEST_CASE_METHOD(DestroyOnCloseFixture, "Printer Manager rebuilds its tree on reopen",
                 "[overlay_destroy_on_close][printer_manager]") {
    auto& p = get_printer_manager_overlay();
    expect_rebuilt_on_reopen([&] { REQUIRE(p.show(lv_screen_active())); },
                             [&] { return p.get_root(); });
}
