// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if HELIX_HAS_PLUGINS

#include "lua_runtime.h"
#include "plugin_backend.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

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

    bool run(const std::string& code) {
        return rt->run_string(code, "test");
    }

    /// A global's value as Lua's tostring() prints it.
    std::string global(const char* name) {
        lua_State* L = rt->state();
        lua_getglobal(L, name);
        std::string s = luaL_tolstring(L, -1, nullptr);
        lua_pop(L, 2);
        return s;
    }
};

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
        b.upload = [this](const std::string& r, const std::string& p, const std::string& c,
                          RpcCallback cb) {
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

} // namespace helix::plugin::test

#endif // HELIX_HAS_PLUGINS
