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
