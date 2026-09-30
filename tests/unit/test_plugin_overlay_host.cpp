// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_nav_manager.h"
#include "ui_panel_home.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/home_panel_test_access.h"
#include "../test_helpers/plugin_host_test_support.h"
#include "config.h"
#include "display_settings_manager.h"
#include "lua_runtime.h"
#include "panel_widget_manager.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {

/// LVGLUITestFixture plus a deterministic navigation seed: animations off so the
/// go_back close path runs inline, and panel_stack_[0] holding a stand-in base panel.
class OverlayFx : public LVGLUITestFixture {
  public:
    std::array<lv_obj_t*, UI_PANEL_COUNT> panels{};

    OverlayFx() {
        DisplaySettingsManager::instance().set_animations_enabled(false);
        for (auto& p : panels)
            p = lv_obj_create(lv_screen_active());
        NavigationManager::instance().set_panels(panels.data());
    }
    ~OverlayFx() override {
        drain();
        process_lvgl(100); // deferred overlay deletes
        DisplaySettingsManager::instance().set_animations_enabled(true);
    }
};

} // namespace

TEST_CASE_METHOD(OverlayFx, "a plugin overlay pushes, closes and runs on_close",
                 "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(
        closed = false
        h = helix.ui.overlay("widget-demo__panel", {on_close = function() closed = true end}))",
                           "t"));
    drain();
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("widget-demo") == 1);

    REQUIRE(rt->run_string("h:close()", "t"));
    drain();
    process_lvgl(500); // let the deferred delete land
    CHECK(rig.host->overlays().open_count("widget-demo") == 0);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    lua_getglobal(rt->state(), "closed");
    CHECK(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);
}

TEST_CASE_METHOD(OverlayFx, "unload pops the plugin's overlays", "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(h = helix.ui.overlay("widget-demo__panel"))", "t"));
    drain();
    REQUIRE(NavigationManager::instance().has_open_overlays());
    rig.host->disable("widget-demo");
    drain(); // close_all queues go_back; its body runs on the next queue pass
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("widget-demo") == 0);
}

TEST_CASE_METHOD(OverlayFx, "an overlay of another plugin's component is refused",
                 "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(ok, err = pcall(helix.ui.overlay, "hello__panel"))", "t"));
    lua_getglobal(rt->state(), "ok");
    CHECK_FALSE(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);
    drain();
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("widget-demo") == 0);
}

TEST_CASE_METHOD(OverlayFx, "overlay attributes are policy-checked on the real path",
                 "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(
        ok, err = pcall(helix.ui.overlay, "widget-demo__panel", {bind_text = "hello__s"})
        h = helix.ui.overlay("widget-demo__panel", {title = "Hi"}))",
                           "t"));
    lua_getglobal(rt->state(), "ok");
    CHECK_FALSE(lua_toboolean(rt->state(), -1)); // bind_text must name an owned subject
    lua_pop(rt->state(), 1);
    lua_getglobal(rt->state(), "err");
    const char* err = lua_tostring(rt->state(), -1);
    CHECK(err != nullptr);
    if (err)
        CHECK(std::string(err).find("bind_text") != std::string::npos);
    lua_pop(rt->state(), 1);
    drain();
    CHECK(rig.host->overlays().open_count("widget-demo") == 1); // title passes policy
}

TEST_CASE_METHOD(OverlayFx, "a fault while an overlay is open pops it inside the drain",
                 "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(h = helix.ui.overlay("widget-demo__panel"))", "t"));
    drain();
    REQUIRE(NavigationManager::instance().has_open_overlays());

    // The spin burns the 50 ms entry budget; the fault handler defers on_fault into
    // the queue, so the unload (and its close_all) runs inside a queue drain.
    CHECK_FALSE(rt->run_string("while true do end", "t"));
    drain(); // on_fault -> unload -> close_all queues go_back
    drain(); // the go_back body pops the overlay
    process_lvgl(500);
    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(rig.info("widget-demo")->status == PluginStatus::Faulted);
    CHECK(rig.host->runtime("widget-demo") == nullptr);
}

TEST_CASE_METHOD(OverlayFx, "destroying the host mid-animation still deletes the overlay",
                 "[plugin][overlay]") {
    // A printer switch destroys the host while the app keeps running, so this case
    // needs the real 200 ms slide animations rather than the fixture's inline path.
    DisplaySettingsManager::instance().set_animations_enabled(true);
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(R"(h = helix.ui.overlay("widget-demo__panel"))", "t"));
    drain(); // the push body ran; the slide-in is still animating
    REQUIRE(NavigationManager::instance().has_open_overlays());
    REQUIRE(lv_obj_find_by_name(lv_screen_active(), "widget-demo_panel_status"));

    rig.host.reset();  // unload_all -> close_all -> go_back queued, then records die
    drain();           // go_back body pops and starts the slide-out
    process_lvgl(500); // animation complete -> deferred close callback -> delete
    drain();
    process_lvgl(100); // the deferred root delete is itself an async timer

    CHECK_FALSE(NavigationManager::instance().has_open_overlays());
    CHECK(lv_obj_find_by_name(lv_screen_active(), "widget-demo_panel_status") == nullptr);
}

TEST_CASE_METHOD(OverlayFx,
                 "an overlay opened from Lua survives the covered tile's deactivate hook",
                 "[plugin][overlay]") {
    HostRig rig(enabled("widget-demo", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("widget-demo")->status == PluginStatus::Loaded);

    // The plugin tile placed on the home panel's main page: pushing an overlay over
    // home runs the tile's on_deactivate Lua hook from inside the queue callback that
    // is running the push.
    auto* cfg = helix::Config::get_instance();
    nlohmann::json tile = {{"id", "widget-demo__tile"},
                           {"enabled", true},
                           {"col", 0},
                           {"row", 0},
                           {"colspan", 2},
                           {"rowspan", 2}};
    nlohmann::json home_cfg = {
        {"main_page_index", 0},
        {"next_page_id", 1},
        {"pages", nlohmann::json::array({nlohmann::json{
                      {"id", "main"}, {"widgets", nlohmann::json::array({tile})}}})}};
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/home", home_cfg);
    auto& mgr = helix::PanelWidgetManager::instance();
    mgr.get_widget_config("home").mark_dirty();
    mgr.clear_panel_config("home");

    HomePanel& panel = get_global_home_panel();
    lv_obj_t* page = lv_obj_create(lv_screen_active());
    lv_obj_set_size(page, 400, 300);
    HomePanelTestAccess::set_page_containers(panel, {page});
    HomePanelTestAccess::setup_gate_observers(panel);
    NavigationManager::instance().register_panel_instance(helix::PanelId::Home, &panel);
    panel.populate_widgets();
    drain();

    LuaRuntime* rt = rig.host->runtime("widget-demo");
    REQUIRE(rt);
    REQUIRE(rt->run_string(
        R"(helix.ui.on("open", function() helix.ui.overlay("widget-demo__panel") end))", "t"));
    rig.host->dispatch_event("widget-demo__open");
    drain(); // push body -> home deactivated -> the tile's deactivate hook re-enters Lua
    CHECK(NavigationManager::instance().has_open_overlays());
    CHECK(rig.host->overlays().open_count("widget-demo") == 1);
    CHECK_FALSE(rt->faulted());

    // Sensible order: the tile attached and activated first, deactivated last, and the
    // runtime still answers afterwards - no entry or budget was lost to the re-entry.
    REQUIRE(rt->run_string("log = table.concat(events, ','); after = true", "t"));
    lua_getglobal(rt->state(), "log");
    std::string log = lua_tostring(rt->state(), -1);
    lua_pop(rt->state(), 1);
    CHECK(log.find("attach") == 0);
    CHECK(log.substr(log.size() - 10) == "deactivate");
    lua_getglobal(rt->state(), "after");
    CHECK(lua_toboolean(rt->state(), -1));
    lua_pop(rt->state(), 1);

    rig.host->disable("widget-demo");
    drain();
    NavigationManager::instance().register_panel_instance(helix::PanelId::Home, nullptr);
    helix::PanelWidgetManager::clear_gate_observers("home");
    HomePanelTestAccess::clear_page_containers(panel);
    // A null panel_widgets node reads as absent everywhere, restoring the
    // default-placement view for later tests in this process.
    cfg->set<nlohmann::json>(cfg->df() + "panel_widgets/home", nlohmann::json());
    mgr.get_widget_config("home").mark_dirty();
    mgr.clear_panel_config("home");
    lv_obj_delete(page);
    drain();
    process_lvgl(500);
}

#endif // HELIX_HAS_PLUGINS
