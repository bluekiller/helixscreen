// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_exclude_object_side_list.h"

#include "ui_gcode_viewer.h"
#include "ui_print_exclude_object_manager.h"
#include "ui_row_text.h"
#include "ui_utils.h"

#include "color_utils.h"
#include "display_numbering.h"
#include "observer_factory.h"
#include "printer_state.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace helix::ui {

namespace {
constexpr uint32_t SLIDE_IN_DURATION_MS = 220;

// Singleton handle so the static XML close callback can find the live instance.
// Only one side list exists at a time (owned by PrintStatusPanel).
ExcludeObjectSideList* g_active_side_list = nullptr;
} // namespace

ExcludeObjectSideList::ExcludeObjectSideList() = default;

ExcludeObjectSideList::~ExcludeObjectSideList() {
    if (g_active_side_list == this) {
        g_active_side_list = nullptr;
    }
    if (root_) {
        lv_obj_delete_async(root_);
        root_ = nullptr;
    }
}

void ExcludeObjectSideList::create(lv_obj_t* parent, PrinterState* printer_state,
                                   PrintExcludeObjectManager* manager, SideListGeometry geom) {
    if (root_) {
        spdlog::warn("[ExcludeObjectSideList] create() called but already active");
        return;
    }
    if (!parent || !printer_state || !manager) {
        spdlog::error("[ExcludeObjectSideList] create() missing required pointers");
        return;
    }

    printer_state_ = printer_state;
    manager_ = manager;

    // Register the close-button XML callback once. Idempotent on repeat calls.
    static bool s_callbacks_registered = false;
    if (!s_callbacks_registered) {
        lv_xml_register_event_cb(nullptr, "on_exclude_side_list_close", on_close_clicked);
        s_callbacks_registered = true;
    }

    g_active_side_list = this;

    root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_side_list", nullptr));
    if (!root_) {
        spdlog::error("[ExcludeObjectSideList] lv_xml_create failed");
        g_active_side_list = nullptr;
        return;
    }

    lv_obj_set_width(root_, lv_pct(geom.width_pct));
    // Bottom-anchored means portrait, which is supposed to arrive measured. The
    // percentage fallback is a guess that cannot track the control stack — the
    // exact failure this sizing replaced — so say so rather than silently
    // shipping a list that eats the object map.
    if (geom.anchor_bottom && geom.height_px <= 0) {
        spdlog::warn("[ExcludeObjectSideList] Portrait list created without measurements; "
                     "falling back to {}% of the column",
                     geom.height_pct);
    }
    // A measured px height wins over the percentage. Portrait sizes the list to
    // the control stack it covers; a percentage there cannot notice the stack
    // shrinking and would keep eating the object map.
    if (geom.height_px > 0) {
        lv_obj_set_height(root_, geom.height_px);
    } else {
        lv_obj_set_height(root_, lv_pct(geom.height_pct));
    }
    lv_obj_set_align(root_, geom.anchor_bottom ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_RIGHT_MID);
    // FLOATING removes us from the parent's flex/layout calculations so we
    // sit on top of sibling columns rather than displacing them.
    lv_obj_add_flag(root_, LV_OBJ_FLAG_FLOATING);

    rows_container_ = lv_obj_find_by_name(root_, "rows_container");
    empty_state_ = lv_obj_find_by_name(root_, "empty_state");

    if (!rows_container_) {
        spdlog::error("[ExcludeObjectSideList] rows_container not found");
    }

    // Force layout so we know the pixel extent to travel for the slide.
    lv_obj_update_layout(parent);
    int slide_distance = geom.anchor_bottom ? lv_obj_get_height(root_) : lv_obj_get_width(root_);
    if (slide_distance <= 0) {
        slide_distance = 200; // fallback for unsized parent
    }

    // Start off-screen past the anchored edge (positive offset relative to
    // RIGHT_MID or BOTTOM_MID), then tween to 0 to sit flush against it.
    if (geom.anchor_bottom) {
        lv_obj_set_y(root_, slide_distance);
    } else {
        lv_obj_set_x(root_, slide_distance);
    }

    populate_rows();

    auto repopulate = [](ExcludeObjectSideList* self, int) {
        if (self->root_) {
            self->populate_rows();
        }
    };
    excluded_version_obs_ = observe<int>(printer_state_->get_excluded_objects_version_subject(),
                                         this, repopulate, printer_state_->get_subjects_lifetime());
    defined_version_obs_ = observe<int>(printer_state_->get_defined_objects_version_subject(), this,
                                        repopulate, printer_state_->get_subjects_lifetime());

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root_);
    lv_anim_set_values(&a, slide_distance, 0);
    lv_anim_set_duration(&a, SLIDE_IN_DURATION_MS);
    lv_anim_set_exec_cb(
        &a, geom.anchor_bottom
                ? [](void* obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t*>(obj), v); }
                : [](void* obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t*>(obj), v); });
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    // Report the height that was actually applied, not both candidates — a line
    // that always prints the percentage reads as "the percentage was used".
    if (geom.height_px > 0) {
        spdlog::debug("[ExcludeObjectSideList] Created ({}%x{}px measured, anchor={}, "
                      "slide_distance={}px)",
                      geom.width_pct, geom.height_px, geom.anchor_bottom ? "bottom" : "right",
                      slide_distance);
    } else {
        spdlog::debug("[ExcludeObjectSideList] Created ({}x{}%, anchor={}, slide_distance={}px)",
                      geom.width_pct, geom.height_pct, geom.anchor_bottom ? "bottom" : "right",
                      slide_distance);
    }
}

void ExcludeObjectSideList::destroy() {
    if (!root_) {
        return;
    }

    // Drop observers first — they capture `this` and the caller is about to
    // free us. Row click handlers also capture `this`; we delete the widget
    // tree asynchronously below, but the rows are children and will be torn
    // down with their parent before any further input can dispatch.
    excluded_version_obs_.reset();
    defined_version_obs_.reset();
    lifetime_.invalidate();

    // Cancel the slide-in animation (no slide-out — the lv_obj_delete_async
    // handles teardown immediately; animating with stale handlers risks UAF
    // on row taps during the out-anim window).
    lv_anim_delete(root_, nullptr);
    lv_obj_delete_async(root_);

    root_ = nullptr;
    rows_container_ = nullptr;
    empty_state_ = nullptr;

    if (g_active_side_list == this) {
        g_active_side_list = nullptr;
    }
}

void ExcludeObjectSideList::on_close_clicked(lv_event_t* /*e*/) {
    spdlog::debug("[ExcludeObjectSideList] Close button clicked");
    if (g_active_side_list && g_active_side_list->close_cb_) {
        g_active_side_list->close_cb_();
    }
}

void ExcludeObjectSideList::populate_rows() {
    if (!rows_container_ || !printer_state_) {
        return;
    }

    // Observers above are observe<int> (deferred via UpdateQueue), so child
    // teardown must go through the async-clean helper to stay outside the batch
    // (CLAUDE.md § "No sync widget deletion in queued callbacks").
    helix::ui::safe_clean_children(rows_container_);

    const auto& defined = printer_state_->get_defined_objects();
    const auto& excluded = printer_state_->get_excluded_objects();
    const auto& current = printer_state_->get_current_object();

    if (empty_state_) {
        if (defined.empty()) {
            lv_obj_remove_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(empty_state_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int index = 0;
    for (const auto& name : defined) {
        bool is_excluded = excluded.count(name) > 0;
        bool is_current = (name == current);
        create_row(rows_container_, index, name, is_excluded, is_current);
        ++index;
    }
}

void ExcludeObjectSideList::create_row(lv_obj_t* parent, int index, const std::string& name,
                                       bool is_excluded, bool is_current) {
    const bool clickable = !is_excluded && manager_;

    char num_buf[8];
    snprintf(num_buf, sizeof(num_buf), "%d", lane_number(index));
    const std::string badge_color =
        helix::color_to_hex_string(lv_color_to_u32(color_for_index(index)));

    const char* status_text = "";
    const char* status_color = "#text_muted";
    if (is_excluded) {
        status_text = lv_tr("Excluded");
    } else if (is_current) {
        status_text = lv_tr("Printing now");
        status_color = "#success";
    }

    const char* attrs[] = {
        "badge_text",    num_buf,
        "badge_color",   badge_color.c_str(),
        "name_color",    is_excluded ? "#text_muted" : "#text",
        "status_text",   status_text,
        "status_color",  status_color,
        "row_opa",       is_excluded ? "150" : "255",
        "row_clickable", clickable ? "true" : "false",
        nullptr,
    };
    lv_obj_t* row = static_cast<lv_obj_t*>(lv_xml_create(parent, "exclude_object_row", attrs));
    if (!row) {
        return;
    }
    helix::ui::set_row_label_text(row, "object_name", name.c_str());

    // L069: the row is a plain lv_obj, so the user_data slot is ours. The
    // helper owns the copy and frees it on LV_EVENT_DELETE.
    if (clickable && helix::ui::set_owned_user_string(row, name)) {
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, this);
    }
}

void ExcludeObjectSideList::on_row_clicked(lv_event_t* e) {
    auto* self = static_cast<ExcludeObjectSideList*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    if (!self || !self->manager_ || !target) {
        return;
    }
    const char* name = helix::ui::get_owned_user_string(target);
    if (!name) {
        return;
    }
    spdlog::info("[ExcludeObjectSideList] Row clicked: '{}'", name);

    // Highlight the matching object in the gcode viewer so the user gets
    // spatial feedback before the confirmation modal appears.
    if (self->gcode_viewer_) {
        std::unordered_set<std::string> highlight = {std::string(name)};
        ui_gcode_viewer_set_highlighted_objects(self->gcode_viewer_, highlight);
    }

    self->manager_->request_exclude(std::string(name));
}

lv_color_t ExcludeObjectSideList::color_for_index(int index) {
    return theme_manager_get_object_palette_color(index);
}

} // namespace helix::ui
