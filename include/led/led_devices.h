// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_backend.h"

#include <cstdint>
#include <string>
#include <vector>

namespace helix::led {

/// Light-button config value meaning "every switchable device".
constexpr const char* LIGHT_BUTTON_ALL = "all";

/// The printer's main light: the first NATIVE or OUTPUT_PIN device whose Klipper
/// object name (the id after its type prefix) is chamber_light, chamber_LED,
/// case_light or caselight, compared case-insensitively and preferred in that
/// order. @p fallback when none matches.
std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices,
                                  const std::string& fallback);

/// Device ids a light button whose `led` config is @p key drives. Empty key: the
/// chamber light. LIGHT_BUTTON_ALL: every switchable device. A switchable id: that
/// device. An id that no longer exists: the chamber light.
std::vector<std::string> resolve_light_targets(const std::string& key,
                                               const std::vector<std::string>& switchable,
                                               const std::string& chamber);

/// resolve_light_targets() over every key, deduplicated in first-seen order. No
/// keys at all means the chamber light.
std::vector<std::string> union_light_targets(const std::vector<std::string>& keys,
                                             const std::vector<std::string>& switchable,
                                             const std::string& chamber);

enum class PowerState : int { Off = 0, On = 1, Unknown = 2 };

/// What the UI can say about one device right now.
struct DeviceState {
    PowerState power = PowerState::Unknown;
    int brightness = 0;      ///< 0-100
    uint32_t rgb = 0xFFFFFF; ///< full-brightness hue; meaningful only when has_rgb
    bool has_rgb = false;    ///< false: draw the theme's light color instead
};

/// What a light button sends when tapped: off while any target is on, on while
/// any is off, and with nothing readable the opposite of what it last sent.
bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on);

} // namespace helix::led
