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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.1.7, REQ-7.1.1
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
    /// Logs a warning for each client that fails; does not abort on partial failure.
    /// @param[in] buf Serialized frame bytes.
    /// @param[in] len Number of bytes in buf.
    void send_to_all_clients(const uint8_t* buf, uint32_t len);

    /// Collect deliverable delayed messages from the impairment engine and
    /// serialize+send each to all clients.
    /// @param[in] now_us Current wall-clock time in microseconds.
    void flush_delayed_to_clients(uint64_t now_us);

    /// Flush delayed messages from the impairment engine into m_recv_queue.
    /// @param[in] now_us Current wall-clock time in microseconds.
    void flush_delayed_to_queue(uint64_t now_us);

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

    /// Iterate all active client fds and call recv_from_client() on each.
    /// Extracted from poll_clients_once() to reduce its CC.
    void drain_readable_clients(uint32_t timeout_ms);
};

#endif // PLATFORM_TCP_BACKEND_HPP
