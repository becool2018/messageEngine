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
 * @file IMbedtlsOps.hpp
 * @brief Pure-virtual interface wrapping mbedTLS and POSIX network calls used
 *        by DtlsUdpBackend and TlsTcpBackend that cannot be triggered in a
 *        loopback test environment without mock injection.
 *
 * Design rationale:
 *   DtlsUdpBackend and TlsTcpBackend have ~120+ error-handling branches that
 *   are architecturally unreachable in a loopback test: mbedTLS library
 *   failures (psa_crypto_init, ssl_setup, etc.) and net I/O failures
 *   (net_bind, net_connect, net_poll) that never occur on 127.0.0.1.
 *   By routing these calls through IMbedtlsOps, tests can inject a mock that
 *   forces each error path — M5 fault injection for Class B coverage
 *   (VVP-001 §4.3 e-i).
 *
 * Power of 10 Rule 9: no explicit function pointers in production code.
 *   IMbedtlsOps uses virtual dispatch (vtable), which is the documented
 *   permitted exception (CLAUDE.md §2, Rule 9 exception).
 *
 * MISRA C++:2023: virtual functions used per rules on polymorphism.
 *   All vtable-dispatched functions here are purely functional adapters;
 *   they do not manage object lifetime (no allocate/free wrappers).
 *
 * Implements: REQ-6.1.1, REQ-6.1.2, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2,
 *             REQ-6.4.3, REQ-6.4.4, REQ-6.4.5
 */
// Implements: REQ-6.1.1, REQ-6.1.2, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5

#ifndef PLATFORM_IMBEDTLS_OPS_HPP
#define PLATFORM_IMBEDTLS_OPS_HPP

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl_cookie.h>
#include <psa/crypto.h>
#include <sys/socket.h>
#include <cstddef>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// IMbedtlsOps — abstract interface for injectable mbedTLS / POSIX calls
// ─────────────────────────────────────────────────────────────────────────────

class IMbedtlsOps {
public:
    virtual ~IMbedtlsOps() {}

    // ── PSA Crypto backend ────────────────────────────────────────────────────

    /// Initialise the PSA Crypto backend (mbedTLS 4.0 PSA shim).
    /// @return PSA_SUCCESS on success; non-zero on failure.
    virtual psa_status_t crypto_init() = 0;

    // ── SSL configuration ─────────────────────────────────────────────────────

    /// Set ssl_config defaults for the given endpoint, transport, and preset.
    /// Wraps mbedtls_ssl_config_defaults().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_config_defaults(mbedtls_ssl_config* conf,
                                    int                 endpoint,
                                    int                 transport,
                                    int                 preset) = 0;

    /// Parse a DER or PEM certificate file and append it to @p chain.
    /// Wraps mbedtls_x509_crt_parse_file().
    /// @return 0 on success; non-zero on failure.
    virtual int x509_crt_parse_file(mbedtls_x509_crt* chain,
                                    const char*       path) = 0;

    /// Parse a private key file and store it in @p ctx.
    /// Wraps mbedtls_pk_parse_keyfile().
    /// @return 0 on success; non-zero on failure.
    virtual int pk_parse_keyfile(mbedtls_pk_context* ctx,
                                 const char*         path,
                                 const char*         pwd) = 0;

    /// Bind @p own_cert and @p pk_key to @p conf as the own certificate.
    /// Wraps mbedtls_ssl_conf_own_cert().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_conf_own_cert(mbedtls_ssl_config*  conf,
                                  mbedtls_x509_crt*    own_cert,
                                  mbedtls_pk_context*  pk_key) = 0;

    /// Initialise the DTLS cookie context (server-side anti-replay).
    /// Wraps mbedtls_ssl_cookie_setup().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_cookie_setup(mbedtls_ssl_cookie_ctx* ctx) = 0;

    // ── SSL session ───────────────────────────────────────────────────────────

    /// Set up @p ssl using the configuration in @p conf.
    /// Wraps mbedtls_ssl_setup().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_setup(mbedtls_ssl_context*  ssl,
                          mbedtls_ssl_config*   conf) = 0;

    /// Bind the expected server hostname for SNI and certificate CN/SAN
    /// verification. Wraps mbedtls_ssl_set_hostname(). Pass nullptr to
    /// explicitly opt out (verify_peer == false or peer_hostname empty).
    /// Safety-critical (SC): HAZ-008
    virtual int ssl_set_hostname(mbedtls_ssl_context* ssl,
                                 const char*          hostname) = 0;

    /// Bind the peer's transport ID (address bytes) to @p ssl for DTLS
    /// cookie verification.  Wraps mbedtls_ssl_set_client_transport_id().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_set_client_transport_id(mbedtls_ssl_context*  ssl,
                                            const unsigned char*  info,
                                            size_t                ilen) = 0;

    // ── SSL I/O ───────────────────────────────────────────────────────────────

    /// Execute one step of the DTLS handshake state machine.
    /// Wraps mbedtls_ssl_handshake().
    /// @return 0 on success; MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_SSL_WANT_WRITE,
    ///         MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED, or a fatal error code.
    // Safety-critical (SC): HAZ-004, HAZ-005, HAZ-006 — verified to M5
    virtual int ssl_handshake(mbedtls_ssl_context* ssl) = 0;

    /// Write @p len bytes from @p buf through the DTLS record layer.
    /// Wraps mbedtls_ssl_write().
    /// @return Number of bytes written (>0) on success; negative error code on failure.
    // Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
    virtual int ssl_write(mbedtls_ssl_context*  ssl,
                          const unsigned char*  buf,
                          size_t                len) = 0;

    /// Read up to @p len bytes of application data from the DTLS record layer.
    /// Wraps mbedtls_ssl_read().
    /// @return Bytes read (>0) on success; 0 on peer close;
    ///         MBEDTLS_ERR_SSL_WANT_READ, MBEDTLS_ERR_SSL_TIMEOUT, or negative on error.
    // Safety-critical (SC): HAZ-004, HAZ-005 — verified to M5
    virtual int ssl_read(mbedtls_ssl_context* ssl,
                         unsigned char*       buf,
                         size_t               len) = 0;

    // ── POSIX network ─────────────────────────────────────────────────────────

    /// Peek one byte from @p sockfd to discover the peer address; does not
    /// consume the datagram (MSG_PEEK semantics).
    /// Wraps recvfrom(..., MSG_PEEK, ...).
    /// @return Bytes peeked (≥0) on success; -1 on failure.
    virtual ssize_t recvfrom_peek(int             sockfd,
                                  void*           buf,
                                  size_t          len,
                                  struct sockaddr* src_addr,
                                  socklen_t*      addrlen) = 0;

    /// Connect a UDP socket to @p addr (sets default send destination and
    /// filters incoming datagrams to the peer address).
    /// Wraps POSIX connect().
    /// @return 0 on success; -1 on failure.
    virtual int net_connect(int                    sockfd,
                            const struct sockaddr* addr,
                            socklen_t              addrlen) = 0;

    /// Parse an IPv4 address string into a binary in_addr structure.
    /// Wraps inet_pton(AF_INET, ...).
    /// @return 1 on success; 0 if src is not valid; -1 on error.
    virtual int inet_pton_ipv4(const char* src, void* dst) = 0;

    // ── TCP network (TlsTcpBackend) ───────────────────────────────────────────

    /// Connect ctx to host:port over TCP.
    /// Wraps mbedtls_net_connect(..., MBEDTLS_NET_PROTO_TCP).
    /// @return 0 on success; non-zero on failure.
    virtual int net_tcp_connect(mbedtls_net_context* ctx,
                                const char*          host,
                                const char*          port) = 0;

    /// Bind ctx to ip:port and listen for TCP connections.
    /// Wraps mbedtls_net_bind(..., MBEDTLS_NET_PROTO_TCP).
    /// @return 0 on success; non-zero on failure.
    virtual int net_tcp_bind(mbedtls_net_context* ctx,
                             const char*          ip,
                             const char*          port) = 0;

    /// Accept one pending TCP connection from listen_ctx into client_ctx.
    /// Wraps mbedtls_net_accept(listen_ctx, client_ctx, nullptr, 0, nullptr).
    /// @return 0 on success; non-zero on failure.
    virtual int net_tcp_accept(mbedtls_net_context* listen_ctx,
                               mbedtls_net_context* client_ctx) = 0;

    /// Set ctx to blocking I/O mode. Wraps mbedtls_net_set_block().
    /// @return 0 on success; non-zero on failure.
    virtual int net_set_block(mbedtls_net_context* ctx) = 0;

    /// Set ctx to non-blocking I/O mode. Wraps mbedtls_net_set_nonblock().
    /// @return 0 on success; non-zero on failure.
    virtual int net_set_nonblock(mbedtls_net_context* ctx) = 0;

    /// Poll ctx for readability (rw=MBEDTLS_NET_POLL_READ) with timeout_ms.
    /// Wraps mbedtls_net_poll().
    /// @return >0 if ready; 0 on timeout; negative on error.
    virtual int net_poll(mbedtls_net_context* ctx,
                         uint32_t             rw,
                         uint32_t             timeout_ms) = 0;

    // ── TLS session management ────────────────────────────────────────────────

    /// Export the current TLS session from @p ssl into @p dst for later resumption.
    /// Wraps mbedtls_ssl_get_session().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_get_session(const mbedtls_ssl_context* ssl,
                                mbedtls_ssl_session*       dst) = 0;

    /// Load a previously-saved TLS session from @p session into @p ssl.
    /// Wraps mbedtls_ssl_set_session().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_set_session(mbedtls_ssl_context*       ssl,
                                const mbedtls_ssl_session* session) = 0;

    /// Initialise the session-ticket context with the given @p lifetime_s.
    /// Wraps mbedtls_ssl_ticket_setup() with AES-256-GCM (mbedTLS 4.0 PSA API)
    /// or MBEDTLS_CIPHER_AES_256_GCM (mbedTLS 2.x/3.x legacy API).
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_ticket_setup(mbedtls_ssl_ticket_context* ctx,
                                 uint32_t                    lifetime_s) = 0;
};

#endif // PLATFORM_IMBEDTLS_OPS_HPP
