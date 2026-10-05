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

namespace snapmaker {

constexpr int kToolCount = 4;

/// Everything the backend reads from one status frame, parsed up front.
struct StatusDelta {
    /// extruder, extruder1 .. extruder3; nullopt when the frame has no such
    /// object.
    std::array<std::optional<ExtruderDelta>, kToolCount> extruders;
    /// toolhead.extruder, the carriage's authority on the picked tool.
    std::optional<std::string> toolhead_extruder;
};

[[nodiscard]] ExtruderDelta parse_extruder_delta(const nlohmann::json& extruder);

[[nodiscard]] StatusDelta parse_status(const nlohmann::json& status);

} // namespace snapmaker
} // namespace helix
