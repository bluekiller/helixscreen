// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_devices.h"

#include "../catch_amalgamated.hpp"

using namespace helix::led;

namespace {

LedStripInfo dev(const std::string& id, LedBackendType backend) {
    LedStripInfo s;
    s.id = id;
    s.name = id;
    s.backend = backend;
    s.supports_color = false;
    s.supports_white = false;
    return s;
}

const LedStripInfo SB = dev("neopixel sb_leds", LedBackendType::NATIVE);

} // namespace

TEST_CASE("resolve_chamber_light: every spelling matches", "[led][devices]") {
    CHECK(resolve_chamber_light({SB, dev("neopixel chamber_light", LedBackendType::NATIVE)},
                                "fb") == "neopixel chamber_light");
    CHECK(resolve_chamber_light({SB, dev("led chamber_LED", LedBackendType::NATIVE)}, "fb") ==
          "led chamber_LED");
    CHECK(resolve_chamber_light({SB, dev("neopixel case_light", LedBackendType::NATIVE)}, "fb") ==
          "neopixel case_light");
    CHECK(resolve_chamber_light({SB, dev("output_pin caselight", LedBackendType::OUTPUT_PIN)},
                                "fb") == "output_pin caselight");
}

TEST_CASE("resolve_chamber_light: case does not matter", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("neopixel Chamber_Light", LedBackendType::NATIVE)}, "fb") ==
          "neopixel Chamber_Light");
    CHECK(resolve_chamber_light({dev("led CASELIGHT", LedBackendType::NATIVE)}, "fb") ==
          "led CASELIGHT");
}

TEST_CASE("resolve_chamber_light: spelling order is preference order", "[led][devices]") {
    const std::vector<LedStripInfo> both = {dev("led caselight", LedBackendType::NATIVE),
                                            dev("neopixel chamber_light", LedBackendType::NATIVE)};
    CHECK(resolve_chamber_light(both, "fb") == "neopixel chamber_light");
}

TEST_CASE("resolve_chamber_light: only whole object names match", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("neopixel chamber_light_bar", LedBackendType::NATIVE),
                                 dev("neopixel my_caselight", LedBackendType::NATIVE)},
                                "fb") == "fb");
}

TEST_CASE("resolve_chamber_light: macros and WLED are not Klipper objects", "[led][devices]") {
    CHECK(resolve_chamber_light({dev("macro:chamber_light", LedBackendType::MACRO),
                                 dev("chamber_light", LedBackendType::WLED)},
                                "fb") == "fb");
}

TEST_CASE("resolve_chamber_light: fallback, and no devices at all", "[led][devices]") {
    CHECK(resolve_chamber_light({SB}, "neopixel sb_leds") == "neopixel sb_leds");
    CHECK(resolve_chamber_light({}, "").empty());
}

TEST_CASE("resolve_light_targets: each key shape", "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a", "neopixel b", "macro:Lamp"};
    CHECK(resolve_light_targets("", sw, "neopixel a") == std::vector<std::string>{"neopixel a"});
    CHECK(resolve_light_targets(LIGHT_BUTTON_ALL, sw, "neopixel a") == sw);
    CHECK(resolve_light_targets("macro:Lamp", sw, "neopixel a") ==
          std::vector<std::string>{"macro:Lamp"});
}

TEST_CASE("resolve_light_targets: an unknown id falls back to the chamber light",
          "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a"};
    CHECK(resolve_light_targets("neopixel gone", sw, "neopixel a") ==
          std::vector<std::string>{"neopixel a"});
}

TEST_CASE("resolve_light_targets: nothing to drive", "[led][devices]") {
    CHECK(resolve_light_targets("", {}, "").empty());
    CHECK(resolve_light_targets(LIGHT_BUTTON_ALL, {}, "").empty());
    CHECK(resolve_light_targets("neopixel gone", {}, "").empty());
}

TEST_CASE("union_light_targets: union in first-seen order, chamber when no buttons",
          "[led][devices]") {
    const std::vector<std::string> sw = {"neopixel a", "neopixel b", "neopixel c"};
    CHECK(union_light_targets({}, sw, "neopixel b") == std::vector<std::string>{"neopixel b"});
    CHECK(union_light_targets({"neopixel c", "", "neopixel c"}, sw, "neopixel b") ==
          std::vector<std::string>{"neopixel c", "neopixel b"});
    CHECK(union_light_targets({"", LIGHT_BUTTON_ALL}, sw, "neopixel b") ==
          std::vector<std::string>{"neopixel b", "neopixel a", "neopixel c"});
}
