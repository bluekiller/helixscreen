// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace helix::tls {

/// Where the CA certificates live. Both empty means no store was found.
struct CaStore {
    std::string file;
    std::string dir;
    bool empty() const {
        return file.empty() && dir.empty();
    }
};

/// Resolves the CA store: a readable $SSL_CERT_FILE / $SSL_CERT_DIR first, then the
/// first readable entry of `system_files`, then `bundled_file`.
CaStore find_ca_store(const std::vector<std::string>& system_files,
                      const std::string& bundled_file);

/// find_ca_store() over the system bundle paths the launcher also searches and the
/// certs/ca-certificates.crt shipped next to the install.
CaStore find_ca_store();

/// A client SSL_CTX (as libhv's hssl_ctx_t) that verifies the server's chain against
/// `store` and its name against the host being connected to. Never null when OpenSSL
/// is built in: a store that fails to load leaves the context with no trust anchors,
/// so every handshake fails instead of passing unverified. Null without OpenSSL.
void* make_client_ctx(const CaStore& store);

/// Makes every libhv HTTPS and WSS client connection verify its server. Call once at
/// startup, before any thread makes a request. With no CA store on the device it logs
/// a warning and leaves connections unverified.
void install_client_verification();

} // namespace helix::tls
