// SPDX-License-Identifier: GPL-3.0-or-later

#include "led/led_device_page.h"

#include "../catch_amalgamated.hpp"

using namespace helix::led;

namespace {

LedStripInfo native(bool color, bool white) {
    LedStripInfo s;
    s.id = "neopixel x";
    s.backend = LedBackendType::NATIVE;
    s.supports_color = color;
    s.supports_white = white;
    return s;
}

LedStripInfo of(LedBackendType backend, bool pwm = false) {
    LedStripInfo s;
    s.id = "x";
    s.backend = backend;
    s.supports_color = false;
    s.supports_white = false;
    s.is_pwm = pwm;
    return s;
}

constexpr auto ANY = MacroLedType::TOGGLE; // ignored unless the device is a macro

} // namespace

TEST_CASE("page: Klipper LED, RGBW", "[led][page]") {
    CHECK(
        classify_device_page(native(true, true), ANY, true) ==
        DevicePage{LampControl::PowerAndBrightness, WhiteMode::WChannel, true, ListKind::Effects});
    CHECK(classify_device_page(native(true, true), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::WChannel, true, ListKind::None});
}

TEST_CASE("page: Klipper LED, RGB", "[led][page]") {
    CHECK(classify_device_page(native(true, false), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::Mixed, true, ListKind::Effects});
    CHECK(classify_device_page(native(true, false), ANY, false) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::Mixed, true, ListKind::None});
}

TEST_CASE("page: Klipper LED, single channel", "[led][page]") {
    CHECK(
        classify_device_page(native(false, false), ANY, false) ==
        DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::LevelChips});
    CHECK(classify_device_page(native(false, false), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Effects});
}

TEST_CASE("page: WLED", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::WLED), ANY, true) ==
          DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::Presets});
}

TEST_CASE("page: output pin, PWM", "[led][page]") {
    CHECK(
        classify_device_page(of(LedBackendType::OUTPUT_PIN, true), ANY, false) ==
        DevicePage{LampControl::PowerAndBrightness, WhiteMode::None, false, ListKind::LevelChips});
}

TEST_CASE("page: output pin, on/off", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::OUTPUT_PIN, false), ANY, false) ==
          DevicePage{LampControl::PowerOnly, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro ON_OFF", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::ON_OFF, false) ==
          DevicePage{LampControl::OnOffButtons, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro TOGGLE", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::TOGGLE, false) ==
          DevicePage{LampControl::ToggleButton, WhiteMode::None, false, ListKind::None});
}

TEST_CASE("page: macro PRESET", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::MACRO), MacroLedType::PRESET, false) ==
          DevicePage{LampControl::None, WhiteMode::None, false, ListKind::Presets});
}

TEST_CASE("page: an effect is not a device", "[led][page]") {
    CHECK(classify_device_page(of(LedBackendType::LED_EFFECT), ANY, true) == DevicePage{});
}

TEST_CASE("white_tone: the W channel carries white on RGBW", "[led][page]") {
    const Rgbw n = white_tone(WhiteTone::Neutral, WhiteMode::WChannel);
    CHECK(n.w == Catch::Approx(1.0));
    CHECK(n.r + n.g + n.b == Catch::Approx(0.0));
    for (auto t : {WhiteTone::Cool, WhiteTone::Warm}) {
        CHECK(white_tone(t, WhiteMode::WChannel).w == Catch::Approx(1.0));
    }
}

TEST_CASE("white_tone: RGB strips mix white and never touch W", "[led][page]") {
    for (auto t : {WhiteTone::Cool, WhiteTone::Neutral, WhiteTone::Warm}) {
        CHECK(white_tone(t, WhiteMode::Mixed).w == Catch::Approx(0.0));
    }
}

TEST_CASE("white_tone: cool is bluer than warm", "[led][page]") {
    for (auto mode : {WhiteMode::WChannel, WhiteMode::Mixed}) {
        const Rgbw cool = white_tone(WhiteTone::Cool, mode);
        const Rgbw warm = white_tone(WhiteTone::Warm, mode);
        CHECK(cool.b > warm.b);
        CHECK(warm.r > cool.r);
    }
    CHECK(white_tone(WhiteTone::Cool, WhiteMode::None) == Rgbw{});
}
