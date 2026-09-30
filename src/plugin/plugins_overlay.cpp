// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugins_overlay.h"

#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_utils.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "plugin_permissions.h"
#include "static_panel_registry.h"
#include "ui/ui_lazy_panel_helper.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace helix::plugin {

namespace {

std::unique_ptr<PluginsOverlay> g_plugins_overlay;
lv_obj_t* g_plugins_panel_cache = nullptr;

} // namespace

PluginsOverlay& get_plugins_overlay() {
    if (!g_plugins_overlay) {
        g_plugins_overlay = std::make_unique<PluginsOverlay>();
        StaticPanelRegistry::instance().register_destroy("PluginsOverlay", []() {
            g_plugins_overlay.reset();
            g_plugins_panel_cache = nullptr;
        });
    }
    return *g_plugins_overlay;
}

void show_plugins_overlay(lv_obj_t* parent, const char* caller) {
    helix::ui::lazy_create_and_push_overlay<PluginsOverlay>(
        get_plugins_overlay, g_plugins_panel_cache, parent, "Plugins", caller);
}

void PluginsOverlay::init_subjects() {
    subjects_initialized_ = true; // rows read the host; the frame binds nothing
}

void PluginsOverlay::register_callbacks() {
    register_plugins_overlay_callbacks();
}

const char* PluginsOverlay::get_name() const {
    return "Plugins";
}

lv_obj_t* PluginsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_)
        return overlay_root_; // a second caller adopts the tree the first built
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "plugins_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[Plugins] cannot create plugins_overlay");
        return nullptr;
    }
    parent_screen_ = parent;
    populate_rows();
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN); // shown by push_overlay
    return overlay_root_;
}

void PluginsOverlay::populate_rows() {
    // A consent or Disable callback can outlive the screen's widget tree; with
    // no root there is nothing to repopulate. Searching from NULL would adopt
    // the active screen instead.
    if (!overlay_root_)
        return;
    lv_obj_t* rows = lv_obj_find_by_name(overlay_root_, "plugins_rows");
    if (!rows)
        return;
    helix::ui::safe_clean_children(rows);
    const PluginHost* host = PluginHost::live();
    if (!host)
        return;
    for (const PluginInfo& info : host->plugins()) {
        const std::string& label =
            info.manifest && !info.manifest->name.empty() ? info.manifest->name : info.dir_name;
        std::string desc = lv_tr(plugin_status_name(info.status));
        if (!info.reason.empty())
            desc += ": " + info.reason;
        const std::string row_name = "row_" + info.dir_name;
        const char* pairs[] = {"name",        row_name.c_str(), "label",
                               label.c_str(), "description",    desc.c_str(),
                               "icon",        "puzzle_outline", "description_min_bp",
                               "0",           "callback",       "plugin_list_row_clicked",
                               nullptr};
        lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(rows, "setting_action_row", pairs));
        if (!row) {
            spdlog::warn("[Plugins] cannot create a row for {}", info.dir_name);
            continue;
        }
        bindings_.push_back(info.dir_name);
        lv_obj_set_user_data(row, &bindings_.back());
    }
}

const std::string* PluginsOverlay::binding_at(const void* ud) {
    for (const std::string& id : bindings_)
        if (&id == ud)
            return &id;
    return nullptr;
}

void PluginsOverlay::set_consent_shower(ConsentShower shower) {
    consent_shower_ = std::move(shower);
}

void PluginsOverlay::show_consent_for(const Manifest& m, const std::vector<Permission>& grown,
                                      std::function<void()> on_yes) {
    if (consent_shower_)
        consent_shower_(m, grown, std::move(on_yes));
    else
        show_consent(m, grown, std::move(on_yes));
}

void PluginsOverlay::activate(const std::string& id) {
    PluginHost* host = PluginHost::live();
    if (!host)
        return;
    const PluginInfo* info = nullptr;
    for (const PluginInfo& p : host->plugins()) {
        if (p.dir_name == id) {
            info = &p;
            break;
        }
    }
    if (!info || !info->manifest)
        return;
    const Manifest& m = *info->manifest;

    switch (info->status) {
    case PluginStatus::Disabled:
    case PluginStatus::NeedsApproval: {
        // An update that grew its permission set re-prompts for the growth
        // only; a first enable asks for the whole set.
        std::vector<Permission> grown;
        if (info->status == PluginStatus::NeedsApproval)
            grown = permission_growth(host->granted(id), m.permissions);
        const auto tok = object_lifetime_.token();
        PluginHost* h = host;
        show_consent_for(m, grown, [this, tok, h, id] {
            if (tok.expired() || PluginHost::live() != h)
                return; // the overlay closed or the host was destroyed meanwhile
            h->enable(id);
            populate_rows();
        });
        break;
    }
    case PluginStatus::Loaded: {
        const auto tok = object_lifetime_.token();
        PluginHost* h = host;
        const std::string name = m.name.empty() ? id : m.name;
        // Only the Disable button disables: dismissing the dialog (backdrop,
        // ESC) leaves the plugin running, so no on_dismiss action exists.
        helix::ui::ConfirmOptions opts;
        opts.cancel_text = lv_tr("Disable");
        opts.on_cancel = [this, tok, h, id] {
            if (tok.expired() || PluginHost::live() != h)
                return;
            h->disable(id);
            populate_rows();
        };
        helix::ui::modal_confirm(
            name.c_str(), lv_tr("Enabled. Open its settings, or disable it?"), ModalSeverity::Info,
            lv_tr("Settings"),
            [tok, h, id] {
                if (tok.expired() || PluginHost::live() != h)
                    return;
                h->open_settings(id);
            },
            opts);
        break;
    }
    case PluginStatus::Faulted:
    case PluginStatus::OverBudget:
        // Re-enable re-runs the load under the current memory budget; consent
        // already covers the granted permission set.
        host->enable(id);
        populate_rows();
        break;
    case PluginStatus::Invalid:
    case PluginStatus::Incompatible:
        break; // the manifest itself is the problem; a dialog cannot fix it
    }
}

namespace {

void plugin_list_row_clicked_cb(lv_event_t* e) {
    PluginsOverlay& ov = get_plugins_overlay();
    // The row root carries the binding; the tap may have landed on a child.
    for (lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e)); obj;
         obj = lv_obj_get_parent(obj)) {
        if (const std::string* id = ov.binding_at(lv_obj_get_user_data(obj))) {
            ov.activate(*id);
            return;
        }
    }
}

} // namespace

void register_plugins_overlay_callbacks() {
    static bool registered = false;
    if (registered)
        return;
    lv_xml_register_event_cb(nullptr, "plugin_list_row_clicked", &plugin_list_row_clicked_cb);
    registered = true;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
