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
 * @file UdpBackend.hpp
 * @brief UDP transport backend implementing TransportInterface.
 *
 * Provides connectionless message transport over UDP/IP.
 * Handles unreliability via impairment simulation and optional
 * application-level retry/ACK logic.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §6.2 (UDP backend),
 * CLAUDE.md §2.2 (Transport abstraction), CLAUDE.md §4.1 (Core operations).
 *
 * Rules applied:
 *   - Power of 10 rules: no dynamic allocation after init, fixed loop bounds,
 *     ≥2 assertions per function, bounded functions.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result enum returns, event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.2.1, REQ-6.2.2, REQ-6.2.3, REQ-6.2.4, REQ-7.1.1
 */

#ifndef PLATFORM_UDP_BACKEND_HPP
#define PLATFORM_UDP_BACKEND_HPP

#include <cstdint>
#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

class ISocketOps;  // forward declaration — see platform/ISocketOps.hpp

// ─────────────────────────────────────────────────────────────────────────────
// UdpBackend: UDP-based transport implementation
// ─────────────────────────────────────────────────────────────────────────────

class UdpBackend : public TransportInterface {
public:
    /// Default constructor — uses the process-wide SocketOpsImpl singleton.
    /// Production code always calls this form.
    UdpBackend();

    /// Injection constructor — accepts any ISocketOps implementation.
    /// Used by tests to inject a mock and exercise error paths that cannot
    /// be triggered in a loopback environment.
    /// @p ops must outlive this UdpBackend instance.
    explicit UdpBackend(ISocketOps& ops);

    ~UdpBackend() override;

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
    ISocketOps*       m_sock_ops;                         ///< Non-owning; never null after ctor
    int               m_fd;                              ///< UDP socket FD
    uint8_t           m_wire_buf[SOCKET_RECV_BUF_BYTES]; ///< Serialization buffer
    TransportConfig   m_cfg;                             ///< Transport configuration
    ImpairmentEngine  m_impairment;                      ///< Impairment simulator
    RingBuffer        m_recv_queue;                      ///< Inbound message queue
    bool              m_open;                            ///< Transport open/closed state

    // ───────────────────────────────────────────────────────────────────────
    // Private helper methods (Power of 10: small, single-purpose functions)
    // ───────────────────────────────────────────────────────────────────────

    /// Attempt to receive one UDP datagram into m_wire_buf, deserialize,
    /// and push the resulting envelope to m_recv_queue.
    /// @param[in] timeout_ms Per-call receive timeout in milliseconds.
    /// @return true if a datagram was received and deserialized successfully.
    bool recv_one_datagram(uint32_t timeout_ms);

    /// Collect deliverable delayed messages from the impairment engine
    /// and push each to m_recv_queue.
    /// @param[in] now_us Current wall-clock time in microseconds.
    void flush_delayed_to_queue(uint64_t now_us);
};

#endif // PLATFORM_UDP_BACKEND_HPP
