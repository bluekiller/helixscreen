// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_printer_retarget.cpp
 * @brief Pointing the live connection at another printer drops what the last one left.
 */

#include "ui_update_queue.h"

#include "../fake_moonraker_client.h"
#include "../lvgl_test_fixture.h"
#include "../test_helpers/config_test_access.h"
#include "../test_helpers/moonraker_manager_test_access.h"
#include "../test_helpers/scoped_moonraker_client.h"
#include "ams_backend_mock.h"
#include "ams_state.h"
#include "app_globals.h"
#include "config.h"
#include "moonraker_manager.h"
#include "printer_retarget.h"
#include "printer_state.h"

#include <memory>
#include <string>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

namespace {

class RetargetFixture : public LVGLTestFixture {
  public:
    RetargetFixture() {
        auto fake = std::make_unique<helix::test::FakeMoonrakerClient>();
        client_ = fake.get();
        helix::MoonrakerManagerTestAccess::install_client(manager_, std::move(fake));
        set_moonraker_manager(&manager_);
        installed_ = std::make_unique<ScopedMoonrakerClient>(client_);

        cfg_ = helix::Config::get_instance();
        saved_data_ = helix::ConfigTestAccess::data(*cfg_);
        saved_active_ = helix::ConfigTestAccess::active_printer_id(*cfg_);
        nlohmann::json data;
        data["config_version"] = 3;
        data["active_printer_id"] = "beta";
        data["printers"]["alpha"]["printer_name"] = "Alpha";
        data["printers"]["beta"]["printer_name"] = "Beta";
        data["printers"]["beta"]["moonraker_host"] = "10.0.0.2";
        data["printers"]["beta"]["moonraker_port"] = 7126;
        helix::ConfigTestAccess::data(*cfg_) = data;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = "beta";

        get_printer_state().init_subjects(false);
        get_printer_state().set_active_printer_name("Alpha");

        auto& ams = helix::AmsState::instance();
        ams.init_subjects(false);
        ams.set_backend(std::make_unique<helix::AmsBackendMock>());
    }

    ~RetargetFixture() override {
        helix::set_connect_gate(nullptr);
        helix::AmsState::instance().set_backend(nullptr);
        helix::ui::UpdateQueue::instance().drain();
        helix::ConfigTestAccess::data(*cfg_) = saved_data_;
        helix::ConfigTestAccess::active_printer_id(*cfg_) = saved_active_;
        installed_.reset();
        set_moonraker_manager(nullptr);
    }

    static std::string shown_name() {
        return lv_subject_get_string(get_printer_state().get_active_printer_name_subject());
    }

    helix::test::FakeMoonrakerClient* client_ = nullptr;

  private:
    MoonrakerManager manager_;
    std::unique_ptr<ScopedMoonrakerClient> installed_;
    helix::Config* cfg_ = nullptr;
    nlohmann::json saved_data_;
    std::string saved_active_;
};

} // namespace

TEST_CASE_METHOD(RetargetFixture, "Retarget: connects to the active printer as a new printer",
                 "[multi-printer][retarget]") {
    REQUIRE(helix::AmsState::instance().backend_count() == 1);
    REQUIRE(shown_name() == "Alpha");

    CHECK(helix::retarget_printer_connection());

    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
    CHECK(helix::AmsState::instance().backend_count() == 0);
    CHECK(shown_name() == "Beta");
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: a closed connect gate leaves it disconnected",
                 "[multi-printer][retarget]") {
    bool asked = false;
    helix::set_connect_gate([&] {
        asked = true;
        // The previous printer's state is already gone when the gate is asked.
        CHECK(helix::AmsState::instance().backend_count() == 0);
        return false;
    });

    CHECK_FALSE(helix::retarget_printer_connection());

    CHECK(asked);
    CHECK(client_->get_last_url().empty());
}

TEST_CASE_METHOD(RetargetFixture, "Reconnect: a closed connect gate leaves it disconnected",
                 "[multi-printer][retarget]") {
    helix::set_connect_gate([] { return false; });

    CHECK_FALSE(helix::reconnect_active_printer());
    CHECK(client_->get_last_url().empty());
}

TEST_CASE_METHOD(RetargetFixture, "Reconnect: the same printer keeps its AMS backends",
                 "[multi-printer][retarget]") {
    CHECK(helix::reconnect_active_printer());

    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
    CHECK(helix::AmsState::instance().backend_count() == 1);
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: a transport that cannot start reports failure",
                 "[multi-printer][retarget]") {
    client_->connect_result = -1;

    CHECK_FALSE(helix::retarget_printer_connection());
    CHECK(client_->get_last_url() == "ws://10.0.0.2:7126/websocket");
}

TEST_CASE_METHOD(RetargetFixture, "Retarget: the active printer's WebSocket URL",
                 "[multi-printer][retarget]") {
    CHECK(helix::active_printer_ws_url() == "ws://10.0.0.2:7126/websocket");
}
