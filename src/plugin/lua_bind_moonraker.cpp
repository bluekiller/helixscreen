// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "json_utils.h"
#include "lua_bindings.h"

namespace helix::plugin {

void require_permission(lua_State* L, Permission p, const char* call) {
    if (!context(L).manifest.permissions.count(p))
        luaL_error(L, "%s needs the '%s' permission in manifest.json", call, permission_name(p));
}

RpcCallback make_resolver(LuaRuntime::Pending p, PushRpc on_ok) {
    return [p, on_ok = std::move(on_ok)](RpcResult r) {
        p.resolve([r = std::move(r), on_ok](lua_State* co) -> int {
            if (!r.ok) {
                lua_pushnil(co);
                lua_pushlstring(co, r.error.data(), r.error.size());
                return 2;
            }
            return on_ok(co, r);
        });
    };
}

int push_rpc_true(lua_State* co, const RpcResult&) {
    lua_pushboolean(co, 1);
    return 1;
}

int push_rpc_value(lua_State* co, const RpcResult& r) {
    push_json(co, r.value);
    return 1;
}

int push_rpc_capped_body(lua_State* co, const json& value, size_t body_bytes) {
    LuaRuntime& rt = LuaRuntime::from(co);
    size_t remaining = rt.memory_cap() > rt.memory_used() ? rt.memory_cap() - rt.memory_used() : 0;
    if (body_bytes > remaining) {
        lua_pushnil(co);
        lua_pushliteral(co, "response larger than the plugin memory cap");
        return 2;
    }
    push_json(co, value);
    return 1;
}

namespace {

// An empty Lua table converts to an array; Moonraker wants an object for params.
json params_arg(lua_State* L, int index) {
    if (lua_isnoneornil(L, index))
        return json::object();
    json p = to_json(L, index);
    if (p.is_array() && p.empty())
        return json::object();
    return p;
}

// A download body lands whole in the plugin's Lua state, so it goes through the memory-cap
// check shared with the http bindings.
int push_rpc_download_body(lua_State* co, const RpcResult& r) {
    size_t bytes = r.value.is_string() ? r.value.get_ref<const std::string&>().size() : 0;
    return push_rpc_capped_body(co, r.value, bytes);
}

int gcode(lua_State* L) {
    require_permission(L, Permission::Gcode, "helix.gcode");
    std::string script = luaL_checkstring(L, 1);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, script](LuaRuntime::Pending p) {
        backend->gcode(script, make_resolver(p, &push_rpc_true));
    });
}

int call(lua_State* L) {
    std::string method = luaL_checkstring(L, 1);
    if (!is_readonly_moonraker_method(method))
        require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.call");
    json params = params_arg(L, 2);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, method, params](LuaRuntime::Pending p) {
        backend->call(method, params, make_resolver(p, &push_rpc_value));
    });
}

int query(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    json objects = json::object();
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "helix.moonraker.query: object names must be strings");
        std::string name = lua_tostring(L, -2);
        objects[name] = lua_isboolean(L, -1) ? json(nullptr) : to_json(L, -1);
        lua_pop(L, 1);
    }
    json params{{"objects", objects}};
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, params](LuaRuntime::Pending p) {
        backend->call("printer.objects.query", params, make_resolver(p, &push_rpc_value));
    });
}

int upload(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.upload");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 3, &len);
    std::string content(data, len);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(
        L, [backend, root, path, content](LuaRuntime::Pending p) {
            backend->upload(root, path, content, make_resolver(p, &push_rpc_true));
        });
}

int download(lua_State* L) {
    require_permission(L, Permission::MoonrakerWrite, "helix.moonraker.download");
    std::string root = luaL_checkstring(L, 1);
    std::string path = luaL_checkstring(L, 2);
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, root, path](LuaRuntime::Pending p) {
        backend->download(root, path, make_resolver(p, &push_rpc_download_body));
    });
}

int on_agent_event(lua_State* L) {
    std::string event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    int ref = rt.ref_value(L, 2);
    LuaRuntime* rtp = &rt;
    LifetimeToken token = rt.token();
    // Runs on the WebSocket thread: inspects JSON only, then defers to the main thread.
    auto off = context(L).backend.on_notify(
        "notify_agent_event", [rtp, token, ref, event](const json& msg) {
            auto params = msg.find("params");
            if (params == msg.end() || !params->is_array() || params->empty() ||
                !(*params)[0].is_object())
                return;
            const json& p0 = (*params)[0];
            if (helix::json_util::safe_string(p0, "event", "") != event)
                return;
            std::string agent = helix::json_util::safe_string(p0, "agent", "");
            json data = p0.contains("data") ? p0["data"] : json();
            token.defer("plugin_agent_event", [rtp, ref, agent, data]() {
                rtp->invoke(ref, [agent, data](lua_State* co) {
                    lua_pushlstring(co, agent.data(), agent.size());
                    push_json(co, data);
                    return 2;
                });
            });
        });
    rt.on_close([off]() { off(); });
    return 0;
}

} // namespace

void install_moonraker_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    static const luaL_Reg fns[] = {{"call", &call},
                                   {"query", &query},
                                   {"upload", &upload},
                                   {"download", &download},
                                   {"on_agent_event", &on_agent_event},
                                   {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_pushcfunction(L, &gcode);
    lua_setfield(L, -2, "gcode");
    lua_newtable(L);
    luaL_setfuncs(L, fns, 0);
    lua_setfield(L, -2, "moonraker");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
