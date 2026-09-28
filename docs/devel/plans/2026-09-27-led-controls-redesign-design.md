# LEDs overlay and per-device light buttons

prestonbrown/helixscreen#1130. Mockups (round 2, approved): https://claude.ai/artifact/2nRtWqZtnizhAgGzYnjhur

## Goal

Stop one list meaning two things. Today `LedController::selected_strips()` is both what the
LED overlay is looking at (`[0]` drives the header, the sections and the macro buttons) and
what the light button, Automatic LED Control and LED on at Start act on. Tapping a chip to
look at a device changes what the light button toggles; PRESET macro devices have no chip
at all. This design separates "which device am I looking at" from "which devices does this
control drive", and gives every device the same page.

## The overlay

- Title **LEDs** (was "LED Control"). Settings keeps "LED Settings".
- One tab per device, in `all_selectable_strips()` order plus every PRESET macro device.
  Tabs only choose the focused device (`focused_strip_`, owned by the overlay, never
  persisted into any selection). One row that scrolls sideways, with a fade at the right
  edge when it overflows. Each tab is a named widget so `ctl click` can reach it.
- Each tab carries a state dot: filled in the light's current color when on, hollow when
  off, dashed when the state cannot be read (macro devices).
- The overlay opens on the device of whatever opened it (below). With no device given it
  opens on the last focused device, else the chamber light (§ Chamber light).

## The device page

Two columns. Left, the lamp: the things touched most. Right, how the light looks. A section
appears only when the device supports it.

| Device | Left | White | Color | Right, below |
|---|---|---|---|---|
| Klipper LED, RGBW | power + brightness | W channel | yes | Effects, when led_effect defines any for the strip |
| Klipper LED, RGB | power + brightness | mixed from RGB | yes | Effects, when defined |
| Klipper LED, single channel | power + brightness | - | - | Level chips |
| WLED | power + brightness | - | - | Presets from the device |
| Output pin, PWM | power + brightness | - | - | Level chips |
| Output pin, on/off | power only, centred | - | - | - |
| Macro, ON_OFF | On and Off buttons | - | - | a note naming the macros |
| Macro, TOGGLE | one Toggle button | - | - | a note naming the macro |
| Macro, PRESET | none | - | - | Presets, full width |

- **Power**: a round button above the brightness slider. When on it fills with the light's
  current color; off it is an outlined grey circle. Never the danger variant. Same place
  on every device that has one.
- **Brightness**: a tall vertical slider whose fill is the current color, with the
  percentage inside it. It is also the color preview; the separate "Color" swatch goes.
- **White**: Cool, Neutral, Warm, labelled. On RGBW strips these set the W channel; on RGB
  they are mixed. Fixed values, not user-editable.
- **Color**: round swatches from `color_presets()`, then a Custom swatch (rainbow ring)
  that opens the existing color picker. The default preset list becomes red, orange, green,
  cyan, blue, purple, pink. A user whose saved `leds/color_presets` equals the old default
  gets the new default; an edited list is kept.
- **Selection is shown on the control**: a ring on the current swatch, a filled chip for
  the running effect or preset. Effects get a "None" chip that stops the running effect.
- **Effects / Presets**: led_effect effects, WLED presets and macro presets share one chip
  style and position. The label is the source's own word: Effects for led_effect, Presets
  for WLED and macros.
- **Level chips** (10, 25, 50, 75, 100%) for dimmable devices with nothing else to show.
- **Macro devices** never show a power state, because it cannot be read. ON_OFF gets
  explicit On and Off buttons where the slider would be; TOGGLE gets one button.
- Every control acts on the focused device only. `toggle_all()` / `set_color_all()` /
  `set_brightness_all()` stop being reachable from the overlay.
- At 480x272 the whole page fits without vertical scrolling; the effects row scrolls
  sideways.

## Home light buttons

- `LedWidget` gets per-instance config following `FanWidget` (`set_config()` reads a
  `led` key, `on_edit_configure()` opens a picker from the edit-mode gear,
  `save_widget_config()` persists it). Several instances can sit on the home screen.
- Picker "This button controls": every device that can be switched (PRESET-only macro
  devices are not listed), then **All lights**. Default when unset: the chamber light.
- The tile shows the device name, and a bulb in the device's current color when on.
- 1x1 tile: tap toggles. 2x1 tile: tap toggles, a separate › zone opens the overlay on that
  device. Long-press stays edit mode (non-goal below).
- A button set to All lights toggles every switchable device; the overlay opens on the
  chamber light.

## Chamber light

One named function beside the LED backend code resolves "the chamber light": match the
Klipper object name against `chamber_light`, `chamber_LED` (Zmod), `case_light`,
`caselight` (case-insensitive), falling back to `first_available_strip()`. The light
button default, the overlay's fallback focus, and the printer-image light chip all use it.
No other code names these spellings.

## What the old selection becomes

`leds/selected_strips` stops being user-facing. Its three jobs split:

- **Light buttons**: per instance, above.
- **Automatic LED Control**: gets its own "Applies to" device row in Settings, beside its
  toggle, stored as `leds/auto_state/strips`. It is a different job from the light button:
  on a Voron the button wants the chamber light and auto-state wants the toolhead LEDs.
- **LED on at Start**: turns on the devices the home light buttons control (union over
  the instances; the chamber light when there are none).
- The "LEDs controlled by the light button" chip row in Settings is removed.

Migration, once, on first load of a config that has `leds/selected_strips` and no
`leds/auto_state/strips`:

- One device selected: existing light buttons get that device.
- Every switchable device selected: existing light buttons get All lights.
- Anything else: the selection becomes the Automatic LED Control target and light buttons
  fall back to the chamber-light default. This is the one case that changes behaviour;
  the release note says so.
- The auto-state target is seeded from the old selection in every case.

## Entry points and focus

| Opened from | Lands on |
|---|---|
| Light tile › zone (2x1) | that tile's device (chamber light for All lights) |
| Printer-image light chip | the chamber light |
| LED controls tile | last focused device, else the chamber light |

The printer-image light chip shows while the chamber light is on (today it follows the
global LED state).

## Non-goals

- Long-press on a light button for a light menu (decided in the issue; the gesture is the
  home screen's edit mode).
- Hand-picked groups of lights for one button. Two buttons, or All lights, cover it.
- Colors for WLED. It gets power, brightness and its own presets, as today.

## Testing

- `focused_strip_` never writes a selection; tapping a tab changes only focus.
- Capability table: each device kind shows exactly its sections (pure function from
  `LedStripInfo`/macro type to a section set, unit-tested per row).
- Chamber-light resolver: each spelling, case, fallback, no devices.
- Migration: the three selection shapes, run-once, auto-state target seeded.
- Light widget: per-instance config round trip, default resolution, All lights toggles
  every switchable device, PRESET devices absent from the picker.
- Overlay opens on the requested device from each entry point.
- Screenshots at 800x480 and 480x272 of every row of the capability table, compared
  against the mockups.
