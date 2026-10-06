// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_print_select_card_view.cpp
 * @brief Unit tests for PrintSelectCardView thumbnail path helpers
 *
 * Tests is_placeholder_thumbnail() and has_real_thumbnail() to prevent
 * regressions where LVGL "A:" drive prefixes break std::filesystem::exists().
 */

#include "ui_panel_print_select.h"
#include "ui_print_select_card_view.h"
#include "ui_virtual_list.h"

#include "../lvgl_ui_test_fixture.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "../catch_amalgamated.hpp"

using helix::ui::PrintSelectCardView;

// ============================================================================
// is_placeholder_thumbnail
// ============================================================================

TEST_CASE("CardView: is_placeholder_thumbnail treats empty path as placeholder",
          "[ui][card_view]") {
    REQUIRE(PrintSelectCardView::is_placeholder_thumbnail(""));
}

TEST_CASE("CardView: is_placeholder_thumbnail rejects real paths", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail(
        "A:/home/user/.cache/helix/abc123_160x160_ARGB8888.bin"));
    REQUIRE_FALSE(PrintSelectCardView::is_placeholder_thumbnail("A:/tmp/thumb.png"));
}

// ============================================================================
// has_real_thumbnail
// ============================================================================

TEST_CASE("CardView: has_real_thumbnail returns false for empty path", "[ui][card_view]") {
    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail(""));
}

TEST_CASE("CardView: has_real_thumbnail returns false for nonexistent file", "[ui][card_view]") {
    REQUIRE_FALSE(
        PrintSelectCardView::has_real_thumbnail("A:/tmp/does_not_exist_helix_test_thumb.bin"));
}

TEST_CASE("CardView: has_real_thumbnail with A: prefix finds existing file", "[ui][card_view]") {
    // Create a temporary file to test against
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail without A: prefix finds existing file", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb2.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }

    REQUIRE(PrintSelectCardView::has_real_thumbnail(tmp.string()));

    std::filesystem::remove(tmp);
}

TEST_CASE("CardView: has_real_thumbnail returns false after file deleted", "[ui][card_view]") {
    auto tmp = std::filesystem::temp_directory_path() / "helix_test_thumb3.bin";
    {
        std::ofstream out(tmp);
        out << "test";
    }
    std::filesystem::remove(tmp);

    REQUIRE_FALSE(PrintSelectCardView::has_real_thumbnail("A:" + tmp.string()));
}

// ============================================================================
// Card pool sizing
// ============================================================================

namespace {

std::vector<PrintFileData> make_files(int n) {
    std::vector<PrintFileData> files(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        files[static_cast<size_t>(i)].filename = "file_" + std::to_string(i) + ".gcode";
    }
    return files;
}

/// Cards a window needs at the container's current scroll offset.
size_t window_cards(lv_obj_t* container, const CardDimensions& dims, int file_count) {
    const int stride = dims.card_height + lv_obj_get_style_pad_row(container, LV_PART_MAIN);
    const int rows = (file_count + dims.num_columns - 1) / dims.num_columns;
    const auto w =
        helix::ui::compute_window(lv_obj_get_scroll_y(container), lv_obj_get_height(container),
                                  stride, rows, PrintSelectCardView::BUFFER_ROWS);
    return static_cast<size_t>(std::min(file_count, w.last * dims.num_columns) -
                               w.first * dims.num_columns);
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture, "CardView: the pool holds the visible window, not a fixed 24",
                 "[ui][card_view][print_select]") {
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(container, 10, LV_PART_MAIN);

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    const CardDimensions dims{4, 2, 160, 200};
    const auto files = make_files(60);

    view.populate(files, dims);
    const size_t first = window_cards(container, dims, 60);
    CHECK(view.pool_size() == first);
    CHECK(first < 24);

    // Scrolled into the middle, the window spans overscan rows on both sides.
    lv_obj_scroll_to_y(container, 1000, LV_ANIM_OFF);
    view.update_visible(files, dims);
    const size_t middle = window_cards(container, dims, 60);
    REQUIRE(middle > first);
    CHECK(view.pool_size() == middle);

    // Shown cards are exactly the window; the two spacers are never hidden.
    int shown = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); ++i) {
        if (!lv_obj_has_flag(lv_obj_get_child(container, static_cast<int32_t>(i)),
                             LV_OBJ_FLAG_HIDDEN)) {
            ++shown;
        }
    }
    CHECK(static_cast<size_t>(shown - 2) == middle);

    view.cleanup();
    lv_obj_delete(container);
}

TEST_CASE_METHOD(
    LVGLUITestFixture,
    "CardView: cards on a solid background draw an opaque gradient, otherwise a masked one",
    "[ui][card_view][print_select]") {
    lv_obj_t* page = lv_obj_create(test_screen());
    lv_obj_set_size(page, 720, 420);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_t* container = lv_obj_create(page);
    lv_obj_set_size(container, 700, 400);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW_WRAP);

    const bool graded = GENERATE(false, true);
    CAPTURE(graded);
    if (graded) {
        lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, LV_PART_MAIN);
        lv_obj_set_style_bg_grad_color(page, lv_color_hex(0x303030), LV_PART_MAIN);
    }

    PrintSelectCardView view;
    REQUIRE(view.setup(container, [](size_t) {}, nullptr));
    view.populate(make_files(8), CardDimensions{4, 2, 160, 200});

    lv_obj_t* gradient = lv_obj_find_by_name(container, "gradient_bg");
    REQUIRE(gradient != nullptr);
    const auto* buf = static_cast<const lv_draw_buf_t*>(lv_image_get_src(gradient));
    REQUIRE(buf != nullptr);
    CHECK(buf->header.cf == (graded ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_NATIVE));

    view.cleanup();
    lv_obj_delete(page);
}
