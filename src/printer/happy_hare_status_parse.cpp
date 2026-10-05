// src/printer/happy_hare_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "happy_hare_status_parse.h"

#include "ams_status_json.h"
#include "json_utils.h"
#include "text_io.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string_view>

namespace helix::happy_hare {
namespace {

namespace tio = ::helix::text_io;

std::optional<int> read_number_as_int(const nlohmann::json& v) {
    if (v.is_number()) {
        return v.get<int>();
    }
    return std::nullopt;
}

/// gate_color_rgb entry: a 0xRRGGBB integer (traditional), or [r, g, b] floats
/// in 0..1 (EMU).
std::optional<uint32_t> read_gate_rgb(const nlohmann::json& v) {
    if (v.is_number_integer()) {
        return static_cast<uint32_t>(v.get<int>());
    }
    if (v.is_array() && v.size() >= 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
        const auto channel = [&](size_t i) {
            return static_cast<uint8_t>(std::clamp(v[i].get<double>(), 0.0, 1.0) * 255.0 + 0.5);
        };
        return (static_cast<uint32_t>(channel(0)) << 16) |
               (static_cast<uint32_t>(channel(1)) << 8) | static_cast<uint32_t>(channel(2));
    }
    return std::nullopt;
}

std::optional<ams::ColorReading> read_gate_color(const nlohmann::json& v) {
    if (!v.is_string()) {
        return std::nullopt;
    }
    return ams::read_lane_color(v.get<std::string>());
}

/// Per-unit gate counts from `num_gates`: a "6,4" string (dissimilar
/// multi-unit), a plain integer (EMU, one unit) or an array ([8] or [6, 4]).
/// Empty when it names none.
std::vector<int> read_num_gates(const nlohmann::json& ng) {
    std::vector<int> counts;
    if (ng.is_string()) {
        const std::string text = ng.get<std::string>();
        for (std::string_view sv : tio::lines(text, ',')) {
            const std::string token(sv);
            const auto count = tio::parse_leading<int>(token);
            if (!count) {
                spdlog::warn("[AMS HappyHare] Ignoring invalid token in num_gates string");
            } else if (*count > 0) {
                counts.push_back(*count);
            } else {
                spdlog::warn("[AMS HappyHare] Ignoring non-positive gate count {} in "
                             "num_gates string",
                             *count);
            }
        }
    } else if (ng.is_number_integer()) {
        if (ng.get<int>() > 0) {
            counts.push_back(ng.get<int>());
        }
    } else if (ng.is_array()) {
        for (const auto& c : ng) {
            if (c.is_number_integer() && c.get<int>() > 0) {
                counts.push_back(c.get<int>());
            }
        }
    }
    return counts;
}

EncoderDelta read_encoder(const nlohmann::json& encoder) {
    EncoderDelta d;
    d.flow_rate = ams::read_field<int>(encoder, "flow_rate");
    d.desired_headroom = ams::read_field<float>(encoder, "desired_headroom");
    d.detection_length = ams::read_field<float>(encoder, "detection_length");
    d.headroom = ams::read_field<float>(encoder, "headroom");
    d.min_headroom = ams::read_field<float>(encoder, "min_headroom");
    return d;
}

FlowguardDelta read_flowguard(const nlohmann::json& fg) {
    FlowguardDelta d;
    d.enabled = ams::read_field<bool>(fg, "enabled");
    d.active = ams::read_field<bool>(fg, "active");
    d.trigger = ams::read_field<std::string>(fg, "trigger");
    d.level = ams::read_field<float>(fg, "level");
    d.max_clog = ams::read_field<float>(fg, "max_clog");
    d.max_tangle = ams::read_field<float>(fg, "max_tangle");
    d.encoder_mode = ams::read_field<int>(fg, "encoder_mode");
    return d;
}

MmuSensorsDelta read_sensors(const nlohmann::json& sensors) {
    MmuSensorsDelta d;
    constexpr std::string_view prefix = "mmu_pre_gate_";
    for (auto it = sensors.begin(); it != sensors.end(); ++it) {
        const std::string& key = it.key();
        if (key.rfind(prefix, 0) != 0) {
            continue;
        }
        const auto gate = tio::parse_leading<int>(key.substr(prefix.size()));
        if (!gate || *gate < 0) {
            continue;
        }
        d.pre_gate.emplace_back(*gate, it.value().is_boolean() && it.value().get<bool>());
    }
    if (sensors.contains("mmu_pre_gate")) {
        d.aggregate_pre_gate =
            sensors["mmu_pre_gate"].is_boolean() && sensors["mmu_pre_gate"].get<bool>();
    }
    return d;
}

DryingObjectDelta read_drying_object(const nlohmann::json& drying) {
    DryingObjectDelta d;
    d.active = ams::read_field<bool>(drying, "active");
    d.current_temp_c = ams::read_field<float>(drying, "current_temp");
    d.target_temp_c = ams::read_field<float>(drying, "target_temp");
    d.remaining_min = ams::read_integer_field(drying, "remaining_min");
    d.duration_min = ams::read_integer_field(drying, "duration_min");
    d.fan_pct = ams::read_integer_field(drying, "fan_pct");
    return d;
}

} // namespace

MmuCoreDelta parse_core(const nlohmann::json& mmu) {
    MmuCoreDelta d;
    d.gate = ams::read_integer_field(mmu, "gate");
    d.tool = ams::read_integer_field(mmu, "tool");
    if (const auto filament = ams::read_field<std::string>(mmu, "filament")) {
        d.filament_loaded = (*filament == "Loaded");
    }
    d.reason_for_pause = ams::read_field<std::string>(mmu, "reason_for_pause");
    d.action = ams::read_field<std::string>(mmu, "action");
    d.filament_pos = ams::read_integer_field(mmu, "filament_pos");
    if (const auto progress = ams::read_integer_field(mmu, "bowden_progress")) {
        d.bowden_progress = std::clamp(*progress, -1, 100);
    }
    d.has_bypass = ams::read_field<bool>(mmu, "has_bypass");
    return d;
}

MmuTopologyDelta parse_topology(const nlohmann::json& mmu) {
    MmuTopologyDelta d;
    if (const auto units = ams::read_integer_field(mmu, "num_units")) {
        d.num_units = std::max(*units, 1);
    }

    std::vector<int> counts;
    if (mmu.contains("num_gates")) {
        counts = read_num_gates(mmu["num_gates"]);
    }
    // An explicit unit_gate_counts array wins over num_gates.
    if (const auto unit_counts = ams::read_array<int>(mmu, "unit_gate_counts", ams::read_integer)) {
        std::vector<int> explicit_counts;
        for (const auto& c : *unit_counts) {
            if (c) {
                explicit_counts.push_back(*c);
            }
        }
        if (!explicit_counts.empty()) {
            counts = std::move(explicit_counts);
        }
    }
    if (!counts.empty()) {
        d.gate_counts = std::move(counts);
    }

    d.active_unit = ams::read_integer_field(mmu, "unit");

    if (const auto ttg = ams::read_array<int>(mmu, "ttg_map", ams::read_integer)) {
        std::vector<int> map;
        map.reserve(ttg->size());
        for (const auto& mapping : *ttg) {
            if (mapping) {
                map.push_back(*mapping);
            }
        }
        d.ttg_map = std::move(map);
    }
    return d;
}

GateIdentityDelta parse_gate_identity(const nlohmann::json& mmu) {
    GateIdentityDelta d;
    d.gate_status = ams::read_array<int>(mmu, "gate_status", ams::read_integer);
    d.color_rgb = ams::read_array<uint32_t>(mmu, "gate_color_rgb", read_gate_rgb);
    d.color = ams::read_array<ams::ColorReading>(mmu, "gate_color", read_gate_color);
    d.material = ams::read_array<std::string>(mmu, "gate_material");
    d.spool_id = ams::read_array<int>(mmu, "gate_spool_id", ams::read_integer);
    d.temperature = ams::read_array<int>(mmu, "gate_temperature", read_number_as_int);
    d.name = ams::read_array<std::string>(mmu, "gate_name");
    d.filament_name = ams::read_array<std::string>(mmu, "gate_filament_name");
    d.endless_spool_group = ams::read_array<int>(mmu, "endless_spool_groups", ams::read_integer);
    return d;
}

MmuTelemetryDelta parse_telemetry(const nlohmann::json& mmu) {
    MmuTelemetryDelta d;
    d.espooler_active = ams::read_field<std::string>(mmu, "espooler_active");
    d.sync_feedback_state = ams::read_field<std::string>(mmu, "sync_feedback_state");
    d.sync_feedback_bias = ams::read_field<float>(mmu, "sync_feedback_bias_modelled");
    d.sync_feedback_bias_raw = ams::read_field<float>(mmu, "sync_feedback_bias_raw");
    d.sync_drive = ams::read_field<bool>(mmu, "sync_drive");
    d.clog_detection_enabled = ams::read_integer_field(mmu, "clog_detection_enabled");

    if (mmu.contains("encoder") && mmu["encoder"].is_object()) {
        d.encoder = read_encoder(mmu["encoder"]);
    }
    if (mmu.contains("flowguard") && mmu["flowguard"].is_object()) {
        d.flowguard = read_flowguard(mmu["flowguard"]);
    }
    if (mmu.contains("leds") && mmu["leds"].is_object()) {
        const auto& leds = mmu["leds"];
        if (leds.contains("unit0") && leds["unit0"].is_object()) {
            d.led_exit_effect = ams::read_field<std::string>(leds["unit0"], "exit_effect");
        }
    }

    d.sync_feedback_flow_rate = ams::read_field<float>(mmu, "sync_feedback_flow_rate");
    d.toolchange_purge_volume = ams::read_field<float>(mmu, "toolchange_purge_volume");

    // The count of completed tool changes (1 = first swap done) as a 0-based
    // index.
    if (const auto count = ams::read_integer_field(mmu, "num_toolchanges")) {
        d.current_toolchange = (*count > 0) ? (*count - 1) : -1;
    }
    if (mmu.contains("slicer_tool_map") && mmu["slicer_tool_map"].is_object()) {
        d.number_of_toolchanges =
            ams::read_integer_field(mmu["slicer_tool_map"], "total_toolchanges").value_or(0);
    }

    if (const auto mode = ams::read_field<std::string>(mmu, "spoolman_support")) {
        d.spoolman_mode = spoolman_mode_from_string(*mode);
    }
    d.pending_spool_id = ams::read_integer_field(mmu, "pending_spool_id");
    return d;
}

MmuStatusDelta parse_mmu_status(const nlohmann::json& mmu) {
    MmuStatusDelta d;
    d.core = parse_core(mmu);
    d.topology = parse_topology(mmu);
    d.identity = parse_gate_identity(mmu);
    d.telemetry = parse_telemetry(mmu);

    if (mmu.contains("sensors") && mmu["sensors"].is_object()) {
        d.sensors = read_sensors(mmu["sensors"]);
    }

    if (mmu.contains("drying_state")) {
        const auto& drying = mmu["drying_state"];
        if (drying.is_object()) {
            DryingDelta dd;
            dd.object = read_drying_object(drying);
            d.drying = std::move(dd);
        } else if (drying.is_array()) {
            // One state per gate: "active"/"queued" heat, anything else is off. A
            // non-string keeps its place as an empty string so the vector stays
            // indexable by gate number.
            DryingDelta dd;
            std::vector<std::string> states;
            states.reserve(drying.size());
            for (const auto& entry : drying) {
                states.push_back(entry.is_string() ? entry.get<std::string>() : std::string{});
            }
            dd.per_gate = std::move(states);
            d.drying = std::move(dd);
        }
    }

    // Happy Hare publishes the endless-spool ENABLE bit under two keys, both
    // tagged DEPRECATED in mmu.py's get_status() with no replacement shipped:
    // read the newer spelling and fall back to the older one.
    for (const char* key : {"endless_spool_enabled", "endless_spool"}) {
        if (mmu.contains(key) && !mmu[key].is_null()) {
            d.endless_spool_enabled = json_util::safe_bool(mmu, key, false);
            break;
        }
    }
    return d;
}

} // namespace helix::happy_hare
