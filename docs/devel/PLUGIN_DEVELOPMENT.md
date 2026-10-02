# Writing a Lua Plugin

A HelixScreen plugin is a folder of Lua and XML that runs on the printer's screen. It can
add home-panel tiles, overlays and settings without compiling anything: an author needs a
text editor and the plugin folder, nothing else. This guide is the contract; every rule in
it is enforced by code, cited so it can be verified against the source.

Two complete plugins ship in the repository:

- `examples/plugins/temp-spark` - a heater sparkline tile. No permissions.
- `examples/plugins/led-effects` - an LED effect toggle. The `gcode` permission.

## 1. What a plugin is

A folder named after the plugin id:

```
my-plugin/
  manifest.json     describes the plugin: id, name, widgets, settings, permissions
  main.lua          runs when the plugin loads
  ui/               XML components, one file per component
    my-plugin__tile.xml
```

The plugin runs in its own sandboxed Lua state (`src/plugin/lua_runtime.cpp#LuaRuntime`).
It cannot freeze or crash the screen: each Lua entry gets a time budget, the state has a
memory cap, and repeated errors disable the plugin while the app keeps running
(§10 Limits and faults).

## 2. Quick start

Run HelixScreen from a checkout and point it at a local plugin folder:

```sh
cp -r examples/plugins/temp-spark examples/plugins/my-plugin
cd examples/plugins/my-plugin
# rename the id everywhere: the folder, manifest.json's "id", and every
# temp-spark__ name in manifest.json, main.lua and the ui/ file names
HELIX_PLUGIN_DIR=examples/plugins ./build/bin/helix-screen --test -vv
```

The `--test` flag runs a mock printer, so nothing touches a real machine. Then:

1. Settings > Plugins > Temperature Sparkline (your plugin's `name`) > enable. A plugin
   with no permissions asks nothing; one with permissions shows a consent dialog first.
2. Add the tile from the home panel's widget catalog, under the Plugins category. A tile
   needs free cells: remove or shrink a stock widget first if the page is full.

Saving any file of the plugin reloads it within about a second in native dev builds (the
same `HELIX_HOT_RELOAD` gate as XML hot reload; `HELIX_PLUGIN_DIR` documents it in
`ENVIRONMENT_VARIABLES.md`). Cross-compiled release builds do not poll.

`HELIX_PLUGIN_DIR` replaces the printer's plugin source with your directory while it is
set, so the fastest loop is: edit, save, watch the log line naming your plugin reload.

## 3. Installing on a printer

Copy the plugin folder into the printer's Moonraker config:

```
printer_data/config/helixscreen/plugins/<id>/
```

The Mainsail or Fluidd file manager works; no restart is needed. The screen mirrors the
folder into a local cache on connect and whenever the files change, shows a toast when a
new plugin arrives, and never enables one on its own - the user enables it in
Settings > Plugins (`src/application/application.cpp#on_plugin_sync`).

A plugin over a sync limit is skipped whole and keeps its previous installed version
(`include/plugin_source.h`): at most 128 files, 8 MB per plugin, 4 MB per file, and 32
plugins in one listing.

## 4. Naming

A plugin id matches `^[a-z][a-z0-9-]{1,31}$`: lowercase, 2 to 32 characters, digits and
hyphens, never an underscore (`src/plugin/plugin_manifest.cpp#is_valid_plugin_id`). The
folder name must equal the id.

Everything a plugin registers is named `<id>__<rest>` with a double underscore
(`kPluginNameSeparator`, `include/plugin_manifest.h#kPluginNameSeparator`):

- XML component files: `ui/<id>__<name>.xml`, and the component's name inside
- widget ids and their components in the manifest
- subjects (Lua hands only the short `<name>`; see §8)
- object `name=` attributes in XML
- `plugin_event` handler names in `user_data`

One global XML and subject scope is shared by the app and every plugin. The app's own
names use single underscores (`extruder_temp`, `settings_*`); only the double underscore
proves a name belongs to a plugin, and a plugin can reach only its own names.

## 5. manifest.json

Parsed by `src/plugin/plugin_manifest.cpp#parse_manifest`. Any error in one field rejects
the whole manifest; Settings > Plugins shows the reasons.

| Field | Type | Default | Rules |
|---|---|---|---|
| `id` | string | required | the id pattern above; must match the folder name |
| `name` | string | required | single line, at most 48 bytes; shown in the UI |
| `version` | string | required | single line, at most 32 bytes |
| `author` | string | empty | single line, at most 64 bytes |
| `description` | string | empty | shown in the Plugins screen |
| `helix_version` | string | empty | a HelixScreen version constraint; a plugin whose constraint fails the running version shows as *incompatible* |
| `permissions` | array | `[]` | zero or more of `gcode`, `moonraker_write`, `http`, `storage` (§9) |
| `memory_mb` | integer | `2` | 1 to 64; the plugin's share of the plugin memory budget |
| `widgets` | array | `[]` | up to 8 home tiles |
| `settings` | array | `[]` | the settings schema (§6) |
| `settings_overlay` | string | empty | one of the plugin's own components, opened instead of the generated settings screen |

Each `widgets` entry:

| Field | Type | Default | Rules |
|---|---|---|---|
| `id` | string | required | `<id>__<name>`, unique in the array |
| `name` | string | required | the catalog entry's display name |
| `component` | string | required | `<id>__<name>`, a file in `ui/` |
| `icon` | string | `puzzle_outline` | an icon name from `include/ui_icon_codepoints.h` |
| `description` | string | empty | catalog help text |
| `colspan`, `rowspan` | integer | `1` | 1 to 8 grid cells |
| `max_colspan`, `max_rowspan` | integer | `0` | `0` = not resizable on that axis; otherwise `colspan` to 8 |

Widget tiles appear in the home panel's widget catalog under the Plugins category, never
enabled by default, one instance each. A tile's saved placement survives a disabled or
absent plugin and returns when it loads again.

## 6. Settings

Each `settings` entry has `key` (matching `[a-z0-9_]{1,64}`), `label` and `type`, and
becomes one row of the plugin's settings screen
(`src/plugin/plugin_settings_overlay.cpp#setting_row_spec`):

| Type | Extra fields | Row |
|---|---|---|
| `bool` | `default` | toggle |
| `int` | `min` < `max`, `default` in range | slider, whole numbers |
| `float` | `min` < `max`, `default` in range | slider, value scaled by 100 |
| `enum` | `options`: non-empty array of strings without newlines; `default` one of them | dropdown |
| `string` | `default`, a string | text input |
| `action` | `callback`: `<id>__<name>` of a `helix.ui.on` handler | button that fires the handler |
| `info` | `subject`: `<id>__<name>` of a plugin subject | read-only row bound to the subject |

In Lua, `helix.settings.get(key)` returns the stored value if it still fits the
declaration, else the default (`src/plugin/lua_bind_io.cpp#effective_setting`).
`helix.settings.on_change(key, fn)` calls `fn(value)` after each accepted change; several
handlers per key are allowed. An `action` row reaches its handler through the same path
as a `plugin_event`.

Enable state and settings are shared across printers: enabling a plugin once enables it
on every printer that ships it, under the same permission check.

**Validate any setting that reaches G-code.** A stored string is whatever was typed. The
led-effects plugin anchors and bounds its effect name before building a command line
(§12): anchor the match (`^...$`), bound the length, and refuse rather than send.

## 7. XML

Each `ui/<id>__<name>.xml` file defines one component and is registered under its file
stem. The whole file passes `src/plugin/plugin_xml_policy.cpp#check_plugin_xml` before
anything is registered; a rejection fails the load and Settings > Plugins shows the
reason with the file name.

Allowed elements (`src/plugin/plugin_xml_policy.cpp#is_allowed_element`):

- `lv_*` widgets (`lv_label`, `lv_bar`, `lv_obj`, and so on)
- the structural `component`, `view`, `api`, `prop`
- `style`, `remove_style*`, `play_timeline_event`, `bind_*` elements
- `event_cb`
- the app chrome widgets `overlay_panel`, `icon`, `text_heading`, `text_body`,
  `text_muted`, `text_small`, `text_xs`, `text_tiny`
- the plugin's own components, by name

A `view` may `extends` only another allowed name. `screen_load_event` and
`screen_create_event` are never available. The `subject_*_event` elements are rejected
outright: they hold a raw subject pointer with no observer, and `plugin_event` plus
`s:set()` in Lua reaches the same effect by name.

Attribute rules (`src/plugin/plugin_xml_policy.cpp#check_plugin_attr`):

- The only callback is `plugin_event`: any `callback`, `event_cb`, `*_callback` or `*_cb`
  attribute must read `plugin_event`.
- `user_data` carries the target: `<id>__<name>` optionally followed by `:arg`. The host
  runs the `helix.ui.on` handler `name` with `arg` as a string, or `nil` without a colon.
- Every subject reference (`subject`, `*_subject`, and any `bind_*` attribute) must name a
  subject this plugin owns, spelled `<id>__<name>`.
- Object `name=` attributes must be `<id>__<name>`, so a screen-wide lookup never
  resolves a plugin object.
- A `$`-prop indirection on any of these attributes is refused.
- `cond` and `*_cond` expressions may name only the plugin's own subjects. Identifiers in
  the expression grammar are `[A-Za-z_][A-Za-z0-9_]*`, and every owned name starts with
  the id, so **a plugin whose id contains a hyphen cannot use `cond` at all**. Use
  `bind_flag_if_eq` (and the other `bind_flag_*` elements) instead; led-effects does.

Because every `bind_*` attribute value must be an owned subject name, `bind_text-fmt` is
rejected: a plugin-supplied printf format bound to a subject is a format-string hazard.
Format in Lua and set a string subject.

Styling uses the app's design tokens, for example `style_pad_all="#space_sm"`,
`style_text_color="#text"`, `style_text_font="#font_heading_medium"`
(`docs/devel/UI_CONTRIBUTOR_GUIDE.md` has the full token list).

## 8. The Lua API

Every entry - the top level of `main.lua`, each handler, each timer callback - runs as
its own coroutine. Async calls yield and come back on the main thread, returning `value`
or `nil, err`; synchronous errors raise Lua errors, which count toward the error limit.
An async call is legal at the top level of `main.lua` and inside any function the runtime
calls, and nowhere else: not in a metamethod, a table iterator, a sort comparator, the
top level of a `require`d module, or a coroutine the plugin creates itself
(`src/plugin/lua_runtime.cpp#await_async`).

### helix.log (`src/plugin/lua_bind_core.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.log.debug(msg)` | | also `info`, `warn`, `error` |
| `print(...)` | | the sandbox's `print` logs at info with the plugin tag, one line capped at 4096 bytes |

### helix.subject (`src/plugin/lua_bind_ui.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.subject.int(name[, init])` | handle | `name` is 1-48 chars of `[a-z0-9_-]`; registered as `<id>__<name>`; at most 128 subjects per plugin |
| `helix.subject.string(name[, init])` | handle | strings are at most 1023 bytes |
| `s:get()` | number or string | |
| `s:set(v)` | | int range is int32; a string over 1023 bytes is an error |
| `s:observe(fn)` | | `fn(value)` on each change, not for the current value; observers and printer watches share a quota of 256 per plugin |

Subjects are the plugin's whole output: XML binds to them, Lua sets them.

### helix.ui (`src/plugin/lua_bind_ui.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.ui.on(name, fn)` | | registers the handler a `plugin_event` or an `action` setting row fires; `name` is 1-48 chars of `[a-z0-9_-]`; `fn(arg)` receives the `user_data` argument string or `nil` |
| `helix.ui.toast(msg[, severity])` | | `severity` is `"info"` (default), `"success"`, `"warning"` or `"error"` |
| `helix.ui.confirm(title, msg[, opts])` | | `opts.severity` (`"info"`, `"warning"`, `"error"`), `opts.confirm_text` (default `"OK"`), `opts.on_confirm`, `opts.on_cancel`; a dismissed dialog runs `on_cancel` too; at most one open dialog per plugin; not during `on_unload` |
| `helix.ui.overlay(component[, attrs])` | handle | opens one of the plugin's own components full-screen; `attrs` is a table of string attributes passed to the component plus an optional `on_close` function; `handle.close()` closes it; the plugin unloading closes every open overlay; not during `on_unload` |

### helix.widget (`src/plugin/lua_bind_widget.cpp`)

`helix.widget(id, hooks)` registers hooks for one declared widget; `id` is the short name
after `<id>__`, and the widget must be in the manifest's `widgets` array. Hooks, all
optional:

| Hook | Called with |
|---|---|
| `on_attach()` | the tile's tree enters the home panel |
| `on_detach()` | the tile's tree leaves it |
| `on_size(cols, rows, w, h)` | `cols`/`rows` in whole cells, `w`/`h` in pixels |
| `on_activate()` / `on_deactivate()` | the page holding the tile is shown / hidden |

### helix.printer (`src/plugin/lua_bind_printer.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.printer.get(name)` | value or `nil` | `nil` before the app subject exists; an unknown name is an error |
| `helix.printer.watch(name, fn)` | | `fn(value)` on each change, not for the current value; a watch lives until the plugin unloads, there is no unwatch |

Fields (`src/plugin/lua_bind_printer.cpp#printer_fields`):

| Name | Type |
|---|---|
| `connected` | boolean |
| `print_state` | string |
| `progress` | integer (percent) |
| `filename` | string |
| `extruder_temp`, `extruder_target` | degrees C |
| `bed_temp`, `bed_target` | degrees C |
| `chamber_temp`, `chamber_target` | degrees C |

No permission is needed for either call.

### helix.moonraker and helix.gcode (`src/plugin/lua_bind_moonraker.cpp`)

| Call | Permission | Returns | Notes |
|---|---|---|---|
| `helix.moonraker.query(objects)` | none | table or `nil, err` | `objects` maps a status object name to `true` (every field) or a list of field names |
| `helix.moonraker.call(method[, params])` | none for the seven read-only methods below, else `moonraker_write` | value or `nil, err` | |
| `helix.moonraker.upload(root, path, content)` | `moonraker_write` | `true` or `nil, err` | writes a file through Moonraker |
| `helix.moonraker.download(root, path)` | `moonraker_write` | string or `nil, err` | a body over the memory cap comes back as an error, not a fault |
| `helix.moonraker.on_agent_event(event, fn)` | none | | `fn(agent, data)` when Moonraker posts that agent event; at most 16 handlers per plugin |
| `helix.moonraker.subscribe(objects, fn)` | none | handle | live status, see below |
| `helix.gcode(script)` | `gcode` | `true` or `nil, err` | runs one G-code script |

Read-only methods callable through `helix.moonraker.call` without any permission
(`src/plugin/plugin_permissions.cpp#is_readonly_moonraker_method`):
`printer.objects.query`, `printer.objects.list`, `server.info`, `server.files.list`,
`server.files.metadata`, `server.temperature_store`, `machine.system_info`.

`subscribe` mirrors Klipper's status subscription: `objects` maps an object name to
`true` or a field-name list, and `fn(status)` receives a table `{[object] = {field = value}}`.
The first delivery carries the queried current values; later ones carry each change, so
the tile always shows what Klipper reports, including changes made elsewhere. The
returned handle's `cancel()` stops it. Limits, each raising an error that names it: at
most 8 subscriptions per plugin totalling 16 objects, 32 fields per object, object names
at most 64 bytes of printable ASCII.

### helix.http (`src/plugin/lua_bind_io.cpp`)

| Call | Permission | Returns |
|---|---|---|
| `helix.http.get(url[, opts])` | `http` | `{status = code, body = text}` or `nil, err` |
| `helix.http.post(url[, opts])` | `http` | same |

`opts` is a table: `body` (string), `headers` (table of strings), `timeout_ms` (default
10000, clamped to 1-60000). Only `http://` and `https://` URLs. At most 2 requests in
flight per plugin. Requests do not follow redirects and refuse the printer's own host
and local interface addresses. A response over the memory cap comes back as an error.

### helix.storage (`src/plugin/lua_bind_io.cpp`)

| Call | Permission | Returns | Notes |
|---|---|---|---|
| `helix.storage.get(key)` | `storage` | value or `nil` | one JSON object in `plugin-data/<id>.json` beside the app settings; invalid JSON starts empty |
| `helix.storage.set(key, value)` | `storage` | | `value` of `nil` erases the key; the whole store is capped at 256 KB |

### helix.settings (`src/plugin/lua_bind_io.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.settings.get(key)` | value | the key must be declared in the manifest; returns the stored value if it fits the declaration, else the default |
| `helix.settings.on_change(key, fn)` | | `fn(value)` after each accepted change |

### helix.timer, helix.sleep, helix.json (`src/plugin/lua_bind_core.cpp`)

| Call | Returns | Notes |
|---|---|---|
| `helix.timer.after(ms, fn)` | handle | one shot; `handle.cancel()` |
| `helix.timer.every(ms, fn)` | handle | repeating; `handle.cancel()` |
| `helix.sleep(ms)` | | async; yields the entry and resumes after the delay |

Intervals are 1 ms to 24 h; a plugin holds at most 64 live timers.

| Call | Returns | Notes |
|---|---|---|
| `helix.json.encode(value)` | string | tables deeper than 32, cycles, and non-string object keys are errors |
| `helix.json.decode(s)` | value or `nil, err` | invalid JSON, or nesting deeper than 64, returns `nil` plus an error string |

### The sandbox

Libraries: `base`, `string`, `table`, `math`, `utf8`, `coroutine`. There is no `io`, `os`,
`package` or `debug`. `load` and `require` accept text chunks only; `dofile`, `loadfile`
and `string.dump` are nil; `collectgarbage` accepts only `count`, `collect` and `step`
(`src/plugin/lua_runtime.cpp#install_sandbox`). `require("name")` loads
`<plugin>/name.lua` or `<plugin>/lib/name.lua`, with dotted names mapping to folders, and
caches the result (`src/plugin/lua_runtime.cpp#lua_require`).

Define a global `on_unload` function to run once before the plugin closes, on reload,
disable, removal, fault and shutdown. It may not open overlays or confirm dialogs.

## 9. Permissions and consent

Four permissions (`src/plugin/plugin_permissions.cpp`):

| Permission | Unlocks |
|---|---|
| `gcode` | `helix.gcode` |
| `moonraker_write` | `helix.moonraker.call` beyond the read-only methods, `upload`, `download` |
| `http` | `helix.http.get`, `helix.http.post` |
| `storage` | `helix.storage.get`, `helix.storage.set` |

Everything else - subjects, the UI calls, `helix.printer`, `helix.moonraker.query`,
`subscribe`, `on_agent_event`, timers, settings - needs no permission. A call without its
permission raises a Lua error naming it.

Enabling a plugin shows what the user is approving, in the consent dialog's own words
(`src/plugin/plugin_consent.cpp#permission_line`; rewording these is a product change):

- `gcode`: "Send any G-code command. This is full control of the printer: every macro,
  including ones that run shell commands, is reachable."
- `moonraker_write`: "Change printer settings through Moonraker, including uploading,
  rewriting and deleting files in the printer's config folder."
- `http`: "Connect to servers on your network and the internet."
- `storage`: "Keep its own data on this screen (up to 256 KB)."

A plugin with no permissions reads "This plugin asks for no special permissions." An
update that asks for a permission the user has not granted parks the plugin at *needs
approval* instead of loading; enabling it again approves only the new lines.

## 10. Limits and faults

| Limit | Value | Source |
|---|---|---|
| Time per Lua entry | 50 ms; once tripped, no depth of `pcall` holds the entry open | `include/lua_runtime.h#LuaRuntime/Limits`, `src/plugin/lua_runtime.cpp#budget_hook` |
| Memory per plugin | the manifest's `memory_mb` (1-64, default 2) | `src/plugin/plugin_host.cpp#load` |
| Memory, all plugins | min(RAM / 16, 64 MB); a plugin that does not fit stays *over memory budget* | `src/plugin/plugin_host.cpp#plugin_memory_budget` |
| Errors | the third within 60 s faults the plugin | `src/plugin/lua_runtime.cpp#report_error` |
| Async results | a body that would cross the memory cap arrives as an error, not a fault | `include/lua_bindings.h#push_rpc_capped_body` |

A fault (time budget, memory, error count, or `main.lua` failing to load) unloads the
plugin, marks it *faulted* with the reason in Settings > Plugins, and shows a "Plugin
disabled" toast. The app keeps running. Turn the plugin off and on in Settings > Plugins
to reload it.

## 11. Debugging

Run with `-vv`: every plugin line is tagged `[plugin <id>]`, including Lua errors with a
traceback and the reason a load or a fault happened. `helix.log.debug` shows at `-vv`
(`docs/devel/LOGGING.md`).

- Settings > Plugins shows each plugin's status and reason: *loaded*, *disabled*,
  *needs approval*, *invalid* (manifest or XML rejected, with the reason), *incompatible*,
  *over memory budget*, *faulted*.
- Read a subject exactly: with the app's remote control running,
  `helix-screen ctl text <id>__<subject>` (`docs/devel/HELIXCTL.md`; pin the socket as
  that doc says when several instances run).
- `HELIX_MOCK_PLUGINS_DIR=<dir>` makes a `--test` run serve that directory as the
  printer's `config/helixscreen/plugins/` folder, exercising install and sync end to end
  without a printer (`docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`).
- The `--test` mock printer defines the LED effects `breathing`, `fire_comet`, `rainbow`
  and `static_white`, so led-effects works against it out of the box.

## 12. Worked examples

**temp-spark** (`examples/plugins/temp-spark`) - no permissions. Subjects are the whole
output, and the sample timer drives them:

```lua
local value_text = helix.subject.string("value", "--")
...
timer = helix.timer.every(helix.settings.get("interval_s") * 1000, tick)
```

Live readings come from `helix.printer.watch` with no permission, and the window is
seeded from Moonraker's own history through the read-only
`server.temperature_store`:

```lua
local ok, e = pcall(helix.printer.watch, h.temp, function(v) ... end)
local store, err = helix.moonraker.call("server.temperature_store", {include_monitors = false})
```

The tile's tap is one `event_cb` in XML and one handler in Lua:

```lua
helix.ui.on("open", function() helix.ui.overlay("temp-spark__detail") end)
```

**led-effects** (`examples/plugins/led-effects`) - the `gcode` permission. The user-typed
effect name is validated before it reaches a command line: anchored character class,
length bound tied to subscribe's 64-byte object-name cap, refuse and toast otherwise:

```lua
if type(name) == "string" and #name <= 64 - #"led_effect " and name:match("^[%w_]+$") then
```

The toggle sends G-code and lets the live subscription report the result, so the tile
never guesses its own state:

```lua
local ok, err = helix.gcode(line)
sub = helix.moonraker.subscribe({[object] = {"enabled"}}, function(status) ... end)
```

The plugin's id contains a hyphen, so its XML uses `bind_flag_if_eq` instead of `cond`,
and `on_size` adapts the tile to its width in whole cells:

```lua
helix.widget("tile", { on_size = function(cols) wide:set(cols >= 2 and 1 or 0) end })
```

The maintainer's view of the same system - the sync pipeline, the sandbox internals, the
subscription union - is `docs/devel/architecture/12-system-services.md`.
