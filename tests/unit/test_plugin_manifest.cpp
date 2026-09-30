// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_manifest.h"
#include "plugin_permissions.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;

namespace {
bool has_error_containing(const ManifestParse& r, const std::string& needle) {
    for (const auto& e : r.errors) {
        if (e.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

ManifestParse with_setting(const std::string& setting) {
    return parse_manifest(R"({"id":"ab","name":"n","version":"1","settings":[)" + setting + "]}");
}

ManifestParse with_widgets(const std::string& widgets) {
    return parse_manifest(R"({"id":"ab","name":"n","version":"1","widgets":[)" + widgets + "]}");
}
} // namespace

TEST_CASE("plugin ids", "[plugin][manifest]") {
    CHECK(is_valid_plugin_id("orca-cal"));
    CHECK(is_valid_plugin_id("ab"));
    CHECK_FALSE(is_valid_plugin_id("a"));
    CHECK_FALSE(is_valid_plugin_id("orca_cal"));
    CHECK_FALSE(is_valid_plugin_id("Orca"));
    CHECK_FALSE(is_valid_plugin_id("1orca"));
    CHECK_FALSE(is_valid_plugin_id(std::string(33, 'a')));
    CHECK(is_valid_plugin_id(std::string(32, 'a')));
}

TEST_CASE("owned names need the id, an underscore and a rest", "[plugin][manifest]") {
    CHECK(is_owned_name("orca-cal", "orca-cal_status"));
    CHECK_FALSE(is_owned_name("orca-cal", "orca-cal_"));
    CHECK_FALSE(is_owned_name("orca-cal", "orca-calx_status"));
    CHECK_FALSE(is_owned_name("orca", "orca-cal_status"));
    CHECK_FALSE(is_owned_name("orca-cal", "status"));
    CHECK(owner_of("orca-cal_my_status") == "orca-cal");
    CHECK(owner_of("nounderscore").empty());
}

TEST_CASE("a complete manifest parses", "[plugin][manifest]") {
    auto r = parse_manifest(R"({
      "id": "orca-cal", "name": "Orca Calibration", "version": "1.2.0",
      "author": "someone", "description": "d", "helix_version": ">=1.1",
      "permissions": ["gcode", "moonraker_write"], "memory_mb": 4,
      "settings": [
        {"key": "companion", "type": "string", "label": "Companion"},
        {"key": "auto_apply", "type": "bool", "label": "Auto", "default": false},
        {"key": "step", "type": "int", "label": "Step", "min": 1, "max": 20, "default": 5},
        {"key": "ratio", "type": "float", "label": "Ratio", "min": 0.5, "max": 1.5, "default": 0.95},
        {"key": "test", "type": "enum", "label": "Test",
         "options": ["temperature", "flow"], "default": "flow"},
        {"key": "ping", "type": "action", "label": "Ping", "callback": "orca-cal_ping"},
        {"key": "state", "type": "info", "label": "State", "subject": "orca-cal_state"}
      ],
      "settings_overlay": "orca-cal_settings",
      "widgets": [{"id": "orca-cal_launch", "name": "Launch", "icon": "tune",
                   "description": "Start", "component": "orca-cal_widget",
                   "colspan": 1, "rowspan": 1, "max_colspan": 2}]
    })");
    REQUIRE(r.errors.empty());
    REQUIRE(r.manifest);
    CHECK(r.manifest->id == "orca-cal");
    CHECK(r.manifest->helix_version == ">=1.1");
    CHECK(r.manifest->memory_mb == 4);
    CHECK(r.manifest->permissions == PermissionSet{Permission::Gcode, Permission::MoonrakerWrite});
    CHECK(r.manifest->settings_overlay == "orca-cal_settings");
    REQUIRE(r.manifest->widgets.size() == 1);
    CHECK(r.manifest->widgets[0].id == "orca-cal_launch");
    CHECK(r.manifest->widgets[0].max_colspan == 2);
    REQUIRE(r.manifest->settings.size() == 7);
    CHECK(r.manifest->settings[2].type == SettingType::Int);
    CHECK(r.manifest->settings[2].default_value == 5);
    CHECK(r.manifest->settings[3].max == 1.5);
    CHECK(r.manifest->settings[4].options == std::vector<std::string>{"temperature", "flow"});
    CHECK(r.manifest->settings[5].callback == "orca-cal_ping");
}

TEST_CASE("memory_mb defaults to 2 and is bounded", "[plugin][manifest]") {
    std::string base = R"({"id":"ab","name":"n","version":"1")";
    CHECK(parse_manifest(base + "}").manifest->memory_mb == 2);
    CHECK(parse_manifest(base + R"(,"memory_mb":64})").manifest->memory_mb == 64);
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":0})"), "memory_mb"));
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":65})"), "memory_mb"));
    CHECK(has_error_containing(parse_manifest(base + R"(,"memory_mb":"4"})"), "memory_mb"));
}

TEST_CASE("manifest errors", "[plugin][manifest]") {
    CHECK(has_error_containing(parse_manifest("{not json"), "not valid JSON"));
    CHECK(has_error_containing(parse_manifest("[]"), "must be an object"));
    CHECK(has_error_containing(parse_manifest(R"({"name":"n","version":"1"})"), "'id'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"Bad","name":"n","version":"1"})"), "'id'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"ab","version":"1"})"), "'name'"));
    CHECK(has_error_containing(parse_manifest(R"({"id":"ab","name":"n"})"), "'version'"));
    CHECK(
        has_error_containing(parse_manifest(R"({"id":"ab","name":"n","version":1})"), "'version'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","permissions":["root"]})"),
        "unknown permission 'root'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","permissions":"gcode"})"),
        "'permissions'"));
    CHECK(has_error_containing(
        parse_manifest(R"({"id":"ab","name":"n","version":"1","settings_overlay":"x_view"})"),
        "settings_overlay"));
    CHECK_FALSE(parse_manifest(R"({"id":"ab","name":"n"})").manifest);
}

TEST_CASE("setting declaration errors", "[plugin][manifest]") {
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"colour","label":"l"})"), "type"));
    CHECK(has_error_containing(with_setting(R"({"key":"K!","type":"bool","label":"l"})"), "key"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"bool"})"), "label"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"int","label":"l","min":5,"max":5})"), "min"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"int","label":"l","min":1,"max":5,"default":9})"),
        "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"int","label":"l","min":1,"max":5,"default":2.5})"),
        "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"enum","label":"l","options":[]})"), "options"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"enum","label":"l","options":["a"],"default":"b"})"),
        "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"action","label":"l","callback":"other_x"})"),
        "callback"));
    CHECK(
        has_error_containing(with_setting(R"({"key":"k","type":"info","label":"l"})"), "subject"));
    CHECK(has_error_containing(with_setting(R"({"key":"k","type":"bool","label":"l","default":1})"),
                               "default"));
    CHECK(has_error_containing(
        with_setting(R"({"key":"k","type":"string","label":"l","default":1})"), "default"));
    CHECK(has_error_containing(
        with_setting(
            R"({"key":"k","type":"bool","label":"l"},{"key":"k","type":"bool","label":"l"})"),
        "duplicate"));
    CHECK(has_error_containing(with_setting(R"("notanobject")"), "must be an object"));
}

TEST_CASE("permissions", "[plugin][manifest]") {
    CHECK(permission_from_string("gcode") == Permission::Gcode);
    CHECK(permission_from_string("moonraker_write") == Permission::MoonrakerWrite);
    CHECK(permission_from_string("http") == Permission::Http);
    CHECK(permission_from_string("storage") == Permission::Storage);
    CHECK_FALSE(permission_from_string("GCODE"));
    CHECK(std::string(permission_name(Permission::MoonrakerWrite)) == "moonraker_write");

    PermissionSet granted{Permission::Gcode};
    CHECK(permission_growth(granted, {Permission::Gcode}).empty());
    CHECK(permission_growth(granted, {}).empty());
    CHECK(permission_growth(granted, {Permission::Http, Permission::Gcode}) ==
          std::vector<Permission>{Permission::Http});

    CHECK(is_readonly_moonraker_method("printer.objects.query"));
    CHECK(is_readonly_moonraker_method("machine.system_info"));
    CHECK_FALSE(is_readonly_moonraker_method("printer.gcode.script"));
    CHECK_FALSE(is_readonly_moonraker_method("server.files.delete_file"));
    CHECK_FALSE(is_readonly_moonraker_method("printer.objects.query.extra"));
}

TEST_CASE("manifest widgets parse with cell spans", "[plugin][manifest]") {
    auto r = with_widgets(R"({"id":"ab_launch","name":"Launch","icon":"tune",
        "description":"Start","component":"ab_widget","colspan":1,"rowspan":1,
        "max_colspan":2,"max_rowspan":1})");
    REQUIRE(r.manifest);
    REQUIRE(r.manifest->widgets.size() == 1);
    const auto& w = r.manifest->widgets[0];
    CHECK(w.id == "ab_launch");
    CHECK(w.component == "ab_widget");
    CHECK(w.icon == "tune");
    CHECK(w.colspan == 1);
    CHECK(w.max_colspan == 2);
    CHECK(w.max_rowspan == 1);
}

TEST_CASE("manifest widgets reject bad names and spans", "[plugin][manifest]") {
    CHECK(has_error_containing(with_widgets(R"({"id":"launch","name":"L","component":"ab_w"})"),
                               "widgets[0]"));
    CHECK(has_error_containing(with_widgets(R"({"id":"ab_l","name":"L","component":"w"})"),
                               "'component'"));
    CHECK(has_error_containing(
        with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w","colspan":9})"), "'colspan'"));
    CHECK(has_error_containing(
        with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w","colspan":2,"max_colspan":1})"),
        "'max_colspan'"));
    CHECK(has_error_containing(with_widgets(R"({"id":"ab_l","name":"L","component":"ab_w"},
                                               {"id":"ab_l","name":"M","component":"ab_w"})"),
                               "duplicate widget id"));
    std::string nine;
    for (int i = 0; i < 9; ++i)
        nine += std::string(i ? "," : "") + R"({"id":"ab_w)" + std::to_string(i) +
                R"(","name":"W","component":"ab_c"})";
    CHECK(has_error_containing(with_widgets(nine), "at most 8"));
}

#endif // HELIX_HAS_PLUGINS
