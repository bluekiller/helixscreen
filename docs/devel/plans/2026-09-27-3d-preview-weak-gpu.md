# 3D G-code Preview on Weak GPUs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the GLES 3D G-code preview smooth on a Raspberry Pi 3B (VideoCore IV) while dragging, sharp soon after release, and cheap during a live print, without regressing fast GPUs.

**Architecture:** A pure policy header (`include/gcode_render_schedule.h`) decides slice quotas, the moving-view stride and resolution, and whether a state change restarts, extends or incrementally updates the render. `GCodeGLESRenderer` turns those decisions into GL calls. Still frames are drawn in slices across LVGL frames into the retained FBO. Moving frames draw every Nth layer at half resolution. Print-progress steps draw only the new layers onto the retained color and depth buffers.

**Tech Stack:** C++17 (CI's clang, so no captured structured bindings), OpenGL ES 2.0 via EGL, LVGL 9.5, Catch2, spdlog.

**Spec:** `docs/devel/plans/2026-09-27-3d-preview-weak-gpu-design.md` (approved 2026-09-27). Read it before any task.

**Reference material for implementers:** `.superpowers/sdd/3d-weak-gpu/renderer-digest.md` (verbatim excerpts of the renderer and viewer as of `3cf983335`: class declaration, `render()`, `render_to_fbo`, `draw_layers`, `create_fbo`, `blit_to_lvgl`, the viewer's draw callback). Hardware drivers: `.superpowers/sdd/3d-weak-gpu/vc4run.sh` (stock-mode load plus scripted drag on the Pi 3B) and `run.sh` (spike autospin benchmark).

## Global Constraints

- Work in worktree `.worktrees/3d-weak-gpu`, branch `feature/3d-weak-gpu`, created with `scripts/setup-worktree.sh feature/3d-weak-gpu` from a main that contains `ab269f564` (the vc4 triangle cap).
- C++17 as compiled by CI's clang: do not capture a structured binding in a lambda.
- `spdlog` only; `// SPDX-License-Identifier: GPL-3.0-or-later` on new files; namespace `helix::gcode` for new code (the namespace ratchet in `scripts/quality-checks.sh` fails on new global symbols).
- All renderer code stays inside the existing `#ifdef ENABLE_GLES_3D` guards. The new policy header is header-only (inline functions), so the ESP32 source manifest needs no change.
- No comment archaeology (no SHAs, "used to", "previously", narrated bug history) and no em-dashes, in code, tests and docs.
- Inner loop: `make -C .worktrees/3d-weak-gpu t F='<tag>'`. Check `pgrep -x -d' ' 'make|clang++|cc1plus'` before building and size `-j` with `scripts/helix-claim jobs`. Run builds in the foreground, never through `head`/`tail`/`grep`.
- Commit with explicit pathspecs: `git -C .worktrees/3d-weak-gpu commit -m "..." -- <paths>`. Never `--no-verify`.
- Spec numbers, verbatim: target slice ~12 ms; moving budget ~25 ms; Pi 3B throughput ~4M triangles/s; readback ~11 ms; gates: slice overhead ≤ ~2 ms, moving ≥ 20 fps, sharp within ~1.5 s, no LVGL frame gap over ~50 ms while refining, incremental ≤ 30 ms, zero `Resetting GPU` lines in dmesg, Pi 5 fps no worse than before.
- Hardware work (the Pi 3B at 192.168.1.163, the Pi 5 at 192.168.30.128) is done by the controller, not by implementer subagents. Take `scripts/helix-claim take device:pi3b` first. Stop the device's `helixscreen` service for a run and restart it afterwards. Motion commands are never sent (the 3B commands the Voron's Moonraker).

## Review Focus

1. **Tap without a drag** (press and release, camera unchanged) after a moving frame has replaced the still image: the sharp image must come back. Pinned by the `decide_job` case "no complete image restarts" in Task 1.
2. **Print progress events arriving faster than a job finishes** (fast prints, `--sim-speed`): the running incremental job extends instead of restarting forever, so the preview never starves. Pinned by the "incremental job extends" case in Task 1.
3. **Dragging before the first still slice has ever run** (the user touches during VBO upload): the rate is still unmeasured, and the moving view must not submit the whole model in one frame. On a Pi 3B that single submission is what wedges the GPU. Pinned by `plan_moving` with rate 0 in Task 1.
4. **A new file, `release_geometry`, a GL failure or a viewport resize while a job is mid-slice:** no slice may draw from freed VBOs, and the pump must stop. Pinned by `cancel_job()` calls in Task 2 plus Task 2's hardware step (load a second file while refining).
5. **Progress moving backwards or ghost mode switching off** (a reprint or a cancel): the image must restart from scratch, not keep stale solid layers. Pinned by the "progress went backwards restarts" and "ghost off restarts" cases in Task 1.

---

### Task 0: Stage 0, measure slicing on the Pi 3B (spike branch, throwaway)

Decides whether the plan continues: on a tiled GPU, every `glFinish` between slices stores the color and depth tiles and reloads them for the next slice.

**Files (spike worktree `.worktrees/3d-perf-pi3b`, branch `spike/3d-perf-pi3b`, never merged):**
- Modify: `include/spike_3dperf.h` (add a knob)
- Modify: `src/rendering/gcode_gles_renderer.cpp` (`render_to_fbo`, `blit_to_lvgl`)

- [ ] **Step 1: Add `HELIX_SPIKE_SLICES=N` and a readback checksum.** In `include/spike_3dperf.h`, read `HELIX_SPIKE_SLICES` once, next to the existing knobs, defaulting to 1:

```cpp
inline int slices() {
    static const int v = [] {
        const char* s = std::getenv("HELIX_SPIKE_SLICES");
        int n = s ? std::atoi(s) : 1;
        return n < 1 ? 1 : n;
    }();
    return v;
}
```

In `render_to_fbo`, replace the single all-layers solid draw (`draw_layers(*active_vbos, draw_start, draw_end, 1.0f, 1.0f);` in the non-ghost branch) with N contiguous chunks, each followed by `glFinish()` and a timing sample:

```cpp
const int n = spike3d::slices();
const int total = draw_end - draw_start + 1;
for (int i = 0; i < n; ++i) {
    int a = draw_start + total * i / n;
    int b = draw_start + total * (i + 1) / n - 1;
    auto s0 = std::chrono::steady_clock::now();
    if (a <= b) draw_layers(*active_vbos, a, b, 1.0f, 1.0f);
    glFinish();
    float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - s0).count();
    spdlog::info("[3DPERF-SLICE] i={} layers={}-{} ms={:.2f}", i, a, b, ms);
}
```

In `blit_to_lvgl`, right after `glReadPixels`, log an FNV-1a checksum of `readback_buf_` when `HELIX_SPIKE_PERF` is set:

```cpp
uint64_t h = 1469598103934665603ull;
for (uint8_t byte : readback_buf_) { h ^= byte; h *= 1099511628211ull; }
spdlog::info("[3DPERF-CRC] {:016x}", h);
```

Check how `HELIX_SPIKE_AUTOSPIN` is enabled in `include/spike_3dperf.h`. If a value of 0 turns the driver off, change the enable test to "the variable is set", so `HELIX_SPIKE_AUTOSPIN=0` re-renders continuously with the camera still, which is what the CRC comparison needs.

- [ ] **Step 2: Build for the Pi.** `make -C .worktrees/3d-perf-pi3b pi-docker > /tmp/spike-slices.log 2>&1; echo exit=$?`. Expected: `exit=0`, `build/pi/bin/helix-screen` updated.

- [ ] **Step 3: Controller runs it on the Pi 3B.** Deploy as in `.superpowers/sdd/3d-weak-gpu/run.sh` (dir `/home/pi/helix-spike`). With the camera held still (`HELIX_SPIKE_AUTOSPIN=0`, 20 frames) and `HELIX_SPIKE_TUBE_SIDES=4`, run on 3DBenchy.gcode with `HELIX_SPIKE_SLICES=1`, then 8, then 32. Record the median `draw` for N=1, the sum of the `[3DPERF-SLICE]` ms for N=8 and N=32, and the `[3DPERF-CRC]` values.

- [ ] **Step 4: Gate.** Overhead per slice = (sum at N − draw at N=1) / N. Pass if it is ≤ 2 ms at N=8 and N=32, **and** all three CRCs match (depth survived every flush). Record the numbers in the ledger. If the gate fails, stop and bring the numbers to Preston before Task 2. Candidates then: fewer, bigger slices, or slices only on GPUs whose measured rate needs them.

---

### Task 1: The pure scheduling policy

**Files:**
- Create: `include/gcode_render_schedule.h`
- Create: `tests/unit/test_gcode_render_schedule.cpp`

**Interfaces:**
- Produces (used by Tasks 2 to 4), all in `namespace helix::gcode::render_schedule`:
  - `constexpr float kSliceTargetMs = 12.0f; constexpr float kMovingBudgetMs = 25.0f; constexpr float kSeedRateTrisPerMs = 4000.0f; constexpr size_t kMinSliceTriangles = 5000; constexpr float kRateSmoothing = 0.3f;`
  - `float effective_rate(float rate)`
  - `float update_rate(float prev_rate, size_t triangles, float elapsed_ms)`
  - `size_t slice_quota(float rate)`
  - `struct MovingPlan { int stride = 1; bool half_resolution = false; }; MovingPlan plan_moving(size_t total_triangles, float rate)`
  - `enum class JobAction { None, Restart, Extend, Incremental };`
  - `struct JobInputs { bool scene_changed; bool have_complete_image; bool job_running; bool job_incremental; bool selection_active; int job_progress; int new_progress; }; JobAction decide_job(const JobInputs&)`

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_gcode_render_schedule.cpp`:

```cpp
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
    REQUIRE(decide_job(idle_complete(10)) == JobAction::None);
}

TEST_CASE("a running job is left alone when nothing changed", "[gcode][render_schedule]") {
    JobInputs in = idle_complete(10);
    in.have_complete_image = false;
    in.job_running = true;
    REQUIRE(decide_job(in) == JobAction::None);
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
```

- [ ] **Step 2: Run to confirm it fails.** `make -C .worktrees/3d-weak-gpu t F='[render_schedule]'`. Expected: compile error, `gcode_render_schedule.h` not found.

- [ ] **Step 3: Write the header** `include/gcode_render_schedule.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace helix::gcode::render_schedule {

/// Wall time one still-frame slice aims for. Leaves an LVGL frame room for the rest of the UI.
constexpr float kSliceTargetMs = 12.0f;
/// Draw time a moving (finger-down) frame may spend.
constexpr float kMovingBudgetMs = 25.0f;
/// Throughput assumed before any slice has been measured. A Pi 3B's VideoCore IV draws about
/// 4M triangles/s, so the first slice and the first moving frame stay short on the weakest GPU.
constexpr float kSeedRateTrisPerMs = 4000.0f;
/// Floor on a slice so per-slice overhead never dominates.
constexpr size_t kMinSliceTriangles = 5000;
/// Weight of the newest measurement in the smoothed rate.
constexpr float kRateSmoothing = 0.3f;

/// The rate to plan with: the measured one, or the seed while nothing has been measured.
inline float effective_rate(float rate) {
    return rate > 0.0f ? rate : kSeedRateTrisPerMs;
}

/// Fold one measured submission into the smoothed triangles-per-millisecond rate.
/// A sample with no triangles or no elapsed time carries no information and is ignored.
inline float update_rate(float prev_rate, size_t triangles, float elapsed_ms) {
    if (triangles == 0 || elapsed_ms <= 0.0f) {
        return prev_rate;
    }
    const float sample = static_cast<float>(triangles) / elapsed_ms;
    if (prev_rate <= 0.0f) {
        return sample;
    }
    return prev_rate + kRateSmoothing * (sample - prev_rate);
}

/// Triangles the next still-frame slice may submit.
inline size_t slice_quota(float rate) {
    const auto quota = static_cast<size_t>(effective_rate(rate) * kSliceTargetMs);
    return quota < kMinSliceTriangles ? kMinSliceTriangles : quota;
}

/// How a moving frame is drawn: every `stride`-th layer, optionally at half resolution.
struct MovingPlan {
    int stride = 1;
    bool half_resolution = false;
};

/// Full detail when the whole model fits the moving budget at the planning rate; otherwise
/// the smallest layer stride that fits, drawn at half resolution.
inline MovingPlan plan_moving(size_t total_triangles, float rate) {
    auto budget = static_cast<size_t>(effective_rate(rate) * kMovingBudgetMs);
    if (budget == 0) {
        budget = 1;
    }
    if (total_triangles <= budget) {
        return {};
    }
    return {static_cast<int>((total_triangles + budget - 1) / budget), true};
}

/// What the renderer does with its time-sliced job after a state change.
enum class JobAction {
    None,        ///< keep going (or keep showing the cached image)
    Restart,     ///< start a full still job from layer 0
    Extend,      ///< the running incremental job grows to the new progress layer
    Incremental, ///< draw only the newly finished layers onto the retained buffers
};

struct JobInputs {
    bool scene_changed = false;       ///< camera, colors, range, viewport or file differ from the job's
    bool have_complete_image = false; ///< the last finished image matches the scene and job_progress
    bool job_running = false;
    bool job_incremental = false;
    bool selection_active = false; ///< highlighted objects need the full selection passes
    int job_progress = -1;         ///< progress layer of the running or last finished job; -1 = no ghost
    int new_progress = -1;         ///< progress layer now requested; -1 = no ghost
};

inline JobAction decide_job(const JobInputs& in) {
    if (in.scene_changed) {
        return JobAction::Restart;
    }
    if (in.new_progress == in.job_progress) {
        return (in.job_running || in.have_complete_image) ? JobAction::None : JobAction::Restart;
    }
    const bool advanced = in.job_progress >= 0 && in.new_progress > in.job_progress;
    if (!advanced || in.selection_active) {
        return JobAction::Restart;
    }
    if (in.job_running) {
        return in.job_incremental ? JobAction::Extend : JobAction::Restart;
    }
    return in.have_complete_image ? JobAction::Incremental : JobAction::Restart;
}

} // namespace helix::gcode::render_schedule
```

- [ ] **Step 4: Run the tests.** `make -C .worktrees/3d-weak-gpu t F='[render_schedule]'`. Expected: all 16 cases pass.

- [ ] **Step 5: Commit.** `git -C .worktrees/3d-weak-gpu commit -m "feat(gcode): pure scheduling policy for a time-sliced 3D preview" -- include/gcode_render_schedule.h tests/unit/test_gcode_render_schedule.cpp`. (Both files are new: `git -C .worktrees/3d-weak-gpu add` them first. The worktree index is private, so the shared-tree add race does not apply.)

---

### Task 2: Still frames drawn in slices

**Files:**
- Modify: `include/gcode_gles_renderer.h` (private job state, split render helpers, `is_refining()`, `CachedRenderState::same_scene`)
- Modify: `src/rendering/gcode_gles_renderer.cpp` (`render`, `render_to_fbo` split, `draw_layers`, `set_content_offset_y`, `set_print_progress_layer`, `set_interaction_mode`, `release_geometry`, `set_prebuilt_geometry`)
- Modify: `src/ui/ui_gcode_viewer.cpp` (the 3D branch of `gcode_viewer_draw_cb`: keep pumping while refining)

**Interfaces:**
- Consumes: `render_schedule::slice_quota`, `update_rate`, `decide_job`, `JobInputs`, `JobAction` (Task 1).
- Produces: `bool GCodeGLESRenderer::is_refining() const`; the private `float gpu_rate_tris_per_ms_`, `size_t uploaded_triangles_` and `bool have_complete_image_` that Tasks 3 and 4 read; `int draw_layers(..., int stride, size_t max_triangles)` returning the next undrawn layer; `bool setup_frame(const GCodeCamera&, float scale, bool clear, glm::mat4& mvp, glm::mat4& mvp_dequant)`; `void pass_ranges(int& draw_start, int& draw_end, int& solid_end, int& ghost_start, bool& ghosting) const`; `void start_job(bool incremental, const CachedRenderState&)`; `void cancel_job()`.

- [ ] **Step 1: Stop dirtying frames that did not change.** `set_content_offset_y` runs on every draw callback (`gcode_viewer_refresh_content_offset`) and dirties unconditionally, so today every redraw re-renders. Change it to:

```cpp
void GCodeGLESRenderer::set_content_offset_y(float offset_percent) {
    const float clamped = std::clamp(offset_percent, -1.0f, 1.0f);
    if (std::abs(clamped - content_offset_y_percent_) < 1e-4f) {
        return;
    }
    content_offset_y_percent_ = clamped;
    frame_dirty_ = true;
}
```

`set_print_progress_layer` keeps storing `progress_layer_` but no longer sets `frame_dirty_`, because progress goes through `decide_job`. `set_interaction_mode` no longer sets `frame_dirty_`; moving is handled explicitly in `render()` in Task 3. Between Tasks 2 and 3, a drag on a slow GPU restarts the still job every frame and shows the last finished image until release. That is expected and short-lived.

- [ ] **Step 2: Add the job state** to the private section of `include/gcode_gles_renderer.h`, after `CachedRenderState cached_state_;`:

```cpp
    // ====== Time-sliced refinement ======

    enum class JobPhase { Solid, Ghost, Overlays, Done };
    struct RefineJob {
        bool active = false;
        bool incremental = false;
        bool first_slice = true;
        JobPhase phase = JobPhase::Done;
        int next_layer = 0;
        int solid_start = 0;
        int solid_end = -1; ///< inclusive
        int ghost_start = 0;
        int ghost_end = -1; ///< inclusive
        int progress_layer = -1;
        int solid_end_limit = -1; ///< an Extend never runs past the job's last drawn layer
        int slices = 0;
        float max_slice_ms = 0.0f;
        std::chrono::steady_clock::time_point started;
    };
    RefineJob job_;
    CachedRenderState job_scene_;     ///< scene the running or last finished job was drawn for
    bool have_complete_image_ = false; ///< draw_buf_ holds a finished still image of job_scene_
    float gpu_rate_tris_per_ms_ = 0.0f; ///< smoothed measured throughput; 0 = unmeasured
    size_t uploaded_triangles_ = 0;     ///< sum of layer_vbos_ triangles

    CachedRenderState snapshot_state(const GCodeCamera& camera) const;
    bool setup_frame(const GCodeCamera& camera, float scale, bool clear, glm::mat4& mvp,
                     glm::mat4& mvp_dequant);
    void pass_ranges(int& draw_start, int& draw_end, int& solid_end, int& ghost_start,
                     bool& ghosting) const;
    void start_job(bool incremental, const CachedRenderState& scene);
    bool run_slice(const ParsedGCodeFile& gcode, const GCodeCamera& camera);
    void cancel_job();
```

and in the public section:

```cpp
    /// True while a still or incremental job still has slices to draw (caller keeps invalidating).
    bool is_refining() const {
        return job_.active && !render_failed();
    }
```

Add to `CachedRenderState`: `bool same_scene(const CachedRenderState& o) const;`, the existing `operator==` minus the `progress_layer` term. Implement `operator==` as `same_scene(o) && progress_layer == o.progress_layer`, so the rule lives in one place.

- [ ] **Step 3: Make `draw_layers` resumable.** New signature:

```cpp
/// Draws layers [layer_start, layer_end] at `stride`, stopping once `max_triangles` have been
/// submitted. Returns the next layer that was not drawn (layer_end + 1 when finished).
int draw_layers(const std::vector<LayerVBO>& vbos, int layer_start, int layer_end,
                float color_scale, float alpha, int stride = 1,
                size_t max_triangles = std::numeric_limits<size_t>::max());
```

Change the loop header to `for (layer = layer_start; layer <= layer_end; layer += stride)`. After each `glDrawArrays`, add `submitted += lv.vertex_count / 3;` and `if (submitted >= max_triangles) { layer += stride; break; }`. Return `layer`, keeping the existing attribute enable and disable and the single `glGetError` per call. Existing call sites keep their behaviour through the defaults.

- [ ] **Step 4: Split `render_to_fbo`.** Move its prologue into `setup_frame`: size clamp (`render_w = max(1, int(viewport_width_ * scale))`, likewise for height), `create_fbo`, bind, viewport, clear only when `clear` is true, depth test, color mask, program, uniforms. It fills `mvp` and `mvp_dequant` and returns false when `create_fbo` fails or there are no VBOs. `render_to_fbo` becomes `setup_frame(camera, 1.0f, true, mvp, mvp_dequant)` followed by its existing passes and overlays, unchanged. After this step, `make -C .worktrees/3d-weak-gpu -j` must build and the preview must look the same (Step 9 checks it).

- [ ] **Step 5: Implement the job.** First extract the pass ranges `render_to_fbo` computes today into one private helper, and use it from `render_to_fbo` too, so every path agrees (Task 3's `render_moving` also calls it):

```cpp
/// Layer ranges for the solid and ghost passes. ghosting is true when 0 <= progress_layer_ < max_layer.
void GCodeGLESRenderer::pass_ranges(int& draw_start, int& draw_end, int& solid_end,
                                    int& ghost_start, bool& ghosting) const {
    const int max_layer = static_cast<int>(layer_vbos_.size()) - 1;
    draw_start = (layer_start_ >= 0) ? layer_start_ : 0;
    draw_end = (layer_end_ >= 0) ? std::min(layer_end_, max_layer) : max_layer;
    ghosting = progress_layer_ >= 0 && progress_layer_ < max_layer;
    solid_end = ghosting ? std::min(progress_layer_, draw_end) : draw_end;
    ghost_start = ghosting ? std::max(progress_layer_ + 1, draw_start) : draw_end + 1;
}
```

Declare it next to `start_job` in the header. `start_job` reads the previous job's progress before resetting, because an incremental job starts right after it and must not clear the retained buffers:

```cpp
void GCodeGLESRenderer::start_job(bool incremental, const CachedRenderState& scene) {
    const int previous_progress = job_.progress_layer;
    int draw_start, draw_end, solid_end, ghost_start;
    bool ghosting;
    pass_ranges(draw_start, draw_end, solid_end, ghost_start, ghosting);
    job_ = RefineJob{};
    job_.active = true;
    job_.incremental = incremental;
    job_.phase = JobPhase::Solid;
    job_.solid_end_limit = draw_end;
    job_.progress_layer = progress_layer_;
    job_.started = std::chrono::steady_clock::now();
    if (incremental) {
        job_.first_slice = false;
        job_.solid_start = std::max(previous_progress + 1, draw_start);
        job_.solid_end = solid_end;
        job_.ghost_start = 0;
        job_.ghost_end = -1;
    } else {
        job_.solid_start = draw_start;
        job_.solid_end = ghosting ? solid_end : draw_end;
        job_.ghost_start = ghost_start;
        job_.ghost_end = ghosting ? draw_end : -1;
        have_complete_image_ = false;
    }
    job_.next_layer = job_.solid_start;
    job_scene_ = scene;
}
```


`run_slice`:

```cpp
bool GCodeGLESRenderer::run_slice(const ParsedGCodeFile& gcode, const GCodeCamera& camera) {
    glm::mat4 mvp, mvp_dequant;
    if (!setup_frame(camera, 1.0f, job_.first_slice, mvp, mvp_dequant)) {
        cancel_job();
        return false;
    }
    job_.first_slice = false;
    const size_t quota = render_schedule::slice_quota(gpu_rate_tris_per_ms_);
    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();
    while (job_.phase != JobPhase::Done && triangles_rendered_ - before < quota) {
        const size_t left = quota - (triangles_rendered_ - before);
        if (job_.phase == JobPhase::Solid) {
            if (job_.next_layer > job_.solid_end) {
                job_.phase = job_.ghost_start <= job_.ghost_end ? JobPhase::Ghost : JobPhase::Overlays;
                job_.next_layer = job_.ghost_start;
                continue;
            }
            job_.next_layer = draw_layers(layer_vbos_, job_.next_layer, job_.solid_end, 1.0f, 1.0f,
                                          1, left);
        } else if (job_.phase == JobPhase::Ghost) {
            if (job_.next_layer > job_.ghost_end) {
                job_.phase = JobPhase::Overlays;
                continue;
            }
            constexpr float GHOST_LIGHTEN_SCALE = 4.0f;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            job_.next_layer = draw_layers(layer_vbos_, job_.next_layer, job_.ghost_end,
                                          GHOST_LIGHTEN_SCALE, ghost_opacity_ / 255.0f, 1, left);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        } else { // Overlays: the same selection tag and brackets render_to_fbo draws
            glUseProgram(0);
            // The tag covers exactly the solid layers, as in render_to_fbo (tag_end is the
            // progress layer while ghosting, the last drawn layer otherwise).
            if (!job_.incremental) {
                render_selection_tag(gcode, mvp_dequant, job_.solid_start, job_.solid_end);
            }
            render_brackets_3d(gcode, mvp);
            job_.phase = JobPhase::Done;
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    gpu_rate_tris_per_ms_ =
        render_schedule::update_rate(gpu_rate_tris_per_ms_, triangles_rendered_ - before, ms);
    job_.slices++;
    job_.max_slice_ms = std::max(job_.max_slice_ms, ms);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (job_.phase != JobPhase::Done) {
        return false;
    }
    job_.active = false;
    spdlog::debug("[GCode GLES] Refine done: {} slices, max slice {:.1f}ms, {:.0f}ms wall, "
                  "rate {:.0f} tris/ms{}",
                  job_.slices, job_.max_slice_ms,
                  std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() -
                                                           job_.started).count(),
                  gpu_rate_tris_per_ms_, job_.incremental ? " (incremental)" : "");
    return true;
}
```

`cancel_job()` sets `job_ = RefineJob{}` and `have_complete_image_ = false`. Call it in `release_geometry`, in `set_prebuilt_geometry`, and wherever `gl_render_failed_` becomes true. At the end of the upload in `render()` (where `geometry_uploaded_ = true`), set `uploaded_triangles_` to the sum of `vbo.vertex_count / 3` over `layer_vbos_`.

- [ ] **Step 6: Rewrite the tail of `render()`.** Replace everything from `// Build current render state for frame-skip check` to the end of the function:

```cpp
    const CachedRenderState current_state = snapshot_state(camera);
    render_schedule::JobInputs in;
    in.scene_changed = frame_dirty_ || !current_state.same_scene(job_scene_);
    in.have_complete_image = have_complete_image_ && draw_buf_;
    in.job_running = job_.active;
    in.job_incremental = job_.incremental;
    in.selection_active = selection_.any_highlighted();
    in.job_progress = job_.progress_layer;
    in.new_progress = progress_layer_;
    switch (render_schedule::decide_job(in)) {
    case render_schedule::JobAction::None:
        break;
    case render_schedule::JobAction::Restart:
        start_job(false, current_state);
        break;
    case render_schedule::JobAction::Incremental:
        start_job(true, current_state);
        break;
    case render_schedule::JobAction::Extend:
        job_.solid_end = std::min(progress_layer_, job_.solid_end_limit);
        job_.progress_layer = progress_layer_;
        job_.phase = JobPhase::Solid;
        break;
    }
    frame_dirty_ = false;
    cached_state_ = current_state;

    if (!job_.active) {
        draw_cached_to_lvgl(layer, widget_coords);
        return;
    }
    arm_gpu_guard();
    const bool done = run_slice(gcode, camera);
    if (done) {
        blit_to_lvgl(layer, widget_coords);
        have_complete_image_ = true;
    } else {
        draw_cached_to_lvgl(layer, widget_coords);
    }
    clear_gpu_guard();
```

`snapshot_state` is the existing block of `current_state.* = ...` assignments, moved into a helper. Add `content_offset_y_percent_`, `viewport_width_` and `viewport_height_` to `CachedRenderState` and to `same_scene`, so a scene key built from state alone is complete.

- [ ] **Step 7: Keep the viewer pumping while refining.** In `src/ui/ui_gcode_viewer.cpp`, 3D branch of `gcode_viewer_draw_cb`, change

```cpp
        if (st->renderer_->is_uploading() || st->needs_3d_refresh_) {
```
to
```cpp
        if (st->renderer_->is_uploading() || st->renderer_->is_refining() || st->needs_3d_refresh_) {
```

The body already `async_call`s an invalidate, which is the pump that drives upload chunks today.

- [ ] **Step 8: Build and run the renderer's existing tests.** `make -C .worktrees/3d-weak-gpu t F='[gcode]'`. Expected: all pass (the `[1555]` case still reports `render_failed()` with no GL). `make -C .worktrees/3d-weak-gpu -j` builds the app.

- [ ] **Step 9: Desktop visual check (implementer).** Launch with the pinned-socket recipe from CLAUDE.md, `HELIX_GCODE_MODE=3D`, `--test -vv --select-file 3DBenchy.gcode`. Confirm `[GCode GLES] Refine done: 1 slices` (a desktop GPU finishes in one slice), then take `ctl screenshot` before and after the change (checkout of the Task 1 commit) and confirm they are pixel-identical in `ctl geom detail_gcode_viewer`'s rectangle. Kill only your own instance, by PID.

- [ ] **Step 10: Commit.** `git -C .worktrees/3d-weak-gpu commit -m "feat(gcode): draw still 3D frames in time-sliced passes so a slow GPU never blocks the UI" -- include/gcode_gles_renderer.h src/rendering/gcode_gles_renderer.cpp src/ui/ui_gcode_viewer.cpp`

- [ ] **Step 11: Hardware gate (controller), Pi 3B.** Push the branch. `make pi-docker` in the worktree. Deploy as `.superpowers/sdd/3d-weak-gpu/vc4run.sh` does (dir `/home/pi/helix-vc4`, stock Auto mode). Run it on 3DBenchy.gcode with `-vv`, then read the `Refine done` lines. Gate: `max slice` ≤ 25 ms, wall ≤ 1.5 s, zero added `Resetting GPU`. Also load exclude_object_test.gcode from print-select while Benchy is refining (`ctl navigate print-select`, click its card): there must be no crash and no GL error, and the new file must refine. Record the numbers in the ledger.

---

### Task 3: The moving view

**Files:**
- Modify: `include/gcode_gles_renderer.h`, `src/rendering/gcode_gles_renderer.cpp` (`render`, new `render_moving`)

**Interfaces:**
- Consumes: `render_schedule::plan_moving`, `update_rate` (Task 1); `setup_frame`, `draw_layers(..., stride, max)`, `gpu_rate_tris_per_ms_`, `uploaded_triangles_`, `cancel_job`, `have_complete_image_` (Task 2).
- Produces: `void render_moving(lv_layer_t*, const ParsedGCodeFile&, const GCodeCamera&, const lv_area_t*)`.

- [ ] **Step 1: Implement `render_moving`.** It draws one full frame with the plan's stride and scale, measures it, and blits. `blit_to_lvgl` already upscales when the FBO is smaller than the widget.

```cpp
void GCodeGLESRenderer::render_moving(lv_layer_t* layer, const ParsedGCodeFile& gcode,
                                      const GCodeCamera& camera, const lv_area_t* widget_coords) {
    cancel_job();
    const render_schedule::MovingPlan plan =
        render_schedule::plan_moving(uploaded_triangles_, gpu_rate_tris_per_ms_);
    glm::mat4 mvp, mvp_dequant;
    if (!setup_frame(camera, plan.half_resolution ? 0.5f : 1.0f, true, mvp, mvp_dequant)) {
        return;
    }
    const int max_layer = static_cast<int>(layer_vbos_.size()) - 1;
    const int draw_start = (layer_start_ >= 0) ? layer_start_ : 0;
    const int draw_end = (layer_end_ >= 0) ? std::min(layer_end_, max_layer) : max_layer;
    const size_t before = triangles_rendered_;
    const auto t0 = std::chrono::steady_clock::now();
    if (progress_layer_ >= 0 && progress_layer_ < max_layer) {
        const int solid_end = std::min(progress_layer_, draw_end);
        if (draw_start <= solid_end) {
            draw_layers(layer_vbos_, draw_start, solid_end, 1.0f, 1.0f, plan.stride);
        }
        const int ghost_start = std::max(progress_layer_ + 1, draw_start);
        if (ghost_start <= draw_end) {
            constexpr float GHOST_LIGHTEN_SCALE = 4.0f;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            draw_layers(layer_vbos_, ghost_start, draw_end, GHOST_LIGHTEN_SCALE,
                        ghost_opacity_ / 255.0f, plan.stride);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    } else {
        draw_layers(layer_vbos_, draw_start, draw_end, 1.0f, 1.0f, plan.stride);
    }
    glUseProgram(0);
    render_brackets_3d(gcode, mvp);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFinish();
    const float ms =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    gpu_rate_tris_per_ms_ =
        render_schedule::update_rate(gpu_rate_tris_per_ms_, triangles_rendered_ - before, ms);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    blit_to_lvgl(layer, widget_coords);
    spdlog::trace("[GCode GLES] Moving frame: stride {}, {} res, {:.1f}ms", plan.stride,
                  plan.half_resolution ? "half" : "full", ms);
}
```

Replace the three range lines and the ghost condition at the top of this function with a `pass_ranges(...)` call (Task 2); the body above spells them out only so the logic is visible.

- [ ] **Step 2: Route finger-down frames.** In `render()`, directly before `const CachedRenderState current_state = snapshot_state(camera);`:

```cpp
    if (interaction_mode_) {
        arm_gpu_guard();
        render_moving(layer, gcode, camera, widget_coords);
        clear_gpu_guard();
        return;
    }
```

`cancel_job()` inside `render_moving` clears `have_complete_image_`. On release, `decide_job` therefore sees no complete image and restarts a still job, even for a tap that did not move the camera (Review Focus 1).

- [ ] **Step 3: Build and run the tests.** `make -C .worktrees/3d-weak-gpu t F='[gcode]'`, then `make -C .worktrees/3d-weak-gpu -j`. Expected: green, and the app builds.

- [ ] **Step 4: Desktop check (implementer).** On a desktop GPU `plan_moving` returns stride 1 and full resolution, so a drag looks exactly as before. Drive `ctl press` / `move` / `release` over `detail_gcode_viewer` and confirm with `-vvv` that the trace shows `Moving frame: stride 1, full res`, then `Refine done` after the release.

- [ ] **Step 5: Commit.** `git -C .worktrees/3d-weak-gpu commit -m "feat(gcode): draw a strided half-resolution view while a finger is down on a slow GPU" -- include/gcode_gles_renderer.h src/rendering/gcode_gles_renderer.cpp`

- [ ] **Step 6: Hardware gate (controller), Pi 3B, plus Preston's eye.** Deploy and run `vc4run.sh 3DBenchy.gcode`. Gate: the viewer's `[GCode Viewer] 3D mode: <ms>` render time during the drag is ≤ 50 ms (20 fps capacity), zero added GPU resets. Then leave the build running on the 3B's panel and ask Preston to rotate, pan and pinch Benchy by hand and judge two things: whether the strided view reads as the model rather than a broken slinky, and whether the release sharpens quickly enough. If it reads as a slinky, try the fallback before changing the design: halve the moving budget's stride by allowing 40 ms (`kMovingBudgetMs`), and re-ask.

---

### Task 4: Incremental layers during a print, proven

**Files:**
- Modify only if Step 1 exposes a problem: `src/rendering/gcode_gles_renderer.cpp`, `include/gcode_render_schedule.h`, `tests/unit/test_gcode_render_schedule.cpp`

**Interfaces:**
- Consumes: `decide_job`'s `Incremental` and `Extend` (Task 1); `start_job`, `run_slice`, `RefineJob` (Task 2).

- [ ] **Step 1: Desktop mock print.** Task 2 already routes `Incremental` and `Extend`. Prove it runs: launch the pinned-socket recipe with `HELIX_MOCK_AUTO_PRINT=1 --sim-speed 6 -vv --select-file 3DBenchy.gcode`, open print status, and confirm that after the first `Refine done` every later one ends in `(incremental)` while the camera is still. Rotate once with `ctl press`/`move`/`release`: the next job must be a full one, and incremental jobs must resume after it. If a rule is wrong, add the failing case to `tests/unit/test_gcode_render_schedule.cpp` first, then fix `decide_job` or its inputs.

- [ ] **Step 2: Build, run `[gcode]`, commit any fixes.** `make -C .worktrees/3d-weak-gpu t F='[gcode],[render_schedule]'`; if Step 1 needed changes, `git -C .worktrees/3d-weak-gpu commit -m "fix(gcode): <what the mock print exposed>" -- <paths>`. No changes means no commit.

- [ ] **Step 3: Hardware gate (controller), Pi 3B.** Run `HELIX_MOCK_AUTO_PRINT=1 ./bin/helix-screen --test --sim-speed 6 -vv` on the 3B (display-only mock; nothing moves). Wait for print status to show the 3D preview (Benchy via `--select-file`), then read the `Refine done ... (incremental)` lines. Gate: each incremental job's wall time ≤ 30 ms, and no full restarts while the camera sits still (count non-incremental `Refine done` lines after the first). Rerun at `--sim-speed 50` to confirm fast layer events extend rather than starve (Review Focus 2): the preview must keep advancing, never stuck on an old layer.

---

### Task 5: Re-tune the vc4 triangle cap

Slicing removes the long submissions behind the GPU wedge. The cap now only bounds memory and time-to-sharp, so it may rise.

**Files:**
- Modify: `include/gcode_gl_fallback.h` (`VC4_TRIANGLE_BUDGET` and its doc comment)
- Modify: `tests/unit/test_gcode_gl_fallback.cpp`, `tests/unit/test_geometry_budget.cpp` (expected values)
- Modify: `docs/devel/GCODE_VIEWER_CONFIG.md` (the cap paragraph)

- [ ] **Step 1: Measure (controller).** Build three Pi binaries with `VC4_TRIANGLE_BUDGET` at 1'000'000, 2'000'000 and 3'000'000 (local edits, not committed). For each, run `vc4run.sh` on exclude_object_test.gcode and eiffel_final_PLA_2h42m.gcode, and during the run sample `grep CmaFree /proc/meminfo` on the Pi. Record the tier, the triangles built, the `Refine done` wall time, the moving `3D mode` ms, the minimum CmaFree, and the GPU resets.

- [ ] **Step 2: Pick the largest cap that passes.** Pass means: refine wall ≤ 1.5 s, moving ≤ 50 ms, minimum CmaFree ≥ 32 MB, zero resets. If only 1M passes, stop here: no change, record the numbers in the ledger.

- [ ] **Step 3: Apply it.** Set `VC4_TRIANGLE_BUDGET` to the chosen value. Update its comment to state what it bounds now, in present tense: "memory and time until the still image is sharp; slicing keeps each GPU submission short". Update the two tests' expected tiers by recomputing from `TRIS_PER_SEG_*`, and the doc paragraph.

- [ ] **Step 4: Run, commit.** `make -C .worktrees/3d-weak-gpu t F='[budget],[gl_fallback]'`, then, with N the value Step 2 chose: `git -C .worktrees/3d-weak-gpu commit -m "tune(gcode): raise the vc4 triangle cap to N now that slicing bounds each GPU submission" -- include/gcode_gl_fallback.h tests/unit/test_gcode_gl_fallback.cpp tests/unit/test_geometry_budget.cpp docs/devel/GCODE_VIEWER_CONFIG.md`

---

### Task 6: Docs, Pi 5 regression, full gates

**Files:**
- Modify: `docs/devel/architecture/16-gcode-pipeline.md` (the 3D render path: moving, still slicing, incremental)
- Modify: `docs/devel/GPU_ACCELERATION.md` (Pi 3B measurements and the learned-rate budget)
- Delete: `docs/devel/plans/2026-09-27-3d-preview-weak-gpu-design.md`, `docs/devel/plans/2026-09-27-3d-preview-weak-gpu.md` (the ship convention in `docs/CLAUDE.md`)

- [ ] **Step 1: Docs.** In `16-gcode-pipeline.md`, describe the three render qualities, the slice loop, `decide_job`, and the moving plan. Cite code as `path#symbol` (for example `src/rendering/gcode_gles_renderer.cpp#GCodeGLESRenderer::run_slice`), never with line numbers. In `GPU_ACCELERATION.md`, add the Pi 3B measurement table from the spec and a sentence on how the renderer learns its rate. Run `make check-doc-anchors` (advisory) and fix anything it reports on these files.

- [ ] **Step 2: Pi 5 regression (controller).** Build `make pi-docker`. On the Pi 5 (`ssh -o HostKeyAlias=192.168.1.113 pbrown@192.168.30.128`), run the same Benchy load and drag as on the 3B, with main's build and with this branch. Gate: the branch's moving `3D mode` ms and time to the first sharp frame are no worse than main's, and the Pi 5 shows `Refine done: 1 slices` or close to it.

- [ ] **Step 3: Full gates.** `make -C .worktrees/3d-weak-gpu full-test-run` (exit 0, read the summary counts). Then push and run `scripts/zeus-run.sh mutate --base <fork-point SHA> --tests '[render_schedule],[gcode],[budget],[gl_fallback]'` from the worktree. Every survivor gets a ruling in the ledger.

- [ ] **Step 4: Commit the docs and delete the plan scaffolding.** Delete both plan files with plain `rm` (never `git rm`), then `git -C .worktrees/3d-weak-gpu commit -m "docs(gcode): document the time-sliced 3D preview; remove the shipped plan" -- docs/devel/architecture/16-gcode-pipeline.md docs/devel/GPU_ACCELERATION.md docs/devel/plans/2026-09-27-3d-preview-weak-gpu-design.md docs/devel/plans/2026-09-27-3d-preview-weak-gpu.md`
