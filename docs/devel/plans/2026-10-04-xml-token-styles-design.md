# XML token styles: helix-xml remembers inline token colors

## Problem

An inline style whose value is a token (`style_text_color="#warning"`) is resolved to a hex
string in `lib/helix-xml/src/xml/lv_xml.c#resolve_consts` before any widget's apply runs, so
the engine forgets which token the author meant. Two consequences:

- A live dark/light switch cannot re-resolve it. The app paints over local styles with
  heuristics (`theme_apply_palette_to_widget`: font-based label colors, bg/border swap maps),
  which overwrite what the author wrote.
- The app cannot tell an authored color from one it may repaint. `text_*` labels carry an
  interim flag (`helix::ui::AUTHORED_TEXT_COLOR_FLAG`, #1735); plain `lv_label` and every
  other widget have nothing.

Since b29be1a56 the color consts themselves follow a live switch, so re-resolving by name
gives the right value.

## Design

### Engine (lib/helix-xml)

1. **Keep the name.** `resolve_consts` records, per attribute slot it rewrote from `#name`,
   the const name, in a side table on the parser state (cleared per element). Only slots
   whose attribute name starts with `style_` need it.
2. **Record on the object.** `apply_styles` in `parsers/lv_xml_obj_parser.c`, for a COLOR
   prop applied from a token, appends `{prop (lv_style_prop_t), selector, const name, last
   applied color}` to a per-object record list. The list is the user_data of an
   `LV_EVENT_DELETE` callback on that object (the `lv_xml_bind_compose.c#group_find`
   pattern): found by scanning the object's event descriptors, freed with the object, no
   global registry. A later inline write of the same prop+selector replaces the entry.
3. **Query.** `bool lv_xml_obj_has_token_style(obj, prop, selector)`.
4. **Re-apply.** `void lv_xml_reapply_token_styles(lv_obj_t * root)` walks the tree; for each
   record it re-resolves the const and sets the local style, **only if the object's current
   local value still equals the record's last-applied color**. A value C++ changed since
   (an error-red label, a selected-row highlight) is left alone. The app calls this once
   after a theme switch, over every screen and the top/sys layers.

Scope v1: color props only (`text_color`, `bg_color`, `border_color`, `bg_grad_color`,
`outline_color`, `shadow_color`, `image_recolor`, `line_color`, `arc_color`, `recolor`),
which is about 930 attributes in `ui_xml/`. Not covered:
- tokens passed through a component `$prop` (about 9 style sites), whose name is resolved at
  the instance tag
- named `<style>` definitions, which resolve at registration
- C-supplied attrs that bypass `resolve_consts`

### App (after GOD-6 lands; the walker moves to `src/ui/theme_live_recolor.cpp`)

- The walker skips any prop+selector the engine reports as a token style, instead of
  checking `AUTHORED_TEXT_COLOR_FLAG`. The flag and its set in `ui_text_apply` are deleted.
- `theme_manager_apply_theme` calls `lv_xml_reapply_token_styles` before the walker.

## Tests

- helix-xml (`lib/helix-xml/tests/cases/test_style.c`, `make test-xml`):
  - token inline color is recorded; a hex literal is not
  - a selector suffix (`-checked`) is recorded with its selector
  - re-apply after `lv_xml_set_const` updates the object
  - re-apply leaves a value C++ changed after creation alone
  - the record dies with the object (no leak, no stale entry)
  - `lv_label` is covered via `lv_xml_obj_apply`
- App: a modal built before a mode switch shows the new mode's token colors after it,
  for a `text_small` with an inline token color and for a plain `lv_label`.
