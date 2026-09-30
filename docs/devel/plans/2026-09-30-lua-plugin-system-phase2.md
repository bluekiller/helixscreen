# Lua Plugin System, Phase 2 (Contributions) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a loaded Lua plugin put widgets on the home panel, open its own overlays, and expose a settings screen, and let the user list, consent to, enable and disable plugins from Settings.

**Architecture:** The widget registry gains runtime definitions owned by the registry, and the home layout keeps widget ids it does not know, so a plugin's tiles come back when it does. A generic `LuaPanelWidget` forwards grid lifecycle calls into the plugin's `helix.widget` hooks. `PluginOverlayHost` pushes plugin components as overlays and pops them on unload. Settings gets a Plugins entry, a consent dialog, and a settings screen generated from the manifest schema out of the existing `setting_*_row` components. Plugin subjects outlive their plugin until nothing observes them, which makes unload safe while plugin objects are still being deleted.

**Tech Stack:** C++17, Lua 5.4.9 (compiled as C++), LVGL 9.5 + helix-xml, Catch2, the project Makefile.

**Spec:** `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`. Read it first. This plan implements its Phase 2 and the four items Phase 1 carried forward. Phase 1 is merged; its code is the base this plan builds on.

## Global Constraints

- Plugin id `^[a-z][a-z0-9-]{1,31}$`. Every name a plugin registers, including widget ids and component names, is `<id>_<rest>`.
- Plugin widgets are in the catalog category Plugins, never enabled by default, and single-instance.
- Manifest spans are in **cells**, 1 to 8 on each axis. The registry stores tracks: `tracks = cells * GridLayout::TRACKS_PER_CELL` (2). Plugin widgets get no half-cell resolution.
- `on_size(cols, rows, w, h)` gives Lua cells (tracks / 2) and pixels.
- At most 8 widgets per manifest.
- Plugin XML may use only the `plugin_event` callback and only subjects the plugin owns (Task 2 has the full rule).
- Consent text names what each permission reaches in plain words. `gcode` reads as full control of the printer. `moonraker_write` names config file rewrites.
- Every `src/plugin/*.cpp` and every new test file is wrapped in `#if HELIX_HAS_PLUGINS` / `#endif // HELIX_HAS_PLUGINS`. Every new `src/plugin/*.cpp` gets a line in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` in the commit that creates it (`python3 scripts/check_esp32_app_srcs.py` exits 0).
- Shared code (the widget registry, `PanelWidgetConfig`, the catalog, the settings panel) is compiled into the ESP32 image with `-fno-exceptions`: no `json::at`, `j.value()`, one-argument `json::parse`, or `std::stoi` there (`json_util::safe_*`).
- New files begin `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- spdlog only; `#include "hv/json.hpp"`; no RTTI; no `std::regex`; comments describe the code as it is now, with no history (CLAUDE.md § Comments); no em-dashes.
- New user-facing strings go through `lv_tr()` / `label_tag`, then `make translation-sync` and `make translations`, and the generated `ui_xml/translations/*.xml` are committed with the change (lesson L064).
- Verification inside a task is `make t F='[tag]'` for the tags the task touches, plus `make` when app code changed. `make full-test-run`, `scripts/zeus-run.sh mutate` and `scripts/zeus-run.sh asan` run once, in Task 11, with ASAN last. Each task proves its tests can fail with a named hand mutation in the commit body.
- Commits use explicit paths; never `git add -A`. `git show --stat HEAD` after every commit.

## Review Focus

- **A plugin that is disabled or faults while its widget is on the home panel and its overlay is open** leaves no dangling pointer: the tile disappears on the next rebuild, the overlay pops, and nothing touches the plugin's freed memory when those objects are deleted later. Pinned in Tasks 3, 6 and 7.
- **Plugin XML that names an app callback or app subject** (`callback="on_estop_clicked"`, `bind_value="extruder_target"`) is rejected at load, so a plugin with no permissions cannot drive the printer through app handlers. Pinned in Task 2.
- **A saved home layout that places a plugin widget while the plugin is absent** keeps the entry through any number of saves and restores it when the plugin returns. Pinned in Task 5.
- **The catalog open while a plugin unloads** rebuilds its rows instead of holding pointers to a definition that is gone. Pinned in Task 4.
- **A settings value written from the generated screen** is validated by the same `set_plugin_setting` rule the Lua side uses, and reaches `helix.settings.on_change` handlers. Pinned in Task 9.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/plugin_manifest.h`, `src/plugin/plugin_manifest.cpp` | Gains `WidgetDecl` and the `widgets` array |
| `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp` | `is_plugin_relative_path`; `run_file` refuses anything else |
| `include/plugin_xml_policy.h`, `src/plugin/plugin_xml_policy.cpp` | What plugin XML may reference. Pure |
| `src/plugin/lua_bind_ui.cpp`, `include/lua_bindings.h` | Retired subjects; `helix.ui.overlay` |
| `include/panel_widget_registry.h`, `src/ui/panel_widget_registry.cpp` | Runtime widget definitions, the Plugins category |
| `include/panel_widget_manager.h`, `src/ui/panel_widget_manager.cpp` | `notify_widget_defs_changed` |
| `include/panel_widget_config.h`, `src/system/panel_widget_config.cpp` | Keeps unknown widget ids across load and save |
| `src/plugin/lua_bind_widget.cpp` | `helix.widget` |
| `include/lua_panel_widget.h`, `src/plugin/lua_panel_widget.cpp` | The home widget that forwards to Lua hooks |
| `include/plugin_overlay_host.h`, `src/plugin/plugin_overlay_host.cpp` | Plugin overlays on the navigation stack |
| `include/plugin_host.h`, `src/plugin/plugin_host.cpp` | Registers widget defs and overlays per plugin; unload order; `PluginHost::live()` |
| `ui_xml/setting_text_row.xml` | Label plus a text value that opens the keyboard |
| `include/plugin_settings_overlay.h`, `src/plugin/plugin_settings_overlay.cpp`, `ui_xml/plugin_settings_overlay.xml` | The generated settings screen |
| `include/plugin_consent.h`, `src/plugin/plugin_consent.cpp` | Consent text (pure) and the consent dialog |
| `include/plugins_overlay.h`, `src/plugin/plugins_overlay.cpp`, `ui_xml/plugins_overlay.xml` | Settings → Plugins |
| `ui_xml/settings_panel.xml`, `src/ui/ui_panel_settings.cpp` | The Plugins row |
| `tests/fixtures/plugins/widget-demo/` | A plugin with a widget, an overlay and every setting type |

---

### Task 1: Manifest widgets and safe `run_file` paths

**Files:**
- Modify: `include/plugin_manifest.h`, `src/plugin/plugin_manifest.cpp`, `include/lua_runtime.h`, `src/plugin/lua_runtime.cpp`
- Test: `tests/unit/test_plugin_manifest.cpp`, `tests/unit/test_lua_runtime.cpp`

**Interfaces:**
- Consumes: `parse_manifest`, `is_owned_name` (Phase 1).
- Produces:

```cpp
namespace helix::plugin {
constexpr int kMaxWidgetCells = 8;
constexpr size_t kMaxWidgetsPerPlugin = 8;

/// One entry of the manifest's `widgets` array. Spans are in grid cells.
struct WidgetDecl {
    std::string id;          ///< <plugin>_<name>
    std::string name;
    std::string icon;        ///< icon name; empty when absent
    std::string description;
    std::string component;   ///< <plugin>_<name>, a file in ui/
    int colspan = 1;
    int rowspan = 1;
    int max_colspan = 0;     ///< 0: not resizable on this axis
    int max_rowspan = 0;
};
// Manifest gains: std::vector<WidgetDecl> widgets;

/// A path inside a plugin: '/'-separated segments of [A-Za-z0-9_.-], none empty, "." or
/// "..", no leading '/', ending in ".lua", at most 128 bytes.
bool is_plugin_relative_path(std::string_view path);
}
```

- [ ] **Step 1: Write the failing manifest tests**

Append to `tests/unit/test_plugin_manifest.cpp`:

```cpp
namespace {
ManifestParse with_widgets(const std::string& widgets) {
    return parse_manifest(R"({"id":"ab","name":"n","version":"1","widgets":[)" + widgets + "]}");
}
} // namespace

TEST_CASE("manifest widgets parse with cell spans", "[plugin][manifest]") {
    auto r = with_widgets(R"({"id":"ab_launch","name":"Launch","icon":"tune",
        "description":"Start","component":"ab_widget","colspan":1,"rowspan":1,
        "max_colspan":2,"max_rowspan":1})");
    REQUIRE(r.manifest);
    REQUIRE(r.manifest->widgets.size() == 1);
    const auto& w = r.manifest->widgets[0];
    CHECK(w.id == "ab_launch");
    CHECK(w.component == "ab_widget");
    CHECK(w.icon == "tune");
    CHECK(w.colspan == 1);
    CHECK(w.max_colspan == 2);
    CHECK(w.max_rowspan == 1);
}

TEST_CASE("manifest widgets reject bad names and spans", "[plugin][manifest]") {
    CHECK(has_error_containing(with_widgets(R"({"id":"launch","name":"L","component":"ab_w"})"),
                               "widgets[0]"));
    CHECK(has_error_containing(with_widgets(R"({"id":"ab_l","name":"L","component":"w"})"),
                               "'component'"));
    CHECK(has_error_containing(
        with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w","colspan":9})"), "'colspan'"));
    CHECK(has_error_containing(
        with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w","colspan":2,"max_colspan":1})"),
        "'max_colspan'"));
    CHECK(has_error_containing(with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w"},
                                               {"id":"ab_l","name":"M","component":"ab_w"})"),
                               "duplicate widget id"));
    std::string nine;
    for (int i = 0; i < 9; ++i)
        nine += std::string(i ? "," : "") + R"({"id":"ab_w)" + std::to_string(i) +
                R"(","name":"W","component":"ab_c"})";
    CHECK(has_error_containing(with_widgets(nine), "at most 8"));
}
```

- [ ] **Step 2: Write the failing path tests**

Append to `tests/unit/test_lua_runtime.cpp`:

```cpp
TEST_CASE("plugin-relative paths stay inside the plugin", "[plugin][lua_runtime]") {
    CHECK(is_plugin_relative_path("main.lua"));
    CHECK(is_plugin_relative_path("lib/util.lua"));
    CHECK_FALSE(is_plugin_relative_path(""));
    CHECK_FALSE(is_plugin_relative_path("/etc/x.lua"));
    CHECK_FALSE(is_plugin_relative_path("../x.lua"));
    CHECK_FALSE(is_plugin_relative_path("lib/../../x.lua"));
    CHECK_FALSE(is_plugin_relative_path("lib//x.lua"));
    CHECK_FALSE(is_plugin_relative_path("./x.lua"));
    CHECK_FALSE(is_plugin_relative_path("main.txt"));
    CHECK_FALSE(is_plugin_relative_path("a b.lua"));
    CHECK_FALSE(is_plugin_relative_path(std::string(125, 'a') + ".lua"));
}

TEST_CASE("run_file refuses a path outside the plugin", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.rt->run_file("../hello/main.lua"));
}
```

Run: `make t F='[manifest]'` and `make t F='[lua_runtime]'`
Expected: compile errors (`widgets`, `is_plugin_relative_path` undeclared).

- [ ] **Step 3: Implement the manifest widgets**

In `include/plugin_manifest.h`, add `kMaxWidgetCells`, `kMaxWidgetsPerPlugin`, `WidgetDecl` (above) and `std::vector<WidgetDecl> widgets;` in `Manifest` after `settings_overlay`.

In `src/plugin/plugin_manifest.cpp`, add beside `parse_setting` and call it from `parse_manifest` after the `settings_overlay` block:

```cpp
void span_field(const json& w, const char* key, int& out, int lo, std::vector<std::string>& local) {
    auto it = w.find(key);
    if (it == w.end())
        return;
    if (!it->is_number_integer() || it->get<int>() < lo || it->get<int>() > kMaxWidgetCells) {
        local.push_back(std::string("'") + key + "' must be an integer from " +
                        std::to_string(lo) + " to " + std::to_string(kMaxWidgetCells));
        return;
    }
    out = it->get<int>();
}

void parse_widgets(const json& arr, Manifest& m, std::vector<std::string>& errors) {
    if (!arr.is_array()) {
        errors.push_back("'widgets' must be an array");
        return;
    }
    if (arr.size() > kMaxWidgetsPerPlugin) {
        errors.push_back("'widgets' may list at most " + std::to_string(kMaxWidgetsPerPlugin));
        return;
    }
    std::set<std::string> seen;
    for (size_t i = 0; i < arr.size(); ++i) {
        const std::string where = "widgets[" + std::to_string(i) + "]";
        const json& w = arr[i];
        if (!w.is_object()) {
            errors.push_back(where + " must be an object");
            continue;
        }
        WidgetDecl d;
        std::vector<std::string> local;
        require_string(w, "id", d.id, local);
        require_string(w, "name", d.name, local);
        require_string(w, "component", d.component, local);
        optional_string(w, "icon", d.icon, local);
        optional_string(w, "description", d.description, local);
        if (!d.id.empty() && !is_owned_name(m.id, d.id))
            local.push_back("'id' must be named " + m.id + "_<name>");
        if (!d.component.empty() && !is_owned_name(m.id, d.component))
            local.push_back("'component' must be named " + m.id + "_<name>");
        span_field(w, "colspan", d.colspan, 1, local);
        span_field(w, "rowspan", d.rowspan, 1, local);
        span_field(w, "max_colspan", d.max_colspan, 0, local);
        span_field(w, "max_rowspan", d.max_rowspan, 0, local);
        if (d.max_colspan != 0 && d.max_colspan < d.colspan)
            local.push_back("'max_colspan' must be 0 or at least 'colspan'");
        if (d.max_rowspan != 0 && d.max_rowspan < d.rowspan)
            local.push_back("'max_rowspan' must be 0 or at least 'rowspan'");
        if (!d.id.empty() && !seen.insert(d.id).second)
            local.push_back("duplicate widget id '" + d.id + "'");
        for (const auto& e : local)
            errors.push_back(where + ": " + e);
        if (local.empty())
            m.widgets.push_back(std::move(d));
    }
}
```

```cpp
    if (auto it = j.find("widgets"); it != j.end())
        parse_widgets(*it, m, r.errors);
```

- [ ] **Step 4: Implement the path check**

Declare `is_plugin_relative_path` in `include/lua_runtime.h` beside `require_candidates`. In `src/plugin/lua_runtime.cpp`:

```cpp
bool is_plugin_relative_path(std::string_view path) {
    constexpr std::string_view kExt = ".lua";
    if (path.empty() || path.size() > 128 || path.front() == '/' || path.size() <= kExt.size() ||
        path.substr(path.size() - kExt.size()) != kExt)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        std::string_view seg = path.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        for (char c : seg) {
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '.' || c == '-';
            if (!ok)
                return false;
        }
        start = end + 1;
    }
    return true;
}
```

At the top of `LuaRuntime::run_file`:

```cpp
    if (!is_plugin_relative_path(relative_path)) {
        report_error("refusing to run '" + relative_path + "': not a path inside the plugin");
        return false;
    }
```

- [ ] **Step 5: Run the tests**

Run: `make t F='[manifest]'` then `make t F='[lua_runtime]'`
Expected: PASS.

Hand mutations: drop the `seg == ".."` test (the `lib/../../x.lua` case goes red); drop the `is_owned_name(m.id, d.component)` check (the `'component'` case goes red).

- [ ] **Step 6: Commit**

```bash
git add include/plugin_manifest.h src/plugin/plugin_manifest.cpp include/lua_runtime.h src/plugin/lua_runtime.cpp tests/unit/test_plugin_manifest.cpp tests/unit/test_lua_runtime.cpp
git commit -m "feat(plugin): manifest widget declarations; run_file accepts only paths inside the plugin"
git show --stat HEAD
```

---

### Task 2: What plugin XML may reference

A plugin with no permissions must not reach the printer through XML: an app callback (`callback="on_estop_clicked"`) or a writable app subject (`bind_value="extruder_target"` on a slider) would bypass every permission. The loader rejects such XML before registering anything.

**Files:**
- Create: `include/plugin_xml_policy.h`, `src/plugin/plugin_xml_policy.cpp`
- Modify: `src/plugin/plugin_host.cpp`, `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_xml_policy.cpp`, `tests/unit/test_plugin_host.cpp`, new fixture `tests/fixtures/plugins/app-callback/`

**Interfaces:**
- Produces:

```cpp
namespace helix::plugin {
/// Empty when `xml`, a component file of plugin `id`, references only what a plugin may;
/// otherwise the first violation, as a sentence. The rules:
///  - an attribute named `callback` or ending in `_callback` has the value `plugin_event`;
///  - `user_data` on an `event_cb` is owned by `id` (optionally followed by `:<arg>`);
///  - an attribute named `subject` or starting with `bind_` names a subject owned by `id`;
///  - none of those attributes takes a `$prop` value, since a prop default cannot be
///    checked against the attribute it feeds;
///  - no `<subjects>`, `<images>` or `<fonts>` section, and no screen load or create event.
std::string check_plugin_xml(std::string_view id, const std::string& xml);
}
```

- [ ] **Step 1: Confirm the element names**

Run: `grep -rn "screen_load_event\|screen_create_event" lib/helix-xml/src/xml/*.c | head`
Use exactly the element names registered there in the banned set below; if helix-xml registers other screen-changing events, add them and say so in the report.

- [ ] **Step 2: Write the failing tests**

`tests/unit/test_plugin_xml_policy.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_xml_policy.h"

#include "../catch_amalgamated.hpp"

using helix::plugin::check_plugin_xml;

namespace {
std::string view_with(const std::string& body) {
    return "<component><view extends=\"lv_obj\">" + body + "</view></component>";
}
} // namespace

TEST_CASE("plugin XML may use plugin_event and its own subjects", "[plugin][xml_policy]") {
    CHECK(check_plugin_xml("ab", view_with(R"(<lv_label bind_text="ab_status"/>
        <lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="ab_go:3"/></lv_button>
        <lv_obj><bind_flag_if_eq subject="ab_busy" flag="hidden" ref_value="0"/></lv_obj>)"))
              .empty());
}

TEST_CASE("plugin XML may not name app callbacks or subjects", "[plugin][xml_policy]") {
    CHECK_FALSE(check_plugin_xml("ab", view_with(
        R"(<lv_button><event_cb trigger="clicked" callback="on_estop_clicked"/></lv_button>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(
        R"(<text_input clear_callback="on_wifi_clear"/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_slider bind_value="extruder_target"/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(
        R"(<lv_obj><bind_flag_if_eq subject="printer_connected" flag="hidden" ref_value="0"/></lv_obj>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(
        R"(<lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="other_go"/></lv_button>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_label bind_text="$status"/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", "<component><subjects><int name=\"x\" value=\"0\"/></subjects>"
                                       "<view extends=\"lv_obj\"/></component>").empty());
    CHECK_FALSE(check_plugin_xml("ab", "not xml <").empty());
}

#endif // HELIX_HAS_PLUGINS
```

Fixture `tests/fixtures/plugins/app-callback/`: `manifest.json` `{"id":"app-callback","name":"AC","version":"1.0.0"}`, an empty `main.lua`, and `ui/app-callback_panel.xml`:

```xml
<?xml version="1.0"?>
<component>
  <view extends="lv_obj">
    <lv_button><event_cb trigger="clicked" callback="on_estop_clicked"/></lv_button>
  </view>
</component>
```

Append to `tests/unit/test_plugin_host.cpp`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "plugin XML naming an app callback is rejected at load",
                 "[plugin][host]") {
    HostRig rig(enabled("app-callback", {}));
    rig.host->load_from("tests/fixtures/plugins");
    const PluginInfo* info = rig.info("app-callback");
    REQUIRE(info);
    CHECK(info->status == PluginStatus::Invalid);
    CHECK(info->reason.find("on_estop_clicked") != std::string::npos);
    CHECK(lv_xml_component_get_scope("app-callback_panel") == nullptr);
}
```

Update any host test that counts fixture directories.

Run: `make t F='[xml_policy]'`
Expected: compile error (header missing).

- [ ] **Step 3: Implement**

`src/plugin/plugin_xml_policy.cpp` walks the document with expat, the parser helix-xml already links (`#include "helix-xml/src/libs/expat/expat.h"`, as `src/application/xml_hot_reloader.cpp` does):

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_xml_policy.h"

#include "plugin_manifest.h"

#include "helix-xml/src/libs/expat/expat.h"

#include <cstring>
#include <string>

namespace helix::plugin {

namespace {

struct Walk {
    std::string id;
    std::string error;
};

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

void fail(Walk& w, std::string msg) {
    if (w.error.empty())
        w.error = std::move(msg);
}

void on_start(void* ud, const XML_Char* name, const XML_Char** attrs) {
    auto& w = *static_cast<Walk*>(ud);
    std::string_view el = name;
    if (el == "subjects" || el == "images" || el == "fonts" || el == "screen_load_event" ||
        el == "screen_create_event") {
        fail(w, "<" + std::string(el) + "> is not available to plugins");
        return;
    }
    for (int i = 0; attrs[i]; i += 2) {
        std::string_view key = attrs[i];
        std::string_view val = attrs[i + 1];
        bool is_callback = key == "callback" || ends_with(key, "_callback");
        bool is_subject = key == "subject" || key.rfind("bind_", 0) == 0;
        bool is_target = el == "event_cb" && key == "user_data";
        if (!is_callback && !is_subject && !is_target)
            continue;
        if (!val.empty() && val.front() == '$') {
            fail(w, std::string(key) + "=\"" + std::string(val) + "\": plugins cannot pass " +
                        std::string(key) + " through a prop");
            continue;
        }
        if (is_callback && val != "plugin_event") {
            fail(w, std::string(key) + "=\"" + std::string(val) +
                        "\": plugins may only use the plugin_event callback");
        } else if (is_subject && !is_owned_name(w.id, val)) {
            fail(w, std::string(key) + "=\"" + std::string(val) + "\": subject must be named " +
                        w.id + "_<name>");
        } else if (is_target && !is_owned_name(w.id, val.substr(0, val.find(':')))) {
            fail(w, "user_data=\"" + std::string(val) + "\": handler must be named " + w.id +
                        "_<name>");
        }
    }
}

void on_end(void*, const XML_Char*) {}

} // namespace

std::string check_plugin_xml(std::string_view id, const std::string& xml) {
    Walk w{std::string(id), {}};
    XML_Parser p = XML_ParserCreate(nullptr);
    XML_SetUserData(p, &w);
    XML_SetElementHandler(p, &on_start, &on_end);
    if (XML_Parse(p, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_ERROR)
        fail(w, std::string("not valid XML: ") + XML_ErrorString(XML_GetErrorCode(p)));
    XML_ParserFree(p);
    return w.error;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
```

`bind_` attributes whose value is not a subject name (a style name in `<bind_style name=...>` is the `name` attribute, not `bind_*`, so it is unaffected) need no exception; if the grep in Step 1 shows a `bind_*` attribute carrying something other than a subject name, exempt it by name and say so in the report.

In `PluginHost::load`, after the ownership check on the file stems and before the shadow check, read each file and reject on the first violation:

```cpp
    for (const auto& p : xmls) {
        std::string why = check_plugin_xml(id, read_file(p));
        if (!why.empty()) {
            info.status = PluginStatus::Invalid;
            info.reason = p.filename().string() + ": " + why;
            return false;
        }
    }
```

Add `src/plugin/plugin_xml_policy.cpp` to `app_srcs_excluded.txt`.

- [ ] **Step 4: Run the tests**

Run: `make t F='[xml_policy]'` then `make t F='[plugin]'`
Expected: PASS. Hand mutations: make the callback test `val == "plugin_event" && false`, and the `$` test never fire; each case goes red.

- [ ] **Step 5: Commit**

```bash
git add include/plugin_xml_policy.h src/plugin/plugin_xml_policy.cpp src/plugin/plugin_host.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt tests/unit/test_plugin_xml_policy.cpp tests/unit/test_plugin_host.cpp tests/fixtures/plugins/app-callback
git commit -m "feat(plugin): plugin XML may use only plugin_event and its own subjects"
git show --stat HEAD
```

---

### Task 3: Plugin subjects outlive the plugin until nothing observes them

helix-xml's bind records (`lib/helix-xml/src/xml/lv_xml_expr.c#expr_bind_delete_cb`, the fragment records in `lv_xml.c#xml_frag_instance_delete_cb`) keep observer handles and detach them when their object is deleted. `lv_subject_deinit` frees those observers first, so deleting a plugin object after its subject was deinitialised writes into freed memory. LVGL sends `LV_EVENT_DELETE` to a parent before its children (`lib/lvgl/src/core/lv_obj_tree.c#obj_delete_core`), so no root-level delete hook can tell when a subtree is gone. Instead, a plugin's subjects are kept, unregistered by name but alive, until their observer list is empty, and freed then. This replaces Phase 1's carry "delete plugin widgets before runtime unload": unload may now run from any context, including the fault path inside an update-queue drain, where synchronous deletion is banned (`include/ui_utils.h#safe_delete`).

**Files:**
- Modify: `src/plugin/lua_bind_ui.cpp`, `include/lua_bindings.h`, `src/plugin/plugin_host.cpp`
- Test: `tests/unit/test_lua_bindings_ui.cpp`

**Interfaces:**
- Produces:

```cpp
namespace helix::plugin {
/// Frees every retired plugin subject that no observer holds any more. Cheap; PluginHost
/// calls it on every load and unload.
void sweep_retired_subjects();
/// Retired subjects still waiting for their observers to go.
size_t retired_subject_count();
}
```

- [ ] **Step 1: Write the failing test**

Append to `tests/unit/test_lua_bindings_ui.cpp` (it already uses `LVGLTestFixture` and `BoundRuntime`; match its includes):

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "a bound object outlives its plugin's runtime safely",
                 "[plugin][lua_bindings_ui]") {
    sweep_retired_subjects();
    size_t before = retired_subject_count();
    lv_obj_t* label = nullptr;
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(s = helix.subject.string("status", "hi"))"));
        label = lv_label_create(lv_screen_active());
        lv_subject_t* subj = lv_xml_get_subject(nullptr, "test-plugin_status");
        REQUIRE(subj);
        lv_label_bind_text(label, subj, nullptr);
    } // runtime destroyed while the label still observes the subject
    CHECK(lv_xml_get_subject(nullptr, "test-plugin_status") == nullptr);
    CHECK(retired_subject_count() == before + 1);
    CHECK(std::string(lv_label_get_text(label)) == "hi");

    lv_obj_delete(label); // detaches from a subject that must still be alive
    sweep_retired_subjects();
    CHECK(retired_subject_count() == before);
}

TEST_CASE_METHOD(LVGLTestFixture, "an unobserved subject is freed at unload",
                 "[plugin][lua_bindings_ui]") {
    sweep_retired_subjects();
    size_t before = retired_subject_count();
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(s = helix.subject.int("n", 1); s:observe(function() end))"));
    }
    CHECK(retired_subject_count() == before);
}
```

The second test also proves the Lua observer is removed explicitly at close (otherwise it keeps the list non-empty and the subject retires).

Run: `make t F='[lua_bindings_ui]'`
Expected: compile error (`sweep_retired_subjects` undeclared).

- [ ] **Step 2: Implement**

In `src/plugin/lua_bind_ui.cpp`:

1. `ObserverCtx` gains `lv_observer_t* handle = nullptr;`; `subject_observe` stores the return of `lv_subject_add_observer` there.
2. A process-lifetime retired list, never destroyed, so an object deleted during process teardown still finds its subject:

```cpp
std::vector<std::unique_ptr<SubjectEntry>>& retired_subjects() {
    static auto* list = new std::vector<std::unique_ptr<SubjectEntry>>();
    return *list;
}

bool unobserved(lv_subject_t& s) {
    return lv_ll_get_head(&s.subs_ll) == nullptr;
}
```

3. The closer becomes:

```cpp
    ctx.rt.on_close([state] {
        for (auto& o : state->observers) {
            if (o->handle)
                lv_observer_remove(o->handle);
        }
        for (auto& s : state->subjects) {
            // A later registration under the same name replaced the record's pointer, so
            // only a record still pointing at this subject is the plugin's to remove.
            if (lv_xml_get_subject(nullptr, s->full_name.c_str()) == &s->subject)
                lv_xml_unregister_subject(nullptr, s->full_name.c_str());
            if (unobserved(s->subject))
                lv_subject_deinit(&s->subject);
            else
                retired_subjects().push_back(std::move(s));
        }
        delete state;
    });
```

4. The two functions:

```cpp
void sweep_retired_subjects() {
    auto& list = retired_subjects();
    for (auto it = list.begin(); it != list.end();) {
        if (unobserved((*it)->subject)) {
            lv_subject_deinit(&(*it)->subject);
            it = list.erase(it);
        } else {
            ++it;
        }
    }
}

size_t retired_subject_count() {
    return retired_subjects().size();
}
```

Add a `ponytail:` comment on `sweep_retired_subjects`: retired subjects are freed at the next plugin load or unload rather than the moment their last observer goes; a sweep on a timer is the upgrade if a device ever shows the list growing.

Declare both in `include/lua_bindings.h`. In `PluginHost::load` (first line) and at the end of `PluginHost::unload`, call `sweep_retired_subjects()`.

- [ ] **Step 3: Run the tests**

Run: `make t F='[lua_bindings_ui]'` then `make t F='[plugin]'`
Expected: PASS. Hand mutations: deinit unconditionally in the closer (the first test fails: the label's text read or the delete touches freed memory, and the retired count check fails); skip the `lv_observer_remove` loop (the second test goes red).

- [ ] **Step 4: Commit**

```bash
git add src/plugin/lua_bind_ui.cpp include/lua_bindings.h src/plugin/plugin_host.cpp tests/unit/test_lua_bindings_ui.cpp
git commit -m "fix(plugin): plugin subjects live until nothing observes them"
git show --stat HEAD
```

---

### Task 4: Runtime widget definitions and the Plugins category

**Files:**
- Modify: `include/panel_widget_registry.h`, `src/ui/panel_widget_registry.cpp`, `include/panel_widget_manager.h`, `src/ui/panel_widget_manager.cpp`
- Test: `tests/unit/test_panel_widget_runtime_defs.cpp`

**Interfaces:**
- Produces:

```cpp
namespace helix {
enum class WidgetCategory { PrintStatus, Temperature, Filament, Controls, System, Plugins };

/// A widget definition added while the app runs. Spans are in grid tracks.
struct RuntimeWidgetDef {
    std::string id;
    std::string display_name;
    std::string icon;
    std::string description;
    int colspan = 2, rowspan = 2, max_colspan = 0, max_rowspan = 0;
    WidgetFactory factory;
};

/// Adds, or replaces, a Plugins-category definition whose strings the registry owns. False
/// when `def.id` is a built-in widget. A definition pointer from find_widget_def() or
/// get_all_widget_defs() is valid until the next register or unregister call.
bool register_runtime_widget_def(RuntimeWidgetDef def);
void unregister_runtime_widget_def(std::string_view id);

// PanelWidgetManager:
/// The set of definitions changed: reload every panel's layout and rebuild every panel and
/// catalog that lists widgets. Main thread.
void notify_widget_defs_changed();
}
```

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_panel_widget_runtime_defs.cpp` (the registry cases are plain `TEST_CASE`s; the rebuild case needs `LVGLTestFixture` for the gate observers):

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include "../lvgl_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

using namespace helix;

TEST_CASE("runtime widget defs register, list and unregister", "[widget_registry]") {
    size_t base = get_all_widget_defs().size();
    RuntimeWidgetDef d;
    d.id = "demo-plug_tile";
    d.display_name = "Demo";
    d.icon = "puzzle_outline";
    d.description = "A plugin tile";
    REQUIRE(register_runtime_widget_def(d));

    const PanelWidgetDef* def = find_widget_def("demo-plug_tile");
    REQUIRE(def);
    CHECK(def->category == WidgetCategory::Plugins);
    CHECK_FALSE(def->default_enabled);
    CHECK_FALSE(def->multi_instance);
    CHECK(std::string(def->display_name) == "Demo");
    CHECK(get_all_widget_defs().size() == base + 1);

    const char* id_before = def->id;
    d.display_name = "Renamed";
    REQUIRE(register_runtime_widget_def(d)); // replaces, keeps the id storage
    CHECK(get_all_widget_defs().size() == base + 1);
    CHECK(find_widget_def("demo-plug_tile")->id == id_before);

    unregister_runtime_widget_def("demo-plug_tile");
    CHECK(find_widget_def("demo-plug_tile") == nullptr);
    CHECK(get_all_widget_defs().size() == base);
}

TEST_CASE("a runtime def cannot take a built-in id", "[widget_registry]") {
    RuntimeWidgetDef d;
    d.id = "print_status";
    d.display_name = "X";
    CHECK_FALSE(register_runtime_widget_def(d));
    CHECK(find_widget_def("print_status")->category == WidgetCategory::PrintStatus);
}

TEST_CASE_METHOD(LVGLTestFixture, "changed definitions rebuild every widget list",
                 "[widget_registry]") {
    int home = 0, catalog = 0;
    auto& mgr = PanelWidgetManager::instance();
    mgr.register_rebuild_callback("test_home", [&] { ++home; });
    mgr.setup_gate_observers("test_catalog", [&] { ++catalog; });
    mgr.notify_widget_defs_changed();
    CHECK(home == 1);
    CHECK(catalog == 1);
    PanelWidgetManager::unregister_rebuild_callback("test_home");
    PanelWidgetManager::clear_gate_observers("test_catalog");
}

TEST_CASE("the Plugins category exists and comes last", "[widget_registry]") {
    const auto& cats = get_widget_categories();
    REQUIRE_FALSE(cats.empty());
    CHECK(cats.back().id == WidgetCategory::Plugins);
    CHECK(std::string(cats.back().icon) == "puzzle_outline");
}
```

Run: `make t F='[widget_registry]'`
Expected: compile error.

- [ ] **Step 2: Implement the registry**

In `src/ui/panel_widget_registry.cpp`:

- Add `{WidgetCategory::Plugins, "Plugins", "Plugins", "puzzle_outline"}` last in `s_widget_categories`. The catalog already skips a category with nothing to place (`ui_widget_catalog_overlay.cpp#populate_category_rows`), so the row appears only when a plugin widget is available.
- Runtime storage that never frees an id string, so a catalog row's `user_data` holding `def.id` stays readable until the row is rebuilt:

```cpp
namespace {
struct RuntimeSlot {
    RuntimeWidgetDef def;
    bool active = false;
};
// Node-based and never erased: a slot's `def.id` buffer lives for the process.
std::map<std::string, RuntimeSlot, std::less<>>& runtime_slots() {
    static auto* slots = new std::map<std::string, RuntimeSlot, std::less<>>();
    return *slots;
}
std::vector<PanelWidgetDef> s_all_defs;
bool s_all_defs_dirty = true;

void rebuild_all_defs() {
    s_all_defs = s_widget_defs;
    for (auto& [id, slot] : runtime_slots()) {
        if (!slot.active)
            continue;
        const RuntimeWidgetDef& r = slot.def;
        PanelWidgetDef d{r.id.c_str(), r.display_name.c_str(), r.icon.c_str(),
                         r.description.c_str(), nullptr, nullptr, WidgetCategory::Plugins};
        d.default_enabled = false;
        d.colspan = r.colspan;
        d.rowspan = r.rowspan;
        d.max_colspan = r.max_colspan;
        d.max_rowspan = r.max_rowspan;
        d.factory = r.factory;
        s_all_defs.push_back(d);
    }
    s_all_defs_dirty = false;
}
} // namespace
```

- `get_all_widget_defs()` returns `s_all_defs` after `if (s_all_defs_dirty) rebuild_all_defs();`. `find_widget_def` searches `get_all_widget_defs()` instead of `s_widget_defs` (keep its multi-instance fallback). `widget_def_count()` returns `get_all_widget_defs().size()`.
- `register_widget_factory` and `register_widget_subjects` still edit `s_widget_defs`, then set `s_all_defs_dirty = true`.
- The two new functions:

```cpp
bool register_runtime_widget_def(RuntimeWidgetDef def) {
    for (const auto& b : s_widget_defs) {
        if (def.id == b.id)
            return false;
    }
    auto& slots = runtime_slots();
    auto it = slots.find(def.id);
    if (it == slots.end()) {
        std::string key = def.id;
        slots.emplace(std::move(key), RuntimeSlot{std::move(def), true});
    } else {
        // Field by field, leaving `id` alone: its buffer is what catalog rows point at.
        RuntimeWidgetDef& cur = it->second.def;
        cur.display_name = std::move(def.display_name);
        cur.icon = std::move(def.icon);
        cur.description = std::move(def.description);
        cur.colspan = def.colspan;
        cur.rowspan = def.rowspan;
        cur.max_colspan = def.max_colspan;
        cur.max_rowspan = def.max_rowspan;
        cur.factory = std::move(def.factory);
        it->second.active = true;
    }
    s_all_defs_dirty = true;
    return true;
}

void unregister_runtime_widget_def(std::string_view id) {
    auto& slots = runtime_slots();
    auto it = slots.find(id);
    if (it == slots.end() || !it->second.active)
        return;
    it->second.active = false;
    it->second.def.factory = nullptr;
    s_all_defs_dirty = true;
}
```

Map nodes never move, and the replace path never assigns `id`, so a slot's id buffer keeps its address for the process. The test's `id_before` check pins this.

- [ ] **Step 3: Implement the rebuild fan-out**

In `PanelWidgetManager`:

```cpp
void PanelWidgetManager::notify_widget_defs_changed() {
    clear_all_panel_configs();
    // Copies: a callback may re-register and move the map entries.
    std::vector<RebuildCallback> cbs;
    for (const auto& [id, cb] : rebuild_callbacks_)
        cbs.push_back(cb);
    for (const auto& [id, cb] : gate_rebuild_callbacks_)
        cbs.push_back(cb);
    for (auto& cb : cbs) {
        if (cb)
            cb();
    }
}
```

The catalog's gate callback runs `WidgetCatalogOverlay::refresh_gated_rows`, which compares a per-definition snapshot, so a changed number of definitions rebuilds its rows.

- [ ] **Step 4: Run the tests**

Run: `make t F='[widget_registry]'`, `make t F='[panel_widget]'`, `make t F='[widget_config]'`
Expected: PASS. Hand mutations: skip the built-in id check (the second test goes red); in `unregister_runtime_widget_def` leave `active` true (the first test goes red); drop the gate-callback loop in `notify_widget_defs_changed` (the rebuild test goes red).

- [ ] **Step 5: Commit**

```bash
git add include/panel_widget_registry.h src/ui/panel_widget_registry.cpp include/panel_widget_manager.h src/ui/panel_widget_manager.cpp tests/unit/test_panel_widget_runtime_defs.cpp
git commit -m "feat(widgets): runtime widget definitions and a Plugins catalog category"
git show --stat HEAD
```

`make translation-sync` then `make translations` for the "Plugins" string; commit the YAMLs and `ui_xml/translations/*.xml` in the same commit.

---

### Task 5: The home layout keeps widget ids it does not know

**Files:**
- Modify: `include/panel_widget_config.h`, `src/system/panel_widget_config.cpp`
- Test: `tests/unit/test_panel_widget_config.cpp`

**Interfaces:**
- Produces: `PageConfig` gains `nlohmann::json retained = nlohmann::json::array();` (the saved entries whose id has no definition, verbatim). `parse_widget_array` gains a trailing `nlohmann::json* retained = nullptr` parameter.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_panel_widget_config.cpp`, using `PanelWidgetConfigFixture::setup_with_pages`:

```cpp
TEST_CASE_METHOD(PanelWidgetConfigFixture, "an unknown widget id survives load and save",
                 "[panel_widget][widget_config]") {
    json unknown = {{"id", "gone-plug_tile"}, {"enabled", true}, {"col", 2}, {"row", 0},
                    {"colspan", 2}, {"rowspan", 2}};
    setup_with_pages({{"main", json::array({{{"id", "print_status"}, {"enabled", true}}, unknown})}});
    PanelWidgetConfig wc("home", config);
    wc.load();
    for (const auto& e : wc.page_entries(0))
        CHECK(e.id != "gone-plug_tile"); // not rendered
    wc.save();

    const json& saved =
        ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"]["home"];
    bool kept = false;
    for (const auto& item : saved["pages"][0]["widgets"])
        kept = kept || item == unknown;
    CHECK(kept);

    PanelWidgetConfig again("home", config);
    again.load();
    again.save();
    int count = 0;
    for (const auto& item : ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"]
                                ["home"]["pages"][0]["widgets"])
        count += item["id"] == "gone-plug_tile";
    CHECK(count == 1); // kept once, not duplicated per save
}

TEST_CASE_METHOD(PanelWidgetConfigFixture, "removing a page keeps its unknown ids",
                 "[panel_widget][widget_config]") {
    json unknown = {{"id", "gone-plug_tile"}, {"enabled", true}, {"col", 0}, {"row", 0}};
    setup_with_pages({{"main", json::array()}, {"extra", json::array({unknown})}});
    PanelWidgetConfig wc("home", config);
    wc.load();
    REQUIRE(wc.remove_page(1));
    wc.save();
    bool kept = false;
    for (const auto& page :
         ConfigTestAccess::data(config)["printers"]["default"]["panel_widgets"]["home"]["pages"])
        for (const auto& item : page["widgets"])
            kept = kept || item["id"] == "gone-plug_tile";
    CHECK(kept);
}
```

Run: `make t F='[widget_config]'`
Expected: FAIL: the unknown entry is dropped (`Dropping unknown widget ID` in the log).

- [ ] **Step 2: Implement**

In `parse_widget_array`, replace the drop with a keep when a destination is given:

```cpp
        if (find_widget_def(id) == nullptr) {
            if (retained)
                retained->push_back(item);
            spdlog::debug("[PanelWidgetConfig] Keeping unknown widget ID: {}", id);
            continue;
        }
```

Record the id in `seen_ids` before the check so a duplicate unknown entry is kept once. Pass `&page.retained` at the three page-format call sites (`load`'s page loop, the seed-path loop, and the pages loop near the end of the file). The legacy flat-array path passes nothing: it is migrated to pages and was already dropping unknown ids.

In `serialize_pages`, after writing the page's known entries:

```cpp
        for (const auto& item : page.retained)
            widgets_array.push_back(item);
```

In `remove_page`, before erasing, move the page's `retained` items onto the main page's `retained` (after adjusting the main index the way the function already does).

- [ ] **Step 3: Run the tests**

Run: `make t F='[widget_config]'` then `make t F='[panel_widget]'`
Expected: PASS. Hand mutations: drop the `serialize_pages` loop (first test red); drop the `remove_page` move (second test red).

- [ ] **Step 4: Commit**

```bash
git add include/panel_widget_config.h src/system/panel_widget_config.cpp tests/unit/test_panel_widget_config.cpp
git commit -m "feat(widgets): the home layout keeps widget ids it has no definition for"
git show --stat HEAD
```

---

### Task 6: `helix.widget`, `LuaPanelWidget`, and widget definitions per plugin

**Files:**
- Create: `src/plugin/lua_bind_widget.cpp`, `include/lua_panel_widget.h`, `src/plugin/lua_panel_widget.cpp`
- Create fixture: `tests/fixtures/plugins/widget-demo/` (`manifest.json`, `main.lua`, `ui/widget-demo_tile.xml`)
- Modify: `include/lua_bindings.h`, `include/plugin_host.h`, `src/plugin/plugin_host.cpp`, `tests/test_helpers/plugin_test_support.h`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_lua_bindings_widget.cpp`, `tests/unit/test_plugin_host.cpp`

**Interfaces:**
- Consumes: `WidgetDecl` (Task 1), `register_runtime_widget_def` / `notify_widget_defs_changed` (Task 4).
- Produces:

```cpp
namespace helix::plugin {
enum class WidgetHook { Attach, Detach, Size, Activate, Deactivate };
void install_widget_bindings(PluginContext& ctx);
/// Runs a hook `helix.widget` registered for `widget_id` (the full id). False when none.
bool dispatch_widget_hook(LuaRuntime& rt, const std::string& widget_id, WidgetHook hook,
                          const LuaRuntime::PushFn& args = {});

class PluginHost {
    // ...
    /// The host the app runs, or nullptr.
    static PluginHost* live();
};

class LuaPanelWidget : public helix::PanelWidget {
  public:
    LuaPanelWidget(std::string plugin_id, std::string widget_id, std::string component);
    // PanelWidget overrides: id, get_component_name, attach, detach, on_activate,
    // on_deactivate, on_size_changed, supports_reuse (false), on_hooked_root_deleted
};
}
```

Lua:

```lua
helix.widget("tile", {
  on_attach = function() end,
  on_detach = function() end,
  on_size = function(cols, rows, w, h) end,   -- cols/rows in cells, w/h in pixels
  on_activate = function() end,
  on_deactivate = function() end,
})
```

`"tile"` excludes the prefix; the full id `<plugin>_tile` must be declared in the manifest's `widgets`, else a Lua error. Calling again replaces the handlers.

- [ ] **Step 1: The fixture**

`tests/fixtures/plugins/widget-demo/manifest.json`:

```json
{
  "id": "widget-demo",
  "name": "Widget Demo",
  "version": "1.0.0",
  "widgets": [
    {"id": "widget-demo_tile", "name": "Demo Tile", "icon": "puzzle_outline",
     "description": "A test tile", "component": "widget-demo_tile",
     "colspan": 1, "rowspan": 1, "max_colspan": 2, "max_rowspan": 2}
  ],
  "settings": [
    {"key": "enabled", "type": "bool", "label": "Enabled", "default": true},
    {"key": "step", "type": "int", "label": "Step", "min": 1, "max": 20, "default": 5},
    {"key": "ratio", "type": "float", "label": "Ratio", "min": 0, "max": 1, "default": 0.5},
    {"key": "mode", "type": "enum", "label": "Mode", "options": ["a", "b"], "default": "a"},
    {"key": "name", "type": "string", "label": "Name", "default": "x"},
    {"key": "ping", "type": "action", "label": "Ping", "callback": "widget-demo_ping"},
    {"key": "state", "type": "info", "label": "State", "subject": "widget-demo_status"}
  ]
}
```

`main.lua`:

```lua
local status = helix.subject.string("status", "idle")
local size = helix.subject.string("size", "")
events = {}

helix.widget("tile", {
  on_attach = function() table.insert(events, "attach") end,
  on_detach = function() table.insert(events, "detach") end,
  on_size = function(c, r, w, h) size:set(c .. "x" .. r) end,
  on_activate = function() table.insert(events, "activate") end,
  on_deactivate = function() table.insert(events, "deactivate") end,
})

helix.ui.on("ping", function() status:set("pinged") end)
helix.ui.on("open", function() helix.ui.overlay("widget-demo_panel") end)
```

`ui/widget-demo_tile.xml`:

```xml
<?xml version="1.0"?>
<component>
  <view extends="lv_obj" width="100%" height="100%">
    <lv_label name="widget-demo_size_label" bind_text="widget-demo_size"/>
    <lv_button name="widget-demo_open_button">
      <event_cb trigger="clicked" callback="plugin_event" user_data="widget-demo_open"/>
    </lv_button>
  </view>
</component>
```

(Task 7 adds `ui/widget-demo_panel.xml`.)

- [ ] **Step 2: Write the failing tests**

`tests/unit/test_lua_bindings_widget.cpp`:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_helpers/plugin_test_support.h"
#include "lua_bindings.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE("widget hooks run for a declared widget", "[plugin][lua_bindings_widget]") {
    BoundRuntime b({&install_widget_bindings});
    WidgetDecl d;
    d.id = "test-plugin_tile";
    b.manifest.widgets.push_back(d);
    REQUIRE(b.t.run(R"(
        got = ""
        helix.widget("tile", {
          on_attach = function() got = got .. "a" end,
          on_size = function(c, r, w, h) got = got .. c .. r .. w .. h end,
        }))"));
    CHECK(dispatch_widget_hook(*b.t.rt, "test-plugin_tile", WidgetHook::Attach));
    CHECK(dispatch_widget_hook(*b.t.rt, "test-plugin_tile", WidgetHook::Size, [](lua_State* co) {
        for (int v : {1, 2, 30, 40})
            lua_pushinteger(co, v);
        return 4;
    }));
    CHECK_FALSE(dispatch_widget_hook(*b.t.rt, "test-plugin_tile", WidgetHook::Activate));
    CHECK(b.t.global("got") == "a123040");
}

TEST_CASE("helix.widget refuses an undeclared widget", "[plugin][lua_bindings_widget]") {
    BoundRuntime b({&install_widget_bindings});
    REQUIRE(b.t.run(R"(ok, err = pcall(helix.widget, "nope", {}))"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("widgets") != std::string::npos);
}

#endif // HELIX_HAS_PLUGINS
```

Append to `tests/unit/test_plugin_host.cpp`:

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "a loaded plugin's widget is in the registry until unload",
                 "[plugin][host]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("widget-demo")->status == PluginStatus::Loaded);
    const helix::PanelWidgetDef* def = helix::find_widget_def("widget-demo_tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 2);     // 1 cell in tracks
    CHECK(def->max_colspan == 4); // 2 cells

    auto w = def->factory("widget-demo_tile");
    REQUIRE(w);
    lv_obj_t* root = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), w->get_component_name().c_str(), nullptr));
    REQUIRE(root);
    w->attach(root, lv_screen_active());
    w->notify_size_changed(4, 2, 200, 100);
    drain();
    CHECK(std::string(lv_label_get_text(lv_obj_find_by_name(root, "widget-demo_size_label"))) ==
          "2x1");

    rig.host->disable("widget-demo");
    CHECK(helix::find_widget_def("widget-demo_tile") == nullptr);
    w->detach(); // the plugin is gone: no hook runs, nothing crashes
    lv_obj_delete(root);
    w.reset();
}

TEST_CASE_METHOD(LVGLTestFixture, "a widget whose component file is missing is invalid",
                 "[plugin][host]") {
    // Fixture widget-missing: manifest declares component widget-missing_tile, no ui/ file.
    HostRig rig(enabled("widget-missing", {}));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("widget-missing")->status == PluginStatus::Invalid);
    CHECK(helix::find_widget_def("widget-missing_tile") == nullptr);
}
```

Add fixture `tests/fixtures/plugins/widget-missing/` (manifest with one widget whose component has no file, empty `main.lua`). Update fixture counts in older host tests.

Run: `make t F='[lua_bindings_widget]'`
Expected: compile error.

- [ ] **Step 3: Implement `helix.widget`**

`src/plugin/lua_bind_widget.cpp` follows `lua_bind_ui.cpp`'s state pattern: a registry-keyed `WidgetState` holding `std::unordered_map<std::string, std::array<int, 5>> hooks` (refs, `LUA_NOREF` when absent), freed by a closer that unrefs nothing (the state dies with `lua_close`).

```cpp
int widget_register(lua_State* L) {
    PluginContext& ctx = context(L);
    std::string local = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::string full = ctx.manifest.id + "_" + local;
    bool declared = std::any_of(ctx.manifest.widgets.begin(), ctx.manifest.widgets.end(),
                                [&](const WidgetDecl& d) { return d.id == full; });
    if (!declared)
        return luaL_error(L, "helix.widget: '%s' is not in manifest.json widgets", full.c_str());
    static const char* const kNames[] = {"on_attach", "on_detach", "on_size", "on_activate",
                                         "on_deactivate"};
    auto [it, fresh] = widget_state(L).hooks.try_emplace(full);
    if (fresh)
        it->second.fill(LUA_NOREF);
    auto& refs = it->second;
    for (int i = 0; i < 5; ++i) {
        if (refs[i] != LUA_NOREF)
            ctx.rt.unref(refs[i]);
        refs[i] = LUA_NOREF;
        lua_getfield(L, 2, kNames[i]);
        if (lua_isfunction(L, -1))
            refs[i] = ctx.rt.ref_value(L, -1);
        lua_pop(L, 1);
    }
    return 0;
}
```


`dispatch_widget_hook` looks up the ref and calls `rt.invoke(ref, args)`, returning false when absent. Register `helix.widget` as a function on the `helix` table.

Add `&install_widget_bindings` to `PluginHost::load`'s installer list and document `BoundRuntime` users may pass it.

- [ ] **Step 4: Implement `LuaPanelWidget`**

```cpp
// include/lua_panel_widget.h
#pragma once
#include "panel_widget.h"
#include "lua_bindings.h"
#include <string>

namespace helix::plugin {

/// A home widget declared by a plugin. Forwards the grid's lifecycle calls to the plugin's
/// helix.widget hooks while the plugin is loaded, and does nothing once it is not.
class LuaPanelWidget : public helix::PanelWidget {
  public:
    LuaPanelWidget(std::string plugin_id, std::string widget_id, std::string component);
    ~LuaPanelWidget() override;
    const char* id() const override { return widget_id_.c_str(); }
    std::string get_component_name() const override { return component_; }
    void attach(lv_obj_t* widget_obj, lv_obj_t* parent_screen) override;
    void detach() override;
    void on_activate() override;
    void on_deactivate() override;
    void on_size_changed(int colspan, int rowspan, int width_px, int height_px) override;
    bool supports_reuse() const override { return false; }

  protected:
    void on_hooked_root_deleted() override { root_ = nullptr; }

  private:
    void run(WidgetHook hook, const LuaRuntime::PushFn& args = {});
    std::string plugin_id_;
    std::string widget_id_;
    std::string component_;
    lv_obj_t* root_ = nullptr;
};

} // namespace helix::plugin
```

```cpp
// src/plugin/lua_panel_widget.cpp (inside #if HELIX_HAS_PLUGINS)
void LuaPanelWidget::run(WidgetHook hook, const LuaRuntime::PushFn& args) {
    PluginHost* host = PluginHost::live();
    LuaRuntime* rt = host ? host->runtime(plugin_id_) : nullptr;
    if (rt)
        dispatch_widget_hook(*rt, widget_id_, hook, args);
}

void LuaPanelWidget::attach(lv_obj_t* widget_obj, lv_obj_t*) {
    root_ = widget_obj;
    install_delete_hook(root_);
    run(WidgetHook::Attach);
}

void LuaPanelWidget::detach() {
    if (!root_)
        return;
    run(WidgetHook::Detach);
    uninstall_delete_hook();
    root_ = nullptr;
}

LuaPanelWidget::~LuaPanelWidget() {
    detach();
}

void LuaPanelWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    constexpr int kTracks = helix::GridLayout::TRACKS_PER_CELL;
    run(WidgetHook::Size, [=](lua_State* co) {
        lua_pushinteger(co, colspan / kTracks);
        lua_pushinteger(co, rowspan / kTracks);
        lua_pushinteger(co, width_px);
        lua_pushinteger(co, height_px);
        return 4;
    });
}
```

`on_activate` / `on_deactivate` forward with no arguments.

- [ ] **Step 5: Register and unregister definitions in `PluginHost`**

- `static PluginHost* live()` returns the file's `g_live_host`.
- `Loaded` gains `std::vector<std::string> widget_ids;`.
- In `load`, after the XML checks and before registering components: every `WidgetDecl::component` must be one of the `ui/` stems, else `Invalid` with `widget '<id>' names component '<c>', which is not in ui/`. After `main.lua` runs successfully:

```cpp
    for (const WidgetDecl& d : m.widgets) {
        helix::RuntimeWidgetDef def;
        def.id = d.id;
        def.display_name = d.name;
        def.icon = d.icon.empty() ? "puzzle_outline" : d.icon;
        def.description = d.description;
        constexpr int kT = helix::GridLayout::TRACKS_PER_CELL;
        def.colspan = d.colspan * kT;
        def.rowspan = d.rowspan * kT;
        def.max_colspan = d.max_colspan * kT;
        def.max_rowspan = d.max_rowspan * kT;
        def.factory = [pid = id, wid = d.id, comp = d.component](const std::string&) {
            return std::make_unique<LuaPanelWidget>(pid, wid, comp);
        };
        if (helix::register_runtime_widget_def(std::move(def)))
            l.widget_ids.push_back(d.id);
        else
            spdlog::warn("[PluginHost] {}: widget id '{}' is taken", id, d.id);
    }
    if (!l.widget_ids.empty() && !bulk_)
        helix::PanelWidgetManager::instance().notify_widget_defs_changed();
```

- `unload`, in the spec's order: after `on_unload` runs, and before the runtime is destroyed, unregister each id in `l.widget_ids` and, when there were any and `!bulk_`, call `notify_widget_defs_changed()`. The rebuild hands the old tiles to deferred deletion; Task 3 keeps their subjects alive until those deletions finish.
- `bool bulk_ = false;` is set by `unload_all()` and `load_from()` around their loops, and one `notify_widget_defs_changed()` runs after the loop when anything changed. Shutdown calls `unload_all` after `NavigationManager::shutdown()`; its rebuild is harmless there, but skip it when `NavigationManager::is_destroyed()` is true.

- [ ] **Step 6: Run the tests**

Run: `make t F='[lua_bindings_widget]'`, `make t F='[plugin]'`, `make t F='[widget_registry]'`, then `make`
Expected: PASS and a clean app build. Hand mutations: skip `unregister_runtime_widget_def` in unload (the host test's `== nullptr` check goes red); drop the `/ kTracks` in `on_size_changed` (the `"2x1"` check goes red).

- [ ] **Step 7: Commit**

```bash
git add src/plugin/lua_bind_widget.cpp include/lua_panel_widget.h src/plugin/lua_panel_widget.cpp include/lua_bindings.h include/plugin_host.h src/plugin/plugin_host.cpp tests/test_helpers/plugin_test_support.h firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt tests/unit/test_lua_bindings_widget.cpp tests/unit/test_plugin_host.cpp tests/fixtures/plugins/widget-demo tests/fixtures/plugins/widget-missing
git commit -m "feat(plugin): plugin home widgets through helix.widget and LuaPanelWidget"
git show --stat HEAD
```

---

### Task 7: Plugin overlays

**Files:**
- Create: `include/plugin_overlay_host.h`, `src/plugin/plugin_overlay_host.cpp`, fixture `tests/fixtures/plugins/widget-demo/ui/widget-demo_panel.xml`
- Modify: `include/lua_bindings.h`, `src/plugin/lua_bind_ui.cpp`, `src/plugin/plugin_host.cpp`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_overlay_host.cpp`, `tests/unit/test_lua_bindings_ui.cpp`

**Interfaces:**
- Produces:

```cpp
namespace helix::plugin {
/// What the ui bindings need to open overlays. PluginHost provides the real one; tests may
/// provide a fake. Handles are positive and unique per process.
struct PluginUi {
    std::function<int(const std::string& component, std::function<void()> on_closed)> open;
    std::function<void(int handle)> close;
};
// PluginContext gains, last:  PluginUi* ui = nullptr;

/// Plugin overlays on the navigation stack. Main thread.
class PluginOverlayHost {
  public:
    /// Creates `component` on the active screen and pushes it. 0 when it cannot be created.
    int open(const std::string& plugin_id, const std::string& component,
             std::function<void()> on_closed);
    void close(int handle);
    /// Pops and deletes every overlay `plugin_id` has open, without running on_closed.
    void close_all(const std::string& plugin_id);
    size_t open_count(const std::string& plugin_id) const;
};
}
```

Lua: `local h = helix.ui.overlay("widget-demo_panel", {on_close = fn})`; `h:close()`. The component must be owned by the plugin (Lua error otherwise). `on_close` runs when the user leaves the overlay (back button or `h:close()`), never at unload.

- [ ] **Step 1: Fixture overlay**

`tests/fixtures/plugins/widget-demo/ui/widget-demo_panel.xml`:

```xml
<?xml version="1.0"?>
<component>
  <view extends="overlay_panel" title="Demo" bg_color="#screen_bg">
    <lv_label name="widget-demo_panel_status" bind_text="widget-demo_status"/>
  </view>
</component>
```

- [ ] **Step 2: Write the failing tests**

`tests/unit/test_plugin_overlay_host.cpp` drives `PluginHost` with the widget-demo fixture under `LVGLTestFixture` (copy `HostRig` from `test_plugin_host.cpp` under a different name, such as `OverlayRig`, so the test-mirror gate sees no duplicate helper):

```cpp
TEST_CASE_METHOD(LVGLTestFixture, "a plugin overlay pushes, closes and runs on_close",
                 "[plugin][overlay]") {
    OverlayRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(
        closed = false
        h = helix.ui.overlay("widget-demo_panel", {on_close = function() closed = true end}))",
                           "t"));
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("widget-demo") == 1);

    REQUIRE(rt->run_string("h:close()", "t"));
    drain();
    process_lvgl(500); // let the slide-out finish
    CHECK(rig.host->overlays().open_count("widget-demo") == 0);
    lua_getglobal(rt->state(), "closed");
    CHECK(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);
}

TEST_CASE_METHOD(LVGLTestFixture, "unload pops the plugin's overlays", "[plugin][overlay]") {
    OverlayRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt->run_string(R"(helix.ui.overlay("widget-demo_panel"))", "t"));
    drain();
    REQUIRE(NavigationManager::instance().has_open_overlays());
    rig.host->disable("widget-demo");
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
}

TEST_CASE_METHOD(LVGLTestFixture, "an overlay of another plugin's component is refused",
                 "[plugin][overlay]") {
    OverlayRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt->run_string(R"(ok, err = pcall(helix.ui.overlay, "hello_panel"))", "t"));
    lua_getglobal(rt->state(), "ok");
    CHECK_FALSE(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);
}
```

`process_lvgl(ms)` is `LVGLTestFixture`'s timer pump (`tests/lvgl_test_fixture.h`).

Run: `make t F='[overlay]'`
Expected: compile error.

- [ ] **Step 3: Implement `PluginOverlayHost`**

Each open overlay is a record `{handle, plugin_id, root, on_closed, Lifecycle}` where `Lifecycle` is a small `IPanelLifecycle` whose `on_activate` does nothing and whose `on_deactivate(DeactivateReason)` does nothing (plugin overlays have no activation work in this phase). Records live in a `std::list` so `Lifecycle` addresses are stable.

`open`:

```cpp
    auto* root = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), component.c_str(), nullptr));
    if (!root)
        return 0;
    auto& rec = records_.emplace_back();
    rec.handle = next_handle_++;
    rec.plugin_id = plugin_id;
    rec.root = root;
    rec.on_closed = std::move(on_closed);
    auto& nav = NavigationManager::instance();
    nav.register_overlay_instance(root, &rec.lifecycle);
    int handle = rec.handle;
    nav.register_overlay_close_callback(root, [this, handle] { on_nav_closed(handle); });
    nav.push_overlay(root);
    return handle;
```

`on_nav_closed(handle)` (the user left, or `close` called `go_back`): find the record, take its `on_closed`, unregister the instance, `helix::ui::safe_delete_deferred(rec.root)`, erase the record, then run `on_closed`. The navigation manager runs close callbacks deferred, so deferred deletion is the allowed form here (`src/ui/overlay_base.cpp#teardown_overlay_ui` does the same).

`close(handle)`: if the record's root is on top (`nav.is_panel_on_top(root)`), `nav.go_back()`; the close callback finishes the job. Otherwise unregister its close callback and instance, deferred-delete the root (the delete hook scrubs it from the stack), erase the record and run `on_closed`.

`close_all(plugin_id)`: for this plugin's records, newest first: `unregister_overlay_close_callback`, then `go_back()` when on top, else nothing; then `unregister_overlay_instance`, `safe_delete_deferred`, erase, and do not run `on_closed` (the plugin is unloading and its Lua is gone).

`PluginHost` owns one `PluginOverlayHost overlays_` with a `const PluginOverlayHost& overlays() const` accessor for tests, fills a per-plugin `PluginUi` (`open` binds the plugin id) into the context, and in `unload` calls `overlays_.close_all(id)` right after `on_unload`, before widget definitions go.

- [ ] **Step 4: Implement `helix.ui.overlay` and closing the confirm dialog**

In `lua_bind_ui.cpp`: `ui_overlay(L)` checks `ctx.ui` is set (else Lua error "helix.ui.overlay is not available here"), checks the component is owned by the plugin, refs `on_close` when given, and calls `ctx.ui->open(component, [rtp, ref, token = rt.token()] { if (!token.expired() && ref != LUA_NOREF) rtp->invoke(ref); })`. A 0 handle is a Lua error "cannot open <component>". It returns a table with a `close` method calling `ctx.ui->close(handle)` (handle captured as an upvalue).

`UiState` records the open confirm dialog pointer from `modal_confirm`, cleared in `release`. The closer calls `Modal::hide(dialog)` when one is still open, so unload does not leave a plugin's question on screen.

- [ ] **Step 5: Run the tests**

Run: `make t F='[overlay]'`, `make t F='[lua_bindings_ui]'`, `make t F='[plugin]'`, `make`
Expected: PASS. Hand mutations: skip `close_all` in unload (the unload test goes red); skip running `on_closed` in `on_nav_closed` (the first test goes red).

- [ ] **Step 6: Commit**

```bash
git add include/plugin_overlay_host.h src/plugin/plugin_overlay_host.cpp include/lua_bindings.h src/plugin/lua_bind_ui.cpp src/plugin/plugin_host.cpp include/plugin_host.h firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt tests/unit/test_plugin_overlay_host.cpp tests/unit/test_lua_bindings_ui.cpp tests/fixtures/plugins/widget-demo/ui/widget-demo_panel.xml
git commit -m "feat(plugin): plugin overlays through helix.ui.overlay; unload pops them"
git show --stat HEAD
```

---

### Task 8: `setting_text_row`

**Files:**
- Create: `ui_xml/setting_text_row.xml`
- Modify: `src/xml_registration.cpp`
- Test: `tests/unit/test_setting_text_row.cpp`

The row matches `setting_toggle_row.xml`'s label column (icon, label, description with the same props and breakpoint styles, copied from that file) and puts a `text_input` where the toggle is:

```xml
<?xml version="1.0"?>
<!-- Copyright (C) 2025-2026 356C LLC -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- A setting whose value is free text; tapping the field opens the keyboard. -->
<component>
  <api>
    <prop name="label" type="string" default="Setting"/>
    <prop name="label_tag" type="string" default=""/>
    <prop name="icon" type="string" default=""/>
    <prop name="description" type="string" default=""/>
    <prop name="description_tag" type="string" default=""/>
    <prop name="value" type="string" default=""/>
    <prop name="placeholder" type="string" default=""/>
    <prop name="callback" type="string" default=""/>
  </api>
  <view extends="lv_obj" ...same outer row attributes as setting_toggle_row...>
    ...same icon and label column as setting_toggle_row...
    <text_input name="value_input" text="$value" placeholder_text="$placeholder" width="40%">
      <event_cb trigger="ready" callback="$callback"/>
      <event_cb trigger="defocused" callback="$callback"/>
    </text_input>
  </view>
</component>
```

Check `text_input`'s attribute names in `src/ui/ui_text_input.cpp` (`placeholder_text` is the documented one) and whether `text` is the right attribute for an initial value; adjust and say so.

- [ ] **Step 1: Write the failing test**

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../lvgl_test_fixture.h"

#include <string>

#include "../catch_amalgamated.hpp"

namespace {
int g_text_row_fired = 0;
void text_row_cb(lv_event_t*) {
    ++g_text_row_fired;
}
} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "setting_text_row shows its value and reports edits",
                 "[setting_text_row]") {
    lv_xml_register_event_cb(nullptr, "test_text_row_cb", &text_row_cb);
    const char* attrs[] = {"label", "Name", "value", "hello", "callback", "test_text_row_cb",
                           nullptr};
    auto* row = static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "setting_text_row", attrs));
    REQUIRE(row);
    lv_obj_t* input = lv_obj_find_by_name(row, "value_input");
    REQUIRE(input);
    CHECK(std::string(lv_textarea_get_text(input)) == "hello");
    g_text_row_fired = 0;
    lv_obj_send_event(input, LV_EVENT_READY, nullptr);
    CHECK(g_text_row_fired == 1);
    lv_obj_delete(row);
}
```

(If the XML test fixture that loads `ui_xml/` components is `XMLTestFixture` rather than `LVGLTestFixture`, use it; check how `test_widget_catalog_categories.cpp` gets components registered.)

Run: `make t F='[setting_text_row]'`
Expected: FAIL (component not registered).

- [ ] **Step 2: Implement and register**

Write the XML above and add `register_xml("setting_text_row.xml");` beside `register_xml("setting_toggle_row.xml");` in `src/xml_registration.cpp`.

- [ ] **Step 3: Run**

Run: `make t F='[setting_text_row]'`. Expected: PASS. Hand mutation: drop the `ready` event_cb (red).

- [ ] **Step 4: Commit**

```bash
git add ui_xml/setting_text_row.xml tests/unit/test_setting_text_row.cpp src/xml_registration.cpp
git commit -m "feat(ui): setting_text_row for free-text settings"
git show --stat HEAD
```

---

### Task 9: The generated plugin settings screen

**Files:**
- Create: `include/plugin_settings_overlay.h`, `src/plugin/plugin_settings_overlay.cpp`, `ui_xml/plugin_settings_overlay.xml`
- Modify: `include/plugin_host.h`, `src/plugin/plugin_host.cpp`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_settings_overlay.cpp`

**Interfaces:**
- Consumes: `SettingDecl` (Phase 1), `set_plugin_setting` (Phase 1), `setting_text_row` (Task 8), `PluginOverlayHost` (Task 7).
- Produces:

```cpp
namespace helix::plugin {
/// The setting_*_row component and its attributes for one declaration, given the current
/// value. Pure, so the mapping is testable without LVGL.
struct SettingRowSpec {
    std::string component;                                  ///< e.g. "setting_toggle_row"
    std::vector<std::pair<std::string, std::string>> attrs; ///< in order
};
SettingRowSpec setting_row_spec(const std::string& plugin_id, const SettingDecl& d,
                                const json& current);

class PluginHost {
    // ...
    /// Opens the plugin's settings: its settings_overlay component when the manifest names
    /// one, else the generated screen. False when the plugin is not loaded.
    bool open_settings(const std::string& id);
    /// set_plugin_setting for the loaded plugin `id`.
    bool set_setting(const std::string& id, const std::string& key, const json& value);
};
}
```

Mapping (spec § manifest.json):

| Type | Component | Attributes |
|---|---|---|
| bool | `setting_toggle_row` | `label`, `callback="plugin_setting_changed"`; the switch's checked state set after creation |
| int, float | `setting_slider_row` | `label`, `min`, `max`, `value` (current), `callback="plugin_setting_changed"`, `trigger="released"`; float scales by 100 to the slider's integer range |
| enum | `setting_dropdown_row` | `label`, `options` joined with `\n`, `callback="plugin_setting_changed"`; the selected index set after creation |
| string | `setting_text_row` | `label`, `value`, `callback="plugin_setting_changed"` |
| action | `setting_action_row` | `label`, `callback="plugin_setting_action"` |
| info | `setting_info_row` | `label`, value bound to the plugin subject `d.subject` (check `setting_info_row.xml` for a bind prop; if it has none, add a `bind_value` prop there, since it must follow the subject) |

Every row's root gets `user_data` pointing at a `RowBinding {plugin_id, key}` owned by the overlay, so the two callbacks find their setting by walking from the event target up to the row root (`lv_obj_get_parent` until a parent's user data is a known binding; keep the set of live bindings in the overlay to check membership).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("each setting type maps to its row", "[plugin][plugin_settings]") {
    SettingDecl b; b.key = "on"; b.label = "On"; b.type = SettingType::Bool;
    CHECK(setting_row_spec("p", b, true).component == "setting_toggle_row");
    SettingDecl i; i.key = "n"; i.label = "N"; i.type = SettingType::Int; i.min = 1; i.max = 20;
    auto si = setting_row_spec("p", i, 5);
    CHECK(si.component == "setting_slider_row");
    CHECK(attr(si, "min") == "1");
    CHECK(attr(si, "max") == "20");
    CHECK(attr(si, "value") == "5");
    SettingDecl f; f.key = "r"; f.label = "R"; f.type = SettingType::Float; f.min = 0; f.max = 1;
    CHECK(attr(setting_row_spec("p", f, 0.5), "value") == "50");
    SettingDecl e; e.key = "m"; e.label = "M"; e.type = SettingType::Enum; e.options = {"a", "b"};
    CHECK(attr(setting_row_spec("p", e, "b"), "options") == "a\nb");
    SettingDecl s; s.key = "s"; s.label = "S"; s.type = SettingType::String;
    CHECK(setting_row_spec("p", s, "x").component == "setting_text_row");
    SettingDecl a; a.key = "go"; a.label = "Go"; a.type = SettingType::Action; a.callback = "p_go";
    CHECK(attr(setting_row_spec("p", a, nullptr), "callback") == "plugin_setting_action");
}

TEST_CASE_METHOD(LVGLTestFixture, "the generated screen writes settings through the plugin's rule",
                 "[plugin][plugin_settings]") {
    SettingsRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt->run_string(R"(seen = nil
        helix.settings.on_change("step", function(v) seen = v end))", "t"));
    REQUIRE(rig.host->open_settings("widget-demo"));
    drain();
    CHECK(rig.host->set_setting("widget-demo", "step", 7));
    CHECK_FALSE(rig.host->set_setting("widget-demo", "step", 99)); // outside [1, 20]
    CHECK(rig.block["settings"]["widget-demo"]["step"] == 7);
    lua_getglobal(rt->state(), "seen");
    CHECK(lua_tointeger(rt->state(), -1) == 7);
    lua_pop(rt->state(), 1);
}
```

`attr(spec, name)` is a local helper returning the attribute value or empty. `SettingsRig` is a third copy of the host rig under its own name.

Add a third case that opens the screen and fires the slider row's event with a value (find the row by `user_data`, set the slider value, send `LV_EVENT_RELEASED`) and checks the stored value, so the row wiring is covered and not just `set_setting`.

Run: `make t F='[plugin_settings]'`. Expected: compile error.

- [ ] **Step 2: Implement**

- `setting_row_spec`: the table above. Numbers are formatted without trailing zeros (`fmt::format("{}", v)` on an integral value; the float slider uses `std::lround(v * 100)`).
- `PluginSettingsOverlay` (an `OverlayBase` subclass, following `src/ui/ui_settings_safety.cpp` for `create`, `show`, `register_overlay_instance` and `push_overlay`) builds `plugin_settings_overlay.xml` (an `overlay_panel` with a scrollable `settings_rows` column and the plugin's name as the title), then one row per declaration with `lv_xml_create(rows, spec.component, attrs)`, applies the post-creation state (toggle checked, dropdown selected), and stores a `RowBinding` in its user data.
- `plugin_setting_changed` (registered once, like `plugin_event`): finds the binding, reads the new value from the target by type (toggle: `lv_obj_has_state(target, LV_STATE_CHECKED)`; slider: `lv_slider_get_value`, divided by 100 for float; dropdown: the option string at `lv_dropdown_get_selected`; text: `lv_textarea_get_text`), and calls `PluginHost::live()->set_setting(...)`. On false (out of range) it restores the row from the stored value.
- `plugin_setting_action`: finds the binding and dispatches the declaration's `callback` through `PluginHost::dispatch_event(callback)`, the path `plugin_event` uses.
- `open_settings`: when `settings_overlay` is set, `overlays_.open(id, m.settings_overlay, {})`; otherwise shows the generated overlay for `id`. A plugin with no declarations and no settings overlay returns false.
- `set_setting`: finds the loaded plugin's context and calls `set_plugin_setting(*l.ctx, key, value)`.
- Unload closes the generated overlay when it shows the unloading plugin (the overlay records its plugin id).

- [ ] **Step 3: Run**

Run: `make t F='[plugin_settings]'`, `make t F='[plugin]'`, `make`. Expected: PASS. Hand mutations: map `Float` to the unscaled value (the `"50"` check goes red); skip `set_plugin_setting` in `set_setting` (the Lua `seen` check goes red).

- [ ] **Step 4: Commit**

```bash
git add include/plugin_settings_overlay.h src/plugin/plugin_settings_overlay.cpp ui_xml/plugin_settings_overlay.xml include/plugin_host.h src/plugin/plugin_host.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt tests/unit/test_plugin_settings_overlay.cpp
git commit -m "feat(plugin): settings screen generated from the manifest schema"
git show --stat HEAD
```

Include `ui_xml/setting_info_row.xml` if Step 2 added a bind prop, and the translation files if any new string was added.

---

### Task 10: Settings → Plugins and consent

**Files:**
- Create: `include/plugin_consent.h`, `src/plugin/plugin_consent.cpp`, `include/plugins_overlay.h`, `src/plugin/plugins_overlay.cpp`, `ui_xml/plugins_overlay.xml`
- Modify: `ui_xml/settings_panel.xml`, `src/ui/ui_panel_settings.cpp`, `include/plugin_host.h`, `src/plugin/plugin_host.cpp`, `app_srcs_excluded.txt`
- Test: `tests/unit/test_plugin_consent.cpp`, `tests/unit/test_plugins_overlay.cpp`

**Interfaces:**
- Produces:

```cpp
namespace helix::plugin {
/// One plain-words line per permission, in enum order.
std::vector<std::string> consent_lines(const PermissionSet& perms);
/// The consent dialog body for enabling `m`, or for approving `grown` on an update when
/// `grown` is non-empty.
std::string consent_message(const Manifest& m, const std::vector<Permission>& grown);
/// Shows the consent dialog; `on_yes` runs only on confirm.
void show_consent(const Manifest& m, const std::vector<Permission>& grown,
                  std::function<void()> on_yes);
}
```

Consent lines (wording approved by Preston 2026-09-29; use verbatim). Settings → Plugins stays hidden until a plugin host exists (approved):

| Permission | Line |
|---|---|
| `gcode` | "Send any G-code command. This is full control of the printer: every macro, including ones that run shell commands, is reachable." |
| `moonraker_write` | "Change printer settings through Moonraker, including uploading, rewriting and deleting files in the printer's config folder." |
| `http` | "Connect to servers on your network and the internet." |
| `storage` | "Keep its own data on this screen (up to 256 KB)." |

A plugin with no permissions gets "This plugin asks for no special permissions." The message starts with "<name> <version> by <author>" (author omitted when empty) and, for an update, "This update asks for new permissions:" before the lines.

- [ ] **Step 1: Write the failing consent tests**

```cpp
TEST_CASE("consent names full control for gcode and config rewrites for moonraker_write",
          "[plugin][consent]") {
    auto lines = consent_lines({Permission::Gcode, Permission::MoonrakerWrite});
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("full control") != std::string::npos);
    CHECK(lines[1].find("config") != std::string::npos);
    CHECK(lines[1].find("rewrit") != std::string::npos);
}

TEST_CASE("an update's consent lists only the new permissions", "[plugin][consent]") {
    Manifest m;
    m.name = "Orca";
    m.version = "1.2.0";
    m.permissions = {Permission::Gcode, Permission::Http};
    std::string msg = consent_message(m, {Permission::Http});
    CHECK(msg.find("new permissions") != std::string::npos);
    CHECK(msg.find("internet") != std::string::npos);
    CHECK(msg.find("full control") == std::string::npos);
}

TEST_CASE("no permissions says so", "[plugin][consent]") {
    Manifest m;
    m.name = "Plain";
    m.version = "1";
    CHECK(consent_message(m, {}).find("no special permissions") != std::string::npos);
}
```

- [ ] **Step 2: Write the failing list tests**

`tests/unit/test_plugins_overlay.cpp` (fourth rig copy, `ListRig`): open the Plugins overlay against the fixture directory and check one row per `PluginInfo` whose description shows the status name, and the reason for Invalid rows (`bad-name` shows its validation error); enabling a disabled plugin through the overlay's handler with consent auto-confirmed (call the handler with a test hook that runs `on_yes` directly, e.g. an injectable `std::function` for `show_consent` on the overlay) leaves it Loaded; disabling unloads it; a Faulted plugin's row offers Re-enable (the `looper` fixture faults at load).

- [ ] **Step 3: Implement consent**

Pure functions as specified; `show_consent` wraps `helix::ui::modal_confirm(lv_tr("Enable plugin?"), msg, ModalSeverity::Warning when gcode or moonraker_write is requested else Info, lv_tr("Enable"), on_yes)`. Every string through `lv_tr`.

- [ ] **Step 4: Implement the Plugins overlay and the Settings row**

- `PluginsOverlay` (an `OverlayBase`, pattern `src/ui/ui_settings_safety.cpp`) lists `PluginHost::live()->plugins()` as `setting_action_row`s: label = manifest name (or directory name when the manifest failed), description = status name plus reason. Tapping a row: Disabled or NeedsApproval → consent (with `permission_growth` against the granted set for NeedsApproval) then `enable(id)`; Loaded → a small action choice through `modal_confirm` with Settings (`open_settings`) and Disable; Faulted or OverBudget → Re-enable (calls `enable`, which re-checks the budget); Invalid or Incompatible → nothing. After any change, rebuild the rows in place (`helix::ui::safe_clean_children` then repopulate, as `WidgetCatalogOverlay::refresh_gated_rows` does).
- `PluginHost` gains `PermissionSet granted(const std::string& id) const` (read from the enabled block, as `consider` does; refactor `consider` to call it so the rule lives once).
- `ui_xml/settings_panel.xml`: a `setting_action_row name="row_plugins" label="Plugins" label_tag="Plugins" icon="puzzle_outline" callback="on_plugins_clicked"` in the HELIXSCREEN group, hidden by `<bind_flag_if_eq subject="settings_plugins_available" flag="hidden" ref_value="0"/>`.
- `SettingsPanel::init_subjects` adds `UI_MANAGED_SUBJECT_INT(plugins_available_subject_, 0, "settings_plugins_available", subjects_)`. `SettingsPanel::on_plugins_clicked` is registered on every build; under `HELIX_HAS_PLUGINS` it shows `PluginsOverlay`, otherwise it does nothing. The subject is set to 1 from `Application::init_plugins` when a host is created. Until Phase 3 adds the Moonraker plugin folder, only `HELIX_PLUGIN_DIR` creates a host, so the row stays hidden for users.

- [ ] **Step 5: Run**

Run: `make t F='[consent]'`, `make t F='[plugins_overlay]'`, `make t F='[plugin]'`, `make`. Then `make translation-sync` and `make translations`.

Expected: PASS. Hand mutations: drop the config wording from the `moonraker_write` line (red); enable without consent in the Disabled branch (the list test's consent-hook count goes red).

- [ ] **Step 6: Drive it once**

```bash
TREE=$(basename "$(git rev-parse --show-toplevel)")
export HELIX_SOCK="/tmp/helix-$TREE.sock" HELIX_CONFIG_DIR="/tmp/helix-config-$TREE"
mkdir -p "$HELIX_CONFIG_DIR"
HELIX_PLUGIN_DIR=$PWD/tests/fixtures/plugins SDL_VIDEODRIVER=dummy \
  ./build/bin/helix-screen --test -vv --remote-socket "$HELIX_SOCK" > /tmp/helix-$TREE.log 2>&1 &
./build/bin/helix-screen ctl -s "$HELIX_SOCK" navigate settings
./build/bin/helix-screen ctl -s "$HELIX_SOCK" click row_plugins
./build/bin/helix-screen ctl -s "$HELIX_SOCK" ls
```

Expected: the Plugins overlay lists every fixture with its status. Stop the instance by the PID found from its socket (CLAUDE.md § Sharing: never `pkill`). Record the `ctl` output in the report; a human judges the look.

- [ ] **Step 7: Commit**

```bash
git add include/plugin_consent.h src/plugin/plugin_consent.cpp include/plugins_overlay.h src/plugin/plugins_overlay.cpp ui_xml/plugins_overlay.xml ui_xml/settings_panel.xml src/ui/ui_panel_settings.cpp include/plugin_host.h src/plugin/plugin_host.cpp src/application/application.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt tests/unit/test_plugin_consent.cpp tests/unit/test_plugins_overlay.cpp translations/*.yml ui_xml/translations/*.xml
git commit -m "feat(plugin): Settings > Plugins with consent before enabling"
git show --stat HEAD
```

---

### Task 11: Docs and the gates

**Files:**
- Modify: `docs/devel/architecture/12-system-services.md`, `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`, `docs/devel/PANEL_WIDGET_GUIDE.md`

- [ ] **Step 1: Docs**

- `12-system-services.md`, the plugin section: plugin widgets through runtime definitions and `LuaPanelWidget` (`src/plugin/lua_panel_widget.cpp#LuaPanelWidget::run`); overlays through `PluginOverlayHost`; unload order; why subjects retire instead of deinit (`src/plugin/lua_bind_ui.cpp#sweep_retired_subjects`); the XML policy (`src/plugin/plugin_xml_policy.cpp#check_plugin_xml`). Cite places as `path#symbol`, never line numbers.
- `PANEL_WIDGET_GUIDE.md`: one short paragraph on runtime definitions and that a layout keeps ids it does not know.
- The spec's `**Status:**` line: `Phases 1 and 2 implemented; Phases 3 to 5 not started.` and, under Unload, replace nothing else; add one sentence to Lifecycle § Unload: "Subjects the plugin registered are unregistered at step 4 and freed once no object observes them."

- [ ] **Step 2: The gates, once, in this order**

Run: `make full-test-run`
Expected: the unit sweep and the bats suite both pass. Read the log; do not pipe the run through `tail` or `grep`.

Run: `python3 scripts/check_esp32_app_srcs.py && make -n HELIX_HAS_PLUGINS=0 | grep -c "lib/lua/"`
Expected: exit 0 and a count of `0`.

Push the branch (never main), then:

Run: `scripts/zeus-run.sh mutate --tests '[plugin],[widget_registry],[widget_config],[setting_text_row]' --base <merge-base with origin/main>`
Expected: every survivor is equivalent (say why in the commit body) or gets a test. Whole new files show as uncompilable, which is expected.

Run LAST: `scripts/zeus-run.sh asan '[plugin],[widget_registry],[widget_config],[setting_text_row]'`, then `python3 scripts/check_asan_leaks.py --baseline scripts/asan_leak_baseline.txt <log>`
Expected: no ASan errors and no leak origin outside the baseline. The retired-subject list is process-lifetime and reachable, so it does not report.

- [ ] **Step 3: Commit and hand off**

```bash
git add docs/devel/architecture/12-system-services.md docs/devel/plans/2026-09-28-lua-plugin-system-design.md docs/devel/PANEL_WIDGET_GUIDE.md
git commit -m "docs(plugin): widgets, overlays, settings and consent for Lua plugins"
git show --stat HEAD
```

Then use superpowers:finishing-a-development-branch. Phase 3 (Moonraker plugin source, `helix.moonraker.subscribe`, hot reload of plugin files) and Phase 4 (the `led-effects` port and the author guide) get their own plans.
