// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui_update_queue.h"

#include "../lvgl_test_fixture.h"
#include "../test_helpers/update_queue_test_access.h"
#include "config.h"
#include "grid_layout.h"
#include "led/led_controller.h"
#include "led/led_devices.h"
#include "moonraker_api_mock.h"
#include "moonraker_client_mock.h"
#include "panel_widget_manager.h"
#include "printer_state.h"
#include "src/ui/panel_widgets/led_widget.h"

#include <algorithm>

#include "../catch_amalgamated.hpp"
#include "hv/json.hpp"

using namespace helix;
using namespace helix::led;

namespace {
struct LedWidgetFixture : public LVGLTestFixture {
    MoonrakerClientMock client{MoonrakerClientMock::PrinterType::VORON_24};
    PrinterState ps;
    std::unique_ptr<MoonrakerAPIMock> api;

    LedWidgetFixture() {
        ps.init_subjects(false);
        ps.set_klippy_state_sync(KlippyState::READY);
        api = std::make_unique<MoonrakerAPIMock>(client, ps);
        auto& ctrl = LedController::instance();
        ctrl.deinit();
        ctrl.init(api.get(), &client);
        for (const char* id : {"neopixel chamber_light", "neopixel sb_leds"}) {
            LedStripInfo s;
            s.id = id;
            s.name = id;
            s.backend = LedBackendType::NATIVE;
            s.supports_color = true;
            s.supports_white = true;
            ctrl.native().add_strip(s);
        }
        LedMacroInfo lamp;
        lamp.display_name = "Lamp";
        lamp.type = MacroLedType::TOGGLE;
        lamp.toggle_macro = "LIGHT_TOGGLE";
        LedMacroInfo party;
        party.display_name = "Party";
        party.type = MacroLedType::PRESET;
        party.presets = {"LED_PARTY"};
        ctrl.set_configured_macros({lamp, party});
        ctrl.rebuild_macro_backend();
    }
    ~LedWidgetFixture() override {
        helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
        LedController::instance().deinit();
    }

    std::string name_of(const std::string& instance_id) {
        return lv_subject_get_string(
            lv_xml_get_subject(nullptr, (instance_id + "_led_name").c_str()));
    }
};

/// A home layout holding exactly @p widgets on its one page, restored on exit.
class ScopedHomeLayout {
  public:
    explicit ScopedHomeLayout(const nlohmann::json& widgets) {
        auto* cfg = Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        const nlohmann::json* prior = cfg->try_get_json(key);
        had_prior_ = prior != nullptr;
        if (had_prior_) {
            prior_ = *prior;
        }
        cfg->set<nlohmann::json>(key, {{"main_page_index", 0},
                                       {"next_page_id", 1},
                                       {"pages", {{{"id", "main"}, {"widgets", widgets}}}}});
        PanelWidgetManager::instance().clear_panel_config("home");
    }
    ~ScopedHomeLayout() {
        auto* cfg = Config::get_instance();
        const std::string key = cfg->df() + "panel_widgets/home";
        cfg->set<nlohmann::json>(key, had_prior_ ? prior_ : nlohmann::json::object());
        cfg->set(cfg->df() + LIGHT_BUTTON_PENDING_PATH, nlohmann::json());
        PanelWidgetManager::instance().clear_panel_config("home");
    }
    ScopedHomeLayout(const ScopedHomeLayout&) = delete;
    ScopedHomeLayout& operator=(const ScopedHomeLayout&) = delete;

  private:
    bool had_prior_ = false;
    nlohmann::json prior_;
};

nlohmann::json placed_light(const std::string& id, int col) {
    return {{"id", id}, {"enabled", true}, {"col", col},
            {"row", 0}, {"colspan", 2},    {"rowspan", 2}};
}

size_t scripts_naming(const MoonrakerClientMock& client, const std::string& what) {
    const auto& h = client.gcode_script_history();
    return static_cast<size_t>(std::count_if(h.begin(), h.end(), [&](const std::string& s) {
        return s.find(what) != std::string::npos;
    }));
}
} // namespace

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: config round trip", "[led][light_button]") {
    LedWidget w("led:1", ps, api.get());
    w.set_config({{"led", "neopixel sb_leds"}});
    CHECK(w.light_key() == "neopixel sb_leds");
    CHECK(w.targets() == std::vector<std::string>{"neopixel sb_leds"});
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: unset means the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: a vanished device resolves to the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "neopixel gone"}});
    CHECK(w.targets() == std::vector<std::string>{"neopixel chamber_light"});
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: All lights toggles every switchable device",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "all"}});
    const auto t = w.targets();
    CHECK(t == LedController::instance().switchable_ids());
    CHECK(std::find(t.begin(), t.end(), "macro:Party") == t.end());
    CHECK(w.overlay_device() == "neopixel chamber_light");
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the picker offers no PRESET device",
                 "[led][light_button]") {
    std::vector<std::string> ids;
    for (const auto& d : light_picker_devices()) {
        ids.push_back(d.id);
    }
    CHECK(std::find(ids.begin(), ids.end(), "macro:Party") == ids.end());
    CHECK(ids == LedController::instance().switchable_ids());
}

TEST_CASE("light_tile_is_wide: two cells or more", "[led][light_button]") {
    CHECK_FALSE(light_tile_is_wide(helix::GridLayout::TRACKS_PER_CELL));
    CHECK_FALSE(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL - 1));
    CHECK(light_tile_is_wide(2 * helix::GridLayout::TRACKS_PER_CELL));
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: a fresh button names and toggles the chamber light",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config(nlohmann::json::object());
    lv_obj_t* root = lv_obj_create(test_screen());
    lv_obj_t* button = lv_obj_create(root);
    lv_obj_add_event_cb(button, LedWidget::light_toggle_cb, LV_EVENT_CLICKED, nullptr);
    w.attach(root, test_screen());

    CHECK(name_of("led") == "Chamber Light");

    client.clear_gcode_script_history();
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    helix::ui::UpdateQueueTestAccess::drain_all(helix::ui::UpdateQueue::instance());
    CHECK(scripts_naming(client, "chamber_light") > 0);
    CHECK(scripts_naming(client, "sb_leds") == 0);
    w.detach();
}

TEST_CASE_METHOD(LedWidgetFixture, "LedWidget: the name follows the chosen device",
                 "[led][light_button]") {
    LedWidget w("led", ps, api.get());
    w.set_config({{"led", "neopixel sb_leds"}});
    w.attach(lv_obj_create(test_screen()), test_screen());
    CHECK(name_of("led") == "Sb LEDs");
    w.detach();

    LedWidget all("led:1", ps, api.get());
    all.set_config({{"led", "all"}});
    all.attach(lv_obj_create(test_screen()), test_screen());
    CHECK(name_of("led:1") == "All lights");
    all.detach();
}

TEST_CASE_METHOD(LedWidgetFixture,
                 "LedWidget: a staged selection lands in every unset home light button",
                 "[led][light_button]") {
    ScopedHomeLayout layout(
        nlohmann::json::array({placed_light("led", 0), placed_light("led:1", 2)}));
    auto* cfg = Config::get_instance();
    cfg->set(cfg->df() + LIGHT_BUTTON_PENDING_PATH, std::string("neopixel sb_leds"));

    LedWidget first("led", ps, api.get());
    LedWidget second("led:1", ps, api.get());
    first.set_panel_id("home");
    second.set_panel_id("home");
    first.set_config(nlohmann::json::object());
    second.set_config(nlohmann::json::object());

    first.attach(lv_obj_create(test_screen()), test_screen());
    second.attach(lv_obj_create(test_screen()), test_screen());

    CHECK(first.light_key() == "neopixel sb_leds");
    CHECK(second.light_key() == "neopixel sb_leds");
    CHECK(first.targets() == std::vector<std::string>{"neopixel sb_leds"});
    CHECK(cfg->try_get_json(cfg->df() + LIGHT_BUTTON_PENDING_PATH)->is_null());
    first.detach();
    second.detach();
}

TEST_CASE("light_icon_look: lit by any on target, dark otherwise", "[led][light_button]") {
    DeviceState off{PowerState::Off, 0, 0xFF0000, true};
    DeviceState unknown;
    DeviceState dim_red{PowerState::On, 20, 0xFF0000, true};
    DeviceState bright_plain{PowerState::On, 80, 0xFFFFFF, false};

    CHECK(light_icon_look({unknown, unknown}).unknown);
    CHECK(light_icon_look({unknown, unknown}).brightness == 0);

    const auto dark = light_icon_look({off, unknown});
    CHECK_FALSE(dark.unknown);
    CHECK(dark.brightness == 0);

    const auto lit = light_icon_look({bright_plain, dim_red, off});
    CHECK(lit.brightness == 80);
    CHECK(lit.has_rgb);
    CHECK(lit.rgb == 0xFF0000);

    CHECK_FALSE(light_icon_look({bright_plain}).has_rgb);
}
