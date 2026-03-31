/**
 * @file IMbedtlsOps.hpp
 * @brief Pure-virtual interface wrapping mbedTLS and POSIX network calls used
 *        by DtlsUdpBackend that cannot be triggered in a loopback test
 *        environment without mock injection.
 *
 * Design rationale:
 *   DtlsUdpBackend has ~14 error-handling branches that are architecturally
 *   unreachable in a loopback test: mbedTLS library failures
 *   (psa_crypto_init, ssl_setup, etc.) and POSIX failures (connect, recvfrom,
 *   inet_pton) that never occur when both endpoints are on 127.0.0.1.
 *   By routing these calls through IMbedtlsOps, tests can inject a mock that
 *   forces each error path, raising branch coverage from 72% toward 100%.
 *
 * Power of 10 Rule 9: no explicit function pointers in production code.
 *   IMbedtlsOps uses virtual dispatch (vtable), which is the documented
 *   permitted exception (CLAUDE.md §2, Rule 9 exception).
 *
 * MISRA C++:2023: virtual functions used per rules on polymorphism.
 *   All vtable-dispatched functions here are purely functional adapters;
 *   they do not manage object lifetime (no allocate/free wrappers).
 *
 * Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5
 */
// Implements: REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5

#ifndef PLATFORM_IMBEDTLS_OPS_HPP
#define PLATFORM_IMBEDTLS_OPS_HPP

#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl_cookie.h>
#include <psa/crypto.h>
#include <sys/socket.h>
#include <cstddef>

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

    /// Bind the peer's transport ID (address bytes) to @p ssl for DTLS
    /// cookie verification.  Wraps mbedtls_ssl_set_client_transport_id().
    /// @return 0 on success; non-zero on failure.
    virtual int ssl_set_client_transport_id(mbedtls_ssl_context*  ssl,
                                            const unsigned char*  info,
                                            size_t                ilen) = 0;

    // ── SSL I/O ───────────────────────────────────────────────────────────────

    /// Write @p len bytes from @p buf through the DTLS record layer.
    /// Wraps mbedtls_ssl_write().
    /// @return Number of bytes written (>0) on success; negative error code on failure.
    virtual int ssl_write(mbedtls_ssl_context*  ssl,
                          const unsigned char*  buf,
                          size_t                len) = 0;

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
};

#endif // PLATFORM_IMBEDTLS_OPS_HPP
