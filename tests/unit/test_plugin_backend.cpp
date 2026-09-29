// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "app_globals.h"
#include "http_executor.h"
#include "plugin_backend.h"

#include <chrono>
#include <future>
#include <vector>

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
    b.download("gcodes", "a.gcode", 1024, sink);
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
    b.download("../etc", "x", 1024, [&](RpcResult r) { down = std::move(r); });
    CHECK_FALSE(up.ok);
    CHECK(up.error.find("root") != std::string::npos);
    CHECK_FALSE(down.ok);
}

TEST_CASE("url_host extracts the host of a web URL", "[plugin][backend]") {
    CHECK(url_host("http://127.0.0.1:7125/x") == "127.0.0.1");
    CHECK(url_host("https://[::1]/") == "::1");
    CHECK(url_host("http://Example.com:80/a?b") == "Example.com");
    CHECK(url_host("http://u:p@h/") == "h");
    CHECK(url_host("not a url").empty());
}

TEST_CASE("is_forbidden_http_target refuses loopback and the printer host", "[plugin][backend]") {
    const std::vector<std::string> printer{"192.168.1.50"};
    CHECK(is_forbidden_http_target({"127.0.0.1"}, printer));
    CHECK(is_forbidden_http_target({"127.5.5.5"}, printer));
    CHECK(is_forbidden_http_target({"::1"}, printer));
    CHECK(is_forbidden_http_target({"::ffff:127.0.0.1"}, printer));
    CHECK(is_forbidden_http_target({"192.168.1.50"}, printer));
    CHECK_FALSE(is_forbidden_http_target({"93.184.216.34"}, printer));
}

TEST_CASE("app backend http refuses the printer's own host", "[plugin][backend]") {
    helix::http::HttpExecutor::slow().start();
    auto b = make_app_backend();
    std::promise<RpcResult> done;
    auto ready = done.get_future();
    b.http("GET", "http://127.0.0.1:7125/x", "", json::object(), 5000, 1024,
           [&done](RpcResult r) { done.set_value(std::move(r)); });
    REQUIRE(ready.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    RpcResult r = ready.get();
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("printer host") != std::string::npos);
    helix::http::HttpExecutor::slow().stop();
}

#endif // HELIX_HAS_PLUGINS
