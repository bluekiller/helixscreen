// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_xml_policy.h"

#include "helix-xml/src/libs/expat/expat.h"
#include "plugin_manifest.h"

#include <string>
#include <string_view>

namespace helix::plugin {

namespace {

struct Walk {
    std::string id;
    std::string error;
};

// helix-xml resolves a child tag both as written and with an "lv_obj-" prefix
// (lv_xml_widget_get_processor), so every element comparison happens on the
// stripped name.
constexpr std::string_view kLvObjPrefix = "lv_obj-";

std::string_view strip_lv_obj_prefix(std::string_view el) {
    if (el.size() > kLvObjPrefix.size() && el.substr(0, kLvObjPrefix.size()) == kLvObjPrefix)
        el.remove_prefix(kLvObjPrefix.size());
    return el;
}

bool starts_with(std::string_view s, std::string_view head) {
    return s.size() >= head.size() && s.substr(0, head.size()) == head;
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

// App chrome a plugin's own components may build on. Grows through Phase 4's author guide.
bool is_allowlisted_app_component(std::string_view name) {
    return name == "overlay_panel";
}

// `el` arrives already stripped of its "lv_obj-" prefix. The closed allowlist below
// rejects the screen load and create events on its own; the explicit check keeps
// that true if the allowlist ever grows.
bool is_allowed_element(std::string_view id, std::string_view el) {
    if (el == "screen_load_event" || el == "screen_create_event")
        return false;
    if (el == "component" || el == "view" || el == "api" || el == "prop")
        return true;
    if (starts_with(el, "lv_"))
        return true;
    if (el == "event_cb" || el == "style" || el == "play_timeline_event" ||
        starts_with(el, "bind_") || starts_with(el, "remove_style") ||
        (starts_with(el, "subject_") && ends_with(el, "_event")))
        return true;
    return is_owned_name(id, el) || is_allowlisted_app_component(el);
}

void fail(Walk& w, std::string msg) {
    if (w.error.empty())
        w.error = std::move(msg);
}

void on_start(void* ud, const XML_Char* name, const XML_Char** attrs) {
    auto& w = *static_cast<Walk*>(ud);
    std::string_view el = strip_lv_obj_prefix(name);
    if (!is_allowed_element(w.id, el)) {
        fail(w, "<" + std::string(name) + "> is not available to plugins");
        return;
    }
    if (el == "view") {
        for (int i = 0; attrs[i]; i += 2) {
            if (std::string_view(attrs[i]) != "extends")
                continue;
            std::string_view val = attrs[i + 1];
            if (!starts_with(val, "lv_") && !is_owned_name(w.id, val) &&
                !is_allowlisted_app_component(val)) {
                fail(w, "extends=\"" + std::string(val) + "\": <" + std::string(val) +
                            "> is not available to plugins");
            }
        }
    }
    for (int i = 0; attrs[i]; i += 2) {
        std::string_view key = attrs[i];
        std::string_view val = attrs[i + 1];
        bool is_callback = key == "callback" || key == "event_cb" || ends_with(key, "_callback") ||
                           ends_with(key, "_cb");
        bool is_subject =
            key == "subject" || ends_with(key, "_subject") || starts_with(key, "bind_");
        bool is_target = el == "event_cb" && key == "user_data";
        if (!is_callback && !is_subject && !is_target)
            continue;
        if (!val.empty() && val.front() == '$') {
            fail(w, std::string(key) + "=\"" + std::string(val) + "\": plugins cannot pass " +
                        std::string(key) + " through a prop");
            continue;
        }
        if (is_callback && val != "plugin_event") {
            fail(w, std::string(key) + "=\"" + std::string(val) +
                        "\": plugins may only use the plugin_event callback");
        } else if (is_subject && !is_owned_name(w.id, val)) {
            fail(w, std::string(key) + "=\"" + std::string(val) + "\": subject must be named " +
                        w.id + "_<name>");
        } else if (is_target && !is_owned_name(w.id, val.substr(0, val.find(':')))) {
            fail(w, "user_data=\"" + std::string(val) + "\": handler must be named " + w.id +
                        "_<name>");
        }
    }
}

void on_end(void*, const XML_Char*) {}

} // namespace

std::string check_plugin_xml(std::string_view id, const std::string& xml) {
    Walk w{std::string(id), {}};
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p)
        return "cannot check XML: out of memory";
    XML_SetUserData(p, &w);
    XML_SetElementHandler(p, &on_start, &on_end);
    if (XML_Parse(p, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_ERROR)
        fail(w, std::string("not valid XML: ") + XML_ErrorString(XML_GetErrorCode(p)));
    XML_ParserFree(p);
    return w.error;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
