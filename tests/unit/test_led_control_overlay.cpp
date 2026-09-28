// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file test_led_control_overlay.cpp
 * @brief The LEDs overlay model: which device is focused, the subjects its page
 * publishes, and that every control acts on the focused device alone.
 *
 * @see ui_led_control_overlay.h
 */

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "app_globals.h"
#include "config.h"
#include "helix-xml/src/xml/lv_xml.h"
#include "led/led_auto_state.h"
#include "led/led_backend.h"
#include "led/led_controller.h"
#include "led/led_device_page.h"
#include "led/ui_led_control_overlay.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "printer_state.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

// Must live in namespace helix::led to match the friend declaration.
namespace helix::led {
class LedControlOverlayTestAccess {
  public:
    explicit LedControlOverlayTestAccess(helix::PrinterState& ps) : overlay_(ps) {
        overlay_.init_subjects();
    }

    /// Opens the overlay on @p requested the way open_led_control_overlay() does,
    /// without building or pushing a widget tree.
    void activate(const std::string& requested) {
        overlay_.request_focus(requested);
        overlay_.on_activate();
    }

    void tap_tab(int i) {
        overlay_.handle_tab_clicked(i);
    }
    [[nodiscard]] std::string focused() const {
        return overlay_.focused_device();
    }
    [[nodiscard]] static int int_subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_int(s);
    }
    [[nodiscard]] static std::string str_subject(const char* name) {
        lv_subject_t* s = lv_xml_get_subject(nullptr, name);
        REQUIRE(s != nullptr);
        return lv_subject_get_string(s);
    }
    void tap_swatch(int i) {
        overlay_.handle_swatch(i);
    }
    void tap_power() {
        overlay_.handle_power();
    }
    void drag_brightness(int pct) {
        overlay_.handle_brightness(pct);
    }
    void tap_white(int tone) {
        overlay_.handle_white(tone);
    }
    void tap_list_chip(int i) {
        overlay_.handle_list_chip(i);
    }
    void tap_level(int pct) {
        overlay_.handle_brightness(pct);
    }
    void tap_macro_on() {
        overlay_.handle_macro_on();
    }
    void tap_macro_off() {
        overlay_.handle_macro_off();
    }
    void tap_macro_toggle() {
        overlay_.handle_macro_toggle();
    }
    void tap_effects_none() {
        overlay_.handle_effects_none();
    }

  private:
    helix::led::LedControlOverlay overlay_;
};
} // namespace helix::led

using helix::led::LedControlOverlayTestAccess;

namespace {

LedStripInfo make_native_strip(const std::string& id, bool supports_color, bool supports_white) {
    LedStripInfo strip;
    strip.name = id;
    strip.id = id;
    strip.backend = LedBackendType::NATIVE;
    strip.supports_color = supports_color;
    strip.supports_white = supports_white;
    return strip;
}

/// Channel-wise equality within the rounding a status round-trip introduces.
bool within_rounding(uint32_t a, uint32_t b) {
    for (int shift : {16, 8, 0}) {
        const int d = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
        if (d < -2 || d > 2) {
            return false;
        }
    }
    return true;
}

void drain() {
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
}

// A controller wired to a mock API with a live connection, so commands reach
// the mock wire and the native color cache.
struct LedApplyColorFixture : public LVGLTestFixture {
    MoonrakerClientMock mock_client{MoonrakerClientMock::PrinterType::VORON_24};
    helix::PrinterState state;
    std::unique_ptr<MoonrakerAPIMock> mock_api;

    LedApplyColorFixture() {
        state.init_subjects(false);
        // execute_gcode()'s halted gate rejects every command while klippy reads
        // SHUTDOWN, and the global connection state gates dispatch too.
        state.set_klippy_state_sync(helix::KlippyState::READY);
        auto& ps = get_printer_state();
        lv_subject_set_int(ps.get_printer_connection_state_subject(),
                           static_cast<int>(helix::ConnectionState::CONNECTED));
        ps.set_klippy_state_sync(helix::KlippyState::READY);
        mock_api = std::make_unique<MoonrakerAPIMock>(mock_client, state);

        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(mock_api.get(), &mock_client);
        drain();
        mock_client.clear_gcode_script_history();
    }

    ~LedApplyColorFixture() override {
        drain();
        LedAutoState::instance().set_strips({});
        LedController::instance().deinit();
    }

    /// Adds a native strip and selects nothing.
    void add_native(const std::string& id, bool color, bool white) {
        LedController::instance().native().add_strip(make_native_strip(id, color, white));
    }

    void add_effect(const std::string& name, const std::string& target, bool enabled) {
        LedEffectInfo e;
        e.name = name;
        e.display_name = LedEffectBackend::display_name_for_effect(name);
        e.target_leds = {target};
        e.enabled = enabled;
        LedController::instance().effects().add_effect(e);
    }

    void set_macros(const std::vector<LedMacroInfo>& macros) {
        auto& ctrl = LedController::instance();
        ctrl.set_configured_macros(macros);
        ctrl.rebuild_macro_backend();
    }

    static LedMacroInfo lamp_macro() {
        LedMacroInfo m;
        m.display_name = "Lamp";
        m.type = MacroLedType::ON_OFF;
        m.on_macro = "LIGHTS_ON";
        m.off_macro = "LIGHTS_OFF";
        return m;
    }

    static LedMacroInfo party_macro() {
        LedMacroInfo m;
        m.display_name = "Party";
        m.type = MacroLedType::PRESET;
        m.presets = {"LED_PARTY", "LED_RAINBOW"};
        return m;
    }

    [[nodiscard]] helix::led::NativeBackend::StripColor sent(const std::string& id) const {
        return LedController::instance().native().get_strip_color(id);
    }

    [[nodiscard]] bool wire_mentions(const std::string& needle) const {
        const auto h = mock_client.gcode_script_history();
        return std::any_of(h.begin(), h.end(), [&needle](const std::string& s) {
            return s.find(needle) != std::string::npos;
        });
    }

    static nlohmann::json leds_config() {
        auto* cfg = Config::get_instance();
        const auto* node = cfg->try_get_json(cfg->df() + "leds");
        return node != nullptr ? *node : nlohmann::json();
    }
};

} // namespace

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a tab tap changes focus and nothing else",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    // Auto-state names a different device than the tab tapped below, so a tap
    // that wrote it would show.
    LedAutoState::instance().set_strips({"neopixel chamber_light"});
    const nlohmann::json before = leds_config();
    const auto selection_before = LedController::instance().selected_strips(); // removed in Task 11

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");
    REQUIRE(access.focused() == "neopixel chamber_light");
    access.tap_tab(1);

    CHECK(access.focused() == "neopixel sb_leds");
    CHECK(access.int_subject("led_focused_tab") == 1);
    CHECK(LedAutoState::instance().strips() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(leds_config() == before);
    CHECK(LedController::instance().selected_strips() == selection_before); // removed in Task 11
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: opens on the requested device", "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel sb_leds");

    CHECK(access.focused() == "neopixel sb_leds");
    CHECK(access.int_subject("led_focused_tab") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: opens on the last focused device",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");
    access.tap_tab(1);
    access.activate("");

    CHECK(access.focused() == "neopixel sb_leds");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: falls back to the chamber light",
                 "[led][overlay]") {
    add_native("neopixel sb_leds", true, false);
    add_native("neopixel chamber_light", true, true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel nonexistent");

    CHECK(access.focused() == "neopixel chamber_light");
    CHECK(access.int_subject("led_focused_tab") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a vanished focused device refocuses the chamber light",
                 "[led][overlay]") {
    add_native("neopixel sb_leds", true, false);
    add_native("neopixel chamber_light", true, true);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("macro:Lamp");
    REQUIRE(access.focused() == "macro:Lamp");

    set_macros({});
    access.activate("");

    CHECK(access.focused() == "neopixel chamber_light");
    CHECK(access.int_subject("led_tab_count") == 2);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: overlay with no devices", "[led][overlay]") {
    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");

    CHECK(access.int_subject("led_tab_count") == 0);
    CHECK(access.focused().empty());
    access.tap_power();
    access.drag_brightness(50);
    access.tap_swatch(0);
    access.tap_white(1);
    access.tap_list_chip(0);
    access.tap_effects_none();
    CHECK(mock_client.gcode_script_history().empty());
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: page subjects follow the classifier",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_effect("led_effect rainbow", "neopixel chamber_light", false);
    set_macros({party_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel chamber_light");

    CHECK(access.int_subject("led_page_lamp") == static_cast<int>(LampControl::PowerAndBrightness));
    CHECK(access.int_subject("led_page_white") == static_cast<int>(WhiteMode::WChannel));
    CHECK(access.int_subject("led_page_color_vis") == 1);
    CHECK(access.int_subject("led_page_list") == static_cast<int>(ListKind::Effects));
    CHECK(access.int_subject("led_chip_count") == 1);
    CHECK(access.str_subject("led_chip_label_0") == "Rainbow");
    CHECK(access.int_subject("led_swatch_count") == 7);

    access.activate("macro:Party");

    CHECK(access.int_subject("led_page_lamp") == static_cast<int>(LampControl::None));
    CHECK(access.int_subject("led_page_white") == static_cast<int>(WhiteMode::None));
    CHECK(access.int_subject("led_page_color_vis") == 0);
    CHECK(access.int_subject("led_page_list") == static_cast<int>(ListKind::Presets));
    CHECK(access.int_subject("led_chip_count") == 2);
    CHECK(access.str_subject("led_chip_label_0") == "Party");
    CHECK(access.str_subject("led_chip_label_1") == "Rainbow");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: tabs carry each device's display name",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");

    CHECK(access.int_subject("led_tab_count") == 2);
    CHECK(access.str_subject("led_tab_name_0") == "Chamber Light");
    CHECK(access.str_subject("led_tab_name_1") == "Lamp");
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: tab dots follow device state", "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    set_macros({lamp_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("");
    REQUIRE(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::Unknown));

    LedController::instance().update_from_status(
        {{"neopixel sb_leds", {{"color_data", {{0.5, 0.5, 0.5}}}}}});
    drain();

    CHECK(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::On));
    CHECK(access.int_subject("led_tab_dot_2") == static_cast<int>(PowerState::Unknown));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a state bump updates dots but not the slider",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    add_native("neopixel sb_leds", true, false);
    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.4, 0.4, 0.4, 0.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel chamber_light");
    REQUIRE(access.int_subject("led_page_brightness") == 40);

    ctrl.update_from_status({{"neopixel chamber_light", {{"color_data", {{0.9, 0.9, 0.9, 0.0}}}}},
                             {"neopixel sb_leds", {{"color_data", {{1.0, 1.0, 1.0}}}}}});
    drain();

    CHECK(access.int_subject("led_page_brightness") == 40);
    CHECK(access.str_subject("led_page_brightness_text") == "40%");
    CHECK(access.int_subject("led_tab_dot_1") == static_cast<int>(PowerState::On));

    // The next control acts at the brightness the slider shows.
    access.tap_swatch(0);
    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel chamber_light").decompose(base, pct, white);
    CHECK(pct == 40);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: controls act on the focused device only",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    access.tap_swatch(0);

    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel strip_a").decompose(base, pct, white);
    CHECK(within_rounding(base, LedController::instance().color_presets()[0]));
    CHECK(white == Catch::Approx(0.0).margin(0.001));
    CHECK_FALSE(LedController::instance().native().has_strip_color("neopixel strip_b"));
    CHECK(access.int_subject("led_selected_swatch") == 0);

    drain();
    mock_client.clear_gcode_script_history();
    access.tap_power();
    drain();

    CHECK(wire_mentions("strip_a"));
    CHECK_FALSE(wire_mentions("strip_b"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a swatch keeps the brightness and clears W",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedController::instance().update_from_status(
        {{"neopixel chamber_light", {{"color_data", {{0.0, 0.0, 0.0, 0.6}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel chamber_light");
    REQUIRE(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    access.tap_swatch(4);

    uint32_t base = 0;
    int pct = 0;
    double white = 0.0;
    sent("neopixel chamber_light").decompose(base, pct, white);
    CHECK(within_rounding(base, LedController::instance().color_presets()[4]));
    CHECK(pct == 60);
    CHECK(white == Catch::Approx(0.0).margin(0.001));
    CHECK(access.int_subject("led_page_white_sel") == -1);
    CHECK(access.int_subject("led_selected_swatch") == 4);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the white section sets W on RGBW",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel chamber_light");
    access.tap_white(static_cast<int>(WhiteTone::Neutral));

    auto c = sent("neopixel chamber_light");
    CHECK(c.w > 0.0);
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
    CHECK(access.int_subject("led_page_white_sel") == static_cast<int>(WhiteTone::Neutral));
    CHECK(access.int_subject("led_selected_swatch") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a single-channel strip dims through W",
                 "[led][overlay]") {
    add_native("led case_light", false, false);
    LedController::instance().update_from_status(
        {{"led case_light", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("led case_light");
    access.drag_brightness(15);

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(0.15).margin(0.001));
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
    CHECK(access.str_subject("led_page_brightness_text") == "15%");
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a level chip sets brightness on a white-only strip", "[led][overlay]") {
    add_native("led case_light", false, false);
    LedController::instance().update_from_status(
        {{"led case_light", {{"color_data", {{0.0, 0.0, 0.0, 1.0}}}}}});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("led case_light");
    REQUIRE(access.int_subject("led_page_list") == static_cast<int>(ListKind::LevelChips));
    REQUIRE(access.int_subject("led_page_level") == 100);

    access.tap_level(25);
    CHECK(access.int_subject("led_page_level") == 25);
    CHECK(sent("led case_light").w == Catch::Approx(0.25).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the None chip stops the focused strip's effect",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", true);
    add_effect("led_effect sparkle", "neopixel strip_b", true);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == 1);

    access.tap_effects_none();
    CHECK(access.int_subject("led_active_chip") == -1);
    drain();

    CHECK(wire_mentions("EFFECT=fire STOP=1"));
    CHECK_FALSE(wire_mentions("EFFECT=sparkle"));
    CHECK_FALSE(wire_mentions("EFFECT=breathe"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an effect chip activates that effect",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == -1);

    access.tap_list_chip(1);
    CHECK(access.int_subject("led_active_chip") == 1);
    drain();

    CHECK(wire_mentions("EFFECT=fire"));
    CHECK_FALSE(wire_mentions("EFFECT=breathe"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a macro preset chip runs that macro",
                 "[led][overlay]") {
    set_macros({party_macro()});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("macro:Party");
    access.tap_list_chip(1);
    drain();

    CHECK(wire_mentions("LED_RAINBOW"));
    CHECK_FALSE(wire_mentions("LED_PARTY"));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: an off white-only strip after a red look puts it all on W",
                 "[led][overlay]") {
    add_native("led case_light", false, false);
    auto& ctrl = LedController::instance();
    ctrl.set_last_color(0xFF4444);
    ctrl.set_last_white(0.0);
    ctrl.set_last_brightness(60);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("led case_light");
    REQUIRE_FALSE(ctrl.native().has_strip_color("led case_light"));
    access.tap_level(100);

    auto c = sent("led case_light");
    CHECK(c.w == Catch::Approx(1.0).margin(0.001));
    CHECK(c.r == Catch::Approx(0.0).margin(0.001));
    CHECK(c.g == Catch::Approx(0.0).margin(0.001));
    CHECK(c.b == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: an off RGB-only strip after a W-only look lights RGB white",
                 "[led][overlay]") {
    add_native("neopixel rgb", true, false);
    auto& ctrl = LedController::instance();
    ctrl.set_last_color(0);
    ctrl.set_last_white(1.0);
    ctrl.set_last_brightness(80);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel rgb");
    REQUIRE(access.int_subject("led_page_brightness") == 80);
    access.drag_brightness(50);

    auto c = sent("neopixel rgb");
    CHECK(c.r == Catch::Approx(0.5).margin(0.005));
    CHECK(c.g == Catch::Approx(0.5).margin(0.005));
    CHECK(c.b == Catch::Approx(0.5).margin(0.005));
    CHECK(c.w == Catch::Approx(0.0).margin(0.001));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an output pin's slider sets that pin",
                 "[led][overlay]") {
    add_native("neopixel chamber_light", true, true);
    LedStripInfo pin;
    pin.name = "Enclosure";
    pin.id = "output_pin enclosure";
    pin.backend = LedBackendType::OUTPUT_PIN;
    pin.supports_color = false;
    pin.supports_white = false;
    pin.is_pwm = true;
    LedController::instance().output_pin().add_pin(pin);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("output_pin enclosure");
    access.drag_brightness(40);
    drain();

    CHECK(wire_mentions("SET_PIN PIN=enclosure VALUE=0.4000"));
    CHECK_FALSE(wire_mentions("SET_LED"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: macro buttons run the focused device's macros",
                 "[led][overlay]") {
    LedMacroInfo toggle;
    toggle.display_name = "Toggle";
    toggle.type = MacroLedType::TOGGLE;
    toggle.toggle_macro = "LIGHT_TOGGLE";
    set_macros({lamp_macro(), toggle});

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("macro:Lamp");
    CHECK(access.str_subject("led_page_note") == "ON: LIGHTS_ON | OFF: LIGHTS_OFF");

    access.tap_macro_on();
    drain();
    CHECK(wire_mentions("LIGHTS_ON"));
    CHECK_FALSE(wire_mentions("LIGHTS_OFF"));

    mock_client.clear_gcode_script_history();
    access.tap_macro_off();
    drain();
    CHECK(wire_mentions("LIGHTS_OFF"));
    CHECK_FALSE(wire_mentions("LIGHTS_ON"));

    access.activate("macro:Toggle");
    CHECK(access.str_subject("led_page_note") == "TOGGLE: LIGHT_TOGGLE");
    mock_client.clear_gcode_script_history();
    access.tap_macro_toggle();
    drain();
    CHECK(wire_mentions("LIGHT_TOGGLE"));
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: the active effect chip follows status",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    REQUIRE(access.int_subject("led_active_chip") == -1);

    LedController::instance().update_from_status({{"led_effect fire", {{"enabled", true}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == 1);

    LedController::instance().update_from_status({{"led_effect fire", {{"enabled", false}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: an unrelated frame keeps a tapped effect chip",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_native("neopixel strip_b", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    LedController::instance().update_from_status(
        {{"neopixel strip_b", {{"color_data", {{1.0, 0.0, 0.0}}}}}});
    drain();

    CHECK(access.int_subject("led_active_chip") == 1);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a frame naming another running effect replaces the tapped chip",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    LedController::instance().update_from_status({{"led_effect breathe", {{"enabled", true}}}});
    drain();

    CHECK(access.int_subject("led_active_chip") == 0);
}

TEST_CASE_METHOD(LedApplyColorFixture,
                 "overlay: a confirming frame ends the pending chip; later frames follow status",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect breathe", "neopixel strip_a", false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    access.tap_list_chip(1);

    auto& ctrl = LedController::instance();
    ctrl.update_from_status({{"led_effect fire", {{"enabled", true}}}});
    drain();
    REQUIRE(access.int_subject("led_active_chip") == 1);

    ctrl.update_from_status({{"led_effect fire", {{"enabled", false}}}});
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}

TEST_CASE_METHOD(LedApplyColorFixture, "overlay: a tapped effect chip no frame answers times out",
                 "[led][overlay]") {
    add_native("neopixel strip_a", true, false);
    add_effect("led_effect fire", "neopixel strip_a", false);

    helix::PrinterState ps;
    LedControlOverlayTestAccess access(ps);
    access.activate("neopixel strip_a");
    access.tap_list_chip(0);
    process_lvgl(1000);
    REQUIRE(access.int_subject("led_active_chip") == 0);

    process_lvgl(4000);
    drain();
    CHECK(access.int_subject("led_active_chip") == -1);
}
