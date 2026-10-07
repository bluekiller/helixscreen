// SPDX-License-Identifier: GPL-3.0-or-later

// Object picks made in print details reach the print: captured at the Print
// tap, sent only once Moonraker confirms the start, refused when they cover
// every object, and dropped (with a toast) when the tap queues the file.

#include "ui_nav_manager.h"
#include "ui_panel_print_status.h"
#include "ui_print_start_controller.h"
#include "ui_update_queue.h"

#include "../test_helpers/moonraker_client_mock_test_access.h"
#include "../test_helpers/navigation_manager_test_access.h"
#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"
#include "../test_helpers/print_start_controller_test_access.h"
#include "../test_helpers/print_state_test_drivers.h"
#include "../test_helpers/registered_backend.h"
#include "../ui_test_utils.h"
#include "ams_backend_mock.h"
#include "app_globals.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "job_queue_state.h"
#include "printer_discovery.h"
#include "printer_state.h"

#include <functional>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::PrintJobState;
using helix::test::set_wire_state;

namespace {

const char* kPlate = "; HEADER\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cone_id_0 CENTER=25,-4\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cube_id_1 CENTER=-36,6\n"
                     "EXCLUDE_OBJECT_DEFINE NAME=Cylinder_id_2 CENTER=-22,30\n"
                     "G28\n";

/// A real panel over the mock API with a three-object file's details open.
class PickStartFixture : private helix::PrintSelectGlobalStateReset,
                         public helix::PrintSelectPanelFixture {
  public:
    PickStartFixture()
        : helix::PrintSelectPanelFixture(helix::PrintSelectFilelistHandler::Unregistered,
                                         helix::PrintSelectVisit::Immediate,
                                         helix::PrintSelectApi::Mock) {
        set_moonraker_api(api_.get());
        helix::PrinterDiscovery hw;
        hw.parse_objects(nlohmann::json{"exclude_object", "extruder"});
        get_printer_state().set_hardware(hw);
        // A halted Klipper refuses G-code, and the reset state reads halted.
        get_printer_state().set_klippy_state_sync(helix::KlippyState::READY);
        helix::ui::set_test_notification_info_hook(
            [this](const std::string& m) { infos.push_back(m); });
        helix::ui::set_test_notification_warning_hook(
            [this](const std::string& m) { warnings.push_back(m); });

        panel_->refresh_files(/*force=*/true);
        drain();
        REQUIRE(panel_->select_file_by_name(file_.name()));
        drain();
        detail = PrintSelectPanelTestAccess::detail_view(*panel_);
        REQUIRE(detail != nullptr);
        REQUIRE(detail->exclude_objects().get_defined_objects().size() == 3);
        mock_client_.clear_gcode_script_history();
    }

    ~PickStartFixture() override {
        PrintSelectPanelTestAccess::hide_detail_view(*panel_);
        drain();
        lv_timer_handler(); // the close callback runs on the next tick
        helix::ui::set_test_notification_info_hook(nullptr);
        helix::ui::set_test_notification_warning_hook(nullptr);
        auto& ps = get_printer_state();
        if (ps.print_state().has_preparing_job()) {
            ps.print_state().retire_preparing(helix::PreparingExit::Superseded);
        }
        set_wire_state(ps, PrintJobState::STANDBY);
        ps.capabilities_state().set_job_queue_available(false);
        ps.set_hardware(helix::PrinterDiscovery{});
        set_moonraker_api(nullptr);
        drain();
    }

    std::vector<std::string> exclusions() const {
        std::vector<std::string> out;
        for (const auto& s : mock_client_.gcode_script_history()) {
            if (s.rfind("EXCLUDE_OBJECT NAME=", 0) == 0) {
                out.push_back(s);
            }
        }
        return out;
    }

    /// Hold printer.print.start's answer until the test releases it.
    void hold_print_start() {
        helix::MoonrakerClientMockTestAccess::set_method_handler(
            mock_client_, "printer.print.start",
            [this](MoonrakerClientMock*, const json&, std::function<void(const json&)> success_cb,
                   std::function<void(const MoonrakerError&)> error_cb) -> bool {
                held_start = std::move(success_cb);
                held_error = std::move(error_cb);
                return true;
            });
    }

    /// Start the open file through the panel's controller, past initiate().
    /// @p hide_details runs the controller's own hide, as a real start does.
    helix::ui::PrintStartController& start_held(bool hide_details) {
        auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
        REQUIRE(controller != nullptr);
        // No status overlay in a unit test; a no-op still lets the start hide details.
        controller->set_navigate_to_print_status(hide_details ? std::function<void()>([]() {})
                                                              : nullptr);
        controller->set_file(file_.name(), "", {}, "");
        controller->set_exclude_picks(detail->exclude_picks());
        hold_print_start();
        PrintStartControllerTestAccess::execute(*controller);
        drain();
        lv_timer_handler(); // the close callback runs on the next tick
        REQUIRE(held_start);
        return *controller;
    }

    static int pick_count() {
        return lv_subject_get_int(lv_xml_get_subject(nullptr, "detail_exclude_pick_count"));
    }

    static bool contains(const std::vector<std::string>& msgs, const std::string& needle) {
        for (const auto& m : msgs) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    helix::PlantedGcode file_{"pick_start.gcode", "", kPlate};
    helix::ui::PrintSelectDetailView* detail = nullptr;
    std::function<void(const json&)> held_start;
    std::function<void(const MoonrakerError&)> held_error;
    std::vector<std::string> infos;
    std::vector<std::string> warnings;
};

} // namespace

TEST_CASE_METHOD(PickStartFixture, "Picks are sent only after Moonraker confirms the start",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    auto& controller = start_held(/*hide_details=*/false);
    CHECK(PrintStartControllerTestAccess::exclude_picks(controller).empty()); // consumed
    CHECK(exclusions().empty()); // nothing before Moonraker answers

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
}

TEST_CASE_METHOD(PickStartFixture,
                 "Opening another file while the start is in flight keeps the started file's picks",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    start_held(/*hide_details=*/false);

    PrintSelectPanelTestAccess::hide_detail_view(*panel_);
    drain();
    lv_timer_handler(); // the close callback runs on the next tick
    helix::PlantedGcode other("other_file.gcode", "", kPlate);
    panel_->refresh_files(/*force=*/true);
    drain();
    REQUIRE(panel_->select_file_by_name(other.name()));
    drain();
    CHECK(detail->exclude_picks().empty());
    detail->toggle_exclude_pick("Cube_id_1");

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cone_id_0"});
    // The confirmation belongs to the started file, not the one now open.
    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cube_id_1"});
}

TEST_CASE_METHOD(PickStartFixture, "A failed start comes back to details with the picks intact",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    drain();
    REQUIRE(pick_count() == 1);
    start_held(/*hide_details=*/true);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
    CHECK(detail->picks_held_for_start());

    held_error(MoonrakerError::unknown("Klipper refused the print", "printer.print.start"));
    drain();
    // The controller re-shows details through this callback when the status
    // overlay it pushed is on top; a unit test pushes none.
    PrintSelectPanelTestAccess::show_detail_view(*panel_);
    drain();

    CHECK(detail->exclude_picks() == std::vector<std::string>{"Cube_id_1"});
    CHECK(pick_count() == 1);
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(PickStartFixture, "A confirmed start spends the picks and releases the hold",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cube_id_1");
    start_held(/*hide_details=*/true);
    REQUIRE(detail->picks_held_for_start());

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(detail->exclude_picks().empty());
    CHECK_FALSE(detail->picks_held_for_start());

    PrintSelectPanelTestAccess::show_detail_view(*panel_); // the same file again
    drain();
    CHECK(detail->exclude_picks().empty());
    CHECK(pick_count() == 0);
}

TEST_CASE_METHOD(PickStartFixture, "The Print tap hands the picks it saw to the start controller",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cylinder_id_2");
    // A running print makes initiate() refuse after set_file(), so the hand-off
    // is read without driving a real start.
    set_wire_state(get_printer_state(), PrintJobState::PRINTING);
    drain();
    panel_->start_print(/*force=*/true);
    drain();
    auto* controller = PrintSelectPanelTestAccess::print_controller(*panel_);
    REQUIRE(controller != nullptr);
    CHECK(PrintStartControllerTestAccess::exclude_picks(*controller) ==
          std::vector<std::string>{"Cylinder_id_2"});
}

TEST_CASE_METHOD(PickStartFixture, "Every object picked refuses the start before anything is sent",
                 "[print_select][pre_start_exclude][start]") {
    detail->toggle_exclude_pick("Cone_id_0");
    detail->toggle_exclude_pick("Cube_id_1");
    detail->toggle_exclude_pick("Cylinder_id_2");
    panel_->start_print(/*force=*/true);
    drain();
    CHECK(contains(warnings, "Every object is set to skip"));
    CHECK(PrintSelectPanelTestAccess::controller_file(*panel_).first.empty());
    CHECK(exclusions().empty());
}

TEST_CASE_METHOD(PickStartFixture, "A queued start drops the picks and says so",
                 "[print_select][pre_start_exclude][start][job_queue]") {
    auto& ps = get_printer_state();
    JobQueueState jqs(api_.get(), &mock_client_);
    JobQueueState* previous = get_job_queue_state();
    set_job_queue_state(&jqs);
    // The button re-decides on print state, not on queue availability.
    ps.capabilities_state().set_job_queue_available(true);
    set_wire_state(ps, PrintJobState::PRINTING);
    drain();
    REQUIRE(lv_subject_get_int(lv_xml_get_subject(nullptr, "print_select_button_mode")) == 1);

    detail->toggle_exclude_pick("Cube_id_1");
    panel_->start_print();
    drain();

    REQUIRE(
        PrintSelectPanelTestAccess::controller_file(*panel_).first.empty()); // queued, not started
    CHECK(contains(infos, "Object picks apply only to prints started now"));
    CHECK(detail->exclude_picks().empty());
    CHECK(exclusions().empty());
    set_job_queue_state(previous);
}

namespace {

/// A tool changer whose remap rewrites the job file, mapping tool 0 to head 1.
struct RewriteRemap {
    helix::test::RegisteredBackend<helix::AmsBackendMock> ams{2};
    std::vector<helix::ToolMapping> mappings;
    RewriteRemap() {
        ams->set_remap_strategy(helix::AmsBackend::RemapStrategy::GcodeRewrite);
        helix::ToolMapping m;
        m.tool_index = 0;
        m.mapped_slot = 1;
        mappings.push_back(m);
    }
};

} // namespace

TEST_CASE_METHOD(PickStartFixture, "A rewritten-remap start carries the picks after confirmation",
                 "[print_select][pre_start_exclude][start][remap]") {
    RewriteRemap remap;
    auto& nav = NavigationManager::instance();
    const auto stack_before = NavigationManagerTestAccess::panel_stack(nav);
    detail->toggle_exclude_pick("Cube_id_1");
    hold_print_start();
    PrintSelectPanelTestAccess::apply_remap(*panel_, remap.mappings);
    drain();
    REQUIRE(held_start);
    CHECK(exclusions().empty());

    held_start(json{{"result", "ok"}});
    drain();
    CHECK(exclusions() == std::vector<std::string>{"EXCLUDE_OBJECT NAME=Cube_id_1"});
    CHECK(detail->exclude_picks().empty());
    CHECK(PrintStatusPanel::get_cached_overlay() != nullptr); // the start navigated

    // The print status tree is process-wide: leave none behind.
    NavigationManagerTestAccess::set_panel_stack(nav, stack_before);
    PrintStatusPanel::destroy_cached_overlay(
        helix::ui::PrintStatusTreeDestroyCause::PanelRegistryTeardown);
    drain();
}

TEST_CASE_METHOD(PickStartFixture, "A rewritten-remap start is refused with every object picked",
                 "[print_select][pre_start_exclude][start][remap]") {
    RewriteRemap remap;
    detail->toggle_exclude_pick("Cone_id_0");
    detail->toggle_exclude_pick("Cube_id_1");
    detail->toggle_exclude_pick("Cylinder_id_2");
    hold_print_start();
    PrintSelectPanelTestAccess::apply_remap(*panel_, remap.mappings);
    drain();
    CHECK(contains(warnings, "Every object is set to skip"));
    CHECK_FALSE(held_start);
}
