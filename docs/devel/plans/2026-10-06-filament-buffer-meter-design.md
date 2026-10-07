# Filament buffer meter: design

Approved in brainstorming with Preston on 2026-10-06. Builds on
`feature/openams-fps-meter` (commits a20b49a83 and 4aaf9c26c).

## Problem

A filament buffer sits between the feeder (AMS hub motor, Happy Hare gear) and
the extruder and absorbs the speed difference between them. Three backends
publish where it sits as a proportional reading: OpenAMS FPS (`lanes[].pressure`
and `set_point`), AFC `AFC_buffer` with `type: FPS_PSF` (`fps_value`,
`smoothed_fps`, `set_point`) and Happy Hare sync feedback (`sync_feedback_bias`).

Today that reading is drawn three different ways or not at all: OpenAMS shows
"Pressure: 53%" as text in a frozen modal, Happy Hare gets a plunger-style
`UiBufferMeter` on page 2 of the clog widget and in the modal, AFC FPS gets the
page but not the modal meter. None of it is live in the modal, and there is no
1x1 home widget.

## Buffer reading versus clog detection

These are different measurements and stay different in the UI.

- **Buffer reading**: where the slack sits right now. It moves all the time in a
  normal print; the feeder steers it toward the set point.
- **Clog detection**: how close the printer is to pausing. It accumulates over
  extruded distance and should sit near zero. Sources: Happy Hare encoder
  (`clog_detection` 1 or 2), Happy Hare FlowGuard (`flowguard.enabled`), AFC
  buffer fault distance (`fault_detection_enabled`), and a CFS fork that
  publishes `path.buffer.active`.

A printer can have either, both or neither. OpenAMS has only a buffer reading; a
plain AFC TurtleNeck has only clog detection.

## Visual design

**The buffer slider.** A housing on the filament line with a block riding on the
strand. A dashed window marks the target, faint red zones mark both ends. Nothing
fills: only the block moves, so it cannot read as a progress bar. It is drawn
upright on every surface (filament flows top to bottom, as on the path canvas):
loose is up, tight is down.

**Colour.** One rule, from the bias (-1 tight .. +1 loose around the set point):

| abs(bias) | Block colour |
|-----------|--------------|
| below 0.3 | neutral grey (`text_muted`) |
| 0.3 to 0.7 | `warning` |
| 0.7 and above | `danger` |

The same rule tints the path-canvas buffer box, so its on-target colour becomes
neutral rather than green.

**Label and number.** "FPS" for a pressure sensor (OpenAMS, AFC FPS_PSF), "Sync"
for Happy Hare. The number is the pressure percentage where the backend reports
one, otherwise the bias as a percentage. The target is shown as "target N%" where
a set point is known.

**No set point, no slider.** A pressure reading without a set point (OpenAMS
publishes `set_point: null` when no unit reports `fps_target`) has nothing to
centre on. Every surface then shows "Pressure: N%" as text and no slider.

## Surfaces

| Surface | Content |
|---------|---------|
| Home widget "Filament buffer" (new), 1x1 default | Upright slider, label, number |
| Same widget, 2x1 | Slider plus a 60 s trace to its right, "running tight/loose/balanced" under it |
| AMS sidebar loaded-spool card | Small upright slider beside the material, alongside the clog arc when a detector exists |
| AMS path canvas | Existing labelled FPS/BUF box, tinted by the colour rule; tap opens the modal |
| Buffer Status modal | Tall upright slider with the 60 s trace scrolling out of it to the right; number, target, lean in words; the backend's own rows (HH spool motor, gear sync, flow; AFC state, distance to fault); single close X, no Cancel/OK; live |
| Clog detection widget | Detector only. Never shows pressure; absent on printers with no detector |

The new widget is gated on a proportional reading existing and disappears when
the backend stops reporting one.

**Deleted:** `UiBufferMeter` (`include/ui_buffer_meter.h`, `src/ui/ui_buffer_meter.cpp`),
the clog widget's carousel and buffer page, and the modal's meter column.

## Architecture

The buffer gets its own small data path. Every rule it shares with the clog meter
stays in one place.

| Piece | Location | Notes |
|-------|----------|-------|
| FPS to bias | `BufferHealth::fps_to_bias()` | Already the single conversion, used by AFC and OpenAMS |
| Which unit | `AmsSystemInfo::pressure_sensor_bias()` / `buffer_bias(unit)` | Already exist. System level: the unit feeding the current slot, else the first with a sensor |
| Bands and colour | One pure function beside the 30/70 bands in `include/clog_meter_geometry.h` | Used by the clog meter status, the path-canvas tint, the slider and the modal's lean text (`buffer_lean()`) |
| Buffer reading | New pure `buffer_reading(const AmsSystemInfo&, int unit)` | Returns present, value %, target % (optional), bias, severity, label. No LVGL, unit-tested |
| Live subjects | `AmsState` publishes the system-level reading as a `buffer_*` subject set | Same pattern as `clog_meter_*`; XML-registered names |
| History | `AmsState` keeps about 60 s of `(time, bias)` per unit with a sensor | Main thread only. Readings arrive on change, so the trace holds the last value as a step; the renderer redraws once a second so it scrolls with no new data |
| Renderer | One `UiBufferSlider` for 1x1, 2x1, card and modal | Size-driven layout; optional trace. Block position, target window and danger zones come from a pure geometry function, tested without LVGL like `clog_bar_geometry()` |
| Modal liveness | Keep 4aaf9c26c's mechanism | Re-reads the backend on `ams_data_revision` / `backend_count` change; falls back to the unsupported message when the backend goes away |

Which unit each surface follows: the home widget and the loaded card use the
system-level reading; the modal and the path box use the unit the user tapped.

**Reverted from 4aaf9c26c:** the second `clog_meter_*` sample for pressure and
"pressure becomes primary when no detector exists". The clog pipeline returns to
detectors only.

## Testing

- Pure: `buffer_reading()` for each backend shape, including no set point; the
  bands and colour rule at each boundary; history window, step hold and expiry;
  slider geometry at each size.
- Widget: "Filament buffer" registers, appears only with a reading, disappears
  when the reading goes; clog widget absent on a pressure-only printer.
- Modal: existing `[live]` cases plus the trace advancing while open.
- Screenshots from the mock (`buffer_fps`, `buffer_fps_loose`,
  `sync_feedback_tight`): 1x1 and 2x1 widget, loaded card, path box, modal with a
  scrolling trace. The UI goldens (`tests/ui/test_screens.py`) are not in CI;
  regenerate them for the AMS panel and home screen.
- Each new test proven red with its code reverted.

## Out of scope

- The CFS clog source: it reads only the fork's `active` flag, so the clog widget
  can appear with no distance data. Separate issue.
- OpenAMS clog detection: klipper_openams publishes none.
