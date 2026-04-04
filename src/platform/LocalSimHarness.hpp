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
 * @file LocalSimHarness.hpp
 * @brief In-process message transport for deterministic testing.
 *
 * LocalSimHarness allows two instances to be linked together for direct,
 * in-memory message passing without real sockets. Used for unit tests and
 * simulation scenarios where determinism and full control are required.
 *
 * Two harnesses are linked via link(); messages sent from one appear in
 * the other's receive queue after impairment simulation.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §6.3 (Testing, simulation),
 * CLAUDE.md §2.2 (Transport abstraction), CLAUDE.md §4.1 (Core operations).
 *
 * Rules applied:
 *   - Power of 10 rules: no dynamic allocation after init, fixed loop bounds,
 *     ≥2 assertions per function, bounded functions.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result enum returns, event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.3.2, REQ-7.2.4
 */

#ifndef PLATFORM_LOCAL_SIM_HARNESS_HPP
#define PLATFORM_LOCAL_SIM_HARNESS_HPP

#include <cstdint>
#include "core/Assert.hpp"
#include "core/TransportInterface.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"
#include "core/RingBuffer.hpp"
#include "platform/ImpairmentEngine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// LocalSimHarness: In-process simulation transport
// ─────────────────────────────────────────────────────────────────────────────

class LocalSimHarness : public TransportInterface {
public:
    LocalSimHarness();
    ~LocalSimHarness() override;

    // TransportInterface implementation
    Result init(const TransportConfig& config) override;
    // Safety-critical (SC): HAZ-001, HAZ-003, HAZ-005, HAZ-006
    Result send_message(const MessageEnvelope& envelope) override;
    // Safety-critical (SC): HAZ-001, HAZ-004, HAZ-005
    Result receive_message(MessageEnvelope& envelope, uint32_t timeout_ms) override;
    void close() override;
    bool is_open() const override;

    /**
     * @brief Link this harness to a peer harness.
     *
     * Messages sent from this harness will be injected into the peer's
     * receive queue (after impairment simulation).
     *
     * Power of 10: ≥2 assertions.
     *
     * @param[in] peer Pointer to peer LocalSimHarness instance.
     *                 Must not be nullptr and must not be this instance.
     */
    void link(LocalSimHarness* peer);

    /**
     * @brief Inject a message into this harness' receive queue.
     *
     * Called by the linked peer to deliver a message directly.
     * Message must be valid; caller is responsible for validation.
     *
     * Power of 10: ≥2 assertions.
     *
     * @param[in] envelope Message to inject.
     * @return OK on success, ERR_FULL if receive queue is full.
     */
    // Safety-critical (SC): HAZ-006
    Result inject(const MessageEnvelope& envelope);

    /**
     * @brief Populate @p out with connection and impairment statistics.
     * REQ-7.2.4 / REQ-7.2.2 — NSC observability accessor.
     */
    void get_transport_stats(TransportStats& out) const override;

private:
    // ───────────────────────────────────────────────────────────────────────
    // Member state (Power of 10 rule 3: fixed allocation, no heap after init)
    // ───────────────────────────────────────────────────────────────────────
    LocalSimHarness*  m_peer;               ///< Linked peer harness (nullptr if unlinked)
    ImpairmentEngine  m_impairment;         ///< Impairment simulator
    RingBuffer        m_recv_queue;         ///< Inbound message queue
    bool              m_open;              ///< Transport open/closed state
    uint32_t          m_connections_opened; ///< REQ-7.2.4: successful link() events
    uint32_t          m_connections_closed; ///< REQ-7.2.4: close() events while linked

    /// Inject each envelope in @p batch into the peer's receive queue.
    /// Tracks whether the current envelope (matched by source_id + message_id) failed.
    /// @param[in] envelope  The envelope passed to send_message (used for identity match).
    /// @param[in] batch     Array of deliverable envelopes from collect_deliverable().
    /// @param[in] count     Number of entries in @p batch.
    /// @return true if the current envelope's inject failed; false otherwise.
    bool flush_outbound_batch(const MessageEnvelope& envelope,
                              const MessageEnvelope* batch,
                              uint32_t count);
};

#endif // PLATFORM_LOCAL_SIM_HARNESS_HPP
