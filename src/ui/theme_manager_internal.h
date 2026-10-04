// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Declarations shared between the src/ui/theme_*.cpp files that implement
// theme_manager.h. Nothing here is public API: callers outside those files go
// through include/theme_manager.h.

#include "lvgl/lvgl.h"
#include "theme_loader.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace helix::theme_detail {

/// Canonical ui_xml directory for token discovery, resolved once through the
/// asset-root seam. On CWD-less targets (ESP-IDF VFS) this is an absolute path
/// under the mount the firmware configured via helix::set_asset_root().
const char* ui_xml_dir();

/// Apply the high-DPI UI scale to one authored px token. Opacities (`*_opacity`)
/// and anything that is not a bare positive integer pass through unchanged.
/// Shared by the responsive resolver and the static-constant registration so a
/// fixed-size box and the scaled glyph inside it cannot disagree.
std::string scale_px_token(const std::string& name, const std::string& value, double scale);

/// Core theme state: which display, which mode, which theme, and what the
/// repeat guard in theme_manager_init() last built. Main-thread only.
struct ThemeRuntime {
    lv_theme_t* current_theme = nullptr;
    bool dark = true;
    lv_display_t* display = nullptr;
    helix::ThemeData active_theme;

    // Repeat-guard state for theme_manager_init(): the display pointer, its
    // resolution and the mode of the last FULL rebuild, plus a count of
    // rebuilds. A repeat call for an unchanged target skips the whole
    // registration pass, which otherwise re-parses ui_xml/ from scratch.
    bool fully_initialized = false;
    int32_t init_h_res = 0;
    int32_t init_v_res = 0;
    int full_init_count = 0;
};

ThemeRuntime& runtime();

/// The subjects the theme publishes to XML. deinit() tears them all down before
/// lv_deinit() so no widget deletion fires a stale observer callback.
struct ThemeSubjects {
    static constexpr size_t kSwatchCount = 16;
    static constexpr size_t kSwatchBufSize = 32;

    /// Monotonic generation counter; observers re-read colours when it moves.
    lv_subject_t changed{};
    int32_t generation = 0;
    bool changed_ready = false;

    /// Cramped-axis tier (0=MICRO..6=XXLARGE) for reactive responsive visibility.
    lv_subject_t breakpoint{};
    bool breakpoint_ready = false;

    /// Vertical-axis tier, for height-aware layout that ui_breakpoint cannot
    /// express because its tier follows the cramped axis.
    lv_subject_t breakpoint_v{};
    bool breakpoint_v_ready = false;

    /// 1 for any portrait class, 0 otherwise. A tier cannot answer "is this
    /// portrait": 480x800 and 800x480 can land on the same one.
    lv_subject_t is_portrait{};
    bool is_portrait_ready = false;

    /// Theme-editor swatch descriptions, bound by name from XML.
    lv_subject_t swatch_desc[kSwatchCount]{};
    char swatch_bufs[kSwatchCount][kSwatchBufSize]{};
    bool swatch_ready = false;

    void deinit();
};

ThemeSubjects& subjects();

} // namespace helix::theme_detail
