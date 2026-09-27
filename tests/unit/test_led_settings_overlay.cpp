// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_settings_overlay.cpp
 * @brief LED Settings overlay: the light-button chip row is gone, and
 *        Automatic LED Control's "Applies to" row drives its own targets.
 */

#include "ui_settings_led.h"

#include "../lvgl_ui_test_fixture.h"
#include "led/led_auto_state.h"
#include "led/led_controller.h"

#include <vector>

#include "../catch_amalgamated.hpp"

TEST_CASE_METHOD(LVGLUITestFixture, "LED settings: no selection row, an Applies to row",
                 "[led][settings]") {
    lv_obj_t* root =
        static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "led_settings_overlay", nullptr));
    REQUIRE(root != nullptr);
    CHECK(lv_obj_find_by_name(root, "row_led_select") == nullptr);
    CHECK(lv_obj_find_by_name(root, "row_auto_state_strips") != nullptr);
}

namespace helix::settings {
class LedSettingsOverlayTestAccess {
  public:
    static void delete_macro(int index) {
        get_led_settings_overlay().handle_delete_macro_device(index);
    }
};
} // namespace helix::settings

TEST_CASE_METHOD(LVGLTestFixture, "deleting a macro device removes it from Applies to",
                 "[led][settings]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    helix::led::LedMacroInfo m;
    m.display_name = "Lamp";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "LIGHT_TOGGLE";
    ctrl.set_configured_macros({m});
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({"macro:Lamp", "neopixel chamber_light"});

    helix::settings::LedSettingsOverlayTestAccess::delete_macro(0);

    CHECK(as.strips() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({});
    ctrl.deinit();
}
