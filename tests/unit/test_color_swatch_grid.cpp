// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_color_picker.h"

#include "../lvgl_ui_test_fixture.h"

#include <vector>

#include "../catch_amalgamated.hpp"

extern "C" {
#include "helix-xml/src/xml/lv_xml.h"
}

namespace {

std::vector<uint32_t> g_tapped;

/// Reads the color the way every grid consumer does: off the event target.
void record_tap(lv_event_t* e) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    g_tapped.push_back(lv_color_to_u32(lv_obj_get_style_bg_color(target, LV_PART_MAIN)) & 0xFFFFFF);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "color_swatch_grid: a tap reports the tapped palette color",
                 "[color_swatch]") {
    lv_xml_register_event_cb(nullptr, "test_grid_tap_cb", record_tap);
    for (const auto palette :
         {helix::ui::ColorPicker::Palette::General, helix::ui::ColorPicker::Palette::Theme}) {
        const char* component = palette == helix::ui::ColorPicker::Palette::General
                                    ? "color_swatch_grid"
                                    : "theme_swatch_grid";
        INFO(component);
        const char* attrs[] = {"swatch_callback", "test_grid_tap_cb", nullptr};
        auto* grid = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), component, attrs));
        REQUIRE(grid != nullptr);
        const auto& expected = helix::ui::swatch_palette(palette);
        REQUIRE(lv_obj_get_child_count(grid) == expected.size());

        g_tapped.clear();
        for (uint32_t i = 0; i < expected.size(); ++i) {
            lv_obj_send_event(lv_obj_get_child(grid, static_cast<int32_t>(i)), LV_EVENT_CLICKED,
                              nullptr);
        }
        CHECK(g_tapped == expected);
        lv_obj_delete(grid);
    }
}

TEST_CASE("swatch_needs_light_edge: bright, unsaturated fills only", "[color_swatch]") {
    CHECK(helix::ui::swatch_needs_light_edge(0xFFFFFF));
    CHECK(helix::ui::swatch_needs_light_edge(0xE8E8E8));
    CHECK(helix::ui::swatch_needs_light_edge(0xE0D5C7));
    CHECK(helix::ui::swatch_needs_light_edge(0xEAF2FF));
    CHECK_FALSE(helix::ui::swatch_needs_light_edge(0xFFEB3B)); // yellow: bright but saturated
    CHECK_FALSE(helix::ui::swatch_needs_light_edge(0x808080)); // mid gray
    CHECK_FALSE(helix::ui::swatch_needs_light_edge(0x1A1A1A));
}
