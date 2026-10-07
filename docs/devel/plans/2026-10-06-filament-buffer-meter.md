# Filament Buffer Meter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw a filament buffer's proportional reading (OpenAMS FPS, AFC FPS_PSF, Happy Hare sync feedback) as one upright slider with a 60 s trace on a new home widget, the loaded-spool card, the path canvas tint and a live Buffer Status modal, and keep the clog meter detector-only.

**Architecture:** A pure `buffer_reading(info, unit)` decides what a buffer reads (source, value, target, bias, severity); `AmsState` publishes the system-level reading as `buffer_*` subjects and keeps a 60 s step trace per unit. One renderer, `UiBufferSlider`, paints any surface from a pure geometry function, and every severity colour comes from the existing 30/70 bands in `clog_meter_geometry.h`.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Catch2, bats

**Spec:** docs/devel/plans/2026-10-06-filament-buffer-meter-design.md

All paths are relative to the worktree root `/home/pbrown/Code/Printing/helixscreen/.worktrees/openams-fps-meter` (branch `feature/openams-fps-meter`, base tip `4aaf9c26c`). Run every command from there.

## Global Constraints

- Bands: `abs(bias)` below 0.3 is on target, 0.3 to 0.7 is a warning, 0.7 and above is danger: `kPressureWarningPct = 30`, `kPressureFaultPct = 70`, compared on `lround(bias * 100)` by `helix::ui::pressure_status(int)`.
- Colour tokens by severity: Ok `text_muted`, Warning `warning`, Fault `danger` (`helix::ui::buffer_status_token()`), resolved with `theme_manager_get_color()`; no hex colours in new code.
- Labels: `FPS` for a pressure sensor (not translated, `// i18n: do not translate - hardware abbreviation`), `lv_tr("Sync")` for Happy Hare.
- Number: a pressure sensor shows its pressure (`smoothed_fps`, the value AFC's own triggers compare) as `N%`; Sync shows the bias as `+N%` / `-N%` / `0%`. Target text is `target N%` where a set point is known.
- No set point: `Pressure: N%` as text on every surface, no slider, no tint, a gap in the trace.
- History: `BufferTrace::kWindowMs = 60000`, step hold, at most `kMaxPoints = 512` points per trace, stamped by `buffer_clock_ms()` (steady clock). Main thread only.
- Trace redraw: a 1000 ms `lv_timer` per trace-drawing renderer, cancelled with `helix::ui::lv_timer_cancel_safe()` in the destructor.
- Widget `filament_buffer`: default 1x1 (`2, 2` tracks), min 1x1, max 2x1 (`4, 2`), no half cells, gate subject `buffer_present`, trace and lean only at 2x1 (`filament_buffer_wide`).
- Logging: spdlog only.
- Observers: `helix::ui::observe<int>(subject, owner, handler, AmsState::instance().get_subjects_lifetime(), ...)`; never a default-constructed lifetime. `ObserverGuard::reset()`, never `release()`.
- No RTTI. Every touched file under `src/` here is in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` or is added to it: no `try`/`throw`, no `json.value()`/`at()`, `<spdlog/fmt/fmt.h>` never `<fmt/format.h>`, `fmt::runtime()` for translated format strings. Run `python3 scripts/check_esp32_app_srcs.py` after any source add or delete.
- Data in C++, appearance in XML: visibility via `bind_flag_if_*` / `bind_flag_if cond=`, text via `bind_text`. C++ only draws (`LV_EVENT_DRAW_MAIN`), cleans up (`LV_EVENT_DELETE`) and publishes subjects.
- Every new subject lands in the same commit as its first reader (XML bind or C++ observer); C++-only reads carry `// SUBJECT_OK: <reason>`. The orphan-subject gate (`scripts/check_orphan_subjects.py`) runs in the commit hook.
- Strings: `lv_tr()` in C++, `translation_tag` in XML, `TR_NOOP` in the registry; `make translation-sync`, hand-fill all nine `translations/*.yml` (reuse `translations/GLOSSARY.md`), `make translations`.
- Comments state the code as it is now: no commit SHAs, no "used to", no review narration. No em-dashes anywhere (code, docs, commit messages).
- Commit with `git commit -m "..." -- <paths>` (new files first get `git add -N <path>`; deleted files go through `git rm -q <path>`). Never `git add -A`, never `--no-verify`.
- Dev loop: `make t F='[tag]'`. `make full-test-run` once, in the last task. Check for a running build first (`pgrep -x -d' ' 'make|clang++|cc1plus'`) and take `scripts/helix-claim take build:openams-fps-meter "<why>"` around builds.

## Review Focus

1. **Multi-lane OpenAMS with no single current slot** (several lanes loaded, `current_slot == -1`): the system reading must fall back to the first unit reporting pressure, a unit's own view must read its own lane, and a unit with a switched buffer must never borrow another lane's reading. Owner: Task 2 (`buffer_reading: the system follows the lane feeding the toolhead`, `buffer_reading: a switched buffer has no reading`).
2. **Backend switching or vanishing while the widget or modal is open**: `clear_backends()` must zero `buffer_present` (the widget gates), drop every trace, and a trace for a unit that left must go; the open modal must fall back to the unsupported text. Owner: Task 6 (`clear_backends takes the reading away`), Task 3 (`a unit that goes away takes its trace with it`), Task 10 (existing `[live]` vanish section kept green).
3. **Set point arriving late or going null mid-print**: the slider must hide, the text must become `Pressure: N%`, bias and status must reset to 0 / Ok, and the trace must record a gap instead of holding the last bias. Owner: Task 6 (`a set point that goes away mid-print leaves text and a gap`).
4. **Long gaps with no update and clock jumps** (OpenAMS republishes only on a 0.02 change): the newest reading must hold across the whole window however old it is; a stamp older than the newest must reset the trace rather than corrupt the order; points after `now` must not draw. Owner: Task 3 (`BufferTrace` cases `a reading that never changes holds across the window`, `a clock that goes back starts the trace again`).
5. **Readings at the band boundaries and sensors past their rails**: 29 / 30 / 69 / 70 percent land on Ok / Warning / Warning / Fault from both sides; a sensor below 0 or above 1 clamps to 0% / 100% and bias -1 / +1 (Fault); a bias beyond +/-1 or NaN must not push the block out of the housing. Owner: Task 2 (`buffer_reading: a sensor past its rails`, `buffer_reading: the bands at their edges`), Task 4 (`buffer_slider_geometry: out-of-range bias stays in the housing`).

Task order follows the suggested one with two moves: the trace store (Task 3) comes before the renderer because `UiBufferSlider` reads it, and each `buffer_*` subject is introduced in the task that first binds it (Task 6 with the loaded card, Task 7 with the widget) so the orphan-subject gate never sees an unread subject.

---

### Task 1: Clog meter is detector-only again

Revert the pressure sample and "pressure is primary with no detector" from 4aaf9c26c. Keep the 30/70 bands, `pressure_status()`, `buffer_lean()`, the live modal, `bump_data_revision()`, the mock scenarios and `set_sync_feedback_bias()`. `clog_meter_is_symmetrical()` stays (Flowguard only): `ClogMeterSample::is_symmetrical()` and `clog_bar_geometry()` both route through it.

**Files:**
- Modify: `include/clog_meter_geometry.h#ClogMeterMode`, `#ClogSample` (delete), `#clog_meter_tint`, `#kPressureWarningPct`, `#pressure_status`, `#buffer_lean`
- Modify: `src/ui/clog_meter_geometry.cpp#clog_meter_tint`, `#clog_meter_status`, `#clog_meter_is_symmetrical`, `#pressure_status`
- Modify: `include/clog_meter_model.h#ClogMeterModel::ClogMeterModel`, `src/ui/clog_meter_model.cpp#ClogMeterModel::ClogMeterModel`
- Modify: `include/ams_state.h` (forward decl of `helix::ui::ClogSample`, `#AmsState::clog_meter_subjects`, `#AmsState::set_source_override` doc, `PressureClogSubjects`, `clog_pressure_`, `clog_meter_mode_` / `clog_meter_value_` comments)
- Modify: `include/ams_types.h#AmsSystemInfo::clog_sources`
- Modify: `src/printer/ams_state_clog.cpp#pressure_reading` (delete), `#AmsState::clog_meter_subjects`, `#AmsState::sync_clog_meter_from_info`
- Modify: `src/printer/ams_state_subjects.cpp#AmsState::init_subjects` (the Pressure-sample block), `#include <utility>`
- Modify: `src/remote/mock_scenarios.cpp#set_fps` comment, `#build_scenarios` precedence comment
- Modify: `src/ui/ui_ams_detail.cpp#ams_detail_setup_path_canvas` (one comment)
- Modify: `docs/devel/FILAMENT_MANAGEMENT.md` § "Clog / flow meter", `docs/devel/FILAMENT_BACKEND_OPENAMS.md` (the `sync_feedback_bias` bullet)
- Modify: `translations/{de,en,es,fr,it,ja,pt,ru,zh}.yml`, `ui_xml/translations/{de,en,es,fr,it,ja,pt,ru,zh,translations}.xml` (drop `TIGHT`, `LOOSE`)
- Delete: `tests/unit/test_clog_meter_pressure_source.cpp`
- Create: `tests/unit/test_clog_meter_sources.cpp`
- Modify: `tests/unit/test_clog_meter_geometry.cpp`

**Interfaces:**
- Consumes: `AmsStateTestAccess::sync_clog_meter(AmsState&, const AmsSystemInfo&)`.
- Produces: `helix::ui::ClogMeterStatus helix::ui::pressure_status(int pct);` (standalone), `AmsState::ClogMeterSubjects AmsState::clog_meter_subjects();` (no parameter), `explicit helix::ui::ClogMeterModel::ClogMeterModel(Callback on_change);`. `ClogMeterMode` has no `Pressure`; `ClogSample` is gone.

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_clog_meter_sources.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_clog_meter_sources.cpp
 * @brief What feeds the clog meter: clog detectors only. A buffer reading is
 *        drawn by the filament buffer surfaces, never by the clog meter.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "ams_state.h"
#include "app_globals.h"
#include "clog_meter_geometry.h"
#include "printer_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterMode;

namespace {

int mode(AmsState& ams) {
    return lv_subject_get_int(ams.get_clog_meter_mode_subject());
}

AmsSystemInfo fps_info(float pressure) {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = pressure;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    unit.buffer_health = fps;
    info.units.push_back(unit);
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

/// The meter subjects and the override are the singleton's, read by later tests.
struct ResetMeter {
    AmsState& ams;
    ~ResetMeter() {
        ams.set_source_override(0);
        AmsStateTestAccess::sync_clog_meter(ams, AmsSystemInfo{});
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "clog meter: a buffer reading alone is not clog detection",
                 "[ams][clog][sources]") {
    get_printer_state().init_subjects(false);
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetMeter reset{ams};

    SECTION("Happy Hare sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == 0);
    }

    SECTION("a filament pressure sensor") {
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }

    SECTION("beside a detector, the meter shows the detector") {
        AmsSystemInfo info = fps_info(0.9f);
        info.encoder_info.enabled = true;
        AmsStateTestAccess::sync_clog_meter(ams, info);
        CHECK(mode(ams) == static_cast<int>(ClogMeterMode::Encoder));
        CHECK(std::string(lv_subject_get_string(ams.get_clog_meter_mode_text_subject())) != "FPS");
    }

    SECTION("a forced detector this printer lacks leaves the meter empty") {
        ams.set_source_override(2); // Flowguard
        AmsStateTestAccess::sync_clog_meter(ams, fps_info(0.32f));
        CHECK(mode(ams) == 0);
    }
}
```

- [ ] **Step 2: Run it.** `make t F='[clog][sources]'`. Expected FAIL: the first, second and fourth sections report `mode(ams) == 4` (the Pressure mode is still primary when no detector exists).

- [ ] **Step 3: Minimal implementation.**

`include/clog_meter_geometry.h`: delete the `Pressure = 4` enumerator, delete `enum class ClogSample` and its comment, and set these comments/declarations:

```cpp
/// Indicator colour for a given mode/value/warning triple.
///
/// A warning is unconditional danger whatever the mode. Otherwise the linear
/// modes ramp primary -> warning -> danger across their 0..100 range, and
/// Flowguard stays primary: its extremes are already labelled at both ends of
/// the scale, so tinting the middle of a symmetrical range says nothing.
ClogMeterTint clog_meter_tint(int mode, int value, int warning);
```

```cpp
/// Bands a filament buffer reading (`|bias| * 100`) is judged against: from
/// kPressureWarningPct it has drifted off its target, from kPressureFaultPct
/// the buffer is close to an end stop.
constexpr int kPressureWarningPct = 30;
constexpr int kPressureFaultPct = 70;

/// Severity of a buffer reading, `bias * 100` (-100..+100), by magnitude.
ClogMeterStatus pressure_status(int pct);

/// Which way a buffer bias (-1..+1) leans. Negative is tension (the extruder
/// pulling harder than the feeder pushes), positive is compression.
enum class BufferLean : int { Balanced, Tight, Loose };
BufferLean buffer_lean(float bias);
```

`src/ui/clog_meter_geometry.cpp`: delete the `if (m == ClogMeterMode::Pressure) {...}` block in `clog_meter_tint()`, delete the Pressure block (`danger_pct = kPressureWarningPct;`) in `clog_meter_status()`, and replace two functions:

```cpp
bool clog_meter_is_symmetrical(int mode) {
    return static_cast<ClogMeterMode>(mode) == ClogMeterMode::Flowguard;
}

ClogMeterStatus pressure_status(int pct) {
    const int magnitude = std::abs(pct);
    if (magnitude >= kPressureFaultPct) {
        return ClogMeterStatus::Fault;
    }
    if (magnitude >= kPressureWarningPct) {
        return ClogMeterStatus::Warning;
    }
    return ClogMeterStatus::Ok;
}
```

`include/clog_meter_model.h`: replace the constructor declaration and drop its `@p which` lines:

```cpp
    explicit ClogMeterModel(Callback on_change);
```

`src/ui/clog_meter_model.cpp`:

```cpp
ClogMeterModel::ClogMeterModel(Callback on_change) : on_change_(std::move(on_change)) {
    auto& ams = AmsState::instance();
    const auto subjects = ams.clog_meter_subjects();
```

`include/ams_state.h`: delete the `namespace helix::ui { enum class ClogSample : int; }` forward declaration, delete `struct PressureClogSubjects` and the `clog_pressure_` member, and set:

```cpp
    /**
     * @brief The subjects the clog meter is published on, as ClogMeterModel reads them
     *
     * Observe with get_subjects_lifetime().
     */
    [[nodiscard]] ClogMeterSubjects clog_meter_subjects();

    /**
     * @brief Set source override for the clog meter
     * @param source 0=auto (priority logic), 1=encoder, 2=flowguard, 3=afc
     */
    void set_source_override(int source);
```

```cpp
    lv_subject_t clog_meter_mode_{};    // ClogMeterMode: 0=none, 1=encoder, 2=flowguard, 3=afc_buffer
    lv_subject_t clog_meter_value_{};   // 0-100 (encoder/afc) or -100..+100 (flowguard)
```

`include/ams_types.h#AmsSystemInfo::clog_sources`: delete the `bool pressure = false;` field and the `s.pressure = sync_feedback_bias > -1.5f;` line.

`src/printer/ams_state_clog.cpp`: delete `pressure_reading()`, and replace `clog_meter_subjects()` and `sync_clog_meter_from_info()`:

```cpp
AmsState::ClogMeterSubjects AmsState::clog_meter_subjects() {
    return {&clog_meter_mode_,       &clog_meter_value_,       &clog_meter_warning_,
            &clog_meter_status_,     &clog_meter_mode_text_,   &clog_meter_danger_pct_,
            &clog_meter_peak_pct_,   &clog_meter_center_text_, &clog_meter_label_left_,
            &clog_meter_label_right_};
}

void AmsState::sync_clog_meter_from_info(const AmsSystemInfo& info) {
    ClogReading r = detector_reading(info, source_override_);
    if (danger_threshold_override_ > 0) {
        r.danger_pct = danger_threshold_override_;
    }
    publish(clog_meter_subjects(), r);

    spdlog::trace("[AMS State] Synced clog meter - mode={}, value={}, warning={}", r.mode, r.value,
                  r.warning);
}
```

`src/printer/ams_state_subjects.cpp`: delete the block that starts `// The Pressure sample: registered for teardown, not yet named in XML.` and the `#include <utility>` it needed.

`src/remote/mock_scenarios.cpp`: the `set_fps` comment becomes `/// A filament pressure sensor on unit 0 reading \`pressure\` against a 0.5 set\n/// point, and nothing else measuring. Read as such by every simulated type\n/// but Happy Hare, whose buffer is system-level.`; the precedence comment becomes `// Source precedence in sync_clog_meter_from_info() is flowguard > encoder >\n// AFC buffer, so each scenario disables the sources above the one it wants.`

`src/ui/ui_ams_detail.cpp#ams_detail_setup_path_canvas`: the comment above `buffer_fault = static_cast<int>(` becomes `// Proportional sync feedback (Happy Hare, or this unit's pressure sensor):\n    // the buffer bands' severity.`

`tests/unit/test_clog_meter_geometry.cpp`: delete `kMode_Pressure`, the cases `clog_bar_geometry: Pressure fills out from the centre` and `clog_meter_tint: Pressure turns warning off balance, either way`, the last `CHECK(clog_meter_status(kMode_Pressure, ...))` and its comment in `pressure_status: the bands, by magnitude`, and restore the symmetry case to:

```cpp
TEST_CASE("ClogMeterSample: only Flowguard reads out from a centre", "[clog][model][1017]") {
    // The arc encodes this as LV_ARC_MODE_SYMMETRICAL over 0..200 and the bar
    // as centre-out geometry. Two encodings are fine; two decisions are not.
    ClogMeterSample fg;
    fg.mode = kMode_Flowguard;
    CHECK(fg.is_symmetrical());
```

(the loop over `None`, `Encoder`, `Buffer` that follows stays). Drop `#include <string>` if nothing else in the file uses it.

`git rm -q tests/unit/test_clog_meter_pressure_source.cpp`.

Docs, `docs/devel/FILAMENT_MANAGEMENT.md` § "Clog / flow meter": replace the paragraph and table that begin `` `AmsState::sync_clog_meter_from_info()` (`src/printer/ams_state_clog.cpp`) builds `` through the paragraph ending `so it is live while open.` with:

```markdown
Three sources (encoder, Flowguard, AFC buffer) feed one set of `clog_meter_*`
subjects, derived in `AmsState::sync_clog_meter_from_info()`
(`src/printer/ams_state_clog.cpp`). Source precedence is **flowguard > encoder > AFC
buffer** (then the legacy `clog_detection` flag), overridable per widget via
`set_source_override()` (0 auto, 1 encoder, 2 flowguard, 3 AFC). A buffer's
position is not clog detection and never reaches these subjects; see
[Filament buffer reading](#filament-buffer-reading).
```

`docs/devel/FILAMENT_BACKEND_OPENAMS.md`: in the `sync_feedback_bias` bullet, replace `It drives the clog-detection\n  widget's buffer page and the clog meter's \`Pressure\` sample (the primary one\n  when no clog detector exists).` with `It drives the clog-detection\n  widget's buffer page.`

Translations: `make translation-obsolete` must list `TIGHT` and `LOOSE`. Delete the `TIGHT:` and `LOOSE:` lines from each of the nine `translations/*.yml`, then `make translations`.

- [ ] **Step 4: Run it.** `make t F='[clog]'`. Expected PASS, including `[clog][sources]`, `[clog][status][pressure]` and `[clog][model][1017]`. Then `make t F='[buffer_status]'` (the live modal still reads `buffer_lean()`): PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N tests/unit/test_clog_meter_sources.cpp
git rm -q tests/unit/test_clog_meter_pressure_source.cpp
git commit -m "refactor(clog): the clog meter shows clog detectors only" \
  -m "A buffer reading (Happy Hare sync feedback, a filament pressure sensor) no longer becomes a clog-meter sample or the primary reading when no detector exists; the buffer gets its own surfaces. The 30/70 bands, buffer_lean(), the live Buffer Status modal and the mock scenarios stay. Red first: [clog][sources] read mode 4 before the revert." \
  -- include/clog_meter_geometry.h src/ui/clog_meter_geometry.cpp include/clog_meter_model.h \
     src/ui/clog_meter_model.cpp include/ams_state.h include/ams_types.h \
     src/printer/ams_state_clog.cpp src/printer/ams_state_subjects.cpp src/remote/mock_scenarios.cpp \
     src/ui/ui_ams_detail.cpp docs/devel/FILAMENT_MANAGEMENT.md docs/devel/FILAMENT_BACKEND_OPENAMS.md \
     translations ui_xml/translations tests/unit/test_clog_meter_sources.cpp \
     tests/unit/test_clog_meter_pressure_source.cpp tests/unit/test_clog_meter_geometry.cpp
git show --stat HEAD
```

---

### Task 2: `buffer_reading()`, the severity colour and the reading's words

**Files:**
- Create: `include/buffer_reading.h`, `src/printer/buffer_reading.cpp`
- Create: `tests/test_helpers/buffer_infos.h`, `tests/unit/test_buffer_reading.cpp`
- Modify: `include/ams_types.h#AmsSystemInfo::feeding_pressure_sensor` (replaced by `feeding_pressure_unit`), `#AmsSystemInfo::pressure_sensor_bias`
- Modify: `include/clog_meter_geometry.h`, `src/ui/clog_meter_geometry.cpp` (add `buffer_status_token`)
- Modify: `tests/unit/test_clog_meter_geometry.cpp` (token case)
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/printer/buffer_reading.cpp` on its own line after `src/printer/ams_state_tool_mapping.cpp`)
- Modify: `translations/*.yml`, `ui_xml/translations/*.xml` (`target {}%`, `Running tight`, `Running loose`, `Running balanced`)

**Interfaces:**
- Consumes: `BufferHealth::has_fps()`, `BufferHealth::fps_to_bias()`, `helix::ui::pressure_status(int)`, `helix::ui::buffer_lean(float)`, `AmsSystemInfo::get_unit(int)`, `AmsSystemInfo::get_unit_position_for_slot(int)`.
- Produces:
  - `int helix::AmsSystemInfo::feeding_pressure_unit() const;`
  - `enum class helix::BufferSource : int { None = 0, Fps = 1, Sync = 2 };`
  - `struct helix::BufferReading { BufferSource source; int unit; bool has_slider; int value_pct; int target_pct; float bias; ui::ClogMeterStatus status; bool present() const; };`
  - `helix::BufferReading helix::buffer_reading(const AmsSystemInfo& info, int unit);`
  - `const char* helix::buffer_label(const BufferReading& r);`
  - `std::string helix::buffer_value_text(const BufferReading& r);`
  - `std::string helix::buffer_target_text(const BufferReading& r);`
  - `const char* helix::buffer_lean_text(const BufferReading& r);`
  - `const char* helix::ui::buffer_status_token(ClogMeterStatus s);`
  - `helix::AmsSystemInfo helix::test::fps_units(std::initializer_list<float> pressures, float set_point = 0.5f, int current_slot = -1);`

- [ ] **Step 1: Write the failing test.** Create `tests/test_helpers/buffer_infos.h`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"

#include <initializer_list>

namespace helix::test {

/// One four-slot unit per entry of @p pressures, each with a filament pressure
/// sensor at that reading against @p set_point (-1 = none published), as
/// OpenAMS reports them. sync_feedback_bias is derived the way the backends do.
inline AmsSystemInfo fps_units(std::initializer_list<float> pressures, float set_point = 0.5f,
                               int current_slot = -1) {
    AmsSystemInfo info;
    int u = 0;
    for (float pressure : pressures) {
        AmsUnit unit;
        unit.unit_index = u;
        unit.first_slot_global_index = u * 4;
        unit.slot_count = 4;
        BufferHealth fps;
        fps.fps_value = fps.smoothed_fps = pressure;
        fps.fps_set_point = set_point;
        fps.fps_reported = true;
        unit.buffer_health = fps;
        info.units.push_back(unit);
        ++u;
    }
    info.total_slots = u * 4;
    info.current_slot = current_slot;
    info.sync_feedback_bias = info.pressure_sensor_bias();
    return info;
}

} // namespace helix::test
```

Create `tests/unit/test_buffer_reading.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_reading.cpp
 * @brief buffer_reading(): which sensor a view reads and what it shows.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/buffer_infos.h"
#include "buffer_reading.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterStatus;

TEST_CASE("buffer_reading: a pressure sensor against its set point", "[buffer][reading]") {
    const BufferReading r = buffer_reading(test::fps_units({0.32f}), -1);
    CHECK(r.source == BufferSource::Fps);
    CHECK(r.present());
    CHECK(r.unit == 0);
    CHECK(r.has_slider);
    CHECK(r.value_pct == 32);
    CHECK(r.target_pct == 50);
    CHECK(r.bias == Catch::Approx(-0.36f));
    CHECK(r.status == ClogMeterStatus::Warning);
}

TEST_CASE("buffer_reading: no set point is a reading with nothing to centre on",
          "[buffer][reading]") {
    const BufferReading r = buffer_reading(test::fps_units({0.62f}, -1.0f), -1);
    CHECK(r.source == BufferSource::Fps);
    CHECK_FALSE(r.has_slider);
    CHECK(r.value_pct == 62);
    CHECK(r.target_pct == -1);
    CHECK(r.bias == 0.0f);
    CHECK(r.status == ClogMeterStatus::Ok);
}

TEST_CASE("buffer_reading: Happy Hare sync feedback is one buffer for the system",
          "[buffer][reading]") {
    AmsSystemInfo info;
    info.sync_feedback_bias = -0.45f;
    const BufferReading r = buffer_reading(info, -1);
    CHECK(r.source == BufferSource::Sync);
    CHECK(r.unit == -1);
    CHECK(r.has_slider);
    CHECK(r.value_pct == -45);
    CHECK(r.bias == Catch::Approx(-0.45f));
    CHECK(r.status == ClogMeterStatus::Warning);

    SECTION("a unit with no buffer of its own reads the same one") {
        AmsUnit unit;
        unit.slot_count = 4;
        info.units.push_back(unit);
        CHECK(buffer_reading(info, 0).source == BufferSource::Sync);
    }

    SECTION("no bias, no reading") {
        info.sync_feedback_bias = -2.0f;
        CHECK_FALSE(buffer_reading(info, -1).present());
    }
}

TEST_CASE("buffer_reading: the system follows the lane feeding the toolhead",
          "[buffer][reading]") {
    // Unit 0 reads 0.9 (bias +0.8), unit 1 reads 0.3 (bias -0.4).
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    CHECK(buffer_reading(info, -1).unit == 1);
    CHECK(buffer_reading(info, -1).bias == Catch::Approx(-0.4f));

    SECTION("several lanes loaded, no single current slot: the first lane with a sensor") {
        info.current_slot = -1;
        const BufferReading r = buffer_reading(info, -1);
        CHECK(r.unit == 0);
        CHECK(r.bias == Catch::Approx(0.8f));
        CHECK(r.status == ClogMeterStatus::Fault);
    }

    SECTION("a unit's own view reads its own lane") {
        CHECK(buffer_reading(info, 0).unit == 0);
        CHECK(buffer_reading(info, 0).bias == Catch::Approx(0.8f));
    }

    SECTION("a unit without a sensor reads the lane feeding the toolhead") {
        info.units[0].buffer_health.reset();
        CHECK(buffer_reading(info, 0).unit == 1);
    }

    SECTION("a unit index past the end reads the system") {
        CHECK(buffer_reading(info, 7).unit == 1);
    }
}

TEST_CASE("buffer_reading: a switched buffer has no reading", "[buffer][reading]") {
    AmsSystemInfo info;
    AmsUnit unit;
    unit.slot_count = 4;
    BufferHealth turtleneck;
    turtleneck.fault_detection_enabled = true;
    turtleneck.distance_to_fault = 12.0f;
    unit.buffer_health = turtleneck;
    info.units.push_back(unit);
    // A pressure sensor elsewhere must not be borrowed by the switched unit.
    AmsSystemInfo other = test::fps_units({0.7f});
    info.units.push_back(other.units[0]);
    info.units[1].unit_index = 1;
    info.units[1].first_slot_global_index = 4;

    CHECK_FALSE(buffer_reading(info, 0).present());
    CHECK(buffer_reading(info, 1).present());
}

TEST_CASE("buffer_reading: a sensor past its rails", "[buffer][reading]") {
    const BufferReading high = buffer_reading(test::fps_units({1.2f}), -1);
    CHECK(high.value_pct == 100);
    CHECK(high.bias == Catch::Approx(1.0f));
    CHECK(high.status == ClogMeterStatus::Fault);

    const BufferReading low = buffer_reading(test::fps_units({-0.05f}), -1);
    CHECK(low.value_pct == 0);
    CHECK(low.bias == Catch::Approx(-1.0f));
    CHECK(low.status == ClogMeterStatus::Fault);
}

TEST_CASE("buffer_reading: the bands at their edges", "[buffer][reading]") {
    // bias = (p - 0.5) / 0.5, judged on lround(bias * 100)
    CHECK(buffer_reading(test::fps_units({0.36f}), -1).status == ClogMeterStatus::Ok);      // -28
    CHECK(buffer_reading(test::fps_units({0.35f}), -1).status == ClogMeterStatus::Warning); // -30
    CHECK(buffer_reading(test::fps_units({0.65f}), -1).status == ClogMeterStatus::Warning); // +30
    CHECK(buffer_reading(test::fps_units({0.84f}), -1).status == ClogMeterStatus::Warning); // +68
    CHECK(buffer_reading(test::fps_units({0.85f}), -1).status == ClogMeterStatus::Fault);   // +70
    CHECK(buffer_reading(test::fps_units({0.15f}), -1).status == ClogMeterStatus::Fault);   // -70
}

TEST_CASE_METHOD(LVGLTestFixture, "buffer reading words", "[buffer][reading][text]") {
    SECTION("pressure with a set point") {
        const BufferReading r = buffer_reading(test::fps_units({0.32f}), -1);
        CHECK(std::string(buffer_label(r)) == "FPS");
        CHECK(buffer_value_text(r) == "32%");
        CHECK(buffer_target_text(r) == "target 50%");
        CHECK(std::string(buffer_lean_text(r)) == "Running tight");
    }
    SECTION("pressure without one") {
        const BufferReading r = buffer_reading(test::fps_units({0.32f}, -1.0f), -1);
        CHECK(buffer_value_text(r) == "Pressure: 32%");
        CHECK(buffer_target_text(r).empty());
        CHECK(std::string(buffer_lean_text(r)).empty());
    }
    SECTION("sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = 0.15f;
        BufferReading r = buffer_reading(info, -1);
        CHECK(std::string(buffer_label(r)) == "Sync");
        CHECK(buffer_value_text(r) == "+15%");
        CHECK(buffer_target_text(r).empty());
        CHECK(std::string(buffer_lean_text(r)) == "Running loose");
        info.sync_feedback_bias = 0.0f;
        r = buffer_reading(info, -1);
        CHECK(buffer_value_text(r) == "0%");
        CHECK(std::string(buffer_lean_text(r)) == "Running balanced");
    }
    SECTION("nothing to read") {
        const BufferReading r;
        CHECK(std::string(buffer_label(r)).empty());
        CHECK(buffer_value_text(r).empty());
    }
}
```

Append to `tests/unit/test_clog_meter_geometry.cpp`:

```cpp
TEST_CASE("buffer_status_token: neutral on target, warning off it, danger at an end stop",
          "[clog][status][buffer]") {
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Ok)) == "text_muted");
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Warning)) == "warning");
    CHECK(std::string(buffer_status_token(ClogMeterStatus::Fault)) == "danger");
}
```

- [ ] **Step 2: Run it.** `make t F='[buffer][reading]'`. Expected FAIL: does not compile, `buffer_reading.h` not found and `buffer_status_token` undeclared.

- [ ] **Step 3: Minimal implementation.**

`include/ams_types.h`: replace `feeding_pressure_sensor()` and `pressure_sensor_bias()` with:

```cpp
    /// Index of the unit whose filament pressure sensor feeds the toolhead: the
    /// current slot's unit, else the first unit that reports pressure (with
    /// several lanes loaded there is no current slot). -1 when none does. A
    /// sensor without a set point counts: it has a reading, just nothing to
    /// centre it on.
    [[nodiscard]] int feeding_pressure_unit() const {
        auto reports = [this](int pos) {
            const auto& health = units[static_cast<size_t>(pos)].buffer_health;
            return health && health->fps_reported;
        };
        const int active = get_unit_position_for_slot(current_slot);
        if (active >= 0 && reports(active)) {
            return active;
        }
        for (int pos = 0; pos < static_cast<int>(units.size()); ++pos) {
            if (reports(pos)) {
                return pos;
            }
        }
        return -1;
    }

    /// System-level bias for backends with a pressure sensor per unit, from
    /// feeding_pressure_unit(). -1.5 when no unit reports pressure against a
    /// set point.
    [[nodiscard]] float pressure_sensor_bias() const {
        const int pos = feeding_pressure_unit();
        return pos >= 0 ? units[static_cast<size_t>(pos)].buffer_health->fps_to_bias() : -1.5f;
    }
```

`include/clog_meter_geometry.h`, after `pressure_status`:

```cpp
/// The design token a buffer reading of this severity is drawn in: neutral on
/// target, warning off it, danger near an end stop.
const char* buffer_status_token(ClogMeterStatus s);
```

`src/ui/clog_meter_geometry.cpp`:

```cpp
const char* buffer_status_token(ClogMeterStatus s) {
    switch (s) {
    case ClogMeterStatus::Warning:
        return "warning";
    case ClogMeterStatus::Fault:
        return "danger";
    case ClogMeterStatus::Ok:
        break;
    }
    return "text_muted";
}
```

Create `include/buffer_reading.h`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "clog_meter_geometry.h"

#include <string>

namespace helix {

/// What a buffer reading comes from, which is also what it is called on screen.
enum class BufferSource : int {
    None = 0, ///< No proportional reading: a switched buffer, or no buffer at all
    Fps = 1,  ///< A filament pressure sensor (OpenAMS lane, AFC FPS_PSF buffer)
    Sync = 2, ///< Happy Hare sync feedback, one buffer for the whole system
};

/// Where a filament buffer sits right now, for every surface that draws one.
struct BufferReading {
    BufferSource source = BufferSource::None;
    /// Position in AmsSystemInfo::units of the sensor read; -1 for Happy Hare's
    /// system-level buffer.
    int unit = -1;
    /// There is a set point to centre on, so the reading has a bias and the
    /// slider draws. A pressure sensor without one is shown as text only.
    bool has_slider = false;
    /// Fps: the pressure, 0..100. Sync: the bias, -100..+100.
    int value_pct = 0;
    /// Fps with a set point: the set point, 0..100. -1 otherwise.
    int target_pct = -1;
    /// -1 tight .. +1 loose around the set point; 0 without one.
    float bias = 0.0f;
    /// ui::pressure_status() of the bias; Ok without a set point.
    ui::ClogMeterStatus status = ui::ClogMeterStatus::Ok;

    [[nodiscard]] bool present() const {
        return source != BufferSource::None;
    }
};

/// The reading for one unit's buffer, or the system's with @p unit -1.
///
/// A unit with its own pressure sensor reads that sensor, and a unit with a
/// switched buffer has no reading. Any other unit, and -1, reads the system:
/// the pressure sensor feeding the toolhead (feeding_pressure_unit()), else
/// Happy Hare's sync feedback.
[[nodiscard]] BufferReading buffer_reading(const AmsSystemInfo& info, int unit);

/// "FPS" or "Sync"; empty with no reading.
[[nodiscard]] const char* buffer_label(const BufferReading& r);

/// The number: "32%" for a pressure, "-45%" for a bias, "Pressure: 32%" for a
/// pressure with no set point; empty with no reading.
[[nodiscard]] std::string buffer_value_text(const BufferReading& r);

/// "target 50%" where a set point is known, else empty.
[[nodiscard]] std::string buffer_target_text(const BufferReading& r);

/// "Running tight" / "Running loose" / "Running balanced" from buffer_lean();
/// empty without a slider.
[[nodiscard]] const char* buffer_lean_text(const BufferReading& r);

} // namespace helix
```

Create `src/printer/buffer_reading.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_reading.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix {

BufferReading buffer_reading(const AmsSystemInfo& info, int unit) {
    BufferReading r;
    const AmsUnit* own = info.get_unit(unit);
    if (own && own->buffer_health && !own->buffer_health->fps_reported) {
        return r; // a switched buffer: where it sits is not measured
    }
    const int sensor = (own && own->buffer_health) ? unit : info.feeding_pressure_unit();
    if (sensor >= 0) {
        const BufferHealth& fps = *info.units[static_cast<size_t>(sensor)].buffer_health;
        r.source = BufferSource::Fps;
        r.unit = sensor;
        r.value_pct = std::clamp(static_cast<int>(std::lround(fps.smoothed_fps * 100.0f)), 0, 100);
        if (fps.has_fps()) {
            r.has_slider = true;
            r.target_pct = static_cast<int>(std::lround(fps.fps_set_point * 100.0f));
            r.bias = fps.fps_to_bias();
        }
    } else if (info.sync_feedback_bias > -1.5f) {
        r.source = BufferSource::Sync;
        r.has_slider = true;
        r.bias = std::clamp(info.sync_feedback_bias, -1.0f, 1.0f);
        r.value_pct = static_cast<int>(std::lround(r.bias * 100.0f));
    }
    if (r.has_slider) {
        r.status = ui::pressure_status(static_cast<int>(std::lround(r.bias * 100.0f)));
    }
    return r;
}

const char* buffer_label(const BufferReading& r) {
    switch (r.source) {
    case BufferSource::Fps:
        return "FPS"; // i18n: do not translate - hardware abbreviation
    case BufferSource::Sync:
        return lv_tr("Sync");
    case BufferSource::None:
        break;
    }
    return "";
}

std::string buffer_value_text(const BufferReading& r) {
    if (!r.present()) {
        return "";
    }
    if (!r.has_slider) {
        return fmt::format("{} {}%", lv_tr("Pressure:"), r.value_pct);
    }
    if (r.source == BufferSource::Sync && r.value_pct != 0) {
        return fmt::format("{:+d}%", r.value_pct);
    }
    return fmt::format("{}%", r.value_pct);
}

std::string buffer_target_text(const BufferReading& r) {
    if (r.target_pct < 0) {
        return "";
    }
    return fmt::format(fmt::runtime(lv_tr("target {}%")), r.target_pct);
}

const char* buffer_lean_text(const BufferReading& r) {
    if (!r.has_slider) {
        return "";
    }
    switch (ui::buffer_lean(r.bias)) {
    case ui::BufferLean::Tight:
        return lv_tr("Running tight");
    case ui::BufferLean::Loose:
        return lv_tr("Running loose");
    case ui::BufferLean::Balanced:
        break;
    }
    return lv_tr("Running balanced");
}

} // namespace helix
```

`app_srcs.txt`: add the line `src/printer/buffer_reading.cpp` (column 0, no trailing text) after `src/printer/ams_state_tool_mapping.cpp`. Run `python3 scripts/check_esp32_app_srcs.py`: PASS.

Translations: `make translation-sync`, then confirm `grep -n "target {}%\|Running tight\|Running loose\|Running balanced" translations/en.yml` shows four keys (add any the extractor missed by hand, English value = key). Fill each language:

| Key | de | es | fr | it | ja | pt | ru | zh |
|---|---|---|---|---|---|---|---|---|
| `target {}%` | `Ziel {}%` | `objetivo {}%` | `cible {}%` | `target {}%` | `目標 {}%` | `alvo {}%` | `цель {}%` | `目标 {}%` |
| `Running tight` | `Läuft straff` | `Va tenso` | `Tendu` | `Teso` | `張り気味` | `Tenso` | `Натянут` | `偏紧` |
| `Running loose` | `Läuft locker` | `Va suelto` | `Détendu` | `Allentato` | `緩み気味` | `Solto` | `Ослаблен` | `偏松` |
| `Running balanced` | `Läuft ausgeglichen` | `Va equilibrado` | `Équilibré` | `Bilanciato` | `均衡` | `Equilibrado` | `Сбалансирован` | `平衡` |

Then `make translations`.

- [ ] **Step 4: Run it.** `make t F='[buffer][reading]'`, then `make t F='[clog][status]'`, then `make t F='[openams]'` and `make t F='[afc]'` (they read `pressure_sensor_bias()` through the backends). Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N include/buffer_reading.h src/printer/buffer_reading.cpp tests/test_helpers/buffer_infos.h tests/unit/test_buffer_reading.cpp
git commit -m "feat(ams): buffer_reading() decides what a filament buffer reads" \
  -m "One pure function picks the sensor a view reads (its own lane, else the lane feeding the toolhead, else Happy Hare sync feedback), with the value, target, bias and severity every buffer surface draws, plus the words for each. buffer_status_token() maps severity to text_muted / warning / danger. Red first: the new cases did not compile without the header." \
  -- include/buffer_reading.h src/printer/buffer_reading.cpp include/ams_types.h \
     include/clog_meter_geometry.h src/ui/clog_meter_geometry.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt translations ui_xml/translations \
     tests/test_helpers/buffer_infos.h tests/unit/test_buffer_reading.cpp tests/unit/test_clog_meter_geometry.cpp
git show --stat HEAD
```

---

### Task 3: 60 s buffer trace per unit

**Files:**
- Modify: `include/buffer_reading.h` (add `buffer_clock_ms`, `BufferTracePoint`, `BufferTrace`), `src/printer/buffer_reading.cpp`
- Create: `src/printer/ams_state_buffer.cpp`
- Modify: `include/ams_state.h` (include, `#AmsState::buffer_trace`, `#AmsState::sync_buffer_from_info`, `buffer_traces_`)
- Modify: `src/printer/ams_state.cpp#AmsState::sync_from_backend`, `#AmsState::clear_backends`
- Modify: `tests/test_helpers/ams_state_test_access.h#AmsStateTestAccess` (add `sync_buffer`, `clear_buffer_traces`)
- Create: `tests/unit/test_buffer_trace.cpp`, `tests/unit/test_ams_state_buffer.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/printer/ams_state_buffer.cpp` after `src/printer/ams_state.cpp`)

**Interfaces:**
- Consumes: `helix::buffer_reading(const AmsSystemInfo&, int)`.
- Produces:
  - `int64_t helix::buffer_clock_ms();`
  - `struct helix::BufferTracePoint { int64_t t_ms; float bias; bool valid; };`
  - `class helix::BufferTrace { static constexpr int64_t kWindowMs = 60000; static constexpr std::size_t kMaxPoints = 512; void record(int64_t now_ms, bool valid, float bias); std::vector<BufferTracePoint> window(int64_t now_ms) const; void clear(); std::size_t size() const; };`
  - `const helix::BufferTrace& helix::AmsState::buffer_trace(int unit) const;`
  - `void helix::AmsState::sync_buffer_from_info(const AmsSystemInfo& info, int64_t now_ms);` (private)
  - `static void AmsStateTestAccess::sync_buffer(AmsState&, const AmsSystemInfo&, int64_t now_ms);`, `static void AmsStateTestAccess::clear_buffer_traces(AmsState&);`

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_buffer_trace.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_trace.cpp
 * @brief BufferTrace: a 60 s step history of one buffer's bias.
 */

#include "buffer_reading.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("BufferTrace: empty draws nothing", "[buffer][trace]") {
    BufferTrace t;
    CHECK(t.window(100000).empty());
}

TEST_CASE("BufferTrace: a reading that never changes holds across the window", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.2f);
    const auto w = t.window(200000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].t_ms == 200000 - BufferTrace::kWindowMs);
    CHECK(w[0].bias == 0.2f);
    CHECK(w[0].valid);
}

TEST_CASE("BufferTrace: each reading holds until the next", "[buffer][trace]") {
    BufferTrace t;
    t.record(1000, true, 0.1f);
    t.record(31000, true, -0.4f);

    const auto w = t.window(61000);
    REQUIRE(w.size() == 2);
    CHECK(w[0].t_ms == 1000);
    CHECK(w[1].t_ms == 31000);

    // Once the first reading leaves the window, the second is what was held
    // at the window's start.
    const auto later = t.window(91000);
    REQUIRE(later.size() == 1);
    CHECK(later[0].t_ms == 31000);
    CHECK(later[0].bias == -0.4f);
}

TEST_CASE("BufferTrace: a repeat adds nothing", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.3f);
    t.record(500, true, 0.3f);
    CHECK(t.size() == 1);
}

TEST_CASE("BufferTrace: a reading that goes away is a gap", "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.2f);
    t.record(10000, false, 0.9f);
    t.record(20000, false, 0.0f);
    REQUIRE(t.size() == 2);
    const auto w = t.window(20000);
    CHECK_FALSE(w.back().valid);
}

TEST_CASE("BufferTrace: old readings expire, keeping the one held into the window",
          "[buffer][trace]") {
    BufferTrace t;
    t.record(0, true, 0.1f);
    t.record(10000, true, 0.2f);
    t.record(70001, true, 0.3f);
    CHECK(t.size() == 2); // 0 is gone; 10000 is held into [10001, 70001]
}

TEST_CASE("BufferTrace: a clock that goes back starts the trace again", "[buffer][trace]") {
    BufferTrace t;
    t.record(50000, true, 0.1f);
    t.record(40000, true, 0.5f);
    REQUIRE(t.size() == 1);
    const auto w = t.window(40000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].bias == 0.5f);
}

TEST_CASE("BufferTrace: points after now are not drawn", "[buffer][trace]") {
    BufferTrace t;
    t.record(1000, true, 0.1f);
    t.record(5000, true, 0.2f);
    const auto w = t.window(3000);
    REQUIRE(w.size() == 1);
    CHECK(w[0].t_ms == 1000);
}

TEST_CASE("BufferTrace: memory is bounded", "[buffer][trace]") {
    BufferTrace t;
    for (int i = 0; i < 600; ++i) {
        t.record(i, true, static_cast<float>(i % 2));
    }
    CHECK(t.size() == BufferTrace::kMaxPoints);
}
```

Create `tests/unit/test_ams_state_buffer.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_state_buffer.cpp
 * @brief AmsState's filament buffer reading: its traces and its subjects.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

/// The traces (and, from the subjects task on, the reading) are the singleton's.
struct ResetBuffer {
    AmsState& ams;
    ~ResetBuffer() {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        AmsStateTestAccess::clear_buffer_traces(ams);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState keeps a trace per buffer reading",
                 "[ams][buffer][trace]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetBuffer reset{ams};

    // Unit 1 feeds the toolhead: unit 0 reads +0.8, unit 1 reads -0.4.
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    AmsStateTestAccess::sync_buffer(ams, info, 1000);

    REQUIRE_FALSE(ams.buffer_trace(-1).window(1000).empty());
    CHECK(ams.buffer_trace(-1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(0).window(1000).back().bias == Catch::Approx(0.8f));
    CHECK(ams.buffer_trace(1).window(1000).back().bias == Catch::Approx(-0.4f));
    CHECK(ams.buffer_trace(5).size() == 0);

    SECTION("a unit that goes away takes its trace with it") {
        info.units.pop_back();
        info.current_slot = -1;
        AmsStateTestAccess::sync_buffer(ams, info, 2000);
        CHECK(ams.buffer_trace(1).size() == 0);
        CHECK(ams.buffer_trace(-1).window(2000).back().bias == Catch::Approx(0.8f));
    }

    SECTION("clear_backends drops every trace") {
        ams.clear_backends();
        CHECK(ams.buffer_trace(-1).size() == 0);
        CHECK(ams.buffer_trace(0).size() == 0);
    }
}
```

- [ ] **Step 2: Run it.** `make t F='[buffer][trace]'`. Expected FAIL: does not compile, `BufferTrace`, `AmsState::buffer_trace` and `AmsStateTestAccess::sync_buffer` are undeclared.

- [ ] **Step 3: Minimal implementation.**

`include/buffer_reading.h`: add `#include <cstddef>`, `#include <cstdint>`, `#include <deque>`, `#include <vector>`, and before the closing namespace:

```cpp
/// Milliseconds on the clock buffer traces are stamped with. Monotonic, so a
/// wall-clock change cannot move a trace.
[[nodiscard]] int64_t buffer_clock_ms();

/// One reading in a BufferTrace. A point with valid false starts a gap.
struct BufferTracePoint {
    int64_t t_ms = 0;
    float bias = 0.0f;
    bool valid = false;
};

/// About a minute of one buffer's bias, for the trace drawn beside a slider.
///
/// Readings arrive only when they change (OpenAMS republishes on a 0.02 move),
/// so each point holds until the next one: the trace is a step line, and a
/// reading that never changes still draws across the whole window. Main thread
/// only.
class BufferTrace {
  public:
    static constexpr int64_t kWindowMs = 60000;
    /// Bounds memory for a reading that changes on every status update.
    static constexpr std::size_t kMaxPoints = 512;

    /// Note the reading at @p now_ms. A repeat of the newest point adds
    /// nothing. A stamp older than the newest means the clock went back, and
    /// the history it can no longer place is dropped.
    void record(int64_t now_ms, bool valid, float bias);

    /// The points that draw the kWindowMs ending at @p now_ms, oldest first.
    /// When history reaches past the window, the first point is the value held
    /// at its start, stamped now_ms - kWindowMs. Points after @p now_ms are
    /// left out.
    [[nodiscard]] std::vector<BufferTracePoint> window(int64_t now_ms) const;

    void clear() {
        points_.clear();
    }
    [[nodiscard]] std::size_t size() const {
        return points_.size();
    }

  private:
    std::deque<BufferTracePoint> points_;
};
```

`src/printer/buffer_reading.cpp`: add `#include <chrono>` and:

```cpp
int64_t buffer_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void BufferTrace::record(int64_t now_ms, bool valid, float bias) {
    if (!points_.empty() && now_ms < points_.back().t_ms) {
        points_.clear();
    }
    if (!points_.empty()) {
        const BufferTracePoint& last = points_.back();
        if (last.valid == valid && (!valid || last.bias == bias)) {
            return;
        }
    }
    points_.push_back({now_ms, valid ? bias : 0.0f, valid});
    // One point at or before the window's start stays: it is the value held
    // into the window.
    while (points_.size() >= 2 && points_[1].t_ms <= now_ms - kWindowMs) {
        points_.pop_front();
    }
    while (points_.size() > kMaxPoints) {
        points_.pop_front();
    }
}

std::vector<BufferTracePoint> BufferTrace::window(int64_t now_ms) const {
    std::vector<BufferTracePoint> out;
    const int64_t start = now_ms - kWindowMs;
    for (const BufferTracePoint& p : points_) {
        if (p.t_ms > now_ms) {
            break;
        }
        if (p.t_ms <= start) {
            out.assign(1, BufferTracePoint{start, p.bias, p.valid});
        } else {
            out.push_back(p);
        }
    }
    return out;
}
```

`include/ams_state.h`: add `#include "buffer_reading.h"` and `#include <map>`. Public, beside the clog-meter accessors:

```cpp
    /**
     * @brief The last minute of one buffer reading, for the trace beside a slider
     * @param unit Unit position, or -1 for the system-level reading (buffer_reading(info, -1))
     *
     * Empty for a unit that has never had a reading. Main thread only.
     */
    [[nodiscard]] const BufferTrace& buffer_trace(int unit) const;
```

Private, beside `sync_clog_meter_from_info`:

```cpp
    /** @brief Note every buffer reading in its trace */
    void sync_buffer_from_info(const AmsSystemInfo& info, int64_t now_ms);
```

and with the clog members:

```cpp
    /// Buffer reading traces, keyed by unit position, -1 for the system-level reading.
    std::map<int, BufferTrace> buffer_traces_;
```

Create `src/printer/ams_state_buffer.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ams_state.h"
#include "ams_state_internal.h"
#include "buffer_reading.h"

#include <iterator>

namespace helix {
using ams_state_detail::assert_main_thread;

void AmsState::sync_buffer_from_info(const AmsSystemInfo& info, int64_t now_ms) {
    const int unit_count = static_cast<int>(info.units.size());
    // A unit that is gone has no trace to show.
    for (auto it = buffer_traces_.begin(); it != buffer_traces_.end();) {
        it = it->first >= unit_count ? buffer_traces_.erase(it) : std::next(it);
    }

    const BufferReading system = buffer_reading(info, -1);
    buffer_traces_[-1].record(now_ms, system.has_slider, system.bias);
    for (int u = 0; u < unit_count; ++u) {
        const BufferReading r = buffer_reading(info, u);
        buffer_traces_[u].record(now_ms, r.has_slider, r.bias);
    }
}

const BufferTrace& AmsState::buffer_trace(int unit) const {
    assert_main_thread();
    static const BufferTrace kEmpty;
    const auto it = buffer_traces_.find(unit);
    return it == buffer_traces_.end() ? kEmpty : it->second;
}

} // namespace helix
```

`src/printer/ams_state.cpp#AmsState::sync_from_backend`, right after `sync_clog_meter_from_info(info);`:

```cpp
    // Sync the filament buffer reading and its traces
    sync_buffer_from_info(info, buffer_clock_ms());
```

`src/printer/ams_state.cpp#AmsState::clear_backends`, after `optimistic_action_until_.reset();`:

```cpp
    // Every trace describes the departing backend's buffers.
    buffer_traces_.clear();
```

`tests/test_helpers/ams_state_test_access.h#AmsStateTestAccess`, after `sync_clog_meter`:

```cpp
    /// Drive the buffer reading sync with a hand-built AmsSystemInfo at a
    /// chosen time, so trace tests need neither a backend nor a clock.
    static void sync_buffer(AmsState& ams, const AmsSystemInfo& info, int64_t now_ms) {
        ams.sync_buffer_from_info(info, now_ms);
    }

    /// Drop every buffer trace, which outlive a test on the singleton.
    static void clear_buffer_traces(AmsState& ams) {
        ams.buffer_traces_.clear();
    }
```

`app_srcs.txt`: add `src/printer/ams_state_buffer.cpp` after `src/printer/ams_state.cpp`. `python3 scripts/check_esp32_app_srcs.py`: PASS.

- [ ] **Step 4: Run it.** `make t F='[buffer][trace]'`, then `make t F='[ams]'` (the AmsState suites run `sync_from_backend()` and `clear_backends()`). Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N src/printer/ams_state_buffer.cpp tests/unit/test_buffer_trace.cpp tests/unit/test_ams_state_buffer.cpp
git commit -m "feat(ams): keep a minute of every filament buffer reading" \
  -m "BufferTrace holds about 60 s of one buffer's bias as a step line on a monotonic clock, with gaps where the reading goes away. AmsState records the system-level reading and each unit's on every sync and drops them with the backend. Red first: the trace cases did not compile without BufferTrace." \
  -- include/buffer_reading.h src/printer/buffer_reading.cpp src/printer/ams_state_buffer.cpp \
     include/ams_state.h src/printer/ams_state.cpp tests/test_helpers/ams_state_test_access.h \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt \
     tests/unit/test_buffer_trace.cpp tests/unit/test_ams_state_buffer.cpp
git show --stat HEAD
```

---

### Task 4: Slider and trace geometry

**Files:**
- Create: `include/buffer_slider_geometry.h`, `src/ui/buffer_slider_geometry.cpp`, `tests/unit/test_buffer_slider_geometry.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/ui/buffer_slider_geometry.cpp` before `src/ui/clog_meter_geometry.cpp`)

**Interfaces:**
- Consumes: `helix::ui::kPressureWarningPct`, `kPressureFaultPct`, `helix::BufferTracePoint`, `helix::BufferTrace::kWindowMs`.
- Produces:
  - `struct helix::ui::BufferSliderGeometry { int block_y, block_h, target_y, target_h, danger_top_h, danger_bottom_y, danger_bottom_h; };`
  - `int helix::ui::buffer_slider_y(float bias, int height);`
  - `helix::ui::BufferSliderGeometry helix::ui::buffer_slider_geometry(float bias, int height);`
  - `struct helix::ui::BufferTraceXY { int x; int y; };`
  - `std::vector<std::vector<helix::ui::BufferTraceXY>> helix::ui::buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width, int height);`

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_buffer_slider_geometry.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_buffer_slider_geometry.cpp
 * @brief Where the buffer slider's block, target window and end stops land,
 *        and how its trace is laid out. Pure, no LVGL.
 */

#include "buffer_slider_geometry.h"

#include <cmath>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

TEST_CASE("buffer_slider_geometry: loose up, tight down", "[buffer][slider][geometry]") {
    // h = 100: block 12 px, 88 px of travel, centre y = 6 + (1 - bias) * 44.
    CHECK(buffer_slider_geometry(0.0f, 100).block_y == 44);
    CHECK(buffer_slider_geometry(1.0f, 100).block_y == 0);
    CHECK(buffer_slider_geometry(-1.0f, 100).block_y == 88);
    CHECK(buffer_slider_geometry(0.0f, 100).block_h == 12);
}

TEST_CASE("buffer_slider_geometry: the target window and end stops follow the bands",
          "[buffer][slider][geometry]") {
    const auto g = buffer_slider_geometry(0.0f, 100);
    CHECK(g.target_y == 37);        // y of +0.3
    CHECK(g.target_h == 26);        // down to y of -0.3
    CHECK(g.danger_top_h == 19);    // y of +0.7
    CHECK(g.danger_bottom_y == 81); // y of -0.7
    CHECK(g.danger_bottom_h == 19);
}

TEST_CASE("buffer_slider_geometry: out-of-range bias stays in the housing",
          "[buffer][slider][geometry]") {
    CHECK(buffer_slider_geometry(3.0f, 100).block_y == 0);
    CHECK(buffer_slider_geometry(-3.0f, 100).block_y == 88);
    CHECK(buffer_slider_geometry(std::nanf(""), 100).block_y == 44);
}

TEST_CASE("buffer_slider_geometry: small and empty boxes", "[buffer][slider][geometry]") {
    const auto small = buffer_slider_geometry(0.0f, 20);
    CHECK(small.block_h == 4);
    CHECK(small.block_y == 8);
    const auto none = buffer_slider_geometry(0.5f, 0);
    CHECK(none.block_h == 0);
    CHECK(none.target_h == 0);
}

TEST_CASE("buffer_trace_polylines: newest beside the slider, each reading a step",
          "[buffer][trace][geometry]") {
    // now = 100 s, 120 px wide: 60 s of history is 120 px, 2 px per second.
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.5f, true}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    const auto& l = lines[0];
    REQUIRE(l.size() == 4);
    CHECK(l[0].x == 0);   // +0.5 from now ...
    CHECK(l[0].y == 28);
    CHECK(l[1].x == 60);  // ... back to 70 s
    CHECK(l[1].y == 28);
    CHECK(l[2].x == 60);  // step to 0.0 ...
    CHECK(l[2].y == 50);
    CHECK(l[3].x == 120); // ... held back to 40 s, the window's start
    CHECK(l[3].y == 50);
    // The trace and the block place a reading at the same height.
    CHECK(l[0].y == buffer_slider_y(0.5f, 100));
}

TEST_CASE("buffer_trace_polylines: a gap breaks the line", "[buffer][trace][geometry]") {
    const std::vector<BufferTracePoint> w = {{40000, 0.0f, true}, {70000, 0.0f, false}};
    const auto lines = buffer_trace_polylines(w, 100000, 120, 100);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].front().x == 60);
    CHECK(lines[0].back().x == 120);
}

TEST_CASE("buffer_trace_polylines: nothing to draw", "[buffer][trace][geometry]") {
    CHECK(buffer_trace_polylines({}, 100000, 120, 100).empty());
    CHECK(buffer_trace_polylines({{0, 0.0f, true}}, 100000, 0, 100).empty());
}
```

- [ ] **Step 2: Run it.** `make t F='[buffer][geometry]'`. Expected FAIL: does not compile, `buffer_slider_geometry.h` not found.

- [ ] **Step 3: Minimal implementation.** Create `include/buffer_slider_geometry.h`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "buffer_reading.h"

#include <cstdint>
#include <vector>

namespace helix::ui {

/// Pixel layout of the upright buffer slider in a box `height` px tall, y down
/// from its top. Loose is up and tight is down, as filament flows top to bottom
/// on the path canvas. A zero height lays out nothing.
struct BufferSliderGeometry {
    int block_y = 0; ///< Top edge of the block riding the strand
    int block_h = 0;
    int target_y = 0; ///< Dashed target window: the band under kPressureWarningPct
    int target_h = 0;
    int danger_top_h = 0;    ///< Loose end stop, from y = 0
    int danger_bottom_y = 0; ///< Tight end stop, down to y = height
    int danger_bottom_h = 0;
};

/// Centre y of the block for @p bias (-1 tight .. +1 loose, clamped; NaN reads
/// as 0). The slider and its trace both place a reading with this, so the
/// trace lines up with the block it scrolls out of.
int buffer_slider_y(float bias, int height);

BufferSliderGeometry buffer_slider_geometry(float bias, int height);

struct BufferTraceXY {
    int x;
    int y;
};

/// The trace as polylines in a box width x height: newest at x = 0 beside the
/// slider, older readings further right, one polyline per run of valid points.
/// Each reading holds as a step until the next.
std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height);

} // namespace helix::ui
```

Create `src/ui/buffer_slider_geometry.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_slider_geometry.h"

#include "clog_meter_geometry.h"

#include <algorithm>
#include <cmath>

namespace helix::ui {

namespace {

int block_height(int height) {
    return std::max(4, height / 8);
}

} // namespace

int buffer_slider_y(float bias, int height) {
    if (height <= 0) {
        return 0;
    }
    const float b = std::isnan(bias) ? 0.0f : std::clamp(bias, -1.0f, 1.0f);
    const int block_h = block_height(height);
    const int travel = height - block_h;
    return block_h / 2 + static_cast<int>(std::lround((1.0f - b) * travel / 2.0f));
}

BufferSliderGeometry buffer_slider_geometry(float bias, int height) {
    BufferSliderGeometry g;
    if (height <= 0) {
        return g;
    }
    g.block_h = block_height(height);
    g.block_y = buffer_slider_y(bias, height) - g.block_h / 2;
    const float warning = kPressureWarningPct / 100.0f;
    const float fault = kPressureFaultPct / 100.0f;
    g.target_y = buffer_slider_y(warning, height);
    g.target_h = buffer_slider_y(-warning, height) - g.target_y;
    g.danger_top_h = buffer_slider_y(fault, height);
    g.danger_bottom_y = buffer_slider_y(-fault, height);
    g.danger_bottom_h = height - g.danger_bottom_y;
    return g;
}

std::vector<std::vector<BufferTraceXY>>
buffer_trace_polylines(const std::vector<BufferTracePoint>& window, int64_t now_ms, int width,
                       int height) {
    std::vector<std::vector<BufferTraceXY>> lines;
    if (width <= 0 || height <= 0) {
        return lines;
    }
    auto x_of = [&](int64_t t_ms) {
        return static_cast<int>(
            std::clamp<int64_t>((now_ms - t_ms) * width / BufferTrace::kWindowMs, 0, width));
    };
    std::vector<BufferTraceXY> run;
    int newer_x = 0; // where the next newer reading began; the newest holds from now
    for (auto it = window.rbegin(); it != window.rend(); ++it) {
        const int older_x = x_of(it->t_ms);
        if (it->valid) {
            const int y = buffer_slider_y(it->bias, height);
            run.push_back({newer_x, y});
            run.push_back({older_x, y});
        } else if (!run.empty()) {
            lines.push_back(std::move(run));
            run.clear();
        }
        newer_x = older_x;
    }
    if (!run.empty()) {
        lines.push_back(std::move(run));
    }
    return lines;
}

} // namespace helix::ui
```

`app_srcs.txt`: add `src/ui/buffer_slider_geometry.cpp` on the line before `src/ui/clog_meter_geometry.cpp`. `python3 scripts/check_esp32_app_srcs.py`: PASS.

- [ ] **Step 4: Run it.** `make t F='[buffer][geometry]'`. Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N include/buffer_slider_geometry.h src/ui/buffer_slider_geometry.cpp tests/unit/test_buffer_slider_geometry.cpp
git commit -m "feat(ui): pure geometry for the filament buffer slider and its trace" \
  -m "buffer_slider_geometry() places the block, the dashed target window and both end-stop zones from the 30/70 bands for any height; buffer_trace_polylines() lays a trace out as steps scrolling right from the slider, breaking at gaps. Red first: the cases did not compile without the header." \
  -- include/buffer_slider_geometry.h src/ui/buffer_slider_geometry.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt tests/unit/test_buffer_slider_geometry.cpp
git show --stat HEAD
```

---

### Task 5: `UiBufferSlider` renderer

Used by nothing yet; Tasks 6, 7 and 10 wire it.

**Files:**
- Create: `include/ui_buffer_slider.h`, `src/ui/ui_buffer_slider.cpp`, `tests/unit/test_ui_buffer_slider.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/ui/ui_buffer_slider.cpp` after `src/ui/ui_buffer_meter.cpp`)

**Interfaces:**
- Consumes: `buffer_slider_geometry()`, `buffer_trace_polylines()`, `buffer_slider_y()`, `buffer_status_token()`, `AmsState::buffer_trace(int)`, `buffer_clock_ms()`, `helix::ui::lv_timer_cancel_safe()`.
- Produces: `class helix::ui::UiBufferSlider { explicit UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj = nullptr, int trace_unit = -1); void set_reading(float bias, ClogMeterStatus status); float bias() const; ClogMeterStatus status() const; int trace_ticks() const; };`

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_ui_buffer_slider.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ui_buffer_slider.cpp
 * @brief UiBufferSlider's lifecycle: the trace timer, and outliving its objects.
 */

#include "ui_buffer_slider.h"

#include "../lvgl_test_fixture.h"
#include "ams_state.h"

#include <memory>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ClogMeterStatus;
using helix::ui::UiBufferSlider;

namespace {
lv_obj_t* box(lv_obj_t* parent, int w, int h) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    return obj;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider redraws its trace once a second",
                 "[buffer][slider]") {
    AmsState::instance().init_subjects(false);
    UiBufferSlider slider(box(test_screen(), 24, 120), box(test_screen(), 160, 120), -1);
    process_lvgl(2100);
    CHECK(slider.trace_ticks() >= 2);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider without a trace runs no timer",
                 "[buffer][slider]") {
    UiBufferSlider slider(box(test_screen(), 24, 120));
    process_lvgl(2100);
    CHECK(slider.trace_ticks() == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider paints a reading", "[buffer][slider]") {
    AmsState::instance().init_subjects(false);
    UiBufferSlider slider(box(test_screen(), 24, 120), box(test_screen(), 160, 120), -1);
    slider.set_reading(-0.45f, ClogMeterStatus::Warning);
    lv_refr_now(nullptr);
    CHECK(slider.bias() == Catch::Approx(-0.45f));
    CHECK(slider.status() == ClogMeterStatus::Warning);
}

TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider outlives the objects it draws into",
                 "[buffer][slider]") {
    lv_obj_t* slider_obj = box(test_screen(), 24, 120);
    lv_obj_t* trace_obj = box(test_screen(), 160, 120);
    auto slider = std::make_unique<UiBufferSlider>(slider_obj, trace_obj, -1);

    lv_obj_delete(trace_obj);
    process_lvgl(1100);
    CHECK(slider->trace_ticks() == 0); // the timer stopped with its object

    lv_obj_delete(slider_obj);
    slider.reset(); // removes no callback from a freed object
    process_lvgl(100);
}
```

- [ ] **Step 2: Run it.** `make t F='[buffer][slider]'`. Expected FAIL: does not compile, `ui_buffer_slider.h` not found.

- [ ] **Step 3: Minimal implementation.** Create `include/ui_buffer_slider.h`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "clog_meter_geometry.h"
#include "lvgl/lvgl.h"

namespace helix::ui {

/**
 * @brief Draws a filament buffer reading as an upright slider, and optionally
 *        its last minute as a trace beside it.
 *
 * A housing on the filament strand with a block riding it: loose is up, tight
 * is down. A dashed window marks the target and faint zones mark both end
 * stops; only the block moves, in buffer_status_token() of its severity. The
 * layout is buffer_slider_geometry(); this class only paints it into objects
 * XML authored and sized.
 */
class UiBufferSlider {
  public:
    /// @p slider_obj draws the slider. @p trace_obj, when given, draws
    /// AmsState::buffer_trace(@p trace_unit) and is redrawn once a second, so
    /// the trace scrolls with no new reading.
    explicit UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj = nullptr,
                            int trace_unit = -1);
    ~UiBufferSlider();

    UiBufferSlider(const UiBufferSlider&) = delete;
    UiBufferSlider& operator=(const UiBufferSlider&) = delete;

    void set_reading(float bias, ClogMeterStatus status);

    [[nodiscard]] float bias() const {
        return bias_;
    }
    [[nodiscard]] ClogMeterStatus status() const {
        return status_;
    }
    /// How many times the once-a-second timer has redrawn the trace.
    [[nodiscard]] int trace_ticks() const {
        return trace_ticks_;
    }

  private:
    static void on_draw(lv_event_t* e);
    static void on_deleted(lv_event_t* e);
    static void on_trace_timer(lv_timer_t* timer);

    void draw_slider(lv_layer_t* layer) const;
    void draw_trace(lv_layer_t* layer) const;

    lv_obj_t* slider_obj_ = nullptr;
    lv_obj_t* trace_obj_ = nullptr;
    int trace_unit_ = -1;
    lv_timer_t* trace_timer_ = nullptr;
    int trace_ticks_ = 0;

    float bias_ = 0.0f;
    ClogMeterStatus status_ = ClogMeterStatus::Ok;
};

} // namespace helix::ui
```

Create `src/ui/ui_buffer_slider.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_buffer_slider.h"

#include "ui_timer_guard.h"

#include "ams_state.h"
#include "buffer_reading.h"
#include "buffer_slider_geometry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

namespace helix::ui {

namespace {
constexpr uint32_t kTraceRedrawMs = 1000;

lv_point_precise_t point(int32_t x, int32_t y) {
    return {static_cast<lv_value_precise_t>(x), static_cast<lv_value_precise_t>(y)};
}
} // namespace

UiBufferSlider::UiBufferSlider(lv_obj_t* slider_obj, lv_obj_t* trace_obj, int trace_unit)
    : slider_obj_(slider_obj), trace_obj_(trace_obj), trace_unit_(trace_unit) {
    if (!slider_obj_) {
        spdlog::error("[BufferSlider] No object to draw the slider into");
        trace_obj_ = nullptr;
        return;
    }
    // Draw and delete hooks only: XML authors and sizes both objects.
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN, this);
            lv_obj_add_event_cb(obj, on_deleted, LV_EVENT_DELETE, this);
        }
    }
    if (trace_obj_) {
        trace_timer_ = lv_timer_create(on_trace_timer, kTraceRedrawMs, this);
    }
}

UiBufferSlider::~UiBufferSlider() {
    lv_timer_cancel_safe(trace_timer_);
    for (lv_obj_t* obj : {slider_obj_, trace_obj_}) {
        if (obj) {
            lv_obj_remove_event_cb_with_user_data(obj, on_draw, this);
            lv_obj_remove_event_cb_with_user_data(obj, on_deleted, this);
        }
    }
}

void UiBufferSlider::set_reading(float bias, ClogMeterStatus status) {
    bias_ = bias;
    status_ = status;
    if (slider_obj_) {
        lv_obj_invalidate(slider_obj_);
    }
}

void UiBufferSlider::on_draw(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!self || !layer) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->draw_slider(layer);
    } else if (target == self->trace_obj_) {
        self->draw_trace(layer);
    }
}

void UiBufferSlider::on_deleted(lv_event_t* e) {
    auto* self = static_cast<UiBufferSlider*>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    if (target == self->slider_obj_) {
        self->slider_obj_ = nullptr;
    }
    if (target == self->trace_obj_) {
        self->trace_obj_ = nullptr;
        lv_timer_cancel_safe(self->trace_timer_);
        self->trace_timer_ = nullptr;
    }
}

void UiBufferSlider::on_trace_timer(lv_timer_t* timer) {
    auto* self = static_cast<UiBufferSlider*>(lv_timer_get_user_data(timer));
    if (self && self->trace_obj_) {
        ++self->trace_ticks_;
        lv_obj_invalidate(self->trace_obj_);
    }
}

void UiBufferSlider::draw_slider(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(slider_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const BufferSliderGeometry g = buffer_slider_geometry(bias_, h);
    const lv_color_t muted = theme_manager_get_color("text_muted");
    const int32_t radius = theme_manager_get_spacing("space_xxs");
    const int32_t cx = a.x1 + w / 2;

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = theme_manager_get_color("danger");
    fill.opa = LV_OPA_20;
    const lv_area_t loose_stop = {a.x1, a.y1, a.x2, a.y1 + g.danger_top_h - 1};
    const lv_area_t tight_stop = {a.x1, a.y1 + g.danger_bottom_y, a.x2, a.y2};
    lv_draw_fill(layer, &fill, &loose_stop);
    lv_draw_fill(layer, &fill, &tight_stop);

    lv_draw_border_dsc_t housing;
    lv_draw_border_dsc_init(&housing);
    housing.color = muted;
    housing.width = 1;
    housing.radius = radius;
    lv_draw_border(layer, &housing, &a);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.width = 2;
    line.p1 = point(cx, a.y1);
    line.p2 = point(cx, a.y2);
    lv_draw_line(layer, &line);

    line.width = 1;
    line.dash_width = 3;
    line.dash_gap = 3;
    for (int32_t y : {a.y1 + g.target_y, a.y1 + g.target_y + g.target_h}) {
        line.p1 = point(a.x1, y);
        line.p2 = point(a.x2, y);
        lv_draw_line(layer, &line);
    }

    fill.color = theme_manager_get_color(buffer_status_token(status_));
    fill.opa = LV_OPA_COVER;
    fill.radius = radius;
    const lv_area_t block = {a.x1 + 2, a.y1 + g.block_y, a.x2 - 2, a.y1 + g.block_y + g.block_h - 1};
    lv_draw_fill(layer, &fill, &block);
}

void UiBufferSlider::draw_trace(lv_layer_t* layer) const {
    lv_area_t a;
    lv_obj_get_content_coords(trace_obj_, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return;
    }
    const lv_color_t muted = theme_manager_get_color("text_muted");

    // The target window carries on across the trace, so a reading reads
    // against it the way the block does.
    const BufferSliderGeometry g = buffer_slider_geometry(0.0f, h);
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = muted;
    line.width = 1;
    line.dash_width = 3;
    line.dash_gap = 3;
    for (int32_t y : {a.y1 + g.target_y, a.y1 + g.target_y + g.target_h}) {
        line.p1 = point(a.x1, y);
        line.p2 = point(a.x2, y);
        lv_draw_line(layer, &line);
    }

    const int64_t now = buffer_clock_ms();
    const auto lines = buffer_trace_polylines(
        AmsState::instance().buffer_trace(trace_unit_).window(now), now, w, h);
    lv_draw_line_dsc_t trace;
    lv_draw_line_dsc_init(&trace);
    trace.color = muted;
    trace.width = 2;
    trace.round_start = 1;
    trace.round_end = 1;
    for (const auto& run : lines) {
        for (std::size_t i = 1; i < run.size(); ++i) {
            trace.p1 = point(a.x1 + run[i - 1].x, a.y1 + run[i - 1].y);
            trace.p2 = point(a.x1 + run[i].x, a.y1 + run[i].y);
            lv_draw_line(layer, &trace);
        }
    }
}

} // namespace helix::ui
```

`app_srcs.txt`: add `src/ui/ui_buffer_slider.cpp` after `src/ui/ui_buffer_meter.cpp`. `python3 scripts/check_esp32_app_srcs.py`: PASS. If `scripts/check_imperative_ui.py` flags the two `lv_obj_add_event_cb` calls in the commit hook, add `// DECLARATIVE_OK: draw and delete hooks have no XML form` on that line.

- [ ] **Step 4: Run it.** `make t F='[buffer][slider]'`. Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N include/ui_buffer_slider.h src/ui/ui_buffer_slider.cpp tests/unit/test_ui_buffer_slider.cpp
git commit -m "feat(ui): UiBufferSlider draws a filament buffer reading and its trace" \
  -m "One renderer for every buffer surface: the slider (housing, strand, dashed target window, end-stop zones, a block in the severity's token) and an optional trace of AmsState's last minute, redrawn once a second. It paints into XML-authored objects and survives them being deleted first. Red first: the cases did not compile without the class." \
  -- include/ui_buffer_slider.h src/ui/ui_buffer_slider.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt tests/unit/test_ui_buffer_slider.cpp
git show --stat HEAD
```

---

### Task 6: System-level `buffer_*` subjects and the loaded-spool card

**Files:**
- Modify: `include/ams_state.h` (subject getters, members, `#AmsState::publish_buffer_reading`)
- Modify: `src/printer/ams_state_buffer.cpp#AmsState::sync_buffer_from_info` (publish), add `#AmsState::publish_buffer_reading`
- Modify: `src/printer/ams_state_internal.h` (add `copy_string_if_changed`), `src/printer/ams_state_clog.cpp#copy_if_changed` (delete; use the shared one)
- Modify: `src/printer/ams_state_subjects.cpp#AmsState::init_subjects`, `#AmsState::register_xml_subject_names`
- Modify: `src/printer/ams_state.cpp#AmsState::clear_backends`
- Modify: `include/ui_buffer_slider.h`, `src/ui/ui_buffer_slider.cpp` (add `follow_system_reading`)
- Modify: `ui_xml/components/ams_loaded_card.xml`, `ui_xml/globals.xml` (tokens `buffer_slider_w`, `buffer_mini_h`)
- Modify: `include/ui_ams_sidebar.h#AmsOperationSidebar` (member), `src/ui/ui_ams_sidebar.cpp#AmsOperationSidebar::setup`, cleanup beside `clog_meter_.reset()`
- Modify: `src/remote/mock_scenarios.cpp#set_fps`, `#clog_scenarios` (add `buffer_fps_no_target`)
- Modify: `tests/unit/test_ams_state_buffer.cpp`, `tests/unit/test_ui_buffer_slider.cpp`
- Create: `tests/unit/test_ams_loaded_card_buffer.cpp`

**Interfaces:**
- Consumes: `buffer_reading()`, `buffer_label()`, `buffer_value_text()`, `UiBufferSlider`.
- Produces:
  - XML subjects `buffer_present`, `buffer_slider`, `buffer_bias_pct`, `buffer_status`, `buffer_label`, `buffer_value_text`.
  - `lv_subject_t* AmsState::get_buffer_present_subject();` and likewise `get_buffer_slider_subject()`, `get_buffer_bias_pct_subject()`, `get_buffer_status_subject()`, `get_buffer_label_subject()`, `get_buffer_value_text_subject()`.
  - `void AmsState::publish_buffer_reading(const BufferReading& r);` (private)
  - `void helix::ams_state_detail::copy_string_if_changed(lv_subject_t* subject, const char* text);`
  - `void helix::ui::UiBufferSlider::follow_system_reading();`
  - Mock scenario `buffer_fps_no_target`.

- [ ] **Step 1: Write the failing test.** Append to `tests/unit/test_ams_state_buffer.cpp` (add `#include "clog_meter_geometry.h"` and `#include <string>`):

```cpp
namespace {
int int_of(lv_subject_t* s) {
    return lv_subject_get_int(s);
}
std::string text_of(lv_subject_t* s) {
    return lv_subject_get_string(s);
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "AmsState publishes the system-level buffer reading",
                 "[ams][buffer][subjects]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    ResetBuffer reset{ams};

    SECTION("Happy Hare sync feedback") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_buffer(ams, info, 0);
        CHECK(int_of(ams.get_buffer_present_subject()) == 1);
        CHECK(int_of(ams.get_buffer_slider_subject()) == 1);
        CHECK(int_of(ams.get_buffer_bias_pct_subject()) == -45);
        CHECK(int_of(ams.get_buffer_status_subject()) ==
              static_cast<int>(ui::ClogMeterStatus::Warning));
        CHECK(text_of(ams.get_buffer_label_subject()) == "Sync");
        CHECK(text_of(ams.get_buffer_value_text_subject()) == "-45%");
    }

    SECTION("a set point that goes away mid-print leaves text and a gap") {
        AmsSystemInfo info = test::fps_units({0.62f});
        AmsStateTestAccess::sync_buffer(ams, info, 1000);
        REQUIRE(int_of(ams.get_buffer_slider_subject()) == 1);

        info.units[0].buffer_health->fps_set_point = -1.0f;
        info.sync_feedback_bias = info.pressure_sensor_bias();
        AmsStateTestAccess::sync_buffer(ams, info, 2000);
        CHECK(int_of(ams.get_buffer_present_subject()) == 1);
        CHECK(int_of(ams.get_buffer_slider_subject()) == 0);
        CHECK(int_of(ams.get_buffer_bias_pct_subject()) == 0);
        CHECK(int_of(ams.get_buffer_status_subject()) == 0);
        CHECK(text_of(ams.get_buffer_value_text_subject()) == "Pressure: 62%");
        const auto w = ams.buffer_trace(-1).window(2000);
        REQUIRE(w.size() == 2);
        CHECK_FALSE(w.back().valid);

        SECTION("and comes back") {
            info.units[0].buffer_health->fps_set_point = 0.5f;
            AmsStateTestAccess::sync_buffer(ams, info, 3000);
            CHECK(int_of(ams.get_buffer_slider_subject()) == 1);
            CHECK(ams.buffer_trace(-1).window(3000).back().valid);
        }
    }

    SECTION("clear_backends takes the reading away") {
        AmsSystemInfo info;
        info.sync_feedback_bias = -0.45f;
        AmsStateTestAccess::sync_buffer(ams, info, 0);
        ams.clear_backends();
        CHECK(int_of(ams.get_buffer_present_subject()) == 0);
        CHECK(text_of(ams.get_buffer_label_subject()).empty());
    }
}
```

Append to `tests/unit/test_ui_buffer_slider.cpp` (add `#include "../test_helpers/ams_state_test_access.h"`):

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "UiBufferSlider follows the system-level reading",
                 "[buffer][slider]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(false);
    UiBufferSlider slider(box(test_screen(), 24, 120));
    slider.follow_system_reading();

    AmsSystemInfo info;
    info.sync_feedback_bias = -0.45f;
    AmsStateTestAccess::sync_buffer(ams, info, 0);
    CHECK(slider.bias() == Catch::Approx(-0.45f));
    CHECK(slider.status() == ClogMeterStatus::Warning);

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    CHECK(slider.bias() == Catch::Approx(0.0f));
    CHECK(slider.status() == ClogMeterStatus::Ok);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
```

Create `tests/unit/test_ams_loaded_card_buffer.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_loaded_card_buffer.cpp
 * @brief The loaded-spool card's buffer slider: shown with a reading, the
 *        slider only with a set point, the number always.
 */

#include "ui_panel_ams.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "ams_state.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
bool hidden(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
std::string text(lv_obj_t* card, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(card, name);
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "loaded card draws the buffer beside the material",
                 "[ams][buffer][loaded_card]") {
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    ensure_ams_widgets_registered();
    auto* card = static_cast<lv_obj_t*>(lv_xml_create(test_screen(), "ams_loaded_card", nullptr));
    REQUIRE(card != nullptr);

    SECTION("with a set point: slider and number") {
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}), 0);
        CHECK_FALSE(hidden(card, "buffer_mini"));
        CHECK_FALSE(hidden(card, "buffer_mini_slider"));
        CHECK(text(card, "buffer_mini_value") == "32%");
        CHECK(text(card, "buffer_mini_label") == "FPS");
    }

    SECTION("without one: the number alone") {
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.32f}, -1.0f), 0);
        CHECK_FALSE(hidden(card, "buffer_mini"));
        CHECK(hidden(card, "buffer_mini_slider"));
        CHECK(text(card, "buffer_mini_value") == "Pressure: 32%");
    }

    SECTION("no reading: nothing") {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        CHECK(hidden(card, "buffer_mini"));
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
```

- [ ] **Step 2: Run it.** `make t F='[buffer][subjects]'`, `make t F='[loaded_card]'`. Expected FAIL: does not compile, `get_buffer_present_subject` and `follow_system_reading` undeclared.

- [ ] **Step 3: Minimal implementation.**

`src/printer/ams_state_internal.h`: add `#include "lvgl/lvgl.h"` and `#include <cstring>`, and:

```cpp
/// Write @p text into a string subject only when it differs, so observers are
/// not notified of a value they already have.
inline void copy_string_if_changed(lv_subject_t* subject, const char* text) {
    if (std::strcmp(lv_subject_get_string(subject), text) != 0) {
        lv_subject_copy_string(subject, text);
    }
}
```

`src/printer/ams_state_clog.cpp`: delete the local `copy_if_changed()`, add `using ams_state_detail::copy_string_if_changed;` beside the existing `using`, and rename its four calls in `publish()` to `copy_string_if_changed`.

`include/ams_state.h`, public, after `buffer_trace()`:

```cpp
    /// The system-level buffer reading (buffer_reading(info, -1)), as the home
    /// widget and the loaded card bind it. Observe with get_subjects_lifetime().
    lv_subject_t* get_buffer_present_subject() {
        return &buffer_present_;
    }
    lv_subject_t* get_buffer_slider_subject() {
        return &buffer_slider_;
    }
    lv_subject_t* get_buffer_bias_pct_subject() {
        return &buffer_bias_pct_;
    }
    lv_subject_t* get_buffer_status_subject() {
        return &buffer_status_;
    }
    lv_subject_t* get_buffer_label_subject() {
        return &buffer_label_;
    }
    lv_subject_t* get_buffer_value_text_subject() {
        return &buffer_value_text_;
    }
```

Private, beside `sync_buffer_from_info`, and change its doc to `/** @brief Publish the system-level buffer reading and note every reading in its trace */`:

```cpp
    /** @brief Write one reading onto the buffer_* subjects */
    void publish_buffer_reading(const BufferReading& r);
```

Members, after `buffer_traces_`:

```cpp
    lv_subject_t buffer_present_{};  // 0/1: a proportional reading exists (widget gate)
    lv_subject_t buffer_slider_{};   // 0/1: it has a set point, so the slider draws
    lv_subject_t buffer_bias_pct_{}; // -100 tight .. +100 loose
    lv_subject_t buffer_status_{};   // ClogMeterStatus of the bias
    lv_subject_t buffer_label_{};    // "FPS" / "Sync"
    char buffer_label_buf_[16]{};
    lv_subject_t buffer_value_text_{}; // "32%", "-45%", "Pressure: 32%"
    char buffer_value_text_buf_[48]{};
```

`src/printer/ams_state_buffer.cpp`: add `#include <cmath>`, `using ams_state_detail::copy_string_if_changed;`, call `publish_buffer_reading(system);` right after `const BufferReading system = buffer_reading(info, -1);`, and:

```cpp
void AmsState::publish_buffer_reading(const BufferReading& r) {
    lv_subject_set_int(&buffer_present_, r.present() ? 1 : 0);
    lv_subject_set_int(&buffer_slider_, r.has_slider ? 1 : 0);
    lv_subject_set_int(&buffer_bias_pct_, static_cast<int>(std::lround(r.bias * 100.0f)));
    lv_subject_set_int(&buffer_status_, static_cast<int>(r.status));
    copy_string_if_changed(&buffer_label_, buffer_label(r));
    copy_string_if_changed(&buffer_value_text_, buffer_value_text(r).c_str());
}
```

`src/printer/ams_state.cpp#AmsState::clear_backends`, under `buffer_traces_.clear();`:

```cpp
    if (initialized_) {
        publish_buffer_reading(BufferReading{});
    }
```

`src/printer/ams_state_subjects.cpp#AmsState::init_subjects`, after the clog block:

```cpp
    // Filament buffer reading, system level
    INIT_SUBJECT_INT(buffer_present, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(buffer_slider, 0, subjects_, register_xml);
    INIT_SUBJECT_INT(buffer_bias_pct, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_INT(buffer_status, 0, subjects_,
                     register_xml); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    INIT_SUBJECT_STRING(buffer_label, "", subjects_, register_xml);
    INIT_SUBJECT_STRING(buffer_value_text, "", subjects_, register_xml);
```

`#AmsState::register_xml_subject_names`, after the clog block:

```cpp
    // Filament buffer reading
    helix::xml::register_subject_in_current_scope("buffer_present", &buffer_present_);
    helix::xml::register_subject_in_current_scope("buffer_slider", &buffer_slider_);
    helix::xml::register_subject_in_current_scope(
        "buffer_bias_pct",
        &buffer_bias_pct_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope(
        "buffer_status",
        &buffer_status_); // SUBJECT_OK: UiBufferSlider::follow_system_reading observes it
    helix::xml::register_subject_in_current_scope("buffer_label", &buffer_label_);
    helix::xml::register_subject_in_current_scope("buffer_value_text", &buffer_value_text_);
```

`include/ui_buffer_slider.h`: add `#include "ui_observer_guard.h"`, the public method and two members:

```cpp
    /// Draw AmsState's system-level reading (buffer_bias_pct, buffer_status),
    /// following it as it changes.
    void follow_system_reading();
```

```cpp
    ObserverGuard bias_observer_;
    ObserverGuard status_observer_;
```

`src/ui/ui_buffer_slider.cpp`: add `#include "observer_factory.h"`; first lines of `~UiBufferSlider()`:

```cpp
    bias_observer_.reset();
    status_observer_.reset();
```

and:

```cpp
void UiBufferSlider::follow_system_reading() {
    auto& ams = AmsState::instance();
    const auto lifetime = ams.get_subjects_lifetime();
    // Immediate: the handlers only store the reading and invalidate.
    bias_observer_ = observe<int>(
        ams.get_buffer_bias_pct_subject(), this,
        [](UiBufferSlider* self, int pct) { self->set_reading(pct / 100.0f, self->status_); },
        lifetime, Dispatch::Immediate);
    status_observer_ = observe<int>(
        ams.get_buffer_status_subject(), this,
        [](UiBufferSlider* self, int status) {
            self->set_reading(self->bias_, static_cast<ClogMeterStatus>(status));
        },
        lifetime, Dispatch::Immediate);
}
```

`ui_xml/globals.xml`, after the `meter_track_h_*` block (and in that block's comment change `(clog_bar_page.xml)` to `(clog_bar_body.xml)`):

```xml
    <!-- buffer_slider_w: the width of the upright filament buffer slider's housing
         (UiBufferSlider) on the home widget and in the Buffer Status modal. -->
    <px name="buffer_slider_w_micro" value="14"/>
    <px name="buffer_slider_w_tiny" value="16"/>
    <px name="buffer_slider_w_small" value="18"/>
    <px name="buffer_slider_w_medium" value="20"/>
    <px name="buffer_slider_w_large" value="24"/>
    <px name="buffer_slider_w_xlarge" value="28"/>
    <px name="buffer_slider_w_xxlarge" value="32"/>
    <!-- buffer_mini_h: the loaded-spool card's small buffer slider, about the
         height of the clog arc beside it. -->
    <px name="buffer_mini_h_micro" value="32"/>
    <px name="buffer_mini_h_tiny" value="36"/>
    <px name="buffer_mini_h_small" value="40"/>
    <px name="buffer_mini_h_medium" value="44"/>
    <px name="buffer_mini_h_large" value="52"/>
    <px name="buffer_mini_h_xlarge" value="60"/>
    <px name="buffer_mini_h_xxlarge" value="72"/>
```

`ui_xml/components/ams_loaded_card.xml`: in the header comment, after the `clog_meter_mode_text` line add `    - buffer_present / buffer_slider / buffer_value_text / buffer_label: the\n      filament buffer, drawn by UiBufferSlider into buffer_mini_slider`; after the closing `</lv_obj>` of `clog_meter`, add:

```xml
    <!-- Filament buffer mini slider (right side, hidden without a reading). The
         slider shows only with a set point to centre on; the number always. -->
    <lv_obj name="buffer_mini"
            width="content" height="content" style_pad_all="0" scrollable="false" flex_flow="column"
            style_flex_main_place="center" style_flex_cross_place="center" style_pad_gap="0">
      <bind_flag_if_eq subject="buffer_present" flag="hidden" ref_value="0"/>
      <lv_obj name="buffer_mini_slider"
              width="#buffer_slider_w" height="#buffer_mini_h" style_pad_all="0" scrollable="false"
              clickable="false">
        <bind_flag_if_eq subject="buffer_slider" flag="hidden" ref_value="0"/>
      </lv_obj>
      <text_tiny name="buffer_mini_value" bind_text="buffer_value_text" style_text_align="center"/>
      <text_tiny name="buffer_mini_label"
                 bind_text="buffer_label" style_text_color="#text_muted" style_text_align="center"/>
    </lv_obj>
```

`include/ui_ams_sidebar.h`: `#include "ui_buffer_slider.h"`; under `std::unique_ptr<UiClogMeter> clog_meter_;` add `std::unique_ptr<UiBufferSlider> buffer_slider_;`. `src/ui/ui_ams_sidebar.cpp#AmsOperationSidebar::setup`, after the clog meter:

```cpp
    // The loaded card's buffer slider follows the system-level reading
    buffer_slider_ =
        std::make_unique<UiBufferSlider>(lv_obj_find_by_name(sidebar_root_, "buffer_mini_slider"));
    buffer_slider_->follow_system_reading();
```

and after `clog_meter_.reset();` in the cleanup: `buffer_slider_.reset();`.

`src/remote/mock_scenarios.cpp`: give `set_fps` a set point and add a scenario:

```cpp
/// A filament pressure sensor on unit 0 reading `pressure` against
/// `set_point` (-1: none published), and nothing else measuring. Read as such
/// by every simulated type but Happy Hare, whose buffer is system-level.
static void set_fps(AmsBackendMock& mock, float pressure, float set_point = 0.5f) {
    clear_clog_sources(mock);
    BufferHealth h;
    h.fps_value = h.smoothed_fps = pressure;
    h.fps_set_point = set_point;
    h.fps_reported = true;
    mock.set_unit_buffer_health(0, h);
}
```

```cpp
    s.push_back({"buffer_fps_no_target", "Filament pressure sensor with no set point",
                 []() { apply_clog_state([](AmsBackendMock& m) { set_fps(m, 0.32f, -1.0f); }); }});
```

(after `buffer_fps_loose`).

- [ ] **Step 4: Run it.** `make t F='[buffer]'`, `make t F='[loaded_card]'`, `make t F='[clog]'` (the shared string helper). Expected PASS. Then `python3 scripts/check_orphan_subjects.py --baseline scripts/orphan_subject_baseline.txt --summary`: PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N tests/unit/test_ams_loaded_card_buffer.cpp
git commit -m "feat(ams): the loaded-spool card draws the filament buffer" \
  -m "AmsState publishes the system-level buffer reading as buffer_* subjects, zeroed with the backend, and the card shows a small upright slider beside the material, the number alone when there is no set point. Mock scenario buffer_fps_no_target added. Red first: the subject and card cases did not compile, then failed on hidden buffer_mini." \
  -- include/ams_state.h src/printer/ams_state_buffer.cpp src/printer/ams_state_internal.h \
     src/printer/ams_state_clog.cpp src/printer/ams_state_subjects.cpp src/printer/ams_state.cpp \
     include/ui_buffer_slider.h src/ui/ui_buffer_slider.cpp ui_xml/components/ams_loaded_card.xml \
     ui_xml/globals.xml include/ui_ams_sidebar.h src/ui/ui_ams_sidebar.cpp src/remote/mock_scenarios.cpp \
     tests/unit/test_ams_state_buffer.cpp tests/unit/test_ui_buffer_slider.cpp \
     tests/unit/test_ams_loaded_card_buffer.cpp
git show --stat HEAD
```

---

### Task 7: "Filament Buffer" home widget

**Files:**
- Create: `src/ui/panel_widgets/filament_buffer_widget.h`, `src/ui/panel_widgets/filament_buffer_widget.cpp`, `ui_xml/components/panel_widget_filament_buffer.xml`, `tests/unit/test_filament_buffer_widget.cpp`
- Modify: `src/ui/panel_widget_registry.cpp` (`s_widget_defs` row, declaration, `init_widget_registrations`)
- Modify: `src/ui/panel_widget_manager.cpp#gate_subject_lifetime`
- Modify: `src/xml_registration.cpp#register_xml_components`
- Modify: `include/ams_state.h`, `src/printer/ams_state_buffer.cpp#AmsState::publish_buffer_reading`, `src/printer/ams_state_subjects.cpp` (subject `buffer_lean_text`)
- Modify: `tests/unit/test_grid_layout.cpp` (half-cell table), `tests/unit/test_registry_span_bands.cpp` (bands table), `tests/unit/test_gate_subject_lifetime.cpp#owners`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/ui/panel_widgets/filament_buffer_widget.cpp` after `src/ui/panel_widgets/fan_widget.cpp`)
- Modify: `translations/*.yml`, `ui_xml/translations/*.xml`

**Interfaces:**
- Consumes: `UiBufferSlider::follow_system_reading()`, `buffer_lean_text()`, `BufferStatusModal::show_for(int)`, subjects from Task 6.
- Produces: `class helix::FilamentBufferWidget : public PanelWidget` with id `"filament_buffer"`; `void helix::register_filament_buffer_widget();`; widget subject `filament_buffer_wide`; XML subject `buffer_lean_text` with `lv_subject_t* AmsState::get_buffer_lean_text_subject();`.

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_filament_buffer_widget.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_filament_buffer_widget.cpp
 * @brief The Filament Buffer home widget: its registry row, its gate, and
 *        what it shows at 1x1 and 2x1.
 */

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/ams_state_test_access.h"
#include "../test_helpers/buffer_infos.h"
#include "../test_helpers/panel_widget_size_harness.h"
#include "ams_state.h"
#include "grid_layout.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"
#include "src/ui/panel_widgets/filament_buffer_widget.h"

#include <string>
#include <string_view>

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
constexpr int kCell = GridLayout::TRACKS_PER_CELL;

bool hidden(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
std::string text(lv_obj_t* obj) {
    REQUIRE(obj != nullptr);
    return lv_label_get_text(obj);
}
} // namespace

TEST_CASE("filament_buffer is one cell, growable to two wide", "[widget_size][filament_buffer]") {
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);
    CHECK(def->colspan == 1 * kCell);
    CHECK(def->rowspan == 1 * kCell);
    CHECK(def->effective_min_colspan() == 1 * kCell);
    CHECK(def->effective_max_colspan() == 2 * kCell);
    CHECK(def->effective_max_rowspan() == 1 * kCell);
    REQUIRE(def->hardware_gate_subject != nullptr);
    CHECK(std::string_view(def->hardware_gate_subject) == "buffer_present");
    CHECK_FALSE(def->default_enabled);
}

TEST_CASE_METHOD(LVGLUITestFixture, "filament_buffer: what each size and reading shows",
                 "[filament_buffer]") {
    PanelWidgetManager::instance().init_widget_subjects();
    auto& ams = AmsState::instance();
    ams.init_subjects(true);
    const auto* def = find_widget_def("filament_buffer");
    REQUIRE(def != nullptr);

    PanelWidgetHarness<FilamentBufferWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);

    AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}), 0);
    lv_subject_t* gate = lv_xml_get_subject(nullptr, "buffer_present");
    REQUIRE(gate != nullptr);
    CHECK(lv_subject_get_int(gate) == 1);

    SECTION("1x1: slider, label and number, no trace") {
        h.resize(def->colspan, def->rowspan, 112, 112);
        CHECK_FALSE(hidden(h.child("buffer_graphics")));
        CHECK(hidden(h.child("buffer_trace")));
        CHECK(hidden(h.child("buffer_lean")));
        CHECK(text(h.child("buffer_label")) == "FPS");
        CHECK(text(h.child("buffer_value")) == "71%");
    }

    SECTION("2x1: the trace and the lean in words") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        CHECK_FALSE(hidden(h.child("buffer_trace")));
        CHECK_FALSE(hidden(h.child("buffer_lean")));
        CHECK(text(h.child("buffer_lean")) == "Running loose");
    }

    SECTION("no set point: the number alone") {
        h.resize(2 * kCell, def->rowspan, 240, 112);
        AmsStateTestAccess::sync_buffer(ams, test::fps_units({0.71f}, -1.0f), 0);
        CHECK(hidden(h.child("buffer_graphics")));
        CHECK(hidden(h.child("buffer_label")));
        CHECK(text(h.child("buffer_value")) == "Pressure: 71%");
    }

    SECTION("the reading goes: the gate closes") {
        AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
        CHECK(lv_subject_get_int(gate) == 0);
    }

    AmsStateTestAccess::sync_buffer(ams, AmsSystemInfo{}, 0);
    AmsStateTestAccess::clear_buffer_traces(ams);
}
```

Add the expectations the registry sweeps require. `tests/unit/test_grid_layout.cpp`, in `expected` after `clog_detection`:

```cpp
        {"filament_buffer", {false, false}}, // slider tile; the trace needs a whole second cell
```

`tests/unit/test_registry_span_bands.cpp`, in `expected` after `clog_detection`:

```cpp
        {"filament_buffer", {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}},
```

`tests/unit/test_gate_subject_lifetime.cpp#owners`: the AMS list becomes `{"ams_slot_count", "ams_supports_bypass", "clog_meter_mode", "buffer_present"}`.

- [ ] **Step 2: Run it.** `make t F='[filament_buffer]'`. Expected FAIL: does not compile, `filament_buffer_widget.h` not found.

- [ ] **Step 3: Minimal implementation.**

`include/ams_state.h`: getter `lv_subject_t* get_buffer_lean_text_subject() { return &buffer_lean_text_; }` beside the other buffer getters; members `lv_subject_t buffer_lean_text_{}; // "Running tight" / "Running loose" / "Running balanced"` and `char buffer_lean_text_buf_[48]{};`. `#AmsState::publish_buffer_reading`: add `copy_string_if_changed(&buffer_lean_text_, buffer_lean_text(r));`. `ams_state_subjects.cpp`: `INIT_SUBJECT_STRING(buffer_lean_text, "", subjects_, register_xml);` after `buffer_value_text`, and `helix::xml::register_subject_in_current_scope("buffer_lean_text", &buffer_lean_text_);` in `register_xml_subject_names()`.

Create `src/ui/panel_widgets/filament_buffer_widget.h`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "panel_widget.h"

#include <memory>

namespace helix {
namespace ui {
class UiBufferSlider;
} // namespace ui

/// Home widget for the filament buffer: where the buffer between the feeder
/// and the extruder sits against its target. 1x1 is the upright slider with its
/// label and number; 2x1 adds the last minute as a trace and the lean in words.
/// Gated on a proportional reading existing (buffer_present).
class FilamentBufferWidget : public PanelWidget {
  public:
    FilamentBufferWidget() = default;
    ~FilamentBufferWidget() override;

    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    const char* id() const override {
        return "filament_buffer";
    }

  private:
    std::unique_ptr<ui::UiBufferSlider> slider_;
};

} // namespace helix
```

Create `src/ui/panel_widgets/filament_buffer_widget.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "filament_buffer_widget.h"

#include "ui_buffer_slider.h"

#include "ams_state.h"
#include "buffer_status_modal.h"
#include "grid_layout.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "panel_widget_registry.h"
#include "static_subject_registry.h"
#include "subject_managed_panel.h"

namespace {

// 1 when the tile is two cells wide, which is what draws the trace and the
// lean in words. Registered before the XML that binds it is parsed.
lv_subject_t s_wide_subject{};
bool s_subjects_initialized = false;
SubjectManager s_subjects;

void filament_buffer_widget_init_subjects() {
    if (s_subjects_initialized)
        return;

    lv_subject_init_int(&s_wide_subject, 0);
    s_subjects.publish("filament_buffer_wide", &s_wide_subject);
    s_subjects_initialized = true;

    StaticSubjectRegistry::instance().register_deinit("FilamentBufferWidgetSubjects", []() {
        if (s_subjects_initialized && lv_is_initialized()) {
            s_subjects.deinit_all();
            s_subjects_initialized = false;
        }
    });
}

} // namespace

namespace helix {

void register_filament_buffer_widget() {
    register_widget_factory("filament_buffer", [](const std::string&) {
        return std::make_unique<FilamentBufferWidget>();
    });
    register_widget_subjects("filament_buffer", filament_buffer_widget_init_subjects);

    lv_xml_register_event_cb(nullptr, "on_filament_buffer_widget_clicked", [](lv_event_t* /*e*/) {
        if (!AmsState::instance().get_backend())
            return;
        BufferStatusModal::show_for(0);
    });
}

FilamentBufferWidget::~FilamentBufferWidget() {
    detach();
}

void FilamentBufferWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    if (!widget_obj)
        return;
    slider_ = std::make_unique<ui::UiBufferSlider>(lv_obj_find_by_name(widget_obj, "buffer_slider_box"),
                                                   lv_obj_find_by_name(widget_obj, "buffer_trace"),
                                                   -1);
    slider_->follow_system_reading();
}

void FilamentBufferWidget::detach() {
    slider_.reset();
}

void FilamentBufferWidget::on_size_changed(int colspan, int /*rowspan*/, int /*width_px*/,
                                           int /*height_px*/) {
    lv_subject_set_int(&s_wide_subject, colspan >= 2 * GridLayout::TRACKS_PER_CELL ? 1 : 0);
}

} // namespace helix
```

Create `ui_xml/components/panel_widget_filament_buffer.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <!--
    Filament Buffer home widget: where the buffer between the feeder and the
    extruder sits against its target.

    1x1: the upright slider (UiBufferSlider draws into buffer_slider_box), the
    source label and the number. 2x1 (filament_buffer_wide): the last minute
    scrolls out of the slider into buffer_trace, and the lean is said in words.
    A reading with no set point (buffer_slider 0) is the number alone.
  -->
  <view name="panel_widget_filament_buffer"
        extends="lv_obj" width="100%" height="100%" style_pad_all="#space_sm" style_pad_gap="#space_xs"
        flex_flow="column" style_flex_main_place="center" style_flex_cross_place="center" scrollable="false">
    <event_cb trigger="clicked" callback="on_filament_buffer_widget_clicked"/>
    <lv_obj name="buffer_graphics"
            width="100%" height="1" flex_grow="1" style_pad_all="0" style_pad_gap="0" flex_flow="row"
            style_flex_main_place="center" scrollable="false" clickable="false" event_bubble="true">
      <bind_flag_if_eq subject="buffer_slider" flag="hidden" ref_value="0"/>
      <lv_obj name="buffer_slider_box"
              width="#buffer_slider_w" height="100%" style_pad_all="0" scrollable="false" clickable="false"
              event_bubble="true"/>
      <lv_obj name="buffer_trace"
              width="1" flex_grow="1" height="100%" style_pad_all="0" scrollable="false" clickable="false"
              event_bubble="true">
        <bind_flag_if_eq subject="filament_buffer_wide" flag="hidden" ref_value="0"/>
      </lv_obj>
    </lv_obj>
    <lv_obj name="buffer_text"
            width="100%" height="content" style_pad_all="0" style_pad_gap="#space_xs" flex_flow="row"
            style_flex_main_place="center" style_flex_cross_place="center" scrollable="false" clickable="false"
            event_bubble="true">
      <text_small name="buffer_label"
                  bind_text="buffer_label" style_text_color="#text_muted" clickable="false" event_bubble="true">
        <bind_flag_if_eq subject="buffer_slider" flag="hidden" ref_value="0"/>
      </text_small>
      <text_small name="buffer_value" bind_text="buffer_value_text" clickable="false" event_bubble="true"/>
      <text_small name="buffer_lean"
                  bind_text="buffer_lean_text" style_text_color="#text_muted" clickable="false"
                  event_bubble="true">
        <bind_flag_if cond="filament_buffer_wide eq 0 or buffer_slider eq 0" flag="hidden"/>
      </text_small>
    </lv_obj>
  </view>
</component>
```

`src/xml_registration.cpp`: `register_xml("components/panel_widget_filament_buffer.xml");` after `components/panel_widget_clog_detection.xml`.

`src/ui/panel_widget_registry.cpp`: declare `void register_filament_buffer_widget();` after `register_clog_detection_widget();`, call it after `register_clog_detection_widget();` in `init_widget_registrations()`, and add after the `clog_detection` row:

```cpp
    {"filament_buffer",  TR_NOOP("Filament Buffer"),   "arrow_up_down",    TR_NOOP("Filament buffer position against its target"), "buffer_present", "Requires a filament pressure sensor or sync feedback", CAT_FILAMENT, false, 2, 2, 2, 2, 4, 2, false, false, false},
```

`src/ui/panel_widget_manager.cpp#gate_subject_lifetime`: the AmsState condition gains `|| std::strcmp(name, "buffer_present") == 0`.

`app_srcs.txt`: add `src/ui/panel_widgets/filament_buffer_widget.cpp` after `src/ui/panel_widgets/fan_widget.cpp`. `python3 scripts/check_esp32_app_srcs.py`: PASS.

Translations: `make translation-sync`; confirm the three registry strings are in `translations/en.yml` (add by hand if `TR_NOOP` is not extracted). Fill:

| Key | de | es | fr | it | ja | pt | ru | zh |
|---|---|---|---|---|---|---|---|---|
| `Filament Buffer` | `Filamentpuffer` | `Búfer de filamento` | `Tampon de filament` | `Buffer filamento` | `フィラメントバッファー` | `Buffer de filamento` | `Буфер филамента` | `耗材缓冲区` |
| `Filament buffer position against its target` | `Position des Filamentpuffers zum Ziel` | `Posición del búfer de filamento respecto al objetivo` | `Position du tampon de filament par rapport à la cible` | `Posizione del buffer filamento rispetto al target` | `目標に対するフィラメントバッファーの位置` | `Posição do buffer de filamento em relação ao alvo` | `Положение буфера филамента относительно цели` | `耗材缓冲区相对目标的位置` |
| `Requires a filament pressure sensor or sync feedback` | `Erfordert einen Filament-Drucksensor oder Sync-Feedback` | `Requiere un sensor de presión de filamento o retroalimentación de sincronización` | `Nécessite un capteur de pression de filament ou un retour de synchronisation` | `Richiede un sensore di pressione del filamento o il feedback di sincronizzazione` | `フィラメント圧力センサーまたは同期フィードバックが必要です` | `Requer um sensor de pressão de filamento ou feedback de sincronização` | `Требуется датчик давления филамента или обратная связь синхронизации` | `需要耗材压力传感器或同步反馈` |

Then `make translations`.

- [ ] **Step 4: Run it.** `make t F='[filament_buffer]'`, `make t F='[widget_def]'`, `make t F='[panel_widget][manager][observer]'`, `make t F='[content_fits][sweep]'`. Expected PASS. If the content-fits sweep reports `filament_buffer` clipping at its minimum, fix the XML (shorter gap, `long_mode="dots"` on `buffer_value`) rather than adding a baseline entry.

- [ ] **Step 5: Commit.**

```bash
git add -N src/ui/panel_widgets/filament_buffer_widget.h src/ui/panel_widgets/filament_buffer_widget.cpp \
  ui_xml/components/panel_widget_filament_buffer.xml tests/unit/test_filament_buffer_widget.cpp
git commit -m "feat(home): Filament Buffer widget" \
  -m "A 1x1 home widget with the upright buffer slider, its label and number, gated on a proportional reading; at 2x1 the last minute scrolls out of the slider and the lean is said in words. A reading with no set point shows the number alone. Tap opens Buffer Status. Red first: the widget cases did not compile, and the registry sweeps failed on an unclassified id." \
  -- src/ui/panel_widgets/filament_buffer_widget.h src/ui/panel_widgets/filament_buffer_widget.cpp \
     ui_xml/components/panel_widget_filament_buffer.xml src/ui/panel_widget_registry.cpp \
     src/ui/panel_widget_manager.cpp src/xml_registration.cpp include/ams_state.h \
     src/printer/ams_state_buffer.cpp src/printer/ams_state_subjects.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt translations ui_xml/translations \
     tests/unit/test_filament_buffer_widget.cpp tests/unit/test_grid_layout.cpp \
     tests/unit/test_registry_span_bands.cpp tests/unit/test_gate_subject_lifetime.cpp
git show --stat HEAD
```

---

### Task 8: Clog Detection widget is the bar alone

The carousel's buffer page goes; the widget is the FlowGuard bar and nothing else.

**Files:**
- Modify: `src/ui/panel_widgets/clog_detection_widget.h`, `src/ui/panel_widgets/clog_detection_widget.cpp`
- Modify: `ui_xml/components/panel_widget_clog_detection.xml`
- Delete: `ui_xml/components/clog_bar_page.xml`
- Modify: `src/xml_registration.cpp` (drop `components/clog_bar_page.xml`), `ui_xml/components/clog_bar_body.xml` (comment naming the page, if any)
- Modify: `tests/unit/test_widget_size_clog_detection.cpp`, `tests/unit/test_grid_layout.cpp` (the `clog_detection` comment)

**Interfaces:**
- Consumes: `UiClogBar(lv_obj_t* parent)` (finds `clog_bar` under its parent).
- Produces: `ClogDetectionWidget` without `carousel_`, `clog_page_`, `buffer_page_`, `buffer_meter_`, `has_buffer_page_`, `build_carousel_pages()`, `on_activate()`.

- [ ] **Step 1: Write the failing test.** In `tests/unit/test_widget_size_clog_detection.cpp` drop `#include "ui_buffer_meter.h"`, update the file comment's `clog_bar_page.xml` mentions to `clog_bar_body.xml`, and add:

```cpp
TEST_CASE_METHOD(LVGLUITestFixture, "clog_detection is the bar alone",
                 "[widget_size][clog_detection]") {
    PanelWidgetManager::instance().init_widget_subjects();
    AmsState::instance().init_subjects(true);

    PanelWidgetHarness<ClogDetectionWidget> h(test_screen());
    REQUIRE(h.root() != nullptr);
    CHECK(h.child("filament_health_carousel") == nullptr);
    CHECK(h.child("clog_bar_track") != nullptr);
}
```

- [ ] **Step 2: Run it.** `make t F='[clog_detection]'`. Expected FAIL: `h.child("filament_health_carousel")` is not null.

- [ ] **Step 3: Minimal implementation.** `ui_xml/components/panel_widget_clog_detection.xml`:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<component>
  <!--
    Clog Detection home widget: the FlowGuard bar (clog_bar_body), driven by
    UiClogBar from the clog_meter_* subjects. Clog detectors only; the filament
    buffer has its own widget.
  -->
  <view name="panel_widget_clog_detection"
        extends="lv_obj" width="100%" height="100%" flex_grow="1" style_pad_all="#space_sm" style_pad_row="#space_xs"
        style_pad_column="0" flex_flow="column" style_flex_main_place="center" style_flex_cross_place="center"
        scrollable="false">
    <event_cb trigger="clicked" callback="on_clog_detection_widget_clicked"/>
    <clog_bar_body/>
  </view>
</component>
```

`clog_detection_widget.h`: class comment becomes `/// Panel widget for clog detection on the home panel: the FlowGuard bar.\n///\n/// A bar rather than an arc (#1017): the widget is authored wide and short,\n/// the shape a horizontal scale wants, and both ends can carry a label, so a\n/// Flowguard reading says which fault it is leaning toward. UiClogMeter's arc\n/// is what the AMS sidebar and loaded card use.`; drop `class UiBufferMeter;`, `void on_activate() override;`, `void build_carousel_pages();`, `carousel_`, `clog_page_`, `buffer_page_`, `buffer_meter_`, `has_buffer_page_`.

`clog_detection_widget.cpp`: drop the `ui_buffer_meter.h`, `ui_carousel.h`, `ams_types.h`, `theme_manager.h` includes, `build_carousel_pages()` and `on_activate()`, and set:

```cpp
void ClogDetectionWidget::attach(lv_obj_t* widget_obj, lv_obj_t* /*parent_screen*/) {
    widget_obj_ = widget_obj;
    if (!widget_obj_)
        return;

    clog_bar_ = std::make_unique<ui::UiClogBar>(widget_obj_);
    apply_config();
}

void ClogDetectionWidget::detach() {
    {
        auto freeze = helix::ui::UpdateQueue::instance().scoped_freeze();
        helix::ui::UpdateQueue::instance().drain();
        clog_bar_.reset();
    }
    widget_obj_ = nullptr;
}

void ClogDetectionWidget::on_size_changed(int /*colspan*/, int /*rowspan*/, int /*width_px*/,
                                          int /*height_px*/) {
    if (clog_bar_)
        clog_bar_->relayout();
}
```

`git rm -q ui_xml/components/clog_bar_page.xml`; drop its `register_xml` line; `grep -rn clog_bar_page ui_xml src include tests docs/devel/FILAMENT_MANAGEMENT.md` and point each remaining mention at `clog_bar_body.xml`. `tests/unit/test_grid_layout.cpp`: the `clog_detection` comment becomes `// horizontal bar scales with the box`.

- [ ] **Step 4: Run it.** `make t F='[clog_detection]'`, `make t F='[widget_def]'`, `make t F='[content_fits][sweep]'`. Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git rm -q ui_xml/components/clog_bar_page.xml
git commit -m "refactor(home): the Clog Detection widget is the FlowGuard bar alone" \
  -m "Its carousel and buffer page are gone; the filament buffer has its own widget. The bar sits in the widget directly. Red first: [clog_detection] found the carousel before the change." \
  -- src/ui/panel_widgets/clog_detection_widget.h src/ui/panel_widgets/clog_detection_widget.cpp \
     ui_xml/components/panel_widget_clog_detection.xml ui_xml/components/clog_bar_page.xml \
     ui_xml/components/clog_bar_body.xml src/xml_registration.cpp \
     tests/unit/test_widget_size_clog_detection.cpp tests/unit/test_grid_layout.cpp
git show --stat HEAD
```

---

### Task 9: Path-canvas buffer box reads the shared rule

The AMS panel's canvas is the whole-backend view (`ALL_UNITS`), so its box shows the system-level reading; the overview's per-unit detail canvas is hub-only and draws no buffer box (`ui_filament_path_canvas_set_hub_only` doc).

**Files:**
- Modify: `include/ui_ams_detail.h` (add `BufferBoxState`, `ams_detail_buffer_box`), `src/ui/ui_ams_detail.cpp#ams_detail_setup_path_canvas`
- Modify: `src/ui/ui_filament_path_internal.h#ThemeCache` (add `color_buffer`), `src/ui/ui_filament_path_canvas.cpp#load_theme_colors`, `src/ui/ui_filament_path_glyphs.cpp#draw_buffer_coil`, `include/ui_filament_path_canvas.h#ui_filament_path_canvas_set_buffer_bias` (doc)
- Create: `tests/unit/test_ams_detail_buffer_box.cpp`

**Interfaces:**
- Consumes: `buffer_reading()`, `buffer_status_token()`.
- Produces: `struct helix::ui::BufferBoxState { bool present; int state; const char* label; int fault; float bias; };`, `helix::ui::BufferBoxState helix::ui::ams_detail_buffer_box(const AmsSystemInfo& info, int unit_index);`

- [ ] **Step 1: Write the failing test.** Create `tests/unit/test_ams_detail_buffer_box.cpp`:

```cpp
// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_ams_detail_buffer_box.cpp
 * @brief What the path canvas's buffer box draws: which reading, its label,
 *        and the severity its tint comes from.
 */

#include "ui_ams_detail.h"

#include "../test_helpers/buffer_infos.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;
using helix::ui::ams_detail_buffer_box;

TEST_CASE("buffer box: the whole-backend view reads the lane feeding the toolhead",
          "[ams][buffer][path]") {
    AmsSystemInfo info = test::fps_units({0.9f, 0.3f}, 0.5f, /*current_slot=*/5);
    const auto box = ams_detail_buffer_box(info, -1);
    CHECK(box.present);
    CHECK(std::string(box.label) == "FPS");
    CHECK(box.bias == Catch::Approx(-0.4f));
    CHECK(box.fault == 1);

    SECTION("a unit's own view reads its own lane") {
        const auto own = ams_detail_buffer_box(info, 0);
        CHECK(own.bias == Catch::Approx(0.8f));
        CHECK(own.fault == 2);
    }
}

TEST_CASE("buffer box: on target is not a fault", "[ams][buffer][path]") {
    const auto box = ams_detail_buffer_box(test::fps_units({0.5f}), -1);
    CHECK(box.fault == 0);
    CHECK(box.bias == Catch::Approx(0.0f));
}

TEST_CASE("buffer box: no set point draws the FPS box with no bias", "[ams][buffer][path]") {
    const auto box = ams_detail_buffer_box(test::fps_units({0.62f}, -1.0f), -1);
    CHECK(box.present);
    CHECK(std::string(box.label) == "FPS");
    CHECK(box.bias == -2.0f);
    CHECK(box.fault == 0);
}

TEST_CASE("buffer box: an AFC fault distance outranks a calm reading", "[ams][buffer][path]") {
    AmsSystemInfo info = test::fps_units({0.5f});
    auto& h = *info.units[0].buffer_health;
    h.fault_detection_enabled = true;
    h.distance_to_fault = 60.0f;
    CHECK(ams_detail_buffer_box(info, -1).fault == 2);
}

TEST_CASE("buffer box: Happy Hare sync feedback", "[ams][buffer][path]") {
    AmsSystemInfo info;
    info.type = AmsType::HAPPY_HARE;
    info.sync_feedback_state = "tension";
    info.sync_feedback_bias = -0.8f;
    const auto box = ams_detail_buffer_box(info, -1);
    CHECK(box.present);
    CHECK(box.state == 2);
    CHECK(std::string(box.label) == "BUF");
    CHECK(box.fault == 2);
}
```

- [ ] **Step 2: Run it.** `make t F='[ams][buffer][path]'`. Expected FAIL: does not compile, `ams_detail_buffer_box` undeclared.

- [ ] **Step 3: Minimal implementation.** `include/ui_ams_detail.h`, inside `namespace helix::ui`:

```cpp
/// What the path canvas's buffer box draws for one view.
struct BufferBoxState {
    bool present = false;
    int state = 0;             ///< Coil shape: 0 neutral, 1 compressed, 2 tension
    const char* label = "BUF"; ///< "FPS" for a filament pressure sensor
    int fault = 0;             ///< ClogMeterStatus: the AFC fault distance or the buffer bands, worse wins
    float bias = -2.0f;        ///< The reading's bias, or -2 with no proportional reading
};

/// The buffer box for @p unit_index; -1 is the whole-backend view, which shows
/// the system-level reading (buffer_reading(info, -1)).
BufferBoxState ams_detail_buffer_box(const AmsSystemInfo& info, int unit_index);
```

(add `#include "ams_types.h"` if the header does not already reach it). `src/ui/ui_ams_detail.cpp`: add `#include "buffer_reading.h"` and, in `namespace helix::ui`:

```cpp
BufferBoxState ams_detail_buffer_box(const AmsSystemInfo& info, int unit_index) {
    BufferBoxState box;
    // The AFC buffer rows describe one unit; the whole-backend view uses unit 0.
    const AmsUnit* unit = info.get_unit(unit_index >= 0 ? unit_index : 0);
    if (unit && unit->buffer_health) {
        const BufferHealth& h = *unit->buffer_health;
        box.present = true;
        if (h.state == "Advancing") {
            box.state = 1;
        } else if (h.state == "Trailing") {
            box.state = 2;
        }
        if (h.fault_detection_enabled && h.distance_to_fault >= 0.0f) {
            if (h.distance_to_fault >= 50.0f) {
                box.fault = 2; // at or past the fault threshold
            } else if (h.distance_to_fault > 0.0f) {
                box.fault = 1; // approaching it
            }
        }
    }
    if (!box.present && info.type == AmsType::HAPPY_HARE) {
        const auto& sf = info.sync_feedback_state;
        if (!sf.empty() && sf != "disabled") {
            box.present = true;
            if (sf == "compressed") {
                box.state = 1;
            } else if (sf == "tension") {
                box.state = 2;
            }
        }
    }

    const BufferReading reading = buffer_reading(info, unit_index);
    if (reading.source == BufferSource::Fps) {
        box.present = true;
        box.label = "FPS"; // i18n: do not translate - hardware abbreviation
    }
    if (reading.has_slider) {
        box.bias = reading.bias;
        box.fault = std::max(box.fault, static_cast<int>(reading.status));
    }
    return box;
}
```

In `ams_detail_setup_path_canvas`, replace everything from `// Set buffer fault state on hub (AFC TurtleNeck buffer health)` through `ui_filament_path_canvas_set_buffer_bias(canvas, buffer_bias);` with:

```cpp
    // The buffer box: AFC buffer health, Happy Hare sync feedback, or a
    // filament pressure sensor, tinted by the buffer bands.
    const helix::ui::BufferBoxState box = helix::ui::ams_detail_buffer_box(info, unit_index);
    ui_filament_path_canvas_set_buffer_fault_state(canvas, box.fault);
    ui_filament_path_canvas_set_buffer_info(canvas, box.present, box.state, box.label);
    ui_filament_path_canvas_set_buffer_bias(canvas, box.bias);
```

and drop the `clog_meter_geometry.h` include if nothing else uses it.

`src/ui/ui_filament_path_internal.h#ThemeCache`: add `lv_color_t color_buffer[3]; // Buffer box by ClogMeterStatus: text_muted, warning, danger`. `src/ui/ui_filament_path_canvas.cpp#load_theme_colors` (add `#include "clog_meter_geometry.h"`), after `color_success`:

```cpp
    for (int s = 0; s < 3; ++s) {
        theme.color_buffer[s] = theme_manager_get_color(
            helix::ui::buffer_status_token(static_cast<helix::ui::ClogMeterStatus>(s)));
    }
```

`src/ui/ui_filament_path_glyphs.cpp#draw_buffer_coil`: the comment above it becomes `// Draw buffer box element: a labeled box like HUB/SELECTOR, its border in the\n// buffer bands' token (neutral on target, warning, danger)`; the `buffer_fault_state >= 2` branch sets `border_color = theme.color_buffer[2];`; the whole `else if (buffer_bias > -1.5f) {...}` branch becomes:

```cpp
    } else if (buffer_bias > -1.5f) {
        border_color = theme.color_buffer[std::clamp(buffer_fault_state, 0, 2)];
        if (has_filament) {
            buf_bg = ph_blend(bg_color, filament_color, 0.33f);
        }
```

`include/ui_filament_path_canvas.h#ui_filament_path_canvas_set_buffer_bias` doc: `When set to a valid value (> -1.5), the buffer box border takes the buffer bands' token for its fault state: text_muted on target, warning, danger. When unavailable (-2.0), falls back to discrete 3-state color logic.`

- [ ] **Step 4: Run it.** `make t F='[ams][buffer][path]'`, `make t F='[canvas]'`, `make t F='[ui_integration][ams]'`. Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git add -N tests/unit/test_ams_detail_buffer_box.cpp
git commit -m "feat(ams): the path canvas buffer box follows the buffer reading" \
  -m "ams_detail_buffer_box() decides the box once: AFC health, Happy Hare sync state, and buffer_reading() for the label, bias and severity, the worse of AFC distance and the bands winning. The whole-backend view shows the lane feeding the toolhead. Its border takes text_muted / warning / danger, so on target reads neutral. Red first: the cases did not compile without the function." \
  -- include/ui_ams_detail.h src/ui/ui_ams_detail.cpp src/ui/ui_filament_path_internal.h \
     src/ui/ui_filament_path_canvas.cpp src/ui/ui_filament_path_glyphs.cpp \
     include/ui_filament_path_canvas.h tests/unit/test_ams_detail_buffer_box.cpp
git show --stat HEAD
```

---

### Task 10: Buffer Status modal rebuilt; `UiBufferMeter` deleted

**Files:**
- Modify: `include/buffer_status_modal.h`, `src/ui/modals/buffer_status_modal.cpp`, `ui_xml/components/buffer_status_modal.xml`
- Modify: `src/ui/ui_panel_ams.cpp#AmsPanel::handle_buffer_click`, `src/ui/panel_widgets/clog_detection_widget.cpp#register_clog_detection_widget`, `src/ui/panel_widgets/filament_buffer_widget.cpp#register_filament_buffer_widget` (`show_for(-1)`)
- Delete: `include/ui_buffer_meter.h`, `src/ui/ui_buffer_meter.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (drop `src/ui/ui_buffer_meter.cpp`), `src/ui/ui_panel_print_status.cpp` (comment citing it)
- Modify: `include/ams_types.h#AmsSystemInfo::buffer_bias` (delete), `include/ams_backend.h#AmsBackend::supports_sync_feedback_visualization` (delete), `tests/unit/test_ams_backend_openams.cpp`, `tests/unit/test_afc_fps_buffer.cpp`
- Modify: `tests/unit/test_buffer_status_modal.cpp`, `tests/unit/test_buffer_status_modal_live.cpp`
- Modify: `translations/*.yml`, `ui_xml/translations/*.xml` (drop `Pressure: {}% (target {}%)` if obsolete)

**Interfaces:**
- Consumes: `buffer_reading()`, `buffer_label()`, `buffer_value_text()`, `buffer_target_text()`, `UiBufferSlider`.
- Produces: `static void BufferStatusModal::show_for(int effective_unit);` (-1 = the reading feeding the toolhead), `helix::BufferReading BufferStatusModal::populate(const helix::AmsSystemInfo&, int);`, XML subjects `buf_value`, `buf_target`, `buf_show_reading` (`buf_pressure` gone), member `std::unique_ptr<helix::ui::UiBufferSlider> slider_`.

- [ ] **Step 1: Write the failing test.** `tests/unit/test_buffer_status_modal.cpp`: add `#include "ui_buffer_slider.h"`, `#include "../lvgl_ui_test_fixture.h"`, `#include "../test_helpers/buffer_infos.h"`, `#include "../test_helpers/registered_backend.h"`, `#include "ams_backend_mock.h"`, `#include "ams_state.h"`, `#include "ui_update_queue.h"`. In `TestableBufferStatusModal` replace `pressure_value()` with:

```cpp
    const char* value_value() {
        return lv_subject_get_string(&value_subject_);
    }
    const char* target_value() {
        return lv_subject_get_string(&target_subject_);
    }
    int show_reading_value() {
        return lv_subject_get_int(&show_reading_subject_);
    }
    const helix::ui::UiBufferSlider* slider() const {
        return slider_.get();
    }
```

In `BufferStatusModal shows a pressure sensor's reading`: the set-point section's last check becomes `CHECK(std::string(modal.value_value()) == "FPS 62%");` plus `CHECK(std::string(modal.target_value()) == "target 50%");`; the no-set-point section's becomes `CHECK(std::string(modal.value_value()) == "Pressure: 62%");` plus `CHECK(std::string(modal.target_value()).empty());`. In `BufferStatusModal populate HH with bias`, section `positive bias shows loose description`, add `REQUIRE(std::string(modal.value_value()) == "Sync +15%");`. Then add:

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "BufferStatusModal with no unit reads the lane feeding the toolhead",
                 "[modals][buffer_status]") {
    TestableBufferStatusModal modal;

    SECTION("OpenAMS, two lanes") {
        helix::AmsSystemInfo info = helix::test::fps_units({0.9f, 0.3f}, 0.5f, 5);
        info.type = helix::AmsType::OPENAMS;
        modal.populate(info, -1);
        CHECK(modal.type_value() == 3);
        CHECK(std::string(modal.value_value()) == "FPS 30%");
        CHECK(std::string(modal.description_value()) == "Filament is pulling tight");
    }

    SECTION("AFC switched buffer: its rows still read") {
        auto info = make_afc_info();
        modal.populate(info, -1);
        CHECK(modal.type_value() == 2);
        CHECK(std::string(modal.afc_state_value()) == "Feeding filament forward");
        CHECK(modal.show_reading_value() == 0);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "BufferStatusModal trace keeps scrolling while open",
                 "[modals][buffer_status][live]") {
    helix::AmsState::instance().init_subjects(true);
    helix::test::RegisteredBackend<helix::AmsBackendMock> mock(4);
    mock->set_tool_changer_mode(true);
    helix::BufferHealth fps;
    fps.fps_value = fps.smoothed_fps = 0.32f;
    fps.fps_set_point = 0.5f;
    fps.fps_reported = true;
    mock->set_unit_buffer_health(0, fps);
    helix::AmsState::instance().sync_from_backend();

    TestableBufferStatusModal modal;
    REQUIRE(modal.show(test_screen()));
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(modal.slider() != nullptr);
    CHECK(modal.slider()->bias() == Catch::Approx(-0.36f));

    const int before = modal.slider()->trace_ticks();
    process_lvgl(2100);
    CHECK(modal.slider()->trace_ticks() >= before + 2);

    modal.hide();
    helix::ui::UpdateQueue::instance().drain();
}
```

`tests/unit/test_buffer_status_modal_live.cpp`: replace both `buf_pressure` checks with `subject_text("buf_value") == "FPS 32%"` / `"FPS 71%"` and add `CHECK(subject_text("buf_target") == "target 50%");` after the first.

- [ ] **Step 2: Run it.** `make t F='[buffer_status]'`. Expected FAIL: does not compile (`value_subject_`, `slider_` undeclared); with stubs it would fail on `afc_state_value() == "No buffer data available"` for unit -1.

- [ ] **Step 3: Minimal implementation.**

`include/buffer_status_modal.h`: replace the includes/forward declarations so the header has `#include "ui_buffer_slider.h"`, `#include "buffer_reading.h"`, `#include <memory>`, and only `class UiClogBar;` forward-declared; replace the class comment's first paragraph with `Read-only modal for one filament buffer: an upright slider with its last minute scrolling out to the right, the reading and target, the lean in words, and the backend's own rows (Happy Hare spool motor, gear sync, flow; AFC state and distance to fault). Closed by its X; nothing to confirm.`; and change:

```cpp
    /// Convenience: create the modal for one unit's buffer and show it; -1 is
    /// the buffer feeding the toolhead. One-shot and stack-owned - ModalStack
    /// frees the instance when its entry goes (#1382).
    static void show_for(int effective_unit);
```

```cpp
    /// Write the modal's subjects for one unit's buffer, and return the
    /// reading the slider draws.
    helix::BufferReading populate(const helix::AmsSystemInfo& info, int effective_unit);
    /// populate() from the active backend's current snapshot, and put the
    /// slider on the same reading.
    void refresh();

    static bool subjects_initialized_;
    std::unique_ptr<helix::ui::UiBufferSlider> slider_;
```

Remove `meter_`, `pressure_subject_`, `pressure_buf_`; add:

```cpp
    static lv_subject_t show_reading_subject_;
    static lv_subject_t value_subject_;
    static char value_buf_[64];
    static lv_subject_t target_subject_;
    static char target_buf_[48];
```

`src/ui/modals/buffer_status_modal.cpp`: drop `#include "ui_buffer_meter.h"`, add `#include "buffer_reading.h"` and `#include <algorithm>`; replace the `pressure_subject_`/`pressure_buf_` definitions with definitions of `show_reading_subject_`, `value_subject_`, `value_buf_[64]{}`, `target_subject_`, `target_buf_[48]{}`; in `init_subjects()` replace the pressure init/register with:

```cpp
    lv_subject_init_int(&show_reading_subject_, 0);
    lv_subject_init_string(&value_subject_, value_buf_, nullptr, sizeof(value_buf_), "");
    lv_subject_init_string(&target_subject_, target_buf_, nullptr, sizeof(target_buf_), "");
```

```cpp
    lv_xml_register_subject(nullptr, "buf_show_reading", &show_reading_subject_);
    lv_xml_register_subject(nullptr, "buf_value", &value_subject_);
    lv_xml_register_subject(nullptr, "buf_target", &target_subject_);
```

Delete `helix::pressure_sensor()`. The destructor body becomes `slider_.reset();\n    delete clog_bar_;` with the comment `// Both go before Modal::~Modal() destroys the dialog tree, so each can\n    // remove its callbacks from widgets that still exist.` Replace `populate`, `on_show`, `refresh`:

```cpp
helix::BufferReading BufferStatusModal::populate(const helix::AmsSystemInfo& info,
                                                 int effective_unit) {
    // Cleared up front: the modal's subjects are static, so a message left from
    // a previous open would otherwise sit under a supported backend's body.
    lv_subject_copy_string(&unsupported_subject_, "");

    // The slider and its words follow this unit's buffer, or with -1 the one
    // feeding the toolhead.
    const helix::BufferReading r = helix::buffer_reading(info, effective_unit);
    lv_subject_set_int(&show_meter_subject_, r.has_slider ? 1 : 0);
    lv_subject_set_int(&show_reading_subject_, r.present() ? 1 : 0);
    lv_subject_copy_string(&description_subject_,
                           r.has_slider ? helix::bias_description(r.bias) : "");
    const std::string value =
        r.has_slider
            ? fmt::format("{} {}", helix::buffer_label(r), helix::buffer_value_text(r))
            : helix::buffer_value_text(r);
    lv_subject_copy_string(&value_subject_, value.c_str());
    lv_subject_copy_string(&target_subject_, helix::buffer_target_text(r).c_str());

    if (info.type == helix::AmsType::HAPPY_HARE) {
        lv_subject_set_int(&type_subject_, 1);
        // (eSpooler, Gear sync and Flow rate blocks unchanged; the trailing
        // "Meter visibility" line is removed: show_meter is set above)
    } else if (info.type == helix::AmsType::AFC) {
        lv_subject_set_int(&type_subject_, 2);
        // AFC's rows describe one unit's buffer: the one asked for, else the
        // one the reading came from.
        const int unit = effective_unit >= 0 ? effective_unit : std::max(r.unit, 0);
        // (the existing AFC block, with `effective_unit` replaced by `unit`)
    } else if (r.source == helix::BufferSource::Fps) {
        // A pressure sensor outside AFC (OpenAMS): the reading is all there is.
        lv_subject_set_int(&type_subject_, 3);
    } else {
        // (the existing unsupported branch, minus its show_meter line)
    }
    return r;
}

void BufferStatusModal::on_show() {
    wire_cancel_button("btn_close");

    if (dialog()) {
        slider_ = std::make_unique<helix::ui::UiBufferSlider>(
            lv_obj_find_by_name(dialog(), "buf_slider"), lv_obj_find_by_name(dialog(), "buf_trace"),
            effective_unit_);
    }
    refresh();

    // (label colour block, the two revision/backend observers and the clog bar
    // creation stay exactly as they are)
}

void BufferStatusModal::refresh() {
    // A vanished backend reads as an empty snapshot, which is the unsupported
    // message rather than whatever the last backend said.
    auto* backend = helix::AmsState::instance().get_backend();
    const auto info = backend ? backend->get_system_info() : helix::AmsSystemInfo{};
    const helix::BufferReading r = populate(info, effective_unit_);
    if (slider_) {
        slider_->set_reading(r.bias, r.status);
    }
}
```

The bodies marked "unchanged" are kept verbatim from the current file: copy them, do not retype them. In the HH section the existing `// Meter visibility` + `lv_subject_set_int(&show_meter_subject_, has_bias ? 1 : 0);` lines go, as does `lv_subject_set_int(&show_meter_subject_, has_bias ? 1 : 0);` in the AFC branch and `lv_subject_set_int(&show_meter_subject_, 0);` in the unsupported branch.

`ui_xml/components/buffer_status_modal.xml`: keep the header row and the clog-bar block; the header comment becomes `<!-- Header row: icon + title + close button. The close X is the only way out: the modal is read-only. -->`. Replace everything from `<!-- Content area: info column (left) + meter column (right) -->` to the end of the view with:

```xml
    <!-- Content: the slider with its last minute scrolling out to the right,
         then the reading in words and the backend's own rows. -->
    <lv_obj width="100%"
            height="content" style_pad_left="#space_lg" style_pad_right="#space_lg" style_pad_top="#space_sm"
            style_pad_bottom="#space_lg" flex_flow="column" style_pad_gap="#space_sm">

      <!-- Drawn by UiBufferSlider; hidden for a reading with no set point to centre on. -->
      <lv_obj name="buf_graphics"
              width="100%" height="150" flex_flow="row" style_pad_all="0" style_pad_gap="0" scrollable="false">
        <bind_flag_if_eq subject="buf_show_meter" flag="hidden" ref_value="0"/>
        <lv_obj name="buf_slider" width="#buffer_slider_w" height="100%" style_pad_all="0" scrollable="false"/>
        <lv_obj name="buf_trace" width="1" flex_grow="1" height="100%" style_pad_all="0" scrollable="false"/>
      </lv_obj>

      <lv_obj name="info_col" width="100%" height="content" flex_flow="column" style_pad_gap="#space_xs">

        <!-- The reading and its target: "FPS 62%  target 50%", "Sync +15%", "Pressure: 62%" -->
        <lv_obj name="reading_row"
                width="100%" height="content" flex_flow="row" style_pad_gap="#space_sm"
                style_flex_cross_place="center">
          <bind_flag_if_eq subject="buf_show_reading" flag="hidden" ref_value="0"/>
          <text_body name="buf_value_label" bind_text="buf_value"/>
          <text_small name="buf_target_label" bind_text="buf_target" style_text_color="#text_muted"/>
        </lv_obj>

        <!-- The lean in words (balanced / pulling tight / loose) -->
        <text_small name="bias_description" width="100%" bind_text="buf_description" long_mode="wrap">
          <bind_flag_if_eq subject="buf_show_meter" flag="hidden" ref_value="0"/>
        </text_small>

        <!-- (unsupported_section, hh_section and afc_section copied verbatim from the
             current file; pressure_section and meter_col are removed) -->

      </lv_obj>
    </lv_obj>
  </view>
</component>
```

(The `<divider_horizontal/>` and `<modal_button_row .../>` are gone.)

Call sites: `AmsPanel::handle_buffer_click`, the `on_clog_detection_widget_clicked` and `on_filament_buffer_widget_clicked` lambdas all call `BufferStatusModal::show_for(-1);`.

Delete `UiBufferMeter`: `git rm -q include/ui_buffer_meter.h src/ui/ui_buffer_meter.cpp`; drop its `app_srcs.txt` line; `src/ui/ui_panel_print_status.cpp`'s comment `(pattern from\n    // ui_buffer_meter.cpp:52 and ui_ams_mini_status.cpp:540)` becomes `(pattern from\n    // ui_ams_mini_status.cpp)`. `python3 scripts/check_esp32_app_srcs.py`: PASS.

Remove the second copy of the per-unit rule: `grep -rn "buffer_bias(\|supports_sync_feedback_visualization" src include` must show only the definitions. Delete `AmsSystemInfo::buffer_bias()` and `AmsBackend::supports_sync_feedback_visualization()` with its doc block. In `tests/unit/test_ams_backend_openams.cpp` replace `info.buffer_bias(n) == Catch::Approx(x)` with `helix::buffer_reading(info, n).bias == Catch::Approx(x)`, `CHECK(info.buffer_bias(0) <= -1.5f);` with `CHECK_FALSE(helix::buffer_reading(info, 0).has_slider);`, `CHECK(backend.supports_sync_feedback_visualization(info));` with `CHECK(helix::buffer_reading(info, -1).has_slider);` and its `CHECK_FALSE` twin likewise; in `tests/unit/test_afc_fps_buffer.cpp` replace `info.buffer_bias(n) == Catch::Approx(x)` the same way and `info.buffer_bias(1) == kNoData` with `!helix::buffer_reading(info, 1).present()`, and reword the comment at line 47 to name `buffer_reading()`. Add `#include "buffer_reading.h"` to both.

Translations: `make translation-obsolete`; if it lists `Pressure: {}% (target {}%)`, delete that key from the nine YAML files and `make translations`.

- [ ] **Step 4: Run it.** `make t F='[buffer_status]'`, `make t F='[openams]'`, `make t F='[afc]'`, `make t F='[clog_detection]'`, `make t F='[filament_buffer]'`. Expected PASS.

- [ ] **Step 5: Commit.**

```bash
git rm -q include/ui_buffer_meter.h src/ui/ui_buffer_meter.cpp
git commit -m "feat(ams): Buffer Status modal draws the buffer slider and its trace" \
  -m "The modal shows a tall upright slider with the last minute scrolling out of it, the reading and target, the lean in words, and the backend's own rows, live while open and closed by its X alone. Every entry point opens it on the buffer feeding the toolhead. UiBufferMeter, AmsSystemInfo::buffer_bias() and supports_sync_feedback_visualization() are gone; buffer_reading() is the one per-unit rule. Red first: AFC with unit -1 read 'No buffer data available'." \
  -- include/buffer_status_modal.h src/ui/modals/buffer_status_modal.cpp \
     ui_xml/components/buffer_status_modal.xml src/ui/ui_panel_ams.cpp \
     src/ui/panel_widgets/clog_detection_widget.cpp src/ui/panel_widgets/filament_buffer_widget.cpp \
     include/ui_buffer_meter.h src/ui/ui_buffer_meter.cpp \
     firmware/helixscreen-esp32/components/helixapp/app_srcs.txt src/ui/ui_panel_print_status.cpp \
     include/ams_types.h include/ams_backend.h tests/unit/test_ams_backend_openams.cpp \
     tests/unit/test_afc_fps_buffer.cpp tests/unit/test_buffer_status_modal.cpp \
     tests/unit/test_buffer_status_modal_live.cpp translations ui_xml/translations
git show --stat HEAD
```

---

### Task 11: Docs, screenshots, goldens and the full run

**Files:**
- Modify: `docs/devel/FILAMENT_MANAGEMENT.md` (new § "Filament buffer reading" after § "AFC buffers: switched vs FPS_PSF"; the `UiBufferMeter` mention in that section; mock scenario list)
- Modify: `docs/devel/FILAMENT_BACKEND_OPENAMS.md`, `docs/devel/FILAMENT_BACKEND_AFC.md` (FPS_PSF paragraph), `docs/devel/architecture/09-home-widgets.md` (widget table)
- Modify: `docs/user/guide/home-panel.md` (widget table, hardware table, tap table, Clog Detection section), `docs/user/guide/filament.md` (lines on the buffer tint and OpenAMS pressure)
- Modify: `docs/devel/CHANGELOG_1_1_DRAFT.md` (the AFC FPS_PSF and OpenAMS entries)
- Create: `docs/images/user/home-filament-buffer.png` (from the 2x1 screenshot)
- Modify: `tests/ui/goldens/ams.png` (regenerated)

**Interfaces:**
- Consumes: everything above.
- Produces: no code.

- [ ] **Step 1: Developer docs.** Add to `docs/devel/FILAMENT_MANAGEMENT.md` after § "AFC buffers: switched vs FPS_PSF":

```markdown
### Filament buffer reading

A buffer reading is where the slack sits between the feeder and the extruder,
steered toward a set point. It is not clog detection (which accumulates toward a
pause and feeds the `clog_meter_*` subjects) and has its own path.

| Piece | Where |
|---|---|
| FPS to bias | `BufferHealth::fps_to_bias()` (`include/ams_types.h`), used by AFC `FPS_PSF` and OpenAMS |
| Which sensor | `buffer_reading(info, unit)` (`include/buffer_reading.h`): a unit's own pressure sensor; a switched buffer reads nothing; otherwise, and for unit -1, the sensor feeding the toolhead (`AmsSystemInfo::feeding_pressure_unit()`), else Happy Hare's `sync_feedback_bias` |
| Bands and colour | `pressure_status()` and `buffer_status_token()` in `include/clog_meter_geometry.h`: below 30 `text_muted`, to 70 `warning`, from 70 `danger`; `buffer_lean()` for the words |
| Subjects | `AmsState` publishes the system-level reading as `buffer_present`, `buffer_slider`, `buffer_bias_pct`, `buffer_status`, `buffer_label`, `buffer_value_text`, `buffer_lean_text` (`src/printer/ams_state_buffer.cpp`) |
| History | `AmsState::buffer_trace(unit)`, a `BufferTrace` of about 60 s per unit and for -1, a step line on `buffer_clock_ms()`; dropped with the backend |
| Renderer | `UiBufferSlider` (`include/ui_buffer_slider.h`) paints the slider and optional trace into XML-authored objects; layout from `buffer_slider_geometry()` / `buffer_trace_polylines()` |

Surfaces: the **Filament Buffer** home widget (`filament_buffer`, 1x1, 2x1 adds
the trace and lean, gated on `buffer_present`), the loaded-spool card's mini
slider, the path canvas's buffer box (`ams_detail_buffer_box()`), and the Buffer
Status modal (`BufferStatusModal::show_for(-1)` opens it on the buffer feeding
the toolhead). A pressure reading with no set point is text only (`Pressure: N%`)
on every surface. Mock scenarios: `buffer_fps`, `buffer_fps_loose`,
`buffer_fps_no_target` (any mock type but Happy Hare), `sync_feedback_tight`
(Happy Hare).
```

In § "AFC buffers: switched vs FPS_PSF" replace `` `UiBufferMeter` and the path-canvas tint `` with `` the buffer surfaces ``. In the Mock scenarios paragraph of § "Clog / flow meter" remove the `buffer_fps` / `buffer_fps_loose` / `sync_feedback_tight` items (they are listed under the new section). In `FILAMENT_BACKEND_OPENAMS.md` rewrite the bullets after `(\`fps_value\`, \`fps_set_point\`, \`fps_reported\`), so:` to: the path box labelled **FPS** and tinted by the buffer bands; `fps_to_bias()` mapping; `sync_feedback_bias` carrying `pressure_sensor_bias()` for the backend contract; the Filament Buffer widget, loaded card and modal reading `buffer_reading()` (the lane feeding the current slot, else the first unit with a sensor); tapping the box opens the modal with the slider, `FPS 62%`, `target 50%`. In `FILAMENT_BACKEND_AFC.md`'s FPS_PSF paragraph name the same surfaces. In `architecture/09-home-widgets.md` add the row `| \`filament_buffer\` | \`FilamentBufferWidget\` | \`buffer_present\` |` after `clog_detection`.

- [ ] **Step 2: Screenshots.** Build the app (`make`), then for each shot start a pinned mock, drive it, capture, stop it:

```bash
TREE=openams-fps-meter
export HELIX_SOCK=/tmp/helix-$TREE.sock HELIX_CONFIG_DIR=/tmp/helix-config-$TREE
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_MOCK_AMS=afc SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
CTL="./build/bin/helix-screen ctl -s $HELIX_SOCK"
$CTL click btn_done   # the mock's plugin-install modal covers home
# Add the widget: Edit Mode, then the catalog (docs/devel/HELIXCTL.md § Home-grid Edit Mode)
$CTL navigate home; $CTL geom filament; $CTL long_press <x> <y>; $CTL click nav_btn_edit_add; $CTL ls
```

Click the "Filament Buffer" catalog row, leave Edit Mode, then capture. Stop the instance by the PID resolved from the socket: `for p in $(pgrep -x helix-screen); do grep -qz "$HELIX_SOCK" /proc/$p/cmdline && kill $p; done`. Shots (all into `/tmp/fps-final/`), each with what it must show:

| File | Mock / scenario | Must show |
|---|---|---|
| `fps-final-widget-1x1.png` | `afc`, `scenario buffer_fps` | Upright slider, amber block below the dashed window, `FPS`, `32%` |
| `fps-final-widget-2x1.png` | `afc`, resize the tile to 2x1 in Edit Mode; `scenario buffer_fps`, wait 10 s, `scenario buffer_fps_loose`, wait 10 s | Slider plus a trace with one step up, `FPS 71% Running loose`; copy it to `docs/images/user/home-filament-buffer.png` |
| `fps-final-widget-no-target.png` | `afc`, `scenario buffer_fps_no_target` | No slider, `Pressure: 32%` only |
| `fps-final-widget-sync.png` | default (Happy Hare), `scenario sync_feedback_tight` | `Sync`, `-45%`, amber block low |
| `fps-final-loaded-card.png` | default, `navigate ams` (or `demo ams`), `scenario buffer_fps` | Mini slider and `32%` / `FPS` beside the material, clog arc beside it |
| `fps-final-path-box.png` | `afc`, `demo ams`, `scenario buffer_fps_loose` | Box labelled `FPS`, amber border |
| `fps-final-path-box-neutral.png` | default, `demo ams` (bias 0.15) | Box `BUF`, neutral grey border, not green |
| `fps-final-modal.png` | `afc`, the `fps-final-widget-2x1` sequence, then `click filament_buffer` (the tile) | Tall slider with a stepped trace, `FPS 71%`, `target 50%`, `Filament is loose`, one X, no Cancel/OK |
| `fps-final-modal-sync.png` | default, `scenario sync_feedback_tight`, tap the tile | Slider, `Sync -45%`, `Filament is pulling tight`, Spool motor / Gear sync / Flow rows |

Read every PNG and check the "Must show" column; `ctl text buf_value` etc. confirm values exactly.

- [ ] **Step 3: User docs.** `docs/user/guide/home-panel.md`: widget table, after **Clog Detection**:

```markdown
| **Filament Buffer** | Where the filament buffer between your feeder and extruder sits against its target: an upright slider (loose up, tight down) with the reading underneath. At 2x1 it adds the last minute as a trace and says whether the filament is running tight, loose or balanced. Tap for Buffer Status. | 1x1 | 1x1 | 2x1 | Yes | A filament pressure sensor or sync feedback |
```

In the **Clog Detection** row drop `, and a buffer sync meter on Happy Hare printers`. Hardware table: `| Filament Buffer | An OpenAMS or AFC filament pressure sensor (FPS), or Happy Hare with sync feedback |`. Tap table: `| Filament Buffer | Opens the Buffer Status modal |`. § "Clog Detection Widget": first sentence ends at `detecting clogs and flow issues.`; replace the sentence that begins `The widget displays a **carousel**` and the bold `Page 1` heading line under it with `The widget shows the FlowGuard bar:`; delete the whole bold `Page 2` (Buffer Sync Meter) block; in "Tapping the Widget" replace the Happy Hare list's `Full-size buffer meter visualization` line with nothing and add after the AFC list: `Where your printer has a filament buffer with a proportional reading, the modal also draws it as a slider with its last minute beside it; see [Filament Buffer Widget](#filament-buffer-widget).` Add a section before "Configuring Clog Detection"'s parent heading ends, as `## Filament Buffer Widget`, with the image `![Filament Buffer widget at 2x1: slider, trace, FPS 71% Running loose](../../images/user/home-filament-buffer.png)` and four sentences: what the buffer is, loose up / tight down, the colours (grey on target, amber off it, red near an end), and that a sensor with no target shows the pressure as text only. `docs/user/guide/filament.md`: line 106's "green when healthy, yellow ..., red ..." becomes "grey on target, amber as it drifts, red near an end stop"; line 107 gains "The Filament Buffer home widget and the loaded-spool card show the same reading." and its "buffer meter" becomes "buffer slider". `docs/devel/CHANGELOG_1_1_DRAFT.md`: in the AFC FPS_PSF entry replace `the buffer meter, path tint and filament page` with `the buffer surfaces`; extend the OpenAMS entry with `; the new Filament Buffer home widget, the loaded-spool card and the Buffer Status modal draw it as a slider with a one-minute trace`.

- [ ] **Step 4: Goldens, then the full run.** UI goldens are not in CI: `python3 -m pytest tests/ui/test_screens.py -k ams --accept-goldens`, then open `tests/ui/goldens/ams.png` and confirm the only change is the buffer box border and the loaded card's slider. `make check-doc-anchors` (advisory). Then, once: `scripts/helix-claim run heavy:sweep -- make full-test-run`. Expected: all green. Optional on zeus after pushing the branch: `scripts/zeus-run.sh asan '[buffer]'` and `scripts/zeus-run.sh asan '[buffer_status]'` (renderer and modal lifetimes).

- [ ] **Step 5: Commit.**

```bash
git add -N docs/images/user/home-filament-buffer.png
git commit -m "docs(ams): the filament buffer reading, its widget and modal" \
  -m "Developer docs gain a Filament buffer reading section (the data path, subjects, trace and renderer) and the backend leaves name the new surfaces; the user guide documents the Filament Buffer widget and drops the clog widget's buffer page. AMS UI golden regenerated." \
  -- docs/devel/FILAMENT_MANAGEMENT.md docs/devel/FILAMENT_BACKEND_OPENAMS.md docs/devel/FILAMENT_BACKEND_AFC.md \
     docs/devel/architecture/09-home-widgets.md docs/devel/CHANGELOG_1_1_DRAFT.md \
     docs/user/guide/home-panel.md docs/user/guide/filament.md docs/images/user/home-filament-buffer.png \
     tests/ui/goldens/ams.png
git show --stat HEAD
```

The spec and this plan stay until Preston says ship; then delete both in the change that merges the work (`docs/CLAUDE.md` § Plans and Specs). Not part of this plan's steps.
