// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_host.h"

#include "ui_toast_manager.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"
#include "version.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace helix::plugin {

namespace {

PluginHost* g_live_host = nullptr;

void plugin_event_cb(lv_event_t* e) {
    auto* user_data = static_cast<const char*>(lv_event_get_user_data(e));
    if (g_live_host && user_data)
        g_live_host->dispatch_event(user_data);
}

// Named for its one job: generic names like join() at file scope register as
// shipped free functions and turn every test helper of the same name into a
// mirror-gate finding.
std::string join_errors(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

void register_plugin_event_callback() {
    static bool registered = false;
    if (registered)
        return;
    lv_xml_register_event_cb(nullptr, "plugin_event", &plugin_event_cb);
    registered = true;
}

size_t plugin_memory_budget(uint64_t mem_total_bytes) {
    return static_cast<size_t>(std::min<uint64_t>(mem_total_bytes / 16, uint64_t(64) << 20));
}

uint64_t read_mem_total() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    uint64_t kb = 0;
    while (in >> key >> kb) {
        if (key == "MemTotal:")
            return kb * 1024;
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 0;
}

const char* plugin_status_name(PluginStatus s) {
    switch (s) {
    case PluginStatus::Disabled:
        return "disabled";
    case PluginStatus::Loaded:
        return "loaded";
    case PluginStatus::NeedsApproval:
        return "needs approval";
    case PluginStatus::Invalid:
        return "invalid";
    case PluginStatus::Incompatible:
        return "incompatible";
    case PluginStatus::OverBudget:
        return "over memory budget";
    case PluginStatus::Faulted:
        return "faulted";
    }
    return "?";
}

PluginHost::PluginHost(Deps deps) : deps_(std::move(deps)) {
    g_live_host = this;
}

PluginHost::~PluginHost() {
    unload_all();
    guard_.invalidate();
    if (g_live_host == this)
        g_live_host = nullptr;
}

void PluginHost::load_from(const std::string& dir) {
    unload_all();
    plugins_.clear();
    dir_ = dir;

    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_directory(ec) && std::filesystem::exists(entry.path() / "manifest.json", ec))
            names.push_back(entry.path().filename().string());
    }
    if (ec)
        spdlog::warn("[PluginHost] cannot scan {}: {}", dir, ec.message());
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        PluginInfo info;
        info.dir_name = name;
        auto parsed =
            parse_manifest(read_file(std::filesystem::path(dir) / name / "manifest.json"));
        if (!parsed.manifest) {
            info.status = PluginStatus::Invalid;
            info.reason = join_errors(parsed.errors, "; ");
        } else if (parsed.manifest->id != name) {
            info.status = PluginStatus::Invalid;
            info.reason = "directory name must match id '" + parsed.manifest->id + "'";
        } else {
            info.manifest = std::move(parsed.manifest);
        }
        plugins_.push_back(std::move(info));
    }

    for (auto& info : plugins_) {
        if (info.manifest && info.status == PluginStatus::Disabled)
            consider(info);
    }
    for (const auto& info : plugins_) {
        spdlog::info("[PluginHost] {}: {}{}", info.dir_name, plugin_status_name(info.status),
                     info.reason.empty() ? "" : " (" + info.reason + ")");
    }
}

json PluginHost::enabled_entry(const std::string& id) const {
    json block = deps_.read_block();
    if (!block.is_object())
        return json();
    auto en = block.find("enabled");
    if (en == block.end() || !en->is_object())
        return json();
    auto it = en->find(id);
    return it == en->end() ? json() : *it;
}

size_t PluginHost::memory_in_use() const {
    size_t total = 0;
    for (const auto& [id, l] : loaded_)
        total += l.memory_bytes;
    return total;
}

void PluginHost::consider(PluginInfo& info) {
    const Manifest& m = *info.manifest;
    info.reason.clear();

    bool app_version_known = helix::version::parse_version(deps_.helix_version).has_value();
    if (!m.helix_version.empty() && app_version_known &&
        !helix::version::check_version_constraint(m.helix_version, deps_.helix_version)) {
        info.status = PluginStatus::Incompatible;
        info.reason = "needs HelixScreen " + m.helix_version;
        return;
    }

    json entry = enabled_entry(m.id);
    if (!entry.is_object()) {
        info.status = PluginStatus::Disabled;
        return;
    }

    PermissionSet granted;
    if (auto p = entry.find("permissions"); p != entry.end() && p->is_array()) {
        for (const auto& n : *p) {
            if (!n.is_string())
                continue;
            if (auto perm = permission_from_string(n.get<std::string>()))
                granted.insert(*perm);
        }
    }
    auto grown = permission_growth(granted, m.permissions);
    if (!grown.empty()) {
        info.status = PluginStatus::NeedsApproval;
        info.reason = "asks for new permissions:";
        for (Permission p : grown)
            info.reason += std::string(" ") + permission_name(p);
        return;
    }

    size_t want = static_cast<size_t>(m.memory_mb) << 20;
    size_t in_use = memory_in_use();
    if (in_use + want > deps_.memory_budget) {
        size_t left = deps_.memory_budget > in_use ? deps_.memory_budget - in_use : 0;
        info.status = PluginStatus::OverBudget;
        info.reason = "needs " + std::to_string(m.memory_mb) + " MB, " +
                      std::to_string(left >> 20) + " MB left of the plugin budget";
        return;
    }

    info.status = load(info) ? PluginStatus::Loaded : info.status;
}

bool PluginHost::load(PluginInfo& info) {
    const Manifest& m = *info.manifest;
    const std::string id = m.id;
    auto root = std::filesystem::path(dir_) / info.dir_name;

    std::vector<std::filesystem::path> xmls;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(root / "ui", ec)) {
        if (e.path().extension() == ".xml")
            xmls.push_back(e.path());
    }
    std::sort(xmls.begin(), xmls.end());
    for (const auto& p : xmls) {
        if (!is_owned_name(id, p.stem().string())) {
            info.status = PluginStatus::Invalid;
            info.reason = "component '" + p.stem().string() + "' must be named " + id + "_<name>";
            return false;
        }
    }
    // A plugin registering an existing name would replace the app's component (and unloading
    // would then remove it), so nothing is registered until every stem is free.
    for (const auto& p : xmls) {
        if (lv_xml_component_get_scope(p.stem().string().c_str())) {
            info.status = PluginStatus::Invalid;
            info.reason = "component '" + p.stem().string() + "' already exists";
            return false;
        }
    }

    auto [it, inserted] = loaded_.try_emplace(id);
    Loaded& l = it->second;
    for (const auto& p : xmls) {
        std::string uri = "A:" + p.string();
        if (lv_xml_register_component_from_file(uri.c_str()) != LV_RESULT_OK) {
            info.status = PluginStatus::Invalid;
            info.reason = "cannot load " + p.filename().string();
            unload(id);
            return false;
        }
        l.components.push_back(p.stem().string());
    }

    json block = deps_.read_block();
    if (block.is_object()) {
        if (auto s = block.find("settings"); s != block.end() && s->is_object()) {
            if (auto e = s->find(id); e != s->end() && e->is_object())
                l.settings = *e;
        }
    }
    if (!l.settings.is_object())
        l.settings = json::object();

    l.memory_bytes = static_cast<size_t>(m.memory_mb) << 20;
    LuaRuntime::Limits limits;
    limits.memory_bytes = l.memory_bytes;
    LifetimeToken token = guard_.token();
    l.rt = std::make_unique<LuaRuntime>(
        id, root.string(), limits, [this, token, id](const std::string& reason) {
            token.defer("plugin_fault", [this, id, reason] { on_fault(id, reason); });
        });
    l.ctx = std::make_unique<PluginContext>(
        PluginContext{*l.rt, deps_.backend, m, &l.settings, [this, id] { save_settings(id); },
                      plugin_storage_path(deps_.settings_path, id)});
    for (Installer install :
         {&install_core_bindings, &install_ui_bindings, &install_printer_bindings,
          &install_moonraker_bindings, &install_io_bindings})
        install(*l.ctx);

    if (!l.rt->run_file("main.lua")) {
        info.status = PluginStatus::Faulted;
        info.reason = l.rt->faulted() ? l.rt->fault_reason() : "main.lua failed; see the log";
        unload(id);
        return false;
    }
    return true;
}

void PluginHost::unload(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    Loaded& l = it->second;
    if (l.rt && !l.rt->faulted()) {
        lua_State* L = l.rt->state();
        lua_getglobal(L, "on_unload");
        if (lua_isfunction(L, -1)) {
            int ref = l.rt->ref_value(L, -1);
            lua_pop(L, 1);
            l.rt->invoke(ref);
            l.rt->unref(ref);
        } else {
            lua_pop(L, 1);
        }
    }
    l.rt.reset();
    for (const auto& c : l.components)
        lv_xml_component_unregister(c.c_str());
    l.ctx.reset();
    loaded_.erase(it);
}

void PluginHost::unload_all() {
    std::vector<std::string> ids;
    for (const auto& [id, l] : loaded_)
        ids.push_back(id);
    for (const auto& id : ids)
        unload(id);
}

void PluginHost::on_fault(const std::string& id, const std::string& reason) {
    if (!loaded_.count(id))
        return; // already unloaded by the load path
    unload(id);
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Faulted;
        info->reason = reason;
        std::string detail = info->manifest->name + ": " + reason;
        ToastManager::instance().show_with_detail(ToastSeverity::WARNING, lv_tr("Plugin disabled"),
                                                  detail.c_str());
    }
}

void PluginHost::save_settings(const std::string& id) {
    auto it = loaded_.find(id);
    if (it == loaded_.end())
        return;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["settings"].is_object())
        block["settings"] = json::object();
    block["settings"][id] = it->second.settings;
    deps_.write_block(block);
}

PluginInfo* PluginHost::find(const std::string& id) {
    for (auto& p : plugins_) {
        if (p.manifest && p.manifest->id == id && p.dir_name == id)
            return &p;
    }
    return nullptr;
}

LuaRuntime* PluginHost::runtime(const std::string& id) {
    auto it = loaded_.find(id);
    return it == loaded_.end() ? nullptr : it->second.rt.get();
}

bool PluginHost::enable(const std::string& id) {
    PluginInfo* info = find(id);
    if (!info || info->status == PluginStatus::Invalid)
        return false;
    json block = deps_.read_block();
    if (!block.is_object())
        block = json::object();
    if (!block["enabled"].is_object())
        block["enabled"] = json::object();
    json perms = json::array();
    for (Permission p : info->manifest->permissions)
        perms.push_back(permission_name(p));
    block["enabled"][id] = {{"version", info->manifest->version}, {"permissions", perms}};
    deps_.write_block(block);
    unload(id);
    info->status = PluginStatus::Disabled;
    consider(*info);
    return info->status == PluginStatus::Loaded;
}

void PluginHost::disable(const std::string& id) {
    unload(id);
    json block = deps_.read_block();
    if (block.is_object() && block.contains("enabled") && block["enabled"].is_object()) {
        block["enabled"].erase(id);
        deps_.write_block(block);
    }
    if (PluginInfo* info = find(id)) {
        info->status = PluginStatus::Disabled;
        info->reason.clear();
    }
}

void PluginHost::dispatch_event(std::string_view user_data) {
    PluginEventTarget target = parse_plugin_event(user_data);
    if (target.id.empty()) {
        spdlog::debug("[PluginHost] ignoring malformed plugin_event '{}'", user_data);
        return;
    }
    LuaRuntime* rt = runtime(target.id);
    if (!rt) {
        spdlog::debug("[PluginHost] plugin_event for '{}', which is not loaded", target.id);
        return;
    }
    if (!dispatch_ui_handler(*rt, target.name, target.arg))
        spdlog::debug("[PluginHost] plugin '{}' has no handler '{}'", target.id, target.name);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
