// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "ui_modal.h"
#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/plugin_test_support.h"
#include "lua_bindings.h"

#include "../catch_amalgamated.hpp"

using namespace helix::plugin;
using namespace helix::plugin::test;

TEST_CASE_METHOD(LVGLTestFixture, "subjects register under the plugin prefix",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        count = helix.subject.int("count", 3)
        label = helix.subject.string("label", "idle")
    )"));
    lv_subject_t* c = lv_xml_get_subject(nullptr, "test-plugin_count");
    lv_subject_t* l = lv_xml_get_subject(nullptr, "test-plugin_label");
    REQUIRE(c);
    REQUIRE(l);
    CHECK(lv_subject_get_int(c) == 3);
    CHECK(std::string(lv_subject_get_string(l)) == "idle");

    REQUIRE(b.t.run(R"(count:set(7); label:set("busy"); got = count:get() .. label:get())"));
    CHECK(lv_subject_get_int(c) == 7);
    CHECK(std::string(lv_subject_get_string(l)) == "busy");
    CHECK(b.t.global("got") == "7busy");
}

TEST_CASE_METHOD(LVGLTestFixture, "observers see changes from C++ and Lua, not registration",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        seen = {}
        local s = helix.subject.int("n", 0)
        s:observe(function(v) seen[#seen + 1] = v end)
        s:set(1)
    )"));
    lv_subject_set_int(lv_xml_get_subject(nullptr, "test-plugin_n"), 2);
    REQUIRE(b.t.run("result = table.concat(seen, ',')"));
    CHECK(b.t.global("result") == "1,2");
}

TEST_CASE_METHOD(LVGLTestFixture, "an observer setting its own subject stops at the depth cap",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    b.t.run(R"(
        local s = helix.subject.int("loop", 0)
        s:observe(function(v) s:set(v + 1) end)
        s:set(1)
    )");
    CHECK(lv_subject_get_int(lv_xml_get_subject(nullptr, "test-plugin_loop")) <= 10);
}

TEST_CASE_METHOD(LVGLTestFixture, "subject names and sizes are validated",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    CHECK_FALSE(b.t.run(R"(helix.subject.int("", 0))"));
    CHECK_FALSE(b.t.run(R"(helix.subject.int("has space", 0))"));
    REQUIRE(b.t.run(R"(helix.subject.int("dup", 0))"));
    // The 3rd error within 60 s faults the runtime, so the size checks need a fresh one.
    CHECK_FALSE(b.t.run(R"(helix.subject.int("dup", 0))"));

    BoundRuntime c({&install_ui_bindings});
    CHECK_FALSE(c.t.run(R"(helix.subject.string("big", string.rep("x", 2000)))"));
    REQUIRE(c.t.run(R"(s = helix.subject.string("small", ""))"));
    CHECK_FALSE(c.t.run(R"(s:set(string.rep("x", 2000)))"));
}

TEST_CASE_METHOD(LVGLTestFixture, "subjects are unregistered when the runtime closes",
                 "[plugin][bindings][ui]") {
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("gone", 1))"));
        REQUIRE(lv_xml_get_subject(nullptr, "test-plugin_gone"));
    }
    CHECK(lv_xml_get_subject(nullptr, "test-plugin_gone") == nullptr);
}

TEST_CASE_METHOD(LVGLTestFixture, "helix.subject bounds subjects per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        for i = 1, 128 do helix.subject.int("s" .. i, 0) end
        ok, err = pcall(helix.subject.int, "one-too-many", 0)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 128 subjects") != std::string::npos);
}

TEST_CASE_METHOD(LVGLTestFixture, "subject observe bounds live observers per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        local s = helix.subject.int("n", 0)
        for i = 1, 256 do s:observe(function() end) end
        ok, err = pcall(s.observe, s, function() end)
    )"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("at most 256") != std::string::npos);
}

namespace {
/// Answers the open confirm dialog the way a user would; every close path releases the
/// plugin's one dialog slot.
void answer_open_confirm(LVGLTestFixture& fx, const char* target, bool backdrop) {
    fx.process_lvgl(50); // the dialog's creation is queued; pump before reading the stack
    helix::ui::UpdateQueue::instance().drain();
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog);
    lv_obj_t* clicked = backdrop ? ModalStack::instance().backdrop_for(dialog)
                                 : lv_obj_find_by_name(dialog, target);
    REQUIRE(clicked);
    lv_obj_send_event(clicked, LV_EVENT_CLICKED, nullptr);
    fx.process_lvgl(50);
    helix::ui::UpdateQueue::instance().drain();
}
} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "helix.ui.confirm allows one open dialog per plugin",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(helix.ui.confirm("One", "body"))"));
    REQUIRE(b.t.run(R"(ok, err = pcall(helix.ui.confirm, "Two", "body"))"));
    CHECK(b.t.global("ok") == "false");
    CHECK(b.t.global("err").find("1 open dialog") != std::string::npos);

    // each close path releases the slot: primary button, cancel button, backdrop dismissal
    answer_open_confirm(*this, "btn_primary", false);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Three", "body", { on_cancel = function() end }))"));
    answer_open_confirm(*this, "btn_secondary", false);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Four", "body"))"));
    answer_open_confirm(*this, nullptr, true);
    REQUIRE(b.t.run(R"(helix.ui.confirm("Five", "body"))"));
    REQUIRE(ModalStack::instance().top_dialog());
}

TEST_CASE_METHOD(LVGLTestFixture, "runtime close leaves an app subject that took the name",
                 "[plugin][bindings][ui]") {
    lv_subject_t app_subject;
    {
        BoundRuntime b({&install_ui_bindings});
        REQUIRE(b.t.run(R"(helix.subject.int("x", 1))"));
        lv_subject_init_int(&app_subject, 5);
        lv_xml_register_subject(nullptr, "test-plugin_x", &app_subject);
    }
    CHECK(lv_xml_get_subject(nullptr, "test-plugin_x") == &app_subject);
    lv_xml_unregister_subject(nullptr, "test-plugin_x");
    lv_subject_deinit(&app_subject);
}

TEST_CASE_METHOD(LVGLTestFixture, "ui.on handlers dispatch with an argument",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        calls = ""
        helix.ui.on("pick", function(arg) calls = calls .. tostring(arg) .. ";" end)
    )"));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::string("3")));
    CHECK(dispatch_ui_handler(*b.t.rt, "pick", std::nullopt));
    CHECK_FALSE(dispatch_ui_handler(*b.t.rt, "missing", std::nullopt));
    CHECK(b.t.global("calls") == "3;nil;");
}

TEST_CASE("plugin_event user_data parsing", "[plugin][bindings][ui]") {
    auto t = parse_plugin_event("orca-cal_start");
    CHECK(t.id == "orca-cal");
    CHECK(t.name == "start");
    CHECK_FALSE(t.arg);

    t = parse_plugin_event("orca-cal_pick:3:4");
    CHECK(t.name == "pick");
    CHECK(t.arg == std::optional<std::string>("3:4"));

    t = parse_plugin_event("orca-cal_my_handler");
    CHECK(t.name == "my_handler");

    CHECK(parse_plugin_event("noowner").id.empty());
    CHECK(parse_plugin_event("_x").id.empty());
    CHECK(parse_plugin_event("orca-cal_").id.empty());
    CHECK(parse_plugin_event("Bad_x").id.empty());
    CHECK(parse_plugin_event("").id.empty());
    CHECK(parse_plugin_event(":x").id.empty());
}

TEST_CASE_METHOD(LVGLTestFixture, "toast and confirm validate their arguments",
                 "[plugin][bindings][ui]") {
    BoundRuntime b({&install_ui_bindings});
    REQUIRE(b.t.run(R"(
        helix.ui.toast("hello")
        helix.ui.toast("careful", "warning")
        helix.ui.confirm("Title", "Body", { severity = "warning", on_confirm = function() end })
    )"));
    CHECK_FALSE(b.t.run(R"(helix.ui.toast("x", "loud"))"));
    CHECK_FALSE(b.t.run(R"(helix.ui.confirm("t", "m", { severity = "loud" }))"));
}

#endif // HELIX_HAS_PLUGINS
