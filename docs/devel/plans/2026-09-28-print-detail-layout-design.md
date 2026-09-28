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
- **Portrait:** a bound style turns `content_container` into a column. The left column becomes
  the top block at full width: header, then the preview card at a measured height (below).
  The options column fills the rest at full width.

### Options column (every size)

Top to bottom:

1. **Scroll area**, `flex_grow=1`, scrollable vertically, scrollbar hidden:
   - Filaments card (unchanged content), header gains the Sliced colors switch.
   - Print options card: header label + option tile grid.
2. **Scroll cue** overlaid on the bottom of the scroll area (see Scroll cue).
3. Prep time estimate (`prep_time_estimate`).
4. Pinned action row: Delete (square, `#button_height_lg`) + Print (`flex_grow=1`).
5. Blocked reason (`print_blocked_reason`).

Items 3-5 never scroll. `history_status_row` and `sliced_colors_row` leave this column.

### Moved elements

- **History** ("Printed 1 time" / "Last print cancelled"): a line in the metadata strip under
  the filename, at every size. Same widgets and names (`history_status_icon`,
  `history_status_label`), same C++ writers.
- **Sliced colors:** a label plus `ui_switch size="small"` in the Filaments card header,
  between the lamps and the chevron, at every size. Same subject
  (`detail_prefer_sliced_colors`), callback (`on_toggle_sliced_colors`) and visibility
  condition (`detail_gcode_viewer_mode eq 1 and detail_viewer_first_frame eq 1`). The switch
  and its label must not bubble clicks to the card (a toggle must not open the remap picker).
  It is now also hidden whenever the Filaments card is hidden. Accepted:
  `PrintSelectDetailView::apply_preview_colors` only switches between default mappings and
  `effective_mappings()`, which differ only when there is a lane mapping to show; queue mode
  (card hidden deliberately) loses the switch.

### Metadata strip at micro portrait

At `ui_breakpoint eq 0 and ui_is_portrait eq 1` (272x480) the strip shows the filename and ONE
stats line: print time, filament weight, layer count. Height, layer height, filament type and
history are hidden at that size. Every other size keeps the full strip.

### Portrait preview height

Portrait only; landscape keeps `flex_grow` in its column. A measured-layout function (the
`src/ui/ui_panel_filament.cpp#fit_portrait_graph` shape: `SIZE_CHANGED` on the content
container) sets the preview card height from a pure decision function:

```
int decide_detail_portrait_preview(int width, int avail_h, int content_h,
                                   int row_pitch, int min_h);
```

- `width`: preview card width. `avail_h`: height the preview and the scroll area share.
  `content_h`: the scroll area's full content height. `row_pitch`: tile height + grid gap.
- Base height = `width * 10 / 16`.
- If `content_h <= avail_h - base`, return base (everything fits, no cue).
- Otherwise the scroll area overflows. Where the visible edge lands inside a tile row, it is
  returned untouched. Where it lands within the grid gap or the outer quarter of a row, shrink
  the preview by just enough (at most half a `row_pitch`) that the edge crosses the middle half
  of a tile.
- Never return less than `min_h`.

Lives in a header with no LVGL dependency so the rule is unit-tested without a display.

## Option tiles

New `ui_xml/components/option_tile.xml`, 2-column grid (`flex_flow="row_wrap"`, each tile
`width` just under 50% of the grid, fixed height per breakpoint tier), at every size.
`compact_toggle_row` has no other user and is deleted with its registration in
`src/xml_registration.cpp`.

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
- **Icons:** the option's own `PrePrintOption::icon` wins when set (verify the form the parser
  stores; the header documents it as a codepoint string). Otherwise a default from the option
  id, as a pure function beside `PrePrintOptionsRenderer::label_key_for`:

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
  `chevron_down` icon at its bottom centre. Not clickable, so touches reach the content under
  it.
- Visibility binds to a new int subject `detail_options_more_below` owned by
  `PrintSelectDetailView`: 1 while `lv_obj_get_scroll_bottom(scroll_area) > 0`. C++ updates it
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
- Default icon: each id in the table, the fallback, and an option's own icon winning.
- `test_pre_print_options_renderer.cpp` moves from switch to tile: a tap toggles the option
  subject, subject changes reach the checked state, plugin-gated visibility still hides a tile.
- New geometry test, style of `tests/unit/test_widget_size_print_status.cpp`, U1 option set, at
  800x480, 480x320, 480x272, 480x800 and 272x480: the Print and Delete buttons lie fully inside
  the screen; each tile's label lies inside its tile; `detail_options_more_below` is 1 exactly
  when the scroll area overflows.
- By hand: `ctl geom` numbers and screenshots, read, at all seven canvases in the Problem
  table, before and after, landscape and portrait.
