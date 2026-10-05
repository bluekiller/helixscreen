// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_status_preview_fetch.cpp
 * @brief What the print-status G-code fetch does with the disk cache, a
 *        closed panel and a failing metadata lookup.
 *
 * Every download is held by the transfer mock until the test releases it, so a
 * test decides when a transfer lands relative to a close, a reopen or a second
 * fetch.
 */

#include "ui_gcode_viewer.h"
#include "ui_panel_print_status.h"

#include "../test_helpers/print_status_preview_fixture.h"

#include <fstream>

using print_status_preview_test::PrintStatusPreviewFixture;

namespace {

constexpr const char* PRINT_A = "pause_markers_demo.gcode";

/// The one cached preview copy in the fixture's private cache directory, or
/// empty when there is none.
std::filesystem::path cached_copy(const std::filesystem::path& cache_root) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cache_root / "gcode_temp", ec)) {
        if (entry.path().filename().string().rfind("print_view_", 0) == 0) {
            return entry.path();
        }
    }
    return {};
}

} // namespace

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a download that lands after the tree is destroyed shows nothing",
                 "[print_status][preview_fetch][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);

    PrintStatusPanelTestAccess::ui_destroyed(*panel_);
    land_dropped(PRINT_A);

    CHECK(gcode_displayed_file().empty());
    CHECK_FALSE(ui_gcode_viewer_has_content(viewer_));
}

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a cached copy of the wrong size is downloaded again",
                 "[print_status][preview_fetch][slow]") {
    report_print(PRINT_A);

    // Seed the cache with a copy the server's file cannot match.
    start_fetch(PRINT_A);
    const auto cache_root = cache_.dir;
    REQUIRE(transfers_.release(PRINT_A));
    drain();
    const auto copy = cached_copy(cache_root);
    REQUIRE_FALSE(copy.empty());
    {
        std::ofstream truncated(copy, std::ios::binary | std::ios::trunc);
        truncated << "partial";
    }

    const size_t held_before = transfers_.held_count();
    PrintStatusPanelTestAccess::load_gcode_for_viewing(*panel_, PRINT_A);
    drain();

    CHECK(transfers_.held_count() == held_before + 1);
}

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a cached copy of the right size is not downloaded again",
                 "[print_status][preview_fetch][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);
    land(PRINT_A);
    REQUIRE_FALSE(cached_copy(cache_.dir).empty());

    PrintStatusPanelTestAccess::load_gcode_for_viewing(*panel_, PRINT_A);
    drain();

    CHECK(transfers_.held_count() == 0);
}

TEST_CASE_METHOD(PrintStatusPreviewFixture,
                 "Print status: a metadata failure leaves a rendered preview on screen",
                 "[print_status][preview_fetch][slow]") {
    report_print(PRINT_A);
    start_fetch(PRINT_A);
    land(PRINT_A);
    REQUIRE(ui_gcode_viewer_has_content(viewer_));

    helix::ScopedEnv metadata_fails("HELIX_MOCK_METADATA_404", "1");
    PrintStatusPanelTestAccess::load_gcode_for_viewing(*panel_, PRINT_A);
    drain();

    CHECK(ui_gcode_viewer_has_content(viewer_));
    CHECK(gcode_displayed_file() == PRINT_A);
    CHECK(transfers_.held_count() == 0);
}
