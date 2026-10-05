// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_grid_edit_redraw.cpp
 * @brief How much of the screen each edit-mode step repaints.
 *
 * A slow panel renders about a millisecond per thousand pixels, so a step that
 * invalidates a widget-sized box every pointer move drags at a few frames a
 * second. These cases read the display's invalidated areas after one step.
 */

#include "../test_fixtures.h"
#include "../test_helpers/grid_edit_mode_test_access.h"
#include "../test_helpers/grid_edit_scene.h"
#include "display/lv_display_private.h" // inv_areas / inv_p are private state
#include "grid_edit_mode.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// Render whatever is pending, so the next step's invalidation starts from none.
void flush_invalidation() {
    lv_display_t* disp = lv_display_get_default();
    lv_refr_now(disp);
    REQUIRE(disp->inv_p == 0);
}

/// Pixels the display has queued for repaint since the last render, after a
/// layout pass turns pending position changes into invalidations.
int32_t invalidated_px() {
    lv_display_t* disp = lv_display_get_default();
    lv_obj_update_layout(lv_display_get_screen_active(disp));
    int32_t px = 0;
    for (uint32_t i = 0; i < disp->inv_p; ++i) {
        if (!disp->inv_area_joined[i]) {
            px += lv_area_get_size(&disp->inv_areas[i]);
        }
    }
    return px;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "GridEditMode: an unchanged snap preview repaints nothing",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_snap");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::PAGE_INDEX));

    constexpr int SPAN = GridEditScene::COLSPAN;
    GridEditModeTestAccess::update_snap_preview(em, 0, 0, SPAN, SPAN, true);
    lv_obj_t* preview = GridEditModeTestAccess::snap_preview(em);
    REQUIRE(preview != nullptr);
    flush_invalidation();

    // A resize step whose pointer stays inside the same snapped cell.
    GridEditModeTestAccess::update_snap_preview(em, 0, 0, SPAN, SPAN, true);
    CHECK(GridEditModeTestAccess::snap_preview(em) == preview);
    CHECK(invalidated_px() == 0);

    // A new cell moves the same rect instead of creating another.
    const helix::CellMetrics m = GridEditModeTestAccess::cell_metrics(em);
    GridEditModeTestAccess::update_snap_preview(em, SPAN, 0, SPAN, SPAN, false);
    CHECK(GridEditModeTestAccess::snap_preview(em) == preview);
    lv_obj_update_layout(scene.container);
    CHECK(lv_obj_get_x(preview) == static_cast<int>(grid_track_origin(m.cell_w, m.gutter, SPAN)));
    CHECK(invalidated_px() > 0);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}

TEST_CASE_METHOD(XMLTestFixture,
                 "GridEditMode: a resize step repaints the outline's edges, not its box",
                 "[grid_edit][grid_edit_redraw]") {
    GridEditScene scene(test_screen(), "test_grid_edit_redraw_outline");
    GridEditMode em;
    em.enter(scene.container, scene.config, static_cast<int>(GridEditScene::PAGE_INDEX));

    GridEditModeTestAccess::make_resize_preview(em, 0, 0, 200, 150);
    flush_invalidation();

    // The right edge follows the pointer 60px outward.
    constexpr int W = 260;
    constexpr int H = 150;
    GridEditModeTestAccess::make_resize_preview(em, 0, 0, W, H);
    const int32_t repainted = invalidated_px();
    INFO("repainted " << repainted << " px for a " << W << "x" << H << " outline");
    CHECK(repainted > 0);
    // A box outline repaints its old and new areas, more than W*H. The bars
    // repaint strips a few pixels thick.
    CHECK(repainted < W * H / 8);

    // Still drawn where asked: the right bar sits on the box's right edge.
    lv_obj_update_layout(scene.container);
    lv_obj_t* right = GridEditModeTestAccess::resize_outline(em)[3];
    REQUIRE(right != nullptr);
    CHECK(lv_obj_get_x(right) + lv_obj_get_width(right) == W);
    CHECK(lv_obj_get_height(right) == H);

    em.exit();
    process_lvgl(50);
    lv_obj_delete(scene.container);
}
