# LEDs Overlay and Per-Device Light Buttons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split "which LED am I looking at" from "which LEDs does this control drive": a per-device LEDs overlay with tabs, per-instance home light buttons, and an auto-state target of its own, replacing `LedController::selected_strips()`.

**Architecture:** Pure decision functions (`include/led/led_devices.h`, `include/led/led_device_page.h`) answer every "which device / which sections / which targets" question. `LedController` gains per-device state and target-explicit operations (`set_power(ids)`, `set_color(ids)`, `set_brightness(ids)`, `device_state(id)`) plus a `led_state_version` subject. Each consumer (auto-state, light buttons, overlay, entry points) moves onto those, and the selection API is deleted last, so no call path can fan out to "the selection" any more.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml (XML -> subjects -> C++), Catch2 (`make t F='[tag]'`), Moonraker mock (`--test`), `helix-screen ctl`.

**Spec:** `docs/devel/plans/2026-09-27-led-controls-redesign-design.md` (binding). Mockups linked there. Issue: prestonbrown/helixscreen#1130.

**Worktree:** `/home/pbrown/Code/Printing/helixscreen/.worktrees/1130-leds-redesign` (branch `feature/1130-leds-redesign`). Below, `$T` is that path. Bash cwd resets between calls: use `make -C "$T"`, `git -C "$T"`.

## Global Constraints

- Before your first edit: `scripts/helix-claim take worktree:1130-leds-redesign "<task>"` from the main tree; release after the commit lands.
- Overlay title is **LEDs** (translation tag `LEDs`); `get_name()` returns `"LEDs"`. Settings keeps "LED Settings".
- Chamber-light spellings, case-insensitive, in this preference order: `chamber_light`, `chamber_LED`, `case_light`, `caselight`; fallback `first_available_strip()`. They appear in `src/led/led_devices.cpp` and nowhere else.
- Default color presets, in order: red `0xFF4444`, orange `0xFF6B35`, green `0x66BB6A`, cyan `0x00BCD4`, blue `0x2962FF`, purple `0x9C27B0`, pink `0xFF4081`.
- Level chips: 10, 25, 50, 75, 100 (%). White tones: Cool, Neutral, Warm, fixed values, not user-editable.
- Config keys: `leds/auto_state/strips` (array of device ids), `leds/light_button_pending` (string, consumed once), per-widget config key `led` (a device id, or `"all"` for All lights; absent = chamber light).
- ESP32 firmware builds `-fno-exceptions`; `python3 scripts/check_esp32_app_srcs.py` rejects in `src/` and headers: `try`/`catch`/`throw`, json `.value("k",d)` and `.at()`, one-arg `json::parse`, `std::sto*`, value-form `any_cast`. Use `json::parse(text,nullptr,false)` + `is_discarded()`, `helix::json_util::safe_*`/`as_*`, `helix::text_io::parse_leading<T>`. Every new `src/**/*.cpp` gets a bare line in `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (the LED stack is compiled on the firmware).
- Declarative UI (`.claude/rules/declarative-ui.md`): no new `lv_obj_add_event_cb`, `lv_label_set_text`, imperative hidden flags or C++ styling. Per-index data goes through `helix::xml::IndexedSubjectPool` + `<repeat count="subject">`; clicks through `<event_cb ... user_data="$i"/>`. `python3 scripts/check_imperative_ui.py --summary` must not rise across the branch.
- Threading (`.claude/rules/threading.md`): LVGL and subjects on the main thread only. WLED/HTTP completions defer through an `AsyncLifetimeGuard` token before touching a subject.
- `#include "hv/json.hpp"`, spdlog only, SPDX header on new files, no RTTI.
- Comments describe the code as it is; no history, no SHAs, no "used to".
- Every task: test first, `make -C "$T" t F='<tag>'` red then green, `make -C "$T" mutate-diff` (or the named hand mutation) proving the test goes red, one line in the commit body naming the mutation. Commit subject ends `(prestonbrown/helixscreen#1130)`. The worktree is private, so `git -C "$T" add <explicit paths>` then commit is fine; never `git add -A`.
- Screenshots at 800x480 and 480x272, saved under `"$SCRATCH"/1130-shots/task-<N>/` where `$SCRATCH` is your session scratchpad directory.
- Non-goals: long-press light menu, hand-picked groups for one button, colors for WLED.

### UI verification harness (used by every UI task)

```bash
T=/home/pbrown/Code/Printing/helixscreen/.worktrees/1130-leds-redesign
TREE=1130-leds-redesign
export HELIX_SOCK=/tmp/helix-$TREE.sock HELIX_CONFIG_DIR=/tmp/helix-config-$TREE
mkdir -p "$HELIX_CONFIG_DIR" "$SCRATCH/1130-shots/task-$N"
make -C "$T" -j"$("$T"/scripts/helix-claim jobs)"
# SIZE is 800x480 or 480x272
( cd "$T" && SDL_VIDEODRIVER=dummy ./build/bin/helix-screen --test --skip-wizard -s "$SIZE" -vv \
    --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 & echo $! > /tmp/helix-$TREE.pid )
CTL() { "$T"/build/bin/helix-screen ctl -s "$HELIX_SOCK" "$@"; }
until CTL ping >/dev/null 2>&1; do sleep 0.5; done
# ... task-specific CTL commands ...
kill "$(cat /tmp/helix-$TREE.pid)"     # your PID only; never pkill helix-screen
```

The default mock (Voron 2.4) has `neopixel chamber_light` (RGBW, three led_effects), `neopixel status_led` (one effect), `led caselight` (single channel), `output_pin Enclosure_LEDs` (PWM) and two WLED strips (`printer_led`, `enclosure_led`). Macro devices come from config: after the first launch has written `$HELIX_CONFIG_DIR/settings.json`, stop the app and seed them (a hand-written file without `config_version` is discarded):

```bash
python3 - "$HELIX_CONFIG_DIR/settings.json" <<'EOF'
import json, sys
p = sys.argv[1]; c = json.load(open(p))
leds = c["printers"][c["active_printer_id"]].setdefault("leds", {})
leds["macro_devices"] = [
  {"name": "Lamp Macro", "type": "on_off", "on_macro": "LIGHTS_ON", "off_macro": "LIGHTS_OFF", "toggle_macro": "", "presets": []},
  {"name": "Toggle Macro", "type": "toggle", "on_macro": "", "off_macro": "", "toggle_macro": "LIGHT_TOGGLE", "presets": []},
  {"name": "Party", "type": "preset", "on_macro": "", "off_macro": "", "toggle_macro": "", "presets": [{"macro": "LED_PARTY"}, {"macro": "LED_RAINBOW"}]}]
json.dump(c, open(p, "w"), indent=2)
EOF
```

The mock has no RGB-only strip and no on/off (non-PWM) output pin; those two capability rows are proven by the classifier tests (Task 2) and the overlay model tests (Task 7), not by screenshots.

### Rulings on spec gaps (binding for executors)

- The chamber-light match considers only NATIVE and OUTPUT_PIN devices (Klipper objects), comparing the id's text after its type prefix; spelling order is preference order.
- A single-channel Klipper LED shows Effects when led_effect targets it, Level chips otherwise.
- Non-home light buttons (controls-panel light cell, print-status light button, Settings "LED light" toggle) act on the chamber light, the same as an unset home light button.
- An empty `leds/auto_state/strips` means the chamber light. The "Applies to" chip row never lets the last chip be deselected.
- The legacy selection migrates at discovery (the only time the switchable set is known). "Every switchable device selected" means the saved selection contains every switchable id; extra ids (a WLED strip not yet discovered) do not break that.
- "Existing light buttons get that device" is done by staging `leds/light_button_pending`; each home light button without a `led` key adopts it the first time it binds, then the key is cleared. Buttons added later default to the chamber light.
- The first-run wizard's LED step writes through the same staging (`stage_light_selection`) instead of `leds/selected_strips`.
- Macro devices show Unknown power everywhere (tab dot, tile bulb); a light button toggling them alternates using what it last sent.
- The last focused device lives in memory for the session only.
- The page follows a live status change for the tab dots and the power button; the brightness slider and swatch selection are re-read only on focus change and activation, so a status frame never moves a slider under a finger.
- A color swatch keeps the current brightness (100% when the light is off) and clears the white channel; white comes only from the White section.
- The light tile shows the › zone when it is at least two cells wide (`colspan >= 2 * GridLayout::TRACKS_PER_CELL`).
- The Custom swatch uses an XML conical gradient. The opt-in NanoVG draw unit has no conical support (`lib/lvgl/src/draw/nanovg/lv_draw_nanovg_grad.c#lv_nanovg_grad_to_paint`); no shipped target enables it.

## Review Focus

1. A light button whose saved device no longer exists (strip renamed in printer.cfg, macro device deleted) must drive and name the chamber light, never do nothing. Pinned by Task 1 (`resolve_light_targets: an unknown id falls back to the chamber light`) and Task 9 (`LedWidget: a vanished device resolves to the chamber light`).
2. A printer with no switchable LEDs at all (or only PRESET macros): the overlay opens without crashing, startup does nothing and waits, widgets stay gated. Pinned by Task 1 (no-devices cases), Task 5 (`apply_startup_preference with no targets defers`), Task 7 (`overlay with no devices`).
3. WLED strips arrive after discovery (async), so the migration sees a saved selection naming a strip that is not yet switchable. It must still classify "everything" as All lights. Pinned by Task 4 (`plan_selection_migration: an undiscovered extra id still means every device`).
4. A status frame arrives while the user drags the brightness slider. The slider must not jump; tab dots must update. Pinned by Task 7 (`a state bump updates dots but not the slider`).
5. A macro device is deleted in Settings while it is an auto-state target or the overlay's last focused tab. It must drop out of auto-state and the overlay must refocus. Pinned by Task 6 (`deleting a macro device removes it from Applies to`) and Task 7 (`a vanished focused device refocuses the chamber light`).

---

### Task 1: Chamber-light resolver and light-target resolution

**Files:**
- Create: `include/led/led_devices.h`, `src/led/led_devices.cpp`, `tests/unit/test_led_devices.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/led/led_devices.cpp` directly after `src/led/led_controller.cpp`)

**Interfaces:**
- Consumes: `helix::led::LedStripInfo`, `LedBackendType` (`include/led/led_backend.h`).
- Produces (namespace `helix::led`):
  - `constexpr const char* LIGHT_BUTTON_ALL = "all";`
  - `std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices, const std::string& fallback);`
  - `std::vector<std::string> resolve_light_targets(const std::string& key, const std::vector<std::string>& switchable, const std::string& chamber);`
  - `std::vector<std::string> union_light_targets(const std::vector<std::string>& keys, const std::vector<std::string>& switchable, const std::string& chamber);`

- [ ] **Step 1: Write the failing test** `tests/unit/test_led_devices.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_devices.h"

#include "../catch_amalgamated.hpp"

using namespace helix::led;

namespace {

LedStripInfo dev(const std::string& id, LedBackendType backend) {
    LedStripInfo s;
    s.id = id;
    s.name = id;
    s.backend = backend;
    s.supports_color = false;
    s.supports_white = false;
    return s;
}

const LedStripInfo SB = dev("neopixel sb_leds", LedBackendType::NATIVE);

} // namespace

TEST_CASE("resolve_chamber_light: every spelling matches", "[led][devices]") {
    CHECK(resolve_chamber_light({SB, dev("neopixel chamber_light", LedBackendType::NATIVE)}, "fb") ==
          "neopixel chamber_light");
    CHECK(resolve_chamber_light({SB, dev("led chamber_LED", LedBackendType::NATIVE)}, "fb") ==
          "led chamber_LED");
    CHECK(resolve_chamber_light({SB, dev("neopixel case_light", LedBackendType::NATIVE)}, "fb") ==
          "neopixel case_light");
    CHECK(resolve_chamber_light({SB, dev("output_pin caselight", LedBackendType::OUTPUT_PIN)},
                                "fb") == "output_pin caselight");
}

TEST_CASE("resolve_chamber_light: case does not matter", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("neopixel Chamber_Light", LedBackendType::NATIVE)}, "fb") ==
          "neopixel Chamber_Light");
    CHECK(resolve_chamber_light({dev("led CASELIGHT", LedBackendType::NATIVE)}, "fb") ==
          "led CASELIGHT");
}

TEST_CASE("resolve_chamber_light: spelling order is preference order", "[led][devices]") {
    const std::vector<LedStripInfo> both = {dev("led caselight", LedBackendType::NATIVE),
                                            dev("neopixel chamber_light", LedBackendType::NATIVE)};
    CHECK(resolve_chamber_light(both, "fb") == "neopixel chamber_light");
}

TEST_CASE("resolve_chamber_light: only whole object names match", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("neopixel chamber_light_bar", LedBackendType::NATIVE),
                                 dev("neopixel my_caselight", LedBackendType::NATIVE)},
                                "fb") == "fb");
}

TEST_CASE("resolve_chamber_light: macros and WLED are not Klipper objects", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("macro:chamber_light", LedBackendType::MACRO),
                                 dev("chamber_light", LedBackendType::WLED)},
                                "fb") == "fb");
}

TEST_CASE("resolve_chamber_light: fallback, and no devices at all", "[led][devices]") {
    CHECK(resolve_chamber_light({SB}, "neopixel sb_leds") == "neopixel sb_leds");
    CHECK(resolve_chamber_light({}, "").empty());
}

TEST_CASE("resolve_light_targets: each key shape", "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a", "neopixel b", "macro:Lamp"};
    CHECK(resolve_light_targets("", sw, "neopixel a") == std::vector<std::string>{"neopixel a"});
    CHECK(resolve_light_targets(LIGHT_BUTTON_ALL, sw, "neopixel a") == sw);
    CHECK(resolve_light_targets("macro:Lamp", sw, "neopixel a") ==
          std::vector<std::string>{"macro:Lamp"});
}

TEST_CASE("resolve_light_targets: an unknown id falls back to the chamber light",
          "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a"};
    CHECK(resolve_light_targets("neopixel gone", sw, "neopixel a") ==
          std::vector<std::string>{"neopixel a"});
}

TEST_CASE("resolve_light_targets: nothing to drive", "[led][devices]") {
    CHECK(resolve_light_targets("", {}, "").empty());
    CHECK(resolve_light_targets(LIGHT_BUTTON_ALL, {}, "").empty());
    CHECK(resolve_light_targets("neopixel gone", {}, "").empty());
}

TEST_CASE("union_light_targets: union in first-seen order, chamber when no buttons",
          "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a", "neopixel b", "neopixel c"};
    CHECK(union_light_targets({}, sw, "neopixel b") == std::vector<std::string>{"neopixel b"});
    CHECK(union_light_targets({"neopixel c", "", "neopixel c"}, sw, "neopixel b") ==
          std::vector<std::string>{"neopixel c", "neopixel b"});
    CHECK(union_light_targets({"", LIGHT_BUTTON_ALL}, sw, "neopixel b") ==
          std::vector<std::string>{"neopixel b", "neopixel a", "neopixel c"});
}
```

- [ ] **Step 2: Run it red**

Run: `make -C "$T" t F='[led][devices]'`
Expected: compile failure, `led/led_devices.h: No such file or directory`.

- [ ] **Step 3: Implement** `include/led/led_devices.h`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_backend.h"

#include <string>
#include <vector>

namespace helix::led {

/// Light-button config value meaning "every switchable device".
constexpr const char* LIGHT_BUTTON_ALL = "all";

/// The printer's main light: the first NATIVE or OUTPUT_PIN device whose Klipper
/// object name (the id after its type prefix) is chamber_light, chamber_LED,
/// case_light or caselight, compared case-insensitively and preferred in that
/// order. @p fallback when none matches.
std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices,
                                  const std::string& fallback);

/// Device ids a light button whose `led` config is @p key drives. Empty key: the
/// chamber light. LIGHT_BUTTON_ALL: every switchable device. A switchable id: that
/// device. An id that no longer exists: the chamber light.
std::vector<std::string> resolve_light_targets(const std::string& key,
                                               const std::vector<std::string>& switchable,
                                               const std::string& chamber);

/// resolve_light_targets() over every key, deduplicated in first-seen order. No
/// keys at all means the chamber light.
std::vector<std::string> union_light_targets(const std::vector<std::string>& keys,
                                             const std::vector<std::string>& switchable,
                                             const std::string& chamber);

} // namespace helix::led
```

`src/led/led_devices.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_devices.h"

#include <algorithm>
#include <cctype>

namespace helix::led {

namespace {

constexpr const char* CHAMBER_LIGHT_NAMES[] = {"chamber_light", "chamber_LED", "case_light",
                                               "caselight"};

std::string lowered(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// "neopixel chamber_light" -> "chamber_light"; an id without a prefix is its own name.
std::string object_name(const std::string& id) {
    const auto space = id.find(' ');
    return space == std::string::npos ? id : id.substr(space + 1);
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices,
                                  const std::string& fallback) {
    for (const char* wanted : CHAMBER_LIGHT_NAMES) {
        const std::string want = lowered(wanted);
        for (const auto& d : devices) {
            if (d.backend != LedBackendType::NATIVE && d.backend != LedBackendType::OUTPUT_PIN) {
                continue;
            }
            if (lowered(object_name(d.id)) == want) {
                return d.id;
            }
        }
    }
    return fallback;
}

std::vector<std::string> resolve_light_targets(const std::string& key,
                                               const std::vector<std::string>& switchable,
                                               const std::string& chamber) {
    if (key == LIGHT_BUTTON_ALL) {
        return switchable;
    }
    if (!key.empty() && contains(switchable, key)) {
        return {key};
    }
    if (chamber.empty()) {
        return {};
    }
    return {chamber};
}

std::vector<std::string> union_light_targets(const std::vector<std::string>& keys,
                                             const std::vector<std::string>& switchable,
                                             const std::string& chamber) {
    if (keys.empty()) {
        return resolve_light_targets("", switchable, chamber);
    }
    std::vector<std::string> out;
    for (const auto& key : keys) {
        for (auto& id : resolve_light_targets(key, switchable, chamber)) {
            if (!contains(out, id)) {
                out.push_back(std::move(id));
            }
        }
    }
    return out;
}

} // namespace helix::led
```

Add `src/led/led_devices.cpp` to `app_srcs.txt` after `src/led/led_controller.cpp`.

- [ ] **Step 4: Run it green, and the firmware gate**

Run: `make -C "$T" t F='[led][devices]'` then `python3 "$T"/scripts/check_esp32_app_srcs.py`
Expected: all `[led][devices]` pass; checker exits 0.

- [ ] **Step 5: Prove the tests can fail**

Mutation: in `resolve_chamber_light` compare `object_name(d.id)` without `lowered()` -> "case does not matter" goes red. Swap the two loops (devices outer) -> "spelling order is preference order" goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_devices.h src/led/led_devices.cpp tests/unit/test_led_devices.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
git -C "$T" commit -m "feat(led): one resolver for the chamber light and light-button targets (prestonbrown/helixscreen#1130)" -m "Mutation: dropping the case fold turns 'case does not matter' red."
```

---

### Task 2: Device-page classifier and white tones

**Files:**
- Create: `include/led/led_device_page.h`, `src/led/led_device_page.cpp`, `tests/unit/test_led_device_page.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/led/led_device_page.cpp` after `src/led/led_devices.cpp`)

**Interfaces:**
- Consumes: `LedStripInfo`, `LedBackendType`, `MacroLedType` (`include/led/led_backend.h`).
- Produces (namespace `helix::led`):
  - `enum class LampControl : int { PowerAndBrightness = 0, PowerOnly = 1, OnOffButtons = 2, ToggleButton = 3, None = 4 };`
  - `enum class WhiteMode : int { None = 0, WChannel = 1, Mixed = 2 };`
  - `enum class ListKind : int { None = 0, Effects = 1, Presets = 2, LevelChips = 3 };`
  - `struct DevicePage { LampControl lamp; WhiteMode white; bool color; ListKind list; bool operator==(const DevicePage&) const; };`
  - `DevicePage classify_device_page(const LedStripInfo& device, MacroLedType macro_type, bool has_effects);`
  - `enum class WhiteTone : int { Cool = 0, Neutral = 1, Warm = 2 };`
  - `struct Rgbw { double r, g, b, w; bool operator==(const Rgbw&) const; };`
  - `Rgbw white_tone(WhiteTone tone, WhiteMode mode);`
  - `constexpr int LEVEL_CHIPS[] = {10, 25, 50, 75, 100};`

- [ ] **Step 1: Write the failing test** `tests/unit/test_led_device_page.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_device_page.h"

#include "../catch_amalgamated.hpp"

using namespace helix::led;

namespace {

LedStripInfo native(bool color, bool white) {
    LedStripInfo s;
    s.id = "neopixel x";
    s.backend = LedBackendType::NATIVE;
    s.supports_color = color;
    s.supports_white = white;
    return s;
}

LedStripInfo of(LedBackendType backend, bool pwm = false) {
    LedStripInfo s;
    s.id = "x";
    s.backend = backend;
    s.supports_color = false;
    s.supports_white = false;
    s.is_pwm = pwm;
    return s;
}

constexpr auto ANY = MacroLedType::TOGGLE; // ignored unless the device is a macro

} // namespace

TEST_CASE("page: Klipper LED, RGBW", "[led][page]") {
    CHECK(classify_device_page(native(true, true), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::WChannel, true, ListKind::Effects});
    CHECK(classify_device_page(native(true, true), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::WChannel, true, ListKind::None});
}

TEST_CASE("page: Klipper LED, RGB", "[led][page]") {
    CHECK(classify_device_page(native(true, false), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::Mixed, true, ListKind::Effects});
    CHECK(classify_device_page(native(true, false), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::Mixed, true, ListKind::None});
}

TEST_CASE("page: Klipper LED, single channel", "[led][page]") {
    CHECK(classify_device_page(native(false, false), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::LevelChips});
    CHECK(classify_device_page(native(false, false), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Effects});
}

TEST_CASE("page: WLED", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::WLED), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Presets});
}

TEST_CASE("page: output pin, PWM", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::OUTPUT_PIN, true), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::LevelChips});
}

TEST_CASE("page: output pin, on/off", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::OUTPUT_PIN, false), ANY, false) ==
          DevicePage{LampControl::PowerOnly, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro ON_OFF", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::ON_OFF, false) ==
          DevicePage{LampControl::OnOffButtons, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro TOGGLE", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::TOGGLE, false) ==
          DevicePage{LampControl::ToggleButton, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro PRESET", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::PRESET, false) ==
          DevicePage{LampControl::None, WhiteMode::None, false, ListKind::Presets});
}

TEST_CASE("page: an effect is not a device", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::LED_EFFECT), ANY, true) == DevicePage{});
}

TEST_CASE("white_tone: the W channel carries white on RGBW", "[led][page]") {
    const Rgbw n = white_tone(WhiteTone::Neutral, WhiteMode::WChannel);
    CHECK(n.w == Catch::Approx(1.0));
    CHECK(n.r + n.g + n.b == Catch::Approx(0.0));
    for (auto t : {WhiteTone::Cool, WhiteTone::Warm}) {
        CHECK(white_tone(t, WhiteMode::WChannel).w == Catch::Approx(1.0));
    }
}

TEST_CASE("white_tone: RGB strips mix white and never touch W", "[led][page]") {
    for (auto t : {WhiteTone::Cool, WhiteTone::Neutral, WhiteTone::Warm}) {
        CHECK(white_tone(t, WhiteMode::Mixed).w == Catch::Approx(0.0));
    }
}

TEST_CASE("white_tone: cool is bluer than warm", "[led][page]") {
    for (auto mode : {WhiteMode::WChannel, WhiteMode::Mixed}) {
        const Rgbw cool = white_tone(WhiteTone::Cool, mode);
        const Rgbw warm = white_tone(WhiteTone::Warm, mode);
        CHECK(cool.b > warm.b);
        CHECK(warm.r > cool.r);
    }
    CHECK(white_tone(WhiteTone::Cool, WhiteMode::None) == Rgbw{});
}
```

- [ ] **Step 2: Run it red**

Run: `make -C "$T" t F='[led][page]'`
Expected: compile failure, `led/led_device_page.h: No such file or directory`.

- [ ] **Step 3: Implement** `include/led/led_device_page.h`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_backend.h"

namespace helix::led {

/// What the left (lamp) column of an LED device page holds.
enum class LampControl : int {
    PowerAndBrightness = 0,
    PowerOnly = 1,
    OnOffButtons = 2,
    ToggleButton = 3,
    None = 4,
};

/// How the White section reaches white, or that there is none.
enum class WhiteMode : int { None = 0, WChannel = 1, Mixed = 2 };

/// The chip row under the right column.
enum class ListKind : int { None = 0, Effects = 1, Presets = 2, LevelChips = 3 };

/// The sections one device's page shows. Values double as the page subjects'
/// integers, so the XML binds to them directly.
struct DevicePage {
    LampControl lamp = LampControl::None;
    WhiteMode white = WhiteMode::None;
    bool color = false;
    ListKind list = ListKind::None;

    bool operator==(const DevicePage& o) const {
        return lamp == o.lamp && white == o.white && color == o.color && list == o.list;
    }
};

/// The page for @p device. @p macro_type is read only for MACRO devices;
/// @p has_effects says whether led_effect defines any effect for this strip.
DevicePage classify_device_page(const LedStripInfo& device, MacroLedType macro_type,
                                bool has_effects);

enum class WhiteTone : int { Cool = 0, Neutral = 1, Warm = 2 };

/// Channel levels 0.0-1.0 at full brightness.
struct Rgbw {
    double r = 0.0, g = 0.0, b = 0.0, w = 0.0;

    bool operator==(const Rgbw& o) const {
        return r == o.r && g == o.g && b == o.b && w == o.w;
    }
};

/// The fixed white tones. RGBW strips light the W channel and tint it with RGB;
/// RGB strips mix white from RGB alone.
Rgbw white_tone(WhiteTone tone, WhiteMode mode);

constexpr int LEVEL_CHIPS[] = {10, 25, 50, 75, 100};

} // namespace helix::led
```

`src/led/led_device_page.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_device_page.h"

namespace helix::led {

DevicePage classify_device_page(const LedStripInfo& device, MacroLedType macro_type,
                                bool has_effects) {
    switch (device.backend) {
    case LedBackendType::NATIVE:
        if (device.supports_color) {
            return {LampControl::PowerAndBrightness,
                    device.supports_white ? WhiteMode::WChannel : WhiteMode::Mixed, true,
                    has_effects ? ListKind::Effects : ListKind::None};
        }
        return {LampControl::PowerAndBrightness, WhiteMode::None, false,
                has_effects ? ListKind::Effects : ListKind::LevelChips};
    case LedBackendType::WLED:
        return {LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Presets};
    case LedBackendType::OUTPUT_PIN:
        if (device.is_pwm) {
            return {LampControl::PowerAndBrightness, WhiteMode::None, false,
                    ListKind::LevelChips};
        }
        return {LampControl::PowerOnly, WhiteMode::None, false, ListKind::None};
    case LedBackendType::MACRO:
        switch (macro_type) {
        case MacroLedType::ON_OFF:
            return {LampControl::OnOffButtons, WhiteMode::None, false, ListKind::None};
        case MacroLedType::TOGGLE:
            return {LampControl::ToggleButton, WhiteMode::None, false, ListKind::None};
        case MacroLedType::PRESET:
            return {LampControl::None, WhiteMode::None, false, ListKind::Presets};
        }
        break;
    case LedBackendType::LED_EFFECT:
        break;
    }
    return {};
}

Rgbw white_tone(WhiteTone tone, WhiteMode mode) {
    if (mode == WhiteMode::WChannel) {
        switch (tone) {
        case WhiteTone::Cool:
            return {0.0, 0.0, 0.25, 1.0};
        case WhiteTone::Neutral:
            return {0.0, 0.0, 0.0, 1.0};
        case WhiteTone::Warm:
            return {0.35, 0.12, 0.0, 1.0};
        }
    }
    if (mode == WhiteMode::Mixed) {
        switch (tone) {
        case WhiteTone::Cool:
            return {0.80, 0.88, 1.0, 0.0};
        case WhiteTone::Neutral:
            return {1.0, 0.95, 0.88, 0.0};
        case WhiteTone::Warm:
            return {1.0, 0.72, 0.42, 0.0};
        }
    }
    return {};
}

} // namespace helix::led
```

Add `src/led/led_device_page.cpp` to `app_srcs.txt`.

- [ ] **Step 4: Run it green, and the firmware gate**

Run: `make -C "$T" t F='[led][page]'` then `python3 "$T"/scripts/check_esp32_app_srcs.py`
Expected: pass; checker exits 0.

- [ ] **Step 5: Prove the tests can fail**

Mutation: make the single-channel branch return `ListKind::LevelChips` regardless of `has_effects` -> "single channel" goes red. Swap the output-pin `is_pwm` branches -> both pin rows go red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_device_page.h src/led/led_device_page.cpp tests/unit/test_led_device_page.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
git -C "$T" commit -m "feat(led): classify each LED device into the sections its page shows (prestonbrown/helixscreen#1130)" -m "Mutation: ignoring has_effects on single-channel strips turns its row red."
```

---

### Task 3: Per-device state and targeted operations in LedController

**Files:**
- Modify: `include/led/led_devices.h`, `src/led/led_devices.cpp` (add `PowerState`, `DeviceState`, `next_power_on`)
- Modify: `include/led/led_controller.h`, `src/led/led_controller.cpp`
- Modify: `src/printer/printer_state.cpp#update_from_status` (the three `led_ctrl.*().update_from_status(state)` lines become one `led_ctrl.update_from_status(state)`)
- Test: `tests/unit/test_led_devices.cpp` (append), create `tests/unit/test_led_device_state.cpp`

**Interfaces:**
- Consumes: Task 1 `resolve_chamber_light`, `resolve_light_targets`.
- Produces:
  - in `led_devices.h`: `enum class PowerState : int { Off = 0, On = 1, Unknown = 2 };` `struct DeviceState { PowerState power = PowerState::Unknown; int brightness = 0; uint32_t rgb = 0xFFFFFF; bool has_rgb = false; };` `bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on);`
  - `bool NativeBackend::update_from_status(const nlohmann::json&)`, `bool LedEffectBackend::update_from_status(...)`, `bool OutputPinBackend::update_from_status(...)` (true when the frame carried one of their objects), `bool OutputPinBackend::has_value(const std::string& pin_id) const`, `bool WledBackend::has_strip_state(const std::string& strip_id) const`.
  - `LedController`:
    - `std::vector<LedStripInfo> all_devices() const;` (`all_selectable_strips()` then every named PRESET macro as `macro:<name>`)
    - `std::vector<std::string> switchable_ids() const;` (ids of `all_selectable_strips()`)
    - `std::string chamber_light() const;` (`resolve_chamber_light(all_selectable_strips(), first_available_strip())`)
    - `std::vector<std::string> light_targets(const std::string& key) const;` (`resolve_light_targets(key, switchable_ids(), chamber_light())`)
    - `DeviceState device_state(const std::string& id) const;`
    - `void set_power(const std::vector<std::string>& ids, bool on);`
    - `bool toggle_power(const std::vector<std::string>& ids);` (returns the state sent)
    - `void set_color(const std::vector<std::string>& ids, double r, double g, double b, double w);`
    - `void set_brightness(const std::vector<std::string>& ids, int brightness_pct);`
    - `void update_from_status(const nlohmann::json& status);`
    - `void refresh_wled_state(std::function<void()> on_done = nullptr);`
    - `lv_subject_t* get_led_state_version_subject();` (registered as `"led_state_version"`, owned by `subjects_`)

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_led_devices.cpp`:

```cpp
TEST_CASE("next_power_on: any on turns everything off", "[led][devices]") {
    CHECK_FALSE(next_power_on({PowerState::Off, PowerState::On}, false));
    CHECK_FALSE(next_power_on({PowerState::Unknown, PowerState::On}, false));
}

TEST_CASE("next_power_on: known off with none on turns on", "[led][devices]") {
    CHECK(next_power_on({PowerState::Off, PowerState::Unknown}, true));
    CHECK(next_power_on({PowerState::Off}, true));
}

TEST_CASE("next_power_on: unreadable state alternates on what was last sent",
          "[led][devices]") {
    CHECK(next_power_on({PowerState::Unknown}, false));
    CHECK_FALSE(next_power_on({PowerState::Unknown, PowerState::Unknown}, true));
    CHECK(next_power_on({}, false));
}
```

Create `tests/unit/test_led_device_state.cpp`. Base the fixture on `LedApplyColorFixture` in `tests/unit/test_led_control_overlay.cpp` (MoonrakerClientMock VORON_24 + MoonrakerAPIMock + `set_klippy_state_sync(READY)`), and drain the UpdateQueue in the derived destructor as `LedControllerFixture` does in `tests/unit/test_led_controller.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

namespace {

LedStripInfo strip(const std::string& id, LedBackendType b, bool color = true,
                   bool white = true) {
    LedStripInfo s;
    s.id = id;
    s.name = id;
    s.backend = b;
    s.supports_color = color;
    s.supports_white = white;
    return s;
}

struct DeviceStateFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    DeviceStateFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        ctrl.native().add_strip(strip("neopixel a", LedBackendType::NATIVE));
        ctrl.native().add_strip(strip("neopixel b", LedBackendType::NATIVE));
        ctrl.native().add_strip(strip("led w", LedBackendType::NATIVE, false, false));
        ctrl.output_pin().add_pin(strip("output_pin enc", LedBackendType::OUTPUT_PIN, false, false));
        ctrl.wled().add_strip(strip("printer_led", LedBackendType::WLED, false, false));
        LedMacroInfo m;
        m.display_name = "Lamp";
        m.type = MacroLedType::ON_OFF;
        m.on_macro = "LIGHTS_ON";
        m.off_macro = "LIGHTS_OFF";
        ctrl.set_configured_macros({m});
        ctrl.rebuild_macro_backend();
    }
    ~DeviceStateFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        LedController::instance().deinit();
    }
    int version() {
        return lv_subject_get_int(LedController::instance().get_led_state_version_subject());
    }
};

} // namespace

TEST_CASE_METHOD(DeviceStateFixture, "device_state: native strip reads from status",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.device_state("neopixel a").power == PowerState::Unknown);
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 0.0, 0.0, 0.0}}}}}});
    const auto s = ctrl.device_state("neopixel a");
    CHECK(s.power == PowerState::On);
    CHECK(s.brightness == 100);
    CHECK(s.rgb == 0xFF0000);
    CHECK(s.has_rgb);
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{0.0, 0.0, 0.0, 0.0}}}}}});
    CHECK(ctrl.device_state("neopixel a").power == PowerState::Off);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: a white-only strip has no hue",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"led w", {{"color_data", {{0.0, 0.0, 0.0, 0.5}}}}}});
    const auto s = ctrl.device_state("led w");
    CHECK(s.power == PowerState::On);
    CHECK(s.brightness == 50);
    CHECK_FALSE(s.has_rgb);
}

TEST_CASE_METHOD(DeviceStateFixture, "device_state: output pin, WLED, macro", "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.device_state("output_pin enc").power == PowerState::Unknown);
    ctrl.update_from_status({{"output_pin enc", {{"value", 0.3}}}});
    CHECK(ctrl.device_state("output_pin enc").power == PowerState::On);
    CHECK(ctrl.device_state("output_pin enc").brightness == 30);

    CHECK(ctrl.device_state("printer_led").power == PowerState::Unknown);
    ctrl.wled().update_strip_state("printer_led", WledStripState{true, 128, -1});
    CHECK(ctrl.device_state("printer_led").power == PowerState::On);
    CHECK(ctrl.device_state("printer_led").brightness == 50);

    CHECK(ctrl.device_state("macro:Lamp").power == PowerState::Unknown);
}

TEST_CASE_METHOD(DeviceStateFixture, "update_from_status bumps the version only for LED objects",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    const int before = version();
    ctrl.update_from_status({{"extruder", {{"temperature", 200.0}}}});
    CHECK(version() == before);
    ctrl.update_from_status({{"neopixel b", {{"color_data", {{0.0, 1.0, 0.0, 0.0}}}}}});
    CHECK(version() == before + 1);
}

TEST_CASE_METHOD(DeviceStateFixture, "set_power reaches only the ids it is given",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.set_power({"neopixel a"}, true);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(ctrl.native().has_strip_color("neopixel a"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel b"));
}

TEST_CASE_METHOD(DeviceStateFixture, "toggle_power turns off a strip that is on",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel a", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK_FALSE(ctrl.toggle_power({"neopixel a"}));
}

TEST_CASE_METHOD(DeviceStateFixture, "toggle_power alternates an unreadable macro",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    CHECK(ctrl.toggle_power({"macro:Lamp"}));
    CHECK_FALSE(ctrl.toggle_power({"macro:Lamp"}));
}

TEST_CASE_METHOD(DeviceStateFixture, "all_devices appends PRESET macros after the switchable ones",
                 "[led][state]") {
    auto& ctrl = LedController::instance();
    LedMacroInfo p;
    p.display_name = "Party";
    p.type = MacroLedType::PRESET;
    p.presets = {"LED_PARTY"};
    auto macros = ctrl.configured_macros();
    macros.push_back(p);
    ctrl.set_configured_macros(macros);
    const auto devices = ctrl.all_devices();
    REQUIRE_FALSE(devices.empty());
    CHECK(devices.back().id == "macro:Party");
    const auto sw = ctrl.switchable_ids();
    CHECK(std::find(sw.begin(), sw.end(), "macro:Party") == sw.end());
}
```

If `NativeBackend::set_color` does not echo into `strip_colors_` through the mock, assert through the mock's executed-gcode record instead (see how `LedApplyColorFixture::sent()` observes it); the assertion must still be "a received a command, b did not".

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][devices]'` and `make -C "$T" t F='[led][state]'`
Expected: compile failures (`next_power_on`, `device_state`, `update_from_status` on `LedController`, `get_led_state_version_subject` undeclared).

- [ ] **Step 3: Implement**

`led_devices.h` additions (after `union_light_targets`; add `#include <cstdint>`):

```cpp
enum class PowerState : int { Off = 0, On = 1, Unknown = 2 };

/// What the UI can say about one device right now.
struct DeviceState {
    PowerState power = PowerState::Unknown;
    int brightness = 0;      ///< 0-100
    uint32_t rgb = 0xFFFFFF; ///< full-brightness hue; meaningful only when has_rgb
    bool has_rgb = false;    ///< false: draw the theme's light color instead
};

/// What a light button sends when tapped: off while any target is on, on while
/// any is off, and with nothing readable the opposite of what it last sent.
bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on);
```

`led_devices.cpp`:

```cpp
bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on) {
    bool any_off = false;
    for (auto s : states) {
        if (s == PowerState::On) {
            return false;
        }
        any_off = any_off || s == PowerState::Off;
    }
    return any_off ? true : !last_sent_on;
}
```

Backends (`src/led/led_controller.cpp`):
- `NativeBackend::update_from_status`, `LedEffectBackend::update_from_status`, `OutputPinBackend::update_from_status` return `bool`: set a local `carried = true` whenever `status.contains(<their id>)` and the entry parsed; return it.
- `bool OutputPinBackend::has_value(const std::string& pin_id) const { return pin_values_.count(pin_id) > 0; }`
- `bool WledBackend::has_strip_state(const std::string& strip_id) const { return strip_states_.count(strip_id) > 0; }`

`LedController`:
- In `init()`'s `if (!version_subject_initialized_)` block: `lv_subject_init_int(&led_state_version_, 0); subjects_.register_subject(&led_state_version_, "led_state_version");`.
- `void bump_state_version()` (private): `if (version_subject_initialized_) lv_subject_set_int(&led_state_version_, lv_subject_get_int(&led_state_version_) + 1);`
- `update_from_status`:

```cpp
void LedController::update_from_status(const nlohmann::json& status) {
    const bool native = native_.update_from_status(status);
    const bool effects = effects_.update_from_status(status);
    const bool pins = output_pin_.update_from_status(status);
    if (native || effects || pins) {
        bump_state_version();
    }
}
```

- `device_state`:

```cpp
DeviceState LedController::device_state(const std::string& id) const {
    DeviceState s;
    switch (backend_for_strip(id)) {
    case LedBackendType::NATIVE: {
        if (!native_.has_strip_color(id)) {
            return s;
        }
        uint32_t base = 0;
        int pct = 0;
        double white = 0.0;
        native_.get_strip_color(id).decompose(base, pct, white);
        s.power = pct > 0 ? PowerState::On : PowerState::Off;
        s.brightness = pct;
        const auto* info = find_strip(native_.strips(), id);
        s.has_rgb = info != nullptr && info->supports_color;
        s.rgb = (base == 0 && white > 0.0) ? 0xFFFFFFu : base;
        return s;
    }
    case LedBackendType::OUTPUT_PIN:
        if (!output_pin_.has_value(id)) {
            return s;
        }
        s.power = output_pin_.get_value(id) > 0.0 ? PowerState::On : PowerState::Off;
        s.brightness = output_pin_.brightness_pct(id);
        return s;
    case LedBackendType::WLED: {
        if (!wled_.has_strip_state(id)) {
            return s;
        }
        const auto w = wled_.get_strip_state(id);
        s.power = w.is_on ? PowerState::On : PowerState::Off;
        s.brightness = std::clamp(w.brightness * 100 / 255, 0, 100);
        return s;
    }
    case LedBackendType::MACRO:
    case LedBackendType::LED_EFFECT:
        return s;
    }
    return s;
}
```

- `set_power(ids, on)`: the body of `toggle_all(bool)` with `ids` replacing `selected_strips_` (keep the per-strip effect stop, the `Settle` factory and every backend branch unchanged). Additionally: each MACRO id records `macro_last_sent_on_[strip_id] = on;` and each NATIVE / OUTPUT_PIN id is inserted into `pending_query_ids_` (`std::set<std::string>`). After the loop, `if (in_flight_count_ == 0) query_led_state();`. `toggle_all(bool on)` becomes `set_power(selected_strips_, on);` until Task 11 deletes it.
- Replace `query_tracked_led_state()` with `query_led_state()`: builds `printer.objects.query` objects from `pending_query_ids_`, clears it, sends; the response path is the existing `helix::ui::queue_update([status]() { get_printer_state().update_from_status(status); })`, which now reaches `LedController::update_from_status`. `note_command_settled()` calls `query_led_state()` at zero.
- `toggle_power`:

```cpp
bool LedController::toggle_power(const std::vector<std::string>& ids) {
    std::vector<PowerState> states;
    bool last_sent_on = false;
    for (const auto& id : ids) {
        states.push_back(device_state(id).power);
        const auto it = macro_last_sent_on_.find(id);
        last_sent_on = last_sent_on || (it != macro_last_sent_on_.end() && it->second);
    }
    const bool on = next_power_on(states, last_sent_on);
    set_power(ids, on);
    return on;
}
```

- `set_color(ids, r, g, b, w)`: the loop of `set_color_all` over `ids` (NATIVE -> `native_.set_color`; OUTPUT_PIN -> luminance `set_value`), keeping `last_color_.white = w`. `set_color_all` becomes `light_on_ = ...; set_color(selected_strips_, r, g, b, w);`.
- `set_brightness(ids, pct)`: the loop of `set_brightness_all` over `ids`. `set_brightness_all` becomes `light_on_ = pct > 0; set_brightness(selected_strips_, pct);`.
- `all_devices()`, `switchable_ids()`, `chamber_light()`, `light_targets(key)` as in Interfaces; `all_devices()` skips unnamed drafts exactly as `all_selectable_strips()` does.
- `refresh_wled_state`:

```cpp
void LedController::refresh_wled_state(std::function<void()> on_done) {
    auto tok = lifetime_.token();
    wled_.poll_status([this, tok, on_done]() {
        tok.defer("LedController::wled_state", [this, on_done]() {
            bump_state_version();
            if (on_done) {
                on_done();
            }
        });
    });
}
```

- `deinit()` clears `macro_last_sent_on_` and `pending_query_ids_`.
- `src/printer/printer_state.cpp#update_from_status`: replace the three backend calls with `led_ctrl.update_from_status(state);`.

- [ ] **Step 4: Run green**

Run: `make -C "$T" t F='[led]'`
Expected: every `[led]` test passes, old and new.

- [ ] **Step 5: Prove the tests can fail**

Mutation: in `set_power`, iterate `selected_strips_` instead of `ids` -> "set_power reaches only the ids it is given" goes red. Drop the `bump_state_version()` call -> the version test goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_devices.h src/led/led_devices.cpp include/led/led_controller.h src/led/led_controller.cpp src/printer/printer_state.cpp tests/unit/test_led_devices.cpp tests/unit/test_led_device_state.cpp
git -C "$T" commit -m "feat(led): per-device state and target-explicit power, color and brightness (prestonbrown/helixscreen#1130)" -m "Mutation: set_power iterating the selection turns 'reaches only the ids it is given' red."
```

---

### Task 4: Selection migration, auto-state targets, color presets

**Files:**
- Modify: `include/led/led_devices.h`, `src/led/led_devices.cpp` (migration plan, presets)
- Modify: `include/led/led_controller.h`, `src/led/led_controller.cpp` (`load_config`, `discover_from_hardware`, `publish_controllable_state`, delete the static `DEFAULT_COLOR_PRESETS` members)
- Modify: `include/led/led_auto_state.h`, `src/led/led_auto_state.cpp`
- Modify: `src/ui/ui_wizard_led_select.cpp#cleanup`, `src/printer/hardware_validator.cpp#configured_led_strips`, `include/wizard_config_paths.h`
- Test: `tests/unit/test_led_devices.cpp`, `tests/unit/test_led_config.cpp`, `tests/unit/test_led_auto_state.cpp`, `tests/unit/test_hardware_validator.cpp`, `tests/unit/test_led_controller.cpp` (the auto-select and prune tests change)

**Interfaces:**
- Consumes: Task 3 `switchable_ids()`, `light_targets()`, `set_power/set_color/set_brightness(ids, ...)`.
- Produces:
  - `led_devices.h`: `constexpr const char* LIGHT_BUTTON_PENDING_PATH = "leds/light_button_pending";` `constexpr const char* AUTO_STATE_STRIPS_PATH = "leds/auto_state/strips";` `struct SelectionMigration { std::string light_button; std::vector<std::string> auto_state_strips; bool operator==(const SelectionMigration&) const; };` `SelectionMigration plan_selection_migration(const std::vector<std::string>& selected, const std::vector<std::string>& switchable);` `constexpr uint32_t DEFAULT_COLOR_PRESETS[7]`, `constexpr uint32_t PRE_1_1_DEFAULT_COLOR_PRESETS[8]`, `std::vector<uint32_t> migrate_color_presets(const std::vector<uint32_t>& saved);`
  - `led_auto_state.h`: `const std::vector<std::string>& LedAutoState::strips() const;` `void LedAutoState::set_strips(const std::vector<std::string>& ids);` `std::vector<std::string> LedAutoState::targets() const;` free `void stage_light_selection(const SelectionMigration& m);`
  - `LedController::apply_startup_preference(const std::vector<std::string>& targets);` (signature change; Task 5 supplies the real targets)
  - `led_controllable` means "at least one switchable device".

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_led_devices.cpp`:

```cpp
TEST_CASE("plan_selection_migration: one device", "[led][migration]") {
    const auto m = plan_selection_migration({"neopixel a"}, {"neopixel a", "neopixel b"});
    CHECK(m.light_button == "neopixel a");
    CHECK(m.auto_state_strips == std::vector<std::string>{"neopixel a"});
}

TEST_CASE("plan_selection_migration: every switchable device", "[led][migration]") {
    const auto m = plan_selection_migration({"neopixel b", "neopixel a"}, {"neopixel a", "neopixel b"});
    CHECK(m.light_button == LIGHT_BUTTON_ALL);
    CHECK(m.auto_state_strips == std::vector<std::string>{"neopixel b", "neopixel a"});
}

TEST_CASE("plan_selection_migration: an undiscovered extra id still means every device",
          "[led][migration]") {
    const auto m = plan_selection_migration({"neopixel a", "printer_led"}, {"neopixel a"});
    CHECK(m.light_button == LIGHT_BUTTON_ALL);
}

TEST_CASE("plan_selection_migration: anything else goes to auto-state only",
          "[led][migration]") {
    const auto m = plan_selection_migration({"neopixel a", "neopixel b"},
                                            {"neopixel a", "neopixel b", "neopixel c"});
    CHECK(m.light_button.empty());
    CHECK(m.auto_state_strips == std::vector<std::string>{"neopixel a", "neopixel b"});
}

TEST_CASE("plan_selection_migration: nothing selected", "[led][migration]") {
    CHECK(plan_selection_migration({}, {"neopixel a"}) == SelectionMigration{});
}

TEST_CASE("migrate_color_presets", "[led][migration]") {
    const std::vector<uint32_t> fresh(std::begin(DEFAULT_COLOR_PRESETS),
                                      std::end(DEFAULT_COLOR_PRESETS));
    const std::vector<uint32_t> old(std::begin(PRE_1_1_DEFAULT_COLOR_PRESETS),
                                    std::end(PRE_1_1_DEFAULT_COLOR_PRESETS));
    CHECK(migrate_color_presets({}) == fresh);
    CHECK(migrate_color_presets(old) == fresh);
    auto reordered = old;
    std::swap(reordered[0], reordered[1]);
    CHECK(migrate_color_presets(reordered) == reordered);
    CHECK(migrate_color_presets({0x123456}) == std::vector<uint32_t>{0x123456});
    CHECK(fresh == std::vector<uint32_t>{0xFF4444, 0xFF6B35, 0x66BB6A, 0x00BCD4, 0x2962FF,
                                         0x9C27B0, 0xFF4081});
}
```

In `tests/unit/test_led_config.cpp` (reuse `LedConfigFixture` and `clear_led_config_paths()`; extend the helper to also null `leds/auto_state/strips` and `leds/light_button_pending`):

```cpp
TEST_CASE_METHOD(LedConfigFixture, "LedController: a legacy selection migrates once at discovery",
                 "[led][config][migration]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + "leds/selected_strips",
             nlohmann::json::array({"neopixel a", "neopixel b"}));
    ctrl.init(nullptr, nullptr);

    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"neopixel a", "neopixel b", "extruder"}));
    ctrl.discover_from_hardware(discovery);

    const auto* strips = cfg->try_get_json(cfg->df() + "leds/auto_state/strips");
    REQUIRE(strips != nullptr);
    CHECK(*strips == nlohmann::json::array({"neopixel a", "neopixel b"}));
    const auto* pending = cfg->try_get_json(cfg->df() + "leds/light_button_pending");
    REQUIRE(pending != nullptr);
    CHECK(*pending == "all");

    // Run once: a changed legacy list does not re-migrate.
    cfg->set(cfg->df() + "leds/selected_strips", nlohmann::json::array({"neopixel b"}));
    cfg->set(cfg->df() + "leds/light_button_pending", nlohmann::json());
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    ctrl.discover_from_hardware(discovery);
    CHECK(*cfg->try_get_json(cfg->df() + "leds/auto_state/strips") ==
          nlohmann::json::array({"neopixel a", "neopixel b"}));
    CHECK(cfg->try_get_json(cfg->df() + "leds/light_button_pending")->is_null());
    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: a fresh config selects nothing and stays controllable",
                 "[led][config][migration]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);
    helix::PrinterDiscovery discovery;
    discovery.parse_objects(nlohmann::json::array({"neopixel a", "extruder"}));
    ctrl.discover_from_hardware(discovery);

    auto* cfg = Config::get_instance();
    const auto* strips = cfg->try_get_json(cfg->df() + "leds/auto_state/strips");
    CHECK((strips == nullptr || strips->is_null()));
    CHECK(ctrl.selected_strips().empty());
    CHECK(lv_subject_get_int(ctrl.get_led_controllable_subject()) == 1);
    ctrl.deinit();
}

TEST_CASE_METHOD(LedConfigFixture, "LedController: the pre-1.1 default presets load as the new default",
                 "[led][config][migration]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + "leds/color_presets",
             nlohmann::json::array({"#FFFFFF", "#FFD700", "#FF6B35", "#4FC3F7", "#FF4444",
                                    "#66BB6A", "#9C27B0", "#00BCD4"}));
    ctrl.init(nullptr, nullptr);
    CHECK(ctrl.color_presets().size() == 7);
    CHECK(ctrl.color_presets().front() == 0xFF4444);
    ctrl.deinit();
}
```

Update the existing expectations in `test_led_config.cpp` "default values after init" and "default presets have correct values" to the 7-entry list, and in `test_led_controller.cpp` rewrite "all strips stale triggers auto-select" and "stale selected strips pruned on discovery": after discovery with nothing saved, `selected_strips()` stays empty and `chamber_light()` returns the discovered `led chamber_LED`.

In `tests/unit/test_led_auto_state.cpp` (add `friend class LedAutoStateTestAccess;` to `LedAutoState`, whose `apply_action` is private):

```cpp
namespace helix::led {
class LedAutoStateTestAccess {
  public:
    static void apply(const LedStateAction& a) {
        LedAutoState::instance().apply_action(a);
    }
};
} // namespace helix::led

namespace {
struct AutoStateTargetFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> api;

    AutoStateTargetFixture() {
        state.init_subjects(false);
        state.set_klippy_state_sync(helix::KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, state);
        auto& ctrl = helix::led::LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            helix::led::LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = helix::led::LedBackendType::NATIVE;
            s.supports_color = true;
            s.supports_white = true;
            ctrl.native().add_strip(s);
        }
    }
    ~AutoStateTargetFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        helix::led::LedAutoState::instance().set_strips({});
        helix::led::LedController::instance().deinit();
    }
};
} // namespace

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: targets fall back to the chamber light",
                 "[led][auto_state]") {
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({});
    CHECK(as.targets() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({"neopixel gone"});
    CHECK(as.targets() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({"neopixel sb_leds"});
    CHECK(as.targets() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: strips round-trip through config",
                 "[led][auto_state]") {
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({"neopixel sb_leds"});
    as.save_config();
    as.set_strips({});
    as.load_config();
    CHECK(as.strips() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(AutoStateTargetFixture, "LedAutoState: a color action reaches only its targets",
                 "[led][auto_state]") {
    auto& ctrl = helix::led::LedController::instance();
    helix::led::LedAutoState::instance().set_strips({"neopixel sb_leds"});
    helix::led::LedStateAction a;
    a.action_type = "color";
    a.color = 0xFF0000;
    a.brightness = 100;
    helix::led::LedAutoStateTestAccess::apply(a);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(ctrl.native().has_strip_color("neopixel sb_leds"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel chamber_light"));
}
```

As in Task 3: if the mock does not echo into the native cache, assert on the mock's executed-gcode record instead; the assertion stays "sb_leds received a command, chamber_light did not".

In `tests/unit/test_hardware_validator.cpp`, a case that sets only `leds/auto_state/strips` = `["neopixel chamber_light"]` and checks the validator treats an LED as configured (no "suggest an LED" item) and flags it missing when discovery lacks it.

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][migration]'` and `make -C "$T" t F='[led][auto_state]'`
Expected: compile failures for `plan_selection_migration`, `migrate_color_presets`, `LedAutoState::set_strips/targets`.

- [ ] **Step 3: Implement**

`led_devices.h`:

```cpp
constexpr const char* LIGHT_BUTTON_PENDING_PATH = "leds/light_button_pending";
constexpr const char* AUTO_STATE_STRIPS_PATH = "leds/auto_state/strips";

/// Where a saved leds/selected_strips list goes when it stops being user-facing.
struct SelectionMigration {
    std::string light_button; ///< device id, LIGHT_BUTTON_ALL, or "" for the chamber light
    std::vector<std::string> auto_state_strips;

    bool operator==(const SelectionMigration& o) const {
        return light_button == o.light_button && auto_state_strips == o.auto_state_strips;
    }
};

SelectionMigration plan_selection_migration(const std::vector<std::string>& selected,
                                            const std::vector<std::string>& switchable);

/// red, orange, green, cyan, blue, purple, pink
constexpr uint32_t DEFAULT_COLOR_PRESETS[] = {0xFF4444, 0xFF6B35, 0x66BB6A, 0x00BCD4,
                                              0x2962FF, 0x9C27B0, 0xFF4081};

/// Recognised on load so that a list nobody edited becomes DEFAULT_COLOR_PRESETS.
constexpr uint32_t PRE_1_1_DEFAULT_COLOR_PRESETS[] = {0xFFFFFF, 0xFFD700, 0xFF6B35, 0x4FC3F7,
                                                      0xFF4444, 0x66BB6A, 0x9C27B0, 0x00BCD4};

/// @p saved, unless it is empty or the untouched pre-1.1 default.
std::vector<uint32_t> migrate_color_presets(const std::vector<uint32_t>& saved);
```

`led_devices.cpp`:

```cpp
SelectionMigration plan_selection_migration(const std::vector<std::string>& selected,
                                            const std::vector<std::string>& switchable) {
    SelectionMigration m;
    m.auto_state_strips = selected;
    if (selected.size() == 1) {
        m.light_button = selected.front();
        return m;
    }
    const bool every =
        !selected.empty() && !switchable.empty() &&
        std::all_of(switchable.begin(), switchable.end(),
                    [&selected](const std::string& id) { return contains(selected, id); });
    if (every) {
        m.light_button = LIGHT_BUTTON_ALL;
    }
    return m;
}

std::vector<uint32_t> migrate_color_presets(const std::vector<uint32_t>& saved) {
    const std::vector<uint32_t> pre(std::begin(PRE_1_1_DEFAULT_COLOR_PRESETS),
                                    std::end(PRE_1_1_DEFAULT_COLOR_PRESETS));
    if (saved.empty() || saved == pre) {
        return {std::begin(DEFAULT_COLOR_PRESETS), std::end(DEFAULT_COLOR_PRESETS)};
    }
    return saved;
}
```

`LedController`:
- `load_config()`: after parsing `leds/color_presets` into `color_presets_`, `color_presets_ = migrate_color_presets(color_presets_);` and delete both `assign(DEFAULT_COLOR_PRESETS...)` branches (the no-config path becomes `color_presets_ = migrate_color_presets({});`). Delete the static members `DEFAULT_COLOR_PRESETS` / `DEFAULT_COLOR_PRESETS_COUNT`.
- New private `void migrate_legacy_selection();`, called in `discover_from_hardware()` right after `rebuild_macro_backend()` and before the prune block:

```cpp
void LedController::migrate_legacy_selection() {
    auto* cfg = Config::get_instance();
    if (cfg == nullptr || selected_strips_.empty()) {
        return;
    }
    const nlohmann::json* existing = cfg->try_get_json(cfg->df() + AUTO_STATE_STRIPS_PATH);
    if (existing != nullptr && existing->is_array()) {
        return;
    }
    const auto plan = plan_selection_migration(selected_strips_, switchable_ids());
    spdlog::info("[LedController] Legacy LED selection ({} strip(s)) -> light button '{}', "
                 "auto-state {} strip(s)",
                 selected_strips_.size(), plan.light_button, plan.auto_state_strips.size());
    stage_light_selection(plan);
}
```

- Delete the "Auto-select all discoverable strips" block in `discover_from_hardware()`.
- `publish_controllable_state()`: `desired = all_selectable_strips().empty() ? 0 : 1;`; call it from `set_configured_macros()` and `rebuild_macro_backend()` as well as its current sites.
- `light_set(bool on)` (removed in Task 11) targets `selected_strips_.empty() ? light_targets("") : selected_strips_`.
- `apply_startup_preference(const std::vector<std::string>& targets)`: replace the `selected_strips_.empty()` defer check with `targets.empty()`, and `light_set(true)` with `set_power(targets, true)`. Temporary caller in `src/application/application.cpp`: `apply_startup_preference(LedController::instance().light_targets(""))` (Task 5 replaces it).

`LedAutoState` (`include/led/led_auto_state.h` / `.cpp`):
- Member `std::vector<std::string> strips_;`; `strips()`, `set_strips(ids)` inline; `targets()`:

```cpp
std::vector<std::string> LedAutoState::targets() const {
    auto& ctrl = LedController::instance();
    const auto switchable = ctrl.switchable_ids();
    std::vector<std::string> out;
    for (const auto& id : strips_) {
        if (std::find(switchable.begin(), switchable.end(), id) != switchable.end()) {
            out.push_back(id);
        }
    }
    return out.empty() ? ctrl.light_targets("") : out;
}
```

- `load_config()`: `strips_ = cfg->get_string_array(cfg->df() + AUTO_STATE_STRIPS_PATH);`. `save_config()`: write `strips_` as a JSON array. `deinit()`: `strips_.clear()`.
- `apply_action()`: `off` -> `ctrl.set_power(targets(), false)`; `color` -> `ctrl.set_color(targets(), r * scale, g * scale, b * scale, 0.0)`; `brightness` -> `ctrl.set_brightness(targets(), action.brightness)`; `wled_preset` -> `for (const auto& id : targets()) if (ctrl.backend_for_strip(id) == LedBackendType::WLED) ctrl.wled().set_preset(id, action.wled_preset);`. Leave the `sync_light_state` calls for Task 11.
- Free function in `led_auto_state.cpp` (declared in the header, namespace `helix::led`):

```cpp
void stage_light_selection(const SelectionMigration& m) {
    auto* cfg = Config::get_instance();
    if (cfg == nullptr) {
        return;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& id : m.auto_state_strips) {
        arr.push_back(id);
    }
    cfg->set(cfg->df() + AUTO_STATE_STRIPS_PATH, arr);
    if (!m.light_button.empty()) {
        cfg->set(cfg->df() + LIGHT_BUTTON_PENDING_PATH, m.light_button);
    }
    auto& auto_state = LedAutoState::instance();
    if (auto_state.is_initialized()) {
        auto_state.set_strips(m.auto_state_strips);
    }
    cfg->save();
}
```

It writes the key rather than calling `LedAutoState::save_config()`, because an uninitialised `LedAutoState` would save empty mappings over the user's.

Wizard (`src/ui/ui_wizard_led_select.cpp#cleanup`): keep writing `leds/strip`; replace the `leds/selected_strips` write with `helix::led::stage_light_selection({save_value, {save_value}})` when `save_value` is non-empty (nothing staged for "None"). Its `config->save()` stays.

Hardware validator (`src/printer/hardware_validator.cpp#configured_led_strips`): read `helix::wizard::LED_AUTO_STATE_STRIPS` (new constant `"leds/auto_state/strips"` in `include/wizard_config_paths.h`) first, then the existing chain.

- [ ] **Step 4: Run green**

Run: `make -C "$T" t F='[led]'` and `make -C "$T" t F='[hardware][validator]'`
Expected: all pass.

- [ ] **Step 5: Prove the tests can fail**

Mutation: drop the `existing->is_array()` early return -> "migrates once" goes red on the second discovery. Replace `std::all_of` with an equality of sizes -> "undiscovered extra id" goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_devices.h src/led/led_devices.cpp include/led/led_controller.h src/led/led_controller.cpp include/led/led_auto_state.h src/led/led_auto_state.cpp src/ui/ui_wizard_led_select.cpp src/printer/hardware_validator.cpp include/wizard_config_paths.h src/application/application.cpp tests/unit/test_led_devices.cpp tests/unit/test_led_config.cpp tests/unit/test_led_auto_state.cpp tests/unit/test_led_controller.cpp tests/unit/test_hardware_validator.cpp
git -C "$T" commit -m "feat(led): auto-state gets its own targets and the saved selection migrates once (prestonbrown/helixscreen#1130)" -m "Mutation: removing the run-once guard turns 'migrates once at discovery' red."
```

---

### Task 5: Light-button config and LED on at Start

**Files:**
- Create: `include/light_button_config.h`, `src/ui/light_button_config.cpp`, `tests/unit/test_light_button_config.cpp`
- Modify: `src/application/application.cpp` (the `apply_startup_preference` call), `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (add `src/ui/light_button_config.cpp` in sorted position among `src/ui/`)
- Test: `tests/unit/test_led_config.cpp` (startup cases)

**Interfaces:**
- Consumes: Task 1 `union_light_targets`; Task 3 `switchable_ids()`, `chamber_light()`, `set_power()`; Task 4 `LIGHT_BUTTON_PENDING_PATH`, `apply_startup_preference(targets)`; `find_widget_def()` (`include/panel_widget_registry.h`); `PanelWidgetConfig` (`include/panel_widget_config.h`).
- Produces (namespace `helix`):
  - `std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home, const std::string& pending);`
  - `bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home);`
  - `std::vector<std::string> home_light_button_targets();`

- [ ] **Step 1: Write the failing tests** `tests/unit/test_light_button_config.cpp`

Layout JSON follows `make_layout()` in `tests/unit/test_widget_catalog_placement.cpp`: an array of `{id, enabled, col, row, colspan, rowspan, config}`. `"led"` is the only light-button id available until Task 9 makes it multi-instance.

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "light_button_config.h"

#include "../helix_test_fixture.h"
#include "config.h"
#include "led/led_devices.h"
#include "panel_widget_config.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;

namespace {

nlohmann::json entry(const std::string& id, bool placed, nlohmann::json config = nullptr) {
    nlohmann::json e = {{"id", id}, {"enabled", placed}, {"col", placed ? 0 : -1},
                        {"row", placed ? 0 : -1}, {"colspan", 2}, {"rowspan", 2}};
    if (!config.is_null()) {
        e["config"] = config;
    }
    return e;
}

PanelWidgetConfig load_panel(const std::string& panel_id, const nlohmann::json& widgets) {
    auto* cfg = Config::get_instance();
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/" + panel_id, widgets);
    PanelWidgetConfig pc(panel_id, *cfg);
    pc.load();
    return pc;
}

} // namespace

TEST_CASE_METHOD(HelixTestFixture, "home_light_button_keys: a configured button reads its key",
                 "[led][light_button]") {
    auto pc = load_panel("t_lb_keys", nlohmann::json::array(
                                           {entry("led", true, {{"led", "neopixel a"}})}));
    CHECK(home_light_button_keys(pc, "all") == std::vector<std::string>{"neopixel a"});
}

TEST_CASE_METHOD(HelixTestFixture, "home_light_button_keys: an unconfigured button reads pending",
                 "[led][light_button]") {
    auto pc = load_panel("t_lb_pending", nlohmann::json::array({entry("led", true)}));
    CHECK(home_light_button_keys(pc, "all") == std::vector<std::string>{"all"});
    CHECK(home_light_button_keys(pc, "") == std::vector<std::string>{""});
}

TEST_CASE_METHOD(HelixTestFixture, "home_light_button_keys: an unplaced button does not count",
                 "[led][light_button]") {
    auto pc = load_panel("t_lb_unplaced", nlohmann::json::array({entry("led", false)}));
    CHECK(home_light_button_keys(pc, "all").empty());
}

TEST_CASE_METHOD(HelixTestFixture, "adopt_pending_light_button: fills unset buttons once",
                 "[led][light_button]") {
    auto* cfg = Config::get_instance();
    auto pc = load_panel("t_lb_adopt", nlohmann::json::array({entry("led", true)}));
    cfg->set(cfg->df() + led::LIGHT_BUTTON_PENDING_PATH, std::string("neopixel a"));

    CHECK(adopt_pending_light_button(*cfg, pc));
    CHECK(pc.get_widget_config("led")["led"] == "neopixel a");
    CHECK(cfg->try_get_json(cfg->df() + led::LIGHT_BUTTON_PENDING_PATH)->is_null());
    CHECK_FALSE(adopt_pending_light_button(*cfg, pc));
}

TEST_CASE_METHOD(HelixTestFixture, "adopt_pending_light_button: never overwrites a choice",
                 "[led][light_button]") {
    auto* cfg = Config::get_instance();
    auto pc = load_panel("t_lb_keep", nlohmann::json::array(
                                           {entry("led", true, {{"led", "neopixel b"}})}));
    cfg->set(cfg->df() + led::LIGHT_BUTTON_PENDING_PATH, std::string("all"));
    CHECK_FALSE(adopt_pending_light_button(*cfg, pc));
    CHECK(pc.get_widget_config("led")["led"] == "neopixel b");
}
```

In `tests/unit/test_led_config.cpp`:

```cpp
TEST_CASE_METHOD(LedConfigFixture, "apply_startup_preference with no targets defers",
                 "[led][config][startup]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    clear_led_config_paths();
    ctrl.init(nullptr, nullptr);
    ctrl.set_led_on_at_start(true);
    ctrl.apply_startup_preference({});
    // Still armed: the next call with a target applies (last_brightness follows the preference).
    ctrl.set_startup_brightness(40);
    ctrl.set_last_brightness(100);
    ctrl.apply_startup_preference({"neopixel a"});
    CHECK(ctrl.last_brightness() == 40);
    ctrl.deinit();
}
```

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][light_button]'`
Expected: compile failure, `light_button_config.h: No such file or directory`.

- [ ] **Step 3: Implement**

`include/light_button_config.h`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

class Config;

namespace helix {

class PanelWidgetConfig;

/// The `led` value of every placed home light button, page by page. A button
/// with no value yet reads as @p pending.
std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home,
                                                const std::string& pending);

/// Copy leds/light_button_pending into every home light button with no `led`
/// value, then clear it. True when a button was written.
bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home);

/// What LED on at Start turns on: every device the home light buttons drive,
/// or the chamber light when there are none.
std::vector<std::string> home_light_button_targets();

} // namespace helix
```

`src/ui/light_button_config.cpp`:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "light_button_config.h"

#include "config.h"
#include "json_utils.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include <cstring>

namespace helix {

namespace {

bool is_light_button(const PanelWidgetEntry& e) {
    const PanelWidgetDef* def = find_widget_def(e.id);
    return def != nullptr && std::strcmp(def->id, "led") == 0;
}

std::string pending_value(const Config& cfg) {
    const nlohmann::json* p = cfg.try_get_json(cfg.df() + led::LIGHT_BUTTON_PENDING_PATH);
    return (p != nullptr && p->is_string()) ? p->get<std::string>() : std::string();
}

} // namespace

std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home,
                                                const std::string& pending) {
    std::vector<std::string> keys;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (!e.is_placed() || !is_light_button(e)) {
                continue;
            }
            const std::string key = json_util::safe_string(e.config, "led");
            keys.push_back(key.empty() ? pending : key);
        }
    }
    return keys;
}

bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home) {
    const std::string value = pending_value(cfg);
    if (value.empty()) {
        return false;
    }
    std::vector<std::string> ids;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (is_light_button(e) && json_util::safe_string(e.config, "led").empty()) {
                ids.push_back(e.id);
            }
        }
    }
    for (const auto& id : ids) {
        nlohmann::json c = home.get_widget_config(id);
        if (!c.is_object()) {
            c = nlohmann::json::object();
        }
        c["led"] = value;
        home.set_widget_config(id, c);
    }
    cfg.set(cfg.df() + led::LIGHT_BUTTON_PENDING_PATH, nlohmann::json());
    cfg.save();
    return !ids.empty();
}

std::vector<std::string> home_light_button_targets() {
    auto& ctrl = led::LedController::instance();
    std::vector<std::string> keys;
    if (auto* cfg = Config::get_instance()) {
        keys = home_light_button_keys(PanelWidgetManager::instance().get_widget_config("home"),
                                      pending_value(*cfg));
    }
    return led::union_light_targets(keys, ctrl.switchable_ids(), ctrl.chamber_light());
}

} // namespace helix
```

`src/application/application.cpp`: `helix::led::LedController::instance().apply_startup_preference(helix::home_light_button_targets());`

Add the manifest line.

- [ ] **Step 4: Run green**

Run: `make -C "$T" t F='[led][light_button]'`, `make -C "$T" t F='[led][config]'`, `python3 "$T"/scripts/check_esp32_app_srcs.py`
Expected: pass; checker 0.

- [ ] **Step 5: Prove the tests can fail**

Mutation: drop the `.empty()` guard in the adopt loop -> "never overwrites a choice" goes red. Drop the `is_placed()` check -> "an unplaced button does not count" goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/light_button_config.h src/ui/light_button_config.cpp tests/unit/test_light_button_config.cpp tests/unit/test_led_config.cpp src/application/application.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs.txt
git -C "$T" commit -m "feat(led): LED on at Start follows the home light buttons (prestonbrown/helixscreen#1130)" -m "Mutation: adopting over a configured button turns 'never overwrites a choice' red."
```

---

### Task 6: Settings: remove the selection row, add "Applies to"

**Files:**
- Modify: `ui_xml/led_settings_overlay.xml`, `src/ui/ui_settings_led.cpp` (`#populate_led_chips_impl`, `#handle_led_chip_clicked`, `#handle_delete_macro_device`, `#populate_auto_state_rows`), `include/ui_settings_led.h`
- Modify: `include/led/led_devices.h`, `src/led/led_devices.cpp` (`toggle_target`)
- Test: `tests/unit/test_led_devices.cpp`, create `tests/unit/test_led_settings_overlay.cpp`

**Interfaces:**
- Consumes: Task 4 `LedAutoState::strips()/set_strips()/targets()/save_config()/evaluate()`; `helix::ui::create_led_chip` (`include/ui_led_chip_factory.h`).
- Produces: `std::vector<std::string> toggle_target(const std::vector<std::string>& current, const std::string& id);` (`helix::led`): removes @p id when present unless it is the only one, appends it otherwise.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_led_devices.cpp`:

```cpp
TEST_CASE("toggle_target: add, remove, and the last one stays", "[led][settings]") {
    CHECK(toggle_target({"a"}, "b") == std::vector<std::string>{"a", "b"});
    CHECK(toggle_target({"a", "b"}, "a") == std::vector<std::string>{"b"});
    CHECK(toggle_target({"a"}, "a") == std::vector<std::string>{"a"});
    CHECK(toggle_target({}, "a") == std::vector<std::string>{"a"});
}
```

`tests/unit/test_led_settings_overlay.cpp` (XML-level, on the fixture the settings overlays' existing tests use; if none, `XMLTestFixture` from `tests/`):

```cpp
TEST_CASE_METHOD(XMLTestFixture, "LED settings: no selection row, an Applies to row",
                 "[led][settings]") {
    lv_obj_t* root = static_cast<lv_obj_t*>(
        lv_xml_create(test_screen(), "led_settings_overlay", nullptr));
    REQUIRE(root != nullptr);
    CHECK(lv_obj_find_by_name(root, "row_led_select") == nullptr);
    CHECK(lv_obj_find_by_name(root, "row_auto_state_strips") != nullptr);
}

namespace helix::settings {
class LedSettingsOverlayTestAccess {
  public:
    static void delete_macro(int index) {
        get_led_settings_overlay().handle_delete_macro_device(index);
    }
};
} // namespace helix::settings

TEST_CASE_METHOD(LVGLTestFixture, "deleting a macro device removes it from Applies to",
                 "[led][settings]") {
    auto& ctrl = helix::led::LedController::instance();
    ctrl.deinit();
    ctrl.init(nullptr, nullptr);
    helix::led::LedMacroInfo m;
    m.display_name = "Lamp";
    m.type = helix::led::MacroLedType::TOGGLE;
    m.toggle_macro = "LIGHT_TOGGLE";
    ctrl.set_configured_macros({m});
    auto& as = helix::led::LedAutoState::instance();
    as.set_strips({"macro:Lamp", "neopixel chamber_light"});

    helix::settings::LedSettingsOverlayTestAccess::delete_macro(0);

    CHECK(as.strips() == std::vector<std::string>{"neopixel chamber_light"});
    as.set_strips({});
    ctrl.deinit();
}
```

Add `friend class LedSettingsOverlayTestAccess;` to `LedSettingsOverlay` (the pattern `LedControlOverlayTestAccess` uses).

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][settings]'`
Expected: `toggle_target` undeclared; after declaring it, the XML test fails on `row_led_select` still existing.

- [ ] **Step 3: Implement**

`led_devices.cpp`:

```cpp
std::vector<std::string> toggle_target(const std::vector<std::string>& current,
                                       const std::string& id) {
    std::vector<std::string> out = current;
    const auto it = std::find(out.begin(), out.end(), id);
    if (it == out.end()) {
        out.push_back(id);
    } else if (out.size() > 1) {
        out.erase(it);
    }
    return out;
}
```

`ui_xml/led_settings_overlay.xml`: delete the whole `group_led_selection` `setting_group`. In `group_auto_state`, directly after `row_auto_state_enabled`:

```xml
<setting_led_chip_row name="row_auto_state_strips"
                      label="Applies to" label_tag="Applies to" icon="lightbulb_outline"
                      description="Lights that follow the printer's state"
                      description_tag="Lights that follow the printer's state">
  <bind_flag_if_eq subject="led_auto_state_enabled" flag="hidden" ref_value="0"/>
</setting_led_chip_row>
```

Change `row_led_on_at_start`'s description (and tag) to `Turn on the lights your light buttons control`.

`src/ui/ui_settings_led.cpp`:
- `populate_led_chips_impl()`: find `row_auto_state_strips`; chips from `LedController::all_selectable_strips()`; selected set = `LedAutoState::instance().targets()`.
- `handle_led_chip_clicked(id)`: `auto& as = LedAutoState::instance(); as.set_strips(toggle_target(as.targets(), id)); as.save_config(); as.evaluate(); populate_led_chips(); populate_auto_state_rows();`. Drop `selected_leds_` / `discovered_leds_` members if nothing else reads them.
- `handle_delete_macro_device()`: replace the `selected_strips` edit with removing `"macro:" + deleted_name` from `LedAutoState::strips()` (`set_strips` + `save_config`).
- `populate_auto_state_rows()`: compute `has_color` over `LedAutoState::targets()` instead of `selected_strips()`; offer `wled_preset` only when some target's `backend_for_strip` is WLED.

- [ ] **Step 4: Run green, and the UI check**

Run: `make -C "$T" t F='[led][settings]'`.
UI check (harness, `N=6`) at 800x480 and 480x272:

```bash
CTL navigate settings; CTL click row_hardware; CTL click row_led_settings
CTL click row_auto_state_enabled        # if off
CTL ls row_auto_state_strips            # one chip per switchable device; chamber_light checked
CTL screenshot "$SCRATCH/1130-shots/task-6/settings-$SIZE.png" --stable
```

Expected: no "LED SELECTION" group; "Applies to" row inside AUTO STATE with the chamber light selected on a fresh config.

- [ ] **Step 5: Prove the tests can fail**

Mutation: remove the `out.size() > 1` guard -> "the last one stays" goes red. Keep the old `selected_strips` edit in `handle_delete_macro_device` -> the delete test goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add ui_xml/led_settings_overlay.xml src/ui/ui_settings_led.cpp include/ui_settings_led.h include/led/led_devices.h src/led/led_devices.cpp tests/unit/test_led_devices.cpp tests/unit/test_led_settings_overlay.cpp
git -C "$T" commit -m "feat(led): Automatic LED Control picks its own lights; the light-button chip row goes (prestonbrown/helixscreen#1130)" -m "Mutation: letting the last chip deselect turns 'the last one stays' red."
```

---

### Task 7: Overlay model: focus, tabs, page state, handlers, open-on-device

**Files:**
- Modify: `include/led/ui_led_control_overlay.h`, `src/ui/ui_led_control_overlay.cpp`
- Modify: `include/helix/xml/indexed_subject_pool.h`, `src/util/indexed_subject_pool.cpp` (`Type::Color`)
- Modify: `include/led/led_devices.h`, `src/led/led_devices.cpp` (`pick_overlay_focus`)
- Test: `tests/unit/test_led_control_overlay.cpp` (rewrite), `tests/unit/test_indexed_subject_pool.cpp`, `tests/unit/test_led_devices.cpp`

This task changes C++ only. Until Task 8 lands, the old overlay XML binds subjects and callbacks this task removes; the parser drops those bindings with a warning, so the overlay draws wrong but nothing crashes. Run Task 8 directly after this one; do not merge between them.

**Interfaces:**
- Consumes: Task 2 `classify_device_page`, `white_tone`, `LEVEL_CHIPS`; Task 3 `all_devices`, `device_state`, `set_power`, `chamber_light`, `refresh_wled_state`, `get_led_state_version_subject`; `ColorPicker` (`include/ui_color_picker.h`).
- Produces:
  - `std::string pick_overlay_focus(const std::string& requested, const std::string& last_focused, const std::string& chamber, const std::vector<std::string>& devices);` (`helix::led`)
  - `IndexedSubjectPool::Type::Color` and `void IndexedSubjectPool::set_color(size_t i, uint32_t rgb);`
  - `lv_obj_t* helix::open_led_control_overlay(lv_obj_t* parent_screen, const std::string& device_id = "");`
  - `void LedControlOverlay::request_focus(const std::string& device_id);` `const std::string& LedControlOverlay::focused_device() const;`
  - XML subjects (names Task 8 binds): `led_tab_count`, `led_focused_tab`, `led_tabs_fade`, pools `led_tab_name_<i>` (string), `led_tab_dot_<i>` (int = `PowerState`), `led_tab_dot_color_<i>` (color); `led_page_lamp`, `led_page_white`, `led_page_color_vis`, `led_page_list` (ints = the Task 2 enums), `led_page_on` (int), `led_page_color` (color), `led_page_fill_text` (color), `led_page_brightness` (int), `led_page_brightness_text` (string), `led_page_white_sel` (int, -1 none), `led_swatch_count` (int), pool `led_swatch_color_<i>` (color), `led_selected_swatch` (int: index, -2 Custom, -1 none), `led_page_list_title` (string), `led_chip_count` (int), pool `led_chip_label_<i>` (string), `led_active_chip` (int: index, -1 = None chip, -2 nothing), `led_page_level` (int, 0 none), `led_page_note` (string).
  - XML callbacks (registered in `register_callbacks()`): `led_tab_clicked_cb` (user_data = index), `led_tabs_scrolled_cb`, `led_power_cb`, `led_brightness_changed_cb`, `led_level_cb` (user_data = percent), `led_white_cb` (user_data = tone 0-2), `led_swatch_cb` (user_data = index), `led_custom_color_cb`, `led_list_chip_cb` (user_data = index), `led_effects_none_cb`, `led_macro_on_cb`, `led_macro_off_cb`, `led_macro_toggle_cb`. `user_data` strings parse with `helix::text_io::parse_leading<int>`.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_led_devices.cpp`:

```cpp
TEST_CASE("pick_overlay_focus: each entry point", "[led][overlay]") {
    const std::vector<std::string> d = {"neopixel a", "neopixel chamber_light", "macro:Party"};
    CHECK(pick_overlay_focus("macro:Party", "neopixel a", "neopixel chamber_light", d) == "macro:Party");
    CHECK(pick_overlay_focus("", "neopixel a", "neopixel chamber_light", d) == "neopixel a");
    CHECK(pick_overlay_focus("", "", "neopixel chamber_light", d) == "neopixel chamber_light");
    CHECK(pick_overlay_focus("gone", "gone", "neopixel chamber_light", d) == "neopixel chamber_light");
    CHECK(pick_overlay_focus("", "", "", d) == "neopixel a");
    CHECK(pick_overlay_focus("x", "y", "z", {}).empty());
}
```

`tests/unit/test_indexed_subject_pool.cpp`:

```cpp
TEST_CASE_METHOD(HelixTestFixture, "IndexedSubjectPool: color slots", "[xml][pool]") {
    helix::xml::IndexedSubjectPool pool("t_color", helix::xml::IndexedSubjectPool::Type::Color);
    pool.ensure_size(2);
    pool.set_color(1, 0x2962FF);
    lv_subject_t* s = lv_xml_get_subject(nullptr, "t_color_1");
    REQUIRE(s == pool.at(1));
    CHECK(lv_color_to_u32(lv_subject_get_color(s)) == lv_color_to_u32(lv_color_hex(0x2962FF)));
}
```

Rewrite `tests/unit/test_led_control_overlay.cpp`. Keep `LedApplyColorFixture`, replacing its `select_strip()` with `void add_native(const std::string& id, bool color, bool white)` (adds the strip, selects nothing); replace `LedControlOverlayTestAccess` with one exposing: `activate(const std::string& requested)` (calls `request_focus` then the activation body without pushing), `tap_tab(int)`, `focused()`, `int_subject(const char* name)` (via `lv_xml_get_subject`), `tap_swatch(int)`, `tap_power()`, `drag_brightness(int)`, `tap_white(int)`, `tap_list_chip(int)`. Cases, each `[led][overlay]`:

```cpp
TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a tab tap changes focus and nothing else",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    auto* cfg = Config::get_instance();
    helix::led::LedAutoState::instance().set_strips({"neopixel sb_leds"});
    const nlohmann::json before = *cfg->try_get_json(cfg->df() + "leds");
    const auto selection_before = LedController::instance().selected_strips(); // removed in Task 11

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");
    REQUIRE(access.focused() == "neopixel chamber_light");
    access.tap_tab(1);

    CHECK(access.focused() == "neopixel sb_leds");
    CHECK(access.int_subject("led_focused_tab") == 1);
    CHECK(helix::led::LedAutoState::instance().strips() == std::vector<std::string>{"neopixel sb_leds"});
    CHECK(*cfg->try_get_json(cfg->df() + "leds") == before);
    CHECK(LedController::instance().selected_strips() == selection_before); // removed in Task 11
}
```

Further cases (write each in full in the same shape):
- "opens on the requested device" (`activate("neopixel sb_leds")` -> focused sb_leds), "opens on the last focused device" (tap tab 1, re-activate with "" -> sb_leds), "falls back to the chamber light".
- "a vanished focused device refocuses the chamber light": focus a macro, delete it via `set_configured_macros({})`, re-activate -> chamber.
- "overlay with no devices": no strips -> `led_tab_count == 0`, `focused()` empty, no crash on `tap_power()`.
- "page subjects follow the classifier": focus RGBW with an effect targeting it -> `led_page_lamp == 0`, `led_page_white == 1`, `led_page_color_vis == 1`, `led_page_list == 1`; focus a PRESET macro -> `led_page_lamp == 4`, `led_page_list == 2`, `led_chip_count == 2`, chip labels `"Party"`/`"Rainbow"` (from `pretty_print_macro`).
- "tab dots follow device state": update_from_status on sb_leds on -> `led_tab_dot_1 == 1`; macro tab -> `2`.
- "a state bump updates dots but not the slider": focus chamber (brightness 40 from status); `update_from_status` for chamber at 90 and sb_leds on -> `led_page_brightness` still 40, `led_tab_dot_1 == 1`.
- "controls act on the focused device only": two native strips, focus a; `tap_swatch(0)` -> a gets red, b gets no color; `tap_power()` -> only a commanded.
- "the white section sets W on RGBW": focus RGBW, `tap_white(1)` -> sent `w > 0`, `r == g == b == 0`.
- "a single-channel strip dims through W": focus `led case_light` whose status is `[[0,0,0,1]]`; `drag_brightness(15)` -> sent `w == 0.15`, `r == g == b == 0`.
- "the None chip stops the focused strip's effect": effect enabled on a -> `led_active_chip` = its index; `tap_effects_none` -> `stop_effect` issued for that effect only.

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][overlay]'` and `make -C "$T" t F='[xml][pool]'`
Expected: compile failures (`pick_overlay_focus`, `Type::Color`, new TestAccess members).

- [ ] **Step 3: Implement**

`pick_overlay_focus` (`led_devices.cpp`):

```cpp
std::string pick_overlay_focus(const std::string& requested, const std::string& last_focused,
                               const std::string& chamber,
                               const std::vector<std::string>& devices) {
    for (const std::string* want : {&requested, &last_focused, &chamber}) {
        if (!want->empty() && contains(devices, *want)) {
            return *want;
        }
    }
    return devices.empty() ? std::string() : devices.front();
}
```

`IndexedSubjectPool`: add `Color` to the enum; in `ensure_size` init with `lv_subject_init_color(subject.get(), lv_color_hex(0))`; `set_color(size_t i, uint32_t rgb)` asserts `type_ == Type::Color` and calls `lv_subject_set_color(subjects_.at(i).get(), lv_color_hex(rgb))`.

`LedControlOverlay` (header + cpp). Structure:
- Delete: `populate_strip_selector`, `handle_strip_selected`, `target_strips_for`, `native_target_strips`, `send_color_to_strips` fan-out, `update_section_visibility`, `populate_color_presets` (the static swatch table), `strip_selector_section_` and the other container pointers, the WLED-only brightness subjects, and every `lv_obj_add_event_cb` loop in `populate_effects`/`populate_wled`/`add_macro_chip`.
- Members: `std::string focused_strip_, last_focused_, requested_focus_; std::vector<LedStripInfo> devices_; DevicePage page_; std::vector<std::string> list_values_;` plus the scalar subjects listed in Interfaces (init in `init_subjects()` with `UI_MANAGED_SUBJECT_INT/STRING/COLOR`), and pools `tab_name_pool_{"led_tab_name", String}`, `tab_dot_pool_{"led_tab_dot", Int}`, `tab_dot_color_pool_{"led_tab_dot_color", Color}`, `swatch_color_pool_{"led_swatch_color", Color}`, `chip_label_pool_{"led_chip_label", String}`, reclaimed in `cleanup()`. `ObserverGuard state_observer_`.
- `on_activate()`: `rebuild_tabs()` (devices_ = `all_devices()`; ensure pools; set names and dots; set `led_tab_count` last); `focus_device(pick_overlay_focus(requested_focus_, last_focused_, ctrl.chamber_light(), ids))`; `requested_focus_.clear()`; `state_observer_ = observe_int_sync<LedControlOverlay>(ctrl.get_led_state_version_subject(), this, [](LedControlOverlay* s, int) { s->on_led_state_changed(); }, ctrl.get_subjects_lifetime());`; if the focused device is WLED, `ctrl.refresh_wled_state()`.
- `focus_device(id)`: `focused_strip_ = last_focused_ = id`; `led_focused_tab` = its index; `load_page_state()` (NATIVE: decompose the cached color into `current_color_/current_brightness_/current_white_`, falling back to `last_*` when off; OUTPUT_PIN / WLED: `device_state(id).brightness`); `publish_page()`.
- `publish_page()`: `page_ = classify_device_page(info, macro type, !effects_for_strip(id).empty())`; publish the four page ints; color subjects (`led_page_color` = hue of `current_color_`/white tone, or `theme_manager_get_color("light_icon_on")` for no-color devices; `led_page_fill_text` = `theme_manager_get_contrast_color(page color)`); brightness + text; `led_page_white_sel` (the tone whose `white_tone()` equals the current RGBW state, else -1); swatches from `LedController::color_presets()` (pool, then `led_swatch_count`); `led_selected_swatch`; list chips per `page_.list` (Effects: `effects_for_strip` display names, `list_values_` = effect names; WLED presets: `get_strip_presets` or numbered `lv_tr("Preset") N` fallback as today, values = ids; macro PRESET: `pretty_print_macro(preset)`, values = gcode), then `led_chip_count`, `led_active_chip`, `led_page_list_title` (`lv_tr("Effects")` / `lv_tr("Presets")`); `led_page_level`; `led_page_note` (`fmt::format(fmt::runtime(lv_tr("ON: {} | OFF: {}")), on, off)` or `lv_tr("TOGGLE: {}")`, the strings Settings already translates).
- `on_led_state_changed()`: refresh tab dots and `led_page_on` + `led_page_color` for the focused device only.
- Handlers (every one acts on `focused_strip_` and nothing else):
  - power: `ctrl.set_power({focused_strip_}, ctrl.device_state(focused_strip_).power != PowerState::On)`.
  - brightness / level: NATIVE -> `apply_current_color()`; OUTPUT_PIN -> `ctrl.output_pin().set_brightness(focused_strip_, pct)`; WLED -> `ctrl.wled().set_brightness(focused_strip_, pct)`. Level also sets `led_page_level`.
  - `apply_current_color()`: stop enabled effects in `effects_for_strip(focused_strip_)` only; `ctrl.native().set_color(focused_strip_, r * bf, g * bf, b * bf, current_white_ * bf)`.
  - white(tone): `Rgbw c = white_tone(tone, page_.white)`; `current_color_` = packed `c.r/g/b` (`to_channel_byte`), `current_white_ = c.w`; brightness 100 if it is 0; apply.
  - swatch(i): `current_color_ = color_presets()[i]; current_white_ = 0;` brightness 100 if 0; apply.
  - custom: the existing `ColorPicker` flow, routed through the swatch path.
  - list chip(i): Effects -> `activate_effect(list_values_[i])`; WLED -> `set_preset(focused_strip_, id)` then `refresh_wled_state()`; macro -> `execute_custom_action(list_values_[i])`.
  - effects none: `stop_effect` for each enabled effect of the focused strip.
  - macro on/off/toggle: `execute_on/off/toggle(strip_macro_name(focused_strip_))`.
  - tabs scrolled: `led_tabs_fade = lv_obj_get_scroll_right(target) > 0` (the target is the row, `lv_event_get_current_target_obj(e)`).
- `on_deactivating()`: reset `state_observer_`; persist `last_*` only when the focused device is NATIVE.
- `get_name()` returns `"LEDs"`.
- `open_led_control_overlay(parent, device_id)`: `overlay.request_focus(device_id)` before the existing create/register/push.

- [ ] **Step 4: Run green**

Run: `make -C "$T" t F='[led]'` and `make -C "$T" t F='[xml][pool]'`
Expected: pass.

- [ ] **Step 5: Prove the tests can fail**

Mutation: make `handle_tab_clicked` call `LedAutoState::set_strips({id})` -> "a tab tap changes focus and nothing else" goes red. Make `on_led_state_changed` call `load_page_state()` -> "a state bump updates dots but not the slider" goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/ui_led_control_overlay.h src/ui/ui_led_control_overlay.cpp include/helix/xml/indexed_subject_pool.h src/util/indexed_subject_pool.cpp include/led/led_devices.h src/led/led_devices.cpp tests/unit/test_led_control_overlay.cpp tests/unit/test_indexed_subject_pool.cpp tests/unit/test_led_devices.cpp
git -C "$T" commit -m "feat(led): the LEDs overlay focuses one device and never touches a selection (prestonbrown/helixscreen#1130)" -m "Mutation: a tab tap writing auto-state strips turns 'changes focus and nothing else' red."
```

---

### Task 8: Overlay view: tabs, two-column page, 480x272

**Files:**
- Rewrite: `ui_xml/led_control_overlay.xml`
- Create: `ui_xml/components/led_device_tab.xml`, `ui_xml/led_list_chip.xml`
- Modify: `ui_xml/led_color_swatch.xml`, `ui_xml/led_action_chip.xml`, `src/xml_registration.cpp` (register the new files beside `led_action_chip.xml`, the tab as `components/led_device_tab.xml`, before `led_control_overlay.xml`)
- Modify (helix-xml, our repo, commit inside the submodule): `lib/helix-xml/src/xml/lv_xml_base_types.c#lv_xml_style_prop_to_enum`, `lib/helix-xml/src/xml/lv_xml_style.c`, `lib/helix-xml/src/xml/parsers/lv_xml_obj_parser.c` (add `bg_main_opa`, `bg_grad_opa` beside `bg_main_stop`, parsed with `lv_xml_to_opa`)
- Modify: `include/ui_icon_codepoints.h` + `scripts/regen_mdi_fonts.sh` `MDI_ICONS` (add `dots_circle` = F1978 `dots-circle`), then `make -C "$T" regen-fonts` and commit every file it rewrites
- Modify: `src/application/application.cpp` (demo `"leds"` -> `helix::open_led_control_overlay(lv_screen_active())`), `scripts/screenshot-recipes.sh` (`leds               demo leds`)
- Test: `tests/unit/test_led_control_overlay.cpp` (XML structure cases)

**Interfaces:**
- Consumes: every Task 7 subject and callback name, verbatim.
- Produces: named widgets `led_tab_row`, `led_tab_<i>` (each tab), `led_tab_fade`, `led_power_btn`, `led_brightness_slider`, `led_white_<cool|neutral|warm>`, `led_swatch_list`, `led_swatch_<i>`, `led_custom_swatch`, `led_chip_row`, `led_chip_none`, `led_chip_<i>`, `led_level_<10|25|50|75|100>`, `led_macro_on`, `led_macro_off`, `led_macro_toggle`, `led_page_note_label`, `led_empty_state`. Task 9/10 checks click these by name.

- [ ] **Step 1: Write the failing test**

```cpp
namespace {
struct OverlayXmlFixture : public LedApplyColorFixture {
    helix::PrinterState ps;
    lv_obj_t* root = nullptr;

    OverlayXmlFixture() {
        add_native("neopixel chamber_light", true, true);
        auto& ctrl = LedController::instance();
        LedEffectInfo glow;
        glow.name = "led_effect glow";
        glow.display_name = "Glow";
        glow.target_leds = {"neopixel chamber_light"};
        ctrl.effects().add_effect(glow);
        init_led_control_overlay(ps);
        root = helix::open_led_control_overlay(test_screen(), "neopixel chamber_light");
        REQUIRE(root != nullptr);
    }
    ~OverlayXmlFixture() override {
        NavigationManager::instance().go_back();
    }
};
} // namespace

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: every named control exists", "[led][overlay][xml]") {
    for (const char* name : {"led_tab_row", "led_tab_0", "led_tab_fade", "led_power_btn",
                             "led_brightness_slider", "led_white_neutral", "led_swatch_0",
                             "led_custom_swatch", "led_chip_none", "led_chip_0", "led_level_50",
                             "led_macro_on", "led_macro_toggle", "led_page_note_label",
                             "led_empty_state"}) {
        INFO(name);
        CHECK(lv_obj_find_by_name(root, name) != nullptr);
    }
    CHECK(lv_obj_find_by_name(root, "strip_selector_section") == nullptr);
}

TEST_CASE_METHOD(OverlayXmlFixture, "overlay XML: the slider fill takes the page color",
                 "[led][overlay][xml]") {
    lv_obj_t* slider = lv_obj_find_by_name(root, "led_brightness_slider");
    lv_subject_set_color(lv_xml_get_subject(nullptr, "led_page_color"), lv_color_hex(0xFF4444));
    CHECK(lv_color_to_u32(lv_obj_get_style_bg_color(slider, LV_PART_INDICATOR)) ==
          lv_color_to_u32(lv_color_hex(0xFF4444)));
}
```

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][overlay][xml]'`
Expected: `led_tab_row` not found.

- [ ] **Step 3: Implement**

helix-xml: in each of the three files add the two properties next to `bg_main_stop` (`else SET_STYLE_IF(bg_main_opa, lv_xml_to_opa(value));`, same for `bg_grad_opa`; in `lv_xml_style_prop_to_enum` map `"bg_main_opa"` -> `LV_STYLE_BG_MAIN_OPA`, `"bg_grad_opa"` -> `LV_STYLE_BG_GRAD_OPA`). Commit in `lib/helix-xml`, push it (ask Preston before the push if the session has no standing OK), commit the bumped pointer here. Read `docs/devel/HELIX_XML_FORK.md` first.

`ui_xml/components/led_device_tab.xml`:

```xml
<component>
  <api>
    <prop name="index" type="string" default="0"/>
  </api>
  <styles>
    <style name="tab_focused" bg_color="#primary" bg_opa="40" border_color="#primary" border_width="#border_width"/>
    <style name="dot_hollow" bg_opa="0" border_width="2" border_color="#text_muted"/>
  </styles>
  <view name="led_device_tab" extends="ui_button" height="#led_tab_height" width="content"
        variant="ghost" flex_flow="row" style_flex_cross_place="center" style_pad_gap="#space_xs"
        style_pad_left="#space_sm" style_pad_right="#space_sm" style_radius="#led_tab_height" focusable="false">
    <bind_style name="tab_focused" subject="led_focused_tab" ref_value="$index"/>
    <lv_obj name="dot_on" width="#led_dot_size" height="#led_dot_size" style_radius="9999" style_bg_opa="255"
            style_border_width="0" clickable="false" event_bubble="true">
      <bind_style_prop prop="bg_color" subject="led_tab_dot_color_${index}"/>
      <bind_flag_if_not_eq subject="led_tab_dot_${index}" flag="hidden" ref_value="1"/>
    </lv_obj>
    <lv_obj name="dot_off" width="#led_dot_size" height="#led_dot_size" style_radius="9999"
            clickable="false" event_bubble="true">
      <style name="dot_hollow"/>
      <bind_flag_if_not_eq subject="led_tab_dot_${index}" flag="hidden" ref_value="0"/>
    </lv_obj>
    <icon name="dot_unknown" src="dots_circle" size="xs" variant="muted" clickable="false" event_bubble="true">
      <bind_flag_if_not_eq subject="led_tab_dot_${index}" flag="hidden" ref_value="2"/>
    </icon>
    <text_small bind_text="led_tab_name_${index}" clickable="false" event_bubble="true"/>
    <event_cb trigger="clicked" callback="led_tab_clicked_cb" user_data="$index"/>
  </view>
</component>
```

`ui_xml/led_control_overlay.xml` skeleton (tokens with `_micro/_small/_medium/_large` variants in this file, which is in `ui_xml/` root so the suffix discovery sees them):

```xml
<component>
  <px name="led_tab_height_micro" value="28"/> <px name="led_tab_height_small" value="32"/>
  <px name="led_tab_height_medium" value="40"/> <px name="led_tab_height_large" value="48"/>
  <px name="led_dot_size" value="12"/>
  <px name="led_swatch_size_micro" value="26"/> <px name="led_swatch_size_small" value="32"/>
  <px name="led_swatch_size_medium" value="40"/> <px name="led_swatch_size_large" value="48"/>
  <px name="led_power_size_micro" value="36"/> <px name="led_power_size_small" value="44"/>
  <px name="led_power_size_medium" value="56"/> <px name="led_power_size_large" value="64"/>
  <px name="led_slider_width_micro" value="44"/> <px name="led_slider_width_small" value="52"/>
  <px name="led_slider_width_medium" value="64"/> <px name="led_slider_width_large" value="72"/>
  <gradients>
    <conical name="led_rainbow" center="50% 50%" angle="0 360">
      <stop color="#FF0000" offset="0"/> <stop color="#FFFF00" offset="42"/>
      <stop color="#00FF00" offset="85"/> <stop color="#00FFFF" offset="128"/>
      <stop color="#0000FF" offset="170"/> <stop color="#FF00FF" offset="213"/>
      <stop color="#FF0000" offset="255"/>
    </conical>
  </gradients>
  <styles>
    <style name="power_on" bg_opa="255" border_width="0"/>
    <style name="power_off" bg_opa="0" border_width="2" border_color="#text_muted"/>
    <style name="swatch_ring" outline_width="2" outline_pad="2" outline_color="#primary"/>
    <style name="chip_active" bg_color="#primary" text_color="#screen_bg"/>
    <style name="lamp_wide" flex_grow="1"/>
  </styles>
  <view name="led_control_overlay" extends="overlay_panel" title="LEDs" title_tag="LEDs" bg_color="#screen_bg">
    <lv_obj name="content_area" width="100%" flex_grow="1" flex_flow="column" scrollable="false"
            style_pad_all="#space_md" style_pad_top="0" style_pad_gap="#space_sm">
      <!-- Tabs: sideways row, fade on the right while it overflows -->
      <lv_obj name="led_tab_strip" width="100%" height="content" style_pad_all="0" scrollable="false">
        <bind_flag_if_eq subject="led_tab_count" flag="hidden" ref_value="0"/>
        <lv_obj name="led_tab_row" width="100%" height="content" flex_flow="row" scrollable="true"
                scroll_dir="hor" scrollbar_mode="off" style_pad_all="0" style_pad_gap="#space_xs">
          <event_cb trigger="scroll" callback="led_tabs_scrolled_cb"/>
          <event_cb trigger="size_changed" callback="led_tabs_scrolled_cb"/>
          <repeat count="led_tab_count">
            <led_device_tab name="led_tab_${i}" index="${i}"/>
          </repeat>
        </lv_obj>
        <lv_obj name="led_tab_fade" floating="true" align="right_mid" width="#space_xxl" height="100%"
                clickable="false" style_border_width="0" style_radius="0" style_bg_opa="255"
                style_bg_color="#screen_bg" style_bg_grad_color="#screen_bg" style_bg_grad_dir="hor"
                style_bg_main_opa="0" style_bg_grad_opa="255">
          <bind_flag_if_eq subject="led_tabs_fade" flag="hidden" ref_value="0"/>
        </lv_obj>
      </lv_obj>

      <!-- Device page: lamp column | look column -->
      <lv_obj name="led_page" width="100%" flex_grow="1" flex_flow="row" scrollable="false"
              style_pad_all="0" style_pad_gap="#space_md">
        <bind_flag_if_eq subject="led_tab_count" flag="hidden" ref_value="0"/>
        <lv_obj name="led_lamp_col" width="content" height="100%" flex_flow="column" scrollable="false"
                style_pad_all="0" style_pad_gap="#space_sm" style_flex_cross_place="center"
                style_flex_main_place="center">
          <bind_flag_if_eq subject="led_page_lamp" flag="hidden" ref_value="4"/>
          <bind_style name="lamp_wide" subject="led_page_lamp" ref_value="1"/>
          <lv_obj name="led_power_btn" width="#led_power_size" height="#led_power_size" style_radius="9999"
                  clickable="true" scrollable="false" style_flex_main_place="center" style_flex_cross_place="center">
            <bind_flag_if cond="led_page_lamp gt 1" flag="hidden"/>
            <bind_style name="power_on" subject="led_page_on" ref_value="1"/>
            <bind_style name="power_off" subject="led_page_on" ref_value="0"/>
            <bind_style_prop prop="bg_color" subject="led_page_color"/>
            <icon src="power" size="sm" clickable="false" event_bubble="true" align="center">
              <bind_style_prop prop="text_color" subject="led_page_fill_text"/>
            </icon>
            <event_cb trigger="clicked" callback="led_power_cb"/>
          </lv_obj>
          <lv_slider name="led_brightness_slider" width="#led_slider_width" flex_grow="1" min_value="0"
                     max_value="100" bind_value="led_page_brightness" style_radius="#border_radius"
                     style_radius:indicator="#border_radius" style_bg_opa:knob="0" style_pad_all:knob="0">
            <bind_flag_if_not_eq subject="led_page_lamp" flag="hidden" ref_value="0"/>
            <bind_style_prop prop="bg_color" selector="indicator" subject="led_page_color"/>
            <text_small name="led_brightness_pct" bind_text="led_page_brightness_text" align="bottom_mid"
                        style_translate_y="-4" clickable="false" event_bubble="true">
              <bind_style_prop prop="text_color" subject="led_page_fill_text"/>
            </text_small>
            <event_cb trigger="value_changed" callback="led_brightness_changed_cb"/>
          </lv_slider>
          <led_action_chip name="led_macro_on" label="On" label_tag="On">
            <bind_flag_if_not_eq subject="led_page_lamp" flag="hidden" ref_value="2"/>
            <event_cb trigger="clicked" callback="led_macro_on_cb"/>
          </led_action_chip>
          <led_action_chip name="led_macro_off" label="Off" label_tag="Off">
            <bind_flag_if_not_eq subject="led_page_lamp" flag="hidden" ref_value="2"/>
            <event_cb trigger="clicked" callback="led_macro_off_cb"/>
          </led_action_chip>
          <led_action_chip name="led_macro_toggle" label="Toggle" label_tag="Toggle">
            <bind_flag_if_not_eq subject="led_page_lamp" flag="hidden" ref_value="3"/>
            <event_cb trigger="clicked" callback="led_macro_toggle_cb"/>
          </led_action_chip>
        </lv_obj>

        <lv_obj name="led_look_col" flex_grow="1" height="100%" flex_flow="column" scrollable="false"
                style_pad_all="0" style_pad_gap="#space_sm">
          <bind_flag_if_eq subject="led_page_lamp" flag="hidden" ref_value="1"/>
          <!-- White -->
          <lv_obj name="led_white_section" width="100%" height="content" flex_flow="column" style_pad_all="0"
                  style_pad_gap="#space_xs" scrollable="false">
            <bind_flag_if_eq subject="led_page_white" flag="hidden" ref_value="0"/>
            <text_small text="White" translation_tag="White" style_text_color="#text_muted"/>
            <lv_obj width="100%" height="content" flex_flow="row" style_pad_all="0" style_pad_gap="#space_xs" scrollable="false">
              <led_action_chip name="led_white_cool" label="Cool" label_tag="Cool" active_subject="led_page_white_sel" active_value="0">
                <event_cb trigger="clicked" callback="led_white_cb" user_data="0"/>
              </led_action_chip>
              <led_action_chip name="led_white_neutral" label="Neutral" label_tag="Neutral" active_subject="led_page_white_sel" active_value="1">
                <event_cb trigger="clicked" callback="led_white_cb" user_data="1"/>
              </led_action_chip>
              <led_action_chip name="led_white_warm" label="Warm" label_tag="Warm" active_subject="led_page_white_sel" active_value="2">
                <event_cb trigger="clicked" callback="led_white_cb" user_data="2"/>
              </led_action_chip>
            </lv_obj>
          </lv_obj>
          <!-- Color -->
          <lv_obj name="led_color_section" width="100%" height="content" flex_flow="column" style_pad_all="0"
                  style_pad_gap="#space_xs" scrollable="false">
            <bind_flag_if_eq subject="led_page_color_vis" flag="hidden" ref_value="0"/>
            <text_small text="Color" translation_tag="Color" style_text_color="#text_muted"/>
            <lv_obj width="100%" height="content" flex_flow="row" style_pad_all="0" style_pad_gap="#space_xs" scrollable="false">
              <lv_obj name="led_swatch_list" width="content" height="content" flex_flow="row" style_pad_all="0"
                      style_pad_gap="#space_xs" scrollable="false">
                <repeat count="led_swatch_count">
                  <led_color_swatch name="led_swatch_${i}" index="${i}"/>
                </repeat>
              </lv_obj>
              <lv_obj name="led_custom_swatch" width="#led_swatch_size" height="#led_swatch_size" style_radius="9999"
                      style_bg_opa="255" style_bg_grad="led_rainbow" clickable="true" scrollable="false">
                <bind_style name="swatch_ring" subject="led_selected_swatch" ref_value="-2"/>
                <event_cb trigger="clicked" callback="led_custom_color_cb"/>
              </lv_obj>
            </lv_obj>
          </lv_obj>
          <!-- Effects / Presets / Level chips -->
          <lv_obj name="led_list_section" width="100%" height="content" flex_flow="column" style_pad_all="0"
                  style_pad_gap="#space_xs" scrollable="false">
            <bind_flag_if_eq subject="led_page_list" flag="hidden" ref_value="0"/>
            <text_small bind_text="led_page_list_title" style_text_color="#text_muted">
              <bind_flag_if_eq subject="led_page_list" flag="hidden" ref_value="3"/>
            </text_small>
            <lv_obj name="led_chip_scroller" width="100%" height="content" flex_flow="row" scrollable="true"
                    scroll_dir="hor" scrollbar_mode="off" style_pad_all="0" style_pad_gap="#space_xs">
              <bind_flag_if_eq subject="led_page_list" flag="hidden" ref_value="3"/>
              <led_action_chip name="led_chip_none" label="None" label_tag="None" active_subject="led_active_chip" active_value="-1">
                <bind_flag_if_not_eq subject="led_page_list" flag="hidden" ref_value="1"/>
                <event_cb trigger="clicked" callback="led_effects_none_cb"/>
              </led_action_chip>
              <lv_obj name="led_chip_row" width="content" height="content" flex_flow="row" style_pad_all="0"
                      style_pad_gap="#space_xs" scrollable="false">
                <repeat count="led_chip_count">
                  <led_list_chip name="led_chip_${i}" index="${i}"/>
                </repeat>
              </lv_obj>
            </lv_obj>
            <lv_obj name="led_level_row" width="100%" height="content" flex_flow="row" style_pad_all="0"
                    style_pad_gap="#space_xs" scrollable="false">
              <bind_flag_if_not_eq subject="led_page_list" flag="hidden" ref_value="3"/>
              <!-- one per LEVEL_CHIPS value -->
              <led_action_chip name="led_level_10" label="10%" active_subject="led_page_level" active_value="10">
                <event_cb trigger="clicked" callback="led_level_cb" user_data="10"/>
              </led_action_chip>
              <led_action_chip name="led_level_25" label="25%" active_subject="led_page_level" active_value="25">
                <event_cb trigger="clicked" callback="led_level_cb" user_data="25"/>
              </led_action_chip>
              <led_action_chip name="led_level_50" label="50%" active_subject="led_page_level" active_value="50">
                <event_cb trigger="clicked" callback="led_level_cb" user_data="50"/>
              </led_action_chip>
              <led_action_chip name="led_level_75" label="75%" active_subject="led_page_level" active_value="75">
                <event_cb trigger="clicked" callback="led_level_cb" user_data="75"/>
              </led_action_chip>
              <led_action_chip name="led_level_100" label="100%" active_subject="led_page_level" active_value="100">
                <event_cb trigger="clicked" callback="led_level_cb" user_data="100"/>
              </led_action_chip>
            </lv_obj>
          </lv_obj>
          <text_small name="led_page_note_label" bind_text="led_page_note" style_text_color="#text_muted" long_mode="wrap" width="100%">
            <bind_flag_if cond="led_page_lamp lt 2 or led_page_lamp gt 3" flag="hidden"/>
          </text_small>
        </lv_obj>
      </lv_obj>

      <lv_obj name="led_empty_state" width="100%" flex_grow="1" flex_flow="column" style_flex_main_place="center"
              style_flex_cross_place="center" scrollable="false" style_pad_all="0">
        <bind_flag_if_not_eq subject="led_tab_count" flag="hidden" ref_value="0"/>
        <text_body text="No LEDs found" translation_tag="No LEDs found"/>
      </lv_obj>
    </lv_obj>
  </view>
</component>
```

The PRESET row puts `led_look_col` alone on the row, so it takes the full width. Before trusting `<bind_style name="tab_focused" ... ref_value="$index">`, confirm with `ctl` that the focused tab really changes; if the bind resolves `$index` once and correctly, keep it, otherwise use `ref_value="${index}"`.

`ui_xml/led_color_swatch.xml`: round (`style_radius="9999"`), size `#led_swatch_size`, prop `index`; `<bind_style_prop prop="bg_color" subject="led_swatch_color_${index}"/>`, `<bind_style name="swatch_ring" subject="led_selected_swatch" ref_value="$index"/>` (move `swatch_ring` into this component's `<styles>`, or into `ui_xml/styles.xml` and borrow it as `styles.swatch_ring` from both files), `<event_cb trigger="clicked" callback="led_swatch_cb" user_data="$index"/>`.

`ui_xml/led_action_chip.xml`: add props `label_tag`, `active_subject` (default ""), `active_value` (default "0"); `translation_tag="$label_tag"`; `<bind_style name="chip_active" subject="$active_subject" ref_value="$active_value"/>` (empty subject installs no binding).

`ui_xml/led_list_chip.xml`: like `led_action_chip` but `bind_text="led_chip_label_${index}"`, active on `led_active_chip == $index`, `<event_cb trigger="clicked" callback="led_list_chip_cb" user_data="$index"/>`.

Icon: add `{"dots_circle", "\xF3\xB1\xA5\xB8"}, // F1978 dots-circle` to `include/ui_icon_codepoints.h` and `dots-circle` to `MDI_ICONS` in `scripts/regen_mdi_fonts.sh`, then `make -C "$T" regen-fonts`; `scripts/validate_icon_fonts.sh` must pass. Run `python3 "$T"/scripts/esp32_check_asset_staleness.py` and follow what it says.

- [ ] **Step 4: Run green, and the UI check**

Run: `make -C "$T" t F='[led]'`, `python3 "$T"/scripts/check_imperative_ui.py --summary` (count at or below the branch's starting count).

UI check (harness, `N=8`, seeded macros) at both sizes:

```bash
CTL demo leds
CTL geom led_tab_row; CTL geom led_page          # page bottom <= content_area bottom, no vertical scroll
for i in 0 1 2 3 4 5 6 7 8; do
  CTL click "led_tab_$i" || break
  CTL screenshot "$SCRATCH/1130-shots/task-8/tab$i-$SIZE.png" --stable
done
CTL text led_page_brightness_text
```

Expected, per tab: chamber_light = power + slider + White + Color + Effects with None; status_led = same with Mixed-free RGBW detection as the mock reports; caselight = slider + Level chips; Enclosure_LEDs = slider + Level chips; printer_led = slider + Presets; Lamp Macro = On/Off + note; Toggle Macro = Toggle + note; Party = Presets full width. At 480x272 `ctl geom` shows every visible control inside `content_area` and the tab row fade visible. Compare each against the mockups with Preston before closing the task.

- [ ] **Step 5: Prove the tests can fail**

Mutation: drop `<bind_style_prop ... selector="indicator">` -> "the slider fill takes the page color" goes red. Rename `led_tab_row` -> "every named control exists" goes red. Revert.

- [ ] **Step 6: Commit** (two commits: helix-xml pointer bump first, then the rest)

```bash
git -C "$T" add lib/helix-xml
git -C "$T" commit -m "chore(xml): bump helix-xml for bg_main_opa and bg_grad_opa (prestonbrown/helixscreen#1130)"
git -C "$T" add ui_xml/led_control_overlay.xml ui_xml/components/led_device_tab.xml ui_xml/led_list_chip.xml ui_xml/led_color_swatch.xml ui_xml/led_action_chip.xml src/xml_registration.cpp include/ui_icon_codepoints.h scripts/regen_mdi_fonts.sh assets/fonts/mdi_icons_*.c src/application/application.cpp scripts/screenshot-recipes.sh tests/unit/test_led_control_overlay.cpp
git -C "$T" commit -m "feat(led): the LEDs overlay page, tabs with state dots and a two-column device page (prestonbrown/helixscreen#1130)" -m "Mutation: dropping the indicator color binding turns 'the slider fill takes the page color' red."
```

---

### Task 9: Home light buttons, per instance

**Files:**
- Modify: `src/ui/panel_widgets/led_widget.h`, `src/ui/panel_widgets/led_widget.cpp`
- Rewrite: `ui_xml/components/panel_widget_led.xml`
- Create: `ui_xml/led_picker.xml` (registered in `src/xml_registration.cpp` beside `fan_picker.xml`)
- Modify: `src/ui/panel_widget_registry.cpp` (`"led"` row: `multi_instance` true), `ui_xml/controls_panel.xml` + `ui_xml/micro/controls_panel.xml` (`light_button` gets `<event_cb trigger="clicked" callback="light_toggle_cb"/>`), `src/ui/ui_panel_controls.cpp` (construct `LedWidget("controls_led", ...)`), `ui_xml/components/home_action_tile.xml` (its header comment counts seven users and names led; make it true)
- Test: create `tests/unit/test_led_widget.cpp`, extend `tests/unit/test_light_button_config.cpp`

**Interfaces:**
- Consumes: Task 3 `light_targets`, `toggle_power`, `device_state`, `chamber_light`, `all_selectable_strips`, `get_led_state_version_subject`; Task 5 `adopt_pending_light_button`; Task 7 `open_led_control_overlay(parent, device_id)`; `ContextMenu::active_as<T>()` (`include/ui_context_menu.h`); `helix::xml::IndexedSubjectPool`.
- Produces:
  - `LedWidget(const std::string& instance_id, PrinterState&, IMoonrakerAPI*)`; `set_config(json)`, `has_edit_configure()` true, `on_edit_configure()`, `void select_light(const std::string& key)`, `std::vector<std::string> targets() const`, `std::string overlay_device() const`, `const std::string& light_key() const`.
  - `bool light_tile_is_wide(int colspan);` (free, `led_widget.h`): `colspan >= 2 * GridLayout::TRACKS_PER_CELL`.
  - `std::vector<led::LedStripInfo> light_picker_devices();` (free, `led_widget.h`): the picker's device rows, `LedController::all_selectable_strips()`. The "All lights" row is fixed XML after them.
  - XML callbacks `light_toggle_cb`, `light_more_cb`, `led_picker_row_cb` (user_data = row index, `"-1"` = All lights).
  - Per-instance subjects `<id>_led_name` (string) and `<id>_led_wide` (int), passed to XML as `name_subject` / `wide_subject`.

- [ ] **Step 1: Write the failing tests** `tests/unit/test_led_widget.cpp`

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "grid_layout.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/led_widget.h"

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

namespace {
struct LedWidgetFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    std::unique_ptr<MoonrakerAPIMock> api;

    LedWidgetFixture() {
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, ps);
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = LedBackendType::NATIVE;
            s.supports_color = true;
            s.supports_white = true;
            ctrl.native().add_strip(s);
        }
        LedMacroInfo lamp;
        lamp.display_name = "Lamp";
        lamp.type = MacroLedType::TOGGLE;
        lamp.toggle_macro = "LIGHT_TOGGLE";
        LedMacroInfo party;
        party.display_name = "Party";
        party.type = MacroLedType::PRESET;
        party.presets = {"LED_PARTY"};
        ctrl.set_configured_macros({lamp, party});
        ctrl.rebuild_macro_backend();
    }
    ~LedWidgetFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        LedController::instance().deinit();
    }
};
} // namespace

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: config round trip", "[led][light_button]") {
    LedWidget w("led:1", ps, api.get());
    w.set_config({{"led", "neopixel sb_leds"}});
    CHECK(w.light_key() == "neopixel sb_leds");
    CHECK(w.targets() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: unset means the chamber light", "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: a vanished device resolves to the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "neopixel gone"}});
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: All lights toggles every switchable device",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "all"}});
    const auto t = w.targets();
    CHECK(t == LedController::instance().switchable_ids());
    CHECK(std::find(t.begin(), t.end(), "macro:Party") == t.end());
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the picker offers no PRESET device",
                 "[led][light_button]") {
    std::vector<std::string> ids;
    for (const auto& d : light_picker_devices()) {
        ids.push_back(d.id);
    }
    CHECK(std::find(ids.begin(), ids.end(), "macro:Party") == ids.end());
    CHECK(ids == LedController::instance().switchable_ids());
}

TEST_CASE("light_tile_is_wide: two cells or more", "[led][light_button]") {
    CHECK_FALSE(light_tile_is_wide(helix::GridLayout::TRACKS_PER_CELL));
    CHECK_FALSE(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL - 1));
    CHECK(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL));
}
```

In `tests/unit/test_light_button_config.cpp` add a two-instance case (`"led"` and `"led:1"` placed, one configured, one not) now that `led` is multi-instance: `home_light_button_keys` returns both, adoption writes only the unconfigured one.

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][light_button]'`
Expected: compile failure on the three-argument `LedWidget` constructor.

- [ ] **Step 3: Implement**

`LedWidget`:
- Constructor takes `instance_id` first; `sizing_{instance_id, TileSizing::Content{"", "", "Light", false}}`; register `name_subject_` (`UI_MANAGED_SUBJECT_STRING`, 64-byte buffer, name `instance_id + "_led_name"`) and `wide_subject_` (`instance_id + "_led_wide"`) in the constructor; `xml_attrs()` returns an owned list = `sizing_.subject_attrs()` pairs + `name_subject`, `wide_subject`.
- `id()` returns `instance_id_.c_str()`.
- `set_config()`: `led_key_ = json_util::safe_string(config, "led")`.
- `targets()`: `LedController::instance().light_targets(led_key_)`. `overlay_device()`: `led_key_ == LIGHT_BUTTON_ALL ? chamber_light() : first of targets()` (empty when none).
- `bind_led()` (runs from `attach()` and on `led_config_version`): when `led_key_` is empty and this widget has a panel, `adopt_pending_light_button(*Config::get_instance(), PanelWidgetManager::instance().get_widget_config("home"))` then re-read `led_key_` from `get_widget_config("home").get_widget_config(id())`; publish the name (`lv_tr("All lights")`, or the device's `LedStripInfo::name`, or empty when nothing resolves); `sizing_.set_content({"", "", name, false}); relayout_for_granted_size();`; observe `get_led_state_version_subject()` -> `update_light_icon()`. It no longer calls `PrinterState::set_tracked_led`.
- `update_light_icon()`: composite over `targets()` from `device_state`: on if any target is On; brightness = max over the on ones; color = the first on target with `has_rgb`, else `theme_manager_get_color("light_icon_on")`; all Unknown -> muted. Keep the existing icon glyph and color calls (pre-existing site; do not add new ones).
- `handle_light_toggle()`: in-flight guard and toast unchanged; `LedController::instance().toggle_power(targets())`; when every target is Unknown, `flash_light_icon()`.
- `on_size_changed(colspan, ...)`: existing `sizing_.measure_and_publish(...)` plus `lv_subject_set_int(&wide_subject_, light_tile_is_wide(colspan) ? 1 : 0)`.
- Callbacks: remove the `lv_obj_add_event_cb(light_btn, ...)` in `attach()`. `light_toggle_cb` / `light_more_cb` resolve the widget by walking ancestors from `lv_event_get_current_target(e)` and accepting a `user_data` pointer only when it is in `LedWidget::live_instances()` (the `recover_widget_from_event` pattern in `src/ui/panel_widgets/print_status_widget.cpp`); `attach()` inserts `this`, `detach()` erases it. `light_more_cb` -> `open_led_control_overlay(parent_screen_, overlay_device())`.
- Picker: `class LedPicker : public helix::ui::ContextMenu` (`HELIX_CONTEXT_MENU_KIND(LedPicker)`, `xml_component_name()` -> `"led_picker"`, card width like `FanPicker`). Its subjects are process-wide because only one picker is open at a time: a function-static struct in `led_widget.cpp` holding `IndexedSubjectPool names{"led_picker_name", String}`, `lv_subject_t count` (`"led_picker_count"`), `lv_subject_t selected` (`"led_picker_selected"`), initialised on first use and torn down through `StaticSubjectRegistry::instance().register_deinit("LedPicker", ...)`. `show_led_picker()` fills them from `light_picker_devices()` (names), sets `selected` to the configured device's index or `-1` for All lights, sets `count` last, then `picker_.show_below_widget(...)`. `led_picker_row_cb`: `auto* p = ContextMenu::active_as<LedPicker>()`; index -> `select_light(id)` or `LIGHT_BUTTON_ALL` for -1; `p->hide()`.
- `select_light(key)`: `led_key_ = key; save_widget_config({{"led", key}}); bind_led();`.

`ui_xml/led_picker.xml`:

```xml
<component>
  <view name="led_picker" extends="context_menu_backdrop">
    <context_menu_card title="This button controls" title_tag="This button controls" pad_gap="#space_xs">
      <lv_obj name="led_picker_list" width="100%" height="content" flex_flow="column" style_pad_all="#space_xs"
              style_pad_gap="0" scrollable="true">
        <repeat count="led_picker_count">
          <ui_button name="led_picker_row_${i}" width="100%" height="#button_height_sm" variant="ghost"
                     bind_text="led_picker_name_${i}" focusable="false">
            <bind_style name="styles.selected_row" subject="led_picker_selected" ref_value="$i"/>
            <event_cb trigger="clicked" callback="led_picker_row_cb" user_data="$i"/>
          </ui_button>
        </repeat>
      </lv_obj>
      <ui_button name="led_picker_all" width="100%" height="#button_height_sm" variant="ghost"
                 text="All lights" translation_tag="All lights" focusable="false">
        <bind_style name="styles.selected_row" subject="led_picker_selected" ref_value="-1"/>
        <event_cb trigger="clicked" callback="led_picker_row_cb" user_data="-1"/>
      </ui_button>
    </context_menu_card>
  </view>
</component>
```

If `styles.selected_row` does not exist in `ui_xml/styles.xml`, add it there (a `bg_color="#primary" bg_opa="40"` highlight) rather than inline. Cap the list height the way `FanPicker::on_created` does, with `// DECLARATIVE_OK: measured cap` if it stays in C++.

`ui_xml/components/panel_widget_led.xml`: keep the four tile props, add `name_subject` and `wide_subject`; root `flex_flow="row"`; a `ui_button name="light_button"` (flex_grow 1, ghost, column, centred) holding `icon name="light_icon"` with the five `tile_icon_*` rung bindings and `text_tiny name="light_name" bind_text="$name_subject"` hidden by `show_widget_labels` / `$tile_label_subject`, `bind_state_if cond="printer_connection_state ne 2 or led_command_in_flight eq 1" state="disabled"`, `<event_cb trigger="clicked" callback="light_toggle_cb"/>`; then `ui_button name="light_more_button"` (content width, ghost) with `icon src="chevron_right"`, hidden when `$wide_subject` is 0, `<event_cb trigger="clicked" callback="light_more_cb"/>`.

Registry: set `multi_instance` true on the `"led"` row. Register `light_more_cb` and `led_picker_row_cb` in `register_led_widget()`; the factory passes the id through.

- [ ] **Step 4: Run green, and the UI check**

Run: `make -C "$T" t F='[led][light_button]'`, then `make -C "$T" t F='[panel_widget]'` (registry and catalog tests that count multi-instance widgets).

UI check (harness, `N=9`). The default home has one light tile at one cell. Stop the app, set that entry's `colspan` to `4` in `$HELIX_CONFIG_DIR/settings.json` (`printers.<id>.panel_widgets.home`, the entry whose `id` is `led`), relaunch:

```bash
CTL navigate home
CTL geom light_more_button          # visible, right edge of the tile
CTL text light_name                 # "Chamber Light"
CTL click light_button; CTL screenshot "$SCRATCH/1130-shots/task-9/tile-on-$SIZE.png" --stable
CTL click light_more_button; CTL current   # LEDs overlay, focused tab = chamber_light
```

Also in edit mode, open the gear on the light tile: the picker lists every switchable device and All lights, and no "Party".

- [ ] **Step 5: Prove the tests can fail**

Mutation: make `light_picker_devices()` return `all_devices()` -> "the picker offers no PRESET device" goes red. Make `targets()` ignore `LIGHT_BUTTON_ALL` -> "All lights toggles every switchable device" goes red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add src/ui/panel_widgets/led_widget.h src/ui/panel_widgets/led_widget.cpp ui_xml/components/panel_widget_led.xml ui_xml/led_picker.xml src/xml_registration.cpp src/ui/panel_widget_registry.cpp ui_xml/controls_panel.xml ui_xml/micro/controls_panel.xml src/ui/ui_panel_controls.cpp ui_xml/components/home_action_tile.xml tests/unit/test_led_widget.cpp tests/unit/test_light_button_config.cpp
git -C "$T" commit -m "feat(led): each home light button controls the light it is set to (prestonbrown/helixscreen#1130)" -m "Mutation: listing every device in the picker turns 'offers no PRESET device' red."
```

---

### Task 10: Entry points and the other light buttons

**Files:**
- Modify: `src/ui/panel_widgets/printer_image_widget.cpp` (`#arm_callout_observers`, `#update_callouts`, `#handle_callout_clicked`)
- Modify: `src/printer/printer_discovery.cpp` (tracked LED = the chamber light when it is a Klipper object)
- Modify: `src/ui/ui_print_light_timelapse.cpp#handle_light_button`, `src/system/settings_manager.cpp#set_led_enabled`
- Modify: `src/ui/panel_widgets/led_controls_widget.cpp#handle_clicked` (explicit `""`, no behaviour change)
- Test: `tests/unit/test_led_entry_points.cpp` (create), `tests/unit/test_printer_image_callouts.cpp` (its light-chip expectations move from the tracked LED to the chamber light)

**Interfaces:**
- Consumes: Task 3 `chamber_light`, `device_state`, `set_power`, `toggle_power`, `get_led_state_version_subject`; Task 7 `open_led_control_overlay(parent, device_id)`, `LedControlOverlay::focused_device()`.
- Produces: `bool chamber_light_on();` (free, `helix::led`, in `led_controller.h`): `device_state(chamber_light()).power == PowerState::On`.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_led_entry_points.cpp`, on a fixture identical to Task 9's `LedWidgetFixture` (native `neopixel chamber_light` and `neopixel sb_leds` on a mock API), named `EntryFixture`:

```cpp
TEST_CASE_METHOD(EntryFixture, "light chip follows the chamber light", "[led][entry]") {
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK_FALSE(helix::led::chamber_light_on());
    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{1.0, 1.0, 1.0, 0.0}}}}}});
    CHECK(helix::led::chamber_light_on());
}

TEST_CASE_METHOD(EntryFixture, "the LED controls tile opens on the last focused device",
                 "[led][entry]") {
    init_led_control_overlay(ps);
    REQUIRE(helix::open_led_control_overlay(test_screen(), "neopixel sb_leds") != nullptr);
    NavigationManager::instance().go_back();
    REQUIRE(helix::open_led_control_overlay(test_screen()) != nullptr); // what the tile calls
    CHECK(get_led_control_overlay().focused_device() == "neopixel sb_leds");
    NavigationManager::instance().go_back();
}

TEST_CASE_METHOD(EntryFixture, "the print-status light toggles only the chamber light",
                 "[led][entry]") {
    PrintLightTimelapseControls controls;
    controls.handle_light_button();
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    auto& ctrl = LedController::instance();
    CHECK(ctrl.native().has_strip_color("neopixel chamber_light"));
    CHECK_FALSE(ctrl.native().has_strip_color("neopixel sb_leds"));
}
```

The printer-image chip's click routing (`handle_callout_clicked(Light)` passing `chamber_light()`) is one line behind a private member; the UI check below proves it rather than a new test hook.

- [ ] **Step 2: Run red**

Run: `make -C "$T" t F='[led][entry]'`
Expected: `chamber_light_on` undeclared.

- [ ] **Step 3: Implement**

- `chamber_light_on()` in `led_controller.cpp`.
- `printer_image_widget.cpp`: in `update_callouts()`, `light_shown = read_int_or_zero(printer_has_led_subject()) && helix::led::chamber_light_on() ? 1 : 0`; in `arm_callout_observers()` replace `ps.get_led_state_subject()` in the observed list with an observer on `LedController::instance().get_led_state_version_subject()` (lifetime `LedController::instance().get_subjects_lifetime()`); `handle_callout_clicked(Light)` -> `open_led_control_overlay(parent_screen_, LedController::instance().chamber_light())`.
- `printer_discovery.cpp`: `tracked = led_ctrl.chamber_light()` when `backend_for_strip(tracked)` is NATIVE or OUTPUT_PIN, else `""`.
- `ui_print_light_timelapse.cpp#handle_light_button`: `LedController::instance().toggle_power({LedController::instance().chamber_light()})`.
- `settings_manager.cpp#set_led_enabled`: `auto& c = LedController::instance(); c.set_power(c.light_targets(""), enabled);`.
- `led_controls_widget.cpp#handle_clicked`: `open_led_control_overlay(parent_screen_, "")`.

- [ ] **Step 4: Run green, and the UI check**

Run: `make -C "$T" t F='[led]'` and `make -C "$T" t F='[printer_image]'` (covers `tests/unit/test_printer_image_callouts.cpp`).
UI check (harness, `N=10`): home, `CTL click light_button` (chamber on) -> `CTL geom callout_chip_light` visible; `CTL click callout_chip_light` -> overlay focused on the chamber tab; screenshot both sizes.

- [ ] **Step 5: Prove the tests can fail**

Mutation: keep `ps.get_led_state_subject()` in `light_shown` -> "light chip follows the chamber light" goes red when only sb_leds is on. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_controller.h src/led/led_controller.cpp src/ui/panel_widgets/printer_image_widget.cpp src/printer/printer_discovery.cpp src/ui/ui_print_light_timelapse.cpp src/system/settings_manager.cpp src/ui/panel_widgets/led_controls_widget.cpp tests/unit/test_led_entry_points.cpp tests/unit/test_printer_image_callouts.cpp
git -C "$T" commit -m "feat(led): the light chip and every fixed light button follow the chamber light (prestonbrown/helixscreen#1130)" -m "Mutation: the chip reading the tracked LED turns 'follows the chamber light' red."
```

---

### Task 11: Delete the selection

**Files:**
- Modify: `include/led/led_controller.h`, `src/led/led_controller.cpp`, `src/led/led_auto_state.cpp`
- Test: `tests/unit/test_led_controller.cpp`, `tests/unit/test_led_config.cpp`, `tests/unit/test_led_auto_state.cpp`, `tests/unit/test_led_native_backend.cpp`, `tests/unit/test_config_null_pollution.cpp`, `tests/unit/test_config_isolation.cpp`, `tests/unit/test_led_control_overlay.cpp`

**Interfaces:**
- Consumes: everything above; no remaining caller of the functions below.
- Produces: removal of `selected_strips()`, `set_selected_strips()`, `light_set(bool)`, `light_toggle()`, `light_is_on()`, `sync_light_state()`, `light_state_trackable()`, `status_tracked_strip()`, `turn_off_all()`, `set_color_all()`, `set_brightness_all()`, `toggle_all()`, `light_on_`. `selected_strips_` becomes `legacy_selection_`: read by `load_config()`, consumed by `migrate_legacy_selection()`, never saved. `first_available_strip()` loses its "prefer selected" clause.

- [ ] **Step 1: Make the deletion the failing test**

Delete the declarations from `led_controller.h` first. `make -C "$T" test` now fails to compile at every remaining caller; that list is the work. Expected callers left: the tests listed above, `LedAutoState::apply_action`'s `sync_light_state` calls, `save_config`'s `selected_strips` write, the prune block in `discover_from_hardware`.

- [ ] **Step 2: Migrate each test to the targeted API**

Mechanical rewrite, one rule: `ctrl.set_selected_strips(X); ctrl.light_set(on);` -> `ctrl.set_power(X, on);`; `set_color_all` / `set_brightness_all` / `turn_off_all` -> `set_color(X, ...)` / `set_brightness(X, ...)` / `set_power(X, false)`; `light_is_on()` checks -> `device_state(id).power` checks after feeding a status frame, or `toggle_power` return values for macros. Tests that only asserted selection bookkeeping (`set_selected_strips bumps version`, `selected_strips can hold WLED strip IDs`, `first_available_strip priority order`'s "prefer selected" leg) are deleted with the behaviour; say which in the commit body. Replace the overlay test's `selected_strips()` check with nothing (the property is now unrepresentable).

- [ ] **Step 3: Remove the implementation**

Delete the bodies; drop `sync_light_state` calls from `LedAutoState::apply_action`; stop writing `leds/selected_strips` in `save_config()`; delete the prune block (nothing is selected to prune). Keep reading the three legacy keys in `load_config()` into `legacy_selection_`.

- [ ] **Step 4: Run green**

Run: `make -C "$T" t F='[led]'`, then `make -C "$T" unit-sweep`, then `grep -rn "selected_strips\|light_set\|set_color_all\|set_brightness_all\|turn_off_all\|toggle_all" "$T"/src "$T"/include` (only `legacy_selection_` plumbing and the wizard/validator key constants remain).
Expected: all green; grep shows no call sites.

- [ ] **Step 5: Prove the tests can fail**

Mutation: reintroduce a fan-out in `set_power` (iterate `switchable_ids()`) -> the migrated "set_power reaches only the ids it is given" and "controls act on the focused device only" go red. Revert.

- [ ] **Step 6: Commit**

```bash
git -C "$T" add include/led/led_controller.h src/led/led_controller.cpp src/led/led_auto_state.cpp tests/unit/test_led_controller.cpp tests/unit/test_led_config.cpp tests/unit/test_led_auto_state.cpp tests/unit/test_led_native_backend.cpp tests/unit/test_config_null_pollution.cpp tests/unit/test_config_isolation.cpp tests/unit/test_led_control_overlay.cpp
git -C "$T" commit -m "refactor(led): delete the LED selection and every call that fanned out over it (prestonbrown/helixscreen#1130)" -m "Mutation: set_power fanning out over switchable_ids() turns the focused-device tests red."
```

---

### Task 12: Translations, docs, changelog, scaffolding

**Files:**
- Modify: `translations/*.yml` (all 8 non-English locales), regenerated translation outputs
- Modify: `docs/devel/LED_CONTROL.md`, `docs/user/guide/settings/led-settings.md`, `docs/user/guide/home-panel.md`, `docs/user/USER_GUIDE.md`, `docs/user/CONFIGURATION.md`, `docs/devel/CHANGELOG_1_1_DRAFT.md`
- Delete: `docs/devel/plans/2026-09-27-led-controls-redesign-design.md`, `docs/devel/plans/2026-09-27-led-controls-redesign-plan.md`

- [ ] **Step 1: Sync and fill every locale**

Run: `make -C "$T" translation-sync-dry-run`, then `make -C "$T" translation-sync`. New keys expected: `This button controls`, `All lights`, `White`, `Cool`, `Neutral`, `Warm`, `Applies to`, `Lights that follow the printer's state`, `Turn on the lights your light buttons control`, `No LEDs found`, `On`, `Off`, `None`, `LEDs` (whichever do not already exist). Fill de, es, fr, it, ja, pt, ru, zh by hand, consistent with `translations/GLOSSARY.md`; no empty placeholders (`grep -n ": ''$\|: \"\"$" translations/*.yml` for the new keys is empty). Run `scripts/check_cjk_font_staleness.sh` and regenerate the CJK fonts if it says so. Delete keys the sync reports obsolete for strings this branch removed (`LEDs controlled by the light button`, `LED SELECTION`, `Turn on LEDs when printer starts`).

- [ ] **Step 2: Docs**

- `docs/devel/LED_CONTROL.md`: rewrite "Config Persistence" (no `selected_strips`; `leds/auto_state/strips`, `leds/light_button_pending`, widget `led`), "UI Components" (tabs, page table from `classify_device_page`, focus rules), "Home Panel Widget Integration" (per-instance config, picker, 2x1 zone, adoption), the chamber-light resolver, and the tests table (`test_led_devices.cpp`, `test_led_device_page.cpp`, `test_led_device_state.cpp`, `test_light_button_config.cpp`, `test_led_widget.cpp`, `test_led_entry_points.cpp`). Cite code as `path#symbol`.
- User docs: the LEDs overlay, what the light button controls and how to change it, Automatic LED Control's "Applies to", and what LED on at Start turns on.
- `CHANGELOG_1_1_DRAFT.md` under Added (or Changed, matching the file's sections), one entry:

```markdown
- **Every light gets the same controls, and each light button picks its own light (#1130)** - the
  LED screen, now called LEDs, has a tab per light with a dot showing whether it is on and in
  what color. Each tab shows only what that light can do: power and brightness, white tones and
  colors, effects or presets, or On and Off for macro lights. Home light buttons each control one
  light or All lights, chosen from the gear in edit mode; a wide button has a › that opens that
  light's tab. Automatic LED Control has its own "Applies to" list. If you had picked some but not
  all of your lights for the light button, that choice now drives Automatic LED Control and your
  light buttons start out on the chamber light; set them from the gear.
```

- [ ] **Step 3: Delete the scaffolding**

Plain `rm`, not `git rm`; Step 5 stages the deletions by path: `rm "$T"/docs/devel/plans/2026-09-27-led-controls-redesign-design.md "$T"/docs/devel/plans/2026-09-27-led-controls-redesign-plan.md`.

- [ ] **Step 4: The completion gate**

Run: `make -C "$T" full-test-run`, `python3 "$T"/scripts/check_esp32_app_srcs.py`, `python3 "$T"/scripts/check_imperative_ui.py --summary`, `make -C "$T" check-doc-anchors`.
Expected: all green; imperative count at or below the branch's base.

- [ ] **Step 5: Commit**

```bash
git -C "$T" add translations docs/devel/LED_CONTROL.md docs/user docs/devel/CHANGELOG_1_1_DRAFT.md docs/devel/plans/2026-09-27-led-controls-redesign-design.md docs/devel/plans/2026-09-27-led-controls-redesign-plan.md
# plus, by explicit path, every generated file `translation-sync` and any CJK font regeneration rewrote (read `git -C "$T" status --short`)
git -C "$T" commit -m "docs(led): LEDs overlay and per-device light buttons, translated in every locale (prestonbrown/helixscreen#1130)"
```

---

## Pre-flight conflict table

Every pair of tasks that shares a file or an interface. Tasks run in numeric order; none of these pairs may run in parallel.

| Tasks | Shared file / interface | Order constraint |
|---|---|---|
| 1, 3, 4, 6, 7 | `include/led/led_devices.h`, `src/led/led_devices.cpp`, `tests/unit/test_led_devices.cpp` | 1 -> 3 -> 4 -> 6 -> 7 (each appends) |
| 1, 2, 5 | `firmware/.../app_srcs.txt` | 1 -> 2 -> 5 |
| 1 -> 3, 5, 9 | `resolve_light_targets`, `union_light_targets`, `LIGHT_BUTTON_ALL` | 1 first |
| 2 -> 7, 8 | `DevicePage` enum values = page subject integers the XML compares against | 2 first; 7 and 8 must agree on 0-4 / 0-2 / 0-3 |
| 3, 4, 10, 11 | `include/led/led_controller.h`, `src/led/led_controller.cpp` | 3 -> 4 -> 10 -> 11 |
| 3 -> 4-11 | `set_power/set_color/set_brightness(ids)`, `device_state`, `get_led_state_version_subject`, `chamber_light`, `light_targets`, `switchable_ids`, `all_devices` | 3 first |
| 3, 10 | `src/printer/printer_state.cpp` (3) and `src/printer/printer_discovery.cpp` (10) both route LED status | 3 -> 10 |
| 4, 11 | `include/led/led_auto_state.h`, `src/led/led_auto_state.cpp` | 4 -> 11 |
| 4 -> 5, 6 | `LedAutoState::strips/set_strips/targets`, `stage_light_selection`, `LIGHT_BUTTON_PENDING_PATH`, `apply_startup_preference(targets)` | 4 first |
| 4, 5, 8 | `src/application/application.cpp` (startup call in 4 and 5; demo in 8) | 4 -> 5 -> 8 |
| 4, 11 | `tests/unit/test_led_controller.cpp`, `test_led_config.cpp`, `test_led_auto_state.cpp` | 4 -> 11 |
| 5, 9 | `tests/unit/test_light_button_config.cpp`; `adopt_pending_light_button` | 5 -> 9 |
| 6 | `src/ui/ui_settings_led.cpp`, `ui_xml/led_settings_overlay.xml` | alone |
| 7, 8, 11 | `tests/unit/test_led_control_overlay.cpp` | 7 -> 8 -> 11 |
| 7 -> 8 | every `led_*` subject and callback name; `IndexedSubjectPool::Type::Color` | 7 then 8 back to back, no merge between |
| 7 -> 9, 10 | `open_led_control_overlay(parent, device_id)`, `focused_device()` | 7 first |
| 8, 9 | `src/xml_registration.cpp` | 8 -> 9 |
| 8 -> 9 | `dots_circle` icon, `led_action_chip` props | 8 first |
| 9, 10 | `src/ui/panel_widgets/` (`led_widget.*` in 9, `led_controls_widget.cpp` and `printer_image_widget.cpp` in 10) and the "light buttons act on the chamber light" rule | 9 -> 10 |
| 11, 12 | `docs/devel/LED_CONTROL.md` describes the API 11 leaves | 11 -> 12 |
