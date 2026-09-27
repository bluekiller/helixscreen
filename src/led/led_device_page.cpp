// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_device_page.h"

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

} // namespace helix::led
