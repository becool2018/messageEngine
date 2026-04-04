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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.2.1, REQ-6.2.2, REQ-6.2.3, REQ-6.2.4, REQ-7.1.1
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
      m_fd(-1), m_wire_buf{}, m_cfg{}, m_open(false)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(SOCKET_RECV_BUF_BYTES > 0U);  // Invariant
}

UdpBackend::UdpBackend(ISocketOps& ops)
    : m_sock_ops(&ops),
      m_fd(-1), m_wire_buf{}, m_cfg{}, m_open(false)
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
    Logger::log(Severity::INFO, "UdpBackend",
               "UDP socket bound to %s:%u", config.bind_ip, config.bind_port);

    NEVER_COMPILED_OUT_ASSERT(m_fd >= 0);  // Post-condition
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Post-condition
    return Result::OK;
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
    // Flush errors from older delayed messages are not attributed to the current
    // send — the current envelope may have a future release_us and remain queued.
    // Power of 10: fixed loop bound (IMPAIR_DELAY_BUF_SIZE compile-time constant).
    MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                                              delayed_envelopes,
                                                              IMPAIR_DELAY_BUF_SIZE);

    for (uint32_t i = 0U; i < delayed_count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);

        uint32_t delayed_len = 0U;
        res = Serializer::serialize(delayed_envelopes[i], m_wire_buf,
                                    SOCKET_RECV_BUF_BYTES, delayed_len);
        if (!result_ok(res)) {
            continue;
        }

        if (!m_sock_ops->send_to(m_fd, m_wire_buf, delayed_len,
                                 m_cfg.peer_ip, m_cfg.peer_port)) {
            Logger::log(Severity::WARNING_LO, "UdpBackend",
                        "send_to failed for delayed envelope at index %u", i);
        }
    }

    NEVER_COMPILED_OUT_ASSERT(wire_len > 0U);  // Post-condition
    return Result::OK;
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

    MessageEnvelope env;
    Result res = Serializer::deserialize(m_wire_buf, out_len, env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "UdpBackend",
                   "Deserialize failed: %u", static_cast<uint8_t>(res));
        return false;
    }

    res = m_recv_queue.push(env);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "UdpBackend",
                   "Recv queue full; dropping datagram from %s:%u",
                   src_ip, src_port);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_delayed_to_queue() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void UdpBackend::flush_delayed_to_queue(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);      // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(m_open);             // Power of 10: transport must be open

    MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE];
    uint32_t count = m_impairment.collect_deliverable(now_us, delayed,
                                                      IMPAIR_DELAY_BUF_SIZE);

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)m_recv_queue.push(delayed[i]);
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
        flush_delayed_to_queue(now_us);

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
    }

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
