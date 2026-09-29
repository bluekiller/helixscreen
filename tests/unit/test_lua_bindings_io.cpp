// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "lua_bindings.h"

#include <fstream>

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

TEST_CASE_METHOD(LVGLTestFixture, "http needs its permission and a web URL",
                 "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings});
    CHECK_FALSE(b.t.run(R"(helix.http.get("https://example.com"))"));

    BoundRuntime h({&install_io_bindings}, {Permission::Http});
    CHECK_FALSE(h.t.run(R"(helix.http.get("file:///etc/passwd"))"));
    CHECK_FALSE(h.t.run(R"(helix.http.get("https://x", { headers = { A = 1 } }))"));
    REQUIRE(h.t.run(
        R"(r = helix.http.post("https://example.com/x", { body = "hi", headers = { A = "b" } }))"));
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

TEST_CASE_METHOD(LVGLTestFixture, "an http body over the plugin memory cap is refused",
                 "[plugin][bindings][io]") {
    BoundRuntime h({&install_io_bindings}, {Permission::Http});
    REQUIRE(h.t.run(R"(r, e = helix.http.post("https://example.com/big"))"));
    REQUIRE(h.fake.requests.size() == 1);
    h.fake.requests[0].reply(
        RpcResult{true, json{{"status", 200}, {"body", std::string(4 << 20, 'x')}}, {}});
    helix::ui::UpdateQueue::instance().drain();
    CHECK(h.t.global("r") == "nil");
    CHECK(h.t.global("e").find("memory cap") != std::string::npos);
    CHECK_FALSE(h.t.rt->faulted());
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.http bounds in-flight requests per plugin",
                 "[plugin][bindings][io]") {
    BoundRuntime h({&install_io_bindings}, {Permission::Http});
    REQUIRE(h.t.run(R"(a = helix.http.get("https://example.com/1"))"));
    REQUIRE(h.t.run(R"(b = helix.http.get("https://example.com/2"))"));
    REQUIRE(h.fake.requests.size() == 2);
    REQUIRE(h.t.run(R"(ok, err = pcall(helix.http.get, "https://example.com/3"))"));
    CHECK(h.t.global("ok") == "false");
    CHECK(h.t.global("err").find("2 requests in flight") != std::string::npos);
    CHECK(h.fake.requests.size() == 2);

    h.fake.requests[0].reply(RpcResult{true, json{{"status", 200}, {"body", "x"}}, {}});
    helix::ui::UpdateQueue::instance().drain();
    REQUIRE(h.t.run(R"(c = helix.http.get("https://example.com/4"))"));
    CHECK(h.fake.requests.size() == 3);
}

TEST_CASE_METHOD(LVGLTestFixture, "http asks for one byte more than the remaining cap",
                 "[plugin][bindings][io]") {
    BoundRuntime h({&install_io_bindings}, {Permission::Http});
    REQUIRE(h.t.run(R"(r = helix.http.get("https://example.com/x"))"));
    REQUIRE(h.fake.requests.size() == 1);
    CHECK(h.fake.requests[0].cap == h.t.rt->memory_cap() - h.t.rt->memory_used() + 1);
    h.fake.requests[0].reply(RpcResult{true, json{{"status", 200}, {"body", "x"}}, {}});
    helix::ui::UpdateQueue::instance().drain();
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

TEST_CASE_METHOD(LVGLTestFixture, "a corrupt storage file reads as empty",
                 "[plugin][bindings][io]") {
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

TEST_CASE_METHOD(LVGLTestFixture, "settings fall back to manifest defaults",
                 "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings}, {}, {step_decl()});
    REQUIRE(b.t.run(R"(v = helix.settings.get("step"))"));
    CHECK(b.t.global("v") == "5");
    CHECK_FALSE(b.t.run(R"(helix.settings.get("nope"))"));

    b.settings["step"] = "not a number"; // stale value from an older manifest
    REQUIRE(b.t.run(R"(v = helix.settings.get("step"))"));
    CHECK(b.t.global("v") == "5");
}

TEST_CASE_METHOD(LVGLTestFixture, "set_plugin_setting validates, saves and notifies",
                 "[plugin][bindings][io]") {
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

TEST_CASE_METHOD(LVGLTestFixture,
                 "a handler registered during on_change is not called for that change",
                 "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings}, {}, {step_decl()});
    REQUIRE(b.t.run(R"(
        log = ""
        helix.settings.on_change("step", function(v)
            log = log .. "a"
            helix.settings.on_change("step", function(w) log = log .. "b" end)
        end)
    )"));
    CHECK(set_plugin_setting(*b.ctx, "step", 9));
    CHECK(b.t.global("log") == "a");
    CHECK(set_plugin_setting(*b.ctx, "step", 10));
    // A runs for both changes (registering B each time); B first fires on the change
    // AFTER the one that registered it.
    CHECK(b.t.global("log") == "aab");
}

TEST_CASE_METHOD(LVGLTestFixture, "storage without a plugin storage path raises",
                 "[plugin][bindings][io]") {
    BoundRuntime b({&install_io_bindings}, {Permission::Storage});
    CHECK_FALSE(b.t.run(R"(helix.storage.get("x"))"));
    CHECK_FALSE(b.t.run(R"(helix.storage.set("x", 1))"));
}

#endif // HELIX_HAS_PLUGINS
