# Lua Plugin System Phase 5: Canvas Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plugins draw through `helix.canvas`, a retained display list replayed by a `plugin_canvas` XML widget, and temp-spark draws its sparkline with it.

**Architecture:** Lua builds a pending `DisplayList` (plain C++ data, charged to the plugin's memory cap) and `commit()` hands it to a process-wide registry keyed by the plugin-owned canvas name. Each live `plugin_canvas` instance replays the committed list for its name in `LV_EVENT_DRAW_MAIN_END`, resolving theme tokens at draw time, so Lua never runs during rendering. Instances report their content size to the registry; the binding defers a Lua `on_size` call through the runtime's lifetime token.

**Tech Stack:** C++17, LVGL 9.5 draw API (`lv_draw_line` with `points`, `lv_draw_rect`, `lv_draw_arc`, `lv_draw_label`), Lua 5.4.9 compiled as C++, helix-xml widget registration, Catch2.

**Spec:** `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`, section "Canvas (phase 5)". This phase completes the spec, so the ship change deletes the spec and this plan (Task 5).

## Global Constraints

- A plugin id matches `^[a-z][a-z0-9-]{1,31}$`; every name a plugin registers is `<id>__<rest>` (`plugin_owned_name`, `is_owned_name`, `kPluginNameSeparator` in `include/plugin_manifest.h`). A canvas name passed to `helix.canvas("x")` becomes `<id>__x`.
- Canvas (spec, verbatim): "A retained display list, so Lua never runs during rendering and there is no pixel buffer." Primitives `line`, `polyline`, `rect` (fill, border, radius), `arc`, `circle`, `text`; "Colors and fonts are theme token names"; "The widget replays the committed list in its own draw event"; "A list is capped at 4,096 primitives."
- Plugin XML passes `check_plugin_xml` (`src/plugin/plugin_xml_policy.cpp`): only the `plugin_event` callback, owned `name=` and subjects, no `cond` for a hyphenated id.
- Binding errors raise with `luaL_error` and name the call and the limit, in the existing form: `"helix.canvas: at most %d canvases per plugin"`. Limits are named `constexpr`s.
- Everything canvas-related runs on the main thread. Lua is entered only through `LuaRuntime::invoke` from a deferred callback (`LifetimeToken::defer`), never from an LVGL draw, layout or size event.
- Plugin sources are wrapped in `#if HELIX_HAS_PLUGINS` and listed in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`; plugin tests are wrapped in `#if HELIX_HAS_PLUGINS`.
- No `_for_testing` methods (lint-gated). Tests use the public registry API the widget itself uses.
- Code comments state the code as it is: no history, no review or mutation narration, no em-dashes. Doc citations are `path#symbol`, never line numbers.
- An example plugin uses each HelixScreen API it needs once, in the shortest honest form.
- Commits: `git commit -m "..." -- <paths>` with explicit paths (a new file: `git add -N <path>` first); never `git add -A`. Every behaviour commit names one hand mutation that turned its test red, in one line of the body.
- Phase 5 does not touch `src/application/application.cpp` (the audit session is splitting it): the widget registers from `register_plugin_event_callback` in `src/plugin/plugin_host.cpp` and the binding installs from PluginHost's installer list. A task that finds it truly needs `application.cpp` stops and asks the controller, who messages the audit session first.
- Per task: `make t F='<tags>'` only. `make full-test-run`, zeus mutate and zeus ASAN run once, in Task 5, by the controller.

## Review Focus

1. A plugin commits while no `plugin_canvas` instance exists (overlay closed, tile not placed): the list must draw as soon as an instance appears. Test in Task 1 ("an instance created after commit draws the committed list") and Task 3 (the detail overlay opens on a list committed earlier).
2. Light/dark switch while a canvas is visible: colors must come from the current theme, not the theme at commit time. Test in Task 1 ("tokens resolve at draw time").
3. Plugin unload or reload while a size notification is queued, or while an instance is on screen: no use-after-free, and the canvas goes blank on unload. Tests in Task 2 (ASAN-relevant lifetime cases).
4. A polyline built in a loop past the cap: an error naming the limit, and the pending list stays usable (clear and redraw works). Test in Task 2.
5. NaN, infinity or huge coordinates, unknown tokens, a non-string text: rejected at the call with an error naming the rule, never reaching the draw. Tests in Task 2.

---

### Task 1: The display list, the canvas registry and the `plugin_canvas` widget

**Files:**
- Create: `include/plugin_canvas.h`, `src/plugin/plugin_canvas.cpp` (add the .cpp to `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` next to the other `src/plugin/` entries)
- Modify: `include/theme_manager.h`, `src/ui/theme_manager.cpp` (`theme_manager_has_color`), `src/plugin/plugin_host.cpp` (`register_plugin_event_callback` also registers the widget), `src/plugin/plugin_xml_policy.cpp` (allow `plugin_canvas`)
- Test: `tests/unit/test_plugin_canvas.cpp` (`[plugin][canvas]`), `tests/unit/test_plugin_xml_policy.cpp` (`[xml_policy]`)

**Interfaces:**
- Produces (in `namespace helix::plugin`, `include/plugin_canvas.h`):

```cpp
constexpr size_t kMaxCanvasUnits = 4096;  // spec cap; a polyline counts each point
constexpr size_t kMaxCanvasTokens = 32;   // distinct color and font tokens per list
constexpr int32_t kMaxCanvasCoord = 16384;

enum class CanvasOp : uint8_t { Line, Polyline, Rect, Arc, Circle, Text };
constexpr uint8_t kNoToken = 0xFF;

/// One drawing command. Coordinates are canvas-relative pixels, origin at the
/// top-left of the widget's content box. Token fields index DisplayList::tokens.
struct CanvasPrim {
    CanvasOp op;
    uint8_t color = kNoToken;  // stroke, or fill for Rect/Circle
    uint8_t border = kNoToken; // Rect/Circle border
    uint8_t font = kNoToken;   // Text
    int32_t width = 1;         // stroke or border width
    int32_t radius = 0;        // Rect corner radius; Arc/Circle radius
    lv_value_precise_t a = 0, b = 0, c = 0, d = 0; // Line x1 y1 x2 y2; Rect x y w h;
                                                   // Arc cx cy start end (degrees); Circle cx cy; Text x y
    uint32_t first = 0, count = 0; // Polyline: range in points; Text: range in text
};

struct DisplayList {
    std::vector<CanvasPrim> prims;
    std::vector<lv_point_precise_t> points;
    std::string text;
    std::vector<std::string> tokens; // color tokens (e.g. "primary") and font tokens (e.g. "body")
    size_t units = 0;
    /// Bytes this list holds, the figure charged to the plugin's memory cap.
    size_t bytes() const;
};

/// Registers the plugin_canvas XML widget once per process.
void register_plugin_canvas_widget();

/// Publishes `list` as the committed list for `name` and invalidates every live
/// instance. A null list blanks the canvas.
void canvas_commit(const std::string& name, std::unique_ptr<DisplayList> list);
/// The committed list for `name`, or nullptr.
const DisplayList* canvas_committed(const std::string& name);
/// Content size the most recent live instance of `name` reported, {0, 0} when none.
std::pair<int32_t, int32_t> canvas_size(const std::string& name);
/// Live plugin_canvas instances named `name`.
size_t canvas_instance_count(const std::string& name);
/// Called (main thread, synchronously, from the size event) when an instance of `name`
/// reports a content size different from the last one. An empty function removes it.
/// The listener must not enter Lua; it defers.
void canvas_set_size_listener(const std::string& name, std::function<void(int32_t, int32_t)> fn);
/// Replays `list` into `layer` with `content` as its origin and resolves every token now.
void draw_display_list(lv_layer_t* layer, const lv_area_t& content, const DisplayList& list);
```

- Produces (`include/theme_manager.h`): `bool theme_manager_has_color(const char* base_name);` true when `theme_manager_get_color` would find the token (both `_light` and `_dark` variants, or a plain constant). Extract the lookup `theme_manager_get_color` already does into one static helper both call; do not write a second copy of it.

**Behaviour:**
- The widget mirrors `src/ui/helix_sparkline.cpp` (read it first): `lv_obj_create` + `lv_obj_remove_style_all`, default size `lv_pct(100)` x `lv_pct(100)`, clear `LV_OBJ_FLAG_SCROLLABLE` and `LV_OBJ_FLAG_CLICKABLE` (a canvas inside a clickable tile must not swallow the tap), draw in `LV_EVENT_DRAW_MAIN_END`, free per-instance state in `LV_EVENT_DELETE`. The XML create callback reads the `name` attribute from `attrs` and keys the instance by it; `lv_xml_obj_apply` is the apply callback.
- Per-instance state: the name (heap string as event user data). On create it joins the registry slot for its name; on delete it leaves. A slot with no instances, no committed list and no listener is erased.
- `LV_EVENT_SIZE_CHANGED`: read `lv_obj_get_content_width/height`; if it differs from the slot's stored size, store it and call the listener. Verify in a test that a freshly created instance reports its size after `lv_obj_update_layout` (if LVGL does not send `SIZE_CHANGED` for the first layout, also report from `LV_EVENT_LAYOUT_CHANGED` or the create path, and say which in the report).
- Draw: `lv_obj_get_content_coords(obj, &content)`; look the slot up by name (a hash lookup per draw; no cached pointer, so a slot erased between frames cannot dangle); if a list is committed, `draw_display_list(lv_event_get_layer(e), content, *list)`.
- `draw_display_list`: resolve each token once per call: colors via `theme_manager_get_color(token)`, fonts via `theme_manager_get_font(("font_" + token).c_str())` (null font: skip that Text). Then per primitive, offset by `content.x1/y1`:
  - Line: `lv_draw_line_dsc_t` with p1/p2, `width`, color.
  - Polyline: `lv_draw_line_dsc_t` with `points`/`point_cnt` from a local translated copy (`lv_draw_line` copies the array, so a local vector is safe).
  - Rect: `lv_draw_rect_dsc_t`; fill sets `bg_color` + `bg_opa = LV_OPA_COVER` (else `bg_opa = LV_OPA_TRANSP`), border sets `border_color` + `border_width`, `radius`; area from x, y, w, h.
  - Circle: a Rect around (cx, cy) with `radius = LV_RADIUS_CIRCLE`.
  - Arc: `lv_draw_arc_dsc_t` with `center`, `radius`, `start_angle`, `end_angle`, `width`, color.
  - Text: `lv_draw_label_dsc_t`, `text` pointing into the list, `text_local = 1`, font and color, area from (x, y) to (content.x2, y + font line height).
- Registration: `register_plugin_event_callback` in `src/plugin/plugin_host.cpp` already guards with a static flag; call `register_plugin_canvas_widget()` inside it.
- Policy: add `"plugin_canvas"` to `is_allowlisted_app_component` in `src/plugin/plugin_xml_policy.cpp` and to its comment. Its only name-taking attribute is `name=`, already checked for ownership.

- [ ] **Step 1: Failing tests** (`tests/unit/test_plugin_canvas.cpp`, `XMLTestFixture` so theme tokens and helix-xml are up; register a tiny component with `lv_xml_register_component_from_data` holding `<plugin_canvas name="t__c" width="200" height="100"/>`, or create the widget with `lv_xml_create` of an inline component, following `tests/unit/test_plugin_host.cpp`'s `lv_xml_create(lv_screen_active(), "<component>", nullptr)`; erase every slot the test used at its end by committing null and deleting instances):
  - "a fresh instance reports its content size": create, `lv_obj_update_layout`, `canvas_size("t__c") == {200, 100}`, `canvas_instance_count("t__c") == 1`; delete the object, `canvas_instance_count == 0`.
  - "the size listener sees each change once": listener counts calls; resize 200x100 -> 300x100 -> 300x100; two calls, last (300, 100).
  - "an instance created after commit draws the committed list": commit a list with one Line, then create an instance, `lv_refr_now(nullptr)`; no crash, and `canvas_committed("t__c")` is the list. Then commit null: `canvas_committed` is nullptr and a refresh draws nothing.
  - "every primitive kind replays": a list with one of each op (tokens "primary", "text", font "body"), committed to a live instance, `lv_refr_now(nullptr)` completes (this is the ASAN case for the draw path).
  - "a committed fill paints its pixels": commit one Rect covering the whole content box with fill "primary"; `lv_snapshot_take` the canvas object (follow `tests/unit/test_spoolman_mark_render.cpp`'s snapshot pattern) and require the center pixel to equal `theme_manager_get_color("primary")`; commit null and the center pixel is no longer that color.
  - "tokens resolve at draw time": commit a full-box Rect filled with a token whose light and dark values differ (e.g. "card_bg"); snapshot, switch dark mode, invalidate, snapshot again; the center pixels differ and each equals `theme_manager_get_color("card_bg")` in its mode. To switch dark mode, find the switch the theme tests use (e.g. `theme_manager_apply_theme(current, !dark)`) and restore the mode at the end. If no test-reachable theme switch exists, say so in the report and assert instead that `draw_display_list` calls the resolver per call (expose the token-resolution step as a free function `resolve_canvas_tokens(const DisplayList&)` returning the resolved colors and fonts, and test that function).
  - "theme_manager_has_color": true for "primary" and "text", false for "no_such_token".
  - In `test_plugin_xml_policy.cpp`: `<plugin_canvas name="p__c"/>` in plugin "p" passes; `<plugin_canvas name="c"/>` fails with the object-name error.
- [ ] **Step 2: Run to verify they fail:** `make t F='[canvas]'` and `make t F='[xml_policy]'` (compile errors count as failing).
- [ ] **Step 3: Implement** per the Behaviour section.
- [ ] **Step 4: Run to verify they pass:** `make t F='[canvas]'`, `make t F='[xml_policy]'`, `make t F='[plugin]'`, `make t F='[theme]'`, then `make` and `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 5: Hand mutations:** (a) make the draw callback return before replaying: "a committed fill paints its pixels" goes red. (b) drop the instance removal in `LV_EVENT_DELETE`: "a fresh instance reports its content size" goes red on the count. Restore both.
- [ ] **Step 6: Commit:** `git commit -m "feat(plugin): plugin_canvas widget replays a committed display list" -- <paths>`

---

### Task 2: `helix.canvas`

**Files:**
- Create: `src/plugin/lua_bind_canvas.cpp` (ESP32-excluded like the other bindings)
- Modify: `include/lua_bindings.h` (declare `install_canvas_bindings`), `src/plugin/plugin_host.cpp` (add it to the installer list after `install_widget_bindings`), `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp` (external charge)
- Test: `tests/unit/test_lua_bindings_canvas.cpp` (`[plugin][lua][canvas]`), `tests/unit/test_lua_runtime.cpp` (`[plugin][lua]`)

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces:
  - `bool LuaRuntime::reserve_external(size_t bytes)`: adds `bytes` to the runtime's used memory unless that passes the cap (then false, unchanged). `void LuaRuntime::release_external(size_t bytes)`. Memory a binding keeps outside the Lua heap on the plugin's behalf counts against the same cap as Lua allocations.
  - `void install_canvas_bindings(PluginContext& ctx);`
  - Lua: `local c = helix.canvas("graph")` returns a handle for `<id>__graph` (the same canvas for repeated calls). Methods:
    - `c:line(x1, y1, x2, y2 [, opts])` with opts `color` (default `"text"`), `width` (default 1).
    - `c:polyline(points [, opts])`: `points` is a flat array `{x1, y1, x2, y2, ...}` of at least 2 points (even length); opts as line.
    - `c:rect(x, y, w, h, opts)`: opts `fill`, `border`, `border_width` (default 1), `radius` (default 0); at least one of `fill`/`border`.
    - `c:circle(cx, cy, r, opts)`: opts `fill`, `border`, `border_width`; at least one of `fill`/`border`.
    - `c:arc(cx, cy, r, start_deg, end_deg [, opts])`: opts `color`, `width`.
    - `c:text(x, y, str [, opts])`: opts `font` (default `"body"`), `color` (default `"text"`); `str` at most 256 bytes.
    - `c:clear()` empties the pending list. `c:commit()` publishes the pending list and starts a new empty one.
    - `c:size()` returns `w, h` (0, 0 while no instance has a size).
    - `c:on_size(fn)` registers the handler called with `w, h` on the main loop after an instance's content size changes; registering when a size is already known schedules one call. `nil` removes it.

**Limits** (named constexprs in `lua_bind_canvas.cpp` unless Task 1 already owns them):
- `kMaxCanvases = 8` per plugin: `"helix.canvas: at most %d canvases per plugin"`.
- `kMaxCanvasUnits` (4096, Task 1) per list, a polyline counting each point: `"helix.canvas: a list holds at most %d primitives (a polyline counts each point)"`. The call that would pass it adds nothing.
- `kMaxCanvasTokens` (32, Task 1) distinct tokens per list.
- Coordinates, sizes and radii: finite numbers with absolute value at most `kMaxCanvasCoord`: `"helix.canvas: coordinates must be finite numbers within +-%d"`. Widths and radii are non-negative; `width`/`border_width` at most 64.
- Color tokens must pass `theme_manager_has_color`, font tokens `theme_manager_get_font("font_" + name) != nullptr`: `"helix.canvas: unknown color token '%s'"` / `"unknown font token '%s'"`.
- Unknown option keys raise `"helix.canvas: unknown option '%s'"`.
- Every byte the pending and committed lists hold is charged with `reserve_external` as the list grows; a call whose growth the cap refuses raises `"helix.canvas: list would exceed the plugin memory cap"` and adds nothing. Commit moves the charge from pending to committed and releases the replaced committed list's charge.

**Lifetime** (copy `src/plugin/lua_bind_widget.cpp`'s pattern: per-runtime state as light userdata in the Lua registry, deleted by an `on_close` closer, no `__gc` that touches it; handles are userdata holding the canvas index, as `src/plugin/lua_bind_core.cpp`'s timer handles do):
- The closer, for each canvas: `canvas_set_size_listener(name, {})`, `canvas_commit(name, nullptr)` (an unloaded plugin's canvases go blank), then deletes the state. It does not call `release_external` (the runtime is going away).
- The size listener captures the runtime pointer, its `LifetimeToken` and the canvas name; it coalesces with a per-canvas pending flag and defers with `token.defer("plugin_canvas_size", ...)`; the deferred body re-reads `canvas_size(name)` and invokes the handler ref with the latest size only if it differs from the size last delivered to Lua.

- [ ] **Step 1: Failing tests** (`tests/unit/test_lua_bindings_canvas.cpp`; `XMLTestFixture` for theme tokens; `BoundRuntime` from `tests/test_helpers/plugin_test_support.h` with `{&install_canvas_bindings}` for pure binding cases; a fixture plugin `tests/fixtures/plugins/canvas-demo/` (manifest, `main.lua`, `ui/canvas-demo__panel.xml` holding `<plugin_canvas name="canvas-demo__c" width="160" height="80"/>`) loaded through `HostRig` from `tests/test_helpers/plugin_host_test_support.h` for lifetime cases; grep every test that counts `tests/fixtures/plugins` dirs and update its count):
  - "commit publishes what was drawn": `c = helix.canvas("c"); c:line(0,0,10,10,{color="primary"}); c:polyline({0,0,5,5,10,0}); c:commit()`; `canvas_committed("test-plugin__c")` has 2 prims, 3 points, units == 1 + 3; the pending list is empty (a second `commit()` blanks: committed has 0 prims).
  - "each primitive validates its arguments": NaN coordinate, `1e9` coordinate, odd-length points, 1-point polyline, rect with neither fill nor border, unknown token, unknown option, 257-byte text, `width = 65`; each `pcall` returns false with the message naming the rule, and the pending list is unchanged.
  - "the unit cap holds and the list stays usable": a loop of `c:line` reaching 4096 succeeds; the 4097th raises the units message; `c:clear()` then one line and commit works.
  - "the canvas count is capped": 8 distinct names succeed; the ninth raises; a repeated name returns the same canvas.
  - "list bytes count against the memory cap": a runtime with a small cap (`TestRuntime` limits) fills a list until `"would exceed the plugin memory cap"`; `memory_used()` rose by about `bytes()`; after `c:clear()` it falls back.
  - In `test_lua_runtime.cpp`: "external reservations share the cap": `reserve_external` succeeds below the cap, fails above it without changing `memory_used()`, and `release_external` restores it.
  - "on_size runs on the main loop with the content size" (HostRig + canvas-demo): `main.lua` registers `c:on_size(function(w, h) helix.subject.string("sz", ""):set(w .. "x" .. h) end)` (create the subject once at load); create the panel with `lv_xml_create`, `lv_obj_update_layout`, `drain()`; subject reads `160x80`; resizing to 200x80 then draining reads `200x80`; two resizes before one drain deliver one call.
  - "a reload delivers the known size to the new runtime": after the case above, `rig.host->rescan({"canvas-demo"})` (or the reload path PluginHost exposes), `drain()`; the subject (recreated by the new runtime) reads the current size with no resize.
  - "unload blanks the canvas and drops queued size calls": resize (queues a deferred call), then disable the plugin before `drain()`; `drain()` runs nothing into the dead runtime (no crash under ASAN), `canvas_committed("canvas-demo__c") == nullptr`, and `lv_refr_now(nullptr)` with the instance still on screen completes.
  - "a deleted instance with a queued size call is harmless": resize, delete the panel object, `drain()`; no crash.
- [ ] **Step 2: Run to verify they fail:** `make t F='[canvas]'`.
- [ ] **Step 3: Implement.** Validate every argument before touching the pending list, so a raise leaves it unchanged.
- [ ] **Step 4: Run to verify they pass:** `make t F='[canvas]'`, `make t F='[plugin]'`, `make t F='[lua]'`, then `make` and `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 5: Hand mutations:** (a) skip `canvas_commit(name, nullptr)` in the closer: "unload blanks the canvas" goes red; (b) skip the per-canvas pending flag: "two resizes before one drain deliver one call" goes red; (c) skip the unit check: the cap case goes red. Restore all.
- [ ] **Step 6: Commit:** `git commit -m "feat(plugin): helix.canvas builds display lists for plugin_canvas" -- <paths>`

---

### Task 3: temp-spark draws its sparkline on a canvas

**Files:**
- Modify: `examples/plugins/temp-spark/main.lua`, `examples/plugins/temp-spark/ui/temp-spark__tile.xml`, `examples/plugins/temp-spark/ui/temp-spark__detail.xml`, `examples/plugins/temp-spark/README.md`, `tests/unit/test_example_plugin_temp_spark.cpp`

**Interfaces:**
- Consumes: `helix.canvas`, `c:polyline`, `c:line`, `c:commit`, `c:size`, `c:on_size` (Task 2); `canvas_committed`, `canvas_size` (Task 1) in the test.
- Produces: canvases `temp-spark__spark` (tile) and `temp-spark__graph` (detail overlay). The 30 `bar` subjects and the 30 `lv_bar` elements in each XML are deleted.

**Lua** (keep every other function as it is). Replace the `bars` table and its loop with the two canvas handles:

```lua
-- Each canvas is a retained drawing: draw() rebuilds its list from the window
-- and commits it, and the widget replays it until the next commit.
local spark = helix.canvas("spark")
local graph = helix.canvas("graph")
```

Define `draw` immediately before `render()`, after the `samples`, `latest`, `target_now` and `timer` locals (a local function only sees locals declared above it):

```lua
-- Points are percent of an absolute-temperature scale, so a reading keeps its
-- height while the window slides. The window fills from the right edge.
local function draw(c, scale, with_target)
    local w, h = c:size()
    if w > 1 and h > 1 and #samples >= 2 then
        local pts = {}
        for i, t in ipairs(samples) do
            pts[#pts + 1] = (N - #samples + i - 1) * (w - 1) / (N - 1)
            pts[#pts + 1] = (h - 1) * (1 - t / scale)
        end
        c:polyline(pts, {color = "primary", width = 2})
        if with_target and target_now and helix.settings.get("show_target") then
            local y = (h - 1) * (1 - target_now / scale)
            c:line(0, y, w - 1, y, {color = "text_muted"})
        end
    end
    c:commit()
end
```

In `render()`, after computing `scale`, replace the `for i = 1, N do ... bars[i]:set(...) end` loop with:

```lua
    draw(spark, scale)
    draw(graph, scale, true)
```

`render()` must compute `scale` the same way it does today (`math.max(50, hi or 0, target_now or 0) * 1.1`), and must still work with an empty window (`lo`/`hi` nil). After the `helix.settings.on_change` lines add:

```lua
-- A canvas reports its size once laid out and again on every resize; the
-- drawing is rebuilt for the new size.
spark:on_size(render)
graph:on_size(render)
```

**XML:**
- Tile: replace the `temp-spark__bars` `lv_obj` and its 30 `lv_bar` children with `<plugin_canvas name="temp-spark__spark" width="100%" flex_grow="1"/>`. Rewrite the top comment to describe the tile as it is (value label above a canvas sparkline).
- Detail: replace the empty `temp-spark__detail_bars` `lv_obj` (and its bars) with `<plugin_canvas name="temp-spark__graph" width="100%" flex_grow="1"/>`; rewrite its comment.

**Tests** (rewrite the bar assertions; keep every text-subject assertion):
- A helper creates the tile like home does: `lv_obj_t* tile = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "temp-spark__tile", nullptr)); lv_obj_set_size(tile, 400, 200); lv_obj_update_layout(tile); drain();` and reads the canvas content size with `canvas_size("temp-spark__spark")`.
- A helper `std::vector<lv_point_precise_t> spark_points()` returns the committed polyline's points (`canvas_committed("temp-spark__spark")`, the single Polyline prim's range).
- "temp-spark backfills the window from the store": with the tile created before answering the backfill, after the answer: 30 points; with `(w, h) = canvas_size(...)` and scale 220, point 1 is `((0) * (w - 1) / 29, (h - 1) * (1 - 25.0 / 220))` and point 30 is `(w - 1, (h - 1) * (1 - 54.0 / 220))`, compared with `Catch::Approx` margin 0.01.
- Create the tile with the helper right after `load_from` in every case below, before any backfill answer.
- "a live reading shifts the window on the timer": window 26..54 then 60 at scale 220: point 1 y is `(h - 1) * (1 - 26.0 / 220)`, point 30 y is `(h - 1) * (1 - 60.0 / 220)`.
- "a failed backfill leaves the plugin loaded and empty": the committed spark list is null or has no prims. Extend it: two live readings (`set_deci("extruder_temp", 300)`, `process_lvgl(1200)`, `set_deci("extruder_temp", 310)`, `process_lvgl(1200)`) give a 2-point polyline whose last x is `w - 1` and first x is `28.0 * (w - 1) / 29` (the window fills from the right).
- "switching heaters refetches for the new store key": scale 75.9: point 1 y is `(h - 1) * (1 - 40.0 / 75.9)`, point 30 y is `(h - 1) * (1 - 69.0 / 75.9)`.
- New "the detail graph draws the target line": open the overlay (the existing overlay case does this), `lv_obj_update_layout` on it, `drain()`; `canvas_committed("temp-spark__graph")` holds one Polyline and one Line whose y equals `(h - 1) * (1 - target / scale)` for the target the case sets; with `show_target` false after a settings change, no Line.
- The XML policy check in the load case now also passes `temp-spark__detail.xml`.

**README:** "What it demonstrates" gains `helix.canvas` (polyline, line, commit, on_size); the file list describes the tile as a value label above a canvas sparkline and the detail as a larger canvas with the target line, plus min/max/target labels. Remove every mention of bars.

- [ ] **Step 1:** Rewrite the tests first; run `make t F='[example]'`: the canvas cases fail (no canvas yet in the plugin).
- [ ] **Step 2:** Change the XML and Lua as above.
- [ ] **Step 3:** `make t F='[example]'` and `make t F='[plugin]'` pass.
- [ ] **Step 4: Hand mutation:** drop the right-alignment (`(N - #samples + i - 1)` becomes `(i - 1)`): the extended failed-backfill case goes red on the first x. Restore.
- [ ] **Step 5: Live check** (pinned socket and config dir per CLAUDE.md, `TREE=lua-plugins-phase5`, `--test`, `HELIX_PLUGIN_DIR=examples/plugins`): enable temp-spark in Settings > Plugins, place the tile, wait for a few samples, `ctl screenshot` the home panel and the detail overlay to `/tmp/p5-temp-spark-*.png`, and `ctl geom temp-spark__spark` to confirm a non-zero size. Look at both screenshots and describe what they show in the report; name the image paths. Kill only your own instance by its socket-resolved PID.
- [ ] **Step 6: Commit:** `git commit -m "feat(plugins): temp-spark draws its sparkline on helix.canvas" -- <paths>`

---

### Task 4: Docs, and moving the spec's durable facts before the spec is deleted

**Files:**
- Modify: `docs/devel/PLUGIN_DEVELOPMENT.md`, `docs/devel/architecture/12-system-services.md`, `docs/devel/HELIX_XML_FORK.md`, `docs/devel/architecture/03-threading-lifetime.md`, the spec's Status line (`docs/devel/plans/2026-09-28-lua-plugin-system-design.md`: "Phases 1 to 5 implemented")

- [ ] **Step 1: Guide.** Read `src/plugin/lua_bind_canvas.cpp`, `include/plugin_canvas.h` and the temp-spark files; document from the code, not this plan:
  - section 7 (XML): the `plugin_canvas` element (owned `name=`, default size 100% x 100%, not clickable by default so a tile's tap still lands).
  - section 8 (Lua API): a `helix.canvas` table with every method, its arguments, defaults and options; coordinates are content-box pixels; the flat points array; `commit` publishes and starts empty; `size` and `on_size`; one canvas name is one drawing, so give a tile canvas and an overlay canvas different names; tokens are the XML color tokens without `#` and font tokens without `font_`.
  - section 10 (limits): canvases per plugin, units per list (polyline counts points), tokens per list, coordinate range, text bytes, list bytes against the memory cap.
  - section 12 (worked examples): temp-spark's `draw()` snippet, quoted verbatim.
- [ ] **Step 2: Architecture.** `12-system-services.md`: one paragraph on the canvas: retained list, registry keyed by plugin-owned name, replay in `LV_EVENT_DRAW_MAIN_END`, tokens resolved at draw time, size reports deferred into Lua through the runtime token, the closer blanks an unloaded plugin's canvases; cite `include/plugin_canvas.h#canvas_commit` and `src/plugin/lua_bind_canvas.cpp`.
- [ ] **Step 3: The spec's durable facts.** Task 5 deletes the spec. Read it section by section (Why, Goals, Non-goals, Decisions, Plugin layout, Components, Lifecycle, Lua API, Limits and faults, Canvas, Testing) and, for every fact that is still true of the code and a maintainer or author would need, confirm it is in `PLUGIN_DEVELOPMENT.md` or `12-system-services.md`. Move each missing one into the right doc in present tense (the "why Lua, why no ESP32 plugins, why main-thread only, why a retained list" rationale belongs in `12-system-services.md`). List every moved fact, and every fact deliberately dropped with its reason, in the report.
- [ ] **Step 4: References.** Rewrite the two lines in `HELIX_XML_FORK.md` and `03-threading-lifetime.md` that call plugins "being rebuilt" and cite the spec: present tense, pointing at `docs/devel/PLUGIN_DEVELOPMENT.md` and `docs/devel/architecture/12-system-services.md`. `grep -rn 'lua-plugin-system-design' docs src include tests scripts *.md` must find nothing but the spec itself.
- [ ] **Step 5:** `make check-doc-anchors` reports nothing on lines you added; no em-dashes in added lines.
- [ ] **Step 6: Commit:** `git commit -m "docs(plugin): helix.canvas in the author guide; the design's durable facts move into the architecture docs" -- <paths>`

---

### Task 5: Gates (controller)

- [ ] **Step 1:** Merge `origin/main` into the branch (`git -c merge.autoStash=false merge origin/main`), resolve, build, run the touched tags. Main carries a new `patches/lvgl_sdl_window.patch` (743d489ed): if the build or the pre-push patch-drift check reports drift, run `make reapply-patches` in this worktree, then rebuild.
- [ ] **Step 2:** In the same merge commit or one right after it, `git rm` both `docs/devel/plans/2026-09-28-lua-plugin-system-design.md` and `docs/devel/plans/2026-10-04-lua-plugin-system-phase5.md`; `grep -rn 'lua-plugin-system-design\|lua-plugin-system-phase5' .` outside build output finds nothing.
- [ ] **Step 3:** `make full-test-run`, `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 4:** Push the branch; `scripts/zeus-run.sh mutate --tests '[plugin],[canvas],[example],[xml_policy],[theme]' --base <merge-base with origin/main> --max-hunks 0`; close real survivors with tests.
- [ ] **Step 5:** LAST: `scripts/zeus-run.sh asan '[plugin],[canvas],[example],[xml_policy]'`, then `python3 scripts/check_asan_leaks.py --baseline scripts/asan_leak_baseline.txt <log>`.
- [ ] **Step 6:** Preston's OK, then merge to main from the main tree under a claim, push, tear down the worktree, tell the audit session.
