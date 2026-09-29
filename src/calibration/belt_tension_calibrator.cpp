// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file belt_tension_calibrator.cpp
 * @brief Implementation of BeltTensionCalibrator state machine
 *
 * Orchestrates the belt tension workflow by delegating API calls to
 * MoonrakerAdvancedAPI.
 */

#include "belt_tension_calibrator.h"

#include "i_moonraker_api.h"
#include "spdlog/spdlog.h"

namespace helix::calibration {

// ============================================================================
// Constructors / Destructor
// ============================================================================

BeltTensionCalibrator::BeltTensionCalibrator() : api_(nullptr) {
    spdlog::debug("[BeltTension] Created without API (test mode)");
}

BeltTensionCalibrator::BeltTensionCalibrator(IMoonrakerAPI* api) : api_(api) {
    spdlog::debug("[BeltTension] Created with API");
}

BeltTensionCalibrator::~BeltTensionCalibrator() {
    // lifetime_ destructor calls invalidate() automatically
}

// ============================================================================
// detect_hardware() — thin wrapper around API
// ============================================================================

void BeltTensionCalibrator::detect_hardware(BeltHardwareDetectCallback on_complete,
                                            BeltErrorCallback on_error) {
    if (!api_) {
        spdlog::warn("[BeltTension] detect_hardware called without API");
        if (on_error) {
            on_error("No API available");
        }
        return;
    }

    state_.store(State::DETECTING_HARDWARE);
    spdlog::info("[BeltTension] Detecting printer hardware capabilities");

    api_->advanced().detect_belt_hardware(
        lifetime_.bg_cb("BeltTensionCalibrator::detect_hw_success",
                        [this, on_complete](const BeltTensionHardware& hw) {
                            hardware_ = hw;
                            state_.store(State::IDLE);
                            if (on_complete)
                                on_complete(hw);
                        }),
        lifetime_.bg_cb("BeltTensionCalibrator::detect_hw_error",
                        [this, on_error](const MoonrakerError& err) {
                            state_.store(State::ERROR);
                            if (on_error)
                                on_error("Hardware detection failed: " + err.message);
                        }));
}

// ============================================================================
// cancel()
// ============================================================================

void BeltTensionCalibrator::cancel() {
    spdlog::info("[BeltTension] Cancelling (was state={})", static_cast<int>(state_.load()));
    state_.store(State::IDLE);
}

// ============================================================================
// reset()
// ============================================================================

void BeltTensionCalibrator::reset() {
    spdlog::info("[BeltTension] Resetting calibrator");

    state_.store(State::IDLE);
    hardware_ = BeltTensionHardware{};
}

} // namespace helix::calibration
