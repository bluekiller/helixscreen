// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "plugin_xml_policy.h"

#include "../catch_amalgamated.hpp"

using helix::plugin::check_plugin_xml;

namespace {
std::string view_with(const std::string& body) {
    return "<component><view extends=\"lv_obj\">" + body + "</view></component>";
}
} // namespace

TEST_CASE("plugin XML may use plugin_event and its own subjects", "[plugin][xml_policy]") {
    CHECK(check_plugin_xml("ab", view_with(R"(<lv_label bind_text="ab_status"/>
        <lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="ab_go:3"/></lv_button>
        <lv_obj><bind_flag_if_eq subject="ab_busy" flag="hidden" ref_value="0"/></lv_obj>)"))
              .empty());
}

TEST_CASE("plugin XML may not name app callbacks or subjects", "[plugin][xml_policy]") {
    CHECK_FALSE(
        check_plugin_xml(
            "ab",
            view_with(
                R"(<lv_button><event_cb trigger="clicked" callback="on_estop_clicked"/></lv_button>)"))
            .empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<text_input clear_callback="on_wifi_clear"/>)"))
                    .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", view_with(R"(<lv_slider bind_value="extruder_target"/>)")).empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab",
            view_with(
                R"(<lv_obj><bind_flag_if_eq subject="printer_connected" flag="hidden" ref_value="0"/></lv_obj>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab",
            view_with(
                R"(<lv_button><event_cb trigger="clicked" callback="plugin_event" user_data="other_go"/></lv_button>)"))
            .empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_label bind_text="$status"/>)")).empty());
    // Ownership would reject "$status" too; the message is what pins the prop rule itself.
    CHECK(check_plugin_xml("ab", view_with(R"(<lv_label bind_text="$status"/>)"))
              .find("through a prop") != std::string::npos);
    CHECK_FALSE(check_plugin_xml("ab",
                                 "<component><subjects><int name=\"x\" value=\"0\"/></subjects>"
                                 "<view extends=\"lv_obj\"/></component>")
                    .empty());
    CHECK_FALSE(check_plugin_xml("ab", "not xml <").empty());
}

TEST_CASE("both spellings of an lv_obj child are checked the same way", "[plugin][xml_policy]") {
    CHECK(
        check_plugin_xml(
            "ab",
            view_with(
                R"(<lv_obj-event_cb trigger="clicked" callback="plugin_event" user_data="ab_go"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml(
            "ab",
            view_with(
                R"(<lv_obj-event_cb trigger="clicked" callback="plugin_event" user_data="other_go"/>)"))
            .empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<screen_load_event/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_obj-screen_load_event/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_obj-screen_create_event/>)")).empty());
}

TEST_CASE("plugin XML may use only allowlisted elements", "[plugin][xml_policy]") {
    // App components declare subject and callback props under free names, so any
    // component off the allowlist could smuggle one past an attribute rule.
    CHECK_FALSE(
        check_plugin_xml("ab", view_with(R"(<setting_toggle_row subject="printer_connected"/>)"))
            .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", "<component><view extends=\"temp_display\"/></component>").empty());
    CHECK(
        check_plugin_xml("ab", "<component><view extends=\"overlay_panel\"/></component>").empty());
    CHECK(check_plugin_xml("ab", "<component><view extends=\"ab_tile\"/></component>").empty());
    CHECK(check_plugin_xml("ab", view_with(R"(<overlay_panel title="Demo"/>)")).empty());
}

TEST_CASE("callback and subject attributes are checked by name and suffix",
          "[plugin][xml_policy]") {
    CHECK_FALSE(
        check_plugin_xml("ab", view_with(R"(<overlay_panel action_button_callback="on_save"/>)"))
            .empty());
    CHECK(check_plugin_xml("ab",
                           view_with(R"(<overlay_panel action_button_callback="plugin_event"/>)"))
              .empty());
    CHECK_FALSE(
        check_plugin_xml("ab", view_with(R"(<overlay_panel title_subject="printer_connected"/>)"))
            .empty());
    CHECK(
        check_plugin_xml("ab", view_with(R"(<overlay_panel title_subject="ab_title"/>)")).empty());
    CHECK_FALSE(check_plugin_xml("ab", view_with(R"(<lv_obj clear_cb="on_wifi_clear"/>)")).empty());
    CHECK_FALSE(
        check_plugin_xml("ab", view_with(R"(<lv_obj event_cb="on_mode_change"/>)")).empty());
}

#endif // HELIX_HAS_PLUGINS
