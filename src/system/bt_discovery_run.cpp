// SPDX-License-Identifier: GPL-3.0-or-later

#include "bt_discovery_run.h"

#include "bluetooth_loader.h"

#include <spdlog/spdlog.h>

#include <system_error>
#include <thread>

namespace helix::bluetooth {

SharedContext::~SharedContext() {
    auto& loader = BluetoothLoader::instance();
    if (auto* ctx = ctx_.load(); ctx && loader.deinit)
        loader.deinit(ctx);
}

helix_bt_context* SharedContext::get() {
    if (auto* ctx = ctx_.load())
        return ctx;
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (auto* ctx = ctx_.load())
        return ctx;
    auto& loader = BluetoothLoader::instance();
    if (!loader.init)
        return nullptr;
    auto* ctx = loader.init();
    ctx_.store(ctx);
    return ctx;
}

struct DiscoveryRun::State {
    std::atomic<bool> alive{true};
    int cancel = 0; // the plugin reads it atomically; ends this scan only
    std::shared_ptr<SharedContext> ctx;
    LifetimeToken token;
    Callbacks callbacks;

    State(std::shared_ptr<SharedContext> c, LifetimeToken t, Callbacks cbs)
        : ctx(std::move(c)), token(std::move(t)), callbacks(std::move(cbs)) {}
};

void DiscoveryRun::report_device(const helix_bt_device* dev, void* user_data) {
    const auto& state = *static_cast<std::shared_ptr<State>*>(user_data);
    if (!dev || !state->alive.load())
        return;
    if (state->callbacks.accept && !state->callbacks.accept(*dev))
        return;

    DiscoveredDevice found;
    found.mac = dev->mac ? dev->mac : "";
    found.name = dev->name ? dev->name : "Unknown";
    found.paired = dev->paired;
    found.is_ble = dev->is_ble;
    found.is_scanner = dev->is_scanner;

    state->token.defer("DiscoveryRun::device", [state, found] {
        if (state->alive.load() && state->callbacks.on_device)
            state->callbacks.on_device(found);
    });
}

bool DiscoveryRun::start(std::shared_ptr<SharedContext> ctx, int timeout_ms, LifetimeToken token,
                         Callbacks callbacks) {
    if (state_)
        state_->alive.store(false);
    auto state = std::make_shared<State>(std::move(ctx), std::move(token), std::move(callbacks));

    // Detached spawns are wrapped: EAGAIN under thread exhaustion throws (#724).
    try {
        std::thread([state, timeout_ms]() mutable {
            auto& loader = BluetoothLoader::instance();
            helix_bt_context* bt = state->ctx->get();
            if (bt && loader.discover)
                loader.discover(bt, timeout_ms, &DiscoveryRun::report_device, &state,
                                &state->cancel);

            const bool context_ok = bt != nullptr;
            state->token.defer("DiscoveryRun::finished", [state, context_ok] {
                if (state->alive.load() && state->callbacks.on_finished)
                    state->callbacks.on_finished(context_ok);
            });
        }).detach();
    } catch (const std::system_error& e) {
        spdlog::error("[BluetoothDiscovery] Failed to spawn discovery thread: {}", e.what());
        return false;
    }

    state_ = std::move(state);
    return true;
}

void DiscoveryRun::cancel() {
    if (!state_)
        return;
    state_->alive.store(false);
    __atomic_store_n(&state_->cancel, 1, __ATOMIC_RELEASE);
    state_.reset();
}

} // namespace helix::bluetooth
