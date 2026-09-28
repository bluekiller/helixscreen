# Belt Tension Resonance Comparison Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring Belt Tension back as a CoreXY path comparison: Klipper `TEST_RESONANCES` sweeps each diagonal, HelixScreen reads the two `/tmp` CSVs and shows how well the paths match.

**Architecture:** Pure functions do the reading and the comparing (`parse_resonance_csv`, `compare_belt_paths`, console-line helpers), so they are tested without LVGL or a printer. A `BeltResonanceCollector` built on the existing `CalibrationCollectorCore` follows Klipper's console for one sweep. The existing `BeltTensionPanel` is rebuilt in place as START / RUNNING / RESULTS / ERROR over an extended `ui_frequency_response_chart`. The mock printer grows a `TEST_RESONANCES` simulation that makes the tuning loop, timing and failure modes reproducible.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2 (`tests/unit/`), spdlog, pure Makefile.

**Spec:** `docs/devel/plans/2026-09-28-belt-tension-resonance-compare-design.md` (read it first; this plan argues from it).

## Global Constraints

- MAJOR work: do it in a worktree made by `scripts/setup-worktree.sh feature/belt-resonance-compare` (never the harness's `.claude/worktrees/`). Claim it: `scripts/helix-claim take worktree:belt-resonance-compare edit --pid $$`.
- Before any build: `pgrep -x -d' ' 'make|clang++|cc1plus'`. Use plain `make` / `make t F='[tag]'`; never hand-pick `-jN`. Never pipe a build through `head`/`tail`/`grep`; redirect to a log and read it.
- Inner test loop: `make t F='[tag]'`. Completion gate at the end: `make full-test-run`.
- Logging: spdlog only, `[BeltTension]` / `[BeltCollector]` / `[MoonrakerClientMock]` prefixes. No `printf`, no `LV_LOG_*`.
- New source files start with `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- JSON: `#include "hv/json.hpp"`. Regex: `helix::Regex` / `helix::regex_search` from `include/helix_regex.h`; never `std::regex`.
- Threading (`.claude/rules/threading.md`): console callbacks arrive off the LVGL thread. Anything touching LVGL or panel state goes through `lifetime_.bg_cb(...)` (panel) or `ui_queue_update` / `tok.defer()`.
- UI: data in C++, appearance in XML (`.claude/rules/declarative-ui.md`). State visibility via `bind_flag_if_not_eq` on `belt_tension_state`. Design tokens, not hex colors, in XML.
- New user-facing strings: `translation_tag` / `title_tag` in XML, `lv_tr("...")` in C++; then `make translation-sync && make translations`, and commit the YAMLs plus `ui_xml/translations/*.xml`.
- Comments describe the code as it is now. No history, no commit SHAs, no "used to", no narrated issues; `(prestonbrown/helixscreen#1721)` as a bare pointer is fine.
- Provisional verdict constants (verbatim from the spec): matched |Δ| ≤ 3 Hz and similarity ≥ 90%; close |Δ| ≤ 8 Hz and similarity ≥ 75%; otherwise adjust. Peak search from 20 Hz to the end of the curve.
- Klipper facts (verbatim from the spec): terminal line `Resonances data written to <path> file`; path taken from that line, never constructed; columns read by header name (`freq`, `psd_xyz`; Kalico adds `accel_per_hz`; multi-chip files have per-chip columns and are an error).
- Commit messages: `feat(belt): ...` / `test(belt): ...` / `refactor(...)`, subject plus a ~4-line body, `(prestonbrown/helixscreen#1721)` on the subject. Each commit body names the mutation that proved its test (Task 11 runs `make mutate-diff`).

## Review Focus

1. **Panel closed mid-sweep.** Back pressed during RUNNING: the collector must stop listening and no callback may touch the destroyed/hidden panel; reopening shows START. Test in Task 9.
2. **Printer with a non-default sweep range** (e.g. `min_freq: 1`, `max_freq: 100`, `hz_per_sec: 2`): progress, the running cursor, the START time estimate and the peak window all follow the printer's config. Tests in Tasks 4, 5 and 9.
3. **A path with no clear resonance** (flat noise, or a curve that ends below 20 Hz): the comparison reports it as invalid and the panel shows ERROR "No resonance peak found for Path X", not a 0 Hz hero number. Tests in Tasks 2 and 9.
4. **Stale file from a previous run.** Klipper errors before writing; an old `/tmp/resonances_*_helix_belt_a.csv` exists. No result may be produced from the old file. Test in Task 6.
5. **Stop, then Start again.** After the emergency stop + firmware restart the printer is not ready; Start must stay disabled until the gate reopens, and a new run must not receive lines meant for the abandoned collector. Test in Task 9.

---

### Task 1: One emergency-stop-then-restart helper

The M112 + `restart_firmware` sequence exists twice (`src/calibration/input_shaper_calibrator.cpp#InputShaperCalibrator::emergency_abort`, `src/ui/ui_panel_calibration_tool_offset.cpp` around the "Stopped" status) and the belt panel needs a third. Fold them onto one function first.

**Files:**
- Create: `include/calibration_abort.h`, `src/calibration/calibration_abort.cpp`
- Modify: `src/calibration/input_shaper_calibrator.cpp` (`emergency_abort`), `src/ui/ui_panel_calibration_tool_offset.cpp` (the `api->emergency_stop(...)` block)
- Test: `tests/unit/test_calibration_abort.cpp`

**Interfaces:**
- Produces: `void helix::emergency_stop_and_restart(IMoonrakerAPI* api, const char* log_tag);` (null `api` logs a warning and returns).

The mock makes both calls observable: `printer.emergency_stop` runs `emergency_stop_internal()`, which puts klippy in `SHUTDOWN`, and `printer.firmware_restart` runs `trigger_restart(true)`, which returns klippy to `READY` after 3 s divided by the mock's speedup. So "SHUTDOWN, then READY again" proves both calls, in order; without the restart klippy stays `SHUTDOWN`.

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_abort.h"

#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("emergency_stop_and_restart sends M112 then a firmware restart",
          "[calibration][abort]") {
    PrinterState state;
    state.init_subjects(false);
    // Speedup 1000: the mock's 3 s firmware restart takes 3 ms.
    MoonrakerClientMock client(MoonrakerClientMock::PrinterType::VORON_24, 1000.0);
    MoonrakerAPIMock api(client, state);
    REQUIRE(client.get_klippy_state() == KlippyState::READY);

    helix::emergency_stop_and_restart(&api, "Test");

    bool saw_shutdown = false;
    bool back_to_ready = false;
    for (int i = 0; i < 400 && !back_to_ready; ++i) {
        UpdateQueueTestAccess::drain_all(UpdateQueue::instance());
        lv_tick_inc(5);
        lv_timer_handler_safe();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto ks = client.get_klippy_state();
        saw_shutdown = saw_shutdown || ks == KlippyState::SHUTDOWN;
        back_to_ready = saw_shutdown && ks == KlippyState::READY;
    }
    CHECK(saw_shutdown);
    CHECK(back_to_ready);
}

TEST_CASE("emergency_stop_and_restart with no API does nothing", "[calibration][abort]") {
    helix::emergency_stop_and_restart(nullptr, "Test"); // must not crash
    SUCCEED();
}
```

Replace the comment block with the concrete assertions once Step 1 names the accessor; the test must fail if either call is missing or they are reversed.

Add the includes the loop needs (`ui_update_queue.h`, the header declaring `UpdateQueueTestAccess` as `test_moonraker_api_input_shaper.cpp` includes it, `lvgl/lvgl.h`, `<thread>`, `<chrono>`), and derive the case from `LVGLTestFixture` (`TEST_CASE_METHOD(LVGLTestFixture, ...)`) if `lv_timer_handler_safe` needs an initialised LVGL, as the input shaper API tests do.

- [ ] **Step 2: Run it to verify it fails**: `make t F='[abort]'`. Expected: link error, `emergency_stop_and_restart` undefined.

- [ ] **Step 3: Implement**

```cpp
// include/calibration_abort.h
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class IMoonrakerAPI;

namespace helix {

/// Stop a calibration that cannot be cancelled cleanly: M112, then a firmware
/// restart once the stop is acknowledged. Fire-and-forget; failures are logged.
void emergency_stop_and_restart(IMoonrakerAPI* api, const char* log_tag);

} // namespace helix
```

```cpp
// src/calibration/calibration_abort.cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "calibration_abort.h"

#include "i_moonraker_api.h"

#include <spdlog/spdlog.h>

#include <string>

namespace helix {

void emergency_stop_and_restart(IMoonrakerAPI* api, const char* log_tag) {
    if (!api) {
        spdlog::warn("[{}] emergency stop requested without an API", log_tag);
        return;
    }
    spdlog::info("[{}] Emergency stop: M112 then firmware restart", log_tag);
    const std::string tag = log_tag;
    api->emergency_stop(
        [api, tag]() {
            api->restart_firmware(
                [tag]() { spdlog::debug("[{}] Firmware restart initiated", tag); },
                [tag](const MoonrakerError& err) {
                    spdlog::error("[{}] Firmware restart failed: {}", tag, err.message);
                });
        },
        [tag](const MoonrakerError& err) {
            spdlog::error("[{}] Emergency stop failed: {}", tag, err.message);
        });
}

} // namespace helix
```

Replace the body of `InputShaperCalibrator::emergency_abort()` after `state_ = State::IDLE;` with `helix::emergency_stop_and_restart(api_, "InputShaperCalibrator");`, and the tool-offset block with `helix::emergency_stop_and_restart(api, "ToolOffsetCal");`. Check `include/i_moonraker_api.h` for the exact `emergency_stop` / `restart_firmware` callback types and match them.

- [ ] **Step 4: Run tests**: `make t F='[abort]'` then `make t F='[input_shaper]'`. Expected: PASS.

- [ ] **Step 5: Commit**: `git add include/calibration_abort.h src/calibration/calibration_abort.cpp tests/unit/test_calibration_abort.cpp src/calibration/input_shaper_calibrator.cpp src/ui/ui_panel_calibration_tool_offset.cpp && git commit -m "refactor(calibration): one emergency-stop-then-restart helper (prestonbrown/helixscreen#1721)"`

---

### Task 2: Path comparison and provisional verdict

**Files:**
- Modify: `include/belt_tension_types.h`, `src/calibration/belt_tension_types.cpp`
- Test: `tests/unit/test_belt_compare.cpp` (new), `tests/unit/test_belt_tension_calibrator.cpp` (prune)

**Interfaces:**
- Consumes: existing `find_peak_frequency(const std::vector<std::pair<float,float>>&, float min, float max) -> PeakResult` and `calculate_similarity(curve_a, curve_b) -> float` (0-100, resamples onto 1 Hz bins over the overlap).
- Produces (all in `helix::calibration`):

```cpp
/// One path's response: (frequency Hz, psd_xyz) pairs, ascending frequency.
using BeltCurve = std::vector<std::pair<float, float>>;

enum class BeltVerdict { MATCHED, CLOSE, ADJUST };

/// Provisional: measured on no real printer yet. Replace from captures before
/// the verdict gains directional advice (prestonbrown/helixscreen#1721).
namespace belt_verdict {
inline constexpr float MATCHED_DELTA_HZ = 3.0f;
inline constexpr float CLOSE_DELTA_HZ = 8.0f;
inline constexpr float MATCHED_SIMILARITY = 90.0f;
inline constexpr float CLOSE_SIMILARITY = 75.0f;
inline constexpr float PEAK_MIN_HZ = 20.0f;
} // namespace belt_verdict

struct BeltComparison {
    bool valid = false;        ///< false when either path has no peak above PEAK_MIN_HZ
    bool peak_a_found = false;
    bool peak_b_found = false;
    float peak_a_hz = 0.0f;
    float peak_b_hz = 0.0f;
    float delta_hz = 0.0f;     ///< peak_a_hz - peak_b_hz, signed: positive = A higher
    float similarity_percent = 0.0f;
    BeltVerdict verdict = BeltVerdict::ADJUST;
};

[[nodiscard]] BeltComparison compare_belt_paths(const BeltCurve& a, const BeltCurve& b);
[[nodiscard]] BeltVerdict belt_verdict_for(float delta_hz, float similarity_percent);
```

- Removes: `BeltStatus`, `belt_status_to_string`, `evaluate_belt_status`, `BeltMeasurement`, `BeltTensionResult` (with `overall_status`, `recommendation`, target/tolerance), `BeltPath::X_AXIS`/`Y_AXIS`, the `BeltMeasurementCallback`/`BeltResultCallback` typedefs. `ui_panel_belt_tension.cpp` still references `evaluate_belt_status`/`belt_status_to_string` at the result card; delete those two lines' uses and leave the result-card code compiling with plain frequency text until Task 9 replaces it. `belt_tension_calibrator.*` still references them; Task 7 rewrites it, so here reduce it only as far as needed to compile (delete `process_csv_data`, `run_auto_sweep`, `test_path`, `execute_resonance_test`, `set_target_frequency`, `set_tolerance`; keep `detect_hardware`, `reset`, `cancel`).

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_belt_compare.cpp`)

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "belt_tension_types.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

namespace {

/// Lorentzian peak on a noise floor, 5-135 Hz at Klipper's 3200/4096 Hz spacing.
BeltCurve curve_with_peak(float peak_hz, float height = 1e4f, float floor = 50.0f) {
    BeltCurve c;
    for (float f = 5.0f; f < 135.0f; f += 0.78125f) {
        const float d = (f - peak_hz) / 5.0f;
        c.emplace_back(f, floor + height / (1.0f + d * d));
    }
    return c;
}

} // namespace

TEST_CASE("identical paths are matched", "[belt][compare]") {
    auto c = curve_with_peak(104.0f);
    auto r = compare_belt_paths(c, c);
    REQUIRE(r.valid);
    CHECK(r.peak_a_hz == Catch::Approx(104.0f).margin(0.8f));
    CHECK(r.delta_hz == Catch::Approx(0.0f).margin(0.01f));
    CHECK(r.similarity_percent > 99.0f);
    CHECK(r.verdict == BeltVerdict::MATCHED);
}

TEST_CASE("delta is signed: positive when A peaks higher", "[belt][compare]") {
    auto r = compare_belt_paths(curve_with_peak(110.0f), curve_with_peak(98.0f));
    REQUIRE(r.valid);
    CHECK(r.delta_hz == Catch::Approx(12.0f).margin(0.8f));
    auto flipped = compare_belt_paths(curve_with_peak(98.0f), curve_with_peak(110.0f));
    CHECK(flipped.delta_hz == Catch::Approx(-12.0f).margin(0.8f));
}

TEST_CASE("verdict tier boundaries on delta", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(3.0f, 95.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(-3.0f, 95.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(3.01f, 95.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(8.0f, 95.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(-8.01f, 95.0f) == BeltVerdict::ADJUST);
}

TEST_CASE("verdict tier boundaries on similarity", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(0.0f, 90.0f) == BeltVerdict::MATCHED);
    CHECK(belt_verdict_for(0.0f, 89.99f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(0.0f, 75.0f) == BeltVerdict::CLOSE);
    CHECK(belt_verdict_for(0.0f, 74.99f) == BeltVerdict::ADJUST);
}

TEST_CASE("verdict is the worse of the two tiers", "[belt][compare][verdict]") {
    CHECK(belt_verdict_for(1.0f, 60.0f) == BeltVerdict::ADJUST);
    CHECK(belt_verdict_for(12.0f, 99.0f) == BeltVerdict::ADJUST);
    CHECK(belt_verdict_for(5.0f, 95.0f) == BeltVerdict::CLOSE);
}

TEST_CASE("a peak below 20 Hz is not a belt peak", "[belt][compare]") {
    BeltCurve low;
    for (float f = 5.0f; f < 19.0f; f += 0.78125f)
        low.emplace_back(f, 1000.0f);
    auto r = compare_belt_paths(low, curve_with_peak(100.0f));
    CHECK_FALSE(r.valid);
    CHECK_FALSE(r.peak_a_found);
    CHECK(r.peak_b_found);
}

TEST_CASE("empty curves are invalid, not a crash", "[belt][compare]") {
    auto r = compare_belt_paths({}, {});
    CHECK_FALSE(r.valid);
}

TEST_CASE("the peak window follows the curve's own end", "[belt][compare]") {
    // A printer configured for max_freq 100: nothing above 100 Hz exists.
    BeltCurve c;
    for (float f = 5.0f; f < 100.0f; f += 0.78125f) {
        const float d = (f - 60.0f) / 5.0f;
        c.emplace_back(f, 50.0f + 1e4f / (1.0f + d * d));
    }
    auto r = compare_belt_paths(c, c);
    REQUIRE(r.valid);
    CHECK(r.peak_a_hz == Catch::Approx(60.0f).margin(0.8f));
}
```

- [ ] **Step 2: Run to verify failure**: `make t F='[compare]'`. Expected: compile errors for `BeltCurve`, `compare_belt_paths`, `belt_verdict_for`.

- [ ] **Step 3: Implement** in `belt_tension_types.cpp`:

```cpp
BeltVerdict belt_verdict_for(float delta_hz, float similarity_percent) {
    using namespace belt_verdict;
    const float d = std::abs(delta_hz);
    auto delta_tier = d <= MATCHED_DELTA_HZ ? BeltVerdict::MATCHED
                      : d <= CLOSE_DELTA_HZ ? BeltVerdict::CLOSE
                                            : BeltVerdict::ADJUST;
    auto sim_tier = similarity_percent >= MATCHED_SIMILARITY ? BeltVerdict::MATCHED
                    : similarity_percent >= CLOSE_SIMILARITY ? BeltVerdict::CLOSE
                                                             : BeltVerdict::ADJUST;
    // Enum order runs best to worst, so the larger value is the worse tier.
    return std::max(delta_tier, sim_tier);
}

BeltComparison compare_belt_paths(const BeltCurve& a, const BeltCurve& b) {
    BeltComparison out;
    if (a.empty() || b.empty())
        return out;
    const auto peak_a = find_peak_frequency(a, belt_verdict::PEAK_MIN_HZ, a.back().first);
    const auto peak_b = find_peak_frequency(b, belt_verdict::PEAK_MIN_HZ, b.back().first);
    out.peak_a_found = peak_a.found;
    out.peak_b_found = peak_b.found;
    out.peak_a_hz = peak_a.frequency;
    out.peak_b_hz = peak_b.frequency;
    if (!peak_a.found || !peak_b.found)
        return out;
    out.delta_hz = peak_a.frequency - peak_b.frequency;
    out.similarity_percent = calculate_similarity(a, b);
    out.verdict = belt_verdict_for(out.delta_hz, out.similarity_percent);
    out.valid = true;
    return out;
}
```

Delete the removed types/functions listed above from both files and delete their tests from `test_belt_tension_calibrator.cpp` (the `evaluate_belt_status`, `belt_status_to_string`, `overall_status`, `recommendation`, `BeltTensionResult`, `run_auto_sweep`, `test_path`, `process_csv_data` cases). Keep every `compute_psd`, `find_peak_frequency`, `calculate_similarity`, `parse_accel_csv` case: the pluck libraries still use them.

- [ ] **Step 4: Run**: `make t F='[belt]'`. Expected: PASS, including the pluck library tests.

- [ ] **Step 5: Commit**: `feat(belt): compare two resonance curves with a provisional verdict (prestonbrown/helixscreen#1721)`.

---

### Task 3: Resonance CSV parser

**Files:**
- Modify: `include/shaper_csv_parser.h`, `src/calibration/shaper_csv_parser.cpp`
- Test: `tests/unit/test_shaper_csv_parser.cpp` (add cases; reuse its `TempCsvFile`)

**Interfaces:**
- Consumes: `BeltCurve` (Task 2).
- Produces (`helix::calibration`):

```cpp
enum class ResonanceCsvError { NONE, MISSING, EMPTY, MULTI_CHIP, NO_PSD_COLUMN };

struct ResonanceCsvData {
    BeltCurve curve;          ///< (freq, psd_xyz), empty unless error == NONE
    ResonanceCsvError error = ResonanceCsvError::NONE;
};

/// Read a TEST_RESONANCES OUTPUT=resonances file. Columns are found by header
/// name; extra columns (Kalico's accel_per_hz) are ignored.
[[nodiscard]] ResonanceCsvData parse_resonance_csv(const std::string& csv_path);
```

`#include "belt_tension_types.h"` in the header for `BeltCurve`. Reuse the file's existing header-splitting / line-reading helpers; do not write a second CSV splitter.

- [ ] **Step 1: Write the failing tests** (append to `test_shaper_csv_parser.cpp`)

```cpp
static const char* MAINLINE_RESONANCES_CSV =
    "freq,psd_x,psd_y,psd_z,psd_xyz\n"
    "5.0,1.0e+01,2.0e+01,3.0e+00,3.3e+01\n"
    "5.8,1.1e+01,2.1e+01,3.0e+00,3.5e+01\n"
    "6.6,1.2e+01,2.2e+01,3.0e+00,3.7e+01\n";

static const char* KALICO_RESONANCES_CSV =
    "freq,psd_x,psd_y,psd_z,psd_xyz,accel_per_hz\n"
    "5.0,1.0e+01,2.0e+01,3.0e+00,3.3e+01,60.0\n"
    "5.8,1.1e+01,2.1e+01,3.0e+00,3.5e+01,60.0\n";

static const char* MULTI_CHIP_CSV =
    "freq,adxl345,adxl345_hotend\n"
    "5.0,1.0e+01,2.0e+01\n";

TEST_CASE("resonance CSV: mainline columns", "[shaper_csv][belt]") {
    TempCsvFile csv(MAINLINE_RESONANCES_CSV);
    auto d = parse_resonance_csv(csv.path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    REQUIRE(d.curve.size() == 3);
    CHECK(d.curve[0].first == Catch::Approx(5.0f));
    CHECK(d.curve[0].second == Catch::Approx(33.0f)); // psd_xyz, not psd_x
    CHECK(d.curve[2].second == Catch::Approx(37.0f));
}

TEST_CASE("resonance CSV: Kalico's extra column is ignored", "[shaper_csv][belt]") {
    TempCsvFile csv(KALICO_RESONANCES_CSV);
    auto d = parse_resonance_csv(csv.path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    REQUIRE(d.curve.size() == 2);
    CHECK(d.curve[1].second == Catch::Approx(35.0f));
}

TEST_CASE("resonance CSV: per-chip columns are reported, not guessed", "[shaper_csv][belt]") {
    TempCsvFile csv(MULTI_CHIP_CSV);
    auto d = parse_resonance_csv(csv.path);
    CHECK(d.error == ResonanceCsvError::MULTI_CHIP);
    CHECK(d.curve.empty());
}

TEST_CASE("resonance CSV: missing file", "[shaper_csv][belt]") {
    auto d = parse_resonance_csv("/tmp/helix-no-such-resonances.csv");
    CHECK(d.error == ResonanceCsvError::MISSING);
}

TEST_CASE("resonance CSV: header only is empty", "[shaper_csv][belt]") {
    TempCsvFile csv("freq,psd_x,psd_y,psd_z,psd_xyz\n");
    CHECK(parse_resonance_csv(csv.path).error == ResonanceCsvError::EMPTY);
}

TEST_CASE("resonance CSV: a truncated last row is dropped, the rest kept",
          "[shaper_csv][belt]") {
    TempCsvFile csv("freq,psd_x,psd_y,psd_z,psd_xyz\n"
                    "5.0,1,2,3,6\n"
                    "5.8,1,2\n");
    auto d = parse_resonance_csv(csv.path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    CHECK(d.curve.size() == 1);
}

TEST_CASE("resonance CSV: no freq or psd_xyz column", "[shaper_csv][belt]") {
    TempCsvFile csv("freq,psd_x,psd_y\n5.0,1,2\n");
    CHECK(parse_resonance_csv(csv.path).error == ResonanceCsvError::NO_PSD_COLUMN);
}
```

- [ ] **Step 2: Run to verify failure**: `make t F='[shaper_csv]'`. Expected: compile error, `parse_resonance_csv` undeclared.

- [ ] **Step 3: Implement.** Rules, in order: file cannot be opened → `MISSING`. Header lacks `freq` → `NO_PSD_COLUMN`. Header has `freq` but no `psd_xyz`: if it has none of `psd_x`/`psd_y`/`psd_z` and more than one other column → `MULTI_CHIP`, else `NO_PSD_COLUMN`. Rows: skip any row with fewer cells than the header or a cell that does not parse as a float (`text_io::parse_leading` or the file's existing float parse; no `std::stof`, it throws). Zero good rows → `EMPTY`. Log each error once with `spdlog::warn("[ShaperCSV] ...: {}", csv_path)`.

- [ ] **Step 4: Run**: `make t F='[shaper_csv]'`. Expected: PASS (old input shaper cases still green).

- [ ] **Step 5: Commit**: `feat(belt): read TEST_RESONANCES CSVs by header name (prestonbrown/helixscreen#1721)`.

---

### Task 4: Shared resonance-tester config and console helpers

`start_resonance_test` queries `configfile.settings.resonance_tester` inline and `InputShaperCollector` owns the sweep-line regex and the CSV-path regex. The belt collector needs all three. Extract them once.

**Files:**
- Create: `include/resonance_console.h`, `src/calibration/resonance_console.cpp`
- Modify: `src/api/moonraker_advanced_api.cpp` (`InputShaperCollector::parse_sweep_line`, `::parse_csv_path`, and the configfile query in `start_resonance_test`)
- Test: `tests/unit/test_resonance_console.cpp`

**Interfaces:**
- Produces (`helix::calibration`):

```cpp
struct ResonanceTesterConfig {
    float min_freq = 5.0f;    ///< Klipper default
    float max_freq = 135.0f;  ///< Klipper default
    float hz_per_sec = 1.0f;  ///< Klipper default
    bool from_printer = false;///< true when read from configfile, false = defaults
    /// Seconds one sweep takes: (max - min) / hz_per_sec.
    [[nodiscard]] float sweep_seconds() const;
};

/// Parse `configfile.settings.resonance_tester` (values may be numbers or
/// strings). Missing keys keep their defaults.
[[nodiscard]] ResonanceTesterConfig parse_resonance_tester_config(const nlohmann::json& settings);

/// "Testing frequency 74 Hz" -> 74. nullopt for any other line.
[[nodiscard]] std::optional<float> parse_testing_frequency(const std::string& line);

/// The path from "<anything> data written to <path>.csv file"; matches both
/// "Shaper calibration data written to" and "Resonances data written to".
[[nodiscard]] std::optional<std::string> parse_written_csv_path(const std::string& line);

/// 0-100 progress of a sweep at `freq`, clamped.
[[nodiscard]] int sweep_percent(float freq, const ResonanceTesterConfig& cfg);
```

- Also produces, in `moonraker_advanced_api.cpp` (file-local): `void query_resonance_tester_config(IMoonrakerClient& client, std::function<void(ResonanceTesterConfig)> on_done);` wrapping the existing `printer.objects.query` for `configfile.settings`; `on_done` receives defaults on any error.

- [ ] **Step 1: Write the failing tests**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "resonance_console.h"

#include "hv/json.hpp"

#include "../catch_amalgamated.hpp"

using namespace helix::calibration;

TEST_CASE("testing-frequency lines parse", "[resonance_console]") {
    CHECK(parse_testing_frequency("Testing frequency 74 Hz") == Catch::Approx(74.0f));
    CHECK(parse_testing_frequency("// Testing frequency 5 Hz") == Catch::Approx(5.0f));
    CHECK_FALSE(parse_testing_frequency("Wait for calculations..").has_value());
}

TEST_CASE("written-to path parses for both Klipper wordings and axis formats",
          "[resonance_console]") {
    CHECK(parse_written_csv_path(
              "Resonances data written to /tmp/resonances_axis=1.000,1.000,0.000_helix_belt_a.csv file")
              .value() == "/tmp/resonances_axis=1.000,1.000,0.000_helix_belt_a.csv");
    CHECK(parse_written_csv_path(
              "Resonances data written to /tmp/resonances_axis=1.000,-1.000_helix_belt_b.csv file")
              .value() == "/tmp/resonances_axis=1.000,-1.000_helix_belt_b.csv");
    CHECK(parse_written_csv_path(
              "Shaper calibration data written to /tmp/calibration_data_x_20260928.csv file")
              .value() == "/tmp/calibration_data_x_20260928.csv");
    CHECK_FALSE(parse_written_csv_path("Testing frequency 74 Hz").has_value());
}

TEST_CASE("resonance_tester config: numbers, strings, missing keys", "[resonance_console]") {
    auto cfg = parse_resonance_tester_config(nlohmann::json::parse(
        R"({"resonance_tester": {"min_freq": 1, "max_freq": "100", "hz_per_sec": 2}})"));
    CHECK(cfg.from_printer);
    CHECK(cfg.min_freq == Catch::Approx(1.0f));
    CHECK(cfg.max_freq == Catch::Approx(100.0f));
    CHECK(cfg.hz_per_sec == Catch::Approx(2.0f));
    CHECK(cfg.sweep_seconds() == Catch::Approx(49.5f));

    auto none = parse_resonance_tester_config(nlohmann::json::object());
    CHECK_FALSE(none.from_printer);
    CHECK(none.sweep_seconds() == Catch::Approx(130.0f));
}

TEST_CASE("sweep percent follows the configured range", "[resonance_console]") {
    ResonanceTesterConfig cfg;
    cfg.min_freq = 1.0f;
    cfg.max_freq = 101.0f;
    CHECK(sweep_percent(51.0f, cfg) == 50);
    CHECK(sweep_percent(0.0f, cfg) == 0);
    CHECK(sweep_percent(500.0f, cfg) == 100);
}
```

- [ ] **Step 2: Run to verify failure**: `make t F='[resonance_console]'`.

- [ ] **Step 3: Implement.** Regexes (`helix::Regex`): `R"(Testing frequency ([\d.]+) Hz)"` and `R"(data written to (\S+\.csv))"`. Config values use `json_util` safe accessors (read the file's existing configfile parse in `start_resonance_test` and move it here, keeping its number-or-string handling). Then make `InputShaperCollector::parse_sweep_line` call `parse_testing_frequency` + `sweep_percent`, `parse_csv_path` call `parse_written_csv_path`, and `start_resonance_test` call `query_resonance_tester_config` feeding `collector->set_sweep_range(cfg.min_freq, cfg.max_freq)` only when `cfg.from_printer`.

- [ ] **Step 4: Run**: `make t F='[resonance_console]'` then `make t F='[input_shaper]'`. Expected: PASS; input shaper behavior unchanged.

- [ ] **Step 5: Commit**: `refactor(calibration): share resonance-tester config and console parsing (prestonbrown/helixscreen#1721)`.

---

### Task 5: Mock `TEST_RESONANCES` as a simulated printer

**Files:**
- Modify: `include/moonraker_client_mock.h`, `src/api/moonraker_client_mock.cpp`, `src/api/moonraker_client_mock_files.cpp` (delete the two `raw_data_belt_path_*` list entries at the `data_store` listing)
- Modify: `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`
- Test: `tests/unit/test_mock_test_resonances.cpp`

**Interfaces:**
- Consumes: `parse_resonance_csv` (Task 3) in tests; `sim_speed().shorten_wait_ms(ms)`; `dispatch_gcode_response(line)`; `calibration_timers_` (existing ownership list for LVGL timers).
- Produces: `enum class BeltMockFailure { NONE, STALL, NOFILE, MULTICHIP, ERROR, KALICO };` at namespace scope in `include/moonraker_client_mock.h` (tests name it unqualified), and public on `MoonrakerClientMock`:

```cpp

/// Current simulated peak for a path ('A' or 'B').
[[nodiscard]] float belt_peak_hz(char path) const;
void set_belt_peaks_hz(float a_hz, float b_hz);
void set_belt_failure(BeltMockFailure f);
/// Milliseconds between "Testing frequency" lines; 0 = derive from hz_per_sec
/// and sim speed. Tests set a small value.
void set_belt_line_interval_ms(uint32_t ms);
/// Path the next TEST_RESONANCES for `name` writes (PID-scoped).
[[nodiscard]] static std::string belt_csv_path(const std::string& axis_name, const std::string& name);
static void remove_belt_csvs();
```

- Env vars (read once in the mock constructor): `HELIX_MOCK_BELT_A_HZ` (default `110`), `HELIX_MOCK_BELT_B_HZ` (default `98`), `HELIX_MOCK_BELT_FAIL` (`stall|nofile|multichip|error|kalico`, default none). Defaults deliberately start at "Adjust needed" so the loop is visible.

**Behavior:**
- Intercept in `gcode_script` next to the `SHAPER_CALIBRATE` block: `if (gcode.find("TEST_RESONANCES") != npos) dispatch_test_resonances_response(gcode);`. Parse `AXIS=` (`1,1` → path A, `1,-1` → path B; anything else → emit `!! Unsupported axis` and return) and `NAME=`.
- Axis name in lines/paths: mainline `axis=1.000,1.000,0.000` / `axis=1.000,-1.000,0.000`; under `KALICO`, `axis=1.000,1.000` / `axis=1.000,-1.000`.
- Tuning loop: keep `belt_measure_count_[2]`. On every measurement of a path after its first, move that path's peak toward the other path's current peak by `min(4.0f, gap)`.
- Transcript per path, played on one `lv_timer` registered in `calibration_timers_`: one `Testing frequency %.0f Hz` per whole Hz from `resonance_min_freq_` to `resonance_max_freq_`, interval `set_belt_line_interval_ms` value or else `sim_speed().shorten_wait_ms(1000.0 / resonance_hz_per_sec_)`. Then write the CSV and emit `Resonances data written to <path> file`.
- CSV: header `freq,psd_x,psd_y,psd_z,psd_xyz` (+`,accel_per_hz` under `KALICO`, with `60.0` in every row); bins from `5.0` to `resonance_max_freq_` step `3200.0/4096`; `psd_xyz = floor + main + secondary` with main Lorentzian at the path's peak (height `3e4`, half-width `5`), secondary at `42 Hz` (height `0.22 * 3e4`, half-width `4.5`), floor `150` plus `std::mt19937 rng(7 + path)` noise of ±10%; `psd_x/psd_y/psd_z` = `0.45/0.45/0.10` of `psd_xyz`. Format `%.1f` / `%.3e` like Klipper.
- Failures: `STALL` stops emitting at the sweep midpoint and never writes; `NOFILE` emits the terminal line without writing (and deletes any existing file at that path); `MULTICHIP` writes header `freq,adxl345,adxl345_hotend`; `ERROR` emits `!! Invalid adxl345 id (got 0 vs e5).` after 3 lines and stops.
- Add `resonance_hz_per_sec_` (default `1.0`) beside `resonance_min_freq_`/`max_freq_`, extend `set_resonance_sweep_range(min, max)` with an overload `set_resonance_sweep_range(min, max, hz_per_sec)`, and serve it in the configfile reply the same way min/max are served.

- [ ] **Step 1: Write the failing tests**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#include "moonraker_client_mock.h"
#include "resonance_console.h"
#include "shaper_csv_parser.h"

#include "hv/json.hpp"
#include "lvgl/lvgl.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "../lvgl_test_fixture.h"

using namespace helix::calibration;

namespace {

struct BeltMockFixture : LVGLTestFixture {
    MoonrakerClientMock mock{MoonrakerClientMock::PrinterType::VORON_24};
    std::vector<std::string> lines;
    BeltMockFixture() {
        mock.set_belt_line_interval_ms(1);
        mock.register_method_callback("notify_gcode_response", "belt_mock_test",
                                      [this](const nlohmann::json& msg) {
                                          lines.push_back(msg["params"][0].get<std::string>());
                                      });
    }
    ~BeltMockFixture() override { MoonrakerClientMock::remove_belt_csvs(); }
    std::optional<std::string> run(const std::string& gcode) {
        lines.clear();
        mock.gcode_script(gcode);
        for (int i = 0; i < 2000; ++i) {
            lv_tick_inc(2);
            lv_timer_handler();
            if (!lines.empty() && parse_written_csv_path(lines.back()))
                return parse_written_csv_path(lines.back());
        }
        return std::nullopt;
    }
};

} // namespace

TEST_CASE_METHOD(BeltMockFixture, "mock sweep emits Klipper's exact lines",
                 "[belt][mock]") {
    auto path = run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
    REQUIRE(path);
    CHECK(lines.front() == "Testing frequency 5 Hz");
    CHECK(lines[lines.size() - 2] == "Testing frequency 135 Hz");
    CHECK(lines.back().rfind("Resonances data written to /tmp/resonances_axis=1.000,1.000,0.000_", 0) == 0);
    auto d = parse_resonance_csv(*path);
    REQUIRE(d.error == ResonanceCsvError::NONE);
    auto peak = find_peak_frequency(d.curve, 20.0f, d.curve.back().first);
    CHECK(peak.frequency == Catch::Approx(mock.belt_peak_hz('A')).margin(1.0f));
}

TEST_CASE_METHOD(BeltMockFixture, "re-testing a path walks it toward the other",
                 "[belt][mock]") {
    mock.set_belt_peaks_hz(110.0f, 98.0f);
    run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(110.0f)); // first measurement: no move
    run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(106.0f));
    run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
    run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
    CHECK(mock.belt_peak_hz('A') == Catch::Approx(98.0f)); // never overshoots
}

TEST_CASE_METHOD(BeltMockFixture, "mock sweep follows the configured range", "[belt][mock]") {
    mock.set_resonance_sweep_range(10.0, 60.0, 2.0);
    run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_b");
    CHECK(lines.front() == "Testing frequency 10 Hz");
    CHECK(lines[lines.size() - 2] == "Testing frequency 60 Hz");
}

TEST_CASE_METHOD(BeltMockFixture, "mock failure modes", "[belt][mock]") {
    SECTION("kalico: two-part axis name and extra column") {
        mock.set_belt_failure(BeltMockFailure::KALICO);
        auto path = run("TEST_RESONANCES AXIS=1,-1 OUTPUT=resonances NAME=helix_belt_b");
        REQUIRE(path);
        CHECK(path->find("axis=1.000,-1.000_helix_belt_b") != std::string::npos);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::NONE);
    }
    SECTION("multichip") {
        mock.set_belt_failure(BeltMockFailure::MULTICHIP);
        auto path = run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
        REQUIRE(path);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::MULTI_CHIP);
    }
    SECTION("nofile") {
        mock.set_belt_failure(BeltMockFailure::NOFILE);
        auto path = run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a");
        REQUIRE(path);
        CHECK(parse_resonance_csv(*path).error == ResonanceCsvError::MISSING);
    }
    SECTION("stall never reports a file") {
        mock.set_belt_failure(BeltMockFailure::STALL);
        CHECK_FALSE(run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a"));
    }
    SECTION("error emits a !! line") {
        mock.set_belt_failure(BeltMockFailure::ERROR);
        CHECK_FALSE(run("TEST_RESONANCES AXIS=1,1 OUTPUT=resonances NAME=helix_belt_a"));
        CHECK(lines.back().rfind("!! ", 0) == 0);
    }
}
```

If `register_method_callback` on the mock needs a different handler signature, match the one `test_moonraker_api_input_shaper.cpp` relies on. If the mock's `PrinterType` enum lives elsewhere, use the spelling that file uses.

- [ ] **Step 2: Run to verify failure**: `make t F='[belt][mock]'`.
- [ ] **Step 3: Implement** the behavior above.
- [ ] **Step 4: Run**: `make t F='[belt][mock]'` and `make t F='[input_shaper]'`. Expected: PASS.
- [ ] **Step 5: Document** the three env vars in `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`, one `### \`HELIX_MOCK_BELT_A_HZ\`` (etc.) heading each with the file's `| Property | Value |` table (**Values**, **Default**, **File** `src/api/moonraker_client_mock.cpp`) and a ```bash usage block, e.g. `HELIX_MOCK_BELT_A_HZ=104 HELIX_MOCK_BELT_B_HZ=102 ./build/bin/helix-screen --test --sim-speed 6 -vv`.
- [ ] **Step 6: Commit**: `feat(mock): simulate TEST_RESONANCES with a tunable belt model (prestonbrown/helixscreen#1721)`.

---

### Task 6: `BeltResonanceCollector` and the new `test_belt_resonance`

**Files:**
- Modify: `include/i_moonraker_sub_apis.h`, `include/moonraker_advanced_api.h`, `src/api/moonraker_advanced_api.cpp`
- Modify: `tests/unit/test_interface_drift_moonraker_api.cpp` (drop the `BELT_TENSION_TIMEOUT_MS` equality)
- Test: `tests/unit/test_moonraker_api_belt_resonance.cpp`

**Interfaces:**
- Consumes: `CalibrationCollectorCore`, `report_collector_rpc_error`, `query_resonance_tester_config`, `parse_testing_frequency`, `sweep_percent`, `parse_written_csv_path` (Task 4), `parse_resonance_csv` (Task 3).
- Produces, in `IAdvancedAPI` and `MoonrakerAdvancedAPI`:

```cpp
/// (percent 0-100, current sweep frequency Hz). Fires off the LVGL thread.
using BeltSweepProgressCallback = std::function<void(int percent, float freq_hz)>;
/// Fires off the LVGL thread with the parsed curve.
using BeltCurveCallback = std::function<void(const helix::calibration::BeltCurve& curve)>;
/// Stops listening and suppresses every later callback. Idempotent.
using BeltRunCancel = std::function<void()>;

/// TEST_RESONANCES AXIS=<axis_param> OUTPUT=resonances NAME=<output_name>.
/// Completes on Klipper's "Resonances data written to" line; there is no
/// overall deadline (the caller owns stall detection and cancels).
[[nodiscard]] virtual BeltRunCancel test_belt_resonance(const std::string& axis_param,
                                                        const std::string& output_name,
                                                        BeltSweepProgressCallback on_progress,
                                                        BeltCurveCallback on_complete,
                                                        ErrorCallback on_error) = 0;
```

- Removes: `excite_belt_at_frequency`, `download_accel_csv`, `BeltResonanceCallback`, `BELT_TENSION_TIMEOUT_MS` (from both headers, the `.cpp`, and the drift test).

**Collector rules** (`class BeltResonanceCollector : public std::enable_shared_from_this<...>` in `moonraker_advanced_api.cpp`, beside `InputShaperCollector`):
- `core_(client, "belt_resonance_collector_")`, `start()` registers via `core_.start(self, ...)` exactly like `InputShaperCollector::start`.
- Per line: `!! ` prefix, `Error: ` prefix, or `Unknown command` + `TEST_RESONANCES` → `complete_error(line)`. `parse_testing_frequency` → `on_progress(sweep_percent(freq, cfg_), freq)`. `parse_written_csv_path` → read with `parse_resonance_csv`; `NONE` → `complete_success(curve)`, otherwise `complete_error` with a message per error (`MISSING`: "Klipper reported {path} but it cannot be read here. HelixScreen must run on the printer's own computer."; `MULTI_CHIP`: "More than one accelerometer reported. Belt Tension supports one."; `EMPTY`/`NO_PSD_COLUMN`: "Klipper's results file {path} has no usable data.").
- Only lines arriving after this collector started count; the file is read only from a path announced to this collector (Review Focus 4).
- `cancel()`: `core_.mark_completed(); core_.unregister();` then never call back.
- `complete_*` pin `auto keepalive = shared_from_this();` and gate on `core_.try_complete()` like `InputShaperCollector`.
- `test_belt_resonance`: create collector, `start()`, `query_resonance_tester_config(client_, [collector](cfg){ collector->set_config(cfg); })`, then `api_.execute_gcode(fmt::format("TEST_RESONANCES AXIS={} OUTPUT=resonances NAME={}", axis_param, output_name), [](){}, [collector, on_error](const MoonrakerError& err){ report_collector_rpc_error("TEST_RESONANCES", get_printer_state(), collector, on_error, err, 0); }, /*timeout_ms=*/0)`. Return `[weak = std::weak_ptr(collector)]() { if (auto c = weak.lock()) c->cancel(); }`. Check `report_collector_rpc_error`'s handling of `backstop_ms == 0`; if 0 is not "no backstop", pass the smallest value meaning none per its code and say so in a comment.

- [ ] **Step 1: Write the failing tests** (fixture copied from `InputShaperTestFixture` in `test_moonraker_api_input_shaper.cpp`: `MoonrakerClientMock`, `PrinterState` with `init_subjects(false)` + `set_klippy_state_sync(KlippyState::READY)`, `MoonrakerAPI`; dtor drains `UpdateQueueTestAccess::drain_all` and calls `MoonrakerClientMock::remove_belt_csvs()`; ctor calls `mock_client_.set_belt_line_interval_ms(1)`)

```cpp
TEST_CASE_METHOD(BeltApiFixture, "test_belt_resonance delivers the parsed curve",
                 "[belt][api]") {
    std::atomic<bool> done{false};
    BeltCurve got;
    int last_percent = -1;
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_a", [&](int pct, float) { last_percent = pct; },
        [&](const BeltCurve& c) { got = c; done = true; },
        [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    REQUIRE(done);
    CHECK(last_percent == 100);
    auto peak = find_peak_frequency(got, 20.0f, got.back().first);
    CHECK(peak.frequency == Catch::Approx(mock_client_.belt_peak_hz('A')).margin(1.0f));
}

TEST_CASE_METHOD(BeltApiFixture, "an error line fails the run", "[belt][api]") {
    mock_client_.set_belt_failure(BeltMockFailure::ERROR);
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_a", nullptr, [&](const BeltCurve&) { FAIL("no curve expected"); },
        [&](const MoonrakerError& e) {
            CHECK(e.message.find("adxl345") != std::string::npos);
            failed = true;
        });
    pump_until(failed);
    CHECK(failed);
}

TEST_CASE_METHOD(BeltApiFixture, "a stale file from an earlier run is never read",
                 "[belt][api]") {
    // Leave a valid file where this run's output would go, then fail the run.
    const auto stale = MoonrakerClientMock::belt_csv_path("axis=1.000,1.000,0.000", "helix_belt_a");
    { std::ofstream(stale) << "freq,psd_x,psd_y,psd_z,psd_xyz\n50.0,1,1,1,3\n"; }
    mock_client_.set_belt_failure(BeltMockFailure::ERROR);
    std::atomic<bool> failed{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_a", nullptr, [&](const BeltCurve&) { FAIL("stale file read"); },
        [&](const MoonrakerError&) { failed = true; });
    pump_until(failed);
    CHECK(failed);
}

TEST_CASE_METHOD(BeltApiFixture, "missing and multi-chip files become errors", "[belt][api]") {
    auto expect_error_containing = [&](BeltMockFailure f, const char* needle) {
        mock_client_.set_belt_failure(f);
        std::atomic<bool> failed{false};
        auto cancel = api_->advanced().test_belt_resonance(
            "1,1", "helix_belt_a", nullptr, [&](const BeltCurve&) { FAIL("no curve"); },
            [&](const MoonrakerError& e) {
                CHECK(e.message.find(needle) != std::string::npos);
                failed = true;
            });
        pump_until(failed);
        CHECK(failed);
    };
    expect_error_containing(BeltMockFailure::NOFILE, "printer's own computer");
    expect_error_containing(BeltMockFailure::MULTICHIP, "More than one accelerometer");
}

TEST_CASE_METHOD(BeltApiFixture, "cancel suppresses every later callback", "[belt][api]") {
    bool called = false;
    auto cancel = api_->advanced().test_belt_resonance(
        "1,1", "helix_belt_a", [&](int, float) { called = true; },
        [&](const BeltCurve&) { called = true; }, [&](const MoonrakerError&) { called = true; });
    cancel();
    cancel(); // idempotent
    for (int i = 0; i < 500; ++i) { lv_tick_inc(2); lv_timer_handler_safe(); }
    CHECK_FALSE(called);
}

TEST_CASE_METHOD(BeltApiFixture, "progress follows the printer's configured range",
                 "[belt][api]") {
    mock_client_.set_resonance_sweep_range(10.0, 60.0, 2.0);
    std::vector<float> freqs;
    std::atomic<bool> done{false};
    auto cancel = api_->advanced().test_belt_resonance(
        "1,-1", "helix_belt_b", [&](int pct, float f) { if (freqs.empty()) CHECK(pct == 0); freqs.push_back(f); },
        [&](const BeltCurve&) { done = true; }, [&](const MoonrakerError& e) { FAIL(e.message); });
    pump_until(done);
    REQUIRE_FALSE(freqs.empty());
    CHECK(freqs.front() == Catch::Approx(10.0f));
    CHECK(freqs.back() == Catch::Approx(60.0f));
}
```

`pump_until(flag)` = the `lv_tick_inc(100); lv_timer_handler_safe(); sleep 5ms` loop from the input shaper test, bounded at 2000 iterations. The configfile reply may land after the first progress lines; if the "range" test shows the first percent computed against defaults, have the collector hold progress until `set_config` arrives or the first 3 lines pass, and keep the test.

- [ ] **Step 2: Run to verify failure**: `make t F='[belt][api]'`.
- [ ] **Step 3: Implement** per the rules above; delete the removed API.
- [ ] **Step 4: Run**: `make t F='[belt][api]'`, `make t F='[drift]'`, `make t F='[input_shaper]'`. Expected: PASS.
- [ ] **Step 5: Commit**: `feat(belt): run TEST_RESONANCES OUTPUT=resonances and read the result locally (prestonbrown/helixscreen#1721)`.

---

### Task 7: Slim `BeltTensionCalibrator` to one path measurement

**Files:**
- Modify: `include/belt_tension_calibrator.h`, `src/calibration/belt_tension_calibrator.cpp`
- Test: `tests/unit/test_belt_tension_calibrator.cpp`

**Interfaces:**
- Consumes: `test_belt_resonance` + `BeltRunCancel` (Task 6), `helix::ensure_homed_then(IMoonrakerAPI*, AsyncLifetimeGuard&, std::function<void()>, std::function<void(const MoonrakerError&)>)`, `helix::emergency_stop_and_restart` (Task 1).
- Produces:

```cpp
enum class BeltPath { PATH_A, PATH_B };

class BeltTensionCalibrator {
  public:
    enum class State { IDLE, DETECTING_HARDWARE, HOMING, MEASURING, ERROR };
    explicit BeltTensionCalibrator(IMoonrakerAPI* api);
    void detect_hardware(BeltHardwareDetectCallback on_complete, BeltErrorCallback on_error);
    /// Home if needed, then sweep one path. Callbacks run on the LVGL thread.
    void measure_path(BeltPath path, std::function<void(int percent, float freq_hz)> on_progress,
                      std::function<void(BeltCurve)> on_complete, BeltErrorCallback on_error);
    /// Stop listening to the running sweep. The printer keeps sweeping.
    void cancel();
    /// Stop the printer: cancel() then emergency_stop_and_restart().
    void emergency_abort();
    void reset();
    [[nodiscard]] State get_state() const;
    [[nodiscard]] static const char* axis_param(BeltPath p); // "1,1" / "1,-1"
    [[nodiscard]] static const char* output_name(BeltPath p); // "helix_belt_a" / "helix_belt_b"
  private:
    std::atomic<State> state_{State::IDLE};
    IMoonrakerAPI* api_ = nullptr;
    BeltTensionHardware hardware_;
    BeltRunCancel run_cancel_;
    helix::AsyncLifetimeGuard lifetime_;
};
```

`measure_path` marshals the API's off-thread callbacks to the LVGL thread with `lifetime_.bg_cb(...)`. `reset()` and the destructor call `cancel()`.

- [ ] **Step 1: Write the failing tests** (replace the deleted sweep cases)

```cpp
TEST_CASE("belt path names match Klipper's axis syntax", "[belt_tension][calibrator]") {
    CHECK(std::string(BeltTensionCalibrator::axis_param(BeltPath::PATH_A)) == "1,1");
    CHECK(std::string(BeltTensionCalibrator::axis_param(BeltPath::PATH_B)) == "1,-1");
    CHECK(std::string(BeltTensionCalibrator::output_name(BeltPath::PATH_A)) == "helix_belt_a");
    CHECK(std::string(BeltTensionCalibrator::output_name(BeltPath::PATH_B)) == "helix_belt_b");
}
```

and, in a fixture built like Task 6's (`MoonrakerAPI` over the mock, homed printer), `measure_path(PATH_B, ...)` delivers a curve whose peak is within 1 Hz of `belt_peak_hz('B')`; `cancel()` right after `measure_path` means neither callback fires after 500 pumped ticks.

- [ ] **Step 2-4:** run `make t F='[calibrator]'` failing, implement, run passing (`make t F='[belt]'` too).
- [ ] **Step 5: Commit**: `refactor(belt): the calibrator measures one path per call (prestonbrown/helixscreen#1721)`.

---

### Task 8: Chart series styles, cursor and tier rules

**Files:**
- Modify: `include/ui_frequency_response_chart.h`, `src/ui/ui_frequency_response_chart.cpp`
- Test: `tests/unit/test_frequency_response_chart_style.cpp`

**Interfaces:**
- Produces:

```cpp
struct FrChartSeriesStyle {
    int32_t line_width = 2;
    bool glow = false; ///< two wider, fainter strokes under the line
    bool fill = false; ///< vertical gradient from 35% of the series color to transparent
};

/// Pure: what a tier may draw. EMBEDDED never reaches here (no chart);
/// BASIC drops glow; STANDARD with animations keeps everything.
[[nodiscard]] FrChartSeriesStyle fr_chart_effective_style(const FrChartSeriesStyle& requested,
                                                          helix::PlatformTier tier,
                                                          bool supports_animations);

void ui_frequency_response_chart_set_series_style(ui_frequency_response_chart_t* chart,
                                                   int series_id, const FrChartSeriesStyle& style);
[[nodiscard]] FrChartSeriesStyle ui_frequency_response_chart_get_series_style(
    ui_frequency_response_chart_t* chart, int series_id);

/// Vertical cursor at freq_hz with the swept region [range min, freq_hz] tinted.
void ui_frequency_response_chart_set_cursor(ui_frequency_response_chart_t* chart, float freq_hz,
                                            lv_color_t color);
void ui_frequency_response_chart_clear_cursor(ui_frequency_response_chart_t* chart);
```

The chart stores the tier it was configured with (`configure_for_platform`) plus `supports_animations` read via `helix::PlatformCapabilities::detect()` once at configure time; `set_series_style` stores `fr_chart_effective_style(requested, tier, anim)`. Glow and fill are drawn in the chart's existing draw-post pass (where muted series are drawn): glow = the series polyline at `width + 6` / opa 25 then `width + 3` / opa 60 before the line; fill = one quad per segment down to the plot bottom, split into two triangles, drawn with the LVGL 9.5 triangle draw descriptor's gradient (check the field names in `lib/lvgl/src/draw/lv_draw_triangle.h`; do not edit `lib/lvgl`). Cursor: a 2 px vertical line in `color` plus a rect from the left plot edge to the cursor at opa 40.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("chart style: STANDARD with animations keeps glow and fill", "[chart][belt]") {
    FrChartSeriesStyle req{3, true, true};
    auto s = fr_chart_effective_style(req, helix::PlatformTier::STANDARD, true);
    CHECK(s.glow);
    CHECK(s.fill);
    CHECK(s.line_width == 3);
}

TEST_CASE("chart style: no animations or BASIC drops glow, keeps fill", "[chart][belt]") {
    FrChartSeriesStyle req{3, true, true};
    CHECK_FALSE(fr_chart_effective_style(req, helix::PlatformTier::STANDARD, false).glow);
    auto basic = fr_chart_effective_style(req, helix::PlatformTier::BASIC, true);
    CHECK_FALSE(basic.glow);
    CHECK(basic.fill);
}

TEST_CASE_METHOD(LVGLTestFixture, "chart stores the effective style per series", "[chart][belt]") {
    auto* chart = ui_frequency_response_chart_create(lv_screen_active());
    ui_frequency_response_chart_configure_for_platform(chart, helix::PlatformTier::BASIC);
    int id = ui_frequency_response_chart_add_series(chart, "A", lv_color_hex(0x4FA3F7));
    ui_frequency_response_chart_set_series_style(chart, id, {3, true, true});
    CHECK_FALSE(ui_frequency_response_chart_get_series_style(chart, id).glow);
    ui_frequency_response_chart_set_cursor(chart, 74.0f, lv_color_hex(0xF2994A));
    ui_frequency_response_chart_clear_cursor(chart);
    ui_frequency_response_chart_destroy(chart);
}
```

- [ ] **Step 2-4:** `make t F='[chart]'` failing, implement, passing. Then `make t F='[input_shaper]'` (the input shaper chart must look and behave the same: its series keep the default style).
- [ ] **Step 5: Look at it.** Build the app, run the local mock with a pinned socket (CLAUDE.md Quick Start box), `ctl navigate` to the input shaper panel's results via its demo (`INPUT_SHAPER_DEMO_*`, see `MOCK_ENVIRONMENT_VARIABLES.md`) and screenshot; the chart must be unchanged.
- [ ] **Step 6: Commit**: `feat(chart): per-series glow and fill, a sweep cursor, tier-gated (prestonbrown/helixscreen#1721)`.

---

### Task 9: Rebuild the Belt Tension panel

**Files:**
- Rewrite: `include/ui_panel_belt_tension.h`, `src/ui/ui_panel_belt_tension.cpp`, `ui_xml/panel_belt_tension.xml`
- Delete: `ui_xml/components/belt_result_card.xml` and its `register_xml("components/belt_result_card.xml")` in `src/xml_registration.cpp`; `src/ui/ui_belt_trace.cpp` + its header + `register_belt_trace_widget()`; `ui_pluck_animation` widget (`include/ui_pluck_animation.h`, its `.cpp`, `register_pluck_animation_widget()`); the `dsp_capable` leg of `evaluate_belt_gate` and `BeltGate::HARDWARE_TOO_SLOW`; `park_y_for_span`, `park_x_center`, `axis_center`, `TARGET_SPAN_MM`, `ParkTarget` from `belt_gating.*`; `PrinterDetector::get_belt_span_offset_mm`. Update `tests/unit/test_belt_gating.cpp` accordingly (drop the dsp and park cases, keep the rest). Grep each deleted name across `src include tests ui_xml` and remove every use.
- Create: `include/ui_belt_path_sketch.h`, `src/ui/ui_belt_path_sketch.cpp`: a `<belt_path_sketch>` XML widget (registered in `src/xml_registration.cpp` before `panel_belt_tension.xml`, following how `register_belt_trace_widget()` was registered) that draws, in a draw event, a rounded frame with "BACK" at the top, a toolhead square in the centre, and two double-headed diagonal arrows: +X+Y in `belt_path_a` labelled "A · 1,1", +X-Y in `belt_path_b` labelled "B · 1,-1". Colors come from the same theme tokens as the chart; labels go through `lv_tr`. It draws no motors or belt routing (spec: it names paths, not belts).
- Modify: `Makefile` comment above `HELIX_HAS_BELT_TUNER` to: `# Compile-out gate for the Belt Tension panel. It reads Klipper's /tmp results, so it only works co-located with Klipper.`
- Test: `tests/unit/test_belt_tension_panel_states.cpp` (rewrite)

**Interfaces:**
- Consumes: Tasks 2, 6 (via 7), 7, 8. `lifetime_` from `OverlayBase` (`lifetime_.bg_cb(name, fn)`), `OperationTimeoutGuard` (`begin(ms, fn)`, `end()`, `is_active()`), `helix::PlatformCapabilities::detect()`.
- Produces:

```cpp
class BeltTensionPanel : public OverlayBase {
  public:
    enum class ViewState { START = 0, RUNNING = 1, RESULTS = 2, ERROR = 3 };
    /// No console line for this long during a run = stalled. Covers Klipper's
    /// post-sweep analysis, which prints nothing.
    static constexpr uint32_t STALL_TIMEOUT_MS = 120000;
    // OverlayBase overrides as today: init_subjects, deinit_subjects, create, get_name,
    // on_activate, on_deactivating, cleanup, on_ui_destroyed; show(); set_api(...).
    void handle_start_clicked();              // Test both
    void handle_retest_clicked(BeltPath path);
    void handle_stop_clicked();               // confirm, then calibrator_->emergency_abort()
    void handle_retry_clicked();
    /// Tests: pick the render tier instead of detecting it. Call before create().
    void set_render_tier_for_test(helix::PlatformTier tier, bool animations);
  private:
    struct PathRun { BeltCurve curve; BeltCurve previous; bool has = false; bool has_previous = false; uint32_t measured_at_ms = 0; };
    PathRun runs_[2];
    std::vector<BeltPath> queue_; // paths left in this run
    OperationTimeoutGuard stall_guard_;
    std::unique_ptr<helix::calibration::BeltTensionCalibrator> calibrator_;
    ui_frequency_response_chart_t* chart_ = nullptr;
    int series_[2] = {-1, -1};
    int ghost_series_[2] = {-1, -1};
    // subjects: see table below
};
```

**Subjects** (all via `UI_MANAGED_SUBJECT_*` into `subjects_`, names are the XML contract):

| Subject | Type | Meaning |
|---|---|---|
| `belt_tension_state` | int | `ViewState` |
| `bt_can_start` | int | gate open |
| `bt_gate_message` | string | gate text when closed |
| `bt_hw_kinematics`, `bt_hw_accel`, `bt_hw_sweep` | string | START rows; sweep = "5-135 Hz · about 4 min" from `ResonanceTesterConfig` (fetched on activate via the same configfile query) |
| `bt_run_title`, `bt_run_detail` | string | "Measuring Path B", "2 of 2 · 2:41 elapsed" / "Re-measuring Path B" with detail "1:12 elapsed" |
| `bt_running_path` | int | 0 A, 1 B (colors the cursor and the "sweeping" label) |
| `bt_peak_a`, `bt_peak_b` | string | "104" or "--" |
| `bt_note_a`, `bt_note_b` | string | "just now · was 110", "3 min ago", "done", "sweeping" |
| `bt_verdict` | int | 0 matched, 1 close, 2 adjust (drives chip color via XML binds) |
| `bt_verdict_text` | string | "Well matched" / "Close" / "Adjust needed" |
| `bt_facts` | string | "6 Hz apart · 91% similar" |
| `bt_rail_value` | int | signed delta in tenths of Hz clamped to ±150 |
| `bt_chart_available` | int | 0 on EMBEDDED (hides chart containers) |
| `bt_error_message` | string | ERROR text |

**Flow rules:**
- Start → queue `{A, B}`; Re-test X → queue `{X}` and keep the other path's run. Before measuring X, move `runs_[X].curve` into `.previous` (only for a single-path re-test; Test both clears both `.previous`).
- Each measurement: `set_view_state(RUNNING)`, `stall_guard_.begin(STALL_TIMEOUT_MS, on_stall)`; on every progress callback `stall_guard_.end(); stall_guard_.begin(...)`, set the chart cursor at `freq_hz` in the path color and update title/detail. A 1 s `lv_timer` updates elapsed time while RUNNING only.
- `on_stall`: `calibrator_->cancel()`, ERROR "Klipper stopped reporting progress. Check the printer, then try again."
- On a curve: store it, pop the queue, run the next or go to RESULTS. RESULTS computes `compare_belt_paths(runs_[0].curve, runs_[1].curve)`; `!valid` → ERROR "No resonance peak found for Path A" (or B) naming the path(s) without one.
- On error: `stall_guard_.end()`, ERROR with the message.
- Stop: `modal_confirm(lv_tr("Stop the check?"), lv_tr("This is an emergency stop: the printer halts and Klipper restarts."), ...)`; on confirm `calibrator_->emergency_abort()`, back to START. Pass `on_dismiss` that does nothing (no guard state to clear).
- `on_deactivating` and `cleanup`: `calibrator_->cancel()`, `stall_guard_.end()`, kill the elapsed timer, set START. The next `on_activate` re-evaluates the gate (Review Focus 1, 5).
- Gate: existing observers (accelerometer, print active, connected) plus the klippy-socket co-location probe; drop `dsp_capable`. `bt_can_start` also requires `calibrator_->get_state() == IDLE`.
- Chart (skip entirely when `bt_chart_available == 0`): created in the RESULTS and RUNNING containers' `chart_host` object; tier from `PlatformCapabilities::detect()` unless `set_render_tier_for_test` ran. Series A/B styled `{3, true, true}`; ghosts added muted with `set_series_muted(..., true)` and shown only when `.has_previous`; peaks marked with `mark_peak`. Colors come from theme tokens: look up the two path colors with the theme accessor the XML token system uses (`theme_manager` lookup of `belt_path_a` / `belt_path_b`); add those two tokens to `config/themes/*.json` for every theme (dark and light) with A a blue and B an orange that meet each theme's contrast.

**XML skeleton** (`ui_xml/panel_belt_tension.xml`, tokens not hex; keep `header_bar` as today):

```xml
<view name="panel_belt_tension" extends="lv_obj" width="#overlay_width_transient" height="100%" align="right_mid">
  <header_bar name="overlay_header" title="Belt Tension" title_tag="Belt Tension"/>
  <lv_obj name="panel_content" width="100%" flex_grow="1" style_pad_all="0" scrollable="false">
    <lv_obj name="state_start" width="100%" height="100%" flex_flow="column">
      <bind_flag_if_not_eq subject="belt_tension_state" flag="hidden" ref_value="0"/>
      <!-- lv_obj row: <belt_path_sketch name="bt_sketch"/> + title,
           explanation, three rows bound to bt_hw_kinematics / bt_hw_accel / bt_hw_sweep,
           gate row bound to bt_gate_message and hidden when bt_can_start eq 1 -->
      <ui_button name="btn_start" text="Start check" translation_tag="Start check">
        <bind_state_if_eq subject="bt_can_start" state="disabled" ref_value="0"/>
        <event_cb trigger="clicked" callback="belt_tension_start_cb"/>
      </ui_button>
    </lv_obj>
    <lv_obj name="state_running" width="100%" height="100%">
      <bind_flag_if_not_eq subject="belt_tension_state" flag="hidden" ref_value="1"/>
      <lv_obj name="chart_host_running" width="100%" height="100%"><bind_flag_if_eq subject="bt_chart_available" flag="hidden" ref_value="0"/></lv_obj>
      <!-- hero row: bt_peak_a / bt_note_a at left, bt_run_title / bt_run_detail centre, bt_peak_b / bt_note_b right -->
      <ui_button name="btn_stop" text="Stop" translation_tag="Stop"><event_cb trigger="clicked" callback="belt_tension_stop_cb"/></ui_button>
    </lv_obj>
    <lv_obj name="state_results" width="100%" height="100%">
      <bind_flag_if_not_eq subject="belt_tension_state" flag="hidden" ref_value="2"/>
      <lv_obj name="chart_host_results" width="100%" height="100%"><bind_flag_if_eq subject="bt_chart_available" flag="hidden" ref_value="0"/></lv_obj>
      <!-- hero row with verdict chip (bt_verdict_text, color via bind on bt_verdict) and bt_facts -->
      <!-- rail: lv_slider name="bt_rail" range -150..150 bound to bt_rail_value, not clickable,
           knob = marker; behind it two centred bands, width 53% (close) and 20% (matched),
           in warning/success tokens; "◀ B tighter" / "matched" / "A tighter ▶" labels -->
      <lv_obj name="results_buttons" flex_flow="row" width="100%">
        <ui_button name="btn_retest_a" text="Re-test A" translation_tag="Re-test A"><event_cb trigger="clicked" callback="belt_tension_retest_a_cb"/></ui_button>
        <ui_button name="btn_retest_b" text="Re-test B" translation_tag="Re-test B"><event_cb trigger="clicked" callback="belt_tension_retest_b_cb"/></ui_button>
        <ui_button name="btn_test_both" text="Test both" translation_tag="Test both"><event_cb trigger="clicked" callback="belt_tension_start_cb"/></ui_button>
      </lv_obj>
    </lv_obj>
    <lv_obj name="state_error" width="100%" height="100%" flex_flow="column">
      <bind_flag_if_not_eq subject="belt_tension_state" flag="hidden" ref_value="3"/>
      <lv_label name="error_label" bind_text="bt_error_message"/>
      <ui_button name="btn_retry" text="Retry" translation_tag="Retry"><event_cb trigger="clicked" callback="belt_tension_retry_cb"/></ui_button>
    </lv_obj>
  </lv_obj>
</view>
```

The rail's band widths are the verdict constants over the ±15 Hz rail (3/15 → 20%, 8/15 → 53%); a comment beside them in the XML says so, and a test (below) pins the constants so a threshold change fails loudly until the XML follows. Small screens (480×272 breakpoint): use the project's layout override mechanism (`docs/devel/UI_CONTRIBUTOR_GUIDE.md` § layout overrides) to move the verdict chip into the header, shrink the hero fonts one step, hide the rail, and shorten "Test both" to "Both". Hero numbers use the largest existing font token (no text shadow: LVGL labels have none, so the halo from the mockup is not built).

- [ ] **Step 1: Write the failing panel tests** (rewrite `test_belt_tension_panel_states.cpp`, same `TEST_CASE_METHOD(XMLTestFixture, ...)` shape and `build_belt_panel` helper minus the deleted components; add a helper that wires `MoonrakerClientMock` + `MoonrakerAPIMock` like `test_queue_update_lifetime.cpp`'s `ScopedPanelApi`, sets `set_belt_line_interval_ms(1)`, homes the mock, and pumps until a subject reaches a value):

```cpp
TEST_CASE_METHOD(XMLTestFixture, "belt: Start runs A then B and lands on RESULTS",
                 "[belt][panel]") {
    auto h = BeltPanelHarness(*this); // builds panel, wires mock API, gate open
    h.mock().set_belt_peaks_hz(104.0f, 98.0f);
    h.panel().handle_start_clicked();
    CHECK(h.state() == BeltTensionPanel::ViewState::RUNNING);
    h.pump_until_state(BeltTensionPanel::ViewState::RESULTS);
    CHECK(h.text("bt_peak_a") == "104");
    CHECK(h.text("bt_peak_b") == "98");
    CHECK(h.integer("bt_verdict") == 1); // 6 Hz apart = close
    CHECK(h.integer("bt_rail_value") == Catch::Approx(60).margin(8));
}

TEST_CASE_METHOD(XMLTestFixture, "belt: Re-test A keeps B and ghosts the old A",
                 "[belt][panel]") {
    auto h = BeltPanelHarness(*this);
    h.mock().set_belt_peaks_hz(110.0f, 98.0f);
    h.panel().handle_start_clicked();
    h.pump_until_state(BeltTensionPanel::ViewState::RESULTS);
    h.panel().handle_retest_clicked(BeltPath::PATH_A);
    CHECK(h.text("bt_run_title") == "Re-measuring Path A");
    h.pump_until_state(BeltTensionPanel::ViewState::RESULTS);
    CHECK(h.text("bt_peak_a") == "106");
    CHECK(h.text("bt_note_a").find("was 110") != std::string::npos);
    CHECK(h.text("bt_peak_b") == "98");
    CHECK(h.ghost_visible(BeltPath::PATH_A));
    CHECK_FALSE(h.ghost_visible(BeltPath::PATH_B));
}

TEST_CASE_METHOD(XMLTestFixture, "belt: each mock failure reaches ERROR", "[belt][panel]") {
    for (auto f : {BeltMockFailure::ERROR, BeltMockFailure::NOFILE, BeltMockFailure::MULTICHIP}) {
        auto h = BeltPanelHarness(*this);
        h.mock().set_belt_failure(f);
        h.panel().handle_start_clicked();
        h.pump_until_state(BeltTensionPanel::ViewState::ERROR);
        CHECK_FALSE(h.text("bt_error_message").empty());
    }
}

TEST_CASE_METHOD(XMLTestFixture, "belt: a stall trips the stall guard", "[belt][panel]") {
    auto h = BeltPanelHarness(*this);
    h.mock().set_belt_failure(BeltMockFailure::STALL);
    h.panel().handle_start_clicked();
    h.advance_ms(BeltTensionPanel::STALL_TIMEOUT_MS + 1000);
    CHECK(h.state() == BeltTensionPanel::ViewState::ERROR);
}

TEST_CASE_METHOD(XMLTestFixture, "belt: closing mid-run stops listening", "[belt][panel]") {
    auto h = BeltPanelHarness(*this);
    h.panel().handle_start_clicked();
    h.pump_ms(200);
    h.panel().on_deactivating(OverlayBase::DeactivateReason{}); // use the enum value tests use elsewhere
    CHECK(h.state() == BeltTensionPanel::ViewState::START);
    h.pump_ms(5000); // the mock keeps emitting; nothing may land
    CHECK(h.state() == BeltTensionPanel::ViewState::START);
}

TEST_CASE_METHOD(XMLTestFixture, "belt: Start stays disabled while not ready", "[belt][panel]") {
    auto h = BeltPanelHarness(*this);
    h.set_klippy_ready(false);
    CHECK(h.integer("bt_can_start") == 0);
    h.set_klippy_ready(true);
    CHECK(h.integer("bt_can_start") == 1);
}

TEST_CASE_METHOD(XMLTestFixture, "belt: a path with no peak is an error naming it",
                 "[belt][panel]") {
    auto h = BeltPanelHarness(*this);
    h.mock().set_resonance_sweep_range(5.0, 18.0, 1.0); // nothing above 20 Hz
    h.panel().handle_start_clicked();
    h.pump_until_state(BeltTensionPanel::ViewState::ERROR);
    CHECK(h.text("bt_error_message").find("Path A") != std::string::npos);
}

TEST_CASE_METHOD(XMLTestFixture, "belt: EMBEDDED tier creates no chart", "[belt][panel]") {
    auto h = BeltPanelHarness(*this, helix::PlatformTier::EMBEDDED, false);
    CHECK(h.integer("bt_chart_available") == 0);
    h.panel().handle_start_clicked();
    h.pump_until_state(BeltTensionPanel::ViewState::RESULTS);
    CHECK_FALSE(h.has_chart());
}

TEST_CASE("belt: the rail bands in XML match the verdict constants", "[belt][panel]") {
    // panel_belt_tension.xml sizes the bands as a share of the ±15 Hz rail.
    CHECK(helix::calibration::belt_verdict::MATCHED_DELTA_HZ == 3.0f);
    CHECK(helix::calibration::belt_verdict::CLOSE_DELTA_HZ == 8.0f);
}
```

`BeltPanelHarness` lives in the test file: it owns the mock client/API, calls `set_render_tier_for_test` before `create()`, exposes `panel()`, `mock()`, `state()` (reads `belt_tension_state`), `text(name)` / `integer(name)` (via `lv_xml_get_subject`), `pump_ms`, `advance_ms` (`lv_tick_inc` in 100 ms steps + `lv_timer_handler`), `pump_until_state` (bounded), `set_klippy_ready`, `ghost_visible(path)` and `has_chart()` (through a `friend struct BeltPanelHarness;` in the panel header, the only test hook).

- [ ] **Step 2: Run to verify failure**: `make t F='[belt][panel]'`.
- [ ] **Step 3: Implement** the panel, XML, sketch widget, deletions, tokens. `bt_hw_sweep` text: `fmt::format(lv_tr("{:.0f}-{:.0f} Hz · about {} min"), cfg.min_freq, cfg.max_freq, minutes)` with `minutes = std::max(1, (int)std::ceil(2.0f * cfg.sweep_seconds() / 60.0f))`; add a panel test asserting it reads "5-135 Hz · about 5 min" for Klipper defaults (2 × 130 s = 260 s → 5).
- [ ] **Step 4: Run**: `make t F='[belt]'`, `make t F='[gating]'`, `make t F='[chart]'`. Expected: PASS.
- [ ] **Step 5: Drive it.** Build, run the pinned-socket mock (`HELIX_MOCK_BELT_A_HZ=110 HELIX_MOCK_BELT_B_HZ=98 ... --test --sim-speed 6 -vv`), `ctl navigate` to Advanced → Belt Tension (row returns in Task 10; until then open it with `ctl` by overlay name), run Start, Re-test A twice, and read `ctl text` for the peak/verdict subjects at each step. Screenshot START, RUNNING, RESULTS at 800×480 and at the 480×272 breakpoint for the final review.
- [ ] **Step 6: Commit**: `feat(belt): Belt Tension compares both paths from resonance sweeps (prestonbrown/helixscreen#1721)`.

---

### Task 10: Row, strings and docs

**Files:**
- Modify: `ui_xml/advanced_panel.xml` (row back inside `<beta_feature>`, `description="Compare belt paths with a resonance sweep"`, same `printer_has_accelerometer` bind and `on_belt_tension_row_clicked`)
- Translations: `make translation-sync && make translations`; commit YAMLs + `ui_xml/translations/*.xml`
- Docs: `docs/user/guide/calibration.md` (Belt Tension section: what it does, requirements incl. "HelixScreen on the printer's own computer" and one accelerometer, running a check, reading results, the re-test loop, that the verdict is provisional; no source references), `docs/user/guide/beta-features.md` (row back), `docs/user/CONFIGURATION.md` (Belt Tension back in the beta list), `README.md` ("belt tension comparison" back in Calibration), new `docs/devel/BELT_TENSION.md` (overview, key files table, flow, Klipper facts, verdict constants and why provisional, mock knobs, tests; style of `docs/devel/SOUND_SYSTEM.md`), index it in `docs/devel/CLAUDE.md` and `docs/README.md`; `docs/devel/BELT_TUNER.md` header: "Panel: none. These libraries back no UI; Belt Tension is `BELT_TENSION.md`."

- [ ] **Step 1:** Make the edits. `ctl navigate advanced` in the mock and confirm `row_belt_tension` exists and opens the panel.
- [ ] **Step 2:** `make t F='[belt]'` and `scripts/check_comment_archaeology.py` on the changed files.
- [ ] **Step 3: Commit**: `feat(belt): Belt Tension returns to the Advanced panel as a beta (prestonbrown/helixscreen#1721)`.

---

### Task 11: Gates, hardware check, and ship cleanup

- [ ] **Step 1:** `make mutate-diff` over the branch. Every surviving mutant in `belt_tension_types.cpp`, `shaper_csv_parser.cpp`, `resonance_console.cpp` or the collector gets a test, or a one-line reason in the final commit body.
- [ ] **Step 2:** `make full-test-run` (unit sweep + bats). Must be green.
- [ ] **Step 3: Hardware, Preston's Voron 2.4.** Ask Preston before the first command that touches the printer; every Start/Re-test moves the machine and needs his go-ahead each time. Run the desktop build against it (`--moonraker ws://HOST:7125` does not work here: the CSV is in the printer's `/tmp`, so run the branch build on the printer's own host). Capture both CSVs to `tests/fixtures/belt/voron24_path_{a,b}.csv` and add a parser + comparison test over them.
- [ ] **Step 4: Hardware, the #1721 reporter.** Post on #1721 asking for a run of the beta build and the two `/tmp/resonances_*_helix_belt_*.csv` files; add them as fixtures when they arrive. Review the provisional thresholds against both machines and record the outcome in `docs/devel/BELT_TENSION.md`.
- [ ] **Step 5:** Delete this plan and the design spec in the change that ships (docs/CLAUDE.md "Plans and Specs").
- [ ] **Step 6:** Independent review of the whole branch (fresh reviewer), then merge per the project's merge flow and tear down the worktree with `scripts/teardown-worktree.sh belt-resonance-compare`.
