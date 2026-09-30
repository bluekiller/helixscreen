// SPDX-License-Identifier: GPL-3.0-or-later

#include "../test_helpers/scoped_env.h"
#include "system/tls_trust.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

#include "../catch_amalgamated.hpp"

using helix::ScopedEnv;
using helix::tls::CaStore;
using helix::tls::find_ca_store;
using helix::tls::make_client_ctx;

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("helix-tls-test-" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string touch(const std::string& name, const std::string& body = "x") const {
        const std::string p = (path / name).string();
        std::ofstream(p) << body;
        return p;
    }
};

/// A self-signed P-256 certificate with the given subjectAltName, valid from
/// `not_before_days` to `not_after_days` relative to now.
struct TestCert {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    TestCert(const char* san, long not_before_days = -1, long not_after_days = 30) {
        EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
        EVP_PKEY_keygen_init(kctx);
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_X9_62_prime256v1);
        EVP_PKEY_keygen(kctx, &key);
        EVP_PKEY_CTX_free(kctx);

        cert = X509_new();
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(cert), not_before_days * 86400);
        X509_gmtime_adj(X509_getm_notAfter(cert), not_after_days * 86400);
        X509_set_pubkey(cert, key);
        X509_NAME* subj = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("helix-test"), -1, -1, 0);
        X509_set_issuer_name(cert, subj);
        X509V3_CTX v3;
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_sign(cert, key, EVP_sha256());
    }
    ~TestCert() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
    std::string write_pem(const TempDir& dir) const {
        const std::string p = (dir.path / "ca.pem").string();
        FILE* f = fopen(p.c_str(), "w");
        PEM_write_X509(f, cert);
        fclose(f);
        return p;
    }
};

/// Runs a TLS handshake over loopback TCP between `client_ctx` and a server
/// presenting `server`. `sni` empty means an IP-literal connect (no SNI, as libhv
/// does for one). Returns whether the client accepted the server.
bool handshake(void* client_ctx, const TestCert& server, const std::string& sni) {
    SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(sctx, server.cert);
    SSL_CTX_use_PrivateKey(sctx, server.key);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(lfd, 1);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    int sfd = accept(lfd, nullptr, nullptr);
    fcntl(cfd, F_SETFL, O_NONBLOCK);
    fcntl(sfd, F_SETFL, O_NONBLOCK);

    SSL* c = SSL_new(static_cast<SSL_CTX*>(client_ctx));
    SSL* s = SSL_new(sctx);
    SSL_set_fd(c, cfd);
    SSL_set_fd(s, sfd);
    if (!sni.empty())
        SSL_set_tlsext_host_name(c, sni.c_str());

    int crc = -1, src = -1;
    for (int i = 0; i < 200 && crc != 1; ++i) {
        crc = SSL_connect(c);
        if (crc != 1 && SSL_get_error(c, crc) != SSL_ERROR_WANT_READ)
            break;
        if (src != 1)
            src = SSL_accept(s);
        usleep(1000);
    }

    SSL_free(c);
    SSL_free(s);
    SSL_CTX_free(sctx);
    close(cfd);
    close(sfd);
    close(lfd);
    return crc == 1;
}

} // namespace

TEST_CASE("find_ca_store: env beats system beats bundled", "[tls]") {
    TempDir dir;
    const std::string env_file = dir.touch("env.pem");
    const std::string sys_file = dir.touch("sys.pem");
    const std::string bundled = dir.touch("bundled.pem");
    const std::string missing = (dir.path / "missing.pem").string();

    SECTION("SSL_CERT_FILE and SSL_CERT_DIR win") {
        ScopedEnv f("SSL_CERT_FILE", env_file.c_str());
        ScopedEnv d("SSL_CERT_DIR", dir.path.c_str());
        const CaStore s = find_ca_store({sys_file}, bundled);
        CHECK(s.file == env_file);
        CHECK(s.dir == dir.path.string());
    }
    SECTION("an unreadable SSL_CERT_FILE falls through to the system bundle") {
        ScopedEnv f("SSL_CERT_FILE", missing.c_str());
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        const CaStore s = find_ca_store({missing, sys_file}, bundled);
        CHECK(s.file == sys_file);
        CHECK(s.dir.empty());
    }
    SECTION("the bundled file is the last resort") {
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        CHECK(find_ca_store({missing}, bundled).file == bundled);
    }
    SECTION("nothing readable reports no store") {
        ScopedEnv f("SSL_CERT_FILE", nullptr);
        ScopedEnv d("SSL_CERT_DIR", nullptr);
        CHECK(find_ca_store({missing}, missing).empty());
        CHECK(find_ca_store({}, "").empty());
    }
}

TEST_CASE("make_client_ctx verifies peers and ignores validity dates", "[tls]") {
    TempDir dir;
    auto* ctx = static_cast<SSL_CTX*>(make_client_ctx({dir.touch("ca.pem"), {}}));
    REQUIRE(ctx != nullptr);
    CHECK((SSL_CTX_get_verify_mode(ctx) & SSL_VERIFY_PEER) != 0);
    CHECK((X509_VERIFY_PARAM_get_flags(SSL_CTX_get0_param(ctx)) & X509_V_FLAG_NO_CHECK_TIME) != 0);
    SSL_CTX_free(ctx);
}

TEST_CASE("client ctx accepts only a trusted certificate for the host", "[tls]") {
    TempDir dir;
    TestCert server("DNS:helix.test,IP:127.0.0.1");

    SECTION("untrusted certificate is rejected") {
        TestCert other("DNS:other.test");
        auto* ctx = make_client_ctx({other.write_pem(dir), {}});
        CHECK_FALSE(handshake(ctx, server, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("an unloadable CA file trusts nothing") {
        auto* ctx = make_client_ctx({dir.touch("empty.pem", ""), {}});
        REQUIRE(ctx != nullptr);
        CHECK_FALSE(handshake(ctx, server, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("trusted certificate") {
        auto* ctx = make_client_ctx({server.write_pem(dir), {}});
        CHECK(handshake(ctx, server, "helix.test"));
        CHECK_FALSE(handshake(ctx, server, "evil.test"));
        CHECK(handshake(ctx, server, "")); // IP literal, matched against the peer address
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("IP literal not in the certificate is rejected") {
        TestCert dns_only("DNS:helix.test");
        auto* ctx = make_client_ctx({dns_only.write_pem(dir), {}});
        CHECK_FALSE(handshake(ctx, dns_only, ""));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
    SECTION("an expired certificate still verifies") {
        TestCert expired("DNS:helix.test", -60, -30);
        auto* ctx = make_client_ctx({expired.write_pem(dir), {}});
        CHECK(handshake(ctx, expired, "helix.test"));
        SSL_CTX_free(static_cast<SSL_CTX*>(ctx));
    }
}
