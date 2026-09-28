# Small-screen layout pass (480x320 and smaller)

Branch `feature/small-screen-layout`, worktree `.worktrees/small-screen-layout`. Driven by the
Snapmaker U1 (480x320, breakpoint `tiny`). Main first; 1.0 takes the pieces that lift cleanly.
Delete this file in the change that ships the last phase (docs/CLAUDE.md convention).

Visual spec: the approved mockups in `/tmp/u1-brainstorm/.superpowers/brainstorm/*/content/`
(`heater-card.html` options A/A, `print-detail.html` A, `option-tiles.html` C,
`status-and-temp.html`). Theme colors there are the HelixScreen dark palette
(`assets/config/themes/defaults/helixscreen.json`); in XML use tokens (`#primary`, `#warning`,
`#success`, `#border`, `#card_bg`, `#elevated_bg`...), never literal colors.

"Tiny and smaller" = `ui_breakpoint` < 2 (Micro=0, Tiny=1; `include/ui_breakpoint.h`). Larger
breakpoints keep today's layout unless a phase says otherwise.

## Phase 1: tool subscript replaces the overlaid badge (main + 1.0)

`ui_xml/components/nozzle_icon.xml#tool_badge` is a disc overlaid on the icon's top-right; at
small sizes it covers the nozzle glyph. Every call site goes through this component
(temp_card_unified, temp_graph_overlay, panel_widget_temperature, panel_widget_preheat,
panel_widget_temp_stack, print_status_detailed_active, controls_panel, micro/controls_panel), so
fix it HERE, once. (A capsule/chip layout was tried first and dropped: it read as a button.)

- When the badge subject is 1: render a bold subscript digit right of the glyph, bottom-aligned
  with the glyph's lower edge (like the 1 in T1), in `#secondary`, ~1px (`#space_xxs`) gap. No
  border, no background, no chrome — the digit never overlaps the glyph.
- Digit is the 1-based number ALONE, from `tool_badge_text` (`lane_number_text()`); display
  contexts never spell "T<n>" (that is the 0-based G-code form — see `include/display_numbering.h`).
- Font: `badge_font` prop, default `#font_body_bold`. noto_sans_bold ships nothing below 14px,
  so at micro/tiny/small the digit clamps to that floor (the smallest bold text size the app
  uses) rather than shrinking toward half the glyph; verify legibility at tiny AND micro.
- When the badge subject is 0 or empty: exactly today's bare icon (single-tool printers must
  look unchanged).
- `nozzle_icon_glyph` keeps its name and its HeaterIconBinder coloring: the icon still lights up
  (heating/at-temp colors) beside the digit, as it does today.
- Check every call site at tiny AND micro AND medium/large with `ctl geom` + screenshots. The
  home print widget's "Tool 1 ▾" picker (`print_status_nozzle_tool_picker.xml`) sits beside a
  nozzle icon that stays bare (`badge_subject=""`): the label carries the number once.
- The temp overlay card: verify whether the tool picker selection and the card's displayed temp
  can disagree (U1 capture: picker "1" selected, card showed 260°C while graph had nozzle 1 at
  ~55°C and nozzle 3 at 260°C). If real, fix so the card shows the picked tool and the digit
  matches it (the overlay pins the picked extruder while open, via
  `PrinterTemperatureState::pin_active_extruder`).

## Phase 2: heater status = glyph + duty (main only)

`src/ui/ui_temperature_utils.cpp#heater_display` / `#status_with_duty` produce
"Heating... · 100%", which overflows the print status heater rows at tiny. Duty display exists
on main only.

- Replace the status word with a small glyph + duty %: flame (`#warning`) while heating,
  check (`#success`) at target, nothing when off; cooling keeps a glyph or short word (pick one
  consistent treatment, say which). Duty % text muted, omitted at 0 as today.
- Chamber row uses the same format; this removes "Heating..." and the doubled
  "Heating · Ready · 100%" seen on the chamber overlay card (`#chamber_status_text`). Check
  every consumer of these helpers (print status panel, temp overlay card, controls panel,
  temperature_service) and keep the decision in one shared function returning a
  classification; each surface maps it to its own rendering.
- Temp value color keeps meaning heating vs at-target.
- Unit tests for the classification (heating/ready/off/cooling x duty 0/partial/100).

## Phase 3: print status buttons (main + 1.0)

`ui_xml/print_status_panel.xml` buttons use `#button_height` (32px at tiny) and leave ~70px dead
under the fan row on a U1; at 57px wide "Cancel"/"Camera" labels clip.

- At tiny and smaller: the button grid takes the remaining column height (flex_grow), buttons
  stack icon over label, labels visible and unclipped. Larger breakpoints unchanged.
- Check the portrait variant is unaffected.

## Phase 4: temperature overlay split (main + 1.0)

`ui_xml/temp_graph_overlay.xml`: landscape 66/33 (`tg_graph_two_col` / `tg_strip_landscape`).

- At tiny and smaller: ~62/38. Tool picker (up to 4+) gets real gaps; preset grid grows to fill
  the dead band under the picker. Graph-only and portrait modes unchanged.

## Phase 5: print detail layout (main; 1.0 if it lifts)

Superseded by `2026-09-28-print-detail-layout-design.md`: option tiles and a pinned action row
at every size, plus portrait, not just tiny.

## Phase 6: home screen at tiny (main + 1.0 where applicable)

Found on the U1 and the mock at 480x320:
- Notifications widget: the red "!" alert badge covers most of the bell icon.
- A home cell renders a crossed-out "unavailable" icon ON TOP of a "Configure" label, which also
  wraps "Configur / e" (U1 idle home, right-middle column). Find which widget and why two states
  draw at once.
- `panel_widget_nozzle_temps`: rows read "47.0°off" (unit/state run together).
- Home print widget (`panel_widget_print_status` / `print_status_detailed_active.xml`) picks up
  phase 1's subscript digit; confirm it at tiny during heating (`HELIX_MOCK_AUTO_PRINT=1 --sim-speed 1`).

## Coordination

- `feature/print-queue` (session helixscreen-1d) merged to main 2026-09-25 before this branch
  was cut: `up_next_row` in `print_status_extras` of `ui_xml/print_status_panel.xml` and the
  portrait variant, and in `print_card_printing` of `panel_widget_print_status.xml`;
  `print_status_detailed_active.xml` root is now `flex_grow="1"`. The geometry test
  `tests/unit/test_widget_size_print_status.cpp` "up next row stays inside the active print card"
  pins it at 800x480 and 480x320: phases 1, 3 and 6 must keep it green.
- Someone else (owner unknown, not helixscreen-1d) committed fb9b708d2 (home icon-only tiles at
  480x320) and has an uncommitted `ui_xml/controls_panel.xml` in the main tree. Re-check
  `git log main` for home/tile work before phase 6.

## Verification per phase

- `ctl geom` numbers at 480x320 (and 800x480 for "unchanged elsewhere") + screenshots actually
  read, before/after, in the commit body in one line.
- XML is hot-reloaded; C++ needs `make t F=` for tests and `make -j` for the app.
- One commit per phase on the branch; independent review before merge.
- Mutation: ONE named hand mutation per commit on its most load-bearing test, at the end.
  Never `make mutate-diff`. XML-only phases need none (geom + screenshots are the proof).
  zeus (`scripts/zeus-run.sh mutate`) only for new logic nothing has mutation-tested, after
  pushing, in parallel with review.
