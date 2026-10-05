// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_print_select_list_view.h"

#include "../lvgl_ui_test_fixture.h"
#include "print_file_data.h"

#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix::ui;

namespace {

std::vector<PrintFileData> make_files(const std::string& print_time) {
    std::vector<PrintFileData> files;
    for (int i = 0; i < 3; i++) {
        PrintFileData f{};
        f.filename = "part" + std::to_string(i) + ".gcode";
        f.print_time_str = print_time;
        files.push_back(f);
    }
    return files;
}

std::string visible_print_times(lv_obj_t* container) {
    std::string out;
    for (uint32_t i = 0; i < lv_obj_get_child_count(container); i++) {
        lv_obj_t* row = lv_obj_get_child(container, i);
        if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        if (lv_obj_t* t = lv_obj_find_by_name(row, "row_print_time")) {
            out += std::string(lv_label_get_text(t)) + ";";
        }
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(LVGLUITestFixture,
                 "PrintSelectListView - repopulating the same indices shows the new data",
                 "[print_select_list_view][ui_integration]") {
    PrintSelectListView view;
    lv_obj_t* container = lv_obj_create(test_screen());
    lv_obj_set_size(container, 600, 400);
    lv_obj_add_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    view.setup(container, nullptr, nullptr);

    view.populate(make_files("1h 10m"), false);
    process_lvgl(50);
    REQUIRE(visible_print_times(container) == "1h 10m;1h 10m;1h 10m;");

    // Same count, same indices: only the row contents differ.
    view.populate(make_files("2h 20m"), false);
    process_lvgl(50);
    REQUIRE(visible_print_times(container) == "2h 20m;2h 20m;2h 20m;");
}
