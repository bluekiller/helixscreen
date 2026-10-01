// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_lazy_load.cpp
 * @brief A print-select panel nobody has opened issues no file traffic and builds no detail view
 *
 * Listing, metadata and thumbnail fetches wait for the first on_activate(). A
 * programmatic selection made before the first listing lands (history reprint,
 * Print Last) is held until the listing arrives.
 */

#include "ui_nav_manager.h"
#include "ui_panel_print_select.h"

#include "../test_helpers/print_select_panel_fixture.h"
#include "../test_helpers/print_select_panel_test_access.h"

#include "../catch_amalgamated.hpp"

using namespace helix;

namespace {
struct UnvisitedPrintSelectFixture : PrintSelectPanelFixture {
    UnvisitedPrintSelectFixture()
        : PrintSelectPanelFixture(PrintSelectFilelistHandler::Registered,
                                  PrintSelectVisit::Deferred) {}
};
} // namespace

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Unvisited print-select neither lists files nor builds the detail view",
                 "[print_select][lazy]") {
    PlantedGcode file("lazy_unvisited.gcode");
    REQUIRE(file.on_disk());
    drain();

    panel_->refresh_files(true);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_size(*panel_) == 0);
    REQUIRE_FALSE(PrintSelectPanelTestAccess::detail_view_built(*panel_));

    NavigationManager::instance().set_active(PanelId::PrintSelect);
    drain();
    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
}

TEST_CASE_METHOD(UnvisitedPrintSelectFixture,
                 "Selecting a file before the first listing opens it once the listing lands",
                 "[print_select][lazy]") {
    PlantedGcode file("lazy_select.gcode");
    REQUIRE(file.on_disk());
    drain();

    NavigationManager::instance().set_active(PanelId::PrintSelect);
    REQUIRE(panel_->select_file_by_name(file.name()));
    drain();

    REQUIRE(PrintSelectPanelTestAccess::list_contains(*panel_, file.name()));
    REQUIRE(PrintSelectPanelTestAccess::detail_view_visible(*panel_));
}
