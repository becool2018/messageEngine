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
 * @file test_ReassemblyBuffer.cpp
 * @brief Unit tests for the ReassemblyBuffer module.
 *
 * Tests:
 *   1. Single-fragment message completes immediately
 *   2. Three fragments arrive in order → reassembled correctly
 *   3. Fragments arrive out of order → reassembled correctly
 *   4. Duplicate fragment silently ignored
 *   5. Inconsistent fragment_count rejected
 *   6. Slot exhaustion → ERR_FULL
 *   7. total_payload_length > MSG_MAX_PAYLOAD_BYTES → ERR_INVALID
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions.
 *   - F-Prime style: assert() for test verification.
 */

// Verifies: REQ-3.2.3, REQ-3.3.3
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/ReassemblyBuffer.hpp"
#include "core/Fragmentation.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a fragment envelope
// ─────────────────────────────────────────────────────────────────────────────
static void make_fragment(MessageEnvelope& frag,
                           uint64_t         message_id,
                           NodeId           src,
                           uint8_t          frag_index,
                           uint8_t          frag_count,
                           uint16_t         total_payload_length,
                           const uint8_t*   payload,
                           uint32_t         payload_len)
{
    envelope_init(frag);
    frag.message_type         = MessageType::DATA;
    frag.source_id            = src;
    frag.destination_id       = 1U;
    frag.message_id           = message_id;
    frag.timestamp_us         = 1000000ULL;
    frag.expiry_time_us       = 5000000ULL;
    frag.priority             = 0U;
    frag.reliability_class    = ReliabilityClass::BEST_EFFORT;
    frag.fragment_index       = frag_index;
    frag.fragment_count       = frag_count;
    frag.total_payload_length = total_payload_length;
    frag.payload_length       = payload_len;
    if (payload != nullptr && payload_len > 0U) {
        (void)memcpy(frag.payload, payload, payload_len);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Single-fragment message (fragment_count == 1) completes immediately
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_reassembly_single_fragment()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t payload[] = { 0xAAU, 0xBBU, 0xCCU };
    MessageEnvelope frag;
    make_fragment(frag, 1ULL, 10U, 0U, 1U, 3U, payload, 3U);

    MessageEnvelope out;
    envelope_init(out);
    Result r = buf.ingest(frag, out);

    assert(r == Result::OK);
    assert(out.message_id == 1ULL);
    assert(out.payload_length == 3U);
    assert(out.payload[0] == 0xAAU);
    assert(out.payload[2] == 0xCCU);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Three fragments arrive in order (0, 1, 2) → reassembled correctly
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.3
static bool test_reassembly_in_order()
{
    ReassemblyBuffer buf;
    buf.init();

    // Build logical payload: 2500 bytes, value = index & 0xFF
    static uint8_t logical_payload[2500U];
    for (uint32_t i = 0U; i < 2500U; ++i) {
        logical_payload[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    // Manually produce three fragments
    MessageEnvelope f0;
    make_fragment(f0, 100ULL, 5U, 0U, 3U, 2500U,
                  &logical_payload[0U], FRAG_MAX_PAYLOAD_BYTES);

    MessageEnvelope f1;
    make_fragment(f1, 100ULL, 5U, 1U, 3U, 2500U,
                  &logical_payload[FRAG_MAX_PAYLOAD_BYTES], FRAG_MAX_PAYLOAD_BYTES);

    MessageEnvelope f2;
    uint32_t last_len = 2500U - 2U * FRAG_MAX_PAYLOAD_BYTES;
    make_fragment(f2, 100ULL, 5U, 2U, 3U, 2500U,
                  &logical_payload[static_cast<size_t>(2U) * FRAG_MAX_PAYLOAD_BYTES], last_len);

    MessageEnvelope out;
    envelope_init(out);

    Result r0 = buf.ingest(f0, out);
    assert(r0 == Result::ERR_AGAIN);

    Result r1 = buf.ingest(f1, out);
    assert(r1 == Result::ERR_AGAIN);

    Result r2 = buf.ingest(f2, out);
    assert(r2 == Result::OK);

    assert(out.payload_length == 2500U);
    // Verify first and last bytes
    assert(out.payload[0] == static_cast<uint8_t>(0U & 0xFFU));
    assert(out.payload[2499] == static_cast<uint8_t>(2499U & 0xFFU));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Fragments arrive out of order (2, 0, 1) → reassembled correctly
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.3
static bool test_reassembly_out_of_order()
{
    ReassemblyBuffer buf;
    buf.init();

    static uint8_t logical_payload[2500U];
    for (uint32_t i = 0U; i < 2500U; ++i) {
        logical_payload[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    uint32_t last_len = 2500U - 2U * FRAG_MAX_PAYLOAD_BYTES;

    MessageEnvelope f0;
    make_fragment(f0, 200ULL, 6U, 0U, 3U, 2500U,
                  &logical_payload[0U], FRAG_MAX_PAYLOAD_BYTES);
    MessageEnvelope f1;
    make_fragment(f1, 200ULL, 6U, 1U, 3U, 2500U,
                  &logical_payload[FRAG_MAX_PAYLOAD_BYTES], FRAG_MAX_PAYLOAD_BYTES);
    MessageEnvelope f2;
    make_fragment(f2, 200ULL, 6U, 2U, 3U, 2500U,
                  &logical_payload[static_cast<size_t>(2U) * FRAG_MAX_PAYLOAD_BYTES], last_len);

    MessageEnvelope out;
    envelope_init(out);

    // Arrive out of order: 2, 0, 1
    Result r2 = buf.ingest(f2, out);
    assert(r2 == Result::ERR_AGAIN);

    Result r0 = buf.ingest(f0, out);
    assert(r0 == Result::ERR_AGAIN);

    Result r1 = buf.ingest(f1, out);
    assert(r1 == Result::OK);

    assert(out.payload_length == 2500U);
    // Verify reassembled content
    assert(out.payload[0] == static_cast<uint8_t>(0U));
    assert(out.payload[1023] == static_cast<uint8_t>(1023U & 0xFFU));
    assert(out.payload[1024] == static_cast<uint8_t>(1024U & 0xFFU));
    assert(out.payload[2499] == static_cast<uint8_t>(2499U & 0xFFU));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Duplicate fragment silently ignored
// Fragments are FRAG_MAX_PAYLOAD_BYTES-sized to match the reassembly offset
// formula (byte_offset = fragment_index * FRAG_MAX_PAYLOAD_BYTES).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_reassembly_duplicate_fragment()
{
    ReassemblyBuffer buf;
    buf.init();

    // Two fragments, each FRAG_MAX_PAYLOAD_BYTES bytes, total = 2 * FRAG_MAX_PAYLOAD_BYTES
    // Fragment 0: fill 0xAA; Fragment 1: fill 0xBB
    static uint8_t p0[FRAG_MAX_PAYLOAD_BYTES];
    static uint8_t p1[FRAG_MAX_PAYLOAD_BYTES];
    for (uint32_t i = 0U; i < FRAG_MAX_PAYLOAD_BYTES; ++i) {
        p0[i] = 0xAAU;
        p1[i] = 0xBBU;
    }

    static const uint32_t TOTAL = 2U * FRAG_MAX_PAYLOAD_BYTES;

    MessageEnvelope f0;
    make_fragment(f0, 300ULL, 7U, 0U, 2U, static_cast<uint16_t>(TOTAL),
                  p0, FRAG_MAX_PAYLOAD_BYTES);
    MessageEnvelope f1;
    make_fragment(f1, 300ULL, 7U, 1U, 2U, static_cast<uint16_t>(TOTAL),
                  p1, FRAG_MAX_PAYLOAD_BYTES);

    MessageEnvelope out;
    envelope_init(out);

    // First ingestion of fragment 0
    Result r0 = buf.ingest(f0, out);
    assert(r0 == Result::ERR_AGAIN);

    // Duplicate fragment 0: should be ignored, not an error
    Result dup = buf.ingest(f0, out);
    assert(dup == Result::ERR_AGAIN);  // silently discarded, still waiting

    // Now ingest fragment 1: should complete
    Result r1 = buf.ingest(f1, out);
    assert(r1 == Result::OK);

    assert(out.payload_length == TOTAL);
    assert(out.payload[0] == 0xAAU);
    // Fragment 1 is placed at byte offset FRAG_MAX_PAYLOAD_BYTES
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES] == 0xBBU);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Inconsistent fragment_count → ERR_INVALID
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_reassembly_inconsistent_count_rejected()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p0[4U] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU };
    static const uint8_t p1[4U] = { 0x11U, 0x22U, 0x33U, 0x44U };

    // First fragment: fragment_count = 2
    MessageEnvelope f0;
    make_fragment(f0, 400ULL, 8U, 0U, 2U, 8U, p0, 4U);

    // Second fragment: fragment_count = 3 (inconsistent!)
    MessageEnvelope f1_bad;
    make_fragment(f1_bad, 400ULL, 8U, 1U, 3U, 8U, p1, 4U);

    MessageEnvelope out;
    envelope_init(out);

    Result r0 = buf.ingest(f0, out);
    assert(r0 == Result::ERR_AGAIN);

    // Inconsistent count should be rejected
    Result r1 = buf.ingest(f1_bad, out);
    assert(r1 == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Slot exhaustion → ERR_FULL
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_reassembly_slot_exhaustion()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p[4U] = { 0x01U, 0x02U, 0x03U, 0x04U };
    MessageEnvelope out;

    // Fill all REASSEMBLY_SLOT_COUNT slots with in-progress messages
    // Each uses message_id = slot_index + 1 and source_id = slot_index + 1
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        MessageEnvelope f;
        make_fragment(f, static_cast<uint64_t>(i) + 1ULL,
                      static_cast<NodeId>(i + 1U),
                      0U, 2U, 8U, p, 4U);
        envelope_init(out);
        Result r = buf.ingest(f, out);
        assert(r == Result::ERR_AGAIN);
    }

    // Next unique (source_id, message_id) pair should return ERR_FULL
    MessageEnvelope f_extra;
    make_fragment(f_extra, 999ULL,
                  static_cast<NodeId>(REASSEMBLY_SLOT_COUNT + 1U),
                  0U, 2U, 8U, p, 4U);
    envelope_init(out);
    Result r_full = buf.ingest(f_extra, out);
    assert(r_full == Result::ERR_FULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: total_payload_length > MSG_MAX_PAYLOAD_BYTES → ERR_INVALID
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_reassembly_payload_too_large_rejected()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p[4U] = { 0xFFU, 0xFEU, 0xFDU, 0xFCU };

    // total_payload_length exceeds MSG_MAX_PAYLOAD_BYTES
    uint16_t oversized = static_cast<uint16_t>(MSG_MAX_PAYLOAD_BYTES + 1U);
    MessageEnvelope f;
    make_fragment(f, 500ULL, 9U, 0U, 2U, oversized, p, 4U);

    MessageEnvelope out;
    envelope_init(out);
    Result r = buf.ingest(f, out);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    int failed = 0;

    if (!test_reassembly_single_fragment()) {
        printf("FAIL: test_reassembly_single_fragment\n"); ++failed;
    } else { printf("PASS: test_reassembly_single_fragment\n"); }

    if (!test_reassembly_in_order()) {
        printf("FAIL: test_reassembly_in_order\n"); ++failed;
    } else { printf("PASS: test_reassembly_in_order\n"); }

    if (!test_reassembly_out_of_order()) {
        printf("FAIL: test_reassembly_out_of_order\n"); ++failed;
    } else { printf("PASS: test_reassembly_out_of_order\n"); }

    if (!test_reassembly_duplicate_fragment()) {
        printf("FAIL: test_reassembly_duplicate_fragment\n"); ++failed;
    } else { printf("PASS: test_reassembly_duplicate_fragment\n"); }

    if (!test_reassembly_inconsistent_count_rejected()) {
        printf("FAIL: test_reassembly_inconsistent_count_rejected\n"); ++failed;
    } else { printf("PASS: test_reassembly_inconsistent_count_rejected\n"); }

    if (!test_reassembly_slot_exhaustion()) {
        printf("FAIL: test_reassembly_slot_exhaustion\n"); ++failed;
    } else { printf("PASS: test_reassembly_slot_exhaustion\n"); }

    if (!test_reassembly_payload_too_large_rejected()) {
        printf("FAIL: test_reassembly_payload_too_large_rejected\n"); ++failed;
    } else { printf("PASS: test_reassembly_payload_too_large_rejected\n"); }

    return (failed > 0) ? 1 : 0;
}
