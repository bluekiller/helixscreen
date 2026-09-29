// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_bindings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kIoStateKey = 0;
constexpr size_t kMaxStorageBytes = 256 * 1024;
constexpr size_t kMaxSettingString = 1024;

struct IoState {
    std::optional<json> storage; ///< loaded on first use
    std::unordered_map<std::string, std::vector<int>> on_change;
};

IoState& io_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kIoStateKey);
    auto* s = static_cast<IoState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

bool is_web_url(const std::string& u) {
    return u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0;
}

// An HTTP response body lands whole in the plugin's Lua state, so it goes through the
// memory-cap check shared with the moonraker download binding.
int push_rpc_http_response(lua_State* co, const RpcResult& r) {
    size_t bytes = 0;
    if (r.value.is_object()) {
        auto it = r.value.find("body");
        if (it != r.value.end() && it->is_string())
            bytes = it->get_ref<const std::string&>().size();
    }
    return push_rpc_capped_body(co, r.value, bytes);
}

int http_request(lua_State* L, const char* method, const char* call_name) {
    require_permission(L, Permission::Http, call_name);
    std::string url = luaL_checkstring(L, 1);
    luaL_argcheck(L, is_web_url(url), 1, "only http:// and https:// URLs");
    std::string body;
    json headers = json::object();
    lua_Integer timeout = 10000;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "body");
        if (!lua_isnil(L, -1)) {
            size_t n = 0;
            const char* b = luaL_checklstring(L, -1, &n);
            body.assign(b, n);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "headers");
        if (!lua_isnil(L, -1)) {
            headers = to_json(L, -1);
            if (headers.is_array() && headers.empty())
                headers = json::object();
            if (!headers.is_object())
                luaL_error(L, "%s: headers must be a table of strings", call_name);
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                if (!it.value().is_string())
                    luaL_error(L, "%s: header '%s' must be a string", call_name, it.key().c_str());
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "timeout_ms");
        if (!lua_isnil(L, -1))
            timeout = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    uint32_t timeout_ms = static_cast<uint32_t>(std::clamp<lua_Integer>(timeout, 1, 60000));
    std::string m = method;
    PluginBackend* backend = &context(L).backend;
    return LuaRuntime::from(L).await_async(L, [backend, m, url, body, headers,
                                               timeout_ms](LuaRuntime::Pending p) {
        backend->http(m, url, body, headers, timeout_ms, make_resolver(p, &push_rpc_http_response));
    });
}

int http_get(lua_State* L) {
    return http_request(L, "GET", "helix.http.get");
}

int http_post(lua_State* L) {
    return http_request(L, "POST", "helix.http.post");
}

json& storage_of(lua_State* L) {
    auto& st = io_state(L);
    if (!st.storage) {
        st.storage = json::object();
        std::ifstream in(context(L).storage_path, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            json j = json::parse(ss.str(), nullptr, false);
            if (j.is_object())
                st.storage = std::move(j);
            else
                spdlog::warn("[plugin {}] storage file is not a JSON object; starting empty",
                             LuaRuntime::from(L).plugin_id());
        }
    }
    return *st.storage;
}

int storage_get(lua_State* L) {
    require_permission(L, Permission::Storage, "helix.storage.get");
    std::string key = luaL_checkstring(L, 1);
    const json& s = storage_of(L);
    auto it = s.find(key);
    if (it == s.end())
        lua_pushnil(L);
    else
        push_json(L, *it);
    return 1;
}

int storage_set(lua_State* L) {
    require_permission(L, Permission::Storage, "helix.storage.set");
    std::string key = luaL_checkstring(L, 1);
    json value = to_json(L, 2);
    json next = storage_of(L);
    if (value.is_null())
        next.erase(key);
    else
        next[key] = std::move(value);
    std::string text = next.dump();
    if (text.size() > kMaxStorageBytes)
        return luaL_error(L, "helix.storage.set: storage would exceed 256 KB");

    const std::string& path = context(L).storage_path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out)
            return luaL_error(L, "helix.storage.set: cannot write %s", tmp.c_str());
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        return luaL_error(L, "helix.storage.set: cannot replace %s", path.c_str());
    storage_of(L) = std::move(next);
    return 0;
}

const SettingDecl* find_decl(const Manifest& m, const std::string& key) {
    for (const auto& d : m.settings) {
        if (d.key == key)
            return &d;
    }
    return nullptr;
}

bool fits(const SettingDecl& d, const json& v) {
    switch (d.type) {
    case SettingType::Bool:
        return v.is_boolean();
    case SettingType::Int:
        return v.is_number_integer() && v.get<double>() >= d.min && v.get<double>() <= d.max;
    case SettingType::Float:
        return v.is_number() && v.get<double>() >= d.min && v.get<double>() <= d.max;
    case SettingType::Enum:
        return v.is_string() && std::find(d.options.begin(), d.options.end(),
                                          v.get<std::string>()) != d.options.end();
    case SettingType::String:
        return v.is_string() && v.get_ref<const std::string&>().size() <= kMaxSettingString;
    case SettingType::Action:
    case SettingType::Info:
        return false;
    }
    return false;
}

int settings_get(lua_State* L) {
    auto& ctx = context(L);
    std::string key = luaL_checkstring(L, 1);
    const SettingDecl* d = find_decl(ctx.manifest, key);
    if (!d)
        return luaL_error(L, "helix.settings.get: '%s' is not declared in manifest.json",
                          key.c_str());
    auto it = ctx.settings->find(key);
    push_json(L, it != ctx.settings->end() && fits(*d, *it) ? *it : d->default_value);
    return 1;
}

int settings_on_change(lua_State* L) {
    auto& ctx = context(L);
    std::string key = luaL_checkstring(L, 1);
    if (!find_decl(ctx.manifest, key))
        return luaL_error(L, "helix.settings.on_change: '%s' is not declared in manifest.json",
                          key.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    io_state(L).on_change[key].push_back(ctx.rt.ref_value(L, 2));
    return 0;
}

} // namespace

std::string plugin_storage_path(const std::string& settings_path, const std::string& id) {
    return (std::filesystem::path(settings_path).parent_path() / "plugin-data" / (id + ".json"))
        .string();
}

bool set_plugin_setting(PluginContext& ctx, const std::string& key, const json& value) {
    const SettingDecl* d = find_decl(ctx.manifest, key);
    if (!d || !fits(*d, value))
        return false;
    (*ctx.settings)[key] = value;
    ctx.save_settings();
    auto& handlers = io_state(ctx.rt.state()).on_change;
    if (auto it = handlers.find(key); it != handlers.end()) {
        for (int ref : it->second) {
            ctx.rt.invoke(ref, [value](lua_State* co) {
                push_json(co, value);
                return 1;
            });
        }
    }
    return true;
}

void install_io_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new IoState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kIoStateKey);
    ctx.rt.on_close([state] { delete state; });

    static const luaL_Reg http_fns[] = {
        {"get", &http_get}, {"post", &http_post}, {nullptr, nullptr}};
    static const luaL_Reg storage_fns[] = {
        {"get", &storage_get}, {"set", &storage_set}, {nullptr, nullptr}};
    static const luaL_Reg settings_fns[] = {
        {"get", &settings_get}, {"on_change", &settings_on_change}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    for (auto [name, fns] : {std::pair{"http", http_fns}, std::pair{"storage", storage_fns},
                             std::pair{"settings", settings_fns}}) {
        lua_newtable(L);
        luaL_setfuncs(L, fns, 0);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
