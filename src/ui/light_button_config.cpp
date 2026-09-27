// SPDX-License-Identifier: GPL-3.0-or-later

#include "light_button_config.h"

#include "config.h"
#include "json_utils.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "panel_widget_config.h"
#include "panel_widget_manager.h"
#include "panel_widget_registry.h"

#include <cstring>

namespace helix {

namespace {

bool is_light_button(const PanelWidgetEntry& e) {
    const PanelWidgetDef* def = find_widget_def(e.id);
    return def != nullptr && std::strcmp(def->id, "led") == 0;
}

std::string pending_value(const Config& cfg) {
    const nlohmann::json* p = cfg.try_get_json(cfg.df() + led::LIGHT_BUTTON_PENDING_PATH);
    return (p != nullptr && p->is_string()) ? p->get<std::string>() : std::string();
}

} // namespace

std::vector<std::string> home_light_button_keys(const PanelWidgetConfig& home,
                                                const std::string& pending) {
    std::vector<std::string> keys;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (!e.is_placed() || !is_light_button(e)) {
                continue;
            }
            const std::string key = json_util::safe_string(e.config, "led");
            keys.push_back(key.empty() ? pending : key);
        }
    }
    return keys;
}

bool adopt_pending_light_button(Config& cfg, PanelWidgetConfig& home) {
    const std::string value = pending_value(cfg);
    if (value.empty()) {
        return false;
    }
    std::vector<std::string> ids;
    for (size_t p = 0; p < home.page_count(); ++p) {
        for (const auto& e : home.page_entries(p)) {
            if (e.is_placed() && is_light_button(e) &&
                json_util::safe_string(e.config, "led").empty()) {
                ids.push_back(e.id);
            }
        }
    }
    for (const auto& id : ids) {
        nlohmann::json c = home.get_widget_config(id);
        if (!c.is_object()) {
            c = nlohmann::json::object();
        }
        c["led"] = value;
        home.set_widget_config(id, c);
    }
    cfg.set(cfg.df() + led::LIGHT_BUTTON_PENDING_PATH, nlohmann::json());
    cfg.save();
    return !ids.empty();
}

std::vector<std::string> home_light_button_targets() {
    auto& ctrl = led::LedController::instance();
    std::vector<std::string> keys;
    if (auto* cfg = Config::get_instance()) {
        keys = home_light_button_keys(PanelWidgetManager::instance().get_widget_config("home"),
                                      pending_value(*cfg));
    }
    return led::union_light_targets(keys, ctrl.switchable_ids(), ctrl.chamber_light());
}

} // namespace helix
