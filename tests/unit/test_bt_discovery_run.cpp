// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later
//
// DiscoveryRun and SharedContext against fake plugin entry points swapped into
// BluetoothLoader. The fake discover() blocks until the test releases it, which is
// what a real 15 s scan does to the worker thread.

#include "../lvgl_test_fixture.h"
#include "bluetooth_loader.h"
#include "bt_discovery_run.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../catch_amalgamated.hpp"

using helix::bluetooth::BluetoothLoader;
using helix::bluetooth::DiscoveredDevice;
using helix::bluetooth::DiscoveryRun;
using helix::bluetooth::SharedContext;

namespace {

int g_fake_ctx_storage = 0;
helix_bt_context* const FAKE_CTX = reinterpret_cast<helix_bt_context*>(&g_fake_ctx_storage);

struct FakePlugin {
    std::mutex mu;
    std::condition_variable cv;
    int inits = 0;
    int deinits = 0;
    int stops = 0;
    int discovers_entered = 0;
    int discovers_released = 0; // discover call N returns once this exceeds N
    bool init_fails = false;
    std::vector<std::string> names_per_call; // device name reported by call N on release
};

FakePlugin* g_fake = nullptr;

extern "C" helix_bt_context* fake_init() {
    std::lock_guard<std::mutex> lock(g_fake->mu);
    ++g_fake->inits;
    return g_fake->init_fails ? nullptr : FAKE_CTX;
}

extern "C" void fake_deinit(helix_bt_context*) {
    std::lock_guard<std::mutex> lock(g_fake->mu);
    ++g_fake->deinits;
}

extern "C" void fake_stop(helix_bt_context*) {
    std::lock_guard<std::mutex> lock(g_fake->mu);
    ++g_fake->stops;
}

extern "C" int fake_discover(helix_bt_context*, int, helix_bt_discover_cb cb, void* user_data) {
    std::string name;
    {
        std::unique_lock<std::mutex> lock(g_fake->mu);
        int call = g_fake->discovers_entered++;
        g_fake->cv.notify_all();
        g_fake->cv.wait(lock, [&] { return g_fake->discovers_released > call; });
        name = g_fake->names_per_call.at(static_cast<size_t>(call));
    }
    helix_bt_device dev = {};
    dev.mac = "AA:BB:CC:DD:EE:FF";
    dev.name = name.c_str();
    dev.is_scanner = true;
    cb(&dev, user_data);
    helix_bt_device skipped = {};
    skipped.mac = "11:22:33:44:55:66";
    skipped.name = "not a scanner";
    cb(&skipped, user_data);
    return 0;
}

/// Swaps the fake plugin into the loader and restores the real pointers afterwards.
struct ScopedFakePlugin {
    FakePlugin fake;
    BluetoothLoader& loader = BluetoothLoader::instance();
    helix_bt_init_fn init = loader.init;
    helix_bt_deinit_fn deinit = loader.deinit;
    helix_bt_discover_fn discover = loader.discover;
    helix_bt_stop_discovery_fn stop = loader.stop_discovery;

    ScopedFakePlugin() {
        g_fake = &fake;
        loader.init = &fake_init;
        loader.deinit = &fake_deinit;
        loader.discover = &fake_discover;
        loader.stop_discovery = &fake_stop;
    }
    ~ScopedFakePlugin() {
        loader.init = init;
        loader.deinit = deinit;
        loader.discover = discover;
        loader.stop_discovery = stop;
        g_fake = nullptr;
    }

    void wait_entered(int n) {
        std::unique_lock<std::mutex> lock(fake.mu);
        fake.cv.wait_for(lock, std::chrono::seconds(5),
                         [&] { return fake.discovers_entered >= n; });
    }
    void release(int n) {
        std::lock_guard<std::mutex> lock(fake.mu);
        fake.discovers_released = n;
        fake.cv.notify_all();
    }
    int count(int FakePlugin::*field) {
        std::lock_guard<std::mutex> lock(fake.mu);
        return fake.*field;
    }
};

struct Seen {
    std::vector<std::string> devices;
    std::vector<bool> finished;
};

DiscoveryRun::Callbacks record_into(Seen& seen) {
    DiscoveryRun::Callbacks cbs;
    cbs.accept = [](const helix_bt_device& d) { return d.is_scanner; };
    cbs.on_device = [&seen](const DiscoveredDevice& d) { seen.devices.push_back(d.name); };
    cbs.on_finished = [&seen](bool ok) { seen.finished.push_back(ok); };
    return cbs;
}

} // namespace

TEST_CASE("DiscoveryRun reports accepted devices on the UI thread, then finishes",
          "[bt][discovery_run][slow]") {
    LVGLTestFixture fixture;
    ScopedFakePlugin plugin;
    plugin.fake.names_per_call = {"Scanner One"};
    helix::AsyncLifetimeGuard owner;
    Seen seen;

    DiscoveryRun run;
    auto ctx = std::make_shared<SharedContext>();
    REQUIRE(run.start(ctx, 15000, owner.token(), record_into(seen)));
    plugin.release(1);

    REQUIRE(fixture.wait_until([&] { return !seen.finished.empty(); }));
    CHECK(seen.devices == std::vector<std::string>{"Scanner One"});
    CHECK(seen.finished == std::vector<bool>{true});
}

TEST_CASE("A cancelled scan stays silent while the next scan reports its own devices",
          "[bt][discovery_run][slow]") {
    LVGLTestFixture fixture;
    ScopedFakePlugin plugin;
    plugin.fake.names_per_call = {"Old Scanner", "New Scanner"};
    helix::AsyncLifetimeGuard owner;
    Seen seen;

    DiscoveryRun run;
    auto ctx = std::make_shared<SharedContext>();
    REQUIRE(run.start(ctx, 15000, owner.token(), record_into(seen)));
    plugin.wait_entered(1);

    run.cancel();
    CHECK(plugin.count(&FakePlugin::stops) == 1);
    REQUIRE(run.start(ctx, 15000, owner.token(), record_into(seen)));
    plugin.wait_entered(2);
    REQUIRE(plugin.count(&FakePlugin::discovers_entered) == 2);

    plugin.release(2); // both scans report and return
    REQUIRE(fixture.wait_until([&] { return !seen.finished.empty(); }));
    // Let the cancelled worker's queued callbacks drain too, had it posted any.
    fixture.wait_until([] { return false; }, 100);

    CHECK(seen.devices == std::vector<std::string>{"New Scanner"});
    CHECK(seen.finished == std::vector<bool>{true});
}

TEST_CASE("SharedContext is created once and deinited only after the last worker lets go",
          "[bt][discovery_run][slow]") {
    LVGLTestFixture fixture;
    ScopedFakePlugin plugin;
    plugin.fake.names_per_call = {"Scanner"};
    helix::AsyncLifetimeGuard owner;
    Seen seen;

    DiscoveryRun run;
    auto ctx = std::make_shared<SharedContext>();
    REQUIRE(run.start(ctx, 15000, owner.token(), record_into(seen)));
    plugin.wait_entered(1);
    CHECK(ctx->get() == FAKE_CTX);
    CHECK(plugin.count(&FakePlugin::inits) == 1);

    // The owner lets go mid-scan: the worker's reference keeps the context alive.
    run.cancel();
    ctx.reset();
    CHECK(plugin.count(&FakePlugin::deinits) == 0);

    plugin.release(1);
    REQUIRE(fixture.wait_until([&] { return plugin.count(&FakePlugin::deinits) == 1; }));
    CHECK(seen.devices.empty());
}

TEST_CASE("DiscoveryRun reports a failed context and never scans", "[bt][discovery_run][slow]") {
    LVGLTestFixture fixture;
    ScopedFakePlugin plugin;
    plugin.fake.init_fails = true;
    helix::AsyncLifetimeGuard owner;
    Seen seen;

    DiscoveryRun run;
    REQUIRE(run.start(std::make_shared<SharedContext>(), 15000, owner.token(), record_into(seen)));

    REQUIRE(fixture.wait_until([&] { return !seen.finished.empty(); }));
    CHECK(seen.finished == std::vector<bool>{false});
    CHECK(plugin.count(&FakePlugin::discovers_entered) == 0);
}

TEST_CASE("DiscoveryRun callbacks stop when their owner is destroyed",
          "[bt][discovery_run][slow]") {
    LVGLTestFixture fixture;
    ScopedFakePlugin plugin;
    plugin.fake.names_per_call = {"Scanner"};
    Seen seen;

    DiscoveryRun run;
    {
        helix::AsyncLifetimeGuard owner;
        REQUIRE(
            run.start(std::make_shared<SharedContext>(), 15000, owner.token(), record_into(seen)));
        plugin.wait_entered(1);
    }
    plugin.release(1);
    run.cancel();
    REQUIRE(fixture.wait_until([&] { return plugin.count(&FakePlugin::deinits) == 1; }));
    fixture.wait_until([] { return false; }, 100);

    CHECK(seen.devices.empty());
    CHECK(seen.finished.empty());
}
