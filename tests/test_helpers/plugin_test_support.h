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

} // namespace helix::plugin::test

#endif // HELIX_HAS_PLUGINS
