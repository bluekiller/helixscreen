// src/printer/snapmaker_status_parse.cpp
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_status_parse.h"

#include "ams_status_json.h"

#include <algorithm>
#include <optional>

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

SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json) {
    SnapmakerRfidInfo info;

    if (json.contains("MAIN_TYPE") && json["MAIN_TYPE"].is_string()) {
        info.main_type = json["MAIN_TYPE"].get<std::string>();
    }
    if (json.contains("SUB_TYPE") && json["SUB_TYPE"].is_string()) {
        info.sub_type = json["SUB_TYPE"].get<std::string>();
    }
    if (json.contains("MANUFACTURER") && json["MANUFACTURER"].is_string()) {
        info.manufacturer = json["MANUFACTURER"].get<std::string>();
    }
    if (json.contains("VENDOR") && json["VENDOR"].is_string()) {
        info.vendor = json["VENDOR"].get<std::string>();
    }
    if (json.contains("ARGB_COLOR") && json["ARGB_COLOR"].is_number()) {
        // ARGB -> RGB: mask off the alpha byte
        uint32_t argb = json["ARGB_COLOR"].get<uint32_t>();
        info.color_rgb = argb & 0x00FFFFFF;
    }
    if (json.contains("HOTEND_MIN_TEMP") && json["HOTEND_MIN_TEMP"].is_number()) {
        info.hotend_min_temp = json["HOTEND_MIN_TEMP"].get<int>();
    }
    if (json.contains("HOTEND_MAX_TEMP") && json["HOTEND_MAX_TEMP"].is_number()) {
        info.hotend_max_temp = json["HOTEND_MAX_TEMP"].get<int>();
    }
    if (json.contains("BED_TEMP") && json["BED_TEMP"].is_number()) {
        info.bed_temp = json["BED_TEMP"].get<int>();
    }
    if (json.contains("WEIGHT") && json["WEIGHT"].is_number()) {
        info.weight_g = json["WEIGHT"].get<int>();
    }
    // CARD_UID is a 4-byte array like [144, 32, 196, 2]. Canonicalize to a
    // comma-joined string so the override system's baseline comparison is a
    // simple string == string check. Empty / missing array stays as empty
    // string (treated as "no tag / unread" by check_hardware_event_clear).
    if (json.contains("CARD_UID") && json["CARD_UID"].is_array()) {
        const auto& arr = json["CARD_UID"];
        std::string uid;
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_number()) {
                // If any byte isn't a number, bail out — partial UIDs aren't
                // safe to compare. Leave info.uid empty so the check is a
                // no-op for this parse.
                uid.clear();
                break;
            }
            if (!uid.empty())
                uid.push_back(',');
            uid += std::to_string(arr[i].get<int>());
        }
        info.uid = std::move(uid);
    }

    return info;
}

FilamentDetectDelta parse_filament_detect(const nlohmann::json& detect) {
    FilamentDetectDelta d;
    d.info = ams::read_indexed<SnapmakerRfidInfo, kToolCount>(
        detect, "info", [](const nlohmann::json& entry) -> std::optional<SnapmakerRfidInfo> {
            if (!entry.is_object()) {
                return std::nullopt;
            }
            return parse_rfid_info(entry);
        });
    d.state = ams::read_indexed<int, kToolCount>(detect, "state");
    return d;
}

std::vector<FeedChannelDelta> parse_feed_channels(const nlohmann::json& status) {
    std::vector<FeedChannelDelta> channels;
    for (const char* feed_key : {"filament_feed left", "filament_feed right"}) {
        if (!status.contains(feed_key) || !status[feed_key].is_object()) {
            continue;
        }
        const auto& feed = status[feed_key];
        for (int lane = 0; lane < kToolCount; lane++) {
            const std::string ext_key = "extruder" + std::to_string(lane);
            if (!feed.contains(ext_key) || !feed[ext_key].is_object()) {
                continue;
            }
            const auto& ch = feed[ext_key];
            FeedChannelDelta d;
            d.lane = lane;
            // Klipper publishes null for these before a sensor's first
            // reading, which reads as absent rather than as false/empty: a
            // frame that said nothing about filament must not clear the port
            // sensor and drop the slot to EMPTY.
            d.filament_detected = ams::read_field<bool>(ch, "filament_detected");
            d.channel_state = ams::read_field<std::string>(ch, "channel_state");
            d.channel_action_state = ams::read_field<std::string>(ch, "channel_action_state");
            d.channel_error = ams::read_field<std::string>(ch, "channel_error");
            d.module_exist = ams::read_field<bool>(ch, "module_exist");
            d.disable_auto = ams::read_field<bool>(ch, "disable_auto");
            channels.push_back(std::move(d));
        }
    }
    return channels;
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

    if (status.contains("filament_detect") && status["filament_detect"].is_object()) {
        d.filament_detect = parse_filament_detect(status["filament_detect"]);
    }

    d.feed_channels = parse_feed_channels(status);

    return d;
}

} // namespace snapmaker
} // namespace helix
