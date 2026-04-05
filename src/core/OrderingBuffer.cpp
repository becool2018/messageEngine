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
 * @file OrderingBuffer.cpp
 * @brief Implementation of per-peer in-order delivery gate.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds (ORDERING_HOLD_COUNT, REASSEMBLY_SLOT_COUNT),
 *     no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: checked casts, no UB.
 *   - F-Prime style: explicit error codes.
 *
 * Implements: REQ-3.3.5
 */
// Implements: REQ-3.3.5

#include "OrderingBuffer.hpp"
#include "Assert.hpp"
#include "Logger.hpp"
#include "Timestamp.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::init
// ─────────────────────────────────────────────────────────────────────────────
void OrderingBuffer::init(NodeId local_node)
{
    NEVER_COMPILED_OUT_ASSERT(ORDERING_HOLD_COUNT > 0U);     // Assert: capacity constant valid
    NEVER_COMPILED_OUT_ASSERT(REASSEMBLY_SLOT_COUNT > 0U);   // Assert: peer capacity valid

    // Power of 10 Rule 2: bounded loops
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        m_hold[i].active = false;
        envelope_init(m_hold[i].env);
    }

    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        m_peers[i].active           = false;
        m_peers[i].src              = 0U;
        m_peers[i].next_expected_seq = 1U;  // sequences start at 1
    }

    m_initialized = true;
    m_local_node  = local_node;

    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: init complete
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::find_peer (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::find_peer(NodeId src) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);        // Assert: valid source

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (m_peers[i].active && m_peers[i].src == src) {
            return i;
        }
    }

    return REASSEMBLY_SLOT_COUNT;  // not found
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::get_or_create_peer (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::get_or_create_peer(NodeId src)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);        // Assert: valid source

    uint32_t idx = find_peer(src);
    if (idx != REASSEMBLY_SLOT_COUNT) {
        return idx;  // already tracked
    }

    // Allocate new peer slot
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (!m_peers[i].active) {
            m_peers[i].active            = true;
            m_peers[i].src               = src;
            m_peers[i].next_expected_seq = 1U;
            return i;
        }
    }

    return REASSEMBLY_SLOT_COUNT;  // no free peer slot
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::find_free_hold (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::find_free_hold() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(ORDERING_HOLD_COUNT > 0U);  // Assert: has capacity

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        if (!m_hold[i].active) {
            return i;
        }
    }

    return ORDERING_HOLD_COUNT;  // all in use
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::find_held (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::find_held(NodeId src, uint32_t seq) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);        // Assert: valid source

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        if (m_hold[i].active &&
            m_hold[i].env.source_id == src &&
            m_hold[i].env.sequence_num == seq) {
            return i;
        }
    }

    return ORDERING_HOLD_COUNT;  // not found
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::ingest
// ─────────────────────────────────────────────────────────────────────────────
Result OrderingBuffer::ingest(const MessageEnvelope& msg,
                               MessageEnvelope&       out,
                               uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);   // Assert: valid timestamp

    // Bypass: control messages (ACK/NAK/HEARTBEAT) are never ordered
    if (envelope_is_control(msg)) {
        envelope_copy(out, msg);
        return Result::OK;
    }

    // Bypass: sequence_num == 0 means UNORDERED
    if (msg.sequence_num == 0U) {
        envelope_copy(out, msg);
        return Result::OK;
    }

    // Ordered DATA message: apply ordering gate
    uint32_t peer_idx = get_or_create_peer(msg.source_id);
    if (peer_idx == REASSEMBLY_SLOT_COUNT) {
        // No peer slot available: ordering guarantee cannot be maintained.
        // Return ERR_FULL so the caller can log and drop the frame rather than
        // silently delivering it out of order (Issue 5 fix: honour ordering contract).
        Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                    "Peer table full; cannot enforce ordering for src=%u", msg.source_id);
        return Result::ERR_FULL;
    }

    uint32_t expected = m_peers[peer_idx].next_expected_seq;

    if (msg.sequence_num < expected) {
        // Duplicate: already delivered; discard silently
        return Result::ERR_AGAIN;
    }

    if (msg.sequence_num == expected) {
        // In order: deliver
        envelope_copy(out, msg);
        m_peers[peer_idx].next_expected_seq = expected + 1U;
        return Result::OK;
    }

    // Out of order: hold for later delivery
    uint32_t hold_idx = find_free_hold();
    if (hold_idx == ORDERING_HOLD_COUNT) {
        return Result::ERR_FULL;
    }

    envelope_copy(m_hold[hold_idx].env, msg);
    m_hold[hold_idx].active = true;

    Logger::log(Severity::INFO, "OrderingBuffer",
                "Held out-of-order msg seq=%u (expected=%u) from src=%u",
                msg.sequence_num, expected, msg.source_id);

    NEVER_COMPILED_OUT_ASSERT(m_hold[hold_idx].active);  // Assert: hold slot is active
    return Result::ERR_AGAIN;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::try_release_next
// ─────────────────────────────────────────────────────────────────────────────
Result OrderingBuffer::try_release_next(NodeId src, MessageEnvelope& out)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);        // Assert: valid source

    uint32_t peer_idx = find_peer(src);
    if (peer_idx == REASSEMBLY_SLOT_COUNT) {
        return Result::ERR_AGAIN;  // no tracking for this peer yet
    }

    uint32_t expected = m_peers[peer_idx].next_expected_seq;
    uint32_t hold_idx = find_held(src, expected);

    if (hold_idx == ORDERING_HOLD_COUNT) {
        return Result::ERR_AGAIN;  // no matching held message
    }

    // Found the next in-sequence held message: deliver and free hold slot
    envelope_copy(out, m_hold[hold_idx].env);
    m_hold[hold_idx].active = false;
    m_peers[peer_idx].next_expected_seq = expected + 1U;

    NEVER_COMPILED_OUT_ASSERT(!m_hold[hold_idx].active);  // Assert: hold slot freed
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::advance_sequence
// ─────────────────────────────────────────────────────────────────────────────
void OrderingBuffer::advance_sequence(NodeId src, uint32_t up_to_seq)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);    // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);         // Assert: valid source

    uint32_t peer_idx = find_peer(src);
    if (peer_idx == REASSEMBLY_SLOT_COUNT) {
        return;  // no tracking for this peer; no-op
    }

    // Only advance if up_to_seq is strictly greater than current
    if (up_to_seq > m_peers[peer_idx].next_expected_seq) {
        m_peers[peer_idx].next_expected_seq = up_to_seq;

        // Issue 4 fix: free any held frames for this source whose sequence_num is
        // now strictly less than next_expected_seq. Those slots can never be
        // delivered by try_release_next() once the cursor has moved past them.
        // Leaving them active leaks hold capacity (ORDERING_HOLD_COUNT = 8 slots).
        // Power of 10 Rule 2: bounded loop (≤ ORDERING_HOLD_COUNT iterations).
        for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
            if (m_hold[i].active &&
                m_hold[i].env.source_id == src &&
                m_hold[i].env.sequence_num < m_peers[peer_idx].next_expected_seq) {
                Logger::log(Severity::WARNING_LO, "OrderingBuffer",
                            "Freeing stale held seq=%u from src=%u (cursor advanced to %u)",
                            m_hold[i].env.sequence_num, src,
                            m_peers[peer_idx].next_expected_seq);
                m_hold[i].active = false;
            }
        }
    }

    NEVER_COMPILED_OUT_ASSERT(m_peers[peer_idx].next_expected_seq >= up_to_seq);  // Assert: advanced
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::sweep_expired_holds — SC: HAZ-001 (Issue 3 fix)
// Walks held slots; for any whose expiry_time_us has passed, copies the freed
// envelope into out_freed[] (if capacity allows), calls advance_sequence()
// past that sequence so the ordering gate unblocks, and frees the slot.
// Power of 10 Rule 2: bounded loop (≤ ORDERING_HOLD_COUNT iterations).
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::sweep_expired_holds(uint64_t         now_us,
                                              MessageEnvelope* out_freed,
                                              uint32_t         out_cap)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);   // Assert: valid timestamp

    uint32_t freed = 0U;

    // Power of 10 Rule 2: fixed upper bound = ORDERING_HOLD_COUNT.
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        if (!m_hold[i].active) {
            continue;
        }
        if (timestamp_expired(m_hold[i].env.expiry_time_us, now_us)) {
            Logger::log(Severity::WARNING_LO, "OrderingBuffer",
                        "Held seq=%u from src=%u expired; advancing ordering gate",
                        m_hold[i].env.sequence_num, m_hold[i].env.source_id);
            // Copy freed envelope to caller's buffer before freeing the slot.
            // Power of 10 Rule 7: out_cap check before write (no overflow).
            if ((out_freed != nullptr) && (freed < out_cap)) {
                envelope_copy(out_freed[freed], m_hold[i].env);
            }
            // Advance past this sequence so later frames can be delivered.
            advance_sequence(m_hold[i].env.source_id,
                             m_hold[i].env.sequence_num + 1U);
            m_hold[i].active = false;
            ++freed;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(freed <= ORDERING_HOLD_COUNT);  // Assert: bounded result
    return freed;
}
