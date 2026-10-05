// include/happy_hare_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ams_types.h"
#include "lane_translation.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "hv/json.hpp"

/**
 * @file happy_hare_status_parse.h
 * @brief Happy Hare's `printer.mmu` status object, read into plain structs.
 *
 * Moonraker sends deltas, so every parsed field is a std::optional and an
 * omitted one reads as nullopt: a parse never resets state, the backend overlays
 * what a frame carries onto what it holds. A per-gate array is a
 * std::vector of optionals sized as the frame sent it. Pure: no locks, no
 * AmsState.
 */
namespace helix::happy_hare {

template <typename T> using GateArray = std::optional<std::vector<std::optional<T>>>;

/// The selector/filament state: where the MMU is and what it is doing.
struct MmuCoreDelta {
    std::optional<int> gate; ///< -1 = none, -2 = bypass
    std::optional<int> tool;
    std::optional<bool> filament_loaded; ///< from `filament`: "Loaded" / "Unloaded"
    std::optional<std::string> reason_for_pause;
    std::optional<std::string> action;
    std::optional<int> filament_pos;
    std::optional<int> bowden_progress; ///< clamped to [-1, 100]
    std::optional<bool> has_bypass;
};

/// How many gates there are and how they split across units.
struct MmuTopologyDelta {
    std::optional<int> num_units; ///< at least 1
    /// Per-unit gate counts, from `unit_gate_counts` when it names any, else
    /// from `num_gates` (a "6,4" string, an integer or an array).
    std::optional<std::vector<int>> gate_counts;
    std::optional<int> active_unit;
    std::optional<std::vector<int>> ttg_map; ///< tool -> gate
};

/// What the MMU says about each gate's spool, one array per key.
struct GateIdentityDelta {
    GateArray<int> gate_status; ///< -1 unknown, 0 empty, 1 available, 2 from buffer
    /// gate_color_rgb: 0xRRGGBB integers, or [r, g, b] floats in 0..1.
    GateArray<uint32_t> color_rgb;
    GateArray<ams::ColorReading> color; ///< gate_color hex strings
    GateArray<std::string> material;
    GateArray<int> spool_id;
    GateArray<int> temperature;
    GateArray<std::string> name;          ///< gate_name
    GateArray<std::string> filament_name; ///< gate_filament_name (EMU)
    GateArray<int> endless_spool_group;
};

struct EncoderDelta {
    std::optional<int> flow_rate;
    std::optional<float> desired_headroom;
    std::optional<float> detection_length;
    std::optional<float> headroom;
    std::optional<float> min_headroom;
};

struct FlowguardDelta {
    std::optional<bool> enabled;
    std::optional<bool> active;
    std::optional<std::string> trigger;
    std::optional<float> level;
    std::optional<float> max_clog;
    std::optional<float> max_tangle;
    std::optional<int> encoder_mode;
};

/// The v4 extended status: eSpooler, sync feedback, clog detection, counters.
struct MmuTelemetryDelta {
    std::optional<std::string> espooler_active;
    std::optional<std::string> sync_feedback_state;
    std::optional<float> sync_feedback_bias; ///< sync_feedback_bias_modelled
    std::optional<float> sync_feedback_bias_raw;
    std::optional<bool> sync_drive;
    std::optional<int> clog_detection_enabled;
    std::optional<EncoderDelta> encoder;
    std::optional<FlowguardDelta> flowguard;
    std::optional<std::string> led_exit_effect; ///< leds.unit0.exit_effect
    std::optional<float> sync_feedback_flow_rate;
    std::optional<float> toolchange_purge_volume;
    /// num_toolchanges as a 0-based index of the current change, -1 for none.
    std::optional<int> current_toolchange;
    /// slicer_tool_map.total_toolchanges; 0 when the object is present without
    /// one.
    std::optional<int> number_of_toolchanges;
    std::optional<SpoolmanMode> spoolman_mode;
    std::optional<int> pending_spool_id;
};

/// `sensors`: the pre-gate sensor readings.
struct MmuSensorsDelta {
    /// `mmu_pre_gate_N` entries in object order: gate index and whether the
    /// sensor is triggered (null and non-booleans read as not triggered).
    std::vector<std::pair<int, bool>> pre_gate;
    /// The aggregate `mmu_pre_gate` of EMU boxes, which only knows the active
    /// gate.
    std::optional<bool> aggregate_pre_gate;
};

struct DryingObjectDelta {
    std::optional<bool> active;
    std::optional<float> current_temp_c;
    std::optional<float> target_temp_c;
    std::optional<int> remaining_min;
    std::optional<int> duration_min;
    std::optional<int> fan_pct;
};

/// `drying_state`: an object (KMS) or one string per gate (EMU).
struct DryingDelta {
    std::optional<DryingObjectDelta> object;
    std::optional<std::vector<std::string>> per_gate;
};

struct MmuStatusDelta {
    MmuCoreDelta core;
    MmuTopologyDelta topology;
    GateIdentityDelta identity;
    MmuTelemetryDelta telemetry;
    std::optional<MmuSensorsDelta> sensors;
    std::optional<DryingDelta> drying;
    /// The endless-spool ENABLE bit, from `endless_spool_enabled` or its older
    /// spelling `endless_spool`.
    std::optional<bool> endless_spool_enabled;
};

[[nodiscard]] MmuCoreDelta parse_core(const nlohmann::json& mmu);
[[nodiscard]] MmuTopologyDelta parse_topology(const nlohmann::json& mmu);
[[nodiscard]] GateIdentityDelta parse_gate_identity(const nlohmann::json& mmu);
[[nodiscard]] MmuTelemetryDelta parse_telemetry(const nlohmann::json& mmu);
[[nodiscard]] MmuStatusDelta parse_mmu_status(const nlohmann::json& mmu);

} // namespace helix::happy_hare
