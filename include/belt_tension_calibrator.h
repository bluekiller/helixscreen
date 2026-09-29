// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file belt_tension_calibrator.h
 * @brief High-level orchestrator for belt tension calibration workflow
 *
 * BeltTensionCalibrator coordinates IMoonrakerAPI calls and provides
 * progress/error callbacks to the UI layer.
 *
 * @see InputShaperCalibrator for the equivalent input shaper workflow
 */

#include "async_lifetime_guard.h"
#include "belt_tension_types.h"

#include <atomic>
#include <string>

// Forward declaration
class IMoonrakerAPI;

namespace helix::calibration {

class BeltTensionCalibrator {
  public:
    /// State machine states
    enum class State {
        IDLE,               ///< Ready to start, no measurement in progress
        DETECTING_HARDWARE, ///< Querying printer for capabilities
        CHECKING_ADXL,      ///< Verifying accelerometer connectivity
        HOMING,             ///< Homing printer axes
        TESTING_PATH_A,     ///< Running resonance sweep on path A
        TESTING_PATH_B,     ///< Running resonance sweep on path B
        RESULTS_READY,      ///< Both paths measured, results available
        ERROR,              ///< An error occurred
    };

    /**
     * @brief Default constructor for tests without API
     *
     * Operations will fail with error callbacks when no API is available.
     */
    BeltTensionCalibrator();

    /**
     * @brief Constructor with API dependency injection
     *
     * @param api Non-owning pointer to IMoonrakerAPI instance
     */
    explicit BeltTensionCalibrator(IMoonrakerAPI* api);

    ~BeltTensionCalibrator();

    // Non-copyable, non-movable (shared alive_ makes move unsound)
    BeltTensionCalibrator(const BeltTensionCalibrator&) = delete;
    BeltTensionCalibrator& operator=(const BeltTensionCalibrator&) = delete;
    BeltTensionCalibrator(BeltTensionCalibrator&&) = delete;
    BeltTensionCalibrator& operator=(BeltTensionCalibrator&&) = delete;

    // ========================================================================
    // State Queries
    // ========================================================================

    [[nodiscard]] State get_state() const {
        return state_.load();
    }
    [[nodiscard]] const BeltTensionHardware& get_hardware() const {
        return hardware_;
    }

    // ========================================================================
    // Hardware Detection
    // ========================================================================

    /**
     * @brief Detect printer hardware capabilities
     *
     * Queries printer.objects.list and printer.objects.query to determine
     * kinematics type, ADXL presence, belted Z, and PWM LED availability.
     *
     * @param on_complete Called with detected hardware on success
     * @param on_error Called with error message on failure
     */
    void detect_hardware(BeltHardwareDetectCallback on_complete, BeltErrorCallback on_error);

    // ========================================================================
    // Control
    // ========================================================================

    /// Cancel any in-progress operation and return to IDLE
    void cancel();

    /// Reset calibrator to initial state, clearing detected hardware
    void reset();

  private:
    std::atomic<State> state_{State::IDLE};
    IMoonrakerAPI* api_ = nullptr;
    BeltTensionHardware hardware_;

    /// Async callback safety guard
    helix::AsyncLifetimeGuard lifetime_;
};

} // namespace helix::calibration
