# Lua Plugin System Design

**Status:** Phases 1 and 2 implemented (Phase 1 on main, Phase 2 on feature/lua-plugins-phase2); Phases 3 to 5 not started.
**Replaces:** the `dlopen` C++ plugin system in `src/plugin/` and `docs/devel/PLUGIN_DEVELOPMENT.md`

## Why

A HelixScreen plugin today is a C++ shared library. An outside author has to cross-compile it for
MIPS, ARM and aarch64 against our headers, match our ABI, and accept that a bug in it crashes the
UI. Nobody outside the project has cleared that bar, and the in-tree demo (`led-effects`) never
worked: its injection point `panel_widget_area` is registered by no panel. The event catalog in
`include/plugin_events.h` is emitted by nothing except `PRINTER_DISCONNECTED`.

The motivating request is a user's OrcaSlicer calibration wizard: pick a test on the printer, have
Orca (which now has a Python plugin system) generate it, print it, pick the best section on the
touchscreen, and push the result back into the Orca filament profile. The Helix half of that is a
wizard UI, some Moonraker calls and a channel to a companion process. None of it needs native code.

## Goals

- An outside author ships a working plugin with a text editor: a manifest, XML and Lua. No toolchain.
- Enough surface for someone supporting their own printer's vendor-specific accessories to do it
  without us (widgets, overlays, settings, printer state, Moonraker, G-code).
- A plugin cannot freeze the UI, exhaust memory, or take down another plugin.
- Plugins install by dropping a folder into the Moonraker config root.

## Non-goals (v1)

- Plugins implementing our capability backends (`AmsBackend`, LED backends, power devices, ...).
  Designed for (see Bindings), not built.
- ESP32. It keeps `HELIX_HAS_PLUGINS=0`.
- Compatibility with the C++ `PluginAPI`. It is deleted.
- Multi-instance plugin widgets, plugin-to-plugin calls, raw `lv_obj` access, nav bar panels.
- A plugin catalog, signing, or install-from-URL.

## Decisions

| Question | Decision |
|---|---|
| Audience | Third-party authors first, with enough surface for vendor-specific plugins |
| Vendor capability backends | Not in v1; bindings designed so a Lua-implemented C++ interface can be added later |
| Language | Lua 5.4, vendored as `lib/lua` (submodule of `github.com/lua/lua` at tag `v5.4.9`), compiled as C++ so a Lua error is a C++ exception and unwinding runs destructors |
| Isolation | One `lua_State` per plugin, in-process, main thread only |
| Trust | Permissions declared in the manifest, shown at enable, enforced per binding |
| Distribution | `printer_data/config/helixscreen/plugins/<id>/`, mirrored over Moonraker's file API |
| Settings | Manifest schema rendered with our `setting_*_row` components; custom XML overlay as escape hatch |
| UI contribution | Home widgets (catalog, drag, resize) that open plugin overlays; a Settings entry per plugin |
| Canvas drawing | Phase 5, as a retained display list |

## Plugin layout

```
printer_data/config/helixscreen/plugins/
  orca-cal/
    manifest.json
    main.lua
    lib/            -- optional, reachable through require
    ui/             -- XML components
      orca-cal__widget.xml
      orca-cal__wizard.xml
```

### Naming

A plugin id matches `^[a-z][a-z0-9-]{1,31}$` and contains no underscore. Every XML component,
XML callback and subject a plugin registers is named `<id>__<rest>`: the separator is a double
underscore, because app names share the single-underscore space (`ams_*`, `extruder_target`,
`settings_*`) and no app name contains `__`, so only the double underscore proves a name is the
plugin's. Two plugins can never collide in LVGL's global XML and subject scope, and the loader
rejects a plugin that registers a name without its own prefix.

### manifest.json

```json
{
  "id": "orca-cal",
  "name": "Orca Calibration",
  "version": "1.2.0",
  "author": "someone",
  "description": "Guided OrcaSlicer calibration from the printer",
  "helix_version": ">=1.1",
  "permissions": ["gcode", "moonraker_write"],
  "memory_mb": 4,
  "widgets": [
    {
      "id": "orca-cal__launcher",
      "name": "Calibrate",
      "icon": "tune",
      "description": "Start a calibration test",
      "component": "orca-cal__widget",
      "colspan": 1, "rowspan": 1,
      "max_colspan": 2, "max_rowspan": 1
    }
  ],
  "settings": [
    {"key": "companion", "type": "string", "label": "Orca companion name"},
    {"key": "auto_apply", "type": "bool", "label": "Apply result without asking", "default": false},
    {"key": "step", "type": "int", "label": "Temperature step", "min": 1, "max": 20, "default": 5},
    {"key": "test", "type": "enum", "label": "Default test",
     "options": ["temperature", "flow", "pressure_advance"], "default": "temperature"},
    {"key": "test_link", "type": "action", "label": "Test companion connection", "callback": "orca-cal__ping"}
  ],
  "settings_overlay": null
}
```

Settings types map onto existing rows: `bool` to `setting_toggle_row`, `int` and `float` with
`min`/`max` to `setting_slider_row`, `enum` to `setting_dropdown_row`, `string` to the new
`setting_text_row`, `action` to `setting_action_row` (fires a plugin callback), and `info` to
`setting_info_row` (shows a plugin subject named by `subject`).

`settings_overlay` names a plugin XML component to use instead of the generated settings screen.
Widget fields mirror `PanelWidgetDef`; the category is always Plugins.

## Components

The old `src/plugin/` goes entirely: `PluginManager`, `PluginAPI`, `PluginRegistry`,
`plugin_events`, `InjectionPointManager`, the `print_status_extras` injection point in
`ui_panel_print_status.cpp`, and `tests/unit/test_plugin_api_subjects.cpp`. The C++ system's only
touch points outside `src/plugin/` are `application.cpp` (init, Moonraker connect, shutdown,
restart) and that injection point.

| Unit | Does | Depends on |
|---|---|---|
| `PluginSource` | Lists and mirrors the Moonraker plugin root into the local cache; reports changes | Moonraker file API |
| `PluginManifest` | Parses and validates `manifest.json` into plain structs. Pure, no Lua or LVGL | nothing |
| `PluginPermissions` | Maps granted permissions to allowed binding calls; computes permission growth between versions. Pure | `PluginManifest` |
| `PluginHost` | Enable, disable, consent, load, unload, reload, fault handling, memory budget | all of the above |
| `LuaRuntime` | One per plugin: `lua_State`, capped allocator, time-budget hook, stripped stdlib, sandboxed `require`, coroutine scheduling, `AsyncLifetimeGuard` | Lua |
| Bindings | One file per area (`log`, `subject`, `ui`, `widget`, `printer`, `moonraker`, `gcode`, `http`, `storage`, `settings`, `timer`, `json`) | `PluginPermissions`, existing app APIs |
| Contributions | `LuaPanelWidget`, `PluginOverlayHost`, Settings → Plugins list, consent screen, schema settings overlay, new `setting_text_row` | widget registry, `NavigationManager`, `setting_*_row` |

Outside the plugin code:

- `panel_widget_registry` accepts widget definitions at runtime, with strings owned by the
  registry, plus an unregister. The static table stays as it is for built-in widgets.
- The widget catalog overlay gains a Plugins category.
- `panel_widget_config` keeps unknown widget ids when loading and writes them back when saving,
  instead of dropping them (`src/system/panel_widget_config.cpp#load`). A kept id with no
  registered definition renders nothing.
- `ui_xml/components/setting_text_row.xml`: a label plus value that opens the keyboard.

### Lua sandbox

Loaded libraries: base (without `dofile`, `loadfile`, and with `load` restricted to text chunks),
`string` (without `string.dump`), `table`, `math`, `utf8`, `coroutine`. `collectgarbage` accepts
only `"count"`, `"collect"` and `"step"`. `io`, `os`, `package` and `debug` are not compiled into
the binary at all (`liolib.c`, `loslib.c`, `loadlib.c`, `ldblib.c` and `linit.c` are left out of
the build).
`require(name)` resolves only to `<plugin>/<name>.lua` or `<plugin>/lib/<name>.lua`, with `..` and
absolute paths rejected.

## Lifecycle

### Boot

1. `PluginHost` reads the `plugins` settings block and loads every enabled plugin from the local
   cache (`~/helixscreen/plugin-cache/<id>/`). Plugin widgets exist at boot and work offline.
2. On Moonraker connect, `PluginSource` lists `config/helixscreen/plugins/`, compares size and
   mtime against the cache, downloads what changed, and deletes cache entries whose source is
   gone. Every plugin whose files changed is reloaded.

`HELIX_PLUGIN_DIR=<path>` makes a local directory the source instead of Moonraker, for authors.

### Settings block

```json
"plugins": {
  "enabled": {"orca-cal": {"version": "1.2.0", "permissions": ["gcode", "moonraker_write"]}},
  "settings": {"orca-cal": {"step": 5, "auto_apply": false}}
}
```

The old `/plugins/enabled` string list is discarded on load; no one has a working plugin to lose.

### Discovery and enabling

- Every cached plugin with a valid manifest appears in Settings → Plugins, disabled by default.
  An invalid one appears with its validation error.
- Enabling shows a consent screen listing the permissions in plain words. `gcode` reads as full
  control of the printer, since every macro, including shell commands, is reachable through it.
- An update that requests permissions beyond those granted stays unloaded and shows "needs
  approval" until the user consents again. An update with the same or fewer permissions loads
  without asking.

### Load

1. Check the memory budget (see Limits). Refuse with a reason shown in Settings if it does not fit.
2. Register each XML component in `ui/` through `lv_xml_register_component_from_file`, rejecting
   unprefixed names.
3. Create the `LuaRuntime` and run the top level of `main.lua` as a coroutine. This registers
   subjects, callbacks and widget handlers.
4. Register the manifest's widget definitions, which puts them in the Add Widget catalog and lets
   kept layout ids render again.

### Unload

One path for reload, disable, removal, fault and shutdown:

1. Run the plugin's global `on_unload` if defined (time-budgeted, errors logged and ignored).
2. Pop the plugin's overlays and modals.
3. Detach its widget instances and unregister its widget definitions.
4. Unregister its subjects and XML components, and drop its `helix.ui.on` handlers.
5. Invalidate its `AsyncLifetimeGuard` so pending replies, timers and HTTP results are dropped.
6. `lua_close`.

Subjects the plugin registered are unregistered at step 4 and freed once no object observes them.

Reload is unload then load. A disabled or removed plugin's widgets disappear from the home panel;
their ids stay in the saved layout and reappear when the plugin returns.

### Hot reload

`XmlHotReloader` watches the cache directory, so an XML change applies within its poll interval.
A Lua change reloads the plugin. With a Moonraker source, the change arrives through Moonraker's
file-list notifications and the mirror.

### Shutdown

`PluginHost` unloads every plugin before the Moonraker client is destroyed and before LVGL
teardown, in `Application`'s existing shutdown and restart paths.

## Lua API

All under one global `helix`. Lua changes the screen only through subjects the XML binds to.

### Always available

| Call | Notes |
|---|---|
| `helix.log.info/warn/error/debug(msg)` | spdlog, tagged with the plugin id |
| `helix.subject.int(name, init)`, `helix.subject.string(name, init)` | `name` excludes the prefix; registered as `<id>__<name>`. Handle: `:get()`, `:set(v)`, `:observe(fn)` |
| `helix.ui.on(name, fn)` | handles XML events addressed to `<id>__<name>` (see Event callbacks) |
| `helix.ui.overlay(component, {on_close})` | pushes and registers through `PluginOverlayHost`; returns a handle with `:close()` |
| `helix.ui.confirm(title, msg, {severity, confirm_text, on_confirm, on_cancel})` | wraps `modal_confirm`; dismissal reaches `on_cancel` |
| `helix.ui.toast(msg, severity)` | |
| `helix.widget(id, {on_attach, on_detach, on_size, on_activate, on_deactivate})` | handlers for a manifest-declared widget; `on_size(cols, rows, w, h)` |
| `helix.printer.get(name)`, `helix.printer.watch(name, fn)` | read-only, a fixed table of Lua-facing names decoupled from our subject names (see Printer state) |
| `helix.moonraker.query(objects)` | printer objects, Klipper's stable contract |
| `helix.moonraker.subscribe(objects, fn)` | **Phase 3.** Moonraker's `printer.objects.subscribe` replaces the connection's whole subscription, so plugin objects have to be merged into the app's union subscription in `MoonrakerDiscoverySequence`, which is WebSocket critical-path work. Until then, poll with `query` on a timer |
| `helix.moonraker.on_agent_event(name, fn)` | Moonraker agent events: the channel to companions such as an Orca plugin |
| `helix.settings.get(key)`, `helix.settings.on_change(key, fn)` | the plugin's schema settings |
| `helix.timer.after(ms, fn)`, `helix.timer.every(ms, fn)` | return a handle with `:cancel()` |
| `helix.sleep(ms)` | yields the current coroutine |
| `helix.json.encode(v)`, `helix.json.decode(s)` | |

### Permission-gated

| Permission | Unlocks |
|---|---|
| `gcode` | `helix.gcode(script)` |
| `moonraker_write` | `helix.moonraker.call(method, params)` for any method. Without it, `call` accepts only a read-only allowlist (`printer.objects.query`, `printer.objects.list`, `server.info`, `server.files.list`, `server.files.metadata`, `machine.system_info`). Also `helix.moonraker.upload(root, path, content)` and `helix.moonraker.download(root, path)` for `gcodes` and `config`, over the existing `ITransfersAPI` |
| `http` | `helix.http.get(url, opts)`, `helix.http.post(url, opts)` on `HttpExecutor` |
| `storage` | `helix.storage.get(key)`, `helix.storage.set(key, v)`: JSON values in `~/helixscreen/config/plugin-data/<id>.json`, capped at 256 KB |

A denied call raises a Lua error naming the missing permission and logs it once per call site.

### Event callbacks

LVGL event callbacks are plain function pointers and the XML engine has no way to unregister one,
so plugins do not register their own. One callback, `plugin_event`, is registered once for the
life of the app. Plugin XML addresses a handler through `user_data`, optionally with an argument
after a colon:

```xml
<event_cb trigger="clicked" callback="plugin_event" user_data="orca-cal__start"/>
<event_cb trigger="clicked" callback="plugin_event" user_data="orca-cal__pick:3"/>
```

`plugin_event` splits the owner id at the first `_`, finds that loaded plugin, and calls the
handler registered with `helix.ui.on("start", fn)` or `helix.ui.on("pick", fn)`, passing the
argument string (or `nil`). An event for a plugin that is not loaded, or a name with no handler,
is logged at debug and ignored.

### Printer state

| Lua name | Type | Source subject |
|---|---|---|
| `connected` | boolean | `printer_connection_state` equals `ConnectionState::CONNECTED` |
| `print_state` | string | `print_state` |
| `progress` | integer percent | `print_progress` |
| `filename` | string | `print_filename` |
| `extruder_temp`, `extruder_target` | number, °C | `extruder_temp`, `extruder_target` (decidegrees / 10) |
| `bed_temp`, `bed_target` | number, °C | `bed_temp`, `bed_target` (decidegrees / 10) |
| `chamber_temp`, `chamber_target` | number, °C | `chamber_temp`, `chamber_effective_target` (decidegrees / 10) |

Any other name is an error. The table is the contract; the subjects behind it can be renamed.

### Async model

Every entry into Lua (top level of `main.lua`, XML callbacks, observers, event handlers, widget
hooks, timers) runs as a new coroutine. Async calls (`gcode`, `moonraker.*`, `http.*`, `sleep`)
yield it; the result arrives on a worker thread, crosses to the main thread through
`UpdateQueue`, checks the plugin's `AsyncLifetimeGuard`, and resumes the coroutine. Async calls
return `value` or `nil, err`.

```lua
local status = helix.subject.string("status", "Idle")

helix.ui.on("start", function()
  status:set("Starting")
  local ok, err = helix.gcode("SDCARD_PRINT_FILE FILENAME=temp_tower.gcode")
  status:set(ok and "Printing" or ("Failed: " .. err))
end)
```

### Binding rule

Lua is compiled as C++, so `lua_error` throws and destructors on a binding's stack run normally.
What remains is that no Lua error may escape into a C frame: every entry into Lua goes through
`lua_resume` or `lua_pcall`, which catch it, and LVGL only ever calls our C++ trampolines, never
Lua directly. The allocator enforces the memory cap only inside those protected entries, so the
unprotected setup work around them (creating the coroutine, pushing arguments) cannot fail with a
memory error and reach Lua's panic handler.

Bindings take plain values and Lua callbacks and never expose C++ object identity, which is what
lets a later trampoline let a Lua table implement a C++ interface.

## Limits and faults

| Limit | Rule |
|---|---|
| Memory per plugin | Allocator cap, default 2 MB, `memory_mb` in the manifest raises it (1 to 64). The cap is a ceiling, not a reservation |
| Memory, all plugins | Budget `min(MemTotal / 16, 64 MB)` computed at startup. A plugin whose cap does not fit what remains is not loaded, and Settings says why |
| Time | Count hook every 10,000 instructions checks a monotonic deadline of 50 ms per entry into Lua. Overrun raises an error and switches the hook to every instruction, so a `pcall` loop cannot swallow it: each instruction outside the innermost `pcall` raises again until the entry unwinds |
| Errors | Every entry runs under `lua_pcall` with a traceback handler. Three errors within 60 s disable the plugin |

An out-of-memory error that reaches the entry point, a time overrun, or the error threshold disables the plugin through the unload
path, shows a toast naming it, and records the reason. Settings → Plugins shows the reason and a
Re-enable button. Other plugins keep running.

## Canvas (phase 5)

A retained display list, so Lua never runs during rendering and there is no pixel buffer.

```lua
local c = helix.canvas("graph")       -- binds to a <plugin_canvas name="orca-cal__graph"/> in XML
c:clear()
c:polyline(points, {color = "primary", width = 2})
c:text(x, y, "215°", {font = "body", color = "text"})
c:commit()                            -- swaps the list and invalidates once
```

Primitives: `line`, `polyline`, `rect` (fill, border, radius), `arc`, `circle`, `text`. Colors and
fonts are theme token names. The widget replays the committed list in its own draw event;
`on_size` gives the plugin its dimensions to rebuild the list. A list is capped at 4,096
primitives.

## Phases

1. **Runtime.** Vendor Lua; `PluginManifest`, `PluginPermissions`, `LuaRuntime`, bindings,
   `PluginHost` loading from `HELIX_PLUGIN_DIR` only; delete the old `src/plugin/` and its call
   sites.
2. **Contributions.** Runtime widget definitions and layout id retention, catalog Plugins
   category, `LuaPanelWidget`, `PluginOverlayHost`, Settings → Plugins, consent, schema settings
   overlay, `setting_text_row`.
3. **Moonraker source.** `PluginSource` mirror, sync on connect, change notifications, permission
   growth on update, and `helix.moonraker.subscribe` through a plugin object set merged into the
   app's union subscription.
4. **Acceptance and docs.** Port `led-effects` to Lua; rewrite `docs/devel/PLUGIN_DEVELOPMENT.md`
   as the Lua author guide; update `docs/devel/architecture/12-system-services.md` and
   `ENVIRONMENT_VARIABLES.md`.
5. **Canvas.**

The widget contract plugin authors code against is the adaptive sizing model; freeze the
`on_size` shape and the manifest span fields only after the home widget adaptive sizing work lands.

## Testing

- **Pure units:** manifest validation (id pattern, prefix violations, unknown permissions, bad
  schema types, span ranges), permission growth between versions, the read-only allowlist.
- **`LuaRuntime`:** memory cap trips and the plugin is disabled; `while true do end` is
  interrupted; a binding error leaks nothing (ASAN on zeus); sandbox rejects `io`, `os`, `debug`,
  binary chunks and `require("../x")`; coroutine yield and resume through a fake async source; a
  reply after unload is dropped.
- **Integration (`LVGLTestFixture`):** a fixture plugin in `tests/fixtures/plugins/` loads, its
  widget attaches, a subject set from Lua reaches bound XML text, an overlay pushes and pops,
  reload works, and after unload no subject, callback or component with its prefix remains
  registered.
- **Layout:** an unknown widget id survives load and save of the home layout.
- **Acceptance:** Lua `led-effects` under `--test`, verified with `ctl`: in the catalog, placeable,
  toggles through `helix.gcode`, generated settings screen works.
- **Mutation:** `make mutate-diff` over `PluginPermissions`, the fault paths and the layout
  retention.
