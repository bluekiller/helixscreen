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
    for (auto it = sensors.begin(); it != sensors.end(); ++it) {
        const std::string& key = it.key();
        for (const std::string_view prefix : {"mmu_pre_gate_", "mmu_entry_"}) {
            if (key.rfind(prefix, 0) != 0) {
                continue;
            }
            const auto gate = tio::parse_leading<int>(key.substr(prefix.size()));
            if (gate && *gate >= 0) {
                d.pre_gate.emplace_back(*gate, it.value().is_boolean() && it.value().get<bool>());
            }
        }
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

/// Which v4 section holds a tunable.
enum class ParamScope { Machine, Unit, Toolhead };

struct ParamRow {
    std::string_view v3;
    std::string_view v4; ///< empty: v4 has no such parameter
    ParamScope scope;
};

// Every tunable the backend reads from configfile or sends through
// MMU_TEST_CONFIG. v4 sources: mmu_machine_parameters.py (Machine),
// unit/mmu_unit_parameters.py and the selector parameter classes, which read
// [mmu_unit_parameters] too (Unit), unit/mmu_toolhead_wrapper.py (Toolhead).
constexpr ParamRow kParams[] = {
    {"form_tip_macro", "form_tip_macro", ParamScope::Machine},
    {"extruder_load_speed", "extruder_load_speed", ParamScope::Machine},
    {"extruder_unload_speed", "extruder_unload_speed", ParamScope::Machine},
    {"gear_from_spool_speed", "gear_load_speed", ParamScope::Unit},
    {"gear_from_buffer_speed", "gear_from_filament_buffer_speed", ParamScope::Unit},
    {"gear_unload_speed", "gear_unload_speed", ParamScope::Unit},
    {"selector_move_speed", "selector_move_speed", ParamScope::Unit},
    {"sync_to_extruder", "sync_to_extruder", ParamScope::Unit},
    {"heater_max_temp", "heater_max_temp", ParamScope::Unit},
    {"clog_detection", "", ParamScope::Unit},
    {"toolhead_sensor_to_nozzle", "toolhead_sensor_to_nozzle", ParamScope::Toolhead},
    {"toolhead_extruder_to_nozzle", "toolhead_extruder_to_nozzle", ParamScope::Toolhead},
    {"toolhead_entry_to_extruder", "toolhead_entry_to_extruder", ParamScope::Toolhead},
    {"toolhead_ooze_reduction", "toolhead_ooze_reduction", ParamScope::Toolhead},
};

const ParamRow* find_param_row(std::string_view key) {
    for (const auto& row : kParams) {
        if (row.v3 == key) {
            return &row;
        }
    }
    return nullptr;
}

const nlohmann::json* find_member(const nlohmann::json& obj, const std::string& key) {
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

std::string read_string_member(const nlohmann::json& obj, const std::string& key) {
    const auto* v = find_member(obj, key);
    return v && v->is_string() ? v->get<std::string>() : std::string{};
}

} // namespace

MachineLayout read_machine_layout(const nlohmann::json& settings,
                                  const nlohmann::json& live_mmu_machine) {
    MachineLayout layout;
    static const nlohmann::json empty = nlohmann::json::object();
    const auto* config_machine = find_member(settings, "mmu_machine");
    const nlohmann::json& config_mm = config_machine ? *config_machine : empty;

    layout.version = read_string_member(live_mmu_machine, "happy_hare_version");
    if (layout.version.empty()) {
        layout.version = read_string_member(config_mm, "happy_hare_version");
    }
    const auto major = tio::parse_leading<int>(layout.version);
    layout.v4 = major && *major >= 4;
    if (!layout.v4) {
        return layout;
    }

    if (const auto* units = find_member(live_mmu_machine, "num_units")) {
        if (const auto n = ams::read_integer(*units)) {
            layout.num_units = std::max(*n, 1);
        }
    }
    for (int u = 0;; ++u) {
        const auto* unit = find_member(live_mmu_machine, "unit_" + std::to_string(u));
        if (!unit) {
            break;
        }
        if (const auto* bypass = find_member(*unit, "has_bypass"); bypass && bypass->is_boolean()) {
            layout.has_bypass = layout.has_bypass.value_or(false) || bypass->get<bool>();
        }
    }

    // Klipper lowercases section names in configfile.settings; unit and
    // toolhead names keep the case they were configured with.
    std::string unit;
    if (const auto* unit0 = find_member(live_mmu_machine, "unit_0")) {
        unit = read_string_member(*unit0, "name");
    }
    if (unit.empty()) {
        if (const auto* units = find_member(config_mm, "units");
            units && units->is_array() && !units->empty() && (*units)[0].is_string()) {
            unit = (*units)[0].get<std::string>();
        }
    }
    unit = tio::to_lower(unit);
    if (!unit.empty()) {
        layout.unit_params_section = "mmu_unit_parameters " + unit;
        std::string toolhead;
        if (const auto* unit_cfg = find_member(settings, "mmu_unit " + unit)) {
            toolhead = read_string_member(*unit_cfg, "toolhead");
        }
        layout.toolhead_section =
            "mmu_toolhead " + tio::to_lower(toolhead.empty() ? "default" : toolhead);
    }
    return layout;
}

std::string_view param_name(std::string_view key, bool v4) {
    if (!v4) {
        return key;
    }
    const ParamRow* row = find_param_row(key);
    return row ? row->v4 : key;
}

bool param_is_per_unit(std::string_view key) {
    const ParamRow* row = find_param_row(key);
    return row && row->scope != ParamScope::Machine;
}

const nlohmann::json* find_config_param(const nlohmann::json& settings, const MachineLayout& layout,
                                        std::string_view key) {
    if (!layout.v4) {
        const auto* mmu = find_member(settings, "mmu");
        return mmu ? find_member(*mmu, std::string(key)) : nullptr;
    }
    const ParamRow* row = find_param_row(key);
    if (!row || row->v4.empty()) {
        return nullptr;
    }
    std::string section;
    switch (row->scope) {
    case ParamScope::Machine:
        section = "mmu_parameters";
        break;
    case ParamScope::Unit:
        section = layout.unit_params_section;
        break;
    case ParamScope::Toolhead:
        section = layout.toolhead_section;
        break;
    }
    const auto* params = find_member(settings, section);
    return params ? find_member(*params, std::string(row->v4)) : nullptr;
}

std::optional<float> read_config_number(const nlohmann::json* v) {
    if (!v) {
        return std::nullopt;
    }
    if (v->is_number()) {
        return v->get<float>();
    }
    if (v->is_string()) {
        return tio::parse_leading<float>(v->get<std::string>());
    }
    return std::nullopt;
}

std::vector<EntrySensorReading> parse_entry_sensor_objects(const nlohmann::json& params) {
    std::vector<EntrySensorReading> readings;
    if (!params.is_object()) {
        return readings;
    }
    constexpr std::string_view prefix = "filament_switch_sensor mmu_entry_";
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string& key = it.key();
        if (key.rfind(prefix, 0) != 0 || !it.value().is_object()) {
            continue;
        }
        const auto gate = tio::parse_leading<int>(key.substr(prefix.size()));
        if (!gate || *gate < 0) {
            continue;
        }
        EntrySensorReading r;
        r.gate = *gate;
        r.detected = ams::read_field<bool>(it.value(), "filament_detected");
        r.enabled = ams::read_field<bool>(it.value(), "enabled");
        if (r.detected || r.enabled) {
            readings.push_back(r);
        }
    }
    return readings;
}

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
