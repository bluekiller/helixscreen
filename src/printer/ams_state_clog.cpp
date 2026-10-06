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

void AmsState::sync_clog_meter_from_info(const AmsSystemInfo& info) {
    // Priority: flowguard > encoder > afc_buffer > pressure > legacy > none
    // Source override: 0=auto (use priority), 1=encoder, 2=flowguard, 3=afc,
    // 4=pressure
    //
    // The three detectors outrank pressure: they are what pauses a print,
    // while a buffer's position is a running reading of how the feed is
    // keeping up. Pressure is shown when nothing detects clogs, or by choice.
    //
    // Every text slot below has exactly one job, and no slot repeats another:
    //   mode_text   what is measuring, and nothing else. It is drawn beside a
    //               15%-wide swatch in ams_loaded_card, where the column is
    //               content-sized: anything appended here (a detection length,
    //               an AFC buffer state) steals width from the material name
    //               and clips it. Keep it to the source's name.
    //   center_buf  the one number that matters
    //   left/right  the two ends of the axis the fill moves along, and only
    //               where those ends mean different things — a linear mode
    //               fills from nothing, which the empty labels leave unsaid so
    //               the track gets the width instead
    // Severity is not a slot at all: clog_meter_status drives a glyph.
    int mode = 0;
    int value = 0;
    int warning = 0;
    // Sized with the member buffers they feed: translated mode names and
    // endpoint labels run past the ASCII lengths (ru "Засор: вручную" = 24B,
    // "СПУТЫВАНИЕ" = 20B) and snprintf would clip mid-codepoint.
    char mode_text[32] = "";
    int new_danger_pct = 75;
    int new_peak_pct = 0;
    char center_buf[16] = "";
    char left_buf[24] = "";
    char right_buf[24] = "";

    // A forced source is used only if this snapshot has it.
    const auto sources = info.clog_sources();
    const bool forced = source_override_ >= 1 && source_override_ <= 4;
    const bool use_encoder = sources.encoder && (!forced || source_override_ == 1);
    const bool use_flowguard = sources.flowguard && (!forced || source_override_ == 2);
    const bool use_afc = sources.afc_buffer && (!forced || source_override_ == 3);
    const bool use_pressure = sources.pressure && (!forced || source_override_ == 4);

    if (use_flowguard) {
        // Flowguard mode: bidirectional (-100 to +100)
        mode = 2;
        value = static_cast<int>(info.flowguard_info.level * 100.0f);
        value = std::clamp(value, -100, 100);

        // A named trigger is Flowguard saying it has tripped.
        if (!info.flowguard_info.trigger.empty()) {
            warning = 1;
        }

        snprintf(mode_text, sizeof(mode_text), "FlowGuard");

        // Enhanced clog detection widget subjects
        new_danger_pct = 80;
        float max_clog = std::abs(info.flowguard_info.max_clog);
        float max_tangle = std::abs(info.flowguard_info.max_tangle);
        new_peak_pct = static_cast<int>(std::max(max_clog, max_tangle) * 100);
        snprintf(center_buf, sizeof(center_buf), "%+d%%",
                 static_cast<int>(info.flowguard_info.level * 100));
        snprintf(left_buf, sizeof(left_buf), "%s", lv_tr("TANGLE"));
        snprintf(right_buf, sizeof(right_buf), "%s", lv_tr("CLOG"));

    } else if (use_encoder) {
        // Encoder mode: 0-100 clog percentage
        mode = 1;
        value = info.encoder_info.get_clog_pct();
        warning = info.encoder_info.is_warning() ? 1 : 0;

        // Source, then how it is armed. The detection length is appended below
        // once it is known to be real — it is configuration, not a scale end,
        // which is where it used to be drawn.
        if (info.encoder_info.detection_mode == 2) {
            snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Clog Auto"));
        } else if (info.encoder_info.detection_mode == 1) {
            snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Clog Manual"));
        } else {
            snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Clog"));
        }

        float det_len = info.encoder_info.detection_length;
        float headroom = info.encoder_info.headroom;
        float desired = info.encoder_info.desired_headroom;
        float min_headroom = info.encoder_info.min_headroom;
        if (det_len > 0) {
            new_danger_pct = static_cast<int>((1.0f - desired / det_len) * 100);
            new_peak_pct = static_cast<int>((1.0f - min_headroom / det_len) * 100);
            snprintf(center_buf, sizeof(center_buf), "%.1fmm", headroom);
        } else {
            new_danger_pct = 75;
            new_peak_pct = value;
            snprintf(center_buf, sizeof(center_buf), "---");
        }
        // Linear: the fill grows from nothing, so the ends say nothing.

    } else {
        // Check AFC buffer fault detection (buffer_health is per-unit, not per-slot)
        for (const auto& unit : info.units) {
            if (use_afc && unit.buffer_health && unit.buffer_health->fault_detection_enabled) {
                mode = 3;
                float dist = unit.buffer_health->distance_to_fault;
                float max_dist = unit.buffer_health->fault_threshold();

                const bool tracking = (dist >= 0 && dist <= max_dist);
                if (!tracking) {
                    // Negative = fault timer stopped, counter stale (normal operation)
                    // Above max = just reset or not yet tracking
                    value = 0;
                    warning = 0;
                } else {
                    // Actively counting down: 0=fault imminent, max_dist=safe
                    value = unit.buffer_health->danger_value();
                    warning = unit.buffer_health->is_warning() ? 1 : 0;
                }

                snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("AFC buffer"));

                new_danger_pct = 75;
                new_peak_pct = value;
                // Not tracking leaves the centre empty, which is the state
                // clog_meter_is_safe() stands the check icon in for.
                if (tracking) {
                    snprintf(center_buf, sizeof(center_buf), "%.0fmm", dist);
                }
                // Linear: the fill grows from nothing, so the ends say nothing.
                break; // Use first unit with fault detection
            }
        }

        // The buffer's position: Happy Hare's sync feedback, or a filament
        // pressure sensor mapped onto the same -1..+1 bias.
        if (mode == 0 && use_pressure) {
            mode = static_cast<int>(helix::ui::ClogMeterMode::Pressure);
            value = std::clamp(static_cast<int>(std::lround(info.sync_feedback_bias * 100.0f)),
                               -100, 100);
            warning =
                helix::ui::pressure_status(value) == helix::ui::ClogMeterStatus::Fault ? 1 : 0;
            new_danger_pct = helix::ui::kPressureFaultPct;
            new_peak_pct = 0;
            if (const BufferHealth* fps = info.feeding_pressure_sensor()) {
                // i18n: do not translate - hardware abbreviation
                snprintf(mode_text, sizeof(mode_text), "FPS");
                snprintf(center_buf, sizeof(center_buf), "%d%%",
                         static_cast<int>(std::lround(fps->fps_value * 100.0f)));
            } else {
                snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Sync"));
                snprintf(center_buf, sizeof(center_buf), "%+d%%", value);
            }
            snprintf(left_buf, sizeof(left_buf), "%s", lv_tr("TIGHT"));
            snprintf(right_buf, sizeof(right_buf), "%s", lv_tr("LOOSE"));
        }

        // Legacy fallback: clog_detection enabled but no encoder_info
        if (mode == 0 && info.clog_detection > 0) {
            mode = 1;
            value = 0; // No headroom data, so there is no clog% to plot
            if (info.clog_detection == 2) {
                snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Clog Auto"));
            } else {
                snprintf(mode_text, sizeof(mode_text), "%s", lv_tr("Clog Manual"));
            }
            // Flow rate is all this path has; it is the reading, so it goes in
            // the centre rather than into a slot of its own.
            if (info.encoder_flow_rate >= 0) {
                snprintf(center_buf, sizeof(center_buf), "%d%%", info.encoder_flow_rate);
            } else {
                snprintf(center_buf, sizeof(center_buf), "---");
            }
            // Legacy: use defaults (danger_pct=75, peak_pct=0, empty labels)
        }
    }

    // Apply danger threshold override if set
    if (danger_threshold_override_ > 0)
        new_danger_pct = danger_threshold_override_;

    lv_subject_set_int(&clog_meter_mode_, mode);
    lv_subject_set_int(&clog_meter_value_, value);
    lv_subject_set_int(&clog_meter_warning_, warning);
    // Severity is derived, not authored per branch, so every source lands on
    // the same rule — and the threshold override above is already folded in.
    const int status =
        static_cast<int>(helix::ui::clog_meter_status(mode, value, warning, new_danger_pct));
    lv_subject_set_int(&clog_meter_status_, status);
    lv_subject_set_int(&clog_meter_symmetrical_,
                       helix::ui::clog_meter_is_symmetrical(mode) ? 1 : 0);
    if (strcmp(lv_subject_get_string(&clog_meter_mode_text_), mode_text) != 0) {
        lv_subject_copy_string(&clog_meter_mode_text_, mode_text);
    }
    lv_subject_set_int(&clog_meter_danger_pct_, new_danger_pct);
    lv_subject_set_int(&clog_meter_peak_pct_, new_peak_pct);
    if (strcmp(lv_subject_get_string(&clog_meter_center_text_), center_buf) != 0) {
        lv_subject_copy_string(&clog_meter_center_text_, center_buf);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_label_left_), left_buf) != 0) {
        lv_subject_copy_string(&clog_meter_label_left_, left_buf);
    }
    if (strcmp(lv_subject_get_string(&clog_meter_label_right_), right_buf) != 0) {
        lv_subject_copy_string(&clog_meter_label_right_, right_buf);
    }

    spdlog::trace("[AMS State] Synced clog meter - mode={}, value={}, warning={}", mode, value,
                  warning);
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
