# Lua Plugin System, Phase 1 (Runtime) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `dlopen` C++ plugin system with a sandboxed Lua 5.4 runtime that loads plugins from `HELIX_PLUGIN_DIR` and gives them subjects, XML event handlers, printer state, Moonraker, G-code, HTTP, storage and settings, all under tests.

**Architecture:** One `lua_State` per plugin on the LVGL main thread, with a capped allocator and a clock-checked instruction hook. Every entry into Lua is a coroutine, so async bindings yield and are resumed through `LifetimeToken::defer`. Bindings reach the app only through a `PluginBackend` struct of `std::function`s, so tests substitute a fake and never need a Moonraker mock. `PluginHost` owns discovery, enable state, the memory budget, load, unload and faults.

**Tech Stack:** C++17, Lua 5.4.9 compiled as C++, LVGL 9.5 + helix-xml, Catch2, the project Makefile.

**Spec:** `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`. Read it first. This plan implements its Phase 1. Phases 2 to 5 get their own plans once this lands, written against the code this plan produces.

## Global Constraints

- Lua 5.4.9: submodule `lib/lua` from `https://github.com/lua/lua.git`, tag `v5.4.9`, compiled with `$(CXX) -x c++ $(SUBMODULE_CXXFLAGS)`.
- Left out of the Lua build: `lua.c onelua.c ltests.c linit.c liolib.c loslib.c loadlib.c ldblib.c`.
- Plugin id `^[a-z][a-z0-9-]{1,31}$`; every name a plugin registers is `<id>_<rest>`.
- Permissions are exactly `gcode`, `moonraker_write`, `http`, `storage`.
- Read-only `moonraker.call` allowlist: `printer.objects.query`, `printer.objects.list`, `server.info`, `server.files.list`, `server.files.metadata`, `machine.system_info`.
- Memory: 2 MB per plugin by default, `memory_mb` 1 to 64; all plugins together `min(MemTotal / 16, 64 MB)`.
- Time: hook every 10,000 instructions, 50 ms deadline per outermost entry; after an overrun the hook runs every instruction.
- A plugin faults on: an out-of-memory error reaching its entry, a time overrun, or 3 errors within 60 s.
- Storage file `<dir of settings.json>/plugin-data/<id>.json`, at most 256 KB.
- Settings block: `/plugins/enabled/<id> = {version, permissions}`, `/plugins/settings/<id>/<key>`.
- Every `src/plugin/*.cpp` and every new test file is wrapped in `#if HELIX_HAS_PLUGINS` / `#endif`, and every new `src/plugin/*.cpp` is listed in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` in the commit that creates it (`scripts/check_esp32_app_srcs.py` fails the commit hook otherwise).
- New files begin `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- spdlog only; `#include "hv/json.hpp"`; `json::parse(s, nullptr, false)`, never the throwing overload or `j.value()`; no RTTI; no `std::regex`; no development history in comments (CLAUDE.md § Comments).
- Lua runs only on the main thread. Background callbacks cross through `LifetimeToken::defer`.
- Worktree commits use explicit paths; never `git add -A` (MEMORY.md). Run `git show --stat HEAD` after every commit.

## Review Focus

- **A plugin that loops forever inside `pcall`** (`while true do pcall(function() while true do end end) end`) is stopped and disabled instead of hanging the UI. Pinned in Task 5.
- **A Lua observer that sets the subject it observes** recurses through LVGL's notify path; the runtime stops at its nesting cap with an error instead of overflowing the C stack. Pinned in Tasks 6 and 9.
- **An async reply that arrives after the plugin was unloaded or faulted** (a slow G-code, an HTTP timeout) is dropped without touching freed memory. Pinned in Tasks 6 and 11.
- **XML that names another plugin's handler, or a plugin that is not loaded** (`user_data="other-plugin_start"`), is ignored rather than dispatched into the wrong runtime. Pinned in Tasks 9 and 13.
- **A settings entry granting fewer permissions than the manifest now asks for** keeps the plugin unloaded with a reason instead of loading it with the smaller set. Pinned in Task 13.

---

## File Structure

| File | Responsibility |
|---|---|
| `lib/lua` (submodule) | Lua 5.4.9 sources |
| `include/lua_include.h` | The one place Lua headers are included |
| `include/plugin_permissions.h`, `src/plugin/plugin_permissions.cpp` | Permission enum, parsing, growth, read-only allowlist. Pure |
| `include/plugin_manifest.h`, `src/plugin/plugin_manifest.cpp` | `manifest.json` parsing and validation, id and name rules. Pure |
| `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp` | `lua_State` lifecycle, allocator, sandbox, `require`, time hook, coroutine entries, async suspend and resume, faults |
| `include/plugin_backend.h`, `src/plugin/plugin_backend_app.cpp` | The app services bindings may use, and their production wiring |
| `include/lua_bindings.h` | `PluginContext`, `install_*` per binding file, shared helpers |
| `src/plugin/lua_bind_core.cpp` | `helix.log`, `helix.json`, `helix.timer`, `helix.sleep` |
| `src/plugin/lua_bind_ui.cpp` | `helix.subject`, `helix.ui.on/toast/confirm`, event parsing and dispatch |
| `src/plugin/lua_bind_printer.cpp` | `helix.printer.get/watch` and the stable field table |
| `src/plugin/lua_bind_moonraker.cpp` | `helix.moonraker.*`, `helix.gcode`, permission and resolver helpers |
| `src/plugin/lua_bind_io.cpp` | `helix.http`, `helix.storage`, `helix.settings` |
| `include/plugin_host.h`, `src/plugin/plugin_host.cpp` | Discovery, enable state, budget, load, unload, faults, the `plugin_event` callback |
| `tests/test_helpers/plugin_test_support.h` | `TestRuntime`, `FakeBackend`, `BoundRuntime`, `TempDir` |
| `tests/fixtures/plugins/` | `require-test/`, `hello/`, `bad-name/`, `looper/` |

---

### Task 0: Worktree and coordination

MAJOR work (CLAUDE.md § Work Classification): it happens in a worktree.

- [ ] **Step 1: Create the worktree**

```bash
cd /home/pbrown/Code/Printing/helixscreen
scripts/setup-worktree.sh feature/lua-plugins
cd .worktrees/lua-plugins
```

- [ ] **Step 2: Claim it and tell peers**

```bash
scripts/helix-claim take worktree:lua-plugins "lua plugin runtime, phase 1" --pid $PPID
```

Run `ListAgents` and send each live peer one line: branch `feature/lua-plugins` deletes `src/plugin/`, `include/plugin_*.h` and `include/injection_point_manager.h`, and edits `src/application/application.cpp`, `src/ui/ui_panel_print_status.cpp`, `Makefile`, `mk/rules.mk`, `mk/tests.mk`.

- [ ] **Step 3: Confirm the base builds**

Run: `make && make test`
Expected: both succeed. If not, stop and report; do not build on a broken base.

---

### Task 1: Remove the C++ plugin system

**Files:**
- Delete: `src/plugin/` (all five `.cpp`), `include/{plugin_api,plugin_events,plugin_manager,plugin_registry,injection_point_manager}.h`, `plugins/led-effects/`, `tests/unit/test_plugin_api_subjects.cpp`, `tests/unit/test_injection_point.cpp`, `docs/devel/PLUGIN_DEVELOPMENT.md`
- Modify: `src/application/application.cpp` (include at :223; `init_plugins()` definition and call around :2213-2286; `on_moonraker_connected` block at :3680-3683; shutdown block at :4883-4887; restart block at :5226-5231)
- Modify: `include/application.h` (`m_plugin_manager`, `init_plugins`)
- Modify: `src/ui/ui_panel_print_status.cpp` (include at :46; `register_point("print_status_extras", ...)` at :1189)
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs.txt` (:108), `app_srcs_excluded.txt` (:99-102), `helixapp_platform_stubs2.cpp` (:25 and any `PluginManager` stubs)
- Modify: `docs/devel/CLAUDE.md` (:121), `docs/devel/architecture/12-system-services.md` (:86, :184), `docs/devel/architecture/03-threading-lifetime.md` (:185), `docs/devel/HELIX_XML_FORK.md` (:116)

Leave `helix_plugin_installer.*`, `ui_plugin_install_modal.*` and their tests alone: they install the Moonraker-side `helix_print` plugin, a different system.

**Interfaces:**
- Consumes: nothing.
- Produces: an app with no plugin code; `HELIX_HAS_PLUGINS` still defined (`Makefile:1151`, `:1177`).

- [ ] **Step 1: Delete the files**

```bash
git rm -r src/plugin include/plugin_api.h include/plugin_events.h include/plugin_manager.h \
  include/plugin_registry.h include/injection_point_manager.h plugins/led-effects \
  tests/unit/test_plugin_api_subjects.cpp tests/unit/test_injection_point.cpp \
  docs/devel/PLUGIN_DEVELOPMENT.md
```

- [ ] **Step 2: Remove the call sites**

In `src/application/application.cpp` delete `#include "plugin_manager.h"`, the `init_plugins()` definition and its call, the `m_plugin_manager->on_moonraker_connected()` block with its comment, and both `m_plugin_manager->unload_all(); m_plugin_manager.reset();` blocks with their comments. In `include/application.h` delete `m_plugin_manager` and `init_plugins()`.

Run: `grep -rn "plugin_manager\|PluginManager\|injection_point\|InjectionPoint\|init_plugins" src include firmware tests`
Expected: no output.

In `src/ui/ui_panel_print_status.cpp` delete the include and the `register_point` call.

Run: `grep -rn "print_status_extras" src ui_xml`
Expected: if the only remaining hit is an XML element, delete that element (it existed only for injection). If C++ still looks it up for another reason, keep it.

- [ ] **Step 3: Update the ESP32 source manifests**

Delete `src/plugin/injection_point_manager.cpp` from `app_srcs.txt` and the four `src/plugin/*.cpp` lines from `app_srcs_excluded.txt`. Delete `#include "plugin_manager.h"` and any `PluginManager` stub definitions from `helixapp_platform_stubs2.cpp`.

Run: `python3 scripts/check_esp32_app_srcs.py`
Expected: exit 0, no "stale" or "undecided" lines.

- [ ] **Step 4: Update the docs**

Delete the `PLUGIN_DEVELOPMENT.md` row from `docs/devel/CLAUDE.md`. In `12-system-services.md`, `03-threading-lifetime.md` and `HELIX_XML_FORK.md`, replace each plugin-system passage with: "Plugins are being rebuilt on sandboxed Lua; the design is `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`." Remove any sentence describing `PluginAPI`, injection points or `.so` loading.

Run: `grep -rn "PLUGIN_DEVELOPMENT\|PluginAPI\|inject_widget" docs README.md CLAUDE.md`
Expected: hits only under `docs/devel/plans/`.

- [ ] **Step 5: Build and test**

Run: `make && make test && make t F='[print_status]'`
Expected: all succeed.

- [ ] **Step 6: Commit**

```bash
git add src/application/application.cpp include/application.h src/ui/ui_panel_print_status.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs.txt \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt \
  firmware/helixscreen-esp32/components/helixapp/helixapp_platform_stubs2.cpp \
  docs/devel/CLAUDE.md docs/devel/architecture/12-system-services.md \
  docs/devel/architecture/03-threading-lifetime.md docs/devel/HELIX_XML_FORK.md
git commit -m "refactor(plugin): remove the dlopen C++ plugin system ahead of the Lua runtime"
git show --stat HEAD
```

Add the XML file to `git add` if Step 2 removed the extras element.

---

### Task 2: Vendor Lua 5.4.9, compiled as C++

**Files:**
- Create: submodule `lib/lua`; `include/lua_include.h`
- Modify: `Makefile` (after `HELIX_HAS_PLUGINS ?= 1` at :1151), `mk/rules.mk` (rule beside the quirc rule at :445; `$(TARGET)` prerequisites at :181), `mk/tests.mk` (:1046), `mk/egl-link.mk` (:108), `mk/pi-dual-link.mk` (:229), `mk/tools.mk` (:123)
- Test: `tests/unit/test_lua_vendored.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `#include "lua_include.h"` gives the Lua C API with Lua errors thrown as C++ exceptions; `$(LUA_OBJS)` is empty when `HELIX_HAS_PLUGINS=0`.

- [ ] **Step 1: Write the failing test**

`tests/unit/test_lua_vendored.cpp`. Copy the Catch2 include line from a neighbouring test such as `tests/unit/test_afc_console_corpus.cpp` and use it in place of the one below if it differs.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_include.h"

#include "../catch_amalgamated.hpp"

TEST_CASE("vendored Lua runs a chunk and reports a runtime error", "[lua]") {
    lua_State* L = luaL_newstate();
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);

    REQUIRE(luaL_dostring(L, "return 6 * 7") == LUA_OK);
    CHECK(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);

    CHECK(luaL_dostring(L, "error('boom')") == LUA_ERRRUN);
    lua_close(L);
}

// Only a C++ build of Lua runs this destructor; a C build longjmps past it.
TEST_CASE("a Lua error unwinds C++ destructors in a C function", "[lua]") {
    static int destroyed = 0;
    struct Probe {
        ~Probe() { ++destroyed; }
    };
    destroyed = 0;

    lua_State* L = luaL_newstate();
    lua_pushcfunction(L, [](lua_State* s) -> int {
        Probe p;
        return luaL_error(s, "raised");
    });
    CHECK(lua_pcall(L, 0, 0, 0) == LUA_ERRRUN);
    CHECK(destroyed == 1);
    lua_close(L);
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[lua]'`
Expected: compile error, `lua_include.h` not found.

- [ ] **Step 3: Add the submodule**

```bash
git submodule add https://github.com/lua/lua.git lib/lua
git -C lib/lua checkout v5.4.9
```

`scripts/setup-worktree.sh` symlinks new submodules on its own. No patches are needed; `.claude/rules/submodules.md` governs any later one.

- [ ] **Step 4: Add the header**

`include/lua_include.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Lua is compiled as C++ (LUA_OBJS in the Makefile), so its API has C++ linkage and a Lua
// error is a C++ exception. Include it only through this header.
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
```

`-isystem lib` is already in `INCLUDES` (`Makefile:835`), so `lua/lua.h` resolves without a new include path.

- [ ] **Step 5: Add the build rules**

In `Makefile`, directly after `HELIX_HAS_PLUGINS ?= 1`:

```make
# Lua 5.4 for the plugin runtime. Compiled as C++ so a Lua error is an exception and a
# binding's destructors run. The io, os, package and debug libraries stay out of the
# binary, which keeps them out of every plugin's reach.
LUA_DIR := lib/lua
ifeq ($(HELIX_HAS_PLUGINS),1)
LUA_EXCLUDED := lua.c onelua.c ltests.c linit.c liolib.c loslib.c loadlib.c ldblib.c
LUA_SRCS := $(filter-out $(addprefix $(LUA_DIR)/,$(LUA_EXCLUDED)),$(wildcard $(LUA_DIR)/*.c))
LUA_OBJS := $(patsubst $(LUA_DIR)/%.c,$(OBJ_DIR)/lua/%.o,$(LUA_SRCS))
else
LUA_OBJS :=
endif
```

`mk/tests.mk` and `mk/rules.mk` are included at `Makefile:1424` and `:1439`, after this block, so their prerequisite lists see `LUA_OBJS`.

In `mk/rules.mk` beside the quirc rule at :445, add a rule with the same echo and `$(Q)` lines as that rule, compiling as C++:

```make
$(OBJ_DIR)/lua/%.o: $(LUA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(Q)$(CXX) -x c++ $(SUBMODULE_CXXFLAGS) $(INCLUDES) -c $< -o $@
```

Add `$(LUA_OBJS)` next to `$(QUIRC_OBJS)` at `mk/rules.mk:181`, `mk/tests.mk:1046`, `mk/egl-link.mk:108`, `mk/pi-dual-link.mk:229` and `mk/tools.mk:123`. At each, read the recipe below: if its link command names objects explicitly instead of using `$^`, add `$(LUA_OBJS)` to the command too.

- [ ] **Step 6: Run to see it pass**

Run: `make t F='[lua]'`
Expected: 2 test cases pass.

- [ ] **Step 7: Prove the app links Lua and the flag-off build does not compile it**

Run: `make && nm -C build/bin/helix-screen | grep -c "lua_newstate"`
Expected: at least 1.

Run: `make -n HELIX_HAS_PLUGINS=0 | grep -c "lib/lua/"`
Expected: `0`.

- [ ] **Step 8: Commit**

```bash
git add .gitmodules lib/lua Makefile mk/rules.mk mk/tests.mk mk/egl-link.mk mk/pi-dual-link.mk \
  mk/tools.mk include/lua_include.h tests/unit/test_lua_vendored.cpp
git commit -m "build(plugin): vendor Lua 5.4.9 as lib/lua, compiled as C++"
git show --stat HEAD
```

---

### Task 3: Manifest and permissions

**Files:**
- Create: `include/plugin_permissions.h`, `src/plugin/plugin_permissions.cpp`, `include/plugin_manifest.h`, `src/plugin/plugin_manifest.cpp`
- Modify: `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_manifest.cpp`

**Interfaces:**
- Consumes: nothing beyond nlohmann JSON.
- Produces:

```cpp
namespace helix::plugin {
enum class Permission { Gcode, MoonrakerWrite, Http, Storage };
using PermissionSet = std::set<Permission>;
std::optional<Permission> permission_from_string(std::string_view name);
const char* permission_name(Permission p);
std::vector<Permission> permission_growth(const PermissionSet& granted, const PermissionSet& requested);
bool is_readonly_moonraker_method(std::string_view method);

using json = nlohmann::json;
enum class SettingType { Bool, Int, Float, Enum, String, Action, Info };
struct SettingDecl;   // key, label, type, default_value, min, max, options, callback, subject
struct Manifest;      // id, name, version, author, description, helix_version, permissions,
                      // memory_mb (default 2), settings, settings_overlay
struct ManifestParse { std::optional<Manifest> manifest; std::vector<std::string> errors; };
ManifestParse parse_manifest(const std::string& text);
bool is_valid_plugin_id(std::string_view id);
bool is_owned_name(std::string_view id, std::string_view name);
std::string_view owner_of(std::string_view name);
}
```

The manifest's `widgets` array is ignored in this phase (Phase 2 parses it), and so is any unknown key.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_plugin_manifest.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_manifest.h"
#include "plugin_permissions.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;

namespace {
bool has_error_containing(const ManifestParse& r, const std::string& needle) {
    for (const auto& e : r.errors) {
        if (e.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

ManifestParse with_setting(const std::string& setting) {
    return parse_manifest(R"({"id":"ab","name":"n","version":"1","settings":[)" + setting + "]}");
}
} // namespace

TEST_CASE("plugin ids", "[plugin][manifest]") {
    CHECK(is_valid_plugin_id("orca-cal"));
    CHECK(is_valid_plugin_id("ab"));
    CHECK_FALSE(is_valid_plugin_id("a"));
    CHECK_FALSE(is_valid_plugin_id("orca_cal"));
    CHECK_FALSE(is_valid_plugin_id("Orca"));
    CHECK_FALSE(is_valid_plugin_id("1orca"));
    CHECK_FALSE(is_valid_plugin_id(std::string(33, 'a')));
    CHECK(is_valid_plugin_id(std::string(32, 'a')));
}

TEST_CASE("owned names need the id, an underscore and a rest", "[plugin][manifest]") {
    CHECK(is_owned_name("orca-cal", "orca-cal_status"));
    CHECK_FALSE(is_owned_name("orca-cal", "orca-cal_"));
    CHECK_FALSE(is_owned_name("orca-cal", "orca-calx_status"));
    CHECK_FALSE(is_owned_name("orca", "orca-cal_status"));
    CHECK_FALSE(is_owned_name("orca-cal", "status"));
    CHECK(owner_of("orca-cal_my_status") == "orca-cal");
    CHECK(owner_of("nounderscore").empty());
}

TEST_CASE("a complete manifest parses", "[plugin][manifest]") {
    auto r = parse_manifest(R"({
      "id": "orca-cal", "name": "Orca Calibration", "version": "1.2.0",
      "author": "someone", "description": "d", "helix_version": ">=1.1",
      "permissions": ["gcode", "moonraker_write"], "memory_mb": 4,
      "settings": [
        {"key": "companion", "type": "string", "label": "Companion"},
        {"key": "auto_apply", "type": "bool", "label": "Auto", "default": false},
        {"key": "step", "type": "int", "label": "Step", "min": 1, "max": 20, "default": 5},
        {"key": "ratio", "type": "float", "label": "Ratio", "min": 0.5, "max": 1.5, "default": 0.95},
        {"key": "test", "type": "enum", "label": "Test",
         "options": ["temperature", "flow"], "default": "flow"},
        {"key": "ping", "type": "action", "label": "Ping", "callback": "orca-cal_ping"},
        {"key": "state", "type": "info", "label": "State", "subject": "orca-cal_state"}
      ],
      "settings_overlay": "orca-cal_settings",
      "widgets": [{"anything": "ignored in phase 1"}]
    })");
    REQUIRE(r.errors.empty());
    REQUIRE(r.manifest);
    CHECK(r.manifest->id == "orca-cal");
    CHECK(r.manifest->helix_version == ">=1.1");
    CHECK(r.manifest->memory_mb == 4);
    CHECK(r.manifest->permissions == PermissionSet{Permission::Gcode, Permission::MoonrakerWrite});
    CHECK(r.manifest->settings_overlay == "orca-cal_settings");
    REQUIRE(r.manifest->settings.size() == 7);
    CHECK(r.manifest->settings[2].type == SettingType::Int);
    CHECK(r.manifest->settings[2].default_value == 5);
    CHECK(r.manifest->settings[3].max == 1.5);
    CHECK(r.manifest->settings[4].options == std::vector<std::string>{"temperature", "flow"});
    CHECK(r.manifest->settings[5].callback == "orca-cal_ping");
}

TEST_CASE("memory_mb defaults to 2 and is bounded", "[plugin][manifest]") {
    std::string base = R"({"id":"ab","name":"n","version":"1")";
    CHECK(parse_manifest(base + "}").manifest->memory_mb == 2);
    CHECK(parse_manifest(base + R"(,"memory_mb":64})").manifest->memory_mb == 64);
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":0})"), "memory_mb"));
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":65})"), "memory_mb"));
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":"4"})"), "memory_mb"));
}

TEST_CASE("manifest errors", "[plugin][manifest]") {
    CHECK(has_error_containing(parse_manifest("{not json"), "not valid JSON"));
    CHECK(has_error_containing(parse_manifest("[]"), "must be an object"));
    CHECK(has_error_containing(parse_manifest(R"({"name":"n","version":"1"})"), "'id'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"Bad","name":"n","version":"1"})"), "'id'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"ab","version":"1"})"), "'name'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"ab","name":"n"})"), "'version'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"ab","name":"n","version":1})"), "'version'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","permissions":["root"]})"),
        "unknown permission 'root'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","permissions":"gcode"})"),
        "'permissions'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","settings_overlay":"x_view"})"),
        "settings_overlay"));
    CHECK_FALSE(parse_manifest(R"({"id":"ab","name":"n"})").manifest);
}

TEST_CASE("setting declaration errors", "[plugin][manifest]") {
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"colour","label":"l"})"), "type"));
    CHECK(has_error_containing(with_setting(R"({"key":"K!","type":"bool","label":"l"})"), "key"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"bool"})"), "label"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"int","label":"l","min":5,"max":5})"), "min"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"int","label":"l","min":1,"max":5,"default":9})"), "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"int","label":"l","min":1,"max":5,"default":2.5})"), "default"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"enum","label":"l","options":[]})"), "options"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"enum","label":"l","options":["a"],"default":"b"})"), "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"action","label":"l","callback":"other_x"})"), "callback"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"info","label":"l"})"), "subject"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"bool","label":"l","default":1})"), "default"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"string","label":"l","default":1})"), "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"bool","label":"l"},{"key":"k","type":"bool","label":"l"})"),
        "duplicate"));
    CHECK(has_error_containing(with_setting(R"("notanobject")"), "must be an object"));
}

TEST_CASE("permissions", "[plugin][manifest]") {
    CHECK(permission_from_string("gcode") == Permission::Gcode);
    CHECK(permission_from_string("moonraker_write") == Permission::MoonrakerWrite);
    CHECK(permission_from_string("http") == Permission::Http);
    CHECK(permission_from_string("storage") == Permission::Storage);
    CHECK_FALSE(permission_from_string("GCODE"));
    CHECK(std::string(permission_name(Permission::MoonrakerWrite)) == "moonraker_write");

    PermissionSet granted{Permission::Gcode};
    CHECK(permission_growth(granted, {Permission::Gcode}).empty());
    CHECK(permission_growth(granted, {}).empty());
    CHECK(permission_growth(granted, {Permission::Http, Permission::Gcode}) ==
          std::vector<Permission>{Permission::Http});

    CHECK(is_readonly_moonraker_method("printer.objects.query"));
    CHECK(is_readonly_moonraker_method("machine.system_info"));
    CHECK_FALSE(is_readonly_moonraker_method("printer.gcode.script"));
    CHECK_FALSE(is_readonly_moonraker_method("server.files.delete_file"));
    CHECK_FALSE(is_readonly_moonraker_method("printer.objects.query.extra"));
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[manifest]'`
Expected: compile error, `plugin_manifest.h` not found.

- [ ] **Step 3: Implement permissions**

`include/plugin_permissions.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <set>
#include <string_view>
#include <vector>

namespace helix::plugin {

/// A capability a plugin declares in its manifest before the matching binding works.
enum class Permission { Gcode, MoonrakerWrite, Http, Storage };

using PermissionSet = std::set<Permission>;

std::optional<Permission> permission_from_string(std::string_view name);
const char* permission_name(Permission p);

/// Permissions in `requested` that `granted` does not cover, in enum order. Non-empty means
/// the user consents again before the plugin loads.
std::vector<Permission> permission_growth(const PermissionSet& granted,
                                          const PermissionSet& requested);

/// Moonraker methods `helix.moonraker.call` accepts without `moonraker_write`.
bool is_readonly_moonraker_method(std::string_view method);

} // namespace helix::plugin
```

`src/plugin/plugin_permissions.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_permissions.h"

#include <array>
#include <utility>

namespace helix::plugin {

namespace {
constexpr std::array<std::pair<Permission, const char*>, 4> kNames{{
    {Permission::Gcode, "gcode"},
    {Permission::MoonrakerWrite, "moonraker_write"},
    {Permission::Http, "http"},
    {Permission::Storage, "storage"},
}};

constexpr std::array<std::string_view, 6> kReadonlyMethods{
    "printer.objects.query", "printer.objects.list",  "server.info",
    "server.files.list",     "server.files.metadata", "machine.system_info",
};
} // namespace

std::optional<Permission> permission_from_string(std::string_view name) {
    for (const auto& [p, n] : kNames) {
        if (name == n)
            return p;
    }
    return std::nullopt;
}

const char* permission_name(Permission p) {
    for (const auto& [q, n] : kNames) {
        if (p == q)
            return n;
    }
    return "?";
}

std::vector<Permission> permission_growth(const PermissionSet& granted,
                                          const PermissionSet& requested) {
    std::vector<Permission> grown;
    for (Permission p : requested) {
        if (!granted.count(p))
            grown.push_back(p);
    }
    return grown;
}

bool is_readonly_moonraker_method(std::string_view method) {
    for (auto m : kReadonlyMethods) {
        if (method == m)
            return true;
    }
    return false;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 4: Implement the manifest**

`include/plugin_manifest.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plugin_permissions.h"

#include "hv/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::plugin {

using json = nlohmann::json;

enum class SettingType { Bool, Int, Float, Enum, String, Action, Info };

/// One entry of the manifest's `settings` array.
struct SettingDecl {
    std::string key;
    std::string label;
    SettingType type = SettingType::Bool;
    json default_value;               ///< null when the manifest gives none
    double min = 0;                   ///< Int and Float
    double max = 0;                   ///< Int and Float
    std::vector<std::string> options; ///< Enum
    std::string callback;             ///< Action: handler name, owned by the plugin
    std::string subject;              ///< Info: subject name, owned by the plugin
};

struct Manifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string helix_version; ///< version constraint, empty when absent
    PermissionSet permissions;
    int memory_mb = 2;
    std::vector<SettingDecl> settings;
    std::string settings_overlay; ///< XML component name, empty when absent
};

/// `manifest` is set only when `errors` is empty.
struct ManifestParse {
    std::optional<Manifest> manifest;
    std::vector<std::string> errors;
};

ManifestParse parse_manifest(const std::string& text);

/// `^[a-z][a-z0-9-]{1,31}$`. No underscore, so the first `_` of a registered name
/// separates its owner.
bool is_valid_plugin_id(std::string_view id);

/// True for `<id>_<rest>` with a non-empty rest.
bool is_owned_name(std::string_view id, std::string_view name);

/// Everything before the first `_`; empty when there is none.
std::string_view owner_of(std::string_view name);

} // namespace helix::plugin
```

`src/plugin/plugin_manifest.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_manifest.h"

#include <algorithm>
#include <set>

namespace helix::plugin {

namespace {

bool is_lower_or_digit(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool is_valid_setting_key(std::string_view k) {
    if (k.empty() || k.size() > 64)
        return false;
    return std::all_of(k.begin(), k.end(), [](char c) { return is_lower_or_digit(c) || c == '_'; });
}

std::optional<SettingType> setting_type_from(const std::string& s) {
    static const std::pair<const char*, SettingType> kTypes[] = {
        {"bool", SettingType::Bool},     {"int", SettingType::Int},
        {"float", SettingType::Float},   {"enum", SettingType::Enum},
        {"string", SettingType::String}, {"action", SettingType::Action},
        {"info", SettingType::Info},
    };
    for (const auto& [n, t] : kTypes) {
        if (s == n)
            return t;
    }
    return std::nullopt;
}

void require_string(const json& j, const char* key, std::string& out,
                    std::vector<std::string>& errors) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        errors.push_back(std::string("'") + key + "' must be a non-empty string");
        return;
    }
    out = it->get<std::string>();
}

void optional_string(const json& j, const char* key, std::string& out,
                     std::vector<std::string>& errors) {
    auto it = j.find(key);
    if (it == j.end())
        return;
    if (!it->is_string()) {
        errors.push_back(std::string("'") + key + "' must be a string");
        return;
    }
    out = it->get<std::string>();
}

void check_range(SettingDecl& d, const json& s, std::vector<std::string>& local) {
    auto mn = s.find("min");
    auto mx = s.find("max");
    if (mn == s.end() || mx == s.end() || !mn->is_number() || !mx->is_number() ||
        mn->get<double>() >= mx->get<double>()) {
        local.push_back("'min' and 'max' must be numbers with min < max");
        return;
    }
    d.min = mn->get<double>();
    d.max = mx->get<double>();
    if (d.default_value.is_null())
        return;
    bool ok = d.default_value.is_number() && d.default_value.get<double>() >= d.min &&
              d.default_value.get<double>() <= d.max &&
              (d.type != SettingType::Int || d.default_value.is_number_integer());
    if (!ok)
        local.push_back("'default' must be a number within [min, max]");
}

void check_enum(SettingDecl& d, const json& s, std::vector<std::string>& local) {
    auto opts = s.find("options");
    if (opts == s.end() || !opts->is_array() || opts->empty()) {
        local.push_back("'options' must be a non-empty array of strings");
        return;
    }
    for (const auto& o : *opts) {
        if (!o.is_string()) {
            local.push_back("'options' must be a non-empty array of strings");
            return;
        }
        d.options.push_back(o.get<std::string>());
    }
    if (!d.default_value.is_null() &&
        (!d.default_value.is_string() ||
         std::find(d.options.begin(), d.options.end(), d.default_value.get<std::string>()) ==
             d.options.end()))
        local.push_back("'default' must be one of 'options'");
}

void parse_setting(const std::string& id, const json& s, size_t index, Manifest& m,
                   std::set<std::string>& seen, std::vector<std::string>& errors) {
    std::string where = "settings[" + std::to_string(index) + "]";
    if (!s.is_object()) {
        errors.push_back(where + " must be an object");
        return;
    }
    auto key_it = s.find("key");
    if (key_it == s.end() || !key_it->is_string() ||
        !is_valid_setting_key(key_it->get_ref<const std::string&>())) {
        errors.push_back(where + ": 'key' must match [a-z0-9_]{1,64}");
        return;
    }
    SettingDecl d;
    d.key = key_it->get<std::string>();
    if (!seen.insert(d.key).second) {
        errors.push_back(where + ": duplicate key '" + d.key + "'");
        return;
    }

    std::vector<std::string> local;
    require_string(s, "label", d.label, local);
    std::string type_name;
    require_string(s, "type", type_name, local);
    std::optional<SettingType> type = type_name.empty() ? std::nullopt : setting_type_from(type_name);
    if (!type_name.empty() && !type)
        local.push_back("unknown type '" + type_name + "'");
    if (type)
        d.type = *type;
    if (auto it = s.find("default"); it != s.end())
        d.default_value = *it;

    if (type == SettingType::Int || type == SettingType::Float) {
        check_range(d, s, local);
    } else if (type == SettingType::Enum) {
        check_enum(d, s, local);
    } else if (type == SettingType::Bool) {
        if (!d.default_value.is_null() && !d.default_value.is_boolean())
            local.push_back("'default' must be true or false");
    } else if (type == SettingType::String) {
        if (!d.default_value.is_null() && !d.default_value.is_string())
            local.push_back("'default' must be a string");
    } else if (type == SettingType::Action) {
        optional_string(s, "callback", d.callback, local);
        if (!is_owned_name(id, d.callback))
            local.push_back("'callback' must be named " + id + "_<name>");
    } else if (type == SettingType::Info) {
        optional_string(s, "subject", d.subject, local);
        if (!is_owned_name(id, d.subject))
            local.push_back("'subject' must be named " + id + "_<name>");
    }

    for (auto& e : local)
        errors.push_back(where + " ('" + d.key + "'): " + e);
    if (local.empty())
        m.settings.push_back(std::move(d));
}

} // namespace

bool is_valid_plugin_id(std::string_view id) {
    if (id.size() < 2 || id.size() > 32 || !(id[0] >= 'a' && id[0] <= 'z'))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return is_lower_or_digit(c) || c == '-'; });
}

bool is_owned_name(std::string_view id, std::string_view name) {
    return name.size() > id.size() + 1 && name.substr(0, id.size()) == id &&
           name[id.size()] == '_';
}

std::string_view owner_of(std::string_view name) {
    auto pos = name.find('_');
    return pos == std::string_view::npos ? std::string_view{} : name.substr(0, pos);
}

ManifestParse parse_manifest(const std::string& text) {
    ManifestParse r;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        r.errors.push_back("manifest.json is not valid JSON");
        return r;
    }
    if (!j.is_object()) {
        r.errors.push_back("manifest.json must be an object");
        return r;
    }

    Manifest m;
    require_string(j, "id", m.id, r.errors);
    if (!m.id.empty() && !is_valid_plugin_id(m.id))
        r.errors.push_back("'id' must match ^[a-z][a-z0-9-]{1,31}$");
    require_string(j, "name", m.name, r.errors);
    require_string(j, "version", m.version, r.errors);
    optional_string(j, "author", m.author, r.errors);
    optional_string(j, "description", m.description, r.errors);
    optional_string(j, "helix_version", m.helix_version, r.errors);

    if (auto it = j.find("permissions"); it != j.end()) {
        if (!it->is_array()) {
            r.errors.push_back("'permissions' must be an array of strings");
        } else {
            for (const auto& p : *it) {
                auto perm = p.is_string() ? permission_from_string(p.get<std::string>()) : std::nullopt;
                if (perm)
                    m.permissions.insert(*perm);
                else
                    r.errors.push_back("unknown permission '" +
                                       (p.is_string() ? p.get<std::string>() : p.dump()) + "'");
            }
        }
    }

    if (auto it = j.find("memory_mb"); it != j.end()) {
        if (!it->is_number_integer() || it->get<int>() < 1 || it->get<int>() > 64)
            r.errors.push_back("'memory_mb' must be an integer from 1 to 64");
        else
            m.memory_mb = it->get<int>();
    }

    if (auto it = j.find("settings"); it != j.end()) {
        if (!it->is_array()) {
            r.errors.push_back("'settings' must be an array");
        } else {
            std::set<std::string> seen;
            for (size_t i = 0; i < it->size(); ++i)
                parse_setting(m.id, (*it)[i], i, m, seen, r.errors);
        }
    }

    if (auto it = j.find("settings_overlay"); it != j.end() && !it->is_null()) {
        if (!it->is_string() || !is_owned_name(m.id, it->get<std::string>()))
            r.errors.push_back("'settings_overlay' must be a component named " + m.id + "_<name>");
        else
            m.settings_overlay = it->get<std::string>();
    }

    if (r.errors.empty())
        r.manifest = std::move(m);
    return r;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Append `src/plugin/plugin_permissions.cpp` and `src/plugin/plugin_manifest.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 5: Run to see it pass**

Run: `make t F='[manifest]'`
Expected: all 7 cases pass.

- [ ] **Step 6: Mutation check**

Run: `make mutate-diff`
Expected: each surviving mutant is equivalent or gets an assertion before commit. Name one killed mutation in the commit body.

- [ ] **Step 7: Commit**

```bash
git add include/plugin_permissions.h include/plugin_manifest.h src/plugin/plugin_permissions.cpp \
  src/plugin/plugin_manifest.cpp tests/unit/test_plugin_manifest.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): manifest and permission rules for Lua plugins"
git show --stat HEAD
```

---

### Task 4: LuaRuntime core (state, allocator, sandbox, require, errors)

**Files:**
- Create: `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp`, `tests/test_helpers/plugin_test_support.h`
- Create: `tests/fixtures/plugins/require-test/util.lua`, `tests/fixtures/plugins/require-test/lib/deep.lua`
- Modify: `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_runtime.cpp`

**Interfaces:**
- Consumes: `lua_include.h`; `AsyncLifetimeGuard`, `LifetimeToken` (`include/async_lifetime_guard.h`: `token()` :257, `invalidate()` :267, `LifetimeToken::defer(tag, fn)` :190, which skips `fn` once the guard is invalidated).
- Produces: the full `LuaRuntime` declaration below. Tasks 5 and 6 change bodies only, never the header.

- [ ] **Step 1: Write the header**

`include/lua_runtime.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lua_include.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix::plugin {

/// Counts errors in a sliding window. `record` returns true when this error reaches the
/// threshold.
class ErrorWindow {
  public:
    using Clock = std::chrono::steady_clock;
    ErrorWindow(size_t threshold, std::chrono::seconds window);
    bool record(Clock::time_point now);

  private:
    size_t threshold_;
    std::chrono::seconds window_;
    std::deque<Clock::time_point> times_;
};

/// `<dir>/<path>.lua` then `<dir>/lib/<path>.lua` for a `require` name of [A-Za-z0-9_]
/// segments joined by single dots. Empty for any other name.
std::vector<std::string> require_candidates(const std::string& plugin_dir, const std::string& name);

/// One plugin's Lua state. Main thread only, except `Pending::resolve`.
class LuaRuntime {
  public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        size_t memory_bytes = 2 * 1024 * 1024;
        std::chrono::milliseconds time_budget{50};
    };

    /// Called once, when the runtime faults. The runtime refuses further entries; the owner
    /// destroys it from a later main-loop turn, never from inside this call.
    using FaultHandler = std::function<void(const std::string& reason)>;

    /// Pushes values onto a coroutine and returns how many.
    using PushFn = std::function<int(lua_State*)>;

    /// A suspended async call. Copyable; `resolve` is thread-safe and one-shot.
    class Pending {
      public:
        void resolve(PushFn push_results) const;

      private:
        friend class LuaRuntime;
        Pending(LuaRuntime* rt, lua_State* co, LifetimeToken token);
        LuaRuntime* rt_;
        lua_State* co_;
        LifetimeToken token_;
        std::shared_ptr<std::atomic<bool>> done_;
    };

    LuaRuntime(std::string plugin_id, std::string plugin_dir, Limits limits, FaultHandler on_fault);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    /// The runtime that owns `L` or any of its coroutines.
    static LuaRuntime& from(lua_State* L);

    const std::string& plugin_id() const { return plugin_id_; }
    const std::string& plugin_dir() const { return plugin_dir_; }
    lua_State* state() const { return L_; }
    size_t memory_used() const { return used_; }
    bool faulted() const { return faulted_; }
    const std::string& fault_reason() const { return fault_reason_; }
    LifetimeToken token() const { return guard_.token(); }

    /// Loads text and runs it as a new entry. False if it failed to load or raised.
    bool run_string(const std::string& code, const std::string& chunk_name);
    /// `run_string` on `<plugin_dir>/<relative_path>`.
    bool run_file(const std::string& relative_path);

    /// Registry reference to a copy of the value at `index` of `L`.
    int ref_value(lua_State* L, int index);
    void unref(int ref);

    /// Calls the function behind `fn_ref` as a new entry. No-op once faulted.
    void invoke(int fn_ref, const PushFn& push_args = {});

    /// For a binding: runs `start` with a Pending for the running coroutine `co`, then
    /// yields it. Raises a Lua error when `co` is not an entry coroutine or cannot yield.
    /// Use as `return rt.await_async(L, ...);`.
    int await_async(lua_State* co, const std::function<void(Pending)>& start);

    /// Work that runs before lua_close, in reverse order of registration.
    void on_close(std::function<void()> fn);

    /// Logs a plugin error; the third within 60 s faults the plugin.
    void report_error(const std::string& message);

  private:
    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void budget_hook(lua_State* L, lua_Debug* ar);
    static int lua_require(lua_State* L);

    void install_sandbox();
    bool spawn(const PushFn& push_args);
    bool enter(lua_State* co, int nargs);
    void resume(lua_State* co, const PushFn& push_results);
    void drop(lua_State* co);
    void fault(const std::string& reason);

    static constexpr int kMaxEntryDepth = 8;
    static constexpr int kHookInterval = 10000;

    std::string plugin_id_;
    std::string plugin_dir_;
    Limits limits_;
    FaultHandler on_fault_;
    lua_State* L_ = nullptr;

    size_t used_ = 0;
    int depth_ = 0; ///< active entries; the memory cap applies only while > 0
    Clock::time_point deadline_{};
    bool yielded_for_async_ = false;
    bool killed_ = false;
    bool faulted_ = false;
    std::string fault_reason_;
    ErrorWindow errors_{3, std::chrono::seconds(60)};

    std::unordered_map<lua_State*, int> threads_; ///< live entry coroutine -> registry ref
    std::vector<std::function<void()>> closers_;
    AsyncLifetimeGuard guard_;
};

} // namespace helix::plugin
```

- [ ] **Step 2: Add the test support header**

`tests/test_helpers/plugin_test_support.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_PLUGINS

#include "lua_runtime.h"

#include <memory>
#include <string>

namespace helix::plugin::test {

/// A runtime whose faults are recorded instead of acted on.
struct TestRuntime {
    std::string fault;
    std::unique_ptr<LuaRuntime> rt;

    explicit TestRuntime(LuaRuntime::Limits limits = {},
                         std::string dir = "tests/fixtures/plugins/require-test",
                         std::string id = "test-plugin") {
        rt = std::make_unique<LuaRuntime>(std::move(id), std::move(dir), limits,
                                          [this](const std::string& r) { fault = r; });
    }

    bool run(const std::string& code) { return rt->run_string(code, "test"); }

    /// A global's value as Lua's tostring() prints it.
    std::string global(const char* name) {
        lua_State* L = rt->state();
        lua_getglobal(L, name);
        std::string s = luaL_tolstring(L, -1, nullptr);
        lua_pop(L, 2);
        return s;
    }
};

} // namespace helix::plugin::test

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 3: Add the require fixtures**

`tests/fixtures/plugins/require-test/util.lua`:

```lua
return { answer = 42 }
```

`tests/fixtures/plugins/require-test/lib/deep.lua`:

```lua
loads = (loads or 0) + 1
return { name = "deep" }
```

This directory has no `manifest.json`, so `PluginHost` (Task 13) skips it when it scans `tests/fixtures/plugins`.

- [ ] **Step 4: Write the failing tests**

`tests/unit/test_lua_runtime.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_helpers/plugin_test_support.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using helix::plugin::test::TestRuntime;

TEST_CASE("runtime runs code and exposes an empty helix table", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run("x = 6 * 7; kind = type(helix)"));
    CHECK(t.global("x") == "42");
    CHECK(t.global("kind") == "table");
}

TEST_CASE("sandbox removes host access", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run(R"(
        io_t, os_t, pkg_t, dbg_t = type(io), type(os), type(package), type(debug)
        dofile_t, loadfile_t, dump_t = type(dofile), type(loadfile), type(string.dump)
        local f, err = load("\27Lua", "bin")
        binary_rejected = (f == nil and err:find("binary") ~= nil)
        text_ok = load("return 5")() == 5
        env_ok = load("return y", "c", "t", { y = 9 })() == 9
        count_ok = type(collectgarbage("count")) == "number"
        stop_ok = pcall(collectgarbage, "stop")
    )"));
    for (const char* g : {"io_t", "os_t", "pkg_t", "dbg_t", "dofile_t", "loadfile_t", "dump_t"})
        CHECK(t.global(g) == "nil");
    CHECK(t.global("binary_rejected") == "true");
    CHECK(t.global("text_ok") == "true");
    CHECK(t.global("env_ok") == "true");
    CHECK(t.global("count_ok") == "true");
    CHECK(t.global("stop_ok") == "false");
}

TEST_CASE("require resolves inside the plugin and caches", "[plugin][lua_runtime]") {
    TestRuntime t;
    REQUIRE(t.run(R"(
        a = require("util").answer
        d1 = require("deep"); d2 = require("deep")
        same = (d1 == d2)
    )"));
    CHECK(t.global("a") == "42");
    CHECK(t.global("same") == "true");
    CHECK(t.global("loads") == "1");
    CHECK_FALSE(t.run(R"(require("missing"))"));
}

TEST_CASE("require names that escape the plugin are refused", "[plugin][lua_runtime]") {
    CHECK(require_candidates("/p", "../x").empty());
    CHECK(require_candidates("/p", "/etc/passwd").empty());
    CHECK(require_candidates("/p", "a..b").empty());
    CHECK(require_candidates("/p", ".a").empty());
    CHECK(require_candidates("/p", "a.").empty());
    CHECK(require_candidates("/p", "").empty());
    CHECK(require_candidates("/p", "a.b") == std::vector<std::string>{"/p/a/b.lua", "/p/lib/a/b.lua"});
}

TEST_CASE("a runtime error is reported, not fatal", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.run("error('first')"));
    CHECK_FALSE(t.rt->faulted());
    CHECK(t.run("ok = true"));
}

TEST_CASE("three errors inside a minute fault the plugin", "[plugin][lua_runtime]") {
    TestRuntime t;
    t.run("error('1')");
    t.run("error('2')");
    CHECK_FALSE(t.rt->faulted());
    t.run("error('3')");
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("errors") != std::string::npos);
    CHECK_FALSE(t.run("late = true"));
    CHECK(t.global("late") == "nil");
}

TEST_CASE("error window slides", "[plugin][lua_runtime]") {
    ErrorWindow w(3, std::chrono::seconds(60));
    auto t0 = ErrorWindow::Clock::time_point{};
    CHECK_FALSE(w.record(t0));
    CHECK_FALSE(w.record(t0 + std::chrono::seconds(30)));
    CHECK_FALSE(w.record(t0 + std::chrono::seconds(61)));
    CHECK(w.record(t0 + std::chrono::seconds(62)));
}

TEST_CASE("out of memory reaching the entry faults the plugin", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    CHECK_FALSE(t.run("local t = {} while true do t[#t + 1] = string.rep('x', 1024) end"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
}

TEST_CASE("out of memory caught by the plugin does not fault it", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    REQUIRE(t.run(R"(
        caught = not pcall(function()
            local t = {} while true do t[#t + 1] = string.rep('x', 1024) end
        end)
        collectgarbage("collect")
    )"));
    CHECK(t.global("caught") == "true");
    CHECK_FALSE(t.rt->faulted());
    CHECK(t.rt->memory_used() < limits.memory_bytes);
}

TEST_CASE("closers run in reverse before the state closes", "[plugin][lua_runtime]") {
    std::vector<int> order;
    {
        TestRuntime t;
        t.rt->on_close([&] { order.push_back(1); });
        t.rt->on_close([&] { order.push_back(2); });
    }
    CHECK(order == std::vector<int>{2, 1});
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 5: Run to see it fail**

Run: `make t F='[lua_runtime]'`
Expected: compile error, `lua_runtime.h` has no implementation (undefined references at link).

- [ ] **Step 6: Implement**

`src/plugin/lua_runtime.cpp`. The `enter`, `budget_hook`, `await_async`, `resume` and `Pending::resolve` bodies here are this task's versions; Tasks 5 and 6 replace them.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_runtime.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace helix::plugin {

namespace {

// Trims the base library. Runs as trusted code before any plugin code.
constexpr const char* kSandboxPrelude = R"(
local load, collect = load, collectgarbage
dofile, loadfile, string.dump = nil, nil, nil
_G.load = function(chunk, name, _, ...)
    if select('#', ...) > 0 then return load(chunk, name, "t", (...)) end
    return load(chunk, name, "t")
end
local allowed = { count = true, collect = true, step = true }
_G.collectgarbage = function(opt, ...)
    opt = opt or "collect"
    if not allowed[opt] then
        error("collectgarbage('" .. tostring(opt) .. "') is not available to plugins", 2)
    end
    return collect(opt, ...)
end
)";

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

const char kLoadedKey = 0; // its address keys the require cache in the registry

} // namespace

ErrorWindow::ErrorWindow(size_t threshold, std::chrono::seconds window)
    : threshold_(threshold), window_(window) {}

bool ErrorWindow::record(Clock::time_point now) {
    times_.push_back(now);
    while (!times_.empty() && now - times_.front() > window_)
        times_.pop_front();
    return times_.size() >= threshold_;
}

std::vector<std::string> require_candidates(const std::string& plugin_dir, const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.')
        return {};
    std::string rel;
    char prev = 0;
    for (char c : name) {
        bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_';
        if (c == '.') {
            if (prev == '.')
                return {};
            rel += '/';
        } else if (word) {
            rel += c;
        } else {
            return {};
        }
        prev = c;
    }
    return {plugin_dir + "/" + rel + ".lua", plugin_dir + "/lib/" + rel + ".lua"};
}

LuaRuntime::Pending::Pending(LuaRuntime* rt, lua_State* co, LifetimeToken token)
    : rt_(rt), co_(co), token_(std::move(token)), done_(std::make_shared<std::atomic<bool>>(false)) {}

LuaRuntime::LuaRuntime(std::string plugin_id, std::string plugin_dir, Limits limits,
                       FaultHandler on_fault)
    : plugin_id_(std::move(plugin_id)), plugin_dir_(std::move(plugin_dir)), limits_(limits),
      on_fault_(std::move(on_fault)) {
    L_ = lua_newstate(&LuaRuntime::alloc, this);
    *static_cast<LuaRuntime**>(lua_getextraspace(L_)) = this;
    lua_atpanic(L_, [](lua_State* L) -> int {
        spdlog::critical("[plugin] unprotected Lua error: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)");
        return 0; // Lua aborts when the panic handler returns
    });
    install_sandbox();
    lua_sethook(L_, &LuaRuntime::budget_hook, LUA_MASKCOUNT, kHookInterval);
}

LuaRuntime::~LuaRuntime() {
    guard_.invalidate();
    for (auto it = closers_.rbegin(); it != closers_.rend(); ++it)
        (*it)();
    closers_.clear();
    lua_close(L_);
}

LuaRuntime& LuaRuntime::from(lua_State* L) {
    return **static_cast<LuaRuntime**>(lua_getextraspace(L));
}

void* LuaRuntime::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* rt = static_cast<LuaRuntime*>(ud);
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        std::free(ptr);
        rt->used_ -= old;
        return nullptr;
    }
    // Only a protected entry may be refused. A refusal anywhere else reaches Lua's panic
    // handler, and the setup work around entries is small and bounded.
    if (nsize > old && rt->depth_ > 0 && rt->used_ - old + nsize > rt->limits_.memory_bytes)
        return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p)
        rt->used_ = rt->used_ - old + nsize;
    return p;
}

void LuaRuntime::install_sandbox() {
    static const luaL_Reg kLibs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const auto& lib : kLibs) {
        luaL_requiref(L_, lib.name, lib.func, 1);
        lua_pop(L_, 1);
    }
    if (luaL_dostring(L_, kSandboxPrelude) != LUA_OK) {
        spdlog::critical("[plugin] sandbox prelude failed: {}", lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    lua_newtable(L_);
    lua_rawsetp(L_, LUA_REGISTRYINDEX, &kLoadedKey);
    lua_pushcfunction(L_, &LuaRuntime::lua_require);
    lua_setglobal(L_, "require");
    lua_newtable(L_);
    lua_setglobal(L_, "helix");
}

int LuaRuntime::lua_require(lua_State* L) {
    auto& rt = from(L);
    std::string name = luaL_checkstring(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kLoadedKey);
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    auto candidates = require_candidates(rt.plugin_dir_, name);
    if (candidates.empty())
        return luaL_error(L, "require('%s'): not a module name", name.c_str());
    for (const auto& path : candidates) {
        if (!file_exists(path))
            continue;
        if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK)
            return lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
        return 1;
    }
    return luaL_error(L, "require('%s'): module not found in the plugin", name.c_str());
}

bool LuaRuntime::run_string(const std::string& code, const std::string& chunk_name) {
    if (faulted_)
        return false;
    if (luaL_loadbufferx(L_, code.data(), code.size(), chunk_name.c_str(), "t") != LUA_OK) {
        std::string msg = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        report_error(msg);
        return false;
    }
    return spawn({});
}

bool LuaRuntime::run_file(const std::string& relative_path) {
    std::ifstream in(plugin_dir_ + "/" + relative_path, std::ios::binary);
    if (!in) {
        report_error("cannot read " + relative_path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return run_string(ss.str(), "@" + relative_path);
}

int LuaRuntime::ref_value(lua_State* L, int index) {
    lua_pushvalue(L, index);
    if (L != L_)
        lua_xmove(L, L_, 1);
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

void LuaRuntime::unref(int ref) {
    luaL_unref(L_, LUA_REGISTRYINDEX, ref);
}

void LuaRuntime::invoke(int fn_ref, const PushFn& push_args) {
    if (faulted_)
        return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, fn_ref);
    spawn(push_args);
}

void LuaRuntime::on_close(std::function<void()> fn) {
    closers_.push_back(std::move(fn));
}

// The function to run is on top of L_'s stack.
bool LuaRuntime::spawn(const PushFn& push_args) {
    lua_State* co = lua_newthread(L_);
    int thread_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    lua_xmove(L_, co, 1);
    int nargs = push_args ? push_args(co) : 0;
    threads_[co] = thread_ref;
    return enter(co, nargs);
}

bool LuaRuntime::enter(lua_State* co, int nargs) {
    ++depth_;
    int nres = 0;
    int status = lua_resume(co, L_, nargs, &nres);
    --depth_;
    if (status == LUA_OK) {
        lua_pop(co, nres);
        drop(co);
        return true;
    }
    if (status == LUA_YIELD) {
        lua_pop(co, nres);
        report_error("coroutine.yield() outside an async call");
        drop(co);
        return false;
    }
    const char* msg = lua_tostring(co, -1);
    luaL_traceback(L_, co, msg ? msg : "(error object is not a string)", 0);
    std::string text = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    drop(co);
    if (status == LUA_ERRMEM)
        fault("out of memory (cap " + std::to_string(limits_.memory_bytes / 1024) + " KB)");
    else
        report_error(text);
    return false;
}

void LuaRuntime::drop(lua_State* co) {
    auto it = threads_.find(co);
    if (it == threads_.end())
        return;
    lua_closethread(co, L_);
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    threads_.erase(it);
}

void LuaRuntime::report_error(const std::string& message) {
    spdlog::warn("[plugin {}] {}", plugin_id_, message);
    if (errors_.record(Clock::now()))
        fault("3 errors within 60 s");
}

void LuaRuntime::fault(const std::string& reason) {
    if (faulted_)
        return;
    faulted_ = true;
    fault_reason_ = reason;
    spdlog::error("[plugin {}] disabled: {}", plugin_id_, reason);
    if (on_fault_)
        on_fault_(reason);
}

void LuaRuntime::budget_hook(lua_State*, lua_Debug*) {}

int LuaRuntime::await_async(lua_State* co, const std::function<void(Pending)>&) {
    return luaL_error(co, "async calls are not available yet");
}

void LuaRuntime::resume(lua_State*, const PushFn&) {}

void LuaRuntime::Pending::resolve(PushFn) const {}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Append `src/plugin/lua_runtime.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 7: Run to see it pass**

Run: `make t F='[lua_runtime]'`
Expected: all 10 cases pass.

- [ ] **Step 8: Commit**

```bash
git add include/lua_runtime.h src/plugin/lua_runtime.cpp tests/test_helpers/plugin_test_support.h \
  tests/unit/test_lua_runtime.cpp tests/fixtures/plugins/require-test \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): sandboxed Lua runtime with a memory cap and plugin-local require"
git show --stat HEAD
```

---

### Task 5: Time budget

**Files:**
- Modify: `src/plugin/lua_runtime.cpp` (`enter`, `budget_hook`)
- Test: `tests/unit/test_lua_runtime.cpp`

**Interfaces:**
- Consumes: Task 4's `LuaRuntime`.
- Produces: an entry that runs past its deadline raises, and the plugin faults with a reason containing `time budget`.

- [ ] **Step 1: Write the failing tests**

Append inside the `#if` of `tests/unit/test_lua_runtime.cpp`:

```cpp
TEST_CASE("an infinite loop is stopped and faults the plugin", "[plugin][lua_runtime][budget]") {
    TestRuntime t;
    auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(t.run("while true do end"));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("pcall cannot swallow the time budget", "[plugin][lua_runtime][budget]") {
    TestRuntime t;
    CHECK_FALSE(t.run(R"(
        while true do
            pcall(function() while true do end end)
        end
    )"));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("time budget") != std::string::npos);
}

TEST_CASE("work inside the budget is untouched", "[plugin][lua_runtime][budget]") {
    TestRuntime t;
    REQUIRE(t.run("s = 0 for i = 1, 200000 do s = s + i end"));
    CHECK(t.global("s") == "20000100000");
    CHECK_FALSE(t.rt->faulted());
}

TEST_CASE("each outermost entry gets a fresh budget", "[plugin][lua_runtime][budget]") {
    TestRuntime t; // 50 ms budget; three 20 ms runs exceed one budget but not their own
    lua_pushcfunction(t.rt->state(), [](lua_State* L) -> int {
        using namespace std::chrono;
        lua_pushinteger(L, duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        return 1;
    });
    lua_setglobal(t.rt->state(), "now_ms");
    const char* spin = "local t0 = now_ms() while now_ms() - t0 < 20 do end";
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK_FALSE(t.rt->faulted());
}
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[budget]'`
Expected: the first case hangs. Interrupt it after a few seconds; the hang is the failure.

- [ ] **Step 3: Implement**

In `enter`, before `++depth_`, start the deadline for an outermost entry only:

```cpp
    if (depth_ == 0) {
        deadline_ = Clock::now() + limits_.time_budget;
        killed_ = false;
    }
```

Replace the tail of `enter`'s error path (after `drop(co);`) with:

```cpp
    if (killed_)
        fault("exceeded its " + std::to_string(limits_.time_budget.count()) + " ms time budget");
    else if (status == LUA_ERRMEM)
        fault("out of memory (cap " + std::to_string(limits_.memory_bytes / 1024) + " KB)");
    else
        report_error(text);
    return false;
```

Replace `budget_hook`:

```cpp
void LuaRuntime::budget_hook(lua_State* L, lua_Debug*) {
    auto& rt = from(L);
    if (!rt.killed_ && Clock::now() < rt.deadline_)
        return;
    rt.killed_ = true;
    // Firing on every instruction means each instruction outside the innermost pcall raises
    // again, so no depth of pcall can hold the entry open.
    lua_sethook(L, &LuaRuntime::budget_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "exceeded the plugin time budget");
}
```

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[lua_runtime]'`
Expected: every case passes, Task 4's included.

- [ ] **Step 5: Commit**

```bash
git add src/plugin/lua_runtime.cpp tests/unit/test_lua_runtime.cpp
git commit -m "feat(plugin): 50 ms time budget per Lua entry that pcall cannot swallow"
git show --stat HEAD
```

---

### Task 6: Coroutine entries and async suspend/resume

**Files:**
- Modify: `src/plugin/lua_runtime.cpp` (`enter`, `await_async`, `resume`, `Pending::resolve`)
- Test: `tests/unit/test_lua_runtime.cpp`

**Interfaces:**
- Consumes: Tasks 4-5; `helix::ui::UpdateQueue::instance().drain()` (`include/ui_update_queue.h`) in tests.
- Produces: `await_async`, which every async binding uses in this shape:

```cpp
int some_async_binding(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string arg = luaL_checkstring(L, 1);
    return rt.await_async(L, [arg](LuaRuntime::Pending p) {
        start_background_work(arg, [p](std::string result) {   // any thread
            p.resolve([result](lua_State* co) { lua_pushstring(co, result.c_str()); return 1; });
        });
    });
}
```

- [ ] **Step 1: Write the failing tests**

Append inside the `#if` of `tests/unit/test_lua_runtime.cpp`:

```cpp
#include "ui_update_queue.h"

namespace {
// helix.test_wait() suspends and hands its Pending to the test.
void install_test_wait(LuaRuntime& rt, std::vector<LuaRuntime::Pending>* sink) {
    lua_State* L = rt.state();
    lua_getglobal(L, "helix");
    lua_pushlightuserdata(L, sink);
    lua_pushcclosure(
        L,
        [](lua_State* co) -> int {
            auto* s = static_cast<std::vector<LuaRuntime::Pending>*>(lua_touserdata(co, lua_upvalueindex(1)));
            return LuaRuntime::from(co).await_async(co, [s](LuaRuntime::Pending p) { s->push_back(p); });
        },
        1);
    lua_setfield(L, -2, "test_wait");
    lua_pop(L, 1);
}

LuaRuntime::PushFn push_int(lua_Integer v) {
    return [v](lua_State* co) {
        lua_pushinteger(co, v);
        return 1;
    };
}
} // namespace

TEST_CASE("an async call suspends and resumes with its result", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);

    REQUIRE(t.run("before = true; got = helix.test_wait(); after = got + 1"));
    CHECK(t.global("before") == "true");
    CHECK(t.global("after") == "nil");
    REQUIRE(pending.size() == 1);

    pending[0].resolve(push_int(41));
    CHECK(t.global("after") == "nil"); // resumes on the main loop, not inside resolve
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("after") == "42");
}

TEST_CASE("resolve is one-shot", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run("n = 0; helix.test_wait(); n = n + 1"));
    pending[0].resolve(push_int(1));
    pending[0].resolve(push_int(2));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("n") == "1");
}

TEST_CASE("a reply after the runtime is gone is dropped", "[plugin][lua_runtime][async]") {
    std::vector<LuaRuntime::Pending> pending;
    {
        TestRuntime t;
        install_test_wait(*t.rt, &pending);
        REQUIRE(t.run("helix.test_wait()"));
    }
    REQUIRE(pending.size() == 1);
    pending[0].resolve(push_int(1));
    helix::ui::UpdateQueue::instance().drain();
    SUCCEED(); // ASAN (Step 5) is what proves nothing touched the destroyed runtime
}

TEST_CASE("a reply after a fault is dropped", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run("helix.test_wait(); resumed = true"));
    t.run("error('1')");
    t.run("error('2')");
    t.run("error('3')");
    REQUIRE(t.rt->faulted());
    pending[0].resolve(push_int(1));
    helix::ui::UpdateQueue::instance().drain();
    CHECK(t.global("resumed") == "nil");
}

TEST_CASE("a bare coroutine.yield at entry level is an error", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    CHECK_FALSE(t.run("coroutine.yield()"));
    CHECK(t.run("ok = true"));
}

TEST_CASE("an async call inside a plugin-made coroutine raises", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    std::vector<LuaRuntime::Pending> pending;
    install_test_wait(*t.rt, &pending);
    REQUIRE(t.run(R"(
        local co = coroutine.wrap(function() return helix.test_wait() end)
        ok, err = pcall(co)
        mentions = tostring(err):find("async call not allowed") ~= nil
    )"));
    CHECK(t.global("ok") == "false");
    CHECK(t.global("mentions") == "true");
    CHECK(pending.empty());
}

TEST_CASE("nested entries stop at the depth cap", "[plugin][lua_runtime][async]") {
    TestRuntime t;
    // helix.reenter(f) runs f as a new entry synchronously, the way a subject observer does.
    lua_State* L = t.rt->state();
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, [](lua_State* co) -> int {
        auto& rt = LuaRuntime::from(co);
        int ref = rt.ref_value(co, 1);
        rt.invoke(ref);
        rt.unref(ref);
        return 0;
    });
    lua_setfield(L, -2, "reenter");
    lua_pop(L, 1);

    t.run(R"(
        depth = 0
        local function recurse() depth = depth + 1; helix.reenter(recurse) end
        recurse()
    )");
    CHECK(t.global("depth") == "8");
}
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[async]'`
Expected: FAIL; `await_async` raises "async calls are not available yet".

- [ ] **Step 3: Implement**

Replace `enter`:

```cpp
bool LuaRuntime::enter(lua_State* co, int nargs) {
    if (depth_ >= kMaxEntryDepth) {
        lua_settop(co, 0);
        drop(co);
        report_error("entries nested more than " + std::to_string(kMaxEntryDepth) +
                     " deep (an observer setting the subject it observes?)");
        return false;
    }
    if (depth_ == 0) {
        deadline_ = Clock::now() + limits_.time_budget;
        killed_ = false;
    }
    bool outer_yielded = yielded_for_async_;
    yielded_for_async_ = false;
    ++depth_;
    int nres = 0;
    int status = lua_resume(co, L_, nargs, &nres);
    --depth_;
    bool yielded_for_async = yielded_for_async_;
    yielded_for_async_ = outer_yielded;

    if (status == LUA_OK) {
        lua_pop(co, nres);
        drop(co);
        return true;
    }
    if (status == LUA_YIELD) {
        lua_pop(co, nres);
        if (yielded_for_async)
            return true; // stays in threads_ until its Pending resolves
        report_error("coroutine.yield() outside an async call");
        drop(co);
        return false;
    }
    const char* msg = lua_tostring(co, -1);
    luaL_traceback(L_, co, msg ? msg : "(error object is not a string)", 0);
    std::string text = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    drop(co);
    if (killed_)
        fault("exceeded its " + std::to_string(limits_.time_budget.count()) + " ms time budget");
    else if (status == LUA_ERRMEM)
        fault("out of memory (cap " + std::to_string(limits_.memory_bytes / 1024) + " KB)");
    else
        report_error(text);
    return false;
}
```

Replace the three stubs:

```cpp
int LuaRuntime::await_async(lua_State* co, const std::function<void(Pending)>& start) {
    // Only entry coroutines are in threads_, which rejects plugin-made coroutines.
    if (!threads_.count(co) || !lua_isyieldable(co))
        return luaL_error(co, "async call not allowed here: call it from a handler or the top "
                              "level of main.lua, not a metamethod, iterator, sort comparator, "
                              "module top level or plugin-made coroutine");
    start(Pending(this, co, guard_.token()));
    yielded_for_async_ = true;
    return lua_yield(co, 0);
}

void LuaRuntime::Pending::resolve(PushFn push_results) const {
    if (done_->exchange(true))
        return;
    LuaRuntime* rt = rt_;
    lua_State* co = co_;
    token_.defer("plugin_resume", [rt, co, push = std::move(push_results)]() { rt->resume(co, push); });
}

void LuaRuntime::resume(lua_State* co, const PushFn& push_results) {
    if (faulted_ || !threads_.count(co))
        return;
    int n = push_results ? push_results(co) : 0;
    enter(co, n);
}
```

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[lua_runtime]'`
Expected: every case passes.

- [ ] **Step 5: ASAN on zeus**

Push the branch, then run: `scripts/zeus-run.sh asan '[lua_runtime]'`
Expected: no ASAN reports. Any leak or use-after-free blocks the task.

- [ ] **Step 6: Commit**

```bash
git add src/plugin/lua_runtime.cpp tests/unit/test_lua_runtime.cpp
git commit -m "feat(plugin): coroutine entries with async suspend and main-thread resume"
git show --stat HEAD
```

---

### Task 7: PluginBackend

**Files:**
- Create: `include/plugin_backend.h`, `src/plugin/plugin_backend_app.cpp`
- Modify: `tests/test_helpers/plugin_test_support.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_backend.cpp`

**Interfaces:**
- Consumes: `get_moonraker_api()` (`include/app_globals.h:67`), `get_moonraker_client()` (`:33`), `IMoonrakerAPI::execute_gcode` (`include/i_moonraker_api.h:206`), `IMoonrakerClient::send_jsonrpc` with success and error callbacks (`include/i_moonraker_client.h:109`), `IMoonrakerAPI::transfers()` → `ITransfersAPI::upload_file`/`download_file` (`include/i_moonraker_sub_apis.h:466`), `register_method_callback`/`unregister_method_callback` (`include/i_moonraker_api.h:125,129`; the callback receives the whole notification, params under `msg["params"]`), `HttpExecutor::fast().submit` (`include/http_executor.h:76,95`), `requests::request` (`hv/requests.h`), `MoonrakerError::message` (`include/moonraker_error.h`).
- Produces:

```cpp
namespace helix::plugin {
struct RpcResult { bool ok = false; json value; std::string error; };
using RpcCallback = std::function<void(RpcResult)>; // may run on any thread
struct PluginBackend {
    std::function<void(const std::string& script, RpcCallback)> gcode;
    std::function<void(const std::string& method, const json& params, RpcCallback)> call;
    std::function<void(const std::string& root, const std::string& path,
                       const std::string& content, RpcCallback)> upload;
    std::function<void(const std::string& root, const std::string& path, RpcCallback)> download;
    std::function<void(const std::string& method, const std::string& url, const std::string& body,
                       const json& headers, uint32_t timeout_ms, RpcCallback)> http;
    std::function<std::function<void()>(const std::string& notify_method,
                                        std::function<void(const json& msg)>)> on_notify;
};
PluginBackend make_app_backend();
}
```

Contracts: `download` yields `value` as a JSON string of the file content. `http` yields `value = {"status": int, "body": string}`, `ok = true` for any HTTP status and `false` only on a transport failure. `on_notify` returns an unregister function. `upload` and `download` accept only the `gcodes` and `config` roots.

- [ ] **Step 1: Write the failing test**

The production wiring is glue over tested APIs. Its test proves it degrades safely with no Moonraker, which is the state unit tests run in.

`tests/unit/test_plugin_backend.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "app_globals.h"
#include "plugin_backend.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;

TEST_CASE("app backend reports no connection instead of crashing", "[plugin][backend]") {
    REQUIRE(get_moonraker_api() == nullptr);
    REQUIRE(get_moonraker_client() == nullptr);
    auto b = make_app_backend();
    std::vector<RpcResult> got;
    auto sink = [&](RpcResult r) { got.push_back(std::move(r)); };
    b.gcode("M117 hi", sink);
    b.call("server.info", json::object(), sink);
    b.upload("gcodes", "a.gcode", "G28", sink);
    b.download("gcodes", "a.gcode", sink);
    REQUIRE(got.size() == 4);
    for (const auto& r : got) {
        CHECK_FALSE(r.ok);
        CHECK(r.error.find("not connected") != std::string::npos);
    }
    auto off = b.on_notify("notify_agent_event", [](const json&) {});
    REQUIRE(off);
    off();
}

TEST_CASE("app backend rejects roots other than gcodes and config", "[plugin][backend]") {
    auto b = make_app_backend();
    RpcResult up, down;
    b.upload("logs", "x", "y", [&](RpcResult r) { up = std::move(r); });
    b.download("../etc", "x", [&](RpcResult r) { down = std::move(r); });
    CHECK_FALSE(up.ok);
    CHECK(up.error.find("root") != std::string::npos);
    CHECK_FALSE(down.ok);
}

#endif // HELIX_HAS_PLUGINS
```

If the `REQUIRE`s at the top fail because a global test fixture installs an API, find where (`grep -rn "set_moonraker_api" tests/*.h tests/*.cpp`) and clear it for this case with the matching setter and `nullptr`, restoring it afterwards.

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[backend]'`
Expected: compile error, `plugin_backend.h` not found.

- [ ] **Step 3: Implement**

`include/plugin_backend.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "hv/json.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace helix::plugin {

using json = nlohmann::json;

struct RpcResult {
    bool ok = false;
    json value;
    std::string error;
};

/// May run on any thread.
using RpcCallback = std::function<void(RpcResult)>;

/// Everything Lua bindings may ask of the app. Bindings never reach Moonraker or the network
/// any other way, which is what lets tests substitute a fake.
struct PluginBackend {
    std::function<void(const std::string& script, RpcCallback)> gcode;
    std::function<void(const std::string& method, const json& params, RpcCallback)> call;
    /// `root` is `gcodes` or `config`.
    std::function<void(const std::string& root, const std::string& path, const std::string& content,
                       RpcCallback)> upload;
    /// `root` is `gcodes` or `config`; `value` is a JSON string holding the file content.
    std::function<void(const std::string& root, const std::string& path, RpcCallback)> download;
    /// `value` is `{"status": int, "body": string}`; `ok` is false only when no response arrived.
    std::function<void(const std::string& method, const std::string& url, const std::string& body,
                       const json& headers, uint32_t timeout_ms, RpcCallback)> http;
    /// Registers a handler for a Moonraker notification method (it receives the whole
    /// message) and returns the function that unregisters it.
    std::function<std::function<void()>(const std::string& notify_method,
                                        std::function<void(const json& msg)>)> on_notify;
};

PluginBackend make_app_backend();

} // namespace helix::plugin
```

`src/plugin/plugin_backend_app.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_backend.h"

#include "app_globals.h"
#include "http_executor.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"

#include "hv/requests.h"

#include <atomic>

namespace helix::plugin {

namespace {

RpcResult failure(std::string message) {
    RpcResult r;
    r.error = std::move(message);
    return r;
}

RpcResult success(json value = json()) {
    return RpcResult{true, std::move(value), {}};
}

bool is_transfer_root(const std::string& root) {
    return root == "gcodes" || root == "config";
}

constexpr const char* kNotConnected = "Moonraker is not connected";

} // namespace

PluginBackend make_app_backend() {
    PluginBackend b;

    b.gcode = [](const std::string& script, RpcCallback cb) {
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->execute_gcode(
            script, [cb]() { cb(success()); }, [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.call = [](const std::string& method, const json& params, RpcCallback cb) {
        auto* client = get_moonraker_client();
        if (!client)
            return cb(failure(kNotConnected));
        client->send_jsonrpc(
            method, params, [cb](const json& result) { cb(success(result)); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.upload = [](const std::string& root, const std::string& path, const std::string& content,
                  RpcCallback cb) {
        if (!is_transfer_root(root))
            return cb(failure("root must be 'gcodes' or 'config'"));
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->transfers().upload_file(
            root, path, content, [cb]() { cb(success()); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.download = [](const std::string& root, const std::string& path, RpcCallback cb) {
        if (!is_transfer_root(root))
            return cb(failure("root must be 'gcodes' or 'config'"));
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->transfers().download_file(
            root, path, [cb](const std::string& body) { cb(success(json(body))); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    // ponytail: one blocking libhv request per call on the fast pool; a streaming client when a
    // plugin needs bodies too large to hold in memory.
    b.http = [](const std::string& method, const std::string& url, const std::string& body,
                const json& headers, uint32_t timeout_ms, RpcCallback cb) {
        helix::HttpExecutor::fast().submit([=]() {
            auto req = std::make_shared<HttpRequest>();
            req->method = method == "POST" ? HTTP_POST : HTTP_GET;
            req->url = url;
            req->timeout = static_cast<int>((timeout_ms + 999) / 1000);
            req->body = body;
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                if (it.value().is_string())
                    req->headers[it.key()] = it.value().get<std::string>();
            }
            auto resp = requests::request(req);
            if (!resp)
                return cb(failure("request to " + url + " failed"));
            cb(success(json{{"status", resp->status_code}, {"body", resp->body}}));
        });
    };

    b.on_notify = [](const std::string& method,
                     std::function<void(const json&)> handler) -> std::function<void()> {
        auto* api = get_moonraker_api();
        if (!api)
            return [] {};
        static std::atomic<unsigned> next_id{0};
        std::string name = "lua_plugin_" + std::to_string(next_id++);
        api->register_method_callback(method, name, std::move(handler));
        return [method, name]() {
            if (auto* a = get_moonraker_api())
                a->unregister_method_callback(method, name);
        };
    };

    return b;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Qualify `HttpExecutor`, `get_moonraker_api` and `get_moonraker_client` with the namespaces their headers declare. Compare the request fields against `src/system/update_checker.cpp#do_http_get` and copy any it sets that this omits (TLS verification settings in particular).

Append `src/plugin/plugin_backend_app.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Add the fake backend to the test support header**

Append to `tests/test_helpers/plugin_test_support.h`, inside the `#if` and namespace, with `#include "plugin_backend.h"` and `<vector>` at the top:

```cpp
/// Records every backend request so a test can inspect it and answer it later.
struct FakeBackend {
    struct Request {
        std::string kind; ///< gcode, call, upload, download, http
        std::string a;    ///< script, method, root, or HTTP method
        std::string b;    ///< path or URL
        std::string c;    ///< upload content or HTTP body
        json params;      ///< call params or HTTP headers
        RpcCallback reply;
    };
    std::vector<Request> requests;
    std::vector<std::pair<std::string, std::function<void(const json&)>>> notify;
    int notify_unregistered = 0;

    PluginBackend backend() {
        PluginBackend b;
        b.gcode = [this](const std::string& s, RpcCallback cb) {
            requests.push_back({"gcode", s, {}, {}, {}, std::move(cb)});
        };
        b.call = [this](const std::string& m, const json& p, RpcCallback cb) {
            requests.push_back({"call", m, {}, {}, p, std::move(cb)});
        };
        b.upload = [this](const std::string& r, const std::string& p, const std::string& c, RpcCallback cb) {
            requests.push_back({"upload", r, p, c, {}, std::move(cb)});
        };
        b.download = [this](const std::string& r, const std::string& p, RpcCallback cb) {
            requests.push_back({"download", r, p, {}, {}, std::move(cb)});
        };
        b.http = [this](const std::string& m, const std::string& u, const std::string& body,
                        const json& h, uint32_t, RpcCallback cb) {
            requests.push_back({"http", m, u, body, h, std::move(cb)});
        };
        b.on_notify = [this](const std::string& m, std::function<void(const json&)> h) {
            notify.emplace_back(m, std::move(h));
            return std::function<void()>([this] { ++notify_unregistered; });
        };
        return b;
    }
};
```

- [ ] **Step 5: Run to see it pass**

Run: `make t F='[backend]'`
Expected: both cases pass.

- [ ] **Step 6: Commit**

```bash
git add include/plugin_backend.h src/plugin/plugin_backend_app.cpp \
  tests/test_helpers/plugin_test_support.h tests/unit/test_plugin_backend.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): PluginBackend, the app services Lua bindings may use"
git show --stat HEAD
```

---

### Task 8: Core bindings (log, json, timer, sleep)

**Files:**
- Create: `include/lua_bindings.h`, `src/plugin/lua_bind_core.cpp`
- Modify: `tests/test_helpers/plugin_test_support.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_core.cpp`

**Interfaces:**
- Consumes: `LuaRuntime`; `helix::ui::LvglTimerGuard` (`include/ui_timer_guard.h:35`: `reset(lv_timer_t* = nullptr)` cancels the held timer and holds the new one; `release()` gives it up without cancelling); `LVGLTestFixture::process_lvgl(ms)` (`tests/lvgl_test_fixture.h:142`). The test harness runs only one-shot LVGL timers (`tests/ui_test_utils.cpp`, repeat count > 0), so `every` re-arms a fresh one-shot on each tick rather than using a repeating timer.
- Produces, in `include/lua_bindings.h`:

```cpp
namespace helix::plugin {
struct PluginContext {
    LuaRuntime& rt;
    PluginBackend& backend;
    const Manifest& manifest;
    json* settings;                     ///< this plugin's /plugins/settings/<id> object
    std::function<void()> save_settings;
    std::string storage_path;           ///< <dir of settings.json>/plugin-data/<id>.json
};
using Installer = void (*)(PluginContext&);
void install_core_bindings(PluginContext& ctx);      // this task; also stores &ctx for context()
void install_ui_bindings(PluginContext& ctx);        // Task 9
void install_printer_bindings(PluginContext& ctx);   // Task 10
void install_moonraker_bindings(PluginContext& ctx); // Task 11
void install_io_bindings(PluginContext& ctx);        // Task 12
PluginContext& context(lua_State* L);
void push_json(lua_State* L, const json& j);
json to_json(lua_State* L, int index); // raises for functions, userdata, cycles, depth > 32
}
```

The owner keeps the `PluginContext` alive until after the runtime is destroyed, because runtime closers may read it.

- [ ] **Step 1: Write the header**

`include/lua_bindings.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lua_runtime.h"
#include "plugin_backend.h"
#include "plugin_manifest.h"

#include <functional>
#include <string>

namespace helix::plugin {

/// What every binding of one plugin may reach. The owner keeps it alive until after the
/// runtime is destroyed, because runtime closers may read it.
struct PluginContext {
    LuaRuntime& rt;
    PluginBackend& backend;
    const Manifest& manifest;
    json* settings; ///< this plugin's /plugins/settings/<id> object
    std::function<void()> save_settings;
    std::string storage_path; ///< <dir of settings.json>/plugin-data/<id>.json
};

using Installer = void (*)(PluginContext&);

/// helix.log, helix.json, helix.timer, helix.sleep. Must run first: it registers the
/// context that context() returns.
void install_core_bindings(PluginContext& ctx);
void install_ui_bindings(PluginContext& ctx);
void install_printer_bindings(PluginContext& ctx);
void install_moonraker_bindings(PluginContext& ctx);
void install_io_bindings(PluginContext& ctx);

PluginContext& context(lua_State* L);

/// Objects and arrays become tables; null becomes nil.
void push_json(lua_State* L, const json& j);

/// Raises a Lua error for functions, userdata, cycles, non-string object keys and nesting
/// deeper than 32. An empty table converts to an empty array.
json to_json(lua_State* L, int index);

} // namespace helix::plugin
```

- [ ] **Step 2: Add `BoundRuntime` and `TempDir` to the test support header**

Append inside the `#if` and namespace of `tests/test_helpers/plugin_test_support.h` (add `#include "lua_bindings.h"`, `<filesystem>`, `<random>`):

```cpp
/// A TestRuntime with a PluginContext and a fake backend, with core bindings installed plus
/// whichever `installers` a test asks for.
struct BoundRuntime {
    FakeBackend fake;
    PluginBackend backend = fake.backend();
    Manifest manifest;
    json settings = json::object();
    int saves = 0;
    std::string storage_path;
    std::unique_ptr<PluginContext> ctx;
    TestRuntime t; // last member: destroyed first, while ctx and fake still exist

    explicit BoundRuntime(std::vector<Installer> installers = {}, PermissionSet perms = {},
                          std::vector<SettingDecl> decls = {}, std::string storage = {})
        : storage_path(std::move(storage)) {
        manifest.id = "test-plugin";
        manifest.name = "Test Plugin";
        manifest.version = "1.0.0";
        manifest.permissions = std::move(perms);
        manifest.settings = std::move(decls);
        ctx = std::make_unique<PluginContext>(
            PluginContext{*t.rt, backend, manifest, &settings, [this] { ++saves; }, storage_path});
        install_core_bindings(*ctx);
        for (Installer install : installers)
            install(*ctx);
    }
};

/// A fresh directory under the system temp dir, removed with its contents on destruction.
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("helix-plugin-test-" + std::to_string(rd()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return (path / name).string(); }
};
```

- [ ] **Step 3: Write the failing tests**

`tests/unit/test_lua_bindings_core.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(LVGLTestFixture, "helix.json round-trips", "[plugin][bindings][core]") {
    BoundRuntime b;
    REQUIRE(b.t.run(R"(
        local s = helix.json.encode({ a = 1, list = { 1, 2, 3 }, nested = { ok = true } })
        local v = helix.json.decode(s)
        a, n, ok = v.a, #v.list, v.nested.ok
        bad = helix.json.decode("{nope")
        empty = helix.json.encode({})
    )"));
    CHECK(b.t.global("a") == "1");
    CHECK(b.t.global("n") == "3");
    CHECK(b.t.global("ok") == "true");
    CHECK(b.t.global("bad") == "nil");
    CHECK(b.t.global("empty") == "[]");
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.json.encode refuses what JSON cannot hold",
                 "[plugin][bindings][core]") {
    BoundRuntime b;
    CHECK_FALSE(b.t.run("helix.json.encode({ f = print })"));
    CHECK_FALSE(b.t.run("local t = {} t.self = t helix.json.encode(t)"));
    CHECK_FALSE(b.t.run("helix.json.encode({ [true] = 1 })"));
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.timer.after fires once and cancel stops it",
                 "[plugin][bindings][core]") {
    BoundRuntime b;
    REQUIRE(b.t.run(R"(
        fired, cancelled_fired = 0, 0
        helix.timer.after(10, function() fired = fired + 1 end)
        local h = helix.timer.after(10, function() cancelled_fired = 1 end)
        h:cancel()
    )"));
    process_lvgl(60);
    CHECK(b.t.global("fired") == "1");
    CHECK(b.t.global("cancelled_fired") == "0");
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.timer.every repeats until cancelled",
                 "[plugin][bindings][core]") {
    BoundRuntime b;
    REQUIRE(b.t.run(R"(
        ticks = 0
        local h
        h = helix.timer.every(10, function()
            ticks = ticks + 1
            if ticks == 3 then h:cancel() end
        end)
    )"));
    process_lvgl(150);
    CHECK(b.t.global("ticks") == "3");
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.sleep yields and resumes", "[plugin][bindings][core]") {
    BoundRuntime b;
    REQUIRE(b.t.run("stage = 1; helix.sleep(10); stage = 2"));
    CHECK(b.t.global("stage") == "1");
    process_lvgl(40);
    helix::ui::UpdateQueue::instance().drain();
    CHECK(b.t.global("stage") == "2");
}

TEST_CASE_METHOD(LVGLTestFixture, "timers and sleeps die with the runtime", "[plugin][bindings][core]") {
    {
        BoundRuntime b;
        REQUIRE(b.t.run("helix.timer.every(5, function() end)"));
        REQUIRE(b.t.run("helix.sleep(5)"));
    }
    process_lvgl(30);
    helix::ui::UpdateQueue::instance().drain();
    SUCCEED(); // ASAN (Step 7) is what proves no timer outlived its state
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.log accepts every level", "[plugin][bindings][core]") {
    BoundRuntime b;
    CHECK(b.t.run(R"(helix.log.debug("d") helix.log.info("i") helix.log.warn("w") helix.log.error("e"))"));
    CHECK_FALSE(b.t.run(R"(helix.log.info())"));
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 4: Run to see it fail**

Run: `make t F='[core]'`
Expected: link errors for `install_core_bindings`.

- [ ] **Step 5: Implement**

`src/plugin/lua_bind_core.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"
#include "ui_timer_guard.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace helix::plugin {

namespace {

const char kContextKey = 0;
const char kTimersKey = 0;
const char kTimerMeta[] = "helix.timer";

// A live timer or sleep. The TimerRegistry owns every TimerState, so an LVGL timer's user
// data can be a plain pointer: a state outlives the timer that points at it.
struct TimerState {
    LuaRuntime* rt = nullptr;
    uint32_t period_ms = 0;
    int fn_ref = LUA_NOREF;                       ///< timers
    std::unique_ptr<LuaRuntime::Pending> pending; ///< sleeps
    bool repeat = false;
    bool live = true;
    helix::ui::LvglTimerGuard timer;
};

void on_timer(lv_timer_t* timer);

struct TimerRegistry {
    uint64_t next_id = 1;
    std::unordered_map<uint64_t, std::unique_ptr<TimerState>> timers;

    // Returns the id Lua handles use; a handle never holds a pointer, so a pruned state
    // cannot dangle.
    uint64_t add(std::unique_ptr<TimerState> s) {
        for (auto it = timers.begin(); it != timers.end();)
            it = it->second->live ? std::next(it) : timers.erase(it);
        uint64_t id = next_id++;
        timers.emplace(id, std::move(s));
        return id;
    }
};

TimerRegistry& timers_of(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kTimersKey);
    auto* r = static_cast<TimerRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *r;
}

lv_timer_t* arm(TimerState* s) {
    lv_timer_t* t = lv_timer_create(&on_timer, s->period_ms, s);
    lv_timer_set_repeat_count(t, 1);
    return t;
}

void stop(TimerState& s) {
    if (!s.live)
        return;
    s.live = false;
    s.timer.reset();
    if (s.fn_ref != LUA_NOREF) {
        s.rt->unref(s.fn_ref);
        s.fn_ref = LUA_NOREF;
    }
    s.pending.reset();
}

void on_timer(lv_timer_t* timer) {
    auto* s = static_cast<TimerState*>(lv_timer_get_user_data(timer));
    if (!s->live)
        return;
    s->timer.release(); // LVGL deletes this one-shot after the callback returns
    LuaRuntime* rt = s->rt;
    if (s->pending) {
        auto pending = std::move(s->pending);
        s->live = false;
        pending->resolve({});
        return;
    }
    int ref = s->fn_ref;
    if (s->repeat) {
        s->timer.reset(arm(s)); // re-armed before Lua runs, so the callback can cancel it
        rt->invoke(ref);        // s may be pruned inside; not touched after this
        return;
    }
    s->live = false;
    s->fn_ref = LUA_NOREF;
    rt->invoke(ref);
    rt->unref(ref);
}

int start_timer(lua_State* L, bool repeat) {
    auto& rt = LuaRuntime::from(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 1 && ms <= 24 * 3600 * 1000, 1, "interval must be 1 ms to 24 h");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    auto s = std::make_unique<TimerState>();
    s->rt = &rt;
    s->period_ms = static_cast<uint32_t>(ms);
    s->repeat = repeat;
    s->fn_ref = rt.ref_value(L, 2);
    s->timer.reset(arm(s.get()));
    uint64_t id = timers_of(L).add(std::move(s));

    *static_cast<uint64_t*>(lua_newuserdatauv(L, sizeof(uint64_t), 0)) = id;
    luaL_setmetatable(L, kTimerMeta);
    return 1;
}

int timer_after(lua_State* L) {
    return start_timer(L, false);
}

int timer_every(lua_State* L) {
    return start_timer(L, true);
}

int timer_cancel(lua_State* L) {
    uint64_t id = *static_cast<uint64_t*>(luaL_checkudata(L, 1, kTimerMeta));
    auto& reg = timers_of(L);
    if (auto it = reg.timers.find(id); it != reg.timers.end())
        stop(*it->second);
    return 0;
}

int sleep_ms(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= 24 * 3600 * 1000, 1, "duration must be 0 ms to 24 h");
    auto& reg = timers_of(L);
    return rt.await_async(L, [&rt, &reg, ms](LuaRuntime::Pending p) {
        auto s = std::make_unique<TimerState>();
        s->rt = &rt;
        s->period_ms = static_cast<uint32_t>(std::max<lua_Integer>(ms, 1));
        s->pending = std::make_unique<LuaRuntime::Pending>(p);
        s->timer.reset(arm(s.get()));
        reg.add(std::move(s));
    });
}

json to_json_impl(lua_State* L, int index, int depth, std::unordered_set<const void*>& seen) {
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return nullptr;
    case LUA_TBOOLEAN:
        return static_cast<bool>(lua_toboolean(L, index));
    case LUA_TNUMBER:
        if (lua_isinteger(L, index))
            return static_cast<int64_t>(lua_tointeger(L, index));
        return lua_tonumber(L, index);
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, index, &len);
        return std::string(s, len);
    }
    case LUA_TTABLE: {
        if (depth > 32)
            luaL_error(L, "json: table nested deeper than 32");
        const void* id = lua_topointer(L, index);
        if (!seen.insert(id).second)
            luaL_error(L, "json: table contains a cycle");
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, index));
        lua_Integer count = 0;
        lua_pushnil(L);
        while (lua_next(L, index)) {
            ++count;
            lua_pop(L, 1);
        }
        json out;
        if (count == n) { // a sequence, including the empty table
            out = json::array();
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, index, i);
                out.push_back(to_json_impl(L, -1, depth + 1, seen));
                lua_pop(L, 1);
            }
        } else {
            out = json::object();
            lua_pushnil(L);
            while (lua_next(L, index)) {
                if (lua_type(L, -2) != LUA_TSTRING)
                    luaL_error(L, "json: object keys must be strings");
                out[lua_tostring(L, -2)] = to_json_impl(L, -1, depth + 1, seen);
                lua_pop(L, 1);
            }
        }
        seen.erase(id);
        return out;
    }
    default:
        luaL_error(L, "json: cannot encode a %s", luaL_typename(L, index));
        return nullptr;
    }
}

int json_encode(lua_State* L) {
    luaL_checkany(L, 1);
    std::string s = to_json(L, 1).dump();
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int json_decode(lua_State* L) {
    json j = json::parse(luaL_checkstring(L, 1), nullptr, false);
    if (j.is_discarded()) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid JSON");
        return 2;
    }
    push_json(L, j);
    return 1;
}

template <spdlog::level::level_enum Level> int log_at(lua_State* L) {
    spdlog::log(Level, "[plugin {}] {}", LuaRuntime::from(L).plugin_id(), luaL_checkstring(L, 1));
    return 0;
}

void add_module(lua_State* L, const char* name, const luaL_Reg* fns) {
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

} // namespace

void push_json(lua_State* L, const json& j) {
    luaL_checkstack(L, 3, "json nested too deeply");
    switch (j.type()) {
    case json::value_t::boolean:
        lua_pushboolean(L, j.get<bool>());
        break;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        lua_pushinteger(L, j.get<lua_Integer>());
        break;
    case json::value_t::number_float:
        lua_pushnumber(L, j.get<double>());
        break;
    case json::value_t::string: {
        const auto& s = j.get_ref<const std::string&>();
        lua_pushlstring(L, s.data(), s.size());
        break;
    }
    case json::value_t::array:
        lua_createtable(L, static_cast<int>(j.size()), 0);
        for (size_t i = 0; i < j.size(); ++i) {
            push_json(L, j[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        break;
    case json::value_t::object:
        lua_createtable(L, 0, static_cast<int>(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it) {
            push_json(L, it.value());
            lua_setfield(L, -2, it.key().c_str());
        }
        break;
    default: // null, discarded, binary
        lua_pushnil(L);
        break;
    }
}

json to_json(lua_State* L, int index) {
    std::unordered_set<const void*> seen;
    return to_json_impl(L, index, 0, seen);
}

PluginContext& context(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kContextKey);
    auto* ctx = static_cast<PluginContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *ctx;
}

void install_core_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    lua_pushlightuserdata(L, &ctx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kContextKey);

    auto* reg = new TimerRegistry;
    lua_pushlightuserdata(L, reg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTimersKey);
    ctx.rt.on_close([reg] {
        for (auto& [id, s] : reg->timers)
            stop(*s);
        delete reg;
    });

    luaL_newmetatable(L, kTimerMeta);
    lua_newtable(L);
    lua_pushcfunction(L, &timer_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg log_fns[] = {{"debug", &log_at<spdlog::level::debug>},
                                       {"info", &log_at<spdlog::level::info>},
                                       {"warn", &log_at<spdlog::level::warn>},
                                       {"error", &log_at<spdlog::level::err>},
                                       {nullptr, nullptr}};
    static const luaL_Reg json_fns[] = {{"encode", &json_encode}, {"decode", &json_decode}, {nullptr, nullptr}};
    static const luaL_Reg timer_fns[] = {{"after", &timer_after}, {"every", &timer_every}, {nullptr, nullptr}};
    add_module(L, "log", log_fns);
    add_module(L, "json", json_fns);
    add_module(L, "timer", timer_fns);

    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &sleep_ms);
    lua_setfield(L, -2, "sleep");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Append `src/plugin/lua_bind_core.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 6: Run to see it pass**

Run: `make t F='[core]'`
Expected: all 7 cases pass.

- [ ] **Step 7: ASAN on zeus**

Run: `scripts/zeus-run.sh asan '[core]'`
Expected: no reports.

- [ ] **Step 8: Commit**

```bash
git add include/lua_bindings.h src/plugin/lua_bind_core.cpp tests/unit/test_lua_bindings_core.cpp \
  tests/test_helpers/plugin_test_support.h \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): helix.log, helix.json, helix.timer and helix.sleep"
git show --stat HEAD
```

---

### Task 9: UI bindings (subjects, event handlers, toast, confirm)

**Files:**
- Create: `src/plugin/lua_bind_ui.cpp`
- Modify: `include/lua_bindings.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_ui.cpp`

**Interfaces:**
- Consumes: `lv_xml_register_subject`, `lv_xml_unregister_subject`, `lv_xml_get_subject` (`lib/helix-xml/src/xml/lv_xml.h:111,128,137`; scope `nullptr` is the global scope); `ToastManager::instance().show(ToastSeverity, const char*)` (global namespace, `include/ui_toast_manager.h:16,49`; enumerators `INFO SUCCESS WARNING ERROR`); `ModalSeverity` (global, `include/ui_modal.h:32`; `Info Warning Error`); `helix::ui::modal_confirm` and `helix::ui::ConfirmOptions` (`include/ui_modal.h:555,603`).
- Produces, added to `include/lua_bindings.h`:

```cpp
namespace helix::plugin {
/// Target of a plugin_event: "<id>_<name>[:arg]". `id` is empty when malformed.
struct PluginEventTarget { std::string id; std::string name; std::optional<std::string> arg; };
PluginEventTarget parse_plugin_event(std::string_view user_data);
/// Runs the helix.ui.on handler `name` of `rt` with `arg` (or nil). False if there is none.
bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name, const std::optional<std::string>& arg);
}
```

Lua surface: `helix.subject.int(name, init)`, `helix.subject.string(name, init)` register `<id>_<name>` (a name already registered raises); handle methods `:get()`, `:set(v)`, `:observe(fn)` where `fn(value)` runs on each change, not at registration; strings at most 1023 bytes. `helix.ui.on(name, fn)` replaces any handler of that name. `helix.ui.toast(msg, severity)` with `"info"` (default), `"success"`, `"warning"`, `"error"`. `helix.ui.confirm(title, msg, opts)` with `opts.severity` (`"info"`, `"warning"`, `"error"`), `opts.confirm_text` (default `"OK"`), `opts.on_confirm`, `opts.on_cancel`; a dismissal reaches `on_cancel`.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_lua_bindings_ui.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(LVGLTestFixture, "subjects register under the plugin prefix", "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        count = helix.subject.int("count", 3)
        label = helix.subject.string("label", "idle")
    )"));
    lv_subject_t* c = lv_xml_get_subject(nullptr, "test-plugin_count");
    lv_subject_t* l = lv_xml_get_subject(nullptr, "test-plugin_label");
    REQUIRE(c);
    REQUIRE(l);
    CHECK(lv_subject_get_int(c) == 3);
    CHECK(std::string(lv_subject_get_string(l)) == "idle");

    REQUIRE(b.t.run(R"(count:set(7); label:set("busy"); got = count:get() .. label:get())"));
    CHECK(lv_subject_get_int(c) == 7);
    CHECK(std::string(lv_subject_get_string(l)) == "busy");
    CHECK(b.t.global("got") == "7busy");
}

TEST_CASE_METHOD(LVGLTestFixture, "observers see changes from C++ and Lua, not registration",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        local s = helix.subject.int("n", 0)
        s:observe(function(v) seen[#seen + 1] = v end)
        s:set(1)
    )"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "test-plugin_n"), 2);
    REQUIRE(b.t.run("result = table.concat(seen, ',')"));
    CHECK(b.t.global("result") == "1,2");
}

TEST_CASE_METHOD(LVGLTestFixture, "an observer setting its own subject stops at the depth cap",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    b.t.run(R"(
        local s = helix.subject.int("loop", 0)
        s:observe(function(v) s:set(v + 1) end)
        s:set(1)
    )");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "test-plugin_loop")) <= 10);
}

TEST_CASE_METHOD(LVGLTestFixture, "subject names and sizes are validated", "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    CHECK_FALSE(b.t.run(R"(helix.subject.int("", 0))"));
    CHECK_FALSE(b.t.run(R"(helix.subject.int("has space", 0))"));
    REQUIRE(b.t.run(R"(helix.subject.int("dup", 0))"));
    CHECK_FALSE(b.t.run(R"(helix.subject.int("dup", 0))"));
    CHECK_FALSE(b.t.run(R"(helix.subject.string("big", string.rep("x", 2000)))"));
    REQUIRE(b.t.run(R"(s = helix.subject.string("small", ""))"));
    CHECK_FALSE(b.t.run(R"(s:set(string.rep("x", 2000)))"));
}

TEST_CASE_METHOD(LVGLTestFixture, "subjects are unregistered when the runtime closes",
                 "[plugin][bindings][ui]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("gone", 1))"));
        REQUIRE(lv_xml_get_subject(nullptr, "test-plugin_gone"));
    }
    CHECK(lv_xml_get_subject(nullptr, "test-plugin_gone") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui.on handlers dispatch with an argument", "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        calls = ""
        helix.ui.on("pick", function(arg) calls = calls .. tostring(arg) .. ";" end)
    )"));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::string("3")));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::nullopt));
    CHECK_FALSE(dispatch_ui_handler(*b.t.rt, "missing", std::nullopt));
    CHECK(b.t.global("calls") == "3;nil;");
}

TEST_CASE("plugin_event user_data parsing", "[plugin][bindings][ui]") {
    auto t = parse_plugin_event("orca-cal_start");
    CHECK(t.id == "orca-cal");
    CHECK(t.name == "start");
    CHECK_FALSE(t.arg);

    t = parse_plugin_event("orca-cal_pick:3:4");
    CHECK(t.name == "pick");
    CHECK(t.arg == std::optional<std::string>("3:4"));

    t = parse_plugin_event("orca-cal_my_handler");
    CHECK(t.name == "my_handler");

    CHECK(parse_plugin_event("noowner").id.empty());
    CHECK(parse_plugin_event("_x").id.empty());
    CHECK(parse_plugin_event("orca-cal_").id.empty());
    CHECK(parse_plugin_event("Bad_x").id.empty());
    CHECK(parse_plugin_event("").id.empty());
    CHECK(parse_plugin_event(":x").id.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "toast and confirm validate their arguments", "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        helix.ui.toast("hello")
        helix.ui.toast("careful", "warning")
        helix.ui.confirm("Title", "Body", { severity = "warning", on_confirm = function() end })
    )"));
    CHECK_FALSE(b.t.run(R"(helix.ui.toast("x", "loud"))"));
    CHECK_FALSE(b.t.run(R"(helix.ui.confirm("t", "m", { severity = "loud" }))"));
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[ui]'`
Expected: compile error for `dispatch_ui_handler`, or link error for `install_ui_bindings`.

- [ ] **Step 3: Implement**

Add to `include/lua_bindings.h` (with `<optional>` and `<string_view>`):

```cpp
/// Target of a plugin_event: "<id>_<name>[:arg]". `id` is empty when malformed.
struct PluginEventTarget {
    std::string id;
    std::string name;
    std::optional<std::string> arg;
};

PluginEventTarget parse_plugin_event(std::string_view user_data);

/// Runs the helix.ui.on handler `name` of `rt` with `arg` (or nil). False if there is none.
bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name, const std::optional<std::string>& arg);
```

`src/plugin/lua_bind_ui.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"
#include "ui_modal.h"
#include "ui_toast_manager.h"

#include <spdlog/spdlog.h>

#include <climits>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kSubjectMeta[] = "helix.subject";
const char kUiStateKey = 0;
constexpr size_t kMaxString = 1024;

struct ObserverCtx {
    LuaRuntime* rt;
    int fn_ref;
    bool armed = false; ///< lv_subject_add_observer reports the current value at once; Lua sees changes only
};

struct SubjectEntry {
    std::string full_name;
    lv_subject_t subject{};
    std::vector<char> buf;
    std::vector<char> prev;
    bool is_string = false;
};

// Everything one runtime registered here. Freed by the runtime's closer.
struct UiState {
    std::vector<std::unique_ptr<SubjectEntry>> subjects;
    std::vector<std::unique_ptr<ObserverCtx>> observers;
    std::unordered_map<std::string, int> handlers; ///< name -> fn ref
};

UiState& ui_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    auto* s = static_cast<UiState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

bool is_valid_local_name(std::string_view n) {
    if (n.empty() || n.size() > 48)
        return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void on_subject_change(lv_observer_t* obs, lv_subject_t* subject) {
    auto* ctx = static_cast<ObserverCtx*>(lv_observer_get_user_data(obs));
    if (!ctx->armed)
        return;
    bool is_string = subject->type == LV_SUBJECT_TYPE_STRING;
    std::string s = is_string ? lv_subject_get_string(subject) : std::string();
    int32_t v = is_string ? 0 : lv_subject_get_int(subject);
    ctx->rt->invoke(ctx->fn_ref, [is_string, s, v](lua_State* co) {
        if (is_string)
            lua_pushlstring(co, s.data(), s.size());
        else
            lua_pushinteger(co, v);
        return 1;
    });
}

SubjectEntry& check_subject(lua_State* L) {
    return **static_cast<SubjectEntry**>(luaL_checkudata(L, 1, kSubjectMeta));
}

int subject_get(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string)
        lua_pushstring(L, lv_subject_get_string(&s.subject));
    else
        lua_pushinteger(L, lv_subject_get_int(&s.subject));
    return 1;
}

int subject_set(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string) {
        size_t len = 0;
        const char* v = luaL_checklstring(L, 2, &len);
        luaL_argcheck(L, len < kMaxString, 2, "string longer than 1023 bytes");
        lv_subject_copy_string(&s.subject, v);
    } else {
        lua_Integer v = luaL_checkinteger(L, 2);
        luaL_argcheck(L, v >= INT32_MIN && v <= INT32_MAX, 2, "integer out of range");
        lv_subject_set_int(&s.subject, static_cast<int32_t>(v));
    }
    return 0;
}

int subject_observe(lua_State* L) {
    auto& s = check_subject(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    auto& ui = ui_state(L);
    ui.observers.push_back(std::make_unique<ObserverCtx>(ObserverCtx{&rt, rt.ref_value(L, 2)}));
    ObserverCtx* ctx = ui.observers.back().get();
    lv_subject_add_observer(&s.subject, &on_subject_change, ctx);
    ctx->armed = true;
    return 0;
}

int make_subject(lua_State* L, bool is_string) {
    auto& rt = LuaRuntime::from(L);
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "subject name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    std::string full = rt.plugin_id() + "_" + name;
    if (lv_xml_get_subject(nullptr, full.c_str()))
        return luaL_error(L, "subject '%s' already exists", full.c_str());

    auto entry = std::make_unique<SubjectEntry>();
    entry->full_name = full;
    entry->is_string = is_string;
    if (is_string) {
        size_t len = 0;
        const char* init = luaL_optlstring(L, 2, "", &len);
        if (len >= kMaxString)
            return luaL_error(L, "subject '%s': initial value longer than 1023 bytes", full.c_str());
        entry->buf.assign(kMaxString, 0);
        entry->prev.assign(kMaxString, 0);
        lv_subject_init_string(&entry->subject, entry->buf.data(), entry->prev.data(), kMaxString, init);
    } else {
        lua_Integer init = luaL_optinteger(L, 2, 0);
        luaL_argcheck(L, init >= INT32_MIN && init <= INT32_MAX, 2, "integer out of range");
        lv_subject_init_int(&entry->subject, static_cast<int32_t>(init));
    }
    lv_xml_register_subject(nullptr, full.c_str(), &entry->subject);

    SubjectEntry* raw = entry.get();
    ui_state(L).subjects.push_back(std::move(entry));
    *static_cast<SubjectEntry**>(lua_newuserdatauv(L, sizeof(SubjectEntry*), 0)) = raw;
    luaL_setmetatable(L, kSubjectMeta);
    return 1;
}

int subject_int(lua_State* L) {
    return make_subject(L, false);
}

int subject_string(lua_State* L) {
    return make_subject(L, true);
}

int ui_on(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "handler name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& handlers = ui_state(L).handlers;
    if (auto it = handlers.find(name); it != handlers.end())
        rt.unref(it->second);
    handlers[name] = rt.ref_value(L, 2);
    return 0;
}

int ui_toast(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    static const char* const kNames[] = {"info", "success", "warning", "error", nullptr};
    static const ToastSeverity kSeverity[] = {ToastSeverity::INFO, ToastSeverity::SUCCESS,
                                              ToastSeverity::WARNING, ToastSeverity::ERROR};
    int i = luaL_checkoption(L, 2, "info", kNames);
    ToastManager::instance().show(kSeverity[i], msg);
    return 0;
}

int ui_confirm(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string title = luaL_checkstring(L, 1);
    std::string msg = luaL_checkstring(L, 2);
    ModalSeverity severity = ModalSeverity::Info;
    std::string confirm_text = "OK";
    int confirm_ref = LUA_NOREF;
    int cancel_ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        static const char* const kNames[] = {"info", "warning", "error", nullptr};
        static const ModalSeverity kSeverity[] = {ModalSeverity::Info, ModalSeverity::Warning,
                                                  ModalSeverity::Error};
        lua_getfield(L, 3, "severity");
        if (!lua_isnil(L, -1))
            severity = kSeverity[luaL_checkoption(L, -1, nullptr, kNames)];
        lua_pop(L, 1);
        lua_getfield(L, 3, "confirm_text");
        if (lua_isstring(L, -1))
            confirm_text = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_confirm");
        if (lua_isfunction(L, -1))
            confirm_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_cancel");
        if (lua_isfunction(L, -1))
            cancel_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
    }
    LuaRuntime* rtp = &rt;
    auto run = [rtp](int ref) {
        if (ref != LUA_NOREF)
            rtp->invoke(ref);
    };
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [run, cancel_ref] { run(cancel_ref); };
    opts.on_dismiss = opts.on_cancel;
    opts.owner_token = rt.token();
    helix::ui::modal_confirm(title.c_str(), msg.c_str(), severity, confirm_text.c_str(),
                             [run, confirm_ref] { run(confirm_ref); }, opts);
    return 0;
}

} // namespace

PluginEventTarget parse_plugin_event(std::string_view user_data) {
    PluginEventTarget t;
    std::string_view head = user_data;
    std::optional<std::string> arg;
    if (auto colon = user_data.find(':'); colon != std::string_view::npos) {
        head = user_data.substr(0, colon);
        arg = std::string(user_data.substr(colon + 1));
    }
    std::string_view id = owner_of(head);
    if (!is_valid_plugin_id(id) || head.size() <= id.size() + 1)
        return t;
    t.id = std::string(id);
    t.name = std::string(head.substr(id.size() + 1));
    t.arg = std::move(arg);
    return t;
}

bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name, const std::optional<std::string>& arg) {
    auto& handlers = ui_state(rt.state()).handlers;
    auto it = handlers.find(name);
    if (it == handlers.end())
        return false;
    rt.invoke(it->second, [arg](lua_State* co) {
        if (arg)
            lua_pushlstring(co, arg->data(), arg->size());
        else
            lua_pushnil(co);
        return 1;
    });
    return true;
}

void install_ui_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new UiState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    ctx.rt.on_close([state] {
        for (auto& s : state->subjects) {
            lv_xml_unregister_subject(nullptr, s->full_name.c_str());
            lv_subject_deinit(&s->subject); // also removes every observer on it
        }
        delete state;
    });

    static const luaL_Reg methods[] = {
        {"get", &subject_get}, {"set", &subject_set}, {"observe", &subject_observe}, {nullptr, nullptr}};
    luaL_newmetatable(L, kSubjectMeta);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    static const luaL_Reg subject_fns[] = {{"int", &subject_int}, {"string", &subject_string}, {nullptr, nullptr}};
    static const luaL_Reg ui_fns[] = {
        {"on", &ui_on}, {"toast", &ui_toast}, {"confirm", &ui_confirm}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, subject_fns, 0);
    lua_setfield(L, -2, "subject");
    lua_newtable(L);
    luaL_setfuncs(L, ui_fns, 0);
    lua_setfield(L, -2, "ui");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Handler and confirm-callback refs are freed by `lua_close`. Append `src/plugin/lua_bind_ui.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[ui]'`
Expected: all 8 cases pass.

- [ ] **Step 5: Mutation check**

Run: `make mutate-diff`
Expected: no surviving mutant in `parse_plugin_event`, `is_valid_local_name` or the duplicate check.

- [ ] **Step 6: Commit**

```bash
git add include/lua_bindings.h src/plugin/lua_bind_ui.cpp tests/unit/test_lua_bindings_ui.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): helix.subject, helix.ui.on, toast and confirm for Lua plugins"
git show --stat HEAD
```

---

### Task 10: Printer bindings

**Files:**
- Create: `src/plugin/lua_bind_printer.cpp`
- Modify: `include/lua_bindings.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_printer.cpp`

**Interfaces:**
- Consumes: `lv_xml_get_subject(nullptr, name)`, `helix::ConnectionState` (`include/connection_state.h:14`), `XMLTestFixture` (`tests/test_fixtures.h:268`, which owns a `PrinterState` with XML-registered subjects). Temperature subjects hold decidegrees.
- Produces, added to `include/lua_bindings.h`:

```cpp
enum class PrinterValueKind { Bool, String, Int, DeciDegrees };
struct PrinterField { const char* lua_name; const char* subject; PrinterValueKind kind; };
/// The stable table from the spec's "Printer state" section.
const std::vector<PrinterField>& printer_fields();
```

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_lua_bindings_printer.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "connection_state.h"
#include "lua_bindings.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_test_support.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(XMLTestFixture, "printer.get maps the stable names", "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    lv_subject_set_int(lv_xml_get_subject(nullptr, "bed_temp"), 605);
    lv_subject_copy_string(lv_xml_get_subject(nullptr, "print_state"), "printing");
    lv_subject_set_int(lv_xml_get_subject(nullptr, "printer_connection_state"),
                       static_cast<int>(helix::ConnectionState::CONNECTED));
    REQUIRE(b.t.run(R"(
        bed = helix.printer.get("bed_temp")
        state = helix.printer.get("print_state")
        conn = helix.printer.get("connected")
    )"));
    CHECK(b.t.global("bed") == "60.5");
    CHECK(b.t.global("state") == "printing");
    CHECK(b.t.global("conn") == "true");
}

TEST_CASE_METHOD(XMLTestFixture, "printer.get refuses names outside the table",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    CHECK_FALSE(b.t.run(R"(helix.printer.get("print_state_enum"))"));
    CHECK_FALSE(b.t.run(R"(helix.printer.get("chamber_effective_target"))"));
}

TEST_CASE_METHOD(XMLTestFixture, "printer.watch reports changes in Lua units",
                 "[plugin][bindings][printer]") {
    BoundRuntime b({&install_printer_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        helix.printer.watch("extruder_target", function(v) seen[#seen + 1] = v end)
    )"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "extruder_target"), 2150);
    REQUIRE(b.t.run("r = table.concat(seen, ',')"));
    CHECK(b.t.global("r") == "215.0");
}

TEST_CASE_METHOD(XMLTestFixture, "printer watchers detach when the runtime closes",
                 "[plugin][bindings][printer]") {
    {
        BoundRuntime b({&install_printer_bindings});
        REQUIRE(b.t.run(R"(helix.printer.watch("bed_temp", function() end))"));
    }
    lv_subject_set_int(lv_xml_get_subject(nullptr, "bed_temp"), 1);
    SUCCEED(); // ASAN (Step 5) is what proves the closed state was not called
}

TEST_CASE("the stable printer table", "[plugin][bindings][printer]") {
    REQUIRE(printer_fields().size() == 10);
    CHECK(std::string(printer_fields()[0].lua_name) == "connected");
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[printer]'`
Expected: link error for `install_printer_bindings`.

- [ ] **Step 3: Implement**

Add the Produces declarations to `include/lua_bindings.h` (with `<vector>`).

`src/plugin/lua_bind_printer.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "connection_state.h"
#include "lua_bindings.h"

namespace helix::plugin {

namespace {

const char kPrinterStateKey = 0;

struct Watch {
    LuaRuntime* rt;
    int fn_ref;
    const PrinterField* field;
    bool armed = false;
    lv_observer_t* observer = nullptr;
};

// Printer subjects outlive every plugin, so every observer is removed when the runtime closes.
struct PrinterBindState {
    std::vector<std::unique_ptr<Watch>> watches;
};

PrinterBindState& printer_state_of(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kPrinterStateKey);
    auto* s = static_cast<PrinterBindState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

const PrinterField* find_field(std::string_view name) {
    for (const auto& f : printer_fields()) {
        if (name == f.lua_name)
            return &f;
    }
    return nullptr;
}

// A plain copy of a field's value, so it can cross into a Lua entry after the LVGL notify.
struct FieldValue {
    PrinterValueKind kind;
    int32_t i = 0;
    std::string s;
};

FieldValue read_field(const PrinterField& f, lv_subject_t* subject) {
    FieldValue v{f.kind};
    if (f.kind == PrinterValueKind::String)
        v.s = lv_subject_get_string(subject);
    else
        v.i = lv_subject_get_int(subject);
    return v;
}

int push_field(lua_State* L, const FieldValue& v) {
    switch (v.kind) {
    case PrinterValueKind::Bool:
        lua_pushboolean(L, v.i == static_cast<int>(helix::ConnectionState::CONNECTED));
        break;
    case PrinterValueKind::String:
        lua_pushlstring(L, v.s.data(), v.s.size());
        break;
    case PrinterValueKind::Int:
        lua_pushinteger(L, v.i);
        break;
    case PrinterValueKind::DeciDegrees:
        lua_pushnumber(L, v.i / 10.0);
        break;
    }
    return 1;
}

void on_printer_change(lv_observer_t* obs, lv_subject_t* subject) {
    auto* w = static_cast<Watch*>(lv_observer_get_user_data(obs));
    if (!w->armed)
        return;
    FieldValue v = read_field(*w->field, subject);
    w->rt->invoke(w->fn_ref, [v](lua_State* co) { return push_field(co, v); });
}

int printer_get(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    const PrinterField* f = find_field(name);
    if (!f)
        return luaL_error(L, "unknown printer field '%s'", name.c_str());
    lv_subject_t* subject = lv_xml_get_subject(nullptr, f->subject);
    if (!subject) {
        lua_pushnil(L);
        return 1;
    }
    return push_field(L, read_field(*f, subject));
}

int printer_watch(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const PrinterField* f = find_field(name);
    if (!f)
        return luaL_error(L, "unknown printer field '%s'", name.c_str());
    lv_subject_t* subject = lv_xml_get_subject(nullptr, f->subject);
    if (!subject)
        return luaL_error(L, "printer field '%s' is not available yet", name.c_str());
    auto& rt = LuaRuntime::from(L);
    auto& state = printer_state_of(L);
    state.watches.push_back(std::make_unique<Watch>(Watch{&rt, rt.ref_value(L, 2), f}));
    Watch* w = state.watches.back().get();
    w->observer = lv_subject_add_observer(subject, &on_printer_change, w);
    w->armed = true;
    return 0;
}

} // namespace

const std::vector<PrinterField>& printer_fields() {
    static const std::vector<PrinterField> kFields{
        {"connected", "printer_connection_state", PrinterValueKind::Bool},
        {"print_state", "print_state", PrinterValueKind::String},
        {"progress", "print_progress", PrinterValueKind::Int},
        {"filename", "print_filename", PrinterValueKind::String},
        {"extruder_temp", "extruder_temp", PrinterValueKind::DeciDegrees},
        {"extruder_target", "extruder_target", PrinterValueKind::DeciDegrees},
        {"bed_temp", "bed_temp", PrinterValueKind::DeciDegrees},
        {"bed_target", "bed_target", PrinterValueKind::DeciDegrees},
        {"chamber_temp", "chamber_temp", PrinterValueKind::DeciDegrees},
        {"chamber_target", "chamber_effective_target", PrinterValueKind::DeciDegrees},
    };
    return kFields;
}

void install_printer_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new PrinterBindState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kPrinterStateKey);
    ctx.rt.on_close([state] {
        for (auto& w : state->watches) {
            if (w->observer)
                lv_observer_remove(w->observer);
        }
        delete state;
    });

    static const luaL_Reg fns[] = {{"get", &printer_get}, {"watch", &printer_watch}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "printer");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Append `src/plugin/lua_bind_printer.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[printer]'`
Expected: all 5 cases pass.

- [ ] **Step 5: ASAN on zeus**

Run: `scripts/zeus-run.sh asan '[printer]'`
Expected: no reports; the detach case catches an observer left behind.

- [ ] **Step 6: Commit**

```bash
git add include/lua_bindings.h src/plugin/lua_bind_printer.cpp tests/unit/test_lua_bindings_printer.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): helix.printer.get and watch over a stable field table"
git show --stat HEAD
```

---

### Task 11: Moonraker and G-code bindings

**Files:**
- Create: `src/plugin/lua_bind_moonraker.cpp`
- Modify: `include/lua_bindings.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_moonraker.cpp`

**Interfaces:**
- Consumes: `PluginContext` (`backend`, `manifest.permissions`), `is_readonly_moonraker_method`, `push_json`, `to_json`, `LuaRuntime::await_async`, `helix::json_util::safe_string(j, key, def)` (`include/json_utils.h:273`).
- Produces, added to `include/lua_bindings.h` for Task 12 to reuse:

```cpp
/// Raises "<call> needs the '<permission>' permission in manifest.json" unless granted.
void require_permission(lua_State* L, Permission p, const char* call);
using PushRpc = std::function<void(lua_State*, const RpcResult&)>;
/// An RpcCallback that resumes `p` with on_ok's value, or with (nil, error) on failure.
RpcCallback make_resolver(LuaRuntime::Pending p, PushRpc on_ok);
void push_rpc_true(lua_State* co, const RpcResult&);
void push_rpc_value(lua_State* co, const RpcResult&);
```

Lua surface:
- `helix.gcode(script)` → `true`, or `nil, err`. Needs `gcode`.
- `helix.moonraker.query(objects)` → result, or `nil, err`. `objects` maps names to a field list or `true` (all fields, sent as `null`).
- `helix.moonraker.call(method, params)` → result, or `nil, err`. Needs `moonraker_write` unless the method is read-only.
- `helix.moonraker.upload(root, path, content)` → `true`, or `nil, err`. Needs `moonraker_write`.
- `helix.moonraker.download(root, path)` → content, or `nil, err`. Needs `moonraker_write`.
- `helix.moonraker.on_agent_event(event, fn)` → nothing; `fn(agent, data)` for every `notify_agent_event` whose `params[1].event` is `event`.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_lua_bindings_moonraker.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
void drain() {
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "gcode needs its permission", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    CHECK_FALSE(b.t.run(R"(helix.gcode("G28"))"));
    CHECK(b.fake.requests.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "gcode suspends until Moonraker answers", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings}, {Permission::Gcode});
    REQUIRE(b.t.run(R"(ok, err = helix.gcode("G28"); done = true)"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].kind == "gcode");
    CHECK(b.fake.requests[0].a == "G28");
    CHECK(b.t.global("done") == "nil");

    b.fake.requests[0].reply(RpcResult{false, {}, "Must home axis first"});
    drain();
    CHECK(b.t.global("done") == "true");
    CHECK(b.t.global("ok") == "nil");
    CHECK(b.t.global("err") == "Must home axis first");
}

TEST_CASE_METHOD(LVGLTestFixture, "call enforces the read-only allowlist", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(helix.moonraker.call("server.info", {}))"));
    CHECK_FALSE(b.t.run(R"(helix.moonraker.call("printer.restart", {}))"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].a == "server.info");
    CHECK(b.fake.requests[0].params == json::object());

    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(helix.moonraker.call("printer.restart"))"));
    CHECK(w.fake.requests.size() == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "query builds printer.objects.query params", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        r = helix.moonraker.query({ extruder = { "temperature" }, toolhead = true })
        t = r and r.status.extruder.temperature
    )"));
    REQUIRE(b.fake.requests.size() == 1);
    CHECK(b.fake.requests[0].a == "printer.objects.query");
    CHECK(b.fake.requests[0].params ==
          json{{"objects", {{"extruder", {"temperature"}}, {"toolhead", nullptr}}}});
    b.fake.requests[0].reply(RpcResult{true, json{{"status", {{"extruder", {{"temperature", 210.5}}}}}}, {}});
    drain();
    CHECK(b.t.global("t") == "210.5");
}

TEST_CASE_METHOD(LVGLTestFixture, "upload and download need moonraker_write", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    CHECK_FALSE(b.t.run(R"(helix.moonraker.upload("gcodes", "a.gcode", "G28"))"));
    CHECK_FALSE(b.t.run(R"(helix.moonraker.download("gcodes", "a.gcode"))"));

    BoundRuntime w({&install_moonraker_bindings}, {Permission::MoonrakerWrite});
    REQUIRE(w.t.run(R"(up = helix.moonraker.upload("gcodes", "a.gcode", "G28\n"))"));
    REQUIRE(w.t.run(R"(body = helix.moonraker.download("gcodes", "a.gcode"))"));
    REQUIRE(w.fake.requests.size() == 2);
    CHECK(w.fake.requests[0].kind == "upload");
    CHECK(w.fake.requests[0].c == "G28\n");
    w.fake.requests[0].reply(RpcResult{true, {}, {}});
    w.fake.requests[1].reply(RpcResult{true, json("G28\n"), {}});
    drain();
    CHECK(w.t.global("up") == "true");
    CHECK(w.t.global("body") == "G28\n");
}

TEST_CASE_METHOD(LVGLTestFixture, "agent events are filtered by event name", "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(
        got = {}
        helix.moonraker.on_agent_event("result", function(agent, data)
            got[#got + 1] = agent .. "=" .. data.temp
        end)
    )"));
    REQUIRE(b.fake.notify.size() == 1);
    CHECK(b.fake.notify[0].first == "notify_agent_event");
    auto& handler = b.fake.notify[0].second;
    handler(json{{"params", {{{"agent", "orca"}, {"event", "other"}, {"data", {{"temp", 1}}}}}}});
    handler(json{{"params", {{{"agent", "orca"}, {"event", "result"}, {"data", {{"temp", 210}}}}}}});
    handler(json{{"params", "malformed"}});
    handler(json::object());
    drain();
    REQUIRE(b.t.run("r = table.concat(got, ',')"));
    CHECK(b.t.global("r") == "orca=210");
}

TEST_CASE_METHOD(LVGLTestFixture, "agent handlers unregister when the runtime closes",
                 "[plugin][bindings][moonraker]") {
    BoundRuntime b({&install_moonraker_bindings});
    REQUIRE(b.t.run(R"(helix.moonraker.on_agent_event("x", function() end))"));
    CHECK(b.fake.notify_unregistered == 0);
    b.t.rt.reset();
    CHECK(b.fake.notify_unregistered == 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "an agent event after unload is dropped", "[plugin][bindings][moonraker]") {
    std::function<void(const json&)> handler;
    {
        BoundRuntime b({&install_moonraker_bindings});
        REQUIRE(b.t.run(R"(helix.moonraker.on_agent_event("x", function() end))"));
        handler = b.fake.notify[0].second;
    }
    handler(json{{"params", {{{"agent", "a"}, {"event", "x"}}}}});
    drain();
    SUCCEED(); // ASAN is what proves the closed runtime was not touched
}

TEST_CASE_METHOD(LVGLTestFixture, "a late reply after unload is dropped", "[plugin][bindings][moonraker]") {
    RpcCallback late;
    {
        BoundRuntime b({&install_moonraker_bindings}, {Permission::Gcode});
        REQUIRE(b.t.run(R"(helix.gcode("G28"))"));
        late = b.fake.requests[0].reply;
    }
    late(RpcResult{true, {}, {}});
    drain();
    SUCCEED();
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[moonraker]'`
Expected: link error for `install_moonraker_bindings`.

- [ ] **Step 3: Implement**

Add the Produces declarations to `include/lua_bindings.h`.

`src/plugin/lua_bind_moonraker.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "json_utils.h"
#include "lua_bindings.h"

namespace helix::plugin {

void require_permission(lua_State* L, Permission p, const char* call) {
    if (!context(L).manifest.permissions.count(p))
        luaL_error(L, "%s needs the '%s' permission in manifest.json", call, permission_name(p));
}

RpcCallback make_resolver(LuaRuntime::Pending p, PushRpc on_ok) {
    return [p, on_ok = std::move(on_ok)](RpcResult r) {
        p.resolve([r = std::move(r), on_ok](lua_State* co) -> int {
            if (!r.ok) {
                lua_pushnil(co);
                lua_pushlstring(co, r.error.data(), r.error.size());
                return 2;
            }
            on_ok(co, r);
            return 1;
        });
    };
}

void push_rpc_true(lua_State* co, const RpcResult&) {
    lua_pushboolean(co, 1);
}

void push_rpc_value(lua_State* co, const RpcResult& r) {
    push_json(co, r.value);
}

namespace {

// An empty Lua table converts to an array; Moonraker wants an object for params.
json params_arg(lua_State* L, int index) {
    if (lua_isnoneornil(L, index))
        return json::object();
    json p = to_json(L, index);
    if (p.is_array() && p.empty())
        return json::object();
    return p;
}

int gcode(lua_State* L) {
    require_permission(L, Permission::Gcode, "helix.gcode");
    std::string script = luaL_checkstring(L, 1);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, script](LuaRuntime::Pending p) {
        backend->gcode(script, make_resolver(p, &push_rpc_true));
    });
}

int call(lua_State* L) {
    std::string method = luaL_checkstring(L, 1);
    if (!is_readonly_moonraker_method(method))
        require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.call");
    json params = params_arg(L, 2);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, method, params](LuaRuntime::Pending p) {
        backend->call(method, params, make_resolver(p, &push_rpc_value));
    });
}

int query(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    json objects = json::object();
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "helix.moonraker.query: object names must be strings");
        std::string name = lua_tostring(L, -2);
        objects[name] = lua_isboolean(L, -1) ? json(nullptr) : to_json(L, -1);
        lua_pop(L, 1);
    }
    json params{{"objects", objects}};
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, params](LuaRuntime::Pending p) {
        backend->call("printer.objects.query", params, make_resolver(p, &push_rpc_value));
    });
}

int upload(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.upload");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 3, &len);
    std::string content(data, len);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, root, path, content](LuaRuntime::Pending p) {
        backend->upload(root, path, content, make_resolver(p, &push_rpc_true));
    });
}

int download(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.download");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, root, path](LuaRuntime::Pending p) {
        backend->download(root, path, make_resolver(p, &push_rpc_value));
    });
}

int on_agent_event(lua_State* L) {
    std::string event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    int ref = rt.ref_value(L, 2);
    LuaRuntime* rtp = &rt;
    LifetimeToken token = rt.token();
    // Runs on the WebSocket thread: inspects JSON only, then defers to the main thread.
    auto off = context(L).backend.on_notify("notify_agent_event", [rtp, token, ref, event](const json& msg) {
        auto params = msg.find("params");
        if (params == msg.end() || !params->is_array() || params->empty() || !(*params)[0].is_object())
            return;
        const json& p0 = (*params)[0];
        if (helix::json_util::safe_string(p0, "event", "") != event)
            return;
        std::string agent = helix::json_util::safe_string(p0, "agent", "");
        json data = p0.contains("data") ? p0["data"] : json();
        token.defer("plugin_agent_event", [rtp, ref, agent, data]() {
            rtp->invoke(ref, [agent, data](lua_State* co) {
                lua_pushlstring(co, agent.data(), agent.size());
                push_json(co, data);
                return 2;
            });
        });
    });
    rt.on_close([off]() { off(); });
    return 0;
}

} // namespace

void install_moonraker_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    static const luaL_Reg fns[] = {{"call", &call},
                                   {"query", &query},
                                   {"upload", &upload},
                                   {"download", &download},
                                   {"on_agent_event", &on_agent_event},
                                   {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &gcode);
    lua_setfield(L, -2, "gcode");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "moonraker");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Check `safe_string`'s exact signature and return type in `include/json_utils.h` and adapt the two calls. Append `src/plugin/lua_bind_moonraker.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[moonraker]'`
Expected: all 9 cases pass.

- [ ] **Step 5: Mutation check and ASAN**

Run: `make mutate-diff`
Expected: no surviving mutant at a `require_permission` call site or the allowlist branch.

Run: `scripts/zeus-run.sh asan '[moonraker]'`
Expected: no reports.

- [ ] **Step 6: Commit**

```bash
git add include/lua_bindings.h src/plugin/lua_bind_moonraker.cpp tests/unit/test_lua_bindings_moonraker.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): helix.gcode and helix.moonraker bindings behind permissions"
git show --stat HEAD
```

---

### Task 12: HTTP, storage and settings bindings

**Files:**
- Create: `src/plugin/lua_bind_io.cpp`
- Modify: `include/lua_bindings.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_io.cpp`

**Interfaces:**
- Consumes: `PluginContext` (`backend.http`, `settings`, `save_settings`, `manifest`, `storage_path`), `require_permission`, `make_resolver`, `push_rpc_value` (Task 11), `push_json`, `to_json`.
- Produces, added to `include/lua_bindings.h` (Phase 2's settings screen uses both):

```cpp
/// Stores `value` in the plugin's settings, saves, and runs its on_change handlers. False,
/// with nothing stored, when `key` is undeclared or `value` does not fit its declaration.
bool set_plugin_setting(PluginContext& ctx, const std::string& key, const json& value);
/// <dir of settings_path>/plugin-data/<id>.json
std::string plugin_storage_path(const std::string& settings_path, const std::string& id);
```

Lua surface:
- `helix.http.get(url, opts)`, `helix.http.post(url, opts)` → `{status=, body=}`, or `nil, err`. Need `http`. `opts.headers` (string to string), `opts.body` (string), `opts.timeout_ms` (default 10000, clamped to 1-60000). Only `http://` and `https://`.
- `helix.storage.get(key)`, `helix.storage.set(key, value)`. Need `storage`. `set(key, nil)` deletes. A `set` that would make the file exceed 256 KB raises and changes nothing. Writes go to `<path>.tmp` then rename.
- `helix.settings.get(key)` → saved value when it still fits its declaration, else the manifest default, else nil. An undeclared key raises.
- `helix.settings.on_change(key, fn)` → `fn(value)` after `set_plugin_setting` changes it.

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_lua_bindings_io.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
SettingDecl step_decl() {
    SettingDecl d;
    d.key = "step";
    d.label = "Step";
    d.type = SettingType::Int;
    d.min = 1;
    d.max = 20;
    d.default_value = 5;
    return d;
}
} // namespace

TEST_CASE("storage path sits beside settings.json", "[plugin][bindings][io]") {
    CHECK(plugin_storage_path("/home/pi/helixscreen/config/settings.json", "orca-cal") ==
          "/home/pi/helixscreen/config/plugin-data/orca-cal.json");
}

TEST_CASE_METHOD(LVGLTestFixture, "http needs its permission and a web URL", "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings});
    CHECK_FALSE(b.t.run(R"(helix.http.get("https://example.com"))"));

    BoundRuntime h({&install_io_bindings}, {Permission::Http});
    CHECK_FALSE(h.t.run(R"(helix.http.get("file:///etc/passwd"))"));
    CHECK_FALSE(h.t.run(R"(helix.http.get("https://x", { headers = { A = 1 } }))"));
    REQUIRE(h.t.run(R"(r = helix.http.post("https://example.com/x", { body = "hi", headers = { A = "b" } }))"));
    REQUIRE(h.fake.requests.size() == 1);
    CHECK(h.fake.requests[0].a == "POST");
    CHECK(h.fake.requests[0].b == "https://example.com/x");
    CHECK(h.fake.requests[0].c == "hi");
    CHECK(h.fake.requests[0].params == json{{"A", "b"}});
    h.fake.requests[0].reply(RpcResult{true, json{{"status", 404}, {"body", "nope"}}, {}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.t.run("s = r.status .. r.body"));
    CHECK(h.t.global("s") == "404nope");
}

TEST_CASE_METHOD(LVGLTestFixture, "storage persists across runtimes", "[plugin][bindings][io]") {
    TempDir dir;
    std::string path = dir.file("data/s.json");
    {
        BoundRuntime b({&install_io_bindings}, {Permission::Storage}, {}, path);
        REQUIRE(b.t.run(R"(
            helix.storage.set("best", { temp = 215 })
            helix.storage.set("gone", 1)
            helix.storage.set("gone", nil)
        )"));
    }
    BoundRuntime b({&install_io_bindings}, {Permission::Storage}, {}, path);
    REQUIRE(b.t.run(R"(t = helix.storage.get("best").temp; g = helix.storage.get("gone"))"));
    CHECK(b.t.global("t") == "215");
    CHECK(b.t.global("g") == "nil");
}

TEST_CASE_METHOD(LVGLTestFixture, "storage refuses to grow past 256 KB", "[plugin][bindings][io]") {
    TempDir dir;
    BoundRuntime b({&install_io_bindings}, {Permission::Storage}, {}, dir.file("s.json"));
    REQUIRE(b.t.run(R"(helix.storage.set("a", string.rep("x", 200 * 1024)))"));
    CHECK_FALSE(b.t.run(R"(helix.storage.set("b", string.rep("y", 100 * 1024)))"));
    REQUIRE(b.t.run(R"(still = #helix.storage.get("a"); bv = helix.storage.get("b"))"));
    CHECK(b.t.global("still") == "204800");
    CHECK(b.t.global("bv") == "nil");
}

TEST_CASE_METHOD(LVGLTestFixture, "a corrupt storage file reads as empty", "[plugin][bindings][io]") {
    TempDir dir;
    std::string path = dir.file("s.json");
    { std::ofstream(path) << "{not json"; }
    BoundRuntime b({&install_io_bindings}, {Permission::Storage}, {}, path);
    REQUIRE(b.t.run(R"(v = helix.storage.get("x"))"));
    CHECK(b.t.global("v") == "nil");
}

TEST_CASE_METHOD(LVGLTestFixture, "storage needs its permission", "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings});
    CHECK_FALSE(b.t.run(R"(helix.storage.get("x"))"));
}

TEST_CASE_METHOD(LVGLTestFixture, "settings fall back to manifest defaults", "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings}, {}, {step_decl()});
    REQUIRE(b.t.run(R"(v = helix.settings.get("step"))"));
    CHECK(b.t.global("v") == "5");
    CHECK_FALSE(b.t.run(R"(helix.settings.get("nope"))"));

    b.settings["step"] = "not a number"; // stale value from an older manifest
    REQUIRE(b.t.run(R"(v = helix.settings.get("step"))"));
    CHECK(b.t.global("v") == "5");
}

TEST_CASE_METHOD(LVGLTestFixture, "set_plugin_setting validates, saves and notifies", "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings}, {}, {step_decl()});
    REQUIRE(b.t.run(R"(
        changes = {}
        helix.settings.on_change("step", function(v) changes[#changes + 1] = v end)
    )"));
    CHECK(set_plugin_setting(*b.ctx, "step", 9));
    CHECK_FALSE(set_plugin_setting(*b.ctx, "step", 99));
    CHECK_FALSE(set_plugin_setting(*b.ctx, "step", "9"));
    CHECK_FALSE(set_plugin_setting(*b.ctx, "nope", 1));
    REQUIRE(b.t.run(R"(v = helix.settings.get("step"); c = table.concat(changes, ","))"));
    CHECK(b.t.global("v") == "9");
    CHECK(b.t.global("c") == "9");
    CHECK(b.saves == 1);
}

#endif // HELIX_HAS_PLUGINS
```

Add `#include <fstream>` for the corrupt-file case.

- [ ] **Step 2: Run to see it fail**

Run: `make t F='[io]'`
Expected: compile error for `plugin_storage_path` / `set_plugin_setting`.

- [ ] **Step 3: Implement**

Add the Produces declarations to `include/lua_bindings.h`.

`src/plugin/lua_bind_io.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kIoStateKey = 0;
constexpr size_t kMaxStorageBytes = 256 * 1024;
constexpr size_t kMaxSettingString = 1024;

struct IoState {
    std::optional<json> storage; ///< loaded on first use
    std::unordered_map<std::string, std::vector<int>> on_change;
};

IoState& io_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kIoStateKey);
    auto* s = static_cast<IoState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

bool is_web_url(const std::string& u) {
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

int http_request(lua_State* L, const char* method, const char* call_name) {
    require_permission(L, Permission::Http, call_name);
    std::string url = luaL_checkstring(L, 1);
    luaL_argcheck(L, is_web_url(url), 1, "only http:// and https:// URLs");
    std::string body;
    json headers = json::object();
    lua_Integer timeout = 10000;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "body");
        if (!lua_isnil(L, -1)) {
            size_t n = 0;
            const char* b = luaL_checklstring(L, -1, &n);
            body.assign(b, n);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "headers");
        if (!lua_isnil(L, -1)) {
            headers = to_json(L, -1);
            if (headers.is_array() && headers.empty())
                headers = json::object();
            if (!headers.is_object())
                luaL_error(L, "%s: headers must be a table of strings", call_name);
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                if (!it.value().is_string())
                    luaL_error(L, "%s: header '%s' must be a string", call_name, it.key().c_str());
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout_ms");
        if (!lua_isnil(L, -1))
            timeout = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    uint32_t timeout_ms = static_cast<uint32_t>(std::clamp<lua_Integer>(timeout, 1, 60000));
    std::string m = method;
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, m, url, body, headers, timeout_ms](LuaRuntime::Pending p) {
        backend->http(m, url, body, headers, timeout_ms, make_resolver(p, &push_rpc_value));
    });
}

int http_get(lua_State* L) {
    return http_request(L, "GET", "helix.http.get");
}

int http_post(lua_State* L) {
    return http_request(L, "POST", "helix.http.post");
}

json& storage_of(lua_State* L) {
    auto& st = io_state(L);
    if (!st.storage) {
        st.storage = json::object();
        std::ifstream in(context(L).storage_path, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            json j = json::parse(ss.str(), nullptr, false);
            if (j.is_object())
                st.storage = std::move(j);
            else
                spdlog::warn("[plugin {}] storage file is not a JSON object; starting empty",
                             LuaRuntime::from(L).plugin_id());
        }
    }
    return *st.storage;
}

int storage_get(lua_State* L) {
    require_permission(L, Permission::Storage, "helix.storage.get");
    std::string key = luaL_checkstring(L, 1);
    const json& s = storage_of(L);
    auto it = s.find(key);
    if (it == s.end())
        lua_pushnil(L);
    else
        push_json(L, *it);
    return 1;
}

int storage_set(lua_State* L) {
    require_permission(L, Permission::Storage, "helix.storage.set");
    std::string key = luaL_checkstring(L, 1);
    json value = to_json(L, 2);
    json next = storage_of(L);
    if (value.is_null())
        next.erase(key);
    else
        next[key] = std::move(value);
    std::string text = next.dump();
    if (text.size() > kMaxStorageBytes)
        return luaL_error(L, "helix.storage.set: storage would exceed 256 KB");

    const std::string& path = context(L).storage_path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out)
            return luaL_error(L, "helix.storage.set: cannot write %s", tmp.c_str());
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return luaL_error(L, "helix.storage.set: cannot replace %s", path.c_str());
    storage_of(L) = std::move(next);
    return 0;
}

const SettingDecl* find_decl(const Manifest& m, const std::string& key) {
    for (const auto& d : m.settings) {
        if (d.key == key)
            return &d;
    }
    return nullptr;
}

bool fits(const SettingDecl& d, const json& v) {
    switch (d.type) {
    case SettingType::Bool:
        return v.is_boolean();
    case SettingType::Int:
        return v.is_number_integer() && v.get<double>() >= d.min && v.get<double>() <= d.max;
    case SettingType::Float:
        return v.is_number() && v.get<double>() >= d.min && v.get<double>() <= d.max;
    case SettingType::Enum:
        return v.is_string() &&
               std::find(d.options.begin(), d.options.end(), v.get<std::string>()) != d.options.end();
    case SettingType::String:
        return v.is_string() && v.get_ref<const std::string&>().size() <= kMaxSettingString;
    case SettingType::Action:
    case SettingType::Info:
        return false;
    }
    return false;
}

int settings_get(lua_State* L) {
    auto& ctx = context(L);
    std::string key = luaL_checkstring(L, 1);
    const SettingDecl* d = find_decl(ctx.manifest, key);
    if (!d)
        return luaL_error(L, "helix.settings.get: '%s' is not declared in manifest.json", key.c_str());
    auto it = ctx.settings->find(key);
    push_json(L, it != ctx.settings->end() && fits(*d, *it) ? *it : d->default_value);
    return 1;
}

int settings_on_change(lua_State* L) {
    auto& ctx = context(L);
    std::string key = luaL_checkstring(L, 1);
    if (!find_decl(ctx.manifest, key))
        return luaL_error(L, "helix.settings.on_change: '%s' is not declared in manifest.json", key.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    io_state(L).on_change[key].push_back(ctx.rt.ref_value(L, 2));
    return 0;
}

} // namespace

std::string plugin_storage_path(const std::string& settings_path, const std::string& id) {
    return (std::filesystem::path(settings_path).parent_path() / "plugin-data" / (id + ".json")).string();
}

bool set_plugin_setting(PluginContext& ctx, const std::string& key, const json& value) {
    const SettingDecl* d = find_decl(ctx.manifest, key);
    if (!d || !fits(*d, value))
        return false;
    (*ctx.settings)[key] = value;
    ctx.save_settings();
    auto& handlers = io_state(ctx.rt.state()).on_change;
    if (auto it = handlers.find(key); it != handlers.end()) {
        for (int ref : it->second) {
            ctx.rt.invoke(ref, [value](lua_State* co) {
                push_json(co, value);
                return 1;
            });
        }
    }
    return true;
}

void install_io_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new IoState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kIoStateKey);
    ctx.rt.on_close([state] { delete state; });

    static const luaL_Reg http_fns[] = {{"get", &http_get}, {"post", &http_post}, {nullptr, nullptr}};
    static const luaL_Reg storage_fns[] = {{"get", &storage_get}, {"set", &storage_set}, {nullptr, nullptr}};
    static const luaL_Reg settings_fns[] = {
        {"get", &settings_get}, {"on_change", &settings_on_change}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    for (auto [name, fns] : {std::pair{"http", http_fns}, std::pair{"storage", storage_fns},
                             std::pair{"settings", settings_fns}}) {
        lua_newtable(L);
        luaL_setfuncs(L, fns, 0);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

Append `src/plugin/lua_bind_io.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Run to see it pass**

Run: `make t F='[io]'`
Expected: all 8 cases pass.

- [ ] **Step 5: Commit**

```bash
git add include/lua_bindings.h src/plugin/lua_bind_io.cpp tests/unit/test_lua_bindings_io.cpp \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git commit -m "feat(plugin): helix.http, helix.storage and helix.settings bindings"
git show --stat HEAD
```

---

### Task 13: PluginHost and application wiring

**Files:**
- Create: `include/plugin_host.h`, `src/plugin/plugin_host.cpp`
- Create: `tests/fixtures/plugins/hello/{manifest.json,main.lua,ui/hello_panel.xml}`, `tests/fixtures/plugins/bad-name/manifest.json`, `tests/fixtures/plugins/looper/{manifest.json,main.lua}`
- Modify: `src/application/application.cpp`, `include/application.h`, `docs/devel/ENVIRONMENT_VARIABLES.md`, `app_srcs_excluded.txt`, translation YAMLs and `ui_xml/translations/*.xml` (generated)
- Test: `tests/unit/test_plugin_host.cpp`

**Interfaces:**
- Consumes: everything above; `Config` (`get<json>(ptr, default)` :309, `set<json>` :348, `save()` :423, `get_path()` :440; `Config::get_instance()` :626); `helix::version::check_version_constraint(constraint, version)` and `helix::version::parse_version` (`include/version.h`); `HELIX_VERSION` (`include/helix_version.h`); `lv_xml_register_component_from_file("A:" + path)` (the POSIX drive letter is `A`, `lv_conf.h:850`) and `lv_xml_component_unregister(name)` (`lib/helix-xml/src/xml/lv_xml_component.h:53,86`); `lv_xml_register_event_cb(nullptr, name, cb)` (`lv_xml.h:159`, no unregister exists); `ToastManager::show_with_detail` (`include/ui_toast_manager.h:67`); `lv_tr` for user-facing text.
- Produces:

```cpp
namespace helix::plugin {
size_t plugin_memory_budget(uint64_t mem_total_bytes); // min(MemTotal / 16, 64 MB)
uint64_t read_mem_total();                             // bytes, 0 when unreadable
enum class PluginStatus { Disabled, Loaded, NeedsApproval, Invalid, Incompatible, OverBudget, Faulted };
const char* plugin_status_name(PluginStatus s);
struct PluginInfo { std::string dir_name; std::optional<Manifest> manifest; PluginStatus status; std::string reason; };
class PluginHost {
  public:
    struct Deps { PluginBackend backend; std::function<json()> read_block;
                  std::function<void(const json&)> write_block; std::string settings_path;
                  std::string helix_version; size_t memory_budget; };
    explicit PluginHost(Deps deps);
    void load_from(const std::string& dir);
    void unload_all();
    const std::vector<PluginInfo>& plugins() const;
    bool enable(const std::string& id);   // Phase 2's consent screen calls this
    void disable(const std::string& id);
    LuaRuntime* runtime(const std::string& id);
    void dispatch_event(std::string_view user_data);
};
void register_plugin_event_callback(); // once per process; forwards to the live host
}
```

`load_from` rules, in order, for each subdirectory containing `manifest.json`, sorted by name:
1. Parse; errors make it `Invalid` with the errors joined by `"; "`.
2. The directory name must equal the id, else `Invalid`. (This also rules out duplicate ids.)
3. A `helix_version` constraint that `deps.helix_version` fails makes it `Incompatible`. A `helix_version` of the app that `parse_version` cannot read (a dev build) satisfies every constraint.
4. No object at `/plugins/enabled/<id>` → `Disabled`.
5. `permission_growth(granted, requested)` non-empty → `NeedsApproval`, reason naming the new permissions.
6. `memory_mb` beyond what remains of the budget → `OverBudget`.
7. Load: register each `ui/*.xml` whose stem the id owns (any other stem makes the plugin `Invalid`, with nothing left registered); create the runtime; build the `PluginContext`; install all five binding groups; `run_file("main.lua")`. A `false` from `run_file` makes it `Faulted` with the runtime's fault reason, or "main.lua failed; see the log", and unloads it.
8. Otherwise `Loaded`.

Unload of one plugin: call the global `on_unload` if it is a function and the runtime has not faulted; destroy the runtime; unregister its components; destroy its context.

- [ ] **Step 1: Write the fixture plugins**

`tests/fixtures/plugins/hello/manifest.json`:

```json
{
  "id": "hello",
  "name": "Hello",
  "version": "1.0.0",
  "helix_version": ">=0.0.1",
  "permissions": ["gcode"],
  "settings": [
    {"key": "greeting", "type": "string", "label": "Greeting", "default": "hi"}
  ]
}
```

`tests/fixtures/plugins/hello/main.lua`:

```lua
local status = helix.subject.string("status", helix.settings.get("greeting"))

helix.ui.on("press", function(arg)
    status:set("pressed " .. tostring(arg))
end)

helix.ui.on("home", function()
    local ok, err = helix.gcode("G28")
    status:set(ok and "homed" or ("failed: " .. err))
end)

function on_unload()
    helix.moonraker.call("server.info", {})
end
```

`tests/fixtures/plugins/hello/ui/hello_panel.xml` (match the root element and attribute spellings of an existing `ui_xml/components/*.xml`, for example `ui_xml/components/compact_toggle_row.xml`):

```xml
<component>
  <view extends="lv_obj">
    <lv_label name="hello_label" bind_text="hello_status"/>
    <lv_button name="hello_button">
      <event_cb trigger="clicked" callback="plugin_event" user_data="hello_press:7"/>
    </lv_button>
  </view>
</component>
```

`tests/fixtures/plugins/bad-name/manifest.json`:

```json
{"id": "other-id", "name": "Mismatch", "version": "1.0.0"}
```

`tests/fixtures/plugins/looper/manifest.json`:

```json
{"id": "looper", "name": "Looper", "version": "1.0.0"}
```

`tests/fixtures/plugins/looper/main.lua`:

```lua
while true do end
```

- [ ] **Step 2: Write the failing tests**

`tests/unit/test_plugin_host.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_host.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "ui_update_queue.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
struct HostRig {
    FakeBackend fake;
    json block;
    int writes = 0;
    std::unique_ptr<PluginHost> host;

    explicit HostRig(json initial = json::object(), size_t budget = size_t(64) << 20)
        : block(std::move(initial)) {
        PluginHost::Deps d;
        d.backend = fake.backend();
        d.read_block = [this] { return block; };
        d.write_block = [this](const json& j) {
            block = j;
            ++writes;
        };
        d.settings_path = "/tmp/helix-plugin-host-test/settings.json";
        d.helix_version = "1.1.0";
        d.memory_budget = budget;
        register_plugin_event_callback();
        host = std::make_unique<PluginHost>(std::move(d));
    }

    const PluginInfo* info(const std::string& dir) {
        for (const auto& p : host->plugins()) {
            if (p.dir_name == dir)
                return &p;
        }
        return nullptr;
    }
};

json enabled(const std::string& id, std::vector<std::string> perms) {
    return json{{"enabled", {{id, {{"version", "1.0.0"}, {"permissions", perms}}}}}};
}

void drain() {
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE("memory budget", "[plugin][host]") {
    CHECK(plugin_memory_budget(uint64_t(128) << 20) == size_t(8) << 20);
    CHECK(plugin_memory_budget(uint64_t(4) << 30) == size_t(64) << 20);
    CHECK(plugin_memory_budget(0) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "plugins are disabled until enabled", "[plugin][host]") {
    HostRig rig;
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.info("require-test") == nullptr); // no manifest: not a plugin
    CHECK(lv_xml_get_subject(nullptr, "hello_status") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "an enabled plugin loads, binds and reacts", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);

    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello_status");
    REQUIRE(status);
    CHECK(std::string(lv_subject_get_string(status)) == "hi");

    auto* panel = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "hello_panel", nullptr));
    REQUIRE(panel);
    lv_obj_t* button = lv_obj_find_by_name(panel, "hello_button");
    REQUIRE(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    CHECK(std::string(lv_subject_get_string(status)) == "pressed 7");

    rig.host->dispatch_event("hello_home");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "G28");
    rig.fake.requests[0].reply(RpcResult{true, {}, {}});
    drain();
    CHECK(std::string(lv_subject_get_string(status)) == "homed");
    lv_obj_delete(panel);
}

TEST_CASE_METHOD(LVGLTestFixture, "unload runs on_unload and leaves nothing registered", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);
    rig.host->unload_all();
    REQUIRE_FALSE(rig.fake.requests.empty());
    CHECK(rig.fake.requests.back().a == "server.info");
    CHECK(lv_xml_get_subject(nullptr, "hello_status") == nullptr);
    CHECK(lv_xml_create(lv_screen_active(), "hello_panel", nullptr) == nullptr);
    rig.host->dispatch_event("hello_press");
    rig.fake.requests.back().reply(RpcResult{true, {}, {}});
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "events for other or unknown plugins are ignored", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello_status");
    rig.host->dispatch_event("other-plugin_press");
    rig.host->dispatch_event("hello_nosuchhandler");
    rig.host->dispatch_event("garbage");
    rig.host->dispatch_event("");
    CHECK(std::string(lv_subject_get_string(status)) == "hi");
}

TEST_CASE_METHOD(LVGLTestFixture, "fewer granted permissions than requested blocks loading",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {}));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::NeedsApproval);
    CHECK(rig.info("hello")->reason.find("gcode") != std::string::npos);
    CHECK(lv_xml_get_subject(nullptr, "hello_status") == nullptr);

    CHECK(rig.host->enable("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Loaded);
    CHECK(rig.block["enabled"]["hello"]["permissions"] == json::array({"gcode"}));
}

TEST_CASE_METHOD(LVGLTestFixture, "an old list-shaped enabled key loads nothing", "[plugin][host]") {
    HostRig rig(json{{"enabled", json::array({"hello"})}});
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.host->enable("hello"));
    CHECK(rig.block["enabled"].is_object());
}

TEST_CASE_METHOD(LVGLTestFixture, "a directory not matching its id is invalid", "[plugin][host]") {
    HostRig rig(enabled("other-id", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("bad-name"));
    CHECK(rig.info("bad-name")->status == PluginStatus::Invalid);
    CHECK_FALSE(rig.host->enable("other-id"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an incompatible helix_version is not loaded", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    PluginHost::Deps d;
    // hello asks for >=0.0.1; an app reporting 0.0.0 fails that.
    d.backend = rig.fake.backend();
    d.read_block = [&] { return rig.block; };
    d.write_block = [&](const json& j) { rig.block = j; };
    d.helix_version = "0.0.0";
    d.memory_budget = size_t(64) << 20;
    rig.host = std::make_unique<PluginHost>(std::move(d));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Incompatible);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin over the memory budget is not loaded", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}), size_t(1) << 20);
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::OverBudget);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin that spins in main.lua faults and unloads", "[plugin][host]") {
    HostRig rig(enabled("looper", {}));
    rig.host->load_from("tests/fixtures/plugins");
    drain();
    CHECK(rig.info("looper")->status == PluginStatus::Faulted);
    CHECK(rig.info("looper")->reason.find("time budget") != std::string::npos);
    CHECK(rig.host->runtime("looper") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "disable unloads and forgets consent", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    rig.host->disable("hello");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK_FALSE(rig.block["enabled"].contains("hello"));
    CHECK(lv_xml_get_subject(nullptr, "hello_status") == nullptr);
}

#endif // HELIX_HAS_PLUGINS
```

- [ ] **Step 3: Run to see it fail**

Run: `make t F='[host]'`
Expected: compile error, `plugin_host.h` not found.

- [ ] **Step 4: Implement**

`include/plugin_host.h`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lua_bindings.h"
#include "lua_runtime.h"
#include "plugin_backend.h"
#include "plugin_manifest.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace helix::plugin {

/// What all plugins together may allocate: min(MemTotal / 16, 64 MB).
size_t plugin_memory_budget(uint64_t mem_total_bytes);

/// MemTotal from /proc/meminfo in bytes; 0 when unreadable.
uint64_t read_mem_total();

enum class PluginStatus { Disabled, Loaded, NeedsApproval, Invalid, Incompatible, OverBudget, Faulted };

const char* plugin_status_name(PluginStatus s);

struct PluginInfo {
    std::string dir_name;
    std::optional<Manifest> manifest; ///< unset when the manifest failed to parse
    PluginStatus status = PluginStatus::Disabled;
    std::string reason; ///< why, for every status but Loaded and Disabled
};

/// Owns every plugin: discovery, enable state, memory budget, load, unload and faults.
/// Main thread only.
class PluginHost {
  public:
    struct Deps {
        PluginBackend backend;
        std::function<json()> read_block;             ///< the current /plugins object
        std::function<void(const json&)> write_block; ///< replaces /plugins and saves
        std::string settings_path;                    ///< for storage file paths
        std::string helix_version;
        size_t memory_budget = 0;
    };

    explicit PluginHost(Deps deps);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /// Unloads everything, rescans `dir`, and loads every enabled plugin.
    void load_from(const std::string& dir);
    void unload_all();

    const std::vector<PluginInfo>& plugins() const { return plugins_; }

    /// Grants the manifest's current permissions and loads the plugin.
    bool enable(const std::string& id);
    void disable(const std::string& id);
    LuaRuntime* runtime(const std::string& id);

    /// The body of the global `plugin_event` XML callback.
    void dispatch_event(std::string_view user_data);

  private:
    struct Loaded {
        json settings;
        std::unique_ptr<PluginContext> ctx;
        std::unique_ptr<LuaRuntime> rt; ///< destroyed before ctx
        std::vector<std::string> components;
        size_t memory_bytes = 0;
    };

    PluginInfo* find(const std::string& id);
    void consider(PluginInfo& info);
    bool load(PluginInfo& info);
    void unload(const std::string& id);
    void on_fault(const std::string& id, const std::string& reason);
    void save_settings(const std::string& id);
    json enabled_entry(const std::string& id) const;
    size_t memory_in_use() const;

    Deps deps_;
    std::string dir_;
    std::vector<PluginInfo> plugins_;
    std::map<std::string, Loaded> loaded_;
    AsyncLifetimeGuard guard_;
};

/// Registers the `plugin_event` XML callback once per process. It forwards to the live host.
void register_plugin_event_callback();

} // namespace helix::plugin
```

`src/plugin/plugin_host.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_host.h"

#include "ui_toast_manager.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace helix::plugin {

namespace {

PluginHost* g_live_host = nullptr;

void plugin_event_cb(lv_event_t* e) {
    auto* user_data = static_cast<const char*>(lv_event_get_user_data(e));
    if (g_live_host && user_data)
        g_live_host->dispatch_event(user_data);
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void register_plugin_event_callback() {
    static bool registered = false;
    if (registered)
        return;
    lv_xml_register_event_cb(nullptr, "plugin_event", &plugin_event_cb);
    registered = true;
}

size_t plugin_memory_budget(uint64_t mem_total_bytes) {
    return static_cast<size_t>(std::min<uint64_t>(mem_total_bytes / 16, uint64_t(64) << 20));
}

uint64_t read_mem_total() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    while (in >> key >> kb) {
        if (key == "MemTotal:")
            return kb * 1024;
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

const char* plugin_status_name(PluginStatus s) {
    switch (s) {
    case PluginStatus::Disabled: return "disabled";
    case PluginStatus::Loaded: return "loaded";
    case PluginStatus::NeedsApproval: return "needs approval";
    case PluginStatus::Invalid: return "invalid";
    case PluginStatus::Incompatible: return "incompatible";
    case PluginStatus::OverBudget: return "over memory budget";
    case PluginStatus::Faulted: return "faulted";
    }
    return "?";
}

PluginHost::PluginHost(Deps deps) : deps_(std::move(deps)) {
    g_live_host = this;
}

PluginHost::~PluginHost() {
    unload_all();
    guard_.invalidate();
    if (g_live_host == this)
        g_live_host = nullptr;
}

void PluginHost::load_from(const std::string& dir) {
    unload_all();
    plugins_.clear();
    dir_ = dir;

    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "manifest.json", ec))
            names.push_back(entry.path().filename().string());
    }
    if (ec)
        spdlog::warn("[PluginHost] cannot scan {}: {}", dir, ec.message());
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        PluginInfo info;
        info.dir_name = name;
        auto parsed = parse_manifest(read_file(std::filesystem::path(dir) / name / "manifest.json"));
        if (!parsed.manifest) {
            info.status = PluginStatus::Invalid;
            info.reason = join(parsed.errors, "; ");
        } else if (parsed.manifest->id != name) {
            info.status = PluginStatus::Invalid;
            info.reason = "directory name must match id '" + parsed.manifest->id + "'";
        } else {
            info.manifest = std::move(parsed.manifest);
        }
        plugins_.push_back(std::move(info));
    }

    for (auto& info : plugins_) {
        if (info.manifest && info.status == PluginStatus::Disabled)
            consider(info);
    }
    for (const auto& info : plugins_) {
        spdlog::info("[PluginHost] {}: {}{}", info.dir_name, plugin_status_name(info.status),
                     info.reason.empty() ? "" : " (" + info.reason + ")");
    }
}

json PluginHost::enabled_entry(const std::string& id) const {
    json block = deps_.read_block();
    if (!block.is_object())
        return json();
    auto en = block.find("enabled");
    if (en == block.end() || !en->is_object())
        return json();
    auto it = en->find(id);
    return it == en->end() ? json() : *it;
}

size_t PluginHost::memory_in_use() const {
    size_t total = 0;
    for (const auto& [id, l] : loaded_)
        total += l.memory_bytes;
    return total;
}

void PluginHost::consider(PluginInfo& info) {
    const Manifest& m = *info.manifest;
    info.reason.clear();

    bool app_version_known = helix::version::parse_version(deps_.helix_version).has_value();
    if (!m.helix_version.empty() && app_version_known &&
        !helix::version::check_version_constraint(m.helix_version, deps_.helix_version)) {
        info.status = PluginStatus::Incompatible;
        info.reason = "needs HelixScreen " + m.helix_version;
        return;
    }

    json entry = enabled_entry(m.id);
    if (!entry.is_object()) {
        info.status = PluginStatus::Disabled;
        return;
    }

    PermissionSet granted;
    if (auto p = entry.find("permissions"); p != entry.end() && p->is_array()) {
        for (const auto& n : *p) {
            if (!n.is_string())
                continue;
            if (auto perm = permission_from_string(n.get<std::string>()))
                granted.insert(*perm);
        }
    }
    auto grown = permission_growth(granted, m.permissions);
    if (!grown.empty()) {
        info.status = PluginStatus::NeedsApproval;
        info.reason = "asks for new permissions:";
        for (Permission p : grown)
            info.reason += std::string(" ") + permission_name(p);
        return;
    }

    size_t want = static_cast<size_t>(m.memory_mb) << 20;
    size_t in_use = memory_in_use();
    if (in_use + want > deps_.memory_budget) {
        size_t left = deps_.memory_budget > in_use ? deps_.memory_budget - in_use : 0;
        info.status = PluginStatus::OverBudget;
        info.reason = "needs " + std::to_string(m.memory_mb) + " MB, " + std::to_string(left >> 20) +
                      " MB left of the plugin budget";
        return;
    }

    info.status = load(info) ? PluginStatus::Loaded : info.status;
}

bool PluginHost::load(PluginInfo& info) {
    const Manifest& m = *info.manifest;
    const std::string id = m.id;
    auto root = std::filesystem::path(dir_) / info.dir_name;

    std::vector<std::filesystem::path> xmls;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root / "ui", ec)) {
        if (e.path().extension() == ".xml")
            xmls.push_back(e.path());
    }
    std::sort(xmls.begin(), xmls.end());
    for (const auto& p : xmls) {
        if (!is_owned_name(id, p.stem().string())) {
            info.status = PluginStatus::Invalid;
            info.reason = "component '" + p.stem().string() + "' must be named " + id + "_<name>";
            return false;
        }
    }

    auto [it, inserted] = loaded_.try_emplace(id);
    Loaded& l = it->second;
    for (const auto& p : xmls) {
        std::string uri = "A:" + p.string();
        if (lv_xml_register_component_from_file(uri.c_str()) != LV_RESULT_OK) {
            info.status = PluginStatus::Invalid;
            info.reason = "cannot load " + p.filename().string();
            unload(id);
            return false;
        }
        l.components.push_back(p.stem().string());
    }

    json block = deps_.read_block();
    if (block.is_object()) {
        if (auto s = block.find("settings"); s != block.end() && s->is_object()) {
            if (auto e = s->find(id); e != s->end() && e->is_object())
                l.settings = *e;
        }
    }
    if (!l.settings.is_object())
        l.settings = json::object();

    l.memory_bytes = static_cast<size_t>(m.memory_mb) << 20;
    LuaRuntime::Limits limits;
    limits.memory_bytes = l.memory_bytes;
    LifetimeToken token = guard_.token();
    l.rt = std::make_unique<LuaRuntime>(id, root.string(), limits, [this, token, id](const std::string& reason) {
        token.defer("plugin_fault", [this, id, reason] { on_fault(id, reason); });
    });
    l.ctx = std::make_unique<PluginContext>(PluginContext{*l.rt, deps_.backend, m, &l.settings,
                                                          [this, id] { save_settings(id); },
                                                          plugin_storage_path(deps_.settings_path, id)});
    for (Installer install : {&install_core_bindings, &install_ui_bindings, &install_printer_bindings,
                              &install_moonraker_bindings, &install_io_bindings})
        install(*l.ctx);

    if (!l.rt->run_file("main.lua")) {
        info.status = PluginStatus::Faulted;
        info.reason = l.rt->faulted() ? l.rt->fault_reason() : "main.lua failed; see the log";
        unload(id);
        return false;
    }
    return true;
}

void PluginHost::unload(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    Loaded& l = it->second;
    if (l.rt && !l.rt->faulted()) {
        lua_State* L = l.rt->state();
        lua_getglobal(L, "on_unload");
        if (lua_isfunction(L, -1)) {
            int ref = l.rt->ref_value(L, -1);
            lua_pop(L, 1);
            l.rt->invoke(ref);
            l.rt->unref(ref);
        } else {
            lua_pop(L, 1);
        }
    }
    l.rt.reset();
    for (const auto& c : l.components)
        lv_xml_component_unregister(c.c_str());
    l.ctx.reset();
    loaded_.erase(it);
}

void PluginHost::unload_all() {
    std::vector<std::string> ids;
    for (const auto& [id, l] : loaded_)
        ids.push_back(id);
    for (const auto& id : ids)
        unload(id);
}

void PluginHost::on_fault(const std::string& id, const std::string& reason) {
    if (!loaded_.count(id))
        return; // already unloaded by the load path
    unload(id);
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Faulted;
        info->reason = reason;
        std::string detail = info->manifest->name + ": " + reason;
        ToastManager::instance().show_with_detail(ToastSeverity::WARNING, lv_tr("Plugin disabled"),
                                                  detail.c_str());
    }
}

void PluginHost::save_settings(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["settings"].is_object())
        block["settings"] = json::object();
    block["settings"][id] = it->second.settings;
    deps_.write_block(block);
}

PluginInfo* PluginHost::find(const std::string& id) {
    for (auto& p : plugins_) {
        if (p.manifest && p.manifest->id == id && p.dir_name == id)
            return &p;
    }
    return nullptr;
}

LuaRuntime* PluginHost::runtime(const std::string& id) {
    auto it = loaded_.find(id);
    return it == loaded_.end() ? nullptr : it->second.rt.get();
}

bool PluginHost::enable(const std::string& id) {
    PluginInfo* info = find(id);
    if (!info || info->status == PluginStatus::Invalid)
        return false;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["enabled"].is_object())
        block["enabled"] = json::object();
    json perms = json::array();
    for (Permission p : info->manifest->permissions)
        perms.push_back(permission_name(p));
    block["enabled"][id] = {{"version", info->manifest->version}, {"permissions", perms}};
    deps_.write_block(block);
    unload(id);
    info->status = PluginStatus::Disabled;
    consider(*info);
    return info->status == PluginStatus::Loaded;
}

void PluginHost::disable(const std::string& id) {
    unload(id);
    json block = deps_.read_block();
    if (block.is_object() && block.contains("enabled") && block["enabled"].is_object()) {
        block["enabled"].erase(id);
        deps_.write_block(block);
    }
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Disabled;
        info->reason.clear();
    }
}

void PluginHost::dispatch_event(std::string_view user_data) {
    PluginEventTarget target = parse_plugin_event(user_data);
    if (target.id.empty()) {
        spdlog::debug("[PluginHost] ignoring malformed plugin_event '{}'", user_data);
        return;
    }
    LuaRuntime* rt = runtime(target.id);
    if (!rt) {
        spdlog::debug("[PluginHost] plugin_event for '{}', which is not loaded", target.id);
        return;
    }
    if (!dispatch_ui_handler(*rt, target.name, target.arg))
        spdlog::debug("[PluginHost] plugin '{}' has no handler '{}'", target.id, target.name);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

`lv_tr` comes from the translation header the rest of `src/ui/` uses; copy its include from `src/ui/ui_change_host_modal.cpp`. `ToastManager`, `ToastSeverity` and `ModalSeverity` are in the global namespace.

The "incompatible" test builds a second `PluginHost` while the first is alive; `g_live_host` points at whichever was constructed last, and the first's destructor must only clear it when it still points at itself. The destructor above already does that.

Append `src/plugin/plugin_host.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 5: Run to see it pass**

Run: `make t F='[host]'`
Expected: all 12 cases pass.

- [ ] **Step 6: Translations**

The toast adds one user-facing string. Run `make translation-sync && make translations`, then stage the YAMLs and `ui_xml/translations/*.xml` it changed (project lesson L064).

- [ ] **Step 7: Wire into Application**

In `include/application.h`, add under `#if HELIX_HAS_PLUGINS`:

```cpp
    std::unique_ptr<helix::plugin::PluginHost> m_plugin_host;
    void init_plugins();
```

with a forward declaration `namespace helix::plugin { class PluginHost; }` near the other forward declarations.

In `src/application/application.cpp`, include `plugin_host.h` and `helix_version.h` under `#if HELIX_HAS_PLUGINS`, call `init_plugins()` at the phase where Task 1 removed the old call, and add:

```cpp
#if HELIX_HAS_PLUGINS
void Application::init_plugins() {
    const char* dir = std::getenv("HELIX_PLUGIN_DIR");
    if (!dir || !*dir)
        return; // the Moonraker plugin folder arrives in Phase 3
    helix::plugin::PluginHost::Deps deps;
    deps.backend = helix::plugin::make_app_backend();
    deps.read_block = [this] { return m_config->get<json>("/plugins", json::object()); };
    deps.write_block = [this](const json& j) {
        m_config->set<json>("/plugins", j);
        m_config->save();
    };
    deps.settings_path = m_config->get_path();
    deps.helix_version = HELIX_VERSION;
    deps.memory_budget = helix::plugin::plugin_memory_budget(helix::plugin::read_mem_total());
    helix::plugin::register_plugin_event_callback();
    m_plugin_host = std::make_unique<helix::plugin::PluginHost>(std::move(deps));
    m_plugin_host->load_from(dir);
}
#endif
```

In both the shutdown path and the restart path, at the places Task 1 removed the old unload blocks (before the Moonraker client and managers are destroyed), add:

```cpp
#if HELIX_HAS_PLUGINS
    // Plugins hold callbacks into the managers destroyed below.
    if (m_plugin_host) {
        m_plugin_host->unload_all();
        m_plugin_host.reset();
    }
#endif
```

Add a row to `docs/devel/ENVIRONMENT_VARIABLES.md` in that file's table format: `HELIX_PLUGIN_DIR`: "Directory of Lua plugins, one subdirectory per plugin. Unset: no plugins load. A development override until the Moonraker plugin folder lands."

- [ ] **Step 8: Verify in the running app**

Launch with the Bash tool's `run_in_background` (not a shell `&`):

```bash
make
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_PLUGIN_DIR=$PWD/tests/fixtures/plugins SDL_VIDEODRIVER=dummy \
  ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1
```

Run: `grep "\[PluginHost\]" /tmp/helix-$TREE.log`
Expected: `hello: disabled`, `bad-name: invalid (...)`, `looper: disabled`.

Stop that instance by its own PID (CLAUDE.md: resolve it from `$HELIX_SOCK`, never `pkill`). Edit `$HELIX_CONFIG_DIR/settings.json` (the file the app wrote, which carries `config_version`) to add `"plugins": {"enabled": {"hello": {"version": "1.0.0", "permissions": ["gcode"]}}}`, relaunch the same way, and check the log shows `hello: loaded`. Then run `./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls` to confirm the app is responsive, and stop it by PID.

- [ ] **Step 9: Commit**

```bash
git add include/plugin_host.h src/plugin/plugin_host.cpp tests/unit/test_plugin_host.cpp \
  tests/fixtures/plugins/hello tests/fixtures/plugins/bad-name tests/fixtures/plugins/looper \
  src/application/application.cpp include/application.h docs/devel/ENVIRONMENT_VARIABLES.md \
  firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git add <translation YAMLs and ui_xml/translations/*.xml that Step 6 changed>
git commit -m "feat(plugin): PluginHost loads Lua plugins from HELIX_PLUGIN_DIR"
git show --stat HEAD
```

---

### Task 14: Gates, docs and hand-off

**Files:**
- Modify: `docs/devel/architecture/12-system-services.md`, `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`

- [ ] **Step 1: Architecture doc**

Replace Task 1's one-line placeholder in `docs/devel/architecture/12-system-services.md` with a short present-tense section: `PluginHost` owns plugins (`src/plugin/plugin_host.cpp#PluginHost::load_from`); one `LuaRuntime` per plugin on the main thread; bindings reach the app only through `PluginBackend`; XML events arrive through the one `plugin_event` callback; faults unload through the update queue; the memory budget. Cite places as `path#symbol` (CLAUDE.md § Doc citations), never line numbers. Point to the design spec for the rest; the author guide is Phase 4.

- [ ] **Step 2: Mark progress in the spec**

Set the spec's `**Status:**` line to: `Phase 1 (runtime) implemented on feature/lua-plugins; Phases 2 to 5 not started.`

- [ ] **Step 3: The completion gate**

Run: `make full-test-run`
Expected: the unit sweep and the bats suite both pass. Do not pipe it through `tail` or `grep`.

Run: `scripts/zeus-run.sh asan '[plugin]'`
Expected: no reports.

Run: `scripts/zeus-run.sh mutate --tests '[plugin]'`
Expected: every survivor is equivalent (say why in the commit body) or gets a test.

Run: `python3 scripts/check_esp32_app_srcs.py && make -n HELIX_HAS_PLUGINS=0 | grep -c "lib/lua/"`
Expected: the check exits 0 and the count is `0`.

- [ ] **Step 4: Commit and hand off**

```bash
git add docs/devel/architecture/12-system-services.md docs/devel/plans/2026-09-28-lua-plugin-system-design.md
git commit -m "docs(plugin): architecture notes for the Lua plugin runtime"
git show --stat HEAD
```

Then use superpowers:finishing-a-development-branch. Phase 2's plan (widgets, overlays, Settings, consent, schema settings, `setting_text_row`) is written against this branch once it has merged.
