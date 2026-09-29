// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_runtime.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace helix::plugin {

namespace {

// Trims the base library. Runs as trusted code before any plugin code.
constexpr const char* kSandboxPrelude = R"(
local load, collect = load, collectgarbage
dofile, loadfile, string.dump = nil, nil, nil
_G.load = function(chunk, name, _, ...)
    if select('#', ...) > 0 then return load(chunk, name, "t", (...)) end
    return load(chunk, name, "t")
end
local allowed = { count = true, collect = true, step = true }
_G.collectgarbage = function(opt, ...)
    opt = opt or "collect"
    if not allowed[opt] then
        error("collectgarbage('" .. tostring(opt) .. "') is not available to plugins", 2)
    end
    return collect(opt, ...)
end
)";

bool file_exists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

const char kLoadedKey = 0; // its address keys the require cache in the registry

} // namespace

ErrorWindow::ErrorWindow(size_t threshold, std::chrono::seconds window)
    : threshold_(threshold), window_(window) {}

bool ErrorWindow::record(Clock::time_point now) {
    times_.push_back(now);
    while (!times_.empty() && now - times_.front() > window_)
        times_.pop_front();
    return times_.size() >= threshold_;
}

std::vector<std::string> require_candidates(const std::string& plugin_dir,
                                            const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.')
        return {};
    std::string rel;
    char prev = 0;
    for (char c : name) {
        bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (c == '.') {
            if (prev == '.')
                return {};
            rel += '/';
        } else if (word) {
            rel += c;
        } else {
            return {};
        }
        prev = c;
    }
    return {plugin_dir + "/" + rel + ".lua", plugin_dir + "/lib/" + rel + ".lua"};
}

LuaRuntime::Pending::Pending(LuaRuntime* rt, lua_State* co, LifetimeToken token)
    : rt_(rt), co_(co), token_(std::move(token)),
      done_(std::make_shared<std::atomic<bool>>(false)) {}

LuaRuntime::LuaRuntime(std::string plugin_id, std::string plugin_dir, Limits limits,
                       FaultHandler on_fault)
    : plugin_id_(std::move(plugin_id)), plugin_dir_(std::move(plugin_dir)), limits_(limits),
      on_fault_(std::move(on_fault)) {
    L_ = lua_newstate(&LuaRuntime::alloc, this);
    *static_cast<LuaRuntime**>(lua_getextraspace(L_)) = this;
    lua_atpanic(L_, [](lua_State* L) -> int {
        spdlog::critical("[plugin] unprotected Lua error: {}",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "(no message)");
        return 0; // Lua aborts when the panic handler returns
    });
    install_sandbox();
    lua_sethook(L_, &LuaRuntime::budget_hook, LUA_MASKCOUNT, kHookInterval);
}

LuaRuntime::~LuaRuntime() {
    guard_.invalidate();
    for (auto it = closers_.rbegin(); it != closers_.rend(); ++it)
        (*it)();
    closers_.clear();
    lua_close(L_);
}

LuaRuntime& LuaRuntime::from(lua_State* L) {
    return **static_cast<LuaRuntime**>(lua_getextraspace(L));
}

void* LuaRuntime::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* rt = static_cast<LuaRuntime*>(ud);
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        std::free(ptr);
        rt->used_ -= old;
        return nullptr;
    }
    // Only a protected entry may be refused. A refusal anywhere else reaches Lua's panic
    // handler, and the setup work around entries is small and bounded.
    if (nsize > old && rt->depth_ > 0 && rt->used_ - old + nsize > rt->limits_.memory_bytes)
        return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p)
        rt->used_ = rt->used_ - old + nsize;
    return p;
}

void LuaRuntime::install_sandbox() {
    static const luaL_Reg kLibs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const auto& lib : kLibs) {
        luaL_requiref(L_, lib.name, lib.func, 1);
        lua_pop(L_, 1);
    }
    if (luaL_dostring(L_, kSandboxPrelude) != LUA_OK) {
        spdlog::critical("[plugin] sandbox prelude failed: {}", lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
    lua_newtable(L_);
    lua_rawsetp(L_, LUA_REGISTRYINDEX, &kLoadedKey);
    lua_pushcfunction(L_, &LuaRuntime::lua_require);
    lua_setglobal(L_, "require");
    lua_newtable(L_);
    lua_setglobal(L_, "helix");
}

int LuaRuntime::lua_require(lua_State* L) {
    auto& rt = from(L);
    std::string name = luaL_checkstring(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kLoadedKey);
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    auto candidates = require_candidates(rt.plugin_dir_, name);
    if (candidates.empty())
        return luaL_error(L, "require('%s'): not a module name", name.c_str());
    for (const auto& path : candidates) {
        if (!file_exists(path))
            continue;
        if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK)
            return lua_error(L);
        lua_call(L, 0, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name.c_str());
        return 1;
    }
    return luaL_error(L, "require('%s'): module not found in the plugin", name.c_str());
}

bool LuaRuntime::run_string(const std::string& code, const std::string& chunk_name) {
    if (faulted_)
        return false;
    if (luaL_loadbufferx(L_, code.data(), code.size(), chunk_name.c_str(), "t") != LUA_OK) {
        std::string msg = lua_tostring(L_, -1);
        lua_pop(L_, 1);
        report_error(msg);
        return false;
    }
    return spawn({});
}

bool LuaRuntime::run_file(const std::string& relative_path) {
    std::ifstream in(plugin_dir_ + "/" + relative_path, std::ios::binary);
    if (!in) {
        report_error("cannot read " + relative_path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return run_string(ss.str(), "@" + relative_path);
}

int LuaRuntime::ref_value(lua_State* L, int index) {
    lua_pushvalue(L, index);
    if (L != L_)
        lua_xmove(L, L_, 1);
    return luaL_ref(L_, LUA_REGISTRYINDEX);
}

void LuaRuntime::unref(int ref) {
    luaL_unref(L_, LUA_REGISTRYINDEX, ref);
}

void LuaRuntime::invoke(int fn_ref, const PushFn& push_args) {
    if (faulted_)
        return;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, fn_ref);
    spawn(push_args);
}

void LuaRuntime::on_close(std::function<void()> fn) {
    closers_.push_back(std::move(fn));
}

// The function to run is on top of L_'s stack.
bool LuaRuntime::spawn(const PushFn& push_args) {
    lua_State* co = lua_newthread(L_);
    int thread_ref = luaL_ref(L_, LUA_REGISTRYINDEX);
    lua_xmove(L_, co, 1);
    int nargs = push_args ? push_args(co) : 0;
    threads_[co] = thread_ref;
    return enter(co, nargs);
}

bool LuaRuntime::enter(lua_State* co, int nargs) {
    ++depth_;
    int nres = 0;
    int status = lua_resume(co, L_, nargs, &nres);
    --depth_;
    if (status == LUA_OK) {
        lua_pop(co, nres);
        drop(co);
        return true;
    }
    if (status == LUA_YIELD) {
        lua_pop(co, nres);
        report_error("coroutine.yield() outside an async call");
        drop(co);
        return false;
    }
    const char* msg = lua_tostring(co, -1);
    luaL_traceback(L_, co, msg ? msg : "(error object is not a string)", 0);
    std::string text = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    drop(co);
    if (status == LUA_ERRMEM)
        fault("out of memory (cap " + std::to_string(limits_.memory_bytes / 1024) + " KB)");
    else
        report_error(text);
    return false;
}

void LuaRuntime::drop(lua_State* co) {
    auto it = threads_.find(co);
    if (it == threads_.end())
        return;
    lua_closethread(co, L_);
    luaL_unref(L_, LUA_REGISTRYINDEX, it->second);
    threads_.erase(it);
}

void LuaRuntime::report_error(const std::string& message) {
    spdlog::warn("[plugin {}] {}", plugin_id_, message);
    if (errors_.record(Clock::now()))
        fault("3 errors within 60 s");
}

void LuaRuntime::fault(const std::string& reason) {
    if (faulted_)
        return;
    faulted_ = true;
    fault_reason_ = reason;
    spdlog::error("[plugin {}] disabled: {}", plugin_id_, reason);
    if (on_fault_)
        on_fault_(reason);
}

void LuaRuntime::budget_hook(lua_State*, lua_Debug*) {}

int LuaRuntime::await_async(lua_State* co, const std::function<void(Pending)>&) {
    return luaL_error(co, "async calls are not available yet");
}

void LuaRuntime::resume(lua_State*, const PushFn&) {}

void LuaRuntime::Pending::resolve(PushFn) const {}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
