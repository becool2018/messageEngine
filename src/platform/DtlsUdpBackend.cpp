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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4,
 *             REQ-6.4.5, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-7.1.1, REQ-7.2.4

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
#include <psa/crypto.h>

#include <sys/socket.h>   // connect(), recvfrom(), MSG_PEEK
#include <netinet/in.h>   // sockaddr_in, sockaddr_storage
#include <poll.h>         // poll()
#include <cstring>

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
    Logger::log(Severity::WARNING_HI, tag, "%s failed: -0x%04X (%s)",
                func, static_cast<unsigned int>(-ret), err_buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

DtlsUdpBackend::DtlsUdpBackend()
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&SocketOpsImpl::instance()),
      m_ops(&MbedtlsOpsImpl::instance()),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U)
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_cookie_init(&m_cookie_ctx);
    mbedtls_net_init(&m_net_ctx);
}

DtlsUdpBackend::DtlsUdpBackend(IMbedtlsOps& ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&SocketOpsImpl::instance()),
      m_ops(&ops),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U)
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
    mbedtls_pk_init(&m_pkey);
    mbedtls_ssl_init(&m_ssl);
    mbedtls_ssl_cookie_init(&m_cookie_ctx);
    mbedtls_net_init(&m_net_ctx);
}

DtlsUdpBackend::DtlsUdpBackend(ISocketOps& sock_ops, IMbedtlsOps& tls_ops)
    : m_ssl_conf{}, m_cert{}, m_ca_cert{}, m_pkey{},
      m_ssl{}, m_cookie_ctx{}, m_timer{}, m_net_ctx{},
      m_sock_ops(&sock_ops),
      m_ops(&tls_ops),
      m_sock_fd(-1), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false), m_tls_enabled(false),
      m_connections_opened(0U), m_connections_closed(0U)
{
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);
    NEVER_COMPILED_OUT_ASSERT(DTLS_MAX_DATAGRAM_BYTES > 0U);
    mbedtls_ssl_config_init(&m_ssl_conf);
    mbedtls_x509_crt_init(&m_cert);
    mbedtls_x509_crt_init(&m_ca_cert);
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
    mbedtls_pk_free(&m_pkey);
    mbedtls_ssl_cookie_free(&m_cookie_ctx);
    // mbedtls_ssl_free() already called inside close()
    mbedtls_psa_crypto_free();
}

// ─────────────────────────────────────────────────────────────────────────────
// load_certs_and_key() — CC-reduction helper for setup_dtls_config
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::load_certs_and_key(const TlsConfig& tls_cfg)
{
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.key_file[0] != '\0');

    if (tls_cfg.verify_peer && tls_cfg.ca_file[0] != '\0') {
        int ret = m_ops->x509_crt_parse_file(&m_ca_cert, tls_cfg.ca_file);
        if (ret != 0) {
            log_mbedtls_err("DtlsUdpBackend", "x509_crt_parse_file (CA)", ret);
            return Result::ERR_IO;
        }
        mbedtls_ssl_conf_ca_chain(&m_ssl_conf, &m_ca_cert, nullptr);
    }

    int ret = m_ops->x509_crt_parse_file(&m_cert, tls_cfg.cert_file);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "x509_crt_parse_file (cert)", ret);
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
    NEVER_COMPILED_OUT_ASSERT(tls_cfg.cert_file[0] != '\0');

    // Init PSA Crypto (mbedTLS 4.0 PSA backend replaces CTR-DRBG/entropy).
    // Power of 10 Rule 3 deviation — init-phase heap allocation inside PSA.
    psa_status_t psa_ret = m_ops->crypto_init();
    if (psa_ret != PSA_SUCCESS) {
        Logger::log(Severity::FATAL, "DtlsUdpBackend",
                    "psa_crypto_init failed: %d", static_cast<int>(psa_ret));
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

    Logger::log(Severity::INFO, "DtlsUdpBackend",
                "DTLS config ready: role=%s verify_peer=%d cert=%s",
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
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "DTLS handshake did not complete in %u iterations",
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
    int wait_ms = static_cast<int>(m_cfg.connect_timeout_ms > 0U
                                   ? m_cfg.connect_timeout_ms : 30000U);
    int pr = poll(&pfd, 1, wait_ms);
    if (pr <= 0) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "server: timeout waiting for first client datagram");
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
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "server: recvfrom(MSG_PEEK) failed");
        return Result::ERR_IO;
    }

    // Connect socket to client address (single-peer model).  After connect(),
    // send()/recv() are peer-locked; mbedtls_net_send/recv BIO callbacks work.
    // MISRA C++:2023 Rule 5.2.4: POSIX connect() requires const sockaddr* cast.
    if (m_ops->net_connect(m_sock_fd,
                           reinterpret_cast<const struct sockaddr*>(&peer_addr),
                           peer_addr_len) < 0) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "server: connect() to client failed");
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
    Logger::log(Severity::INFO, "DtlsUdpBackend",
                "DTLS handshake complete (server): cipher=%s",
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

    // Build peer sockaddr; connect() on UDP sets default destination and filters
    // incoming datagrams to the peer address only.
    struct sockaddr_in peer;
    (void)memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(m_cfg.peer_port);
    // MISRA C++:2023 Rule 5.2.4: inet_pton result stored into in_addr (same type).
    if (m_ops->inet_pton_ipv4(m_cfg.peer_ip, &peer.sin_addr) != 1) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "client: inet_pton failed for %s", m_cfg.peer_ip);
        return Result::ERR_IO;
    }

    // MISRA C++:2023 Rule 5.2.4: POSIX connect() requires const sockaddr* cast.
    if (m_ops->net_connect(m_sock_fd,
                           reinterpret_cast<const struct sockaddr*>(&peer),
                           static_cast<socklen_t>(sizeof(peer))) < 0) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "client: connect() to %s:%u failed",
                    m_cfg.peer_ip, m_cfg.peer_port);
        return Result::ERR_IO;
    }

    m_net_ctx.fd = m_sock_fd;

    int ret = m_ops->ssl_setup(&m_ssl, &m_ssl_conf);
    if (ret != 0) {
        log_mbedtls_err("DtlsUdpBackend", "ssl_setup (client)", ret);
        return Result::ERR_IO;
    }

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
    Logger::log(Severity::INFO, "DtlsUdpBackend",
                "DTLS handshake complete (client): cipher=%s",
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

bool DtlsUdpBackend::validate_source(const char* src_ip, uint16_t src_port) const
{
    NEVER_COMPILED_OUT_ASSERT(src_ip != nullptr);           // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_cfg.peer_ip[0] != '\0');  // Pre-condition

    // REQ-6.2.4: DTLS path — connect() + DTLS record MAC already filter non-peer
    // datagrams at the kernel and TLS record layers.  No additional check needed.
    if (m_tls_enabled) {
        return true;
    }

    // Plaintext path: always validate IP address.
    bool ip_match = (strncmp(src_ip, m_cfg.peer_ip, sizeof(m_cfg.peer_ip)) == 0);

    // Port validation: in client mode the server responds from its configured port;
    // in server mode the client sends from an ephemeral bind port that differs from
    // m_cfg.peer_port, so port validation is skipped for the server role.
    bool port_match = m_is_server ? true : (src_port == m_cfg.peer_port);

    if (!ip_match || !port_match) {
        Logger::log(Severity::WARNING_LO, "DtlsUdpBackend",
                    "Dropped datagram from unexpected source %s:%u (expected %s:%u)",
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
        char     src_ip[48];
        uint16_t src_port = 0U;
        if (!m_sock_ops->recv_from(m_sock_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                                   timeout_ms, &out_len, src_ip, &src_port)) {
            return false;
        }

        // REQ-6.2.4: validate source before deserialization (drop spoofed datagrams)
        if (!validate_source(src_ip, src_port)) {
            return false;
        }
    }

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "DtlsUdpBackend",
                    "recv_one_dtls_datagram: deserialize failed: %u",
                    static_cast<uint8_t>(res));
        return false;
    }

    res = m_recv_queue.push(env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "recv_one_dtls_datagram: recv queue full; dropping datagram");
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_wire() — drain impairment delay buffer into receive queue
// ─────────────────────────────────────────────────────────────────────────────
//
// Despite its name, this function operates on the receive path: it collects
// impairment-delayed inbound envelopes from the delay buffer and pushes them
// into m_recv_queue for delivery to the caller of receive_message().
// The name is preserved for consistency with the header declaration.

void DtlsUdpBackend::flush_delayed_to_wire(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);
    NEVER_COMPILED_OUT_ASSERT(m_open);

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);
    // Power of 10 Rule 2: fixed loop bound (IMPAIR_DELAY_BUF_SIZE)
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)m_recv_queue.push(delayed[i]);
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
        Logger::log(Severity::FATAL, "DtlsUdpBackend", "socket_create_udp failed");
        return Result::ERR_IO;
    }

    if (!m_sock_ops->set_reuseaddr(m_sock_fd)) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend", "socket_set_reuseaddr failed");
        m_sock_ops->do_close(m_sock_fd);
        m_sock_fd = -1;
        return Result::ERR_IO;
    }

    if (!m_sock_ops->do_bind(m_sock_fd, config.bind_ip, config.bind_port)) {
        Logger::log(Severity::FATAL, "DtlsUdpBackend",
                    "socket_bind failed on port %u", config.bind_port);
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
        Logger::log(Severity::INFO, "DtlsUdpBackend",
                    "Plaintext UDP %s bound to %s:%u",
                    m_is_server ? "server" : "client",
                    config.bind_ip, config.bind_port);
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result DtlsUdpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::DTLS_UDP);
    NEVER_COMPILED_OUT_ASSERT(!m_open);

    m_cfg         = config;
    m_is_server   = config.is_server;
    m_tls_enabled = config.tls.tls_enabled;

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
        if (!m_sock_ops->send_to(m_sock_fd, buf, len,
                                 m_cfg.peer_ip, m_cfg.peer_port)) {
            Logger::log(Severity::WARNING_LO, "DtlsUdpBackend",
                        "send_message: socket_send_to failed to %s:%u",
                        m_cfg.peer_ip, m_cfg.peer_port);
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
            Logger::log(Severity::WARNING_LO, "DtlsUdpBackend",
                        "send_wire_bytes failed for delayed envelope");
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
        Logger::log(Severity::WARNING_LO, "DtlsUdpBackend",
                    "send_message: serialize failed");
        return res;
    }

    // Enforce DTLS MTU to prevent IP fragmentation (REQ-6.4.4)
    if (wire_len > DTLS_MAX_DATAGRAM_BYTES) {
        Logger::log(Severity::WARNING_HI, "DtlsUdpBackend",
                    "send_message: serialized len %u exceeds DTLS MTU %u; rejected",
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
    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us, delayed,
                                                              IMPAIR_DELAY_BUF_SIZE);

    if (flush_outbound_batch(envelope, delayed, delayed_count)) {
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

    m_open = false;
    Logger::log(Severity::INFO, "DtlsUdpBackend",
                "Transport closed (DTLS=%s)", m_tls_enabled ? "ON" : "OFF");
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
