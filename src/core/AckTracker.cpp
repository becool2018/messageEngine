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
 * @file AckTracker.cpp
 * @brief Implementation of ACK tracking, expiry detection, and retry support.
 *
 * Requirement traceability: CLAUDE.md §3.2 (ACK handling),
 * messageEngine/CLAUDE.md §3.3 (reliable delivery with retry).
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds (ACK_TRACKER_CAPACITY), ≥2 assertions per method.
 *   - MISRA C++: no dynamic allocation, no exceptions.
 *   - F-Prime style: simple state machine (FREE/PENDING/ACKED), deterministic behavior.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2, REQ-7.2.3
 */

#include "AckTracker.hpp"
#include "Assert.hpp"
#include "Logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────────────────────

void AckTracker::init()
{
    // Power of 10 rule 3: zero-initialize fixed buffer during init phase
    (void)memset(m_slots, 0, sizeof(m_slots));

    m_count = 0U;
    m_initialized = true;
    m_last_sweep_us = 0U;
    ack_tracker_stats_init(m_stats);  // REQ-7.2.3: zero all observability counters

    // Power of 10 rule 5: post-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_count == 0U);  // Assert: tracker is empty
    NEVER_COMPILED_OUT_ASSERT(m_stats.timeouts == 0U);  // Assert: stats zeroed

    // Verify all slots are FREE
    // Power of 10 rule 3: bounded loop (ACK_TRACKER_CAPACITY)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::FREE);  // Assert: all slots free after init
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Track a new message
// ─────────────────────────────────────────────────────────────────────────────

Result AckTracker::track(const MessageEnvelope& env, uint64_t deadline_us)
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count is consistent

    // Power of 10 rule 2: bounded search (fixed loop, ACK_TRACKER_CAPACITY)
    // Find the first FREE slot
    // Power of 10 rule 3: loop bound is provable constant
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (m_slots[i].state == EntryState::FREE) {
            // Found free slot; fill it
            envelope_copy(m_slots[i].env, env);
            m_slots[i].deadline_us = deadline_us;
            m_slots[i].state = EntryState::PENDING;

            // Increment count
            m_count++;

            // Power of 10 rule 5: post-condition assertions
            NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::PENDING);  // Assert: slot is pending
            NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);          // Assert: count is valid

            return Result::OK;
        }
    }

    // Power of 10 rule 5: no free slot assertion
    NEVER_COMPILED_OUT_ASSERT(m_count == ACK_TRACKER_CAPACITY);  // Assert: tracker is full

    return Result::ERR_FULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Mark a message as ACKed
// ─────────────────────────────────────────────────────────────────────────────

Result AckTracker::on_ack(NodeId src, uint64_t msg_id)
{
    // Power of 10 rule 5: pre-condition assertion
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count is consistent

    // Power of 10 rule 2: bounded search (fixed loop, ACK_TRACKER_CAPACITY)
    // Find matching PENDING entry
    // Power of 10 rule 3: loop bound is provable constant
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if ((m_slots[i].state == EntryState::PENDING) &&
            (m_slots[i].env.source_id == src) &&
            (m_slots[i].env.message_id == msg_id)) {
            // Found the matching entry; mark as ACKED
            m_slots[i].state = EntryState::ACKED;
            ++m_stats.acks_received;  // REQ-7.2.3: record PENDING→ACKED transition

            // Power of 10 rule 5: post-condition assertion
            NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::ACKED);  // Assert: slot is ACKed

            return Result::OK;
        }
    }

    // Power of 10 rule 5: not found assertion
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count is still valid

    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker::cancel() — rollback a PENDING slot without a phantom ACK stat.
// Transitions PENDING→FREE directly; does not increment acks_received.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────

Result AckTracker::cancel(NodeId src, uint64_t msg_id)
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count is consistent
    NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID);           // Assert: valid source

    // Power of 10 rule 2: bounded search (fixed loop, ACK_TRACKER_CAPACITY)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if ((m_slots[i].state == EntryState::PENDING) &&
            (m_slots[i].env.source_id == src) &&
            (m_slots[i].env.message_id == msg_id)) {
            // Release slot directly to FREE; do NOT increment acks_received.
            // This is a rollback path: the message was never put on the wire,
            // so no phantom ACK stat should be recorded.
            m_slots[i].state = EntryState::FREE;
            if (m_count > 0U) { --m_count; }

            // Power of 10 rule 5: post-condition assertions
            NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::FREE);  // Assert: slot freed
            NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);       // Assert: count valid

            return Result::OK;
        }
    }

    // Power of 10 rule 5: not-found assertion
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count still valid

    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// sweep_one_slot() — private helper (Power of 10: single-purpose, ≤1 page)
// ─────────────────────────────────────────────────────────────────────────────

uint32_t AckTracker::sweep_one_slot(uint32_t         idx,
                                    uint64_t         now_us,
                                    MessageEnvelope* expired_buf,
                                    uint32_t         buf_cap,
                                    uint32_t         expired_count)
{
    NEVER_COMPILED_OUT_ASSERT(idx < ACK_TRACKER_CAPACITY);   // Power of 10: bounds
    NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr);       // Power of 10: valid output buffer

    if (m_slots[idx].state == EntryState::PENDING &&
        now_us >= m_slots[idx].deadline_us) {
        // Expired PENDING: copy to output buffer if space available, then release
        uint32_t added = 0U;
        if (expired_count < buf_cap) {
            envelope_copy(expired_buf[expired_count], m_slots[idx].env);
            added = 1U;
        }
        ++m_stats.timeouts;  // REQ-7.2.3: record ACK timeout event
        m_slots[idx].state = EntryState::FREE;
        if (m_count > 0U) { --m_count; }
        return added;
    }

    if (m_slots[idx].state == EntryState::ACKED) {
        // ACKED: release immediately; nothing to return to caller
        m_slots[idx].state = EntryState::FREE;
        if (m_count > 0U) { --m_count; }
    }

    return 0U;  // FREE slots and non-expired PENDING require no action
}

// ─────────────────────────────────────────────────────────────────────────────
// sweep_expired()
// ─────────────────────────────────────────────────────────────────────────────

uint32_t AckTracker::sweep_expired(uint64_t         now_us,
                                   MessageEnvelope* expired_buf,
                                   uint32_t         buf_cap)
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr);               // Assert: output buffer is valid
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);      // Assert: count is consistent

    // G-1: monotonic-time contract — now_us must be non-decreasing (CLOCK_MONOTONIC).
    // A backward timestamp causes entries to never expire, permanently leaking slots.
    // Replaced NEVER_COMPILED_OUT_ASSERT with a defensive clamp + WARNING_HI so that
    // a clock glitch (NTP step, test harness, wrap) degrades gracefully rather than
    // triggering an abort/reset in a server process.
    NEVER_COMPILED_OUT_ASSERT(now_us != 0ULL);  // Assert: zero timestamp is always invalid
    if (now_us < m_last_sweep_us) {
        Logger::log(Severity::WARNING_HI, "AckTracker",
                    "sweep_expired: non-monotonic timestamp (now=%llu < last=%llu); clamping",
                    static_cast<unsigned long long>(now_us),
                    static_cast<unsigned long long>(m_last_sweep_us));
        now_us = m_last_sweep_us;
    }
    m_last_sweep_us = now_us;

    uint32_t expired_count = 0U;

    // Power of 10 rule 2: bounded loop (ACK_TRACKER_CAPACITY)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        expired_count += sweep_one_slot(i, now_us, expired_buf, buf_cap, expired_count);
    }

    // Power of 10 rule 5: post-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);          // Assert: count is still valid
    NEVER_COMPILED_OUT_ASSERT(expired_count <= buf_cap);                 // Assert: didn't exceed output capacity

    return expired_count;
}

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker::get_send_timestamp() — REQ-7.2.1 latency helper
// NSC: read-only lookup; no state change.
// ─────────────────────────────────────────────────────────────────────────────

Result AckTracker::get_send_timestamp(NodeId src, uint64_t msg_id, uint64_t& out_ts) const
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID);         // Assert: valid source
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);// Assert: count consistent

    // Power of 10 rule 2: bounded search
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if ((m_slots[i].state == EntryState::PENDING) &&
            (m_slots[i].env.source_id == src) &&
            (m_slots[i].env.message_id == msg_id)) {
            out_ts = m_slots[i].env.timestamp_us;
            NEVER_COMPILED_OUT_ASSERT(out_ts > 0ULL);  // Assert: send timestamp recorded
            return Result::OK;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count still valid
    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker::get_tracked_destination() — F-7 forge-ACK prevention helper
// NSC: read-only lookup; no state change.
// ─────────────────────────────────────────────────────────────────────────────

Result AckTracker::get_tracked_destination(NodeId our_id, uint64_t msg_id, NodeId& out_dst) const
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(our_id != NODE_ID_INVALID);         // Assert: valid local id
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);   // Assert: count consistent

    // Power of 10 rule 2: bounded search
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if ((m_slots[i].state == EntryState::PENDING) &&
            (m_slots[i].env.source_id == our_id) &&
            (m_slots[i].env.message_id == msg_id)) {
            out_dst = m_slots[i].env.destination_id;
            NEVER_COMPILED_OUT_ASSERT(out_dst != NODE_ID_INVALID);  // Assert: valid destination
            return Result::OK;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count still valid
    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// AckTracker::get_stats() — REQ-7.2.3 observability accessor
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

const AckTrackerStats& AckTracker::get_stats() const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: slot count never exceeds capacity
    // Second assertion: enforce the precondition that init() was called before get_stats().
    // m_initialized is set true by init() and never reset; asserting it here guarantees
    // get_stats() is not called on an uninitialized tracker.
    //
    // NOTE: any assertion comparing monotonic counters (acks_received, timeouts) against
    // a capacity-derived bound is a time-bomb. Both counters grow without bound across
    // rounds; a bound of the form "timeouts <= acks_received + ACK_TRACKER_CAPACITY" is
    // violated after exactly 2 × ACK_TRACKER_CAPACITY timeouts with zero ACKs and is
    // therefore invalid as a permanent invariant. No counter-vs-capacity bound belongs here.
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: init() has been called before get_stats()
    return m_stats;
}
