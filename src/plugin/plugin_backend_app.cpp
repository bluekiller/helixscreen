// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "app_globals.h"
#include "http_executor.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "plugin_backend.h"

#include <atomic>

namespace helix::plugin {

namespace {

RpcResult failure(std::string message) {
    RpcResult r;
    r.error = std::move(message);
    return r;
}

RpcResult success(json value = json()) {
    return RpcResult{true, std::move(value), {}};
}

bool is_transfer_root(const std::string& root) {
    return root == "gcodes" || root == "config";
}

constexpr const char* kNotConnected = "Moonraker is not connected";

} // namespace

PluginBackend make_app_backend() {
    PluginBackend b;

    b.gcode = [](const std::string& script, RpcCallback cb) {
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->execute_gcode(
            script, [cb]() { cb(success()); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.call = [](const std::string& method, const json& params, RpcCallback cb) {
        auto* client = get_moonraker_client();
        if (!client)
            return cb(failure(kNotConnected));
        client->send_jsonrpc(
            method, params, [cb](const json& result) { cb(success(result)); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.upload = [](const std::string& root, const std::string& path, const std::string& content,
                  RpcCallback cb) {
        if (!is_transfer_root(root))
            return cb(failure("root must be 'gcodes' or 'config'"));
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->transfers().upload_file(
            root, path, content, [cb]() { cb(success()); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    b.download = [](const std::string& root, const std::string& path, RpcCallback cb) {
        if (!is_transfer_root(root))
            return cb(failure("root must be 'gcodes' or 'config'"));
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->transfers().download_file(
            root, path, [cb](const std::string& body) { cb(success(json(body))); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    // ponytail: one blocking libhv request per call on the fast pool; a streaming client when a
    // plugin needs bodies too large to hold in memory.
    b.http = [](const std::string& method, const std::string& url, const std::string& body,
                const json& headers, uint32_t timeout_ms, RpcCallback cb) {
        http::HttpExecutor::fast().submit([=]() {
            auto req = std::make_shared<HttpRequest>();
            req->method = method == "POST" ? HTTP_POST : HTTP_GET;
            req->url = url;
            req->timeout = static_cast<int>((timeout_ms + 999) / 1000);
            req->body = body;
            req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
            if (headers.is_object()) {
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    if (it.value().is_string())
                        req->headers[it.key()] = it.value().get<std::string>();
                }
            }
            auto resp = requests::request(req);
            if (!resp)
                return cb(failure("request to " + url + " failed"));
            cb(success(json{{"status", resp->status_code}, {"body", resp->body}}));
        });
    };

    b.on_notify = [](const std::string& method,
                     std::function<void(const json&)> handler) -> std::function<void()> {
        auto* api = get_moonraker_api();
        if (!api)
            return [] {};
        static std::atomic<unsigned> next_id{0};
        std::string name = "lua_plugin_" + std::to_string(next_id++);
        api->register_method_callback(method, name, std::move(handler));
        return [method, name]() {
            if (auto* a = get_moonraker_api())
                a->unregister_method_callback(method, name);
        };
    };

    return b;
}

} // namespace helix::plugin

#endif // HELIX_HAS_PLUGINS
