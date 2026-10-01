// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_nav_manager.h"
#include "ui_panel_home.h"

#include "../test_fixtures.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "display_settings_manager.h"
#include "panel_widget_registry.h"
#include "plugin_host.h"
#include "plugin_xml_policy.h"

#include <fstream>
#include <iterator>

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// App subjects (the heater fields the plugin watches) plus a seeded
/// NavigationManager, so one fixture covers the subject cases and the overlay case.
class TempSparkFx : public XMLTestFixture {
  public:
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};

    TempSparkFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~TempSparkFx() override {
        drain();
        process_lvgl(100); // deferred overlay deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
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

int bar(size_t i) {
    const std::string name = "temp-spark__bar" + std::to_string(i);
    return lv_subject_get_int(lv_xml_get_subject(nullptr, name.c_str()));
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

    // Loading already ran the policy; one explicit pass keeps that contract named.
    std::ifstream in("examples/plugins/temp-spark/ui/temp-spark__tile.xml");
    REQUIRE(in);
    std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(check_plugin_xml("temp-spark", {"temp-spark__tile", "temp-spark__detail"}, xml) ==
          std::string());
}

TEST_CASE_METHOD(TempSparkFx, "temp-spark backfills the window from the store",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000); // scale = max(50, peak, 200) * 1.1 = 220
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "server.temperature_store");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    // floor(25 / 220 * 100 + 0.5) = 11, floor(54 / 220 * 100 + 0.5) = 25
    CHECK(bar(1) == 11);
    CHECK(bar(30) == 25);
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
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("extruder_temp", 600);                    // the watch records 60.0 for the next tick
    CHECK(text_subject("temp-spark__value") == "54°"); // sampling waits for the timer
    // helix.timer.every is a chain of one-shot LVGL timers, which
    // lv_timer_handler_safe fires deterministically on the virtual clock.
    process_lvgl(1200);

    // window 26..54,60: floor(26 / 220 * 100 + 0.5) = 12, floor(60 / 220 * 100 + 0.5) = 27
    CHECK(bar(1) == 12);
    CHECK(bar(30) == 27);
    CHECK(text_subject("temp-spark__value") == "60°");
    CHECK(text_subject("temp-spark__min_text") == "26°"); // the 25 left the window
}

TEST_CASE_METHOD(TempSparkFx, "a failed backfill leaves the plugin loaded and empty",
                 "[plugin][example]") {
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    REQUIRE(rig.fake.requests.size() == 1);
    rig.fake.requests[0].reply(RpcResult{false, {}, "store unavailable"});
    drain();

    CHECK(rig.info("temp-spark")->status == PluginStatus::Loaded);
    for (size_t i : {size_t(1), size_t(15), size_t(30)})
        CHECK(bar(i) == 0);
    CHECK(text_subject("temp-spark__value") == "--");
}

TEST_CASE_METHOD(TempSparkFx, "switching heaters refetches for the new store key",
                 "[plugin][example]") {
    set_deci("extruder_target", 2000);
    HostRig rig(spark_block());
    rig.host->load_from("examples/plugins");
    size_t next = 0;
    answer_calls(rig, next, store_series("extruder", 25.0));

    set_deci("bed_target", 600); // scale = max(50, 69, 60) * 1.1 = 75.9
    REQUIRE(rig.host->set_setting("temp-spark", "heater", "heater_bed"));
    REQUIRE(rig.fake.requests.size() == 2);
    answer_calls(rig, next, store_series("heater_bed", 40.0));

    // floor(40 / 75.9 * 100 + 0.5) = 53, floor(69 / 75.9 * 100 + 0.5) = 91
    CHECK(bar(1) == 53);
    CHECK(bar(30) == 91);
    CHECK(text_subject("temp-spark__value") == "69°");
    CHECK(text_subject("temp-spark__target_text") == "60°");
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
