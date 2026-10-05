// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The System page's rows act on what they show. The XML names its callbacks by
// string and silently skips one that is not registered, so a row whose callback
// went missing stays on screen and does nothing. These tests build the page the
// way the overlay does (callbacks registered, then the XML) and drive each row.

#include "ui_panel_settings.h"
#include "ui_settings_system.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "settings_manager.h"
#include "system_settings_manager.h"

#include "../catch_amalgamated.hpp"

namespace {

struct SystemCallbacksFixture : LVGLUITestFixture {
    lv_obj_t* root_ = nullptr;

    SystemCallbacksFixture() {
        SettingsManager::instance().init_subjects();
        get_global_settings_panel().init_subjects();
        helix::settings::get_system_settings_overlay().init_subjects();
        helix::settings::get_system_settings_overlay().register_callbacks();
        root_ = static_cast<lv_obj_t*>(
            lv_xml_create(test_screen(), "settings_system_overlay", nullptr));
        REQUIRE(root_ != nullptr);
        process_lvgl(5);
    }

    ~SystemCallbacksFixture() override {
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_delete(root_);
        }
        helix::ui::UpdateQueue::instance().drain();
        get_global_settings_panel().deinit_subjects();
        helix::ui::UpdateQueue::instance().drain();
    }

    lv_obj_t* part(const char* row, const char* name) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        lv_obj_t* p = lv_obj_find_by_name(r, name);
        REQUIRE(p != nullptr);
        return p;
    }

    /// Whether the row's own widget has a click handler. An unregistered
    /// callback is skipped when the XML is built, which leaves none.
    bool is_tappable(const char* row) {
        lv_obj_t* r = lv_obj_find_by_name(root_, row);
        REQUIRE(r != nullptr);
        return lv_obj_get_event_count(r) > 0;
    }
};

} // namespace

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: the telemetry toggle drives the setting",
                 "[settings][system_callbacks]") {
    auto& sys = SystemSettingsManager::instance();
    const bool before = sys.get_telemetry_enabled();

    lv_obj_t* toggle = part("row_telemetry", "toggle");
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(sys.get_telemetry_enabled());

    lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    lv_obj_send_event(toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK_FALSE(sys.get_telemetry_enabled());

    sys.set_telemetry_enabled(before);
    helix::ui::UpdateQueue::instance().drain();
}

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: the log-level dropdown applies its choice",
                 "[settings][system_callbacks]") {
    auto& sys = SystemSettingsManager::instance();
    const int before = sys.get_log_level_index();
    const int target = before == 2 ? 1 : 2;

    lv_obj_t* dropdown = part("row_log_level", "dropdown");
    lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(target));
    lv_obj_send_event(dropdown, LV_EVENT_VALUE_CHANGED, nullptr);
    CHECK(sys.get_log_level_index() == target);

    sys.set_log_level_by_index(before);
}

TEST_CASE_METHOD(SystemCallbacksFixture, "System page: every action row has a click handler",
                 "[settings][system_callbacks]") {
    for (const char* row : {"row_security", "row_performance", "row_telemetry_view_data",
                            "row_restart_helix", "row_factory_reset"}) {
        CAPTURE(row);
        CHECK(is_tappable(row));
    }
}
