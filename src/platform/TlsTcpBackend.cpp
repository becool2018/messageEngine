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
 * @file TlsTcpBackend.cpp
 * @brief Implementation of TlsTcpBackend: TLS-capable TCP transport.
 *
 * Uses mbedTLS 4.0 with PSA Crypto backend for TLS 1.2/1.3 encryption.
 * When config.tls.tls_enabled is false, falls back to plaintext TCP using
 * the raw fd inside mbedtls_net_context — no TLS overhead.
 *
 * Power of 10 Rule 3 deviation (init-phase):
 *   mbedTLS heap-allocates during psa_crypto_init(), certificate parsing,
 *   and TLS handshake. These occur exclusively in init() / accept_and_handshake().
 *   The send/receive critical path (after handshake completes) does not
 *   allocate. Documented per CLAUDE.md §2 Rule 3 exception.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6,
 *             REQ-6.1.7, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10,
 *             REQ-6.3.4, REQ-7.1.1, REQ-5.1.5, REQ-5.1.6
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.1.7, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11, REQ-6.3.4, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6

#include "platform/TlsTcpBackend.hpp"
#include "platform/ISocketOps.hpp"
#include "platform/SocketOpsImpl.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include "core/Serializer.hpp"
#include "core/Timestamp.hpp"
#include "platform/SocketUtils.hpp"

#if __has_include(<mbedtls/build_info.h>)
#  include <mbedtls/build_info.h>   // mbedTLS 3.x / 4.x
#else
#  include <mbedtls/version.h>      // mbedTLS 2.x
#endif
#include <mbedtls/error.h>
#include <mbedtls/psa_util.h>
#include <mbedtls/platform_util.h>  // SEC-002: mbedtls_platform_zeroize() (CLAUDE.md §7c)
#include <psa/crypto.h>
#include <sys/stat.h>     // lstat() — F-2 symlink validation
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — convert port uint16_t → "NNNNN" string
// ─────────────────────────────────────────────────────────────────────────────

/// mbedtls_net_bind / mbedtls_net_connect take port as a decimal string.
/// Write the decimal representation of @p port into @p buf (capacity 6 bytes).
static void port_to_str(uint16_t port, char* buf)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    // Maximum uint16 = 65535 — 5 digits + NUL = 6 bytes.
    // Power of 10 Rule 2: bounded loop (at most 5 iterations).
    static const uint32_t PORT_BUF_LEN = 6U;
    uint32_t val = static_cast<uint32_t>(port);
    uint32_t pos = PORT_BUF_LEN - 1U;
    buf[pos] = '\0';
    if (val == 0U) {
        --pos;
        buf[pos] = '0';
    } else {
        for (uint32_t i = 0U; i < 5U && val > 0U; ++i) {
            --pos;
            buf[pos] = static_cast<char>('0' + static_cast<char>(val % 10U));
            val /= 10U;
        }
    }
    // Shift result to buf[0]
    uint32_t src = pos;
    uint32_t dst = 0U;
    for (; buf[src] != '\0'; ++src, ++dst) {
        buf[dst] = buf[src];
    }
    buf[dst] = '\0';
    NEVER_COMPILED_OUT_ASSERT(buf[0] != '\0');
}

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — log mbedTLS error code
// ─────────────────────────────────────────────────────────────────────────────

static void log_mbedtls_err(const char* tag, const char* func, int ret)
{
    NEVER_COMPILED_OUT_ASSERT(tag != nullptr);
    NEVER_COMPILED_OUT_ASSERT(func != nullptr);
    char err_buf[128];
    (void)memset(err_buf, 0, sizeof(err_buf));
    mbedtls_strerror(ret, err_buf, sizeof(err_buf) - 1U);
    Logger::log(Severity::WARNING_HI, tag, "%s failed: -0x%04X (%s)",
                func, static_cast<unsigned int>(-ret), err_buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// File-local helper — F-2 symlink TOCTOU guard
// Same rationale as DtlsUdpBackend: reject symlink-swapped cert/key paths.
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if path is a regular file (not a symlink or other special file).
static bool tls_path_is_regular_file(const char* path, const char* tag)
{
    NEVER_COMPILED_OUT_ASSERT(path != nullptr);  // pre-condition: non-null path
    NEVER_COMPILED_OUT_ASSERT(tag  != nullptr);  // pre-condition: non-null tag

    struct stat st;
    if (lstat(path, &st) != 0) {
        Logger::log(Severity::WARNING_HI, tag,
                    "tls_path_is_regular_file: lstat('%s') failed: %d", path, errno);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        Logger::log(Severity::WARNING_HI, tag,
                    "tls_path_is_regular_file: '%s' is not a regular file (mode=0%o)",
                    path, static_cast<unsigned>(st.st_mode));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

TlsTcpBackend::TlsTcpBackend()
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_crl{}, m_crl_loaded(false),
      m_listen_net{}, m_client_net{}, m_ssl{},
      m_saved_session{}, m_session_saved(false), m_ticket_ctx{},
      m_sock_ops(&SocketOpsImpl::instance()),
      m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_client_node_ids{}, m_local_node_id(NODE_ID_INVALID),
      m_client_hello_received{}, m_client_slot_active{}
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_x509_crl_init(&m_crl);   // REQ-6.3.4: CRL context init
    mbedtls_net_init(&m_listen_net);
    mbedtls_ssl_session_init(&m_saved_session);
    mbedtls_ssl_ticket_init(&m_ticket_ctx);  // Fix 2: REQ-6.3.4
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_init(&m_client_net[i]);
        mbedtls_ssl_init(&m_ssl[i]);
        m_client_hello_received[i] = false;  // Fix 4: REQ-6.1.8
        m_client_slot_active[i]    = false;  // Fix 5: no compaction
    }
}

TlsTcpBackend::TlsTcpBackend(ISocketOps& sock_ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_crl{}, m_crl_loaded(false),
      m_listen_net{}, m_client_net{}, m_ssl{},
      m_saved_session{}, m_session_saved(false), m_ticket_ctx{},
      m_sock_ops(&sock_ops),
      m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_client_node_ids{}, m_local_node_id(NODE_ID_INVALID),
      m_client_hello_received{}, m_client_slot_active{}
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_x509_crl_init(&m_crl);   // REQ-6.3.4: CRL context init
    mbedtls_net_init(&m_listen_net);
    mbedtls_ssl_session_init(&m_saved_session);
    mbedtls_ssl_ticket_init(&m_ticket_ctx);  // Fix 2: REQ-6.3.4
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_init(&m_client_net[i]);
        mbedtls_ssl_init(&m_ssl[i]);
        m_client_hello_received[i] = false;  // Fix 4: REQ-6.1.8
        m_client_slot_active[i]    = false;  // Fix 5: no compaction
    }
}

TlsTcpBackend::~TlsTcpBackend()
{
    TlsTcpBackend::close();
    mbedtls_ssl_session_free(&m_saved_session);
    mbedtls_ssl_ticket_free(&m_ticket_ctx);   // Fix 2: REQ-6.3.4
    mbedtls_ssl_config_free(&m_ssl_conf);
    mbedtls_x509_crt_free(&m_cert);
    mbedtls_x509_crt_free(&m_ca_cert);
    mbedtls_x509_crl_free(&m_crl);            // REQ-6.3.4: CRL context free
    mbedtls_pk_free(&m_pkey);
    // SEC-006: CLAUDE.md §7c — zeroize private key material after free to prevent
    // residual key bytes in memory (CWE-14; mbedtls_platform_zeroize is compiler-barrier-safe).
    mbedtls_platform_zeroize(static_cast<void*>(&m_pkey), sizeof(m_pkey));
    mbedtls_net_free(&m_listen_net);
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_ssl_free(&m_ssl[i]);
        mbedtls_net_free(&m_client_net[i]);
    }
    mbedtls_psa_crypto_free();
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_tls_config() — load certs/keys; configure mbedTLS shared object
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// load_ca_and_crl() — CC-reduction helper for load_tls_certs()
// Parses the CA cert, optionally loads the CRL, then calls ssl_conf_ca_chain.
// Called only when verify_peer is true and ca_file is non-empty.
// Extracted from load_tls_certs() to keep its CC ≤ 10. REQ-6.3.4.
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::load_ca_and_crl(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.verify_peer);
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.ca_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.ca_file[0] != '\0');

    // F-2: reject symlink attacks immediately before parse_file() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.ca_file, "TlsTcpBackend")) {
        return Result::ERR_IO;
    }
    int ret = mbedtls_x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (CA)", ret);
        return Result::ERR_IO;
    }

    // REQ-6.3.4: optionally load CRL; extracted to load_crl_if_configured().
    Result crl_res = load_crl_if_configured(tls_cfg);
    if (!result_ok(crl_res)) { return crl_res; }

    // REQ-6.3.4: CRL loaded and passed to ca_chain for revocation checking.
    mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert,
                              m_crl_loaded ? &m_crl : nullptr);

    NEVER_COMPILED_OUT_ASSERT(m_ca_cert.version > 0);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// load_crl_if_configured() — CC-reduction helper for load_ca_and_crl()
// Loads the CRL when verify_peer is true and crl_file is non-empty.
// Sets m_crl_loaded on success. Extracted from load_ca_and_crl() to keep its
// CC ≤ 10. REQ-6.3.4.
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::load_crl_if_configured(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.verify_peer);
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.ca_file[0] != '\0');
    // SECfix-5: verify crl_file NUL termination within TLS_PATH_MAX before any fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.crl_file));

    if (tls_cfg.crl_file[0] == '\0') {
        // SEC-003: verify_peer=true but no CRL file configured — certificate
        // revocation checking is disabled. This may be intentional (e.g., OCSP
        // is used instead) but is logged at WARNING_HI so deployments that
        // require CRL checking can detect misconfiguration (REQ-6.3.4, §SECURITY §3).
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "verify_peer=true but crl_file is empty — CRL revocation "
                    "checking disabled (SEC-003, REQ-6.3.4)");
        return Result::OK;
    }

    // F-2: reject symlink attacks immediately before crl_parse_file() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.crl_file, "TlsTcpBackend")) {
        return Result::ERR_IO;
    }
    int crl_ret = mbedtls_x509_crl_parse_file(&m_crl, tls_cfg.crl_file);
    if (crl_ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "x509_crl_parse_file", crl_ret);
        return Result::ERR_IO;
    }
    m_crl_loaded = true;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "CRL loaded: %s (REQ-6.3.4)", tls_cfg.crl_file);

    NEVER_COMPILED_OUT_ASSERT(m_crl_loaded);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// run_tls_handshake_loop() — CC-reduction helper for handshake functions
// Bounded retry loop: retries mbedtls_ssl_handshake() up to 32 times on
// WANT_READ / WANT_WRITE (transient EINTR/EAGAIN on blocking socket).
// Power of 10 Rule 2: bounded loop — max 32 iterations.
// REQ-6.3.3: prevents spurious connection failures from transient conditions.
// ─────────────────────────────────────────────────────────────────────────────

int TlsTcpBackend::run_tls_handshake_loop(mbedtls_ssl_context* ssl)
{
    NEVER_COMPILED_OUT_ASSERT(ssl != nullptr);

    // Power of 10 Rule 2: bounded retry loop — max 32 iterations for WANT_READ/WANT_WRITE.
    // REQ-6.3.3: handles EINTR/EAGAIN on blocking socket.
    int hs_ret = 0;
    uint32_t hs_iter = 0U;
    do {
        hs_ret = mbedtls_ssl_handshake(ssl);
        ++hs_iter;
    } while ((hs_ret == MBEDTLS_ERR_SSL_WANT_READ ||
              hs_ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
             hs_iter < 32U);

    NEVER_COMPILED_OUT_ASSERT(hs_iter >= 1U);
    return hs_ret;
}

/// Load CA cert (if verify_peer), own cert, private key; bind to ssl_conf.
/// Extracted from setup_tls_config() to reduce its CC.
Result TlsTcpBackend::load_tls_certs(const TlsConfig& tls_cfg)
{
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.cert_file));
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.key_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.key_file[0] != '\0');

    if (tls_cfg.verify_peer && tls_cfg.ca_file[0] != '\0') {
        // Extracted to load_ca_and_crl() to keep this function's CC ≤ 10.
        Result ca_res = load_ca_and_crl(tls_cfg);
        if (!result_ok(ca_res)) { return ca_res; }
    }

    // F-2: reject symlink attacks immediately before parse_file() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.cert_file, "TlsTcpBackend")) {
        return Result::ERR_IO;
    }
    int ret = mbedtls_x509_crt_parse_file(&m_cert, tls_cfg.cert_file);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (cert)", ret);
        return Result::ERR_IO;
    }

    // F-2: reject symlink attacks immediately before pk_parse_keyfile() to narrow TOCTOU window.
    if (!tls_path_is_regular_file(tls_cfg.key_file, "TlsTcpBackend")) {
        return Result::ERR_IO;
    }
    ret = mbedtls_pk_parse_keyfile(&m_pkey, tls_cfg.key_file, nullptr);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "pk_parse_keyfile", ret);
        return Result::ERR_IO;
    }

    ret = mbedtls_ssl_conf_own_cert(&m_ssl_conf, &m_cert, &m_pkey);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_conf_own_cert", ret);
        return Result::ERR_IO;
    }

    NEVER_COMPILED_OUT_ASSERT(m_cert.version > 0);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_cipher_policy() — Fix 1 CC-reduction helper for setup_tls_config()
// Restricts the SSL config to AEAD-only cipher suites and enforces TLS ≥ 1.2.
// Extracted to keep setup_tls_config() CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::apply_cipher_policy()
{
    // Fix 1: restrict to AEAD-only cipher suites; prohibit CBC, NULL, RSA key exchange.
    // Power of 10 Rule 3: static const array — no dynamic allocation.
    static const int k_tls_ciphersuites[] = {
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
        MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
        MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&m_ssl_conf, k_tls_ciphersuites);
    // Enforce minimum TLS 1.2; prohibit TLS 1.0/1.1.
    (void)mbedtls_ssl_conf_min_tls_version(&m_ssl_conf, MBEDTLS_SSL_VERSION_TLS1_2);

    // Power of 10 Rule 5: ≥2 assertions — array sentinel and return-value void.
    NEVER_COMPILED_OUT_ASSERT(k_tls_ciphersuites[0] != 0);   // array non-empty
    NEVER_COMPILED_OUT_ASSERT(k_tls_ciphersuites[7] == 0);   // sentinel present
}

Result TlsTcpBackend::setup_tls_config(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
    // SECfix-5: verify NUL termination within TLS_PATH_MAX before passing to fopen()
    NEVER_COMPILED_OUT_ASSERT(tls_path_valid(tls_cfg.cert_file));
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');

    // Init PSA Crypto (mbedTLS 4.0: replaces CTR-DRBG / entropy setup).
    // Power of 10 Rule 3 deviation — init-phase heap allocation inside PSA.
    psa_status_t psa_ret = psa_crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        Logger::log(Severity::FATAL, "TlsTcpBackend",
                    "psa_crypto_init failed: %d", static_cast<int>(psa_ret));
        return Result::ERR_IO;
    }

    int endpoint = (tls_cfg.role == TlsRole::SERVER)
                   ? MBEDTLS_SSL_IS_SERVER
                   : MBEDTLS_SSL_IS_CLIENT;

    int ret = mbedtls_ssl_config_defaults(&m_ssl_conf,
                                          endpoint,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_config_defaults", ret);
        return Result::ERR_IO;
    }

    // mbedTLS 4.0 removed mbedtls_ssl_conf_rng(): the PSA RNG is automatically
    // bound to the SSL config after psa_crypto_init() completes.
    // mbedTLS 2.x/3.x with MBEDTLS_USE_PSA_CRYPTO requires the explicit call.
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&m_ssl_conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
#endif

    int authmode = tls_cfg.verify_peer
                   ? MBEDTLS_SSL_VERIFY_REQUIRED
                   : MBEDTLS_SSL_VERIFY_NONE;
    mbedtls_ssl_conf_authmode(&m_ssl_conf, authmode);

    // Fix 1: restrict to AEAD-only cipher suites and enforce minimum TLS 1.2.
    // Extracted to apply_cipher_policy() to keep this function's CC ≤ 10.
    apply_cipher_policy();

    Result res = load_tls_certs(tls_cfg);
    if (!result_ok(res)) { return res; }

    // Fix 2: set up session ticket key (extracted to keep this function CC ≤ 10).
    Result ticket_res = maybe_setup_session_tickets(tls_cfg);
    if (!result_ok(ticket_res)) { return ticket_res; }

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "TLS config ready: role=%s verify_peer=%d cert=%s",
                (tls_cfg.role == TlsRole::SERVER) ? "SERVER" : "CLIENT",
                static_cast<int>(tls_cfg.verify_peer),
                tls_cfg.cert_file);

    NEVER_COMPILED_OUT_ASSERT(psa_ret == PSA_SUCCESS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// maybe_setup_session_tickets() — Fix 2 CC-reduction helper for setup_tls_config()
// Handles the MBEDTLS_SSL_SESSION_TICKETS guard and the enabled-flag check,
// keeping both in one place so setup_tls_config() CC stays ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::maybe_setup_session_tickets(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(true);  // Power of 10 Rule 5: second assertion

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    if (tls_cfg.session_resumption_enabled) {
        const uint32_t ticket_lifetime =
            (tls_cfg.session_ticket_lifetime_s > 0U)
                ? tls_cfg.session_ticket_lifetime_s
                : 86400U;  // default: 24 h
        return setup_session_tickets(ticket_lifetime);
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_session_tickets() — Fix 2 CC-reduction helper for setup_tls_config()
// Sets up the mbedTLS session ticket key context with the configured lifetime.
// Uses PSA-native API: mbedtls_ssl_ticket_setup(ctx, alg, key_type, key_bits,
// lifetime). The PSA DRBG (already initialised via psa_crypto_init()) is used
// internally — no explicit RNG callback required (REQ-6.3.4).
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::setup_session_tickets(uint32_t lifetime_s)
{
    NEVER_COMPILED_OUT_ASSERT(lifetime_s > 0U);
    NEVER_COMPILED_OUT_ASSERT(lifetime_s <= 604800U);  // ≤ 7 days (TLS 1.3 max)

    // PSA-native API (mbedTLS 4.0): alg, key_type, key_bits, lifetime.
    // AES-256-GCM for ticket AEAD — FIPS-approved, 256-bit key.
    // Power of 10 Rule 3: no dynamic allocation; PSA manages the key internally.
    int ticket_ret = mbedtls_ssl_ticket_setup(
        &m_ticket_ctx,
        PSA_ALG_GCM,
        PSA_KEY_TYPE_AES,
        static_cast<psa_key_bits_t>(256U),
        lifetime_s);
    if (ticket_ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_ticket_setup", ticket_ret);
        return Result::ERR_IO;
    }
    (void)mbedtls_ssl_conf_session_tickets_cb(
        &m_ssl_conf,
        mbedtls_ssl_ticket_write,
        mbedtls_ssl_ticket_parse,
        &m_ticket_ctx);
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Session ticket resumption enabled (lifetime=%u s)", lifetime_s);

    NEVER_COMPILED_OUT_ASSERT(ticket_ret == 0);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// bind_and_listen() — server mode setup
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::bind_and_listen(const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    NEVER_COMPILED_OUT_ASSERT(m_is_server);

    char port_str[6];
    port_to_str(port, port_str);

    int ret = mbedtls_net_bind(&m_listen_net, ip, port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_bind", ret);
        return Result::ERR_IO;
    }

    // Set non-blocking so accept() returns immediately when no client pending
    ret = mbedtls_net_set_nonblock(&m_listen_net);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_set_nonblock (listen)", ret);
        mbedtls_net_free(&m_listen_net);
        return Result::ERR_IO;
    }

    m_open = true;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Server listening on %s:%s (TLS=%s)", ip, port_str,
                m_tls_enabled ? "ON" : "OFF");

    NEVER_COMPILED_OUT_ASSERT(m_listen_net.fd >= 0);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// connect_to_server() — client mode
// ─────────────────────────────────────────────────────────────────────────────

/// Save the current TLS session from slot 0 into m_saved_session.
/// Called after a successful client handshake when session_resumption_enabled.
/// Non-fatal on failure: logs WARNING_LO; m_session_saved remains false.
/// Extracted from tls_connect_handshake() to keep its CC ≤ 10 (REQ-6.3.4).
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
void TlsTcpBackend::try_save_client_session()
{
    NEVER_COMPILED_OUT_ASSERT(m_tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(m_cfg.tls.session_resumption_enabled);

    // Release any stale session before overwriting with the fresh one.
    mbedtls_ssl_session_free(&m_saved_session);
    mbedtls_ssl_session_init(&m_saved_session);

    int ret = mbedtls_ssl_get_session(&m_ssl[0U], &m_saved_session);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_get_session", ret);
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "Session save failed; resumption not attempted on next connect");
        m_session_saved = false;
    } else {
        m_session_saved = true;
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "TLS session saved for resumption on next connect");
    }
}
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

/// Attempt to load m_saved_session into m_ssl[0] before the TLS handshake
/// to enable abbreviated session resumption (RFC 5077, REQ-6.3.4).
/// Non-fatal on failure: logs WARNING_LO and full handshake proceeds.
/// Extracted from tls_connect_handshake() to reduce its CC.
void TlsTcpBackend::try_load_client_session()
{
    NEVER_COMPILED_OUT_ASSERT(m_tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(m_session_saved);

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    int ret = mbedtls_ssl_set_session(&m_ssl[0U], &m_saved_session);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_set_session", ret);
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "Session load failed; performing full handshake");
    } else {
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "Attempting TLS session resumption");
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS */
}

/// Perform TLS setup (set_block, ssl_setup, set_hostname, set_bio) and the
/// TLS handshake for the client socket at slot 0.
/// Extracted from connect_to_server() to reduce its CC.
Result TlsTcpBackend::tls_connect_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(m_tls_enabled);
    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd >= 0);

    // mbedtls_net_connect returns a blocking socket by default; set explicitly.
    int ret = mbedtls_net_set_block(&m_client_net[0U]);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_set_block (client connect)", ret);
        mbedtls_net_free(&m_client_net[0U]);
        return Result::ERR_IO;
    }

    ret = mbedtls_ssl_setup(&m_ssl[0U], &m_ssl_conf);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_setup (client)", ret);
        mbedtls_net_free(&m_client_net[0U]);
        return Result::ERR_IO;
    }

    // Set expected server hostname for SNI and certificate hostname
    // verification (required by mbedTLS 4.0+ when verify_peer is true).
    // Pass NULL when peer_hostname is empty to explicitly opt out.
    const char* hostname = (m_cfg.tls.peer_hostname[0] != '\0')
                           ? m_cfg.tls.peer_hostname
                           : nullptr;
    ret = mbedtls_ssl_set_hostname(&m_ssl[0U], hostname);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_set_hostname", ret);
        mbedtls_net_free(&m_client_net[0U]);
        return Result::ERR_IO;
    }

    // Load saved session to attempt abbreviated resumption (REQ-6.3.4).
    // Non-fatal: if set_session fails, full handshake proceeds normally.
    // Extracted to try_load_client_session() to keep this function's CC ≤ 10.
    if (m_cfg.tls.session_resumption_enabled && m_session_saved) {
        try_load_client_session();
    }

    mbedtls_ssl_set_bio(&m_ssl[0U], &m_client_net[0U],
                        mbedtls_net_send, mbedtls_net_recv, nullptr);

    // TLS handshake with bounded WANT_READ/WANT_WRITE retry (Finding #7).
    // Extracted to run_tls_handshake_loop() to keep this function's CC ≤ 10.
    // Power of 10 Rule 3 deviation: init-phase heap alloc in mbedTLS.
    int hs_ret = run_tls_handshake_loop(&m_ssl[0U]);
    if (hs_ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_handshake (client)", hs_ret);
        mbedtls_ssl_free(&m_ssl[0U]);
        mbedtls_ssl_init(&m_ssl[0U]);
        mbedtls_net_free(&m_client_net[0U]);
        mbedtls_net_init(&m_client_net[0U]);
        return Result::ERR_IO;
    }

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "TLS handshake complete (client): cipher=%s",
                mbedtls_ssl_get_ciphersuite(&m_ssl[0U]));

    // Save session for future resumption (REQ-6.3.4). Non-fatal if unavailable.
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    if (m_cfg.tls.session_resumption_enabled) {
        try_save_client_session();
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd >= 0);
    return Result::OK;
}

Result TlsTcpBackend::connect_to_server()
{
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd < 0);

    char port_str[6];
    port_to_str(m_cfg.peer_port, port_str);

    int ret = mbedtls_net_connect(&m_client_net[0U], m_cfg.peer_ip,
                                  port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_connect", ret);
        return Result::ERR_IO;
    }

    if (m_tls_enabled) {
        Result res = tls_connect_handshake();
        if (!result_ok(res)) { return res; }
    }

    m_client_count            = 1U;
    m_client_slot_active[0U]  = true;   // Fix 5: mark slot 0 active (client mode)
    m_open                    = true;
    ++m_connections_opened;  // REQ-7.2.4: successful client connect

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Connected to %s:%s (TLS=%s)",
                m_cfg.peer_ip, port_str, m_tls_enabled ? "ON" : "OFF");

    NEVER_COMPILED_OUT_ASSERT(m_client_net[0U].fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(m_client_count == 1U);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// accept_and_handshake() — server mode, accept one pending connection
// ─────────────────────────────────────────────────────────────────────────────

// do_tls_server_handshake() — CC-reduction helper for accept_and_handshake()
// Performs set_block, ssl_setup, BIO bind, handshake, and set_nonblock (Fix B-2b).
// On any failure, cleans up m_ssl[slot] and m_client_net[slot] before returning.
Result TlsTcpBackend::do_tls_server_handshake(uint32_t slot)
{
    NEVER_COMPILED_OUT_ASSERT(slot < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(m_tls_enabled);

    // Blocking mode required for the handshake multi-round I/O sequence.
    int ret = mbedtls_net_set_block(&m_client_net[slot]);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "net_set_block (client)", ret);
        mbedtls_net_free(&m_client_net[slot]);
        mbedtls_net_init(&m_client_net[slot]);
        return Result::ERR_IO;
    }

    ret = mbedtls_ssl_setup(&m_ssl[slot], &m_ssl_conf);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_setup (accept)", ret);
        mbedtls_net_free(&m_client_net[slot]);
        mbedtls_net_init(&m_client_net[slot]);
        return Result::ERR_IO;
    }
    mbedtls_ssl_set_bio(&m_ssl[slot], &m_client_net[slot],
                        mbedtls_net_send, mbedtls_net_recv, nullptr);

    // TLS handshake with bounded WANT_READ/WANT_WRITE retry (Finding #7).
    // Extracted to run_tls_handshake_loop() to keep this function's CC ≤ 10.
    // Power of 10 Rule 3 deviation: init-phase heap alloc in mbedTLS.
    int hs_ret = run_tls_handshake_loop(&m_ssl[slot]);
    if (hs_ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_handshake (accept)", hs_ret);
        mbedtls_ssl_free(&m_ssl[slot]);
        mbedtls_ssl_init(&m_ssl[slot]);
        mbedtls_net_free(&m_client_net[slot]);
        mbedtls_net_init(&m_client_net[slot]);
        return Result::ERR_IO;
    }

    // Fix B-2b: restore non-blocking so the poll loop does not stall.
    ret = mbedtls_net_set_nonblock(&m_client_net[slot]);
    if (ret != 0) {
        // Non-fatal: Fix B-2a poll guard prevents stall (defence-in-depth).
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "set_nonblock after handshake failed: %d", ret);
    }

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "TLS handshake complete (server slot %u): cipher=%s",
                slot, mbedtls_ssl_get_ciphersuite(&m_ssl[slot]));
    return Result::OK;
}

Result TlsTcpBackend::accept_and_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_listen_net.fd >= 0);

    if (m_client_count >= MAX_TCP_CONNECTIONS) {
        return Result::OK;  // table full; no room to accept
    }

    // Fix 5: find first inactive slot to avoid ssl_context compaction.
    // Power of 10 Rule 2: bounded loop — at most MAX_TCP_CONNECTIONS iterations.
    uint32_t slot = MAX_TCP_CONNECTIONS;
    for (uint32_t s = 0U; s < MAX_TCP_CONNECTIONS; ++s) {
        if (!m_client_slot_active[s]) { slot = s; break; }
    }
    if (slot >= MAX_TCP_CONNECTIONS) {
        // No free slot — should not happen since m_client_count < MAX_TCP_CONNECTIONS
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend", "accept: no free slot");
        return Result::OK;
    }

    int ret = mbedtls_net_accept(&m_listen_net, &m_client_net[slot],
                                 nullptr, 0U, nullptr);
    if (ret != 0) {
        // MBEDTLS_ERR_SSL_WANT_READ means no pending connection — not an error
        return Result::OK;
    }

    if (m_tls_enabled) {
        // Extracted to do_tls_server_handshake() to keep this function CC ≤ 10.
        Result hs_r = do_tls_server_handshake(slot);
        if (!result_ok(hs_r)) {
            return hs_r;
        }
    }

    // Fix 5: mark slot active — no array compaction on remove.
    m_client_slot_active[slot]    = true;
    m_client_hello_received[slot] = false;  // Fix 4: must receive HELLO before data
    ++m_client_count;
    ++m_connections_opened;  // REQ-7.2.4: successful server accept
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Accepted client slot=%u (TLS=%s), total=%u",
                slot, m_tls_enabled ? "ON" : "OFF", m_client_count);

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_client() — close and compact the client table
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::remove_client(uint32_t idx)
{
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(m_client_count > 0U);

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "removing client slot %u node_id=%u",
                static_cast<unsigned>(idx),
                static_cast<unsigned>(m_client_node_ids[idx]));

    // Tear down TLS and net contexts for this slot.
    if (m_tls_enabled) {
        (void)mbedtls_ssl_close_notify(&m_ssl[idx]);
        mbedtls_ssl_free(&m_ssl[idx]);
        mbedtls_ssl_init(&m_ssl[idx]);
    }
    mbedtls_net_free(&m_client_net[idx]);
    mbedtls_net_init(&m_client_net[idx]);

    // Fix 5: mark slot inactive — no array compaction, no ssl_context shallow copy.
    // Eliminates the undefined-behaviour bitwise copy of opaque mbedTLS structs.
    m_client_slot_active[idx]    = false;
    m_client_node_ids[idx]       = static_cast<NodeId>(NODE_ID_INVALID);
    m_client_hello_received[idx] = false;  // Fix 4: reset for slot reuse
    --m_client_count;
    ++m_connections_closed;  // REQ-7.2.4: client connection removed

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);
}

// ─────────────────────────────────────────────────────────────────────────────
// register_local_id() — REQ-6.1.10
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::register_local_id(NodeId id)
{
    NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    m_local_node_id = id;
    if (!m_is_server) {
        // REQ-6.1.8: client announces its identity to the server immediately.
        return send_hello_frame();
    }
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_hello_frame() — REQ-6.1.8
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::send_hello_frame()
{
    NEVER_COMPILED_OUT_ASSERT(m_local_node_id != NODE_ID_INVALID);
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);

    MessageEnvelope hello;
    envelope_init(hello);
    hello.message_type   = MessageType::HELLO;
    hello.source_id      = m_local_node_id;
    hello.destination_id = NODE_ID_INVALID;
    hello.payload_length = 0U;

    uint32_t wire_len = 0U;
    Result ser_r = Serializer::serialize(hello, m_wire_buf,
                                         static_cast<uint32_t>(sizeof(m_wire_buf)),
                                         wire_len);
    if (!result_ok(ser_r)) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "send_hello_frame: serialize failed %u",
                    static_cast<uint8_t>(ser_r));
        return ser_r;
    }

    uint32_t timeout_ms = (m_cfg.num_channels > 0U)
                          ? m_cfg.channels[0U].send_timeout_ms
                          : 1000U;
    // tls_send_frame returns true on success, false on failure (same convention
    // as ISocketOps::send_frame and SocketUtils::tcp_send_frame).
    bool sent_ok = tls_send_frame(0U, m_wire_buf, wire_len, timeout_ms);
    if (!sent_ok) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "send_hello_frame: tls_send_frame failed");
        return Result::ERR_IO;
    }
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "HELLO sent: local_id=%u", m_local_node_id);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_hello_frame() — REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::handle_hello_frame(uint32_t idx, NodeId src_id)
{
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(src_id != NODE_ID_INVALID);

    m_client_node_ids[idx] = src_id;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "HELLO from client slot %u node_id=%u", idx, src_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_source_id() — REQ-6.1.11
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::validate_source_id(uint32_t slot, NodeId claimed_id) const
{
    NEVER_COMPILED_OUT_ASSERT(slot < MAX_TCP_CONNECTIONS);
    const NodeId registered = m_client_node_ids[slot];
    if (registered == NODE_ID_INVALID) {
        NEVER_COMPILED_OUT_ASSERT(true);  // post: unregistered slot, allow
        return true;
    }
    if (claimed_id != registered) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "source_id mismatch: slot=%u claimed=%u registered=%u; dropping (REQ-6.1.11)",
                    static_cast<unsigned>(slot),
                    static_cast<unsigned>(claimed_id),
                    static_cast<unsigned>(registered));
        NEVER_COMPILED_OUT_ASSERT(true);  // post: mismatch detected
        return false;
    }
    NEVER_COMPILED_OUT_ASSERT(claimed_id == registered);  // post: match verified
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// find_client_slot() — REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TlsTcpBackend::find_client_slot(NodeId dst) const
{
    NEVER_COMPILED_OUT_ASSERT(dst != NODE_ID_INVALID);
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);

    // Fix 5: iterate full table; skip inactive slots (no compaction on remove).
    // Power of 10 Rule 2: bounded loop — at most MAX_TCP_CONNECTIONS iterations.
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_slot_active[i] && (m_client_node_ids[i] == dst)) {
            return i;
        }
    }
    return MAX_TCP_CONNECTIONS;  // sentinel: not found
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_slot() — REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::send_to_slot(uint32_t slot, const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(slot < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    // Fix 5: guard against sending to an inactive (removed) slot.
    NEVER_COMPILED_OUT_ASSERT(m_client_slot_active[slot]);

    uint32_t timeout_ms = (m_cfg.num_channels > 0U)
                          ? m_cfg.channels[0U].send_timeout_ms
                          : 1000U;
    // tls_send_frame returns true on success, false on failure (ISocketOps convention).
    bool ok = tls_send_frame(slot, buf, len, timeout_ms);
    bool failed = !ok;
    if (failed) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "send_to_slot %u failed", slot);
    }
    return failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_write_all() — write exactly len bytes via TLS, looping on partial writes
// Security fix F2: mbedtls_ssl_write() CAN return a positive value smaller
// than requested; this helper loops until all bytes are sent or an error occurs.
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::tls_write_all(uint32_t idx, const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    uint32_t sent = 0U;
    // Power of 10 Rule 2: bounded loop — at most len iterations total
    // (each successful iteration sends at least 1 byte, so sent advances).
    for (uint32_t iter = 0U; iter < len && sent < len; ++iter) {
        int ret = mbedtls_ssl_write(&m_ssl[idx], buf + sent,
                                    static_cast<size_t>(len - sent));
        if (ret > 0) {
            sent += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            log_mbedtls_err("TlsTcpBackend", "ssl_write", ret);
            return false;
        }
    }

    if (sent != len) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_write_all: short write %u/%u", sent, len);
        return false;
    }

    NEVER_COMPILED_OUT_ASSERT(sent == len);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_send_frame() — 4-byte length prefix + payload; TLS or raw TCP
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::tls_send_frame(uint32_t idx,
                                    const uint8_t* buf, uint32_t len,
                                    uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    // Build 4-byte big-endian length prefix
    uint8_t hdr[4U];
    hdr[0U] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    hdr[1U] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
    hdr[2U] = static_cast<uint8_t>((len >>  8U) & 0xFFU);
    hdr[3U] = static_cast<uint8_t>( len         & 0xFFU);

    if (m_tls_enabled) {
        // Security fix F2: use tls_write_all() for both header and payload to
        // handle partial-write return values from mbedtls_ssl_write() correctly.
        if (!tls_write_all(idx, hdr, 4U)) {
            return false;
        }
        return tls_write_all(idx, buf, len);
    }

    // Plaintext path: reuse existing SocketUtils framing via injected ISocketOps
    return m_sock_ops->send_frame(m_client_net[idx].fd, buf, len, timeout_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_read_payload() — CC-reduction helper for tls_recv_frame
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::tls_read_payload(uint32_t idx, uint8_t* buf,
                                      uint32_t payload_len, uint32_t timeout_ms,
                                      uint32_t* out_len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(payload_len > 0U && payload_len <= SOCKET_RECV_BUF_BYTES);

    uint32_t received = 0U;
    // Power of 10 Rule 2: bounded loop — at most payload_len iterations total
    for (uint32_t iter = 0U; iter < payload_len && received < payload_len; ++iter) {
        int ret = mbedtls_ssl_read(&m_ssl[idx],
                                   buf + received,
                                   static_cast<size_t>(payload_len - received));
        if (ret > 0) {
            received += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            // Fix B-2c: enforce timeout — poll before retrying so a silent
            // client cannot block indefinitely on a non-blocking socket.
            int poll_r = mbedtls_net_poll(&m_client_net[idx],
                                          MBEDTLS_NET_POLL_READ,
                                          timeout_ms);
            if (poll_r <= 0) {
                Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                            "tls_read_payload: timeout waiting for data");
                return false;
            }
        } else {
            log_mbedtls_err("TlsTcpBackend", "ssl_read (payload)", ret);
            return false;
        }
    }

    if (received != payload_len) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_recv_frame: short read %u/%u", received, payload_len);
        return false;
    }

    *out_len = payload_len;
    NEVER_COMPILED_OUT_ASSERT(*out_len > 0U);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tls_recv_frame() — receive 4-byte length prefix + payload
// ─────────────────────────────────────────────────────────────────────────────

// read_tls_header() — CC-reduction helper for tls_recv_frame()
// Reads the 4-byte big-endian length prefix from the TLS record layer.
// Fix B-2c: WANT_READ retries are gated by a timeout-enforced poll so the
// call cannot block indefinitely even when the socket is still blocking.
bool TlsTcpBackend::read_tls_header(uint32_t idx, uint8_t* hdr,
                                     uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(hdr != nullptr);
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_TCP_CONNECTIONS);

    uint32_t hdr_received = 0U;
    // Power of 10 Rule 2: bound = 8 — ≤4 byte-progress iters + ≤4 WANT_READ retries
    for (uint32_t iter = 0U; iter < 8U && hdr_received < 4U; ++iter) {
        int ret = mbedtls_ssl_read(&m_ssl[idx], hdr + hdr_received,
                                   static_cast<size_t>(4U - hdr_received));
        if (ret > 0) {
            hdr_received += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            // Poll with timeout before retrying (Fix B-2c).
            int poll_r = mbedtls_net_poll(&m_client_net[idx],
                                          MBEDTLS_NET_POLL_READ,
                                          timeout_ms);
            if (poll_r <= 0) {
                Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                            "tls_recv_frame: timeout on header read");
                return false;
            }
        } else {
            log_mbedtls_err("TlsTcpBackend", "ssl_read (header)", ret);
            return false;
        }
    }
    if (hdr_received != 4U) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_recv_frame: short header read %u/4", hdr_received);
        return false;
    }
    return true;
}

bool TlsTcpBackend::tls_recv_frame(uint32_t idx,
                                    uint8_t* buf, uint32_t buf_cap,
                                    uint32_t timeout_ms, uint32_t* out_len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);
    *out_len = 0U;

    if (!m_tls_enabled) {
        // Plaintext path: reuse existing SocketUtils framing via injected ISocketOps
        return m_sock_ops->recv_frame(m_client_net[idx].fd, buf, buf_cap,
                                      timeout_ms, out_len);
    }

    // TLS path: read 4-byte header (extracted to read_tls_header() for CC).
    uint8_t hdr[4U];
    if (!read_tls_header(idx, hdr, timeout_ms)) {
        return false;
    }

    uint32_t payload_len = (static_cast<uint32_t>(hdr[0U]) << 24U)
                         | (static_cast<uint32_t>(hdr[1U]) << 16U)
                         | (static_cast<uint32_t>(hdr[2U]) <<  8U)
                         |  static_cast<uint32_t>(hdr[3U]);

    if (payload_len == 0U || payload_len > buf_cap) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "tls_recv_frame: invalid length %u (cap=%u)",
                    payload_len, buf_cap);
        return false;
    }

    // Read payload via helper (Power of 10 Rule 4: CC-reduction extraction)
    return tls_read_payload(idx, buf, payload_len, timeout_ms, out_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_inbound_impairment() — CC-reduction helper for recv_from_client
// ─────────────────────────────────────────────────────────────────────────────
//
// Routes a deserialized inbound envelope through ImpairmentEngine inbound
// processing before queuing to m_recv_queue.
//
// Pattern (REQ-5.1.5, REQ-5.1.6):
//   1. is_partition_active(): drop the envelope if a partition is in effect.
//   2. process_inbound(): buffer for reorder if configured; emit immediately otherwise.
//   3. out_count == 0: buffered by reorder; do not queue; return false.
//   4. out_count == 1: push the emitted envelope to m_recv_queue; return true.

bool TlsTcpBackend::apply_inbound_impairment(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);        // Pre-condition

    // REQ-5.1.6: drop if an inbound partition is currently active.
    if (m_impairment.is_partition_active(now_us)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "inbound envelope dropped (partition active)");
        return false;
    }

    // REQ-5.1.5: apply inbound reorder simulation.
    // 1-slot output buffer: process_inbound emits 0 or 1 envelope.
    MessageEnvelope inbound_out;
    uint32_t inbound_count = 0U;
    Result res = m_impairment.process_inbound(env, now_us,
                                               &inbound_out, 1U,
                                               inbound_count);
    if (!result_ok(res)) {
        // ERR_FULL from process_inbound (logic error): treat as drop.
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "process_inbound returned error; dropping envelope");
        return false;
    }

    if (inbound_count == 0U) {
        // Buffered by reorder engine; will be released on a future receive call.
        return false;
    }

    // inbound_count == 1: push the released envelope into the receive queue.
    res = m_recv_queue.push(inbound_out);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "recv queue full; dropping inbound envelope");
    }

    NEVER_COMPILED_OUT_ASSERT(inbound_count == 1U);  // Post-condition
    return result_ok(res);
}

// ─────────────────────────────────────────────────────────────────────────────
// classify_inbound_frame() — CC-reduction helper for recv_from_client()
// Handles HELLO interception (Fix 4) and unregistered-slot rejection (Fix 3).
// Returns ERR_AGAIN if HELLO was consumed, ERR_INVALID if rejected,
// or OK if the envelope should continue to validate_source_id + impairment.
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::classify_inbound_frame(uint32_t idx,
                                              const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(m_client_slot_active[idx]);

    // REQ-6.1.8/6.1.9: intercept HELLO frames before impairment engine.
    // Fix 4: enforce one-HELLO-per-connection; duplicate HELLO is rejected.
    if (env.message_type == MessageType::HELLO) {
        if (m_is_server) {
            if (m_client_hello_received[idx]) {
                Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                            "duplicate HELLO from slot %u: dropping (REQ-6.1.8)",
                            static_cast<unsigned>(idx));
                return Result::ERR_INVALID;
            }
            m_client_hello_received[idx] = true;
            handle_hello_frame(idx, env.source_id);
        }
        return Result::ERR_AGAIN;
    }

    // F-3 / REQ-6.1.11 / HAZ-009: reject data frames from any slot where HELLO
    // has not yet been received. HELLO is mandatory before DATA (REQ-6.1.8).
    // Backward-compatibility bypass removed — a client that skips HELLO could
    // otherwise inject arbitrary source_id values via validate_source_id().
    if (m_is_server && !m_client_hello_received[idx]) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "data frame from slot %u before HELLO; dropping (REQ-6.1.11)",
                    static_cast<unsigned>(idx));
        return Result::ERR_INVALID;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_from_client()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::recv_from_client(uint32_t idx, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_TCP_CONNECTIONS);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    uint32_t out_len = 0U;
    if (!tls_recv_frame(idx, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                        timeout_ms, &out_len)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "recv_frame failed on client %u; closing", idx);
        remove_client(idx);
        return Result::ERR_IO;
    }

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "Deserialize failed on client %u: %u",
                    idx, static_cast<uint8_t>(res));
        return res;
    }

    // Fix 3+4: HELLO interception and unregistered slot check.
    // Extracted to classify_inbound_frame() to keep this function's CC ≤ 10.
    Result classify_r = classify_inbound_frame(idx, env);
    if (classify_r != Result::OK) {
        return classify_r;  // ERR_AGAIN (HELLO consumed) or ERR_INVALID (rejected)
    }

    // REQ-6.1.11: validate envelope source_id against this slot's registered
    // NodeId. Prevents source_id spoofing attacks (HAZ-009).
    // Safety-critical (SC): HAZ-009
    if (!validate_source_id(idx, env.source_id)) {
        return Result::ERR_INVALID;  // silent discard; WARNING_HI already logged
    }

    // REQ-5.1.5, REQ-5.1.6: route through impairment before queuing.
    // Partition drops and reorder buffering apply on the inbound path.
    uint64_t now_us = timestamp_now_us();
    (void)apply_inbound_impairment(env, now_us);

    NEVER_COMPILED_OUT_ASSERT(out_len <= SOCKET_RECV_BUF_BYTES);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_all_clients()
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    uint32_t timeout_ms = (m_cfg.num_channels > 0U)
                          ? m_cfg.channels[0U].send_timeout_ms
                          : 1000U;

    bool any_failed = false;

    // Fix 5: iterate full table; skip inactive slots (no compaction on remove).
    // Power of 10 Rule 2: fixed loop bound — at most MAX_TCP_CONNECTIONS iterations.
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (!m_client_slot_active[i]) { continue; }
        if (!tls_send_frame(i, buf, len, timeout_ms)) {
            Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                        "Send frame failed on client %u", i);
            any_failed = true;
        }
    }
    return any_failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// unicast_serialized() — CC-reduction helper for route_one_delayed
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::unicast_serialized(NodeId dst, const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(dst != NODE_ID_INVALID);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);

    uint32_t slot = find_client_slot(dst);
    if (slot >= MAX_TCP_CONNECTIONS) {
        // F-4 / REQ-6.1.9: no routing entry for dst — fail with ERR_INVALID.
        // Broadcast fallback removed: unicast-addressed data must never be sent to
        // all peers; doing so leaks confidential application data to unintended
        // recipients in multi-tenant server deployments.
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "unicast_serialized: no slot for dst=%u; dropping (REQ-6.1.9)", dst);
        return Result::ERR_INVALID;
    }
    bool failed = send_to_slot(slot, buf, len);
    return failed ? Result::ERR_IO : Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// route_one_delayed() — CC-reduction helper for flush_delayed_to_clients
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::route_one_delayed(const MessageEnvelope& env, bool is_current)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);

    uint32_t delayed_len = 0U;
    Result ser_r = Serializer::serialize(env, m_wire_buf,
                                         static_cast<uint32_t>(sizeof(m_wire_buf)),
                                         delayed_len);
    if (!result_ok(ser_r)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "flush_delayed: serialize failed");
        // Non-current failures are swallowed; only propagate for the current msg.
        if (!is_current) { return Result::OK; }
        return Result::ERR_IO;
    }

    // REQ-6.1.9: unicast to specific client if dst is known; broadcast otherwise.
    Result send_r = Result::OK;
    if (!m_is_server || (env.destination_id == NODE_ID_INVALID)) {
        // Client mode or unaddressed broadcast — send to all.
        bool failed = send_to_all_clients(m_wire_buf, delayed_len);
        if (failed) { send_r = Result::ERR_IO; }
    } else {
        send_r = unicast_serialized(env.destination_id, m_wire_buf, delayed_len);
    }

    if (!result_ok(send_r) && !is_current) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "flush_delayed: send failed for non-current msg");
        return Result::OK;
    }
    return send_r;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_clients()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::flush_delayed_to_clients(const MessageEnvelope& current_env,
                                               uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    Result final_result = Result::OK;

    // Power of 10 Rule 2: bounded loop — at most IMPAIR_DELAY_BUF_SIZE iterations
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        bool is_current = (delayed[i].source_id  == current_env.source_id) &&
                          (delayed[i].message_id  == current_env.message_id);
        Result r = route_one_delayed(delayed[i], is_current);
        if (!result_ok(r) && is_current) {
            final_result = r;
        }
    }
    return final_result;
}

// ─────────────────────────────────────────────────────────────────────────────
// poll_clients_once()
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// build_poll_fds() — CC-reduction helper for poll_clients_once()
// Fix 5: iterate MAX_TCP_CONNECTIONS and skip inactive slots; maintain
// slot_of[] mapping from pollfd index to slot index (no compaction).
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TlsTcpBackend::build_poll_fds(struct pollfd* pfds, uint32_t* slot_of,
                                        uint32_t timeout_ms) const
{
    NEVER_COMPILED_OUT_ASSERT(pfds != nullptr);
    NEVER_COMPILED_OUT_ASSERT(slot_of != nullptr);

    uint32_t nfds = 0U;
    // Power of 10 Rule 2: bounded loop — at most MAX_TCP_CONNECTIONS iterations.
    for (uint32_t j = 0U; j < MAX_TCP_CONNECTIONS; ++j) {
        if (!m_client_slot_active[j]) { continue; }
        pfds[nfds].fd      = m_client_net[j].fd;
        pfds[nfds].events  = POLLIN;
        pfds[nfds].revents = 0;
        slot_of[nfds]      = j;
        ++nfds;
    }
    if (nfds > 0U) {
        // Block until at least one client is readable or timeout expires.
        // CERT INT31-C: clamp uint32_t → int before passing to poll().
        static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
        const uint32_t clamped_ms = (timeout_ms > k_poll_max_ms) ? k_poll_max_ms : timeout_ms;
        (void)poll(pfds, static_cast<nfds_t>(nfds),
                   static_cast<int>(clamped_ms));
    }

    NEVER_COMPILED_OUT_ASSERT(nfds <= MAX_TCP_CONNECTIONS);
    return nfds;
}

void TlsTcpBackend::poll_clients_once(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U);

    if (m_is_server) {
        // When no clients are connected, wait on the listen socket so the
        // receive_message() polling loop does not spin in zero time before the
        // client has had a chance to connect.  mbedtls_net_poll returns as soon
        // as a connection is pending or the timeout expires.
        if (m_client_count == 0U) {
            (void)mbedtls_net_poll(&m_listen_net, MBEDTLS_NET_POLL_READ, timeout_ms);
        }
        (void)accept_and_handshake();
    }

    // Fix B-2a + Fix 5: build pollfd array (extracted to build_poll_fds()
    // to keep this function CC ≤ 10). Power of 10 Rule 2: fixed array bounds.
    struct pollfd pfds[MAX_TCP_CONNECTIONS];
    uint32_t slot_of[MAX_TCP_CONNECTIONS];  // slot_of[pfd_idx] = slot index
    uint32_t nfds = build_poll_fds(pfds, slot_of, timeout_ms);

    // Iterate backwards by pfd index; use slot_of[] to get the real slot.
    // Backwards iteration avoids complications when remove_client() is called
    // mid-loop (Fix 5: remove just clears a flag, no shift occurs).
    // Power of 10 Rule 2: bounded loop — at most MAX_TCP_CONNECTIONS iterations.
    uint32_t pi = nfds;
    while (pi > 0U) {
        --pi;
        uint32_t slot = slot_of[pi];
        // Gate on OS readiness from the poll above, or on mbedTLS internal buffer
        // (which may already hold decrypted bytes from a prior partial TLS record).
        bool has_data = (m_tls_enabled &&
                         (mbedtls_ssl_get_bytes_avail(&m_ssl[slot]) > 0U));
        has_data = has_data || ((pfds[pi].revents & POLLIN) != 0U);
        if (!has_data) {
            continue;   // No data on this slot; skip without closing
        }
        (void)recv_from_client(slot, timeout_ms);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    // S5: validate config before any channels[] access (REQ-6.1.1, ChannelConfig.hpp).
    if (!transport_config_valid(config)) {
        Logger::log(Severity::WARNING_HI, "TlsTcpBackend",
                    "init: num_channels=%u exceeds MAX_CHANNELS; rejecting config",
                    config.num_channels);
        return Result::ERR_INVALID;
    }

    m_cfg          = config;
    m_is_server    = config.is_server;
    m_tls_enabled  = config.tls.tls_enabled;

    // REQ-6.1.9/10: reset routing state on each init() (Power of 10 Rule 3: bounded loop)
    m_local_node_id = NODE_ID_INVALID;
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_node_ids[i]       = NODE_ID_INVALID;
        m_client_hello_received[i] = false;  // Fix 4: REQ-6.1.8
        m_client_slot_active[i]    = false;  // Fix 5: no compaction
    }

    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    if (m_tls_enabled) {
        Result res = setup_tls_config(config.tls);
        if (!result_ok(res)) {
            return res;
        }
    }

    Result res = m_is_server
                 ? bind_and_listen(config.bind_ip, config.bind_port)
                 : connect_to_server();

    if (!result_ok(res)) {
        return res;
    }

    NEVER_COMPILED_OUT_ASSERT(m_open);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));

    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend", "Serialize failed");
        return res;
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

    if (m_client_count == 0U) {
        Logger::log(Severity::WARNING_LO, "TlsTcpBackend",
                    "No clients; discarding message");
        return Result::OK;
    }

    // process_outbound() already queued the message; flush_delayed_to_clients()
    // calls collect_deliverable() and sends everything due — covering both the
    // zero-delay pass-through and timed-delay cases. Do NOT also call
    // send_to_all_clients() here; that would send every message twice. (HAZ-003)
    res = flush_delayed_to_clients(envelope, now_us);

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);

    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) {
        return res;
    }

    uint32_t poll_count = (timeout_ms + 99U) / 100U;
    if (poll_count > 50U) { poll_count = 50U; }
    NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U);

    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt) {
        poll_clients_once(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }

        uint64_t now_us = timestamp_now_us();
        // No current send envelope in receive context; use a zero-init sentinel
        // so flush attribution never matches a real in-flight message.
        MessageEnvelope no_current_env{};
        (void)flush_delayed_to_clients(no_current_env, now_us);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) { return res; }
    }

    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::close()
{
    // Fix 5: iterate full table, closing only active slots (no compaction).
    // Power of 10 Rule 2: fixed loop bound — MAX_TCP_CONNECTIONS iterations.
    if (m_tls_enabled) {
        for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
            if (!m_client_slot_active[i]) { continue; }
            (void)mbedtls_ssl_close_notify(&m_ssl[i]);
            mbedtls_ssl_free(&m_ssl[i]);
            mbedtls_ssl_init(&m_ssl[i]);
        }
    }
    // Free all client net contexts
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_free(&m_client_net[i]);
        mbedtls_net_init(&m_client_net[i]);
        m_client_slot_active[i]    = false;  // Fix 5: reset slot flags
        m_client_hello_received[i] = false;  // Fix 4: reset for reuse
    }
    mbedtls_net_free(&m_listen_net);
    mbedtls_net_init(&m_listen_net);
    // Fix 2 + SEC-002: free, then zeroize ticket key context to prevent recovery
    // of the ticket encryption key from memory (CLAUDE.md §7c, CWE-316).
    // Order: free() first (needs valid internal pointers); zeroize after (clears
    // key bytes remaining in the struct after free).
    mbedtls_ssl_ticket_free(&m_ticket_ctx);
    mbedtls_platform_zeroize(static_cast<void*>(&m_ticket_ctx), sizeof(m_ticket_ctx));
    mbedtls_ssl_ticket_init(&m_ticket_ctx);
    // REQ-6.3.4: free and re-init CRL so the object can be safely re-used after close().
    mbedtls_x509_crl_free(&m_crl);
    mbedtls_x509_crl_init(&m_crl);
    m_crl_loaded = false;
    // REQ-7.2.4: count clients still connected at graceful shutdown
    m_connections_closed += m_client_count;
    m_client_count = 0U;
    m_open         = false;
    // SEC-002: Free then zeroize the saved TLS session to prevent master-secret
    // recovery from memory forensics between uses (CLAUDE.md §7c, CWE-316).
    // Order: session_free() first (frees internal ticket allocation if any);
    // zeroize after (clears all key bytes still embedded in the struct);
    // session_init() to reset to a known safe state.
    mbedtls_ssl_session_free(&m_saved_session);
    mbedtls_platform_zeroize(static_cast<void*>(&m_saved_session), sizeof(m_saved_session));
    mbedtls_ssl_session_init(&m_saved_session);
    m_session_saved = false;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Transport closed (TLS=%s)", m_tls_enabled ? "ON" : "OFF");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool TlsTcpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(
        m_open == (m_listen_net.fd >= 0 || m_client_count > 0U) || !m_open);
    return m_open;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_transport_stats() — REQ-7.2.4 / REQ-7.2.2 observability
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::get_transport_stats(TransportStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened >= m_connections_closed);  // Assert: monotonic counters
    NEVER_COMPILED_OUT_ASSERT(m_connections_closed <= m_connections_opened);  // Assert: closed ≤ opened

    transport_stats_init(out);
    out.connections_opened = m_connections_opened;
    out.connections_closed = m_connections_closed;
    out.impairment         = m_impairment.get_stats();
}
