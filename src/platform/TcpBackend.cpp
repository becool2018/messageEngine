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
 * @file TcpBackend.cpp
 * @brief Implementation of TCP transport backend.
 *
 * Implements connection management, framing, and impairment simulation
 * for message delivery over TCP.
 *
 * Rules applied:
 *   - Power of 10: all functions ≤1 page, ≥2 assertions each, fixed loop bounds.
 *   - MISRA C++: no exceptions, all return values checked.
 *   - F-Prime style: event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.1.7, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11, REQ-6.1.12, REQ-6.3.5, REQ-7.1.1, REQ-7.2.4, REQ-5.1.5, REQ-5.1.6
 */

#include "platform/TcpBackend.hpp"
#include "platform/ISocketOps.hpp"
#include "platform/SocketOpsImpl.hpp"
#include "platform/SocketUtils.hpp"
#include "core/Assert.hpp"
#include "core/Serializer.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include <poll.h>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

TcpBackend::TcpBackend()
    : m_sock_ops(&SocketOpsImpl::instance()),
      m_listen_fd(-1), m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_local_node_id(NODE_ID_INVALID)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);  // Invariant
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_fds[i]            = -1;
        m_client_node_ids[i]       = NODE_ID_INVALID;
        m_client_hello_received[i] = false;   // REQ-6.1.8: no HELLO received yet
        m_client_accept_ts[i]      = 0ULL;    // REQ-6.1.12: no accept timestamp yet
        m_hello_queue[i]           = NODE_ID_INVALID;  // REQ-3.3.6
    }
    m_hello_queue_read  = 0U;  // REQ-3.3.6
    m_hello_queue_write = 0U;  // REQ-3.3.6
}

TcpBackend::TcpBackend(ISocketOps& ops)
    : m_sock_ops(&ops),
      m_listen_fd(-1), m_client_count(0U), m_wire_buf{}, m_cfg{},
      m_open(false), m_is_server(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_local_node_id(NODE_ID_INVALID)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U);  // Invariant
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_fds[i]            = -1;
        m_client_node_ids[i]       = NODE_ID_INVALID;
        m_client_hello_received[i] = false;   // REQ-6.1.8: no HELLO received yet
        m_client_accept_ts[i]      = 0ULL;    // REQ-6.1.12: no accept timestamp yet
        m_hello_queue[i]           = NODE_ID_INVALID;  // REQ-3.3.6
    }
    m_hello_queue_read  = 0U;  // REQ-3.3.6
    m_hello_queue_write = 0U;  // REQ-3.3.6
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

TcpBackend::~TcpBackend()
{
    // Power of 10: explicit qualified call avoids virtual dispatch in destructor
    // (static dispatch to concrete implementation is the intended behaviour here)
    TcpBackend::close();
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::bind_and_listen(const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);          // Power of 10: valid pointer
    NEVER_COMPILED_OUT_ASSERT(m_is_server);            // Server mode only

    m_listen_fd = m_sock_ops->create_tcp(socket_is_ipv6(ip));
    if (m_listen_fd < 0) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_create_tcp failed in server mode");
        return Result::ERR_IO;
    }
    if (!m_sock_ops->set_reuseaddr(m_listen_fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    if (!m_sock_ops->do_bind(m_listen_fd, ip, port)) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_bind failed on port %u", port);
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    if (!m_sock_ops->do_listen(m_listen_fd, static_cast<int>(MAX_TCP_CONNECTIONS))) {
        Logger::log(Severity::FATAL, "TcpBackend", "socket_listen failed");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    // Make listen fd non-blocking so accept_clients() returns immediately
    // when no pending connection exists, allowing receive_message() to honour
    // its timeout. Power of 10 Rule 2 deviation: infrastructure poll loop.
    if (!m_sock_ops->set_nonblocking(m_listen_fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "socket_set_nonblocking failed on listen fd");
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
        return Result::ERR_IO;
    }
    m_open = true;
    Logger::log(Severity::INFO, "TcpBackend", "Server listening on %s:%u", ip, port);
    NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0);       // Power of 10: post-condition
    return Result::OK;
}

Result TcpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(!m_open);  // Not already initialized

    // S5: validate config before any channels[] access (REQ-6.1.1, ChannelConfig.hpp).
    if (!transport_config_valid(config)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "init: num_channels=%u exceeds MAX_CHANNELS; rejecting config",
                    config.num_channels);
        return Result::ERR_INVALID;
    }

    m_cfg       = config;
    m_is_server = config.is_server;

    // Initialize receive queue and impairment engine
    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    // REQ-6.1.8, REQ-6.1.9, REQ-6.1.12: zero node-identity routing table during init phase
    // Power of 10 rule 2: fixed loop bound (MAX_TCP_CONNECTIONS)
    m_local_node_id = NODE_ID_INVALID;
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        m_client_node_ids[i]       = NODE_ID_INVALID;
        m_client_hello_received[i] = false;  // REQ-6.1.8: no HELLO received yet
        m_client_accept_ts[i]      = 0ULL;   // REQ-6.1.12: no accept timestamp yet
    }

    Result res;
    if (m_is_server) {
        res = bind_and_listen(config.bind_ip, config.bind_port);
    } else {
        res = connect_to_server();
    }
    if (!result_ok(res)) {
        return res;
    }

    NEVER_COMPILED_OUT_ASSERT(m_open);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// connect_to_server()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::connect_to_server()
{
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);  // Client mode only
    NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] == -1);  // Not yet connected

    int fd = m_sock_ops->create_tcp(socket_is_ipv6(m_cfg.peer_ip));
    if (fd < 0) {
        Logger::log(Severity::FATAL, "TcpBackend",
                   "socket_create_tcp failed in client mode");
        return Result::ERR_IO;
    }

    if (!m_sock_ops->set_reuseaddr(fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                   "socket_set_reuseaddr failed");
        m_sock_ops->do_close(fd);
        return Result::ERR_IO;
    }

    if (!m_sock_ops->connect_with_timeout(fd, m_cfg.peer_ip, m_cfg.peer_port,
                                          m_cfg.connect_timeout_ms)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                   "Connection to %s:%u failed", m_cfg.peer_ip, m_cfg.peer_port);
        m_sock_ops->do_close(fd);
        return Result::ERR_IO;
    }

    m_client_fds[0U] = fd;
    m_client_count   = 1U;
    m_open           = true;
    ++m_connections_opened;  // REQ-7.2.4: successful client connect

    Logger::log(Severity::INFO, "TcpBackend",
               "Connected to %s:%u", m_cfg.peer_ip, m_cfg.peer_port);

    NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] >= 0);  // Post-condition
    NEVER_COMPILED_OUT_ASSERT(m_client_count == 1U);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// accept_clients()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::accept_clients()
{
    NEVER_COMPILED_OUT_ASSERT(m_is_server);  // Server mode only
    NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0);  // Listening socket must be open

    if (m_client_count >= MAX_TCP_CONNECTIONS) {
        // Already at capacity; don't try to accept
        return Result::OK;
    }

    int client_fd = m_sock_ops->do_accept(m_listen_fd);
    if (client_fd < 0) {
        // No pending connection (EAGAIN in non-blocking mode) is not an error
        return Result::OK;
    }

    // Add new client to array
    NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);
    m_client_fds[m_client_count]            = client_fd;
    m_client_hello_received[m_client_count] = false;             // REQ-6.1.8: must receive HELLO before data
    m_client_accept_ts[m_client_count]      = timestamp_now_us(); // REQ-6.1.12: record accept time for HELLO timeout
    ++m_client_count;
    ++m_connections_opened;  // REQ-7.2.4: successful server accept

    Logger::log(Severity::INFO, "TcpBackend",
               "Accepted client %u, total clients: %u", m_client_count - 1U, m_client_count);

    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_client_fd() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::remove_client_fd(int client_fd)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);           // Power of 10: valid descriptor
    NEVER_COMPILED_OUT_ASSERT(m_client_count > 0U);      // Power of 10: at least one client exists

    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] == client_fd) {
            m_sock_ops->do_close(m_client_fds[i]);
            m_client_fds[i] = -1;
            // Compact the fd, node-id, hello-flag, and accept-timestamp arrays by shifting left.
            // Power of 10 rule 2: bound is m_client_count - 1 (< MAX_TCP_CONNECTIONS)
            for (uint32_t j = i; j < m_client_count - 1U; ++j) {
                m_client_fds[j]            = m_client_fds[j + 1U];
                m_client_node_ids[j]       = m_client_node_ids[j + 1U];
                m_client_hello_received[j] = m_client_hello_received[j + 1U];  // REQ-6.1.8
                m_client_accept_ts[j]      = m_client_accept_ts[j + 1U];       // REQ-6.1.12
            }
            m_client_fds[m_client_count - 1U]            = -1;
            m_client_node_ids[m_client_count - 1U]       = NODE_ID_INVALID;
            m_client_hello_received[m_client_count - 1U] = false;   // REQ-6.1.8
            m_client_accept_ts[m_client_count - 1U]      = 0ULL;    // REQ-6.1.12
            --m_client_count;
            ++m_connections_closed;  // REQ-7.2.4: connection removed
            NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS);  // Power of 10: post-condition
            return;
        }
    }
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

bool TcpBackend::apply_inbound_impairment(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);        // Pre-condition

    // REQ-5.1.6: drop if an inbound partition is currently active.
    if (m_impairment.is_partition_active(now_us)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
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
        Logger::log(Severity::WARNING_LO, "TcpBackend",
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
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "recv queue full; dropping inbound envelope");
    }

    NEVER_COMPILED_OUT_ASSERT(inbound_count == 1U);  // Post-condition
    return result_ok(res);
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_from_client()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);  // Power of 10: valid fd
    NEVER_COMPILED_OUT_ASSERT(m_open);          // Power of 10: transport must be open

    uint32_t out_len = 0U;
    if (!m_sock_ops->recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, timeout_ms, &out_len)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend", "recv_frame failed; closing connection");
        remove_client_fd(client_fd);
        return Result::ERR_IO;
    }

    // Deserialize the received frame
    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                   "Deserialize failed: %u", static_cast<uint8_t>(res));
        return res;
    }

    // REQ-6.1.8: intercept HELLO frames before impairment processing.
    // process_hello_frame() enforces one-HELLO-per-connection (Fix 4) and
    // registers the client's NodeId in the routing table. HELLO frames are
    // never delivered to the DeliveryEngine.
    if (env.message_type == MessageType::HELLO) {
        if (m_is_server) {
            return process_hello_frame(client_fd, env.source_id);
        }
        return Result::ERR_AGAIN;  // client received HELLO echo; consumed
    }

    // REQ-6.1.11 / HAZ-009: reject data frames from unregistered slots (server mode
    // only). Clients must complete HELLO registration before any data frame is
    // accepted. The guard is skipped in client mode because the server slot has
    // NODE_ID_INVALID by design (server never sends HELLO back) and the slot would
    // always appear unregistered, causing all client-mode receives to be dropped.
    if (m_is_server && is_unregistered_slot(client_fd)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "data frame from unregistered slot fd=%d: dropping (REQ-6.1.11)",
                    client_fd);
        return Result::ERR_INVALID;
    }

    // REQ-6.1.11: validate envelope source_id against this slot's registered
    // NodeId. Prevents source_id spoofing attacks (HAZ-009).
    // Safety-critical (SC): HAZ-009
    if (!validate_source_id(client_fd, env.source_id)) {
        return Result::ERR_INVALID;  // silent discard; WARNING_HI already logged
    }

    // SEC-025 (client mode): lock in the server's NodeId from the first accepted
    // inbound frame. validate_source_id() allowed it (slot was NODE_ID_INVALID);
    // record it now so all subsequent frames are validated against this NodeId.
    // Uses m_client_node_ids[0] — the only slot in client mode
    // (m_client_count == 1, set by connect_to_server()). Mirrors TlsTcpBackend SEC-011.
    if (!m_is_server && (m_client_node_ids[0U] == NODE_ID_INVALID) &&
        (env.source_id != NODE_ID_INVALID)) {
        m_client_node_ids[0U] = env.source_id;
        Logger::log(Severity::INFO, "TcpBackend",
                    "SEC-025: server NodeId locked in as %u (REQ-6.1.11)",
                    static_cast<unsigned int>(env.source_id));
    }

    // REQ-5.1.5, REQ-5.1.6: route through impairment before queuing.
    // Partition drops and reorder buffering apply on the inbound path.
    uint64_t now_us = timestamp_now_us();
    (void)apply_inbound_impairment(env, now_us);

    NEVER_COMPILED_OUT_ASSERT(out_len <= SOCKET_RECV_BUF_BYTES);  // Power of 10: post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_all_clients() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool TcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);         // Power of 10: valid buffer
    NEVER_COMPILED_OUT_ASSERT(len > 0U);               // Power of 10: non-empty frame

    bool any_failed = false;

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] < 0) {
            continue;
        }
        if (!m_sock_ops->send_frame(m_client_fds[i], buf, len,
                                    m_cfg.channels[0U].send_timeout_ms)) {
            Logger::log(Severity::WARNING_LO, "TcpBackend",
                       "Send frame failed on client %u", i);
            any_failed = true;
        }
    }
    return any_failed;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_one_delayed() — CC-reduction helper for flush_delayed_to_clients
// ─────────────────────────────────────────────────────────────────────────────
//
// Serializes a single delayed envelope and routes it (unicast or broadcast).
// REQ-6.1.9: server sends to specific client by NodeId when destination is
// known; falls back to broadcast when destination is NODE_ID_INVALID.
// @param[in]  env     Envelope to serialize and send.
// @param[out] failed  Set to true if the send call returned failure.
// @return ERR_INVALID if unicast destination has no registered slot; OK otherwise.

Result TcpBackend::send_one_delayed(const MessageEnvelope& env, bool& failed)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);  // Invariant

    failed = false;

    uint32_t wire_len = 0U;
    Result ser_r = Serializer::serialize(env, m_wire_buf,
                                         static_cast<uint32_t>(sizeof(m_wire_buf)),
                                         wire_len);
    if (!result_ok(ser_r)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                    "send_one_delayed: serialize failed");
        failed = true;
        return Result::OK;  // caller uses failed flag; ERR_IO not distinguishable here
    }

    // REQ-6.1.9: unicast routing — broadcast when not server or dst unknown.
    bool do_broadcast = (!m_is_server) || (env.destination_id == NODE_ID_INVALID);
    if (do_broadcast) {
        failed = send_to_all_clients(m_wire_buf, wire_len);
        return Result::OK;
    }

    uint32_t slot = find_client_slot(env.destination_id);
    if (slot >= MAX_TCP_CONNECTIONS) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "send_one_delayed: no slot for dst=%u", env.destination_id);
        return Result::ERR_INVALID;
    }
    failed = send_to_slot(slot, m_wire_buf, wire_len);
    // F-7: propagate send failure as ERR_IO so apply_send_result does not depend
    // solely on is_current detection to surface unicast send errors. This ensures
    // RELIABLE_RETRY messages receive ERR_IO from send_message() and are retried.
    return failed ? Result::ERR_IO : Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_send_result() — CC-reduction helper for flush_delayed_to_clients
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::apply_send_result(Result route_r, bool failed,
                                   bool is_current, Result& final_result)
{
    NEVER_COMPILED_OUT_ASSERT(final_result == Result::OK ||
                              final_result == Result::ERR_IO ||
                              final_result == Result::ERR_INVALID);  // Pre-condition
    // F-7: route_r may now be ERR_IO when send_one_delayed returns failure
    // directly (unicast send_to_slot failed), in addition to the existing
    // ERR_INVALID case (no routing slot for destination).
    NEVER_COMPILED_OUT_ASSERT(route_r == Result::OK ||
                              route_r == Result::ERR_IO   ||
                              route_r == Result::ERR_INVALID);       // Pre-condition

    if (!result_ok(route_r) && is_current) {
        final_result = route_r;
        return;
    }
    if (failed && is_current) {
        final_result = Result::ERR_IO;
        return;
    }
    if (failed) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                    "flush_delayed: send failed for non-current msg");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_clients() — private helper
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::flush_delayed_to_clients(const MessageEnvelope& current_env,
                                             uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);          // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(m_open);                 // Power of 10: transport must be open

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    Result final_result = Result::OK;

    // Power of 10 rule 2: fixed loop bound (IMPAIR_DELAY_BUF_SIZE)
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        bool is_current = (delayed[i].source_id == current_env.source_id) &&
                          (delayed[i].message_id == current_env.message_id);

        bool failed = false;
        Result route_r = send_one_delayed(delayed[i], failed);
        apply_send_result(route_r, failed, is_current, final_result);
    }
    return final_result;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_poll_fds() — CC-reduction helper for poll_clients_once
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TcpBackend::build_poll_fds(struct pollfd* pfds, uint32_t cap,
                                     bool* has_listen_out) const
{
    NEVER_COMPILED_OUT_ASSERT(pfds != nullptr);
    NEVER_COMPILED_OUT_ASSERT(has_listen_out != nullptr);

    uint32_t nfds = 0U;
    bool server_ok      = m_is_server && (m_listen_fd >= 0);
    *has_listen_out     = server_ok && (m_client_count < MAX_TCP_CONNECTIONS);

    if (*has_listen_out) {
        pfds[0U].fd      = m_listen_fd;
        pfds[0U].events  = POLLIN;
        pfds[0U].revents = 0;
        nfds = 1U;
    }

    // Power of 10 Rule 2: fixed bound (MAX_TCP_CONNECTIONS)
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] >= 0) {
            NEVER_COMPILED_OUT_ASSERT(nfds < cap);
            pfds[nfds].fd      = m_client_fds[i];
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
    }
    NEVER_COMPILED_OUT_ASSERT(nfds <= cap);
    return nfds;
}

// ─────────────────────────────────────────────────────────────────────────────
// drain_readable_clients() — CC-reduction helper for poll_clients_once
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::drain_readable_clients(const struct pollfd* pfds, uint32_t nfds,
                                         uint32_t timeout_ms, bool has_listen)
{
    NEVER_COMPILED_OUT_ASSERT(pfds != nullptr);
    NEVER_COMPILED_OUT_ASSERT(nfds <= MAX_TCP_CONNECTIONS + 1U);

    const uint32_t base = has_listen ? 1U : 0U;
    // Power of 10 Rule 2: bounded by m_client_count ≤ MAX_TCP_CONNECTIONS
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        const uint32_t pfd_idx = base + i;
        if (pfd_idx >= nfds) {
            break;
        }
        if ((pfds[pfd_idx].revents & POLLIN) != 0) {
            (void)recv_from_client(m_client_fds[i], timeout_ms);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// poll_clients_once() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::poll_clients_once(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);                  // Power of 10: transport must be open
    NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U);    // Power of 10: reasonable per-poll timeout

    // REQ-6.1.12: evict server slots that timed out waiting for HELLO before polling.
    // Sweep first so that stale fds are removed from m_client_fds before build_poll_fds.
    if (m_is_server) {
        sweep_hello_timeouts();
    }

    // Build a pollfd set: listen fd (server, not at capacity) + all client fds.
    // Power of 10: fixed-size stack array; bounded build loop.
    static const uint32_t MAX_POLL_FDS = MAX_TCP_CONNECTIONS + 1U;
    struct pollfd pfds[MAX_POLL_FDS];
    bool has_listen = false;

    uint32_t nfds = build_poll_fds(pfds, MAX_POLL_FDS, &has_listen);

    if (nfds == 0U) {
        return;  // No fds to watch; outer loop handles retry timing
    }

    // Block until a fd is readable or timeout_ms elapses.
    // Power of 10 Rule 2 deviation: infrastructure poll; bounded per-call timeout.
    // CERT INT31-C: timeout_ms <= 60000U (asserted above) << INT_MAX, so the
    // static_cast<int> is safe without an explicit clamp.
    int poll_rc = poll(pfds, static_cast<nfds_t>(nfds),
                       static_cast<int>(timeout_ms));
    if (poll_rc <= 0) {
        return;  // Timeout (0) or error (−1): nothing to accept or receive
    }

    // Accept new connection if listen fd is readable
    if (has_listen && ((pfds[0U].revents & POLLIN) != 0)) {
        (void)accept_clients();
    }

    drain_readable_clients(pfds, nfds, timeout_ms, has_listen);
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);                    // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));  // Pre-condition

    // Serialize to wire buffer
    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TcpBackend", "Serialize failed");
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

    // Discard if no clients are connected
    if (m_client_count == 0U) {
        Logger::log(Severity::WARNING_LO, "TcpBackend",
                   "No clients connected; discarding message");
        return Result::OK;
    }

    // process_outbound() already queued the message into the impairment delay
    // buffer (with release_us = now_us when latency is 0). flush_delayed_to_clients()
    // collects all deliverable messages and sends them — covering both the 0-delay
    // pass-through case and the delayed case.  Calling send_to_all_clients() here as
    // well would cause every message to be sent twice.
    res = flush_delayed_to_clients(envelope, now_us);

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // Post-condition
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition

    // Check receive queue first
    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) {
        return res;
    }

    uint32_t poll_count = (timeout_ms + 99U) / 100U;
    if (poll_count > 50U) {
        poll_count = 50U;  // Cap at 5 seconds worth
    }
    NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U);  // Power of 10: bounded loop count

    // Power of 10 rule 2: fixed loop bound (capped above)
    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt) {
        poll_clients_once(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }

        uint64_t now_us = timestamp_now_us();
        // No current send envelope in receive context; use a zero-init sentinel
        // so flush attribution never matches a real in-flight message.
        MessageEnvelope no_current_env{};
        (void)flush_delayed_to_clients(no_current_env, now_us);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }
    }

    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::close()
{
    if (m_listen_fd >= 0) {
        m_sock_ops->do_close(m_listen_fd);
        m_listen_fd = -1;
    }

    // REQ-7.2.4: count clients still connected at graceful shutdown
    uint32_t closed_count = 0U;

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i) {
        if (m_client_fds[i] >= 0) {
            m_sock_ops->do_close(m_client_fds[i]);
            m_client_fds[i] = -1;
            ++closed_count;
        }
    }

    m_connections_closed += closed_count;
    m_client_count = 0U;
    m_open         = false;
    Logger::log(Severity::INFO, "TcpBackend", "Transport closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool TcpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open == (m_listen_fd >= 0 || m_client_count > 0U) || !m_open);
    return m_open;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_transport_stats() — REQ-7.2.4 / REQ-7.2.2 observability
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::get_transport_stats(TransportStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened >= m_connections_closed);  // Assert: monotonic counters
    NEVER_COMPILED_OUT_ASSERT(m_connections_closed <= m_connections_opened);  // Assert: closed ≤ opened

    transport_stats_init(out);
    out.connections_opened = m_connections_opened;
    out.connections_closed = m_connections_closed;
    out.impairment         = m_impairment.get_stats();
}

// ─────────────────────────────────────────────────────────────────────────────
// register_local_id() — REQ-6.1.10: store our NodeId; client sends HELLO
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::register_local_id(NodeId id)
{
    NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID);  // Pre-condition: valid node ID
    NEVER_COMPILED_OUT_ASSERT(m_open);                 // Pre-condition: transport is open

    m_local_node_id = id;
    if (!m_is_server) {
        return send_hello_frame();
    }
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_hello_frame() — REQ-6.1.8: client announces its NodeId to the server
// ─────────────────────────────────────────────────────────────────────────────

Result TcpBackend::send_hello_frame()
{
    NEVER_COMPILED_OUT_ASSERT(m_local_node_id != NODE_ID_INVALID);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(!m_is_server);                         // Client mode only

    MessageEnvelope hello;
    envelope_init(hello);
    hello.message_type   = MessageType::HELLO;
    hello.source_id      = m_local_node_id;
    hello.destination_id = NODE_ID_INVALID;   // server NodeId not yet known
    hello.payload_length = 0U;

    uint32_t wire_len = 0U;
    Result ser_r = Serializer::serialize(hello, m_wire_buf,
                                         static_cast<uint32_t>(sizeof(m_wire_buf)),
                                         wire_len);
    if (!result_ok(ser_r)) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "send_hello_frame: serialize failed %u",
                    static_cast<uint8_t>(ser_r));
        return ser_r;
    }

    // G-8: guard against calling send_frame with an invalid fd (-1).
    // m_client_fds[0] is set by connect_to_server(); if the connection was never
    // established or was closed, this would pass -1 to send_frame, causing silent
    // failure or UB in the underlying send() system call.
    if (m_client_fds[0U] < 0) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "send_hello_frame: no active connection (fd=%d); aborting HELLO",
                    m_client_fds[0U]);
        return Result::ERR_IO;
    }

    // send_frame returns true on success, false on failure.
    bool sent_ok = m_sock_ops->send_frame(m_client_fds[0U], m_wire_buf, wire_len,
                                          m_cfg.channels[0U].send_timeout_ms);
    if (!sent_ok) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "send_hello_frame: send_frame failed");
        return Result::ERR_IO;
    }
    Logger::log(Severity::INFO, "TcpBackend",
                "HELLO sent: local_id=%u", m_local_node_id);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// close_and_evict_slot() — G-3: close a rejected connection and free its slot.
// Calls remove_client_fd(), which closes the socket via m_sock_ops->do_close()
// and compacts the m_client_fds / m_client_node_ids / m_client_hello_received
// arrays.  CALLERS MUST RETURN IMMEDIATELY after this call — all slot indices
// are invalidated by the array compaction.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::close_and_evict_slot(int client_fd)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);                         // Pre-condition: valid fd
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);  // Invariant: count in range

    Logger::log(Severity::WARNING_HI, "TcpBackend",
                "close_and_evict_slot: closing rejected fd=%d", client_fd);
    remove_client_fd(client_fd);

    // Post-condition: count is still in range after compaction.
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);
}

// ─────────────────────────────────────────────────────────────────────────────
// sweep_hello_timeouts()
// REQ-6.1.12 (H-5 / HAZ-023 / CWE-400): evict server slots that have not sent
// a HELLO within hello_timeout_ms (= channels[0].recv_timeout_ms) of acceptance.
// Called at the start of poll_clients_once() (server mode only) before building
// the pollfd set, so timed-out fds are removed before any poll() call.
// Safety-critical (SC): HAZ-023
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::sweep_hello_timeouts()
{
    // Power of 10 Rule 5 — Assert 1: must be server mode (client has no slots to sweep).
    NEVER_COMPILED_OUT_ASSERT(m_is_server);
    // Assert 2: client count in range.
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);

    const uint64_t now_us      = timestamp_now_us();
    // hello_timeout_ms defaults to channels[0].recv_timeout_ms per REQ-6.1.12.
    // CERT INT30-C: recv_timeout_ms <= UINT32_MAX; cast to uint64 before × 1000 is safe.
    const uint64_t timeout_us  =
        static_cast<uint64_t>(m_cfg.channels[0U].recv_timeout_ms) * 1000ULL;

    // Power of 10 Rule 2: bounded by m_client_count ≤ MAX_TCP_CONNECTIONS.
    // Conditional increment: after close_and_evict_slot() the arrays are compacted
    // (element i becomes the former i+1), so we must NOT advance i after eviction.
    for (uint32_t i = 0U; i < m_client_count; ) {
        if (!m_client_hello_received[i] &&
            (m_client_accept_ts[i] != 0ULL) &&
            ((now_us - m_client_accept_ts[i]) > timeout_us)) {
            Logger::log(Severity::WARNING_HI, "TcpBackend",
                        "HELLO timeout: evicting fd=%d (slot %u); "
                        "no HELLO within %u ms (REQ-6.1.12 / HAZ-023)",
                        m_client_fds[i], i,
                        m_cfg.channels[0U].recv_timeout_ms);
            close_and_evict_slot(m_client_fds[i]);
            // close_and_evict_slot compacts arrays — do not increment i.
        } else {
            ++i;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_hello_frame() — REQ-6.1.9: record client NodeId in routing table
// ─────────────────────────────────────────────────────────────────────────────

void TcpBackend::handle_hello_frame(int client_fd, NodeId src_id)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);             // Pre-condition: valid fd
    NEVER_COMPILED_OUT_ASSERT(src_id != NODE_ID_INVALID);  // Pre-condition: valid NodeId

    // F-13 (SECfix-13): reject HELLO if src_id is already registered to a different
    // slot.  Without this check a malicious client can impersonate an already-connected
    // peer by sending a HELLO with that peer's node_id, allowing it to receive traffic
    // addressed to the legitimate peer (HAZ-009 source-id spoofing).
    // G-3: close and evict the impostor's connection so it cannot retry or consume a slot.
    // close_and_evict_slot() compacts the slot arrays; return immediately after.
    // Power of 10 rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    for (uint32_t j = 0U; j < m_client_count; ++j) {
        if ((m_client_node_ids[j] == src_id) && (m_client_fds[j] != client_fd)) {
            Logger::log(Severity::WARNING_HI, "TcpBackend",
                        "HELLO rejected: node_id=%u already registered on slot %u (HAZ-009); evicting",
                        static_cast<unsigned>(src_id), j);
            close_and_evict_slot(client_fd);
            return;  // Slot indices invalidated by compaction; must not continue.
        }
    }

    // Power of 10 rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (m_client_fds[i] == client_fd) {
            m_client_node_ids[i] = src_id;
            Logger::log(Severity::INFO, "TcpBackend",
                        "HELLO from client slot %u node_id=%u", i, src_id);

            // REQ-3.3.6: enqueue src_id so DeliveryEngine can reset stale ordering
            // state for this peer.  A reconnecting peer starts with seq=1; without
            // this notification the ordering buffer would discard all new messages.
            // Power of 10 Rule 2: capacity check guards against queue-full overflow.
            uint32_t next_write = (m_hello_queue_write + 1U) %
                                  static_cast<uint32_t>(MAX_TCP_CONNECTIONS);
            if (next_write != m_hello_queue_read) {
                m_hello_queue[m_hello_queue_write] = src_id;
                m_hello_queue_write = next_write;
            }
            return;
        }
    }
    Logger::log(Severity::WARNING_LO, "TcpBackend",
                "handle_hello_frame: fd not found");
}

// ─────────────────────────────────────────────────────────────────────────────
// TcpBackend::pop_hello_peer() — REQ-3.3.6
// Dequeue one NodeId from the HELLO reconnect queue.  Returns NODE_ID_INVALID
// when the queue is empty.  Called by DeliveryEngine::drain_hello_reconnects().
// Power of 10 Rule 5: 2 assertions. CC <= 3.
// ─────────────────────────────────────────────────────────────────────────────
NodeId TcpBackend::pop_hello_peer()
{
    NEVER_COMPILED_OUT_ASSERT(m_hello_queue_read  < static_cast<uint32_t>(MAX_TCP_CONNECTIONS));  // Assert: valid read index
    NEVER_COMPILED_OUT_ASSERT(m_hello_queue_write < static_cast<uint32_t>(MAX_TCP_CONNECTIONS));  // Assert: valid write index

    if (m_hello_queue_read == m_hello_queue_write) {
        return NODE_ID_INVALID;  // queue empty
    }

    NodeId src = m_hello_queue[m_hello_queue_read];
    m_hello_queue_read = (m_hello_queue_read + 1U) %
                         static_cast<uint32_t>(MAX_TCP_CONNECTIONS);
    return src;
}

// ─────────────────────────────────────────────────────────────────────────────
// is_unregistered_slot() — CC-reduction helper for recv_from_client (Fix 3)
// REQ-6.1.11 / HAZ-009: detect non-HELLO frames from slots that never sent HELLO.
// ─────────────────────────────────────────────────────────────────────────────
//
// Returns true when client_fd is found in the slot table AND its registered
// NodeId is NODE_ID_INVALID (meaning HELLO has not yet been received).
// Returns false when the slot is registered OR when fd is not in the table
// (client-mode path — server never sends HELLO, so server slot has no NodeId).

bool TcpBackend::is_unregistered_slot(int client_fd) const
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);                        // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS); // Invariant

    // Power of 10 rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    for (uint32_t s = 0U; s < m_client_count; ++s) {
        if (m_client_fds[s] == client_fd) {
            return (m_client_node_ids[s] == static_cast<NodeId>(NODE_ID_INVALID));
        }
    }
    // fd not found: client-mode path (server fd has no registered NodeId); allow.
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// process_hello_frame() — REQ-6.1.8: one-HELLO-per-connection enforcement (Fix 4)
// CC-reduction helper for recv_from_client().
// ─────────────────────────────────────────────────────────────────────────────
//
// Looks up the slot for client_fd, rejects a second HELLO from an already-
// registered slot (WARNING_HI + ERR_INVALID), and on first HELLO calls
// handle_hello_frame() and sets m_client_hello_received[slot].
// Returns ERR_AGAIN on success (frame consumed; not forwarded to DeliveryEngine).

Result TcpBackend::process_hello_frame(int client_fd, NodeId src_id)
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);              // Pre-condition: valid fd
    NEVER_COMPILED_OUT_ASSERT(src_id != NODE_ID_INVALID);   // Pre-condition: valid NodeId

    // Find the slot for this fd.
    // Power of 10 rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    uint32_t slot_idx = MAX_TCP_CONNECTIONS;
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (m_client_fds[i] == client_fd) {
            slot_idx = i;
            break;
        }
    }

    if (slot_idx >= MAX_TCP_CONNECTIONS) {
        // fd not found — should never happen if called from recv_from_client
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "process_hello_frame: fd=%d not in slot table", client_fd);
        return Result::ERR_AGAIN;  // consume frame; nothing else we can do
    }

    // REQ-6.1.8 / Fix 4: reject duplicate HELLO from an already-registered slot.
    // G-3: close and evict to prevent resource hold and retry flooding.
    // close_and_evict_slot() compacts the slot arrays; return immediately after.
    if (m_client_hello_received[slot_idx]) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "duplicate HELLO from slot %u fd=%d node_id=%u: evicting (REQ-6.1.8)",
                    static_cast<unsigned>(slot_idx), client_fd,
                    static_cast<unsigned>(src_id));
        close_and_evict_slot(client_fd);
        return Result::ERR_INVALID;  // Slot indices invalidated; must not continue.
    }

    // First HELLO: register NodeId and mark slot as registered.
    handle_hello_frame(client_fd, src_id);
    m_client_hello_received[slot_idx] = true;

    NEVER_COMPILED_OUT_ASSERT(m_client_hello_received[slot_idx]);  // Post-condition
    return Result::ERR_AGAIN;  // consumed; not delivered to DeliveryEngine
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_source_id() — REQ-6.1.11: verify envelope source_id matches slot
// Safety-critical (SC): HAZ-009
// ─────────────────────────────────────────────────────────────────────────────

bool TcpBackend::validate_source_id(int client_fd, NodeId claimed_id) const
{
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS);  // Invariant

    // F-3: use m_client_count (≤ MAX_TCP_CONNECTIONS per invariant above) to avoid
    // scanning uninitialised slots beyond the live table.  The static upper bound
    // visible to analysis tools is MAX_TCP_CONNECTIONS.
    // Power of 10 Rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    NodeId registered = NODE_ID_INVALID;
    bool   found      = false;
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (m_client_fds[i] == client_fd) {
            registered = m_client_node_ids[i];
            found      = true;
            break;
        }
    }

    // fd not found: in server mode this should never occur — is_unregistered_slot()
    // filters unregistered fds before we reach here. Treat as untrusted and reject.
    // In client mode the server fd is always at slot 0, so not-found is impossible
    // during normal operation; allow as a defensive fallback.
    if (!found) {
        if (m_is_server) {
            Logger::log(Severity::WARNING_HI, "TcpBackend",
                        "validate_source_id: fd=%d not in slot table; dropping",
                        client_fd);
            return false;
        }
        return true;  // client-mode defensive fallback (should not occur)
    }

    // NODE_ID_INVALID registered means client-mode path: server never sends HELLO
    // so its slot always carries NODE_ID_INVALID. In server mode this case is
    // blocked upstream by the is_unregistered_slot() guard (F-2 fix).
    if (registered == NODE_ID_INVALID) {
        NEVER_COMPILED_OUT_ASSERT(!m_is_server);  // must only reach here in client mode
        return true;
    }

    if (claimed_id != registered) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "source_id mismatch: claimed=%u registered=%u; dropping (REQ-6.1.11)",
                    static_cast<unsigned>(claimed_id),
                    static_cast<unsigned>(registered));
        NEVER_COMPILED_OUT_ASSERT(claimed_id != registered);  // post: mismatch detected
        return false;
    }

    NEVER_COMPILED_OUT_ASSERT(claimed_id == registered);  // post: match verified
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// find_client_slot() — look up slot index by destination NodeId
// ─────────────────────────────────────────────────────────────────────────────

uint32_t TcpBackend::find_client_slot(NodeId dst) const
{
    NEVER_COMPILED_OUT_ASSERT(dst != NODE_ID_INVALID);               // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS); // Pre-condition

    // Power of 10 rule 2: fixed loop bound (m_client_count ≤ MAX_TCP_CONNECTIONS)
    for (uint32_t i = 0U; i < m_client_count; ++i) {
        if (m_client_node_ids[i] == dst) {
            return i;
        }
    }
    return MAX_TCP_CONNECTIONS;  // not-found sentinel
}

// ─────────────────────────────────────────────────────────────────────────────
// send_to_slot() — send serialized data to one client slot
// ─────────────────────────────────────────────────────────────────────────────

bool TcpBackend::send_to_slot(uint32_t slot, const uint8_t* buf, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(slot < m_client_count);  // Pre-condition: valid slot
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);          // Pre-condition: valid buffer

    bool ok = m_sock_ops->send_frame(m_client_fds[slot], buf, len,
                                     m_cfg.channels[0U].send_timeout_ms);
    if (!ok) {
        Logger::log(Severity::WARNING_HI, "TcpBackend",
                    "send_to_slot %u failed", slot);
    }
    // send_frame returns true on success; invert for "failed" convention
    return !ok;
}
