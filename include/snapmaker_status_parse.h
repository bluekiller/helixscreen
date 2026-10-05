// include/snapmaker_status_parse.h
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "hv/json.hpp"

/**
 * @file snapmaker_status_parse.h
 * @brief The U1's Moonraker status objects, read into plain structs.
 *
 * Moonraker sends deltas: a frame names only the fields that changed, so every
 * parsed field is a std::optional and an omitted one reads as nullopt. A parse
 * never resets state; the backend overlays what a frame carries onto what it
 * holds. Pure: no I/O, no locks, no AmsState.
 */
namespace helix {

/// What one status frame said about one extruder object.
struct ExtruderDelta {
    std::optional<std::string> state; ///< "PARKED", "ACTIVE", "ACTIVATING"
    std::optional<bool> park_pin;
    std::optional<bool> active_pin;
    std::optional<bool> activating_move;
    std::array<std::optional<float>, 3> extruder_offset;
    std::optional<int> switch_count;
    std::optional<int> retry_count;
    std::optional<int> error_count;
};

/// Per-extruder tool state from Snapmaker custom Klipper fields
struct ExtruderToolState {
    std::string state;                                ///< e.g., "PARKED", "ACTIVE", "ACTIVATING"
    bool park_pin = false;                            ///< Tool is in park position
    bool active_pin = false;                          ///< Tool is in active position
    bool activating_move = false;                     ///< Tool change move in progress
    std::array<float, 3> extruder_offset = {0, 0, 0}; ///< XYZ offset
    int switch_count = 0;                             ///< Total tool changes for this extruder
    int retry_count = 0;                              ///< Tool change retries
    int error_count = 0;                              ///< Tool change errors

    /// Overlays the fields @p delta carries; an omitted field keeps its value.
    void apply(const ExtruderDelta& delta);
};

/// RFID tag data parsed from filament_detect info
struct SnapmakerRfidInfo {
    std::string main_type;         ///< e.g., "PLA", "PETG"
    std::string sub_type;          ///< e.g., "SnapSpeed", "Basic"
    std::string manufacturer;      ///< e.g., "Polymaker"
    std::string vendor;            ///< e.g., "Snapmaker"
    uint32_t color_rgb = 0x808080; ///< RGB color (ARGB masked to 0x00FFFFFF)
    int hotend_min_temp = 0;
    int hotend_max_temp = 0;
    int bed_temp = 0;
    int weight_g = 0; ///< Spool weight in grams
    /// Canonical string form of CARD_UID (e.g. "144,32,196,2"). Empty when no
    /// tag is present, the RFID reader is disabled, or the field is missing.
    /// Used by the override system as the hardware-event signal: a change
    /// means the physical spool was swapped.
    std::string uid;
};

namespace snapmaker {

constexpr int kToolCount = 4;

/// The `filament_detect` object: the tag reader's per-channel answer.
struct FilamentDetectDelta {
    /// `info[i]`: the whole tag record for channel i, present when the frame
    /// carried an object for it. A record is a reading, replaced as a whole.
    std::array<std::optional<SnapmakerRfidInfo>, kToolCount> info;
    /// `state[i]`: the entrance/tag reader's raw state for channel i.
    std::array<std::optional<int>, kToolCount> state;
};

/// Everything the backend reads from one status frame, parsed up front.
struct StatusDelta {
    /// extruder, extruder1 .. extruder3; nullopt when the frame has no such
    /// object.
    std::array<std::optional<ExtruderDelta>, kToolCount> extruders;
    /// toolhead.extruder, the carriage's authority on the picked tool.
    std::optional<std::string> toolhead_extruder;
    /// nullopt when the frame has no filament_detect object.
    std::optional<FilamentDetectDelta> filament_detect;
};

[[nodiscard]] ExtruderDelta parse_extruder_delta(const nlohmann::json& extruder);

[[nodiscard]] SnapmakerRfidInfo parse_rfid_info(const nlohmann::json& json);

[[nodiscard]] FilamentDetectDelta parse_filament_detect(const nlohmann::json& detect);

[[nodiscard]] StatusDelta parse_status(const nlohmann::json& status);

} // namespace snapmaker
} // namespace helix
