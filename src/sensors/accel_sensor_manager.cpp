// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accel_sensor_manager.h"

#include "json_utils.h"
#include "spdlog/spdlog.h"
#include "static_subject_registry.h"

#include <algorithm>

namespace helix::sensors {

// ============================================================================
// Singleton
// ============================================================================

AccelSensorManager& AccelSensorManager::instance() {
    static AccelSensorManager instance;
    return instance;
}

AccelSensorManager::AccelSensorManager() = default;

AccelSensorManager::~AccelSensorManager() = default;

// ============================================================================
// Discovery, Status and Config
// ============================================================================

void AccelSensorManager::discover_from_config(const nlohmann::json& config_keys) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[AccelSensorManager] Discovering accelerometer sensors from {} config keys",
                  config_keys.size());

    // Clear existing sensors
    sensors_.clear();

    // Iterate over config keys (section names like "adxl345", "adxl345 bed", "lis2dw hotend")
    for (auto it = config_keys.begin(); it != config_keys.end(); ++it) {
        const std::string& config_key = it.key();
        std::string sensor_name;
        AccelSensorType type = AccelSensorType::ADXL345;

        if (!parse_klipper_name(config_key, sensor_name, type)) {
            continue;
        }

        AccelSensorConfig config(config_key, sensor_name, type);
        sensors_.push_back(config);

        // Initialize state if not already present
        if (states_.find(config_key) == states_.end()) {
            AccelSensorState state;
            state.available = true;
            states_[config_key] = state;
        } else {
            states_[config_key].available = true;
        }

        spdlog::debug("[AccelSensorManager] Discovered sensor from config: {} (type: {})",
                      sensor_name, accel_type_to_string(type));
    }

    // Detect Beacon onboard accelerometer
    // Beacon RevH has a LIS2DW that registers as accel chip "beacon"
    if (config_keys.contains("beacon") && config_keys["beacon"].is_object()) {
        const auto& beacon_cfg = config_keys["beacon"];
        if (beacon_cfg.contains("accel_scale") || beacon_cfg.contains("accel_axes_map")) {
            AccelSensorConfig config("beacon", "beacon", AccelSensorType::LIS2DW);
            sensors_.push_back(config);

            if (states_.find("beacon") == states_.end()) {
                AccelSensorState state;
                state.available = true;
                states_["beacon"] = state;
            } else {
                states_["beacon"].available = true;
            }

            spdlog::debug("[AccelSensorManager] Discovered Beacon onboard accelerometer (LIS2DW)");
        }
    }

    // Fallback: detect beacon accelerometer via resonance_tester config
    if (states_.find("beacon") == states_.end() && config_keys.contains("resonance_tester") &&
        config_keys["resonance_tester"].is_object()) {
        const auto& rt_cfg = config_keys["resonance_tester"];
        bool beacon_referenced = false;
        for (const auto& field : {"accel_chip", "accel_chip_x", "accel_chip_y"}) {
            if (rt_cfg.contains(field) && rt_cfg[field].is_string()) {
                const auto& chip = rt_cfg[field].get<std::string>();
                if (chip == "beacon" || chip.rfind("beacon ", 0) == 0) {
                    beacon_referenced = true;
                    break;
                }
            }
        }
        if (beacon_referenced) {
            AccelSensorConfig config("beacon", "beacon", AccelSensorType::LIS2DW);
            sensors_.push_back(config);

            AccelSensorState state;
            state.available = true;
            states_["beacon"] = state;

            spdlog::debug(
                "[AccelSensorManager] Discovered Beacon accelerometer via resonance_tester "
                "reference");
        }
    }

    // Mark sensors that disappeared as unavailable
    for (auto& [name, state] : states_) {
        bool found = false;
        for (const auto& sensor : sensors_) {
            if (sensor.klipper_name == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            state.available = false;
        }
    }

    // Remove stale entries to prevent unbounded memory growth
    for (auto it = states_.begin(); it != states_.end();) {
        if (!it->second.available) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }

    // Update sensor count subject
    if (subjects_initialized_) {
        lv_subject_set_int(&sensor_count_, static_cast<int>(sensors_.size()));
    }

    spdlog::info("[AccelSensorManager] Discovered {} accelerometer sensors from config",
                 sensors_.size());
}

void AccelSensorManager::update_from_status(const nlohmann::json& /*status*/) {
    // Klipper accelerometers have no get_status(), so no status frame ever
    // carries them. Discovery and config are the whole picture.
}

void AccelSensorManager::load_config(const nlohmann::json& config) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[AccelSensorManager] Loading config");

    if (!config.contains("sensors") || !config["sensors"].is_array()) {
        spdlog::debug("[AccelSensorManager] No sensors config found");
        return;
    }

    for (const auto& sensor_json : config["sensors"]) {
        if (!sensor_json.contains("klipper_name")) {
            continue;
        }

        std::string klipper_name = helix::json_util::as_string(sensor_json["klipper_name"]);
        auto* sensor = find_config(klipper_name);

        if (sensor) {
            if (sensor_json.contains("role")) {
                sensor->role =
                    accel_role_from_string(helix::json_util::as_string(sensor_json["role"]));
            }
            if (sensor_json.contains("enabled")) {
                sensor->enabled = helix::json_util::as_bool(sensor_json["enabled"]);
            }
            spdlog::debug("[AccelSensorManager] Loaded config for {}: role={}, enabled={}",
                          klipper_name, accel_role_to_string(sensor->role), sensor->enabled);
        }
    }

    spdlog::info("[AccelSensorManager] Config loaded");
}

nlohmann::json AccelSensorManager::save_config() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    spdlog::debug("[AccelSensorManager] Saving config");

    nlohmann::json config;
    nlohmann::json sensors_array = nlohmann::json::array();

    for (const auto& sensor : sensors_) {
        nlohmann::json sensor_json;
        sensor_json["klipper_name"] = sensor.klipper_name;
        sensor_json["role"] = accel_role_to_string(sensor.role);
        sensor_json["enabled"] = sensor.enabled;
        sensor_json["type"] = accel_type_to_string(sensor.type);
        sensors_array.push_back(sensor_json);
    }

    config["sensors"] = sensors_array;

    spdlog::info("[AccelSensorManager] Config saved");
    return config;
}

// ============================================================================
// Initialization
// ============================================================================

void AccelSensorManager::init_subjects() {
    if (subjects_initialized_) {
        return;
    }

    spdlog::trace("[AccelSensorManager] Initializing subjects");

    // Initialize subjects with SubjectManager for automatic cleanup
    UI_MANAGED_SUBJECT_INT(sensor_count_, 0, "accel_count", subjects_);

    subjects_initialized_ = true;

    // Self-register cleanup — ensures deinit runs before lv_deinit()
    StaticSubjectRegistry::instance().register_deinit(
        "AccelSensorManager", []() { AccelSensorManager::instance().deinit_subjects(); });

    spdlog::trace("[AccelSensorManager] Subjects initialized");
}

void AccelSensorManager::deinit_subjects() {
    if (!subjects_initialized_) {
        return;
    }

    spdlog::trace("[AccelSensorManager] Deinitializing subjects");
    subjects_.deinit_all();
    subjects_initialized_ = false;
    spdlog::trace("[AccelSensorManager] Subjects deinitialized");
}

// ============================================================================
// Sensor Queries
// ============================================================================

bool AccelSensorManager::has_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return !sensors_.empty();
}

std::vector<AccelSensorConfig> AccelSensorManager::get_sensors() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_; // Return thread-safe copy
}

size_t AccelSensorManager::sensor_count() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return sensors_.size();
}

// ============================================================================
// Configuration
// ============================================================================

void AccelSensorManager::set_sensor_role(const std::string& klipper_name, AccelSensorRole role) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // If assigning a role, clear it from any other sensor first
    if (role != AccelSensorRole::NONE) {
        for (auto& sensor : sensors_) {
            if (sensor.role == role && sensor.klipper_name != klipper_name) {
                spdlog::debug("[AccelSensorManager] Clearing role {} from {}",
                              accel_role_to_string(role), sensor.sensor_name);
                sensor.role = AccelSensorRole::NONE;
            }
        }
    }

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->role = role;
        spdlog::info("[AccelSensorManager] Set role for {} to {}", sensor->sensor_name,
                     accel_role_to_string(role));
    }
}

void AccelSensorManager::set_sensor_enabled(const std::string& klipper_name, bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto* sensor = find_config(klipper_name);
    if (sensor) {
        sensor->enabled = enabled;
        spdlog::info("[AccelSensorManager] Set enabled for {} to {}", sensor->sensor_name, enabled);
    }
}

// ============================================================================
// State Queries
// ============================================================================

std::optional<AccelSensorState> AccelSensorManager::get_sensor_state(AccelSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (role == AccelSensorRole::NONE) {
        return std::nullopt;
    }

    const auto* config = find_config_by_role(role);
    if (!config) {
        return std::nullopt;
    }

    auto it = states_.find(config->klipper_name);
    if (it == states_.end()) {
        return std::nullopt;
    }

    return it->second; // Return thread-safe copy
}

bool AccelSensorManager::is_sensor_available(AccelSensorRole role) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (role == AccelSensorRole::NONE) {
        return false;
    }

    const auto* config = find_config_by_role(role);
    if (!config || !config->enabled) {
        return false;
    }

    auto it = states_.find(config->klipper_name);
    return it != states_.end() && it->second.available;
}

// ============================================================================
// LVGL Subjects
// ============================================================================

lv_subject_t* AccelSensorManager::get_sensor_count_subject() {
    return &sensor_count_;
}

// ============================================================================
// Private Helpers
// ============================================================================

bool AccelSensorManager::parse_klipper_name(const std::string& klipper_name,
                                            std::string& sensor_name, AccelSensorType& type) const {
    // Supported accelerometer prefixes
    const std::vector<std::pair<std::string, AccelSensorType>> prefixes = {
        {"adxl345", AccelSensorType::ADXL345},   {"lis2dw", AccelSensorType::LIS2DW},
        {"lis3dh", AccelSensorType::LIS3DH},     {"mpu9250", AccelSensorType::MPU9250},
        {"icm20948", AccelSensorType::ICM20948},
    };

    for (const auto& [prefix, sensor_type] : prefixes) {
        // Check if klipper_name starts with the prefix
        if (klipper_name.rfind(prefix, 0) == 0) {
            // Exact match (e.g., "adxl345")
            if (klipper_name.size() == prefix.size()) {
                sensor_name = prefix;
                type = sensor_type;
                return true;
            }
            // Match with suffix (e.g., "adxl345 bed")
            if (klipper_name.size() > prefix.size() && klipper_name[prefix.size()] == ' ') {
                sensor_name = klipper_name.substr(prefix.size() + 1);
                type = sensor_type;
                return true;
            }
        }
    }

    return false;
}

AccelSensorConfig* AccelSensorManager::find_config(const std::string& klipper_name) {
    for (auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const AccelSensorConfig* AccelSensorManager::find_config(const std::string& klipper_name) const {
    for (const auto& sensor : sensors_) {
        if (sensor.klipper_name == klipper_name) {
            return &sensor;
        }
    }
    return nullptr;
}

const AccelSensorConfig* AccelSensorManager::find_config_by_role(AccelSensorRole role) const {
    for (const auto& sensor : sensors_) {
        if (sensor.role == role) {
            return &sensor;
        }
    }
    return nullptr;
}

} // namespace helix::sensors
