// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "lua_runtime.h"
#include "plugin_backend.h"
#include "plugin_manifest.h"

#include <functional>
#include <string>

namespace helix::plugin {

/// What every binding of one plugin may reach. The owner keeps it alive until after the
/// runtime is destroyed, because runtime closers may read it.
struct PluginContext {
    LuaRuntime& rt;
    PluginBackend& backend;
    const Manifest& manifest;
    json* settings; ///< this plugin's /plugins/settings/<id> object
    std::function<void()> save_settings;
    std::string storage_path; ///< <dir of settings.json>/plugin-data/<id>.json
};

using Installer = void (*)(PluginContext&);

/// helix.log, helix.json, helix.timer, helix.sleep. Must run first: it registers the
/// context that context() returns.
void install_core_bindings(PluginContext& ctx);
void install_ui_bindings(PluginContext& ctx);
void install_printer_bindings(PluginContext& ctx);
void install_moonraker_bindings(PluginContext& ctx);
void install_io_bindings(PluginContext& ctx);

PluginContext& context(lua_State* L);

/// Objects and arrays become tables; null becomes nil.
void push_json(lua_State* L, const json& j);

/// Raises a Lua error for functions, userdata, cycles, non-string object keys and nesting
/// deeper than 32. An empty table converts to an empty array.
json to_json(lua_State* L, int index);

} // namespace helix::plugin
