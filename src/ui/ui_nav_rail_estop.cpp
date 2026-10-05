// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_nav_rail_estop.h"

#include "ui_effects.h"
#include "ui_utils.h"

#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace helix::ui {

void RailEstop::create(lv_obj_t* navbar) {
    navbar_ = navbar;
    lv_obj_t* slot = lv_obj_find_by_name(navbar, "nav_estop_slot");
    lv_obj_t* screen = lv_obj_get_screen(navbar);
    if (!slot || !screen) {
        spdlog::debug("[NavigationManager] No nav_estop_slot in this navbar; no rail E-stop");
        return;
    }
    estop_ = static_cast<lv_obj_t*>(lv_xml_create(screen, "rail_estop", nullptr));
    if (!estop_) {
        spdlog::error("[NavigationManager] rail_estop component would not build");
        return;
    }
    lv_obj_set_name(estop_, "nav_btn_estop");
    set_always_on_top(estop_);
    spdlog::debug("[NavigationManager] Rail E-stop created over nav_estop_slot");
    // DECLARATIVE_OK: LV_EVENT_DELETE cleanup has no declarative equivalent.
    lv_obj_add_event_cb(estop_, estop_deleted_cb, LV_EVENT_DELETE, this);
    // The slot moves whenever the rail lays out (it appears, the orientation
    // flips), and the button has to follow it there.
    lv_obj_add_event_cb(navbar, navbar_layout_changed_cb, LV_EVENT_LAYOUT_CHANGED, this);
    sync();
}

void RailEstop::estop_deleted_cb(lv_event_t* e) {
    // Only the current E-stop: a replaced one dying late must not clear its
    // successor.
    auto* self = static_cast<RailEstop*>(lv_event_get_user_data(e));
    if (lv_event_get_target_obj(e) == self->estop_) {
        self->estop_ = nullptr;
        set_always_on_top(nullptr);
    }
}

void RailEstop::navbar_layout_changed_cb(lv_event_t* e) {
    static_cast<RailEstop*>(lv_event_get_user_data(e))->sync();
}

void RailEstop::sync() {
    if (!estop_ || !navbar_) {
        return;
    }
    lv_obj_t* slot = lv_obj_find_by_name(navbar_, "nav_estop_slot");
    if (!slot) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(slot, &area);

    // The keyboard can shift the whole layout up while it is open; the slot's
    // home position is its offset within that layout, which rests at y=0.
    lv_obj_t* layout_root = navbar_;
    while (lv_obj_get_parent(layout_root) &&
           lv_obj_get_parent(layout_root) != lv_obj_get_screen(layout_root)) {
        layout_root = lv_obj_get_parent(layout_root);
    }
    lv_area_t root_area;
    lv_obj_get_coords(layout_root, &root_area);
    int32_t y = area.y1 - root_area.y1;

    // With the keyboard open over the bottom of a side rail, the E-stop rides
    // in the rail column just above the keyboard's top edge, never over a key.
    // A portrait bottom bar has no column above the keyboard: everything there
    // is the overlay's own content, the text field first.
    const bool side_rail = lv_obj_get_height(navbar_) > lv_obj_get_width(navbar_);
    if (keyboard_top_ >= 0 && side_rail) {
        const int32_t size = lv_obj_get_height(estop_);
        const int32_t above = keyboard_top_ - size - theme_manager_get_spacing("space_xs");
        y = std::min(y, above);
    }
    // Screen children are positioned in screen coordinates.
    lv_obj_set_pos(estop_, area.x1, y);
}

void RailEstop::set_keyboard_top(int32_t top) {
    keyboard_top_ = top;
    sync();
    // Only a side rail leaves room above the keyboard. A portrait bottom bar is
    // under it, and an E-stop raised there would sit on the keyboard's keys.
    if (top >= 0 && estop_ && navbar_ && lv_obj_get_height(navbar_) > lv_obj_get_width(navbar_)) {
        lv_obj_move_foreground(estop_);
    }
}

void RailEstop::destroy() {
    if (estop_) {
        set_always_on_top(nullptr);
        safe_delete_deferred(estop_);
    }
    navbar_ = nullptr;
    keyboard_top_ = -1;
}

RailEstop::ScopedHide::ScopedHide(RailEstop& rail) {
    if (rail.estop_ && !lv_obj_has_flag(rail.estop_, LV_OBJ_FLAG_HIDDEN)) {
        hidden_ = rail.estop_;
        lv_obj_add_flag(hidden_, LV_OBJ_FLAG_HIDDEN);
    }
}

RailEstop::ScopedHide::~ScopedHide() {
    if (hidden_) {
        lv_obj_remove_flag(hidden_, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace helix::ui
