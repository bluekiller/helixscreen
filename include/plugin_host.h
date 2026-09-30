// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lua_bindings.h"
#include "lua_runtime.h"
#include "plugin_backend.h"
#include "plugin_manifest.h"
#include "plugin_overlay_host.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace helix::plugin {

/// What all plugins together may allocate: min(MemTotal / 16, 64 MB).
size_t plugin_memory_budget(uint64_t mem_total_bytes);

/// MemTotal from /proc/meminfo in bytes; 0 when unreadable.
uint64_t read_mem_total();

enum class PluginStatus {
    Disabled,
    Loaded,
    NeedsApproval,
    Invalid,
    Incompatible,
    OverBudget,
    Faulted
};

const char* plugin_status_name(PluginStatus s);

struct PluginInfo {
    std::string dir_name;
    std::optional<Manifest> manifest; ///< unset when the manifest failed to parse
    PluginStatus status = PluginStatus::Disabled;
    std::string reason; ///< why, for every status but Loaded and Disabled
};

/// Owns every plugin: discovery, enable state, memory budget, load, unload and faults.
/// Main thread only.
class PluginHost {
  public:
    struct Deps {
        PluginBackend backend;
        std::function<json()> read_block;             ///< the current /plugins object
        std::function<void(const json&)> write_block; ///< replaces /plugins and saves
        std::string settings_path;                    ///< for storage file paths
        std::string helix_version;
        size_t memory_budget = 0;
    };

    explicit PluginHost(Deps deps);
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    /// Unloads everything, rescans `dir`, and loads every enabled plugin.
    void load_from(const std::string& dir);
    void unload_all();

    /// The host the app runs, or nullptr.
    static PluginHost* live();

    const std::vector<PluginInfo>& plugins() const {
        return plugins_;
    }

    /// Grants the manifest's current permissions and loads the plugin.
    bool enable(const std::string& id);
    void disable(const std::string& id);
    LuaRuntime* runtime(const std::string& id);

    /// The overlays every loaded plugin has on the navigation stack.
    const PluginOverlayHost& overlays() const {
        return overlays_;
    }

    /// The body of the global `plugin_event` XML callback.
    void dispatch_event(std::string_view user_data);

  private:
    struct Loaded {
        json settings;
        std::unique_ptr<PluginContext> ctx;
        std::unique_ptr<LuaRuntime> rt; ///< destroyed before ctx
        /// The ui handle this plugin's bindings reach; binds `this` host, so it dies
        /// with the record, before rt's state can call it again.
        PluginUi ui;
        /// Each component this plugin registered, with the scope it created: an app that later
        /// registers the same name replaces the scope, and unload must then leave it alone.
        std::vector<std::pair<std::string, const void*>> components;
        /// The widget definitions this plugin registered, to unregister at unload.
        std::vector<std::string> widget_ids;
        size_t memory_bytes = 0;
    };

    PluginInfo* find(const std::string& id);
    void consider(PluginInfo& info);
    bool load(PluginInfo& info);
    void unload(const std::string& id);
    void on_fault(const std::string& id, const std::string& reason);
    void save_settings(const std::string& id);
    json enabled_entry(const std::string& id) const;
    size_t memory_in_use() const;

    Deps deps_;
    std::string dir_;
    std::vector<PluginInfo> plugins_;
    std::map<std::string, Loaded> loaded_;
    AsyncLifetimeGuard guard_;
    /// Set while load_from / unload_all loop over plugins: widget definitions still change,
    /// but each change only marks the set dirty; one notify runs after the loop.
    bool bulk_ = false;
    /// Widget definitions changed since the last notify_widget_defs_changed(). Survives a
    /// bulk unload_all so load_from's single end-of-scan notify covers plugins that went.
    bool widget_defs_dirty_ = false;
    PluginOverlayHost overlays_;
};

/// Registers the `plugin_event` XML callback once per process. It forwards to the live host.
void register_plugin_event_callback();

} // namespace helix::plugin
