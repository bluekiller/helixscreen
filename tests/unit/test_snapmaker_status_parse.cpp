// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "snapmaker_status_parse.h"

#include "../catch_amalgamated.hpp"

using json = nlohmann::json;
using namespace helix;

TEST_CASE("Snapmaker status parse reads only the extruder fields a frame carries",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_extruder_delta(json{{"temperature", 215.5}});
    CHECK_FALSE(d.state);
    CHECK_FALSE(d.park_pin);
    CHECK_FALSE(d.active_pin);
    CHECK_FALSE(d.activating_move);
    CHECK_FALSE(d.switch_count);
    CHECK_FALSE(d.extruder_offset[0]);

    const auto named = snapmaker::parse_extruder_delta(
        json{{"park_pin", false}, {"extruder_offset", json::array({1.5})}});
    REQUIRE(named.park_pin);
    CHECK(*named.park_pin == false);
    REQUIRE(named.extruder_offset[0]);
    CHECK_FALSE(named.extruder_offset[1]);
}

TEST_CASE("Snapmaker status parse refuses fields of the wrong type", "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_extruder_delta(
        json{{"state", 3}, {"park_pin", "yes"}, {"switch_count", "7"}, {"extruder_offset", "x"}});
    CHECK_FALSE(d.state);
    CHECK_FALSE(d.park_pin);
    CHECK_FALSE(d.switch_count);
    CHECK_FALSE(d.extruder_offset[0]);
}

TEST_CASE("Snapmaker status parse files each extruder object under its tool",
          "[snapmaker][status_parse]") {
    const auto d = snapmaker::parse_status(json{{"extruder", json{{"state", "ACTIVE"}}},
                                                {"extruder2", json{{"state", "PARKED"}}},
                                                {"extruder3", "not an object"},
                                                {"toolhead", json{{"extruder", "extruder2"}}}});
    REQUIRE(d.extruders[0]);
    CHECK(*d.extruders[0]->state == "ACTIVE");
    CHECK_FALSE(d.extruders[1]);
    REQUIRE(d.extruders[2]);
    CHECK(*d.extruders[2]->state == "PARKED");
    CHECK_FALSE(d.extruders[3]);
    CHECK(d.toolhead_extruder == "extruder2");

    CHECK_FALSE(snapmaker::parse_status(json::object()).toolhead_extruder);
}
