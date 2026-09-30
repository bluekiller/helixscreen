// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "plugin_permissions.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hv/json.hpp"

namespace helix::plugin {

using json = nlohmann::json;

enum class SettingType { Bool, Int, Float, Enum, String, Action, Info };

constexpr int kMaxWidgetCells = 8;
constexpr size_t kMaxWidgetsPerPlugin = 8;

/// One entry of the manifest's `settings` array.
struct SettingDecl {
    std::string key;
    std::string label;
    SettingType type = SettingType::Bool;
    json default_value;               ///< null when the manifest gives none
    double min = 0;                   ///< Int and Float
    double max = 0;                   ///< Int and Float
    std::vector<std::string> options; ///< Enum
    std::string callback;             ///< Action: handler name, owned by the plugin
    std::string subject;              ///< Info: subject name, owned by the plugin
};

/// One entry of the manifest's `widgets` array. Spans are in grid cells.
struct WidgetDecl {
    std::string id; ///< <plugin>_<name>
    std::string name;
    std::string icon; ///< icon name; empty when absent
    std::string description;
    std::string component; ///< <plugin>_<name>, a file in ui/
    int colspan = 1;
    int rowspan = 1;
    int max_colspan = 0; ///< 0: not resizable on this axis
    int max_rowspan = 0;
};

struct Manifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string helix_version; ///< version constraint, empty when absent
    PermissionSet permissions;
    int memory_mb = 2;
    std::vector<SettingDecl> settings;
    std::string settings_overlay; ///< XML component name, empty when absent
    std::vector<WidgetDecl> widgets;
};

/// `manifest` is set only when `errors` is empty.
struct ManifestParse {
    std::optional<Manifest> manifest;
    std::vector<std::string> errors;
};

ManifestParse parse_manifest(const std::string& text);

/// `^[a-z][a-z0-9-]{1,31}$`. No underscore, so the first `_` of a registered name
/// separates its owner.
bool is_valid_plugin_id(std::string_view id);

/// True for `<id>_<rest>` with a non-empty rest.
bool is_owned_name(std::string_view id, std::string_view name);

/// Everything before the first `_`; empty when there is none.
std::string_view owner_of(std::string_view name);

} // namespace helix::plugin
