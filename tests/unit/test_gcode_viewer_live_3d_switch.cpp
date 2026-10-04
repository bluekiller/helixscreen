// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A live 2D -> 3D switch on a file that was loaded in 2D builds its geometry on
// the viewer's build thread, and a budget refusal falls back to 2D for that
// file only: the 3D mode the user picked stays set for the next file.

#include "ui_gcode_viewer.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "gcode_parser.h"

#include <glm/glm.hpp>
#include <memory>

#include "../catch_amalgamated.hpp"

#ifdef ENABLE_GLES_3D

using namespace helix::gcode;
using helix::GcodeViewerRenderMode;

namespace {

/// One drawable segment, but a drawable count no memory budget can cover, so
/// GeometryBudgetManager refuses the build on any host.
std::unique_ptr<ParsedGCodeFile> make_over_budget_file() {
    auto file = std::make_unique<ParsedGCodeFile>();
    file->filename = "over_budget.gcode";

    Layer layer;
    layer.z_height = 0.2f;
    ToolpathSegment seg;
    seg.start = glm::vec3(10.0f, 10.0f, 0.2f);
    seg.end = glm::vec3(50.0f, 50.0f, 0.2f);
    seg.is_extrusion = true;
    layer.segments.push_back(seg);
    layer.segment_count_extrusion = 1;
    layer.bounding_box.expand(seg.start);
    layer.bounding_box.expand(seg.end);

    file->layers.push_back(std::move(layer));
    file->total_segments = 1;
    file->drawable_segments = size_t{1} << 40;
    file->global_bounding_box.expand(seg.start);
    file->global_bounding_box.expand(seg.end);
    return file;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture,
                 "a refused live 3D switch falls back to 2D for this file without the LVGL "
                 "thread building",
                 "[gcode_viewer][gcode][render_mode]") {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_t* viewer = ui_gcode_viewer_create(parent);
    REQUIRE(viewer != nullptr);
    lv_obj_set_size(viewer, 400, 300);
    lv_obj_update_layout(viewer);

    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Layer2D);
    helix::test_access::gcode_viewer_install_loaded_file(viewer, make_over_budget_file());
    REQUIRE(ui_gcode_viewer_is_using_2d_mode(viewer));

    ui_gcode_viewer_set_render_mode(viewer, GcodeViewerRenderMode::Render3D);

    // The build has not been answered on the calling thread: the viewer is in
    // 3D, waiting on geometry.
    CHECK_FALSE(ui_gcode_viewer_is_using_2d_mode(viewer));

    helix::test_access::gcode_viewer_wait_for_build(viewer);
    helix::ui::UpdateQueue::instance().drain();

    // Refused: this file draws in 2D, and the user's choice of 3D survives it.
    CHECK(ui_gcode_viewer_is_using_2d_mode(viewer));
    CHECK(helix::test_access::gcode_viewer_render_mode(viewer) == GcodeViewerRenderMode::Render3D);

    lv_obj_delete(parent);
}

#endif // ENABLE_GLES_3D
