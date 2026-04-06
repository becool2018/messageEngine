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
 *             REQ-6.1.8, REQ-6.1.9, REQ-6.1.10,
 *             REQ-6.3.4, REQ-7.1.1, REQ-5.1.5, REQ-5.1.6
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.3.4, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6

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
#include <psa/crypto.h>
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
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

TlsTcpBackend::TlsTcpBackend()
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_listen_net{}, m_client_net{}, m_ssl{},
      m_saved_session{}, m_session_saved(false),
      m_sock_ops(&SocketOpsImpl::instance()),
      m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_client_node_ids{}, m_local_node_id(NODE_ID_INVALID)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_net_init(&m_listen_net);
    mbedtls_ssl_session_init(&m_saved_session);
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_init(&m_client_net[i]);
        mbedtls_ssl_init(&m_ssl[i]);
    }
}

TlsTcpBackend::TlsTcpBackend(ISocketOps& sock_ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_listen_net{}, m_client_net{}, m_ssl{},
      m_saved_session{}, m_session_saved(false),
      m_sock_ops(&sock_ops),
      m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_client_node_ids{}, m_local_node_id(NODE_ID_INVALID)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_net_init(&m_listen_net);
    mbedtls_ssl_session_init(&m_saved_session);
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_init(&m_client_net[i]);
        mbedtls_ssl_init(&m_ssl[i]);
    }
}

TlsTcpBackend::~TlsTcpBackend()
{
    TlsTcpBackend::close();
    mbedtls_ssl_session_free(&m_saved_session);
    mbedtls_ssl_config_free(&m_ssl_conf);
    mbedtls_x509_crt_free(&m_cert);
    mbedtls_x509_crt_free(&m_ca_cert);
    mbedtls_pk_free(&m_pkey);
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

/// Load CA cert (if verify_peer), own cert, private key; bind to ssl_conf.
/// Extracted from setup_tls_config() to reduce its CC.
Result TlsTcpBackend::load_tls_certs(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.key_file[0] != '\0');

    if (tls_cfg.verify_peer && tls_cfg.ca_file[0] != '\0') {
        int ret = mbedtls_x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (CA)", ret);
            return Result::ERR_IO;
        }
        mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert, nullptr);
    }

    int ret = mbedtls_x509_crt_parse_file(&m_cert, tls_cfg.cert_file);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "x509_crt_parse_file (cert)", ret);
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

Result TlsTcpBackend::setup_tls_config(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.tls_enabled);
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

    Result res = load_tls_certs(tls_cfg);
    if (!result_ok(res)) { return res; }

    // Enable session tickets when session_resumption_enabled is true (REQ-6.3.4).
    // Server: advertises ticket support so resuming clients can skip the full
    //         handshake (RFC 5077).  Client: enables client-side ticket processing.
    // Guard: MBEDTLS_SSL_SESSION_TICKETS confirmed present in mbedTLS 4.0 install.
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    if (tls_cfg.session_resumption_enabled) {
        mbedtls_ssl_conf_session_tickets(&m_ssl_conf,
                                         MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "Session ticket resumption enabled (lifetime=%u s)",
                    tls_cfg.session_ticket_lifetime_s);
    }
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    Logger::log(Severity::INFO, "TlsTcpBackend",
                "TLS config ready: role=%s verify_peer=%d cert=%s",
                (tls_cfg.role == TlsRole::SERVER) ? "SERVER" : "CLIENT",
                static_cast<int>(tls_cfg.verify_peer),
                tls_cfg.cert_file);

    NEVER_COMPILED_OUT_ASSERT(psa_ret == PSA_SUCCESS);
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

    // TLS handshake (Power of 10 Rule 3 deviation: init-phase heap alloc)
    ret = mbedtls_ssl_handshake(&m_ssl[0U]);
    if (ret != 0) {
        log_mbedtls_err("TlsTcpBackend", "ssl_handshake (client)", ret);
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

    m_client_count = 1U;
    m_open         = true;
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

Result TlsTcpBackend::accept_and_handshake()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);
    NEVER_COMPILED_OUT_ASSERT(m_listen_net.fd >= 0);

    if (m_client_count >= MAX_TCP_CONNECTIONS) {
        return Result::OK;  // table full; no room to accept
    }

    uint32_t slot = m_client_count;
    int ret = mbedtls_net_accept(&m_listen_net, &m_client_net[slot],
                                 nullptr, 0U, nullptr);
    if (ret != 0) {
        // MBEDTLS_ERR_SSL_WANT_READ means no pending connection — not an error
        return Result::OK;
    }

    if (m_tls_enabled) {
        // Set client socket to blocking mode before TLS handshake.
        // mbedtls_net_accept() on a non-blocking listen socket returns a
        // client socket that inherits O_NONBLOCK; the TLS handshake requires
        // blocking I/O to complete its multi-round read/write sequence.
        ret = mbedtls_net_set_block(&m_client_net[slot]);
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

        // TLS handshake (init-phase — Power of 10 Rule 3 deviation documented)
        ret = mbedtls_ssl_handshake(&m_ssl[slot]);
        if (ret != 0) {
            log_mbedtls_err("TlsTcpBackend", "ssl_handshake (accept)", ret);
            mbedtls_ssl_free(&m_ssl[slot]);
            mbedtls_ssl_init(&m_ssl[slot]);
            mbedtls_net_free(&m_client_net[slot]);
            mbedtls_net_init(&m_client_net[slot]);
            return Result::ERR_IO;
        }
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "TLS handshake complete (server slot %u): cipher=%s",
                    slot, mbedtls_ssl_get_ciphersuite(&m_ssl[slot]));
    }

    ++m_client_count;
    ++m_connections_opened;  // REQ-7.2.4: successful server accept
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "Accepted client %u (TLS=%s), total=%u",
                slot, m_tls_enabled ? "ON" : "OFF", m_client_count);

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_client() — close and compact the client table
// ─────────────────────────────────────────────────────────────────────────────

void TlsTcpBackend::remove_client(uint32_t idx)
{
    NEVER_COMPILED_OUT_ASSERT(idx < m_client_count);
    NEVER_COMPILED_OUT_ASSERT(m_client_count > 0U);

    if (m_tls_enabled) {
        (void)mbedtls_ssl_close_notify(&m_ssl[idx]);
        mbedtls_ssl_free(&m_ssl[idx]);
        mbedtls_ssl_init(&m_ssl[idx]);
    }
    mbedtls_net_free(&m_client_net[idx]);
    mbedtls_net_init(&m_client_net[idx]);

    // Compact: shift remaining slots left
    for (uint32_t j = idx; j < m_client_count - 1U; ++j) {
        m_client_net[j]      = m_client_net[j + 1U];
        m_ssl[j]             = m_ssl[j + 1U];
        m_client_node_ids[j] = m_client_node_ids[j + 1U];  // REQ-6.1.9: compact routing table
        // Security fix F3: after struct-copying ssl[j+1] into ssl[j], the SSL
        // context in slot j has an internal BIO pointer that still refers to
        // &m_client_net[j+1].  Re-associate the BIO to &m_client_net[j] before
        // zeroing slot j+1; otherwise all TLS reads/writes on slot j would use
        // fd=-1 (the zeroed context at j+1) after mbedtls_net_init clears it.
        if (m_tls_enabled) {
            mbedtls_ssl_set_bio(&m_ssl[j], &m_client_net[j],
                                mbedtls_net_send, mbedtls_net_recv, nullptr);
        }
        // Re-initialise the slots we just moved away from
        mbedtls_net_init(&m_client_net[j + 1U]);
        mbedtls_ssl_init(&m_ssl[j + 1U]);
        m_client_node_ids[j + 1U] = NODE_ID_INVALID;
    }
    --m_client_count;
    ++m_connections_closed;  // REQ-7.2.4: client connection removed
    NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);
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
    bool failed = tls_send_frame(0U, m_wire_buf, wire_len, timeout_ms);
    if (failed) {
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
    NEVER_COMPILED_OUT_ASSERT(idx < m_client_count);
    NEVER_COMPILED_OUT_ASSERT(src_id != NODE_ID_INVALID);

    m_client_node_ids[idx] = src_id;
    Logger::log(Severity::INFO, "TlsTcpBackend",
                "HELLO from client slot %u node_id=%u", idx, src_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// find_client_slot() — REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TlsTcpBackend::find_client_slot(NodeId dst) const
{
    NEVER_COMPILED_OUT_ASSERT(dst != NODE_ID_INVALID);
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);

    // Power of 10 Rule 2: bounded loop — at most MAX_TCP_CONNECTIONS iterations
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (m_client_node_ids[i] == dst) {
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
    NEVER_COMPILED_OUT_ASSERT(slot < m_client_count);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);

    uint32_t timeout_ms = (m_cfg.num_channels > 0U)
                          ? m_cfg.channels[0U].send_timeout_ms
                          : 1000U;
    bool failed = tls_send_frame(slot, buf, len, timeout_ms);
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
                                      uint32_t payload_len, uint32_t* out_len)
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
            continue;
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

    // TLS path: read 4-byte header first.
    // Security fix F2: loop to handle partial header reads from mbedtls_ssl_read().
    uint8_t hdr[4U];
    uint32_t hdr_received = 0U;
    // Power of 10 Rule 2: bounded loop — at most 4 iterations for 4-byte header
    for (uint32_t iter = 0U; iter < 4U && hdr_received < 4U; ++iter) {
        int ret = mbedtls_ssl_read(&m_ssl[idx], hdr + hdr_received,
                                   static_cast<size_t>(4U - hdr_received));
        if (ret > 0) {
            hdr_received += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            continue;
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
    return tls_read_payload(idx, buf, payload_len, out_len);
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
// recv_from_client()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::recv_from_client(uint32_t idx, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(idx < m_client_count);
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

    // REQ-6.1.8/6.1.9: intercept HELLO frames before impairment engine.
    // Server records the client's NodeId for unicast routing; HELLO is not
    // delivered to upper layers (return ERR_AGAIN = "not an error; keep going").
    if (env.message_type == MessageType::HELLO) {
        if (m_is_server) {
            handle_hello_frame(idx, env.source_id);
        }
        return Result::ERR_AGAIN;
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

    // Power of 10 Rule 2: fixed loop bound
    for (uint32_t i = 0U; i < m_client_count; ++i) {
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
        // REQ-6.1.9: no routing entry for dst (client has not yet sent HELLO).
        // Fall back to broadcast so existing code paths are not disrupted before
        // HELLO registration completes; log at INFO for visibility.
        Logger::log(Severity::INFO, "TlsTcpBackend",
                    "unicast_serialized: no slot for dst=%u — broadcast fallback", dst);
        bool failed = send_to_all_clients(buf, len);
        return failed ? Result::ERR_IO : Result::OK;
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

    // Power of 10 Rule 2: fixed loop bound
    // Iterate backwards to avoid index shift from remove_client() inside the loop
    uint32_t i = m_client_count;
    while (i > 0U) {
        --i;
        (void)recv_from_client(i, timeout_ms);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result TlsTcpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    m_cfg          = config;
    m_is_server    = config.is_server;
    m_tls_enabled  = config.tls.tls_enabled;

    // REQ-6.1.9/10: reset routing state on each init() (Power of 10 Rule 3: bounded loop)
    m_local_node_id = NODE_ID_INVALID;
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_node_ids[i] = NODE_ID_INVALID;
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
    if (m_tls_enabled) {
        for (uint32_t i = 0U; i < m_client_count; ++i) {
            (void)mbedtls_ssl_close_notify(&m_ssl[i]);
            mbedtls_ssl_free(&m_ssl[i]);
            mbedtls_ssl_init(&m_ssl[i]);
        }
    }
    // Free all client net contexts
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        mbedtls_net_free(&m_client_net[i]);
        mbedtls_net_init(&m_client_net[i]);
    }
    mbedtls_net_free(&m_listen_net);
    mbedtls_net_init(&m_listen_net);
    // REQ-7.2.4: count clients still connected at graceful shutdown
    m_connections_closed += m_client_count;
    m_client_count = 0U;
    m_open         = false;
    // Reset session-saved flag on close so a stale session is not accidentally
    // resumed after re-init() — the session is re-saved on the next handshake.
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
