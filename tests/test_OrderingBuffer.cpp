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

// Verifies: REQ-3.3.5
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/OrderingBuffer.hpp"

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
// Test 5: Hold buffer exhaustion → ERR_FULL
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_ordering_hold_full()
{
    OrderingBuffer buf;
    buf.init(1U);

    MessageEnvelope out;
    envelope_init(out);

    // Fill the hold buffer with out-of-order messages (seq 2..ORDERING_HOLD_COUNT+1)
    // seq=1 never arrives, so all higher seqs will be held.
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ORDERING_HOLD_COUNT; ++i) {
        MessageEnvelope m;
        make_ordered(m, 50U, i + 2U, static_cast<uint8_t>(i));
        Result r = buf.ingest(m, out, 1000000ULL + static_cast<uint64_t>(i));
        assert(r == Result::ERR_AGAIN);
    }

    // One more out-of-order message should return ERR_FULL
    MessageEnvelope m_extra;
    make_ordered(m_extra, 50U, ORDERING_HOLD_COUNT + 2U, 0xFFU);
    Result r_full = buf.ingest(m_extra, out, 2000000ULL);
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

    // Seventeenth peer: peer table exhausted — ordering contract cannot be upheld.
    // ingest() must return ERR_FULL (Issue 5 fix: explicit rejection, not silent drop).
    MessageEnvelope m17;
    make_ordered(m17, 200U, 1U, 0xFFU);
    Result r17 = buf.ingest(m17, out, 1000001ULL);
    assert(r17 == Result::ERR_FULL);  // Assert: seventeenth peer rejected

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
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
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

    return (failed > 0) ? 1 : 0;
}
