// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme_manager.h"

#include "ui_button.h"
#include "ui_error_reporting.h"
#include "ui_fonts.h"
#include "ui_gradient_canvas.h"
#include "ui_icon.h"
#include "ui_observer_guard.h"
#include "ui_split_button.h"
#include "ui_switch.h"

#include "asset_manager.h"
#include "border_radius_sizes.h"
#include "config.h"
#include "data_root_resolver.h"
#include "display_metrics.h"
#include "helix-xml/src/libs/expat/expat.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "layout_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/themes/lv_theme_private.h"
#include "settings_manager.h"
#include "text_io.h"
#include "theme_loader.h"
#include "theme_manager_internal.h"
#include "theme_token_table.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __ANDROID__
#include "system/android_jni.h"

#include <SDL_system.h>
#include <jni.h>

/// Push the theme's screen_bg color to Android's window background so the area
/// behind transparent system bars matches the app theme (dark or light).
static void android_set_window_bg_color(lv_color_t color) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env)
        return;

    // Cached global ref owned by helix_activity_class() — never released here.
    jclass cls = helix::android::helix_activity_class(env);
    if (!cls)
        return;

    jmethodID method = env->GetStaticMethodID(cls, "setWindowBackgroundColor", "(I)V");
    if (!method) {
        env->ExceptionClear();
        return;
    }

    // Pack as 0xFFRRGGBB (fully opaque)
    uint32_t rgb = lv_color_to_u32(color);
    jint argb = static_cast<jint>(0xFF000000u | rgb);
    env->CallStaticVoidMethod(cls, method, argb);
}
#endif // __ANDROID__

using namespace helix;

namespace helix::theme_detail {

ThemeRuntime& runtime() {
    static ThemeRuntime rt;
    return rt;
}

ThemeSubjects& subjects() {
    static ThemeSubjects subs;
    return subs;
}

void ThemeSubjects::deinit() {
    auto drop = [](lv_subject_t& subject, bool& ready) {
        if (ready) {
            lv_subject_deinit(&subject);
            ready = false;
        }
    };
    if (changed_ready) {
        generation = 0;
    }
    drop(changed, changed_ready);
    drop(breakpoint, breakpoint_ready);
    drop(breakpoint_v, breakpoint_v_ready);
    drop(is_portrait, is_portrait_ready);
    if (swatch_ready) {
        for (auto& subject : swatch_desc) {
            lv_subject_deinit(&subject);
        }
        swatch_ready = false;
    }
}

} // namespace helix::theme_detail

using helix::theme_detail::runtime;
using helix::theme_detail::subjects;
using helix::theme_detail::ThemeSubjects;

// Color-swap map for container theming (replaces name-based heuristics)
struct ColorSwapEntry {
    lv_color_t from;
    lv_color_t to;
};

static std::vector<ColorSwapEntry> bg_swap_map;
static std::vector<ColorSwapEntry> border_swap_map;

static bool color_eq(lv_color_t a, lv_color_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

/// Add entry to swap map, skipping duplicates where `from` already exists.
/// Logs a debug warning on collision so theme authors can spot flattened palettes.
static void swap_map_add(std::vector<ColorSwapEntry>& map, lv_color_t from, lv_color_t to,
                         const char* name) {
    for (const auto& e : map) {
        if (color_eq(e.from, from)) {
            spdlog::debug("[Theme] Swap map collision: '{}' has same color as earlier entry "
                          "(0x{:02X}{:02X}{:02X}), skipping",
                          name, from.red, from.green, from.blue);
            return;
        }
    }
    map.push_back({from, to});
}

// ============================================================================
// LVGL Theme Infrastructure (formerly in theme_compat.cpp)
// ============================================================================

// Static theme instance - persists for lifetime of app
static lv_theme_t helix_theme;
static lv_theme_t* default_theme_backup = nullptr;

// Additional styles not in StyleRole enum (widget-specific parts)
static lv_style_t dropdown_indicator_style;
static lv_style_t checkbox_text_style;
static lv_style_t checkbox_box_style;
static lv_style_t checkbox_indicator_style;
static lv_style_t switch_track_style;
static lv_style_t switch_indicator_style;
static lv_style_t switch_knob_style;
static lv_style_t slider_track_style;
static lv_style_t slider_indicator_style;
static lv_style_t slider_knob_style;
static lv_style_t slider_disabled_style;
static lv_color_t dropdown_accent_color;
static bool extra_styles_initialized = false;

/**
 * @brief 16-color semantic palette for theme initialization (internal use)
 */
struct theme_palette_t {
    lv_color_t screen_bg;   // 0: Main app background
    lv_color_t overlay_bg;  // 1: Sidebar/panel background
    lv_color_t card_bg;     // 2: Card surfaces
    lv_color_t elevated_bg; // 3: Elevated/control surfaces (buttons, inputs)
    lv_color_t border;      // 4: Borders and dividers
    lv_color_t text;        // 5: Primary text
    lv_color_t text_muted;  // 6: Secondary text
    lv_color_t text_subtle; // 7: Hint/tertiary text
    lv_color_t primary;     // 8: Primary accent
    lv_color_t secondary;   // 9: Secondary accent
    lv_color_t tertiary;    // 10: Tertiary accent
    lv_color_t info;        // 11: Info states
    lv_color_t success;     // 12: Success states
    lv_color_t warning;     // 13: Warning states
    lv_color_t danger;      // 14: Error/danger states
    lv_color_t focus;       // 15: Focus ring color
};

// Forward declarations for theme infrastructure
static void init_extra_styles(const theme_palette_t* palette, int border_radius);
static void update_handle_styles(const theme_palette_t* palette, int border_radius);
static void helix_theme_apply(lv_theme_t* theme, lv_obj_t* obj);

/**
 * @brief Build theme_palette_t from ModePalette
 *
 * Converts the C++ ModePalette struct (hex strings) to C theme_palette_t (lv_color_t).
 * Used to pass colors to theme_core functions.
 *
 * @param mode_palette ModePalette with hex color strings
 * @return theme_palette_t with parsed lv_color_t values
 */
static theme_palette_t build_palette_from_mode(const helix::ModePalette& mode_palette) {
    theme_palette_t palette = {};
    palette.screen_bg = theme_manager_parse_hex_color(mode_palette.screen_bg.c_str());
    palette.overlay_bg = theme_manager_parse_hex_color(mode_palette.overlay_bg.c_str());
    palette.card_bg = theme_manager_parse_hex_color(mode_palette.card_bg.c_str());
    palette.elevated_bg = theme_manager_parse_hex_color(mode_palette.elevated_bg.c_str());
    palette.border = theme_manager_parse_hex_color(mode_palette.border.c_str());
    palette.text = theme_manager_parse_hex_color(mode_palette.text.c_str());
    palette.text_muted = theme_manager_parse_hex_color(mode_palette.text_muted.c_str());
    palette.text_subtle = theme_manager_parse_hex_color(mode_palette.text_subtle.c_str());
    palette.primary = theme_manager_parse_hex_color(mode_palette.primary.c_str());
    palette.secondary = theme_manager_parse_hex_color(mode_palette.secondary.c_str());
    palette.tertiary = theme_manager_parse_hex_color(mode_palette.tertiary.c_str());
    palette.info = theme_manager_parse_hex_color(mode_palette.info.c_str());
    palette.success = theme_manager_parse_hex_color(mode_palette.success.c_str());
    palette.warning = theme_manager_parse_hex_color(mode_palette.warning.c_str());
    palette.danger = theme_manager_parse_hex_color(mode_palette.danger.c_str());
    palette.focus = theme_manager_parse_hex_color(mode_palette.focus.c_str());
    return palette;
}

/**
 * @brief Get the current mode palette based on dark/light mode
 *
 * Returns reference to appropriate ModePalette from active_theme.
 * Falls back to the available palette if the requested mode is not supported.
 */
static const helix::ModePalette& get_current_mode_palette() {
    if (runtime().dark && runtime().active_theme.supports_dark()) {
        return runtime().active_theme.dark;
    } else if (!runtime().dark && runtime().active_theme.supports_light()) {
        return runtime().active_theme.light;
    } else if (runtime().active_theme.supports_dark()) {
        return runtime().active_theme.dark;
    } else {
        return runtime().active_theme.light;
    }
}

// ============================================================================
// LVGL Theme Infrastructure - Apply Callbacks & Style Initialization
// ============================================================================

/**
 * @brief Update handle/knob styles from current theme properties
 *
 * Called on initial setup and on every theme switch to apply handle_style
 * and handle_color from the active theme. Switch knobs always stay round.
 */
static void update_handle_styles(const theme_palette_t* palette, int border_radius) {
    bool bar_knob = (runtime().active_theme.properties.handle_style == "bar");
    int32_t slider_knob_radius = bar_knob ? 2 : LV_RADIUS_CIRCLE;

    // Resolve handle color token to palette color
    lv_color_t knob_color = palette->primary;
    const auto& hc = runtime().active_theme.properties.handle_color;
    if (hc == "text")
        knob_color = palette->text;
    else if (hc == "secondary")
        knob_color = palette->secondary;
    else if (hc == "tertiary")
        knob_color = palette->tertiary;

    // Switch knob: handle_color applies, but always round (no bar style)
    lv_style_set_bg_color(&switch_knob_style, knob_color);

    // Slider track/indicator colors
    lv_style_set_bg_color(&slider_track_style, palette->border);
    lv_style_set_radius(&slider_track_style, border_radius);
    lv_style_set_bg_color(&slider_indicator_style, palette->primary);

    // Slider knob: both handle_color and handle_style apply
    lv_style_set_bg_color(&slider_knob_style, knob_color);
    lv_style_set_border_color(&slider_knob_style, palette->border);
    lv_style_set_border_width(&slider_knob_style, bar_knob ? 0 : 1);
    lv_style_set_radius(&slider_knob_style, slider_knob_radius);
    if (bar_knob) {
        lv_style_set_pad_left(&slider_knob_style, -4);
        lv_style_set_pad_right(&slider_knob_style, -4);
        lv_style_set_pad_top(&slider_knob_style, 8);
        lv_style_set_pad_bottom(&slider_knob_style, 8);
    } else {
        // Responsive knob padding: smaller at tiny/micro to avoid clipping in compact cards
        auto* display = lv_display_get_default();
        auto bp = display ? breakpoint_for(responsive_dimension(display)) : UiBreakpoint::Medium;
        int32_t knob_pad = (bp <= UiBreakpoint::Tiny) ? LV_DPX(4) : LV_DPX(6);
        lv_style_set_pad_left(&slider_knob_style, knob_pad);
        lv_style_set_pad_right(&slider_knob_style, knob_pad);
        lv_style_set_pad_top(&slider_knob_style, knob_pad);
        lv_style_set_pad_bottom(&slider_knob_style, knob_pad);
    }

    // Slider knob shadow: functional depth cue
    int knob_shadow_w = runtime().active_theme.properties.shadow_intensity > 0
                            ? runtime().active_theme.properties.shadow_intensity
                            : 4;
    int knob_shadow_opa = runtime().active_theme.properties.shadow_opa > 0
                              ? runtime().active_theme.properties.shadow_opa
                              : LV_OPA_30;
    lv_style_set_shadow_width(&slider_knob_style, knob_shadow_w);
    lv_style_set_shadow_color(&slider_knob_style, lv_color_black());
    lv_style_set_shadow_opa(&slider_knob_style, static_cast<lv_opa_t>(knob_shadow_opa));

    // Update dropdown accent and other palette-dependent colors
    dropdown_accent_color = palette->secondary;
    lv_style_set_text_color(&checkbox_text_style, palette->text);
    lv_style_set_bg_color(&checkbox_box_style, palette->elevated_bg);
    lv_style_set_border_color(&checkbox_box_style, palette->border);
    lv_style_set_bg_color(&checkbox_indicator_style, palette->primary);
    lv_style_set_border_color(&checkbox_indicator_style, palette->primary);
    uint8_t cb_lum = lv_color_luminance(palette->primary);
    lv_style_set_text_color(&checkbox_indicator_style,
                            (cb_lum > 140) ? lv_color_black() : lv_color_white());
    lv_style_set_bg_color(&switch_track_style, palette->border);
    lv_style_set_bg_color(&switch_indicator_style, palette->secondary);
}

/**
 * @brief Initialize the extra widget-specific styles
 *
 * These are styles for widget parts not covered by the StyleRole enum.
 */
static void init_extra_styles(const theme_palette_t* palette, int border_radius) {
    if (extra_styles_initialized)
        return;

    dropdown_accent_color = palette->secondary;

    // Dropdown indicator - MDI font for chevron
    lv_style_init(&dropdown_indicator_style);
    lv_style_set_text_font(&dropdown_indicator_style, &mdi_icons_24);

    // Checkbox styles
    lv_style_init(&checkbox_text_style);
    lv_style_set_text_color(&checkbox_text_style, palette->text);

    lv_style_init(&checkbox_box_style);
    lv_style_set_bg_color(&checkbox_box_style, palette->elevated_bg);
    lv_style_set_bg_opa(&checkbox_box_style, LV_OPA_COVER);
    lv_style_set_border_color(&checkbox_box_style, palette->border);
    lv_style_set_border_width(&checkbox_box_style, 2);
    lv_style_set_radius(&checkbox_box_style, 4);

    lv_style_init(&checkbox_indicator_style);
    lv_style_set_bg_color(&checkbox_indicator_style, palette->primary);
    lv_style_set_bg_opa(&checkbox_indicator_style, LV_OPA_COVER);
    lv_style_set_border_color(&checkbox_indicator_style, palette->primary);
    // Checkmark: set bg_image_src to bold check symbol, rendered via text_font
    lv_style_set_bg_image_src(&checkbox_indicator_style, LV_SYMBOL_OK);
    lv_style_set_text_font(&checkbox_indicator_style, &mdi_icons_16);
    // Contrast text color based on primary luminance (same pattern as ui_button)
    uint8_t cb_lum = lv_color_luminance(palette->primary);
    lv_style_set_text_color(&checkbox_indicator_style,
                            (cb_lum > 140) ? lv_color_black() : lv_color_white());

    // Switch styles
    lv_style_init(&switch_track_style);
    lv_style_set_bg_color(&switch_track_style, palette->border);
    lv_style_set_bg_opa(&switch_track_style, LV_OPA_COVER);

    lv_style_init(&switch_indicator_style);
    lv_style_set_bg_color(&switch_indicator_style, palette->secondary);
    lv_style_set_bg_opa(&switch_indicator_style, LV_OPA_COVER);

    lv_style_init(&switch_knob_style);
    lv_style_set_bg_opa(&switch_knob_style, LV_OPA_COVER);
    lv_style_set_radius(&switch_knob_style, LV_RADIUS_CIRCLE);

    // Slider styles
    lv_style_init(&slider_track_style);
    lv_style_set_bg_opa(&slider_track_style, LV_OPA_COVER);

    lv_style_init(&slider_indicator_style);
    lv_style_set_bg_opa(&slider_indicator_style, LV_OPA_COVER);

    lv_style_init(&slider_knob_style);
    lv_style_set_bg_opa(&slider_knob_style, LV_OPA_COVER);

    lv_style_init(&slider_disabled_style);
    lv_style_set_opa(&slider_disabled_style, LV_OPA_50);

    extra_styles_initialized = true;

    // Apply theme-dependent handle styles (also called on theme switch)
    update_handle_styles(palette, border_radius);
}

// Forward declaration — full definition is below with palette apply functions
static bool is_on_elevated_surface(lv_obj_t* obj);

/**
 * @brief HelixScreen theme apply callback - applies styles based on widget type
 *
 * This is called by LVGL for every widget created. It first applies the default
 * theme, then layers our custom styles on top.
 */
static void helix_theme_apply(lv_theme_t* theme, lv_obj_t* obj) {
    (void)theme;

    // First apply LVGL default theme (provides base padding, switch tracks, etc.)
    if (default_theme_backup && default_theme_backup->apply_cb) {
        default_theme_backup->apply_cb(default_theme_backup, obj);
    }

    auto& tm = ThemeManager::instance();

    // Global disabled state
    lv_obj_add_style(obj, tm.get_style(StyleRole::Disabled), LV_PART_MAIN | LV_STATE_DISABLED);

    // Plain lv_obj containers get transparent background (layout containers)
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::ObjBase), LV_PART_MAIN);
    }

#if LV_USE_BUTTON
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::Button), LV_PART_MAIN);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Pressed), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);
    }
#endif

#if LV_USE_TEXTAREA
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
    }
#endif

#if LV_USE_DROPDOWN
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
        lv_obj_add_style(obj, &dropdown_indicator_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);

        // Force local radius to theme value — LVGL's default theme can set
        // a larger radius on dropdown buttons that survives our added styles.
        // Local styles always win over added styles, so this guarantees
        // dropdowns render at the theme's border_radius.
        lv_obj_set_style_radius(obj, tm.current_palette().border_radius, LV_PART_MAIN);

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
    }
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);

        // Clip highlight rectangles to rounded corners
        lv_obj_set_style_clip_corner(obj, true, LV_PART_MAIN);

        // Add responsive line spacing (1x font height) for comfortable touch targets
        const lv_font_t* list_font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        if (list_font) {
            int32_t line_space = lv_font_get_line_height(list_font);
            lv_obj_set_style_text_line_space(obj, line_space, LV_PART_MAIN);
        }

        // Compute contrast text for dropdown accent
        uint8_t lum = lv_color_luminance(dropdown_accent_color);
        lv_color_t selected_text = (lum > 140) ? lv_color_black() : lv_color_white();

        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(obj, selected_text, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(obj, dropdown_accent_color,
                                  LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER,
                                LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(obj, selected_text,
                                    LV_PART_SELECTED | LV_STATE_CHECKED | LV_STATE_PRESSED);
    }
#endif

#if LV_USE_ROLLER
    if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);
    }
#endif

#if LV_USE_SPINBOX
    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_obj_add_style(obj, tm.get_style(StyleRole::InputBg), LV_PART_MAIN);

        // On elevated surfaces (dialogs, raised cards), override to overlay_bg for contrast
        if (is_on_elevated_surface(obj)) {
            lv_obj_set_style_bg_color(obj, tm.current_palette().overlay_bg, LV_PART_MAIN);
        }
    }
#endif

#if LV_USE_CHECKBOX
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &checkbox_text_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &checkbox_box_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &checkbox_indicator_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
    }
#endif

#if LV_USE_SWITCH
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &switch_track_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &switch_indicator_style, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_style(obj, &switch_knob_style, LV_PART_KNOB);
        lv_obj_add_style(obj, tm.get_style(StyleRole::Focused), LV_STATE_FOCUSED);
    }
#endif

#if LV_USE_SLIDER
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &slider_track_style, LV_PART_MAIN);
        lv_obj_add_style(obj, &slider_indicator_style, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &slider_knob_style, LV_PART_KNOB);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_MAIN | LV_STATE_DISABLED);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_INDICATOR | LV_STATE_DISABLED);
        lv_obj_add_style(obj, &slider_disabled_style, LV_PART_KNOB | LV_STATE_DISABLED);
    }
#endif
}

/**
 * @brief Resolve border radius pixels from size index + current display breakpoint.
 */
static int resolve_border_radius(const helix::ThemeProperties& props) {
    int32_t resp_res = responsive_dimension(runtime().display);
    const char* suffix = theme_manager_get_breakpoint_suffix(resp_res);
    return helix::BorderRadiusSizes::pixels(props.border_radius_size, suffix);
}

/**
 * @brief Convert theme_palette_t to ThemePalette for ThemeManager
 */
static ThemePalette convert_to_theme_palette(const theme_palette_t* p,
                                             const helix::ThemeProperties& props) {
    ThemePalette palette;
    palette.screen_bg = p->screen_bg;
    palette.overlay_bg = p->overlay_bg;
    palette.card_bg = p->card_bg;
    palette.elevated_bg = p->elevated_bg;
    palette.border = p->border;
    palette.text = p->text;
    palette.text_muted = p->text_muted;
    palette.text_subtle = p->text_subtle;
    palette.primary = p->primary;
    palette.secondary = p->secondary;
    palette.tertiary = p->tertiary;
    palette.info = p->info;
    palette.success = p->success;
    palette.warning = p->warning;
    palette.danger = p->danger;
    palette.focus = p->focus;
    palette.border_radius = resolve_border_radius(props);
    palette.button_radius = helix::BorderRadiusSizes::button_pixels(
        props.border_radius_size,
        theme_manager_get_breakpoint_suffix(responsive_dimension(runtime().display)));
    palette.border_width = props.border_width;
    palette.border_opacity = props.border_opacity;
    palette.shadow_width = props.shadow_intensity;
    palette.shadow_opa = props.shadow_opa;
    palette.shadow_offset_y = props.shadow_offset_y;
    return palette;
}

/**
 * @brief Sync the mutable palette manager to active_theme
 *
 * The legacy ThemeManager can be flipped in place (dark-mode toggle,
 * palette previews, tests); the next theme_manager_init() call is the
 * boundary that re-syncs it with active_theme. Pure in-memory
 * conversion - no file or XML parsing - so both the full init and the
 * repeat-skip path can afford it.
 */
static void resync_palette_manager(bool is_dark) {
    // Build palettes from active_theme for contrast calculations.
    // For single-mode themes, use the valid palette for both sides to avoid
    // parsing empty color strings from the unsupported mode.
    bool has_dark = runtime().active_theme.supports_dark();
    bool has_light = runtime().active_theme.supports_light();
    const auto& dark_src = has_dark ? runtime().active_theme.dark : runtime().active_theme.light;
    const auto& light_src = has_light ? runtime().active_theme.light : runtime().active_theme.dark;
    theme_palette_t dark_theme_pal = build_palette_from_mode(dark_src);
    theme_palette_t light_theme_pal = build_palette_from_mode(light_src);

    const auto& props = runtime().active_theme.properties;
    ThemePalette dark_pal = convert_to_theme_palette(&dark_theme_pal, props);
    ThemePalette light_pal = convert_to_theme_palette(&light_theme_pal, props);

    auto& tm = ThemeManager::instance();
    tm.set_palettes(light_pal, dark_pal);
    tm.init();
    tm.set_dark_mode(is_dark);
}

/**
 * @brief Initialize the HelixScreen LVGL theme
 *
 * Sets up ThemeManager, initializes extra widget styles, and registers
 * the helix_theme with LVGL.
 */
static lv_theme_t* theme_init_lvgl(lv_display_t* display, const theme_palette_t* palette,
                                   bool is_dark, const lv_font_t* base_font) {
    resync_palette_manager(is_dark);

    // Initialize widget-specific styles not in StyleRole enum
    const auto& props = runtime().active_theme.properties;
    init_extra_styles(palette, resolve_border_radius(props));

    // Create LVGL default theme as base (we'll layer on top)
    default_theme_backup =
        lv_theme_default_init(display, palette->primary, palette->secondary, is_dark, base_font);

    // Initialize our custom theme
    lv_theme_set_apply_cb(&helix_theme, helix_theme_apply);
    helix_theme.font_small = base_font;
    helix_theme.font_normal = base_font;
    helix_theme.font_large = base_font;
    helix_theme.color_primary = palette->primary;
    helix_theme.color_secondary = palette->secondary;

    spdlog::trace("[Theme] Initialized HelixScreen theme via ThemeManager");
    return &helix_theme;
}

/**
 * @brief Update theme colors without full re-initialization
 */
static void theme_update_colors(bool is_dark) {
    auto& tm = ThemeManager::instance();

    // Build palettes, falling back to the valid mode for single-mode themes
    bool has_dark = runtime().active_theme.supports_dark();
    bool has_light = runtime().active_theme.supports_light();
    const auto& dark_src = has_dark ? runtime().active_theme.dark : runtime().active_theme.light;
    const auto& light_src = has_light ? runtime().active_theme.light : runtime().active_theme.dark;
    theme_palette_t dark_theme_pal = build_palette_from_mode(dark_src);
    theme_palette_t light_theme_pal = build_palette_from_mode(light_src);

    const auto& props = runtime().active_theme.properties;
    ThemePalette dark_pal = convert_to_theme_palette(&dark_theme_pal, props);
    ThemePalette light_pal = convert_to_theme_palette(&light_theme_pal, props);

    tm.set_palettes(light_pal, dark_pal);

    tm.set_dark_mode(is_dark);

    // Update handle/knob styles from new theme properties and palette
    const theme_palette_t& current_pal = is_dark ? dark_theme_pal : light_theme_pal;
    update_handle_styles(&current_pal, resolve_border_radius(props));

    spdlog::debug("[Theme] Updated colors, dark_mode={}", is_dark);
}

/**
 * Auto-register theme-aware color constants from all XML files
 *
 * Parses all XML files in ui_xml/ to find color pairs (xxx_light, xxx_dark) and registers
 * the base name (xxx) as a runtime constant with the appropriate value
 * based on current theme mode.
 */
static void theme_manager_register_color_pairs(lv_xml_component_scope_t* scope, bool dark_mode) {
    // Find all color tokens with _light and _dark suffixes from all XML files
    auto light_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "color", "_light");
    auto dark_tokens =
        theme_manager_parse_all_xml_for_suffix(theme_detail::ui_xml_dir(), "color", "_dark");

    // For each _light color, check if _dark exists and register base name
    int registered = 0;
    for (const auto& [base_name, light_val] : light_tokens) {
        auto dark_it = dark_tokens.find(base_name);
        if (dark_it != dark_tokens.end()) {
            const char* selected = dark_mode ? dark_it->second.c_str() : light_val.c_str();
            spdlog::trace("[Theme] Registering color {}: selected={}", base_name, selected);
            lv_xml_register_const(scope, base_name.c_str(), selected);
            registered++;
        }
    }

    spdlog::trace("[Theme] Auto-registered {} theme-aware color pairs (dark_mode={})", registered,
                  dark_mode);
}

/**
 * Register static constants from all XML files
 *
 * Parses all XML files for <color>, <px>, and <string> elements and registers
 * any that do NOT have dynamic suffixes (_light, _dark, _small, _medium, _large).
 * These static constants are registered first so dynamic variants can override them.
 */
static void theme_manager_register_static_constants(lv_xml_component_scope_t* scope) {
    const std::vector<std::string> skip_suffixes = {
        "_light", "_dark", "_micro", "_tiny", "_small", "_medium", "_large", "_xlarge", "_xxlarge"};

    auto has_dynamic_suffix = [&](const std::string& name) {
        for (const auto& suffix : skip_suffixes) {
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        }
        return false;
    };

    int color_count = 0, px_count = 0, string_count = 0;

    auto color_tokens =
        theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "color");

    for (const auto& [name, value] : color_tokens) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            color_count++;
        }
    }

    const double px_scale = helix::DisplayMetrics::active_scale();
    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "px")) {
        if (!has_dynamic_suffix(name)) {
            // Static tokens scale too. They are fixed-size boxes (icon badges,
            // chips, swatches, column widths) whose contents are scaled fonts,
            // so leaving them authored-size is what makes the glyph overflow.
            const std::string scaled = theme_detail::scale_px_token(name, value, px_scale);
            lv_xml_register_const(scope, name.c_str(), scaled.c_str());
            px_count++;
        }
    }

    for (const auto& [name, value] :
         theme_manager_parse_all_xml_for_element(theme_detail::ui_xml_dir(), "string")) {
        if (!has_dynamic_suffix(name)) {
            lv_xml_register_const(scope, name.c_str(), value.c_str());
            string_count++;
        }
    }

    spdlog::debug(
        "[Theme] Registered {} static colors, {} static px, {} static strings (ui_scale={:.3f})",
        color_count, px_count, string_count, px_scale);
}

/**
 * @brief Register semantic colors from dual-palette system
 *
 * Uses the new ModePalette from theme.dark and theme.light to register
 * all 16 semantic color names with _light/_dark variants.
 *
 * For themes with only one mode (dark-only or light-only), only the available
 * variant is registered. For dual-mode themes, both variants are registered.
 *
 * Also registers legacy aliases for backward compatibility with existing XML.
 *
 * @param scope LVGL XML scope to register constants in
 * @param theme Theme data with dual palettes
 * @param dark_mode Whether to use dark mode values for base names
 */
static void theme_manager_register_semantic_colors(lv_xml_component_scope_t* scope,
                                                   const helix::ThemeData& theme, bool dark_mode) {
    // Check which palettes are available
    bool has_dark = theme.supports_dark();
    bool has_light = theme.supports_light();

    // Determine which palette to use for base name registration
    // For dark-only themes in light mode, still use dark palette
    // For light-only themes in dark mode, still use light palette
    const helix::ModePalette* current_palette = nullptr;
    if (dark_mode && has_dark) {
        current_palette = &theme.dark;
    } else if (!dark_mode && has_light) {
        current_palette = &theme.light;
    } else if (has_dark) {
        current_palette = &theme.dark;
    } else if (has_light) {
        current_palette = &theme.light;
    }

    if (!current_palette) {
        spdlog::error("[Theme] No valid palette available in theme");
        return;
    }

    // Register helper - registers base, _dark, and _light variants (if available)
    auto register_color = [&](const char* name, size_t index) {
        const std::string& current_val = current_palette->at(index);

        char dark_name[128], light_name[128];
        snprintf(dark_name, sizeof(dark_name), "%s_dark", name);
        snprintf(light_name, sizeof(light_name), "%s_light", name);

        // Register base name with current mode's value
        if (!current_val.empty()) {
            lv_xml_register_const(scope, name, current_val.c_str());
        }

        // Register _dark variant if dark palette is available
        if (has_dark) {
            const std::string& dark_val = theme.dark.at(index);
            if (!dark_val.empty()) {
                lv_xml_register_const(scope, dark_name, dark_val.c_str());
            }
        }

        // Register _light variant if light palette is available
        if (has_light) {
            const std::string& light_val = theme.light.at(index);
            if (!light_val.empty()) {
                lv_xml_register_const(scope, light_name, light_val.c_str());
            }
        }
    };

    // Register all 16 semantic colors from ModePalette
    auto& names = helix::ModePalette::color_names();
    for (size_t i = 0; i < 16; ++i) {
        register_color(names[i], i);
    }

    // Swatch descriptions for theme editor - registered as string subjects
    // so bind_text="swatch_N_desc" works in XML (consts don't resolve for bind_text)
    static constexpr const char* swatch_descriptions[ThemeSubjects::kSwatchCount] = {
        "App background",    "Panel/sidebar background", "Card surfaces",
        "Elevated surfaces", "Borders and dividers",     "Primary text",
        "Secondary text",    "Subtle/hint text",         "Primary accent",
        "Secondary accent",  "Tertiary accent",          "Info states",
        "Success states",    "Warning states",           "Danger/error states",
        "Focus ring",
    };

    if (!subjects().swatch_ready) {
        for (size_t i = 0; i < ThemeSubjects::kSwatchCount; ++i) {
            lv_subject_init_string(&subjects().swatch_desc[i], subjects().swatch_bufs[i], nullptr,
                                   ThemeSubjects::kSwatchBufSize, swatch_descriptions[i]);
            char key[24];
            snprintf(key, sizeof(key), "swatch_%zu_desc", i);
            ObserverGuard::mark_subject_teardown_exempt(&subjects().swatch_desc[i]);
            lv_xml_register_subject(nullptr, key, &subjects().swatch_desc[i]);
        }
        subjects().swatch_ready = true;
    }

    spdlog::debug("[Theme] Registered 16 semantic colors + legacy aliases (dark={}, light={})",
                  has_dark, has_light);
}

/**
 * @brief Register theme properties (border_radius, border_width, etc.) as XML constants
 *
 * These override the default values from globals.xml, allowing themes to customize
 * geometry like corner radius and border width - similar to how colors work.
 *
 * IMPORTANT: Must be called BEFORE theme_manager_register_static_constants() since
 * LVGL ignores duplicate lv_xml_register_const calls (first registration wins).
 *
 * @param scope LVGL XML scope to register constants in
 * @param theme Theme data with properties
 */
static void theme_manager_register_theme_properties(lv_xml_component_scope_t* scope,
                                                    const helix::ThemeData& theme, bool dark_mode) {
    char buf[32];

    // Register border_radius and button_radius from size table + current breakpoint
    int32_t resp_res = responsive_dimension(runtime().display);
    const char* suffix = theme_manager_get_breakpoint_suffix(resp_res);
    int radius_px = helix::BorderRadiusSizes::pixels(theme.properties.border_radius_size, suffix);
    snprintf(buf, sizeof(buf), "%d", radius_px);
    lv_xml_register_const(scope, "border_radius", buf);

    // Register border_width
    snprintf(buf, sizeof(buf), "%d", theme.properties.border_width);
    lv_xml_register_const(scope, "border_width", buf);

    // Register border_opacity (0-255)
    snprintf(buf, sizeof(buf), "%d", theme.properties.border_opacity);
    lv_xml_register_const(scope, "border_opacity", buf);

    // Register shadow properties
    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_intensity);
    lv_xml_register_const(scope, "shadow_intensity", buf);

    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_opa);
    lv_xml_register_const(scope, "shadow_opa", buf);

    snprintf(buf, sizeof(buf), "%d", theme.properties.shadow_offset_y);
    lv_xml_register_const(scope, "shadow_offset_y", buf);

    // The colour a cast shadow is drawn in. Fixed, not part of the themeable
    // palette: a shadow is the absence of light in both light and dark themes,
    // and the palette's 16 semantic slots are surfaces/text/accents. Exists so
    // XML never has to hardcode a hex (see ui_xml/overlay_panel.xml).
    lv_xml_register_const(scope, "shadow_cast", "0x000000");

    // Opacity for the transient-overlay cast shadow (#1178). Mode-dependent
    // because the same alpha reads very differently against the surface behind
    // it: on dark themes the strip is already near-black and the shadow needs
    // weight to register at all, while on light themes it lands on a white
    // panel and the same value reads as a heavy black band.
    lv_xml_register_const(scope, "overlay_shadow_opa", dark_mode ? "200" : "100");

    spdlog::debug("[Theme] Registered properties: border_radius={}px (size={}, {}), "
                  "border_width={}, border_opacity={}, shadow=({},{},{})",
                  radius_px, theme.properties.border_radius_size,
                  helix::BorderRadiusSizes::name(theme.properties.border_radius_size),
                  theme.properties.border_width, theme.properties.border_opacity,
                  theme.properties.shadow_intensity, theme.properties.shadow_opa,
                  theme.properties.shadow_offset_y);
}

/**
 * @brief Register fixed object color palette tokens for the exclude-object map view.
 *
 * These 8 colors are theme-invariant — they are chosen to be distinguishable on
 * dark thumbnail backgrounds. They are registered as hard-coded constants so they
 * are available regardless of whether globals.xml is loaded from the filesystem.
 *
 * Registered tokens: object_color_1 through object_color_8.
 * LVGL ignores duplicate lv_xml_register_const calls (first registration wins).
 *
 * @param scope LVGL XML scope to register constants in
 */
static void theme_manager_register_object_colors(lv_xml_component_scope_t* scope) {
    static const struct {
        const char* name;
        uint32_t hex;
    } OBJECT_COLORS[] = {
        {"object_color_1", 0x7c8aff}, // periwinkle blue
        {"object_color_2", 0x4ecdc4}, // teal
        {"object_color_3", 0xf9c74f}, // golden yellow
        {"object_color_4", 0xa78bfa}, // soft purple
        {"object_color_5", 0xf472b6}, // pink
        {"object_color_6", 0xfb923c}, // orange
        {"object_color_7", 0x34d399}, // emerald
        {"object_color_8", 0x60a5fa}, // sky blue
    };

    for (const auto& entry : OBJECT_COLORS) {
        char buf[8];
        snprintf(buf, sizeof(buf), "#%06x", entry.hex);
        lv_xml_register_const(scope, entry.name, buf);
    }

    spdlog::debug("[Theme] Registered {} object color palette tokens", std::size(OBJECT_COLORS));
}

/**
 * @brief Load active theme from config
 *
 * Reads /display/theme from config, loads corresponding JSON file.
 * Falls back to the compiled-in HelixScreen theme if not found.
 *
 * HELIX_THEME env var overrides config (useful for testing/screenshots).
 */
static helix::ThemeData theme_manager_load_active_theme() {
    std::string themes_dir = helix::get_themes_directory();

    // Ensure the user themes directory exists (shipped themes live in defaults/)
    helix::ensure_themes_directory(themes_dir);

    // Check for HELIX_THEME env var override (useful for testing/screenshots)
    std::string theme_name;
    const char* env_theme = std::getenv("HELIX_THEME");
    if (env_theme && env_theme[0] != '\0') {
        theme_name = env_theme;
        spdlog::info("[Theme] Using HELIX_THEME override: {}", theme_name);
    } else {
        // Read theme name from config
        Config* config = Config::get_instance();
        theme_name = config->get<std::string>("/display/theme", helix::DEFAULT_THEME);
    }

    // Load theme file (supports fallback from user themes to defaults)
    auto theme = helix::load_theme_from_file(theme_name);

    if (!theme.is_valid()) {
        spdlog::warn("[Theme] Theme '{}' not found or invalid, using built-in {}", theme_name,
                     helix::DEFAULT_THEME);
        theme = helix::get_builtin_fallback_theme();
    }

    spdlog::info("[Theme] Loaded theme: {} ({})", theme.name, theme.filename);
    return theme;
}

void theme_manager_init(lv_display_t* display, bool use_dark_mode_param) {
    auto tm_init_start = std::chrono::steady_clock::now();

    // Repeat guard: the same display at the same size in the same mode, with
    // the theme still initialized, needs nothing re-registered. theme_manager_
    // apply_theme() owns mode changes and theme_manager_refresh_*() owns
    // republishing, and theme_manager_deinit() clears theme_subject_initialized,
    // so every path that actually changes the state re-runs.
    const int32_t h_res = display ? lv_display_get_horizontal_resolution(display) : 0;
    const int32_t v_res = display ? lv_display_get_vertical_resolution(display) : 0;
    if (runtime().fully_initialized && runtime().display == display &&
        use_dark_mode_param == runtime().dark && h_res == runtime().init_h_res &&
        v_res == runtime().init_v_res && subjects().changed_ready) {
        // The registration pass can stay skipped, but two pieces of mutable
        // state still have to come back in line, because this call is the
        // boundary that restores them.
        //
        // The palette manager, because theme previews and dark-mode toggles
        // flip it in place. A no-op flip is cheap (the setters early-return).
        resync_palette_manager(use_dark_mode_param);

        // The responsive state, because ui_breakpoint is process-global and
        // helix::widget_size::current_breakpoint() reads it rather than the
        // display — so a value left disagreeing with the display decides
        // layout for every widget that asks afterwards, not just the caller
        // that moved it. This resolves px tokens and republishes the three
        // subjects from the display without re-reading ui_xml/, which is where
        // the cost this guard exists to skip actually lives.
        theme_manager_refresh_layout_constants(display);

        spdlog::debug("[Theme] theme_manager_init: unchanged target ({}x{} {}), reusing", h_res,
                      v_res, use_dark_mode_param);
        return;
    }

    runtime().display = display;
    runtime().dark = use_dark_mode_param;
    runtime().init_h_res = h_res;
    runtime().init_v_res = v_res;
    runtime().full_init_count++;

    // Initialize theme change notification subject
    if (!subjects().changed_ready) {
        lv_subject_init_int(&subjects().changed, 0);
        subjects().changed_ready = true;
        ObserverGuard::mark_subject_teardown_exempt(&subjects().changed);
    }

    // Override runtime theme constants based on light/dark mode preference
    lv_xml_component_scope_t* scope = lv_xml_component_get_scope("globals");
    if (!scope) {
        spdlog::critical(
            "[Theme] FATAL: Failed to get globals scope for runtime constant registration");
        std::exit(EXIT_FAILURE);
    }

    // Load active theme from config/themes directory
    runtime().active_theme = theme_manager_load_active_theme();

    // Register semantic colors from dual-palette system (includes _light/_dark variants and base
    // names) NOTE: Legacy palette registration removed - was causing token collisions (text_light
    // conflict)
    theme_manager_register_semantic_colors(scope, runtime().active_theme, runtime().dark);

    // Register theme properties (border_radius, etc.) - must be before static constants
    // so theme values override globals.xml defaults (first registration wins in LVGL)
    theme_manager_register_theme_properties(scope, runtime().active_theme, runtime().dark);

    // Register static constants (colors, px, strings without dynamic suffixes)
    theme_manager_register_static_constants(scope);

    // Register fixed object color palette tokens (theme-invariant, hard-coded)
    theme_manager_register_object_colors(scope);

    // Auto-register all color pairs from globals.xml (xxx_light/xxx_dark -> xxx)
    // This handles screen_bg, text, header_text, elevated_bg, card_bg, etc.
    theme_manager_register_color_pairs(scope, runtime().dark);

    // Register responsive constants (must be before theme init so fonts are available)
    theme_manager_register_responsive_spacing(display);
    theme_manager_register_responsive_fonts(display);

    // Initialize ui_breakpoint subject for reactive responsive visibility
    {
        int32_t resp_res = responsive_dimension(display);
        UiBreakpoint bp = breakpoint_for(resp_res);

        if (!subjects().breakpoint_ready) {
            lv_subject_init_int(&subjects().breakpoint, to_int(bp));
            subjects().breakpoint_ready = true;
        } else {
            lv_subject_set_int(&subjects().breakpoint, to_int(bp));
        }
        ObserverGuard::mark_subject_teardown_exempt(&subjects().breakpoint);
        lv_xml_register_subject(nullptr, "ui_breakpoint", &subjects().breakpoint);
        spdlog::debug("[Theme] Registered ui_breakpoint subject: {} (min_dim={})", to_int(bp),
                      resp_res);
    }

    // Vertical companion. ui_breakpoint deliberately keeps the cramped tier --
    // every ref_value in ui_xml/ is written against it -- so height-aware layout
    // binds to this instead of changing what the old subject means.
    {
        int32_t vert_res = responsive_vertical_dimension(display);
        UiBreakpoint vbp = breakpoint_for(vert_res);

        if (!subjects().breakpoint_v_ready) {
            lv_subject_init_int(&subjects().breakpoint_v, to_int(vbp));
            subjects().breakpoint_v_ready = true;
        } else {
            lv_subject_set_int(&subjects().breakpoint_v, to_int(vbp));
        }
        ObserverGuard::mark_subject_teardown_exempt(&subjects().breakpoint_v);
        lv_xml_register_subject(nullptr, "ui_breakpoint_v", &subjects().breakpoint_v);
        spdlog::debug("[Theme] Registered ui_breakpoint_v subject: {} (vert_dim={})", to_int(vbp),
                      vert_res);
    }

    // Startup SEED only. LayoutManager (Phase 8b) is not initialized yet, so
    // the override-aware value cannot be computed here; detect_layout_type() of
    // the live display gives the right physical answer, and Application
    // republishes via theme_manager_refresh_orientation() once LayoutManager
    // resolves any --layout override (#1255). The rotation path
    // (theme_manager_refresh_layout_constants) goes through the same helper.
    {
        int32_t hor_res = lv_display_get_horizontal_resolution(display);
        int32_t ver_res = lv_display_get_vertical_resolution(display);
        int is_portrait = is_portrait_layout(detect_layout_type(hor_res, ver_res)) ? 1 : 0;

        if (!subjects().is_portrait_ready) {
            lv_subject_init_int(&subjects().is_portrait, is_portrait);
            subjects().is_portrait_ready = true;
        } else {
            lv_subject_set_int(&subjects().is_portrait, is_portrait);
        }
        ObserverGuard::mark_subject_teardown_exempt(&subjects().is_portrait);
        lv_xml_register_subject(nullptr, "ui_is_portrait", &subjects().is_portrait);
        spdlog::debug("[Theme] Registered ui_is_portrait subject: {} ({}x{})", is_portrait, hor_res,
                      ver_res);
    }

    // Validate critical color pairs were registered (fail-fast if missing)
    static const char* required_colors[] = {"screen_bg", "text", "text_muted", nullptr};
    for (const char** name = required_colors; *name != nullptr; ++name) {
        if (!lv_xml_get_const(nullptr, *name)) {
            spdlog::critical(
                "[Theme] FATAL: Missing required color pair {}_light/{}_dark in globals.xml", *name,
                *name);
            std::exit(EXIT_FAILURE);
        }
    }

    spdlog::trace("[Theme] Runtime constants set for {} mode", runtime().dark ? "dark" : "light");

    // Read responsive font based on current breakpoint
    // NOTE: We read the variant directly because base constants are removed to enable
    // responsive overrides (LVGL ignores lv_xml_register_const for existing constants)
    int32_t resp_res = responsive_dimension(display);
    const char* size_suffix = theme_manager_get_breakpoint_suffix(resp_res);

    char font_variant_name[64];
    snprintf(font_variant_name, sizeof(font_variant_name), "font_body%s", size_suffix);
    const char* font_body_name = lv_xml_get_const(nullptr, font_variant_name);

    // Fallback chain (matches responsive spacing/font token resolution):
    //   _xxlarge → _xlarge → _large
    //   _xlarge  → _large
    //   _micro   → _tiny  → _small
    //   _tiny    → _small
    if (!font_body_name && strcmp(size_suffix, "_xxlarge") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_xlarge");
        if (!font_body_name)
            font_body_name = lv_xml_get_const(nullptr, "font_body_large");
    } else if (!font_body_name && strcmp(size_suffix, "_xlarge") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_large");
    } else if (!font_body_name && strcmp(size_suffix, "_micro") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_tiny");
        if (!font_body_name)
            font_body_name = lv_xml_get_const(nullptr, "font_body_small");
    } else if (!font_body_name && strcmp(size_suffix, "_tiny") == 0) {
        font_body_name = lv_xml_get_const(nullptr, "font_body_small");
    }

    // The reads above resolve the suffixed variant const (e.g. font_body_medium),
    // which is supplied by globals.xml's component consts. On the firmware
    // release path that nullptr-scope read can miss it, but the responsive font
    // registrar (theme_manager_register_responsive_fonts, run just above) always
    // registers the breakpoint-selected BASE name "font_body" into this same
    // scope — so fall back to it. Desktop resolves the variant directly and
    // never reaches this line.
    if (!font_body_name) {
        font_body_name = lv_xml_get_const(nullptr, "font_body");
    }

    const lv_font_t* base_font =
        font_body_name ? lv_xml_get_font(nullptr, font_body_name) : nullptr;
    if (!base_font) {
        // Resolve the fallback through the font REGISTRY (returns NULL if the
        // face isn't linked/registered — e.g. a pruned firmware tier has no
        // "noto_sans_16"), NOT via the &noto_sans_16 symbol: on such builds that
        // symbol can resolve to a NULL/weak address and theme_init_lvgl would
        // dereference it (Guru Meditation, PC=0). If even that misses, use
        // LVGL's built-in default, which is always valid. NEVER hand a NULL font
        // to LVGL.
        const lv_font_t* fallback = lv_xml_get_font(nullptr, "noto_sans_16");
        if (fallback) {
            spdlog::warn("[Theme] Failed to get font '{}', using noto_sans_16", font_variant_name);
            base_font = fallback;
        } else {
            spdlog::error("[Theme] Failed to get font '{}' and fallback noto_sans_16 is not "
                          "registered; using lv_font_get_default()",
                          font_variant_name);
            base_font = lv_font_get_default();
        }
    }

    // Build palette from current mode
    const helix::ModePalette& mode_palette = get_current_mode_palette();
    theme_palette_t palette = build_palette_from_mode(mode_palette);

    // Initialize custom HelixScreen theme (wraps LVGL default theme)
    runtime().current_theme = theme_init_lvgl(display, &palette, runtime().dark, base_font);

    if (runtime().current_theme) {
        lv_display_set_theme(display, runtime().current_theme);
        spdlog::debug("[Theme] Initialized HelixScreen theme: {} mode",
                      runtime().dark ? "dark" : "light");
        spdlog::trace("[Theme] Colors: primary={}, screen={}, card={}", mode_palette.primary,
                      mode_palette.screen_bg, mode_palette.card_bg);
    } else {
        spdlog::error("[Theme] Failed to initialize HelixScreen theme");
    }

#ifdef __ANDROID__
    // Set Android window background to match theme so transparent system bars
    // don't reveal a white window background on startup
    android_set_window_bg_color(theme_manager_parse_hex_color(mode_palette.screen_bg.c_str()));
#endif

    spdlog::debug("[Theme] theme_manager_init took {} ms (token table {})",
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - tm_init_start)
                      .count(),
                  helix::theme_tokens::enabled() ? "on" : "off");
    runtime().fully_initialized = true;
}

// NAMESPACE_OK: joins the global theme_manager_init/deinit family
int theme_manager_full_init_count() {
    return runtime().full_init_count;
}

void theme_manager_deinit() {
    // Deinitialize subjects BEFORE lv_deinit() to prevent crash in lv_observer_remove().
    // lv_subject_deinit() removes all observers from each subject AND removes the
    // unsubscribe_on_delete_cb from widgets. Without this, lv_deinit() -> obj_delete_core()
    // fires stale callbacks that try to remove from corrupted observer linked lists.
    subjects().deinit();
    spdlog::trace("[Theme] Deinitialized theme subjects");
}

/**
 * Walk widget tree and force style refresh on each widget
 *
 * This is needed for widgets that have local/inline styles from XML.
 * Theme styles are automatically refreshed by lv_obj_report_style_change(),
 * but local styles need explicit refresh.
 */
static lv_obj_tree_walk_res_t refresh_style_cb(lv_obj_t* obj, void* user_data) {
    (void)user_data;
    // Force LVGL to recalculate all style properties for this widget
    lv_obj_refresh_style(obj, LV_PART_ANY, LV_STYLE_PROP_ANY);
    return LV_OBJ_TREE_WALK_NEXT;
}

void theme_manager_refresh_widget_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Walk entire tree and refresh each widget's styles
    lv_obj_tree_walk(root, refresh_style_cb, nullptr);
}

void theme_manager_apply_theme(const helix::ThemeData& theme, bool dark_mode) {
    if (!runtime().display) {
        spdlog::error("[Theme] Cannot apply theme: theme not initialized");
        return;
    }

    // Respect theme mode support constraints
    bool effective_dark = dark_mode;
    auto mode_support = theme.get_mode_support();
    if (mode_support == helix::ThemeModeSupport::DARK_ONLY) {
        effective_dark = true;
    } else if (mode_support == helix::ThemeModeSupport::LIGHT_ONLY) {
        effective_dark = false;
    }

    // Capture old palette colors before overwriting, for swap map (copy, not ref!)
    const helix::ModePalette old_mp =
        runtime().dark ? runtime().active_theme.dark : runtime().active_theme.light;
    bool have_old = !old_mp.screen_bg.empty();

    runtime().active_theme = theme;
    runtime().dark = effective_dark;

    // The repeat guard keys on the display, its resolution and the mode, none of
    // which name the theme. Replacing active_theme here would otherwise leave a
    // later theme_manager_init() free to skip its registration pass and keep
    // serving whatever was applied, instead of reloading the configured theme.
    runtime().fully_initialized = false;

    spdlog::info("[Theme] Applying theme '{}' in {} mode", theme.name,
                 effective_dark ? "dark" : "light");

    // Log palette for debugging
    const helix::ModePalette& mode_palette = get_current_mode_palette();
    spdlog::debug("[Theme] Colors: screen={}, card={}, text={}", mode_palette.screen_bg,
                  mode_palette.card_bg, mode_palette.text);

    // Build color swap map (old baked values → new values), deduplicating collisions
    bg_swap_map.clear();
    border_swap_map.clear();
    if (have_old) {
        auto p = theme_manager_parse_hex_color;
        const helix::ModePalette& new_mp = mode_palette;
        swap_map_add(bg_swap_map, p(old_mp.screen_bg.c_str()), p(new_mp.screen_bg.c_str()),
                     "screen_bg");
        swap_map_add(bg_swap_map, p(old_mp.card_bg.c_str()), p(new_mp.card_bg.c_str()), "card_bg");
        swap_map_add(bg_swap_map, p(old_mp.elevated_bg.c_str()), p(new_mp.elevated_bg.c_str()),
                     "elevated_bg");
        swap_map_add(bg_swap_map, p(old_mp.overlay_bg.c_str()), p(new_mp.overlay_bg.c_str()),
                     "overlay_bg");
        swap_map_add(bg_swap_map, p(old_mp.border.c_str()), p(new_mp.border.c_str()), "border");
        swap_map_add(border_swap_map, p(old_mp.border.c_str()), p(new_mp.border.c_str()), "border");
    }

    // Update ThemeManager stored palettes and apply current mode
    theme_update_colors(effective_dark);

    // Re-register XML constants: semantic colors, theme properties, and color pairs
    theme_manager_register_semantic_colors(nullptr, runtime().active_theme, effective_dark);
    theme_manager_register_theme_properties(nullptr, runtime().active_theme, effective_dark);

    // Update border_radius constant for live preview (register_const is first-wins,
    // so we need update_const for subsequent changes)
    {
        const char* bp_suffix =
            theme_manager_get_breakpoint_suffix(responsive_dimension(runtime().display));
        int radius_px = helix::BorderRadiusSizes::pixels(
            runtime().active_theme.properties.border_radius_size, bp_suffix);
        char radius_buf[16];
        snprintf(radius_buf, sizeof(radius_buf), "%d", radius_px);
        lv_xml_update_const(nullptr, "border_radius", radius_buf);
    }

    // Same first-wins caveat as border_radius above: the shadow opacity differs
    // between light and dark, so a live mode flip has to update it. #1178
    lv_xml_update_const(nullptr, "overlay_shadow_opa", effective_dark ? "200" : "100");

    theme_manager_register_color_pairs(nullptr, effective_dark);

    // Update screen background directly (XML inline styles are baked at parse time)
    lv_color_t screen_bg = theme_manager_parse_hex_color(mode_palette.screen_bg.c_str());
    lv_obj_set_style_bg_color(lv_screen_active(), screen_bg, LV_PART_MAIN);

#ifdef __ANDROID__
    // Sync Android window background so area behind transparent system bars matches
    android_set_window_bg_color(screen_bg);
#endif

    // Refresh widget tree: shared styles + local/inline styles + palette-styled widgets
    theme_manager_refresh_widget_tree(lv_screen_active());
    theme_apply_current_palette_to_tree(lv_screen_active());

    // Refresh ui_gradient_canvas widgets for the new dark/light mode
    ui_gradient_canvas_theme_update(lv_screen_active());

    // Invalidate and notify
    lv_obj_invalidate(lv_screen_active());
    theme_manager_notify_change();

    spdlog::info("[Theme] Theme apply complete (generation={})", subjects().generation);
}

void theme_manager_toggle_dark_mode() {
    theme_manager_apply_theme(runtime().active_theme, !runtime().dark);
}

bool theme_manager_is_dark_mode() {
    return runtime().dark;
}

const helix::ThemeData& theme_manager_get_active_theme() {
    return runtime().active_theme;
}

helix::ThemeModeSupport theme_manager_get_mode_support() {
    return runtime().active_theme.get_mode_support();
}

bool theme_manager_supports_dark_mode() {
    return runtime().active_theme.supports_dark();
}

bool theme_manager_supports_light_mode() {
    return runtime().active_theme.supports_light();
}

lv_subject_t* theme_manager_get_changed_subject() {
    return &subjects().changed;
}

lv_subject_t* theme_manager_get_breakpoint_subject() {
    return &subjects().breakpoint;
}

void theme_manager_notify_change() {
    if (!subjects().changed_ready)
        return;
    subjects().generation++;
    lv_subject_set_int(&subjects().changed, subjects().generation);
    spdlog::debug("[Theme] Notified theme change (generation={})", subjects().generation);
}

void theme_manager_preview(const helix::ThemeData& theme) {
    theme_manager_apply_theme(theme, runtime().dark);
}

void theme_manager_preview(const helix::ThemeData& theme, bool is_dark) {
    theme_manager_apply_theme(theme, is_dark);
}

// theme_manager_refresh_preview_elements() removed — was ~450 lines of
// widget-by-name updates. Replaced by theme_manager_apply_theme() which
// uses theme_apply_current_palette_to_tree() for generic palette application.

// ============================================================================
// Palette Application Functions (for DRY preview styling)
// ============================================================================

/**
 * Check if a font is one of the MDI icon fonts (forward declaration)
 */
static bool is_muted_text_font(const lv_font_t* font);

/**
 * Helper to update button label text with contrast-aware color
 *
 * A filled button is an accent surface: its text starts from the palette text
 * colour and shifts toward its own pole just enough to stay readable
 * (theme_manager_get_contrast_adjusted_text()).
 */
static void apply_button_text_contrast(lv_obj_t* btn) {
    if (!btn)
        return;

    // Get button's background color and pick a readable foreground for it
    lv_color_t bg_color = lv_obj_get_style_bg_color(btn, LV_PART_MAIN);
    lv_color_t current_text = theme_manager_get_color("text");
    lv_color_t text_color = theme_manager_get_contrast_adjusted_text(current_text, bg_color);

    // Check for disabled state - use muted color
    bool btn_disabled = lv_obj_has_state(btn, LV_STATE_DISABLED);
    if (btn_disabled) {
        // Blend toward gray for disabled state
        text_color = lv_color_mix(text_color, lv_color_hex(0x888888), 128);
    }

    // Get current muted color to detect text/muted-variant icons
    lv_color_t current_muted = theme_manager_get_color("text_muted");

    // Also check contrast text from both palettes for icon detection
    auto& tm = ThemeManager::instance();
    lv_color_t dark_text = tm.dark_palette().text;
    lv_color_t light_text = tm.light_palette().text;

    // Helper lambda to check if icon color is a "text-like" color that should get contrast.
    // Pure black and white count: they are what a previous pass of this helper
    // wrote, and a re-preview against a different fill must be free to flip them.
    auto is_text_variant_color = [&](lv_color_t c) {
        return lv_color_eq(c, current_text) || lv_color_eq(c, current_muted) ||
               lv_color_eq(c, dark_text) || lv_color_eq(c, light_text) ||
               lv_color_eq(c, lv_color_black()) || lv_color_eq(c, lv_color_white());
    };

    // Update all label children in the button
    // For icons: only apply contrast if they're using text/muted variant
    // Skip icons with semantic colors (primary, warning, etc.)
    uint32_t count = lv_obj_get_child_count(btn);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(btn, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
            if (helix::ui::is_icon_font(font)) {
                // Icon: only apply contrast if it's using text/muted variant
                lv_color_t icon_color = lv_obj_get_style_text_color(child, LV_PART_MAIN);
                if (is_text_variant_color(icon_color)) {
                    lv_obj_set_style_text_color(child, text_color, LV_PART_MAIN);
                }
            } else {
                // Regular label: use muted color for muted-style fonts, contrast for others
                const lv_font_t* font = lv_obj_get_style_text_font(child, LV_PART_MAIN);
                lv_obj_set_style_text_color(
                    child, is_muted_text_font(font) ? current_muted : text_color, LV_PART_MAIN);
            }
        }
        // Also check nested containers (some buttons have container > label structure)
        uint32_t nested_count = lv_obj_get_child_count(child);
        for (uint32_t j = 0; j < nested_count; j++) {
            lv_obj_t* nested = lv_obj_get_child(child, j);
            if (lv_obj_check_type(nested, &lv_label_class)) {
                const lv_font_t* nested_font = lv_obj_get_style_text_font(nested, LV_PART_MAIN);
                if (helix::ui::is_icon_font(nested_font)) {
                    lv_color_t icon_color = lv_obj_get_style_text_color(nested, LV_PART_MAIN);
                    if (is_text_variant_color(icon_color)) {
                        lv_obj_set_style_text_color(nested, text_color, LV_PART_MAIN);
                    }
                } else {
                    lv_obj_set_style_text_color(
                        nested, is_muted_text_font(nested_font) ? current_muted : text_color,
                        LV_PART_MAIN);
                }
            }
        }
    }
}

/**
 * Check if a font is a "small" semantic font (text_small, text_xs, text_heading use muted color)
 * Returns true for fonts that should use text_muted color
 */
static bool is_muted_text_font(const lv_font_t* font) {
    if (!font)
        return false;

    // Get semantic font pointers for comparison
    static const lv_font_t* font_small = nullptr;
    static const lv_font_t* font_xs = nullptr;
    static const lv_font_t* font_heading = nullptr;
    static bool fonts_initialized = false;

    if (!fonts_initialized) {
        const char* small_name = lv_xml_get_const(nullptr, "font_small");
        const char* xs_name = lv_xml_get_const(nullptr, "font_xs");
        const char* heading_name = lv_xml_get_const(nullptr, "font_heading");
        if (small_name)
            font_small = lv_xml_get_font(nullptr, small_name);
        if (xs_name)
            font_xs = lv_xml_get_font(nullptr, xs_name);
        if (heading_name)
            font_heading = lv_xml_get_font(nullptr, heading_name);
        fonts_initialized = true;
    }

    // text_small, text_xs, and text_heading all use muted color (per ui_text.cpp)
    return font == font_small || font == font_xs || font == font_heading;
}

/**
 * @brief Check if an object is on an elevated background surface
 *
 * Detects two cases where inputs need overlay_bg for contrast:
 * 1. Inside a dialog (marked with LV_OBJ_FLAG_USER_1 in ui_dialog_xml_create())
 * 2. Inside any container whose opaque background matches elevated_bg
 *
 * This allows text_input, dropdowns, etc. to auto-contrast on raised cards
 * without manual style_bg_color overrides in XML.
 */
static bool is_on_elevated_surface(lv_obj_t* obj) {
    auto& tm = ThemeManager::instance();
    lv_color_t elevated = tm.current_palette().elevated_bg;
    lv_obj_t* parent = lv_obj_get_parent(obj);
    while (parent) {
        if (lv_obj_has_flag(parent, LV_OBJ_FLAG_USER_1))
            return true;
        lv_opa_t opa = lv_obj_get_style_bg_opa(parent, LV_PART_MAIN);
        if (opa > LV_OPA_50) {
            lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
            if (color_eq(bg, elevated))
                return true;
        }
        parent = lv_obj_get_parent(parent);
    }
    return false;
}

void theme_apply_palette_to_widget(lv_obj_t* obj, const helix::ModePalette& palette) {
    if (!obj)
        return;

    // Parse palette colors
    lv_color_t screen_bg = theme_manager_parse_hex_color(palette.screen_bg.c_str());
    lv_color_t overlay_bg = theme_manager_parse_hex_color(palette.overlay_bg.c_str());
    lv_color_t elevated_bg = theme_manager_parse_hex_color(palette.elevated_bg.c_str());
    lv_color_t border = theme_manager_parse_hex_color(palette.border.c_str());
    lv_color_t text_primary = theme_manager_parse_hex_color(palette.text.c_str());
    lv_color_t text_muted = theme_manager_parse_hex_color(palette.text_muted.c_str());
    lv_color_t primary = theme_manager_parse_hex_color(palette.primary.c_str());
    lv_color_t secondary = theme_manager_parse_hex_color(palette.secondary.c_str());
    lv_color_t tertiary = theme_manager_parse_hex_color(palette.tertiary.c_str());

    // Compute knob color: brighter of primary vs tertiary
    lv_color_t knob_color = theme_compute_more_saturated(primary, tertiary);

    // ==========================================================================
    // LABELS - Use font-based detection instead of name matching
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);

        // Skip icons (MDI font) - they use the icon variant system with shared
        // ThemeManager styles that auto-update on theme change. Setting inline
        // colors here would override variant styles (muted, secondary, etc.)
        // and HeatingIconAnimator's temperature-based colors.
        if (helix::ui::is_icon_font(font)) {
            return;
        }

        // Labels inside buttons get auto-contrast based on button background
        lv_obj_t* parent = lv_obj_get_parent(obj);
        if (parent && lv_obj_check_type(parent, &lv_button_class)) {
            // Button text - contrast is handled by apply_button_text_contrast on parent
            // Just skip, the button handler will update child labels
            return;
        }

        // Labels inside dark overlays (e.g., metadata on thumbnails) need light text
        // regardless of theme mode. Walk ancestors to find nearest opaque container.
        for (lv_obj_t* anc = parent; anc != nullptr; anc = lv_obj_get_parent(anc)) {
            lv_opa_t anc_opa = lv_obj_get_style_bg_opa(anc, LV_PART_MAIN);
            if (anc_opa >= LV_OPA_50) {
                lv_color_t anc_bg = lv_obj_get_style_bg_color(anc, LV_PART_MAIN);
                if (theme_compute_brightness(anc_bg) < 80) {
                    lv_obj_set_style_text_color(obj, lv_color_white(), LV_PART_MAIN);
                    return;
                }
                break; // found opaque ancestor, not dark — fall through to normal
            }
        }

        // Small/heading fonts get muted color, body fonts get primary
        if (is_muted_text_font(font)) {
            lv_obj_set_style_text_color(obj, text_muted, LV_PART_MAIN);
        } else {
            lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        }
        return;
    }

    // ==========================================================================
    // BUTTONS - background, border, and text contrast
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_button_class)) {
        // Get current button background to check if it's a "neutral" button
        lv_color_t current_bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
        uint8_t r = current_bg.red;
        uint8_t g = current_bg.green;
        uint8_t b = current_bg.blue;

        // Check if button is "neutral" (grayscale or very desaturated)
        // Accent buttons (primary, secondary, etc.) have colorful backgrounds
        int max_rgb = std::max({(int)r, (int)g, (int)b});
        int min_rgb = std::min({(int)r, (int)g, (int)b});
        int saturation = (max_rgb > 0) ? ((max_rgb - min_rgb) * 255 / max_rgb) : 0;

        // If saturation is low (<30), this is a neutral/gray button - apply elevated_bg
        if (saturation < 30) {
            lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_MAIN);
        }

        lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
        apply_button_text_contrast(obj);
        return;
    }

    // ==========================================================================
    // INTERACTIVE WIDGETS - specific styling per widget type
    // ==========================================================================

    // Checkboxes - box border, primary bg when checked, contrast checkmark
    if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        lv_obj_set_style_border_color(obj, border, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_INDICATOR);
        // Checked state: primary background with contrasting checkmark
        lv_obj_set_style_bg_color(obj, primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(obj, primary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        uint8_t lum = lv_color_luminance(primary);
        lv_color_t check_color = (lum > 140) ? lv_color_black() : lv_color_white();
        lv_obj_set_style_text_color(obj, check_color, LV_PART_INDICATOR | LV_STATE_CHECKED);
        return;
    }

    // Switches - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, secondary, LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB | LV_STATE_CHECKED);
        return;
    }

    // Sliders - track, indicator, knob
    if (lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, secondary, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(obj, knob_color, LV_PART_KNOB);
        lv_obj_set_style_shadow_color(obj, screen_bg, LV_PART_KNOB);
        return;
    }

    // Dropdowns - background, border, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_border_color(obj, border, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Textareas - background, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Spinboxes - background, text
    // On elevated surfaces (dialogs, raised cards), use overlay_bg for contrast
    if (lv_obj_check_type(obj, &lv_spinbox_class)) {
        lv_color_t bg = is_on_elevated_surface(obj) ? overlay_bg : elevated_bg;
        lv_obj_set_style_bg_color(obj, bg, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        return;
    }

    // Dropdown lists (popup menus)
    if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_color_t dropdown_accent = theme_compute_more_saturated(primary, secondary);
        lv_obj_set_style_bg_color(obj, elevated_bg, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(obj, text_primary, LV_PART_MAIN);
        lv_obj_set_style_bg_color(obj, dropdown_accent, LV_PART_SELECTED);
        return;
    }

    // ==========================================================================
    // DIVIDERS - detect by structure: thin lv_obj (1-2px) with visible bg, no children
    // ==========================================================================
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        int32_t w = lv_obj_get_width(obj);
        int32_t h = lv_obj_get_height(obj);
        lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
        uint32_t child_count = lv_obj_get_child_count(obj);

        // Divider: thin (<=2px in one dimension), visible bg, no children
        bool is_thin_horizontal = (h <= 2 && w > h * 10);
        bool is_thin_vertical = (w <= 2 && h > w * 10);
        bool is_divider =
            (is_thin_horizontal || is_thin_vertical) && bg_opa > 0 && child_count == 0;

        if (is_divider) {
            lv_obj_set_style_bg_color(obj, border, LV_PART_MAIN);
            return;
        }
    }

    // ==========================================================================
    // CONTAINERS - color-swap map (replaces name-based heuristics)
    // ==========================================================================
    // Swap bg_color if it matches any old semantic color
    lv_opa_t bg_opa_check = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
    if (bg_opa_check > 0 && !bg_swap_map.empty()) {
        lv_color_t current_bg = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);
        for (const auto& entry : bg_swap_map) {
            if (color_eq(current_bg, entry.from)) {
                lv_obj_set_style_bg_color(obj, entry.to, LV_PART_MAIN);
                break;
            }
        }
    }

    // Swap border_color if it matches any old semantic color
    int32_t bw = lv_obj_get_style_border_width(obj, LV_PART_MAIN);
    if (bw > 0 && !border_swap_map.empty()) {
        lv_color_t current_border = lv_obj_get_style_border_color(obj, LV_PART_MAIN);
        for (const auto& entry : border_swap_map) {
            if (color_eq(current_border, entry.from)) {
                lv_obj_set_style_border_color(obj, entry.to, LV_PART_MAIN);
                break;
            }
        }
    }
}

void theme_apply_palette_to_tree(lv_obj_t* root, const helix::ModePalette& palette) {
    if (!root)
        return;

    // Apply to this widget
    theme_apply_palette_to_widget(root, palette);

    // Recurse into children
    uint32_t child_count = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(root, i);
        theme_apply_palette_to_tree(child, palette);
    }
}

void theme_apply_current_palette_to_tree(lv_obj_t* root) {
    if (!root)
        return;

    // Get the active palette based on current mode
    const helix::ModePalette& palette =
        runtime().dark ? runtime().active_theme.dark : runtime().active_theme.light;

    const char* root_name = lv_obj_get_name(root);
    spdlog::debug("[Theme] Applying current palette to tree root={}",
                  root_name ? root_name : "(screen)");
    theme_apply_palette_to_tree(root, palette);
}

void theme_apply_palette_to_screen_dropdowns(const helix::ModePalette& palette) {
    // Style any screen-level popups (dropdown lists, modals, etc.)
    // These are direct children of the screen, not part of the overlay tree
    lv_color_t elevated_bg = theme_manager_parse_hex_color(palette.elevated_bg.c_str());
    lv_color_t text_color = theme_manager_parse_hex_color(palette.text.c_str());
    lv_color_t border = theme_manager_parse_hex_color(palette.border.c_str());
    lv_color_t primary = theme_manager_parse_hex_color(palette.primary.c_str());
    lv_color_t secondary = theme_manager_parse_hex_color(palette.secondary.c_str());

    // Use more saturated of primary/secondary for highlight (avoids white/gray primaries)
    lv_color_t dropdown_accent = theme_compute_more_saturated(primary, secondary);

    // Text color for selected based on accent luminance
    uint8_t lum = lv_color_luminance(dropdown_accent);
    lv_color_t selected_text = (lum > 140) ? lv_color_black() : lv_color_white();

    lv_obj_t* screen = lv_screen_active();
    uint32_t child_count = lv_obj_get_child_count(screen);
    spdlog::debug("[Theme] Screen has {} children", child_count);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* child = lv_obj_get_child(screen, i);

        // Dropdown lists get special treatment for selection highlighting
        if (lv_obj_check_type(child, &lv_dropdownlist_class)) {
            lv_obj_set_style_bg_color(child, elevated_bg, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(child, text_color, LV_PART_MAIN);
            lv_obj_set_style_border_color(child, border, LV_PART_MAIN);
            lv_obj_set_style_bg_color(child, dropdown_accent, LV_PART_SELECTED);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_SELECTED);
            lv_obj_set_style_text_color(child, selected_text, LV_PART_SELECTED);
            continue;
        }

        // Other screen-level children (modals, etc.) - apply palette to entire tree
        // Skip the main app layout (it's handled separately by the overlay system)
        const char* name = lv_obj_get_name(child);
        if (name && strcmp(name, "app_layout") == 0) {
            continue;
        }

        // Apply palette to this popup and all its children
        spdlog::debug("[Theme] Applying palette to screen popup: {}", name ? name : "(unnamed)");
        theme_apply_palette_to_tree(child, palette);
    }
}

/**
 * Get theme-appropriate color variant with fallback for static colors
 *
 * First attempts to look up {base_name}_light and {base_name}_dark from globals.xml,
 * selecting the appropriate one based on current theme mode. If the theme variants
 * don't exist, falls back to {base_name} directly (for static colors like
 * warning, danger that are the same in both themes).
 *
 * @param base_name Color constant base name (e.g., "screen_bg", "warning")
 * @return Parsed color, or black (0x000000) if not found
 *
 * Example:
 *   lv_color_t bg = theme_manager_get_color("screen_bg");
 *   // Returns screen_bg_light in light mode, screen_bg_dark in dark mode
 *
 *   lv_color_t warn = theme_manager_get_color("warning");
 *   // Returns warning directly (static, no theme variants)
 */
lv_color_t theme_manager_get_color(const char* base_name) {
    if (!base_name) {
        spdlog::error("[Theme] theme_manager_get_color: NULL base_name");
        return lv_color_hex(0x000000);
    }

    // Construct variant names: {base_name}_light and {base_name}_dark
    char light_name[128];
    char dark_name[128];
    snprintf(light_name, sizeof(light_name), "%s_light", base_name);
    snprintf(dark_name, sizeof(dark_name), "%s_dark", base_name);

    // Use silent lookups to avoid LVGL warnings when probing for variants
    // Pattern 1: Theme-aware color with _light/_dark variants
    const char* light_str = lv_xml_get_const_silent(nullptr, light_name);
    const char* dark_str = lv_xml_get_const_silent(nullptr, dark_name);

    if (light_str && dark_str) {
        // Both variants exist - use theme-appropriate one
        return theme_manager_parse_hex_color(runtime().dark ? dark_str : light_str);
    }

    // Pattern 2: Static color with just base name (no variants)
    const char* base_str = lv_xml_get_const_silent(nullptr, base_name);
    if (base_str) {
        return theme_manager_parse_hex_color(base_str);
    }

    // Pattern 3: Partial variants (error case)
    if (light_str || dark_str) {
        spdlog::error("[Theme] Color {} has only one variant (_light or _dark), need both",
                      base_name);
        return lv_color_hex(0x000000);
    }

    // Nothing found — only log error if theme is initialized (otherwise this is
    // benign, e.g. tests or early init before theme_manager_init() is called)
    if (runtime().current_theme) {
        spdlog::error("[Theme] Color not found: {} (no base, no _light/_dark variants)", base_name);
    } else {
        spdlog::trace("[Theme] Color not found (theme not initialized): {}", base_name);
    }
    return lv_color_hex(0x000000);
}

lv_color_t theme_manager_get_object_palette_color(int index) {
    constexpr int OBJECT_PALETTE_SIZE = 8;
    char token[32];
    snprintf(token, sizeof(token), "object_color_%d", (index % OBJECT_PALETTE_SIZE) + 1);
    return theme_manager_get_color(token);
}

/**
 * Apply theme-appropriate background color to object
 *
 * Convenience wrapper that gets the color variant and applies it to the object.
 *
 * @param obj LVGL object to apply color to
 * @param base_name Color constant base name (e.g., "screen_bg", "card_bg")
 * @param part Style part to apply to (default: LV_PART_MAIN)
 *
 * Example:
 *   theme_manager_apply_bg_color(screen, "screen_bg", LV_PART_MAIN);
 *   // Applies screen_bg_light/dark depending on theme mode
 */
void theme_manager_apply_bg_color(lv_obj_t* obj, const char* base_name, lv_part_t part) {
    if (!obj) {
        spdlog::error("[Theme] theme_manager_apply_bg_color: NULL object");
        return;
    }

    lv_color_t color = theme_manager_get_color(base_name);
    lv_obj_set_style_bg_color(obj, color, part);
}
