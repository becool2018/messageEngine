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
 * @file AckTracker.hpp
 * @brief Tracks outstanding messages awaiting ACK, detects timeouts and retries.
 *
 * Requirement traceability: CLAUDE.md §3.2 (ACK handling, retry logic),
 * messageEngine/CLAUDE.md §3.3 (reliable delivery semantics).
 *
 * Rules applied:
 *   - Power of 10: fixed capacity (ACK_TRACKER_CAPACITY), no dynamic allocation,
 *     ≥2 assertions per method, provable loop bounds.
 *   - MISRA C++: no STL, no exceptions, ≤1 ptr indirection.
 *   - F-Prime style: simple state machine (FREE/PENDING/ACKED), deterministic behavior.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2, REQ-7.2.3, REQ-3.2.11
 */

#ifndef CORE_ACK_TRACKER_HPP
#define CORE_ACK_TRACKER_HPP

#include <cstdint>
#include <cstring>
#include "Assert.hpp"
#include "Types.hpp"
#include "MessageEnvelope.hpp"
#include "DeliveryStats.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker: tracks outstanding messages awaiting ACK, timeout management
// ─────────────────────────────────────────────────────────────────────────────

class AckTracker {
public:
    /// Initialize the tracker (zero all state).
    /// Must be called once during system initialization.
    void init();

    // Safety-critical (SC): HAZ-002, HAZ-006 — verified to M5
    /// Add a message to be tracked for ACK receipt.
    /// @param env         [in] message being tracked
    /// @param deadline_us [in] absolute time after which message is considered timed out
    /// @return OK on success; ERR_FULL if ACK_TRACKER_CAPACITY is reached
    Result track(const MessageEnvelope& env, uint64_t deadline_us);

    // Safety-critical (SC): HAZ-002, HAZ-018 — verified to M5
    /// Mark a message as ACKed.
    /// @param src    [in] source (sender) of the message
    /// @param msg_id [in] message ID that was ACKed
    /// @return OK on success; ERR_INVALID if no matching pending message found
    Result on_ack(NodeId src, uint64_t msg_id);

    // Safety-critical (SC): HAZ-002
    /// Cancel a PENDING slot without incrementing acks_received.
    /// Used for rollback when the associated send failed before hitting the wire.
    /// Transitions the slot directly FREE and decrements m_count; no stat bump.
    /// @param src    [in] source node ID of the pending message
    /// @param msg_id [in] message ID of the pending message
    /// @return OK on success; ERR_INVALID if no matching PENDING slot
    Result cancel(NodeId src, uint64_t msg_id);

    // Safety-critical (SC): HAZ-002, HAZ-006 — verified to M5
    /// Remove all expired entries and return them for retry/failure handling.
    /// @param now_us      [in] current time
    /// @param expired_buf [out] buffer to store expired envelopes
    /// @param buf_cap     [in] capacity of expired_buf
    /// @return number of expired entries removed (0 to buf_cap)
    uint32_t sweep_expired(uint64_t         now_us,
                          MessageEnvelope* expired_buf,
                          uint32_t         buf_cap);

    /// Retrieve the send timestamp recorded for a PENDING slot.
    /// Used by DeliveryEngine to compute send→ACK round-trip latency.
    /// @param src      [in]  source node ID (matches env.source_id in the slot)
    /// @param msg_id   [in]  message ID of the tracked message
    /// @param out_ts   [out] populated with env.timestamp_us on success
    /// @return OK if found; ERR_INVALID if no matching PENDING slot
    /// NSC: read-only lookup; no state change.
    Result get_send_timestamp(NodeId src, uint64_t msg_id, uint64_t& out_ts) const;

    // Safety-critical (SC): HAZ-002, HAZ-018 — forge-ACK prevention (F-7)
    /// Look up the destination_id (peer) recorded in a PENDING slot.
    /// Used by DeliveryEngine::process_ack() to verify that an incoming ACK
    /// was sent by the node we actually sent the message to, preventing
    /// ACK forgery by any node that learns our node_id and a pending message_id.
    /// @param our_id   [in]  our local node ID (matches env.source_id in the slot)
    /// @param msg_id   [in]  message ID of the tracked message
    /// @param out_dst  [out] populated with env.destination_id (the expected sender
    ///                        of the ACK) on success
    /// @return OK if a PENDING slot matching (our_id, msg_id) was found;
    ///         ERR_INVALID if no such slot exists.
    /// NSC: read-only lookup; no state change.
    Result get_tracked_destination(NodeId our_id, uint64_t msg_id, NodeId& out_dst) const;

    /// Return a snapshot of ACK-tracker statistics (REQ-7.2.3).
    /// NSC: read-only observability accessor.
    /// Power of 10 rule 5: ≥2 assertions enforced inside.
    const AckTrackerStats& get_stats() const;

private:
    /// Process one slot during sweep_expired.
    /// Copies expired PENDING envelope to buf (if space), releases ACKED slots.
    /// @return 1 if an expired envelope was added to buf, 0 otherwise.
    uint32_t sweep_one_slot(uint32_t         idx,
                            uint64_t         now_us,
                            MessageEnvelope* expired_buf,
                            uint32_t         buf_cap,
                            uint32_t         expired_count);

    // Power of 10 rule 9: ≤1 pointer indirection; using simple fixed array
    /// Entry state machine
    enum class EntryState : uint8_t {
        FREE = 0U,     ///< slot unused
        PENDING = 1U,  ///< message sent, awaiting ACK
        ACKED = 2U     ///< message ACKed; slot will be freed on next sweep
    };

    /// Entry in the tracker
    struct Entry {
        MessageEnvelope env;         // original message being tracked
        uint64_t        deadline_us; // absolute time of ACK timeout
        EntryState      state;       // FREE, PENDING, or ACKED
    };

    // Power of 10 rule 3: fixed-size allocation at static scope
    Entry           m_slots[ACK_TRACKER_CAPACITY] = {};  ///< fixed array of tracker entries
    uint32_t        m_count = 0U;                        ///< number of non-FREE slots currently in use
    AckTrackerStats m_stats = {};                        ///< REQ-7.2.3 observability counters
    bool            m_initialized = false;               ///< false until init() is called; guards against use before init
    uint64_t        m_last_sweep_us = 0U;                ///< Monotonic-time enforcement: timestamp of last sweep_expired call.
};

#endif // CORE_ACK_TRACKER_HPP
