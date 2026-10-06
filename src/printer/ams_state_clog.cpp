// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ams_state_clog.cpp
 * @brief AmsState: the clog detection meter subjects
 *
 * One of the files AmsState's definitions are split across by concern; the
 * class and its threading contract are in ams_state.h.
 */

#include "ams_state.h"
#include "ams_state_internal.h"
#include "clog_meter_geometry.h"
#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace helix {
using ams_state_detail::assert_main_thread;

namespace {

/// One clog-meter sample, before it is published. The text buffers are sized
/// with the subject buffers they feed: translated mode names and endpoint
/// labels run past the ASCII lengths (ru "Засор: вручную" = 24B, "СПУТЫВАНИЕ" =
/// 20B) and snprintf would clip mid-codepoint.
struct ClogReading {
    int mode = 0;
    int value = 0;
    int warning = 0;
    int danger_pct = 75;
    int peak_pct = 0;
    char mode_text[32] = "";
    char center[16] = "";
    char left[24] = "";
    char right[24] = "";
};

/// The clog detector's reading. Priority: flowguard > encoder > afc_buffer >
/// legacy > none. Source override: 0=auto (use priority), 1=encoder,
/// 2=flowguard, 3=afc; a forced source is used only if this snapshot has it.
ClogReading detector_reading(const AmsSystemInfo& info, int source_override) {
    // Every text slot below has exactly one job, and no slot repeats another:
    //   mode_text   what is measuring, and nothing else. It is drawn beside a
    //               15%-wide swatch in ams_loaded_card, where the column is
    //               content-sized: anything appended here (a detection length,
    //               an AFC buffer state) steals width from the material name
    //               and clips it. Keep it to the source's name.
    //   center      the one number that matters
    //   left/right  the two ends of the axis the fill moves along, and only
    //               where those ends mean different things — a linear mode
    //               fills from nothing, which the empty labels leave unsaid so
    //               the track gets the width instead
    // Severity is not a slot at all: clog_meter_status drives a glyph.
    ClogReading r;
    const auto sources = info.clog_sources();
    const bool forced = source_override >= 1 && source_override <= 3;
    const bool use_encoder = sources.encoder && (!forced || source_override == 1);
    const bool use_flowguard = sources.flowguard && (!forced || source_override == 2);
    const bool use_afc = sources.afc_buffer && (!forced || source_override == 3);

    if (use_flowguard) {
        // Flowguard mode: bidirectional (-100 to +100)
        r.mode = 2;
        r.value = static_cast<int>(info.flowguard_info.level * 100.0f);
        r.value = std::clamp(r.value, -100, 100);

        // A named trigger is Flowguard saying it has tripped.
        if (!info.flowguard_info.trigger.empty()) {
            r.warning = 1;
        }

        snprintf(r.mode_text, sizeof(r.mode_text), "FlowGuard");

        // Enhanced clog detection widget subjects
        r.danger_pct = 80;
        float max_clog = std::abs(info.flowguard_info.max_clog);
        float max_tangle = std::abs(info.flowguard_info.max_tangle);
        r.peak_pct = static_cast<int>(std::max(max_clog, max_tangle) * 100);
        snprintf(r.center, sizeof(r.center), "%+d%%",
                 static_cast<int>(info.flowguard_info.level * 100));
        snprintf(r.left, sizeof(r.left), "%s", lv_tr("TANGLE"));
        snprintf(r.right, sizeof(r.right), "%s", lv_tr("CLOG"));

    } else if (use_encoder) {
        // Encoder mode: 0-100 clog percentage
        r.mode = 1;
        r.value = info.encoder_info.get_clog_pct();
        r.warning = info.encoder_info.is_warning() ? 1 : 0;

        // Source, then how it is armed. The detection length is appended below
        // once it is known to be real — it is configuration, not a scale end,
        // which is where it used to be drawn.
        if (info.encoder_info.detection_mode == 2) {
            snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Clog Auto"));
        } else if (info.encoder_info.detection_mode == 1) {
            snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Clog Manual"));
        } else {
            snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Clog"));
        }

        float det_len = info.encoder_info.detection_length;
        float headroom = info.encoder_info.headroom;
        float desired = info.encoder_info.desired_headroom;
        float min_headroom = info.encoder_info.min_headroom;
        if (det_len > 0) {
            r.danger_pct = static_cast<int>((1.0f - desired / det_len) * 100);
            r.peak_pct = static_cast<int>((1.0f - min_headroom / det_len) * 100);
            snprintf(r.center, sizeof(r.center), "%.1fmm", headroom);
        } else {
            r.danger_pct = 75;
            r.peak_pct = r.value;
            snprintf(r.center, sizeof(r.center), "---");
        }
        // Linear: the fill grows from nothing, so the ends say nothing.

    } else {
        // Check AFC buffer fault detection (buffer_health is per-unit, not per-slot)
        for (const auto& unit : info.units) {
            if (use_afc && unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
                r.mode = 3;
                float dist = unit.buffer_health->distance_to_fault;
                float max_dist = unit.buffer_health->fault_threshold();

                const bool tracking = (dist >= 0 && dist <= max_dist);
                if (!tracking) {
                    // Negative = fault timer stopped, counter stale (normal operation)
                    // Above max = just reset or not yet tracking
                    r.value = 0;
                    r.warning = 0;
                } else {
                    // Actively counting down: 0=fault imminent, max_dist=safe
                    r.value = unit.buffer_health->danger_value();
                    r.warning = unit.buffer_health->is_warning() ? 1 : 0;
                }

                snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("AFC buffer"));

                r.danger_pct = 75;
                r.peak_pct = r.value;
                // Not tracking leaves the centre empty, which is the state
                // clog_meter_is_safe() stands the check icon in for.
                if (tracking) {
                    snprintf(r.center, sizeof(r.center), "%.0fmm", dist);
                }
                // Linear: the fill grows from nothing, so the ends say nothing.
                break; // Use first unit with fault detection
            }
        }

        // Legacy fallback: clog_detection enabled but no encoder_info
        if (r.mode == 0 && info.clog_detection > 0) {
            r.mode = 1;
            r.value = 0; // No headroom data, so there is no clog% to plot
            if (info.clog_detection == 2) {
                snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Clog Auto"));
            } else {
                snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Clog Manual"));
            }
            // Flow rate is all this path has; it is the reading, so it goes in
            // the centre rather than into a slot of its own.
            if (info.encoder_flow_rate >= 0) {
                snprintf(r.center, sizeof(r.center), "%d%%", info.encoder_flow_rate);
            } else {
                snprintf(r.center, sizeof(r.center), "---");
            }
            // Legacy: use defaults (danger_pct=75, peak_pct=0, empty labels)
        }
    }

    return r;
}

/// The buffer's position: Happy Hare's sync feedback, or a filament pressure
/// sensor mapped onto the same -1..+1 bias. Mode None when there is neither.
ClogReading pressure_reading(const AmsSystemInfo& info) {
    ClogReading r;
    if (info.sync_feedback_bias <= -1.5f) {
        return r;
    }
    r.mode = static_cast<int>(helix::ui::ClogMeterMode::Pressure);
    r.value =
        std::clamp(static_cast<int>(std::lround(info.sync_feedback_bias * 100.0f)), -100, 100);
    r.warning = helix::ui::pressure_status(r.value) == helix::ui::ClogMeterStatus::Fault ? 1 : 0;
    r.danger_pct = helix::ui::kPressureFaultPct;
    if (const BufferHealth* fps = info.feeding_pressure_sensor()) {
        // i18n: do not translate - hardware abbreviation
        snprintf(r.mode_text, sizeof(r.mode_text), "FPS");
        snprintf(r.center, sizeof(r.center), "%d%%",
                 static_cast<int>(std::lround(fps->fps_value * 100.0f)));
    } else {
        snprintf(r.mode_text, sizeof(r.mode_text), "%s", lv_tr("Sync"));
        snprintf(r.center, sizeof(r.center), "%+d%%", r.value);
    }
    snprintf(r.left, sizeof(r.left), "%s", lv_tr("TIGHT"));
    snprintf(r.right, sizeof(r.right), "%s", lv_tr("LOOSE"));
    return r;
}

void copy_if_changed(lv_subject_t* subject, const char* text) {
    if (strcmp(lv_subject_get_string(subject), text) != 0) {
        lv_subject_copy_string(subject, text);
    }
}

void publish(const AmsState::ClogMeterSubjects& s, const ClogReading& r) {
    lv_subject_set_int(s.mode, r.mode);
    lv_subject_set_int(s.value, r.value);
    lv_subject_set_int(s.warning, r.warning);
    // Severity is derived, not authored per source, so every source lands on
    // the same rule, with any threshold override already folded in.
    lv_subject_set_int(s.status, static_cast<int>(helix::ui::clog_meter_status(
                                     r.mode, r.value, r.warning, r.danger_pct)));
    copy_if_changed(s.mode_text, r.mode_text);
    lv_subject_set_int(s.danger_pct, r.danger_pct);
    lv_subject_set_int(s.peak_pct, r.peak_pct);
    copy_if_changed(s.center_text, r.center);
    copy_if_changed(s.label_left, r.left);
    copy_if_changed(s.label_right, r.right);
}

} // namespace

AmsState::ClogMeterSubjects AmsState::clog_meter_subjects(helix::ui::ClogSample which) {
    if (which == helix::ui::ClogSample::Pressure) {
        auto& p = clog_pressure_;
        return {&p.mode,       &p.value,    &p.warning,     &p.status,     &p.mode_text,
                &p.danger_pct, &p.peak_pct, &p.center_text, &p.label_left, &p.label_right};
    }
    return {&clog_meter_mode_,       &clog_meter_value_,       &clog_meter_warning_,
            &clog_meter_status_,     &clog_meter_mode_text_,   &clog_meter_danger_pct_,
            &clog_meter_peak_pct_,   &clog_meter_center_text_, &clog_meter_label_left_,
            &clog_meter_label_right_};
}

void AmsState::sync_clog_meter_from_info(const AmsSystemInfo& info) {
    const ClogReading pressure = pressure_reading(info);
    ClogReading primary = detector_reading(info, source_override_);
    // With no detector, the pressure reading is all there is to show.
    if (primary.mode == 0) {
        primary = pressure;
    }
    if (danger_threshold_override_ > 0) {
        primary.danger_pct = danger_threshold_override_;
    }

    publish(clog_meter_subjects(helix::ui::ClogSample::Primary), primary);
    publish(clog_meter_subjects(helix::ui::ClogSample::Pressure), pressure);

    spdlog::trace("[AMS State] Synced clog meter - mode={}, value={}, warning={}, pressure={}",
                  primary.mode, primary.value, primary.warning, pressure.value);
}

void AmsState::set_source_override(int source) {
    assert_main_thread();
    source_override_ = source;
    spdlog::debug("[AMS State] Source override set to {}", source);
    // Re-sync to apply the override
    auto* backend = get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        sync_clog_meter_from_info(info);
    }
}

void AmsState::set_danger_threshold_override(int pct) {
    assert_main_thread();
    danger_threshold_override_ = pct;
    spdlog::debug("[AMS State] Danger threshold override set to {}", pct);
    // Re-sync to apply the override
    auto* backend = get_backend();
    if (backend) {
        auto info = backend->get_system_info();
        sync_clog_meter_from_info(info);
    }
}
} // namespace helix
