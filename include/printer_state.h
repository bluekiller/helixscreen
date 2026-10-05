// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui_heater_config.h" // helix::HeaterType

#include "app_macro_activity.h"
#include "app_motion_activity.h"
#include "async_lifetime_guard.h"
#include "capability_overrides.h"
#include "hardware_validator.h"
#include "lvgl/lvgl.h"
#include "print_start_phase.h"
#include "printer_calibration_state.h"
#include "printer_capabilities_state.h"
#include "printer_composite_visibility_state.h"
#include "printer_detector.h"
#include "printer_discovery.h"
#include "printer_excluded_objects_state.h"
#include "printer_fan_state.h"
#include "printer_hardware_validation_state.h"
#include "printer_motion_state.h"
#include "printer_network_state.h"
#include "printer_plugin_status_state.h"
#include "printer_print_state.h"
#include "printer_profile_state.h"
#include "printer_temperature_state.h"
#include "printer_versions_state.h"
#include "spdlog/spdlog.h"
#include "state/subject_macros.h"
#include "subject_managed_panel.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "hv/json.hpp" // libhv's nlohmann json (via cpputil/)

namespace helix {

/**
 * @brief Network connection status states
 */
enum class NetworkStatus {
    DISCONNECTED, ///< No network connection
    CONNECTING,   ///< Connecting to network
    CONNECTED     ///< Connected to network
};

/**
 * @brief Printer connection status states
 */
enum class PrinterStatus {
    DISCONNECTED, ///< Printer not connected
    READY,        ///< Printer connected and ready
    PRINTING,     ///< Printer actively printing
    ERROR         ///< Printer in error state
};

/**
 * @brief Klipper firmware state (klippy_state from Moonraker)
 *
 * Represents the state of the Klipper firmware service, independent of
 * the Moonraker WebSocket connection. When klippy_state is not READY,
 * the printer cannot accept G-code commands even if Moonraker is connected.
 */
enum class KlippyState {
    READY = 0,    ///< Normal operation, printer ready for commands
    STARTUP = 1,  ///< Klipper is starting up (during RESTART/FIRMWARE_RESTART)
    SHUTDOWN = 2, ///< Emergency shutdown (M112)
    ERROR = 3     ///< Klipper error state (check klippy.log)
};

/**
 * @brief Print job state (from Moonraker print_stats.state)
 *
 * Represents the state of the current print job as reported by Klipper/Moonraker.
 * This is the canonical enum for print job state throughout HelixScreen.
 *
 * @note Values are chosen to match the integer representation used internally
 *       by MoonrakerClientMock for backward compatibility.
 */
enum class PrintJobState {
    STANDBY = 0,   ///< No active print, printer idle (Moonraker: "standby")
    PRINTING = 1,  ///< Actively printing (Moonraker: "printing")
    PAUSED = 2,    ///< Print paused (Moonraker: "paused")
    COMPLETE = 3,  ///< Print finished successfully (Moonraker: "complete")
    CANCELLED = 4, ///< Print cancelled by user (Moonraker: "cancelled")
    ERROR = 5      ///< Print failed with error (Moonraker: "error")
};

/**
 * @brief Has the printer taken a job?
 *
 * True for PRINTING and PAUSED: the two wire states reachable only from a job
 * Klipper has accepted. A pause is not a lesser form of idle - the job exists,
 * its clock runs, and `CANCEL_PRINT`/`PAUSE` land on something real.
 *
 * This is the wire-level half of the pair. Ask it when the decision turns on
 * what the PRINTER reports: whether a macro would reach a live job, whether a
 * sibling field like `print_filename` describes a running print, whether a
 * transition to a terminal state ended a real one.
 *
 * @warning NOT the same question as `job_holds_machine(PrintState)`, which also
 *          counts `Preparing`. Preparing is a job the app has committed to and
 *          the printer has not reported, so during a host-side pre-start block
 *          this predicate is false while the toolhead is genuinely moving. Ask
 *          `job_holds_machine()` for "would acting now fight the printer", and
 *          this one for "does the printer hold a job". Several callers depend on
 *          the narrower answer and say so at their call site.
 *
 * The 0/1 subject mirror of this predicate is `print_active`, set from
 * `PrinterPrintState::status_indicates_active_print()`, which asks the same
 * question of a raw status payload and is defined in terms of this function.
 */
constexpr bool printer_has_job(PrintJobState state) {
    return state == PrintJobState::PRINTING || state == PrintJobState::PAUSED;
}

/**
 * @brief Terminal outcome of a print job (for UI persistence)
 *
 * Captures how the last print ended. Unlike PrintJobState (which always reflects
 * the current Moonraker state, including STANDBY after completion), PrintOutcome
 * persists the terminal state until a new print starts.
 *
 * This allows the UI to show "Print Complete!" or "Print Cancelled" badges and
 * Reprint buttons even after Moonraker transitions to STANDBY.
 *
 * @note NONE means either no print has occurred, or we're in the middle of a print.
 */
enum class PrintOutcome {
    NONE = 0,      ///< No completed print (printing, or never printed)
    COMPLETE = 1,  ///< Last print finished successfully
    CANCELLED = 2, ///< Last print was cancelled by user
    ERROR = 3      ///< Last print failed with error
};

/**
 * @brief Parse Moonraker print state string to PrintJobState enum
 *
 * Converts Moonraker's print_stats.state string to the corresponding enum.
 * Unknown strings default to STANDBY.
 *
 * @param state_str Moonraker state string (e.g., "printing", "paused")
 * @return Corresponding PrintJobState enum value
 */
PrintJobState parse_print_job_state(const char* state_str);

/**
 * @brief Convert PrintJobState enum to display string
 *
 * Returns a human-readable string for UI display.
 *
 * @param state PrintJobState enum value
 * @return Display string (e.g., "Printing", "Paused")
 */
const char* print_job_state_to_string(PrintJobState state);

/**
 * @brief Whether a kinematics string names a round-bed machine
 *
 * The value is PrinterDiscovery::kinematics(), read from
 * configfile.config.printer.kinematics because toolhead.kinematics comes
 * back null in the status payload.
 */
constexpr bool circular_bed_kinematics(std::string_view kinematics) {
    return kinematics == "delta" || kinematics == "rotary_delta";
}

/// Whether a kinematics string is a CoreXY whose two diagonals are the two belt
/// paths, which is what the Belt Tension comparison measures. CoreXZ,
/// hybrid_corexy/hybrid_corexz, cartesian, delta and the rest are not.
constexpr bool belt_path_kinematics(std::string_view kinematics) {
    return kinematics == "corexy" || kinematics == "limited_corexy";
}

/// One notify_status_update, unpacked. `status` points into the notification.
struct StatusFrame {
    const json* status = nullptr; ///< params[0], the printer objects that changed
    double eventtime = 0.0;       ///< params[1]; 0 when the frame was synthesized
    bool from_cached_snapshot = false;
};

/**
 * @brief Unpack a Moonraker notification into the frame update_from_status() takes
 *
 * params[1] is Klipper's eventtime. It is monotonic-clock derived, so it survives
 * a Klipper restart and only rewinds on a host reboot: a usable freshness key
 * within one connection. CACHED_SNAPSHOT_MARKER says the frame is a replay of an
 * earlier snapshot rather than current traffic.
 *
 * @return The frame, or nullopt for anything that is not a notify_status_update
 */
std::optional<StatusFrame> parse_status_notification(const json& notification);
/// The frame points into the notification, so a temporary would leave it dangling.
std::optional<StatusFrame> parse_status_notification(json&&) = delete;

/**
 * @brief Printer state manager with LVGL 9 reactive subjects
 *
 * Implements hybrid architecture:
 * - LVGL subjects for UI-bound data (automatic reactive updates)
 * - JSON cache for complex data (file lists, capabilities, metadata)
 *
 * @note Thread Safety: Public setters that update LVGL subjects (set_klipper_version,
 *       set_printer_connection_state, etc.) use queue_update internally to defer updates to the
 *       main thread. This allows safe calls from WebSocket callbacks without risking
 *       "Invalidate area not allowed during rendering" assertions.
 */
class PrinterState {
  public:
    /**
     * @brief Construct printer state manager
     *
     * Initializes internal data structures. Call init_subjects() before
     * creating XML components.
     */
    PrinterState();

    /**
     * @brief Destroy printer state manager
     *
     * Cleans up LVGL subjects and releases resources.
     */
    ~PrinterState();

    /**
     * @brief Initialize all LVGL subjects
     *
     * MUST be called BEFORE creating XML components that bind to these subjects.
     * Can be called multiple times safely - subsequent calls are ignored.
     *
     * @param register_xml If true, registers subjects with LVGL XML system (default).
     *                     Set to false in tests to avoid XML observer creation.
     */
    void init_subjects(bool register_xml = true);

    /**
     * @brief Check if subjects have been initialized
     */
    bool are_subjects_initialized() const {
        return subjects_initialized_;
    }

    /**
     * @brief Deinitialize all subjects across all state components
     *
     * Cascades to all 13 sub-component deinit_subjects() methods and
     * then deinitializes PrinterState's own subjects.
     */
    void deinit_subjects();

    /**
     * @brief Update state from raw status data
     *
     * Updates subjects from a printer status object. Can be called directly
     * with subscription response data or extracted from notifications.
     * This is the core update logic used by both initial state and notifications.
     *
     * @param status Printer status object (e.g., from result.status or params[0])
     * @param eventtime Klipper's monotonic event timestamp (notification params[1]).
     *        0.0 means "no timestamp" — a synthesized status rather than a Klipper
     *        frame. Only the klippy-state freshness guard reads it; every other
     *        field is still last-write-wins.
     * @param from_cached_snapshot true when this payload was captured earlier and is
     *        being replayed (the discovery subscription response). Provenance is
     *        STATED, never inferred from a zero eventtime — the mock client also
     *        dispatches untimestamped status, and those ARE current.
     */
    void update_from_status(const json& status, double eventtime = 0.0,
                            bool from_cached_snapshot = false,
                            std::optional<uint64_t> frame_epoch = std::nullopt);

    //
    // Domain components. Each owns its subjects and the state behind them;
    // reach a domain's subjects and queries through its accessor. Setters the
    // WebSocket thread calls stay on PrinterState, which defers them to the
    // main thread.
    //
    helix::PrinterTemperatureState& temperature_state() {
        return temperature_state_;
    }
    const helix::PrinterTemperatureState& temperature_state() const {
        return temperature_state_;
    }
    helix::PrinterMotionState& motion_state() {
        return motion_state_;
    }
    const helix::PrinterMotionState& motion_state() const {
        return motion_state_;
    }
    helix::PrinterFanState& fan_state() {
        return fan_state_;
    }
    const helix::PrinterFanState& fan_state() const {
        return fan_state_;
    }
    helix::PrinterPrintState& print_state() {
        return print_domain_;
    }
    const helix::PrinterPrintState& print_state() const {
        return print_domain_;
    }
    helix::PrinterCapabilitiesState& capabilities_state() {
        return capabilities_state_;
    }
    const helix::PrinterCapabilitiesState& capabilities_state() const {
        return capabilities_state_;
    }
    helix::PrinterPluginStatusState& plugin_status_state() {
        return plugin_status_state_;
    }
    const helix::PrinterPluginStatusState& plugin_status_state() const {
        return plugin_status_state_;
    }
    helix::PrinterCalibrationState& calibration_state() {
        return calibration_state_;
    }
    const helix::PrinterCalibrationState& calibration_state() const {
        return calibration_state_;
    }
    helix::PrinterHardwareValidationState& hardware_validation_state() {
        return hardware_validation_state_;
    }
    const helix::PrinterHardwareValidationState& hardware_validation_state() const {
        return hardware_validation_state_;
    }
    helix::PrinterCompositeVisibilityState& composite_visibility_state() {
        return composite_visibility_state_;
    }
    const helix::PrinterCompositeVisibilityState& composite_visibility_state() const {
        return composite_visibility_state_;
    }
    helix::PrinterNetworkState& network_state() {
        return network_state_;
    }
    const helix::PrinterNetworkState& network_state() const {
        return network_state_;
    }
    helix::PrinterVersionsState& versions_state() {
        return versions_state_;
    }
    const helix::PrinterVersionsState& versions_state() const {
        return versions_state_;
    }
    helix::PrinterExcludedObjectsState& excluded_objects_state() {
        return excluded_objects_state_;
    }
    const helix::PrinterExcludedObjectsState& excluded_objects_state() const {
        return excluded_objects_state_;
    }
    helix::PrinterProfileState& profile_state() {
        return profile_state_;
    }
    const helix::PrinterProfileState& profile_state() const {
        return profile_state_;
    }

    //
    // Subject accessors for XML binding
    //

    // Temperature subjects (decidegrees: value * 10 for 0.1C resolution)
    // Example: 205.3C is stored as 2053. Divide by 10 for display.
    // Delegated to PrinterTemperatureState component.

    // Active extruder subjects — track whichever extruder is currently active
    lv_subject_t* get_active_extruder_temp_subject() {
        return temperature_state_.get_active_extruder_temp_subject();
    }
    lv_subject_t* get_active_extruder_target_subject() {
        return temperature_state_.get_active_extruder_target_subject();
    }

    /// Duty for one heater, so every surface renders the same number rather
    /// than each mapping heater type to subject on its own.
    lv_subject_t* get_heater_power_subject(helix::HeaterType type) {
        switch (type) {
        case helix::HeaterType::Bed:
            return temperature_state_.get_bed_power_subject();
        case helix::HeaterType::Chamber:
            return temperature_state_.get_chamber_power_subject();
        case helix::HeaterType::Nozzle:
        default:
            return temperature_state_.get_extruder_power_subject();
        }
    }

    // Heater duty cycle, whole percent, -1 until a heater reports one.
    lv_subject_t* get_extruder_power_subject() {
        return temperature_state_.get_extruder_power_subject();
    }
    /// A specific extruder's duty with its lifetime token (use when creating
    /// observers). Distinct from the nullary overload, which is the ACTIVE
    /// extruder's mirror.
    lv_subject_t* get_extruder_power_subject(const std::string& name, SubjectLifetime& lifetime) {
        return temperature_state_.get_extruder_power_subject(name, lifetime);
    }

    // Multi-extruder discovery
    void init_extruders(const std::vector<std::string>& heaters) {
        temperature_state_.init_extruders(heaters);
    }

    /// Re-format the text this state translates as it discovers hardware
    /// (extruder and fan names, hardware-health texts) in the current language.
    void refresh_translated_texts() {
        temperature_state_.refresh_display_names();
        fan_state_.refresh_display_names();
        hardware_validation_state_.refresh_texts();
    }

    // Per-extruder subject access (returns nullptr if not found)
    // Prefer the overloads with SubjectLifetime when creating observers!
    lv_subject_t* get_extruder_temp_subject(const std::string& name) {
        return temperature_state_.get_extruder_temp_subject(name);
    }
    lv_subject_t* get_extruder_target_subject(const std::string& name) {
        return temperature_state_.get_extruder_target_subject(name);
    }
    lv_subject_t* get_extruder_temp_subject(const std::string& name, SubjectLifetime& lifetime) {
        return temperature_state_.get_extruder_temp_subject(name, lifetime);
    }
    lv_subject_t* get_extruder_target_subject(const std::string& name, SubjectLifetime& lifetime) {
        return temperature_state_.get_extruder_target_subject(name, lifetime);
    }

    int extruder_count() const {
        return temperature_state_.extruder_count();
    }

    const std::string& active_extruder_name() const {
        return temperature_state_.active_extruder_name();
    }

    void set_active_extruder(const std::string& name) {
        temperature_state_.set_active_extruder(name);
    }

    // Active extruder's latched last non-zero target (°C); 0 if unknown.
    float get_active_extruder_last_nonzero_target() const {
        return temperature_state_.get_active_extruder_last_nonzero_target();
    }

    // Clear the nozzle load latch (last non-zero target); empty name = active extruder.
    void clear_nozzle_load_latch(const std::string& extruder_name = "") {
        temperature_state_.clear_load_latch(extruder_name);
    }

    lv_subject_t* get_extruder_version_subject() {
        return temperature_state_.get_extruder_version_subject();
    }

    lv_subject_t* get_bed_temp_subject() {
        return temperature_state_.get_bed_temp_subject();
    }
    lv_subject_t* get_bed_temp_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_bed_temp_subject(lifetime);
    }
    lv_subject_t* get_bed_target_subject() {
        return temperature_state_.get_bed_target_subject();
    }
    lv_subject_t* get_bed_target_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_bed_target_subject(lifetime);
    }
    lv_subject_t* get_chamber_temp_subject() {
        return temperature_state_.get_chamber_temp_subject();
    }
    /// A chamber filament-drying cycle is running (0/1).
    lv_subject_t* get_chamber_dryer_active_subject() {
        return temperature_state_.get_chamber_dryer_active_subject();
    }
    lv_subject_t* get_chamber_temp_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_chamber_temp_subject(lifetime);
    }
    lv_subject_t* get_chamber_target_subject() {
        return temperature_state_.get_chamber_target_subject();
    }
    lv_subject_t* get_chamber_target_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_chamber_target_subject(lifetime);
    }
    lv_subject_t* get_chamber_fan_target_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_chamber_fan_target_subject(lifetime);
    }
    lv_subject_t* get_chamber_effective_target_subject() {
        return temperature_state_.get_chamber_effective_target_subject();
    }
    lv_subject_t* get_chamber_effective_target_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_chamber_effective_target_subject(lifetime);
    }
    lv_subject_t* get_chamber_mode_subject() {
        return temperature_state_.get_chamber_mode_subject();
    }
    lv_subject_t* get_chamber_mode_subject(SubjectLifetime& lifetime) {
        return temperature_state_.get_chamber_mode_subject(lifetime);
    }

    // Print progress subjects - delegated to PrinterPrintState component
    lv_subject_t* get_print_progress_subject() {
        return print_domain_.get_print_progress_subject();
    } // 0-100
    lv_subject_t* get_print_progress_display_subject() {
        return print_domain_.get_print_progress_display_subject();
    } // 0-100, frozen after a print ends
    lv_subject_t* get_print_progress_text_subject() {
        return print_domain_.get_print_progress_text_subject();
    } // "N%" for the display value

    // Scheduled-pause markers (prestonbrown/helixscreen#1509) - delegated to
    // PrinterPrintState component. See there for the absent-by-default rule.
    void set_scheduled_pauses(std::vector<helix::gcode::ScheduledPause> pauses,
                              helix::gcode::ProgressAxis axis, const std::string& source_filename) {
        print_domain_.set_scheduled_pauses(std::move(pauses), axis, source_filename);
    }
    [[nodiscard]] const std::vector<helix::gcode::ScheduledPause>& get_scheduled_pauses() const {
        return print_domain_.get_scheduled_pauses();
    }
    [[nodiscard]] helix::gcode::ProgressAxis get_pause_marker_axis() const {
        return print_domain_.get_pause_marker_axis();
    }
    [[nodiscard]] bool pause_markers_match_current_file() const {
        return print_domain_.pause_markers_match_current_file();
    }
    lv_subject_t* get_pause_markers_version_subject() {
        return print_domain_.get_pause_markers_version_subject();
    }
    lv_subject_t* get_print_filename_subject() {
        return print_domain_.get_print_filename_subject();
    }
    lv_subject_t* get_print_state_subject() {
        return print_domain_.get_print_state_subject();
    } // "standby", "printing", "paused", "complete" (string for UI display)

    /**
     * @brief Get print thumbnail path subject for UI binding
     *
     * String subject holding the LVGL path to the current print's thumbnail.
     * Set by PrintStatusPanel when thumbnail loads, cleared when print ends.
     * HomePanel observes this to show the same thumbnail on the print card.
     *
     * @return Pointer to string subject
     */
    lv_subject_t* get_print_thumbnail_path_subject() {
        return print_domain_.get_print_thumbnail_path_subject();
    }

    /**
     * @brief Gcode filename the current thumbnail path was produced for
     *
     * Set before the path subject is published, so an observer of
     * get_print_thumbnail_path_subject() can trust it describes the path it sees.
     *
     * @return Filename, or "" when no thumbnail identity has been set
     */
    [[nodiscard]] const std::string& get_print_thumbnail_file() const {
        return print_domain_.get_print_thumbnail_file();
    }

    /**
     * @brief The canonical name of the print currently being shown
     *
     * The single answer to "which print is this". See
     * PrinterPrintState::get_effective_print_filename() for why it exists and
     * why it is published before the raw filename subject.
     */
    [[nodiscard]] const std::string& get_effective_print_filename() const {
        return print_domain_.get_effective_print_filename();
    }

    /// Name this print by something other than what print_stats reports.
    /// See PrinterPrintState::set_print_identity_override().
    void set_print_identity_override(const std::string& name) {
        print_domain_.set_print_identity_override(name);
    }

    /// Drop the identity override and re-derive from the reported filename.
    void clear_print_identity_override() {
        print_domain_.clear_print_identity_override();
    }

    /// What is overriding this print's reported name, or "" when nothing is.
    [[nodiscard]] const std::string& get_print_identity_override() const {
        return print_domain_.get_print_identity_override();
    }

    /// Bumped whenever get_effective_print_filename() actually changes. See
    /// PrinterPrintState::get_print_identity_epoch_subject().
    lv_subject_t* get_print_identity_epoch_subject() {
        return print_domain_.get_print_identity_epoch_subject();
    }

    /**
     * @brief Set the current print's thumbnail, tagged with the file it is for
     *
     * The path alone carries no identity, so consumers cannot tell a fresh
     * thumbnail from a previous job's. Pairing it with @p for_file makes
     * staleness decidable. Main thread only — publishing fires observers.
     *
     * @param for_file Gcode filename this path was produced for ("" to clear identity)
     * @param path LVGL-compatible path (e.g., "A:/tmp/thumbnail_xxx.bin"), "" to clear
     */
    void set_print_thumbnail(const std::string& for_file, const std::string& path);

#if defined(HELIX_PLATFORM_ESP32)
    /**
     * @brief Get the PSRAM thumbnail generation subject for UI binding
     *
     * Integer subject bumped whenever the current print's PSRAM-resident
     * thumbnail is replaced. ESP32 has no disk thumbnail cache, so
     * print_thumbnail_path stays empty there and consumers observe this
     * counter, then read get_print_psram_thumbnail().
     */
    lv_subject_t* get_print_psram_thumb_gen_subject() {
        return print_domain_.get_print_psram_thumb_gen_subject();
    }

    /**
     * @brief Get the current print's PSRAM-resident thumbnail (may be nullptr)
     *
     * Main thread only. Hold the returned shared_ptr for as long as a widget's
     * image src points at its descriptor.
     */
    [[nodiscard]] std::shared_ptr<helix::ui::EspPsramThumbnail> get_print_psram_thumbnail() const {
        return print_domain_.get_print_psram_thumbnail();
    }

    /**
     * @brief Install the current print's PSRAM-resident thumbnail
     *
     * Main thread only — bumps the generation subject and may destroy the
     * previous thumbnail (which calls lv_image_cache_drop()).
     */
    void set_print_psram_thumbnail(std::shared_ptr<helix::ui::EspPsramThumbnail> thumb);
#endif

    /**
     * @brief Get print job state enum subject
     *
     * Integer subject holding PrintJobState enum value for type-safe comparisons.
     * Use this for logic, use get_print_state_subject() for UI display binding.
     *
     * @return Pointer to integer subject (cast value to PrintJobState)
     */
    lv_subject_t* get_print_state_enum_subject() {
        return print_domain_.get_print_state_enum_subject();
    }

    /**
     * @brief Lifetime token for the "static" print subjects.
     *
     * Cross-singleton observers (e.g. AmsState's print-state observer) MUST
     * pass this token to `observe<int>(...)` — otherwise an ObserverGuard
     * outliving a `deinit_subjects()` cycle in tests will UAF in
     * `lv_observer_remove()`.
     */
    [[nodiscard]] SubjectLifetime get_static_print_subjects_lifetime() const {
        return print_domain_.get_static_subjects_lifetime();
    }

    /**
     * @brief Lifetime token covering EVERY subject reachable through PrinterState.
     *
     * The per-domain tokens above are narrower — one component's subjects, or a
     * single dynamic subject. This one is flipped false at the top of
     * `deinit_subjects()`, before any component tears its subjects down, so it
     * covers the whole tree: temperature, motion, fan, LED, print, capabilities,
     * excluded objects, and PrinterState's own.
     *
     * Any observer whose owner can outlive a `deinit_subjects()` cycle MUST pass
     * this (or a narrower token for the same subject) to `observe_*`. Panels held
     * in process-lifetime singletons are exactly that case: `deinit_subjects()`
     * runs `lv_subject_deinit()`, which frees every observer node, and an
     * ObserverGuard that never learned the subject died then calls
     * `lv_observer_remove()` on freed memory the next time it resets.
     *
     * Prefer the narrower per-subject token where one exists (dynamic per-fan and
     * per-extruder subjects die independently of a full deinit); use this for the
     * static subjects that only die with the whole PrinterState.
     */
    [[nodiscard]] SubjectLifetime get_subjects_lifetime() const {
        return subjects_.get_subjects_lifetime();
    }

    /**
     * @brief Get print active subject for UI binding
     *
     * Integer subject: 1 when PRINTING or PAUSED, 0 otherwise.
     * Derived from print_state_enum for simpler XML bindings (avoids OR logic).
     * Use for card visibility that should show during any active print.
     *
     * @return Pointer to integer subject (0 or 1)
     */
    lv_subject_t* get_print_active_subject() {
        return print_domain_.get_print_active_subject();
    }

    /**
     * @brief Get print outcome subject for UI binding
     *
     * Integer subject holding PrintOutcome enum value for terminal print state.
     * Unlike print_state_enum (which reflects live Moonraker state), print_outcome
     * persists how the last print ended until a new print starts.
     *
     * Use this for showing completion/cancellation UI (badges, reprint buttons)
     * that should persist after Moonraker transitions back to STANDBY.
     *
     * @return Pointer to integer subject (cast value to PrintOutcome)
     */
    lv_subject_t* get_print_outcome_subject() {
        return print_domain_.get_print_outcome_subject();
    }

    /**
     * @brief Set print outcome for UI badge display
     *
     * Call this to manually set the print outcome (e.g., from AbortManager
     * when Moonraker reports "standby" instead of "cancelled" after M112).
     *
     * @param outcome The print outcome value to set
     */
    void set_print_outcome(PrintOutcome outcome);

    /**
     * @brief Get subject for showing print progress card on home panel
     *
     * Combined subject: 1 when print_active==1 AND print_start_phase==0.
     * Simplifies XML bindings by avoiding conflicting multi-binding logic.
     *
     * @return Pointer to integer subject (0 or 1)
     */
    lv_subject_t* get_print_show_progress_subject() {
        return print_domain_.get_print_show_progress_subject();
    }

    /**
     * @brief Get subject for display-ready print filename
     *
     * Clean filename without path or .helix_temp prefix, suitable for UI display.
     * Set by PrintStatusPanel when processing raw print_filename.
     *
     * @return Pointer to string subject
     */
    lv_subject_t* get_print_display_filename_subject() {
        return print_domain_.get_print_display_filename_subject();
    }

    /**
     * @brief Set display-ready print filename for UI binding
     *
     * Called by PrintStatusPanel after cleaning up the raw filename.
     *
     * @param name Clean display name (e.g., "Body1" not ".helix_temp/modified_123_Body1.gcode")
     */
    void set_print_display_filename(const std::string& name);

    /**
     * @brief Get current print job state as enum
     *
     * Convenience method for direct enum access without subject lookup.
     *
     * @return Current PrintJobState
     */
    PrintJobState get_print_job_state() const;

    /// The derived print lifecycle. Prefer this over reading
    /// get_print_lifecycle_subject() by hand — see
    /// PrinterPrintState::get_print_lifecycle() for why the hand-cast is a trap.
    [[nodiscard]] PrintState get_print_lifecycle() const {
        return print_domain_.get_print_lifecycle();
    }

    /**
     * @brief Check if a new print can be started
     *
     * Returns true if the printer is in a state that allows starting a new print.
     * A print can be started when the printer is idle (STANDBY), a previous print
     * finished (COMPLETE, CANCELLED), or the printer recovered from an error (ERROR).
     * Also checks that no print workflow is currently in progress (e.g., G-code
     * downloading/modifying/uploading).
     *
     * @return true if start_print() can be called safely
     */
    [[nodiscard]] bool can_start_new_print() const;

    /**
     * @brief Set the print-in-progress flag (UI workflow state)
     *
     * Call with true when starting the print preparation workflow
     * (downloading/modifying/uploading G-code), and false when complete.
     * This flag is checked by can_start_new_print() to prevent:
     * - Double-tap issues during long G-code modification workflows
     * - UI elements from indicating "ready to print" during preparation
     * - Race conditions from concurrent print requests
     *
     * Updates the print_in_progress_ subject so UI observers can react.
     *
     * Thread-safe: Uses helix::ui::queue_update() to defer LVGL subject updates
     * to the main thread. Can be safely called from WebSocket callbacks.
     */
    void set_print_in_progress(bool in_progress);

    /**
     * @brief Check if a print workflow is currently in progress
     *
     * Returns true during print preparation (G-code download/modify/upload),
     * even though the printer's physical state may still be STANDBY.
     */
    [[nodiscard]] bool is_print_in_progress() const {
        return print_domain_.is_print_in_progress();
    }

    /**
     * @brief True when Klipper's virtual_sdcard.is_active is reporting
     * active gcode playback.
     *
     * Distinct from PrintJobState — a `paused` print can have
     * is_active=false (Snapmaker U1 dirty-bed exception). Used by
     * AmsBackend::prepare_for_resume to detect terminated-with-exception
     * state and switch to "Restart from beginning?" UX.
     */
    [[nodiscard]] bool is_sdcard_active() const {
        return print_domain_.is_sdcard_active();
    }

    /**
     * @brief virtual_sdcard.pl_env_valid — Snapmaker-fork Power-Loss-Recovery flag.
     *
     * Delegated to PrinterPrintState; see its accessor docs.
     */
    lv_subject_t* get_pl_env_valid_subject() {
        return print_domain_.get_pl_env_valid_subject();
    }

    [[nodiscard]] bool is_pl_env_valid() const {
        return print_domain_.is_pl_env_valid();
    }

    /**
     * @brief virtual_sdcard.file_path — the file a PLR restore would resume.
     * Only meaningful when is_pl_env_valid() is true.
     */
    [[nodiscard]] const std::string& pl_recovery_file() const {
        return print_domain_.pl_recovery_file();
    }

    /// Clear the cached PLR recovery file path. Delegated to PrinterPrintState;
    /// see its accessor docs. Main-thread only.
    /**
     * @brief print_stats.power_loss presence — Creality-fork PLR capability.
     *
     * See PrinterPrintState::get_plr_power_loss_signal_subject().
     */
    lv_subject_t* get_plr_power_loss_signal_subject() {
        return print_domain_.get_plr_power_loss_signal_subject();
    }

    /// True when print_stats.power_loss has been seen (Creality Klipper fork).
    [[nodiscard]] bool is_plr_power_loss_signal() const {
        return print_domain_.is_plr_power_loss_signal();
    }

    /**
     * @brief PLR passive backend: discovered resume-macro capability and the
     * live interrupted flag.
     *
     * See PrinterPrintState::get_plr_resume_macro_subject() for the semantics
     * (booleans only; delta frames without the key leave the flag alone).
     */
    lv_subject_t* get_plr_resume_macro_subject() {
        return print_domain_.get_plr_resume_macro_subject();
    }

    [[nodiscard]] bool is_plr_resume_macro_present() const {
        return print_domain_.is_plr_resume_macro_present();
    }

    lv_subject_t* get_plr_interrupted_flag_subject() {
        return print_domain_.get_plr_interrupted_flag_subject();
    }

    [[nodiscard]] bool is_plr_interrupted_flag() const {
        return print_domain_.is_plr_interrupted_flag();
    }

    void clear_pl_recovery_file() {
        print_domain_.clear_pl_recovery_file();
    }

    /**
     * @brief True when Klipper's pause_resume.is_paused is set.
     *
     * Reflects the last received pause_resume.is_paused value from Moonraker.
     * Safe to read synchronously from the main thread.
     */
    [[nodiscard]] bool is_paused() const {
        return is_paused_;
    }

    /**
     * @brief Reset UI state when starting a new print
     *
     * Clears the print_complete flag and resets progress to prepare for
     * a new print. Call this BEFORE navigating to print status panel.
     */
    void reset_for_new_print();

    /**
     * @brief Get the print-in-progress subject for observing workflow state
     *
     * Value is 1 when print preparation is in progress, 0 otherwise.
     */
    lv_subject_t* get_print_in_progress_subject() {
        return print_domain_.get_print_in_progress_subject();
    }

    // Filament used subject (from print_stats.filament_used, in mm)
    // Delegated to PrinterPrintState component
    lv_subject_t* get_print_filament_used_subject() {
        return print_domain_.get_print_filament_used_subject();
    }

    /**
     * @brief Per-extruder filament_used (mm), 0-based.
     *
     * Dynamic subject — observers MUST capture the returned lifetime token and
     * subscribe via observe<int>(..., lifetime). See
     * PrinterPrintState::get_extruder_filament_used_subject for full contract.
     *
     * @param extruder_idx 0-based extruder index (0 = "extruder", 1 = "extruder1", ...)
     * @param[out] lifetime Token whose expiration signals subject death
     * @return Non-null subject pointer (created lazily on first access).
     */
    lv_subject_t* get_extruder_filament_used_subject(int extruder_idx, SubjectLifetime& lifetime) {
        return print_domain_.get_extruder_filament_used_subject(extruder_idx, lifetime);
    }

    // Layer tracking subjects (from print_stats.info.current_layer/total_layer)
    // Delegated to PrinterPrintState component
    lv_subject_t* get_print_layer_current_subject() {
        return print_domain_.get_print_layer_current_subject();
    }
    lv_subject_t* get_print_layer_total_subject() {
        return print_domain_.get_print_layer_total_subject();
    }

    /**
     * @brief Set total layer count from file metadata
     *
     * Called when print starts to initialize total layers from file metadata.
     * Moonraker notifications may update this later via SET_PRINT_STATS_INFO.
     */
    void set_print_layer_total(int total) {
        print_domain_.set_print_layer_total(total);
    }

    /**
     * @brief Set slice layer heights from file metadata (for Z-height derivation)
     *
     * Enables the Z-height current-layer fallback for printers whose slicer never
     * reports a layer number. Thread-safe (marshals internally).
     */
    void set_print_layer_heights(double layer_height, double first_layer_height) {
        print_domain_.set_print_layer_heights(layer_height, first_layer_height);
    }

    /**
     * @brief Set current layer number (gcode response fallback)
     *
     * Thread-safe. Called from gcode response parser when
     * print_stats.info.current_layer doesn't fire.
     */
    void set_print_layer_current(int layer) {
        print_domain_.set_print_layer_current(layer);
    }

    /**
     * @brief Check if real layer data has been received from slicer/Moonraker.
     * When false, layer count is estimated from print progress.
     */
    bool has_real_layer_data() const {
        return print_domain_.has_real_layer_data();
    }

    /**
     * @brief Is the displayed current layer trustworthy (not a progress guess)?
     *
     * True for real slicer/Moonraker layer fields AND for Z-height-derived
     * layers; false only for the progress-fraction estimate. The print-status
     * label uses this to decide whether to show the "~" estimate prefix.
     */
    bool layer_is_accurate() const {
        return print_domain_.layer_is_accurate();
    }

    /**
     * @brief Sticky: has this printer EVER reported a real layer field this session?
     *
     * Delegated to PrinterPrintState. NOT reset between prints. Used by the
     * pre-print completion gate (MoonrakerManager::should_complete_preprint) to
     * pick the real-first-layer path vs the print_duration fallback.
     */
    bool printer_reports_layers() const {
        return print_domain_.printer_reports_layers();
    }

    /**
     * @brief Set slicer's estimated total print time (from file metadata)
     *
     * Used as fallback for remaining time when print_duration is still 0.
     */
    void set_estimated_print_time(int seconds) {
        print_domain_.set_estimated_print_time(seconds);
    }

    /**
     * @brief Get slicer's estimated total print time
     */
    int get_estimated_print_time() const {
        return print_domain_.get_estimated_print_time();
    }

    // Print time tracking subjects (in seconds) - delegated to PrinterPrintState
    lv_subject_t* get_print_duration_subject() {
        return print_domain_.get_print_duration_subject();
    }
    lv_subject_t* get_print_elapsed_subject() {
        return print_domain_.get_print_elapsed_subject();
    }
    lv_subject_t* get_print_time_left_subject() {
        return print_domain_.get_print_time_left_subject();
    }

    // ========================================================================
    // PRINT START PROGRESS (detected from G-code response during PRINT_START)
    // ========================================================================

    /**
     * @brief Get print start phase subject for UI binding
     *
     * Integer subject holding PrintStartPhase enum value.
     * Use with bind_flag_if_eq/not_eq in XML to show/hide progress overlay.
     */
    void begin_preparing(const PrintJobRef& job) {
        print_domain_.begin_preparing(job);
    }
    void retire_preparing(PreparingExit reason) {
        print_domain_.retire_preparing(reason);
    }
    [[nodiscard]] bool has_preparing_job() const {
        return print_domain_.has_preparing_job();
    }
    lv_subject_t* get_preparing_epoch_subject() {
        return print_domain_.get_preparing_epoch_subject();
    }
    lv_subject_t* get_print_lifecycle_prev_subject() {
        return print_domain_.get_print_lifecycle_prev_subject();
    }
    [[nodiscard]] PreparingExit last_preparing_exit() const {
        return print_domain_.last_preparing_exit();
    }

    lv_subject_t* get_print_lifecycle_subject() {
        return print_domain_.get_print_lifecycle_subject();
    }

    /// Boolean form of job_holds_machine(print_lifecycle). See
    /// PrinterPrintState::get_job_holds_machine_subject().
    lv_subject_t* get_job_holds_machine_subject() {
        return print_domain_.get_job_holds_machine_subject();
    }

    /// job_holds_machine || spools on the bed. See
    /// PrinterPrintState::get_machine_motion_blocked_subject().
    lv_subject_t* get_machine_motion_blocked_subject() {
        return print_domain_.get_machine_motion_blocked_subject();
    }
    void set_spool_latch(bool on, std::vector<std::string> extra_tokens = {}) {
        print_domain_.set_spool_latch(on, std::move(extra_tokens));
    }
    [[nodiscard]] bool spool_latch_active() const {
        return print_domain_.spool_latch_active();
    }
    [[nodiscard]] std::vector<std::string> spool_latch_extra_tokens() const {
        return print_domain_.spool_latch_extra_tokens();
    }

    lv_subject_t* get_print_start_phase_subject() {
        return print_domain_.get_print_start_phase_subject();
    }

    /**
     * @brief Get print start message subject for UI binding
     *
     * String subject with human-readable phase description (e.g., "Heating Nozzle...").
     * Use with bind_text in XML.
     */
    lv_subject_t* get_print_start_message_subject() {
        return print_domain_.get_print_start_message_subject();
    }

    /**
     * @brief Get print start progress subject for UI binding
     *
     * Integer subject with 0-100% progress based on weighted phase completion.
     * Use with bind_value on lv_bar in XML.
     */
    lv_subject_t* get_print_start_progress_subject() {
        return print_domain_.get_print_start_progress_subject();
    }

    /**
     * @brief Set predicted pre-print time remaining (main-thread only)
     */
    void set_print_start_time_left(const char* text) {
        print_domain_.set_print_start_time_left(text);
    }

    /**
     * @brief Clear predicted pre-print time remaining
     */
    void clear_print_start_time_left() {
        print_domain_.clear_print_start_time_left();
    }

    /**
     * @brief Get pre-print remaining seconds subject for augmenting total remaining
     */
    lv_subject_t* get_preprint_remaining_subject() {
        return print_domain_.get_preprint_remaining_subject();
    }

    /**
     * @brief Set pre-print remaining seconds (main-thread only)
     */
    void set_preprint_remaining_seconds(int seconds) {
        print_domain_.set_preprint_remaining_seconds(seconds);
    }

    /**
     * @brief Get pre-print elapsed seconds subject
     */
    lv_subject_t* get_preprint_elapsed_subject() {
        return print_domain_.get_preprint_elapsed_subject();
    }

    /**
     * @brief Set pre-print elapsed seconds (main-thread only)
     */
    void set_preprint_elapsed_seconds(int seconds) {
        print_domain_.set_preprint_elapsed_seconds(seconds);
    }

    /// Klipper display message from M117 / display_status.message
    lv_subject_t* get_display_message_subject() {
        return print_domain_.get_display_message_subject();
    }

    /// 1 when display_message is non-empty, 0 when empty
    lv_subject_t* get_display_message_visible_subject() {
        return print_domain_.get_display_message_visible_subject();
    }

    /// Klipper print_stats.message — pause/error reason from firmware
    lv_subject_t* get_print_message_subject() {
        return print_domain_.get_print_message_subject();
    }

    /// print_stats.exception id (Snapmaker U1 structured pause descriptor), -1
    /// when no exception is latched. See PrinterPrintState getter docs (#991).
    [[nodiscard]] int get_print_exception_id() const {
        return print_domain_.get_print_exception_id();
    }

    /// print_stats.exception code, -1 when absent.
    [[nodiscard]] int get_print_exception_code() const {
        return print_domain_.get_print_exception_code();
    }

    /// print_stats.exception message — pause reason text (empty when no exception).
    [[nodiscard]] const std::string& get_print_exception_message() const {
        return print_domain_.get_print_exception_message();
    }

    /**
     * @brief Check if currently in print start phase
     *
     * Convenience method to check if we're showing PRINT_START progress.
     *
     * @return true if phase is not IDLE
     */
    bool is_in_print_start() const;

    /**
     * @brief Set print start phase and update message/progress
     *
     * Called by PrintStartCollector when phases are detected.
     * Updates all three subjects: phase, message, and progress.
     *
     * @param phase Current PrintStartPhase
     * @param message Human-readable message (e.g., "Heating Nozzle...")
     * @param progress Estimated progress 0-100%
     */
    void set_print_start_state(PrintStartPhase phase, const char* message, int progress);

    /**
     * @brief Reset print start to IDLE
     *
     * Called when print initialization completes or print is cancelled.
     */
    void reset_print_start_state();

    // Note: Derived subjects (xy_homed, z_homed, all_homed) are panel-local in ControlsPanel

    // ========================================================================
    // MULTI-FAN API - Delegated to PrinterFanState component
    // ========================================================================

    /**
     * @brief Firmware-persisted Z-offset in microns, or nullopt when unknown
     *
     * Convenience wrapper over the two subjects above for the display/adjust
     * helpers in helix::zoffset.
     */
    std::optional<int> get_persisted_z_offset_microns() {
        if (lv_subject_get_int(motion_state_.get_persisted_z_offset_valid_subject()) == 0) {
            return std::nullopt;
        }
        return lv_subject_get_int(motion_state_.get_persisted_z_offset_subject());
    }

    // ========================================================================
    // PENDING Z-OFFSET DELTA (for tracking adjustments made during print)
    // Delegated to PrinterMotionState component.
    // ========================================================================

    /**
     * @brief Get subject indicating whether Z-offset can be manually saved
     *
     * Returns 1 when the printer's Z-offset calibration strategy requires
     * HelixScreen to save (PROBE_CALIBRATE or ENDSTOP), 0 when the
     * firmware/macros handle persistence automatically (FIRMWARE_MANAGED).
     * Used in XML to hide the "Save Z-Offset" button for auto-saved printers.
     */
    lv_subject_t* get_z_offset_can_save_subject() {
        return profile_state_.get_z_offset_can_save_subject();
    }

    /**
     * @brief Set printer connection state (Moonraker WebSocket)
     *
     * Updates both printer_connection_state and printer_connection_message subjects.
     * Called by main.cpp WebSocket callbacks.
     *
     * @param state 0=disconnected, 1=connecting, 2=connected, 3=reconnecting, 4=failed
     * @param message Status message ("Connecting...", "Ready", "Disconnected", etc.)
     */
    void set_printer_connection_state(int state, const char* message);

    /// Remote-screen verdict from the live websocket endpoint (thread-safe;
    /// defers the subject write to the main thread). Published by
    /// MoonrakerManager on CONNECTED edges.
    void set_moonraker_is_remote(bool remote);

    /// Main-thread read of moonraker_is_remote (true = connected Moonraker is
    /// not this host). For UI decision points; background code uses
    /// helix::is_moonraker_on_same_host() directly.
    bool is_moonraker_remote();

    /**
     * @brief Set Klipper firmware state (thread-safe, async)
     *
     * Updates klippy_state subject via queue_update to ensure thread safety.
     * Called when Moonraker sends klippy state notifications from WebSocket
     * callbacks (notify_klippy_ready, notify_klippy_disconnected).
     *
     * @param state KlippyState enum value
     */
    void set_klippy_state(KlippyState state);

    /**
     * @brief Set Klipper firmware state (synchronous, main-thread only)
     *
     * Directly updates klippy_state subject without async deferral.
     * Only call this from the main LVGL thread. Use for testing or when
     * already on the main thread.
     *
     * @param state KlippyState enum value
     */
    void set_klippy_state_sync(KlippyState state);

    /**
     * @brief Seed Klipper firmware state, but never override a live value
     *
     * For startup-only sources that describe the printer as it was when the
     * request was issued — `printer.info`'s `state` field, whose response can
     * land seconds after the WebSocket has already reported a newer state.
     * No-ops once any live source (a webhooks frame carrying an eventtime, or a
     * notify_klippy_* message) has set the state.
     *
     * @param state KlippyState enum value
     */
    void set_klippy_state_if_unseeded(KlippyState state);

    /**
     * @brief Update printer capability subjects from PrinterDiscovery
     *
     * Updates subjects that control visibility of pre-print option checkboxes.
     * Applies user-configured overrides from settings.json before updating subjects.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param hardware PrinterDiscovery populated from printer.objects.list
     */
    void set_hardware(helix::PrinterDiscovery hardware);

    /**
     * @brief Set Klipper software version from printer.info
     *
     * Updates klipper_version_ subject for Settings panel About section.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param version Version string (e.g., "v0.12.0-108-g2c7a9d58")
     */
    void set_klipper_version(const std::string& version);

    /**
     * @brief Set Moonraker software version from server.info
     *
     * Updates moonraker_version_ subject for Settings panel About section.
     * Called by main.cpp after MoonrakerClient::discover_printer() completes.
     *
     * @param version Version string (e.g., "v0.8.0-143-g2c7a9d58")
     */
    void set_moonraker_version(const std::string& version);

    /**
     * @brief Set OS version from machine.system_info
     *
     * Updates os_version_ subject for Settings panel About section.
     * Called after MoonrakerClient::discover_printer() completes.
     *
     * @param version OS distribution name (e.g., "Forge-X 1.4.0")
     */
    void set_os_version(const std::string& version);

    /**
     * @brief Get the capability overrides for external access
     *
     * Allows other components to check effective capability availability
     * with user overrides applied.
     *
     * @return Reference to the CapabilityOverrides instance
     */
    [[nodiscard]] const CapabilityOverrides& get_capability_overrides() const {
        return capability_overrides_;
    }

    /**
     * @brief Get cached hardware discovery result
     *
     * Provides access to the full list of heaters and sensors discovered
     * during hardware enumeration. Used by the chamber assignment UI to
     * populate dropdown options.
     *
     * @return Reference to the cached PrinterDiscovery instance
     */
    [[nodiscard]] const helix::PrinterDiscovery& get_discovery() const {
        return discovery_;
    }

    /**
     * @brief Check if Spoolman is available
     *
     * Reads the printer_has_spoolman subject value. Safe to call from any thread
     * (reads a single int).
     */
    bool is_spoolman_available() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasSpoolman)) == 1;
    }

    /**
     * @brief Check if Moonraker's job_queue component is present
     *
     * Reads the printer_has_job_queue subject value. Safe to call from any
     * thread (reads a single int).
     */
    bool is_job_queue_available() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasJobQueue)) == 1;
    }

    /// True if at least one enabled webcam has been detected
    bool has_webcam() const {
        return lv_subject_get_int(capabilities_state_.subject(Capability::HasWebcam)) == 1;
    }

    /**
     * @brief Set timelapse plugin availability status
     *
     * Called after verifying the moonraker-timelapse plugin is installed.
     * Updates printer_has_timelapse subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param available True if moonraker-timelapse plugin is installed and responding
     */
    void set_timelapse_available(bool available);

    /**
     * @brief Seed the default state of the synthesized timelapse pre-print option
     *
     * The moonraker-timelapse plugin has no per-print concept — the pre-print
     * toggle writes the GLOBAL `enabled` setting at print start. This seeds the
     * toggle's default from that global setting so a user who enabled timelapse
     * globally isn't silently disabled by starting a print without touching the
     * toggle (#1094). Fetched at discovery via machine.timelapse.get_settings.
     *
     * Thread-safe: Can be called from any thread, defers to the main thread and
     * re-synthesizes the option set there.
     *
     * @param enabled Global moonraker-timelapse `enabled` value
     */
    void set_timelapse_default_enabled(bool enabled);

    /**
     * @brief Set HelixPrint plugin installation status
     *
     * Called after checking Moonraker for the helix_print plugin.
     * Updates helix_plugin_installed_ subject for UI visibility gating.
     *
     * Thread-safe: Can be called from any thread, defers LVGL update to main thread.
     *
     * @param installed True if HelixPrint plugin is installed
     */
    void set_helix_plugin_installed(bool installed);

    // === Visibility Subject Getters (pre-print options card aggregate) ===

    /**
     * @brief Set printer kinematics type and update has_individual_xyz_homing and
     *        bed_moves subjects.
     *
     * Updates printer_has_individual_xyz_homing_ and printer_bed_moves_ subjects
     * based on kinematics type:
     *
     * - Deltas cannot home XYZ axes individually.
     * - CoreXY printers typically have bed moving on Z (Voron 2.4, RatRig).
     * - Cartesian/Delta printers typically have gantry moving on Z (Ender 3, Prusa).
     *
     * @param kinematics Kinematics type string from toolhead config
     */
    void set_kinematics(const std::string& kinematics);

    /**
     * @brief Apply effective bed_moves value based on Z movement style override
     *
     * Reads ZMovementStyle from SettingsManager and applies:
     * - AUTO: uses auto_detected_bed_moves_ from kinematics detection
     * - BED_MOVES: forces printer_bed_moves = true
     * - NOZZLE_MOVES: forces printer_bed_moves = false
     *
     * Called from set_kinematics() and SettingsManager::set_z_movement_style().
     */
    void apply_effective_bed_moves();

    /**
     * @brief Resolve printer_is_enclosed and printer_can_bed_dry
     *        (prestonbrown/helixscreen#1730)
     *
     * Inputs: the printer database's enclosed flag, the enclosure override, a
     * configured chamber heater, a heated bed and the Z travel. Called wherever
     * one of those changes; writes only on change.
     */
    void refresh_bed_drying_capability();

    /**
     * @brief Whether a blocking non-print operation is currently in progress
     *
     * True while the printer is executing a blocking operation that holds
     * Klipper's single-threaded g-code lock but is NOT a normal file print:
     * homing (G28), BED_MESH_CALIBRATE, QUAD_GANTRY_LEVEL, PROBE_ACCURACY,
     * an interactive manual probe, or a long macro. Discretionary g-code sent
     * during such an op just queues until it finishes, then times out — so a
     * send-boundary guard uses this to refuse it early with a toast.
     *
     * Signal =
     *   (idle_timeout.state == "Printing" AND print_job_state NOT IN {PRINTING, PAUSED})
     *   OR manual_probe.is_active
     *
     * Real file prints (PRINTING/PAUSED) are excluded — mid-print fan/temp tweaks
     * are legitimate and Klipper handles them between moves.
     *
     * @return true if a blocking non-print operation is active
     */
    bool is_blocking_operation_active();

    /**
     * @brief Like is_blocking_operation_active(), but attributes self-inflicted busy
     *
     * Treats busy-ness attributable to the app's own recent jog activity (in
     * flight, or acked within AppMotionActivity::GRACE_WINDOW) as NOT blocking.
     * Manual probe remains an absolute block. Guards for discretionary gcode use
     * THIS predicate so back-to-back jogs don't self-block — idle_timeout reports
     * "Printing" during any move, including our own jog (spec 2026-07-15).
     *
     * @return true if a blocking non-print operation NOT caused by the app is active
     */
    bool is_external_blocking_operation_active();

    /// App-initiated motion (jog) activity tracker; Task 3 stamps it from the
    /// motion API so is_external_blocking_operation_active() can subtract self-busy.
    helix::AppMotionActivity& app_motion_activity() {
        return app_motion_activity_;
    }

    /// App-initiated macro/homing/calibration/filament-op activity tracker.
    ///
    /// Consulted ONLY by the busy-queue toast decision in
    /// IMoonrakerAPI::execute_gcode, which suppresses the "printer is busy"
    /// notification when the blocking op is one the user just started here
    /// (prestonbrown/helixscreen#1206).
    ///
    /// Deliberately NOT read by is_blocking_operation_active() or
    /// is_external_blocking_operation_active(). Those predicates also gate
    /// motion, and subtracting app-initiated macros there would let a late jog
    /// through during a filament op — a toolhead-collision hazard (#1108).
    helix::AppMacroActivity& app_macro_activity() {
        return app_macro_activity_;
    }

    /**
     * @brief Get the configured (saved) z-offset in microns
     *
     * Returns the printer's saved z-offset value before calibration started.
     * For probe printers: reads probe z_offset from ProbeSensorManager.
     * For endstop printers: reads stepper_z position_endstop from config.
     *
     * @return Z-offset in microns (e.g., -1500 for -1.500mm)
     */
    int get_configured_z_offset_microns();

    // ========================================================================
    // HARDWARE VALIDATION API
    // ========================================================================

    // ========================================================================
    // PRINTER TYPE AND PRINT START CAPABILITIES
    // ========================================================================

    /**
     * @brief Set the printer type synchronously (main-thread only)
     *
     * Directly updates printer type without async deferral.
     * Only call this from the main LVGL thread (e.g., in tests with init_subjects(false)).
     *
     * @param type Printer type name (e.g., "FlashForge Adventurer 5M Pro")
     */
    void set_printer_type_sync(const std::string& type);

    /**
     * @brief Record that an installed SET_GCODE_OFFSET wrapper owns z-offset
     *        persistence (zoffset:: matched a provider in discovery)
     *
     * Re-resolves the calibration strategy: with the offset persisted by the
     * wrapper, the probe fold in "Save Z Offset" would double-apply it on
     * every Klipper restart (prestonbrown/helixscreen#1401), so the strategy
     * becomes FIRMWARE_MANAGED and the save path stands down. Sticky across
     * printer-type re-resolution. Thread-safe: defers to the main thread.
     * @param provider_name for the log line only
     */
    void set_z_offset_external_persistence(const std::string& provider_name);

    /// The provider is gone or was never really there: restore the type-derived
    /// strategy. Two callers - rediscovery finding no provider (module
    /// uninstalled), and update_from_status() when a frame refutes a provider
    /// detected on an ambiguous signature (a SET_GCODE_OFFSET wrapper that
    /// stores nothing). Thread-safe like the setter.
    void clear_z_offset_external_persistence();

    /// Main-thread bodies; tests reach these directly.
    void set_z_offset_external_persistence_internal(const std::string& provider_name);
    void clear_z_offset_external_persistence_internal();

    /**
     * @brief Get the current printer type name
     *
     * @return Const reference to the stored printer type string
     */
    const std::string& get_printer_type() const {
        return profile_state_.printer_type();
    }

    /**
     * @brief Get the pre-print option set for the current printer type
     *
     * Returns the option set fetched from the database when set_printer_type()
     * was called. If the printer type is unknown or not set, returns an empty
     * option set.
     *
     * @return Const reference to the PrePrintOptionSet
     */
    const PrePrintOptionSet& get_pre_print_option_set() const {
        return profile_state_.pre_print_option_set();
    }

    /**
     * @brief Get the Z-offset calibration strategy for this printer
     */
    ZOffsetCalibrationStrategy get_z_offset_calibration_strategy() const {
        return profile_state_.z_offset_calibration_strategy();
    }

    // ========================================================================
    // MULTI-PRINTER SUBJECTS
    // ========================================================================

    /**
     * @brief Get the active printer display name subject
     *
     * String subject holding the human-readable name of the active printer.
     * Use with bind_text in XML to display the current printer name.
     */
    lv_subject_t* get_active_printer_name_subject() {
        return &active_printer_name_;
    }

    /**
     * @brief Get the printer type subject
     *
     * String subject updated on every change to the resolved printer type
     * (detection, wizard, printer manager). Consumers that resolved state
     * from the type at attach time — printer artwork, for one — re-resolve
     * by observing it, since auto-detection settles after the home panel
     * is built on a fresh install.
     *
     * The subject resets to "" on deinit_subjects()/re-init, and the
     * setter's no-change early return means a soft restart repopulates it
     * only on the next real type change. Treat it as a change signal and
     * read the value from Config or get_printer_type().
     */
    lv_subject_t* get_printer_type_subject() {
        return profile_state_.get_printer_type_subject();
    }

    /**
     * @brief Set the active printer display name
     *
     * Updates the string subject with the given name. Main-thread only.
     *
     * @param name Human-readable printer name
     */
    void set_active_printer_name(const std::string& name);

  private:
    void unregister_static_deinit();

    /// RAII manager for automatic subject cleanup - deinits all subjects on destruction
    SubjectManager subjects_;

    /// Temperature state component (extruder and bed temperatures)
    helix::PrinterTemperatureState temperature_state_;

    /// Motion state component (position, speed/flow, z-offset)
    helix::PrinterMotionState motion_state_;

    /// Fan state component (fan speed, multi-fan tracking)
    helix::PrinterFanState fan_state_;

    /// Print state component (progress, state, timing, layers, print start)
    helix::PrinterPrintState print_domain_;

    /// Capabilities state component (hardware capabilities, feature availability)
    helix::PrinterCapabilitiesState capabilities_state_;

    /// Plugin status component (helix_plugin_installed, helix_macros_status)
    helix::PrinterPluginStatusState plugin_status_state_;

    /// Calibration state component (firmware retraction, manual probe, motor state)
    helix::PrinterCalibrationState calibration_state_;

    /// App-initiated motion (jog) activity tracker for busy-guard attribution
    helix::AppMotionActivity app_motion_activity_;

    /// App-initiated macro/filament-op activity tracker for busy-TOAST attribution
    /// only — never consulted by the blocking-op predicates (see accessor).
    helix::AppMacroActivity app_macro_activity_;

    /// Hardware validation state component (issue counts, severity, status text)
    helix::PrinterHardwareValidationState hardware_validation_state_;

    /// Composite visibility state component (has_any_preprint_options aggregate)
    helix::PrinterCompositeVisibilityState composite_visibility_state_;

    /// Network state component (connection, klippy, nav buttons)
    helix::PrinterNetworkState network_state_;

    /// Versions state component (klipper and moonraker version strings)
    helix::PrinterVersionsState versions_state_;

    /// Excluded objects state component (excluded_objects_version, excluded_objects set)
    helix::PrinterExcludedObjectsState excluded_objects_state_;

    /// Printer type, its pre-print option set and z-offset calibration strategy
    helix::PrinterProfileState profile_state_;

    // Note: Print subjects are now managed by print_domain_ component
    // (print_progress_, print_filename_, print_state_, print_state_enum_,
    //  print_outcome_, print_active_, print_show_progress_, print_display_filename_,
    //  print_thumbnail_path_, print_layer_current_, print_layer_total_,
    //  print_duration_, print_time_left_, print_start_phase_, print_start_message_,
    //  print_start_progress_, print_in_progress_)

    // Note: Motion subjects (position_x_, position_y_, position_z_, homed_axes_,
    // speed_factor_, flow_factor_, gcode_z_offset_, pending_z_offset_delta_)
    // are now managed by motion_state_ component

    // Note: Fan subjects (fan_speed_, fans_, fans_version_, fan_speed_subjects_)
    // are now managed by fan_state_ component

    // Note: Network subjects (printer_connection_state_, printer_connection_message_,
    // network_status_, klippy_state_, nav_buttons_enabled_, was_ever_connected_)
    // are now managed by network_state_ component

    // Note: Excluded objects subjects (excluded_objects_version_, excluded_objects_)
    // are now managed by excluded_objects_state_ component

    // Note: Printer capability subjects (printer_has_qgl_, printer_has_z_tilt_,
    // printer_has_bed_mesh_, printer_has_nozzle_clean_, printer_has_probe_,
    // printer_has_heater_bed_, printer_has_led_, printer_has_accelerometer_,
    // printer_has_spoolman_, printer_has_speaker_, printer_has_timelapse_,
    // printer_has_purge_line_, printer_has_firmware_retraction_, printer_bed_moves_)
    // are now managed by capabilities_state_ component

    // Note: Plugin status subjects (helix_plugin_installed_, helix_macros_status_)
    // are now managed by plugin_status_state_ component

    // Note: Aggregate visibility subject (has_any_preprint_options_) is managed
    // by composite_visibility_state_ component. The legacy per-op can_show_*
    // subjects were retired — nothing in XML or production C++ ever read them.

    // Note: Firmware retraction, manual probe, and motor state subjects
    // (retract_length_, retract_speed_, unretract_extra_length_, unretract_speed_,
    //  manual_probe_active_, manual_probe_z_position_, motors_enabled_)
    // are now managed by calibration_state_ component

    // Note: Version subjects (klipper_version_, moonraker_version_) are now managed
    // by versions_state_ component

    // Note: Hardware validation subjects (hardware_status_level_,
    // hardware_critical_count_, hardware_warning_count_, hardware_info_count_,
    // hardware_session_count_, hardware_status_title_, hardware_status_detail_,
    // hardware_issues_label_, hardware_validation_result_) are managed by the
    // hardware_validation_state_ component

    // Note: String buffers are now managed by their respective component classes
    // - homed_axes_buf_ is now in motion_state_ component
    // - print-related buffers are now in print_domain_ component
    // - hardware validation buffers are now in hardware_validation_state_ component
    // - printer_connection_message_buf_ is now in network_state_ component
    // - klipper_version_buf_, moonraker_version_buf_ are now in versions_state_ component

    // Multi-printer subjects (owned directly by PrinterState)
    lv_subject_t active_printer_name_{};
    char active_printer_name_buf_[128];

    // Initialization guard to prevent multiple subject initializations
    bool subjects_initialized_ = false;

    /// Generation guard for the setters that defer their work to the main
    /// thread. Invalidated by `deinit_subjects()` and by destruction, so a
    /// callback still queued when the subjects go away is dropped instead of
    /// running against a torn-down subject tree (#1165, #1146). Distinct from
    /// the `SubjectLifetime` tokens handed to observers, which are
    /// `shared_ptr<bool>` death signals and carry no deferral machinery.
    AsyncLifetimeGuard async_lifetime_;

    // Cached display pointer to detect LVGL reinitialization (for test isolation)
    lv_display_t* cached_display_ = nullptr;

    // Note: was_ever_connected_ is now managed by network_state_ component

    // Capability override layer (user config overrides for auto-detected capabilities)
    CapabilityOverrides capability_overrides_;

    // Cached hardware discovery result (for UI access to heater/sensor lists)
    helix::PrinterDiscovery discovery_;

    /// Last kinematics string (to skip redundant recomputation)
    std::string last_kinematics_;

    /// Auto-detected bed_moves value from kinematics (before user override)
    bool auto_detected_bed_moves_ = false;

    /// Klipper pause_resume.is_paused: true when the print is paused via PAUSE gcode
    bool is_paused_ = false;

    // ============================================================================
    // Main-thread internal methods (run from queued callbacks)
    // ============================================================================
    // These methods contain the actual LVGL subject updates and must only be called
    // from the main thread. Public setters such as set_klipper_version() reach them
    // through async_lifetime_.defer(), ensuring thread safety.

    friend class PrinterStateTestAccess;
    friend class PrinterTemperatureStateTestAccess;

    void set_klipper_version_internal(const std::string& version);
    void set_moonraker_version_internal(const std::string& version);
    void set_os_version_internal(const std::string& version);
    void set_klippy_state_internal(KlippyState state);
    void set_printer_type_internal(const std::string& type);

    /// Merge the settings a self-storing firmware currently holds into the
    /// pre-print option defaults, keyed by option id, and resynthesise if any
    /// changed. Safe from any thread. Merges rather than replaces: Moonraker
    /// sends deltas, so a frame mentioning one setting is silent about the
    /// rest, not a report that they are off.
    void merge_firmware_option_defaults(std::map<std::string, bool> defaults);

    /// Main-thread half of set_klippy_state_if_unseeded(): re-checks the guard in
    /// the same serialized order as the webhooks parse, then applies.
    void set_klippy_state_if_unseeded_internal(KlippyState state);

    /**
     * @brief Synthesize runtime-dependent options (timelapse, etc.) into the
     *        cached `PrePrintOptionSet`.
     *
     * Some options aren't declared in the printer database — they're driven
     * by runtime capability discovery (e.g. the `timelapse` toggle only
     * appears when the moonraker-timelapse plugin is installed). This helper
     * appends those options to whatever the database loaded, so the rest of
     * the system can treat them uniformly.
     *
     * Idempotent — call after the database load and again whenever one of
     * the runtime capabilities changes (e.g. `set_timelapse_available()`).
     * Re-running clears any previously synthesized options before re-adding
     * the ones that should currently be present.
     */
    void apply_dynamic_options();

    /// The moonraker-timelapse plugin is available (main thread only).
    bool timelapse_available();

    /**
     * @brief Update combined nav_buttons_enabled subject
     *
     * Recalculates nav_buttons_enabled based on connection and klippy state.
     * Called whenever printer_connection_state or klippy_state changes.
     */
    void update_nav_buttons_enabled();

    /**
     * @brief Refresh the has_any_preprint_options aggregate
     *
     * Recomputes the aggregate visibility subject from current plugin status,
     * capability subjects, and the framework option count. Called whenever
     * helix_plugin_installed, printer_has_*, or the cached PrePrintOptionSet
     * change. Must be called from the main thread (typically via async callbacks).
     */
    void update_gcode_modification_visibility();
};

} // namespace helix
