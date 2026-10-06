// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The numbered badges that identify an exclude-object target. The side list,
// the thumbnail map and the 2D/3D render all show one; they must agree on the
// number and colour for the same object, which compute_object_badges() decides.

#include "ui_exclude_object_badges.h"
#include "ui_exclude_object_map_view.h"

#include "../test_fixtures.h"
#include "gcode_layer_renderer.h"
#include "gcode_parser.h"
#include "printer_excluded_objects_state.h"
#include "theme_manager.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrinterExcludedObjectsState;
using helix::gcode::ParsedGCodeFile;
using helix::ui::compute_object_badges;
using helix::ui::ObjectBadge;
using ObjectInfo = PrinterExcludedObjectsState::ObjectInfo;

namespace {

ObjectInfo klipper_object(const std::string& name, glm::vec2 center, glm::vec2 bmin, glm::vec2 bmax,
                          bool has_center = true, bool has_bbox = true) {
    ObjectInfo o;
    o.name = name;
    o.center = center;
    o.bbox_min = bmin;
    o.bbox_max = bmax;
    o.has_center = has_center;
    o.has_bbox = has_bbox;
    return o;
}

ObjectInfo bare_object(const std::string& name) {
    return klipper_object(name, {}, {}, {}, false, false);
}

void add_parsed_object(ParsedGCodeFile& f, const std::string& name, glm::vec2 center,
                       glm::vec3 bmin, glm::vec3 bmax) {
    helix::gcode::GCodeObject o;
    o.name = name;
    o.center = center;
    o.bounding_box.expand(bmin);
    o.bounding_box.expand(bmax);
    f.objects[name] = o;
}

const ObjectBadge& by_name(const std::vector<ObjectBadge>& badges, const std::string& name) {
    for (const auto& b : badges) {
        if (b.name == name) {
            return b;
        }
    }
    FAIL("no badge for " << name);
    return badges.front();
}

} // namespace

TEST_CASE("Object badges number in defined order, not name order", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        klipper_object("Zeta", {10, 10}, {0, 0}, {20, 20}),
        klipper_object("Alpha", {50, 50}, {40, 40}, {60, 60}),
        klipper_object("Mid", {90, 90}, {80, 80}, {100, 100}),
    });

    // The parsed map is name-sorted (Alpha, Mid, Zeta); it must not decide order.
    ParsedGCodeFile parsed;
    add_parsed_object(parsed, "Alpha", {50, 50}, {40, 40, 0}, {60, 60, 5});
    add_parsed_object(parsed, "Mid", {90, 90}, {80, 80, 0}, {100, 100, 5});
    add_parsed_object(parsed, "Zeta", {10, 10}, {0, 0, 0}, {20, 20, 5});

    for (const ParsedGCodeFile* p : {static_cast<const ParsedGCodeFile*>(nullptr),
                                     static_cast<const ParsedGCodeFile*>(&parsed)}) {
        const auto badges = compute_object_badges(state, p);
        REQUIRE(badges.size() == 3);
        CHECK(badges[0].name == "Zeta");
        CHECK(badges[1].name == "Alpha");
        CHECK(badges[2].name == "Mid");
        for (int i = 0; i < 3; ++i) {
            CHECK(badges[i].defined_index == i);
            CHECK(badges[i].number == std::to_string(i + 1));
        }
    }
    state.deinit_subjects();
}

TEST_CASE("Object badges carry the excluded and current flags", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        klipper_object("A", {10, 10}, {0, 0}, {20, 20}),
        klipper_object("B", {50, 50}, {40, 40}, {60, 60}),
        klipper_object("C", {90, 90}, {80, 80}, {100, 100}),
    });
    state.set_excluded_objects({"B"});
    state.set_current_object("C");

    const auto badges = compute_object_badges(state, nullptr);
    REQUIRE(badges.size() == 3);
    CHECK_FALSE(badges[0].excluded);
    CHECK_FALSE(badges[0].current);
    CHECK(badges[1].excluded);
    CHECK_FALSE(badges[1].current);
    CHECK_FALSE(badges[2].excluded);
    CHECK(badges[2].current);
    state.deinit_subjects();
}

TEST_CASE("Object badge anchor falls back center -> parsed center -> bbox", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects_with_geometry({
        // Klipper CENTER wins over everything else.
        klipper_object("klipper_center", {11, 12}, {0, 0}, {40, 40}),
        // No Klipper CENTER: the parsed file's CENTER.
        klipper_object("parsed_center", {}, {0, 0}, {40, 40}, false, true),
        // Neither CENTER: Klipper's bbox centre.
        klipper_object("klipper_bbox", {}, {100, 200}, {120, 240}, false, true),
        // Nothing from Klipper, no parsed CENTER: the toolpath bbox centre.
        bare_object("parsed_bbox"),
        // Nothing anywhere: still numbered, no anchor.
        bare_object("nothing"),
    });

    ParsedGCodeFile parsed;
    add_parsed_object(parsed, "klipper_center", {70, 70}, {60, 60, 0}, {80, 80, 9});
    add_parsed_object(parsed, "parsed_center", {33, 34}, {0, 0, 0}, {40, 40, 3});
    add_parsed_object(parsed, "parsed_bbox", {0, 0}, {10, 20, 0}, {30, 60, 7});

    const auto badges = compute_object_badges(state, &parsed);
    REQUIRE(badges.size() == 5);

    const auto& kc = by_name(badges, "klipper_center");
    REQUIRE(kc.has_anchor);
    CHECK(kc.anchor == glm::vec2(11, 12));
    REQUIRE(kc.top_z.has_value());
    CHECK(*kc.top_z == Catch::Approx(9.0f));

    const auto& pc = by_name(badges, "parsed_center");
    REQUIRE(pc.has_anchor);
    CHECK(pc.anchor == glm::vec2(33, 34));

    const auto& kb = by_name(badges, "klipper_bbox");
    REQUIRE(kb.has_anchor);
    CHECK(kb.anchor == glm::vec2(110, 220));
    CHECK_FALSE(kb.top_z.has_value());

    const auto& pb = by_name(badges, "parsed_bbox");
    REQUIRE(pb.has_anchor);
    CHECK(pb.anchor == glm::vec2(20, 40));
    CHECK(*pb.top_z == Catch::Approx(7.0f));

    const auto& none = by_name(badges, "nothing");
    CHECK_FALSE(none.has_anchor);
    CHECK(none.defined_index == 4);
    CHECK(none.number == "5");

    // Without a parsed file the parsed-only objects lose their anchor, but
    // nobody's number moves.
    const auto unparsed = compute_object_badges(state, nullptr);
    CHECK(by_name(unparsed, "parsed_center").anchor == glm::vec2(20, 20)); // Klipper bbox
    CHECK_FALSE(by_name(unparsed, "parsed_bbox").has_anchor);
    CHECK(by_name(unparsed, "nothing").number == "5");
    state.deinit_subjects();
}

TEST_CASE("Object badges with names only (no geometry)", "[exclude_badges]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    state.set_defined_objects({"one", "two"});

    const auto badges = compute_object_badges(state, nullptr);
    REQUIRE(badges.size() == 2);
    CHECK_FALSE(badges[0].has_anchor);
    CHECK(badges[1].number == "2");
    state.deinit_subjects();
}

// The side list colours its chips from the same object palette, by defined index.
TEST_CASE_METHOD(XMLTestFixture, "Object badge colour is the object palette at its index",
                 "[exclude_badges]") {
    for (int i = 0; i < 10; ++i) {
        INFO("index " << i);
        CHECK(lv_color_eq(helix::ui::object_badge_color(i),
                          theme_manager_get_object_palette_color(i)));
    }
    CHECK_FALSE(lv_color_eq(helix::ui::object_badge_color(0), helix::ui::object_badge_color(1)));
    CHECK(helix::ui::object_badge_diameter() > 0);
    CHECK(helix::ui::object_badge_opa(true) < helix::ui::object_badge_opa(false));
}

TEST_CASE("badge_hit_index finds the disc under a point", "[exclude_badges]") {
    const std::vector<glm::vec2> centers = {{10, 10}, {100, 100}};
    CHECK(helix::ui::badge_hit_index(centers, 12, 9, 5.0f) == 0);
    CHECK(helix::ui::badge_hit_index(centers, 104, 103, 5.0f) == 1);
    CHECK(helix::ui::badge_hit_index(centers, 50, 50, 5.0f) == -1);
    CHECK(helix::ui::badge_hit_index(centers, 16, 10, 5.0f) == -1);
}

// ============================================================================
// Thumbnail map: badge numbers and colours key on the defined index
// ============================================================================

TEST_CASE_METHOD(XMLTestFixture, "Map view key keeps defined numbering when an object has no bbox",
                 "[exclude_badges][exclude_map]") {
    REQUIRE(register_component("components/exclude_object_map"));
    state().excluded_objects_state().set_defined_objects_with_geometry({
        klipper_object("First", {35, 35}, {20, 20}, {50, 50}),
        bare_object("NoBox"),
        klipper_object("Third", {115, 115}, {100, 100}, {130, 130}),
    });

    helix::ui::ExcludeObjectMapView view;
    view.create(test_screen(), &state().excluded_objects_state(), 235.0f, 235.0f, nullptr, nullptr);
    REQUIRE(view.is_active());
    process_lvgl(30);

    lv_obj_t* key_bar = lv_obj_find_by_name(view.root(), "key_bar");
    REQUIRE(key_bar);

    bool found_third = false;
    const uint32_t n = lv_obj_get_child_count(key_bar);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* entry = lv_obj_get_child(key_bar, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(entry) < 2) {
            continue;
        }
        lv_obj_t* dot = lv_obj_get_child(entry, 0);
        lv_obj_t* label = lv_obj_get_child(entry, 1);
        const std::string text = lv_label_get_text(label);
        if (text.find("Third") == std::string::npos) {
            continue;
        }
        found_third = true;
        CHECK(text.rfind("3 ", 0) == 0);
        CHECK(lv_color_eq(lv_obj_get_style_bg_color(dot, LV_PART_MAIN),
                          helix::ui::object_badge_color(2)));
    }
    CHECK(found_third);

    // The rect's own badge already keyed on the defined index; keep it so.
    lv_obj_t* rect = lv_obj_find_by_name(view.root(), "obj_rect_2");
    REQUIRE(rect);

    view.destroy();
    process_lvgl(30);
}

// ============================================================================
// 2D render: a badge projected through the live transform lands on its object
// ============================================================================

namespace {

// Two hollow squares, 4 layers tall each, far apart on the bed.
ParsedGCodeFile make_two_squares() {
    ParsedGCodeFile f;
    struct Sq {
        const char* name;
        float x0, y0;
    };
    const Sq squares[] = {{"Left", 20.0f, 20.0f}, {"Right", 120.0f, 60.0f}};
    constexpr float kSide = 30.0f;
    constexpr int kLayers = 4;
    for (int li = 0; li < kLayers; ++li) {
        helix::gcode::Layer layer;
        layer.z_height = 0.2f * static_cast<float>(li + 1);
        for (const auto& s : squares) {
            const int16_t idx = f.intern_object_name(s.name);
            const glm::vec3 c[4] = {{s.x0, s.y0, layer.z_height},
                                    {s.x0 + kSide, s.y0, layer.z_height},
                                    {s.x0 + kSide, s.y0 + kSide, layer.z_height},
                                    {s.x0, s.y0 + kSide, layer.z_height}};
            for (int k = 0; k < 4; ++k) {
                helix::gcode::ToolpathSegment seg;
                seg.start = c[k];
                seg.end = c[(k + 1) % 4];
                seg.is_extrusion = true;
                seg.object_name_index = idx;
                layer.segments.push_back(seg);
                layer.bounding_box.expand(seg.start);
                f.global_bounding_box.expand(seg.start);
            }
            ++layer.segment_count_extrusion;
        }
        f.layers.push_back(std::move(layer));
    }
    for (const auto& s : squares) {
        add_parsed_object(f, s.name, {s.x0 + kSide / 2, s.y0 + kSide / 2}, {s.x0, s.y0, 0.2f},
                          {s.x0 + kSide, s.y0 + kSide, 0.2f * kLayers});
    }
    f.total_segments = 4 * 2 * kLayers;
    return f;
}

} // namespace

TEST_CASE("2D badge anchors project onto their own object", "[exclude_badges][layer_renderer]") {
    PrinterExcludedObjectsState state;
    state.init_subjects(false);
    // Klipper names only, defined Right-first, so number order != map order.
    state.set_defined_objects({"Right", "Left"});

    auto parsed = make_two_squares();
    helix::gcode::GCodeLayerRenderer renderer;
    renderer.set_gcode(&parsed);
    renderer.set_canvas_size(400, 300);
    renderer.auto_fit();
    renderer.set_current_layer(static_cast<int>(parsed.layers.size()) - 1);

    const auto badges = compute_object_badges(state, &parsed);
    REQUIRE(badges.size() == 2);
    for (const auto& b : badges) {
        INFO(b.name);
        REQUIRE(b.has_anchor);
        const float z =
            std::min(b.top_z.value_or(renderer.current_layer_z()), renderer.current_layer_z());
        const glm::ivec2 p = renderer.project_to_screen(b.anchor.x, b.anchor.y, z);
        CHECK(p.x >= 0);
        CHECK(p.x < 400);
        CHECK(p.y >= 0);
        CHECK(p.y < 300);

        // Tapping the badge picks the object it labels.
        const auto picked = renderer.pick_object_at(p.x, p.y);
        REQUIRE(picked.has_value());
        CHECK(*picked == b.name);
    }
    state.deinit_subjects();
}
