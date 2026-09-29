// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "lua_bindings.h"

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

TEST_CASE_METHOD(LVGLTestFixture, "timers and sleeps die with the runtime",
                 "[plugin][bindings][core]") {
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
    CHECK(b.t.run(
        R"(helix.log.debug("d") helix.log.info("i") helix.log.warn("w") helix.log.error("e"))"));
    CHECK_FALSE(b.t.run(R"(helix.log.info())"));
}

#endif // HELIX_HAS_PLUGINS
