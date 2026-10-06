// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "platform_capabilities.h"

#include "../catch_amalgamated.hpp"

using helix::PlatformTier;

namespace {
int32_t pressed_scale_for_tier(lv_obj_t* screen, lv_subject_t* tier, PlatformTier t) {
    lv_subject_set_int(tier, static_cast<int>(t));
    auto* card = static_cast<lv_obj_t*>(lv_xml_create(screen, "ui_card", nullptr));
    REQUIRE(card != nullptr);
    lv_obj_add_state(card, LV_STATE_PRESSED);
    const int32_t scale = lv_obj_get_style_transform_scale_x(card, LV_PART_MAIN);
    lv_obj_delete(card);
    return scale;
}
} // namespace

// A scaled card renders through a TRANSFORM layer, so the limited tiers press
// without the scale-down; capable tiers keep it.
TEST_CASE_METHOD(LVGLUITestFixture, "ui_card: pressed scale-down on the capable tier only",
                 "[ui_card][platform_tier]") {
    lv_subject_t* tier = lv_xml_get_subject(nullptr, "platform_tier");
    REQUIRE(tier != nullptr);
    const int saved = lv_subject_get_int(tier);

    CHECK(pressed_scale_for_tier(test_screen(), tier, PlatformTier::STANDARD) < LV_SCALE_NONE);
    CHECK(pressed_scale_for_tier(test_screen(), tier, PlatformTier::BASIC) == LV_SCALE_NONE);
    CHECK(pressed_scale_for_tier(test_screen(), tier, PlatformTier::EMBEDDED) == LV_SCALE_NONE);

    lv_subject_set_int(tier, saved);
    helix::ui::UpdateQueue::instance().drain();
}
