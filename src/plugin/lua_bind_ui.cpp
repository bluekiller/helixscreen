// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_toast_manager.h"

#include "lua_bindings.h"

#include <spdlog/spdlog.h>

#include <climits>
#include <unordered_map>

namespace helix::plugin {

namespace {

const char kSubjectMeta[] = "helix.subject";
const char kUiStateKey = 0;
constexpr size_t kMaxString = 1024;

struct ObserverCtx {
    LuaRuntime* rt;
    int fn_ref;
    bool armed =
        false; ///< lv_subject_add_observer reports the current value at once; Lua sees changes only
};

struct SubjectEntry {
    std::string full_name;
    lv_subject_t subject{};
    std::vector<char> buf;
    std::vector<char> prev;
    bool is_string = false;
};

// Everything one runtime registered here. Freed by the runtime's closer.
struct UiState {
    std::vector<std::unique_ptr<SubjectEntry>> subjects;
    std::vector<std::unique_ptr<ObserverCtx>> observers;
    std::unordered_map<std::string, int> handlers; ///< name -> fn ref
};

UiState& ui_state(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    auto* s = static_cast<UiState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *s;
}

bool is_valid_local_name(std::string_view n) {
    if (n.empty() || n.size() > 48)
        return false;
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

void on_subject_change(lv_observer_t* obs, lv_subject_t* subject) {
    auto* ctx = static_cast<ObserverCtx*>(lv_observer_get_user_data(obs));
    if (!ctx->armed)
        return;
    bool is_string = subject->type == LV_SUBJECT_TYPE_STRING;
    std::string s = is_string ? lv_subject_get_string(subject) : std::string();
    int32_t v = is_string ? 0 : lv_subject_get_int(subject);
    ctx->rt->invoke(ctx->fn_ref, [is_string, s, v](lua_State* co) {
        if (is_string)
            lua_pushlstring(co, s.data(), s.size());
        else
            lua_pushinteger(co, v);
        return 1;
    });
}

SubjectEntry& check_subject(lua_State* L) {
    return **static_cast<SubjectEntry**>(luaL_checkudata(L, 1, kSubjectMeta));
}

int subject_get(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string)
        lua_pushstring(L, lv_subject_get_string(&s.subject));
    else
        lua_pushinteger(L, lv_subject_get_int(&s.subject));
    return 1;
}

int subject_set(lua_State* L) {
    auto& s = check_subject(L);
    if (s.is_string) {
        size_t len = 0;
        const char* v = luaL_checklstring(L, 2, &len);
        luaL_argcheck(L, len < kMaxString, 2, "string longer than 1023 bytes");
        lv_subject_copy_string(&s.subject, v);
    } else {
        lua_Integer v = luaL_checkinteger(L, 2);
        luaL_argcheck(L, v >= INT32_MIN && v <= INT32_MAX, 2, "integer out of range");
        lv_subject_set_int(&s.subject, static_cast<int32_t>(v));
    }
    return 0;
}

int subject_observe(lua_State* L) {
    auto& s = check_subject(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& rt = LuaRuntime::from(L);
    auto& ui = ui_state(L);
    ui.observers.push_back(std::make_unique<ObserverCtx>(ObserverCtx{&rt, rt.ref_value(L, 2)}));
    ObserverCtx* ctx = ui.observers.back().get();
    lv_subject_add_observer(&s.subject, &on_subject_change, ctx);
    ctx->armed = true;
    return 0;
}

int make_subject(lua_State* L, bool is_string) {
    auto& rt = LuaRuntime::from(L);
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "subject name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    std::string full = rt.plugin_id() + "_" + name;
    if (lv_xml_get_subject(nullptr, full.c_str()))
        return luaL_error(L, "subject '%s' already exists", full.c_str());

    auto entry = std::make_unique<SubjectEntry>();
    entry->full_name = full;
    entry->is_string = is_string;
    if (is_string) {
        size_t len = 0;
        const char* init = luaL_optlstring(L, 2, "", &len);
        if (len >= kMaxString)
            return luaL_error(L, "subject '%s': initial value longer than 1023 bytes",
                              full.c_str());
        entry->buf.assign(kMaxString, 0);
        entry->prev.assign(kMaxString, 0);
        lv_subject_init_string(&entry->subject, entry->buf.data(), entry->prev.data(), kMaxString,
                               init);
    } else {
        lua_Integer init = luaL_optinteger(L, 2, 0);
        luaL_argcheck(L, init >= INT32_MIN && init <= INT32_MAX, 2, "integer out of range");
        lv_subject_init_int(&entry->subject, static_cast<int32_t>(init));
    }
    lv_xml_register_subject(nullptr, full.c_str(), &entry->subject);

    SubjectEntry* raw = entry.get();
    ui_state(L).subjects.push_back(std::move(entry));
    *static_cast<SubjectEntry**>(lua_newuserdatauv(L, sizeof(SubjectEntry*), 0)) = raw;
    luaL_setmetatable(L, kSubjectMeta);
    return 1;
}

int ui_on(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string name = luaL_checkstring(L, 1);
    if (!is_valid_local_name(name))
        return luaL_error(L, "handler name '%s' must be 1-48 of [a-z0-9_-]", name.c_str());
    luaL_checktype(L, 2, LUA_TFUNCTION);
    auto& handlers = ui_state(L).handlers;
    if (auto it = handlers.find(name); it != handlers.end())
        rt.unref(it->second);
    handlers[name] = rt.ref_value(L, 2);
    return 0;
}

int ui_toast(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    static const char* const kNames[] = {"info", "success", "warning", "error", nullptr};
    static const ToastSeverity kSeverity[] = {ToastSeverity::INFO, ToastSeverity::SUCCESS,
                                              ToastSeverity::WARNING, ToastSeverity::ERROR};
    int i = luaL_checkoption(L, 2, "info", kNames);
    ToastManager::instance().show(kSeverity[i], msg);
    return 0;
}

int ui_confirm(lua_State* L) {
    auto& rt = LuaRuntime::from(L);
    std::string title = luaL_checkstring(L, 1);
    std::string msg = luaL_checkstring(L, 2);
    ModalSeverity severity = ModalSeverity::Info;
    std::string confirm_text = "OK";
    int confirm_ref = LUA_NOREF;
    int cancel_ref = LUA_NOREF;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        static const char* const kNames[] = {"info", "warning", "error", nullptr};
        static const ModalSeverity kSeverity[] = {ModalSeverity::Info, ModalSeverity::Warning,
                                                  ModalSeverity::Error};
        lua_getfield(L, 3, "severity");
        if (!lua_isnil(L, -1))
            severity = kSeverity[luaL_checkoption(L, -1, nullptr, kNames)];
        lua_pop(L, 1);
        lua_getfield(L, 3, "confirm_text");
        if (lua_isstring(L, -1))
            confirm_text = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_confirm");
        if (lua_isfunction(L, -1))
            confirm_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "on_cancel");
        if (lua_isfunction(L, -1))
            cancel_ref = rt.ref_value(L, -1);
        lua_pop(L, 1);
    }
    LuaRuntime* rtp = &rt;
    auto run = [rtp](int ref) {
        if (ref != LUA_NOREF)
            rtp->invoke(ref);
    };
    helix::ui::ConfirmOptions opts;
    opts.on_cancel = [run, cancel_ref] { run(cancel_ref); };
    opts.on_dismiss = opts.on_cancel;
    opts.owner_token = rt.token();
    helix::ui::modal_confirm(
        title.c_str(), msg.c_str(), severity, confirm_text.c_str(),
        [run, confirm_ref] { run(confirm_ref); }, opts);
    return 0;
}

} // namespace

PluginEventTarget parse_plugin_event(std::string_view user_data) {
    PluginEventTarget t;
    std::string_view head = user_data;
    std::optional<std::string> arg;
    if (auto colon = user_data.find(':'); colon != std::string_view::npos) {
        head = user_data.substr(0, colon);
        arg = std::string(user_data.substr(colon + 1));
    }
    std::string_view id = owner_of(head);
    if (!is_valid_plugin_id(id) || head.size() <= id.size() + 1)
        return t;
    t.id = std::string(id);
    t.name = std::string(head.substr(id.size() + 1));
    t.arg = std::move(arg);
    return t;
}

bool dispatch_ui_handler(LuaRuntime& rt, const std::string& name,
                         const std::optional<std::string>& arg) {
    auto& handlers = ui_state(rt.state()).handlers;
    auto it = handlers.find(name);
    if (it == handlers.end())
        return false;
    rt.invoke(it->second, [arg](lua_State* co) {
        if (arg)
            lua_pushlstring(co, arg->data(), arg->size());
        else
            lua_pushnil(co);
        return 1;
    });
    return true;
}

void install_ui_bindings(PluginContext& ctx) {
    lua_State* L = ctx.rt.state();
    auto* state = new UiState;
    lua_pushlightuserdata(L, state);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kUiStateKey);
    ctx.rt.on_close([state] {
        for (auto& s : state->subjects) {
            lv_xml_unregister_subject(nullptr, s->full_name.c_str());
            lv_subject_deinit(&s->subject); // also removes every observer on it
        }
        delete state;
    });

    static const luaL_Reg methods[] = {{"get", &subject_get},
                                       {"set", &subject_set},
                                       {"observe", &subject_observe},
                                       {nullptr, nullptr}};
    luaL_newmetatable(L, kSubjectMeta);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    // Lambdas rather than named wrappers: file-scope `subject_int`/`subject_string`
    // collide with helpers in existing test files and trip the test-mirror ratchet.
    static const luaL_Reg subject_fns[] = {
        {"int", [](lua_State* L) { return make_subject(L, false); }},
        {"string", [](lua_State* L) { return make_subject(L, true); }},
        {nullptr, nullptr}};
    static const luaL_Reg ui_fns[] = {
        {"on", &ui_on}, {"toast", &ui_toast}, {"confirm", &ui_confirm}, {nullptr, nullptr}};
    lua_getglobal(L, "helix");
    lua_newtable(L);
    luaL_setfuncs(L, subject_fns, 0);
    lua_setfield(L, -2, "subject");
    lua_newtable(L);
    luaL_setfuncs(L, ui_fns, 0);
    lua_setfield(L, -2, "ui");
    lua_pop(L, 1);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
