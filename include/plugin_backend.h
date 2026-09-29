// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    /// `max_bytes` caps the transfer: the fetch is stopped once that much body has arrived.
    std::function<void(const std::string& root, const std::string& path, size_t max_bytes,
                       RpcCallback)>
        download;
    /// `value` is `{"status": int, "body": string}`; `ok` is false only when no response
    /// arrived. `max_body` caps the response body; the connection is cut once that much
    /// has arrived.
    std::function<void(const std::string& method, const std::string& url, const std::string& body,
                       const json& headers, uint32_t timeout_ms, size_t max_body, RpcCallback)>
        http;
    /// Registers a handler for a Moonraker notification method (it receives the whole
    /// message) and returns the function that unregisters it.
    std::function<std::function<void()>(const std::string& notify_method,
                                        std::function<void(const json& msg)>)>
        on_notify;
};

PluginBackend make_app_backend();

/// The host of `scheme://[user[:password]@]host[:port]/...`, without the port and without
/// IPv6 brackets. Empty when the URL has no authority.
std::string url_host(const std::string& url);

/// True when any resolved target address is loopback (127.0.0.0/8, ::1, ::ffff:127.x) or
/// equals a resolved address of the printer's own host: those addresses carry the control
/// that the gcode/moonraker_write permissions gate, so plugin http may never reach them.
bool is_forbidden_http_target(const std::vector<std::string>& resolved_ips,
                              const std::vector<std::string>& printer_ips);

} // namespace helix::plugin
