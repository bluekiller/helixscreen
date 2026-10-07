// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "buffer_reading.h"

#include "lvgl/src/others/translation/lv_translation.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace helix {

BufferReading buffer_reading(const AmsSystemInfo& info, int unit) {
    BufferReading r;
    const AmsUnit* own = info.get_unit(unit);
    if (own && own->buffer_health && !own->buffer_health->fps_reported) {
        return r; // a switched buffer: where it sits is not measured
    }
    const int sensor = (own && own->buffer_health) ? unit : info.feeding_pressure_unit();
    if (sensor >= 0) {
        const BufferHealth& fps = *info.units[static_cast<size_t>(sensor)].buffer_health;
        r.source = BufferSource::Fps;
        r.unit = sensor;
        r.value_pct = std::clamp(static_cast<int>(std::lround(fps.smoothed_fps * 100.0f)), 0, 100);
        if (fps.has_fps()) {
            r.has_slider = true;
            r.target_pct = static_cast<int>(std::lround(fps.fps_set_point * 100.0f));
            r.bias = fps.fps_to_bias();
        }
    } else if (info.sync_feedback_bias > -1.5f) {
        r.source = BufferSource::Sync;
        r.has_slider = true;
        r.bias = std::clamp(info.sync_feedback_bias, -1.0f, 1.0f);
        r.value_pct = static_cast<int>(std::lround(r.bias * 100.0f));
    }
    if (r.has_slider) {
        r.status = ui::pressure_status(static_cast<int>(std::lround(r.bias * 100.0f)));
    }
    return r;
}

const char* buffer_label(const BufferReading& r) {
    switch (r.source) {
    case BufferSource::Fps:
        return "FPS"; // i18n: do not translate - hardware abbreviation
    case BufferSource::Sync:
        return lv_tr("Sync");
    case BufferSource::None:
        break;
    }
    return "";
}

std::string buffer_value_text(const BufferReading& r) {
    if (!r.present()) {
        return "";
    }
    if (!r.has_slider) {
        return fmt::format("{} {}%", lv_tr("Pressure:"), r.value_pct);
    }
    if (r.source == BufferSource::Sync && r.value_pct != 0) {
        return fmt::format("{:+d}%", r.value_pct);
    }
    return fmt::format("{}%", r.value_pct);
}

std::string buffer_target_text(const BufferReading& r) {
    if (r.target_pct < 0) {
        return "";
    }
    return fmt::format(fmt::runtime(lv_tr("target {}%")), r.target_pct);
}

const char* buffer_lean_text(const BufferReading& r) {
    if (!r.has_slider) {
        return "";
    }
    switch (ui::buffer_lean(r.bias)) {
    case ui::BufferLean::Tight:
        return lv_tr("Running tight");
    case ui::BufferLean::Loose:
        return lv_tr("Running loose");
    case ui::BufferLean::Balanced:
        break;
    }
    return lv_tr("Running balanced");
}

} // namespace helix
