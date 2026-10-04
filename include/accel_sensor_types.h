// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace helix::sensors {

/// @brief Role assigned to an accelerometer sensor
enum class AccelSensorRole {
    NONE = 0,         ///< Discovered but not assigned to a role
    INPUT_SHAPER = 1, ///< Used for input shaping calibration
};

/// @brief Type of accelerometer hardware
enum class AccelSensorType {
    ADXL345 = 1,  ///< ADXL345 accelerometer
    LIS2DW = 2,   ///< LIS2DW accelerometer
    LIS3DH = 3,   ///< LIS3DH accelerometer
    MPU9250 = 4,  ///< MPU9250 accelerometer
    ICM20948 = 5, ///< ICM20948 accelerometer
};

/// @brief Configuration for an accelerometer sensor
struct AccelSensorConfig {
    std::string klipper_name; ///< Full Klipper name (e.g., "adxl345", "adxl345 bed")
    std::string sensor_name;  ///< Short name (e.g., "adxl345", "bed")
    AccelSensorType type = AccelSensorType::ADXL345;
    AccelSensorRole role = AccelSensorRole::NONE;
    bool enabled = true;

    AccelSensorConfig() = default;

    AccelSensorConfig(std::string klipper_name_, std::string sensor_name_, AccelSensorType type_)
        : klipper_name(std::move(klipper_name_)), sensor_name(std::move(sensor_name_)),
          type(type_) {}
};

/// @brief Runtime state for an accelerometer sensor
struct AccelSensorState {
    bool available = false; ///< Sensor available in current config
};

/// @brief Convert role enum to config string
/// @param role The role to convert
/// @return Config-safe string for JSON storage
[[nodiscard]] inline std::string accel_role_to_string(AccelSensorRole role) {
    switch (role) {
    case AccelSensorRole::NONE:
        return "none";
    case AccelSensorRole::INPUT_SHAPER:
        return "input_shaper";
    default:
        return "none";
    }
}

/// @brief Parse role string to enum
/// @param str The config string to parse
/// @return Parsed role, or NONE if unrecognized
[[nodiscard]] inline AccelSensorRole accel_role_from_string(const std::string& str) {
    if (str == "input_shaper")
        return AccelSensorRole::INPUT_SHAPER;
    return AccelSensorRole::NONE;
}

/// @brief Convert role to display string
/// @param role The role to convert
/// @return Human-readable role name for UI display
[[nodiscard]] inline std::string accel_role_to_display_string(AccelSensorRole role) {
    switch (role) {
    case AccelSensorRole::NONE:
        return "Unassigned";
    case AccelSensorRole::INPUT_SHAPER:
        return "Input Shaper";
    default:
        return "Unassigned";
    }
}

/// @brief Convert type enum to config string
/// @param type The type to convert
/// @return Config-safe string
[[nodiscard]] inline std::string accel_type_to_string(AccelSensorType type) {
    switch (type) {
    case AccelSensorType::ADXL345:
        return "adxl345";
    case AccelSensorType::LIS2DW:
        return "lis2dw";
    case AccelSensorType::LIS3DH:
        return "lis3dh";
    case AccelSensorType::MPU9250:
        return "mpu9250";
    case AccelSensorType::ICM20948:
        return "icm20948";
    default:
        return "adxl345";
    }
}

/// @brief Parse type string to enum
/// @param str The config string to parse
/// @return Parsed type, defaults to ADXL345 if unrecognized
[[nodiscard]] inline AccelSensorType accel_type_from_string(const std::string& str) {
    if (str == "adxl345")
        return AccelSensorType::ADXL345;
    if (str == "lis2dw")
        return AccelSensorType::LIS2DW;
    if (str == "lis3dh")
        return AccelSensorType::LIS3DH;
    if (str == "mpu9250")
        return AccelSensorType::MPU9250;
    if (str == "icm20948")
        return AccelSensorType::ICM20948;
    return AccelSensorType::ADXL345;
}

} // namespace helix::sensors
