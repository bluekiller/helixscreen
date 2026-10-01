// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file printer_composite_visibility_state.cpp
 * @brief Aggregate `has_any_preprint_options` visibility subject
 *
 * Manages the single aggregate subject driving the PRINT OPTIONS card's
 * visibility in print_file_detail.xml. Computed from plugin-gated hardware
 * capabilities, the (un-gated) timelapse capability, and the new
 * PrePrintOption framework's option count.
 */

#include "printer_composite_visibility_state.h"

#include "state/subject_macros.h"

#include <spdlog/spdlog.h>

namespace helix {

void PrinterCompositeVisibilityState::init_subjects(bool register_xml) {
    if (subjects_initialized_) {
        spdlog::debug("[PrinterCompositeVisibilityState] Subjects already initialized, skipping");
        return;
    }

    spdlog::trace("[PrinterCompositeVisibilityState] Initializing subjects (register_xml={})",
                  register_xml);

    INIT_SUBJECT_INT(has_any_preprint_options, 0, subjects_, register_xml);

    subjects_initialized_ = true;
    spdlog::trace("[PrinterCompositeVisibilityState] Subjects initialized successfully");
}

void PrinterCompositeVisibilityState::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::debug("[PrinterCompositeVisibilityState] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
}

void PrinterCompositeVisibilityState::update_visibility(
    bool plugin_installed, const PrinterCapabilitiesState& capabilities,
    size_t framework_option_count) {
    // has_any_preprint_options = (plugin_installed && any plugin-gated cap)
    //                          || timelapse capability (no plugin gate)
    //                          || framework_option_count > 0
    const bool any_plugin_gated_cap =
        lv_subject_get_int(capabilities.subject(Capability::HasBedMesh)) ||
        lv_subject_get_int(capabilities.subject(Capability::HasQgl)) ||
        lv_subject_get_int(capabilities.subject(Capability::HasZTilt)) ||
        lv_subject_get_int(capabilities.subject(Capability::HasNozzleClean)) ||
        lv_subject_get_int(capabilities.subject(Capability::HasPurgeLine));

    const bool any_visible = (plugin_installed && any_plugin_gated_cap) ||
                             lv_subject_get_int(capabilities.subject(Capability::HasTimelapse)) ||
                             framework_option_count > 0;

    const int new_any = any_visible ? 1 : 0;
    lv_subject_set_int(&has_any_preprint_options_, new_any);

    if (!last_log_state_initialized_ || new_any != last_any_ || plugin_installed != last_plugin_) {
        spdlog::debug("[PrinterCompositeVisibilityState] has_any_preprint_options={} (plugin={}, "
                      "framework={})",
                      new_any, plugin_installed, framework_option_count);
        last_any_ = new_any;
        last_plugin_ = plugin_installed;
        last_log_state_initialized_ = true;
    }
}

} // namespace helix
