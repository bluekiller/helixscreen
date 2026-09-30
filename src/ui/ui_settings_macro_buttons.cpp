// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_settings_macro_buttons.h"

#include "ui_callback_helpers.h"
#include "ui_event_safety.h"
#include "ui_nav_manager.h"

#include "app_globals.h"
#include "config.h"
#include "i_moonraker_api.h"
#include "quick_action_slots.h"
#include "standard_macros.h"
#include "static_panel_registry.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace helix::settings {

// ============================================================================
// GLOBAL INSTANCE
// ============================================================================

static std::unique_ptr<MacroButtonsOverlay> g_macro_buttons_overlay;

MacroButtonsOverlay& get_macro_buttons_overlay() {
    if (!g_macro_buttons_overlay) {
        g_macro_buttons_overlay = std::make_unique<MacroButtonsOverlay>();
        StaticPanelRegistry::instance().register_destroy("MacroButtonsOverlay",
                                                         []() { g_macro_buttons_overlay.reset(); });
    }
    return *g_macro_buttons_overlay;
}

// ============================================================================
// CONSTRUCTOR / DESTRUCTOR
// ============================================================================

MacroButtonsOverlay::MacroButtonsOverlay() {
    spdlog::trace("[{}] Constructor", get_name());
}

MacroButtonsOverlay::~MacroButtonsOverlay() {
    deinit_subjects();
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void MacroButtonsOverlay::init_subjects() {
    if (subjects_initialized_) {
        spdlog::warn("[{}] init_subjects() called twice - ignoring", get_name());
        return;
    }

    // No subjects needed for this overlay - dropdowns populated imperatively
    subjects_initialized_ = true;
    spdlog::debug("[{}] Subjects initialized", get_name());
}

void MacroButtonsOverlay::register_callbacks() {
    register_xml_callbacks({
        // Quick button dropdown callbacks
        {"on_quick_button_1_changed", on_quick_button_1_changed},
        {"on_quick_button_2_changed", on_quick_button_2_changed},
        {"on_quick_button_3_changed", on_quick_button_3_changed},
        {"on_quick_button_4_changed", on_quick_button_4_changed},
        // Standard macro slot callbacks
        {"on_load_filament_changed", on_load_filament_changed},
        {"on_unload_filament_changed", on_unload_filament_changed},
        {"on_purge_changed", on_purge_changed},
        {"on_pause_changed", on_pause_changed},
        {"on_resume_changed", on_resume_changed},
        {"on_cancel_changed", on_cancel_changed},
        {"on_bed_mesh_changed", on_bed_mesh_changed},
        {"on_bed_level_changed", on_bed_level_changed},
        {"on_clean_nozzle_changed", on_clean_nozzle_changed},
        {"on_heat_soak_changed", on_heat_soak_changed},
        {"on_park_toolhead_changed", on_park_toolhead_changed},
    });

    spdlog::debug("[{}] Callbacks registered", get_name());
}

void MacroButtonsOverlay::deinit_subjects() {
    // SubjectManager handles cleanup automatically via RAII
    subjects_initialized_ = false;
}

// ============================================================================
// UI CREATION
// ============================================================================

lv_obj_t* MacroButtonsOverlay::create(lv_obj_t* parent) {
    if (!parent) {
        spdlog::error("[{}] NULL parent", get_name());
        return nullptr;
    }

    // Create overlay from XML
    overlay_root_ = static_cast<lv_obj_t*>(lv_xml_create(parent, "macro_buttons_overlay", nullptr));
    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay from XML", get_name());
        return nullptr;
    }

    // Initially hidden
    lv_obj_add_flag(overlay_root_, LV_OBJ_FLAG_HIDDEN);
    spdlog::info("[{}] Overlay created", get_name());

    return overlay_root_;
}

void MacroButtonsOverlay::show(lv_obj_t* parent_screen) {
    spdlog::debug("[{}] show() called", get_name());

    // Ensure subjects and callbacks are initialized
    if (!subjects_initialized_) {
        init_subjects();
        register_callbacks();
    }

    // Create overlay on first access (lazy initialization)
    if (!overlay_root_ && parent_screen) {
        create(parent_screen);
    }

    if (!overlay_root_) {
        spdlog::error("[{}] Failed to create overlay", get_name());
        return;
    }

    // Register with NavigationManager for lifecycle callbacks
    NavigationManager::instance().register_overlay_instance(overlay_root_, this);

    // Push overlay onto navigation history and show it
    NavigationManager::instance().push_overlay(overlay_root_);
}

// ============================================================================
// LIFECYCLE
// ============================================================================

void MacroButtonsOverlay::on_activate() {
    OverlayBase::on_activate();
    // Populate dropdowns when overlay becomes visible (handles printer reconnection)
    populate_dropdowns();
}

// ============================================================================
// DROPDOWN POPULATION
// ============================================================================

void MacroButtonsOverlay::populate_dropdowns() {
    if (!overlay_root_) {
        return;
    }

    spdlog::debug("[{}] Refreshing macro dropdowns...", get_name());

    // === Populate Quick Button Dropdowns ===
    // Options: "(Empty)", then slot display names
    std::string quick_button_options = "(Empty)";
    for (const auto& slot : StandardMacros::instance().all()) {
        quick_button_options += "\n" + slot.display_name;
    }
    // The light toggle comes last, after every standard macro slot
    quick_button_options += std::string("\n") + lv_tr("Light");

    // Each dropdown shows what its slot resolves to, so a slot the light fills
    // by default reads "Light" rather than the "(Empty)" it stores.
    const helix::StoredQuickSlots stored = helix::read_stored_quick_slots();
    const auto kinds = helix::resolve_current_quick_slots(stored);
    std::vector<std::string> slot_names;
    for (const auto& slot : StandardMacros::instance().all()) {
        slot_names.push_back(slot.slot_name);
    }
    for (size_t i = 0; i < helix::kQuickButtonKeys.size(); ++i) {
        const std::string row_name = "row_quick_button_" + std::to_string(i + 1);
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, row_name.c_str());
        lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
        if (dropdown) {
            lv_dropdown_set_options(dropdown, quick_button_options.c_str());
            lv_dropdown_set_selected(dropdown, static_cast<uint32_t>(helix::quick_slot_picker_index(
                                                   kinds[i], stored.value[i], slot_names)));
        }
    }

    // === Populate Standard Macro Dropdowns ===
    // Get sorted list of all printer macros from IMoonrakerAPI
    printer_macros_.clear();
    IMoonrakerAPI* api = get_moonraker_api();
    if (api) {
        const auto& macros = api->hardware().macros();
        for (const auto& macro : macros) {
            printer_macros_.push_back(macro);
        }
        std::sort(printer_macros_.begin(), printer_macros_.end());
    }

    // Row names matching XML
    const std::vector<std::pair<StandardMacroSlot, std::string>> slot_rows = {
        {StandardMacroSlot::LoadFilament, "row_load_filament"},
        {StandardMacroSlot::UnloadFilament, "row_unload_filament"},
        {StandardMacroSlot::Purge, "row_purge"},
        {StandardMacroSlot::Pause, "row_pause"},
        {StandardMacroSlot::Resume, "row_resume"},
        {StandardMacroSlot::Cancel, "row_cancel"},
        {StandardMacroSlot::BedMesh, "row_bed_mesh"},
        {StandardMacroSlot::BedLevel, "row_bed_level"},
        {StandardMacroSlot::CleanNozzle, "row_clean_nozzle"},
        {StandardMacroSlot::HeatSoak, "row_heat_soak"},
        {StandardMacroSlot::ParkToolhead, "row_park_toolhead"},
    };

    for (const auto& [slot, row_name] : slot_rows) {
        lv_obj_t* row = lv_obj_find_by_name(overlay_root_, row_name.c_str());
        lv_obj_t* dropdown = row ? lv_obj_find_by_name(row, "dropdown") : nullptr;
        if (!dropdown)
            continue;

        const auto& info = StandardMacros::instance().get(slot);

        // Build options string - first option shows what "no override" resolves to.
        // Must follow StandardMacroInfo's own priority, or the row claims one
        // thing while dispatch runs another.
        std::string options;
        if (!info.shipped_macro.empty()) {
            // A shipped sequence is multi-line gcode, not a name — printing it
            // raw would fill the dropdown with a script.
            options = std::string("(Auto: ") + lv_tr("printer default") + ")";
        } else if (!info.detected_macro.empty()) {
            options = "(Auto: " + info.detected_macro + ")";
        } else if (!info.fallback_macro.empty()) {
            options = "(Auto: " + info.fallback_macro + ")";
        } else {
            options = "(Empty)";
        }

        // Add all printer macros
        for (const auto& macro : printer_macros_) {
            options += "\n" + macro;
        }

        lv_dropdown_set_options(dropdown, options.c_str());

        // Set selected value
        if (!info.configured_macro.empty()) {
            // Find the configured macro in the list
            int idx = 1; // Start after "(Auto/Empty)"
            for (const auto& macro : printer_macros_) {
                if (macro == info.configured_macro) {
                    lv_dropdown_set_selected(dropdown, idx);
                    break;
                }
                ++idx;
            }
        } else {
            // Use auto (index 0)
            lv_dropdown_set_selected(dropdown, 0);
        }
    }

    spdlog::debug("[{}] Macro dropdowns refreshed ({} printer macros)", get_name(),
                  printer_macros_.size());
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

std::string MacroButtonsOverlay::quick_button_index_to_slot_name(int index) {
    if (index == 0) {
        return ""; // Empty - no slot assigned
    }
    // Map index-1 to StandardMacroSlot enum order
    const auto& slots = StandardMacros::instance().all();
    if (index - 1 < static_cast<int>(slots.size())) {
        return slots[index - 1].slot_name;
    }
    if (index - 1 == static_cast<int>(slots.size())) {
        return std::string(helix::kQuickSlotLight);
    }
    return "";
}

std::string MacroButtonsOverlay::get_selected_macro_from_dropdown(lv_obj_t* dropdown) {
    char buf[128]; // 128 bytes to handle longer macro names
    lv_dropdown_get_selected_str(dropdown, buf, sizeof(buf));
    std::string selected(buf);

    // Check for special options
    if (selected.find("(Auto") == 0 || selected.find("(Empty)") == 0) {
        return ""; // Clear configured macro, use auto-detection
    }

    return selected; // Return the macro name
}

// ============================================================================
// EVENT HANDLERS
// ============================================================================

void MacroButtonsOverlay::handle_quick_button_1_changed(int index) {
    std::string slot_name = quick_button_index_to_slot_name(index);

    Config* config = Config::get_instance();
    config->set<std::string>(helix::kQuickButtonKeys[0], slot_name);
    config->save();

    spdlog::info("[{}] Quick button 1 set to: {}", get_name(),
                 slot_name.empty() ? "(empty)" : slot_name);
}

void MacroButtonsOverlay::handle_quick_button_2_changed(int index) {
    std::string slot_name = quick_button_index_to_slot_name(index);

    Config* config = Config::get_instance();
    config->set<std::string>(helix::kQuickButtonKeys[1], slot_name);
    config->save();

    spdlog::info("[{}] Quick button 2 set to: {}", get_name(),
                 slot_name.empty() ? "(empty)" : slot_name);
}

void MacroButtonsOverlay::handle_quick_button_3_changed(int index) {
    std::string slot_name = quick_button_index_to_slot_name(index);

    Config* config = Config::get_instance();
    config->set<std::string>(helix::kQuickButtonKeys[2], slot_name);
    config->save();

    spdlog::info("[{}] Quick button 3 set to: {}", get_name(),
                 slot_name.empty() ? "(empty)" : slot_name);
}

void MacroButtonsOverlay::handle_quick_button_4_changed(int index) {
    std::string slot_name = quick_button_index_to_slot_name(index);

    Config* config = Config::get_instance();
    config->set<std::string>(helix::kQuickButtonKeys[3], slot_name);
    config->save();

    spdlog::info("[{}] Quick button 4 set to: {}", get_name(),
                 slot_name.empty() ? "(empty)" : slot_name);
}

void MacroButtonsOverlay::handle_standard_macro_changed(StandardMacroSlot slot,
                                                        lv_obj_t* dropdown) {
    std::string macro = get_selected_macro_from_dropdown(dropdown);

    StandardMacros::instance().set_macro(slot, macro);

    const auto& info = StandardMacros::instance().get(slot);
    spdlog::info("[{}] {} macro set to: {} (resolved: {})", get_name(), info.display_name,
                 macro.empty() ? "(auto)" : macro, info.get_macro());
}

// ============================================================================
// STATIC CALLBACKS
// ============================================================================

void MacroButtonsOverlay::on_quick_button_1_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_quick_button_1_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_macro_buttons_overlay().handle_quick_button_1_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_quick_button_2_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_quick_button_2_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_macro_buttons_overlay().handle_quick_button_2_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_quick_button_3_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_quick_button_3_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_macro_buttons_overlay().handle_quick_button_3_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_quick_button_4_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_quick_button_4_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    int index = static_cast<int>(lv_dropdown_get_selected(dropdown));
    get_macro_buttons_overlay().handle_quick_button_4_changed(index);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_load_filament_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_load_filament_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::LoadFilament,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_unload_filament_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_unload_filament_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::UnloadFilament,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_purge_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_purge_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::Purge, dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_pause_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_pause_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::Pause, dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_resume_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_resume_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::Resume, dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_cancel_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_cancel_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::Cancel, dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_bed_mesh_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_bed_mesh_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::BedMesh, dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_bed_level_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_bed_level_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::BedLevel,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_clean_nozzle_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_clean_nozzle_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::CleanNozzle,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_park_toolhead_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_park_toolhead_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::ParkToolhead,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

void MacroButtonsOverlay::on_heat_soak_changed(lv_event_t* e) {
    LVGL_SAFE_EVENT_CB_BEGIN("[MacroButtonsOverlay] on_heat_soak_changed");
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    get_macro_buttons_overlay().handle_standard_macro_changed(StandardMacroSlot::HeatSoak,
                                                              dropdown);
    LVGL_SAFE_EVENT_CB_END();
}

} // namespace helix::settings
