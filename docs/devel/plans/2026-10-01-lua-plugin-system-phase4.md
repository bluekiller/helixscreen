# Lua Plugin System Phase 4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the plugin system end to end with a second, permissioned example plugin (`led-effects`, ported from the deleted C++ demo) and write the Lua plugin author guide.

**Architecture:** `led-effects` is a 1x1 home tile that toggles one klipper-led_effect effect with `helix.gcode` and reads the effect's real state through `helix.moonraker.subscribe`; it is the gcode-permission counterpart to the zero-permission `temp-spark`. Two small enablers come first: the `--test` mock delivers synthetic status updates to method-callback registrants (as the live WebSocket path does) and models `led_effect` state, and the plugin XML allowlist admits the app's `icon` and `text_*` semantic widgets so plugin tiles can look native. `docs/devel/PLUGIN_DEVELOPMENT.md` then documents the system as it exists, with both examples as worked references.

**Tech Stack:** C++17, LVGL 9.5 + helix-xml, Lua 5.4.9 (compiled as C++), Catch2, Markdown.

**Spec:** `docs/devel/plans/2026-09-28-lua-plugin-system-design.md` (§ Phases item 4, § Testing "Acceptance").

## Global Constraints

- A plugin id matches `^[a-z][a-z0-9-]{1,31}$`; every component, subject, object name and event a plugin registers is `<id>__<rest>` (`kPluginNameSeparator`, `include/plugin_manifest.h`).
- Plugin XML passes `check_plugin_xml` (`src/plugin/plugin_xml_policy.cpp`): the only callback is `plugin_event`, subjects and object `name=`s are the plugin's own, and a plugin whose id contains a hyphen cannot use `cond` (use `bind_flag_if_eq` and friends instead).
- Document and use only the Lua API that exists in `src/plugin/lua_bind_*.cpp`. Read the current code, not memory: confirm dialogs carry their own captions (`fff1708de`), and plugin overlays sit on `OverlayBase::show` (`743eed883`).
- An example plugin uses each HelixScreen API it needs once, in the shortest honest form (the `temp-spark` rule).
- Code comments state the code as it is: no history, no review or mutation narration, no em-dashes. Doc citations are `path#symbol`, never line numbers.
- Plugin tests are wrapped in `#if HELIX_HAS_PLUGINS`; plugin sources stay out of the ESP32 image.
- Commits: `git commit -m "..." -- <paths>` with explicit paths (a new file: `git add -N <path>` first); never `git add -A`. Every behaviour commit names one hand mutation that turned its test red, in one line of the body.
- Per task: `make t F='<tags>'` only. `make full-test-run`, zeus mutate and zeus ASAN run once, in the final task, by the controller.

## Review Focus

1. A settings string that reaches a G-code line: an effect name containing a space, newline or `;` must never produce a second G-code command. Expected: the plugin refuses any name outside `[A-Za-z0-9_]`, sends nothing, and says so. Pinned in Task 3 ("an unsafe effect name sends nothing").
2. Changing the effect setting while a subscription is live: the old object must leave the plugin's object set and a late delta for the old effect must not flip the tile. Pinned in Task 3 ("switching effects follows only the new one").
3. A refused or failed G-code (unknown effect, Klipper not ready): the tile must keep showing Klipper's state, not a guessed one, and the plugin must stay loaded. Pinned in Task 3 ("a failed toggle keeps Klipper's state").
4. The mock's new method-callback fan-out must deliver each synthetic update exactly once to each registrant, so no existing mock-driven consumer sees duplicates. Pinned in Task 1 ("a synthetic status update reaches each registrant once").
5. Admitting `icon` and `text_*` must not open a subject, callback or object-name path the policy does not check. Pinned in Task 2 ("app widgets keep the name rules").

---

### Task 1: The mock models led_effect state and reaches method callbacks

**Files:**
- Modify: `include/moonraker_client_mock.h`, `src/api/moonraker_client_mock.cpp`, `src/api/moonraker_client_mock_objects.cpp`
- Test: the existing mock behaviour tests that already drive LED G-code (find with `grep -ln 'SET_LED_EFFECT\|led_states_\|dispatch_method_callback' tests/unit/*.cpp`; `tests/unit/test_moonraker_mock_behavior.cpp` is the likely home), tag `[mock][led_effect]`

**Interfaces:**
- Produces: `MoonrakerClientMock::dispatch_status_update(const json& status, bool from_cached_snapshot)` override; `SET_LED_EFFECT EFFECT=<name> STOP=1` support; `printer.objects.query` answers `led_effect <name>.enabled` from mock state.
- Consumes: `MoonrakerClient::dispatch_status_update` (base), `MoonrakerClientMock::dispatch_method_callback(method, msg)` (`src/api/moonraker_client_mock.cpp#dispatch_method_callback`).

Today, every synthetic status change the mock makes (`SET_LED_EFFECT`, replay events, print simulation) goes through the base `MoonrakerClient::dispatch_status_update`, which fans out to `notify_callbacks_` only. The live WebSocket path (`src/api/moonraker_client.cpp`, the `notify_status_update` branch of the message handler) delivers to `notify_callbacks_` AND to `method_callbacks_["notify_status_update"]`. Plugin subscriptions register as method callbacks, so under `--test` a plugin never sees what a G-code did. The mock's periodic temperature loop already calls `dispatch_method_callback` itself; that loop does not go through `dispatch_status_update`, so it is unaffected.

Ruling recorded in preflight: the fix is mock-only. Production `MoonrakerClient::dispatch_status_update` is on the WebSocket critical path and its synthetic dispatches (cached snapshot replay, the subscription refresh response) do not need to reach plugin subscriptions, which take their first values from their own query.

Also, the mock answers every `printer.objects.query` for a `led_effect` object with `enabled: false` (`src/api/moonraker_client_mock_objects.cpp`, the "LED effect objects" loop), so a plugin that re-queries after a toggle reads the wrong state; and `SET_LED_EFFECT ... STOP=1` (klipper-led_effect's way to stop one effect) currently ENABLES the effect.

- [ ] **Step 1: Failing tests**

Add to the mock behaviour test file, using the file's existing fixture for a connected mock and its existing way of sending G-code (follow the nearest `SET_LED` test):

```cpp
TEST_CASE("a synthetic status update reaches each registrant once", "[mock][led_effect]") {
    // A notify callback and a method callback for notify_status_update both
    // registered on the mock; one dispatch_status_update({"led_effect rainbow":
    // {"enabled": true}}) must call each exactly once, and the method callback's
    // message must be {"method": "notify_status_update", "params": [status, 0.0]}.
}

TEST_CASE("SET_LED_EFFECT STOP=1 stops only that effect", "[mock][led_effect]") {
    // SET_LED_EFFECT EFFECT=rainbow, then SET_LED_EFFECT EFFECT=breathing, then
    // SET_LED_EFFECT EFFECT=rainbow STOP=1: the last status update carries
    // "led_effect rainbow".enabled == false and does not carry
    // "led_effect breathing".enabled == false.
}

TEST_CASE("a query reports which effects are running", "[mock][led_effect]") {
    // After SET_LED_EFFECT EFFECT=rainbow, printer.objects.query for
    // {"led_effect rainbow": null, "led_effect breathing": null} returns
    // rainbow.enabled == true and breathing.enabled == false.
    // After STOP_LED_EFFECTS, rainbow.enabled == false.
}
```

Write each body with real calls and REQUIREs; the comments above are the required assertions, not placeholders for vaguer ones.

- [ ] **Step 2: Run to verify they fail:** `make t F='[led_effect]'`. Expected: all three fail (method callback count 0; rainbow stays enabled after STOP=1; query says false).

- [ ] **Step 3: Implement**

1. `include/moonraker_client_mock.h`: declare `void dispatch_status_update(const json& status, bool from_cached_snapshot = false) override;` next to `dispatch_method_callback`, with a one-line comment: the live WebSocket path delivers `notify_status_update` to method-callback registrants too, so synthetic updates must reach them.
2. `src/api/moonraker_client_mock.cpp`: define it as the base call, then `dispatch_method_callback("notify_status_update", msg)` where `msg` is `{"method": "notify_status_update", "params": [status, 0.0]}`, plus `CACHED_SNAPSHOT_MARKER: true` when `from_cached_snapshot` (match the base's wrapping exactly; read `src/api/moonraker_client.cpp#dispatch_status_update`).
3. Add `std::set<std::string> enabled_led_effects_;` guarded by the existing `led_mutex_`. In the `SET_LED_EFFECT` handler, parse an optional `STOP=1` token: with it, erase the effect and dispatch only `{"led_effect <name>": {"enabled": false}}`; without it, keep today's behaviour (enable the named effect, disable the others, update `color_data`) and record the set as exactly `{name}` (today's handler is exclusive; keep that). `STOP_LED_EFFECTS` clears the set.
4. `src/api/moonraker_client_mock_objects.cpp`: the `led_effect` query loop reports `enabled` from `enabled_led_effects_` under `led_mutex_`.

- [ ] **Step 4: Run to verify they pass:** `make t F='[led_effect]'`, then `make t F='[mock]'`, `make t F='[led]'` and `make t F='[plugin]'` (the fan-out must not change any existing mock-driven test).

- [ ] **Step 5: Hand mutations:** (a) drop the `dispatch_method_callback` line: "reaches each registrant once" goes red; (b) ignore `STOP=1`: "stops only that effect" goes red. Restore both.

- [ ] **Step 6: Commit:** `git commit -m "fix(mock): led_effect state survives queries, STOP=1 stops one effect, and synthetic status updates reach method callbacks" -- <paths>`

---

### Task 2: Plugin XML may use the app's icon and text widgets

**Files:**
- Modify: `src/plugin/plugin_xml_policy.cpp` (`is_allowlisted_app_component` and the comment above it)
- Test: `tests/unit/test_plugin_xml_policy.cpp` (`[plugin][xml_policy]`)

**Interfaces:**
- Produces: plugin XML may use `icon`, `text_heading`, `text_body`, `text_muted`, `text_small`, `text_xs`, `text_tiny` (all registered in `src/ui/ui_icon.cpp` and `src/ui/ui_text.cpp`; `text_tiny` is an alias of `text_xs`).
- Consumes: nothing new.

These widgets take only presentational attributes (`icon`: `src`, `size`, `variant`, `color`; text widgets: base label attributes plus `transform`). Their subject-taking attributes are the generic `bind_*` ones, which `check_plugin_attr` already classifies by name, and `name=` is already checked for ownership on every element. So the change is the allowlist plus tests that pin the name rules still apply on these elements. `text_button` and `text_input` stay out: they carry interaction the plugin event model does not route.

- [ ] **Step 1: Failing tests** in `tests/unit/test_plugin_xml_policy.cpp`:

```cpp
TEST_CASE("plugin XML may use the app's icon and text widgets", "[plugin][xml_policy]") {
    for (const char* el : {"icon", "text_heading", "text_body", "text_muted", "text_small",
                           "text_xs", "text_tiny"}) {
        std::string xml = std::string("<component><view extends=\"lv_obj\"><") + el +
                          " name=\"demo__x\"/></view></component>";
        CHECK(check_plugin_xml("demo", {"demo__w"}, xml) == std::string());
    }
    CHECK(check_plugin_xml("demo", {"demo__w"},
                           "<component><view extends=\"lv_obj\"><text_button/></view></component>") !=
          std::string());
}

TEST_CASE("app widgets keep the name rules", "[plugin][xml_policy]") {
    // An unowned object name, an unowned bind_text subject, and a foreign callback are
    // each rejected on icon and on text_body exactly as on lv_label.
    const char* bad[] = {
        "<icon name=\"other__x\"/>",
        "<text_body bind_text=\"extruder_temp\"/>",
        "<icon><event_cb trigger=\"clicked\" callback=\"settings_open\"/></icon>",
    };
    for (const char* inner : bad) {
        std::string xml = std::string("<component><view extends=\"lv_obj\">") + inner +
                          "</view></component>";
        CHECK(check_plugin_xml("demo", {"demo__w"}, xml) != std::string());
    }
}
```

The file wraps fragments with its `view_with(...)` helper (`check_plugin_xml("ab", {}, view_with(R"(<lv_label .../>)"))`); use it instead of hand-built `<component><view>` strings, keeping the assertions above.

- [ ] **Step 2: Run to verify they fail:** `make t F='[xml_policy]'`. Expected: the first case fails for every element (not on the allowlist); the second passes already (rejected as an unknown element), which is fine: it pins the rules once the allowlist grows.

- [ ] **Step 3: Implement:** extend `is_allowlisted_app_component` to the seven names. Rewrite the comment above it as a present-tense statement of what belongs on the list, e.g. `// App widgets a plugin's own components may build on: chrome and presentational widgets whose only name-taking attributes are the generic bind_* and name= ones checked below.`

- [ ] **Step 4: Run to verify they pass:** `make t F='[xml_policy]'`, `make t F='[plugin]'`.

- [ ] **Step 5: Hand mutation:** remove `icon` from the list: the first case goes red. Restore.

- [ ] **Step 6: Commit:** `git commit -m "feat(plugin): plugin XML may use the app's icon and text widgets" -- <paths>`

---

### Task 3: The led-effects example plugin

**Files:**
- Create: `examples/plugins/led-effects/manifest.json`, `examples/plugins/led-effects/main.lua`, `examples/plugins/led-effects/ui/led-effects__tile.xml`, `examples/plugins/led-effects/README.md`
- Test: `tests/unit/test_example_plugin_led_effects.cpp` (`[plugin][example]`); check every other user of `examples/plugins` (`grep -rn 'examples/plugins' tests scripts mk Makefile`) still passes with a second plugin directory present.

**Interfaces:**
- Consumes: Task 1 (mock fan-out and led_effect state, for the live check), Task 2 (`icon`, `text_body`, `text_small` in plugin XML), existing `HostRig`/`FakeBackend` (`tests/test_helpers/plugin_host_test_support.h`, `tests/test_helpers/plugin_test_support.h`), `PluginHost::runtime(id)`, `PluginHost::set_setting`, `PluginHost::dispatch_event`, `dispatch_widget_hook(LuaRuntime&, const std::string&, WidgetHook, const LuaRuntime::PushFn&)` (`include/lua_bindings.h`).
- Produces: subjects `led-effects__effect` (string), `led-effects__state` (string, "On"/"Off"), `led-effects__active` (int 0/1), `led-effects__wide` (int 0/1); event `led-effects__toggle`; widget `led-effects__tile` (1x1, grows to 2x1).

The deleted C++ demo (`git show 7459f687d^:plugins/led-effects/src/led_effects_plugin.cpp`) toggled a hard-coded `chamber_light` with `SET_LED` and only logged on print events. The port keeps the idea (a home tile that toggles LEDs through G-code) and fixes what never worked: the effect is a setting instead of a hard-coded name, and the tile shows Klipper's real state instead of a local guess. Dropped, recorded in the README: plain `SET_LED` light toggling (the built-in LED widget does that) and print-event reactions (the plugin API has no print events; `helix.printer.watch("print_state", fn)` is the documented route).

- [ ] **Step 1: Write the plugin**

`examples/plugins/led-effects/manifest.json`:

```json
{
  "id": "led-effects",
  "name": "LED Effects",
  "version": "1.0.0",
  "author": "HelixScreen",
  "description": "Turn one klipper-led_effect effect on or off from a home tile that shows its live state.",
  "permissions": ["gcode"],
  "widgets": [
    {
      "id": "led-effects__tile",
      "name": "LED Effect",
      "icon": "lightbulb_outline",
      "description": "Toggle an LED effect",
      "component": "led-effects__tile",
      "colspan": 1,
      "rowspan": 1,
      "max_colspan": 2,
      "max_rowspan": 1
    }
  ],
  "settings": [
    {"key": "effect", "type": "string", "label": "Effect name", "default": "rainbow"}
  ]
}
```

`examples/plugins/led-effects/main.lua`:

```lua
-- SPDX-License-Identifier: GPL-3.0-or-later
-- led-effects: a home tile that turns one klipper-led_effect effect on or off.
-- The permissioned companion to temp-spark: it sends G-code, so enabling it
-- asks for the gcode permission, and it shows the effect's state as Klipper
-- reports it through a live subscription rather than guessing.

-- Subjects the tile binds. A name given here registers as led-effects__<name>.
local effect_label = helix.subject.string("effect", "--")
local state_text = helix.subject.string("state", "Off")
local active = helix.subject.int("active", 0)
local wide = helix.subject.int("wide", 0)

local sub = nil -- the live subscription for the selected effect

-- The name goes into a G-code line, so only the characters Klipper section
-- names use are accepted: anything else could append a second command.
local function effect_name()
    local name = helix.settings.get("effect")
    if type(name) == "string" and name:match("^[%w_]+$") then
        return name
    end
    return nil
end

local function show(on)
    active:set(on and 1 or 0)
    state_text:set(on and "On" or "Off")
end

-- The first callback carries the queried state and every later one a change,
-- so the tile always shows what Klipper says, including changes made elsewhere.
local function follow()
    if sub then
        sub:cancel()
        sub = nil
    end
    local name = effect_name()
    effect_label:set(name or "Invalid name")
    show(false)
    if not name then
        return
    end
    local object = "led_effect " .. name
    sub = helix.moonraker.subscribe({[object] = {"enabled"}}, function(status)
        local s = status[object]
        if s and s.enabled ~= nil then
            show(s.enabled)
        end
    end)
end

-- The tile's tap. STOP=1 stops only this effect, leaving any other running.
-- The tile does not flip on its own: the subscription reports the result.
helix.ui.on("toggle", function()
    local name = effect_name()
    if not name then
        helix.ui.toast("Effect names may use only letters, digits and _", "warning")
        return
    end
    local line = "SET_LED_EFFECT EFFECT=" .. name
    if active:get() == 1 then
        line = line .. " STOP=1"
    end
    local ok, err = helix.gcode(line)
    if not ok then
        helix.ui.toast("LED effect failed: " .. tostring(err), "error")
    end
end)

-- At two cells wide the tile also names the effect.
helix.widget("tile", {
    on_size = function(cols)
        wide:set(cols >= 2 and 1 or 0)
    end,
})

helix.settings.on_change("effect", follow)
follow()
```

Check before relying on them, and adjust the Lua (not the API) if any differs: the subscribe callback's table shape (`src/plugin/lua_bind_moonraker.cpp#subscribe`), that JSON booleans arrive as Lua booleans, that `helix.settings.get` returns the schema default when unset, and that `helix.gcode` returns `ok, err`.

`examples/plugins/led-effects/ui/led-effects__tile.xml`: a column, centred, whole tile clickable with `<event_cb trigger="clicked" callback="plugin_event" user_data="led-effects__toggle"/>`; two `icon`s (`led-effects__on`, `src="lightbulb_on"`, `variant="primary"`, hidden while `led-effects__active` is 0; `led-effects__off`, `src="lightbulb_outline"`, `variant="secondary"`, hidden while it is 1) using `<bind_flag_if_eq subject="led-effects__active" flag="hidden" ref_value="..."/>`; a `text_body name="led-effects__state" bind_text="led-effects__state"`; a `text_small name="led-effects__effect" bind_text="led-effects__effect"` hidden while `led-effects__wide` is 0. Design tokens only (`#space_*`, no literal colours); copy the `view` attributes from `examples/plugins/temp-spark/ui/temp-spark__tile.xml`. A hyphenated id cannot use `cond`; `bind_flag_if_eq` is the way.

`examples/plugins/led-effects/README.md`, in temp-spark's README shape: what it demonstrates (`helix.gcode` behind the `gcode` permission and its consent, `helix.moonraker.subscribe` as the source of truth, validating a setting before it reaches G-code, `helix.widget` `on_size`, `bind_flag_if_eq` instead of `cond`, `helix.ui.toast`), Run it (`HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv`, enable in Settings > Plugins and approve the G-code permission, add the tile from the catalog's Plugins category; the `--test` printer has the effects `breathing`, `fire_comet`, `rainbow`, `static_white`), Layout (one line per file), and one line on what the C++ demo did that this does not (plain `SET_LED` toggling lives in the built-in LED widget; there are no print events, watch `print_state` instead).

- [ ] **Step 2: Failing tests** in `tests/unit/test_example_plugin_led_effects.cpp`, mirroring `tests/unit/test_example_plugin_temp_spark.cpp` (same includes, an `XMLTestFixture` subclass, `HostRig`, `drain()`), with a helper `json led_block(const std::string& effect)` that enables `led-effects` with `{"gcode"}` and sets `settings["led-effects"]["effect"]`, and a helper that answers the subscribe's `printer.objects.query` `call` request with `{"status": {"led_effect <name>": {"enabled": <bool>}}}`. Cases, each with these assertions:

1. "led-effects needs the gcode permission": granted `{"gcode"}` loads (`PluginStatus::Loaded`, `find_widget_def("led-effects__tile")` non-null with `colspan == 2` and `rowspan == 2`, in tracks); in a separate scope, granted `{}` parks at `PluginStatus::NeedsApproval`. Also run `check_plugin_xml("led-effects", {"led-effects__tile"}, <file text>)` and require an empty result.
2. "the tile follows the effect's live state": after load, `fake.object_sets.back()` is `{"led-effects", {"led_effect rainbow": ["enabled"]}}`; answering the query with `enabled: false` gives `led-effects__state == "Off"`; firing the `notify_status_update` handler in `fake.notify` with `{"params": [{"led_effect rainbow": {"enabled": true}}, 0.0]}` and draining gives `"On"` and `led-effects__active == 1`.
3. "a tap toggles through helix.gcode": with the state Off, `dispatch_event("led-effects__toggle")` adds one `gcode` request `SET_LED_EFFECT EFFECT=rainbow`; reply success; deliver `enabled: true`; a second tap adds `SET_LED_EFFECT EFFECT=rainbow STOP=1`.
4. "an unsafe effect name sends nothing": `set_setting("led-effects", "effect", "rainbow\nFIRMWARE_RESTART")`, then a tap: no `gcode` request is added, `led-effects__effect == "Invalid name"`, and `fake.object_sets.back().second` is an empty object (the old subscription was cancelled).
5. "switching effects follows only the new one": `set_setting(..., "effect", "breathing")`: `object_sets.back()` names `led_effect breathing`; a delta `{"led_effect rainbow": {"enabled": true}}` leaves the state `"Off"`; a delta for breathing `enabled: true` makes it `"On"`.
6. "a failed toggle keeps Klipper's state": state Off, tap, reply `RpcResult{false, {}, "Unknown effect"}`: state stays `"Off"` and the plugin stays `Loaded`.
7. "the tile names the effect when two cells wide": `dispatch_widget_hook(*rig.host->runtime("led-effects"), "led-effects__tile", WidgetHook::Size, push)` with `push` pushing `2, 1, 200, 100` gives `led-effects__wide == 1`; pushing `1, 1, 100, 100` gives 0.
8. "every example plugin loads": `enabled_all({{"temp-spark", {}}, {"led-effects", {"gcode"}}})` and `load_from("examples/plugins")`: both `Loaded`.

- [ ] **Step 3: Run to verify they fail** (before the plugin files exist, or with `main.lua` stubbed): `make t F='[example]'`.

- [ ] **Step 4: Run to verify they pass:** `make t F='[example]'`, `make t F='[plugin]'`.

- [ ] **Step 5: Hand mutations:** (a) drop the `name:match` guard so any string is accepted: case 4 goes red; (b) make `follow()` skip `sub:cancel()`: case 5 goes red. Restore both.

- [ ] **Step 6: Live check** (pinned socket and config dir per CLAUDE.md; never `pkill`; kill only your PID resolved from your socket):
  1. `HELIX_PLUGIN_DIR=$PWD/examples/plugins ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK"`.
  2. Settings > Plugins: LED Effects is listed disabled; enable it; the consent dialog names the G-code permission (`ctl text`); approve.
  3. Add the tile from home edit mode's catalog, Plugins category (free a cell first if the page is full).
  4. `ctl text led-effects__state` reads `Off`; `ctl click` the tile; the log shows `SET_LED_EFFECT: enabling 'rainbow'` and `ctl text led-effects__state` reads `On`; click again; the log shows the STOP and the state reads `Off`.
  5. Open the plugin's generated settings screen and show the Effect name row's value with `ctl text`. If `ctl` can drive the text row, set `breathing` and repeat step 4 for it; if it cannot, say so in the report (case 5 covers the path).
  Paste the exact `ctl` and log lines into the report.

- [ ] **Step 7: Commit:** `git add -N <new files>` then `git commit -m "feat(plugins): led-effects, the permissioned example plugin" -- <paths>`

---

### Task 4: The Lua plugin author guide and the remaining docs

**Files:**
- Create: `docs/devel/PLUGIN_DEVELOPMENT.md`
- Modify: `docs/devel/CLAUDE.md` (index row), `docs/devel/ENVIRONMENT_VARIABLES.md` (one link line), `CONTRIBUTING.md` (its "Write a plugin" row already links the guide; check the wording), `docs/devel/architecture/12-system-services.md` (three edits below), `src/plugin/plugin_xml_policy.cpp` only if Task 2 left a comment naming a phase (it should not), `docs/devel/plans/2026-09-28-lua-plugin-system-design.md` (Status line and the stale separator sentence), `examples/plugins/temp-spark/README.md` (one line pointing at the guide)

**Interfaces:**
- Consumes: everything shipped, read from code. Sources: `src/plugin/lua_bind_*.cpp` (the API), `src/plugin/plugin_manifest.cpp` (manifest fields and limits), `src/plugin/plugin_permissions.cpp` (`kReadonlyMethods`), `src/plugin/plugin_consent.cpp#permission_line` (consent wording), `src/plugin/lua_runtime.cpp` (sandbox, time budget), `src/plugin/plugin_host.cpp` (budget, faults, `plugin_event`), `src/plugin/plugin_xml_policy.cpp` (XML rules), `src/plugin/plugin_settings_overlay.cpp#setting_row_spec` (settings types), `include/plugin_source.h` (sync limits), `docs/devel/architecture/12-system-services.md` (the maintainer view, which the guide links to rather than repeats).
- Produces: the guide.

The guide is for an outside author with a text editor and no toolchain. Present tense, task-first, short sentences; every API row checked against the binding code (a wrong row is worse than a missing one). Sections:

1. **What a plugin is**: a folder with `manifest.json`, `main.lua` and `ui/*.xml`; runs sandboxed; cannot freeze the UI or crash it. Point at the two examples: `examples/plugins/temp-spark` (no permissions) and `examples/plugins/led-effects` (G-code).
2. **Quick start**: copy temp-spark, rename the id everywhere (folder, manifest, file names, every `temp-spark__` name), `HELIX_PLUGIN_DIR=<dir> ./build/bin/helix-screen --test -vv`, enable in Settings > Plugins, add the tile. Hot reload: saving any plugin file reloads the plugin in native dev builds.
3. **Installing on a printer**: drop the folder into `printer_data/config/helixscreen/plugins/<id>/` (Mainsail/Fluidd file manager works); the screen mirrors it on connect and when files change, shows a toast for a new plugin, never enables one on its own. Sync limits (files, sizes, plugin count) from `include/plugin_source.h`.
4. **Naming**: id pattern, the `<id>__<rest>` rule for components, subjects, object names and events, and why (one global XML and subject scope).
5. **manifest.json**: every field with type, default and limit (from `plugin_manifest.cpp`): `id`, `name`, `version`, `author`, `description`, `helix_version`, `permissions`, `memory_mb`, `widgets[]` (id, name, icon, description, component, colspan, rowspan, max_colspan, max_rowspan; spans in cells; up to 8 widgets), `settings[]`, `settings_overlay`.
6. **Settings**: the seven types and the row each becomes; `int`/`float` min/max; `enum` options; `action` (`callback` fires a `helix.ui.on` handler, check the exact routing in `plugin_settings_overlay.cpp`); `info` (`subject`); `helix.settings.get` and `on_change`; enable state and settings are shared across printers; validate any setting that reaches G-code (led-effects is the example).
7. **XML**: allowed elements (lv_* widgets, the structural ones, `bind_*`, `event_cb`, `style`, `overlay_panel`, `icon`, the `text_*` widgets, the plugin's own components), the only callback `plugin_event` with `user_data="<id>__<name>[:arg]"`, no `subject_*_event`, no `cond` for a hyphenated id (use `bind_flag_if_eq`), design tokens. Rejected XML fails the load with a reason shown in Settings > Plugins.
8. **The Lua API**: one table per module, each row the call, what it returns, the permission it needs and the limit it has. Modules: `helix.log`, `helix.subject`, `helix.ui` (`on`, `toast`, `confirm` with its option keys as the code reads them, `overlay` and the handle's `close`), `helix.widget` (`on_attach`, `on_detach`, `on_size(cols, rows, w, h)` in whole cells and pixels, `on_activate`, `on_deactivate`), `helix.printer` (`get`, `watch`, and the full name table from `lua_bind_printer.cpp`, no unwatch), `helix.moonraker` (`query`, `call` with the read-only allowlist and `moonraker_write`, `upload`, `download`, `on_agent_event`, `subscribe` with its limits and the handle's `cancel`), `helix.gcode`, `helix.http`, `helix.storage`, `helix.settings`, `helix.timer` (`after`, `every`, `cancel`), `helix.sleep`, `helix.json`. The async model: every entry is a coroutine; async calls return `value` or `nil, err`.
9. **Permissions and consent**: the four permissions, what each unlocks, the consent wording the user sees (quote `permission_line`), and that an update asking for more parks at "needs approval".
10. **Limits and faults**: memory per plugin and in total, the 50 ms entry budget, three errors in 60 s, the sandbox (which libraries, `require` rules), what a fault looks like to the user and how to re-enable.
11. **Debugging**: `-vv` logs tagged with the plugin id, `helix.log`, Settings > Plugins status and fault reasons, `ctl text <id>__<subject>` to read a subject, the `--test` mock (`HELIX_MOCK_PLUGINS_DIR` to serve plugins as if from Moonraker; the mock's LED effects).
12. **Worked examples**: a short walk through each example's `main.lua`, quoting at most a few lines each and linking the files.

Other edits:
- `docs/devel/CLAUDE.md`: add a row `| \`PLUGIN_DEVELOPMENT.md\` | Writing a Lua plugin: manifest, XML rules, the helix.* API, permissions, limits, debugging |` in the UI group, next to the `PANEL_WIDGET_GUIDE.md` row.
- `docs/devel/architecture/12-system-services.md`: (a) in the Plugins paragraph, `ui/<id>_<name>.xml` becomes `ui/<id>__<name>.xml`; (b) delete the sentence "The `on_size` hook's cell arguments and the manifest's span fields are provisional until the Phase 4 author guide; home-widget adaptive sizing is still landing." (adaptive sizing has landed; the guide documents the contract); (c) after the temp-spark sentence, add that `examples/plugins/led-effects` is the permissioned counterpart, and link the author guide once.
- `docs/devel/ENVIRONMENT_VARIABLES.md` is already current (`HELIX_PLUGIN_DIR` documents the hot reload); no edit, but link the guide from that entry in one line.
- The spec: Status becomes `Phases 1 to 4 implemented; Phase 5 not started.`; in § Event callbacks, "splits the owner id at the first `_`" becomes "splits the owner id at the first `__`".
- `examples/plugins/temp-spark/README.md`: one line under the title pointing at `docs/devel/PLUGIN_DEVELOPMENT.md`.

- [ ] **Step 1: Write the guide and the edits.**
- [ ] **Step 2: Check:** `make check-doc-anchors` (fix every finding in lines you added); `grep -n '—' docs/devel/PLUGIN_DEVELOPMENT.md` is empty; every `helix.` call named in the guide appears in `src/plugin/lua_bind_*.cpp` (`grep -o 'helix\.[a-z_.]*' docs/devel/PLUGIN_DEVELOPMENT.md | sort -u`, then check each).
- [ ] **Step 3: Commit:** `git add -N docs/devel/PLUGIN_DEVELOPMENT.md` then `git commit -m "docs(plugin): the Lua plugin author guide" -- <paths>`

---

### Task 5: Gates (controller)

- [ ] **Step 1:** Merge `origin/main` into the branch and resolve conflicts (the audit session sweeps nearby files; expect conflicts in the mock files from Task 1 if it touched them).
- [ ] **Step 2:** `make full-test-run`; `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 3:** Push the branch, then `scripts/zeus-run.sh mutate --tests '[plugin],[mock],[xml_policy],[led_effect]' --base $(git merge-base HEAD origin/main) --max-hunks 0`. Triage survivors (behaviour-preserving vs real gap); close real gaps with tests.
- [ ] **Step 4:** LAST: `scripts/zeus-run.sh asan '[plugin],[mock],[led_effect]'`, then `python3 scripts/check_asan_leaks.py --baseline scripts/asan_leak_baseline.txt <log>`.
- [ ] **Step 5:** This plan ships with the branch: delete `docs/devel/plans/2026-10-01-lua-plugin-system-phase4.md` in the merge. The design spec stays until Phase 5 ships.
