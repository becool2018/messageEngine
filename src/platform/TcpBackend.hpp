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
 * @file TcpBackend.hpp
 * @brief TCP transport backend implementing TransportInterface.
 *
 * Provides connection-oriented message transport over TCP/IP.
 * Supports both client and server modes. In server mode, listens for and
 * manages multiple client connections. In client mode, maintains a single
 * connection to a peer.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §6.1 (TCP backend),
 * CLAUDE.md §2.2 (Transport abstraction), CLAUDE.md §4.1 (Core operations).
 *
 * Rules applied:
 *   - Power of 10 rules: no dynamic allocation after init, fixed loop bounds,
 *     ≥2 assertions per function, bounded functions.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result enum returns, event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.1.7, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6
 */

#ifndef PLATFORM_TCP_BACKEND_HPP
#define PLATFORM_TCP_BACKEND_HPP

#include <cstdint>
#include <poll.h>
#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

class ISocketOps;  // forward declaration — see platform/ISocketOps.hpp

// ─────────────────────────────────────────────────────────────────────────────
// TcpBackend: TCP-based transport implementation
// ─────────────────────────────────────────────────────────────────────────────

class TcpBackend : public TransportInterface {
public:
    /// Default constructor — uses the process-wide SocketOpsImpl singleton.
    /// Production code always calls this form.
    TcpBackend();

    /// Injection constructor — accepts any ISocketOps implementation.
    /// Used in tests to inject a mock. @p ops must outlive this instance.
    explicit TcpBackend(ISocketOps& ops);
    ~TcpBackend() override;

    // TransportInterface implementation
    Result init(const TransportConfig& config) override;
    // Safety-critical (SC): HAZ-005, HAZ-006 — verified to M5
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-004, HAZ-005 — verified to M5
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void close() override;
    bool is_open() const override;
    /// REQ-7.2.4 / REQ-7.2.2 — NSC observability accessor.
    void get_transport_stats(TransportStats& out) const override;

    /// REQ-6.1.10: store our node ID; client mode sends HELLO frame to server.
    Result register_local_id(NodeId id) override;

    // REQ-3.3.6: pop one NodeId from the HELLO reconnect queue.
    // Returns NODE_ID_INVALID when the queue is empty.
    // Called by DeliveryEngine::drain_hello_reconnects() to reset stale ordering state.
    NodeId pop_hello_peer() override;

private:
    // ───────────────────────────────────────────────────────────────────────
    // Member state (Power of 10 rule 3: fixed allocation, no heap after init)
    // ───────────────────────────────────────────────────────────────────────
    ISocketOps*        m_sock_ops;                            ///< Non-owning; never null after ctor
    int                m_listen_fd;                           ///< Server listening socket
    int                m_client_fds[MAX_TCP_CONNECTIONS];     ///< Connected client FDs
    uint32_t           m_client_count;                        ///< Number of active clients
    uint8_t            m_wire_buf[SOCKET_RECV_BUF_BYTES];     ///< Serialization buffer
    TransportConfig    m_cfg;                                 ///< Transport configuration
    ImpairmentEngine   m_impairment;                          ///< Impairment simulator
    RingBuffer         m_recv_queue;                          ///< Inbound message queue
    bool               m_open;                                ///< Transport open/closed state
    bool               m_is_server;                           ///< Server vs client mode
    uint32_t           m_connections_opened;                  ///< REQ-7.2.4: successful connect/accept events
    uint32_t           m_connections_closed;                  ///< REQ-7.2.4: disconnect events
    NodeId             m_client_node_ids[MAX_TCP_CONNECTIONS];     ///< NodeId for each client slot (NODE_ID_INVALID = unknown)
    NodeId             m_local_node_id;                            ///< Our own node identity (set by register_local_id)
    bool               m_client_hello_received[MAX_TCP_CONNECTIONS]; ///< True once HELLO received for this slot — REQ-6.1.8

    // REQ-3.3.6: circular FIFO of NodeIds from recently-registered HELLO frames.
    // Populated by handle_hello_frame(); drained by pop_hello_peer().
    // DeliveryEngine polls this each receive() to reset stale ordering state on reconnect.
    // Power of 10 Rule 3: fixed-capacity; no heap after init.
    NodeId             m_hello_queue[MAX_TCP_CONNECTIONS] = {};   ///< HELLO peer NodeId queue
    uint32_t           m_hello_queue_read  = 0U;                   ///< Next read index (mod MAX_TCP_CONNECTIONS)
    uint32_t           m_hello_queue_write = 0U;                   ///< Next write index (mod MAX_TCP_CONNECTIONS)

    // ───────────────────────────────────────────────────────────────────────
    // Private helper methods (Power of 10: small, single-purpose functions)
    // ───────────────────────────────────────────────────────────────────────

    /// Attempt to connect to a remote server as a client.
    /// @return OK on successful connection, ERR_IO on failure.
    Result connect_to_server();

    /// Create listening socket, bind, and listen (server mode only).
    /// @param[in] ip   Bind address string.
    /// @param[in] port Bind port number.
    /// @return OK on success; ERR_IO on any socket setup failure.
    Result bind_and_listen(const char* ip, uint16_t port);

    /// Accept pending client connections (non-blocking poll).
    /// Adds new clients to m_client_fds if space available.
    /// @return OK or warning; does not fail if no pending connection.
    Result accept_clients();

    /// Remove and close a specific client fd from the active list.
    /// Compacts the fd array; does nothing if fd is not found.
    /// @param[in] client_fd File descriptor to remove.
    void remove_client_fd(int client_fd);

    /// Receive and deserialize a frame from a client.
    /// On connection loss, closes the fd and removes from active list.
    /// @param[in] client_fd File descriptor of connected client.
    /// @param[in] timeout_ms Receive timeout in milliseconds.
    /// @return OK on frame received and queued, ERR_IO on connection loss.
    Result recv_from_client(int client_fd, uint32_t timeout_ms);

    /// Send serialized data to all currently connected clients.
    /// Logs a warning for each client that fails.
    /// @param[in] buf Serialized frame bytes.
    /// @param[in] len Number of bytes in buf.
    /// @return true if any send_frame() call fails, false if all succeed.
    bool send_to_all_clients(const uint8_t* buf, uint32_t len);

    /// Collect deliverable delayed messages from the impairment engine and
    /// serialize+send each to all clients.
    /// Failure attribution: if the send fails for the current envelope
    /// (matched by source_id + message_id), ERR_IO is returned.
    /// Failures for other envelopes are logged at WARNING_LO and skipped.
    /// @param[in] current_env The envelope just queued by send_message().
    /// @param[in] now_us      Current wall-clock time in microseconds.
    /// @return ERR_IO if send fails for current_env; OK otherwise.
    Result flush_delayed_to_clients(const MessageEnvelope& current_env,
                                    uint64_t now_us);

    /// Poll all connected clients once (accept new ones if server).
    /// Receives up to one frame per client into m_recv_queue.
    /// @param[in] timeout_ms Per-client receive timeout in milliseconds.
    void poll_clients_once(uint32_t timeout_ms);

    // ── CC-reduction helpers (extracted to keep each caller CC ≤ 10) ─────────

    /// Build a pollfd array from the listen fd (server, if applicable) and all
    /// active client fds. Sets *has_listen_out to indicate whether pfds[0] is
    /// the listen fd. Returns the number of entries populated.
    /// Extracted from poll_clients_once() to reduce its CC.
    uint32_t build_poll_fds(struct pollfd* pfds, uint32_t cap,
                             bool* has_listen_out) const;

    /// Iterate active clients; call recv_from_client() only on fds with POLLIN set.
    /// @param[in] pfds       pollfd array snapshot from build_poll_fds().
    /// @param[in] nfds       Number of entries in pfds.
    /// @param[in] timeout_ms Per-client receive timeout passed to recv_from_client().
    /// @param[in] has_listen True if pfds[0] is the listen fd (client fds start at index 1).
    void drain_readable_clients(const struct pollfd* pfds, uint32_t nfds,
                                 uint32_t timeout_ms, bool has_listen);

    /// Route a deserialized inbound envelope through the ImpairmentEngine.
    /// Checks partition state (inbound drop) then calls process_inbound()
    /// for reorder simulation. Pushes to m_recv_queue only if delivered.
    /// NSC: bookkeeping helper; routes inbound envelope through impairment before queuing.
    /// @param[in] env    Deserialized inbound envelope.
    /// @param[in] now_us Current wall-clock time in microseconds.
    /// @return true if envelope was pushed to m_recv_queue; false if dropped or buffered.
    bool apply_inbound_impairment(const MessageEnvelope& env, uint64_t now_us);

    /// Build and send a HELLO envelope to the server (client mode only).
    /// Called once from register_local_id() after m_local_node_id is set.
    Result send_hello_frame();

    /// CC-reduction helper for recv_from_client().
    /// Processes an inbound HELLO frame under the one-HELLO-per-connection rule
    /// (REQ-6.1.8 / Fix 4). Looks up the slot for @p client_fd, rejects duplicate
    /// HELLOs, and on first HELLO calls handle_hello_frame() and marks the slot.
    /// @param[in] client_fd  File descriptor that sent the HELLO.
    /// @param[in] src_id     NodeId from the HELLO envelope's source_id field.
    /// @return ERR_INVALID if this is a duplicate HELLO; ERR_AGAIN otherwise
    ///         (indicating the frame was consumed and must not reach DeliveryEngine).
    Result process_hello_frame(int client_fd, NodeId src_id);

    /// CC-reduction helper for recv_from_client() (Fix 3).
    /// Checks whether a non-HELLO frame from @p client_fd should be rejected
    /// because the slot has not yet completed HELLO registration (REQ-6.1.11 / HAZ-009).
    /// @param[in] client_fd  File descriptor of the sending client.
    /// @return true if the frame must be dropped (slot unregistered); false if allowed.
    bool is_unregistered_slot(int client_fd) const;

    /// Record the NodeId of a connecting client in the routing table.
    /// @param[in] client_fd  File descriptor that sent the HELLO.
    /// @param[in] src_id     NodeId extracted from the HELLO envelope.
    void handle_hello_frame(int client_fd, NodeId src_id);

    /// G-3: Close and evict a client slot whose HELLO was rejected.
    /// Calls remove_client_fd() which closes the socket and compacts the slot
    /// arrays; callers MUST return immediately after this call because all
    /// slot indices are invalidated by the compaction.
    /// @param[in] client_fd  File descriptor to close and evict.
    void close_and_evict_slot(int client_fd);

    /// REQ-6.1.11 / HAZ-009: verify envelope source_id matches the NodeId
    /// registered in the HELLO from this fd's slot. Returns false (and logs
    /// WARNING_HI) if the slot has a registered identity that does not match.
    /// Returns true (allow) if slot is unregistered (NODE_ID_INVALID).
    /// Safety-critical (SC): HAZ-009
    bool validate_source_id(int client_fd, NodeId claimed_id) const;

    /// Find the client array slot for a given destination NodeId.
    /// @return slot index in [0, m_client_count) or MAX_TCP_CONNECTIONS if not found.
    uint32_t find_client_slot(NodeId dst) const;

    /// Send serialized data to a single client slot.
    /// @param[in] slot  Index into m_client_fds[].
    /// @param[in] buf   Frame bytes.
    /// @param[in] len   Number of bytes.
    /// @return true if send fails, false on success.
    bool send_to_slot(uint32_t slot, const uint8_t* buf, uint32_t len);

    /// CC-reduction helper for flush_delayed_to_clients().
    /// Serialize one delayed envelope and route it (unicast or broadcast).
    /// @param[in]  env      Envelope to send.
    /// @param[out] failed   Set to true if the send failed.
    /// @return ERR_INVALID if unicast destination is unknown; OK otherwise.
    Result send_one_delayed(const MessageEnvelope& env, bool& failed);

    /// CC-reduction helper for flush_delayed_to_clients().
    /// Update final_result based on send outcome for one delayed envelope.
    /// @param[in]  route_r    Result from send_one_delayed().
    /// @param[in]  failed     Whether the send call reported failure.
    /// @param[in]  is_current True if this envelope matches the caller's current send.
    /// @param[out] final_result Accumulates the worst error for the current envelope.
    static void apply_send_result(Result route_r, bool failed,
                                  bool is_current, Result& final_result);
};

#endif // PLATFORM_TCP_BACKEND_HPP
