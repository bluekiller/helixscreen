# Print file detail: option tiles, pinned actions, portrait

Design spec. Replaces Phase 5 of `2026-09-25-small-screen-layout.md`. Delete this file in the
change that ships the work (docs/CLAUDE.md convention).

Mockups (approved 2026-09-28, hand-drawn HTML, proportions approximate):
`/tmp/u1-brainstorm/.superpowers/brainstorm/1644569-1790625859/content/detail-layout-v2.html`
(layout and scroll cue) and `micro-meta.html` (variant A). Tile style: `option-tiles.html`
choice C in `/tmp/u1-brainstorm/.superpowers/brainstorm/1980085-1790305260/content/`.

## Problem

`ui_xml/print_file_detail.xml` puts pre-print options in `compact_toggle_row`s inside a
right column that is `height="100%"` and not scrollable. Measured with the Snapmaker U1's four
options (`bed_mesh`, `shaper_calibrate`, `flow_calibrate`, `u1_timelapse`):

| Canvas | Print button top | Other defects |
|---|---|---|
| 480x272 | y=288, off screen | labels run under their switches |
| 480x320 | y=330, off screen | same |
| 800x480 | y=486, off screen | same |
| 1024x600 | y=484, fits | same |
| 1280x720 | y=584, fits | none |
| 480x800 | visible, "Print" clipped | landscape two-column layout squeezed to 480 wide; labels half hidden; 112px trash button |
| 272x480 | 29px wide | same squeeze, worse |

The label has no width bound, so a long label draws under the switch. Nothing below the
options can be reached once they overflow.

## Goal

At every breakpoint, landscape and portrait: every option label is readable and never
overlaps a control, Delete and Print are always fully on screen, and content that does not fit
scrolls with a visible cue.

## Layout

### One tree, restyled for portrait

`print_file_detail.xml` stays ONE widget tree. Portrait changes styles bound to
`ui_is_portrait`, never the tree: no `<if cond="ui_is_portrait">` and no `ui_xml/portrait/`
variant. A reactive `<if>` tears down and rebuilds the subtree on rotation, and
`PrintSelectDetailView` holds pointers into it (13 `lv_obj_find_by_name` sites, the G-code
viewer, the options container, the filament mapping card); bed mesh needed a dedicated rewire
path (`src/ui/ui_panel_bed_mesh.cpp#setup_orientation_rewire_observer`) to survive the same
rebuild.

- **Landscape** (unchanged arrangement): `content_container` is a row, left column 5/9
  (header + preview card), right column 4/9 (the options column below).
- **Portrait:** `content_container` becomes a column. The left column becomes the top block at
  full width: header, then the preview card at a measured height (below). The options column
  fills the rest at full width.
- **Mechanism:** the inline `flex_flow`, `flex_grow`, `width` and `height` attributes on
  `content_container`, `left_column` and `options_section` are removed, and each becomes a
  complementary `bind_style_if_eq` pair on `ui_is_portrait` (ref 0 and ref 1), the
  `mapping_rows_standard` / `mapping_rows_tall` pattern already in this file. An inline
  attribute beside a bound style wins over it (declarative-ui rule 6), so a single bound style
  over the old attributes does nothing. The style engine accepts all four properties in a
  named style.

### Options column (every size)

Top to bottom:

1. **Scroll area**, `flex_grow=1`, scrollable vertically, scrollbar hidden, in this order:
   - `bypass_source_card` (unchanged; its "the lanes below" copy still holds).
   - `sliced_colors_row` (unchanged: same name, label, switch, bindings).
   - Filaments card (unchanged).
   - Print options card: header label + option tile grid.
2. **Scroll cue** overlaid on the bottom of the scroll area (see Scroll cue).
3. Prep time estimate (`prep_time_estimate`).
4. Pinned action row: Delete (square, `#button_height_lg`) + Print (`flex_grow=1`).
5. Blocked reason (`print_blocked_reason`).

Items 3-5 never scroll. `history_status_row` leaves this column.

### Moved elements

- **History** ("Printed 1 time" / "Last print cancelled"): `history_status_row` moves, by name
  and with its children, into the metadata strip under the filename, at every size.
  `update_history_display` finds it by name and owns its `hidden` flag, so it is unchanged. The
  row sits inside a new wrapper, `detail_history_wrap`; any size-driven hide targets the
  wrapper, never the row, so the C++ writer and the breakpoint binding never share a flag.
- **Sliced colors stays its own row**, now at the top of the scroll area. It does NOT move into
  the Filaments header: at 272x480 that header cannot carry the lamps, the chevron and a
  labelled switch, which `tests/unit/test_print_select_detail_subjects.cpp` ("Sliced colors
  toggle sits outside the filament card") pins. Keeping the row also keeps its name, its
  visibility condition and the test on `sliced_colors_row`, and a tap on its label never
  reaches the card's remap handler.

### Metadata strip at micro portrait

At `ui_breakpoint eq 0 and ui_is_portrait eq 1` (272x480) the strip shows the filename and ONE
stats line, `metadata_row_1` (print time, filament weight). `metadata_row_2`,
`metadata_row_3` and `detail_history_wrap` are hidden at that size, each by one
`bind_flag_if` on the container. Layer count drops with row 2; the approved mockup showed it on
the stats line, but composing it there would mean a second label bound to the same subject for
one size. Every other size keeps the full strip.

### Portrait preview height

Portrait only; landscape keeps `flex_grow` in its column. A measured-layout function (the
`src/ui/ui_panel_filament.cpp#fit_portrait_graph` shape: `LV_EVENT_LAYOUT_CHANGED` on the
content container, a structural exception) sets the preview card height from a pure decision
function:

```
int decide_detail_portrait_preview(int width, int avail_h, int content_h,
                                   int row_pitch, int min_h);
```

- `width`: preview card width. `avail_h`: height the preview and the scroll area share.
  `content_h`: the scroll area's full content height. `row_pitch`: tile height + grid gap.
  The caller also knows where the tile grid starts inside the scroll content; pass it as
  `grid_top` (add the parameter) so the rule can tell a tile edge from space above the grid.
- `min_h = width / 3`: below that the preview stops reading as a model.
- Base height = `width * 10 / 16`.
- If `content_h <= avail_h - base`, return base (everything fits, no cue).
- Otherwise the scroll area overflows. If the visible edge lands above `grid_top`, return base
  (the cue alone carries it). If it lands in the middle half of a tile, return base. If it lands
  in a grid gap or in either outer quarter of a tile, shrink the preview by the smallest amount
  that puts the edge in the middle half of a tile. That amount is at most `gap + tile / 4`,
  which is under half a `row_pitch` at every tier because the grid gap is a spacing token far
  smaller than a tile.
- Never return less than `min_h`.

Lives in a header with no LVGL dependency so the rule is unit-tested without a display.

## Option tiles

New `ui_xml/components/option_tile.xml`, 2-column grid (`flex_flow="row_wrap"`, each tile
`width` just under 50% of the grid, fixed height per breakpoint tier), at every size.
`compact_toggle_row` has no other user and is deleted with its registration in
`src/xml_registration.cpp` and its row in `kNoMachineControlFiles` in
`tests/unit/test_job_holds_machine.cpp` (the census fails a listed path that no longer exists).
`option_tile.xml` takes a `callback` prop wired to an `<event_cb>` the way its predecessor did,
so it gets that census row instead.

- **Look (approved style C):** icon + label; label wraps to 2 lines, then ellipsis; 1px
  `#border` outline on `#card_bg`. Checked: `#primary` outline, icon tinted `#primary`, a small
  `#primary` check tab in the top-right corner. Tokens only, no literal colors.
- **Behaviour:** the whole tile is the tap target. It is a checkable object: a tap toggles
  `LV_STATE_CHECKED` and fires `value_changed`, the event `compact_toggle_row`'s switch fires
  today. Checked-state styles draw the outline and tint. The check tab is a child, and LVGL
  styles a child by its own state, not its parent's, so the renderer binds the tab's `hidden`
  flag to the option subject next to the checked-state binding it installs on the tile.
- **Wiring:** `PrePrintOptionsRenderer` (`src/ui/ui_pre_print_options_renderer.cpp`) creates
  `option_tile` where it creates `compact_toggle_row` now, keeps every per-option subject,
  observer, visibility binding and callback, and finds the tile itself instead of a `toggle`
  child. The option state provider for `PrintPreparationManager` is unchanged.
- **Icons:** from the option id, as a pure function beside
  `PrePrintOptionsRenderer::label_key_for`. `PrePrintOption::icon` is left alone and not read:
  no printer database entry sets it and its stored form (documented as a codepoint string) does
  not match the icon names below; wire it when a printer ships one.

  | id | icon |
  |---|---|
  | `bed_mesh` | `grid_large` |
  | `shaper_calibrate` | `sine_wave` |
  | `flow_calibrate` | `gauge` |
  | `timelapse`, `u1_timelapse` | `camera_timer` |
  | `ai_detect` | `robot` |
  | `qgl`, `z_tilt` | `spirit_level` |
  | anything else | `tune` |

  All exist in `include/ui_icon_codepoints.h`; no font regeneration.

## Scroll cue

Shown only while content is hidden below the scroll area's visible bottom:

- A fade from transparent to the column background over the bottom of the scroll area, with a
  `chevron_down` icon at its bottom centre. The scroll area and the cue share a wrapper; the
  cue is `ignore_layout` + `align="bottom_mid"`, so it overlays instead of taking a flex slot,
  and it is not a child of the scroll area, so it does not scroll away. It must never be
  `clickable`: touches reach the content under it only because hit-testing skips
  non-clickable objects.
- Visibility: `detail_options_more_below eq 1 and settings_page_scroll_buttons eq 0`.
  `detail_options_more_below` is a new int subject owned by `PrintSelectDetailView`, 1 while
  `lv_obj_get_scroll_bottom(scroll_area) > 0`. Where page-scroll buttons are on (default on
  ESP32), `PageScrollAutoInject` gives the overflowing scroll area its chevron gutter, and that
  gutter is the cue; the fade would only duplicate it. C++ updates it
  on the scroll area's `SCROLL`, `SCROLL_END` and `SIZE_CHANGED` events (scroll and size events
  are a structural exception in `.claude/rules/declarative-ui.md`) and after the option rows are
  populated.
- The portrait preview nudge above makes the cut land inside a tile, so part of a tile shows
  under the fade. Landscape has no free height to nudge; the fade and chevron carry it alone.

## Mock persona

`HELIX_MOCK_PRINTER=snapmaker_u1` in `src/application/moonraker_manager.cpp`, saving printer
type "Snapmaker U1" the way the `k1` persona names its type, so the four-option case is one
env var. Document it in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

## Testing

- `decide_detail_portrait_preview`: fits (base returned); overflow with the edge in a gap
  (shrunk, edge now mid-tile); overflow with the edge already mid-tile (base returned); nudge
  capped at half a row; `min_h` floor.
- Default icon: each id in the table and the fallback.
- `test_pre_print_options_renderer.cpp` moves from switch to tile: a tap toggles the option
  subject, subject changes reach the checked state, plugin-gated visibility still hides a tile.
- `test_print_select_detail_subjects.cpp`: the sliced-colors cases stay green unchanged (the row
  keeps its name and stays outside the card); add a case that `history_status_row` is inside
  the metadata strip.
- New geometry test forcing the canvas and `ui_is_portrait` the way
  `tests/unit/test_overlay_height_portrait.cpp` does, U1 option set, at 800x480, 480x320,
  480x272, 480x800 and 272x480: the Print and Delete buttons lie fully inside
  the screen; each tile's label lies inside its tile; `detail_options_more_below` is 1 exactly
  when the scroll area overflows.
- By hand: `ctl geom` numbers and screenshots, read, at all seven canvases in the Problem
  table, before and after, landscape and portrait.
