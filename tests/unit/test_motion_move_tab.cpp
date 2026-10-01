// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_motion_move_tab.cpp
 * @brief The Move tab's presets, Park fallback and gating
 *
 * Run with: ./build/bin/helix-tests "[motion][move-tab]"
 *
 * The motion panel's Jog/Move tab switch drives everything through the
 * motion_tab subject: the tab containers' hidden flags follow it, the preset
 * grid computes its target at tap time from the live axis envelope, and Park
 * resolves through StandardMacros, lifting Z and parking over the rear of the
 * plate when no park macro exists. Every grid button is disabled while a print
 * is active, the toolhead is busy, or the nav buttons are off, and the C++
 * handlers re-check the print and nav gates.
 */

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_panel_motion.h"
#include "ui_update_queue.h"
#include "ui_utils.h"

#include "../lvgl_ui_test_fixture.h"
#include "app_globals.h"
#include "moonraker_api.h"
#include "moonraker_client_mock.h"
#include "printer_discovery.h"
#include "printer_state.h"
#include "settings_manager.h"
#include "standard_macros.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <array>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

// The full status frame that makes the panel actionable: axes homed, a
// 235x235x275 envelope, klippy ready, no print running.
nlohmann::json ready_status(const char* homed_axes) {
    return {
        {"toolhead",
         {{"homed_axes", homed_axes},
          {"axis_minimum", {0.0, 0.0, 0.0, 0.0}},
          {"axis_maximum", {235.0, 235.0, 275.0, 0.0}}}},
        {"gcode_move", {{"gcode_position", {10.0, 10.0, 10.0, 0.0}}}},
    };
}

class MoveTabFixture : public LVGLUITestFixture {
  public:
    MoveTabFixture() {
        std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());

        helix::SettingsManager::instance().init_subjects();

        lv_obj_t* cached = nullptr;
        REQUIRE(get_global_motion_panel().show(lv_screen_active()));
        cached = get_global_motion_panel().get_root();
        drain();

        set_moonraker_api(&api_);
        client_.clear_gcode_script_history();

        get_printer_state().update_from_status(ready_status("xyz"));
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        lv_subject_set_int(get_printer_state().get_print_state_enum_subject(),
                           static_cast<int>(helix::PrintJobState::STANDBY));
        // No live connection exists in the fixture, so the derived gate would
        // stay shut; set it the way a connected READY printer would.
        lv_subject_set_int(get_printer_state().get_nav_buttons_enabled_subject(), 1);
        drain();
    }

    ~MoveTabFixture() override {
        set_moonraker_api(previous_api_);
        if (lv_obj_t* top = Modal::get_top()) {
            Modal::hide(top);
        }
        StandardMacros::instance().reset();
        drain();
        StaticPanelRegistry::instance().destroy_all();
        drain();
    }

    static void drain() {
        for (int i = 0; i < 8; ++i) {
            helix::ui::UpdateQueue::instance().drain();
        }
    }

    static void click(lv_obj_t* widget) {
        lv_obj_send_event(widget, LV_EVENT_CLICKED, nullptr);
        drain();
    }

    lv_obj_t* panel_widget(const char* name) const {
        lv_obj_t* widget = lv_obj_find_by_name(get_global_motion_panel().get_root(), name);
        REQUIRE(widget != nullptr);
        return widget;
    }

    std::string all_scripts() const {
        std::string joined;
        for (const auto& script : client_.gcode_script_history()) {
            joined += script;
            joined += "\n";
        }
        return joined;
    }

    // 235x235 with a 10% inset: the rear-right preset lands at 211.5/211.5
    // and the front row at Y 23.5.
    static constexpr float kRearRightXY = 211.5f;
    static constexpr float kFrontY = 23.5f;

    MoonrakerClientMock client_{MoonrakerClientMock::PrinterType::VORON_24};
    MoonrakerAPI api_{client_, get_printer_state()};
    IMoonrakerAPI* previous_api_ = get_moonraker_api();
};

} // namespace

TEST_CASE_METHOD(MoveTabFixture, "motion tab switching toggles the tab containers",
                 "[motion][move-tab][xml]") {
    auto& ps = get_printer_state();
    // Jog is the panel's landing tab: the jog row is up, the Move tab is not.
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_mode_fine"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));

    get_global_motion_panel().set_motion_tab(1);
    drain();
    CHECK(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("jog_mode_fine"), LV_OBJ_FLAG_HIDDEN));
    CHECK_FALSE(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));
    // The Z column stays on every tab.
    CHECK_FALSE(lv_obj_has_flag(panel_widget("z_controls"), LV_OBJ_FLAG_HIDDEN));

    get_global_motion_panel().set_motion_tab(0);
    drain();
    CHECK_FALSE(lv_obj_has_flag(panel_widget("jog_row"), LV_OBJ_FLAG_HIDDEN));
    CHECK(lv_obj_has_flag(panel_widget("move_tab"), LV_OBJ_FLAG_HIDDEN));
}

TEST_CASE_METHOD(MoveTabFixture, "the tab rail renders filled pills", "[motion][move-tab][xml]") {
    // The rail's direct children are the zone_tab roots: the active tab is a
    // solid primary pill, idle tabs sit on the card fill. The AMS strip's
    // flat/selected-card look is the zone_tab default; the motion XML opts
    // into the pill pair by style-name props.
    lv_obj_t* rail = panel_widget("motion_tab_rail");
    REQUIRE(lv_obj_get_child_count(rail) >= 2);
    lv_obj_t* active = lv_obj_get_child(rail, 0);
    lv_obj_t* idle = lv_obj_get_child(rail, 1);
    const auto& palette = ThemeManager::instance().current_palette();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(active, LV_PART_MAIN), palette.primary));
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(idle, LV_PART_MAIN), palette.card_bg));
}

TEST_CASE_METHOD(MoveTabFixture, "homed preset tap sends one absolute move", "[motion][move-tab]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("preset_rear_right"));

    const std::string scripts = all_scripts();
    CHECK(scripts.find("G0 X211.5 Y211.5") != std::string::npos);
    // Exactly one absolute move, and no homing detour for homed axes.
    CHECK(scripts.find("G28") == std::string::npos);
    size_t move_entries = 0;
    for (const auto& script : client_.gcode_script_history()) {
        if (script.find("X211.5") != std::string::npos) {
            ++move_entries;
        }
    }
    CHECK(move_entries == 1);
}

TEST_CASE_METHOD(MoveTabFixture, "unhomed preset tap homes before moving", "[motion][move-tab]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();
    get_printer_state().update_from_status(ready_status(""));
    drain();

    click(panel_widget("preset_front"));

    const std::string scripts = all_scripts();
    CHECK(scripts.find("G28") != std::string::npos);
    CHECK(scripts.find("Y23.5") != std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "park runs the configured macro", "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder", "gcode_macro PARK"});
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::ParkToolhead).detected_macro ==
            "PARK");

    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    const std::string scripts = all_scripts();
    CHECK(scripts.find("PARK") != std::string::npos);
    CHECK(scripts.find("Y23.5") == std::string::npos); // fallback not taken
}

TEST_CASE_METHOD(MoveTabFixture,
                 "park without a macro lifts Z and parks over the rear of the plate",
                 "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);
    REQUIRE(StandardMacros::instance().get(StandardMacroSlot::ParkToolhead).is_empty());

    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_park"));

    // 235x235 plate, commanded Z 10: centred in X, 10mm inside the rear edge,
    // nozzle up 10mm, and never the front preset.
    const std::string scripts = all_scripts();
    CHECK(scripts.find("PARK") == std::string::npos);
    CHECK(scripts.find("X117.5") != std::string::npos);
    CHECK(scripts.find("Y225") != std::string::npos);
    CHECK(scripts.find("Z20") != std::string::npos);
    CHECK(scripts.find("Y23.5") == std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "park that has to home leaves Z where homing put it",
                 "[motion][move-tab]") {
    StandardMacros::instance().reset();
    helix::PrinterDiscovery hardware;
    hardware.parse_objects(nlohmann::json{"extruder"});
    StandardMacros::instance().init(hardware);

    get_global_motion_panel().set_motion_tab(1);
    drain();
    get_printer_state().update_from_status(ready_status(""));
    drain();

    client_.clear_gcode_script_history();
    click(panel_widget("move_park"));

    // The commanded Z the panel holds predates the G28, so a lift from it
    // could command a descent; homing already leaves Z at a safe height.
    bool homed = false;
    bool parked = false;
    bool z_moved = false;
    for (const auto& line : client_.gcode_script_history()) {
        if (line.rfind("G28", 0) == 0) {
            homed = true;
        }
        if (line.find("Y225") != std::string::npos) {
            parked = true;
        }
        if (homed && line.rfind("G0", 0) == 0 && line.find('Z') != std::string::npos) {
            z_moved = true;
        }
    }
    CHECK(homed);
    CHECK(parked);
    CHECK_FALSE(z_moved);
}

TEST_CASE_METHOD(MoveTabFixture, "the grid greys out while the toolhead is busy, Z stays live",
                 "[motion][move-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(1);
    drain();

    lv_obj_t* preset = panel_widget("preset_center");
    lv_obj_t* park = panel_widget("move_park");
    lv_obj_t* z_up = panel_widget("z_up_small");

    ps.update_from_status({{"idle_timeout", {{"state", "Printing"}}}});
    drain();
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(z_up, LV_STATE_DISABLED));

    ps.update_from_status({{"idle_timeout", {{"state", "Ready"}}}});
    drain();
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK_FALSE(lv_obj_has_state(park, LV_STATE_DISABLED));
}

TEST_CASE_METHOD(MoveTabFixture, "motors off opens the shared confirm", "[motion][move-tab]") {
    get_global_motion_panel().set_motion_tab(1);
    drain();

    click(panel_widget("move_motors_off"));

    lv_obj_t* dialog = Modal::get_top();
    REQUIRE(dialog != nullptr);

    // Dismissing the confirm must not disable the motors.
    lv_obj_t* secondary = lv_obj_find_by_name(dialog, "btn_secondary");
    REQUIRE(secondary != nullptr);
    click(secondary);
    CHECK(all_scripts().find("M84") == std::string::npos);
}

TEST_CASE_METHOD(MoveTabFixture, "a print or disabled nav gates the move grid",
                 "[motion][move-tab]") {
    auto& ps = get_printer_state();
    get_global_motion_panel().set_motion_tab(1);
    drain();

    lv_obj_t* preset = panel_widget("preset_rear_right");
    lv_obj_t* park = panel_widget("move_park");
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));

    // The print gate rides the lifecycle pipeline (job_holds_machine ->
    // machine_motion_blocked), never the raw print_active subject: that reads 0
    // while a host-side pre-print block is already homing the toolhead.
    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();
    CHECK(lv_subject_get_int(ps.get_machine_motion_blocked_subject()) == 1);
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(park, LV_STATE_DISABLED));

    ps.update_from_status({{"print_stats", {{"state", "standby"}}}});
    lv_subject_set_int(ps.get_nav_buttons_enabled_subject(), 0);
    drain();
    CHECK(lv_obj_has_state(preset, LV_STATE_DISABLED));

    lv_subject_set_int(ps.get_nav_buttons_enabled_subject(), 1);
    drain();
    CHECK_FALSE(lv_obj_has_state(preset, LV_STATE_DISABLED));

    // The backstop: with the gate shut, a synthesized click on an enabled
    // widget still moves nothing.
    ps.update_from_status({{"print_stats", {{"state", "printing"}}}});
    drain();
    lv_obj_clear_state(preset, LV_STATE_DISABLED);
    click(preset);
    CHECK(all_scripts().empty());
}

TEST_CASE_METHOD(MoveTabFixture,
                 "MotionPanel: a boot-time init_subjects() marks the panel initialized",
                 "[motion][move-tab][init]") {
    // SubjectInitializer initializes the panel before anything opens it; show()
    // skips init_subjects() only when the panel reports itself initialized.
    helix::ui::destroy_static_panels(); // the fixture's own panel is already initialized
    auto& panel = get_global_motion_panel();
    REQUIRE_FALSE(panel.are_subjects_initialized());

    panel.init_subjects();

    CHECK(panel.are_subjects_initialized());
}
