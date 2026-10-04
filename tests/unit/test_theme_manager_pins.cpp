// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_theme_manager_pins.cpp
 * @brief Pins the observable behaviour of the theme manager: the XML constant
 *        set it registers, the change-generation subject, live recolouring of
 *        inline colours, and the lifetime of the subjects it publishes.
 *
 * The constant snapshot lives in tests/fixtures/theme_const_golden.txt. After an
 * intentional token change, regenerate it with
 * `HELIX_UPDATE_GOLDEN=1 ./build/bin/helix-tests "[theme_pins]"` and review the diff.
 */

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "lvgl/lvgl.h"
#include "theme_loader.h"
#include "theme_manager.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "../catch_amalgamated.hpp"

namespace {

std::string golden_path() {
    std::string src = __FILE__;
    auto pos = src.rfind("/tests/unit/");
    const std::string root = pos != std::string::npos ? src.substr(0, pos) + "/" : "";
    return root + "tests/fixtures/theme_const_golden.txt";
}

/// Every token name the theme system is responsible for, read back through the
/// XML constant registry the way a widget would.
std::set<std::string> token_names() {
    std::set<std::string> names;
    for (const char* type : {"color", "px", "string"}) {
        for (const auto& [name, value] : theme_manager_parse_all_xml_for_element("ui_xml", type)) {
            names.insert(name);
        }
    }
    for (const char* suffix : {"_light", "_dark", "_micro", "_tiny", "_small", "_medium", "_large",
                               "_xlarge", "_xxlarge"}) {
        for (const char* type : {"color", "px", "string"}) {
            for (const auto& [name, value] :
                 theme_manager_parse_all_xml_for_suffix("ui_xml", type, suffix)) {
                names.insert(name);
            }
        }
    }
    for (const char* name : helix::ModePalette::color_names()) {
        names.insert(name);
        names.insert(std::string(name) + "_light");
        names.insert(std::string(name) + "_dark");
    }
    for (const char* name :
         {"border_radius", "border_width", "border_opacity", "shadow_intensity", "shadow_opa",
          "shadow_offset_y", "shadow_cast", "overlay_shadow_opa", "overlay_width_transient",
          "overlay_width_destination", "nav_width", "font_body", "font_small", "font_heading"}) {
        names.insert(name);
    }
    for (int i = 1; i <= 8; ++i) {
        names.insert("object_color_" + std::to_string(i));
    }
    return names;
}

std::string snapshot(int32_t w, int32_t h, bool dark) {
    lv_display_t* disp = lv_display_get_default();
    ScopedResolution res(disp, w, h);
    theme_manager_refresh_layout_constants(disp);
    theme_manager_apply_theme(helix::get_builtin_fallback_theme(), dark);

    std::ostringstream out;
    out << "## " << w << "x" << h << (dark ? " dark" : " light") << "\n";
    for (const auto& name : token_names()) {
        const char* value = lv_xml_get_const_silent(nullptr, name.c_str());
        out << name << "=" << (value ? value : "<unset>") << "\n";
    }
    // Mode-aware resolution: the base names themselves are first-wins, so the
    // light/dark difference is only visible through the accessor.
    char hex[16];
    for (const auto& [base, v] :
         theme_manager_parse_all_xml_for_suffix("ui_xml", "color", "_light")) {
        snprintf(hex, sizeof(hex), "%06x",
                 lv_color_to_u32(theme_manager_get_color(base.c_str())) & 0xFFFFFF);
        out << "color:" << base << "=" << hex << "\n";
    }
    for (const char* name : helix::ModePalette::color_names()) {
        snprintf(hex, sizeof(hex), "%06x",
                 lv_color_to_u32(theme_manager_get_color(name)) & 0xFFFFFF);
        out << "palette:" << name << "=" << hex << "\n";
    }
    return out.str();
}

int generation() {
    return lv_subject_get_int(theme_manager_get_changed_subject());
}

/// Puts the fixture's theme and mode back so later cases see what they expect.
struct RestoreTheme {
    helix::ThemeData theme = theme_manager_get_active_theme();
    bool dark = theme_manager_is_dark_mode();
    ~RestoreTheme() {
        theme_manager_apply_theme(theme, dark);
    }
};

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "theme constants match the golden snapshot",
                 "[theme_pins][theme]") {
    RestoreTheme restore;

    std::string actual;
    for (const auto& [w, h] : {std::pair<int32_t, int32_t>{480, 272}, {800, 480}, {1280, 720}}) {
        for (bool dark : {true, false}) {
            actual += snapshot(w, h, dark);
        }
    }

    if (std::getenv("HELIX_UPDATE_GOLDEN")) {
        std::ofstream(golden_path()) << actual;
        WARN("golden snapshot rewritten: " << golden_path());
        return;
    }

    std::ifstream f(golden_path());
    REQUIRE(f.good());
    std::stringstream expected;
    expected << f.rdbuf();

    // Compare line by line so a failure names the token, not a 150KB blob.
    std::istringstream a(actual), e(expected.str());
    std::string al, el, section;
    int line = 0;
    while (true) {
        const bool ha = static_cast<bool>(std::getline(a, al));
        const bool he = static_cast<bool>(std::getline(e, el));
        if (!ha && !he)
            break;
        ++line;
        if (ha && al.rfind("## ", 0) == 0)
            section = al;
        INFO("section " << section << ", line " << line);
        REQUIRE(al == el);
    }
}

TEST_CASE_METHOD(LVGLUITestFixture, "theme_changed generation bumps on apply and toggle",
                 "[theme_pins][theme]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();

    const int g0 = generation();
    theme_manager_apply_theme(theme, true);
    CHECK(generation() == g0 + 1);

    theme_manager_apply_theme(theme, false);
    CHECK(generation() == g0 + 2);

    theme_manager_toggle_dark_mode();
    CHECK(generation() == g0 + 3);
    CHECK(theme_manager_is_dark_mode());

    theme_manager_notify_change();
    CHECK(generation() == g0 + 4);
}

TEST_CASE_METHOD(LVGLUITestFixture, "inline card_bg and border recolor on a theme switch",
                 "[theme_pins][theme]") {
    RestoreTheme restore;
    const auto theme = helix::get_builtin_fallback_theme();
    theme_manager_apply_theme(theme, true);

    const lv_color_t dark_card = theme_manager_parse_hex_color(theme.dark.card_bg.c_str());
    const lv_color_t light_card = theme_manager_parse_hex_color(theme.light.card_bg.c_str());
    const lv_color_t dark_border = theme_manager_parse_hex_color(theme.dark.border.c_str());
    const lv_color_t light_border = theme_manager_parse_hex_color(theme.light.border.c_str());
    REQUIRE_FALSE(lv_color_eq(dark_card, light_card));
    REQUIRE_FALSE(lv_color_eq(dark_border, light_border));

    lv_obj_t* box = lv_obj_create(test_screen());
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, dark_card, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, dark_border, LV_PART_MAIN);

    theme_manager_apply_theme(theme, false);
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(box, LV_PART_MAIN), light_card));
    CHECK(lv_color_eq(lv_obj_get_style_border_color(box, LV_PART_MAIN), light_border));

    theme_manager_toggle_dark_mode();
    CHECK(lv_color_eq(lv_obj_get_style_bg_color(box, LV_PART_MAIN), dark_card));
    CHECK(lv_color_eq(lv_obj_get_style_border_color(box, LV_PART_MAIN), dark_border));
}

namespace {
void noop_cb(lv_observer_t*, lv_subject_t*) {}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "swatch, breakpoint and portrait subjects follow init/deinit",
                 "[theme_pins][theme]") {
    std::vector<lv_subject_t*> subjects;
    for (const char* name : {"ui_breakpoint", "ui_breakpoint_v", "ui_is_portrait"}) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        subjects.push_back(s);
    }
    for (int i = 0; i < 16; ++i) {
        lv_subject_t* s =
            lv_xml_get_subject(nullptr, ("swatch_" + std::to_string(i) + "_desc").c_str());
        REQUIRE(s != nullptr);
        subjects.push_back(s);
    }
    subjects.push_back(theme_manager_get_changed_subject());
    CHECK(lv_subject_get_int(subjects[0]) ==
          lv_subject_get_int(theme_manager_get_breakpoint_subject()));
    CHECK(std::string(lv_subject_get_string(subjects[3])) == "App background");
    CHECK(std::string(lv_subject_get_string(subjects[18])) == "Focus ring");

    for (lv_subject_t* s : subjects) {
        const uint32_t before = lv_ll_get_len(&s->subs_ll);
        lv_subject_add_observer(s, noop_cb, nullptr);
        REQUIRE(lv_ll_get_len(&s->subs_ll) == before + 1);
    }

    theme_manager_deinit();
    for (lv_subject_t* s : subjects) {
        CHECK(lv_ll_get_len(&s->subs_ll) == 0);
    }

    // The next init publishes them again and restarts the generation count.
    theme_manager_init(lv_display_get_default(), false);
    CHECK(generation() == 0);
    CHECK(lv_xml_get_subject(nullptr, "ui_is_portrait") != nullptr);
    CHECK(lv_xml_get_subject(nullptr, "swatch_15_desc") != nullptr);
}
