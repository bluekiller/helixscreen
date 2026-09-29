// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_tile_rung.h"

#include "helix-xml/src/xml/lv_xml.h"
#include "helix-xml/src/xml/lv_xml_parser.h"
#include "helix-xml/src/xml/lv_xml_utils.h"
#include "helix-xml/src/xml/lv_xml_widget.h"
#include "helix/ui/shared_font_style.h"
#include "src/ui/panel_widgets/tile_layout.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace helix::ui {

namespace {

/// One row per TileLadder, one column per rung. The rows move together: one
/// rung index selects the glyph, the value beside it and the label under it,
/// so a tile never draws a 64px glyph next to 12px digits.
///
/// The label row stays at font_xs through lg, the rung every medium-and-up tier
/// draws a one-cell tile at, so a tile at its natural size keeps its authored
/// caption and only a tile given more room than that grows it. font_small is
/// the larger step because it is the same light weight as font_xs.
constexpr const char* kLadders[3][kTileRungs] = {
    {"icon_font_xs", "icon_font_sm", "icon_font_md", "icon_font_lg", "icon_font_xl"},
    {"font_xs", "font_xs", "font_small", "font_body", "font_heading"},
    {"font_xs", "font_xs", "font_xs", "font_xs", "font_small"},
};

/// The ladder, offset and one-line flag ride in the observer's user data, so a
/// binding needs no allocation and nothing to free when its object goes away.
constexpr int kOneLineBit = 1 << 12;

void* pack(TileLadder ladder, int offset, bool one_line) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(
        static_cast<int>(ladder) * 256 + (offset + 128) + (one_line ? kOneLineBit : 0)));
}

void rung_observer_cb(lv_observer_t* observer, lv_subject_t* subject) {
    const auto packed =
        static_cast<int>(reinterpret_cast<intptr_t>(lv_observer_get_user_data(observer)));
    const auto ladder = static_cast<TileLadder>((packed & ~kOneLineBit) / 256);
    const int offset = packed % 256 - 128;
    auto* obj = static_cast<lv_obj_t*>(lv_observer_get_target(observer));
    const char* token = tile_rung_font_token(ladder, lv_subject_get_int(subject) + offset);
    const lv_font_t* font = theme_manager_get_font(token);
    if (ladder == TileLadder::Disc) {
        // DECLARATIVE_OK: measured from the computed face, which XML cannot name.
        const int edge = tile_disc_edge(font);
        lv_obj_set_size(obj, edge, edge);
        return;
    }
    apply_font_style(obj, font);
    // A dotted label ellipsizes only at a fixed height; at content height it
    // wraps instead. One line of the face it now draws in is that height.
    // DECLARATIVE_OK: measured from the computed face, which XML cannot name.
    if ((packed & kOneLineBit) && font) {
        lv_obj_set_height(obj, lv_font_get_line_height(font));
    }
}

void* bind_tile_rung_create(lv_xml_parser_state_t* state, const char** attrs) {
    (void)attrs;
    return lv_xml_state_get_parent(state);
}

void bind_tile_rung_apply(lv_xml_parser_state_t* state, const char** attrs) {
    const char* subject_name = lv_xml_get_value_of(attrs, "subject");
    if (!subject_name || subject_name[0] == '\0') {
        return; // an optional subject left at its empty default: no binding
    }
    lv_subject_t* subject = lv_xml_get_subject(&state->scope, subject_name);
    if (!subject) {
        spdlog::warn("[TileRung] subject '{}' does not exist; the face stays as authored",
                     subject_name);
        return;
    }

    const char* ladder_name = lv_xml_get_value_of(attrs, "ladder");
    TileLadder ladder = TileLadder::Icon;
    if (ladder_name && std::strcmp(ladder_name, "value") == 0) {
        ladder = TileLadder::Value;
    } else if (ladder_name && std::strcmp(ladder_name, "label") == 0) {
        ladder = TileLadder::Label;
    } else if (ladder_name && std::strcmp(ladder_name, "disc") == 0) {
        ladder = TileLadder::Disc;
    } else if (ladder_name && std::strcmp(ladder_name, "icon") != 0) {
        spdlog::warn("[TileRung] unknown ladder '{}'; binding the icon ladder", ladder_name);
    }

    const char* offset_str = lv_xml_get_value_of(attrs, "offset");
    const int offset = offset_str ? lv_xml_atoi(offset_str) : 0;
    const char* one_line_str = lv_xml_get_value_of(attrs, "one_line");
    const bool one_line = one_line_str && std::strcmp(one_line_str, "true") == 0;

    bind_tile_rung(static_cast<lv_obj_t*>(lv_xml_state_get_parent(state)), subject, ladder, offset,
                   one_line);
}

} // namespace

const char* tile_rung_font_token(TileLadder ladder, int rung) {
    const int row =
        ladder == TileLadder::Disc ? static_cast<int>(TileLadder::Icon) : static_cast<int>(ladder);
    return kLadders[row][std::clamp(rung, 0, kTileRungs - 1)];
}

int tile_disc_edge(const lv_font_t* icon_face) {
    // Half again the glyph's line height: the proportion #icon_badge_size
    // holds to the md glyph on the tiers that author it.
    return icon_face ? static_cast<int>(lv_font_get_line_height(icon_face)) * 3 / 2 : 0;
}

void bind_tile_rung(lv_obj_t* obj, lv_subject_t* subject, TileLadder ladder, int offset,
                    bool one_line) {
    if (!obj || !subject) {
        return;
    }
    lv_subject_add_observer_obj(subject, rung_observer_cb, obj, pack(ladder, offset, one_line));
}

void register_tile_rung_binding() {
    lv_xml_register_widget("lv_obj-bind_tile_rung", bind_tile_rung_create, bind_tile_rung_apply);
}

} // namespace helix::ui
