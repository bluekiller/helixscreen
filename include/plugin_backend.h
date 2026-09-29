// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "hv/json.hpp"

namespace helix::plugin {

using json = nlohmann::json;

struct RpcResult {
    bool ok = false;
    json value;
    std::string error;
};

/// May run on any thread.
using RpcCallback = std::function<void(RpcResult)>;

/// Everything Lua bindings may ask of the app. Bindings never reach Moonraker or the network
/// any other way, which is what lets tests substitute a fake.
struct PluginBackend {
    std::function<void(const std::string& script, RpcCallback)> gcode;
    std::function<void(const std::string& method, const json& params, RpcCallback)> call;
    /// `root` is `gcodes` or `config`.
    std::function<void(const std::string& root, const std::string& path, const std::string& content,
                       RpcCallback)>
        upload;
    /// `root` is `gcodes` or `config`; `value` is a JSON string holding the file content.
    std::function<void(const std::string& root, const std::string& path, RpcCallback)> download;
    /// `value` is `{"status": int, "body": string}`; `ok` is false only when no response arrived.
    std::function<void(const std::string& method, const std::string& url, const std::string& body,
                       const json& headers, uint32_t timeout_ms, RpcCallback)>
        http;
    /// Registers a handler for a Moonraker notification method (it receives the whole
    /// message) and returns the function that unregisters it.
    std::function<std::function<void()>(const std::string& notify_method,
                                        std::function<void(const json& msg)>)>
        on_notify;
};

PluginBackend make_app_backend();

} // namespace helix::plugin
