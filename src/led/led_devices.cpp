// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_devices.h"

#include <algorithm>
#include <cctype>

namespace helix::led {

namespace {

constexpr const char* CHAMBER_LIGHT_NAMES[] = {"chamber_light", "chamber_LED", "case_light",
                                               "caselight"};

std::string lowered(std::string s) {
    for (auto& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// "neopixel chamber_light" -> "chamber_light"; an id without a prefix is its own name.
std::string object_name(const std::string& id) {
    const auto space = id.find(' ');
    return space == std::string::npos ? id : id.substr(space + 1);
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

std::string resolve_chamber_light(const std::vector<LedStripInfo>& devices,
                                  const std::string& fallback) {
    for (const char* wanted : CHAMBER_LIGHT_NAMES) {
        const std::string want = lowered(wanted);
        for (const auto& d : devices) {
            if (d.backend != LedBackendType::NATIVE && d.backend != LedBackendType::OUTPUT_PIN) {
                continue;
            }
            if (lowered(object_name(d.id)) == want) {
                return d.id;
            }
        }
    }
    return fallback;
}

std::vector<std::string> resolve_light_targets(const std::string& key,
                                               const std::vector<std::string>& switchable,
                                               const std::string& chamber) {
    if (key == LIGHT_BUTTON_ALL) {
        return switchable;
    }
    if (!key.empty() && contains(switchable, key)) {
        return {key};
    }
    if (chamber.empty()) {
        return {};
    }
    return {chamber};
}

std::vector<std::string> union_light_targets(const std::vector<std::string>& keys,
                                             const std::vector<std::string>& switchable,
                                             const std::string& chamber) {
    if (keys.empty()) {
        return resolve_light_targets("", switchable, chamber);
    }
    std::vector<std::string> out;
    for (const auto& key : keys) {
        for (auto& id : resolve_light_targets(key, switchable, chamber)) {
            if (!contains(out, id)) {
                out.push_back(std::move(id));
            }
        }
    }
    return out;
}

bool next_power_on(const std::vector<PowerState>& states, bool last_sent_on) {
    bool any_off = false;
    for (auto s : states) {
        if (s == PowerState::On) {
            return false;
        }
        any_off = any_off || s == PowerState::Off;
    }
    return any_off ? true : !last_sent_on;
}

} // namespace helix::led
