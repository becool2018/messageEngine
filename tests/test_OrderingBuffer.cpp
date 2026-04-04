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

    return (failed > 0) ? 1 : 0;
}
