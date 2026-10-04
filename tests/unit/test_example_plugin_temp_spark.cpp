// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_nav_manager.h"
#include "ui_panel_home.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "display_settings_manager.h"
#include "panel_widget_registry.h"
#include "plugin_canvas.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"

#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using Catch::Approx;
using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// App subjects (the heater fields the plugin watches) plus a seeded
/// NavigationManager, so one fixture covers the subject cases and the overlay case.
class TempSparkFx : public XMLTestFixture {
  public:
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};
    lv_obj_t* tile = nullptr;

    TempSparkFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~TempSparkFx() override {
        if (tile)
            lv_obj_delete(tile);
        drain();
        process_lvgl(100); // deferred overlay deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }

    /// The home tile at its 2x1 size, laid out and drained: the spark canvas
    /// reports its content size and the plugin redraws for it.
    lv_obj_t* make_tile() {
        tile =
            static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "temp-spark__tile", nullptr));
        REQUIRE(tile);
        lv_obj_set_size(tile, 400, 200);
        lv_obj_update_layout(tile);
        drain();
        return tile;
    }
};

/// The enabled block plus a 1 s sample interval, so a timer tick is one wait_ms away.
json spark_block() {
    json block = enabled("temp-spark", {});
    block["settings"]["temp-spark"] = {
        {"heater", "extruder"}, {"interval_s", 1}, {"show_target", true}};
    return block;
}

/// A store payload with one heater whose temperatures count from `from`.
json store_series(const std::string& heater, double from) {
    std::vector<double> temps;
    for (int i = 0; i < 30; ++i)
        temps.push_back(from + i);
    return json{{heater, {{"temperatures", temps}}}};
}

/// The committed spark polyline's points, oldest first; empty when the spark
/// canvas holds no single-polyline list.
std::vector<lv_point_precise_t> spark_points() {
    const DisplayList* list = canvas_committed("temp-spark__spark");
    if (!list || list->prims.size() != 1 || list->prims[0].op != CanvasOp::Polyline)
        return {};
    const CanvasPrim& p = list->prims[0];
    return {list->points.begin() + p.first, list->points.begin() + p.first + p.count};
}

std::string text_subject(const char* name) {
    return lv_subject_get_string(lv_xml_get_subject(nullptr, name));
}

void set_deci(const char* subject, int deci) {
    lv_subject_set_int(lv_xml_get_subject(nullptr, subject), deci);
}

/// Answers the call requests since `next`, in order, with `value`.
void answer_calls(HostRig& rig, size_t& next, const json& value) {
    while (next < rig.fake.requests.size()) {
        auto& r = rig.fake.requests[next++];
        if (r.kind == "call")
            r.reply(RpcResult{true, value, {}});
    }
    drain();
}

} // namespace

TEST_CASE_METHOD(TempSparkFx, "temp-spark loads with a registered 2x1 widget",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.info("temp-spark"));
    CHECK(rig.info("temp-spark")->status == PluginStatus::Loaded);

    const helix::PanelWidgetDef* def = helix::find_widget_def("temp-spark__tile");
    REQUIRE(def);
    CHECK(def->category == helix::WidgetCategory::Plugins);
    CHECK(def->colspan == 4); // two cells, in tracks
    CHECK(def->rowspan == 2);
    rig.host->disable("temp-spark");
    CHECK(helix::find_widget_def("temp-spark__tile") == nullptr);

    // Loading already ran the policy; one explicit pass per UI file keeps that
    // contract named.
    for (const char* file : {"temp-spark__tile", "temp-spark__detail"}) {
        const std::string path = std::string("examples/plugins/temp-spark/ui/") + file + ".xml";
        CAPTURE(path);
        std::ifstream in(path);
        REQUIRE(in);
        std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(check_plugin_xml("temp-spark", {"temp-spark__tile", "temp-spark__detail"}, xml) ==
              std::string());
    }
}

TEST_CASE_METHOD(TempSparkFx, "temp-spark backfills the window from the store",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000); // scale = max(50, peak, 200) * 1.1 = 220
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile(); // the spark canvas knows its size before the window arrives
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "server.temperature_store");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    const auto [w, h] = canvas_size("temp-spark__spark");
    REQUIRE(w > 1);
    REQUIRE(h > 1);
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    // A full window spans the whole width: 25 degrees at x 0, 54 at the edge.
    CHECK(pts[0].x == Approx(0.0).margin(0.01));
    CHECK(pts[0].y == Approx((h - 1) * (1 - 25.0 / 220)).margin(0.01));
    CHECK(pts[29].x == Approx(w - 1).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - 54.0 / 220)).margin(0.01));
    CHECK(text_subject("temp-spark__value") == "54°");
    CHECK(text_subject("temp-spark__min_text") == "25°");
    CHECK(text_subject("temp-spark__max_text") == "54°");
    CHECK(text_subject("temp-spark__target_text") == "200°");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "temp-spark__show_target")) == 1);
}

TEST_CASE_METHOD(TempSparkFx, "a live reading shifts the window on the timer",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("extruder_temp", 600);                    // the watch records 60.0 for the next tick
    CHECK(text_subject("temp-spark__value") == "54°"); // sampling waits for the timer
    // helix.timer.every is a chain of one-shot LVGL timers, which
    // lv_timer_handler_safe fires deterministically on the virtual clock.
    process_lvgl(1200);

    // Window 26..54, 60: the oldest point rose, the newest sits at the edge.
    const auto [w, h] = canvas_size("temp-spark__spark");
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    CHECK(pts[0].x == Approx(0.0).margin(0.01));
    CHECK(pts[0].y == Approx((h - 1) * (1 - 26.0 / 220)).margin(0.01));
    CHECK(pts[29].x == Approx(w - 1).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - 60.0 / 220)).margin(0.01));
    CHECK(text_subject("temp-spark__value") == "60°");
    CHECK(text_subject("temp-spark__min_text") == "26°"); // the 25 left the window
}

TEST_CASE_METHOD(TempSparkFx, "a failed backfill leaves the plugin loaded and empty",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    REQUIRE(rig.fake.requests.size() == 1);
    rig.fake.requests[0].reply(RpcResult{false, {}, "store unavailable"});
    drain();

    CHECK(rig.info("temp-spark")->status == PluginStatus::Loaded);
    const DisplayList* spark = canvas_committed("temp-spark__spark");
    CHECK((spark == nullptr || spark->prims.empty()));
    CHECK(text_subject("temp-spark__value") == "--");

    // Live sampling fills the window from the right edge: two ticks give a
    // two-point line, the newest at the edge and the first where sample 29 of
    // 30 belongs.
    set_deci("extruder_temp", 300);
    process_lvgl(1200);
    set_deci("extruder_temp", 310);
    process_lvgl(1200);
    const int32_t w = canvas_size("temp-spark__spark").first;
    REQUIRE(w > 1);
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 2);
    CHECK(pts[0].x == Approx(28.0 * (w - 1) / 29).margin(0.01));
    CHECK(pts[1].x == Approx(w - 1).margin(0.01));
}

TEST_CASE_METHOD(TempSparkFx, "switching heaters refetches for the new store key",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    make_tile();
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("bed_target", 600); // scale = max(50, 69, 60) * 1.1 = 75.9
    REQUIRE(rig.host->set_setting("temp-spark", "heater", "heater_bed"));
    REQUIRE(rig.fake.requests.size() == 2);
    answer_calls(rig, next, store_series("heater_bed", 40.0));

    const auto [w, h] = canvas_size("temp-spark__spark");
    const std::vector<lv_point_precise_t> pts = spark_points();
    REQUIRE(pts.size() == 30);
    CHECK(pts[0].y == Approx((h - 1) * (1 - 40.0 / 75.9)).margin(0.01));
    CHECK(pts[29].x == Approx(w - 1).margin(0.01));
    CHECK(pts[29].y == Approx((h - 1) * (1 - 69.0 / 75.9)).margin(0.01));
    CHECK(text_subject("temp-spark__value") == "69°");
    CHECK(text_subject("temp-spark__target_text") == "60°");
}

TEST_CASE_METHOD(TempSparkFx, "the detail graph draws the target line", "[plugin][example]") {
    set_deci("extruder_target", 2000); // scale = max(50, peak, 200) * 1.1 = 220
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    rig.host->dispatch_event("temp-spark__open");
    drain();
    lv_obj_t* graph = lv_obj_find_by_name(lv_screen_active(), "temp-spark__graph");
    REQUIRE(graph);
    lv_obj_update_layout(graph);
    drain();

    const auto [w, h] = canvas_size("temp-spark__graph");
    REQUIRE(w > 1);
    REQUIRE(h > 1);
    const DisplayList* list = canvas_committed("temp-spark__graph");
    REQUIRE(list);
    const CanvasPrim* line = nullptr;
    size_t polylines = 0;
    for (const CanvasPrim& p : list->prims) {
        if (p.op == CanvasOp::Polyline)
            ++polylines;
        else if (p.op == CanvasOp::Line)
            line = &p;
    }
    CHECK(polylines == 1);
    REQUIRE(line);
    CHECK(line->b == Approx((h - 1) * (1 - 200.0 / 220)).margin(0.01));
    CHECK(line->d == Approx((h - 1) * (1 - 200.0 / 220)).margin(0.01));

    // show_target off removes the line and keeps the window.
    REQUIRE(rig.host->set_setting("temp-spark", "show_target", false));
    drain();
    list = canvas_committed("temp-spark__graph");
    REQUIRE(list);
    line = nullptr;
    polylines = 0;
    for (const CanvasPrim& p : list->prims) {
        if (p.op == CanvasOp::Polyline)
            ++polylines;
        else if (p.op == CanvasOp::Line)
            line = &p;
    }
    CHECK(line == nullptr);
    CHECK(polylines == 1);
}

TEST_CASE_METHOD(TempSparkFx, "the tile event opens the detail overlay until unload",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    rig.host->dispatch_event("temp-spark__open");
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("temp-spark") == 1);

    rig.host->disable("temp-spark");
    drain(); // close_all queues go_back; its body runs on the next queue pass
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("temp-spark") == 0);
}

#endif // HELIX_HAS_PLUGINS
