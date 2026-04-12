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
 *   - Client: saves the negotiated session into a caller-injected TlsSessionStore
 *     after the first handshake and presents it on reconnect to attempt
 *     abbreviated resumption (RFC 5077).  Call set_session_store() before init().
 *   - Server: enables session ticket support via
 *     mbedtls_ssl_conf_session_tickets() so connecting clients may resume.
 *   - Session state is stored in caller-owned TlsSessionStore (no dynamic
 *     allocation on the critical path — Power of 10 Rule 3).
 *   - Caller must call store.zeroize() when session material is no longer needed
 *     (SECURITY_ASSUMPTIONS.md §13, CLAUDE.md §7c).
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
 * THREAD-SAFETY (DEF-021-1 fixed — SECURITY_ASSUMPTIONS.md §14):
 *   close() is safe to call concurrently from multiple threads.  m_open is
 *   std::atomic<bool>; close() uses compare_exchange_strong to guarantee
 *   exactly one caller executes teardown even under data races (CWE-416 fixed).
 *   All other mutating methods (init, send_message, receive_message,
 *   register_local_id) are NOT thread-safe with respect to each other and
 *   require external serialization by the caller.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function,
 *     all return values checked, fixed stack buffers.
 *   - MISRA C++:2023: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result return codes; Logger::log() for events.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.12,
 *             REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6,
 *             REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11,
 *             REQ-6.3.4, REQ-6.3.6, REQ-6.3.7, REQ-6.3.8, REQ-6.3.9,
 *             REQ-7.1.1, REQ-5.1.5, REQ-5.1.6
 */
// Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11, REQ-6.1.12, REQ-6.3.4, REQ-6.3.6, REQ-6.3.7, REQ-6.3.8, REQ-6.3.9, REQ-6.3.10, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6

#ifndef PLATFORM_TLS_TCP_BACKEND_HPP
#define PLATFORM_TLS_TCP_BACKEND_HPP

#include <atomic>          // std::atomic<bool> for m_open (DEF-021-1 / CWE-416 fix)
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

class ISocketOps;      // forward declaration — see platform/ISocketOps.hpp
class IMbedtlsOps;     // forward declaration — see platform/IMbedtlsOps.hpp
struct TlsSessionStore; // forward declaration — see platform/TlsSessionStore.hpp

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

    /// Dual-injection constructor — accepts both ISocketOps and IMbedtlsOps.
    /// Used by M5 fault-injection tests to trigger mbedTLS dependency-failure
    /// branches (VVP-001 §4.3 e-i).
    /// Both @p sock_ops and @p tls_ops must outlive this TlsTcpBackend instance.
    TlsTcpBackend(ISocketOps& sock_ops, IMbedtlsOps& tls_ops);

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

    // REQ-3.3.6: pop one NodeId from the HELLO reconnect queue.
    // Returns NODE_ID_INVALID when the queue is empty.
    // Called by DeliveryEngine::drain_hello_reconnects() to reset stale ordering state.
    NodeId pop_hello_peer() override;

    /// Inject an external TlsSessionStore for session resumption across
    /// close() → init() cycles (REQ-6.3.4).  Must be called before init().
    /// The store is not owned by this backend; the caller controls its lifetime
    /// and must call store.zeroize() when session material is no longer needed.
    ///
    /// Preconditions: backend must not be open (m_open == false).
    /// The pointer must either be null or equal to the previously-injected pointer
    /// (prevents accidental re-injection of a different store after session save).
    ///
    /// NSC: lifecycle configuration only; no runtime message-delivery policy.
    void set_session_store(TlsSessionStore* store);

    /// Visible for unit testing — logs the TLS 1.2 forward-secrecy advisory.
    /// Production code calls this through try_save_client_session() which
    /// passes mbedtls_ssl_get_version(); exposed here so all three branches
    /// (nullptr / "TLSv1.2" / other) can be exercised without a live TLS
    /// connection (Class B branch coverage, REQ-6.3.4).
    /// NSC: logging helper only; no message-delivery policy.
    static bool log_fs_warning_if_tls12(const char* ver);

    /// Visible for unit testing — REQ-6.3.8 forward secrecy rejection logic.
    /// Returns ERR_IO when all three conditions hold: feature_enabled=true,
    /// had_session=true, and ver=="TLSv1.2".
    /// Exposed as public static so the TLS 1.2 rejection branch (unreachable
    /// in a loopback environment that always negotiates TLS 1.3) can be driven
    /// directly by a unit test without a live TLS 1.2 connection
    /// (Class B branch coverage, REQ-6.3.8, HAZ-020).
    // Safety-critical (SC): HAZ-020 — forward secrecy enforcement.
    static Result check_forward_secrecy(const char* ver,
                                        bool        feature_enabled,
                                        bool        had_session);

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
    /// Caller-injected session store for client-side resumption across close() → init().
    /// Non-owning pointer; caller controls lifetime and must call store.zeroize() when done.
    /// Null means no session resumption attempted even if session_resumption_enabled=true.
    /// Injected via set_session_store() before init(); checked in tls_connect_handshake().
    TlsSessionStore*       m_session_store_ptr;
    /// One-time forward-secrecy warning guard — set on first TLS 1.2 session save to
    /// prevent repetitive WARNING_LO spam across multiple reconnect cycles (NSC).
    bool                   m_fs_warning_logged;
    /// Fix 2: session ticket key context — holds ticket encryption key and lifetime.
    /// Initialized in both constructors; setup in setup_tls_config(); freed in
    /// close() and destructor. Server-side only (REQ-6.3.4).
    mbedtls_ssl_ticket_context m_ticket_ctx;

    // ── Injected interfaces (non-owning; never null after ctor) ──────────────
    ISocketOps*       m_sock_ops;                          ///< Non-owning; never null after ctor
    IMbedtlsOps*      m_tls_ops;                           ///< Non-owning; never null after ctor

    // ── Transport state ──────────────────────────────────────────────────────
    uint32_t          m_client_count;
    uint8_t           m_wire_buf[SOCKET_RECV_BUF_BYTES];
    TransportConfig   m_cfg;
    ImpairmentEngine  m_impairment;
    RingBuffer        m_recv_queue;
    std::atomic<bool> m_open;  // DEF-021-1: atomic for re-entrant-safe close() (CWE-416)
    bool              m_is_server;
    bool              m_tls_enabled;              ///< From config.tls.tls_enabled
    uint32_t          m_connections_opened;       ///< REQ-7.2.4: successful connect/accept events
    uint32_t          m_connections_closed;       ///< REQ-7.2.4: disconnect events
    NodeId            m_client_node_ids[MAX_TCP_CONNECTIONS];  ///< NodeId per client slot (NODE_ID_INVALID = unknown) — REQ-6.1.9
    NodeId            m_local_node_id;            ///< Our own node identity (set by register_local_id) — REQ-6.1.10
    bool              m_client_hello_received[MAX_TCP_CONNECTIONS]; ///< True once HELLO received for this slot — REQ-6.1.8
    uint64_t          m_client_accept_ts[MAX_TCP_CONNECTIONS];      ///< Accept timestamp (µs) for each slot — REQ-6.1.12

    // REQ-3.3.6: circular FIFO of NodeIds from recently-registered HELLO frames.
    // Populated by handle_hello_frame(); drained by pop_hello_peer().
    // DeliveryEngine polls this each receive() to reset stale ordering state on reconnect.
    // Power of 10 Rule 3: fixed-capacity; no heap after init.
    NodeId            m_hello_queue[MAX_TCP_CONNECTIONS] = {};  ///< HELLO peer NodeId queue
    uint32_t          m_hello_queue_read  = 0U;                  ///< Next read index (mod MAX_TCP_CONNECTIONS)
    uint32_t          m_hello_queue_write = 0U;                  ///< Next write index (mod MAX_TCP_CONNECTIONS)
    bool              m_client_slot_active[MAX_TCP_CONNECTIONS]; ///< Fix 5: true = slot in use; avoids ssl_context copy

    // ── Private helpers ──────────────────────────────────────────────────────

    /// Validate TLS-specific config constraints before state mutation in init().
    /// REQ-6.3.6 (H-1): verify_peer=true requires non-empty ca_file → ERR_IO + FATAL.
    /// REQ-6.3.7 (H-2): require_crl=true requires non-empty crl_file → ERR_INVALID + FATAL.
    /// REQ-6.3.9 (H-8): verify_peer=false + non-empty peer_hostname → ERR_INVALID + WARNING_HI.
    /// Only called when tls_enabled is true. Returns OK when all constraints are satisfied.
    // Safety-critical (SC): HAZ-020 — incorrect TLS config validation.
    Result validate_tls_init_config(const TlsConfig& tls_cfg);

    /// Enforce REQ-6.3.8 after a successful client handshake.
    /// Calls check_forward_secrecy() and zeroizes the session store on rejection.
    /// Returns ERR_IO if the resumption is rejected; OK otherwise.
    // Safety-critical (SC): HAZ-020 — forward secrecy enforcement.
    Result enforce_forward_secrecy_if_required(bool had_session);

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
    /// Non-static: uses m_tls_ops->ssl_handshake() for M5 fault injection.
    /// @return 0 on success, non-zero mbedTLS error code on failure.
    int run_tls_handshake_loop(mbedtls_ssl_context* ssl);

    /// Set up the session ticket context and register callbacks on m_ssl_conf.
    /// Fix 2: extracted from setup_tls_config() to keep its CC ≤ 10 (REQ-6.3.4).
    /// Called only when session_resumption_enabled is true and
    /// MBEDTLS_SSL_SESSION_TICKETS is defined; cppcheck-suppress guards the
    /// false-positive unusedPrivateFunction when tickets are compiled out.
    // cppcheck-suppress unusedPrivateFunction
    Result setup_session_tickets(uint32_t lifetime_s);

    /// Apply Fix 1 cipher suite restriction and minimum TLS version to m_ssl_conf.
    /// Extracted from setup_tls_config() to reduce its cognitive complexity.
    void apply_cipher_policy();

    /// Configure session tickets if session_resumption_enabled; no-op otherwise.
    /// Fix 2: extracted from setup_tls_config() to reduce its CC (REQ-6.3.4).
    /// Called only when MBEDTLS_SSL_SESSION_TICKETS is defined.
    // cppcheck-suppress unusedPrivateFunction
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
    // Safety-critical (SC): HAZ-008 — enforces verify_peer+hostname before handshake;
    // failure allows server impersonation (CWE-297, SECURITY_ASSUMPTIONS.md §6).
    Result tls_connect_handshake();

    /// Return true when the injected TlsSessionStore has a valid session that
    /// can be presented to the server for abbreviated resumption (RFC 5077).
    /// Used in tls_connect_handshake() to keep its CC ≤ 10 by moving the
    /// three-part boolean guard into this separate helper.
    /// NSC: pure boolean predicate with no side effects.
    bool has_resumable_session() const;

#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    /// Save the TLS session from slot 0 into *m_session_store_ptr after a
    /// successful client handshake.  Called only when session_resumption_enabled
    /// is true and m_session_store_ptr is non-null.
    /// Failure is non-fatal: logs WARNING_LO and leaves store.session_valid false.
    /// Also logs a one-time WARNING_HI if TLS 1.2 is negotiated (no forward secrecy
    /// for resumed sessions — RFC 5077, SECURITY_ASSUMPTIONS.md §13; system-wide
    /// impact per CLAUDE.md §4 WARNING_HI taxonomy).
    /// Extracted from tls_connect_handshake() to keep its CC ≤ 10 (REQ-6.3.4).
    // Safety-critical (SC): HAZ-012, HAZ-017, HAZ-021 — stores TLS session
    // material; delegates to TlsSessionStore::try_save() which is mutex-protected
    // per REQ-6.3.10 (CWE-362).
    void try_save_client_session();
#endif /* MBEDTLS_SSL_SESSION_TICKETS */

    /// Attempt to load *m_session_store_ptr into m_ssl[0] before the TLS handshake
    /// to enable abbreviated session resumption (RFC 5077, REQ-6.3.4).
    /// Called only when session_resumption_enabled is true, m_session_store_ptr is
    /// non-null, and m_session_store_ptr->session_valid is true.
    /// Failure is non-fatal: logs WARNING_LO and full handshake proceeds.
    /// Extracted from tls_connect_handshake() to reduce its CC.
    // Safety-critical (SC): HAZ-017, HAZ-021 — delegates to
    // TlsSessionStore::try_load() which re-checks session_valid under the
    // mutex lock (TOCTOU prevention) per REQ-6.3.10 (CWE-362).
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
    // Safety-critical (SC): HAZ-009 — performs cross-slot scan for duplicate NodeId;
    // failure allows a second connection to hijack an active peer's routing entry.
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

    /// REQ-6.1.12 (H-5 / HAZ-023 / CWE-400): sweep all server slots that have
    /// not sent a HELLO within hello_timeout_ms (= channels[0].recv_timeout_ms)
    /// of acceptance and evict them.  Prevents slot-exhaustion DoS via slow-connect.
    /// Safety-critical (SC): HAZ-023
    void sweep_hello_timeouts();

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
