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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.10,
 *             REQ-6.2.4, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3,
 *             REQ-6.4.4, REQ-6.4.5, REQ-7.1.1
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.10, REQ-6.2.4, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-7.1.1, REQ-7.2.4

#ifndef PLATFORM_DTLS_UDP_BACKEND_HPP
#define PLATFORM_DTLS_UDP_BACKEND_HPP

#include <cstdint>
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_crl.h>
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
    // NSC: DTLS/UDP has no per-client registration; no-op per REQ-6.1.10.
    Result register_local_id(NodeId id) override;
    // Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-004, HAZ-005 — verified to M5
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void   close() override;
    bool   is_open() const override;
    /// REQ-7.2.4 / REQ-7.2.2 — NSC observability accessor.
    void   get_transport_stats(TransportStats& out) const override;

private:
    // ── mbedTLS contexts (fixed static allocation — Power of 10 Rule 3) ─────
    mbedtls_ssl_config           m_ssl_conf;    ///< Shared DTLS configuration
    mbedtls_x509_crt             m_cert;        ///< Own certificate chain
    mbedtls_x509_crt             m_ca_cert;     ///< CA certificate for peer verification
    mbedtls_x509_crl             m_crl;         ///< CRL for certificate revocation checking. REQ-6.3.4
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
    bool            m_crl_loaded;                     ///< True if a CRL was loaded in load_certs_and_key(). REQ-6.3.4
    NodeId          m_peer_node_id;                   ///< Peer NodeId learned from HELLO (NODE_ID_INVALID until received). REQ-6.2.4
    bool            m_peer_hello_received;            ///< True once HELLO received from peer. REQ-6.1.8
    uint32_t        m_connections_opened;             ///< REQ-7.2.4: successful handshake/bind events
    uint32_t        m_connections_closed;             ///< REQ-7.2.4: close events

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

    /// Validate that the source address of a received plaintext datagram matches
    /// the configured peer (REQ-6.2.4).  On the DTLS path, returns true immediately
    /// (connect() + DTLS record MAC already filter non-peer traffic).
    /// Logs WARNING_LO and returns false on plaintext mismatch.
    /// @param[in] src_ip    Source IP string returned by recv_from().
    /// @param[in] src_port  Source port returned by recv_from().
    /// @return true if source is acceptable; false otherwise.
    bool validate_source(const char* src_ip, uint16_t src_port) const;

    /// Attempt to receive one datagram (DTLS or plaintext), deserialize it,
    /// and push the resulting envelope to m_recv_queue.
    /// @param timeout_ms Per-call receive timeout in milliseconds.
    /// @return true if a datagram was received and pushed successfully.
    bool recv_one_dtls_datagram(uint32_t timeout_ms);

    /// Send impairment-delayed outbound messages to the wire via send_one_envelope().
    /// @param now_us Current wall-clock time in microseconds.
    void flush_delayed_to_wire(uint64_t now_us);

    /// Apply inbound impairment (partition drop / reorder) to a deserialized envelope
    /// and push the result to m_recv_queue if deliverable.
    /// Extracted from recv_one_dtls_datagram() to keep its CC ≤ 10.
    /// @param[in] env  Deserialized inbound envelope.
    /// @return true if a message was pushed to m_recv_queue; false otherwise.
    bool apply_inbound_impairment(const MessageEnvelope& env);

    /// Deserialize @p out_len bytes from m_wire_buf, run HELLO/source_id
    /// validation, and push deliverable envelopes via apply_inbound_impairment().
    /// Extracted from recv_one_dtls_datagram() to keep its CC ≤ 10.
    /// @param out_len Number of bytes in m_wire_buf to deserialize.
    /// @return true if a message was pushed to m_recv_queue.
    // Safety-critical (SC): HAZ-005 — on the inbound wire-to-envelope path; an error
    // here could deliver a malformed or corrupt envelope to the DeliveryEngine.
    bool deserialize_and_dispatch(uint32_t out_len);

    /// REQ-6.2.4 / REQ-6.1.8: process an inbound HELLO or validate source_id on
    /// data frames using the registered peer NodeId.
    /// Extracted from recv_one_dtls_datagram() to keep its CC ≤ 10.
    /// @param[in]  env        Deserialized inbound envelope.
    /// @param[out] consumed   Set to true if the envelope was consumed (HELLO) and
    ///                        must not be passed to the impairment engine.
    /// @return false if the envelope must be dropped (spoofing or duplicate HELLO);
    ///         true if the envelope is allowed through (or was consumed as HELLO).
    // Safety-critical (SC): HAZ-009 — enforces peer NodeId binding; failure allows
    // source_id spoofing to corrupt ACK/retry state in the DeliveryEngine.
    bool process_hello_or_validate(const MessageEnvelope& env, bool& consumed);

    // ── CC-reduction helpers (extracted to keep each caller CC ≤ 10) ─────────

    /// Load CA cert (if verify_peer), own cert, private key; bind to ssl_conf.
    /// Extracted from setup_dtls_config to reduce its CC.
    Result load_certs_and_key(const TlsConfig& tls_cfg);

    /// Load CRL from tls_cfg.crl_file (if non-empty) and bind to ssl_conf.
    /// Called from load_certs_and_key() after the CA cert is loaded.
    /// Extracted to keep load_certs_and_key() CC ≤ 10. REQ-6.3.4
    Result load_crl_if_configured(const TlsConfig& tls_cfg);

    /// Configure DTLS cookie anti-replay context (server role only, REQ-6.4.2).
    /// Extracted from setup_dtls_config to reduce its CC.
    Result setup_cookie_if_server(const TlsConfig& tls_cfg);

    /// Perform a single DTLS ssl_read; handle WANT_READ/TIMEOUT non-errors.
    /// Extracted from recv_one_dtls_datagram to reduce its CC.
    /// @param out_len Set to bytes received on success.
    /// @return true if a record was read successfully.
    bool try_tls_recv(uint32_t* out_len);

    /// Create UDP socket, set SO_REUSEADDR, and bind to configured address.
    /// Extracted from init() to reduce its CC.
    Result create_and_bind_udp_socket(const TransportConfig& config);

    /// Run DTLS handshake (if TLS enabled) or set m_open for plaintext UDP.
    /// Extracted from init() to reduce its CC.
    Result run_tls_handshake_phase(const TransportConfig& config);

    /// Send @p len bytes from @p buf via TLS or plaintext UDP.
    /// Extracted from send_message() to reduce its CC.
    Result send_wire_bytes(const uint8_t* buf, uint32_t len);

    /// Serialize and send each envelope in @p batch[0..count-1].
    /// Tracks whether the current envelope (matched by source_id + message_id) failed.
    /// Non-current failures: log WARNING_LO and continue.
    /// @return true if the current envelope's send failed; false otherwise.
    bool flush_outbound_batch(const MessageEnvelope& envelope,
                              const MessageEnvelope* batch,
                              uint32_t count);

    /// Serialize and send a single envelope from the deliverable batch.
    /// @param[in] env        Envelope to serialize and send.
    /// @param[in] is_current True if @p env is the envelope from the current send_message call.
    /// @return true if the send failed and @p is_current is true; false otherwise.
    bool send_one_envelope(const MessageEnvelope& env, bool is_current);
};

#endif // PLATFORM_DTLS_UDP_BACKEND_HPP
