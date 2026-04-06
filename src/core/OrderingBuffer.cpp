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
 *   - Power of 10: fixed loop bounds (ORDERING_HOLD_COUNT, ORDERING_PEER_COUNT),
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

// SECfix-7: per-peer maximum hold slots.  A single source may occupy at most
// half the total hold capacity so it cannot starve all other ordered peers.
// With ORDERING_HOLD_COUNT = 8 this gives 4 slots per peer.
static const uint32_t k_hold_per_peer_max = ORDERING_HOLD_COUNT / 2U;

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::init
// ─────────────────────────────────────────────────────────────────────────────
void OrderingBuffer::init(NodeId local_node)
{
    NEVER_COMPILED_OUT_ASSERT(ORDERING_HOLD_COUNT > 0U);     // Assert: capacity constant valid
    NEVER_COMPILED_OUT_ASSERT(ORDERING_PEER_COUNT > 0U);   // Assert: peer capacity valid

    // Power of 10 Rule 2: bounded loops
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        m_hold[i].active = false;
        envelope_init(m_hold[i].env);
    }

    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        m_peers[i].active            = false;
        m_peers[i].src               = 0U;
        m_peers[i].next_expected_seq = 1U;  // sequences start at 1
        m_peers[i].lru_stamp         = 0U;
    }
    m_lru_counter = 0U;

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
    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        if (m_peers[i].active && m_peers[i].src == src) {
            return i;
        }
    }

    return ORDERING_PEER_COUNT;  // not found
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::count_holds_for_peer (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::count_holds_for_peer(NodeId src) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);       // Assert: valid source

    uint32_t count = 0U;
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        if (m_hold[i].active && m_hold[i].env.source_id == src) {
            ++count;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(count <= ORDERING_HOLD_COUNT);  // Assert: count bounded
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::evict_peer_no_holds (private)
// Phase-1 eviction: scan active peers for one with no hold slots.
// Evicts immediately if found and returns the freed index.
// Returns ORDERING_PEER_COUNT if every peer has at least one hold.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::evict_peer_no_holds()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);             // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(ORDERING_PEER_COUNT > 0U);  // Assert: valid capacity

    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        if (!m_peers[i].active) { continue; }
        if (count_holds_for_peer(m_peers[i].src) == 0U) {
            Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                        "peer table full: evicting peer src=%u (no holds; SECfix-2)",
                        static_cast<unsigned>(m_peers[i].src));
            m_peers[i].active = false;
            return i;
        }
    }
    return ORDERING_PEER_COUNT;  // all peers have holds
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::find_lru_peer_idx (private)
// Scan active peers and return the index with the smallest lru_stamp.
// Called only when the peer table is full, so at least one entry is active.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::find_lru_peer_idx() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);             // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(ORDERING_PEER_COUNT > 0U);  // Assert: valid capacity

    // G-7: use ORDERING_PEER_COUNT as a sentinel for "not yet found" and skip
    // inactive slots.  The prior code started from index 0 unconditionally,
    // silently picking an inactive peer as the LRU candidate, causing
    // free_holds_for_peer() to operate on the wrong source_id.
    uint32_t lru_idx = ORDERING_PEER_COUNT;  // sentinel: not yet found
    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        if (!m_peers[i].active) { continue; }
        if (lru_idx == ORDERING_PEER_COUNT ||
            m_peers[i].lru_stamp < m_peers[lru_idx].lru_stamp) {
            lru_idx = i;
        }
    }
    NEVER_COMPILED_OUT_ASSERT(lru_idx < ORDERING_PEER_COUNT);  // Assert: valid LRU slot found
    return lru_idx;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::free_holds_for_peer (private)
// Release all hold slots whose source_id matches the peer at peer_idx.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
void OrderingBuffer::free_holds_for_peer(uint32_t peer_idx)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(peer_idx < ORDERING_PEER_COUNT);  // Assert: valid index

    for (uint32_t j = 0U; j < ORDERING_HOLD_COUNT; ++j) {
        if (m_hold[j].active &&
            m_hold[j].env.source_id == m_peers[peer_idx].src) {
            m_hold[j].active = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::evict_lru_peer (private)
// Selects the LRU peer slot: first looks for a peer with zero active hold slots
// (evicting such a peer loses no buffered state); if all have holds, picks the
// one with the smallest lru_stamp.  Frees all hold slots belonging to the chosen
// peer and logs WARNING_HI.
// SECfix-2: prevents 16 spoofed senders permanently exhausting the peer table.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::evict_lru_peer()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: initialized

    // Phase 1: prefer a peer with no active hold slots (no buffered state).
    uint32_t no_holds_idx = evict_peer_no_holds();
    if (no_holds_idx != ORDERING_PEER_COUNT) {
        return no_holds_idx;
    }

    // Phase 2: all peers have holds — evict the one with the smallest lru_stamp.
    uint32_t lru_idx = find_lru_peer_idx();
    free_holds_for_peer(lru_idx);
    Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                "peer table full: evicting LRU peer src=%u (SECfix-2)",
                static_cast<unsigned>(m_peers[lru_idx].src));
    m_peers[lru_idx].active = false;

    NEVER_COMPILED_OUT_ASSERT(lru_idx < ORDERING_PEER_COUNT);  // Assert: valid index
    return lru_idx;
}

// ─────────────────────────────────────────────────────────────────────────────
// OrderingBuffer::get_or_create_peer (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::get_or_create_peer(NodeId src)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);        // Assert: valid source

    uint32_t idx = find_peer(src);
    if (idx != ORDERING_PEER_COUNT) {
        // Update LRU stamp on access (SECfix-2)
        ++m_lru_counter;
        m_peers[idx].lru_stamp = m_lru_counter;
        return idx;  // already tracked
    }

    // Allocate new peer slot
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        if (!m_peers[i].active) {
            m_peers[i].active            = true;
            m_peers[i].src               = src;
            m_peers[i].next_expected_seq = 1U;
            ++m_lru_counter;
            m_peers[i].lru_stamp         = m_lru_counter;
            return i;
        }
    }

    // All peer slots full: evict the LRU peer to make room.
    // SECfix-2: prevents 16 spoofed senders permanently exhausting the peer table.
    uint32_t evicted = evict_lru_peer();
    m_peers[evicted].active            = true;
    m_peers[evicted].src               = src;
    m_peers[evicted].next_expected_seq = 1U;
    ++m_lru_counter;
    m_peers[evicted].lru_stamp         = m_lru_counter;

    NEVER_COMPILED_OUT_ASSERT(evicted < ORDERING_PEER_COUNT);  // Assert: valid slot returned
    return evicted;
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
// OrderingBuffer::advance_next_expected (private)
// Guards u32 wraparound when incrementing next_expected_seq for a peer slot.
// On wraparound (0xFFFFFFFF -> 0), logs WARNING_HI and resets to 1 so the
// ordering gate does not stall permanently on the next expected value of 0.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
void OrderingBuffer::advance_next_expected(uint32_t peer_idx)
{
    NEVER_COMPILED_OUT_ASSERT(peer_idx < ORDERING_PEER_COUNT);  // Assert: valid peer index
    NEVER_COMPILED_OUT_ASSERT(m_peers[peer_idx].active);         // Assert: peer slot is active

    uint32_t next = m_peers[peer_idx].next_expected_seq + 1U;

    if (next == 0U) {
        // Wraparound from 0xFFFFFFFF -> 0; reset to 1 to avoid ordering stall.
        Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                    "sequence number wraparound for peer slot %u; resetting to 1",
                    static_cast<unsigned>(peer_idx));
        next = 1U;
    }

    // Assert: next must never be 0 after the wraparound guard above.
    // Sequence 0 is reserved for UNORDERED bypass (sequence_num == 0 bypasses gate).
    NEVER_COMPILED_OUT_ASSERT(next != 0U);  // Assert: sequence zero is reserved and never expected

    m_peers[peer_idx].next_expected_seq = next;
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
    if (peer_idx == ORDERING_PEER_COUNT) {
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
        advance_next_expected(peer_idx);
        return Result::OK;
    }

    // Out of order: hold for later delivery.
    // Bug fix: check whether this (src, seq) pair is already held before
    // allocating a new slot.  Without this check a retransmitted out-of-order
    // frame would consume a second hold slot, exhausting ORDERING_HOLD_COUNT
    // capacity and causing ERR_FULL for legitimate new messages.
    uint32_t existing_hold = find_held(msg.source_id, msg.sequence_num);
    if (existing_hold != ORDERING_HOLD_COUNT) {
        // Already held: silently discard the duplicate (same behaviour as the
        // already-delivered duplicate path above).
        return Result::ERR_AGAIN;
    }

    // SECfix-7: enforce per-peer hold cap to prevent single source exhausting all hold slots.
    if (count_holds_for_peer(msg.source_id) >= k_hold_per_peer_max) {
        Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                    "per-peer hold cap reached for src=%u; dropping out-of-order msg seq=%u",
                    msg.source_id, msg.sequence_num);
        return Result::ERR_FULL;
    }

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
    if (peer_idx == ORDERING_PEER_COUNT) {
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
    advance_next_expected(peer_idx);

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
    if (peer_idx == ORDERING_PEER_COUNT) {
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
// OrderingBuffer::seq_next_guarded (private, static)
// Returns seq + 1, guarded against uint32_t wraparound (CERT INT30-C).
// If seq == UINT32_MAX, logs WARNING_HI and returns 1 (matching
// advance_next_expected() wraparound policy).  Never returns 0.
// Power of 10 Rule 5: 2 assertions. CC <= 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t OrderingBuffer::seq_next_guarded(uint32_t seq)
{
    NEVER_COMPILED_OUT_ASSERT(seq != 0U);  // Assert: 0 is the UNORDERED sentinel; must not be held

    uint32_t next = seq;
    if (seq < UINT32_MAX) {
        next = seq + 1U;
    } else {
        // CERT INT30-C: UINT32_MAX + 1 would wrap to 0 (the UNORDERED sentinel).
        // Reset to 1 instead, consistent with advance_next_expected() policy.
        Logger::log(Severity::WARNING_HI, "OrderingBuffer",
                    "sequence_num UINT32_MAX in seq_next_guarded; resetting to 1");
        next = 1U;
    }

    NEVER_COMPILED_OUT_ASSERT(next != 0U);  // Assert: result is never the UNORDERED sentinel
    return next;
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
            // seq_next_guarded() enforces CERT INT30-C: never wraps to 0.
            advance_sequence(m_hold[i].env.source_id,
                             seq_next_guarded(m_hold[i].env.sequence_num));
            m_hold[i].active = false;
            ++freed;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(freed <= ORDERING_HOLD_COUNT);  // Assert: bounded result
    return freed;
}
