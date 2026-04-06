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
 * @file OrderingBuffer.hpp
 * @brief Per-peer in-order delivery gate for ORDERED DATA messages.
 *
 * OrderingBuffer maintains:
 *   - Per-peer sequence tracking (next expected sequence number per source).
 *   - A bounded hold buffer for out-of-order messages pending the missing gap.
 *
 * Bypass rules (messages delivered immediately without entering the ordering gate):
 *   - Control messages (MessageType != DATA): always bypass.
 *   - sequence_num == 0: treated as UNORDERED; always bypass.
 *
 * For ORDERED DATA messages (sequence_num > 0):
 *   - If sequence_num == next_expected_for(src): deliver and advance next_expected.
 *   - If sequence_num > next_expected_for(src): hold in m_hold (ERR_AGAIN returned).
 *   - If sequence_num < next_expected_for(src): discard as duplicate (ERR_AGAIN).
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: no templates, no STL, no exceptions.
 *   - F-Prime style: explicit error codes, simple state.
 *
 * State machine per (src, sequence) slot:
 *   FREE → HELD (on ERR_AGAIN from ingest())
 *   HELD → DELIVERED (on try_release_next() when sequence matches)
 *   DELIVERED → FREE
 *
 * Implements: REQ-3.3.5
 */
// Implements: REQ-3.3.5

#ifndef CORE_ORDERING_BUFFER_HPP
#define CORE_ORDERING_BUFFER_HPP

#include <cstdint>
#include "Types.hpp"
#include "MessageEnvelope.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer
// ─────────────────────────────────────────────────────────────────────────────

class OrderingBuffer {
public:
    /// Must be called once during system initialization.
    /// @param local_node  The local node ID (used to identify self in logging).
    void init(NodeId local_node);

    // Safety-critical (SC): HAZ-001 — ordering gate determines delivery sequence.
    /// Ingest one fully-reassembled logical message.
    ///
    /// @param msg    [in]  Logical envelope (post-reassembly).
    /// @param out    [out] Filled with msg if it is next in sequence.
    /// @param now_us [in]  Current time in microseconds (for expiry check of held msgs).
    /// @return OK      if the message is in order and ready for delivery (out is valid).
    ///         ERR_AGAIN if the message was held (out-of-order) or discarded (duplicate).
    ///         ERR_FULL  if the hold buffer is exhausted.
    Result ingest(const MessageEnvelope& msg, MessageEnvelope& out, uint64_t now_us);

    /// Release the next in-sequence message from the hold buffer for a given src.
    /// Called after ingest() returns OK to drain any subsequently-held messages
    /// that are now contiguous.
    ///
    /// @param src [in]  Source node whose held messages to check.
    /// @param out [out] Filled with the next in-sequence message on OK.
    /// @return OK      if a held message matched and is ready for delivery.
    ///         ERR_AGAIN if no held message matches next_expected for this src.
    Result try_release_next(NodeId src, MessageEnvelope& out);

    /// Advance the expected sequence for @p src past @p up_to_seq (exclusive).
    /// Used to skip a gap after a timeout so ordering does not stall indefinitely.
    void advance_sequence(NodeId src, uint32_t up_to_seq);

    /// Sweep all held slots: for any held message whose expiry_time_us has passed,
    /// call advance_sequence() past that slot's sequence number so the ordering
    /// gate does not stall forever on a permanently lost gap.
    /// Call periodically (e.g., from DeliveryEngine::sweep_ack_timeouts()).
    ///
    /// @param now_us   Current time in microseconds (used to check expiry).
    /// @param out_freed Caller-supplied buffer to receive freed envelopes
    ///                  (for stats and event emission by the caller).
    ///                  Must point to an array of at least out_cap elements.
    /// @param out_cap  Capacity of out_freed (use ORDERING_HOLD_COUNT or larger).
    /// @return Number of expired hold slots released (written to out_freed[0..n-1]).
    /// Power of 10 Rule 2: bounded loop (≤ ORDERING_HOLD_COUNT iterations).
    // Safety-critical (SC): HAZ-001 — prevents permanent ordering stall on gap loss.
    uint32_t sweep_expired_holds(uint64_t         now_us,
                                  MessageEnvelope* out_freed,
                                  uint32_t         out_cap);

private:
    // ─────────────────────────────────────────────────────────────────────────
    // HoldSlot: one queued out-of-order message
    // ─────────────────────────────────────────────────────────────────────────
    struct HoldSlot {
        MessageEnvelope env;
        bool            active;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // PeerState: per-source sequence tracking
    // ─────────────────────────────────────────────────────────────────────────
    struct PeerState {
        NodeId   src;
        uint32_t next_expected_seq;
        bool     active;
    };

    HoldSlot  m_hold[ORDERING_HOLD_COUNT] = {};
    // Peer table sized to ORDERING_PEER_COUNT (= MAX_PEER_NODES = 16) so all
    // concurrent ordered-channel peers can be tracked without hitting ERR_FULL.
    PeerState m_peers[ORDERING_PEER_COUNT] = {};
    bool      m_initialized = false;
    NodeId    m_local_node  = 0U;

    // Private helpers — extracted to keep ingest() CC ≤ 10

    // Find or create a PeerState for src. Returns REASSEMBLY_SLOT_COUNT on failure
    // (no free peer slot).
    uint32_t get_or_create_peer(NodeId src);

    // Find existing peer state for src. Returns REASSEMBLY_SLOT_COUNT if not found.
    uint32_t find_peer(NodeId src) const;

    // Find a free hold slot. Returns ORDERING_HOLD_COUNT if all are in use.
    uint32_t find_free_hold() const;

    // Find a held message for (src, seq). Returns ORDERING_HOLD_COUNT if not found.
    uint32_t find_held(NodeId src, uint32_t seq) const;

    /// Advance next_expected_seq for peer at peer_idx by one, guarding against u32 wraparound.
    /// On wraparound (0xFFFFFFFF -> 0), logs WARNING_HI and resets to 1U.
    /// Power of 10 Rule 5: 2 assertions. CC <= 10.
    void advance_next_expected(uint32_t peer_idx);

    /// Return seq + 1, guarded against uint32_t wraparound.
    /// CERT INT30-C: if seq == UINT32_MAX, returns 1 (matching advance_next_expected()
    /// wraparound policy) and logs WARNING_HI. Never returns 0 (the UNORDERED sentinel).
    /// Power of 10 Rule 5: 2 assertions. CC <= 10.
    static uint32_t seq_next_guarded(uint32_t seq);
};

#endif // CORE_ORDERING_BUFFER_HPP
