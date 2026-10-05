// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_api_error_i18n.cpp
 * @brief Error text the network layer hands the UI reaches the user in their language.
 *
 * Two surfaces: MoonrakerError::user_message(), which every error toast appends,
 * and MoonrakerEvent, whose English `message` is built on the WebSocket thread
 * and translated by the presenter from `message_tag` on the main thread.
 */

#include "../lvgl_test_fixture.h"
#include "../test_helpers/abort_manager_test_access.h"
#include "../test_helpers/moonraker_request_tracker_test_access.h"
#include "abort_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "moonraker_error.h"
#include "moonraker_events.h"
#include "moonraker_request_tracker.h"
#include "rpc_error_policy.h"
#include "translation_loader.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {

class ScopedGerman {
  public:
    ScopedGerman() {
        helix::ui::ensure_translation_loaded("de");
        lv_translation_set_language("de");
    }
    ~ScopedGerman() {
        lv_translation_set_language(helix::ui::kIdentityLocale);
    }
    ScopedGerman(const ScopedGerman&) = delete;
    ScopedGerman& operator=(const ScopedGerman&) = delete;
};

} // namespace

TEST_CASE_METHOD(LVGLTestFixture, "MoonrakerError::user_message translates its curated text",
                 "[api-error-i18n][i18n]") {
    ScopedGerman de;

    CHECK(MoonrakerError::timeout("printer.gcode.script", 30000).user_message() ==
          "Zeitüberschreitung der Anfrage. Der Drucker ist möglicherweise beschäftigt.");
    CHECK(MoonrakerError::connection_lost("printer.gcode.script").user_message() ==
          "Verbindung zum Drucker verloren.");

    // Klipper's own words are not ours to translate.
    MoonrakerError klipper;
    klipper.type = MoonrakerErrorType::JSON_RPC_ERROR;
    klipper.message = "Must home axis first";
    CHECK(klipper.user_message() == "Must home axis first");
}

TEST_CASE_METHOD(LVGLTestFixture, "Tracker RPC_ERROR event renders in the active language",
                 "[api-error-i18n][i18n]") {
    helix::AbortManagerTestAccess::reset(helix::AbortManager::instance());
    MoonrakerRequestTracker tracker;

    // No error callback and not silent: the generic RPC_ERROR toast is the only surface.
    PendingRequest request;
    request.id = 4243;
    request.method = "printer.objects.query";
    request.timeout_ms = 60000;
    request.timestamp = std::chrono::steady_clock::now();
    request.intent = helix::rpc_error_policy::CallerIntent{/*silent=*/false,
                                                           /*surfaces_errors=*/false};
    const json error_response = {
        {"jsonrpc", "2.0"},
        {"id", 4243},
        {"error", {{"code", -32601}, {"message", "Klippy not ready"}}},
    };

    std::vector<MoonrakerEvent> events;
    MoonrakerRequestTrackerTestAccess::inject_request(tracker, 4243, request);
    REQUIRE(tracker.route_response(error_response,
                                   [&events](const MoonrakerEvent& e) { events.push_back(e); }));
    REQUIRE(events.size() == 1);
    const MoonrakerEvent& evt = events[0];

    // The log copy stays English whatever the UI language.
    CHECK(evt.message == "Printer command 'printer.objects.query' failed: Klippy not ready");
    REQUIRE(evt.message_tag != nullptr);

    ScopedGerman de;
    CHECK(evt.render(lv_tr(evt.message_tag)) ==
          "Druckerbefehl 'printer.objects.query' fehlgeschlagen: Klippy not ready");
}

TEST_CASE("MoonrakerEvent::render tolerates a translation's placeholder count",
          "[api-error-i18n][i18n]") {
    const MoonrakerEvent evt = MoonrakerEvent::translatable(
        MoonrakerEventType::REQUEST_TIMEOUT, "cmd '{}' after {}ms", {"G28", "5000"}, false);
    CHECK(evt.message == "cmd 'G28' after 5000ms");
    CHECK(evt.render("only {}") == "only G28");
    CHECK(evt.render("{} {} {}") == "G28 5000 {}");
    CHECK(evt.render("{ not a placeholder }") == "{ not a placeholder }");
}
