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
 * @file UdpBackend.cpp
 * @brief Implementation of UDP transport backend.
 *
 * Implements connectionless message delivery over UDP with impairment
 * simulation support.
 *
 * Rules applied:
 *   - Power of 10: all functions ≤1 page, ≥2 assertions each, fixed loop bounds.
 *   - MISRA C++: no exceptions, all return values checked.
 *   - F-Prime style: event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.1.5, REQ-5.1.6, REQ-6.1.10, REQ-6.2.1, REQ-6.2.2, REQ-6.2.3, REQ-6.2.4, REQ-7.1.1, REQ-7.2.4
 */

#include "platform/UdpBackend.hpp"
#include "platform/ISocketOps.hpp"
#include "platform/SocketOpsImpl.hpp"
#include "platform/SocketUtils.hpp"
#include "core/Assert.hpp"
#include "core/Serializer.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

UdpBackend::UdpBackend()
    : m_sock_ops(&SocketOpsImpl::instance()),
      m_fd(-1), m_wire_buf{}, m_cfg{}, m_open(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_peer_node_id(NODE_ID_INVALID), m_peer_hello_received(false),
      m_local_node_id(NODE_ID_INVALID)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);  // Invariant
}

UdpBackend::UdpBackend(ISocketOps& ops)
    : m_sock_ops(&ops),
      m_fd(-1), m_wire_buf{}, m_cfg{}, m_open(false),
      m_connections_opened(0U), m_connections_closed(0U),
      m_peer_node_id(NODE_ID_INVALID), m_peer_hello_received(false),
      m_local_node_id(NODE_ID_INVALID)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);  // Invariant
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

UdpBackend::~UdpBackend()
{
    // Power of 10: explicit qualified call avoids virtual dispatch in destructor
    UdpBackend::close();
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result UdpBackend::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::UDP);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(!m_open);  // Not already initialized

    // S5: validate config before any channels[] access (REQ-6.1.1, ChannelConfig.hpp).
    if (!transport_config_valid(config)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "init: num_channels=%u exceeds MAX_CHANNELS; rejecting config",
                    config.num_channels);
        return Result::ERR_INVALID;
    }

    m_cfg = config;

    // Create UDP socket
    m_fd = m_sock_ops->create_udp(socket_is_ipv6(config.bind_ip));
    if (m_fd < 0) {
        Logger::log(Severity::FATAL, "UdpBackend", "socket_create_udp failed");
        return Result::ERR_IO;
    }

    // Set SO_REUSEADDR
    if (!m_sock_ops->set_reuseaddr(m_fd)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                   "socket_set_reuseaddr failed");
        m_sock_ops->do_close(m_fd);
        m_fd = -1;
        return Result::ERR_IO;
    }

    // Bind to local address
    if (!m_sock_ops->do_bind(m_fd, config.bind_ip, config.bind_port)) {
        Logger::log(Severity::FATAL, "UdpBackend",
                   "socket_bind failed on port %u", config.bind_port);
        m_sock_ops->do_close(m_fd);
        m_fd = -1;
        return Result::ERR_IO;
    }

    // Initialize receive queue and impairment engine
    m_recv_queue.init();

    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    m_open = true;
    ++m_connections_opened;  // REQ-7.2.4: successful UDP socket bind counts as connection
    Logger::log(Severity::INFO, "UdpBackend",
               "UDP socket bound to %s:%u", config.bind_ip, config.bind_port);

    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);  // Post-condition
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// register_local_id()
// ─────────────────────────────────────────────────────────────────────────────

Result UdpBackend::register_local_id(NodeId id)
{
    NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID);  // pre-condition: valid NodeId
    NEVER_COMPILED_OUT_ASSERT(m_open);  // pre-condition: transport must be initialised
    // REQ-6.1.8 / REQ-6.1.10: store NodeId then send HELLO so the peer can register
    // this side before any DATA frame arrives.  Mirrors TCP client send_hello_frame().
    m_local_node_id = id;
    return send_hello_datagram();
}

// ─────────────────────────────────────────────────────────────────────────────
// send_hello_datagram() — private helper (REQ-6.1.8, REQ-6.1.10)
//
// Serializes a HELLO envelope and sends it directly to the configured peer via
// send_to(), bypassing the impairment engine.  HELLO is a control frame whose
// delivery must not be subject to artificial loss or delay.
// ─────────────────────────────────────────────────────────────────────────────

Result UdpBackend::send_hello_datagram()
{
    NEVER_COMPILED_OUT_ASSERT(m_local_node_id != NODE_ID_INVALID);  // pre: NodeId set
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);                            // pre: socket open

    if (m_cfg.peer_ip[0] == '\0' || m_cfg.peer_port == 0U) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "send_hello_datagram: peer not configured; cannot send HELLO");
        return Result::ERR_INVALID;
    }

    MessageEnvelope hello;
    envelope_init(hello);
    hello.message_type   = MessageType::HELLO;
    hello.source_id      = m_local_node_id;
    hello.destination_id = NODE_ID_INVALID;  // peer NodeId not yet known
    hello.payload_length = 0U;

    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(hello, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "send_hello_datagram: serialize failed");
        return res;
    }

    if (!m_sock_ops->send_to(m_fd, m_wire_buf, wire_len,
                             m_cfg.peer_ip, m_cfg.peer_port)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "send_hello_datagram: send_to %s:%u failed",
                    m_cfg.peer_ip, static_cast<unsigned int>(m_cfg.peer_port));
        return Result::ERR_IO;
    }

    Logger::log(Severity::INFO, "UdpBackend",
                "HELLO sent: local_id=%u", static_cast<unsigned int>(m_local_node_id));
    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // post: something was sent
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_one_envelope() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool UdpBackend::send_one_envelope(const MessageEnvelope& env, bool is_current)
{
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(m_sock_ops != nullptr);

    uint32_t dlen = 0U;
    Result res = Serializer::serialize(env, m_wire_buf, SOCKET_RECV_BUF_BYTES, dlen);
    if (!result_ok(res)) {
        return is_current;  // Serialize failed: attribute to caller only if current
    }

    if (!m_sock_ops->send_to(m_fd, m_wire_buf, dlen,
                             m_cfg.peer_ip, m_cfg.peer_port)) {
        if (!is_current) {
            Logger::log(Severity::WARNING_LO, "UdpBackend",
                        "send_to failed for delayed envelope");
        }
        return is_current;  // Send failed: attribute to caller only if current
    }
    return false;  // Success
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_outbound_batch() — private helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Sends each envelope in @p batch to the configured peer via send_one_envelope().
// Three-case attribution:
//   Current envelope in batch and send fails  → return true (ERR_IO to caller).
//   Current envelope NOT in batch             → return false (queued for later).
//   Non-current envelope send fails           → log WARNING_LO, continue, return false.
// Power of 10: fixed loop bound (count ≤ IMPAIR_DELAY_BUF_SIZE).

bool UdpBackend::flush_outbound_batch(const MessageEnvelope& envelope,
                                       const MessageEnvelope* batch,
                                       uint32_t count)
{
    NEVER_COMPILED_OUT_ASSERT(batch  != nullptr);
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

Result UdpBackend::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));  // Pre-condition

    // Serialize to wire buffer
    uint32_t wire_len = 0U;
    Result res = Serializer::serialize(envelope, m_wire_buf,
                                       SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "UdpBackend", "Serialize failed");
        return res;
    }

    // F-9: enforce UDP datagram MTU to prevent IP fragmentation (REQ-6.2.3).
    // DtlsUdpBackend enforces this at line 1114; plaintext UdpBackend must match.
    if (wire_len > DTLS_MAX_DATAGRAM_BYTES) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "send_message: serialized len %u exceeds UDP MTU %u; rejected",
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

    // process_outbound() already queued the message into the delay buffer
    // (release_us = now_us when latency is 0). collect_deliverable() returns
    // all messages whose release_us <= now_us, covering both the zero-delay
    // pass-through and the timed-delay cases. Do NOT also call send_to() for
    // `envelope` directly — that causes every message to be sent twice. (HAZ-003)
    // flush_outbound_batch() handles the three-case attribution (see its comment).
    MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                                              delayed_envelopes,
                                                              IMPAIR_DELAY_BUF_SIZE);

    if (flush_outbound_batch(envelope, delayed_envelopes, delayed_count)) {
        return Result::ERR_IO;
    }

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// validate_source() — private helper (REQ-6.2.4)
//
// Compares src_ip/src_port returned by recv_from() against the configured peer.
// Returns true when the source matches; returns false and logs WARNING_LO when
// it does not.  Extracted as a helper to keep recv_one_datagram CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────

bool UdpBackend::validate_source(const char* src_ip, uint16_t src_port) const
{
    NEVER_COMPILED_OUT_ASSERT(src_ip != nullptr);                    // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_cfg.peer_ip[0] != '\0');            // Pre-condition

    // REQ-6.2.4: validate source address against configured peer.
    // CERT: use strcmp() not strncmp(…, sizeof(buf)) — strncmp reads up to N bytes past
    // each string's NUL terminator when the string is shorter than N, reaching into
    // uninitialised buffer memory and breaking spoofing detection.  Both src_ip and
    // m_cfg.peer_ip are NUL-terminated by construction (inet_ntop / config parser).
    bool ip_match   = (strcmp(src_ip, m_cfg.peer_ip) == 0);
    bool port_match = (src_port == m_cfg.peer_port);

    if (!ip_match || !port_match) {
        // REQ-6.3.2: source spoofing is a security event; classify as WARNING_HI.
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "Dropped datagram from unexpected source %s:%u (expected %s:%u)",
                    src_ip, static_cast<unsigned int>(src_port),
                    m_cfg.peer_ip, static_cast<unsigned int>(m_cfg.peer_port));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// process_hello_or_validate() — private helper (REQ-6.1.8, REQ-6.2.4)
//
// Enforces HELLO-before-data and source_id binding for the plaintext UDP path,
// mirroring DtlsUdpBackend::process_hello_or_validate() exactly.
//
// HELLO frame rules (REQ-6.1.8):
//   - First HELLO from peer: store source_id in m_peer_node_id, set flag.
//   - Duplicate HELLO: log WARNING_HI, drop.
//
// Data frame rules (REQ-6.2.4):
//   - No HELLO received yet: log WARNING_HI, drop (no backward-compat bypass).
//   - HELLO received: if source_id != m_peer_node_id, log WARNING_HI, drop.
// ─────────────────────────────────────────────────────────────────────────────

bool UdpBackend::process_hello_or_validate(const MessageEnvelope& env,
                                            bool& consumed)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);      // pre: transport must be open
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);   // pre: socket valid

    consumed = false;

    if (env.message_type == MessageType::HELLO) {
        if (m_peer_hello_received) {
            // REQ-6.1.8: only one HELLO per connection allowed
            Logger::log(Severity::WARNING_HI, "UdpBackend",
                        "process_hello_or_validate: duplicate HELLO from NodeId=%u; dropping",
                        static_cast<unsigned int>(env.source_id));
            return false;
        }
        // First HELLO: register the peer NodeId
        m_peer_node_id       = env.source_id;
        m_peer_hello_received = true;
        Logger::log(Severity::INFO, "UdpBackend",
                    "UDP: HELLO received, peer NodeId %u registered",
                    static_cast<unsigned int>(m_peer_node_id));
        consumed = true;  // HELLO consumed; must not reach DeliveryEngine
        return true;
    }

    // REQ-6.2.4 / REQ-6.1.8: HELLO is mandatory before any data frame.
    // No backward-compat bypass: a peer that never sends HELLO could inject
    // arbitrary source_id values, corrupting ordering state and exhausting the
    // dedup window.
    if (!m_peer_hello_received) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "process_hello_or_validate: data frame before HELLO; "
                    "dropping (source_id=%u)",
                    static_cast<unsigned int>(env.source_id));
        return false;
    }

    // source_id must match the NodeId registered in the HELLO.
    if (env.source_id != m_peer_node_id) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                    "process_hello_or_validate: source_id %u != registered %u; "
                    "spoofing attempt \xe2\x80\x94 dropping (REQ-6.2.4)",
                    static_cast<unsigned int>(env.source_id),
                    static_cast<unsigned int>(m_peer_node_id));
        return false;
    }

    return true;  // source_id matches registered peer NodeId
}

// ─────────────────────────────────────────────────────────────────────────────
// recv_one_datagram() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool UdpBackend::recv_one_datagram(uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);             // Power of 10: transport must be open
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);          // Power of 10: valid socket

    uint32_t out_len = 0U;
    char     src_ip[48];
    uint16_t src_port = 0U;

    if (!m_sock_ops->recv_from(m_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                               timeout_ms, &out_len, src_ip, &src_port)) {
        return false;  // Timeout or error on this poll
    }

    // REQ-6.2.4: validate source before deserialization (drop spoofed datagrams)
    if (!validate_source(src_ip, src_port)) {
        return false;
    }

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "UdpBackend",
                   "Deserialize failed: %u", static_cast<uint8_t>(res));
        return false;
    }

    // REQ-6.1.8 / REQ-6.2.4: enforce HELLO-before-data and source_id binding.
    bool consumed = false;
    if (!process_hello_or_validate(env, consumed)) {
        return false;  // frame dropped (data-before-HELLO or source_id mismatch)
    }
    if (consumed) {
        return false;  // HELLO consumed; must not reach DeliveryEngine
    }

    // REQ-5.1.5, REQ-5.1.6: apply inbound impairment (partition drop / reorder)
    MessageEnvelope inbound_out[1U];
    uint32_t        inbound_count = 0U;
    uint64_t        now_us = timestamp_now_us();
    res = m_impairment.process_inbound(env, now_us, inbound_out, 1U, inbound_count);

    if (res == Result::ERR_IO) {
        // Partition dropped the message; do not queue
        return false;
    }

    if (inbound_count == 0U) {
        // Reorder engine buffered the message; nothing to push yet
        return false;
    }

    NEVER_COMPILED_OUT_ASSERT(inbound_count == 1U);  // Power of 10: exactly one output
    res = m_recv_queue.push(inbound_out[0]);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                   "Recv queue full; dropping datagram from %s:%u",
                   src_ip, src_port);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_wire() — private helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Collects outbound envelopes whose impairment-delay timer has expired and
// sends each one to the wire via send_one_envelope().  Failures are logged
// by send_one_envelope() but not propagated (is_current = false).

void UdpBackend::flush_delayed_to_wire(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);      // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(m_open);             // Power of 10: transport must be open

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    // Power of 10 rule 2: fixed loop bound
    // Send delayed outbound envelopes to the wire via send_one_envelope().
    // L-5: Power of 10 Rule 7 — store return value; log on failure.
    //      send_one_envelope() already logs WARNING_LO internally for non-current
    //      failures, but we must not discard the return value (Rule 7).
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        const bool send_failed = send_one_envelope(delayed[i], false);
        if (send_failed) {
            Logger::log(Severity::WARNING_HI, "UdpBackend",
                        "flush_delayed_to_wire: send_one_envelope failed for slot %u", i);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result UdpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);    // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0); // Pre-condition

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
        (void)recv_one_datagram(100U);

        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }

        uint64_t now_us = timestamp_now_us();
        flush_delayed_to_wire(now_us);

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

void UdpBackend::close()
{
    if (m_fd >= 0) {
        m_sock_ops->do_close(m_fd);
        m_fd = -1;
        ++m_connections_closed;  // REQ-7.2.4: socket close event
    }

    // Reset peer registration so a subsequent init() + HELLO starts clean.
    m_peer_node_id        = NODE_ID_INVALID;
    m_peer_hello_received = false;

    m_open = false;
    Logger::log(Severity::INFO, "UdpBackend", "Transport closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool UdpBackend::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open == (m_fd >= 0) || !m_open);  // Invariant
    return m_open;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_transport_stats() — REQ-7.2.4 / REQ-7.2.2 observability
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

void UdpBackend::get_transport_stats(TransportStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened >= m_connections_closed);  // Assert: monotonic counters
    NEVER_COMPILED_OUT_ASSERT(m_connections_closed <= m_connections_opened);  // Assert: closed ≤ opened

    transport_stats_init(out);
    out.connections_opened = m_connections_opened;
    out.connections_closed = m_connections_closed;
    out.impairment         = m_impairment.get_stats();
}
