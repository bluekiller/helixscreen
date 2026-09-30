// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "helix-xml/src/xml/lv_xml_component.h"
#include "plugin_host.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

namespace {
struct HostRig {
    FakeBackend fake;
    json block;
    int writes = 0;
    std::unique_ptr<PluginHost> host;

    explicit HostRig(json initial = json::object(), size_t budget = size_t(64) << 20)
        : block(std::move(initial)) {
        PluginHost::Deps d;
        d.backend = fake.backend();
        d.read_block = [this] { return block; };
        d.write_block = [this](const json& j) {
            block = j;
            ++writes;
        };
        d.settings_path = "/tmp/helix-plugin-host-test/settings.json";
        d.helix_version = "1.1.0";
        d.memory_budget = budget;
        register_plugin_event_callback();
        host = std::make_unique<PluginHost>(std::move(d));
    }

    const PluginInfo* info(const std::string& dir) {
        for (const auto& p : host->plugins()) {
            if (p.dir_name == dir)
                return &p;
        }
        return nullptr;
    }
};

json enabled(const std::string& id, std::vector<std::string> perms) {
    return json{{"enabled", {{id, {{"version", "1.0.0"}, {"permissions", perms}}}}}};
}

void drain() {
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE("memory budget", "[plugin][host]") {
    CHECK(plugin_memory_budget(uint64_t(128) << 20) == size_t(8) << 20);
    CHECK(plugin_memory_budget(uint64_t(4) << 30) == size_t(64) << 20);
    CHECK(plugin_memory_budget(0) == 0);
}

TEST_CASE_METHOD(LVGLTestFixture, "plugins are disabled until enabled", "[plugin][host]") {
    HostRig rig;
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.info("require-test") == nullptr); // no manifest: not a plugin
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "an enabled plugin loads, binds and reacts", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);

    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello__status");
    REQUIRE(status);
    CHECK(std::string(lv_subject_get_string(status)) == "hi");

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "hello__panel", nullptr));
    REQUIRE(panel);
    lv_obj_t* button = lv_obj_find_by_name(panel, "hello_button");
    REQUIRE(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    CHECK(std::string(lv_subject_get_string(status)) == "pressed 7");

    rig.host->dispatch_event("hello__home");
    REQUIRE(rig.fake.requests.size() == 1);
    CHECK(rig.fake.requests[0].a == "G28");
    rig.fake.requests[0].reply(RpcResult{true, {}, {}});
    drain();
    CHECK(std::string(lv_subject_get_string(status)) == "homed");
    lv_obj_delete(panel);
}

TEST_CASE_METHOD(LVGLTestFixture, "unload runs on_unload and leaves nothing registered",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);
    rig.host->unload_all();
    REQUIRE_FALSE(rig.fake.requests.empty());
    CHECK(rig.fake.requests.back().a == "server.info");
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
    CHECK(lv_xml_create(lv_screen_active(), "hello__panel", nullptr) == nullptr);
    rig.host->dispatch_event("hello__press");
    rig.fake.requests.back().reply(RpcResult{true, {}, {}});
    drain();
}

TEST_CASE_METHOD(LVGLTestFixture, "events for other or unknown plugins are ignored",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    lv_subject_t* status = lv_xml_get_subject(nullptr, "hello__status");
    rig.host->dispatch_event("other-plugin_press");
    rig.host->dispatch_event("hello__nosuchhandler");
    rig.host->dispatch_event("garbage");
    rig.host->dispatch_event("");
    CHECK(std::string(lv_subject_get_string(status)) == "hi");
}

TEST_CASE_METHOD(LVGLTestFixture, "fewer granted permissions than requested blocks loading",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {}));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::NeedsApproval);
    CHECK(rig.info("hello")->reason.find("gcode") != std::string::npos);
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);

    CHECK(rig.host->enable("hello"));
    CHECK(rig.info("hello")->status == PluginStatus::Loaded);
    CHECK(rig.block["enabled"]["hello"]["permissions"] == json::array({"gcode"}));
}

TEST_CASE_METHOD(LVGLTestFixture, "an old list-shaped enabled key loads nothing",
                 "[plugin][host]") {
    HostRig rig(json{{"enabled", json::array({"hello"})}});
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK(rig.host->enable("hello"));
    CHECK(rig.block["enabled"].is_object());
}

TEST_CASE_METHOD(LVGLTestFixture, "a directory not matching its id is invalid", "[plugin][host]") {
    HostRig rig(enabled("other-id", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("bad-name"));
    CHECK(rig.info("bad-name")->status == PluginStatus::Invalid);
    CHECK_FALSE(rig.host->enable("other-id"));
}

TEST_CASE_METHOD(LVGLTestFixture, "an incompatible helix_version is not loaded", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    PluginHost::Deps d;
    // hello asks for >=0.0.1; an app reporting 0.0.0 fails that.
    d.backend = rig.fake.backend();
    d.read_block = [&] { return rig.block; };
    d.write_block = [&](const json& j) { rig.block = j; };
    d.helix_version = "0.0.0";
    d.memory_budget = size_t(64) << 20;
    rig.host = std::make_unique<PluginHost>(std::move(d));
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::Incompatible);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin over the memory budget is not loaded",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}), size_t(1) << 20);
    rig.host->load_from("tests/fixtures/plugins");
    CHECK(rig.info("hello")->status == PluginStatus::OverBudget);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin that spins in main.lua faults and unloads",
                 "[plugin][host]") {
    HostRig rig(enabled("looper", {}));
    rig.host->load_from("tests/fixtures/plugins");
    drain();
    CHECK(rig.info("looper")->status == PluginStatus::Faulted);
    CHECK(rig.info("looper")->reason.find("time budget") != std::string::npos);
    CHECK(rig.host->runtime("looper") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "a plugin component that shadows an app component is rejected",
                 "[plugin][host]") {
    REQUIRE(lv_xml_register_component_from_data(
        "shadow__panel", "<component><view extends=\"lv_obj\" width=\"content\" height=\"content\">"
                         "<lv_label name=\"app_child\"/></view></component>"));
    HostRig rig(enabled("shadow", {}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("shadow"));
    CHECK(rig.info("shadow")->status == PluginStatus::Invalid);
    CHECK(rig.info("shadow")->reason.find("already exists") != std::string::npos);
    CHECK(rig.host->runtime("shadow") == nullptr);

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "shadow__panel", nullptr));
    REQUIRE(panel);
    REQUIRE(lv_obj_find_by_name(panel, "app_child"));
    lv_obj_delete(panel);
    lv_xml_component_unregister("shadow__panel");
}

TEST_CASE_METHOD(LVGLTestFixture, "unload leaves an app component that took the plugin's name",
                 "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    REQUIRE(rig.info("hello")->status == PluginStatus::Loaded);
    REQUIRE(lv_xml_register_component_from_data(
        "hello__panel", "<component><view extends=\"lv_obj\" width=\"content\" height=\"content\">"
                        "<lv_label name=\"app_child\"/></view></component>"));
    rig.host->disable("hello");

    auto* panel =
        static_cast<lv_obj_t*>(lv_xml_create(lv_screen_active(), "hello__panel", nullptr));
    REQUIRE(panel);
    CHECK(lv_obj_find_by_name(panel, "app_child"));
    lv_obj_delete(panel);
    lv_xml_component_unregister("hello__panel");
}

TEST_CASE_METHOD(LVGLTestFixture, "plugin XML naming an app callback is rejected at load",
                 "[plugin][host]") {
    HostRig rig(enabled("app-callback", {}));
    rig.host->load_from("tests/fixtures/plugins");
    const PluginInfo* info = rig.info("app-callback");
    REQUIRE(info);
    CHECK(info->status == PluginStatus::Invalid);
    CHECK(info->reason.find("on_estop_clicked") != std::string::npos);
    CHECK(lv_xml_component_get_scope("app-callback__panel") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "disable unloads and forgets consent", "[plugin][host]") {
    HostRig rig(enabled("hello", {"gcode"}));
    rig.host->load_from("tests/fixtures/plugins");
    rig.host->disable("hello");
    CHECK(rig.info("hello")->status == PluginStatus::Disabled);
    CHECK_FALSE(rig.block["enabled"].contains("hello"));
    CHECK(lv_xml_get_subject(nullptr, "hello__status") == nullptr);
}

#endif // HELIX_HAS_PLUGINS
