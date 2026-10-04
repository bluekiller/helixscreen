// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// A theme switch and the palette walker against colors the XML author wrote
// inline. The engine records every inline `style_*` color written as a global
// `#const` or a literal; the switch re-resolves the `#const` ones, and the
// walker leaves every authored color alone while still theming the rest.

#include "../test_fixtures.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "lvgl/lvgl.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

/// Puts the fixture's theme and mode back so later cases see what they expect.
struct RestoreTheme {
    helix::ThemeData theme = theme_manager_get_active_theme();
    bool dark = theme_manager_is_dark_mode();
    ~RestoreTheme() {
        theme_manager_apply_theme(theme, dark);
    }
};

uint32_t rgb(lv_color_t c) {
    return lv_color_to_u32(c) & 0xFFFFFF;
}

uint32_t hex(const std::string& s) {
    return rgb(theme_manager_parse_hex_color(s.c_str()));
}

uint32_t const_rgb(const char* name) {
    const char* v = lv_xml_get_const_silent(nullptr, name);
    REQUIRE(v != nullptr);
    return rgb(theme_manager_parse_hex_color(v));
}

uint32_t text_rgb(lv_obj_t* obj) {
    return rgb(lv_obj_get_style_text_color(obj, LV_PART_MAIN));
}

uint32_t bg_rgb(lv_obj_t* obj) {
    return rgb(lv_obj_get_style_bg_color(obj, LV_PART_MAIN));
}

lv_obj_t* named(lv_obj_t* root, const char* name) {
    lv_obj_t* obj = lv_obj_find_by_name(root, name);
    REQUIRE(obj != nullptr);
    return obj;
}

} // namespace

TEST_CASE_METHOD(XMLTestFixture, "the palette walker keeps an inline token color on a plain label",
                 "[theme][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_label name="tok" text="Primary" style_text_color="#primary"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_plain_token", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_plain_token");
    REQUIRE(root != nullptr);
    lv_obj_t* tok = named(root, "tok");

    const uint32_t primary = const_rgb("primary");
    REQUIRE(primary != const_rgb("text"));
    REQUIRE(text_rgb(tok) == primary);

    theme_apply_current_palette_to_tree(root);
    CHECK(text_rgb(tok) == primary);
}

TEST_CASE_METHOD(XMLTestFixture, "the palette walker still themes a label with no inline color",
                 "[theme][xml]") {
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_label name="plain" text="Plain"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_plain_label", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_plain_label");
    REQUIRE(root != nullptr);
    lv_obj_t* plain = named(root, "plain");

    lv_obj_set_style_text_color(plain, lv_color_hex(0x00FF00), LV_PART_MAIN);
    theme_apply_current_palette_to_tree(root);
    CHECK(text_rgb(plain) == const_rgb("text"));
}

TEST_CASE_METHOD(XMLTestFixture,
                 "a tree built before a mode switch shows the new mode's token colors after it",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, false);
    REQUIRE(hex(theme.light.card_bg) != hex(theme.dark.card_bg));
    REQUIRE(hex(theme.light.text_muted) != hex(theme.dark.text_muted));

    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_obj name="card" width="200" height="100" style_bg_opa="255" style_bg_color="#card_bg">
      <text_small name="muted" text="Muted" style_text_color="#text_muted"/>
      <lv_label name="plain_muted" text="Muted" style_text_color="#text_muted"/>
    </lv_obj>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_mode_switch", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_mode_switch");
    REQUIRE(root != nullptr);
    // A popup on the top layer is outside the active screen's tree.
    lv_obj_t* top =
        static_cast<lv_obj_t*>(lv_xml_create(lv_layer_top(), "lr_mode_switch", nullptr));
    REQUIRE(top != nullptr);

    CHECK(bg_rgb(named(root, "card")) == hex(theme.light.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.light.text_muted));

    theme_manager_apply_theme(theme, true);
    CHECK(bg_rgb(named(root, "card")) == hex(theme.dark.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.dark.text_muted));
    CHECK(text_rgb(named(root, "plain_muted")) == hex(theme.dark.text_muted));
    CHECK(bg_rgb(named(top, "card")) == hex(theme.dark.card_bg));
    CHECK(text_rgb(named(top, "muted")) == hex(theme.dark.text_muted));

    theme_manager_apply_theme(theme, false);
    CHECK(bg_rgb(named(root, "card")) == hex(theme.light.card_bg));
    CHECK(text_rgb(named(root, "muted")) == hex(theme.light.text_muted));

    lv_obj_delete(top);
}

TEST_CASE_METHOD(XMLTestFixture, "a token color C++ overwrote after creation survives a switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, false);

    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <text_small name="status" text="Status" style_text_color="#text_muted"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_cpp_overwrite", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_cpp_overwrite");
    REQUIRE(root != nullptr);
    lv_obj_t* status = named(root, "status");

    lv_obj_set_style_text_color(status, lv_color_hex(0xC01020), LV_PART_MAIN);
    theme_manager_apply_theme(theme, true);
    CHECK(text_rgb(status) == 0xC01020);
}

TEST_CASE_METHOD(XMLTestFixture, "a literal white status text survives a mode switch",
                 "[theme][xml]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);

    // The QR scanner's white status line. Kept off a dark ancestor here, where
    // the walker's light-text rule would paint white anyway and prove nothing.
    const char* xml = R"(<component>
  <view extends="lv_obj" width="300" height="300">
    <lv_obj name="backdrop" width="200" height="100" style_bg_opa="255" style_bg_color="0x000000"/>
    <text_body name="status" text="Scanning" style_text_color="#FFFFFF"/>
  </view>
</component>)";
    REQUIRE(lv_xml_register_component_from_data("lr_literal_white", xml) == LV_RESULT_OK);
    lv_obj_t* root = create_component("lr_literal_white");
    REQUIRE(root != nullptr);

    theme_manager_apply_theme(theme, false);
    CHECK(text_rgb(named(root, "status")) == 0xFFFFFF);
    CHECK(bg_rgb(named(root, "backdrop")) == 0x000000);
    theme_manager_apply_theme(theme, true);
    CHECK(text_rgb(named(root, "status")) == 0xFFFFFF);
}
