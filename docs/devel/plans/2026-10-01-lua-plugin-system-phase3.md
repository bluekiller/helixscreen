# Lua Plugin System, Phase 3 (Moonraker Source) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Plugins install by dropping a folder into the printer's `config/helixscreen/plugins/`: HelixScreen mirrors that folder over Moonraker into a local cache, loads enabled plugins from the cache at boot (offline too), re-syncs on every connect and on file changes, and lets plugins subscribe to Klipper objects through the app's one union subscription.

**Architecture:** `PluginSource` diffs a Moonraker `server.files.list` of the `config` root against a per-printer cache index, downloads what changed into a staging directory and swaps each plugin in atomically, then tells `PluginHost` which plugin ids changed or went away; `PluginHost::rescan` reloads only those. The WebSocket layer gains a subscription-extras provider: `MoonrakerDiscoverySequence` merges plugin objects into the app's `printer.objects.subscribe` union, and a refresh re-sends the union when plugins change their set. `HELIX_PLUGIN_DIR` stays the author's local source, with a polling hot-reload watcher.

**Tech Stack:** C++17, Lua 5.4.9 (compiled as C++), LVGL 9.5 + helix-xml, Catch2, the project Makefile, Moonraker JSON-RPC.

**Spec:** `docs/devel/plans/2026-09-28-lua-plugin-system-design.md`, Phase 3 ("Moonraker source") plus the Hot reload section. Read both the spec's Lifecycle section and this plan's Rulings before any task.

## Rulings against the spec

These were decided while planning; each task below assumes them.

1. **Cache location.** The spec says `~/helixscreen/plugin-cache/<id>/`. A Moonraker web update `rmtree()`s the install directory, and on AD5M, K1 and K2 the persistent storage is elsewhere. The cache lives at `get_helix_cache_dir("plugins") + "/<printer_id>/<id>/"` (`include/app_globals.h#get_helix_cache_dir`), which already picks persistent, non-tmpfs storage per platform. Per printer, because a printer switch must not show another printer's plugins.
2. **Hot reload.** The spec says `XmlHotReloader` watches the cache. `XmlHotReloader` re-registers a changed XML file directly (`include/xml_hot_reloader.h#XmlHotReloader`), which would skip the plugin XML policy (`src/plugin/plugin_xml_policy.cpp#check_plugin_xml`), the "register exactly the checked bytes" rule, and the component scope bookkeeping in `PluginHost::Loaded::components`. So any change to a plugin's files reloads the whole plugin through `PluginHost::rescan`. With a Moonraker source the trigger is `notify_filelist_changed`; with `HELIX_PLUGIN_DIR` it is a polling watcher (Task 5).
3. **Initial values for `helix.moonraker.subscribe`.** The union subscribe response goes to the app's status parsers (`IMoonrakerClient::dispatch_status_update`), not to plugins. A plugin's subscription gets its first values from one `printer.objects.query` for its own objects, then live deltas from `notify_status_update`.
4. **A plugin never breaks the app's subscription.** If a subscribe carrying plugin objects returns an error, the sequence re-sends the app's own objects alone and drops the extras until the plugin set changes again.

## Global Constraints

- Plugin id `^[a-z][a-z0-9-]{1,31}$`. Every name a plugin registers is `<id>__<rest>` (double underscore), built and checked only through `plugin_owned_name()` / `is_owned_name()` in `include/plugin_manifest.h`.
- Distribution root on the printer: Moonraker root `config`, path prefix `helixscreen/plugins/<id>/` (`printer_data/config/helixscreen/plugins/<id>/`).
- `HELIX_PLUGIN_DIR=<path>` makes a local directory the source instead of Moonraker, for authors. No Moonraker sync runs while it is set.
- ESP32 keeps `HELIX_HAS_PLUGINS=0`. Every `src/plugin/*.cpp` and every new plugin test file is wrapped in `#if HELIX_HAS_PLUGINS` / `#endif // HELIX_HAS_PLUGINS`. Every new `src/plugin/*.cpp` gets a line in `firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt` in the commit that creates it (`python3 scripts/check_esp32_app_srcs.py` exits 0).
- Shared (non-plugin) code, including everything under `src/api/`, is compiled into the ESP32 `-fno-exceptions` image: no `json::at`, `j.value()`, one-argument `json::parse`, or `std::stoi` there (`helix::json_util::safe_*`, `json::parse(s, nullptr, false)`).
- Consumers depend on `IMoonrakerClient` / `IMoonrakerAPI` and the sub-API interfaces only, never the concrete classes (lint-gated in `tests/shell/test_code_lint.bats`). New client methods go on the interface; `tests/unit/test_interface_drift_*.cpp` (`[compile][drift]`) must pass.
- Threading (`.claude/rules/threading.md`): Moonraker and transfer callbacks run on background threads. Parse into plain values there, then `token.defer(tag, fn)` to the main thread; never touch LVGL, `PluginHost` or a Lua state off the main thread.
- New files begin `// Copyright (C) 2025-2026 356C LLC` then `// SPDX-License-Identifier: GPL-3.0-or-later`.
- spdlog only; `#include "hv/json.hpp"`; no RTTI; no `std::regex`; comments describe the code as it is now, with no history; no em-dashes.
- New user-facing strings go through `lv_tr()` / `label_tag`, then `make translation-sync` and `make translations`; the generated `ui_xml/translations/*.xml` are committed in the same commit (lesson L064).
- Verification inside a task is `make t F='[tag]'` for the tags the task touches, plus `make` when app code changed, plus `bats` on any bats file the task edits. `make full-test-run`, `scripts/zeus-run.sh mutate` and `scripts/zeus-run.sh asan` run once, in Task 8, with ASAN last. Each task proves its tests can fail with a named hand mutation in the commit body.
- Commits use explicit paths; never `git add -A`. `git show --stat HEAD` after every commit.

## Review Focus

- **A sync that dies mid-download** (Wi-Fi drop, Moonraker restart) leaves the previously cached version of that plugin intact and loaded, reports nothing changed for it, and retries on the next connect. Pinned in Task 2.
- **A printer with no `helixscreen/plugins` folder** (almost every user) produces an empty source and no errors; a listing that *fails* (disconnected) deletes nothing from the cache. Pinned in Task 2.
- **A plugin update on the printer that adds a permission** while the plugin is loaded unloads it, marks it NeedsApproval, removes its home tiles, and asks for consent on re-enable. Pinned in Tasks 1 and 4.
- **A plugin subscribing to an object Klipper does not have, or a subscribe error while plugin objects are in the union,** never costs the app its own subscription. Pinned in Task 6.
- **An editor saving a plugin writes several files in a burst** (each fires `notify_filelist_changed`); the app runs one sync, not one per event. Pinned in Task 4.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/plugin_host.h`, `src/plugin/plugin_host.cpp` | `rescan(ids)`: add, reload or remove specific plugins; `read_plugin_info` shared with `load_from` |
| `include/plugin_source.h`, `src/plugin/plugin_source.cpp` | Diff the Moonraker listing against the cache index, stage downloads, atomic per-plugin swap. Deps injected |
| `include/plugin_source_app.h`, `src/plugin/plugin_source_app.cpp` | Production deps for `PluginSource` over `IFilesAPI` / `ITransfersAPI`; the filelist-change predicate; the debounced sync driver |
| `include/plugin_dir_watcher.h`, `src/plugin/plugin_dir_watcher.cpp` | `HELIX_PLUGIN_DIR` hot reload: per-plugin signature poll on an `lv_timer` |
| `include/moonraker_subscription_merge.h`, `src/api/moonraker_subscription_merge.cpp` | Pure union of the app's objects map and plugin extras. Shared code |
| `include/i_moonraker_client.h`, `include/moonraker_client.h`, `src/api/moonraker_client.cpp`, `include/moonraker_discovery_sequence.h`, `src/api/moonraker_discovery_sequence.cpp` | Extras provider, merged subscribe, `refresh_subscription()`, error fallback |
| `include/plugin_backend.h`, `src/plugin/plugin_backend_app.cpp` | `set_plugin_objects(id, objects)`; the app's per-plugin object registry and coalesced refresh |
| `src/plugin/lua_bind_moonraker.cpp` | `helix.moonraker.subscribe(objects, fn)` |
| `src/application/application.cpp`, `include/application.h` | Host always on, cache per printer, boot from cache, sync on discovery, printer switch, Settings row visibility |
| `src/api/moonraker_api_mock.cpp`, `include/moonraker_api_mock.h` | `HELIX_MOCK_PLUGINS_DIR`: serve a local directory as `config/helixscreen/plugins/` |
| `tests/unit/test_plugin_host_rescan.cpp`, `test_plugin_source.cpp`, `test_plugin_source_app.cpp`, `test_plugin_dir_watcher.cpp`, `test_moonraker_subscription_merge.cpp`, `test_moonraker_subscription_extras.cpp`, `test_lua_bindings_subscribe.cpp`, `test_plugin_phase3_wiring.cpp` | One test file per unit |
| `docs/devel/architecture/12-system-services.md`, `docs/devel/ENVIRONMENT_VARIABLES.md`, `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md`, the spec | Docs (Task 8) |

---

### Task 1: `PluginHost::rescan` reloads, adds or removes specific plugins

**Files:**
- Modify: `include/plugin_host.h`, `src/plugin/plugin_host.cpp`
- Test: `tests/unit/test_plugin_host_rescan.cpp` (`[plugin][host][rescan]`)

**Interfaces:**
- Consumes: `PluginHost::load_from`, `consider`, `unload`, `PluginInfo`, `parse_manifest`, the Phase 2 test rig in `tests/test_helpers/plugin_host_test_support.h` (`HostRig`, `enabled()`, `drain()`).
- Produces:
  - `void PluginHost::rescan(const std::vector<std::string>& ids);` Main thread. For each id: if `dir_/<id>/manifest.json` exists, unload it if loaded, re-read its `PluginInfo`, insert or replace it in `plugins_` (kept sorted by `dir_name`), and `consider` it; if it no longer exists, unload it and erase it from `plugins_`. Plugins not named are untouched (same runtime, same `gen`). One `notify_widget_defs_changed()` at the end, through the existing `bulk_` / `widget_defs_dirty_` pair.
  - `static PluginInfo read_plugin_info(const std::string& dir, const std::string& name);` (file-local in the .cpp is fine) used by both `load_from` and `rescan`: parses `manifest.json`, sets Invalid with the joined errors, or Invalid for a directory/id mismatch.
  - `const std::string& PluginHost::dir() const { return dir_; }`

- [ ] **Step 1: Write the failing tests**

Model the fixture on `tests/unit/test_plugin_host.cpp` (reuse `HostRig`, `enabled`, `drain` from `plugin_host_test_support.h`; do not copy them). Each case builds its own plugin tree in a `TempDir` (from `plugin_test_support.h`) by copying fixture plugins, so it can edit files freely.

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_helpers/plugin_host_test_support.h"
#include "../test_helpers/plugin_test_support.h"
#include "plugin_host.h"

#include <filesystem>
#include <fstream>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;
namespace fs = std::filesystem;

namespace {

void copy_fixture(const fs::path& to, const std::string& name) {
    fs::copy(fs::path("tests/fixtures/plugins") / name, to / name, fs::copy_options::recursive);
}

void write(const fs::path& p, const std::string& s) {
    std::ofstream(p, std::ios::trunc) << s;
}

} // namespace

TEST_CASE_METHOD(HostRig, "rescan reloads only the named plugin", "[plugin][host][rescan]") {
    TempDir tmp;
    copy_fixture(tmp.path(), "hello");
    copy_fixture(tmp.path(), "widget-demo");
    enabled({"hello", "widget-demo"});
    host().load_from(tmp.path().string());
    LuaRuntime* hello_before = host().runtime("hello");
    LuaRuntime* demo_before = host().runtime("widget-demo");
    REQUIRE(hello_before != nullptr);
    REQUIRE(demo_before != nullptr);

    write(tmp.path() / "hello" / "main.lua", "reloaded = true\n");
    host().rescan({"hello"});
    drain();

    CHECK(host().runtime("widget-demo") == demo_before);
    REQUIRE(host().runtime("hello") != nullptr);
    CHECK(TestRuntime::global_of(*host().runtime("hello"), "reloaded") == "true");
}

TEST_CASE_METHOD(HostRig, "rescan adds a new plugin and removes a deleted one",
                 "[plugin][host][rescan]") {
    TempDir tmp;
    copy_fixture(tmp.path(), "hello");
    enabled({"hello", "widget-demo"});
    host().load_from(tmp.path().string());
    REQUIRE(host().plugins().size() == 1);

    copy_fixture(tmp.path(), "widget-demo");
    fs::remove_all(tmp.path() / "hello");
    host().rescan({"hello", "widget-demo"});
    drain();

    REQUIRE(host().plugins().size() == 1);
    CHECK(host().plugins()[0].dir_name == "widget-demo");
    CHECK(host().runtime("hello") == nullptr);
    CHECK(host().runtime("widget-demo") != nullptr);
}

TEST_CASE_METHOD(HostRig, "rescan of an update that grows permissions needs approval",
                 "[plugin][host][rescan]") {
    TempDir tmp;
    copy_fixture(tmp.path(), "hello");
    enabled({"hello"});
    host().load_from(tmp.path().string());
    REQUIRE(host().runtime("hello") != nullptr);

    // Same plugin, now asking for gcode, which was never granted.
    auto mpath = tmp.path() / "hello" / "manifest.json";
    json m = json::parse(read_text(mpath), nullptr, false);
    REQUIRE(m.is_object());
    m["permissions"] = json::array({"gcode"});
    write(mpath, m.dump());
    host().rescan({"hello"});
    drain();

    CHECK(host().runtime("hello") == nullptr);
    CHECK(host().plugins()[0].status == PluginStatus::NeedsApproval);
}

TEST_CASE_METHOD(HostRig, "rescan of an invalid manifest keeps the entry with its error",
                 "[plugin][host][rescan]") {
    TempDir tmp;
    copy_fixture(tmp.path(), "hello");
    enabled({"hello"});
    host().load_from(tmp.path().string());
    write(tmp.path() / "hello" / "manifest.json", "{ not json");
    host().rescan({"hello"});
    drain();
    REQUIRE(host().plugins().size() == 1);
    CHECK(host().plugins()[0].status == PluginStatus::Invalid);
    CHECK(host().runtime("hello") == nullptr);
}

#endif // HELIX_HAS_PLUGINS
```

`TestRuntime::global_of`, `read_text` and `enabled({...})` may not exist with those exact names: read `plugin_test_support.h` / `plugin_host_test_support.h` first and use what is there (a runtime global reader, a file reader, a helper that writes the `/plugins/enabled` block). If a needed helper is missing, add it to the shared header rather than to this file. `hello`'s manifest must be enable-able with no permissions; if it declares any, grant them in `enabled`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make t F='[rescan]' > /tmp/p3-t1.log 2>&1; echo exit=$?`
Expected: compile failure, `no member named 'rescan' in 'PluginHost'`.

- [ ] **Step 3: Implement**

In `src/plugin/plugin_host.cpp`, move the manifest-reading body out of `load_from`'s first loop into:

```cpp
namespace {

PluginInfo read_plugin_info(const std::string& dir, const std::string& name) {
    PluginInfo info;
    info.dir_name = name;
    auto parsed = parse_manifest(read_file(std::filesystem::path(dir) / name / "manifest.json"));
    if (!parsed.manifest) {
        info.status = PluginStatus::Invalid;
        info.reason = join_errors(parsed.errors, "; ");
    } else if (parsed.manifest->id != name) {
        info.status = PluginStatus::Invalid;
        info.reason = "directory name must match id '" + parsed.manifest->id + "'";
    } else {
        info.manifest = std::move(parsed.manifest);
    }
    return info;
}

} // namespace
```

`load_from` calls it per name. Then:

```cpp
void PluginHost::rescan(const std::vector<std::string>& ids) {
    bulk_ = true;
    for (const auto& id : ids) {
        if (loaded_.count(id))
            unload(id);
        auto it = std::find_if(plugins_.begin(), plugins_.end(),
                               [&](const PluginInfo& p) { return p.dir_name == id; });
        std::error_code ec;
        bool present =
            std::filesystem::exists(std::filesystem::path(dir_) / id / "manifest.json", ec);
        if (!present) {
            if (it != plugins_.end())
                plugins_.erase(it);
            spdlog::info("[PluginHost] {}: removed", id);
            continue;
        }
        PluginInfo info = read_plugin_info(dir_, id);
        if (it != plugins_.end()) {
            *it = std::move(info);
        } else {
            auto pos = std::lower_bound(
                plugins_.begin(), plugins_.end(), id,
                [](const PluginInfo& p, const std::string& n) { return p.dir_name < n; });
            it = plugins_.insert(pos, std::move(info));
        }
        if (it->manifest && it->status == PluginStatus::Disabled)
            consider(*it);
        spdlog::info("[PluginHost] {}: {}{}", it->dir_name, plugin_status_name(it->status),
                     it->reason.empty() ? "" : " (" + it->reason + ")");
    }
    bulk_ = false;
    if (widget_defs_dirty_) {
        PanelWidgetManager::instance().notify_widget_defs_changed();
        widget_defs_dirty_ = false;
    }
}
```

Validate each id before touching the filesystem: an id that fails `is_valid_plugin_id` (the Phase 1 id check in `plugin_manifest.h`; use whatever it is named) is skipped with a warning, so a hostile listing cannot make `rescan` reach outside `dir_`. Add a fifth test case for that: `host().rescan({"../escape"})` leaves `plugins()` unchanged.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make t F='[rescan]' > /tmp/p3-t1.log 2>&1; echo exit=$?`, then `make t F='[host]'` and `make t F='[plugin]'` (the `load_from` refactor must not change existing behavior).
Expected: PASS.

- [ ] **Step 5: Hand mutations**

(a) In `rescan`, skip the `unload(id)` call: "rescan reloads only the named plugin" goes red (`reloaded` never set, or a double registration error). (b) Delete the `plugins_.erase(it)` line: "adds a new plugin and removes a deleted one" goes red on `size() == 1`. Restore both.

- [ ] **Step 6: Commit**

```bash
git add tests/unit/test_plugin_host_rescan.cpp
git commit -m "feat(plugin): PluginHost::rescan reloads, adds or removes named plugins" -m "rescan(ids) unloads, re-reads and reconsiders only the named plugins, adds new ones in sorted order and drops removed ones; load_from shares read_plugin_info. mutation: skipped unload(id) in rescan; the reload case went red. mutation: dropped plugins_.erase; the add/remove case went red." -- include/plugin_host.h src/plugin/plugin_host.cpp tests/unit/test_plugin_host_rescan.cpp tests/test_helpers/plugin_host_test_support.h tests/test_helpers/plugin_test_support.h
git show --stat HEAD
```

---

### Task 2: `PluginSource` mirrors the Moonraker plugin root into a per-printer cache

**Files:**
- Create: `include/plugin_source.h`, `src/plugin/plugin_source.cpp` (add `src/plugin/plugin_source.cpp` to `app_srcs_excluded.txt`)
- Test: `tests/unit/test_plugin_source.cpp` (`[plugin][source]`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces (all main thread; the deps may complete on any thread, and `PluginSource` hops back with its own `AsyncLifetimeGuard`):

```cpp
namespace helix::plugin {

/// One file under config/helixscreen/plugins/ as Moonraker lists it.
struct RemoteFile {
    std::string path;    ///< relative to the plugin root: "<id>/main.lua", "<id>/ui/x.xml"
    uint64_t size = 0;
    double modified = 0.0;
};

struct SourceDeps {
    /// Lists every file under config/helixscreen/plugins/. `ok` false means the listing
    /// failed (disconnected, Moonraker error): nothing may be deleted on a failed listing.
    /// An absent folder is ok with an empty vector.
    std::function<void(std::function<void(bool ok, std::vector<RemoteFile>)>)> list;
    /// Downloads one file (path relative to the plugin root) to `dest`.
    std::function<void(const std::string& path, const std::string& dest,
                       std::function<void(bool ok, std::string error)>)>
        download;
};

struct SyncResult {
    std::vector<std::string> changed;  ///< ids whose cache content changed (new or updated)
    std::vector<std::string> removed;  ///< ids whose source is gone; their cache dir is deleted
    std::vector<std::string> failed;   ///< ids left at their previous cached version
    std::vector<std::string> rejected; ///< ids refused by a limit, with the reason logged
};

/// Per-plugin limits. A plugin over any of them is skipped whole (reported as rejected);
/// its previous cached version, if any, stays.
constexpr size_t kMaxFilesPerPlugin = 128;
constexpr uint64_t kMaxBytesPerPlugin = 8ull << 20;
constexpr uint64_t kMaxBytesPerFile = 4ull << 20;
constexpr size_t kMaxPlugins = 32;

class PluginSource {
  public:
    /// `cache_dir` is the per-printer cache root; it and its `.index.json` are created on
    /// the first sync.
    PluginSource(SourceDeps deps, std::string cache_dir);
    ~PluginSource();

    /// Lists, diffs against the cache index, downloads what changed into
    /// `<cache>/.staging/<id>/`, swaps each fully downloaded plugin into place, deletes
    /// plugins whose source is gone, and calls `done` once on the main thread. A second
    /// sync requested while one runs is queued and runs once after it (never in parallel).
    void sync(std::function<void(const SyncResult&)> done);

    bool syncing() const;
    const std::string& cache_dir() const;
};

/// Splits "<id>/<rest>" and checks both halves: the id matches the plugin id pattern and
/// the rest is a relative path with no "..", no leading "/", no backslash, no empty
/// segment and no NUL. Returns false for anything else. Pure.
bool split_plugin_file(const std::string& path, std::string& id, std::string& rest);

} // namespace helix::plugin
```

Index format (`<cache>/.index.json`): `{"<id>": {"<rest>": [size, modified], ...}, ...}`. A plugin is "changed" when its file set differs from the index in any path, size or modified time.

- [ ] **Step 1: Write the failing tests**

A fake deps pair records every call and answers from a map, so each case controls the remote tree and failures exactly:

```cpp
// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "plugin_source.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using helix::plugin::test::TempDir;
namespace fs = std::filesystem;

namespace {

struct FakeRemote {
    std::map<std::string, std::pair<std::string, double>> files; ///< path -> (content, mtime)
    bool list_ok = true;
    std::set<std::string> fail_download;
    std::vector<std::string> downloads;

    SourceDeps deps() {
        SourceDeps d;
        d.list = [this](std::function<void(bool, std::vector<RemoteFile>)> cb) {
            std::vector<RemoteFile> out;
            for (const auto& [p, v] : files)
                out.push_back({p, v.first.size(), v.second});
            cb(list_ok, list_ok ? out : std::vector<RemoteFile>{});
        };
        d.download = [this](const std::string& p, const std::string& dest,
                            std::function<void(bool, std::string)> cb) {
            downloads.push_back(p);
            if (fail_download.count(p)) {
                cb(false, "connection reset");
                return;
            }
            fs::create_directories(fs::path(dest).parent_path());
            std::ofstream(dest, std::ios::binary) << files.at(p).first;
            cb(true, {});
        };
        return d;
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

SyncResult run_sync(PluginSource& src) {
    SyncResult out;
    bool done = false;
    src.sync([&](const SyncResult& r) {
        out = r;
        done = true;
    });
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(done);
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "sync downloads a new plugin into the cache",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["spark/manifest.json"] = {"{\"id\":\"spark\"}", 100.0};
    remote.files["spark/main.lua"] = {"x = 1", 100.0};
    PluginSource src(remote.deps(), cache.path().string());

    SyncResult r = run_sync(src);

    CHECK(r.changed == std::vector<std::string>{"spark"});
    CHECK(slurp(cache.path() / "spark" / "main.lua") == "x = 1");
    CHECK_FALSE(fs::exists(cache.path() / ".staging" / "spark"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an unchanged plugin is not downloaded again",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["spark/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path().string());
    run_sync(src);
    remote.downloads.clear();

    SyncResult r = run_sync(src);

    CHECK(r.changed.empty());
    CHECK(remote.downloads.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "a changed file re-downloads only its plugin",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["a/manifest.json"] = {"{}", 100.0};
    remote.files["b/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path().string());
    run_sync(src);
    remote.downloads.clear();

    remote.files["b/manifest.json"] = {"{\"v\":2}", 200.0};
    SyncResult r = run_sync(src);

    CHECK(r.changed == std::vector<std::string>{"b"});
    for (const auto& d : remote.downloads)
        CHECK(d.rfind("b/", 0) == 0);
    CHECK(slurp(cache.path() / "b" / "manifest.json") == "{\"v\":2}");
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin gone from the source is deleted from the cache",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["gone/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path().string());
    run_sync(src);
    REQUIRE(fs::exists(cache.path() / "gone"));

    remote.files.clear(); // the printer has no plugins folder any more
    SyncResult r = run_sync(src);

    CHECK(r.removed == std::vector<std::string>{"gone"});
    CHECK_FALSE(fs::exists(cache.path() / "gone"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a failed listing deletes nothing", "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["keep/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path().string());
    run_sync(src);

    remote.list_ok = false;
    SyncResult r = run_sync(src);

    CHECK(r.removed.empty());
    CHECK(r.changed.empty());
    CHECK(fs::exists(cache.path() / "keep" / "manifest.json"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a download that fails keeps the previous version",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["p/manifest.json"] = {"{\"v\":1}", 100.0};
    remote.files["p/main.lua"] = {"v = 1", 100.0};
    PluginSource src(remote.deps(), cache.path().string());
    run_sync(src);

    remote.files["p/manifest.json"] = {"{\"v\":2}", 200.0};
    remote.files["p/main.lua"] = {"v = 2", 200.0};
    remote.fail_download.insert("p/main.lua");
    SyncResult r = run_sync(src);

    CHECK(r.failed == std::vector<std::string>{"p"});
    CHECK(r.changed.empty());
    CHECK(slurp(cache.path() / "p" / "manifest.json") == "{\"v\":1}");
    CHECK(slurp(cache.path() / "p" / "main.lua") == "v = 1");

    remote.fail_download.clear(); // the next connect retries
    SyncResult r2 = run_sync(src);
    CHECK(r2.changed == std::vector<std::string>{"p"});
    CHECK(slurp(cache.path() / "p" / "main.lua") == "v = 2");
}

TEST_CASE("split_plugin_file refuses anything outside the plugin", "[plugin][source]") {
    std::string id, rest;
    CHECK(split_plugin_file("spark/ui/spark__tile.xml", id, rest));
    CHECK(id == "spark");
    CHECK(rest == "ui/spark__tile.xml");
    CHECK_FALSE(split_plugin_file("spark/../other/main.lua", id, rest));
    CHECK_FALSE(split_plugin_file("/etc/passwd", id, rest));
    CHECK_FALSE(split_plugin_file("spark", id, rest));
    CHECK_FALSE(split_plugin_file("Spark/main.lua", id, rest));
    CHECK_FALSE(split_plugin_file("spark//main.lua", id, rest));
    CHECK_FALSE(split_plugin_file("spark\\main.lua", id, rest));
    CHECK_FALSE(split_plugin_file("bad_id/main.lua", id, rest));
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin over a limit is rejected whole",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["big/manifest.json"] = {"{}", 1.0};
    remote.files["big/blob.bin"] = {std::string(kMaxBytesPerFile + 1, 'x'), 1.0};
    remote.files["ok/manifest.json"] = {"{}", 1.0};
    PluginSource src(remote.deps(), cache.path().string());

    SyncResult r = run_sync(src);

    CHECK(r.rejected == std::vector<std::string>{"big"});
    CHECK(r.changed == std::vector<std::string>{"ok"});
    CHECK_FALSE(fs::exists(cache.path() / "big"));
    for (const auto& d : remote.downloads)
        CHECK(d.rfind("big/", 0) != 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a sync requested during a sync runs once after it",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["p/manifest.json"] = {"{}", 1.0};
    int dones = 0;
    PluginSource src(remote.deps(), cache.path().string());
    src.sync([&](const SyncResult&) { ++dones; });
    src.sync([&](const SyncResult&) { ++dones; });
    src.sync([&](const SyncResult&) { ++dones; });
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(dones == 3); // every caller hears back
    CHECK(remote.downloads.size() == 1); // but the queued syncs found nothing new
}

#endif // HELIX_HAS_PLUGINS
```

The fake completes synchronously; the production deps complete on worker threads. `PluginSource` must behave the same either way, so it always hops through its guard's `defer` before touching its state: the test's `drain()` is what makes that visible.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make t F='[source]' > /tmp/p3-t2.log 2>&1; echo exit=$?`
Expected: compile failure, `plugin_source.h` not found.

- [ ] **Step 3: Implement `split_plugin_file`**

```cpp
bool split_plugin_file(const std::string& path, std::string& id, std::string& rest) {
    auto slash = path.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= path.size())
        return false;
    id = path.substr(0, slash);
    rest = path.substr(slash + 1);
    if (!is_valid_plugin_id(id)) // the Phase 1 id check in plugin_manifest.h
        return false;
    if (rest.find('\\') != std::string::npos || rest.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= rest.size()) {
        auto end = rest.find('/', start);
        std::string seg = rest.substr(start, end == std::string::npos ? std::string::npos
                                                                      : end - start);
        if (seg.empty() || seg == "." || seg == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}
```

- [ ] **Step 4: Implement `PluginSource`**

The flow of one sync, all on the main thread except inside the deps:

1. `deps_.list` -> `guard_.defer`: if `!ok`, finish with an empty `SyncResult` (touch nothing).
2. Group the listing by id with `split_plugin_file` (skip and log a path that fails it). Drop ids beyond `kMaxPlugins` (sorted, keep the first N; the rest are `rejected`). For each id, check `kMaxFilesPerPlugin`, `kMaxBytesPerPlugin`, `kMaxBytesPerFile`; over any -> `rejected`, no download.
3. Load `.index.json` (`json::parse(text, nullptr, false)`; a missing or corrupt index means "everything changed"). For each remaining id, compare its `{rest: [size, modified]}` map with the index entry; equal -> skip.
4. For each changed id, download its files one at a time into `<cache>/.staging/<id>/<rest>` (`std::filesystem::create_directories` for the parent). Any failure -> `failed`, remove `.staging/<id>`, keep the old cache and its index entry.
5. When all of an id's files are in: swap with rename: `<cache>/<id>` -> `<cache>/.old-<id>` (if present), `.staging/<id>` -> `<cache>/<id>`, remove `.old-<id>`. Update the index entry. Append to `changed`.
6. Ids in the index (or with a cache directory) absent from a successful listing -> `remove_all(<cache>/<id>)`, drop from the index, append to `removed`.
7. Write the index atomically (write `.index.json.tmp`, rename). Call `done`, then start a queued sync if one was requested (all queued callers get the second run's result).

Keep downloads sequential: plugin trees are small, and one transfer at a time keeps the WebSocket and the file transfer executor free for the app.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make t F='[source]'` then `make`.
Expected: PASS.

- [ ] **Step 6: Hand mutations**

(a) Remove the `!ok` early return: "a failed listing deletes nothing" goes red. (b) Swap before checking all files downloaded (move step 5 into the per-file loop): "a download that fails keeps the previous version" goes red. Restore both.

- [ ] **Step 7: Commit**

```bash
git add include/plugin_source.h src/plugin/plugin_source.cpp tests/unit/test_plugin_source.cpp
git commit -m "feat(plugin): PluginSource mirrors the Moonraker plugin folder into a per-printer cache" -m "Lists config/helixscreen/plugins over Moonraker, diffs against a per-printer index, stages downloads and swaps each plugin in only when all its files arrived. A failed listing deletes nothing. mutation: removed the failed-listing early return; the delete-nothing case went red. mutation: swapped per file; the failed-download case went red." -- include/plugin_source.h src/plugin/plugin_source.cpp tests/unit/test_plugin_source.cpp firmware/helixscreen-esp32/components/helixapp/app_srcs_excluded.txt
git show --stat HEAD
```

---

### Task 3: Production source deps, the filelist predicate, and `HELIX_MOCK_PLUGINS_DIR`

**Files:**
- Create: `include/plugin_source_app.h`, `src/plugin/plugin_source_app.cpp` (excluded from ESP32)
- Modify: `include/moonraker_api_mock.h`, `src/api/moonraker_api_mock.cpp`
- Test: `tests/unit/test_plugin_source_app.cpp` (`[plugin][source_app]`)

**Interfaces:**
- Consumes: `SourceDeps`, `RemoteFile` (Task 2); `IFilesAPI::list_files`, `ITransfersAPI::download_file_to_path` (`include/i_moonraker_sub_apis.h`); `helix::json_util::notification_payload` (`include/json_utils.h`).
- Produces:

```cpp
namespace helix::plugin {

/// The plugin root inside Moonraker's config root.
constexpr const char* kPluginRootPath = "helixscreen/plugins/";

/// SourceDeps over the app's Moonraker API: `list` is one server.files.list of the config
/// root filtered to kPluginRootPath (Moonraker lists a root whole), `download` is
/// download_file_to_path. `api` must outlive the deps' callbacks; the caller guards them.
SourceDeps make_moonraker_source_deps(IMoonrakerAPI* api);

/// True when a notify_filelist_changed message touches config/helixscreen/plugins/: its
/// item, or a move/copy source_item, sits in the config root under kPluginRootPath. Pure.
bool is_plugin_filelist_change(const json& msg);

} // namespace helix::plugin
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("is_plugin_filelist_change matches only the plugin folder", "[plugin][source_app]") {
    auto msg = [](const std::string& root, const std::string& path) {
        return json{{"method", "notify_filelist_changed"},
                    {"params", json::array({{{"action", "create_file"},
                                             {"item", {{"root", root}, {"path", path}}}}})}};
    };
    CHECK(is_plugin_filelist_change(msg("config", "helixscreen/plugins/spark/main.lua")));
    CHECK(is_plugin_filelist_change(msg("config", "helixscreen/plugins/spark")));
    CHECK_FALSE(is_plugin_filelist_change(msg("config", "saved_variables.cfg")));
    CHECK_FALSE(is_plugin_filelist_change(msg("config", "helixscreen/settings.json")));
    CHECK_FALSE(is_plugin_filelist_change(msg("gcodes", "helixscreen/plugins/x/main.lua")));
    CHECK_FALSE(is_plugin_filelist_change(json::object()));

    json moved = {{"params", json::array({{{"action", "move_file"},
                                           {"item", {{"root", "config"}, {"path", "old/x.lua"}}},
                                           {"source_item",
                                            {{"root", "config"},
                                             {"path", "helixscreen/plugins/spark/x.lua"}}}}})}};
    CHECK(is_plugin_filelist_change(moved));
}
```

A second case drives `make_moonraker_source_deps` against `MoonrakerFileAPIMock` with injected config files (`set_config_files`, `include/moonraker_api_mock.h`) and checks that `list` returns only the `helixscreen/plugins/` entries with that prefix stripped (`spark/main.lua`, not `helixscreen/plugins/spark/main.lua`) and that `saved_variables.cfg` is absent. Build the mock API the way other tests do (grep `set_config_files(` under `tests/unit/` for a fixture that already does it and reuse its construction).

A third case covers `HELIX_MOCK_PLUGINS_DIR` (below): point it at `tests/fixtures/plugins` with `helix::test::ScopedEnv` (or the existing env guard in `tests/test_helpers/`), list through the mock, and check `widget-demo/manifest.json` is listed and downloads byte-identical to the fixture file.

- [ ] **Step 2: Run to verify they fail** (`make t F='[source_app]'`): missing header.

- [ ] **Step 3: Implement**

```cpp
bool is_plugin_filelist_change(const json& msg) {
    const json* payload = helix::json_util::notification_payload(msg);
    if (!payload)
        return false;
    auto in_root = [](const json& item) {
        if (!item.is_object())
            return false;
        std::string root = helix::json_util::safe_string(item, "root");
        std::string path = helix::json_util::safe_string(item, "path");
        std::string_view prefix(kPluginRootPath);
        std::string_view bare = prefix.substr(0, prefix.size() - 1); // the folder itself
        return root == "config" && (path.rfind(prefix, 0) == 0 || path == bare);
    };
    auto it = payload->find("item");
    auto src = payload->find("source_item");
    return (it != payload->end() && in_root(*it)) || (src != payload->end() && in_root(*src));
}
```

`make_moonraker_source_deps`: `list` calls `api->files().list_files("config", "", false, ...)` (check the accessor name on `IMoonrakerAPI` for the files sub-API), keeps entries whose `path` starts with `kPluginRootPath` and is not a directory, strips the prefix, maps `size`/`modified`; the error callback reports `ok == false`. `download` calls `api->transfers().download_file_to_path("config", std::string(kPluginRootPath) + path, dest, ...)`.

`HELIX_MOCK_PLUGINS_DIR` (mock only, `HELIX_ENABLE_MOCKS` builds): in `MoonrakerFileAPIMock::list_files`, when `root == "config"` and the variable names a directory, append one `FileInfo` per regular file under it with `path = "helixscreen/plugins/" + relative`, real size and mtime (from `std::filesystem::last_write_time`, converted to seconds); in the transfers mock's `download_file_to_path`, serve `config/helixscreen/plugins/<rel>` from that directory (copy the file to `dest`). Other roots and paths fall through exactly as now. This is what lets a `--test` run exercise Phase 3 end to end with `examples/plugins/` or the fixtures.

- [ ] **Step 4: Run to verify they pass** (`make t F='[source_app]'`, `make`).

- [ ] **Step 5: Hand mutations**

(a) Drop the `source_item` half of the predicate: the move case goes red. (b) Skip the prefix strip in `list`: the mock listing case goes red. Restore.

- [ ] **Step 6: Commit** (explicit paths incl. `app_srcs_excluded.txt`; `git show --stat HEAD`).

---

### Task 4: The app runs a plugin host on every build that has plugins

**Files:**
- Modify: `src/application/application.cpp`, `include/application.h`, `src/plugin/plugin_source_app.cpp` (the sync driver), `include/plugin_source_app.h`
- Test: `tests/unit/test_plugin_phase3_wiring.cpp` (`[plugin][wiring]`)

**Interfaces:**
- Consumes: Tasks 1-3; `get_helix_cache_dir` (`include/app_globals.h`); `Config::get_active_printer_id` (`include/config.h`); `Application::init_plugins`, `setup_discovery_callbacks`, the printer-switch path (`application.cpp`, "8. Reload plugins"), both shutdown paths that `unload_all()` + reset the host.
- Produces:

```cpp
namespace helix::plugin {

/// get_helix_cache_dir("plugins") + "/<printer_id>" ("default" when the id is empty).
std::string plugin_cache_dir_for(const std::string& printer_id);

/// Owns a PluginSource and the host it feeds. sync_now() runs one sync and rescans the
/// changed and removed ids; request_sync() coalesces a burst of requests into one sync
/// `debounce_ms` after the last one.
class PluginSyncDriver {
  public:
    PluginSyncDriver(PluginHost& host, SourceDeps deps, std::string cache_dir,
                     uint32_t debounce_ms = 1500);
    ~PluginSyncDriver(); ///< cancels its timer; pending replies are dropped by the guard
    void sync_now();
    void request_sync();
    /// Called after every completed sync with its result (tests, and the toast below).
    std::function<void(const SyncResult&)> on_synced;
};

} // namespace helix::plugin
```

Behavior to wire in `application.cpp`:

1. `init_plugins()` always creates the host on `HELIX_HAS_PLUGINS` builds.
   - `HELIX_PLUGIN_DIR` set: `load_from(dir)` exactly as today, no driver (Task 5 adds the watcher).
   - Otherwise: `cache = plugin_cache_dir_for(m_config->get_active_printer_id())`, `load_from(cache)` (boot works offline from the last sync), and create `m_plugin_sync = std::make_unique<PluginSyncDriver>(*m_plugin_host, make_moonraker_source_deps(api), cache)`.
2. In `setup_discovery_callbacks`' `on_discovery_complete` main-thread body (runs on every connect and reconnect): `if (app->m_plugin_sync) app->m_plugin_sync->sync_now();`.
3. Register a method callback for `notify_filelist_changed` (handler name `"PluginSync"`, unregistered in shutdown before the driver dies): on the WebSocket thread it only evaluates `is_plugin_filelist_change(msg)`; on true it defers `request_sync()` through a lifetime token (the same pattern as `src/print/print_history_manager.cpp#PrintHistoryManager` `notify_filelist_changed`).
4. Printer switch: `init_plugins()` already runs at step 8; it must first reset `m_plugin_sync` then the host (driver before host: the driver holds a host reference), and build both for the new printer's cache.
5. Shutdown and restart paths: reset `m_plugin_sync` before `m_plugin_host->unload_all()`.
6. Settings > Plugins row: see "Questions for Preston" at the end. Default implemented here: the row shows when the host exists and `plugins()` is non-empty, re-evaluated after every `load_from` and every sync (a helper `update_plugins_row_visibility()` sets `settings_plugins_available`).
7. A new plugin id (in `changed`, not previously in `plugins()`) shows one toast: "New plugin available: <name>. Enable it in Settings > Plugins." (`ToastManager` usage per `ui_toast_manager.h`; translated string).

- [ ] **Step 1: Write the failing tests**

Unit-level cases in `test_plugin_phase3_wiring.cpp` drive `PluginSyncDriver` with the Task 2 `FakeRemote` pattern and a real `PluginHost` from `HostRig` pointed at the driver's cache dir:

- "a sync that adds a plugin reaches the host": remote has `widget-demo` files copied from the fixture (read them from `tests/fixtures/plugins/widget-demo/` into the fake map); `sync_now()`, drain; `host().plugins()` contains `widget-demo` (Disabled: nothing is enabled).
- "an update that grows permissions unloads the plugin and needs approval": enable `hello`, sync it in, confirm Loaded; change the remote manifest to add `gcode` with a later mtime; `sync_now()`; status NeedsApproval, `runtime("hello") == nullptr`.
- "a burst of change requests runs one sync": call `request_sync()` five times, advance time past the debounce (`process_lvgl` in steps, per `tests/CLAUDE.md`, or the fixture's `wait_until`), drain; `remote.list_calls == 1` (add a counter to the fake).
- "the driver dies before its timer fires": `request_sync()` then destroy the driver, then `process_lvgl(3000)`; no crash, no list call.
- "plugin_cache_dir_for keys the cache by printer": two ids give two different directories, both under `get_helix_cache_dir("plugins")` (set `HELIX_CACHE_DIR` to a `TempDir` with the env guard so the test does not touch real caches).

Wiring in `application.cpp` is verified by `make` plus the live check in Step 5.

- [ ] **Step 2: Run to verify they fail** (`make t F='[wiring]'`).

- [ ] **Step 3: Implement the driver and the wiring** as listed above. The debounce is an `lv_timer_t*` cancelled in the destructor with `lv_timer_cancel_safe` (`.claude/rules/threading.md` rule 5).

- [ ] **Step 4: Run to verify they pass** (`make t F='[wiring]'`, `make t F='[plugin]'`, `make`).

- [ ] **Step 5: Live check (numbers, not pixels)**

Pinned socket and config dir per CLAUDE.md, `SDL_VIDEODRIVER=dummy`, `HELIX_MOCK_PLUGINS_DIR=$PWD/tests/fixtures/plugins`, no `HELIX_PLUGIN_DIR`, `--test -vv`. Grep the log for `[PluginHost] widget-demo: Disabled` after the first discovery; `ctl navigate settings` then `ctl ls` shows the Plugins row visible; open it and `ctl text` a row. Record commands and output in the report. Kill only your instance, by PID from your socket.

- [ ] **Step 6: Hand mutations**: (a) remove the `sync_now()` call from `on_discovery_complete` is not unit-visible, so mutate inside the driver instead: drop the `host_.rescan(...)` call and see "a sync that adds a plugin reaches the host" go red; (b) replace the debounce with an immediate sync and see the burst case go red.

- [ ] **Step 7: Commit** (translations in the same commit if the toast string is new).

---

### Task 5: Hot reload for `HELIX_PLUGIN_DIR`

**Files:**
- Create: `include/plugin_dir_watcher.h`, `src/plugin/plugin_dir_watcher.cpp` (excluded from ESP32)
- Modify: `src/application/application.cpp` (create it in `init_plugins` when `HELIX_PLUGIN_DIR` is set and `RuntimeConfig::hot_reload_enabled()`)
- Test: `tests/unit/test_plugin_dir_watcher.cpp` (`[plugin][watcher]`)

**Interfaces:**

```cpp
namespace helix::plugin {

/// A cheap fingerprint of one plugin directory: file count, total size and the newest
/// mtime of every regular file under it. Pure over the filesystem.
struct DirSignature {
    size_t files = 0;
    uint64_t bytes = 0;
    int64_t newest_mtime_ns = 0;
    bool operator==(const DirSignature&) const = default; // C++20; write it out for C++17
};

std::map<std::string, DirSignature> scan_plugin_dir(const std::string& dir);

/// Polls `dir` every `interval_ms` on an lv_timer and calls host.rescan() with the ids
/// whose signature changed, appeared or disappeared since the last poll.
class PluginDirWatcher {
  public:
    PluginDirWatcher(PluginHost& host, std::string dir, uint32_t interval_ms = 1000);
    ~PluginDirWatcher();
    void poll_now(); ///< one poll, for tests
};

} // namespace helix::plugin
```

- [ ] **Step 1: Failing tests**: copy `hello` into a `TempDir`, `load_from`, construct the watcher, `poll_now()` (no change: the host runtime pointer stays the same); rewrite `hello/main.lua` with a later mtime (`std::filesystem::last_write_time(p, now + 2s)` so the test does not sleep); `poll_now()`; the runtime pointer changed and the new global is visible. Second case: create `widget-demo` in the dir; `poll_now()` adds it. Third: delete `hello`; it is removed. Fourth: construct, destroy, `process_lvgl(2000)`: no crash (timer cancelled).
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.** `scan_plugin_dir` uses `std::filesystem::recursive_directory_iterator` with an `error_code` (no throwing overloads), one level of ids (`directory_iterator` over `dir`), skipping names that fail the plugin id check.
- [ ] **Step 4: Run to verify they pass.**
- [ ] **Step 5: Hand mutation**: compare only `files` in the signature; the rewrite case (same file count) goes red.
- [ ] **Step 6: Commit.**

---

### Task 6: The WebSocket layer merges plugin objects into the union subscription

This is critical-path work: every status update in the app rides this subscription. Read `src/api/moonraker_discovery_sequence.cpp#complete_discovery_subscription` and `#build_subscription_objects` in full first, and `docs/devel/THREADING.md`.

**Files:**
- Create: `include/moonraker_subscription_merge.h`, `src/api/moonraker_subscription_merge.cpp` (shared code: ESP32-safe, not excluded)
- Modify: `include/i_moonraker_client.h`, `include/moonraker_client.h`, `src/api/moonraker_client.cpp`, `include/moonraker_discovery_sequence.h`, `src/api/moonraker_discovery_sequence.cpp`, the interface drift tests if they enumerate methods
- Test: `tests/unit/test_moonraker_subscription_merge.cpp` (`[moonraker][subscription]`), `tests/unit/test_moonraker_subscription_extras.cpp` (`[moonraker][subscription]`)

**Interfaces:**

```cpp
namespace helix {

/// Union of two printer.objects.subscribe objects maps. For an object in both, null (every
/// field) wins; otherwise the field lists are unioned, keeping `app`'s order then `extra`'s
/// new fields. An object only in `extra` is added as given. `app` is never narrowed. A
/// malformed `extra` entry (not null, not an array of strings) is ignored. Pure.
nlohmann::json merge_subscription_objects(const nlohmann::json& app, const nlohmann::json& extra);

} // namespace helix
```

On `IMoonrakerClient` (implemented by `MoonrakerClient`; the mock inherits it):

```cpp
/// Objects merged into every printer.objects.subscribe the discovery sequence sends. Called
/// on the thread that builds the subscription; must be cheap and thread-safe.
virtual void set_subscription_extras_provider(std::function<json()> provider) = 0;

/// Re-sends printer.objects.subscribe with the last app objects merged with the provider's
/// current extras. A no-op before the first subscription of a connection completes (that
/// subscription already includes the extras) and while disconnected.
virtual void refresh_subscription() = 0;
```

`MoonrakerDiscoverySequence` keeps `last_app_objects_` (the `build_subscription_objects` result of the current connection, under the existing `hardware_mutex_` or its own mutex) and a `subscribed_` flag for the connection; `complete_discovery_subscription` sends `merge_subscription_objects(app, extras)`. The refresh response's `result.status` goes through `client_.dispatch_status_update(...)`, so an app object that changed during the round trip is not lost. On an error response to a subscribe that carried extras: log a warning naming the extra objects, re-send the app objects alone, and ignore extras until the provider's result changes (compare the dumped JSON).

- [ ] **Step 1: Pure merge tests**

```cpp
TEST_CASE("merge keeps every app object and field", "[moonraker][subscription]") {
    json app = {{"extruder", json::array({"temperature", "target"})}, {"print_stats", nullptr}};
    json extra = {{"extruder", json::array({"power"})}, {"temperature_sensor chamber", nullptr}};
    json m = helix::merge_subscription_objects(app, extra);
    CHECK(m["extruder"] == json::array({"temperature", "target", "power"}));
    CHECK(m["print_stats"].is_null());
    CHECK(m.contains("print_stats"));
    CHECK(m["temperature_sensor chamber"].is_null());
    CHECK(m.size() == 3);
}

TEST_CASE("null in either side subscribes every field", "[moonraker][subscription]") {
    json a = {{"fan", json::array({"speed"})}};
    CHECK(helix::merge_subscription_objects(a, {{"fan", nullptr}})["fan"].is_null());
    CHECK(helix::merge_subscription_objects({{"fan", nullptr}}, {{"fan", json::array({"rpm"})}})["fan"].is_null());
}

TEST_CASE("empty or malformed extras leave the app map unchanged", "[moonraker][subscription]") {
    json app = {{"toolhead", json::array({"position"})}};
    CHECK(helix::merge_subscription_objects(app, json::object()) == app);
    CHECK(helix::merge_subscription_objects(app, json()) == app);
    CHECK(helix::merge_subscription_objects(app, {{"x", 5}}) == app);
    CHECK(helix::merge_subscription_objects(app, {{"y", json::array({1, 2})}}) == app);
}

TEST_CASE("duplicate fields are not repeated", "[moonraker][subscription]") {
    json m = helix::merge_subscription_objects({{"e", json::array({"t"})}},
                                               {{"e", json::array({"t", "t", "p"})}});
    CHECK(m["e"] == json::array({"t", "p"}));
}
```

- [ ] **Step 2: Client-level tests** (`test_moonraker_subscription_extras.cpp`). Use the real discovery sequence over the mock transport, the pattern in `tests/unit/test_accel_discovery_wiring.cpp` (`AccelDiscoveryClient` calls `MoonrakerClient::discover_printer` on a `MoonrakerClientMock`). Subclass it once more to record every `printer.objects.subscribe` params object (override the `send_jsonrpc` overload the sequence uses; forward to the base so the mock still answers). Cases:
  - "extras ride the first subscription": set a provider returning `{"temperature_sensor spark": null}` before discovery; the recorded subscribe contains it and every object `build_subscription_objects` produced (compare against a subscribe recorded with no provider: extras-less objects must be a subset, field lists included).
  - "refresh adds and removes plugin objects without dropping app objects": after discovery, change the provider result, `refresh_subscription()`; the second subscribe equals `merge(app, new)`; then provider returns `{}` and refresh again: the third subscribe equals the first no-provider subscribe exactly.
  - "refresh before the first subscription completes sends nothing".
  - "reconnect includes the extras": run discovery a second time (same client, as a reconnect does); its subscribe includes the current extras.
  - "a subscribe error with extras falls back to the app objects": make the mock answer the first subscribe that carries `bogus_object` with an error (a test-only hook on the recording subclass: return an error through the error callback instead of forwarding), then check the next recorded subscribe equals the app objects alone and that the provider's same result does not trigger another attempt.
  - "the refresh response reaches status callbacks": register a `register_notify_update` callback, refresh, drain; it saw a status dict containing an app object.
- [ ] **Step 3: Run to verify they fail.**
- [ ] **Step 4: Implement** `merge_subscription_objects` (json only, `is_array` / `is_string` / `is_null` checks, no `at`), the two interface methods, and the sequence changes. Keep every existing log line of `complete_discovery_subscription`; add one `spdlog::info("[Moonraker Client] Subscribing {} plugin object(s)", n)` when extras are non-empty.
- [ ] **Step 5: Run to verify they pass**: `make t F='[subscription]'`, `make t F='[moonraker]'`, `make t F='[compile][drift]'`, `make t F='[discovery]'`, `make`, `python3 scripts/check_esp32_app_srcs.py`.
- [ ] **Step 6: Hand mutations**: (a) in the merge, let `extra`'s field list replace `app`'s instead of unioning: "merge keeps every app object and field" goes red; (b) in the error path, re-send with extras: the fallback case goes red; (c) skip `dispatch_status_update` for the refresh response: the last client case goes red.
- [ ] **Step 7: Commit.**

---

### Task 7: `helix.moonraker.subscribe(objects, fn)`

**Files:**
- Modify: `include/plugin_backend.h`, `src/plugin/plugin_backend_app.cpp`, `src/plugin/lua_bind_moonraker.cpp`, `tests/test_helpers/plugin_test_support.h` (FakeBackend), `src/application/application.cpp` (install the provider)
- Test: `tests/unit/test_lua_bindings_subscribe.cpp` (`[plugin][lua][subscribe]`)

**Interfaces:**
- `PluginBackend` gains `std::function<void(const std::string& plugin_id, const json& objects)> set_plugin_objects;` (an empty object clears the plugin's set).
- `plugin_backend_app.cpp` keeps a process-wide `PluginObjectRegistry` (`std::mutex`, `std::map<std::string, json>`), exposes `json plugin_objects_union()` (merged with `merge_subscription_objects` across plugins, starting from `json::object()`), and after each `set_plugin_objects` schedules one `refresh_subscription()` on the next main-loop tick (a single pending flag; `helix::ui::queue_update`), so ten plugins subscribing at load cost one refresh. `Application::setup_discovery_callbacks` calls `client->set_subscription_extras_provider(&helix::plugin::plugin_objects_union)` (on non-plugin builds nothing sets a provider).
- Lua: `local h = helix.moonraker.subscribe({extruder = {"temperature"}, ["fan"] = true}, function(status) ... end)`. The table maps an object name to a list of field names or `true` (every field). Returns a handle with `:cancel()`. The callback receives a table `{object = {field = value, ...}, ...}` holding only this subscription's objects and, when it named fields, only those fields: first once with the initial values from one `printer.objects.query`, then on every `notify_status_update` delta that touches them.
- Limits (named constexprs in `lua_bind_moonraker.cpp`, errors name the limit like the Phase 1 ones): `kMaxSubscribedObjects = 16` per plugin across all its subscriptions, `kMaxFieldsPerObject = 32`, `kMaxSubscriptions = 8` per plugin. Object names: printable ASCII, at most 64 bytes, no control characters.
- The binding keeps the plugin's merged set in its `MoonrakerBindState` and calls `set_plugin_objects(id, set)` on every subscribe and cancel, and `set_plugin_objects(id, {})` from the runtime closer, so unload shrinks the union.
- One `on_notify("notify_status_update", ...)` per plugin (registered with the first subscription, unregistered in the closer), filtering on the WebSocket thread into plain JSON per subscription and deferring through the runtime's token, with the same memory-cap check `on_agent_event` uses.

- [ ] **Step 1: Failing tests** (BoundRuntime with `install_moonraker_bindings`; FakeBackend records `set_plugin_objects` calls in `std::vector<std::pair<std::string, json>> object_sets`):
  - "subscribe publishes the plugin's objects": run `h = helix.moonraker.subscribe({extruder={"temperature"}}, function(s) last = s end)`; the last `object_sets` entry is `{"test-plugin", {"extruder": ["temperature"]}}`.
  - "the first callback carries queried values": the binding issued a `call` request `printer.objects.query` with those objects; reply `{"status": {"extruder": {"temperature": 210.5, "target": 215}}}`; drain; `last.extruder.temperature == 210.5` and `last.extruder.target == nil` (filtered to the named fields).
  - "deltas reach only the matching subscription": two subscriptions (extruder, heater_bed); fire the fake's `notify_status_update` handler with `{"params": [{"heater_bed": {"temperature": 60}}, 12.3]}`; drain; only the bed callback ran.
  - "cancel shrinks the set; closing the runtime clears it": `h:cancel()` -> last set lacks `extruder`; destroy the runtime -> last entry is `{"test-plugin", {}}` and the fake's `notify_unregistered` went up.
  - "limits": 17 objects in one call raise an error naming the limit (one expected error per runtime, per the 3-errors rule); 9 subscriptions raise on the ninth; `{[string.rep("x", 65)] = true}` raises.
  - A registry-level case: two plugin ids set objects; `plugin_objects_union()` contains both; clearing one leaves the other.
- [ ] **Step 2: Run to verify they fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run to verify they pass** (`make t F='[subscribe]'`, `make t F='[plugin]'`, `make`).
- [ ] **Step 5: Hand mutations**: (a) skip the field filter: the "queried values" case goes red; (b) skip `set_plugin_objects(id, {})` in the closer: the cancel/close case goes red.
- [ ] **Step 6: Live check**: with `HELIX_MOCK_PLUGINS_DIR` (Task 3), a small fixture plugin under `tests/fixtures/plugins/subscribe-demo/` (a `main.lua` that subscribes to `extruder` `temperature` and writes it into a string subject shown by a tile) enabled via Settings > Plugins; `grep "plugin object" /tmp/helix-*.log` shows the refresh, and `ctl text` on the tile shows a changing temperature across two reads a few seconds apart.
- [ ] **Step 7: Commit.**

---

### Task 8: Docs and the gates

**Files:**
- Modify: `docs/devel/architecture/12-system-services.md` (plugin section: the source, the cache location and why it is per printer, rescan, the dev watcher, the subscription extras and their failure fallback), `docs/devel/ENVIRONMENT_VARIABLES.md` (`HELIX_PLUGIN_DIR` now also enables hot reload in native dev builds), `docs/devel/MOCK_ENVIRONMENT_VARIABLES.md` (`HELIX_MOCK_PLUGINS_DIR`), the spec (Status line: Phases 1 to 3 implemented; the `helix.moonraker.subscribe` row loses its "Phase 3" caveat; the Lifecycle "Boot" paragraph and the Hot reload section state the cache location and the reload rule from this plan's Rulings 1 and 2).

- [ ] **Step 1: Docs**, present tense, `path#symbol` citations, no em-dashes. Commit.
- [ ] **Step 2: Gates, once, in this order (controller-run):**
  1. `make full-test-run`, `python3 scripts/check_esp32_app_srcs.py`, `make -n HELIX_HAS_PLUGINS=0 | grep -c 'lib/lua/'` (expect 0).
  2. Push the branch, then `scripts/zeus-run.sh mutate --tests '[plugin],[subscription],[moonraker]' --base <merge-base with origin/main> --max-hunks 0`. Triage survivors (behavior-preserving vs real gap); close real gaps with tests.
  3. LAST: `scripts/zeus-run.sh asan '[plugin],[subscription],[moonraker]'`, then `python3 scripts/check_asan_leaks.py --baseline scripts/asan_leak_baseline.txt <log>`.

---

## Product decisions (Preston, 2026-09-30: all three defaults approved)

1. **Settings > Plugins on normal installs.** Default: the row shows only once at least one plugin exists in the printer's `helixscreen/plugins/` folder. Alternative: always show it, with an empty state that says where to drop plugins.
2. **Enable state and plugin settings with several printers.** Default: global (`/plugins` stays top-level, as in Phases 1-2): a plugin id enabled once is enabled on every printer that ships that id, gated by the same permission-growth check. Alternative: per printer (`df() + "plugins"`), so each printer asks for consent separately.
3. **New plugin toast.** Default: one toast when a sync finds a plugin id not seen before. Alternative: no toast; the user finds it in Settings.
