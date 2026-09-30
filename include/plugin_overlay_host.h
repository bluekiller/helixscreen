// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "async_lifetime_guard.h"
#include "lvgl/lvgl.h"
#include "panel_lifecycle.h"

#include <functional>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace helix::plugin {

/// What the ui bindings need to open overlays. PluginHost provides the real one; tests
/// may provide a fake. Handles are positive and unique per process.
struct PluginUi {
    /// Extra attributes for lv_xml_create, already policy-checked by the caller.
    using Attrs = std::vector<std::pair<std::string, std::string>>;
    std::function<int(const std::string& component, std::function<void()> on_closed,
                      const Attrs& attrs)>
        open;
    std::function<void(int handle)> close;
};

/// Plugin overlays on the navigation stack. Main thread.
class PluginOverlayHost {
  public:
    /// Creates `component` on the active screen and pushes it. 0 when it cannot be
    /// created. `on_closed` runs when the overlay is closed for any reason but an
    /// unload of its plugin.
    int open(const std::string& plugin_id, const std::string& component,
             std::function<void()> on_closed, const PluginUi::Attrs& attrs = {});
    void close(int handle);
    /// Pops and deletes every overlay `plugin_id` has open, without running on_closed.
    void close_all(const std::string& plugin_id);
    size_t open_count(const std::string& plugin_id) const;

  private:
    /// No-op lifecycle: plugin overlays have no activation work in this phase, but
    /// every push needs a registered instance so deactivation dispatch finds one.
    struct OverlayLifecycle : IPanelLifecycle {
        void on_activate() override {}
        void on_deactivate(DeactivateReason) override {}
        const char* get_name() const override {
            return name;
        }
        const char* name = "plugin-overlay";
    };

    struct Record {
        int handle = 0;
        std::string plugin_id;
        std::string name; ///< the component, surfaced through the lifecycle's get_name()
        lv_obj_t* root = nullptr;
        std::function<void()> on_closed;
        OverlayLifecycle lifecycle;
        /// Popped via go_back and waiting for the navigation close callback; the
        /// callback may have been silenced by close_all.
        bool closing = false;
    };

    std::list<Record>::iterator find(int handle);
    /// Unregisters `it` from navigation, deferred-deletes its root and erases it,
    /// returning the next record. Runs on_closed only when the plugin is still there
    /// to receive it.
    std::list<Record>::iterator finish(std::list<Record>::iterator it, bool run_on_closed);
    /// The navigation close callback: the overlay left the stack.
    void on_nav_closed(int handle);

    std::list<Record> records_;
    int next_handle_ = 1;
    AsyncLifetimeGuard guard_;
};

} // namespace helix::plugin
