# 3D G-code preview on weak GPUs: design

Status: design approved in conversation 2026-09-27, spec awaiting review.
Implementation plan: to be written from this spec.

## Goal

Make the GLES 3D G-code preview usable on Raspberry Pi 3B-class hardware (VideoCore IV,
Mesa `vc4`, GLES 2.0) without making it worse anywhere else. "Usable" means all three:

1. Rotate, pan and pinch track the fingers smoothly. A coarser image while a finger is down is
   fine if it sharpens after release.
2. The live layer-progress view during a print keeps working.
3. Neither of those starves the rest of the UI: touch stays responsive while the preview works.

## What was measured (Pi 3B, 3DBenchy, 368x390 viewer, 2026-09-27)

Throwaway instrumentation on branch `spike/3d-perf-pi3b` split each frame with `glFinish`:

| Drawn triangles | Draw | Readback + repack | Frame | fps |
|---|---|---|---|---|
| 115k (4 tube sides, every 8th layer) | 27.6 ms | 11 ms | 39 ms | 22.5 |
| 115k at half-res FBO | 21.0 ms | 5 ms | 26 ms | 29.6 |
| 271k (8 sides, every 8th layer) | 53 ms | 11 ms | 65 ms | 13.8 |
| 923k (4 sides, every layer) | 233 ms | 11 ms | 245 ms | 3.9 |
| 4.66M (stock tier 1, 16 sides, 168 MB of VBOs) | 2.1 to 4.0 s | 11 to 22 ms | - | ~0.3 |

- The GPU draws about 4M triangles/s up to ~1M triangles, and roughly 3x slower at 168 MB of
  VBOs (the CMA pool is 256 MB). Drawing is nearly all of the cost; readback is a fixed ~11 ms.
- Multi-second frames trip the kernel's GPU hang check. The GPU then resets about once a second,
  even after the process exits, until a reboot.
- The 2D renderer draws the same file at 13 ms per frame.
- Share of extrusion moves that are visible features (outer and overhang walls, top and bottom
  surfaces, bridges): Benchy 41%, exclude_object_test 33%, Eiffel 84%.

The immediate hazard, stock Auto mode picking 4.66M triangles on a Pi 3B, is fixed separately
on `fix/vc4-triangle-cap`. There, `GeometryBudgetManager::select_tier` takes a GPU triangle
budget (1M on a `vc4` render node) and drops to 2D when even 4-sided tubes exceed it. This design
builds on that fix.

## Current pipeline, as it bears on this design

- `src/rendering/gcode_gles_renderer.cpp#GCodeGLESRenderer::render` runs synchronously in the
  viewer's LVGL draw callback on the UI thread. It uses a private EGL context. It renders the
  whole scene into an FBO with one `glDrawArrays` per layer VBO, then does `glReadPixels`, a CPU
  RGBA to RGB888 repack with a Y flip, and `lv_draw_image`.
- An idle camera blits the cached image (`CachedRenderState`) with no GL work.
- `set_interaction_mode` only marks the frame dirty; there is no reduced-quality path.
- Every print-progress layer change re-renders the whole scene: solid layers, then ghost layers
  at `ghost_opacity`, blended with depth writes off.

## Design

### Three render qualities

| Quality | When | What it draws |
|---|---|---|
| Moving | a finger is down (rotate, pan, pinch) | full detail if it fits the moving budget; otherwise every Nth layer VBO at a half-resolution FBO |
| Still | camera idle; after release; file, color or selection change | full geometry, time-sliced across LVGL frames into a retained FBO, one readback at the end |
| Incremental | the print advances a layer and the camera has not moved | only the newly finished layers, drawn onto the retained color and depth buffers |

Pan and pinch take the same moving path as rotate. Transforming the cached image instead was
considered and dropped: it leaves blank edges and blurs the zoom, and a second moving path is
not worth it when the proxy already runs at 22 to 30 fps.

### Learned GPU rate

The renderer does not keep a table of GPU names for speed. It measures its own slices (below)
and keeps a smoothed triangles-per-millisecond rate for the session. The `vc4` triangle budget
from the wedge fix seeds the first slice quota and still bounds geometry size; everything else
derives from the measured rate.

### Still: time-sliced refinement

- A still job holds a camera snapshot, the progress layer, the layer range, and `next_layer`.
- Each LVGL frame, the draw callback binds the retained FBO and draws layers from `next_layer`
  until it reaches a triangle quota (per-layer triangle counts come from
  `RibbonGeometry::layer_strip_ranges`). It then calls `glFinish` and records the slice time.
- The quota adapts toward a target slice time of about 12 ms.
- The screen shows the last completed image until the job finishes. On completion the job does
  one readback and repack, and the result becomes the cached image.
- No single GPU submission runs longer than one slice, so model size can no longer trip the
  kernel hang check. The triangle budget remains as a memory and time-to-sharp bound.
- These restart the job from layer 0: a camera change, a new file, a color, selection or
  layer-range change, or a ghost setting change. A finger going down switches to moving.
- On a GPU where the whole scene fits within one slice (Pi 5 class), the job finishes in a
  single frame, which is today's behaviour.

### Moving: the proxy rule

- If the learned rate says a full-detail frame fits the moving budget (~25 ms), draw full
  detail. Fast GPUs are unchanged.
- Otherwise stride = ceil(total triangles / (rate x budget)), drawn into a half-resolution FBO
  and upscaled into the existing draw buffer. The stride reuses the existing layer VBOs, so the
  proxy costs no extra GPU memory.
- On release, a still job starts from the new camera.

### Incremental: live print progress

- Trigger: the progress layer advances from k to k', the last still job completed with the
  current camera, and no still job is running. Layers (k, k'] are drawn solid, with depth writes
  on, into the retained FBO, through the same sliced loop, followed by one readback.
- Ghost layers never write depth, so a new solid layer draws over ghost pixels and hides behind
  earlier solid layers correctly. The only loss is the ghost tint where the new layer lands,
  about 2% opacity by default.
- Layer events that arrive during an incremental job extend its end layer instead of
  restarting it.
- These fall back to a full still job: progress moving backwards, a new print, or any
  still-restart condition.

### Pure decision layer

The policy lives in one small header of pure functions, tested without GL:

- the slice-quota controller: measured slice time and quota in, next quota and updated rate out
- the stride picker: total triangles, rate and budget in, stride and resolution scale out
- the job rule: for each state change (camera, progress up or down, file, color, selection,
  finger down or up), whether to restart, extend or run incrementally, or do nothing

The renderer only wires GL calls to these decisions.

### Explicitly out of scope

- A render thread. On a 2-core board LVGL's render thread measured as pure overhead
  (`LV_OS_NONE` on MIPS/K1). Slicing gives the responsiveness without it.
- Visible-feature culling (skipping infill and inner walls on finished views): worth 1.2x to 3x
  depending on the model. It is a later stage with its own measurement.
- Indexed drawing: unmeasured.
- Any change to the 2D renderer or the bed mesh renderer.
- The crash guard clearing after the first good frame. Slicing removes the long submissions
  that made that gap dangerous.

## Measurement and acceptance

Each stage re-applies the spike instrumentation from `spike/3d-perf-pi3b`; it is never merged.
A stage does not build on the previous one until the previous one meets its gate on the Pi 3B:

| Stage | Gate on the Pi 3B |
|---|---|
| 0. Slice spike | each `glFinish` tile store and reload between slices costs at most ~2 ms; depth survives across slices |
| 1. Moving | Benchy rotates at 20 fps or better |
| 2. Still | full detail within ~1.5 s after release; no LVGL frame gap over ~50 ms while refining |
| 3. Incremental | a layer event costs 30 ms or less |
| every stage | zero `Resetting GPU` lines in dmesg; Pi 5 fps no worse than before |

Preston checks by eye on the Pi 3B that the strided moving view reads as the model rather than
a broken slinky. If a stage misses its gate, the numbers decide: tune it, or drop it, as the
starfield worker thread was dropped.

## Risks

- VideoCore IV is a tiled GPU. Every flush between slices stores the color and depth tiles and
  reloads them for the next slice. Stage 0 exists to measure this before anything depends on it.
- A mid-job camera change throws away partial work. That is intended: moving takes over.
- Half-resolution upscale quality on the moving view needs the eye check.
