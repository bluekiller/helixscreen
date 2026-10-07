// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ams_types.h"
#include "clog_meter_geometry.h"

#include <string>

namespace helix {

/// What a buffer reading comes from, which is also what it is called on screen.
enum class BufferSource : int {
    None = 0, ///< No proportional reading: a switched buffer, or no buffer at all
    Fps = 1,  ///< A filament pressure sensor (OpenAMS lane, AFC FPS_PSF buffer)
    Sync = 2, ///< Happy Hare sync feedback, one buffer for the whole system
};

/// Where a filament buffer sits right now, for every surface that draws one.
struct BufferReading {
    BufferSource source = BufferSource::None;
    /// Position in AmsSystemInfo::units of the sensor read; -1 for Happy Hare's
    /// system-level buffer.
    int unit = -1;
    /// There is a set point to centre on, so the reading has a bias and the
    /// slider draws. A pressure sensor without one is shown as text only.
    bool has_slider = false;
    /// Fps: the pressure, 0..100. Sync: the bias, -100..+100.
    int value_pct = 0;
    /// Fps with a set point: the set point, 0..100. -1 otherwise.
    int target_pct = -1;
    /// -1 tight .. +1 loose around the set point; 0 without one.
    float bias = 0.0f;
    /// ui::pressure_status() of the bias; Ok without a set point.
    ui::ClogMeterStatus status = ui::ClogMeterStatus::Ok;

    [[nodiscard]] bool present() const {
        return source != BufferSource::None;
    }
};

/// The reading for one unit's buffer, or the system's with @p unit -1.
///
/// A unit with its own pressure sensor reads that sensor, and a unit with a
/// switched buffer has no reading. Any other unit, and -1, reads the system:
/// the pressure sensor feeding the toolhead (feeding_pressure_unit()), else
/// Happy Hare's sync feedback.
[[nodiscard]] BufferReading buffer_reading(const AmsSystemInfo& info, int unit);

/// "FPS" or "Sync"; empty with no reading.
[[nodiscard]] const char* buffer_label(const BufferReading& r);

/// The number: "32%" for a pressure, "-45%" for a bias, "Pressure: 32%" for a
/// pressure with no set point; empty with no reading.
[[nodiscard]] std::string buffer_value_text(const BufferReading& r);

/// "target 50%" where a set point is known, else empty.
[[nodiscard]] std::string buffer_target_text(const BufferReading& r);

/// "Running tight" / "Running loose" / "Running balanced" from buffer_lean();
/// empty without a slider.
[[nodiscard]] const char* buffer_lean_text(const BufferReading& r);

} // namespace helix
