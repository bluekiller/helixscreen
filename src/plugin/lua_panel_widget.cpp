// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "lua_panel_widget.h"

#include "grid_layout.h"
#include "plugin_host.h"

namespace helix::plugin {

LuaPanelWidget::LuaPanelWidget(std::string plugin_id, std::string widget_id, std::string component,
                               LifetimeToken runtime)
    : plugin_id_(std::move(plugin_id)), widget_id_(std::move(widget_id)),
      component_(std::move(component)), runtime_(std::move(runtime)) {}

void LuaPanelWidget::run(WidgetHook hook, const LuaRuntime::PushFn& args) {
    if (runtime_.expired())
        return;
    PluginHost* host = PluginHost::live();
    LuaRuntime* rt = host ? host->runtime(plugin_id_) : nullptr;
    if (rt)
        dispatch_widget_hook(*rt, widget_id_, hook, args);
}

void LuaPanelWidget::attach(lv_obj_t* widget_obj, lv_obj_t*) {
    root_ = widget_obj;
    install_delete_hook(root_);
    run(WidgetHook::Attach);
}

void LuaPanelWidget::detach() {
    if (!root_)
        return;
    run(WidgetHook::Detach);
    uninstall_delete_hook();
    root_ = nullptr;
}

LuaPanelWidget::~LuaPanelWidget() {
    detach();
}

void LuaPanelWidget::on_size_changed(int colspan, int rowspan, int width_px, int height_px) {
    // The grid speaks tracks; a plugin's widget is authored in whole cells.
    constexpr int kTracks = helix::GridLayout::TRACKS_PER_CELL;
    run(WidgetHook::Size, [=](lua_State* co) {
        lua_pushinteger(co, colspan / kTracks);
        lua_pushinteger(co, rowspan / kTracks);
        lua_pushinteger(co, width_px);
        lua_pushinteger(co, height_px);
        return 4;
    });
}

void LuaPanelWidget::on_activate() {
    run(WidgetHook::Activate);
}

void LuaPanelWidget::on_deactivate() {
    run(WidgetHook::Deactivate);
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
