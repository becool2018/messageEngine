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
 * @file TransportInterface.hpp
 * @brief Abstract transport interface exposed to the core / app layers.
 *
 * Requirement traceability: CLAUDE.md §4.1 (Core operations).
 *
 * Rules applied:
 *   - Power of 10 rule 9: no function pointers; virtual dispatch via vtable.
 *   - F-Prime style: no STL, no exceptions, no templates.
 *   - Architecture rule: higher layers depend on this abstraction, never on
 *     raw socket APIs.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.1.10, REQ-7.2.4
 */

#ifndef CORE_TRANSPORT_INTERFACE_HPP
#define CORE_TRANSPORT_INTERFACE_HPP

#include "MessageEnvelope.hpp"
#include "ChannelConfig.hpp"
#include "Types.hpp"
#include "DeliveryStats.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// TransportInterface
//
// Pure-virtual interface.  Platform backends (TcpBackend, UdpBackend,
// LocalSimHarness) implement this.  The DeliveryEngine and app layer call it.
// ─────────────────────────────────────────────────────────────────────────────
class TransportInterface {
public:
    virtual ~TransportInterface() {}

    /**
     * @brief Initialise transport with given configuration.
     * @return OK on success, otherwise an error code.
     *
     * All resource allocation (sockets, buffers) happens here.
     * No allocation occurs on critical paths after init() returns OK.
     * (Power of 10 rule 3)
     */
    virtual Result init(const TransportConfig& config) = 0;

    /**
     * @brief Notify the transport of this endpoint's local NodeId.
     *
     * Called by DeliveryEngine::init() immediately after init() returns OK.
     * TCP/TLS client backends use this to send the on-connect HELLO registration
     * frame (REQ-6.1.8, REQ-6.1.10). TCP/TLS server backends store the local
     * NodeId for reference. All other transports return OK without action.
     *
     * @param[in] id  Local NodeId; must not be NODE_ID_INVALID.
     * @return OK on success; ERR_IO if the HELLO frame could not be sent.
     */
    // NSC: registration is a lifecycle call, not on the message delivery path.
    virtual Result register_local_id(NodeId id)
    {
        (void)id;       // default: no-op for non-TCP transports
        return Result::OK;
    }

    /**
     * @brief Pop the NodeId of the most-recently-registered peer (HELLO received).
     *
     * Called by DeliveryEngine::receive() to detect peer reconnections and reset
     * stale per-peer ordering state (REQ-3.3.6). Returns NODE_ID_INVALID when no
     * new HELLO has been received since the last call.
     *
     * TCP/TLS server backends push a NodeId each time handle_hello_frame()
     * successfully registers a client NodeId. UDP/DTLS drop duplicate HELLOs so
     * reconnect semantics do not apply; those backends use the default no-op.
     *
     * @return NodeId of peer that sent HELLO; NODE_ID_INVALID (0) if none pending.
     */
    // NSC: lifecycle notification; not on the message delivery path.
    virtual NodeId pop_hello_peer()
    {
        return NODE_ID_INVALID;  // default: no-op for UDP/DTLS and non-connection-oriented transports
    }

    // Safety-critical (SC): HAZ-001, HAZ-005, HAZ-006 — verified to M5
    /**
     * @brief Queue / send a single message envelope.
     * @return OK if accepted; ERR_FULL if channel queue is full;
     *         ERR_IO on socket/send failure.
     */
    virtual Result send_message(const MessageEnvelope& envelope) = 0;

    // Safety-critical (SC): HAZ-001, HAZ-004, HAZ-005 — verified to M5
    /**
     * @brief Block until a message is received or @p timeout_ms elapses.
     * @param[out] envelope  Populated on success.
     * @param[in]  timeout_ms  0 = poll (non-blocking).
     * @return OK on message received; ERR_TIMEOUT if no message arrived.
     */
    virtual Result receive_message(MessageEnvelope& envelope,
                                   uint32_t         timeout_ms) = 0;

    /**
     * @brief Flush pending outbound messages and close all resources.
     *
     * After close(), init() may be called again to reinitialise.
     */
    virtual void close() = 0;

    /**
     * @brief Return true if the transport is currently open and usable.
     */
    virtual bool is_open() const = 0;

    /**
     * @brief Populate @p out with connection and impairment statistics.
     *
     * REQ-7.2.4 (connection counts) and REQ-7.2.2 (impairment counters).
     * NSC: read-only observability; no effect on transport state.
     * Implementations zero fields they do not track.
     *
     * @param[out] out TransportStats struct to fill.
     */
    virtual void get_transport_stats(TransportStats& out) const = 0;
};

#endif // CORE_TRANSPORT_INTERFACE_HPP
