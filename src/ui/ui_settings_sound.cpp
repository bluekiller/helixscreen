// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ui_settings_sound.cpp
 * @brief Implementation of SoundSettingsOverlay
 */

#include "ui_settings_sound.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"
#include "ui_sound_preview_overlay.h"

#include "alsa_device_enum.h"
#include "audio_settings_manager.h"
#include "format_utils.h"
#include "sound_manager.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace helix::settings {

// ============================================================================
// SINGLETON ACCESSOR
// ============================================================================

static std::unique_ptr<SoundSettingsOverlay> g_sound_settings_overlay;

SoundSettingsOverlay& get_sound_settings_overlay() {
    if (!g_sound_settings_overlay) {
        g_sound_settings_overlay = std::make_unique<SoundSettingsOverlay>();
        StaticPanelRegistry::instance().register_destroy(
            "SoundSettingsOverlay", []() { g_sound_settings_overlay.reset(); });
    }
    return *g_sound_settings_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

SoundSettingsOverlay::SoundSettingsOverlay() {
    spdlog::debug("[{}] Created", get_name());
}

SoundSettingsOverlay::~SoundSettingsOverlay() {
    spdlog::trace("[{}] Destroyed", get_name());
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void SoundSettingsOverlay::init_subjects() {
    // Every bound subject is owned by a settings manager and registered globally
    // at startup.
    subjects_initialized_ = true;
}

void SoundSettingsOverlay::register_callbacks() {
    register_xml_callbacks({
        {"on_sounds_changed", on_sounds_changed},
        {"on_volume_changed", on_volume_changed},
        {"on_volume_commit", on_volume_commit},
        {"on_ui_sounds_changed", on_ui_sounds_changed},
        {"on_sound_theme_changed", on_sound_theme_changed},
        {"on_audio_device_changed", on_audio_device_changed},
        {"on_preview_sounds", on_preview_sounds},
        {"on_test_tracker", on_test_tracker},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* SoundSettingsOverlay::create(lv_obj_t* parent) {
    if (overlay_root_) {
        spdlog::warn("[{}] create() called but overlay already exists", get_name());
        return overlay_root_;
    }

    spdlog::debug("[{}] Creating overlay...", get_name());

    overlay_root_ =
        static_cast<lv_obj_t*>(lv_xml_create(parent, "settings_sound_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);

    spdlog::info("[{}] Overlay created", get_name());
    return overlay_root_;
}

void SoundSettingsOverlay::show(lv_obj_t* parent_screen) {
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

void SoundSettingsOverlay::on_activate() {
    OverlayBase::on_activate();

    init_sounds_toggle();
    init_volume_slider();
    init_sound_theme_dropdown();
    init_audio_device_dropdown();

#ifndef HELIX_HAS_TRACKER
    if (overlay_root_) {
        lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "container_test_tracker");
        if (container) {
            lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

// ============================================================================
// INIT METHODS
// ============================================================================

void SoundSettingsOverlay::init_sounds_toggle() {
    if (!overlay_root_)
        return;

    lv_obj_t* sounds_row = lv_obj_find_by_name(overlay_root_, "row_sounds");
    if (sounds_row) {
        lv_obj_t* toggle = lv_obj_find_by_name(sounds_row, "toggle");
        if (toggle) {
            if (AudioSettingsManager::instance().get_sounds_enabled()) {
                lv_obj_add_state(toggle, LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(toggle, LV_STATE_CHECKED);
            }
            spdlog::trace("[{}] Sounds toggle initialized", get_name());
        }
    }
}

void SoundSettingsOverlay::init_volume_slider() {
    if (!overlay_root_)
        return;

    lv_obj_t* volume_row = lv_obj_find_by_name(overlay_root_, "row_volume");
    if (!volume_row)
        return;

    lv_obj_t* slider = lv_obj_find_by_name(volume_row, "slider");
    if (slider) {
        int volume = AudioSettingsManager::instance().get_volume();
        lv_slider_set_value(slider, volume, LV_ANIM_OFF);

        helix::format::format_percent(volume, volume_value_buf_, sizeof(volume_value_buf_));

        lv_obj_add_event_cb(slider, on_volume_released, LV_EVENT_RELEASED, nullptr);

        spdlog::debug("[{}] Volume slider initialized to {}%", get_name(), volume);
    }

    lv_obj_t* value_label = lv_obj_find_by_name(volume_row, "value_label");
    if (value_label) {
        lv_label_set_text(value_label, volume_value_buf_);
    }
}

void SoundSettingsOverlay::init_sound_theme_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* theme_row = lv_obj_find_by_name(overlay_root_, "row_sound_theme");
    if (!theme_row)
        return;

    lv_obj_t* dropdown = lv_obj_find_by_name(theme_row, "dropdown");
    if (dropdown) {
        auto& settings = AudioSettingsManager::instance();
        auto themes = SoundManager::instance().get_available_themes();
        std::string current_theme = settings.get_sound_theme();

        std::string options;
        int selected_index = 0;
        for (int i = 0; i < static_cast<int>(themes.size()); i++) {
            if (i > 0)
                options += "\n";
            options += themes[i];
            if (themes[i] == current_theme) {
                selected_index = i;
            }
        }

        if (!options.empty()) {
            lv_dropdown_set_options(dropdown, options.c_str());
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected_index));
        }
        spdlog::trace("[{}] Sound theme dropdown ({} themes, current={})", get_name(),
                      themes.size(), current_theme);
    }
}

void SoundSettingsOverlay::init_audio_device_dropdown() {
    if (!overlay_root_)
        return;

    lv_obj_t* container = lv_obj_find_by_name(overlay_root_, "container_audio_device");
    if (!container)
        return;

    // Visibility is driven declaratively by the compound cond on
    // container_audio_device (sounds_enabled OR audio_device_available). On
    // non-ALSA backends the row is hidden by the binding, so there is nothing
    // to populate — just return.
    if (!SoundManager::instance().has_alsa_backend()) {
        return;
    }

    lv_obj_t* row = lv_obj_find_by_name(container, "row_audio_device");
    lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
    if (!dropdown)
        return;

    auto devices = helix::audio::list();
    std::string current = helix::audio::resolve_alsa_device();

    std::string options;
    int selected_index = 0;
    for (int i = 0; i < static_cast<int>(devices.size()); i++) {
        if (i > 0)
            options += "\n";
        options += devices[i].label;
        if (devices[i].pcm == current) {
            selected_index = i;
        }
    }
    if (!options.empty()) {
        lv_dropdown_set_options(dropdown, options.c_str());
        lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(selected_index));
    }

    // Env lock: HELIX_ALSA_DEVICE wins, so the picker can't change anything.
    if (const char* e = std::getenv("HELIX_ALSA_DEVICE"); e && e[0] != '\0') {
        lv_obj_add_state(dropdown, LV_STATE_DISABLED);
        spdlog::info("[{}] Output device picker disabled (HELIX_ALSA_DEVICE override)", get_name());
    }
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void SoundSettingsOverlay::handle_audio_device_changed(int index) {
    auto devices = helix::audio::list();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
        spdlog::warn("[{}] Output device index {} out of range ({})", get_name(), index,
                     devices.size());
        return;
    }
    const std::string& pcm = devices[index].pcm;
    spdlog::info("[{}] Output device changed: {} ({})", get_name(), devices[index].label, pcm);

    if (!SoundManager::instance().set_output_device(pcm)) {
        // Device failed (or fell back) — re-sync the dropdown to what's actually open.
        spdlog::warn("[{}] Output device '{}' did not apply; re-syncing dropdown", get_name(), pcm);
        init_audio_device_dropdown();
    }
}

void SoundSettingsOverlay::handle_sounds_changed(bool enabled) {
    spdlog::info("[{}] Sounds toggled: {}", get_name(), enabled ? "ON" : "OFF");
    AudioSettingsManager::instance().set_sounds_enabled(enabled);

    if (enabled) {
        SoundManager::instance().play_test_beep();
    }
}

void SoundSettingsOverlay::handle_volume_commit(int value) {
    spdlog::info("[{}] Volume committed: {}%", get_name(), value);
    AudioSettingsManager::instance().set_volume(value);
}

void SoundSettingsOverlay::handle_volume_changed(int value) {
    // Per drag tick: subject + readout only. handle_volume_commit() persists.
    AudioSettingsManager::instance().preview_volume(value);

    helix::format::format_percent(value, volume_value_buf_, sizeof(volume_value_buf_));

    if (overlay_root_) {
        lv_obj_t* volume_row = lv_obj_find_by_name(overlay_root_, "row_volume");
        if (volume_row) {
            lv_obj_t* value_label = lv_obj_find_by_name(volume_row, "value_label");
            if (value_label) {
                lv_label_set_text(value_label, volume_value_buf_);
            }
        }
    }
}

void SoundSettingsOverlay::handle_ui_sounds_changed(bool enabled) {
    spdlog::info("[{}] UI Sounds toggled: {}", get_name(), enabled ? "ON" : "OFF");
    AudioSettingsManager::instance().set_ui_sounds_enabled(enabled);
}

void SoundSettingsOverlay::handle_sound_theme_changed(int index) {
    auto themes = SoundManager::instance().get_available_themes();
    if (index >= 0 && index < static_cast<int>(themes.size())) {
        const auto& theme_name = themes[index];
        spdlog::info("[{}] Sound theme changed: {} (index {})", get_name(), theme_name, index);
        AudioSettingsManager::instance().set_sound_theme(theme_name);
        SoundManager::instance().set_theme(theme_name);
        SoundManager::instance().play("test_beep");
    } else {
        spdlog::warn("[{}] Sound theme index {} out of range ({})", get_name(), index,
                     themes.size());
    }
}

void SoundSettingsOverlay::handle_preview_sounds() {
    spdlog::info("[{}] Opening sound preview", get_name());
    get_sound_preview_overlay().show(parent_screen_);
}

void SoundSettingsOverlay::handle_test_tracker() {
#ifdef HELIX_HAS_TRACKER
    auto& sm = SoundManager::instance();
    if (sm.is_tracker_playing()) {
        spdlog::info("[{}] Stopping tracker playback", get_name());
        sm.stop_tracker();
    } else {
        spdlog::info("[{}] Starting tracker playback: crocketts_theme.mod", get_name());
        sm.play_file("assets/sounds/crocketts_theme.mod");
    }
#else
    spdlog::warn("[{}] Tracker playback not available (HELIX_HAS_TRACKER not defined)", get_name());
#endif
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void SoundSettingsOverlay::on_audio_device_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_audio_device_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_sound_settings_overlay().handle_audio_device_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_sounds_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_sounds_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_sound_settings_overlay().handle_sounds_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_volume_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_volume_changed");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_sound_settings_overlay().handle_volume_changed(value);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_volume_commit(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_volume_commit");
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int value = lv_slider_get_value(slider);
    get_sound_settings_overlay().handle_volume_commit(value);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_volume_released(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_volume_released");
    SoundManager::instance().play_test_beep();
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_ui_sounds_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_ui_sounds_changed");
    auto* toggle = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    get_sound_settings_overlay().handle_ui_sounds_changed(enabled);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_sound_theme_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_sound_theme_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_sound_settings_overlay().handle_sound_theme_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_preview_sounds(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_preview_sounds");
    get_sound_settings_overlay().handle_preview_sounds();
    LVGL_SAFE_EVENT_CB_END();
}

void SoundSettingsOverlay::on_test_tracker(lv_event_t* /*e*/) {
    LVGL_SAFE_EVENT_CB_BEGIN("[SoundSettingsOverlay] on_test_tracker");
    get_sound_settings_overlay().handle_test_tracker();
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
