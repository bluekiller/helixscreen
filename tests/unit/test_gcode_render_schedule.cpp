// SPDX-License-Identifier: GPL-3.0-or-later
#include "gcode_render_schedule.h"

#include "../catch_amalgamated.hpp"

using namespace helix::gcode::render_schedule;

TEST_CASE("an unmeasured rate falls back to the seed rate", "[gcode][render_schedule]") {
    REQUIRE(effective_rate(0.0f) == kSeedRateTrisPerMs);
    REQUIRE(effective_rate(-1.0f) == kSeedRateTrisPerMs);
    REQUIRE(effective_rate(9000.0f) == 9000.0f);
}

TEST_CASE("the rate learns from slices and ignores empty samples", "[gcode][render_schedule]") {
    REQUIRE(update_rate(0.0f, 48000, 12.0f) == Catch::Approx(4000.0f));
    // Smoothed: 4000 + 0.3 * (8000 - 4000)
    REQUIRE(update_rate(4000.0f, 96000, 12.0f) == Catch::Approx(5200.0f));
    REQUIRE(update_rate(4000.0f, 0, 12.0f) == 4000.0f);
    REQUIRE(update_rate(4000.0f, 1000, 0.0f) == 4000.0f);
}

TEST_CASE("slice quotas aim at the target slice time", "[gcode][render_schedule]") {
    REQUIRE(slice_quota(0.0f) == static_cast<size_t>(kSeedRateTrisPerMs * kSliceTargetMs));
    REQUIRE(slice_quota(10000.0f) == 120000);
    REQUIRE(slice_quota(1.0f) == kMinSliceTriangles);
}

TEST_CASE("the moving plan keeps fast GPUs at full detail", "[gcode][render_schedule]") {
    // Pi 5 class: 40k tris/ms, budget 1M; a 600k model fits.
    MovingPlan p = plan_moving(600000, 40000.0f);
    REQUIRE(p.stride == 1);
    REQUIRE_FALSE(p.half_resolution);
}

TEST_CASE("the moving plan strides and halves resolution on a Pi 3B", "[gcode][render_schedule]") {
    // 4000 tris/ms * 25 ms = 100k budget; Benchy tier 3 is 523k triangles.
    MovingPlan p = plan_moving(523000, 4000.0f);
    REQUIRE(p.stride == 6);
    REQUIRE(p.half_resolution);
}

TEST_CASE("an unmeasured rate never submits a huge model in one moving frame",
          "[gcode][render_schedule]") {
    MovingPlan p = plan_moving(4660000, 0.0f);
    REQUIRE(p.stride == 47); // ceil(4.66M / 100k)
    REQUIRE(p.half_resolution);
    REQUIRE(plan_moving(0, 0.0f).stride == 1);
}

static JobInputs idle_complete(int progress) {
    JobInputs in{};
    in.have_complete_image = true;
    in.job_progress = progress;
    in.new_progress = progress;
    return in;
}

TEST_CASE("nothing changed means no work", "[gcode][render_schedule]") {
    REQUIRE(decide_job(idle_complete(10)) == JobAction::Keep);
}

TEST_CASE("a running job is left alone when nothing changed", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    REQUIRE(decide_job(in) == JobAction::Keep);
}

TEST_CASE("no complete image restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false; // a moving frame replaced the still image
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("any scene change restarts, even mid incremental job", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.scene_changed = true;
    in.job_running = true;
    in.job_incremental = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress advancing over a complete image is incremental", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.new_progress = 12;
    REQUIRE(decide_job(in) == JobAction::Incremental);
}

TEST_CASE("progress advancing during an incremental job extends it", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    in.job_incremental = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Extend);
}

TEST_CASE("progress advancing during a still job restarts it", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("progress went backwards restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.new_progress = 3;
    REQUIRE(decide_job(in) == JobAction::Restart);
}

TEST_CASE("ghost off restarts, and ghost on from off restarts", "[gcode][render_schedule]") {
    JobInputs off = idle_complete(10);
    off.new_progress = -1;
    REQUIRE(decide_job(off) == JobAction::Restart);
    JobInputs on = idle_complete(-1);
    on.new_progress = 4;
    REQUIRE(decide_job(on) == JobAction::Restart);
}

TEST_CASE("a highlighted selection forces full restarts", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.selection_active = true;
    in.new_progress = 11;
    REQUIRE(decide_job(in) == JobAction::Restart);
}
