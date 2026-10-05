// src/printer/snapmaker_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_status_parse.h"

#include <algorithm>

namespace helix {

void ExtruderToolState::apply(const ExtruderDelta& delta) {
    if (delta.state) {
        state = *delta.state;
    }
    if (delta.park_pin) {
        park_pin = *delta.park_pin;
    }
    if (delta.active_pin) {
        active_pin = *delta.active_pin;
    }
    if (delta.activating_move) {
        activating_move = *delta.activating_move;
    }
    for (size_t i = 0; i < extruder_offset.size(); ++i) {
        if (delta.extruder_offset[i]) {
            extruder_offset[i] = *delta.extruder_offset[i];
        }
    }
    if (delta.switch_count) {
        switch_count = *delta.switch_count;
    }
    if (delta.retry_count) {
        retry_count = *delta.retry_count;
    }
    if (delta.error_count) {
        error_count = *delta.error_count;
    }
}

namespace snapmaker {

ExtruderDelta parse_extruder_delta(const nlohmann::json& json) {
    ExtruderDelta d;
    if (json.contains("state") && json["state"].is_string()) {
        d.state = json["state"].get<std::string>();
    }
    if (json.contains("park_pin") && json["park_pin"].is_boolean()) {
        d.park_pin = json["park_pin"].get<bool>();
    }
    if (json.contains("active_pin") && json["active_pin"].is_boolean()) {
        d.active_pin = json["active_pin"].get<bool>();
    }
    if (json.contains("activating_move") && json["activating_move"].is_boolean()) {
        d.activating_move = json["activating_move"].get<bool>();
    }
    if (json.contains("extruder_offset") && json["extruder_offset"].is_array()) {
        const auto& arr = json["extruder_offset"];
        for (size_t i = 0; i < std::min(arr.size(), d.extruder_offset.size()); i++) {
            if (arr[i].is_number()) {
                d.extruder_offset[i] = arr[i].get<float>();
            }
        }
    }
    if (json.contains("switch_count") && json["switch_count"].is_number()) {
        d.switch_count = json["switch_count"].get<int>();
    }
    if (json.contains("retry_count") && json["retry_count"].is_number()) {
        d.retry_count = json["retry_count"].get<int>();
    }
    if (json.contains("error_count") && json["error_count"].is_number()) {
        d.error_count = json["error_count"].get<int>();
    }
    return d;
}

StatusDelta parse_status(const nlohmann::json& status) {
    StatusDelta d;

    // Klipper names the first extruder "extruder" and the rest "extruder1"...
    static const char* const extruder_keys[kToolCount] = {"extruder", "extruder1", "extruder2",
                                                          "extruder3"};
    for (int i = 0; i < kToolCount; i++) {
        if (status.contains(extruder_keys[i]) && status[extruder_keys[i]].is_object()) {
            d.extruders[i] = parse_extruder_delta(status[extruder_keys[i]]);
        }
    }

    if (status.contains("toolhead") && status["toolhead"].is_object()) {
        const auto& th = status["toolhead"];
        if (th.contains("extruder") && th["extruder"].is_string()) {
            d.toolhead_extruder = th["extruder"].get<std::string>();
        }
    }

    return d;
}

} // namespace snapmaker
} // namespace helix
