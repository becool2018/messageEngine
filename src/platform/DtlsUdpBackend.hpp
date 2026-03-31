/**
 * @file DtlsUdpBackend.hpp
 * @brief DTLS-capable UDP transport backend implementing TransportInterface.
 *
 * Provides authenticated, encrypted datagram transport using DTLS 1.2/1.3
 * (RFC 6347 / RFC 9147) over a UDP socket.  When config.tls.tls_enabled is
 * false, operates as plaintext UDP and is functionally equivalent to UdpBackend
 * (REQ-6.3.4: swappable without changing higher layers).
 *
 * When config.tls.tls_enabled is true:
 *   - Server: waits for the first client datagram, performs DTLS cookie
 *     anti-replay exchange (REQ-6.4.2), then completes the DTLS handshake.
 *   - Client: connects to the server address and performs the DTLS handshake.
 *   - All send/receive operations pass through the mbedTLS DTLS record layer.
 *   - No application-level length prefix is needed: DTLS preserves datagram
 *     boundaries (unlike TCP), so mbedtls_ssl_write/read return whole records.
 *
 * Encryption library: mbedTLS 4.0 (PSA Crypto backend).
 *   Transport: MBEDTLS_SSL_TRANSPORT_DATAGRAM (REQ-6.4.1).
 *   Retransmission timer: mbedtls_timing_delay_context (REQ-6.4.3).
 *   Cookie anti-replay: mbedtls_ssl_cookie_ctx (server only, REQ-6.4.2).
 *   MTU: DTLS_MAX_DATAGRAM_BYTES enforced via mbedtls_ssl_set_mtu (REQ-6.4.4).
 *
 * Single-peer model: each DtlsUdpBackend instance communicates with exactly
 * one remote endpoint.  Server mode blocks in init() until the first client
 * arrives and the DTLS handshake completes.  This matches UdpBackend's model.
 *
 * Power of 10 Rule 3 note:
 *   mbedTLS heap-allocates during psa_crypto_init(), certificate parsing,
 *   and the DTLS handshake.  These occur exclusively in init() — not on the
 *   send/receive critical path.  Documented as an init-phase deviation.
 *
 * Power of 10 Rule 9 note:
 *   Three mbedTLS API calls require function-pointer arguments:
 *     mbedtls_ssl_set_bio()           — BIO send/recv callbacks
 *     mbedtls_ssl_set_timer_cb()      — DTLS retransmission timer (REQ-6.4.3)
 *     mbedtls_ssl_conf_dtls_cookies() — DTLS cookie callbacks (REQ-6.4.2)
 *   All three use only library-defined symbols; no application-level function
 *   pointer declarations are introduced.  Deviation is documented at each call
 *   site in DtlsUdpBackend.cpp.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function,
 *     all return values checked, fixed stack buffers.
 *   - MISRA C++:2023: no STL, no exceptions, ≤1 pointer indirection per
 *     expression.  reinterpret_cast used only for POSIX sockaddr* patterns
 *     (Rule 5.2.4 permission cited at each use).
 *   - F-Prime style: Result return codes; Logger::log() for events.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4,
 *             REQ-6.4.5, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-7.1.1

#ifndef PLATFORM_DTLS_UDP_BACKEND_HPP
#define PLATFORM_DTLS_UDP_BACKEND_HPP

#include <cstdint>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/timing.h>

#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

class IMbedtlsOps;  // forward declaration — see platform/IMbedtlsOps.hpp
class ISocketOps;   // forward declaration — see platform/ISocketOps.hpp

// ─────────────────────────────────────────────────────────────────────────────
// DtlsUdpBackend
// ─────────────────────────────────────────────────────────────────────────────

class DtlsUdpBackend : public TransportInterface {
public:
    /// Default constructor — uses the process-wide MbedtlsOpsImpl singleton.
    /// Production code always calls this form.
    DtlsUdpBackend();

    /// Injection constructor — accepts any IMbedtlsOps implementation.
    /// Used by tests to inject a mock and exercise error paths that cannot
    /// be triggered in a loopback environment.
    /// @p ops must outlive this DtlsUdpBackend instance.
    explicit DtlsUdpBackend(IMbedtlsOps& ops);

    /// Dual injection constructor — accepts both ISocketOps and IMbedtlsOps.
    /// Allows tests to inject mocks for both socket and TLS operations,
    /// enabling fault injection on POSIX socket error paths.
    /// @p sock_ops and @p tls_ops must both outlive this DtlsUdpBackend instance.
    explicit DtlsUdpBackend(ISocketOps& sock_ops, IMbedtlsOps& tls_ops);

    ~DtlsUdpBackend() override;

    // ── TransportInterface implementation ────────────────────────────────────
    Result init(const TransportConfig& config) override;
    // Safety-critical (SC): HAZ-005, HAZ-006
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-004, HAZ-005
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void   close() override;
    bool   is_open() const override;

private:
    // ── mbedTLS contexts (fixed static allocation — Power of 10 Rule 3) ─────
    mbedtls_ssl_config           m_ssl_conf;    ///< Shared DTLS configuration
    mbedtls_x509_crt             m_cert;        ///< Own certificate chain
    mbedtls_x509_crt             m_ca_cert;     ///< CA certificate for peer verification
    mbedtls_pk_context           m_pkey;        ///< Own private key
    mbedtls_ssl_context          m_ssl;         ///< Single DTLS session (single-peer model)
    mbedtls_ssl_cookie_ctx       m_cookie_ctx;  ///< DTLS cookie context (server, REQ-6.4.2)
    mbedtls_timing_delay_context m_timer;       ///< DTLS retransmission timer (REQ-6.4.3)
    mbedtls_net_context          m_net_ctx;     ///< Wraps m_sock_fd for BIO callbacks

    // ── Injected socket and mbedTLS/POSIX operations interfaces ─────────────
    ISocketOps*     m_sock_ops;                       ///< Non-owning; never null after ctor
    IMbedtlsOps*    m_ops;                            ///< Non-owning; never null after ctor

    // ── Socket and transport state ───────────────────────────────────────────
    int             m_sock_fd;                        ///< POSIX SOCK_DGRAM file descriptor
    uint8_t         m_wire_buf[SOCKET_RECV_BUF_BYTES]; ///< Serialization / receive buffer
    TransportConfig m_cfg;                            ///< Transport configuration copy
    ImpairmentEngine m_impairment;                    ///< Impairment simulator
    RingBuffer      m_recv_queue;                     ///< Inbound message ring buffer
    bool            m_open;                           ///< True after successful init()
    bool            m_is_server;                      ///< Role derived from config
    bool            m_tls_enabled;                    ///< From config.tls.tls_enabled

    // ── Private helpers ──────────────────────────────────────────────────────

    /// Configure the shared mbedTLS ssl_conf for DTLS (DATAGRAM transport).
    /// Loads certificates, keys, sets cookie callbacks (server), sets timeouts.
    /// Called once during init() when tls_enabled is true.
    Result setup_dtls_config(const TlsConfig& tls_cfg);

    /// Server: poll for first client datagram, peek peer address, connect
    /// the socket, set up the DTLS session, and complete the handshake.
    Result server_wait_and_handshake();

    /// Client: connect socket to peer, set up DTLS session, complete handshake.
    Result client_connect_and_handshake();

    /// Run the DTLS handshake loop on m_ssl, handling HELLO_VERIFY_REQUIRED
    /// (server cookie exchange) and WANT_READ / WANT_WRITE returns.
    /// @param peer_addr  Peer sockaddr bytes (for transport-ID re-set after cookie).
    /// @param addr_len   Length of peer_addr in bytes.
    Result run_dtls_handshake(const void* peer_addr, uint32_t addr_len);

    /// Attempt to receive one datagram (DTLS or plaintext), deserialize it,
    /// and push the resulting envelope to m_recv_queue.
    /// @param timeout_ms Per-call receive timeout in milliseconds.
    /// @return true if a datagram was received and pushed successfully.
    bool recv_one_dtls_datagram(uint32_t timeout_ms);

    /// Drain impairment-delayed inbound messages into m_recv_queue.
    /// @param now_us Current wall-clock time in microseconds.
    void flush_delayed_to_queue(uint64_t now_us);
};

#endif // PLATFORM_DTLS_UDP_BACKEND_HPP
