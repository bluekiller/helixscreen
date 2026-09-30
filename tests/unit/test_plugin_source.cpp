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
#include <thread>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using helix::plugin::test::read_text;
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
    PluginSource src(remote.deps(), cache.path.string());

    SyncResult r = run_sync(src);

    CHECK(r.changed == std::vector<std::string>{"spark"});
    CHECK(read_text(cache.path / "spark" / "main.lua") == "x = 1");
    CHECK_FALSE(fs::exists(cache.path / ".staging" / "spark"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an unchanged plugin is not downloaded again",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["spark/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path.string());
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
    remote.files["aa/manifest.json"] = {"{}", 100.0};
    remote.files["bb/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path.string());
    run_sync(src);
    remote.downloads.clear();

    remote.files["bb/manifest.json"] = {"{\"v\":2}", 200.0};
    SyncResult r = run_sync(src);

    CHECK(r.changed == std::vector<std::string>{"bb"});
    for (const auto& d : remote.downloads)
        CHECK(d.rfind("bb/", 0) == 0);
    CHECK(read_text(cache.path / "bb" / "manifest.json") == "{\"v\":2}");
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin gone from the source is deleted from the cache",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["gone/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path.string());
    run_sync(src);
    REQUIRE(fs::exists(cache.path / "gone"));

    remote.files.clear(); // the printer has no plugins folder any more
    SyncResult r = run_sync(src);

    CHECK(r.removed == std::vector<std::string>{"gone"});
    CHECK_FALSE(fs::exists(cache.path / "gone"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a failed listing deletes nothing", "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["keep/manifest.json"] = {"{}", 100.0};
    PluginSource src(remote.deps(), cache.path.string());
    run_sync(src);

    remote.list_ok = false;
    SyncResult r = run_sync(src);

    CHECK(r.removed.empty());
    CHECK(r.changed.empty());
    CHECK(fs::exists(cache.path / "keep" / "manifest.json"));
}

TEST_CASE_METHOD(LVGLTestFixture, "a download that fails keeps the previous version",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["pp/manifest.json"] = {"{\"v\":1}", 100.0};
    remote.files["pp/main.lua"] = {"v = 1", 100.0};
    PluginSource src(remote.deps(), cache.path.string());
    run_sync(src);

    remote.files["pp/manifest.json"] = {"{\"v\":2}", 200.0};
    remote.files["pp/main.lua"] = {"v = 2", 200.0};
    remote.fail_download.insert("pp/main.lua");
    SyncResult r = run_sync(src);

    CHECK(r.failed == std::vector<std::string>{"pp"});
    CHECK(r.changed.empty());
    CHECK(read_text(cache.path / "pp" / "manifest.json") == "{\"v\":1}");
    CHECK(read_text(cache.path / "pp" / "main.lua") == "v = 1");

    remote.fail_download.clear(); // the next connect retries
    SyncResult r2 = run_sync(src);
    CHECK(r2.changed == std::vector<std::string>{"pp"});
    CHECK(read_text(cache.path / "pp" / "main.lua") == "v = 2");
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

TEST_CASE_METHOD(LVGLTestFixture, "a plugin over a limit is rejected whole", "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["big/manifest.json"] = {"{}", 1.0};
    remote.files["big/blob.bin"] = {std::string(kMaxBytesPerFile + 1, 'x'), 1.0};
    remote.files["ok/manifest.json"] = {"{}", 1.0};
    PluginSource src(remote.deps(), cache.path.string());

    SyncResult r = run_sync(src);

    CHECK(r.rejected == std::vector<std::string>{"big"});
    CHECK(r.changed == std::vector<std::string>{"ok"});
    CHECK_FALSE(fs::exists(cache.path / "big"));
    for (const auto& d : remote.downloads)
        CHECK(d.rfind("big/", 0) != 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "a sync requested during a sync runs once after it",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["pp/manifest.json"] = {"{}", 1.0};
    int dones = 0;
    PluginSource src(remote.deps(), cache.path.string());
    src.sync([&](const SyncResult&) { ++dones; });
    src.sync([&](const SyncResult&) { ++dones; });
    src.sync([&](const SyncResult&) { ++dones; });
    helix::ui::UpdateQueue::instance().drain();
    helix::ui::UpdateQueue::instance().drain();
    CHECK(dones == 3);                   // every caller hears back
    CHECK(remote.downloads.size() == 1); // but the queued syncs found nothing new
}

TEST_CASE_METHOD(LVGLTestFixture, "leftover .old and .staging dirs are cleaned on the next sync",
                 "[plugin][source]") {
    TempDir cache;
    FakeRemote remote;
    remote.files["pp/manifest.json"] = {"{}", 1.0};
    PluginSource src(remote.deps(), cache.path.string());
    fs::create_directories(cache.path / ".old-x");
    std::ofstream(cache.path / ".old-x" / "manifest.json", std::ios::binary) << "{}";
    fs::create_directories(cache.path / ".staging" / "y");

    SyncResult r = run_sync(src);

    CHECK_FALSE(fs::exists(cache.path / ".old-x"));
    CHECK_FALSE(fs::exists(cache.path / ".staging" / "y"));
    CHECK(r.changed == std::vector<std::string>{"pp"});
    CHECK(fs::exists(cache.path / ".index.json"));
}

namespace {

/// Answers every dep from a worker thread, like the production deps completing on the
/// WebSocket and transfer threads. Each call blocks until its worker has queued the
/// continuation, so the test stays deterministic while the hop runs off the main thread.
struct WorkerRemote : FakeRemote {
    SourceDeps deps() {
        SourceDeps d;
        d.list = [this](std::function<void(bool, std::vector<RemoteFile>)> cb) {
            std::vector<RemoteFile> out;
            for (const auto& [p, v] : files)
                out.push_back({p, v.first.size(), v.second});
            const bool ok = list_ok;
            std::thread worker([cb, ok, out] { cb(ok, ok ? out : std::vector<RemoteFile>{}); });
            worker.join();
        };
        d.download = [this](const std::string& p, const std::string& dest,
                            std::function<void(bool, std::string)> cb) {
            downloads.push_back(p); // the dep call itself runs on the main thread
            const bool fail = fail_download.count(p) != 0;
            const std::string content = fail ? std::string{} : files.at(p).first;
            std::thread worker([cb, fail, content, dest] {
                if (fail) {
                    cb(false, "connection reset");
                    return;
                }
                fs::create_directories(fs::path(dest).parent_path());
                std::ofstream(dest, std::ios::binary) << content;
                cb(true, {});
            });
            worker.join();
        };
        return d;
    }
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "deps completing on worker threads sync and queue",
                 "[plugin][source]") {
    TempDir cache;
    WorkerRemote remote;
    remote.files["pp/manifest.json"] = {"{}", 1.0};
    int dones = 0;
    PluginSource src(remote.deps(), cache.path.string());
    src.sync([&](const SyncResult&) { ++dones; });
    src.sync([&](const SyncResult&) { ++dones; }); // queued: the first is still running
    src.sync([&](const SyncResult&) { ++dones; }); // queued: coalesced into the same run
    for (int i = 0; i < 6 && dones < 3; ++i)
        helix::ui::UpdateQueue::instance().drain();
    CHECK(dones == 3); // every caller hears back
    CHECK_FALSE(src.syncing());
    CHECK(remote.downloads.size() == 1); // the queued run found nothing new
    CHECK(read_text(cache.path / "pp" / "manifest.json") == "{}");
}

#endif // HELIX_HAS_PLUGINS
