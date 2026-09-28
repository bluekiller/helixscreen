# Belt Tension: resonance path comparison (design)

Tracking: prestonbrown/helixscreen#1721. Status: design approved in brainstorm 2026-09-28,
awaiting spec review and an implementation plan.

## Goal

Bring Belt Tension back to the Advanced panel as a motor-driven comparison of the two CoreXY
belt paths. Klipper's `TEST_RESONANCES` shakes the toolhead along each diagonal
(`AXIS=1,1` for Path A, `AXIS=1,-1` for Path B), writes each response to a CSV in `/tmp`,
and HelixScreen overlays the two responses and says how well they match.

It replaces the pluck tuner, withdrawn from the UI after its first real-hardware test
(`docs/devel/BELT_TUNER.md` § Validation status). Motor excitation is identical every run,
which is what the pluck method could not give.

## Decisions

| Question | Decision |
|---|---|
| What the tool reports | **Match only.** How closely the two paths' responses agree and which one peaks higher. No absolute tension target: sweep peaks are whole-gantry modes, not belt pitch. |
| Tuning loop | **Re-test one path.** The first run measures both; Results offers Re-test A, Re-test B, Test both. Built so a guided loop ("adjust this belt, this way") can be added later without rework. |
| Verdict | **Provisional verdict now** ("Well matched" / "Close" / "Adjust needed"), labelled beta. Directional advice ("loosen A or tighten B") is the follow-up step, gated on validation data. |
| Where it lives | **Rework the existing Belt Tension panel in place.** Pluck UI states are deleted; the pluck analysis libraries in `src/calibration/` stay compiled for reuse. |
| Visual direction | **"Big numbers over glowing curves"** (mockup option C). |
| Effects | Glow and other costly effects ride the existing `PlatformCapabilities` tiers. |

## Non-goals

- Absolute belt tension or a target frequency.
- Directional adjustment advice (follow-up, see "Toward guided tuning").
- Cartesian printers: an X vs Y comparison does not measure belt matching. CoreXY only.
- Printers with more than one accelerometer reporting during the sweep (clear error, not
  guessed).
- Persisting results across panel sessions. Curves live in memory while the panel is open.
- Remote HelixScreen (not on Klipper's host). The CSV lands in Klipper's `/tmp`, which
  Moonraker cannot serve, so the existing co-location gate stays.

## Screens

Four states replace today's five (`include/ui_panel_belt_tension.h#ViewState`): **START,
RUNNING, RESULTS, ERROR**. Mockups (local, gitignored):
`.superpowers/brainstorm/1440913-1790624439/content/results-direction-v2.html` and
`flow-c-v2.html`.

Colors: Path A blue, Path B orange, used identically in every screen, the sketch, the chart
and the buttons. Exact values come from theme tokens at implementation time, not the mockup
hex codes.

### START

- Left: a top-view sketch of the toolhead with two double-headed diagonal arrows, blue
  "A · 1,1" (+X+Y) and orange "B · 1,-1" (+X-Y). It names **paths, not belts**: which
  physical belt a path loads depends on the printer, so no motors or belt routing are drawn.
  The help text explains the mapping.
- Right: "Compare your belt paths", one line of explanation, then rows for Kinematics,
  Accelerometer, and Sweep (range and estimated time from the printer's `[resonance_tester]`
  config, for example "5-135 Hz · about 4 min"). A failing gate leg replaces these rows with
  its message.
- One primary button: Start check.

### RUNNING

- Klipper produces no data until the file is written, so **the frequency axis is the
  progress bar**: a cursor in the running path's color walks from the sweep start to end,
  driven by `Testing frequency N Hz` lines, with the swept region tinted behind it.
- A finished path's curve is drawn underneath while the other sweeps.
- Header numbers: finished path shows its peak; the running path shows `--` and "sweeping".
- Centre: "Measuring Path B", "2 of 2 · 2:41 elapsed". A single-path re-test reads
  "Re-measuring Path B" with no "n of 2".
- Between paths and after the last one, a short "Analyzing" state.
- Stop (danger-styled): confirm, then emergency stop and firmware restart, the same as the
  input shaper panel (`ui_panel_input_shaper.cpp#cancel_calibration`). `TEST_RESONANCES`
  cannot be stopped cleanly mid-sweep.

### RESULTS

- Top row: Path A peak as a hero number at left, Path B at right, each with the path label
  above and freshness below ("just now · was 110", "3 min ago"). Centre: verdict chip, then
  "6 Hz apart · 91% similar".
- Behind them, full width: both curves overlaid with a gradient fill under each and each peak
  marked.
- After a single-path re-test, that path's previous curve stays on the chart, muted and
  dashed, so the user sees which way the adjustment moved the peak.
- Bottom: a delta rail, centred on "matched", green band for the matched tier, amber for
  close, a marker at the signed delta, "◀ B tighter" and "A tighter ▶" at the ends.
- Buttons: Re-test A, Re-test B (each with its path's color dot), Test both (primary).

### ERROR

Message plus Retry. Sources: Klipper error on the command, stall, "written to" file missing
or unreadable, unexpected CSV shape (multi-chip), the gate closing mid-run (disconnect, print
started).

### Small screens (480×272 class)

Verdict chip moves into the header, hero numbers shrink, the rail is dropped (numbers and
chip carry it), buttons compress (Re-test A / Re-test B / Both).

### Platform tiers

`include/platform_capabilities.h#PlatformCapabilities`:

| Tier | Results rendering |
|---|---|
| STANDARD with `supports_animations` | Full: curves with "glow" (the line drawn three times at falling opacity and rising width, no blur), gradient fill, hero-number halo. A real blur only if the stacked strokes fall short on hardware, and then behind the same gate. |
| BASIC | Plain line plus gradient fill, no glow, the chart's existing 50-point decimation. |
| EMBEDDED (`supports_charts == false`) | No chart object. Hero numbers, verdict chip, facts line and the rail carry the result. |

The chart is `ui_frequency_response_chart` (multi-series, `set_series_muted`, `mark_peak`,
`configure_for_platform`). The panel already reserves `chart_series_a_/b_`.

## Backend

### What Klipper does

Verified in `klippy/extras/resonance_tester.py` and `shaper_calibrate.py` for mainline
Klipper, Kalico, and the FlashForge AD5X stock fork:

- `TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a` prints
  `Testing frequency %.0f Hz` once per whole Hz, then
  `Resonances data written to <path> file`. The file is closed before that line is printed,
  so the line means the data is complete.
- Path: `/tmp/resonances_<axis name>_helix_belt_a.csv`. The axis name differs by variant:
  mainline `axis=1.000,1.000,0.000`, Kalico and the AD5X fork `axis=1.000,1.000`. A point
  suffix appears only when `POINT=` is passed (we do not pass it). **The path is always taken
  from the console line, never constructed.**
- Columns: `freq,psd_x,psd_y,psd_z,psd_xyz`. Kalico appends `accel_per_hz`. With more than
  one accelerometer dataset the columns are per-chip names instead. **Parse by header name.**

### Running a path

- Generalize `moonraker_advanced_api.cpp#InputShaperCollector` to serve both commands rather
  than copying it. Shared: console subscription, progress from `Testing frequency` scaled to
  the printer's configured range, the "written to" path capture (`parse_csv_path`), stall
  logging, idle fallback and backstop timers. Per command: the terminal line pattern and the
  result handling.
- `MoonrakerAdvancedAPI::test_belt_resonance` sends `OUTPUT=resonances` and hands back the
  parsed curve. **No fixed deadline**: `BELT_TENSION_TIMEOUT_MS` goes; a run fails only on
  the stall window.
- Fixed names `helix_belt_a` / `helix_belt_b`, so repeat runs overwrite and `/tmp` does not
  accumulate. The previous curve for the ghost line is held in memory.
- The co-location gate keeps its current proxy (klippy socket reachable from this host). A
  read failure after a successful run still lands in ERROR with the path in the message.

### Reading and comparing

- Extend `shaper_csv_parser.cpp#parse_shaper_csv` (same format minus shaper columns) to read
  a resonances file by header name, returning frequencies and `psd_xyz`. Local `std::ifstream`
  as today. Distinct errors for missing file, empty/short file, and multi-chip header.
- A pure comparison function, two curves in, result out:
  - **Peak**: strongest peak from 20 Hz to the end of the sweep (existing
    `find_peak_frequency`).
  - **Similarity**: existing Pearson `calculate_similarity` after resampling both curves onto
    common frequency points.
  - **Delta**: `peak_a - peak_b`, **signed**. It drives the rail now and directional advice
    later.
  - **Verdict**: the worse of the two tiers below. All constants in one place, commented as
    provisional.

| | Matched | Close | Adjust needed |
|---|---|---|---|
| \|Δ peak\| | ≤ 3 Hz | ≤ 8 Hz | > 8 Hz |
| Similarity | ≥ 90% | ≥ 75% | < 75% |

### Removed

- The `OUTPUT=raw_data` path and `data_store` download (`download_accel_csv`), which could
  never find Klipper's file.
- Target and tolerance in `BeltTensionResult`, `evaluate_belt_status`, `overall_status` and
  the target-based recommendation strings.
- `excite_belt_at_frequency` if nothing calls it (strobe-mode remnant).
- Pluck UI: POSITION and LISTEN states, park/span logic, `belt_trace` widget, the
  `dsp_capable` gate leg, replay observer. `BeltStreamClient::socket_reachable` stays (the
  co-location probe). Pluck analysis code in `src/calibration/` stays compiled.
- `HELIX_HAS_BELT_TUNER`'s Makefile comment is rewritten for what it gates now.

## Mock

The mock does not simulate `TEST_RESONANCES` today. It gets a small simulated printer, in the
shape of the existing `SHAPER_CALIBRATE` mock (`moonraker_client_mock.cpp`, which writes a
real `/tmp` CSV):

- **Model.** Per-path simulated tension sets the main peak (frequency proportional to the
  square root of tension), plus a weaker secondary mode and a noise floor, on Klipper's real
  frequency bin spacing.
- **Tuning loop.** `HELIX_MOCK_BELT_A_HZ` / `HELIX_MOCK_BELT_B_HZ` set starting peaks. Each
  re-test of one path moves its peak toward the other by a fixed step, so repeated re-tests
  walk Adjust needed → Close → Matched.
- **Timing.** `Testing frequency` lines at the configured `hz_per_sec`; `--sim-speed`
  compresses it (6× gives both paths in about 40 s).
- **Failures** via `HELIX_MOCK_BELT_FAIL=stall|nofile|multichip|error|kalico`.
- **Fidelity.** Console strings and CSV headers copied from the Klipper sources above and
  pinned by a unit test. Real captures become fixtures when they exist.
- Document the new variables in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`.

## Testing

- **Comparison**: peak, similarity, signed delta, every verdict tier boundary on both axes.
- **Parser**: mainline, Kalico (`accel_per_hz`), multi-chip, empty, truncated, missing.
- **Console matching**: both axis-name formats in the "written to" line; progress scaling.
- **Collector**: stall window, handoff on the terminal line, Klipper error.
- **Panel**: START → RUNNING → RESULTS; Re-test A keeps B and ghosts the old A; Test both
  clears ghosts; Stop; each `HELIX_MOCK_BELT_FAIL` mode reaches ERROR; gate legs.
- **Tiers**: RESULTS at EMBEDDED creates no chart; BASIC has no glow series; STANDARD has
  them.
- `make mutate-diff` over the comparison and the parser before merge.
- **Hardware, before the beta ships**: one run on Preston's Voron 2.4 and one on the #1721
  reporter's Voron 2.4 350. Their CSVs become parser fixtures and the provisional thresholds
  are reviewed against them.

## Toward guided tuning (follow-up, not in this work)

The signed delta and per-path history are kept so a later change can:

- Replace the provisional thresholds with ones drawn from real captures.
- Add "loosen A or tighten B" advice and offer the next single-path re-test directly.
- Map paths to physical tensioners where the printer database knows the machine.

## Touch list

- `include/ui_panel_belt_tension.h`, `src/ui/ui_panel_belt_tension.cpp`,
  `ui_xml/panel_belt_tension.xml`; `ui_xml/components/belt_result_card.xml` removed with its
  registration (the hero numbers replace the cards; the belt panel is its only user)
- `src/ui/ui_belt_trace.cpp` / its header and registration in `src/xml_registration.cpp`
  (removed)
- `include/belt_tension_types.h`, `src/calibration/belt_tension_types.cpp`,
  `src/calibration/belt_tension_calibrator.cpp` and header, `src/calibration/belt_gating.cpp`
- `src/api/moonraker_advanced_api.cpp`, `include/moonraker_advanced_api.h`,
  `include/i_moonraker_sub_apis.h`
- `src/calibration/shaper_csv_parser.cpp`, `include/shaper_csv_parser.h`
- `src/api/moonraker_client_mock.cpp`, `src/api/moonraker_client_mock_files.cpp`
- `ui_xml/advanced_panel.xml` (row back, beta-gated, new description)
- New strings through `make translation-sync` / `make translations`
- Docs: `docs/user/guide/calibration.md`, `docs/user/guide/beta-features.md`,
  `docs/user/CONFIGURATION.md`, `README.md`, `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`; new `docs/devel/BELT_TENSION.md`
  as this tool's developer doc (indexed in `docs/devel/CLAUDE.md` and `docs/README.md`);
  `docs/devel/BELT_TUNER.md` stays as the doc for the retained pluck libraries, its header
  pointing to `BELT_TENSION.md` for the UI
- Tests under `tests/unit/` for each area above
