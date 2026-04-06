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
 * @file TlsTcpBackend.hpp
 * @brief TLS-capable TCP transport backend implementing TransportInterface.
 *
 * Drop-in replacement for TcpBackend that adds optional mbedTLS encryption.
 * When config.tls.tls_enabled is false, operates in plaintext mode and
 * is functionally identical to TcpBackend (REQ-6.3.4: swappable without
 * changing higher layers).
 *
 * When config.tls.tls_enabled is true:
 *   - Loads the PEM certificate (cert_file), private key (key_file), and
 *     optional CA certificate (ca_file) during init().
 *   - Performs a TLS 1.2+ handshake on every accepted or initiated connection.
 *   - All send/receive operations pass through mbedTLS record layer.
 *   - Message framing (4-byte big-endian length prefix) is preserved.
 *
 * When config.tls.session_resumption_enabled is true (requires tls_enabled):
 *   - Client: saves the negotiated session after the first handshake and
 *     presents it on reconnect to attempt abbreviated resumption (RFC 5077).
 *   - Server: enables session ticket support via
 *     mbedtls_ssl_conf_session_tickets() so connecting clients may resume.
 *   - Session state is stored in fixed-size mbedtls_ssl_session m_saved_session
 *     (no dynamic allocation on the critical path — Power of 10 Rule 3).
 *
 * Encryption library: mbedTLS 4.0 (PSA Crypto backend).
 *   RNG: PSA Crypto internal DRBG (psa_crypto_init() called in init()).
 *   No legacy CTR-DRBG or entropy context required.
 *
 * Power of 10 Rule 3 note:
 *   mbedTLS internally heap-allocates during the TLS handshake (record buffers,
 *   certificate parsing). This occurs only during init()/accept() — not on the
 *   send/receive critical path. This is documented as an init-phase deviation.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function,
 *     all return values checked, fixed stack buffers.
 *   - MISRA C++:2023: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result return codes; Logger::log() for events.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6,
 *             REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11,
 *             REQ-6.3.4, REQ-7.1.1, REQ-5.1.5, REQ-5.1.6
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11, REQ-6.3.4, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6

#ifndef PLATFORM_TLS_TCP_BACKEND_HPP
#define PLATFORM_TLS_TCP_BACKEND_HPP

#include <cstdint>
#include <poll.h>          // struct pollfd — used in build_poll_fds() helper
#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_crl.h>    // Fix CRL: certificate revocation list support (REQ-6.3.4)
#include <mbedtls/pk.h>
#include <mbedtls/ssl_ticket.h>   // Fix 2: session ticket key context (REQ-6.3.4)

#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

class ISocketOps;   // forward declaration — see platform/ISocketOps.hpp

// ─────────────────────────────────────────────────────────────────────────────
// TlsTcpBackend
// ─────────────────────────────────────────────────────────────────────────────

class TlsTcpBackend : public TransportInterface {
public:
    /// Default constructor — uses the process-wide SocketOpsImpl singleton.
    /// Production code always calls this form.
    TlsTcpBackend();

    /// Injection constructor — accepts any ISocketOps implementation.
    /// Used by tests to inject a mock and exercise error paths that cannot
    /// be triggered in a loopback environment.
    /// @p sock_ops must outlive this TlsTcpBackend instance.
    explicit TlsTcpBackend(ISocketOps& sock_ops);

    ~TlsTcpBackend() override;

    // ── TransportInterface implementation ────────────────────────────────────
    Result init(const TransportConfig& config) override;
    // Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-004, HAZ-005 — verified to M5
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void   close() override;
    bool   is_open() const override;
    /// REQ-7.2.4 / REQ-7.2.2 — NSC observability accessor.
    void   get_transport_stats(TransportStats& out) const override;
    /// REQ-6.1.10: store our node ID; client mode sends HELLO frame to server.
    Result register_local_id(NodeId id) override;

private:
    // ── mbedTLS contexts (fixed static allocation — Power of 10 Rule 3) ─────
    mbedtls_ssl_config  m_ssl_conf;                        ///< Shared TLS config
    mbedtls_x509_crt    m_cert;                            ///< Own certificate chain
    mbedtls_x509_crt    m_ca_cert;                         ///< CA certificate for verification
    mbedtls_pk_context  m_pkey;                            ///< Own private key
    mbedtls_x509_crl    m_crl;                             ///< CRL for certificate revocation checking. REQ-6.3.4
    bool                m_crl_loaded;                      ///< True if a CRL was loaded. REQ-6.3.4
    mbedtls_net_context m_listen_net;                      ///< Server listen socket
    mbedtls_net_context m_client_net[MAX_TCP_CONNECTIONS]; ///< Per-client net contexts
    mbedtls_ssl_context m_ssl[MAX_TCP_CONNECTIONS];        ///< Per-client TLS sessions

    // ── TLS session resumption state (REQ-6.3.4) ─────────────────────────────
    /// Saved TLS session for client-side resumption (session tickets, RFC 5077).
    /// Fixed-size mbedTLS struct — no dynamic allocation (Power of 10 Rule 3).
    /// Initialized in both constructors; freed in close() and destructor.
    mbedtls_ssl_session    m_saved_session;
    /// true when m_saved_session contains a valid, resumable TLS session.
    bool                   m_session_saved;
    /// Fix 2: session ticket key context — holds ticket encryption key and lifetime.
    /// Initialized in both constructors; setup in setup_tls_config(); freed in
    /// close() and destructor. Server-side only (REQ-6.3.4).
    mbedtls_ssl_ticket_context m_ticket_ctx;

    // ── Injected socket operations interface ─────────────────────────────────
    ISocketOps*       m_sock_ops;                          ///< Non-owning; never null after ctor

    // ── Transport state ──────────────────────────────────────────────────────
    uint32_t          m_client_count;
    uint8_t           m_wire_buf[SOCKET_RECV_BUF_BYTES];
    TransportConfig   m_cfg;
    ImpairmentEngine  m_impairment;
    RingBuffer        m_recv_queue;
    bool              m_open;
    bool              m_is_server;
    bool              m_tls_enabled;              ///< From config.tls.tls_enabled
    uint32_t          m_connections_opened;       ///< REQ-7.2.4: successful connect/accept events
    uint32_t          m_connections_closed;       ///< REQ-7.2.4: disconnect events
    NodeId            m_client_node_ids[MAX_TCP_CONNECTIONS];  ///< NodeId per client slot (NODE_ID_INVALID = unknown) — REQ-6.1.9
    NodeId            m_local_node_id;            ///< Our own node identity (set by register_local_id) — REQ-6.1.10
    bool              m_client_hello_received[MAX_TCP_CONNECTIONS]; ///< True once HELLO received for this slot — REQ-6.1.8
    bool              m_client_slot_active[MAX_TCP_CONNECTIONS]; ///< Fix 5: true = slot in use; avoids ssl_context copy

    // ── Private helpers ──────────────────────────────────────────────────────

    /// Load certificates and key; configure mbedTLS shared config object.
    /// Called once during init() when tls_enabled is true.
    Result setup_tls_config(const TlsConfig& tls_cfg);

    /// Load CA cert (if verify_peer), own cert, private key; bind to ssl_conf.
    /// Extracted from setup_tls_config() to reduce its CC.
    Result load_tls_certs(const TlsConfig& tls_cfg);

    /// Load the CA cert and (if configured) the CRL; register via ca_chain.
    /// Called only when verify_peer is true and ca_file is non-empty.
    /// Extracted from load_tls_certs() to keep its CC ≤ 10. REQ-6.3.4.
    Result load_ca_and_crl(const TlsConfig& tls_cfg);

    /// Load CRL from tls_cfg.crl_file when verify_peer is true and the path is
    /// non-empty. Sets m_crl_loaded on success. Extracted from load_ca_and_crl()
    /// to keep its CC ≤ 10. REQ-6.3.4.
    Result load_crl_if_configured(const TlsConfig& tls_cfg);

    /// Execute a bounded mbedTLS handshake retry loop on @p ssl, retrying up
    /// to 32 times while the result is WANT_READ or WANT_WRITE (EINTR/EAGAIN).
    /// Extracted from tls_connect_handshake() and do_tls_server_handshake() to
    /// keep their CC ≤ 10. REQ-6.3.3.
    /// @return 0 on success, non-zero mbedTLS error code on failure.
    static int run_tls_handshake_loop(mbedtls_ssl_context* ssl);

    /// Set up the session ticket context and register callbacks on m_ssl_conf.
    /// Fix 2: extracted from setup_tls_config() to keep its CC ≤ 10 (REQ-6.3.4).
    /// Called only when session_resumption_enabled is true.
    Result setup_session_tickets(uint32_t lifetime_s);

    /// Apply Fix 1 cipher suite restriction and minimum TLS version to m_ssl_conf.
    /// Extracted from setup_tls_config() to reduce its cognitive complexity.
    void apply_cipher_policy();

    /// Configure session tickets if session_resumption_enabled; no-op otherwise.
    /// Fix 2: extracted from setup_tls_config() to reduce its CC (REQ-6.3.4).
    Result maybe_setup_session_tickets(const TlsConfig& tls_cfg);

    /// Handle an inbound HELLO frame (Fix 4) or reject data from unregistered
    /// slots (Fix 3). Extracted from recv_from_client() to keep its CC ≤ 10.
    /// @param[in]  idx  Client slot index.
    /// @param[in]  env  Deserialized inbound envelope.
    /// @return ERR_AGAIN if HELLO consumed; ERR_INVALID if rejected;
    ///         OK if envelope should proceed to validate_source_id + impairment.
    // Safety-critical (SC): HAZ-009 — enforces one-HELLO-per-slot and rejects
    // data from unregistered slots; failure allows NodeId hijack or pre-auth injection.
    Result classify_inbound_frame(uint32_t idx, const MessageEnvelope& env);

    /// Build pollfd array for active client slots and run poll().
    /// Fix 5: extracted from poll_clients_once() to keep its CC ≤ 10.
    /// Fills pfds[] and slot_of[] and returns the number of fds populated.
    uint32_t build_poll_fds(struct pollfd* pfds, uint32_t* slot_of,
                            uint32_t timeout_ms) const;

    /// Bind listen socket and start accepting (server mode).
    Result bind_and_listen(const char* ip, uint16_t port);

    /// Establish outbound connection and optionally perform TLS handshake.
    Result connect_to_server();

    /// Perform TLS setup (set_block, ssl_setup, set_hostname, BIO, handshake)
    /// for the client socket at slot 0. Called by connect_to_server() when
    /// tls_enabled is true. Extracted to reduce connect_to_server() CC.
    Result tls_connect_handshake();

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    /// Save the TLS session from slot 0 into m_saved_session after a successful
    /// client handshake.  Called only when session_resumption_enabled is true.
    /// Failure is non-fatal: logs WARNING_LO and leaves m_session_saved false.
    /// Extracted from tls_connect_handshake() to keep its CC ≤ 10 (REQ-6.3.4).
    void try_save_client_session();
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    /// Attempt to load m_saved_session into m_ssl[0] before the TLS handshake
    /// to enable abbreviated session resumption (RFC 5077, REQ-6.3.4).
    /// Called only when session_resumption_enabled is true and m_session_saved
    /// is true.  Failure is non-fatal: logs WARNING_LO and full handshake
    /// proceeds.  Extracted from tls_connect_handshake() to reduce its CC.
    void try_load_client_session();

    /// Accept one pending connection and optionally perform TLS handshake.
    /// Does nothing if no pending connection or table is full.
    Result accept_and_handshake();

    /// Perform TLS setup (set_block, ssl_setup, BIO, handshake, set_nonblock)
    /// for the server-accepted socket at @p slot.
    /// Extracted from accept_and_handshake() to keep its CC ≤ 10 (Fix B-2b).
    /// On failure, frees and re-inits m_ssl[slot] and m_client_net[slot].
    Result do_tls_server_handshake(uint32_t slot);

    /// Read the 4-byte length-prefix header from the TLS record layer,
    /// enforcing @p timeout_ms on WANT_READ retries (Fix B-2c).
    /// Extracted from tls_recv_frame() to keep its CC ≤ 10.
    /// @return true on success (exactly 4 bytes read); false on error/timeout.
    bool read_tls_header(uint32_t idx, uint8_t* hdr, uint32_t timeout_ms);

    /// Receive and deserialize one frame from client at index @p idx.
    Result recv_from_client(uint32_t idx, uint32_t timeout_ms);

    /// Serialize + send @p buf/@p len to all connected clients.
    /// @return true if any tls_send_frame() call fails, false if all succeed.
    bool send_to_all_clients(const uint8_t* buf, uint32_t len);

    /// Close and remove client at array index @p idx; compact the table.
    void remove_client(uint32_t idx);

    /// Build and send a HELLO envelope to the server (client mode, slot 0).
    /// REQ-6.1.8: client announces its NodeId immediately after connection.
    Result send_hello_frame();

    /// Record the NodeId of a connecting client in the routing table.
    /// REQ-6.1.9: server stores client NodeId for unicast routing.
    /// @param[in] idx     Slot index that sent the HELLO.
    /// @param[in] src_id  NodeId extracted from the HELLO envelope.
    void handle_hello_frame(uint32_t idx, NodeId src_id);

    /// REQ-6.1.11 / HAZ-009: verify envelope source_id matches the NodeId
    /// registered via HELLO for slot idx. Returns false + WARNING_HI on mismatch.
    /// Returns true if slot is unregistered (NODE_ID_INVALID).
    /// Safety-critical (SC): HAZ-009
    bool validate_source_id(uint32_t slot, NodeId claimed_id) const;

    /// Find the client array slot for a given destination NodeId.
    /// REQ-6.1.9: unicast routing lookup.
    /// Fix 5: iterates MAX_TCP_CONNECTIONS, skipping inactive slots.
    /// @return slot index in [0, MAX_TCP_CONNECTIONS) or MAX_TCP_CONNECTIONS if not found.
    uint32_t find_client_slot(NodeId dst) const;

    /// Send serialized data to a single client slot via tls_send_frame.
    /// Fix 5: validates m_client_slot_active[slot] before sending.
    /// @param[in] slot  Client array index.
    /// @param[in] buf   Serialized frame data.
    /// @param[in] len   Frame length in bytes.
    /// @return true if tls_send_frame() fails, false on success.
    bool send_to_slot(uint32_t slot, const uint8_t* buf, uint32_t len);

    /// Route one delayed envelope: serialize, select unicast or broadcast, send.
    /// Extracted from flush_delayed_to_clients() to keep its CC ≤ 10.
    /// @param[in] env       Envelope to send.
    /// @param[in] is_current  True when env matches the in-flight send envelope.
    /// @return OK on success; ERR_IO on send failure; ERR_INVALID if unicast
    ///         destination is not found.
    Result route_one_delayed(const MessageEnvelope& env, bool is_current);

    /// Send pre-serialized bytes to the unicast slot for @p dst, or return
    /// ERR_INVALID if no slot is found.  Extracted from route_one_delayed()
    /// to keep its CC ≤ 10.
    /// @param[in] dst     Destination NodeId; must not be NODE_ID_INVALID.
    /// @param[in] buf     Serialized frame data.
    /// @param[in] len     Frame length in bytes.
    /// @return OK on success; ERR_INVALID if no slot found; ERR_IO on send failure.
    Result unicast_serialized(NodeId dst, const uint8_t* buf, uint32_t len);

    /// Flush impairment-delayed messages out to connected clients.
    /// Failure attribution: if the send fails for the current envelope
    /// (matched by source_id + message_id), ERR_IO is returned.
    /// Failures for other envelopes are logged at WARNING_LO and skipped.
    /// @param[in] current_env The envelope just queued by send_message().
    /// @param[in] now_us      Current wall-clock time in microseconds.
    /// @return ERR_IO if send fails for current_env; OK otherwise.
    Result flush_delayed_to_clients(const MessageEnvelope& current_env,
                                    uint64_t now_us);

    /// Accept new clients (server) and drain one frame per active client.
    void poll_clients_once(uint32_t timeout_ms);

    // ── TLS-aware framing helpers ─────────────────────────────────────────────

    /// Send a 4-byte length-prefixed frame via TLS (tls_enabled) or raw TCP.
    bool tls_send_frame(uint32_t idx, const uint8_t* buf, uint32_t len,
                        uint32_t timeout_ms);

    /// Receive a 4-byte length-prefixed frame via TLS (tls_enabled) or raw TCP.
    bool tls_recv_frame(uint32_t idx, uint8_t* buf, uint32_t buf_cap,
                        uint32_t timeout_ms, uint32_t* out_len);

    // ── CC-reduction helpers ──────────────────────────────────────────────────

    /// Write exactly @p len bytes from @p buf via m_ssl[idx], looping on
    /// partial writes and MBEDTLS_ERR_SSL_WANT_WRITE continuations.
    /// Returns false (and logs WARNING_HI) if the full write cannot be completed.
    /// Loop bound: at most len iterations (each iteration sends ≥1 byte).
    /// Security fix F2: replaces single mbedtls_ssl_write() calls in
    /// tls_send_frame() to handle partial-write return values correctly.
    bool tls_write_all(uint32_t idx, const uint8_t* buf, uint32_t len);

    /// Read exactly @p payload_len bytes from m_ssl[idx] into @p buf, handling
    /// MBEDTLS_ERR_SSL_WANT_READ continuations with a timeout-gated poll.
    /// Sets *out_len on success. Extracted from tls_recv_frame() to reduce CC.
    /// Fix B-2c: timeout_ms enforced via mbedtls_net_poll on WANT_READ.
    bool tls_read_payload(uint32_t idx, uint8_t* buf,
                          uint32_t payload_len, uint32_t timeout_ms,
                          uint32_t* out_len);

    /// Route a deserialized inbound envelope through the ImpairmentEngine.
    /// Checks partition state (inbound drop) then calls process_inbound()
    /// for reorder simulation. Pushes to m_recv_queue only if delivered.
    /// NSC: bookkeeping helper; routes inbound envelope through impairment before queuing.
    /// @param[in] env    Deserialized inbound envelope.
    /// @param[in] now_us Current wall-clock time in microseconds.
    /// @return true if envelope was pushed to m_recv_queue; false if dropped or buffered.
    bool apply_inbound_impairment(const MessageEnvelope& env, uint64_t now_us);
};

#endif // PLATFORM_TLS_TCP_BACKEND_HPP
