// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "../test_helpers/plugin_test_support.h"
#include "lua_runtime.h"

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
    CHECK(require_candidates("/p", "a.b") ==
          std::vector<std::string>{"/p/a/b.lua", "/p/lib/a/b.lua"});
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

TEST_CASE("source larger than the memory cap is refused", "[plugin][lua_runtime]") {
    LuaRuntime::Limits limits;
    limits.memory_bytes = 256 * 1024;
    TestRuntime t(limits);
    std::string ones;
    ones.reserve(1000000);
    for (int i = 0; i < 500000; ++i)
        ones += "1,";
    std::string chunk = "local t = {" + ones + "}";
    CHECK(chunk.size() > limits.memory_bytes);
    CHECK_FALSE(t.run(chunk));
    CHECK(t.rt->faulted());
    CHECK(t.fault.find("memory") != std::string::npos);
    CHECK(t.rt->memory_used() <= 256 * 1024);
}

TEST_CASE("run_file refuses a missing file and a directory", "[plugin][lua_runtime]") {
    TestRuntime t;
    CHECK_FALSE(t.rt->run_file("no-such-file.lua"));
    CHECK_FALSE(t.rt->run_file("lib"));
    CHECK_FALSE(t.rt->faulted());
}

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
        lua_pushinteger(
            L, duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        return 1;
    });
    lua_setglobal(t.rt->state(), "now_ms");
    const char* spin = "local t0 = now_ms() while now_ms() - t0 < 20 do end";
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK(t.run(spin));
    CHECK_FALSE(t.rt->faulted());
}

#endif // HELIX_HAS_PLUGINS
