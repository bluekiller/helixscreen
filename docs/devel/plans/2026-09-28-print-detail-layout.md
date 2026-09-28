# Print File Detail Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pre-print options become a 2-column grid of option tiles, Delete and Print are pinned on screen at every size, and the print file detail view has a portrait arrangement, with a fade + chevron cue when the options scroll.

**Architecture:** One widget tree in `ui_xml/print_file_detail.xml`, rearranged for portrait by complementary bound styles on `ui_is_portrait`. The options column becomes a scroll area plus a pinned action block. Two pure functions (portrait preview height, default option icon) carry the logic and are unit-tested; the view calls them from a layout event and a scroll event.

**Tech Stack:** C++17, LVGL 9.5 + the helix-xml engine (`lib/helix-xml/`), Catch2 (`tests/unit/`), `helix-screen ctl` for geometry and screenshots.

**Spec:** `docs/devel/plans/2026-09-28-print-detail-layout-design.md` (commit c9a0149cd). Read it before any task.

## Global Constraints

- Work in a worktree: `scripts/setup-worktree.sh feature/print-detail-layout`, claim it with `scripts/helix-claim take worktree:print-detail-layout`.
- DATA in C++, APPEARANCE in XML (`.claude/rules/declarative-ui.md`). Scroll, size and layout events are the sanctioned C++ hooks here.
- Tokens only: colors `#primary` `#border` `#card_bg` `#elevated_bg` `#text_muted`, spacing `#space_*`. No literal colors; raw pixel literals need a `SIZE_OK:` comment (`scripts/check_hardcoded_pixels.py` ratchets them).
- An inline attribute beats a bound style. Anything that changes with `ui_is_portrait` or `ui_breakpoint` is two bound styles, never one bound style over an inline attribute.
- New code lives in `namespace helix` or a child (`scripts/check_namespace_compliance.py --summary` ratchet).
- Comments describe the code as it is now. No SHAs, no "used to", no narrated history. No em-dashes anywhere.
- Commits: pathspec or `git add` of your own paths inside the worktree; never `--no-verify`; `git show --stat HEAD` after each.
- Builds in the FOREGROUND. `make t F='[tag]'` is the loop. Never `pkill helix-screen`; kill your own instance by PID from its socket.
- Drive the app with a pinned socket and config dir (CLAUDE.md Quick Start box).

## Review Focus

1. **Rotation while the view is open** (SDL window resized across the portrait boundary): the tree is restyled, not rebuilt, so every pointer the view holds stays valid and the preview height is recomputed. Task 5's layout handler must run on every `LV_EVENT_LAYOUT_CHANGED`, and clear its override when the canvas goes landscape.
2. **Option set changes while open** (multi-printer switch repopulates rows): the more-below subject must be recomputed after `populate_option_rows()`, not only on scroll. Task 5 pins it.
3. **Zero options** (`has_any_preprint_options` 0): the options card hides, the scroll area holds only filaments, and nothing overflows at 800x480. Task 6 includes a zero-option canvas check.
4. **Tap on a tile while its option is plugin-hidden then re-shown**: visibility binding stays on the tile, not a child. Task 2 keeps the existing plugin-visibility test green on tiles.
5. **Language change**: the tile label keeps `label_tag` so `lv_label_set_translation_tag` re-resolves it. Task 2's label test asserts the translation tag on the tile's label.

---

### Task 1: Pure layout decision for the portrait preview

**Files:**
- Create: `include/print_detail_layout.h`
- Test: `tests/unit/test_print_detail_layout.cpp`

**Interfaces:**
- Produces: `int helix::ui::decide_detail_portrait_preview(int width, int avail_h, int content_h, int grid_top, int tile_h, int gap);` Returns the preview card height in px. `min_h` is derived inside as `width / 3`.

- [ ] **Step 1: Write the failing tests**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#include "print_detail_layout.h"

#include "../catch_amalgamated.hpp"

using helix::ui::decide_detail_portrait_preview;

namespace {
// 480x800 portrait numbers: preview 452 wide -> base 282 (16:10).
constexpr int W = 452;
constexpr int BASE = W * 10 / 16;
constexpr int TILE = 48;
constexpr int GAP = 6;
constexpr int GRID_TOP = 120; // tile grid starts 120px into the scroll content

// Where the visible edge falls inside the scroll content for a given preview height.
int edge_for(int avail_h, int preview_h) {
    return avail_h - preview_h;
}
bool mid_tile(int edge) {
    if (edge < GRID_TOP) {
        return false;
    }
    const int in_row = (edge - GRID_TOP) % (TILE + GAP);
    return in_row >= TILE / 4 && in_row <= TILE - TILE / 4;
}
} // namespace

TEST_CASE("portrait preview: content fits -> 16:10 base, no nudge", "[print_detail_layout]") {
    const int avail = BASE + 300;
    CHECK(decide_detail_portrait_preview(W, avail, 250, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: overflow with edge in a grid gap is nudged mid-tile",
          "[print_detail_layout]") {
    // Put the edge exactly at the end of row 1 (inside the gap).
    const int edge = GRID_TOP + TILE + 2;
    const int avail = BASE + edge;
    const int h = decide_detail_portrait_preview(W, avail, 1000, GRID_TOP, TILE, GAP);
    CHECK(h < BASE);
    CHECK(BASE - h <= GAP + TILE / 4);
    CHECK(mid_tile(edge_for(avail, h)));
}

TEST_CASE("portrait preview: overflow with edge in a tile's outer quarter is nudged",
          "[print_detail_layout]") {
    const int edge = GRID_TOP + TILE - 3; // bottom outer quarter of row 1
    const int avail = BASE + edge;
    const int h = decide_detail_portrait_preview(W, avail, 1000, GRID_TOP, TILE, GAP);
    CHECK(mid_tile(edge_for(avail, h)));
}

TEST_CASE("portrait preview: overflow with edge already mid-tile is left alone",
          "[print_detail_layout]") {
    const int edge = GRID_TOP + TILE / 2;
    CHECK(decide_detail_portrait_preview(W, BASE + edge, 1000, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: edge above the tile grid is left alone", "[print_detail_layout]") {
    const int edge = GRID_TOP - 10;
    CHECK(decide_detail_portrait_preview(W, BASE + edge, 1000, GRID_TOP, TILE, GAP) == BASE);
}

TEST_CASE("portrait preview: never below width / 3", "[print_detail_layout]") {
    // 272x480: 256 wide, base 160, floor 85. An edge in a gap right at the floor.
    const int w = 256;
    const int floor = w / 3;
    const int h = decide_detail_portrait_preview(w, 160 + GRID_TOP + TILE + 1, 1000, GRID_TOP,
                                                 TILE, GAP);
    CHECK(h >= floor);
    CHECK(decide_detail_portrait_preview(w, 0, 1000, GRID_TOP, TILE, GAP) >= floor);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[print_detail_layout]'`
Expected: compile error, `print_detail_layout.h` not found.

- [ ] **Step 3: Implement**

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>

namespace helix::ui {

/// Portrait preview card height for the print file detail view.
///
/// The preview is 16:10 of its width. When the options below it overflow,
/// the scroll area's visible bottom edge should cut a tile through its middle
/// half, so part of a tile shows under the fade cue: an edge in a grid gap or
/// a tile's outer quarter shrinks the preview until it does. Edges above the
/// tile grid are left alone; the cue alone carries those. Never below width/3,
/// where the preview stops reading as a model.
inline int decide_detail_portrait_preview(int width, int avail_h, int content_h, int grid_top,
                                          int tile_h, int gap) {
    const int base = width * 10 / 16;
    const int min_h = width / 3;
    if (content_h <= avail_h - base) {
        return std::max(base, min_h);
    }
    const int edge = avail_h - base;
    const int pitch = tile_h + gap;
    if (edge < grid_top || pitch <= 0) {
        return std::max(base, min_h);
    }
    const int in_row = (edge - grid_top) % pitch;
    const int lo = tile_h / 4;
    const int hi = tile_h - tile_h / 4;
    if (in_row >= lo && in_row <= hi) {
        return std::max(base, min_h);
    }
    // Shrinking the preview moves the edge down. Past `hi` the next tile's
    // middle half starts at pitch + lo; below `lo` it starts at lo.
    const int shift = in_row < lo ? lo - in_row : pitch + lo - in_row;
    return std::max(base - shift, min_h);
}

} // namespace helix::ui
```

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[print_detail_layout]'`
Expected: 6 test cases pass.

- [ ] **Step 5: Commit**

```bash
git add include/print_detail_layout.h tests/unit/test_print_detail_layout.cpp
git commit -m "feat(ui): pure portrait preview height rule for the print file detail view"
```

Hand mutation for the body: change `pitch + lo - in_row` to `pitch - in_row`; the gap test goes red.

---

### Task 2: Option tiles replace the switch rows

**Files:**
- Create: `ui_xml/components/option_tile.xml`
- Delete: `ui_xml/components/compact_toggle_row.xml`
- Modify: `src/xml_registration.cpp` (swap the registration at the `compact_toggle_row` line)
- Modify: `include/ui_pre_print_options_renderer.h`, `src/ui/ui_pre_print_options_renderer.cpp`
- Modify: `tests/unit/test_job_holds_machine.cpp` (`kNoMachineControlFiles`: replace the `compact_toggle_row.xml` row with `ui_xml/components/option_tile.xml`, alphabetical position)
- Modify: `tests/unit/test_pre_print_options_renderer.cpp`
- Modify: `ui_xml/print_file_detail.xml` (only `pre_print_options_container`: row wrap grid)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `static std::string PrePrintOptionsRenderer::default_icon_for(const std::string& id);` `lv_obj_t* PrePrintOptionsRenderer::get_toggle(const std::string& id) const;` (renamed from `get_switch`; returns the tile, the checkable object). `OptionRow::switch_widget` is renamed `toggle_widget`, `on_switch_value_changed` is renamed `on_toggle_value_changed`, `SwitchUserData` is renamed `ToggleUserData`. The tile's name is `option_tile_<id>`.

- [ ] **Step 1: Write the failing tests** (append to `test_pre_print_options_renderer.cpp`; change the 4 existing `get_switch(` calls to `get_toggle(`)

```cpp
TEST_CASE("PrePrintOptionsRenderer: default icon per option id", "[pre_print_options][icons]") {
    CHECK(PrePrintOptionsRenderer::default_icon_for("bed_mesh") == "grid_large");
    CHECK(PrePrintOptionsRenderer::default_icon_for("shaper_calibrate") == "sine_wave");
    CHECK(PrePrintOptionsRenderer::default_icon_for("flow_calibrate") == "gauge");
    CHECK(PrePrintOptionsRenderer::default_icon_for("timelapse") == "camera_timer");
    CHECK(PrePrintOptionsRenderer::default_icon_for("u1_timelapse") == "camera_timer");
    CHECK(PrePrintOptionsRenderer::default_icon_for("ai_detect") == "robot");
    CHECK(PrePrintOptionsRenderer::default_icon_for("qgl") == "spirit_level");
    CHECK(PrePrintOptionsRenderer::default_icon_for("z_tilt") == "spirit_level");
    CHECK(PrePrintOptionsRenderer::default_icon_for("nozzle_clean") == "tune");
    CHECK(PrePrintOptionsRenderer::default_icon_for("") == "tune");
}

TEST_CASE_METHOD(LVGLUITestFixture, "PrePrintOptionsRenderer: a tap on a tile toggles its option",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    std::string toggled_id;
    int toggled_state = -1;
    renderer.populate(container, make_multi_category_set(), nullptr,
                      [&](const std::string& id, int s) {
                          toggled_id = id;
                          toggled_state = s;
                      });

    lv_obj_t* tile = renderer.get_toggle("nozzle_clean"); // default off
    REQUIRE(tile != nullptr);
    REQUIRE(lv_obj_has_flag(tile, LV_OBJ_FLAG_CHECKABLE));
    REQUIRE_FALSE(lv_obj_has_state(tile, LV_STATE_CHECKED));

    // What a real tap does: LVGL's base RELEASED handler flips CHECKED and
    // sends VALUE_CHANGED (lv_obj.c event handler).
    lv_obj_add_state(tile, LV_STATE_CHECKED);
    lv_obj_send_event(tile, LV_EVENT_VALUE_CHANGED, nullptr);

    CHECK(toggled_id == "nozzle_clean");
    CHECK(toggled_state == 1);
    CHECK(renderer.get_state("nozzle_clean") == 1);
}

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrePrintOptionsRenderer: tile children follow the checked state",
                 "[print_file_detail][pre_print_options]") {
    PrePrintOptionsRenderer renderer;
    lv_obj_t* container = lv_obj_create(test_screen());
    renderer.populate(container, make_multi_category_set(), nullptr, nullptr);

    lv_obj_t* tile = renderer.get_toggle("bed_mesh"); // default on
    REQUIRE(tile != nullptr);
    lv_obj_t* tab = lv_obj_find_by_name(tile, "check_tab");
    lv_obj_t* label = lv_obj_find_by_name(tile, "label");
    REQUIRE(tab != nullptr);
    REQUIRE(label != nullptr);
    CHECK(lv_obj_has_state(tab, LV_STATE_CHECKED)); // state_trickle
    CHECK(std::string(lv_label_get_translation_tag(label)) == "Auto Bed Mesh");

    renderer.set_state("bed_mesh", 0);
    process_lvgl(10);
    CHECK_FALSE(lv_obj_has_state(tile, LV_STATE_CHECKED));
    CHECK_FALSE(lv_obj_has_state(tab, LV_STATE_CHECKED));
}
```

If `get_state` / `set_state` / `lv_label_get_translation_tag` are named differently, read `include/ui_pre_print_options_renderer.h` and `lib/helix-xml` and use the real names; the assertions stay the same.

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[pre_print_options]'`
Expected: compile errors (`default_icon_for`, `get_toggle` not declared).

- [ ] **Step 3: Create `ui_xml/components/option_tile.xml`**

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- One pre-print option as a checkable tile: icon + label (2 lines, then
     ellipsis) in an outline. The whole tile is the tap target. Checked draws
     the #primary outline, tints the icon and shows the corner check tab;
     state_trickle hands the tile's checked state to its children so each
     child styles itself with a :checked selector. Created by
     PrePrintOptionsRenderer, which binds the option subject to the tile's
     checked state and wires value_changed. -->
<component>
  <api>
    <prop name="label" type="string" default="Option"/>
    <prop name="label_tag" type="string" default=""/>
    <prop name="icon" type="string" default="tune"/>
    <prop name="callback" type="string" default=""/>
  </api>
  <view name="option_tile"
        extends="lv_obj" width="48%" height="#option_tile_height" flex_flow="row" style_flex_cross_place="center"
        style_pad_hor="#space_sm" style_pad_ver="0" style_pad_gap="#space_xs" style_radius="#border_radius"
        style_border_width="1" style_border_color="#border" style_border_color:checked="#primary"
        style_bg_color="#card_bg" style_bg_opa="255" checkable="true" state_trickle="true" scrollable="false">
    <event_cb trigger="value_changed" callback="$callback"/>
    <icon name="icon" src="$icon" size="sm" variant="secondary" clickable="false"
          style_text_color:checked="#primary"/>
    <text_small name="label"
                text="$label" translation_tag="$label_tag" flex_grow="1" long_mode="dots"
                style_max_height="#option_tile_label_max_h" style_text_color:checked="#text" clickable="false"/>
    <!-- Corner check tab: invisible until checked. opa, not the hidden flag:
         a style can follow state, a flag cannot. -->
    <lv_obj name="check_tab"
            width="#option_tile_tab" height="#option_tile_tab" align="top_right" ignore_layout="true"
            style_radius="#border_radius" style_bg_color="#primary" style_bg_opa="255" style_border_width="0"
            style_pad_all="0" style_opa="0" style_opa:checked="255" clickable="false" scrollable="false">
      <icon src="check" size="xs" variant="on_primary" align="center" clickable="false"/>
    </lv_obj>
  </view>
</component>
```

Define the `option_tile_*` tokens in `ui_xml/globals.xml` next to the other component px tokens, one value per tier suffix (`_micro` `_tiny` `_small` `_medium` `_large` `_xlarge`): tile height 40/40/44/48/56/64, label max height = two `font_small` line boxes per tier (read the line heights in `globals.xml`), tab 14/14/16/16/18/20. Verify the icon widget honours `style_text_color:checked` over its `variant` (read `src/ui/ui_icon.cpp`); if the variant color is applied as a local style that wins, pass the tint through a bound style instead and note it in the report. If `on_primary` is not a variant, use the one `ui_button` uses for text on a primary background.

- [ ] **Step 4: Switch the renderer to tiles**

In `make_row`: create `"option_tile"` with attrs `label`, `label_tag`, `icon` = `default_icon_for(opt.id)`, keep `callback` unset exactly as the old row did (the comment block above the create call explains why; update its wording from "compact_toggle_row" to "option_tile" and "switch" to "tile"). The toggle widget is the created tile itself, so drop the `lv_obj_find_by_name(row_obj, "toggle")` lookup and its error branch; `row.row` and `row.toggle_widget` are the same object. Name it: `lv_obj_set_name(row_obj, ("option_tile_" + opt.id).c_str());`. The observer, visibility binding and value-changed wiring stay as they are, targeting the tile.

Add beside `label_key_for`:

```cpp
std::string PrePrintOptionsRenderer::default_icon_for(const std::string& id) {
    if (id == "bed_mesh") {
        return "grid_large";
    }
    if (id == "shaper_calibrate") {
        return "sine_wave";
    }
    if (id == "flow_calibrate") {
        return "gauge";
    }
    if (id == "timelapse" || id == "u1_timelapse") {
        return "camera_timer";
    }
    if (id == "ai_detect") {
        return "robot";
    }
    if (id == "qgl" || id == "z_tilt") {
        return "spirit_level";
    }
    return "tune";
}
```

Declare it public static in the header with a one-line doc. Rename per Interfaces and update the header's class doc ("label on the left and ui_switch on the right" becomes the tile description).

In `print_file_detail.xml`, `pre_print_options_container` becomes the grid: `flex_flow="row_wrap"`, `style_pad_row="#space_xs"`, `style_pad_column="#space_xs"`, `style_flex_main_place="space_between"`.

Delete `compact_toggle_row.xml`; in `src/xml_registration.cpp` replace its `register_xml` line with `register_xml("components/option_tile.xml");`; update the census row.

- [ ] **Step 5: Run to verify**

Run: `make t F='[pre_print_options]'` then `make t F='[job_holds_machine]'` (read the tag from the census TEST_CASE if different).
Expected: all pass, including the pre-existing plugin-visibility and label cases.

- [ ] **Step 6: Look at it**

Launch the mock at 800x480 with the U1 option set (set `/printers/default/type` to `Snapmaker U1` in the config dir's `settings-test.json`, or use Task 3's persona if it has landed), open a file, `ctl screenshot`, open the image. Four tiles, two per row, labels wrap to 2 lines, Bed Mesh and Timelapse checked. Tap one with `ctl click option_tile_shaper_calibrate` and confirm the outline, tint and tab appear.

- [ ] **Step 7: Commit**

```bash
git add ui_xml/components/option_tile.xml ui_xml/globals.xml ui_xml/print_file_detail.xml \
  src/xml_registration.cpp include/ui_pre_print_options_renderer.h src/ui/ui_pre_print_options_renderer.cpp \
  tests/unit/test_pre_print_options_renderer.cpp tests/unit/test_job_holds_machine.cpp
git rm ui_xml/components/compact_toggle_row.xml
git commit -m "feat(ui): pre-print options are checkable tiles in a 2-column grid, so long labels wrap instead of running under a switch"
```

Hand mutation: drop `state_trickle="true"`; the children-follow-state test goes red.

---

### Task 3: Snapmaker U1 mock persona

**Files:**
- Modify: `src/application/moonraker_manager.cpp` (both `HELIX_MOCK_PRINTER` blocks)
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` (the `HELIX_MOCK_PRINTER` values row and one example)

**Interfaces:**
- Produces: `HELIX_MOCK_PRINTER=snapmaker_u1` = the multi-extruder mock with saved printer type `Snapmaker U1`, so the four U1 pre-print options appear.

- [ ] **Step 1: Implement.** In the first block, extend the named-type branch the k1 personas use:

```cpp
        if (mock_printer == "k1" || mock_printer == "k1max" || mock_printer == "snapmaker_u1") {
            const std::string named = mock_printer == "k1max"          ? "Creality K1 Max"
                                      : mock_printer == "snapmaker_u1" ? "Snapmaker U1"
                                                                       : "Creality K1C";
```

Update the comment above it to say the persona's printer type is part of what the env var declares, for these three. In the second block add before the `voron_24` fallthrough:

```cpp
            } else if (t == "snapmaker_u1") {
                type = MoonrakerClientMock::PrinterType::MULTI_EXTRUDER;
                type_name = "Snapmaker U1 (multi-extruder mock)";
```

and add `snapmaker_u1` to the "Valid:" warning list and the header comment list.

- [ ] **Step 2: Verify.** `HELIX_MOCK_PRINTER=snapmaker_u1 ./build/bin/helix-screen --test -vv ...`, open a file, `ctl ls | grep option_tile_` lists the four ids (or four switch rows if Task 2 has not landed). Log shows `saved printer type 'Snapmaker U1'`.

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(mock): HELIX_MOCK_PRINTER=snapmaker_u1 persona with the U1's four pre-print options" -- src/application/moonraker_manager.cpp docs/devel/MOCK_ENVIRONMENT_VARIABLES.md
```

---

### Task 4: Options column scrolls above a pinned action block; history into the strip

**Files:**
- Modify: `ui_xml/print_file_detail.xml`
- Modify: `tests/unit/test_print_select_detail_subjects.cpp`

**Interfaces:**
- Consumes: Task 2's tile grid inside `pre_print_options_container`.
- Produces: widget names `detail_options_wrap` (non-flex wrapper, `flex_grow=1`), `detail_options_scroll` (the scroll area), `detail_options_cue` (the fade overlay), `detail_history_wrap` (wrapper around `history_status_row` in the strip). Task 5 finds `detail_options_scroll` and `content_container` by name.

- [ ] **Step 1: Write the failing test** (append to the `print_file_detail.xml structure` section, reusing `make_detail_root`):

```cpp
TEST_CASE_METHOD(LVGLUITestFixture, "History row lives in the metadata strip",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* const row = lv_obj_find_by_name(root, "history_status_row");
    lv_obj_t* const strip = lv_obj_find_by_name(root, "detail_metadata_overlay");
    REQUIRE(row != nullptr);
    REQUIRE(strip != nullptr);
    bool inside = false;
    for (lv_obj_t* p = lv_obj_get_parent(row); p; p = lv_obj_get_parent(p)) {
        inside = inside || p == strip;
    }
    CHECK(inside);
    CHECK(std::string(lv_obj_get_name(lv_obj_get_parent(row))) == "detail_history_wrap");
}

TEST_CASE_METHOD(LVGLUITestFixture, "Print button sits outside the options scroll area",
                 "[print_select][detail][xml]") {
    lv_obj_t* const root = make_detail_root(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_t* const scroll = lv_obj_find_by_name(root, "detail_options_scroll");
    lv_obj_t* const print = lv_obj_find_by_name(root, "print_button");
    REQUIRE(scroll != nullptr);
    REQUIRE(print != nullptr);
    for (lv_obj_t* p = lv_obj_get_parent(print); p; p = lv_obj_get_parent(p)) {
        CHECK(p != scroll);
    }
    CHECK(lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE));
    // The fade must never take touches from the tiles under it.
    lv_obj_t* const cue = lv_obj_find_by_name(root, "detail_options_cue");
    REQUIRE(cue != nullptr);
    CHECK_FALSE(lv_obj_has_flag(cue, LV_OBJ_FLAG_CLICKABLE));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[print_select][detail][xml]'`
Expected: the two new cases fail (names not found); the existing sliced-colors cases pass.

- [ ] **Step 3: Restructure the XML**

`options_section` keeps its name and becomes: 

```
options_section (flex column)
  detail_options_wrap      width=100% flex_grow=1 height=0, NOT flex (plain lv_obj), scrollable=false
    detail_options_scroll  width=100% height=100% flex column, pad_gap=#space_sm,
                           scrollable, scroll_dir="ver", scrollbar_mode="off"
      bypass_source_card   (moved, unchanged)
      sliced_colors_row    (moved, unchanged)
      filament_mapping_card (moved, unchanged)
      options_card         (moved, unchanged; its container is Task 2's grid)
    detail_options_cue     ignore_layout, align=bottom_mid, width=100%, height=#space_xxl (or the
                           nearest token near 40px), bg_grad_dir="ver", bg_color=#card_bg bg_opa=0
                           -> bg_grad_color=#card_bg at full opa, clickable="false";
                           child: icon src="chevron_down" size="sm" variant="secondary",
                           align="bottom_mid", clickable="false".
                           Visibility binding is added in Task 5 (the subject does not exist yet);
                           until then add hidden="true" so nothing changes on screen.
  prep_time_estimate       (moved out of the old wrapper, unchanged)
  button_row               (unchanged)
  print_blocked_reason     (unchanged)
```

Delete the old flex spacer and the old wrapper `lv_obj` around the options card (its comment explains a wrapper that no longer exists). Read `docs/devel/LVGL9_XML_GUIDE.md` for the gradient attribute names this engine accepts and use them exactly.

Move `history_status_row` into `detail_metadata_overlay`, directly after `detail_filename_label`, wrapped in `<lv_obj name="detail_history_wrap" width="100%" height="content" style_pad_all="0" scrollable="false">`. Keep the row's name, children, `hidden="true"` default. Drop its `style_pad_bottom`.

- [ ] **Step 4: Run to verify it passes**

Run: `make t F='[print_select]'`
Expected: all `[print_select]` cases pass, including both sliced-colors cases unchanged.

- [ ] **Step 5: Look at it** at 800x480 and 480x320 with the U1 persona: Print button visible (`ctl geom print_button`: y + h <= screen height), history line under the filename, the scroll area scrolls (`ctl scroll detail_options_scroll ...`, read HELIXCTL.md for the syntax). Screenshot and open both.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(ui): print file detail options scroll above a pinned Delete/Print row, and the print history line sits under the filename" -- ui_xml/print_file_detail.xml tests/unit/test_print_select_detail_subjects.cpp
```

Body line: `ctl geom print_button` before/after at 800x480 (before: y=486).

---

### Task 5: Scroll cue subject and the portrait arrangement

**Files:**
- Modify: `include/ui_print_select_detail_view.h`, `src/ui/ui_print_select_detail_view.cpp`
- Modify: `ui_xml/print_file_detail.xml`
- Modify: `tests/unit/test_print_select_detail_subjects.cpp`

**Interfaces:**
- Consumes: `decide_detail_portrait_preview` (Task 1); names from Task 4.
- Produces: subject `detail_options_more_below` (int, 0/1); `void PrintSelectDetailView::update_options_more_below();` `void PrintSelectDetailView::fit_portrait_preview();`

- [ ] **Step 1: Write the failing test** (append; build through the real view like the "Sliced colors row is shown only while..." case, same callback registrations):

```cpp
TEST_CASE_METHOD(LVGLUITestFixture, "More-below subject tracks the options scroll area",
                 "[print_select][detail][xml]") {
    register_xml_callbacks({
        {"on_print_select_detail_backdrop", detail_noop_cb},
        {"on_print_select_print_button", detail_noop_cb},
        {"on_print_select_delete_button", detail_noop_cb},
        {"on_print_detail_back_clicked", detail_noop_cb},
        {"on_toggle_sliced_colors", detail_noop_cb},
    });
    helix::ui::PrintSelectDetailView view;
    view.init_subjects();
    lv_obj_t* const root = view.create(test_screen());
    REQUIRE(root != nullptr);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* const scroll = lv_obj_find_by_name(root, "detail_options_scroll");
    lv_subject_t* const more = lv_xml_get_subject(nullptr, "detail_options_more_below");
    REQUIRE(scroll != nullptr);
    REQUIRE(more != nullptr);

    // Force an overflow: a tall child in the scroll area.
    lv_obj_t* filler = lv_obj_create(scroll);
    lv_obj_set_size(filler, 10, 4000);
    lv_obj_update_layout(root);
    lv_obj_send_event(scroll, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 1);

    lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 0);

    lv_obj_delete(filler);
    lv_obj_update_layout(root);
    lv_obj_send_event(scroll, LV_EVENT_SIZE_CHANGED, nullptr);
    process_lvgl(20);
    CHECK(lv_subject_get_int(more) == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make t F='[print_select][detail][xml]'`
Expected: fails, `detail_options_more_below` subject is null.

- [ ] **Step 3: Subject and scroll wiring**

In `init_subjects()` next to the other `UI_MANAGED_SUBJECT_INT` lines:

```cpp
    UI_MANAGED_SUBJECT_INT(detail_options_more_below_, 0, "detail_options_more_below", subjects_);
```

Declare `lv_subject_t detail_options_more_below_{};` beside `detail_prefer_sliced_colors_` in the header. In `create()`, after the `pre_print_options_container` lookup:

```cpp
    options_scroll_ = lv_obj_find_by_name(overlay_root_, "detail_options_scroll");
    if (options_scroll_) {
        // DECLARATIVE_OK: scroll and size events have no XML trigger; the handler
        // only publishes a subject, the cue's visibility is bound in XML.
        for (lv_event_code_t code : {LV_EVENT_SCROLL, LV_EVENT_SCROLL_END, LV_EVENT_SIZE_CHANGED}) {
            lv_obj_add_event_cb(
                options_scroll_,
                [](lv_event_t* e) {
                    static_cast<PrintSelectDetailView*>(lv_event_get_user_data(e))
                        ->update_options_more_below();
                },
                code, this);
        }
    }
```

```cpp
void PrintSelectDetailView::update_options_more_below() {
    if (!options_scroll_) {
        return;
    }
    const int more = lv_obj_get_scroll_bottom(options_scroll_) > 0 ? 1 : 0;
    if (lv_subject_get_int(&detail_options_more_below_) != more) {
        lv_subject_set_int(&detail_options_more_below_, more);
    }
}
```

Null `options_scroll_` wherever `history_status_row_` is nulled (the teardown at the `history_status_row_ = nullptr;` site). Call `update_options_more_below()` at the end of `populate_option_rows()` after `lv_obj_update_layout(options_scroll_)` (Review Focus 2). Check how this file registers raw event callbacks elsewhere (search `lv_obj_add_event_cb(` in it) and match that form, including any lifetime guard it uses; if the file routes through a helper, use the helper.

In the XML, replace the cue's `hidden="true"` with:

```xml
<bind_flag_if cond="detail_options_more_below eq 1 and settings_page_scroll_buttons eq 0"
              flag="hidden" invert="true"/>
```

- [ ] **Step 4: Portrait restyle (XML)**

Add to `<styles>`:

```xml
    <!-- Landscape: preview column left, options column right. Portrait: the
         preview block on top, options below, both full width. Paired so each
         orientation is always covered; inline attributes would win over them. -->
    <style name="content_row" flex_flow="row"/>
    <style name="content_column" flex_flow="column"/>
    <style name="left_landscape" height="100%" flex_grow="5" width="0"/>
    <style name="left_portrait" height="content" flex_grow="0" width="100%"/>
    <style name="options_landscape" height="100%" flex_grow="4" width="0"/>
    <style name="options_portrait" height="0" flex_grow="1" width="100%"/>
    <style name="card_landscape" flex_grow="1"/>
    <style name="card_portrait" flex_grow="0"/>
```

Remove `flex_flow` from `content_container`, `height`/`flex_grow` from `left_column` and `options_section`, `flex_grow` from `detail_card`, and bind each pair on `ui_is_portrait` (`bind_style_if_eq ... ref_value="0"` / `"1"`). Keep every other attribute.

Micro portrait metadata (spec: row 1 only). On `metadata_row_2`, `metadata_row_3` and `detail_history_wrap`:

```xml
<bind_flag_if cond="ui_breakpoint eq 0 and ui_is_portrait eq 1" flag="hidden"/>
```

- [ ] **Step 5: Portrait preview height (C++)**

Register `LV_EVENT_LAYOUT_CHANGED` on `content_container` (same form as Step 3) calling `fit_portrait_preview()`:

```cpp
void PrintSelectDetailView::fit_portrait_preview() {
    lv_obj_t* card = overlay_root_ ? lv_obj_find_by_name(overlay_root_, "detail_card") : nullptr;
    if (!card || !options_scroll_) {
        return;
    }
    if (lv_subject_get_int(theme_manager_get_is_portrait_subject()) == 0) {
        // Landscape: the card fills its column by flex; drop any portrait height.
        lv_obj_remove_local_style_prop(card, LV_STYLE_HEIGHT, 0);
        return;
    }
    const int width = lv_obj_get_width(card);
    const int avail = lv_obj_get_height(card) + lv_obj_get_height(options_scroll_);
    const int content = lv_obj_get_scroll_y(options_scroll_) +
                        lv_obj_get_height(options_scroll_) +
                        lv_obj_get_scroll_bottom(options_scroll_);

    // The tile grid's top, measured inside the scroll area's content.
    lv_obj_t* grid = pre_print_options_container_;
    int grid_top = lv_obj_get_scroll_y(options_scroll_);
    for (lv_obj_t* o = grid; o && o != options_scroll_; o = lv_obj_get_parent(o)) {
        grid_top += lv_obj_get_y(o); // relative to its parent, so the chain sums
    }
    lv_obj_t* first_tile = grid ? lv_obj_get_child(grid, 0) : nullptr;
    const int tile_h = first_tile ? lv_obj_get_height(first_tile) : 0;
    const int gap = grid ? lv_obj_get_style_pad_row(grid, LV_PART_MAIN) : 0;

    const int h =
        helix::ui::decide_detail_portrait_preview(width, avail, content, grid_top, tile_h, gap);
    if (lv_obj_get_height(card) != h) {
        // DECLARATIVE_OK: measured layout; the height depends on runtime pixel sizes.
        lv_obj_set_height(card, h);
    }
}
```

`lv_obj_get_y` is relative to the parent in LVGL 9 (check `lib/lvgl/src/core/lv_obj_pos.c`; if it includes the parent's scroll offset, subtract each parent's `lv_obj_get_scroll_y`). Resolve the portrait subject accessor's real name from `include/theme_manager.h` (search `is_portrait`). A tiles-free option set gives `tile_h == 0`; the function then returns the base height. Setting the height triggers another LAYOUT_CHANGED; the `!= h` check ends it. Confirm with `-vvv` trace logging that it settles in one extra pass.

- [ ] **Step 6: Run tests**

Run: `make t F='[print_select]'` and `make t F='[print_detail_layout]'`
Expected: all pass.

- [ ] **Step 7: Look at it** with `HELIX_MOCK_PRINTER=snapmaker_u1` at 480x800, 272x480 and 600x1024 (portrait) and 800x480 (landscape unchanged from Task 4). Screenshot each and open them. Portrait: preview on top at ~16:10, tiles below, pinned Print row, fade + chevron only where content is hidden; at 272x480 the strip shows filename + one stats line. Resize across the portrait boundary is not reachable with `-s`; skip it here, Task 6's test covers the restyle.

- [ ] **Step 8: Commit**

```bash
git commit -m "feat(ui): print file detail has a portrait arrangement and a fade cue when options scroll" -- include/ui_print_select_detail_view.h src/ui/ui_print_select_detail_view.cpp ui_xml/print_file_detail.xml tests/unit/test_print_select_detail_subjects.cpp
```

Hand mutation: make `update_options_more_below` always publish 1; the scroll-to-end CHECK goes red.

---

### Task 6: Geometry test at five canvases, docs, verification sweep

**Files:**
- Create: `tests/unit/test_print_detail_geometry.cpp`
- Modify: `docs/devel/UI_CONTRIBUTOR_GUIDE.md` (one paragraph: the print file detail view is one tree restyled for portrait; name the pattern so the next panel copies it)
- Delete (last commit of the branch, when it merges): this plan and the spec. `docs/devel/plans/2026-09-25-small-screen-layout.md` stays while its other phases are open.

- [ ] **Step 1: Write the geometry test.** Build the real view (as in Task 5's test) with the U1 option set injected into the printer state the view reads (`printer_state_->get_pre_print_option_set()`: find how `test_print_select_detail_subjects.cpp` or the renderer tests obtain a `PrinterState` with an option set; `PrinterDetector::get_pre_print_option_set("Snapmaker U1")` returns the DB set). For each canvas:

```cpp
struct Canvas { int w; int h; };
const Canvas canvases[] = {{800, 480}, {480, 320}, {480, 272}, {480, 800}, {272, 480}};
for (const auto& c : canvases) {
    INFO(c.w << "x" << c.h);
    ScopedResolution res(lv_display_get_default(), c.w, c.h);
    theme_manager_refresh_layout_constants(); // tier + ui_is_portrait follow the pixels
    // build view, show root, lv_obj_update_layout(root), process_lvgl(50)
    lv_area_t a;
    for (const char* name : {"print_button", "delete_button"}) {
        lv_obj_get_coords(lv_obj_find_by_name(root, name), &a);
        CHECK(a.x1 >= 0);
        CHECK(a.y1 >= 0);
        CHECK(a.x2 < c.w);
        CHECK(a.y2 < c.h);
    }
    // every tile label inside its tile
    // (walk pre_print_options_container children; label coords within tile coords)
    // more-below == (scroll_bottom > 0)
}
```

Read `tests/lvgl_test_fixture.h` (`ScopedResolution`, `reclaim_display`) for the exact scope rules; build a fresh view per canvas and destroy it before the scope closes. If `theme_manager_refresh_layout_constants` is named differently, use the real name from the `ScopedResolution` doc comment. Also run one canvas (800x480) with an EMPTY option set and check the Print button and that `options_card` is hidden (Review Focus 3).

- [ ] **Step 2: Prove it can fail.** Temporarily put `scrollable="false"` back on `detail_options_scroll` and move `button_row` inside it; run `make t F='[print_detail_geometry]'`; expect red at 800x480; restore.

- [ ] **Step 3: Run.** `make t F='[print_detail_geometry]'` green, then `make unit-sweep`.

- [ ] **Step 4: Verification sweep by hand.** With `HELIX_MOCK_PRINTER=snapmaker_u1`, for 480x272, 480x320, 800x480, 1024x600, 1280x720, 480x800, 272x480: `ctl geom print_button`, `ctl geom detail_options_scroll`, screenshot, open each image. Write the table (size, Print top/bottom, cue shown, notes) into the report. Before-numbers are in the spec's Problem table.

- [ ] **Step 5: Docs + commit**

```bash
git add tests/unit/test_print_detail_geometry.cpp docs/devel/UI_CONTRIBUTOR_GUIDE.md
git commit -m "test(ui): print file detail keeps Delete and Print on screen and tile labels inside tiles at five canvases"
```

- [ ] **Step 6: Gate.** `make full-test-run` green before review.
