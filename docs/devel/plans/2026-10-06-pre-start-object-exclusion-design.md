# Pre-start object exclusion - design

Status: approved design, not yet planned or built.

## Goal

In a file's print details view, before starting, the user picks objects to skip. The
print then runs with those objects excluded from the first layer, without touching the
G-code file. It must feel exactly like excluding objects on print status: the same skip
button in the same corner of the preview, the same numbered badges on the map and the
2D/3D render, the same object list.

Done when: pick objects in details, press Print, and those objects never print; print
status lists them as Excluded from the start.

## Constraints

- Klipper clears `exclude_object` state when a print starts (`virtual_sdcard:reset_file`),
  so exclusions cannot be sent before the start. They are sent right after Moonraker
  confirms it. `EXCLUDE_OBJECT NAME=` is queued behind `PRINT_START` and runs before the
  first `EXCLUDE_OBJECT_START`. Sliced files put `EXCLUDE_OBJECT_DEFINE` in the header,
  ahead of the start macro.
- No file rewrite, so no HelixPrint plugin dependency. Works on every printer with
  `[exclude_object]`, the Snapmaker U1 included.
- DRY: print status and print details share one implementation of exclude mode. No
  parallel "pending objects" model, no second parser, no copied panel wiring.

## Architecture

### One model, two owners

`PrinterExcludedObjectsState` stays the only model. Print status keeps reading the
printer's live instance. Print details owns a private instance, initialised with
`init_subjects(register_xml=false)` so its subjects never collide with the live one.
That instance is filled from the file: defined objects plus geometry, the excluded set
holding the user's pending picks, and no current object.

### Components take a state and a tap callback

`ExcludeObjectSideList` and `ExcludeObjectMapView` stop taking `PrinterState*` and
`PrintExcludeObjectManager*`. They take a `PrinterExcludedObjectsState*` and an
`on_object_tapped(name)` callback.

- Print status: live state; the callback calls `PrintExcludeObjectManager::request_exclude`
  (confirm, 5s undo, send), unchanged.
- Print details: private state; the callback toggles the name in that state's excluded
  set. No confirmation, because nothing is committed until Print.

Badges (`compute_object_badges`), map and list already redraw from the state's version
subjects, so details needs no code of its own to keep them current.

### One shared exclude-mode controller

`PrintStatusPanel::show_exclude_map_view` / `hide_exclude_map_view` currently wire map or
render mode, the side list, the render badges and the tap routing. That moves into one
controller both panels own. Given the preview stack, the G-code viewer, a state and a tap
callback, it shows or hides exclude mode. The skip button (`btn_objects`, icon
`icon_debug_step_over`) moves into shared preview markup so details gets the identical
button.

### Object list source

The details file scan (`PrintPreparationManager::scan_file_for_operations`, first
`PRINTER_STOP_SCAN_BYTES`) also collects `EXCLUDE_OBJECT_DEFINE` names, centres and
polygons, through `GCodeParser::parse_exclude_object_command` exposed as a free function,
not a second parser. When the 2D/3D preview has parsed the whole file, its object outlines
are used, matching print status's existing precedence.

### Sending after start

Pressing Print with picks passes the excluded names into the start path. After Moonraker
confirms the start, each name goes through the existing `api->exclude_object(name)`
(15 minute silent timeout, made for waiting behind `PRINT_START`). This covers every
start path: direct, plugin modify, and the pre-start G-code wait.

## Behaviour

- Skip button: shown only when the printer has `[exclude_object]` and the file defines at
  least 2 objects (the rule print status uses). Hidden for 3MF and for files without
  definitions.
- Opening it shows the same view as print status in the current preview mode: top-down
  map plus list in thumbnail mode, render with numbered badges plus list in 2D/3D.
- A tap on a badge, outline or row toggles the object instantly. Excluded objects fade and
  their row reads "Excluded" (the existing string). No row reads "Printing now".
- Once anything is picked, the skip button shows a count badge.
- Picks persist while the user stays on that file's details, including closing and
  reopening the view. They clear on going back to the list or opening another file, and
  are consumed when the print starts.
- Reprint (print status, history) never carries picks.

## Edge cases and errors

- Every object excluded: Print is refused with a toast, before any heating.
- Definitions beyond the scan window: the button appears once the 2D/3D preview's parse
  finds them.
- The send fails after the start (name mismatch, connection loss): an error toast names
  the objects not excluded. The print continues, and print status's list shows them
  unexcluded so the user can exclude them there.
- Names are compared upper-case, as Klipper stores them.
- A start that goes into the job queue instead of starting now: picks do not carry in
  this version. Pressing Print shows "Object picks apply only to prints started now" and
  queues without them.

## Testing

Unit, test first:
- Scan: names, centres and polygons from `EXCLUDE_OBJECT_DEFINE` via the shared parser;
  quoted names; no definitions; definitions past the window.
- Shared exclude-mode controller: print status's existing exclude tests pass unchanged.
- Details: a tap toggles the private state; the count badge follows picks; picks clear on
  leaving the file; the button is hidden without definitions or without `[exclude_object]`.
- Start path: exclusions are sent only after the start is confirmed, never before; all
  excluded refuses the start; a queued start drops picks with a toast; a failed send
  toasts the names.
- Mutation: each new piece goes red when its code is reverted.

Mock: pick 2 of 3 objects in details, start, and confirm with `ctl` that print status
shows them Excluded from the first layer. Screenshots in thumbnail, 2D and 3D, landscape
and portrait.

Real printer: one Snapmaker U1 print with one object excluded, cancelled once the first
layer shows it skipped, with approval before starting. The U1's live Klipper log is
`/oem/klippylogs/klippy.log`.
