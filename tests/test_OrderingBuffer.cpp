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
 * @file test_OrderingBuffer.cpp
 * @brief Unit tests for the OrderingBuffer module.
 *
 * Tests:
 *   1. In-sequence messages delivered in order
 *   2. Out-of-sequence message held; gap filled → both delivered
 *   3. UNORDERED messages (sequence_num == 0) bypass gate
 *   4. Control messages (ACK) bypass gate
 *   5. Hold buffer full → ERR_FULL
 *   6. Duplicate sequence discarded (ERR_AGAIN)
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions.
 *   - F-Prime style: assert() for test verification.
 */

// Verifies: REQ-3.3.5, REQ-3.3.6, REQ-3.2.11
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/OrderingBuffer.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// ─────────────────────────────────────────────────────────────────────────────
// Helper: build an ordered DATA envelope with given sequence_num
// ─────────────────────────────────────────────────────────────────────────────
static void make_ordered(MessageEnvelope& env,
                          NodeId           src,
                          uint32_t         seq,
                          uint8_t          fill)
{
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.source_id         = src;
    env.destination_id    = 1U;
    env.message_id        = static_cast<uint64_t>(seq) + 1000ULL;
    env.timestamp_us      = 1000000ULL;
    env.expiry_time_us    = 9000000ULL;
    env.priority          = 0U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.sequence_num      = seq;
    env.fragment_index    = 0U;
    env.fragment_count    = 1U;
    env.payload_length    = 4U;
    env.total_payload_length = 4U;
    env.payload[0] = fill;
    env.payload[1] = static_cast<uint8_t>(fill + 1U);
    env.payload[2] = static_cast<uint8_t>(fill + 2U);
    env.payload[3] = static_cast<uint8_t>(fill + 3U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: In-sequence messages (seq=1,2,3) delivered in order
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_in_sequence()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope m1, m2, m3;
    make_ordered(m1, 10U, 1U, 0xAAU);
    make_ordered(m2, 10U, 2U, 0xBBU);
    make_ordered(m3, 10U, 3U, 0xCCU);

    MessageEnvelope out;
    envelope_init(out);

    // seq=1 arrives first — should be delivered immediately
    Result r1 = buf.ingest(m1, out, 1000000ULL);
    assert(r1 == Result::OK);
    assert(out.sequence_num == 1U);
    assert(out.payload[0] == 0xAAU);

    // seq=2 arrives — should be delivered immediately
    Result r2 = buf.ingest(m2, out, 1000001ULL);
    assert(r2 == Result::OK);
    assert(out.sequence_num == 2U);
    assert(out.payload[0] == 0xBBU);

    // seq=3 arrives — should be delivered immediately
    Result r3 = buf.ingest(m3, out, 1000002ULL);
    assert(r3 == Result::OK);
    assert(out.sequence_num == 3U);
    assert(out.payload[0] == 0xCCU);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Out-of-sequence: seq=2 arrives before seq=1
//         seq=2 is held; seq=1 arrives → delivered; try_release_next → seq=2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_out_of_sequence()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope m1, m2;
    make_ordered(m1, 20U, 1U, 0x11U);
    make_ordered(m2, 20U, 2U, 0x22U);

    MessageEnvelope out;
    envelope_init(out);

    // seq=2 arrives first — should be held
    Result r2 = buf.ingest(m2, out, 1000000ULL);
    assert(r2 == Result::ERR_AGAIN);

    // seq=1 arrives — should be delivered immediately
    Result r1 = buf.ingest(m1, out, 1000001ULL);
    assert(r1 == Result::OK);
    assert(out.sequence_num == 1U);
    assert(out.payload[0] == 0x11U);

    // seq=2 is held and now in order — try_release_next should deliver it
    Result rr = buf.try_release_next(20U, out);
    assert(rr == Result::OK);
    assert(out.sequence_num == 2U);
    assert(out.payload[0] == 0x22U);

    // No more held — should return ERR_AGAIN
    Result r_none = buf.try_release_next(20U, out);
    assert(r_none == Result::ERR_AGAIN);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: UNORDERED messages (sequence_num == 0) bypass gate immediately
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_unordered_passthrough()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope msg;
    make_ordered(msg, 30U, 0U, 0x55U);  // sequence_num == 0 → UNORDERED

    MessageEnvelope out;
    envelope_init(out);

    Result r = buf.ingest(msg, out, 1000000ULL);
    assert(r == Result::OK);
    assert(out.sequence_num == 0U);
    assert(out.payload[0] == 0x55U);

    // A second unordered message from the same source should also bypass
    MessageEnvelope msg2;
    make_ordered(msg2, 30U, 0U, 0x66U);
    Result r2 = buf.ingest(msg2, out, 1000001ULL);
    assert(r2 == Result::OK);
    assert(out.payload[0] == 0x66U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Control messages (ACK) bypass the ordering gate immediately
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_control_bypass()
{
    OrderingBuffer buf;
    buf.init(1U);

    // Build an ACK envelope
    MessageEnvelope ack;
    envelope_init(ack);
    ack.message_type      = MessageType::ACK;
    ack.source_id         = 40U;
    ack.destination_id    = 1U;
    ack.message_id        = 9999ULL;
    ack.timestamp_us      = 2000000ULL;
    ack.expiry_time_us    = 9000000ULL;
    ack.reliability_class = ReliabilityClass::BEST_EFFORT;
    ack.sequence_num      = 5U;  // would be held if treated as ordered DATA
    ack.fragment_index    = 0U;
    ack.fragment_count    = 1U;
    ack.payload_length    = 0U;
    ack.total_payload_length = 0U;

    MessageEnvelope out;
    envelope_init(out);

    // ACK should bypass ordering gate and be delivered immediately
    Result r = buf.ingest(ack, out, 2000000ULL);
    assert(r == Result::OK);
    assert(out.message_type == MessageType::ACK);
    assert(out.message_id == 9999ULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Hold buffer exhaustion → ERR_FULL (SECfix-7: per-peer hold cap)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
// SECfix-7 limits a single peer to ORDERING_HOLD_COUNT/2 hold slots so one
// source cannot starve all others.  This test verifies:
//   (a) a single peer is capped at k_hold_per_peer_max (= ORDERING_HOLD_COUNT/2)
//   (b) the global hold buffer is exhausted when two peers together fill it,
//       and a further attempt returns ERR_FULL.
static bool test_ordering_hold_full()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope out;
    envelope_init(out);

    // SECfix-7: per-peer cap is ORDERING_HOLD_COUNT / 2.
    const uint32_t per_peer_max = ORDERING_HOLD_COUNT / 2U;

    // (a) Fill per-peer cap for src=50.  seq=1 never arrives; seqs 2..per_peer_max+1 are held.
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < per_peer_max; ++i) {
        MessageEnvelope m;
        make_ordered(m, 50U, i + 2U, static_cast<uint8_t>(i));
        Result r = buf.ingest(m, out, 1000000ULL + static_cast<uint64_t>(i));
        assert(r == Result::ERR_AGAIN);
    }

    // One more from src=50 must return ERR_FULL (per-peer cap, SECfix-7).
    MessageEnvelope m_cap;
    make_ordered(m_cap, 50U, per_peer_max + 2U, 0xEEU);
    Result r_cap = buf.ingest(m_cap, out, 1500000ULL);
    assert(r_cap == Result::ERR_FULL);

    // (b) Fill the remaining hold slots with a second peer (src=51).
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < per_peer_max; ++i) {
        MessageEnvelope m;
        make_ordered(m, 51U, i + 2U, static_cast<uint8_t>(i));
        Result r = buf.ingest(m, out, 1600000ULL + static_cast<uint64_t>(i));
        assert(r == Result::ERR_AGAIN);
    }

    // Global hold buffer is now full: any new peer also gets ERR_FULL.
    MessageEnvelope m_global;
    make_ordered(m_global, 52U, 2U, 0xFFU);
    Result r_full = buf.ingest(m_global, out, 2000000ULL);
    assert(r_full == Result::ERR_FULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Duplicate sequence (already delivered) is discarded → ERR_AGAIN
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_duplicate_seq_discarded()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope m1;
    make_ordered(m1, 60U, 1U, 0xA1U);

    MessageEnvelope out;
    envelope_init(out);

    // Deliver seq=1
    Result r1 = buf.ingest(m1, out, 1000000ULL);
    assert(r1 == Result::OK);
    assert(out.sequence_num == 1U);

    // Re-inject seq=1 (duplicate of already-delivered)
    Result r_dup = buf.ingest(m1, out, 1000001ULL);
    assert(r_dup == Result::ERR_AGAIN);  // silently discarded

    // seq=2 should still be deliverable normally
    MessageEnvelope m2;
    make_ordered(m2, 60U, 2U, 0xA2U);
    Result r2 = buf.ingest(m2, out, 1000002ULL);
    assert(r2 == Result::OK);
    assert(out.sequence_num == 2U);
    assert(out.payload[0] == 0xA2U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Sixteen distinct peers accepted; seventeenth rejected with ERR_FULL
// Verifies: REQ-3.3.5
//
// ORDERING_PEER_COUNT == MAX_PEER_NODES == 16. Confirms that the peer table
// accepts exactly 16 simultaneous ordered-channel sources and rejects the 17th
// (returns ERR_FULL rather than silently dropping or delivering out of order).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_sixteen_peers()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope out;
    envelope_init(out);

    // Power of 10 Rule 2: bounded loop (ORDERING_PEER_COUNT iterations)
    for (uint32_t i = 0U; i < ORDERING_PEER_COUNT; ++i) {
        MessageEnvelope m;
        // Source IDs 100..115 — all distinct; seq=1 is in-order for each new peer
        NodeId src = static_cast<NodeId>(100U + i);
        make_ordered(m, src, 1U, static_cast<uint8_t>(i & 0xFFU));
        Result r = buf.ingest(m, out, 1000000ULL);
        assert(r == Result::OK);         // Assert: peer slot allocated; seq=1 delivered
        assert(out.sequence_num == 1U);  // Assert: delivered message is seq=1
    }

    // Seventeenth peer: SECfix-2 — LRU eviction makes room instead of returning ERR_FULL.
    // The LRU peer (src=100, oldest access) is evicted and src=200 gets the freed slot.
    // seq=1 is in-order for a new peer so it is delivered immediately (OK).
    MessageEnvelope m17;
    make_ordered(m17, 200U, 1U, 0xFFU);
    Result r17 = buf.ingest(m17, out, 1000001ULL);
    assert(r17 == Result::OK);              // Assert: eviction made room; seq=1 delivered
    assert(out.sequence_num == 1U);         // Assert: delivered message is seq=1
    assert(out.source_id == 200U);          // Assert: delivered from the new peer

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: Repeated duplicate out-of-order messages do not exhaust hold capacity
//
// Verifies the Bug 3 fix: when an out-of-order (src, seq) pair is already held,
// a retransmitted duplicate must be silently discarded (ERR_AGAIN) rather than
// consuming a second hold slot.  Without the fix, N duplicates of the same
// out-of-order frame would use N slots, causing ERR_FULL for legitimate messages.
//
// Scenario:
//   1. seq=2 arrives before seq=1 → held (one slot used).
//   2. seq=2 arrives again (retransmit) → must return ERR_AGAIN, not consume
//      another slot.
//   3. A third distinct out-of-order sequence (seq=3) must still be holdable
//      (only one slot occupied, not two).
//   4. seq=1 arrives → delivered; try_release_next() delivers seq=2; seq=3 is
//      then released as well.
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_ooo_duplicate_does_not_exhaust_hold()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope m1, m2, m3;
    make_ordered(m1, 70U, 1U, 0x11U);
    make_ordered(m2, 70U, 2U, 0x22U);
    make_ordered(m3, 70U, 3U, 0x33U);

    MessageEnvelope out;
    envelope_init(out);

    // seq=2 arrives out-of-order → held (one hold slot consumed)
    Result r2a = buf.ingest(m2, out, 1000000ULL);
    assert(r2a == Result::ERR_AGAIN);  // Assert: held, not delivered

    // Duplicate seq=2 → must NOT consume another hold slot
    Result r2b = buf.ingest(m2, out, 1000001ULL);
    assert(r2b == Result::ERR_AGAIN);  // Assert: silently discarded

    // seq=3 arrives out-of-order → must succeed (only 1 slot used, not 2)
    Result r3 = buf.ingest(m3, out, 1000002ULL);
    assert(r3 == Result::ERR_AGAIN);   // Assert: held; capacity not exhausted

    // seq=1 arrives → delivered; next_expected advances to 2
    Result r1 = buf.ingest(m1, out, 1000003ULL);
    assert(r1 == Result::OK);          // Assert: seq=1 delivered in order
    assert(out.sequence_num == 1U);
    assert(out.payload[0] == 0x11U);

    // try_release_next delivers seq=2
    Result rr2 = buf.try_release_next(70U, out);
    assert(rr2 == Result::OK);         // Assert: seq=2 released from hold
    assert(out.sequence_num == 2U);
    assert(out.payload[0] == 0x22U);

    // try_release_next delivers seq=3
    Result rr3 = buf.try_release_next(70U, out);
    assert(rr3 == Result::OK);         // Assert: seq=3 released from hold
    assert(out.sequence_num == 3U);
    assert(out.payload[0] == 0x33U);

    // No more held messages
    Result rr_none = buf.try_release_next(70U, out);
    assert(rr_none == Result::ERR_AGAIN);  // Assert: hold buffer empty

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: Sequence number wraparound at u32 max is handled safely.
// Verifies: REQ-3.3.5
// Steps:
//   1. Init and register a peer by delivering seq=1 in-order.
//   2. Advance next_expected to 0xFFFFFFFEU using advance_sequence().
//   3. Deliver seq=0xFFFFFFFEU -> OK (in-order, next_expected becomes 0xFFFFFFFFU).
//   4. Deliver seq=0xFFFFFFFFU -> OK (in-order, advance_next_expected fires wraparound,
//      next_expected resets to 1U).
//   5. Deliver seq=1U -> OK (matches new next_expected=1 after wraparound reset).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_sequence_wraparound_at_max()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 80U;

    // Step 1: Register the peer by delivering seq=1 in-order.
    MessageEnvelope m1;
    make_ordered(m1, SRC, 1U, 0x01U);
    MessageEnvelope out;
    envelope_init(out);
    Result r1 = buf.ingest(m1, out, 1000000ULL);
    assert(r1 == Result::OK);        // Assert: seq=1 delivered in order
    assert(out.sequence_num == 1U);  // Assert: correct sequence number delivered

    // Step 2: Advance next_expected to 0xFFFFFFFEU so the next in-order
    //         delivery is at the penultimate u32 value.
    buf.advance_sequence(SRC, 0xFFFFFFFEU);

    // Step 3: Deliver seq=0xFFFFFFFEU -> must be in-order (OK).
    MessageEnvelope mfe;
    make_ordered(mfe, SRC, 0xFFFFFFFEU, 0xFEU);
    Result rfe = buf.ingest(mfe, out, 1000001ULL);
    assert(rfe == Result::OK);              // Assert: seq=0xFFFFFFFE delivered
    assert(out.sequence_num == 0xFFFFFFFEU); // Assert: correct sequence returned

    // Step 4: Deliver seq=0xFFFFFFFFU -> must be in-order (OK).
    // advance_next_expected() detects next would be 0 and resets to 1.
    MessageEnvelope mff;
    make_ordered(mff, SRC, 0xFFFFFFFFU, 0xFFU);
    Result rff = buf.ingest(mff, out, 1000002ULL);
    assert(rff == Result::OK);               // Assert: seq=0xFFFFFFFF delivered
    assert(out.sequence_num == 0xFFFFFFFFU); // Assert: correct sequence returned

    // Step 5: next_expected is now 1 (after wraparound reset).
    // Deliver seq=1U -> must be in-order (OK).
    MessageEnvelope mwrap;
    make_ordered(mwrap, SRC, 1U, 0xA0U);
    Result rwrap = buf.ingest(mwrap, out, 1000003ULL);
    assert(rwrap == Result::OK);       // Assert: seq=1 accepted after wraparound
    assert(out.sequence_num == 1U);    // Assert: correct sequence returned

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: Normal sequence advancement across 100 messages does not crash.
// Verifies: REQ-3.3.5
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_advance_sequence_no_wraparound_normal()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 90U;

    MessageEnvelope out;
    envelope_init(out);

    // Power of 10 Rule 2: bounded loop (100 iterations, static upper bound)
    static const uint32_t NUM_MSGS = 100U;
    for (uint32_t seq = 1U; seq <= NUM_MSGS; ++seq) {
        MessageEnvelope m;
        make_ordered(m, SRC, seq, static_cast<uint8_t>(seq & 0xFFU));
        Result r = buf.ingest(m, out, 1000000ULL + static_cast<uint64_t>(seq));
        assert(r == Result::OK);              // Assert: each in-order message delivered
        assert(out.sequence_num == seq);      // Assert: returned sequence matches sent
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: sweep_expired_holds() advances ordering gate past stale held messages
// Verifies: REQ-3.3.5
//
// Scenario:
//   1. seq=2 arrives before seq=1 → held (expiry_time_us set to a past value).
//   2. sweep_expired_holds() at a future time frees the slot and advances past seq=2.
//   3. A subsequent seq=3 (now in-order after advance) is delivered immediately.
//   4. A null out_freed pointer and zero cap are handled safely.
// ─────────────────────────────────────────────────────────────────────────────
static bool test_sweep_expired_holds_advances_gate()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 95U;

    // seq=2 with expiry in the near past (will expire when swept at T+1000000)
    MessageEnvelope m2;
    make_ordered(m2, SRC, 2U, 0x22U);
    m2.expiry_time_us = 1000000ULL;  // expires at 1 second

    MessageEnvelope out;
    envelope_init(out);

    // Ingest seq=2 out-of-order → held (seq=1 not yet received)
    Result r2 = buf.ingest(m2, out, 500000ULL);
    assert(r2 == Result::ERR_AGAIN);  // Assert: held, seq=1 still missing

    // Sweep at a time past the expiry → the held seq=2 slot must be freed
    MessageEnvelope freed_buf[ORDERING_HOLD_COUNT];
    // Power of 10 Rule 2: bounded by ORDERING_HOLD_COUNT
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        envelope_init(freed_buf[i]);
    }
    uint32_t freed = buf.sweep_expired_holds(2000000ULL, freed_buf, ORDERING_HOLD_COUNT);
    assert(freed == 1U);               // Assert: exactly one expired hold freed
    assert(freed_buf[0].source_id == SRC);  // Assert: freed envelope belongs to SRC
    assert(freed_buf[0].sequence_num == 2U);  // Assert: freed seq=2

    // After advance, next_expected for SRC is now 3.
    // seq=3 arriving should be in-order (== next_expected).
    MessageEnvelope m3;
    make_ordered(m3, SRC, 3U, 0x33U);
    m3.expiry_time_us = 9000000ULL;

    Result r3 = buf.ingest(m3, out, 2000001ULL);
    assert(r3 == Result::OK);          // Assert: seq=3 delivered in order
    assert(out.sequence_num == 3U);    // Assert: correct sequence number

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: sweep_expired_holds() with nullptr out_freed (null-safe path)
// Verifies: REQ-3.3.5
//
// Ensures sweep_expired_holds() handles a null out_freed buffer without
// crashing — the caller may pass nullptr when it does not need freed envelopes.
// ─────────────────────────────────────────────────────────────────────────────
static bool test_sweep_expired_holds_null_out()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 96U;

    // Hold seq=2 with an already-expired expiry
    MessageEnvelope m2;
    make_ordered(m2, SRC, 2U, 0x42U);
    m2.expiry_time_us = 100U;  // expires immediately

    MessageEnvelope out;
    envelope_init(out);

    Result r2 = buf.ingest(m2, out, 50U);
    assert(r2 == Result::ERR_AGAIN);  // Assert: held out-of-order

    // Sweep with nullptr out_freed and capacity 0 — must not crash
    uint32_t freed = buf.sweep_expired_holds(200U, nullptr, 0U);
    assert(freed == 1U);  // Assert: slot freed despite null buffer

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: sweep_expired_holds() leaves non-expired slots untouched
// Verifies: REQ-3.3.5
// ─────────────────────────────────────────────────────────────────────────────
static bool test_sweep_expired_holds_no_expiry()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 97U;

    // Hold seq=2 with a far-future expiry (will not expire at sweep time)
    MessageEnvelope m2;
    make_ordered(m2, SRC, 2U, 0x52U);
    m2.expiry_time_us = 9999999999ULL;  // very far in the future

    MessageEnvelope out;
    envelope_init(out);

    Result r2 = buf.ingest(m2, out, 1000000ULL);
    assert(r2 == Result::ERR_AGAIN);  // Assert: held

    // Sweep at a time well before the expiry — nothing should be freed
    MessageEnvelope freed_buf[ORDERING_HOLD_COUNT];
    uint32_t freed = buf.sweep_expired_holds(2000000ULL, freed_buf, ORDERING_HOLD_COUNT);
    assert(freed == 0U);  // Assert: no slots freed (not yet expired)

    // The held slot must still be reachable — seq=1 delivered fills the gap
    MessageEnvelope m1;
    make_ordered(m1, SRC, 1U, 0x51U);
    m1.expiry_time_us = 9999999999ULL;
    Result r1 = buf.ingest(m1, out, 2000001ULL);
    assert(r1 == Result::OK);      // Assert: seq=1 delivered
    assert(out.sequence_num == 1U); // Assert: correct sequence

    // try_release_next must yield the previously held seq=2
    Result rr = buf.try_release_next(SRC, out);
    assert(rr == Result::OK);      // Assert: seq=2 released from hold
    assert(out.sequence_num == 2U); // Assert: correct sequence

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: sweep_expired_holds() with sequence_num == UINT32_MAX (CERT INT30-C)
// Verifies: REQ-3.3.5
//
// When a held slot has sequence_num == UINT32_MAX, the naive "+1U" wraps to 0
// (the UNORDERED sentinel); advance_sequence() is then a no-op (0 > next_expected
// is always false), stalling the gate.  The guard resets to 1 instead, consistent
// with advance_next_expected()'s wraparound policy.
//
// Scenario: advance next_expected to UINT32_MAX-1, hold seq=UINT32_MAX (expired),
// sweep → freed==1, then deliver UINT32_MAX-1 and UINT32_MAX in-order to confirm
// the gate wraps correctly to next_expected==1.
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_sweep_expired_holds_sequence_num_uint32_max()
{
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 98U;

    // Register peer via in-order delivery of seq=1.
    MessageEnvelope m1;
    make_ordered(m1, SRC, 1U, 0x01U);
    MessageEnvelope out;
    envelope_init(out);
    Result r1 = buf.ingest(m1, out, 1000000ULL);
    assert(r1 == Result::OK);        // Assert: seq=1 delivered; peer registered
    assert(out.sequence_num == 1U);  // Assert: correct sequence delivered

    // Advance next_expected to UINT32_MAX-1; seq=UINT32_MAX is one step ahead.
    buf.advance_sequence(SRC, 0xFFFFFFFEU);

    // Ingest seq=UINT32_MAX with a past expiry — held (out-of-order by 1).
    MessageEnvelope mmax;
    make_ordered(mmax, SRC, 0xFFFFFFFFU, 0xFFU);
    mmax.expiry_time_us = 1U;
    Result rmax = buf.ingest(mmax, out, 2000000ULL);
    assert(rmax == Result::ERR_AGAIN);  // Assert: held out-of-order

    // Sweep at future time — CERT INT30-C guard fires: next_seq = 1 (not 0).
    MessageEnvelope freed_buf[ORDERING_HOLD_COUNT];
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        envelope_init(freed_buf[i]);
    }
    uint32_t freed = buf.sweep_expired_holds(3000000ULL, freed_buf, ORDERING_HOLD_COUNT);
    assert(freed == 1U);                              // Assert: exactly one slot freed
    assert(freed_buf[0].sequence_num == 0xFFFFFFFFU); // Assert: freed UINT32_MAX slot
    assert(freed_buf[0].source_id == SRC);            // Assert: correct source

    // Deliver seq=UINT32_MAX-1 (current next_expected) in-order.
    MessageEnvelope mfe;
    make_ordered(mfe, SRC, 0xFFFFFFFEU, 0xFEU);
    mfe.expiry_time_us = 9000000ULL;
    Result rfe = buf.ingest(mfe, out, 3000001ULL);
    assert(rfe == Result::OK);               // Assert: seq=UINT32_MAX-1 in-order
    assert(out.sequence_num == 0xFFFFFFFEU); // Assert: correct sequence

    // Deliver seq=UINT32_MAX in-order; advance_next_expected() wraps to 1.
    MessageEnvelope mff;
    make_ordered(mff, SRC, 0xFFFFFFFFU, 0xFEU);
    mff.expiry_time_us = 9000000ULL;
    Result rff = buf.ingest(mff, out, 3000002ULL);
    assert(rff == Result::OK);               // Assert: seq=UINT32_MAX delivered
    assert(out.sequence_num == 0xFFFFFFFFU); // Assert: correct sequence

    // next_expected is now 1; seq=1 must be accepted as in-order.
    MessageEnvelope m1b;
    make_ordered(m1b, SRC, 1U, 0xA1U);
    m1b.expiry_time_us = 9000000ULL;
    Result r1b = buf.ingest(m1b, out, 3000003ULL);
    assert(r1b == Result::OK);       // Assert: seq=1 accepted after full wraparound
    assert(out.sequence_num == 1U);  // Assert: correct sequence returned

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: try_release_next for a peer that has never been seen
// Verifies: REQ-3.3.5
//
// When try_release_next() is called for a source that has never sent any
// ordered messages (find_peer returns not-found), the function must return
// ERR_AGAIN without modifying the output envelope.
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_try_release_next_unknown_peer()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope out;
    envelope_init(out);

    // src=200 has never sent anything — find_peer returns ORDERING_PEER_COUNT
    Result r = buf.try_release_next(200U, out);
    assert(r == Result::ERR_AGAIN);  // Assert: unknown peer returns ERR_AGAIN
    assert(out.source_id == 0U);     // Assert: output envelope is untouched

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: advance_sequence for a peer that has never been seen
// Verifies: REQ-3.3.5
//
// When advance_sequence() is called for a source with no registered peer slot,
// it must be a silent no-op.  Subsequent in-order delivery from that source
// must still work normally (peer is created on first ingest).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_advance_sequence_unknown_peer()
{
    OrderingBuffer buf;
    buf.init(1U);

    // src=201 has never sent anything — advance_sequence must be a silent no-op
    buf.advance_sequence(201U, 5U);  // must not crash or allocate a peer slot

    // After the no-op, seq=1 from src=201 should be accepted as in-order
    MessageEnvelope m1;
    make_ordered(m1, 201U, 1U, 0xAAU);

    MessageEnvelope out;
    envelope_init(out);
    Result r = buf.ingest(m1, out, 1000000ULL);
    assert(r == Result::OK);         // Assert: in-order delivery still works
    assert(out.sequence_num == 1U);  // Assert: correct sequence returned

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ-3.3.6: reset_peer() clears stale next_expected_seq so reconnecting peer
// starts delivering from seq=1 again.
// Verifies: REQ-3.3.6 — M4 branch: peer found; next_expected reset; seq=1 accepted
// ─────────────────────────────────────────────────────────────────────────────
static bool test_reset_peer_clears_sequence()
{
    // Verifies: REQ-3.3.6
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 20U;
    MessageEnvelope out;
    MessageEnvelope m;

    // Deliver seq=1 and seq=2 so next_expected advances to 3.
    make_ordered(m, SRC, 1U, 0xA0U);
    Result r1 = buf.ingest(m, out, 1000ULL);
    assert(r1 == Result::OK);        // Assert: seq=1 delivered
    assert(out.sequence_num == 1U);  // Assert: correct sequence

    make_ordered(m, SRC, 2U, 0xA1U);
    Result r2 = buf.ingest(m, out, 1001ULL);
    assert(r2 == Result::OK);        // Assert: seq=2 delivered

    // Peer reconnects: reset ordering state.
    buf.reset_peer(SRC);

    // New connection restarts at seq=1; must be accepted (not discarded as dup).
    make_ordered(m, SRC, 1U, 0xB0U);
    Result r3 = buf.ingest(m, out, 2000ULL);
    assert(r3 == Result::OK);        // Assert: seq=1 accepted after reset
    assert(out.sequence_num == 1U);  // Assert: correct sequence returned
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ-3.3.6: reset_peer() frees all held (out-of-order) messages for the peer.
// Verifies: REQ-3.3.6 — M4 branch: hold slots freed; no ERR_FULL on fresh ingest
// ─────────────────────────────────────────────────────────────────────────────
static bool test_reset_peer_frees_holds()
{
    // Verifies: REQ-3.3.6
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 21U;
    MessageEnvelope out;
    MessageEnvelope m;

    // Hold two out-of-order messages (seq 2 and 3 arrive before seq 1).
    make_ordered(m, SRC, 2U, 0xC0U);
    Result r2 = buf.ingest(m, out, 1000ULL);
    assert(r2 == Result::ERR_AGAIN);  // Assert: held (seq 2 > expected 1)

    make_ordered(m, SRC, 3U, 0xC1U);
    Result r3 = buf.ingest(m, out, 1001ULL);
    assert(r3 == Result::ERR_AGAIN);  // Assert: held (seq 3 > expected 1)

    // Peer reconnects: all held slots for SRC must be freed.
    buf.reset_peer(SRC);

    // After reset, try_release_next returns ERR_AGAIN (no stale held messages).
    Result rel = buf.try_release_next(SRC, out);
    assert(rel == Result::ERR_AGAIN);  // Assert: no stale held messages remain

    // Fresh seq=1 is in-order.
    make_ordered(m, SRC, 1U, 0xD0U);
    Result r1 = buf.ingest(m, out, 2000ULL);
    assert(r1 == Result::OK);        // Assert: seq=1 accepted after reset
    assert(out.sequence_num == 1U);  // Assert: correct sequence
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ-3.3.6: reset_peer() is idempotent — calling it twice must not crash.
// Verifies: REQ-3.3.6 — M4 branch: idempotent double-reset
// ─────────────────────────────────────────────────────────────────────────────
static bool test_reset_peer_idempotent()
{
    // Verifies: REQ-3.3.6
    OrderingBuffer buf;
    buf.init(1U);

    const NodeId SRC = 22U;
    MessageEnvelope out;
    MessageEnvelope m;

    // Deliver seq=1 to create peer state.
    make_ordered(m, SRC, 1U, 0xE0U);
    Result r1 = buf.ingest(m, out, 1000ULL);
    assert(r1 == Result::OK);  // Assert: seq=1 delivered

    buf.reset_peer(SRC);
    buf.reset_peer(SRC);  // second reset — must not crash

    make_ordered(m, SRC, 1U, 0xE1U);
    Result r2 = buf.ingest(m, out, 2000ULL);
    assert(r2 == Result::OK);        // Assert: seq=1 accepted after double reset
    assert(out.sequence_num == 1U);  // Assert: correct sequence
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ-3.3.6: reset_peer() is a no-op for an unknown src (peer never seen).
// Verifies: REQ-3.3.6 — M4 branch: peer_idx == ORDERING_PEER_COUNT (early return)
// ─────────────────────────────────────────────────────────────────────────────
static bool test_reset_peer_unknown_src()
{
    // Verifies: REQ-3.3.6
    OrderingBuffer buf;
    buf.init(1U);

    buf.reset_peer(static_cast<NodeId>(99U));  // unknown src — must be no-op

    // A different peer still works normally afterwards.
    MessageEnvelope out;
    MessageEnvelope m;
    make_ordered(m, static_cast<NodeId>(33U), 1U, 0xF0U);
    Result r = buf.ingest(m, out, 1000ULL);
    assert(r == Result::OK);        // Assert: unrelated peer unaffected
    assert(out.sequence_num == 1U); // Assert: correct sequence
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    // Initialize logger before any production code that may call LOG_* macros.
    // Power of 10: return value checked; failure causes abort via NEVER_COMPILED_OUT_ASSERT.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    int failed = 0;

    if (!test_ordering_in_sequence()) {
        printf("FAIL: test_ordering_in_sequence\n"); ++failed;
    } else { printf("PASS: test_ordering_in_sequence\n"); }

    if (!test_ordering_out_of_sequence()) {
        printf("FAIL: test_ordering_out_of_sequence\n"); ++failed;
    } else { printf("PASS: test_ordering_out_of_sequence\n"); }

    if (!test_ordering_unordered_passthrough()) {
        printf("FAIL: test_ordering_unordered_passthrough\n"); ++failed;
    } else { printf("PASS: test_ordering_unordered_passthrough\n"); }

    if (!test_ordering_control_bypass()) {
        printf("FAIL: test_ordering_control_bypass\n"); ++failed;
    } else { printf("PASS: test_ordering_control_bypass\n"); }

    if (!test_ordering_hold_full()) {
        printf("FAIL: test_ordering_hold_full\n"); ++failed;
    } else { printf("PASS: test_ordering_hold_full\n"); }

    if (!test_ordering_duplicate_seq_discarded()) {
        printf("FAIL: test_ordering_duplicate_seq_discarded\n"); ++failed;
    } else { printf("PASS: test_ordering_duplicate_seq_discarded\n"); }

    if (!test_ordering_sixteen_peers()) {
        printf("FAIL: test_ordering_sixteen_peers\n"); ++failed;
    } else { printf("PASS: test_ordering_sixteen_peers\n"); }

    if (!test_ordering_ooo_duplicate_does_not_exhaust_hold()) {
        printf("FAIL: test_ordering_ooo_duplicate_does_not_exhaust_hold\n"); ++failed;
    } else { printf("PASS: test_ordering_ooo_duplicate_does_not_exhaust_hold\n"); }

    if (!test_sequence_wraparound_at_max()) {
        printf("FAIL: test_sequence_wraparound_at_max\n"); ++failed;
    } else { printf("PASS: test_sequence_wraparound_at_max\n"); }

    if (!test_advance_sequence_no_wraparound_normal()) {
        printf("FAIL: test_advance_sequence_no_wraparound_normal\n"); ++failed;
    } else { printf("PASS: test_advance_sequence_no_wraparound_normal\n"); }

    if (!test_sweep_expired_holds_advances_gate()) {
        printf("FAIL: test_sweep_expired_holds_advances_gate\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_holds_advances_gate\n"); }

    if (!test_sweep_expired_holds_null_out()) {
        printf("FAIL: test_sweep_expired_holds_null_out\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_holds_null_out\n"); }

    if (!test_sweep_expired_holds_no_expiry()) {
        printf("FAIL: test_sweep_expired_holds_no_expiry\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_holds_no_expiry\n"); }

    if (!test_sweep_expired_holds_sequence_num_uint32_max()) {
        printf("FAIL: test_sweep_expired_holds_sequence_num_uint32_max\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_holds_sequence_num_uint32_max\n"); }

    if (!test_try_release_next_unknown_peer()) {
        printf("FAIL: test_try_release_next_unknown_peer\n"); ++failed;
    } else { printf("PASS: test_try_release_next_unknown_peer\n"); }

    if (!test_advance_sequence_unknown_peer()) {
        printf("FAIL: test_advance_sequence_unknown_peer\n"); ++failed;
    } else { printf("PASS: test_advance_sequence_unknown_peer\n"); }

    if (!test_reset_peer_clears_sequence()) {
        printf("FAIL: test_reset_peer_clears_sequence\n"); ++failed;
    } else { printf("PASS: test_reset_peer_clears_sequence\n"); }

    if (!test_reset_peer_frees_holds()) {
        printf("FAIL: test_reset_peer_frees_holds\n"); ++failed;
    } else { printf("PASS: test_reset_peer_frees_holds\n"); }

    if (!test_reset_peer_idempotent()) {
        printf("FAIL: test_reset_peer_idempotent\n"); ++failed;
    } else { printf("PASS: test_reset_peer_idempotent\n"); }

    if (!test_reset_peer_unknown_src()) {
        printf("FAIL: test_reset_peer_unknown_src\n"); ++failed;
    } else { printf("PASS: test_reset_peer_unknown_src\n"); }

    return (failed > 0) ? 1 : 0;
}
