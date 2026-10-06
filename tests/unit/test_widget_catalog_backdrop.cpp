// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_nav_manager.h"
#include "ui_widget_catalog_overlay.h"

#include "../lvgl_ui_test_fixture.h"
#include "config.h"
#include "panel_widget_config.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

std::vector<lv_obj_t*> visible_children(lv_obj_t* parent) {
    std::vector<lv_obj_t*> out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t* c = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
            out.push_back(c);
    }
    return out;
}

} // namespace

// The catalog changes the screen in one frame: nothing it adds is visible until
// the navigation push lands, and the push and the backdrop land in the same
// queue drain. A backdrop shown at show() time is drawn in a frame of its own,
// and the whole screen is drawn again when the push follows.
TEST_CASE_METHOD(LVGLUITestFixture, "widget catalog: nothing shows before the push lands",
                 "[widget_catalog]") {
    PanelWidgetConfig config("test_widget_catalog_backdrop", *Config::get_instance());
    config.load();
    const auto before = visible_children(test_screen());

    WidgetCatalogOverlay::show(test_screen(), config, [](const std::string&) {});
    CHECK(visible_children(test_screen()) == before);

    process_lvgl(10);
    lv_obj_t* overlay = lv_obj_find_by_name(test_screen(), "widget_catalog_overlay");
    REQUIRE(overlay != nullptr);
    CHECK_FALSE(lv_obj_has_flag(overlay, LV_OBJ_FLAG_HIDDEN));
    // The backdrop arrived with the push, behind the panel.
    const auto after = visible_children(test_screen());
    CHECK(after.size() >= before.size() + 2);
    CHECK(after.back() == overlay);

    NavigationManager::instance().go_back();
    process_lvgl(10);
}
