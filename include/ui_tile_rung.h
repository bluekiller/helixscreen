// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file ui_tile_rung.h
 * @brief The face each home-tile part draws in at each size rung
 *
 * A sizing tile publishes one rung index (TileSizing); every part of the tile
 * binds its face to that index here. The ladders are the single table both
 * sides read: TileSizing measures a rung in these faces, and <bind_tile_rung>
 * draws it in them, so what is measured is what is drawn.
 *
 * @threading Main thread only
 */

#include "lvgl/lvgl.h"

namespace helix::ui {

/// Which part of a tile an object draws, and so which face ladder it follows.
enum class TileLadder : int {
    Icon = 0,
    Value = 1,
    Label = 2,
    /// The disc behind a badged glyph: the icon ladder, drawn as a square of
    /// tile_disc_edge() of that face instead of as a face.
    Disc = 3,
    /// A count badge on a glyph's top-right shoulder (notification_badge):
    /// sized from the icon face at the rung, its count drawn in its own row
    /// of faces.
    Pip = 4,
};

/// The face a tile part draws in at a rung, and the scale it is drawn at.
///
/// Every rung but the icon's xxl is a theme token at 1x. The icon's xxl rung is
/// a size (#tile_icon_xxl_size): it draws in the largest MDI face this build
/// links at or below that size, scaled up to reach it, never past kTileMaxScale.
struct TileFace {
    const lv_font_t* font = nullptr;
    int32_t scale = LV_SCALE_NONE;

    /// @p unscaled px at this face's scale.
    int px(int unscaled) const {
        return static_cast<int>(unscaled * scale / LV_SCALE_NONE);
    }
};

/// A bitmap glyph drawn past twice its size stops reading as the glyph.
inline constexpr int32_t kTileMaxScale = 2 * LV_SCALE_NONE;

/// The face @p ladder draws in at @p rung. Out-of-range rungs clamp to the
/// ladder's ends.
TileFace tile_rung_face(TileLadder ladder, int rung);

/// The theme font token @p ladder names at @p rung; for the icon's xxl rung,
/// the xl token it grows from.
const char* tile_rung_font_token(TileLadder ladder, int rung);

/// Edge of the disc a badged glyph in @p icon_face sits in, in px.
int tile_disc_edge(const TileFace& icon_face);

/// Edge of the count badge on a glyph drawn in @p icon_face, holding a count in
/// @p count_face.
int tile_pip_edge(const TileFace& icon_face, const lv_font_t* count_face);

/// Keep @p obj's face on @p ladder at the rung @p subject holds, shifted by
/// @p offset rungs. The face is resolved from the current tier's token each
/// time the rung changes. @p one_line also holds the object to one line of that
/// face, which a long_mode="dots" label needs to ellipsize rather than wrap.
void bind_tile_rung(lv_obj_t* obj, lv_subject_t* subject, TileLadder ladder, int offset = 0,
                    bool one_line = false);

/// Register `<bind_tile_rung ladder="icon|value|label|disc|pip" subject="..." offset="0"
/// one_line="false"/>`, a child element of any widget. An empty subject installs
/// no binding, so a component used outside a sizing tile keeps its authored face.
void register_tile_rung_binding();

} // namespace helix::ui
