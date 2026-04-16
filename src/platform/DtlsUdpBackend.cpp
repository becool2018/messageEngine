// Copyright 2026 Don Jessup
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file DtlsUdpBackend.cpp
 * @brief Implementation of DtlsUdpBackend: DTLS-capable UDP transport.
 *
 * Uses mbedTLS 4.0 with PSA Crypto backend for DTLS 1.2/1.3 encryption over
 * UDP.  When config.tls.tls_enabled is false, falls back to plaintext UDP
 * identical to UdpBackend — no DTLS overhead.
 *
 * Key DTLS differences from TlsTcpBackend (TLS/TCP):
 *   - MBEDTLS_SSL_TRANSPORT_DATAGRAM: no byte-stream, records are whole datagrams.
 *   - No 4-byte length prefix: DTLS record layer preserves message boundaries.
 *   - mbedtls_ssl_set_timer_cb(): required for DTLS handshake retransmission.
 *   - mbedtls_ssl_cookie_ctx: server-side anti-amplification cookie exchange.
 *   - mbedtls_ssl_set_mtu(): caps DTLS record size to DTLS_MAX_DATAGRAM_BYTES.
 *   - Single-peer model: socket is connect()ed to one peer after first datagram.
 *
 * Power of 10 Rule 3 deviation (init-phase):
 *   mbedTLS heap-allocates during psa_crypto_init(), certificate parsing,
 *   and DTLS handshake.  These occur exclusively in init().  The send/receive
 *   critical path (after handshake) does not allocate.
 *
 * Power of 10 Rule 9 deviations:
 *   Three mbedTLS API calls require function-pointer arguments (documented at
 *   each call site):
 *     mbedtls_ssl_set_bio()           — BIO send/recv/recv_timeout
 *     mbedtls_ssl_set_timer_cb()      — DTLS retransmission timer
 *     mbedtls_ssl_conf_dtls_cookies() — DTLS cookie write/check (server)
 *   All use only library-defined symbols; no application-level function pointer
 *   declarations are introduced.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.10,
 *             REQ-5.1.5, REQ-5.1.6,
 *             REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4,
 *             REQ-6.4.5, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.1.5, REQ-5.1.6, REQ-6.1.10, REQ-6.2.4, REQ-6.3.4, REQ-6.3.6, REQ-6.3.7, REQ-6.3.9, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-6.4.6, REQ-7.1.1, REQ-7.2.4

#include "platform/DtlsUdpBackend.hpp"
#include "platform/ISocketOps.hpp"
#include "platform/MbedtlsOpsImpl.hpp"
#include "platform/SocketOpsImpl.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include "core/Serializer.hpp"
#include "core/Timestamp.hpp"
#include "platform/SocketUtils.hpp"
#include "core/ImpairmentConfig.hpp"

#if __has_include(<mbedtls/build_info.h>)
#  include <mbedtls/build_info.h>   // mbedTLS 3.x / 4.x
#else
#  include <mbedtls/version.h>      // mbedTLS 2.x
#endif
#include <mbedtls/error.h>
#include <mbedtls/psa_util.h>
#include <mbedtls/platform_util.h>  // SEC-006: mbedtls_platform_zeroize() (CLAUDE.md §7c)
#include <psa/crypto.h>

#include <sys/socket.h>   // connect(), recvfrom(), MSG_PEEK
#include <sys/stat.h>     // lstat() — F-2 symlink validation
#include <netinet/in.h>   // sockaddr_in, sockaddr_storage
#include <climits>        // INT_MAX — SEC-017 CERT INT31-C poll timeout clamp
#include <poll.h>         // poll()
#include <cstring>
#include <cerrno>    // errno — used in tls_path_is_regular_file() lstat() error path

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — log mbedTLS error code
// ─────────────────────────────────────────────────────────────────────────────

static void log_mbedtls_err(const char* tag, const char* func, int ret)
{
    NEVER_COMPILED_OUT_ASSERT(tag  != nullptr);
    NEVER_COMPILED_OUT_ASSERT(func != nullptr);
    char err_buf[128];
    (void)memset(err_buf, 0, sizeof(err_buf));
    mbedtls_strerror(ret, err_buf, sizeof(err_buf) - 1U);
    Logger::log(Severity::WARNING_HI, __FILE__, __LINE__, tag, "%s failed: -0x%04X (%s)",
                func, static_cast<unsigned int>(-ret), err_buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — F-2 symlink TOCTOU guard
// Reject paths that resolve to a symlink.  A symlink can be swapped after
// tls_path_valid() checks the string but before fopen()/parse_file() opens it
// (TOCTOU window).  By calling lstat() immediately before each parse_file()
// call we shrink the window to the irreducible OS race and detect most attacks.
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if path is a regular file (not a symlink or other special file).
/// @param path  NUL-terminated file path (caller must have validated NUL termination).
/// @param tag   Logging tag for the calling backend.
static bool tls_path_is_regular_file(const char* path, const char* tag)
{
    NEVER_COMPILED_OUT_ASSERT(path != nullptr);  // pre-condition: non-null path
    NEVER_COMPILED_OUT_ASSERT(tag  != nullptr);  // pre-condition: non-null tag

    struct stat st;
    // MISRA C++:2023 Rule 5.2.4 exemption: lstat() is a POSIX API; no application-level
    // pointer cast is performed here.
    if (lstat(path, &st) != 0) {
        Logger::log(Severity::WARNING_HI, __FILE__, __LINE__, tag,
                    "tls_path_is_regular_file: lstat('%s') failed: %d", path, errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        Logger::log(Severity::WARNING_HI, __FILE__, __LINE__, tag,
                    "tls_path_is_regular_file: '%s' is not a regular file (mode=0%o)",
                    path, static_cast<unsigned>(st.st_mode));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

DtlsUdpBackend::DtlsUdpBackend()
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_crl{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&SocketOpsImpl::instance()),
      m_ops(&MbedtlsOpsImpl::instance()),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_crl_loaded(false),
      m_peer_node_id(NODE_ID_INVALID), m_peer_hello_received(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_local_node_id(NODE_ID_INVALID),  // SEC-018: initialized at declaration
      m_pending_src_port(0U),            // SEC-027: candidate port, not yet HELLO-validated
      m_peer_src_port(0U)                // SEC-023: locked only after first valid HELLO
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_x509_crl_init(&m_crl);
    mbedtls_pk_init(&m_pkey);
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_cookie_init(&m_cookie_ctx);
    mbedtls_net_init(&m_net_ctx);
}

DtlsUdpBackend::DtlsUdpBackend(IMbedtlsOps& ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_crl{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&SocketOpsImpl::instance()),
      m_ops(&ops),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_crl_loaded(false),
      m_peer_node_id(NODE_ID_INVALID), m_peer_hello_received(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_local_node_id(NODE_ID_INVALID),  // SEC-018: initialized at declaration
      m_pending_src_port(0U),            // SEC-027: candidate port, not yet HELLO-validated
      m_peer_src_port(0U)                // SEC-023: locked only after first valid HELLO
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_x509_crl_init(&m_crl);
    mbedtls_pk_init(&m_pkey);
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_cookie_init(&m_cookie_ctx);
    mbedtls_net_init(&m_net_ctx);
}

DtlsUdpBackend::DtlsUdpBackend(ISocketOps& sock_ops, IMbedtlsOps& tls_ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_crl{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&sock_ops),
      m_ops(&tls_ops),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_crl_loaded(false),
      m_peer_node_id(NODE_ID_INVALID), m_peer_hello_received(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_local_node_id(NODE_ID_INVALID),  // SEC-018: initialized at declaration
      m_pending_src_port(0U),            // SEC-027: candidate port, not yet HELLO-validated
      m_peer_src_port(0U)                // SEC-023: locked only after first valid HELLO
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_x509_crl_init(&m_crl);
    mbedtls_pk_init(&m_pkey);
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_cookie_init(&m_cookie_ctx);
    mbedtls_net_init(&m_net_ctx);
}

DtlsUdpBackend::~DtlsUdpBackend()
{
    DtlsUdpBackend::close();
    mbedtls_ssl_config_free(&m_ssl_conf);
    mbedtls_x509_crt_free(&m_cert);
    mbedtls_x509_crt_free(&m_ca_cert);
    mbedtls_x509_crl_free(&m_crl);  // REQ-6.3.4: release CRL memory
    mbedtls_pk_free(&m_pkey);
    // SEC-006: CLAUDE.md §7c — zeroize private key material after free to prevent
    // residual key bytes in memory (CWE-14; mbedtls_platform_zeroize is compiler-barrier-safe).
    mbedtls_platform_zeroize(static_cast<void*>(&m_pkey), sizeof(m_pkey));
    mbedtls_ssl_cookie_free(&m_cookie_ctx);
    // mbedtls_ssl_free() already called inside close()
    mbedtls_psa_crypto_free();
}

// ─────────────────────────────────────────────────────────────────────────────
// load_certs_and_key() — CC-reduction helper for setup_dtls_config
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::load_certs_and_key(const TlsConfig& tls_cfg)
{
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.cert_file));
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.key_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.key_file[0] != '\0');

    if (tls_cfg.verify_peer && tls_cfg.ca_file[0] != '\0') {
        // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
        NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.ca_file));
        // F-2: reject symlink attacks immediately before parse_file() to narrow TOCTOU window.
        if (!tls_path_is_regular_file(tls_cfg.ca_file, "DtlsUdpBackend")) {
            return Result::ERR_IO;
        }
        int ret = m_ops->x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file);
        if (ret != 0) {
            log_mbedtls_err("DtlsUdpBackend", "x509_crt_parse_file (CA)", ret);
            return Result::ERR_IO;
        }
        // REQ-6.3.4: load CRL (if configured) and bind CA chain + CRL to ssl_conf.
        Result crl_res = load_crl_if_configured(tls_cfg);
        if (!result_ok(crl_res)) { return crl_res; }
    }

    return load_own_cert_and_key(tls_cfg);
}

// ─────────────────────────────────────────────────────────────────────────────
// load_own_cert_and_key() — CC-reduction helper for load_certs_and_key (F-2)
// Validates cert/key paths via lstat(), parses the certificate and private key,
// and binds them to ssl_conf.  Extracted to keep load_certs_and_key() CC ≤ 10.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::load_own_cert_and_key(const TlsConfig& tls_cfg)
{
    // SECfix-5 / F-2: path validity and symlink guards
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.cert_file));
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.key_file));

    // F-2: reject symlink attacks immediately before parse_file() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.cert_file, "DtlsUdpBackend")) {
        return Result::ERR_IO;
    }
    int ret = m_ops->x509_crt_parse_file(&m_cert, tls_cfg.cert_file);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "x509_crt_parse_file (cert)", ret);
        return Result::ERR_IO;
    }

    // F-2: reject symlink attacks immediately before pk_parse_keyfile() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.key_file, "DtlsUdpBackend")) {
        return Result::ERR_IO;
    }
    ret = m_ops->pk_parse_keyfile(&m_pkey, tls_cfg.key_file, nullptr);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "pk_parse_keyfile", ret);
        return Result::ERR_IO;
    }

    ret = m_ops->ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_conf_own_cert", ret);
        return Result::ERR_IO;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// load_crl_if_configured() — CC-reduction helper for load_certs_and_key
// ─────────────────────────────────────────────────────────────────────────────
//
// Parses the CRL file when tls_cfg.crl_file is non-empty; updates m_crl_loaded.
// Then calls mbedtls_ssl_conf_ca_chain() with m_crl when loaded, nullptr otherwise.
// Power of 10 Rule 3 deviation: mbedtls_x509_crl_parse_file() may heap-allocate
// internally; this occurs exclusively in init() (not on the send/receive path).

Result DtlsUdpBackend::load_crl_if_configured(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.verify_peer);  // pre: only called when verify_peer
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before any fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.ca_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.ca_file[0] != '\0');  // pre: CA cert was loaded
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.crl_file));

    if (tls_cfg.crl_file[0] != '\0') {
        // F-2: reject symlink attacks immediately before crl_parse_file() to narrow TOCTOU window.
        if (!tls_path_is_regular_file(tls_cfg.crl_file, "DtlsUdpBackend")) {
            return Result::ERR_IO;
        }
        int crl_ret = mbedtls_x509_crl_parse_file(&m_crl, tls_cfg.crl_file);
        if (crl_ret != 0) {
            log_mbedtls_err("DtlsUdpBackend", "x509_crl_parse_file", crl_ret);
            return Result::ERR_IO;
        }
        m_crl_loaded = true;
    } else {
        // SEC-003: verify_peer=true but no CRL file configured — certificate
        // revocation checking is disabled. This may be intentional (e.g., OCSP
        // is used instead) but is logged at WARNING_HI so deployments that
        // require CRL checking can detect misconfiguration (REQ-6.3.4, SECURITY §3).
        LOG_WARN_HI("DtlsUdpBackend", "verify_peer=true but crl_file is empty — CRL revocation "
                    "checking disabled (SEC-003, REQ-6.3.4)");
    }

    // Bind CA cert + CRL (or nullptr if not loaded) to ssl_conf.
    mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert,
                              m_crl_loaded ? &m_crl : nullptr);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_cookie_if_server() — CC-reduction helper for setup_dtls_config
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::setup_cookie_if_server(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(m_ops != nullptr);

    if (tls_cfg.role == TlsRole::SERVER) {
        // DTLS cookie anti-replay (REQ-6.4.2)
        // Power of 10 Rule 9 deviation: mbedtls_ssl_conf_dtls_cookies() requires
        // function-pointer arguments as part of the mbedTLS 4.0 DTLS cookie API.
        // Using library-defined symbols only; no application-level fn-ptr declarations.
        int ret = m_ops->ssl_cookie_setup(&m_cookie_ctx);
        if (ret != 0) {
            log_mbedtls_err("DtlsUdpBackend", "ssl_cookie_setup", ret);
            return Result::ERR_IO;
        }
        // C-2 / REQ-3.2.11 / HAZ-018: mbedtls_ssl_cookie_check() uses
        // mbedtls_ct_memcmp() internally for constant-time cookie comparison,
        // satisfying the timing-safe equality requirement (CLAUDE.md §7d).
        // No application-level ct wrapper is required here.
        mbedtls_ssl_conf_dtls_cookies(&m_ssl_conf,
                                      mbedtls_ssl_cookie_write,
                                      mbedtls_ssl_cookie_check,
                                      &m_cookie_ctx);
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_dtls_config() — configure shared ssl_conf for DTLS transport
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::setup_dtls_config(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
    // F-19: verify DTLS_MAX_DATAGRAM_BYTES is large enough to hold at least one
    // byte of payload beyond the wire header; catches misconfigured builds where
    // someone sets DTLS_MAX_DATAGRAM_BYTES ≤ WIRE_HEADER_SIZE.
    static_assert(DTLS_MAX_DATAGRAM_BYTES > Serializer::WIRE_HEADER_SIZE,
                  "DTLS_MAX_DATAGRAM_BYTES must exceed WIRE_HEADER_SIZE");
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.cert_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');

    // Init PSA Crypto (mbedTLS 4.0 PSA backend replaces CTR-DRBG/entropy).
    // Power of 10 Rule 3 deviation — init-phase heap allocation inside PSA.
    psa_status_t psa_ret = m_ops->crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        LOG_FATAL("DtlsUdpBackend", "psa_crypto_init failed: %d", static_cast<int>(psa_ret));
        return Result::ERR_IO;
    }

    int endpoint = (tls_cfg.role == TlsRole::SERVER)
                   ? MBEDTLS_SSL_IS_SERVER
                   : MBEDTLS_SSL_IS_CLIENT;

    // MBEDTLS_SSL_TRANSPORT_DATAGRAM selects DTLS mode (REQ-6.4.1)
    int ret = m_ops->ssl_config_defaults(&m_ssl_conf,
                                         endpoint,
                                         MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_config_defaults", ret);
        return Result::ERR_IO;
    }

    // mbedTLS 4.0 removed mbedtls_ssl_conf_rng(): the PSA RNG is automatically
    // bound to the SSL config after psa_crypto_init() completes.
    // mbedTLS 2.x/3.x with MBEDTLS_USE_PSA_CRYPTO requires the explicit call.
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&m_ssl_conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
#endif

    // DTLS handshake retransmission: min 1 s, max 10 s (RFC 6347 §4.2.4)
    mbedtls_ssl_conf_handshake_timeout(&m_ssl_conf, 1000U, 10000U);

    // Per-poll read timeout for receive_message() loop (100 ms per iteration)
    mbedtls_ssl_conf_read_timeout(&m_ssl_conf, 100U);

    int authmode = tls_cfg.verify_peer
                   ? MBEDTLS_SSL_VERIFY_REQUIRED
                   : MBEDTLS_SSL_VERIFY_NONE;
    mbedtls_ssl_conf_authmode(&m_ssl_conf, authmode);

    Result res = load_certs_and_key(tls_cfg);
    if (!result_ok(res)) { return res; }

    res = setup_cookie_if_server(tls_cfg);
    if (!result_ok(res)) { return res; }

    // Security fix: restrict to AEAD-only cipher suites; prohibit CBC, NULL, and RSA-kx.
    // Power of 10 Rule 3: static const array — no dynamic allocation.
    static const int k_allowed_ciphersuites[] = {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
        MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
        MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
#endif
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        0  // terminator
    };
    mbedtls_ssl_conf_ciphersuites(&m_ssl_conf, k_allowed_ciphersuites);

    // Enforce minimum DTLS 1.2; prohibit DTLS 1.0.
    // mbedTLS 3.x+: mbedtls_ssl_conf_min_tls_version / MBEDTLS_SSL_VERSION_TLS1_2.
    // mbedTLS 2.x:  mbedtls_ssl_conf_min_version / major=3, minor=3 (TLS 1.2 IANA encoding).
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    (void)mbedtls_ssl_conf_min_tls_version(&m_ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);
#else
    mbedtls_ssl_conf_min_version(&m_ssl_conf,
                                 MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
#endif

    LOG_INFO("DtlsUdpBackend", "DTLS config ready: role=%s verify_peer=%d cert=%s",
                (tls_cfg.role == TlsRole::SERVER) ? "SERVER" : "CLIENT",
                static_cast<int>(tls_cfg.verify_peer),
                tls_cfg.cert_file);

    NEVER_COMPILED_OUT_ASSERT(psa_ret == PSA_SUCCESS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_dtls_handshake() — bounded DTLS handshake loop
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::run_dtls_handshake(const void* peer_addr, uint32_t addr_len)
{
    NEVER_COMPILED_OUT_ASSERT(peer_addr != nullptr);
    NEVER_COMPILED_OUT_ASSERT(addr_len > 0U);

    // Power of 10 Rule 2 deviation: this loop has no statically provable finite
    // bound in the general case (DTLS handshake flights depend on network round
    // trips).  DTLS_HANDSHAKE_MAX_ITER = 32 is a conservative upper bound
    // covering: 1 HELLO_VERIFY_REQUIRED reset + 7 RFC-6347 flights × 4
    // WANT_READ/WANT_WRITE retries each.  Per CLAUDE.md §2.2 exception:
    // per-iteration work is bounded; terminates on handshake completion, timeout,
    // or fatal error.
    static const uint32_t DTLS_HANDSHAKE_MAX_ITER = 32U;
    bool done = false;

    for (uint32_t iter = 0U; iter < DTLS_HANDSHAKE_MAX_ITER && !done; ++iter) {
        int ret = m_ops->ssl_handshake(&m_ssl);
        if (ret == 0) {
            done = true;
        } else if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
            // Server received ClientHello without valid cookie; reset session and
            // re-arm the DTLS session so the second ClientHello (with cookie) is
            // processed correctly.
            mbedtls_ssl_session_reset(&m_ssl);
            // Power of 10 Rule 9 deviation: library-defined symbols only (see header).
            mbedtls_ssl_set_bio(&m_ssl, &m_net_ctx,
                                mbedtls_net_send, mbedtls_net_recv,
                                mbedtls_net_recv_timeout);
            mbedtls_ssl_set_timer_cb(&m_ssl, &m_timer,
                                     mbedtls_timing_set_delay,
                                     mbedtls_timing_get_delay);
            // MISRA C++:2023 Rule 5.2.4: reinterpret peer_addr bytes as unsigned char*
            // for the DTLS transport-ID API — same type representation, required by API.
            (void)mbedtls_ssl_set_client_transport_id(
                &m_ssl,
                reinterpret_cast<const unsigned char*>(peer_addr),
                static_cast<size_t>(addr_len));
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                   ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls_err("DtlsUdpBackend", "ssl_handshake", ret);
            return Result::ERR_IO;
        }
    }

    if (!done) {
        LOG_WARN_HI("DtlsUdpBackend", "DTLS handshake did not complete in %u iterations",
                    DTLS_HANDSHAKE_MAX_ITER);
        return Result::ERR_IO;
    }

    NEVER_COMPILED_OUT_ASSERT(done);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// server_wait_and_handshake()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::server_wait_and_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);

    // Wait for first client datagram (with configurable timeout)
    struct pollfd pfd;
    pfd.fd     = m_sock_fd;
    pfd.events = POLLIN;
    // SEC-017: CERT INT31-C — clamp uint32_t timeout to INT_MAX before
    // narrowing to int for poll(). Values > INT_MAX cast to a negative int,
    // which POSIX defines as "block indefinitely" — an unintended DoS path.
    static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
    const uint32_t raw_timeout = (m_cfg.connect_timeout_ms > 0U)
                                 ? m_cfg.connect_timeout_ms : 30000U;
    const uint32_t clamped_timeout = (raw_timeout > k_poll_max_ms)
                                     ? k_poll_max_ms : raw_timeout;
    int wait_ms = static_cast<int>(clamped_timeout);
    int pr = poll(&pfd, 1, wait_ms);
    if (pr <= 0) {
        LOG_WARN_HI("DtlsUdpBackend", "server: timeout waiting for first client datagram");
        return Result::ERR_TIMEOUT;
    }

    // Peek first datagram to discover client address; MSG_PEEK leaves it
    // in the socket buffer so the DTLS engine reads it on the first recv().
    struct sockaddr_storage peer_addr;
    (void)memset(&peer_addr, 0, sizeof(peer_addr));
    socklen_t peer_addr_len = static_cast<socklen_t>(sizeof(peer_addr));
    uint8_t peek_buf[1U];
    // MISRA C++:2023 Rule 5.2.4: POSIX recvfrom() requires sockaddr* cast;
    // sockaddr_storage is the POSIX-standard storage type for any sockaddr variant.
    ssize_t n = m_ops->recvfrom_peek(
        m_sock_fd, peek_buf, sizeof(peek_buf),
        reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_addr_len);
    if (n < 0) {
        LOG_WARN_HI("DtlsUdpBackend", "server: recvfrom(MSG_PEEK) failed");
        return Result::ERR_IO;
    }

    // Connect socket to client address (single-peer model).  After connect(),
    // send()/recv() are peer-locked; mbedtls_net_send/recv BIO callbacks work.
    // MISRA C++:2023 Rule 5.2.4: POSIX connect() requires const sockaddr* cast.
    if (m_ops->net_connect(m_sock_fd,
                           reinterpret_cast<const struct sockaddr*>(&peer_addr),
                           peer_addr_len) < 0) {
        LOG_WARN_HI("DtlsUdpBackend", "server: connect() to client failed");
        return Result::ERR_IO;
    }

    m_net_ctx.fd = m_sock_fd;

    int ret = m_ops->ssl_setup(&m_ssl, &m_ssl_conf);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_setup (server)", ret);
        return Result::ERR_IO;
    }

    // Power of 10 Rule 9 deviation: mbedtls_ssl_set_bio() requires function-pointer
    // arguments as part of the mbedTLS BIO API contract.  Using library-defined
    // symbols mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout only;
    // no application-level function pointer declarations introduced.
    mbedtls_ssl_set_bio(&m_ssl, &m_net_ctx,
                        mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    // Power of 10 Rule 9 deviation: mbedtls_ssl_set_timer_cb() requires function-pointer
    // arguments for DTLS retransmission timer (REQ-6.4.3).  Using library-defined
    // symbols mbedtls_timing_set_delay, mbedtls_timing_get_delay only.
    mbedtls_ssl_set_timer_cb(&m_ssl, &m_timer,
                             mbedtls_timing_set_delay, mbedtls_timing_get_delay);

    // Cap DTLS record size to avoid IP fragmentation (REQ-6.4.4)
    mbedtls_ssl_set_mtu(&m_ssl, static_cast<uint16_t>(DTLS_MAX_DATAGRAM_BYTES));

    // Set client transport ID for cookie binding (REQ-6.4.2)
    // MISRA C++:2023 Rule 5.2.4: peer_addr treated as byte array for transport ID.
    ret = m_ops->ssl_set_client_transport_id(
        &m_ssl,
        reinterpret_cast<const unsigned char*>(&peer_addr),
        static_cast<size_t>(peer_addr_len));
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_set_client_transport_id", ret);
        return Result::ERR_IO;
    }

    Result res = run_dtls_handshake(&peer_addr,
                                    static_cast<uint32_t>(peer_addr_len));
    if (!result_ok(res)) {
        mbedtls_ssl_free(&m_ssl);
        mbedtls_ssl_init(&m_ssl);
        return res;
    }

    m_open = true;
    ++m_connections_opened;  // REQ-7.2.4: DTLS server handshake complete
    LOG_INFO("DtlsUdpBackend", "DTLS handshake complete (server): cipher=%s",
                mbedtls_ssl_get_ciphersuite(&m_ssl));

    NEVER_COMPILED_OUT_ASSERT(m_open);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// client_connect_and_handshake()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::client_connect_and_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);

    // SEC-001: REQ-6.4.6 enforcement — verify_peer=true with an empty
    // peer_hostname disables CN/SAN certificate hostname verification while
    // still appearing to validate the peer. An attacker with any cert from
    // the same trusted CA can impersonate the server (CWE-297). Reject
    // this configuration at connection time rather than silently degrading.
    if (m_cfg.tls.verify_peer && (m_cfg.tls.peer_hostname[0] == '\0')) {
        LOG_WARN_HI("DtlsUdpBackend", "client_connect_and_handshake: verify_peer=true but "
                    "peer_hostname is empty — hostname verification would be "
                    "skipped; refusing connection (SEC-001, REQ-6.4.6)");
        return Result::ERR_INVALID;
    }

    // Build peer sockaddr; connect() on UDP sets default destination and filters
    // incoming datagrams to the peer address only.
    struct sockaddr_in peer;
    (void)memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(m_cfg.peer_port);
    // MISRA C++:2023 Rule 5.2.4: inet_pton result stored into in_addr (same type).
    if (m_ops->inet_pton_ipv4(m_cfg.peer_ip, &peer.sin_addr) != 1) {
        LOG_WARN_HI("DtlsUdpBackend", "client: inet_pton failed for %s", m_cfg.peer_ip);
        return Result::ERR_IO;
    }

    // MISRA C++:2023 Rule 5.2.4: POSIX connect() requires const sockaddr* cast.
    if (m_ops->net_connect(m_sock_fd,
                           reinterpret_cast<const struct sockaddr*>(&peer),
                           static_cast<socklen_t>(sizeof(peer))) < 0) {
        LOG_WARN_HI("DtlsUdpBackend", "client: connect() to %s:%u failed",
                    m_cfg.peer_ip, m_cfg.peer_port);
        return Result::ERR_IO;
    }

    m_net_ctx.fd = m_sock_fd;

    int ret = m_ops->ssl_setup(&m_ssl, &m_ssl_conf);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_setup (client)", ret);
        return Result::ERR_IO;
    }

    // REQ-6.4.6: bind expected server hostname for SNI + CN/SAN verification.
    // Mirrors TlsTcpBackend behaviour. Pass nullptr to opt out when peer_hostname
    // is empty (verify_peer == false or explicit opt-out).
    // Safety-critical (SC): HAZ-008
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);  // pre: client path only
    const char* hostname = (m_cfg.tls.peer_hostname[0] != '\0')
                           ? m_cfg.tls.peer_hostname
                           : nullptr;
    ret = m_ops->ssl_set_hostname(&m_ssl, hostname);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_set_hostname (client)", ret);
        mbedtls_ssl_free(&m_ssl);
        mbedtls_ssl_init(&m_ssl);
        return Result::ERR_IO;
    }
    NEVER_COMPILED_OUT_ASSERT(true);  // post: hostname bound or opt-out acknowledged

    // Power of 10 Rule 9 deviations: library-defined symbols only (see header).
    mbedtls_ssl_set_bio(&m_ssl, &m_net_ctx,
                        mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
    mbedtls_ssl_set_timer_cb(&m_ssl, &m_timer,
                             mbedtls_timing_set_delay, mbedtls_timing_get_delay);
    mbedtls_ssl_set_mtu(&m_ssl, static_cast<uint16_t>(DTLS_MAX_DATAGRAM_BYTES));

    // Client does not send a transport ID; pass peer address for symmetry only.
    Result res = run_dtls_handshake(&peer, static_cast<uint32_t>(sizeof(peer)));
    if (!result_ok(res)) {
        mbedtls_ssl_free(&m_ssl);
        mbedtls_ssl_init(&m_ssl);
        return res;
    }

    m_open = true;
    ++m_connections_opened;  // REQ-7.2.4: DTLS client handshake complete
    LOG_INFO("DtlsUdpBackend", "DTLS handshake complete (client): cipher=%s",
                mbedtls_ssl_get_ciphersuite(&m_ssl));

    NEVER_COMPILED_OUT_ASSERT(m_open);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_source() — private helper (REQ-6.2.4)
//
// Plaintext path: validates src_ip/src_port against the configured peer.
// DTLS path:      returns true immediately — connect() + DTLS record MAC already
//                 filter non-peer datagrams at the kernel and TLS record layers.
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::validate_source(const char* src_ip, uint16_t src_port)
{
    NEVER_COMPILED_OUT_ASSERT(src_ip != nullptr);           // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_cfg.peer_ip[0] != '\0');  // Pre-condition

    // REQ-6.2.4: DTLS path — connect() + DTLS record MAC already filter non-peer
    // datagrams at the kernel and TLS record layers.  The envelope source_id is
    // validated in recv_one_dtls_datagram() via HELLO-based NodeId registration
    // (m_peer_node_id / m_peer_hello_received) — see REQ-6.2.4 handling there.
    if (m_tls_enabled) {
        return true;
    }

    // Plaintext path: always validate IP address.
    // CERT: use strcmp() not strncmp(…, sizeof(buf)) — same rationale as UdpBackend:
    // strncmp reads up to N bytes past each string's NUL, reaching uninitialised memory.
    bool ip_match = (strcmp(src_ip, m_cfg.peer_ip) == 0);

    // Port validation:
    // - Client mode: server always responds from m_cfg.peer_port; enforce it.
    // - Server mode (SEC-023 / SEC-027 two-phase port-locking):
    //     Phase 1 (pre-HELLO): m_peer_src_port == 0.  Accept any port from the
    //       trusted IP; store src_port in m_pending_src_port for later commit.
    //       Do NOT lock yet — a malformed packet or DATA-before-HELLO from the
    //       right IP must not poison the locked port (P2 finding, SEC-027).
    //     Phase 2 (post-HELLO): m_peer_src_port is non-zero (committed by
    //       process_hello_or_validate() on the first valid HELLO).  Enforce
    //       equality; reject any datagram from a different port.
    bool port_match = false;
    if (m_is_server) {
        if (m_peer_src_port == 0U) {
            // SEC-027: record candidate port; commit to m_peer_src_port only
            // after a valid HELLO is confirmed in process_hello_or_validate().
            m_pending_src_port = src_port;
            port_match = true;  // IP-only validation until port is locked
        } else {
            port_match = (src_port == m_peer_src_port);
        }
    } else {
        port_match = (src_port == m_cfg.peer_port);
    }

    if (!ip_match || !port_match) {
        // SEC-005 / SEC-023: REQ-6.2.4 / REQ-6.3.2 — source mismatch is a security
        // event; WARNING_HI matches UdpBackend behaviour and satisfies severity table.
        LOG_WARN_HI("DtlsUdpBackend", "Dropped datagram from unexpected source %s:%u (expected %s:%u) "
                    "(SEC-005, SEC-023, REQ-6.2.4)",
                    src_ip, static_cast<unsigned int>(src_port),
                    m_cfg.peer_ip, static_cast<unsigned int>(m_cfg.peer_port));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// try_tls_recv() — CC-reduction helper for recv_one_dtls_datagram
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::try_tls_recv(uint32_t* out_len)
{
    NEVER_COMPILED_OUT_ASSERT(m_tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);

    // mbedtls_ssl_conf_read_timeout() is set to 100 ms; ssl_read returns
    // MBEDTLS_ERR_SSL_TIMEOUT when no record arrives within that window.
    int ret = m_ops->ssl_read(&m_ssl, m_wire_buf,
                              static_cast<size_t>(SOCKET_RECV_BUF_BYTES));
    if (ret <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_TIMEOUT) {
            log_mbedtls_err("DtlsUdpBackend", "ssl_read", ret);
        }
        return false;
    }
    *out_len = static_cast<uint32_t>(ret);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// deserialize_and_dispatch() — CC-reduction helper for recv_one_dtls_datagram
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::deserialize_and_dispatch(uint32_t out_len)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(out_len > 0U);

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        LOG_WARN_LO("DtlsUdpBackend", "deserialize_and_dispatch: deserialize failed: %u",
                    static_cast<uint8_t>(res));
        return false;
    }

    // REQ-6.2.4 / REQ-6.1.8: process HELLO or validate source_id before delivery.
    bool consumed = false;
    if (!process_hello_or_validate(env, consumed)) {
        return false;  // spoofing attempt or duplicate HELLO — drop
    }
    if (consumed) {
        return false;  // HELLO consumed; must not reach DeliveryEngine
    }

    // REQ-5.1.5, REQ-5.1.6: apply inbound impairment; push to queue if deliverable
    return apply_inbound_impairment(env);
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_one_dtls_datagram() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::recv_one_dtls_datagram(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);

    uint32_t out_len = 0U;

    if (m_tls_enabled) {
        if (!try_tls_recv(&out_len)) { return false; }
    } else {
        // Plaintext path: delegate to ISocketOps (REQ-6.4.5)
        char     src_ip[48]  = {};
        uint16_t src_port    = 0U;
        if (!m_sock_ops->recv_from(m_sock_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                                   timeout_ms, &out_len, src_ip, &src_port)) {
            return false;
        }
        // REQ-6.2.4 / SEC-023: validate source before deserialization.
        // validate_source() also learns and locks the peer's ephemeral source port
        // on the first accepted plaintext datagram (server mode).
        if (!validate_source(src_ip, src_port)) {
            return false;
        }
    }

    // Deserialize, validate source_id, apply impairment, push to queue
    return deserialize_and_dispatch(out_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// process_hello_or_validate() — REQ-6.2.4 / REQ-6.1.8 source-id validation
// ─────────────────────────────────────────────────────────────────────────────
//
// DTLS is point-to-point (single-peer model) so there is at most one peer.
// An authorized DTLS peer can still set source_id to any value in the envelope
// payload.  This helper enforces that source_id matches the NodeId the peer
// announced in its HELLO frame.
//
// HELLO frame rules (REQ-6.1.8):
//   - First HELLO from peer: store source_id in m_peer_node_id, set flag.
//   - Duplicate HELLO: log WARNING_HI, return false (drop).
//
// Data frame rules (REQ-6.2.4):
//   - No HELLO received yet (m_peer_node_id == NODE_ID_INVALID): allow through
//     for backward compatibility with peers that do not send HELLO.
//   - HELLO received: if source_id != m_peer_node_id, log WARNING_HI, drop.

bool DtlsUdpBackend::process_hello_or_validate(const MessageEnvelope& env,
                                                bool& consumed)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // pre: transport must be open
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);  // pre: socket valid

    consumed = false;

    if (env.message_type == MessageType::HELLO) {
        if (m_peer_hello_received) {
            // REQ-6.1.8: only one HELLO per connection allowed
            LOG_WARN_HI("DtlsUdpBackend", "process_hello_or_validate: duplicate HELLO from NodeId=%u; dropping",
                        static_cast<unsigned int>(env.source_id));
            return false;
        }
        // First HELLO: register the peer NodeId
        m_peer_node_id        = env.source_id;
        m_peer_hello_received = true;
        // SEC-027: commit the candidate port now that HELLO is validated.
        // Extracted to commit_pending_src_port() to keep CC ≤ 10.
        commit_pending_src_port();
        LOG_INFO("DtlsUdpBackend", "DTLS: HELLO received, peer NodeId %u registered, port %u locked",
                    static_cast<unsigned int>(m_peer_node_id),
                    static_cast<unsigned int>(m_peer_src_port));
        // SEC-026: server sends a HELLO response so the client can register
        // the server's NodeId and pass its own HELLO-before-data guard.
        // Only sent when m_local_node_id is already set (register_local_id()
        // has been called); if not yet set, the response is deferred until
        // register_local_id() is called (DeliveryEngine calls it immediately
        // after init(), so the window is zero in normal operation).
        if (m_is_server && (m_local_node_id != NODE_ID_INVALID)) {
            (void)send_hello_datagram();
        }
        consumed = true;  // HELLO consumed; must not reach DeliveryEngine
        return true;
    }

    // F-2 / REQ-6.2.4 / REQ-6.1.8: HELLO is mandatory before any data frame.
    // Backward-compatibility bypass removed — a peer that never sends HELLO could
    // otherwise inject arbitrary source_id values for the lifetime of the connection,
    // corrupting ordering state and exhausting the dedup window.
    if (!m_peer_hello_received) {
        LOG_WARN_HI("DtlsUdpBackend", "process_hello_or_validate: data frame before HELLO; "
                    "dropping (source_id=%u)",
                    static_cast<unsigned int>(env.source_id));
        return false;
    }

    // source_id must match the NodeId registered in the HELLO.
    if (env.source_id != m_peer_node_id) {
        LOG_WARN_HI("DtlsUdpBackend", "process_hello_or_validate: source_id %u != registered %u; "
                    "spoofing attempt — dropping (REQ-6.2.4)",
                    static_cast<unsigned int>(env.source_id),
                    static_cast<unsigned int>(m_peer_node_id));
        return false;
    }

    return true;  // source_id matches registered peer NodeId
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_inbound_impairment() — CC-reduction helper for recv_one_dtls_datagram
// ─────────────────────────────────────────────────────────────────────────────
//
// Calls process_inbound() with the current wall-clock time.  Returns false if
// the message is dropped (partition) or buffered (reorder); returns true when
// the message is pushed to m_recv_queue.

bool DtlsUdpBackend::apply_inbound_impairment(const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Power of 10: transport must be open

    MessageEnvelope inbound_out[1U];
    uint32_t        inbound_count = 0U;
    uint64_t        now_us = timestamp_now_us();
    Result res = m_impairment.process_inbound(env, now_us, inbound_out, 1U,
                                              inbound_count);

    if (res == Result::ERR_IO) {
        // Partition dropped the message; do not queue
        return false;
    }

    if (inbound_count == 0U) {
        // Reorder engine buffered the message; nothing to push yet
        return false;
    }

    NEVER_COMPILED_OUT_ASSERT(inbound_count == 1U);  // Power of 10: exactly one output
    res = m_recv_queue.push(inbound_out[0]);
    if (!result_ok(res)) {
        LOG_WARN_HI("DtlsUdpBackend", "apply_inbound_impairment: recv queue full; dropping datagram");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_wire() — send expired delay-buffer envelopes to the wire
// ─────────────────────────────────────────────────────────────────────────────
//
// Collects outbound envelopes whose impairment-delay timer has expired and
// sends each one to the wire via send_one_envelope().  Failures are logged
// by send_one_envelope() but not propagated (is_current = false).

void DtlsUdpBackend::flush_delayed_to_wire(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    uint32_t count = m_impairment.collect_deliverable(now_us, m_delay_buf,
                                                      IMPAIR_DELAY_BUF_SIZE);
    // Power of 10 Rule 2: fixed loop bound (IMPAIR_DELAY_BUF_SIZE)
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)send_one_envelope(m_delay_buf[i], false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// create_and_bind_udp_socket() — CC-reduction helper for init
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::create_and_bind_udp_socket(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd < 0);
    NEVER_COMPILED_OUT_ASSERT(m_sock_ops != nullptr);

    m_sock_fd = m_sock_ops->create_udp(socket_is_ipv6(config.bind_ip));
    if (m_sock_fd < 0) {
        LOG_FATAL("DtlsUdpBackend", "socket_create_udp failed");
        return Result::ERR_IO;
    }

    if (!m_sock_ops->set_reuseaddr(m_sock_fd)) {
        LOG_WARN_HI("DtlsUdpBackend", "socket_set_reuseaddr failed");
        m_sock_ops->do_close(m_sock_fd);
        m_sock_fd = -1;
        return Result::ERR_IO;
    }

    if (!m_sock_ops->do_bind(m_sock_fd, config.bind_ip, config.bind_port)) {
        LOG_FATAL("DtlsUdpBackend", "socket_bind failed on port %u", config.bind_port);
        m_sock_ops->do_close(m_sock_fd);
        m_sock_fd = -1;
        return Result::ERR_IO;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_tls_handshake_phase() — CC-reduction helper for init
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::run_tls_handshake_phase(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    if (m_tls_enabled) {
        Result res = setup_dtls_config(config.tls);
        if (!result_ok(res)) {
            m_sock_ops->do_close(m_sock_fd);
            m_sock_fd = -1;
            return res;
        }
        res = m_is_server ? server_wait_and_handshake()
                          : client_connect_and_handshake();
        if (!result_ok(res)) {
            m_sock_ops->do_close(m_sock_fd);
            m_sock_fd = -1;
            return res;
        }
    } else {
        // Plaintext UDP — no handshake; open immediately (REQ-6.4.5)
        m_open = true;
        ++m_connections_opened;  // REQ-7.2.4: plaintext UDP bind complete
        LOG_INFO("DtlsUdpBackend", "Plaintext UDP %s bound to %s:%u",
                    m_is_server ? "server" : "client",
                    config.bind_ip, config.bind_port);
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_dtls_init_config() — REQ-6.3.6/7/9 config validation before init
// ─────────────────────────────────────────────────────────────────────────────

/// Validate TLS/DTLS-specific config constraints; called early in init() before
/// any state mutation.  Returns OK immediately when tls_enabled=false.
///
/// REQ-6.3.6 (H-1, HAZ-020): verify_peer=true with no ca_file is meaningless
/// certificate chain verification — reject immediately (CWE-295).
/// REQ-6.3.7 (H-2, HAZ-020): require_crl=true with no crl_file bypasses
/// revocation checking — reject immediately (CWE-295).
/// REQ-6.3.9 (H-8, HAZ-025): verify_peer=false with a non-empty peer_hostname
/// is an unsafe state (CWE-297).
// Safety-critical (SC): HAZ-020, HAZ-025
Result DtlsUdpBackend::validate_dtls_init_config(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.ca_file));  // Assert: NUL-terminated

    if (!tls_cfg.tls_enabled) {
        return Result::OK;  // All checks are TLS/DTLS-specific; skip when TLS is off.
    }

    // REQ-6.3.6 (H-1): ca_file must be non-empty when verify_peer=true.
    if (tls_cfg.verify_peer && (tls_cfg.ca_file[0] == '\0')) {
        LOG_FATAL("DtlsUdpBackend", "REQ-6.3.6/HAZ-020: verify_peer=true but ca_file is empty — "
                    "no trust anchor; init rejected (H-1, CWE-295)");
        return Result::ERR_IO;
    }

    // REQ-6.3.7 (H-2): crl_file must be non-empty when require_crl=true and verify_peer=true.
    if (tls_cfg.require_crl && tls_cfg.verify_peer && (tls_cfg.crl_file[0] == '\0')) {
        LOG_FATAL("DtlsUdpBackend", "REQ-6.3.7/HAZ-020: require_crl=true but crl_file is empty — "
                    "CRL revocation check disabled; init rejected (H-2, CWE-295)");
        return Result::ERR_INVALID;
    }

    // REQ-6.3.9 (H-8): verify_peer=false with a non-empty peer_hostname is an unsafe
    // operator configuration — hostname set for SNI but cert validation disabled (CWE-297).
    if ((!tls_cfg.verify_peer) && (tls_cfg.peer_hostname[0] != '\0')) {
        LOG_WARN_HI("DtlsUdpBackend", "REQ-6.3.9/HAZ-025: verify_peer=false but peer_hostname is non-empty — "
                    "unsafe configuration; init rejected (H-8, CWE-297)");
        return Result::ERR_INVALID;
    }

    // Post-condition: H-1 satisfied — verify_peer=true only when ca_file is set.
    NEVER_COMPILED_OUT_ASSERT(!(tls_cfg.verify_peer && (tls_cfg.ca_file[0] == '\0')));
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::DTLS_UDP);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    // S5: validate config before any channels[] access (REQ-6.1.1, ChannelConfig.hpp).
    if (!transport_config_valid(config)) {
        LOG_WARN_HI("DtlsUdpBackend", "init: num_channels=%u exceeds MAX_CHANNELS; rejecting config",
                    config.num_channels);
        return Result::ERR_INVALID;
    }

    // REQ-6.3.6/7/9: validate DTLS-specific config before any state mutation.
    // validate_dtls_init_config() returns OK immediately when tls_enabled=false.
    // Returns ERR_IO (H-1), ERR_INVALID (H-2, H-8), or OK.
    {
        Result vres = validate_dtls_init_config(config.tls);
        if (!result_ok(vres)) { return vres; }
    }

    m_cfg         = config;
    m_is_server   = config.is_server;
    m_tls_enabled = config.tls.tls_enabled;

    // F-5 / REQ-6.4.1: DtlsUdpBackend is IPv4-only (client_connect_and_handshake
    // constructs sockaddr_in / AF_INET). Detect IPv6 addresses (contain ':') and
    // fail fast here rather than silently connect() to 0.0.0.0:port after inet_pton
    // returns 0. To add IPv6: replace inet_pton(AF_INET,...) with getaddrinfo().
    // Power of 10 Rule 2: bounded loop — at most sizeof(peer_ip) iterations.
    for (uint32_t ci = 0U;
         ci < sizeof(m_cfg.peer_ip) && m_cfg.peer_ip[ci] != '\0';
         ++ci) {
        if (m_cfg.peer_ip[ci] == ':') {
            LOG_FATAL("DtlsUdpBackend", "IPv6 peer_ip not supported; DtlsUdpBackend is IPv4-only "
                        "(F-5, REQ-6.4.1). Use an IPv4 address.");
            return Result::ERR_INVALID;
        }
    }

    // REQ-6.2.4 / SEC-023 / SEC-027: reset peer registration state for a fresh init()
    m_peer_node_id        = static_cast<NodeId>(NODE_ID_INVALID);
    m_peer_hello_received = false;
    m_pending_src_port    = 0U;   // SEC-027: clear candidate port for new session
    m_peer_src_port       = 0U;   // SEC-023: clear locked port for new session
    m_crl_loaded          = false;

    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    Result res = create_and_bind_udp_socket(config);
    if (!result_ok(res)) { return res; }

    res = run_tls_handshake_phase(config);
    if (!result_ok(res)) { return res; }

    NEVER_COMPILED_OUT_ASSERT(m_open);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// register_local_id()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::register_local_id(NodeId id)
{
    NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID);  // pre-condition: valid NodeId
    NEVER_COMPILED_OUT_ASSERT(m_open);  // pre-condition: transport must be initialised
    // REQ-6.1.8 / REQ-6.1.10 / SEC-026: store NodeId, then send HELLO in client mode
    // so the server registers this side before any DATA frame arrives.  Server mode
    // skips HELLO here — no client peer address is known at registration time.  The
    // server sends its HELLO response inside process_hello_or_validate() the moment
    // the first client HELLO arrives (SEC-026: bidirectional NodeId registration).
    m_local_node_id = id;
    if (!m_is_server) {
        return send_hello_datagram();
    }
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// commit_pending_src_port() — private helper (SEC-027)
//
// Moves m_pending_src_port into m_peer_src_port, completing the two-phase
// port-locking protocol.  Called from process_hello_or_validate() on the
// first valid HELLO so that only a peer sending a valid HELLO can lock the
// port — not any raw datagram from the trusted IP.
// ─────────────────────────────────────────────────────────────────────────────

void DtlsUdpBackend::commit_pending_src_port()
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // pre: transport must be open
    // No-op in client mode (port is the configured m_cfg.peer_port) or when
    // no pending port was recorded (DTLS path never sets m_pending_src_port).
    if (m_is_server && (m_pending_src_port != 0U)) {
        m_peer_src_port    = m_pending_src_port;
        m_pending_src_port = 0U;
        NEVER_COMPILED_OUT_ASSERT(m_peer_src_port != 0U);  // post: port now locked
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// send_hello_datagram() — private helper (REQ-6.1.8, REQ-6.1.10, SEC-026)
//
// Serializes a HELLO envelope and sends it via send_wire_bytes() (DTLS or
// plaintext UDP), bypassing the impairment engine.  Used by the client to
// initiate registration and by the server to respond to the first client HELLO
// (SEC-026: bidirectional NodeId registration).
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::send_hello_datagram()
{
    NEVER_COMPILED_OUT_ASSERT(m_local_node_id != NODE_ID_INVALID);  // pre: NodeId set
    // SEC-026: server also calls this to respond to client HELLO; no is_server guard.

    MessageEnvelope hello;
    envelope_init(hello);
    hello.message_type   = MessageType::HELLO;
    hello.source_id      = m_local_node_id;
    hello.destination_id = NODE_ID_INVALID;  // server NodeId not yet known
    hello.payload_length = 0U;

    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(hello, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res) || wire_len > DTLS_MAX_DATAGRAM_BYTES) {
        LOG_WARN_HI("DtlsUdpBackend", "send_hello_datagram: serialize failed or MTU exceeded");
        return Result::ERR_IO;
    }

    Result send_res = send_wire_bytes(m_wire_buf, wire_len);
    if (send_res != Result::OK) {
        LOG_WARN_HI("DtlsUdpBackend", "send_hello_datagram: send_wire_bytes failed");
        return send_res;
    }

    LOG_INFO("DtlsUdpBackend", "HELLO sent: local_id=%u", static_cast<unsigned int>(m_local_node_id));
    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // post: something was sent
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_wire_bytes() — CC-reduction helper for send_message
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::send_wire_bytes(const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U && len <= DTLS_MAX_DATAGRAM_BYTES);

    if (m_tls_enabled) {
        int ret = m_ops->ssl_write(&m_ssl, buf, static_cast<size_t>(len));
        if (ret < 0) {
            log_mbedtls_err("DtlsUdpBackend", "ssl_write", ret);
            return Result::ERR_IO;
        }
    } else {
        // SEC-026 / SEC-023: server must reply to the client's ephemeral source
        // port (m_peer_src_port, learned on first datagram), not m_cfg.peer_port
        // which is the server's own listen port. Client always uses m_cfg.peer_port.
        const uint16_t dest_port = (m_is_server && (m_peer_src_port != 0U))
                                   ? m_peer_src_port : m_cfg.peer_port;
        if (!m_sock_ops->send_to(m_sock_fd, buf, len,
                                 m_cfg.peer_ip, dest_port)) {
            LOG_WARN_LO("DtlsUdpBackend", "send_message: socket_send_to failed to %s:%u",
                        m_cfg.peer_ip, dest_port);
            return Result::ERR_IO;
        }
    }
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_one_envelope() — CC-reduction helper for flush_outbound_batch
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::send_one_envelope(const MessageEnvelope& env, bool is_current)
{
    NEVER_COMPILED_OUT_ASSERT(m_sock_ops != nullptr);
    NEVER_COMPILED_OUT_ASSERT(m_ops     != nullptr);

    uint32_t dlen = 0U;
    Result res = Serializer::serialize(env, m_wire_buf, SOCKET_RECV_BUF_BYTES, dlen);
    if (!result_ok(res) || dlen > DTLS_MAX_DATAGRAM_BYTES) {
        return is_current;  // Serialize or MTU failure: attribute only if current
    }

    Result send_res = send_wire_bytes(m_wire_buf, dlen);
    if (send_res != Result::OK) {
        if (!is_current) {
            LOG_WARN_LO("DtlsUdpBackend", "send_wire_bytes failed for delayed envelope");
        }
        return is_current;  // Send failed: attribute only if current
    }
    return false;  // Success
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_outbound_batch() — CC-reduction helper for send_message
// ─────────────────────────────────────────────────────────────────────────────
//
// Sends each envelope in @p batch to the peer via send_one_envelope().
// Three-case attribution:
//   Current envelope in batch and send fails  → return true (ERR_IO to caller).
//   Current envelope NOT in batch             → return false (queued for later).
//   Non-current envelope send fails           → log WARNING_LO, continue, return false.
// Power of 10 Rule 2: fixed loop bound (count ≤ IMPAIR_DELAY_BUF_SIZE).

bool DtlsUdpBackend::flush_outbound_batch(const MessageEnvelope& envelope,
                                           const MessageEnvelope* batch,
                                           uint32_t count)
{
    NEVER_COMPILED_OUT_ASSERT(batch != nullptr);
    NEVER_COMPILED_OUT_ASSERT(count <= IMPAIR_DELAY_BUF_SIZE);

    bool current_failed = false;
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        bool is_current = (batch[i].source_id  == envelope.source_id &&
                           batch[i].message_id == envelope.message_id);
        if (send_one_envelope(batch[i], is_current)) {
            current_failed = true;
        }
    }
    return current_failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));

    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        LOG_WARN_LO("DtlsUdpBackend", "send_message: serialize failed");
        return res;
    }

    // Enforce DTLS MTU to prevent IP fragmentation (REQ-6.4.4)
    if (wire_len > DTLS_MAX_DATAGRAM_BYTES) {
        LOG_WARN_HI("DtlsUdpBackend", "send_message: serialized len %u exceeds DTLS MTU %u; rejected",
                    wire_len, DTLS_MAX_DATAGRAM_BYTES);
        return Result::ERR_INVALID;
    }

    // Apply impairment: process_outbound queues the message into the delay buffer.
    // ERR_IO   — intentional loss-impairment drop; return OK (expected behavior).
    // ERR_FULL — delay buffer full; message not queued; propagate to caller.
    // OK       — message queued with release_us = now_us (no latency) or future time.
    uint64_t now_us = timestamp_now_us();
    res = m_impairment.process_outbound(envelope, now_us);
    if (res == Result::ERR_IO) {
        return Result::OK;  // intentional loss-impairment drop
    }
    if (res != Result::OK) {
        return res;  // ERR_FULL: delay buffer full; message not queued
    }

    // process_outbound() already queued the message; collect_deliverable() returns
    // all messages due now — covering both the zero-delay pass-through and timed-
    // delay cases. Do NOT also call send_wire_bytes() here; that would send every
    // message twice. (HAZ-003)
    // flush_outbound_batch() handles the three-case attribution (see its comment).
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us, m_delay_buf,
                                                              IMPAIR_DELAY_BUF_SIZE);

    if (flush_outbound_batch(envelope, m_delay_buf, delayed_count)) {
        return Result::ERR_IO;
    }

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::receive_message(MessageEnvelope& envelope,
                                        uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(m_sock_fd >= 0);

    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) { return res; }

    uint32_t poll_count = (timeout_ms + 99U) / 100U;
    if (poll_count > 50U) { poll_count = 50U; }
    NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U);

    // Power of 10 Rule 2: fixed loop bound (poll_count ≤ 50)
    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt) {
        (void)recv_one_dtls_datagram(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }

        uint64_t now_us = timestamp_now_us();
        flush_delayed_to_wire(now_us);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }
    }

    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void DtlsUdpBackend::close()
{
    if (m_tls_enabled && m_open) {
        // Best-effort DTLS close_notify; peer may already be gone
        (void)mbedtls_ssl_close_notify(&m_ssl);
    }
    mbedtls_ssl_free(&m_ssl);
    mbedtls_ssl_init(&m_ssl);

    m_net_ctx.fd = -1;

    if (m_sock_fd >= 0) {
        m_sock_ops->do_close(m_sock_fd);
        m_sock_fd = -1;
        ++m_connections_closed;  // REQ-7.2.4: socket close event
    }

    // REQ-6.2.4 / SEC-023 / SEC-027: reset peer registration so a subsequent init() starts fresh
    m_peer_node_id        = static_cast<NodeId>(NODE_ID_INVALID);
    m_peer_hello_received = false;
    m_pending_src_port    = 0U;   // SEC-027: clear candidate port
    m_peer_src_port       = 0U;   // SEC-023: clear locked port for new session

    m_open = false;
    LOG_INFO("DtlsUdpBackend", "Transport closed (DTLS=%s)", m_tls_enabled ? "ON" : "OFF");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool DtlsUdpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open == (m_sock_fd >= 0) || !m_open);
    return m_open;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_transport_stats() — REQ-7.2.4 / REQ-7.2.2 observability
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

void DtlsUdpBackend::get_transport_stats(TransportStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened >= m_connections_closed);  // Assert: monotonic counters
    NEVER_COMPILED_OUT_ASSERT(m_connections_closed <= m_connections_opened);  // Assert: closed ≤ opened

    transport_stats_init(out);
    out.connections_opened = m_connections_opened;
    out.connections_closed = m_connections_closed;
    out.impairment         = m_impairment.get_stats();
}
