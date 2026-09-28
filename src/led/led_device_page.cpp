// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_device_page.h"

#include "led/led_color_utils.h"

#include <algorithm>

namespace helix::led {

DevicePage classify_device_page(const LedStripInfo& device, MacroLedType macro_type,
                                bool has_effects) {
    switch (device.backend) {
    case LedBackendType::NATIVE:
        if (device.supports_color) {
            return {LampControl::PowerAndBrightness,
                    device.supports_white ? WhiteMode::WChannel : WhiteMode::Mixed, true,
                    has_effects ? ListKind::Effects : ListKind::None};
        }
        return {LampControl::PowerAndBrightness, WhiteMode::None, false,
                has_effects ? ListKind::Effects : ListKind::LevelChips};
    case LedBackendType::WLED:
        return {LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Presets};
    case LedBackendType::OUTPUT_PIN:
        if (device.is_pwm) {
            return {LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::LevelChips};
        }
        return {LampControl::PowerOnly, WhiteMode::None, false, ListKind::None};
    case LedBackendType::MACRO:
        switch (macro_type) {
        case MacroLedType::ON_OFF:
            return {LampControl::OnOffButtons, WhiteMode::None, false, ListKind::None};
        case MacroLedType::TOGGLE:
            return {LampControl::ToggleButton, WhiteMode::None, false, ListKind::None};
        case MacroLedType::PRESET:
            return {LampControl::None, WhiteMode::None, false, ListKind::Presets};
        }
        break;
    case LedBackendType::LED_EFFECT:
        break;
    }
    return {};
}

Rgbw white_tone(WhiteTone tone, WhiteMode mode) {
    if (mode == WhiteMode::WChannel) {
        switch (tone) {
        case WhiteTone::Cool:
            return {0.0, 0.0, 0.25, 1.0};
        case WhiteTone::Neutral:
            return {0.0, 0.0, 0.0, 1.0};
        case WhiteTone::Warm:
            return {0.35, 0.12, 0.0, 1.0};
        }
    }
    if (mode == WhiteMode::Mixed) {
        switch (tone) {
        case WhiteTone::Cool:
            return {0.80, 0.88, 1.0, 0.0};
        case WhiteTone::Neutral:
            return {1.0, 0.95, 0.88, 0.0};
        case WhiteTone::Warm:
            return {1.0, 0.72, 0.42, 0.0};
        }
    }
    return {};
}

uint32_t output_rgb(double r, double g, double b, double w) {
    const double m = std::max({r + w, g + w, b + w});
    if (m <= 0.0) {
        return 0xFFFFFF;
    }
    return pack_rgb((r + w) / m, (g + w) / m, (b + w) / m);
}

Look fit_look(uint32_t rgb, double w, const LedStripInfo& device) {
    if (!device.supports_color) {
        return device.supports_white ? Look{0, 1.0} : Look{0xFFFFFF, 0.0};
    }
    if (rgb == 0 && w <= 0.0) {
        return {0xFFFFFF, 0.0};
    }
    if (!device.supports_white && w > 0.0) {
        double r = 0.0, g = 0.0, b = 0.0;
        unpack_rgb(rgb, r, g, b);
        return {output_rgb(r, g, b, w), 0.0};
    }
    return {rgb, w};
}

} // namespace helix::led
