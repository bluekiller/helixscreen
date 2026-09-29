// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_appearance.cpp
 * @brief Implementation of AppearanceSettingsOverlay
 */

#include "ui_settings_appearance.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_modal.h"
#include "ui_nav_manager.h"
#include "ui_theme_editor_overlay.h"
#include "ui_toast_manager.h"
#include "ui_utils.h"

#include "border_radius_sizes.h"
#include "display_settings_manager.h"
#include "lvgl/src/others/translation/lv_translation.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "theme_manager.h"

#include <spdlog/spdlog.h>

#include <memory>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<AppearanceSettingsOverlay> g_appearance_settings_overlay;

AppearanceSettingsOverlay& get_appearance_settings_overlay() {
    if (!g_appearance_settings_overlay) {
        g_appearance_settings_overlay = std::make_unique<AppearanceSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "AppearanceSettingsOverlay", []() { g_appearance_settings_overlay.reset(); });
    }
    return *g_appearance_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

AppearanceSettingsOverlay::AppearanceSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

AppearanceSettingsOverlay::~AppearanceSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void AppearanceSettingsOverlay::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    // Theme Apply button disabled subject (1=disabled initially)
    UI_MANAGED_SUBJECT_INT(theme_apply_disabled_subject_, 1, "theme_apply_disabled", subjects_);

    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void AppearanceSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_animations_changed", on_animations_changed},
        {"on_dark_mode_changed", on_dark_mode_changed},
        {"on_widget_labels_changed", on_widget_labels_changed},
        {"on_bed_mesh_mode_changed", on_bed_mesh_mode_changed},
        {"on_toolhead_style_changed", on_toolhead_style_changed},
        {"on_gcode_mode_changed", on_gcode_mode_changed},
        {"on_z_movement_style_changed", on_z_movement_style_changed},

        // Theme explorer
        {"on_theme_preset_changed", on_theme_preset_changed},
        {"on_theme_settings_clicked", on_theme_settings_clicked},
        {"on_preview_dark_mode_toggled", on_preview_dark_mode_toggled},
        {"on_edit_colors_clicked", on_edit_colors_clicked},
        {"on_preview_open_modal", on_preview_open_modal},
        {"on_apply_theme_clicked", on_apply_theme_clicked},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* AppearanceSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_appearance_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void AppearanceSettingsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    parent_screen_ = parent_screen;

    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    if (!overlay_root_ && parent_screen_) {
        create(parent_screen_);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Cannot show - overlay not created", get_name());
        return;
    }

    NavigationManager::instance().register_overlay_instance(overlay_root_, this);
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void AppearanceSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_animations_toggle();
    init_bed_mesh_dropdown();
    init_toolhead_style_dropdown();
    init_gcode_mode_dropdown();
    init_z_movement_dropdown();
}

// ============================================================================
// INIT METHODS
// ============================================================================

void AppearanceSettingsOverlay::init_animations_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_animations");
    if (!row)
        return;

    lv_obj_t* toggle = lv_obj_find_by_name(row, "toggle");
    if (toggle) {
        if (DisplaySettingsManager::instance().get_animations_enabled()) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        }
        spdlog::trace("[{}] Animations toggle initialized", get_name());
    }
}

void AppearanceSettingsOverlay::init_bed_mesh_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* bed_mesh_row = lv_obj_find_by_name(overlay_root_, "row_bed_mesh_mode");
    lv_obj_t* bed_mesh_dropdown =
        bed_mesh_row ? lv_obj_find_by_name(bed_mesh_row, "dropdown") : nullptr;
    if (bed_mesh_dropdown) {
        int current_mode = DisplaySettingsManager::instance().get_bed_mesh_render_mode();
        lv_dropdown_set_selected(bed_mesh_dropdown, current_mode);

        spdlog::debug("[{}] Bed mesh mode dropdown initialized to {} ({})", get_name(),
                      current_mode, current_mode == 0 ? "Auto" : (current_mode == 1 ? "3D" : "2D"));
    }
}

void AppearanceSettingsOverlay::init_toolhead_style_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_toolhead_style");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        lv_dropdown_set_options(dropdown, SettingsManager::get_toolhead_style_options().c_str());
        auto style = SettingsManager::instance().get_toolhead_style();
        lv_dropdown_set_selected(
            dropdown,
            static_cast<uint32_t>(SettingsManager::toolhead_style_to_dropdown_index(style)));
        spdlog::trace("[{}] Toolhead style dropdown initialized (style={}, dropdown_index={})",
                      get_name(), static_cast<int>(style),
                      SettingsManager::toolhead_style_to_dropdown_index(style));
    }
}

void AppearanceSettingsOverlay::init_gcode_mode_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_gcode_mode");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto& display_settings = DisplaySettingsManager::instance();
#ifndef ENABLE_GLES_3D
        // Without GLES, remove "3D View" option
        lv_dropdown_set_options(dropdown, (std::string(lv_tr("Auto")) + "\n" + lv_tr("2D Layers") +
                                           "\n" + lv_tr("Thumbnail Only"))
                                              .c_str());
        int mode = display_settings.get_gcode_render_mode();
        int index = 0; // Auto
        if (mode == 2)
            index = 1; // 2D Layers
        else if (mode == 3)
            index = 2; // Thumbnail Only
        lv_dropdown_set_selected(dropdown, index);
#else
        int mode = display_settings.get_gcode_render_mode();
        lv_dropdown_set_selected(dropdown, mode);
#endif
        spdlog::trace("[{}] G-code mode dropdown initialized", get_name());
    }
}

void AppearanceSettingsOverlay::init_z_movement_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* row = lv_obj_find_by_name(overlay_root_, "row_z_movement_style");
    if (!row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(row, "dropdown");
    if (dropdown) {
        auto style = SettingsManager::instance().get_z_movement_style();
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(style));
        spdlog::trace("[{}] Z movement style dropdown initialized (style={})", get_name(),
                      static_cast<int>(style));
    }
}

void AppearanceSettingsOverlay::init_theme_preset_dropdown(lv_obj_t* root) {
    if (!root)
        return;

    lv_obj_t* theme_preset_dropdown = lv_obj_find_by_name(root, "theme_preset_dropdown");
    if (theme_preset_dropdown) {
        std::string options = DisplaySettingsManager::instance().get_theme_options();
        lv_dropdown_set_options(theme_preset_dropdown, options.c_str());

        int current_index = DisplaySettingsManager::instance().get_theme_index();
        lv_dropdown_set_selected(theme_preset_dropdown, static_cast<uint32_t>(current_index));

        spdlog::debug("[{}] Theme dropdown initialized to index {} ({})", get_name(), current_index,
                      DisplaySettingsManager::instance().get_theme_name());
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void AppearanceSettingsOverlay::handle_animations_changed(bool enabled) {
    spdlog::info("[{}] Animations toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_animations_enabled(enabled);
}

void AppearanceSettingsOverlay::handle_dark_mode_changed(bool enabled) {
    spdlog::info("[{}] Dark mode toggled: {}", get_name(), enabled ? "ON" : "OFF");
    DisplaySettingsManager::instance().set_dark_mode(enabled);
    theme_manager_apply_theme(theme_manager_get_active_theme(), enabled);
}

void AppearanceSettingsOverlay::handle_widget_labels_changed(bool enabled) {
    spdlog::info("[{}] Widget labels toggled: {}", get_name(), enabled ? "ON" : "OFF");
    SettingsManager::instance().set_show_widget_labels(enabled);
}

void AppearanceSettingsOverlay::handle_bed_mesh_mode_changed(int mode) {
    spdlog::info("[{}] Bed mesh render mode changed: {} ({})", get_name(), mode,
                 mode == 0 ? "Auto" : (mode == 1 ? "3D" : "2D"));
    DisplaySettingsManager::instance().set_bed_mesh_render_mode(mode);
}

void AppearanceSettingsOverlay::handle_toolhead_style_changed(int index) {
    auto style = SettingsManager::dropdown_index_to_toolhead_style(index);
    spdlog::info("[{}] Toolhead style changed: {} (dropdown index {})", get_name(),
                 static_cast<int>(style), index);
    SettingsManager::instance().set_toolhead_style(style);
}

void AppearanceSettingsOverlay::handle_gcode_mode_changed(int index) {
#ifndef ENABLE_GLES_3D
    static const int INDEX_TO_MODE[] = {0, 2, 3}; // Auto, 2D Layers, Thumbnail Only
    int mode = (index >= 0 && index <= 2) ? INDEX_TO_MODE[index] : 0;
#else
    int mode = index;
#endif

    static const char* MODE_NAMES[] = {"Auto", "3D", "2D Layers", "Thumbnail Only"};
    spdlog::info("[{}] G-code render mode changed: {} ({})", get_name(), mode,
                 (mode >= 0 && mode <= 3) ? MODE_NAMES[mode] : "Unknown");
    DisplaySettingsManager::instance().set_gcode_render_mode(mode);
}

void AppearanceSettingsOverlay::handle_z_movement_style_changed(int index) {
    auto style = static_cast<ZMovementStyle>(index);
    spdlog::info("[{}] Z movement style changed: {} ({})", get_name(), index,
                 index == 0 ? "Auto" : (index == 1 ? "Bed Moves" : "Nozzle Moves"));
    SettingsManager::instance().set_z_movement_style(style);
}

// ============================================================================
// THEME EXPLORER
// ============================================================================

void AppearanceSettingsOverlay::handle_theme_preset_changed(int index) {
    if (theme_explorer_overlay_ && lv_obj_is_visible(theme_explorer_overlay_)) {
        handle_explorer_theme_changed(index);
        return;
    }

    DisplaySettingsManager::instance().set_theme_by_index(index);

    spdlog::info("[{}] Theme changed to index {} ({})", get_name(), index,
                 DisplaySettingsManager::instance().get_theme_name());
}

void AppearanceSettingsOverlay::handle_explorer_theme_changed(int index) {
    if (index < 0 || index >= static_cast<int>(cached_themes_.size())) {
        spdlog::error("[{}] Invalid theme index {}", get_name(), index);
        return;
    }

    std::string theme_name = cached_themes_[index].filename;
    helix::ThemeData theme = helix::load_theme_from_file(theme_name);

    if (!theme.is_valid()) {
        spdlog::error("[{}] Failed to load theme '{}' for preview", get_name(), theme_name);
        return;
    }

    preview_theme_name_ = theme_name;

    bool supports_dark = theme.supports_dark();
    bool supports_light = theme.supports_light();

    if (theme_explorer_overlay_) {
        lv_obj_t* dark_toggle =
            lv_obj_find_by_name(theme_explorer_overlay_, "preview_dark_mode_toggle");
        lv_obj_t* toggle_container =
            lv_obj_find_by_name(theme_explorer_overlay_, "dark_mode_toggle_container");

        if (dark_toggle) {
            if (supports_dark && supports_light) {
                lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' supports both modes, toggle enabled", get_name(),
                              theme_name);
            } else if (supports_dark) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = true;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is dark-only, forcing dark mode", get_name(),
                              theme_name);
            } else if (supports_light) {
                lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
                lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
                preview_is_dark_ = false;
                if (toggle_container) {
                    lv_obj_remove_flag(toggle_container, LV_OBJ_FLAG_HIDDEN);
                }
                spdlog::debug("[{}] Theme '{}' is light-only, forcing light mode", get_name(),
                              theme_name);
            }
        }
    }

    theme_manager_preview(theme);

    lv_subject_set_int(&theme_apply_disabled_subject_, index == original_theme_index_ ? 1 : 0);

    handle_preview_dark_mode_toggled(preview_is_dark_);

    spdlog::debug("[{}] Explorer preview: theme '{}' (index {})", get_name(), theme_name, index);
}

void AppearanceSettingsOverlay::handle_theme_settings_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    if (!theme_explorer_overlay_) {
        spdlog::debug("[{}] Creating theme explorer overlay...", get_name());
        theme_explorer_overlay_ =
            static_cast<lv_obj_t*>(lv_xml_create(parent_screen_, "theme_preview_overlay", nullptr));
        if (!theme_explorer_overlay_) {
            spdlog::error("[{}] Failed to create theme explorer overlay", get_name());
            return;
        }

        lv_obj_add_flag(theme_explorer_overlay_, LV_OBJ_FLAG_HIDDEN);

        NavigationManager::instance().register_overlay_instance(theme_explorer_overlay_, nullptr);
        NavigationManager::instance().register_overlay_close_callback(
            theme_explorer_overlay_, [this]() {
                theme_manager_apply_theme(original_theme_, theme_manager_is_dark_mode());
                if (theme_explorer_overlay_) {
                    helix::ui::defocus_tree(theme_explorer_overlay_);
                    lv_obj_add_flag(theme_explorer_overlay_, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_t* to_delete = theme_explorer_overlay_;
                    theme_explorer_overlay_ = nullptr;
                    lv_obj_delete_async(to_delete);
                }
                cached_themes_.clear();
            });
    }

    sync_explorer_to_active_theme();

    NavigationManager::instance().push_overlay(theme_explorer_overlay_);
}

void AppearanceSettingsOverlay::sync_explorer_to_active_theme() {
    if (!theme_explorer_overlay_)
        return;

    init_theme_preset_dropdown(theme_explorer_overlay_);

    cached_themes_ = helix::discover_themes(helix::get_themes_directory());

    original_theme_index_ = DisplaySettingsManager::instance().get_theme_index();
    preview_theme_name_ = DisplaySettingsManager::instance().get_theme_name();
    original_theme_ = theme_manager_get_active_theme();

    preview_is_dark_ = theme_manager_is_dark_mode();
    lv_obj_t* dark_toggle =
        lv_obj_find_by_name(theme_explorer_overlay_, "preview_dark_mode_toggle");
    if (dark_toggle) {
        if (preview_is_dark_) {
            lv_obj_add_state(dark_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(dark_toggle, LV_STATE_CHECKED);
        }

        bool supports_dark = theme_manager_supports_dark_mode();
        bool supports_light = theme_manager_supports_light_mode();
        if (supports_dark && supports_light) {
            lv_obj_remove_state(dark_toggle, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(dark_toggle, LV_STATE_DISABLED);
        }
    }

    lv_subject_set_int(&theme_apply_disabled_subject_, 1);
}

void AppearanceSettingsOverlay::handle_apply_theme_clicked() {
    lv_obj_t* dropdown = theme_explorer_overlay_
                             ? lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown")
                             : nullptr;
    if (!dropdown) {
        spdlog::warn("[{}] Apply clicked but dropdown not found", get_name());
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);

    DisplaySettingsManager::instance().set_theme_by_index(selected_index);
    std::string theme_name = DisplaySettingsManager::instance().get_theme_name();

    theme_manager_apply_theme(theme_manager_get_active_theme(), theme_manager_is_dark_mode());

    original_theme_index_ = selected_index;
    original_theme_ = theme_manager_get_active_theme();

    spdlog::info("[{}] Theme '{}' applied (index {})", get_name(), theme_name, selected_index);

    std::string display_name = theme_name;
    if (selected_index >= 0 && selected_index < static_cast<int>(cached_themes_.size())) {
        display_name = cached_themes_[selected_index].display_name;
    }
    std::string toast_msg = "Theme set to " + display_name;
    ToastManager::instance().show(ToastSeverity::SUCCESS, toast_msg.c_str());

    NavigationManager::instance().go_back();
}

void AppearanceSettingsOverlay::handle_edit_colors_clicked() {
    if (!parent_screen_) {
        spdlog::warn("[{}] Theme settings clicked without parent screen", get_name());
        return;
    }

    if (!theme_settings_overlay_) {
        spdlog::debug("[{}] Creating theme editor overlay...", get_name());
        auto& overlay = get_theme_editor_overlay();

        if (!overlay.are_subjects_initialized()) {
            overlay.init_subjects();
        }
        overlay.register_callbacks();

        theme_settings_overlay_ = overlay.create(parent_screen_);
        if (!theme_settings_overlay_) {
            spdlog::error("[{}] Failed to create theme editor overlay", get_name());
            return;
        }

        NavigationManager::instance().register_overlay_instance(theme_settings_overlay_, &overlay);
    }

    if (theme_settings_overlay_) {
        std::string theme_name = !preview_theme_name_.empty()
                                     ? preview_theme_name_
                                     : DisplaySettingsManager::instance().get_theme_name();
        get_theme_editor_overlay().set_editing_dark_mode(preview_is_dark_);
        get_theme_editor_overlay().load_theme(theme_name);
        NavigationManager::instance().push_overlay(theme_settings_overlay_);
    }
}

void AppearanceSettingsOverlay::handle_preview_dark_mode_toggled(bool is_dark) {
    preview_is_dark_ = is_dark;

    if (!theme_explorer_overlay_) {
        return;
    }

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown");
    if (!dropdown) {
        return;
    }

    int selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index < 0 || selected_index >= static_cast<int>(cached_themes_.size())) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);

    if (!theme.is_valid()) {
        return;
    }

    theme_manager_preview(theme, is_dark);

    spdlog::debug("[{}] Preview dark mode toggled to {}", get_name(), is_dark ? "dark" : "light");
}

void AppearanceSettingsOverlay::apply_preview_palette_to_screen_popups() {
    if (!theme_explorer_overlay_ || cached_themes_.empty()) {
        return;
    }

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_explorer_overlay_, "theme_preset_dropdown");
    if (!dropdown) {
        return;
    }

    uint32_t selected_index = lv_dropdown_get_selected(dropdown);
    if (selected_index >= cached_themes_.size()) {
        return;
    }

    helix::ThemeData theme = helix::load_theme_from_file(cached_themes_[selected_index].filename);
    if (!theme.is_valid()) {
        return;
    }

    const helix::ModePalette* palette = nullptr;
    if (preview_is_dark_ && theme.supports_dark()) {
        palette = &theme.dark;
    } else if (!preview_is_dark_ && theme.supports_light()) {
        palette = &theme.light;
    } else {
        palette = theme.supports_dark() ? &theme.dark : &theme.light;
    }

    theme_apply_palette_to_screen_dropdowns(*palette);

    lv_obj_t* modal_dialog = lv_obj_find_by_name(lv_screen_active(), "modal_dialog");
    if (modal_dialog) {
        const char* suffix = theme_manager_get_breakpoint_suffix(responsive_dimension(nullptr));
        int radius_px =
            helix::BorderRadiusSizes::pixels(theme.properties.border_radius_size, suffix);
        lv_obj_set_style_radius(modal_dialog, radius_px, LV_PART_MAIN);
    }
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void AppearanceSettingsOverlay::on_animations_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_animations_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_appearance_settings_overlay().handle_animations_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_dark_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_dark_mode_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_appearance_settings_overlay().handle_dark_mode_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_widget_labels_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_widget_labels_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_appearance_settings_overlay().handle_widget_labels_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_bed_mesh_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_bed_mesh_mode_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int mode = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_appearance_settings_overlay().handle_bed_mesh_mode_changed(mode);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_toolhead_style_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_toolhead_style_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_appearance_settings_overlay().handle_toolhead_style_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_gcode_mode_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_gcode_mode_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_appearance_settings_overlay().handle_gcode_mode_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_z_movement_style_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_z_movement_style_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_appearance_settings_overlay().handle_z_movement_style_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_theme_preset_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_theme_preset_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = lv_dropdown_get_selected(dropdown);
    get_appearance_settings_overlay().handle_theme_preset_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_theme_settings_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_theme_settings_clicked");
    LV_UNUSED(e);
    get_appearance_settings_overlay().handle_theme_settings_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_apply_theme_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_apply_theme_clicked");
    LV_UNUSED(e);
    get_appearance_settings_overlay().handle_apply_theme_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_edit_colors_clicked(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_edit_colors_clicked");
    LV_UNUSED(e);
    get_appearance_settings_overlay().handle_edit_colors_clicked();
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_preview_dark_mode_toggled(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_preview_dark_mode_toggled");
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool is_dark = lv_obj_has_state(target, LV_STATE_CHECKED);
    get_appearance_settings_overlay().handle_preview_dark_mode_toggled(is_dark);
    LVGL_SAFE_EVENT_CB_END();
}

void AppearanceSettingsOverlay::on_preview_open_modal(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[AppearanceSettingsOverlay] on_preview_open_modal");
    LV_UNUSED(e);

    helix::ui::modal_confirm(
        lv_tr("Sample Dialog"),
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod "
        "tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim "
        "veniam, quis nostrud exercitation ullamco laboris.",
        ModalSeverity::Info, "OK", nullptr); // i18n: universal

    auto& overlay = get_appearance_settings_overlay();
    overlay.apply_preview_palette_to_screen_popups();

    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
