// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "led/led_backend.h"

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

} // namespace helix::led
