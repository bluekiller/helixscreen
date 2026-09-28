// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

// The wizard's WiFi step reads the connection through
// WiFiManager::get_status_async(): on wpa_supplicant a status read is a
// control-socket round trip that can wait 10s, so no path may make one on the
// UI thread.

#include "ui_wizard_wifi.h"

#include "../lvgl_ui_test_fixture.h"
#include "../test_helpers/scoped_runtime_config.h"
#include "../test_helpers/wizard_wifi_test_access.h"
#include "http_executor.h"
#include "runtime_config.h"
#include "wifi_backend_mock.h"
#include "wifi_manager.h"

#include <memory>
#include <string>
#include <thread>

#include "../catch_amalgamated.hpp"

using Access = WizardWifiStepTestAccess;

namespace {

constexpr const char* kSsid = "HomeNet";
constexpr const char* kIp = "192.168.1.77";
constexpr const char* kMockMac = "MAC: de:ad:be:ef:ca:fe"; // WifiBackendMock::get_status

bool names(const std::string& text, const std::string& ssid) {
    return text.find(ssid) != std::string::npos;
}

class WizardWifiStatusFixture : public LVGLUITestFixture {
  protected:
    ScopedRuntimeConfig scoped_config;
    WifiBackendMock* wifi = nullptr;

    WizardWifiStatusFixture() {
        get_runtime_config()->test_mode = true;
        get_runtime_config()->use_real_wifi = false;
        helix::http::HttpExecutor::fast().start();

        step_.init_subjects();
        step_.register_callbacks();
        REQUIRE(step_.create(test_screen()) != nullptr);

        auto backend = std::make_unique<WifiBackendMock>();
        REQUIRE(backend->start().success());
        wifi = backend.get();
        manager_ = std::make_shared<helix::WiFiManager>(std::move(backend), /*silent=*/true);
        manager_->init_self_reference(manager_);
        Access::wifi_manager(step_) = manager_;

        wifi->set_connected_state(true, kSsid, kIp, 70);
        wifi->clear_status_callers();
    }

    ~WizardWifiStatusFixture() override {
        wifi->release_held_status();
    }

    WizardWifiStep& step() {
        return step_;
    }

    bool no_read_on_this_thread() {
        const auto callers = wifi->status_callers();
        REQUIRE_FALSE(callers.empty());
        for (const auto& id : callers) {
            if (id == std::this_thread::get_id()) {
                return false;
            }
        }
        return true;
    }

    bool shows_connection() {
        return names(Access::status(step_), kSsid) && Access::ip(step_) == kIp &&
               Access::mac(step_) == kMockMac;
    }

  private:
    WizardWifiStep step_;
    std::shared_ptr<helix::WiFiManager> manager_;
};

} // namespace

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: applying backend state reads the connection off the UI thread",
                 "[wizard_wifi][wifi_status_async]") {
    Access::apply_backend_state(step());
    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: the network list marks the connected network from an async read",
                 "[wizard_wifi][wifi_status_async]") {
    Access::refresh_list(step(), {WiFiNetwork("Other", 90, true), WiFiNetwork(kSsid, 70, true)});

    auto connected_rows = [&]() {
        lv_obj_t* list = Access::network_list(step());
        int n = 0;
        for (uint32_t i = 0; list && i < lv_obj_get_child_count(list); ++i) {
            if (lv_obj_has_state(lv_obj_get_child(list, i), LV_STATE_CHECKED)) {
                n++;
            }
        }
        return n;
    };
    REQUIRE(wait_until([&]() { return connected_rows() == 1 && shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: a successful join shows its address from an async read",
                 "[wizard_wifi][wifi_status_async]") {
    Access::set_current_ssid(step(), kSsid);
    Access::announce_connected(step());
    REQUIRE(wait_until([&]() { return shows_connection(); }));
    CHECK(no_read_on_this_thread());
}

TEST_CASE_METHOD(WizardWifiStatusFixture,
                 "wizard wifi: a connection read landing after WiFi is turned off is ignored",
                 "[wizard_wifi][wifi_status_async]") {
    wifi->hold_next_status();
    Access::apply_backend_state(step());
    REQUIRE(wait_until([&]() { return !wifi->status_callers().empty(); }));

    lv_subject_set_int(&Access::wifi_enabled(step()), 0);
    wifi->release_held_status();
    CHECK_FALSE(wait_until([&]() { return names(Access::status(step()), kSsid); }, 300));
}
