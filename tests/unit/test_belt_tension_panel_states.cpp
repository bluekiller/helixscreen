// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_belt_tension_panel_states.cpp
 * @brief The belt tension panel's four view states, its run queue and its gate.
 *
 * Drives the real singleton panel through MoonrakerClientMock's TEST_RESONANCES
 * transcript and asserts on the bt_* subjects the XML binds:
 *
 * 1. Start queues both paths, runs them in order and lands on RESULTS with
 *    the similarity headline, verdict, paired-peak facts and chart markers
 *    derived from compare_belt_paths() over the printer's sweep range.
 * 2. Re-test keeps the sibling path's run and ghosts the re-measured path's
 *    previous curve.
 * 3. Every mock failure mode and a silent sweep reach ERROR; a path whose
 *    sweep never rises above the peak floor is an error naming the path.
 * 4. Deactivating mid-run cancels listening and returns to START.
 * 5. Start is disabled while klippy is not READY, and re-arms.
 * 6. EMBEDDED tier never creates a chart.
 *
 * The XML side (containers, buttons, subject bindings) is pinned in the
 * container/binding cases below.
 */

#include "ui_belt_path_sketch.h"
#include "ui_frequency_response_chart.h"
#include "ui_modal.h"
#include "ui_panel_belt_tension.h"
#include "ui_update_queue.h"

#include "../test_fixtures.h"
#include "../test_helpers/mock_kinematics_env.h"
#include "../test_helpers/printer_state_test_access.h"
#include "app_globals.h"
#include "belt_tension_types.h"
#include "lvgl/lvgl.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <cmath>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::UpdateQueue;

/// Registers the bt_* subjects the panel binds, seeds the global PrinterState
/// the gate observes, and wires the panel to a mock client. One instance per
/// TEST_CASE; the destructor unwires in the reverse order.
class BeltPanelFixture : public XMLTestFixture {
  public:
    explicit BeltPanelFixture(helix::PlatformTier tier = helix::PlatformTier::STANDARD)
        : client_(MoonrakerClientMock::PrinterType::VORON_24), api_(client_, api_state_) {
        // A previous test's mock run in THIS process may have left the
        // per-PID belt CSVs behind; the terminal-line path must start absent.
        MoonrakerClientMock::remove_belt_csvs();
        client_.set_belt_line_interval_ms(1);

        api_state_.init_subjects(false);
        api_state_.set_klippy_state_sync(KlippyState::READY);

        // The panel observes the GLOBAL PrinterState, not this fixture's own;
        // seed it the way a live connection would.
        PrinterStateTestAccess::reset(get_printer_state());
        get_printer_state().init_subjects(false);
        // Seed the INPUTS a live connection produces, not the derived subject:
        // nav_buttons_enabled is recomputed from connection state + klippy on
        // every klippy transition, so poking it directly would silently close
        // the gate again the moment klippy state changed.
        get_printer_state().set_printer_connection_state(
            static_cast<int>(ConnectionState::CONNECTED), nullptr);
        get_printer_state().set_klippy_state_sync(KlippyState::READY);
        lv_subject_set_int(get_printer_state().get_print_active_subject(), 0);
        lv_subject_copy_string(get_printer_state().get_homed_axes_subject(), "xyz");
        set_accel_subject(1);

        panel_ = &get_global_belt_tension_panel();
        ui_panel_belt_tension_register_callbacks();
        panel_->deinit_subjects();
        panel_->init_subjects();
        panel_->set_api(&client_, &api_);
        panel_->set_render_tier_for_test(tier, true);

        helix::ui::register_belt_path_sketch_widget();
        REQUIRE(register_component("header_bar"));
        REQUIRE(register_component("panel_belt_tension"));
        view_ = panel_->create(test_screen());
        REQUIRE(view_ != nullptr);
        UpdateQueue::instance().drain();

        panel_->on_activate();
        UpdateQueue::instance().drain();
    }

    ~BeltPanelFixture() override {
        panel_->on_deactivate(DeactivateReason::NavigateAway);
        UpdateQueue::instance().drain();
        // Tears the widget tree down through the panel (nulls its cached root,
        // destroys the chart while the tree is still whole) BEFORE this
        // fixture's base deletes the screen underneath it.
        panel_->destroy_overlay_ui(view_);
        UpdateQueue::instance().drain();
        panel_->set_api(nullptr, nullptr);
        panel_->deinit_subjects();
        MoonrakerClientMock::remove_belt_csvs();
    }

    BeltTensionPanel& panel() {
        return *panel_;
    }
    MoonrakerClientMock& mock() {
        return client_;
    }
    lv_obj_t* view() {
        return view_;
    }

    /// Friendship pass-throughs: the chart and ghost series are not exposed
    /// by any subject, so the ghost case reads them directly.
    ui_frequency_response_chart_t* panel_chart() {
        return panel_->chart_;
    }
    int panel_ghost_id(int path_index) {
        return panel_->ghost_series_[path_index];
    }
    int panel_series_id(int path_index) {
        return panel_->series_[path_index];
    }
    lv_obj_t* panel_low_ram_dialog() {
        return panel_->low_ram_dialog_;
    }

    int state_int(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    std::string text(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        INFO("subject not registered: " << name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }

    /// Advance virtual time in 100ms steps, pumping LVGL timers and the
    /// UpdateQueue so mock sweep lines, guard timers and marshalled callbacks
    /// all get their turn.
    void pump_ms(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 100) {
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
    }
    bool pump_until_state(int want, uint32_t budget_ms = 60000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (state_int("belt_tension_state") == want) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return state_int("belt_tension_state") == want;
    }
    /// Pumps until the gate opens (hardware detect + config probe answered).
    bool wait_gate_open(uint32_t budget_ms = 10000) {
        for (uint32_t t = 0; t < budget_ms; t += 100) {
            if (state_int("bt_can_start") == 1) {
                return true;
            }
            lv_tick_inc(100);
            lv_timer_handler_safe();
            UpdateQueue::instance().drain();
        }
        return state_int("bt_can_start") == 1;
    }

    void set_klippy_ready(bool ready) {
        get_printer_state().set_klippy_state_sync(ready ? KlippyState::READY
                                                        : KlippyState::SHUTDOWN);
        pump_ms(2000);
    }

  private:
    /// printer_has_accelerometer lives on PrinterCapabilitiesState, which unit
    /// tests never register; provide the name the panel looks up.
    static void set_accel_subject(int value) {
        static lv_subject_t s;
        lv_subject_init_int(&s, value);
        lv_xml_register_subject(nullptr, "printer_has_accelerometer", &s);
    }

    MoonrakerClientMock client_;
    PrinterState api_state_;
    MoonrakerAPIMock api_;
    BeltTensionPanel* panel_ = nullptr;
    lv_obj_t* view_ = nullptr;
};

// ============================================================================
// XML shape
// ============================================================================

TEST_CASE("belt tension panel has a container for every view state", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    // One per ViewState value, in enum order.
    for (const char* name : {"state_start", "state_running", "state_results", "state_error"}) {
        INFO("missing state container: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // Every named object the state machine and ctl driving depend on.
    for (const char* name :
         {"btn_start", "btn_stop", "btn_retest_a", "btn_retest_b", "btn_test_both", "btn_retry",
          "bt_similarity", "bt_facts_label", "bt_unpaired_label", "chart_host_running",
          "chart_host_results", "bt_sketch", "error_label"}) {
        INFO("missing named object: " << name);
        CHECK(lv_obj_find_by_name(fx.view(), name) != nullptr);
    }
    // The pluck-tuner states these replaced must be gone, not left alongside.
    CHECK(lv_obj_find_by_name(fx.view(), "state_position") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_listen") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "state_compare") == nullptr);
    // The direction rail is gone until real captures show a dependable
    // signed signal.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_rail") == nullptr);
    // The per-path hero numbers are gone: the strongest pair's frequencies say
    // the least about the belts, so similarity is the only headline number.
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_a") == nullptr);
    CHECK(lv_obj_find_by_name(fx.view(), "bt_peak_b") == nullptr);
}

TEST_CASE("belt tension panel binds only subjects that exist", "[belt][panel][xml]") {
    BeltPanelFixture fx;

    for (const char* name :
         {"belt_tension_state", "bt_can_start", "bt_gate_message", "bt_hw_kinematics",
          "bt_hw_accel", "bt_hw_sweep", "bt_run_title", "bt_run_detail", "bt_running_path",
          "bt_note_a", "bt_note_b", "bt_verdict", "bt_verdict_text", "bt_similarity", "bt_facts",
          "bt_unpaired", "bt_has_unpaired", "bt_chart_available", "bt_error_message"}) {
        INFO("subject not registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) != nullptr);
    }
    // Retired with the pluck tuner. Leaving them registered would let a stale
    // binding survive review by continuing to resolve.
    for (const char* name : {"bt_hw_adxl", "bt_live_freq", "bt_median_freq", "bt_committed",
                             "bt_pluck_count", "bt_rail_value"}) {
        INFO("retired subject still registered: " << name);
        CHECK(lv_xml_get_subject(nullptr, name) == nullptr);
    }
}

// ============================================================================
// Run flow
// ============================================================================

TEST_CASE("Start runs A then B and lands on RESULTS", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_peaks_hz(104.0f, 98.0f);
    REQUIRE(fx.wait_gate_open());

    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));

    // With no previous run the note names the age and nothing else.
    CHECK(fx.text("bt_note_a") == "just now");
    CHECK(fx.text("bt_note_b") == "just now");
    CHECK(fx.state_int("bt_verdict") == static_cast<int>(helix::calibration::BeltVerdict::CLOSE));
    CHECK(fx.text("bt_verdict_text") == "Fair match");

    // The headline is the similarity; the facts line lists the pairs, A/B.
    const std::string similarity = fx.text("bt_similarity");
    CHECK(similarity.back() == '%');
    CHECK(fx.text("bt_facts").find("104/98") != std::string::npos);

    // The listed pairs are numbered on both curves, the strongest as 1.
    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    const auto marks_a = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(0));
    const auto marks_b = ui_frequency_response_chart_get_markers(chart, fx.panel_series_id(1));
    REQUIRE_FALSE(marks_a.empty());
    REQUIRE_FALSE(marks_b.empty());
    CHECK(marks_a.front().number == 1);
    CHECK(marks_a.front().freq_hz == Catch::Approx(104.0f).margin(1.0f));
    CHECK(marks_b.front().number == 1);
    CHECK(marks_b.front().freq_hz == Catch::Approx(98.0f).margin(1.0f));
}

TEST_CASE("Re-test A keeps B and ghosts the old A", "[belt][panel][chart]") {
    BeltPanelFixture fx; // mock defaults: A=110, B=98
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // 110 and 98 sit 12 Hz apart, past the 10 Hz pairing cap, so the only pair
    // the defaults form is the ~42 Hz rig peak both curves share, and each
    // belt peak is listed as unpaired.
    const std::string similarity_before = fx.text("bt_similarity");
    REQUIRE(similarity_before.back() == '%');
    CHECK(fx.state_int("bt_has_unpaired") == 1);
    CHECK(fx.text("bt_unpaired").find("Only on A: 110") != std::string::npos);
    CHECK(fx.text("bt_unpaired").find("Only on B: 98") != std::string::npos);

    fx.panel().handle_retest_clicked(helix::calibration::BeltPath::PATH_A);
    REQUIRE(fx.state_int("belt_tension_state") ==
            static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
    CHECK(fx.text("bt_run_title") == "Re-measuring Path A");
    // A re-measure walks A toward B by at most 4 Hz: 110 -> 106, now 8 Hz from
    // B, inside the cap: the belt-hump pair outranks the 42 Hz one.
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    // "was" repeats the similarity the comparison showed before the re-measure.
    CHECK(fx.text("bt_note_a").find("was " + similarity_before) != std::string::npos);
    CHECK(fx.state_int("bt_has_unpaired") == 0);

    auto* chart = fx.panel_chart();
    REQUIRE(chart != nullptr);
    CHECK(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(0)));
    CHECK_FALSE(ui_frequency_response_chart_is_series_visible(chart, fx.panel_ghost_id(1)));
}

TEST_CASE("each mock failure reaches ERROR", "[belt][panel]") {
    BeltPanelFixture fx;
    for (const BeltMockFailure failure :
         {BeltMockFailure::ERROR, BeltMockFailure::NOFILE, BeltMockFailure::MULTICHIP}) {
        fx.mock().set_belt_failure(failure);
        REQUIRE(fx.wait_gate_open());
        fx.panel().handle_start_clicked();
        REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
        CHECK(fx.text("bt_error_message").empty() == false);

        fx.panel().handle_retry_clicked();
        CHECK(fx.state_int("belt_tension_state") ==
              static_cast<int>(BeltTensionPanel::ViewState::START));
    }
}

TEST_CASE("a stall trips the stall guard", "[belt][panel]") {
    BeltPanelFixture fx;
    fx.mock().set_belt_failure(BeltMockFailure::STALL);
    // 1ms lines: the sweep's quiet half arrives in the first moments of the
    // pump, so the whole budget is the silent STALL_TIMEOUT the guard waits.
    fx.mock().set_belt_line_interval_ms(1);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    // The sweep dies at its midpoint and goes quiet. The guard re-arms on
    // every progress line, so its window starts at the LAST line, not at
    // Start: drain the live half first (one line per pump step, ~65 of them
    // over the mock's 5-135 Hz range), then wait out a full silent
    // STALL_TIMEOUT from that point.
    fx.pump_ms(15000);
    fx.pump_ms(BeltTensionPanel::STALL_TIMEOUT_MS + 2000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::ERROR));
    CHECK(fx.text("bt_error_message").find("stopped reporting progress") != std::string::npos);
    CHECK(fx.text("bt_error_message").find("restart Klipper") != std::string::npos);
}

TEST_CASE("closing mid-run stops listening", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RUNNING)));

    fx.panel().on_deactivate(DeactivateReason::NavigateAway);
    UpdateQueue::instance().drain();
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    // The mock sweep timer keeps firing, but the cancelled run may not act on
    // it: the panel has to stay on START.
    fx.pump_ms(5000);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));
}

TEST_CASE("Start stays disabled while klippy is not ready", "[belt][panel][gating]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());

    fx.set_klippy_ready(false);
    CHECK(fx.state_int("bt_can_start") == 0);

    fx.set_klippy_ready(true);
    INFO("gate message after re-ready: " << fx.text("bt_gate_message"));
    CHECK(fx.state_int("bt_can_start") == 1);
}

TEST_CASE("a CoreXZ reports a closed gate naming its kinematics", "[belt][panel][gating]") {
    // The persona must be in place before the fixture's on_activate fires the
    // hardware detect, and the mock reads the env at query time.
    helix_test::MockKinematicsEnv corexz("corexz");
    BeltPanelFixture fx;

    // Wait for detection to land: bt_hw_kinematics leaves "Detecting..." and
    // shows the raw name (an UNKNOWN kinematics has no friendly label).
    bool detected = false;
    for (uint32_t t = 0; t < 10000 && !detected; t += 100) {
        detected = fx.text("bt_hw_kinematics") == "corexz";
        if (!detected) {
            fx.pump_ms(100);
        }
    }
    REQUIRE(detected);

    CHECK(fx.state_int("bt_can_start") == 0);
    const std::string message = fx.text("bt_gate_message");
    INFO("gate message: " << message);
    CHECK(message.find("CoreXY") != std::string::npos);
    CHECK(message.find("corexz") != std::string::npos);
}

TEST_CASE("a sweep with too few frequency bins is an error", "[belt][panel]") {
    BeltPanelFixture fx;
    // A 5-6 Hz sweep writes only two bins; the comparison needs at least three
    // in-band bins per curve to say anything.
    fx.mock().set_resonance_sweep_range(5.0, 6.0);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::ERROR)));
    CHECK(fx.text("bt_error_message").find("Not enough frequency data") != std::string::npos);
}

TEST_CASE("EMBEDDED tier creates no chart", "[belt][panel][chart]") {
    BeltPanelFixture fx(helix::PlatformTier::EMBEDDED);
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    CHECK(fx.state_int("bt_chart_available") == 0);
    CHECK(fx.panel_chart() == nullptr);
}

TEST_CASE("a rebuild drops the chart with the tree it lives in", "[belt][panel][chart]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().handle_start_clicked();
    REQUIRE(fx.pump_until_state(static_cast<int>(BeltTensionPanel::ViewState::RESULTS)));
    REQUIRE(fx.panel_chart() != nullptr);

    // What a hot-reload does: free the whole widget tree and create it again.
    // The chart's object is a child of a host inside that tree, so the panel
    // must not come out of rebuild still holding a pointer to it.
    REQUIRE(fx.panel().rebuild());
    UpdateQueue::instance().drain();
    CHECK(fx.panel_chart() == nullptr);
    CHECK(fx.state_int("bt_chart_available") == 0);

    // The rebuilt tree still carries both hosts, and a fresh run rebuilds the
    // chart into the re-cached one.
    CHECK(lv_obj_find_by_name(fx.panel().get_root(), "chart_host_results") != nullptr);
}

TEST_CASE("sweep fact line comes from the printer's resonance_tester config", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    // Mock defaults 5-135 Hz at 1 Hz/s: two sweeps take ceil(2*130/60) = 5 min.
    CHECK(fx.text("bt_hw_sweep") == "5-135 Hz · about 5 min");
}

TEST_CASE("low RAM asks before the sweep starts", "[belt][panel]") {
    BeltPanelFixture fx;
    helix::ui::modal_init_subjects();
    REQUIRE(fx.register_component("modal_dialog"));
    REQUIRE(fx.wait_gate_open());
    fx.panel().set_total_ram_mb_for_test(128);

    fx.panel().handle_start_clicked();
    fx.pump_ms(500);
    // Klipper analyses the sweep on this host: on a small board the warning
    // comes first and nothing moves until it is answered.
    lv_obj_t* dialog = ModalStack::instance().top_dialog();
    REQUIRE(dialog != nullptr);
    CHECK(fx.panel_low_ram_dialog() == dialog);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::START));

    // Continuing starts the sweep and clears the handle.
    lv_obj_t* go = lv_obj_find_by_name(dialog, "btn_primary");
    REQUIRE(go != nullptr);
    lv_obj_send_event(go, LV_EVENT_CLICKED, nullptr);
    fx.pump_ms(500);
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}

TEST_CASE("enough RAM starts the sweep without asking", "[belt][panel]") {
    BeltPanelFixture fx;
    REQUIRE(fx.wait_gate_open());
    fx.panel().set_total_ram_mb_for_test(1024);

    fx.panel().handle_start_clicked();
    CHECK(fx.panel_low_ram_dialog() == nullptr);
    CHECK(fx.state_int("belt_tension_state") ==
          static_cast<int>(BeltTensionPanel::ViewState::RUNNING));
}
