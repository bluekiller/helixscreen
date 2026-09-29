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
#include <ifaddrs.h>
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

/// `ip` in the form addresses are compared in: an IPv4-mapped IPv6 address as plain IPv4,
/// with no IPv6 zone suffix.
std::string canonical_ip(std::string ip) {
    if (auto pct = ip.find('%'); pct != std::string::npos)
        ip.resize(pct);
    if (ip.rfind("::ffff:", 0) == 0 && ip.find('.') != std::string::npos)
        ip.erase(0, 7);
    return ip;
}

/// Loopback, or an unspecified address, which Linux connects to the local host.
bool is_this_host_ip(const std::string& ip) {
    return ip.rfind("127.", 0) == 0 || ip.rfind("0.", 0) == 0 || ip == "::1" || ip == "::";
}

/// Every address `host` resolves to, canonical numeric form; empty when it does not resolve.
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
    char buf[NI_MAXHOST];
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, buf, sizeof(buf), nullptr, 0,
                        NI_NUMERICHOST) == 0)
            ips.push_back(canonical_ip(buf));
    }
    freeaddrinfo(result);
    return ips;
}

/// Every address assigned to one of this machine's interfaces. Moonraker listens on all
/// of them, so the printer's LAN address reaches it as surely as loopback does.
std::vector<std::string> local_interface_ips() {
    std::vector<std::string> ips;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0)
        return ips;
    char buf[NI_MAXHOST];
    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr)
            continue;
        int family = ifa->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_INET6)
            continue;
        socklen_t len = family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
        if (getnameinfo(ifa->ifa_addr, len, buf, sizeof(buf), nullptr, 0, NI_NUMERICHOST) == 0)
            ips.push_back(canonical_ip(buf));
    }
    freeifaddrs(list);
    return ips;
}

std::string configured_moonraker_host() {
    if (helix::Config* cfg = helix::Config::get_instance())
        return cfg->get<std::string>(cfg->df() + "moonraker_host", "localhost");
    return "localhost";
}

} // namespace

HttpTarget plan_http_target(const std::string& url, const std::vector<std::string>& forbidden_ips) {
    HttpTarget t;
    // The client re-parses the URL at send time; parsing it the same way here is what makes
    // the checked host the connected one. An empty host parses as libhv's 127.0.0.1.
    HttpRequest probe;
    probe.url = url;
    probe.ParseUrl();
    std::vector<std::string> ips = resolve_host_ips(probe.host);
    if (ips.empty()) {
        t.error = "cannot resolve " + probe.host;
        return t;
    }
    for (const auto& ip : ips) {
        bool forbidden = is_this_host_ip(ip);
        for (const auto& f : forbidden_ips)
            forbidden = forbidden || ip == canonical_ip(f);
        if (forbidden) {
            t.error = kPrinterHostRefused;
            return t;
        }
    }
    t.ok = true;
    if (probe.IsHttps()) {
        // ponytail: an https host is not pinned, so a DNS answer that changes between this
        // check and the connect (rebinding) still reaches this machine; Moonraker speaks plain
        // HTTP and cannot complete a TLS handshake. Pin with SNI kept on the name if a local
        // TLS service ever needs protecting.
        t.connect_url = url;
        return t;
    }
    const std::string& ip = ips.front();
    std::string literal = ip.find(':') != std::string::npos ? "[" + ip + "]" : ip;
    t.connect_url = probe.scheme + "://" + literal + ":" + std::to_string(probe.port) + probe.path;
    t.host_header = probe.headers["Host"];
    return t;
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
        // Config belongs to the main thread, which is where the binding calls this.
        std::string printer_host = configured_moonraker_host();
        http::HttpExecutor::slow().submit([=]() {
            // Resolving blocks on DNS, so the check runs here, off the main thread. A followed
            // redirect would land on a host this check never saw, so the plugin's requests do
            // not follow redirects.
            std::vector<std::string> forbidden = local_interface_ips();
            for (auto& ip : resolve_host_ips(printer_host))
                forbidden.push_back(std::move(ip));
            HttpTarget t = plan_http_target(url, forbidden);
            if (!t.ok)
                return cb(failure(t.error));
            auto req = std::make_shared<HttpRequest>();
            req->method = method == "POST" ? HTTP_POST : HTTP_GET;
            req->url = t.connect_url;
            if (!t.host_header.empty())
                req->headers["Host"] = t.host_header;
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
            // A raw pointer: the callback lives inside *req, so owning req here would be a
            // reference cycle. It only runs during requests::request, which holds req.
            HttpRequest* r = req.get();
            req->http_cb = [r, out, max_body](HttpMessage*, http_parser_state state,
                                              const char* data, size_t size) {
                if (state != HP_BODY || data == nullptr || size == 0)
                    return;
                if (out->size() >= max_body) { // keep the cancel armed on every later chunk
                    r->Cancel();
                    return;
                }
                out->append(data, std::min(size, max_body - out->size()));
                if (out->size() >= max_body)
                    r->Cancel();
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
