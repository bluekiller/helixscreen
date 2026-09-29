// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#if HELIX_HAS_PLUGINS

#include "app_globals.h"
#include "config.h"
#include "http_executor.h"
#include "hv/requests.h"
#include "i_moonraker_api.h"
#include "i_moonraker_client.h"
#include "moonraker_error.h"
#include "plugin_backend.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <netdb.h>
#include <sys/socket.h>

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
constexpr const char* kPrinterHostRefused = "requests to the printer host are not allowed";

bool is_loopback_ip(const std::string& ip) {
    return ip.rfind("127.", 0) == 0 || ip == "::1" || ip.rfind("::ffff:127.", 0) == 0;
}

/// Every address `host` resolves to, in numeric form; empty when it does not resolve.
std::vector<std::string> resolve_host_ips(const std::string& host) {
    std::vector<std::string> ips;
    if (host.empty())
        return ips;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0)
        return ips;
    char buf[INET6_ADDRSTRLEN];
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, buf, sizeof(buf), nullptr, 0,
                        NI_NUMERICHOST) == 0)
            ips.emplace_back(buf);
    }
    freeaddrinfo(result);
    return ips;
}

std::string configured_moonraker_host() {
    if (helix::Config* cfg = helix::Config::get_instance())
        return cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    return "localhost";
}

} // namespace

std::string url_host(const std::string& url) {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return {};
    size_t authority = scheme_end + 3;
    size_t end = url.find_first_of("/?#", authority);
    std::string host_port =
        url.substr(authority, end == std::string::npos ? std::string::npos : end - authority);
    if (auto at = host_port.rfind('@'); at != std::string::npos)
        host_port = host_port.substr(at + 1);
    if (host_port.empty())
        return {};
    if (host_port.front() == '[') {
        auto close = host_port.find(']');
        if (close == std::string::npos)
            return {};
        return host_port.substr(1, close - 1);
    }
    if (auto colon = host_port.find(':'); colon != std::string::npos)
        host_port = host_port.substr(0, colon);
    return host_port;
}

bool is_forbidden_http_target(const std::vector<std::string>& resolved_ips,
                              const std::vector<std::string>& printer_ips) {
    for (const auto& ip : resolved_ips) {
        if (is_loopback_ip(ip))
            return true;
        for (const auto& p : printer_ips) {
            if (ip == p)
                return true;
        }
    }
    return false;
}

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

    b.download = [](const std::string& root, const std::string& path, size_t max_bytes,
                    RpcCallback cb) {
        if (!is_transfer_root(root))
            return cb(failure("root must be 'gcodes' or 'config'"));
        auto* api = get_moonraker_api();
        if (!api)
            return cb(failure(kNotConnected));
        api->transfers().download_file_partial(
            root, path, max_bytes, [cb](const std::string& body) { cb(success(json(body))); },
            [cb](const MoonrakerError& e) { cb(failure(e.message)); });
    };

    // One blocking libhv request per call on the slow pool: a plugin body can be large and
    // must never crowd Moonraker's own REST traffic. The body is streamed through http_cb
    // and cut off at max_body, the same mid-body abort the partial download uses, so a
    // huge response never occupies a worker for the full transfer.
    b.http = [](const std::string& method, const std::string& url, const std::string& body,
                const json& headers, uint32_t timeout_ms, size_t max_body, RpcCallback cb) {
        http::HttpExecutor::slow().submit([=]() {
            // Resolving both hosts blocks on DNS, so the check runs here, off the main thread.
            // A followed redirect would land on a host this check never saw, so the plugin's
            // requests do not follow redirects.
            if (is_forbidden_http_target(resolve_host_ips(url_host(url)),
                                         resolve_host_ips(configured_moonraker_host())))
                return cb(failure(kPrinterHostRefused));
            auto req = std::make_shared<HttpRequest>();
            req->method = method == "POST" ? HTTP_POST : HTTP_GET;
            req->url = url;
            req->timeout = static_cast<int>((timeout_ms + 999) / 1000);
            req->body = body;
            req->redirect = 0;
            req->headers["User-Agent"] = std::string("HelixScreen/") + HELIX_VERSION;
            if (headers.is_object()) {
                for (auto it = headers.begin(); it != headers.end(); ++it) {
                    if (it.value().is_string())
                        req->headers[it.key()] = it.value().get<std::string>();
                }
            }
            auto out = std::make_shared<std::string>();
            req->http_cb = [req, out, max_body](HttpMessage*, http_parser_state state,
                                                const char* data, size_t size) {
                if (state != HP_BODY || data == nullptr || size == 0)
                    return;
                if (out->size() >= max_body) { // keep the cancel armed on every later chunk
                    req->Cancel();
                    return;
                }
                out->append(data, std::min(size, max_body - out->size()));
                if (out->size() >= max_body)
                    req->Cancel();
            };
            auto resp = requests::request(req);
            if (!resp)
                return cb(failure("request to " + url + " failed"));
            cb(success(json{{"status", resp->status_code}, {"body", *out}}));
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
