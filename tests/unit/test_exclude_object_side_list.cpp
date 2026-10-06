// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_exclude_object_side_list.cpp
 * @brief The exclude-object side list rebuilds rows only when the defined object
 *        set changes; exclusions and the printing object restyle rows in place.
 *
 * The printing object changes many times per layer, so a rebuild there would
 * reset the list's scroll position under the user's finger.
 */

#include "ui_exclude_object_side_list.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_update_queue.h"

#include "../lvgl_ui_test_fixture.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "printer_state.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"

using namespace helix;
using namespace helix::ui;

namespace {

std::vector<std::string> object_names(int n, const char* prefix = "obj_") {
    std::vector<std::string> names;
    for (int i = 0; i < n; ++i) {
        names.push_back(prefix + std::to_string(i));
    }
    return names;
}

std::vector<lv_obj_t*> rows_of(lv_obj_t* container) {
    std::vector<lv_obj_t*> rows;
    const uint32_t n = lv_obj_get_child_count(container);
    for (uint32_t i = 0; i < n; ++i) {
        rows.push_back(lv_obj_get_child(container, static_cast<int32_t>(i)));
    }
    return rows;
}

/// Whether @p row shows a visible label reading @p text. Checks what the user
/// sees, not how the row is built.
bool shows_text(lv_obj_t* row, const char* text) {
    const uint32_t n = lv_obj_get_child_count(row);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN) ||
            lv_obj_get_style_opa(child, LV_PART_MAIN) == LV_OPA_TRANSP) {
            continue;
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* t = lv_label_get_text(child);
            if (t && std::strcmp(t, text) == 0) {
                return true;
            }
        }
        if (shows_text(child, text)) {
            return true;
        }
    }
    return false;
}

class SideListFixture : public LVGLUITestFixture {
  public:
    SideListFixture() : manager(nullptr, state(), nullptr) {
        objects().set_defined_objects(object_names(20));
        objects().set_current_object("obj_0");
        list.create(test_screen(), &state(), &manager, exclude_side_list_geometry(false));
        settle();
        container = lv_obj_find_by_name(list.root(), "rows_container");
    }

    ~SideListFixture() override {
        list.destroy();
        objects().clear_objects();
        settle();
    }

    PrinterExcludedObjectsState& objects() {
        return state().excluded_objects_state();
    }

    void settle() {
        UpdateQueue::instance().drain();
        process_lvgl(400);
    }

    PrintExcludeObjectManager manager;
    ExcludeObjectSideList list;
    lv_obj_t* container = nullptr;
};

} // namespace

TEST_CASE_METHOD(SideListFixture,
                 "Side list keeps its rows and scroll position when the printing object changes",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);

    lv_obj_scroll_to_y(container, 60, LV_ANIM_OFF);
    REQUIRE(lv_obj_get_scroll_y(container) == 60);

    objects().set_current_object("obj_5");
    settle();

    REQUIRE(rows_of(container) == before);
    CHECK(lv_obj_get_scroll_y(container) == 60);
    CHECK(shows_text(before[5], "Printing now"));
    CHECK_FALSE(shows_text(before[0], "Printing now"));
}

TEST_CASE_METHOD(SideListFixture, "Side list restyles an excluded row in place",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);
    REQUIRE(lv_obj_has_flag(before[3], LV_OBJ_FLAG_CLICKABLE));
    REQUIRE(lv_obj_get_style_opa(before[3], LV_PART_MAIN) == LV_OPA_COVER);

    objects().set_excluded_objects({"obj_3"});
    settle();

    REQUIRE(rows_of(container) == before);
    CHECK_FALSE(lv_obj_has_flag(before[3], LV_OBJ_FLAG_CLICKABLE));
    CHECK(lv_obj_get_style_opa(before[3], LV_PART_MAIN) == 150);
    CHECK(shows_text(before[3], "Excluded"));
    CHECK(lv_obj_has_flag(before[4], LV_OBJ_FLAG_CLICKABLE));
    CHECK_FALSE(shows_text(before[4], "Excluded"));
}

TEST_CASE_METHOD(SideListFixture, "Side list rebuilds its rows when the defined objects change",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    const auto before = rows_of(container);
    REQUIRE(before.size() == 20);

    objects().set_defined_objects(object_names(3, "part_"));
    settle();

    const auto after = rows_of(container);
    REQUIRE(after.size() == 3);
    for (lv_obj_t* row : after) {
        CHECK(std::find(before.begin(), before.end(), row) == before.end());
    }
}

namespace {
/// Whatever the name length, a status appearing must not reflow the name:
/// sweep lengths across the point where a name stops fitting beside a status,
/// since that is where a row would grow.
void check_row_heights_hold(SideListFixture& f) {
    for (int len = 4; len <= 64; len += 2) {
        const std::string name = "Part_" + std::string(static_cast<size_t>(len), 'm');
        INFO("name length " << name.size());
        f.objects().set_defined_objects({name, "obj_1"});
        f.objects().set_current_object("");
        f.settle();
        auto rows = rows_of(f.container);
        REQUIRE(rows.size() == 2);
        lv_obj_update_layout(f.container);
        const int32_t idle_h = lv_obj_get_height(rows[0]);

        f.objects().set_current_object(name);
        f.settle();
        lv_obj_update_layout(f.container);
        CHECK(lv_obj_get_height(rows[0]) == idle_h);

        f.objects().set_excluded_objects({name});
        f.settle();
        lv_obj_update_layout(f.container);
        CHECK(lv_obj_get_height(rows[0]) == idle_h);
        f.objects().set_excluded_objects({});
    }
}
} // namespace

TEST_CASE_METHOD(SideListFixture, "Side list rows keep their height as the printing object moves",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    check_row_heights_hold(*this);
}

TEST_CASE_METHOD(SideListFixture,
                 "Portrait side list rows keep their height and put the status beside the name",
                 "[exclude_side_list]") {
    lv_subject_t* portrait = lv_xml_get_subject(nullptr, "ui_is_portrait");
    REQUIRE(portrait != nullptr);
    const int was = lv_subject_get_int(portrait);
    lv_subject_set_int(portrait, 1);
    list.destroy();
    settle();
    list.create(test_screen(), &state(), &manager, exclude_side_list_geometry(true));
    settle();
    container = lv_obj_find_by_name(list.root(), "rows_container");
    REQUIRE(container != nullptr);

    check_row_heights_hold(*this);

    // Side by side: the status slot sits right of the name, on the same line.
    objects().set_defined_objects({"Cube_id_1_copy_0", "obj_1"});
    objects().set_current_object("Cube_id_1_copy_0");
    settle();
    lv_obj_t* row = rows_of(container)[0];
    lv_obj_update_layout(row);
    lv_obj_t* name = lv_obj_find_by_name(row, "object_name");
    lv_obj_t* status = lv_obj_find_by_name(row, "status_printing");
    REQUIRE(name != nullptr);
    REQUIRE(status != nullptr);
    lv_area_t na, sa;
    lv_obj_get_coords(name, &na);
    lv_obj_get_coords(status, &sa);
    CHECK(sa.x1 > na.x2);                                      // right of the name
    CHECK(lv_area_get_width(&na) > lv_obj_get_width(row) / 2); // the name keeps most of the row
    const int32_t status_mid = (sa.y1 + sa.y2) / 2;
    CHECK(status_mid >= na.y1); // level with the name, not below it
    CHECK(status_mid <= na.y2);

    lv_subject_set_int(portrait, was);
}

namespace {
struct StateWatch {
    lv_obj_t* container = nullptr;
    std::vector<std::string> row_names_at_publish;
};

void record_rows(lv_observer_t* observer, lv_subject_t*) {
    auto* w = static_cast<StateWatch*>(lv_observer_get_user_data(observer));
    std::string names;
    const uint32_t n = lv_obj_get_child_count(w->container);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* label = lv_obj_find_by_name(
            lv_obj_get_child(w->container, static_cast<int32_t>(i)), "object_name");
        names += label ? lv_label_get_text(label) : "?";
        names += ",";
    }
    w->row_names_at_publish.push_back(names);
}
} // namespace

TEST_CASE_METHOD(SideListFixture, "Side list never publishes a new object's state onto an old row",
                 "[exclude_side_list]") {
    REQUIRE(container != nullptr);
    objects().set_defined_objects({"old_a", "old_b"});
    objects().set_current_object("old_a");
    settle();
    REQUIRE(rows_of(container).size() == 2);

    lv_subject_t* row1 = lv_xml_get_subject(nullptr, "exclude_row_state_1");
    REQUIRE(row1 != nullptr);
    StateWatch watch{container, {}};
    lv_observer_t* obs = lv_subject_add_observer(row1, record_rows, &watch);
    watch.row_names_at_publish.clear(); // the add fires once

    // The excluded-version observer is queued before the defined-version one,
    // so it runs while the rows still show the old list.
    objects().set_excluded_objects({"new_d"});
    objects().set_defined_objects({"new_c", "new_d"});
    settle();
    lv_observer_remove(obs);

    REQUIRE_FALSE(watch.row_names_at_publish.empty());
    for (const auto& names : watch.row_names_at_publish) {
        INFO("rows when row 1's state was published: " << names);
        CHECK(names.find("old_") == std::string::npos);
    }
    CHECK(shows_text(rows_of(container)[1], "Excluded"));
}
