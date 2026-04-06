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

// Verifies: REQ-3.2.3, REQ-3.3.3, REQ-3.2.9
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
    Result r = buf.ingest(frag, out, 1000000ULL);

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

    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);

    Result r1 = buf.ingest(f1, out, 1000000ULL);
    assert(r1 == Result::ERR_AGAIN);

    Result r2 = buf.ingest(f2, out, 1000000ULL);
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
    Result r2 = buf.ingest(f2, out, 1000000ULL);
    assert(r2 == Result::ERR_AGAIN);

    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);

    Result r1 = buf.ingest(f1, out, 1000000ULL);
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
    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);

    // Duplicate fragment 0: should be ignored, not an error
    Result dup = buf.ingest(f0, out, 1000000ULL);
    assert(dup == Result::ERR_AGAIN);  // silently discarded, still waiting

    // Now ingest fragment 1: should complete
    Result r1 = buf.ingest(f1, out, 1000000ULL);
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

    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);

    // Inconsistent count should be rejected
    Result r1 = buf.ingest(f1_bad, out, 1000000ULL);
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
        Result r = buf.ingest(f, out, 1000000ULL);
        assert(r == Result::ERR_AGAIN);
    }

    // Next unique (source_id, message_id) pair should return ERR_FULL
    MessageEnvelope f_extra;
    make_fragment(f_extra, 999ULL,
                  static_cast<NodeId>(REASSEMBLY_SLOT_COUNT + 1U),
                  0U, 2U, 8U, p, 4U);
    envelope_init(out);
    Result r_full = buf.ingest(f_extra, out, 1000000ULL);
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
    Result r = buf.ingest(f, out, 1000000ULL);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: Short per-fragment payloads do not expose stale bytes on reassembly
//
// Verifies the Bug 1 fix: open_slot() must zero the accumulation buffer so
// that when a second message reuses the slot, bytes from the first message are
// not visible in the output even if the new message's fragments carry fewer
// payload bytes than FRAG_MAX_PAYLOAD_BYTES.
//
// Scenario:
//   1. Send a 2-fragment message (msg A) that fills the slot with 0xFF bytes.
//      Each fragment is FRAG_MAX_PAYLOAD_BYTES long; total = 2 * FRAG_MAX_PAYLOAD_BYTES.
//   2. Reassemble msg A → slot is freed.
//   3. Send a 2-fragment message (msg B) using the same slot.
//      Fragment 0 carries FRAG_MAX_PAYLOAD_BYTES bytes of 0xAA.
//      Fragment 1 carries only 4 bytes of 0xBB (short last fragment).
//      total_payload_length = FRAG_MAX_PAYLOAD_BYTES + 4.
//   4. After reassembly, bytes [FRAG_MAX_PAYLOAD_BYTES .. FRAG_MAX_PAYLOAD_BYTES+3]
//      must all be 0xBB.  Without the fix they would be 0xFF (stale from msg A).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.3
static bool test_reassembly_short_last_frag_no_stale_bytes()
{
    ReassemblyBuffer buf;
    buf.init();

    // ── Message A: two full-size fragments, fill 0xFF ────────────────────────
    static uint8_t fa_payload[FRAG_MAX_PAYLOAD_BYTES];
    for (uint32_t i = 0U; i < FRAG_MAX_PAYLOAD_BYTES; ++i) {
        fa_payload[i] = 0xFFU;
    }
    static const uint32_t TOTAL_A = 2U * FRAG_MAX_PAYLOAD_BYTES;

    MessageEnvelope fa0;
    make_fragment(fa0, 800ULL, 11U, 0U, 2U, static_cast<uint16_t>(TOTAL_A),
                  fa_payload, FRAG_MAX_PAYLOAD_BYTES);
    MessageEnvelope fa1;
    make_fragment(fa1, 800ULL, 11U, 1U, 2U, static_cast<uint16_t>(TOTAL_A),
                  fa_payload, FRAG_MAX_PAYLOAD_BYTES);

    MessageEnvelope out;
    envelope_init(out);

    Result ra0 = buf.ingest(fa0, out, 1000000ULL);
    assert(ra0 == Result::ERR_AGAIN);  // Assert: first fragment held
    Result ra1 = buf.ingest(fa1, out, 1000000ULL);
    assert(ra1 == Result::OK);         // Assert: message A assembled
    assert(out.payload_length == TOTAL_A);
    assert(out.payload[0] == 0xFFU);
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES - 1U] == 0xFFU);

    // ── Message B: fragment 0 full-size (0xAA), fragment 1 short (0xBB) ─────
    static const uint32_t SHORT_LEN = 4U;
    static const uint32_t TOTAL_B   = FRAG_MAX_PAYLOAD_BYTES + SHORT_LEN;

    static uint8_t fb0_payload[FRAG_MAX_PAYLOAD_BYTES];
    for (uint32_t i = 0U; i < FRAG_MAX_PAYLOAD_BYTES; ++i) {
        fb0_payload[i] = 0xAAU;
    }
    static const uint8_t fb1_payload[SHORT_LEN] = { 0xBBU, 0xBBU, 0xBBU, 0xBBU };

    MessageEnvelope fb0;
    make_fragment(fb0, 801ULL, 11U, 0U, 2U, static_cast<uint16_t>(TOTAL_B),
                  fb0_payload, FRAG_MAX_PAYLOAD_BYTES);
    MessageEnvelope fb1;
    make_fragment(fb1, 801ULL, 11U, 1U, 2U, static_cast<uint16_t>(TOTAL_B),
                  fb1_payload, SHORT_LEN);

    envelope_init(out);
    Result rb0 = buf.ingest(fb0, out, 1000000ULL);
    assert(rb0 == Result::ERR_AGAIN);  // Assert: fragment 0 held

    Result rb1 = buf.ingest(fb1, out, 1000000ULL);
    assert(rb1 == Result::OK);         // Assert: message B assembled

    assert(out.payload_length == TOTAL_B);  // Assert: correct total length

    // Fragment 0 bytes must be 0xAA
    assert(out.payload[0] == 0xAAU);
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES - 1U] == 0xAAU);

    // Fragment 1 bytes: without the fix these would be 0xFF (stale from message A)
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES + 0U] == 0xBBU);
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES + 1U] == 0xBBU);
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES + 2U] == 0xBBU);
    assert(out.payload[FRAG_MAX_PAYLOAD_BYTES + 3U] == 0xBBU);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: record_fragment offset boundary (F4 assertion must not fire)
// Verifies: REQ-3.2.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_record_fragment_offset_boundary()
{
    ReassemblyBuffer buf;
    buf.init();

    // Build a 4-fragment message with fragment_index = FRAG_MAX_COUNT-1 (maximum)
    // total_payload_length = FRAG_MAX_COUNT * FRAG_MAX_PAYLOAD_BYTES (maximum valid)
    static const uint32_t TOTAL = FRAG_MAX_COUNT * FRAG_MAX_PAYLOAD_BYTES;
    static const uint8_t p[4U] = { 0x01U, 0x02U, 0x03U, 0x04U };

    // Send all four fragments, ending with the max-index one
    MessageEnvelope f0;
    make_fragment(f0, 9000ULL, 20U, 0U, static_cast<uint8_t>(FRAG_MAX_COUNT),
                  static_cast<uint16_t>(TOTAL), p, 4U);
    MessageEnvelope f1;
    make_fragment(f1, 9000ULL, 20U, 1U, static_cast<uint8_t>(FRAG_MAX_COUNT),
                  static_cast<uint16_t>(TOTAL), p, 4U);
    MessageEnvelope f2;
    make_fragment(f2, 9000ULL, 20U, 2U, static_cast<uint8_t>(FRAG_MAX_COUNT),
                  static_cast<uint16_t>(TOTAL), p, 4U);
    // Last fragment: fragment_index = FRAG_MAX_COUNT - 1
    MessageEnvelope f3;
    make_fragment(f3, 9000ULL, 20U, static_cast<uint8_t>(FRAG_MAX_COUNT - 1U),
                  static_cast<uint8_t>(FRAG_MAX_COUNT),
                  static_cast<uint16_t>(TOTAL), p, 4U);

    MessageEnvelope out;
    envelope_init(out);

    // Ingest first three fragments
    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);  // Assert: slot opened, not complete
    Result r1 = buf.ingest(f1, out, 1000000ULL);
    assert(r1 == Result::ERR_AGAIN);
    Result r2 = buf.ingest(f2, out, 1000000ULL);
    assert(r2 == Result::ERR_AGAIN);

    // Ingest the maximum-index fragment: assertion in record_fragment must not fire
    Result r3 = buf.ingest(f3, out, 1000000ULL);
    assert(r3 == Result::OK);         // Assert: no crash, reassembly complete

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: sweep_stale returns 0 when threshold is zero (no-op)
// Verifies: REQ-3.2.9
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.9
static bool test_sweep_stale_no_op_when_threshold_zero()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p[4U] = { 0x01U, 0x02U, 0x03U, 0x04U };
    MessageEnvelope f0;
    make_fragment(f0, 10000ULL, 30U, 0U, 2U, 8U, p, 4U);

    MessageEnvelope out;
    envelope_init(out);

    // Inject first fragment of a 2-fragment message — slot opens
    Result r = buf.ingest(f0, out, 1000000ULL);
    assert(r == Result::ERR_AGAIN);  // Assert: slot opened, waiting for fragment 1

    // Threshold == 0 must be a no-op regardless of elapsed time
    uint32_t freed = buf.sweep_stale(1000000ULL + 1000000ULL, 0U);
    assert(freed == 0U);  // Assert: no slots freed when threshold is zero

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: sweep_stale frees an open slot and it becomes reusable
// Verifies: REQ-3.2.9
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.9
static bool test_sweep_stale_frees_open_slot()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p[4U] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU };
    // Two fragments; send only the first to leave the slot open
    MessageEnvelope f0;
    make_fragment(f0, 11000ULL, 31U, 0U, 2U, 8U, p, 4U);
    MessageEnvelope f1;
    make_fragment(f1, 11000ULL, 31U, 1U, 2U, 8U, p, 4U);

    MessageEnvelope out;
    envelope_init(out);

    // Open the slot at T = 1000000 us
    static const uint64_t T = 1000000ULL;
    Result r0 = buf.ingest(f0, out, T);
    assert(r0 == Result::ERR_AGAIN);  // Assert: slot opened

    // Sweep at T + 5000000 with threshold 4000000 — slot is stale
    uint32_t freed = buf.sweep_stale(T + 5000000ULL, 4000000ULL);
    assert(freed == 1U);  // Assert: one stale slot freed

    // Inject both fragments fresh — slot must be reusable
    Result ra = buf.ingest(f0, out, T + 5000001ULL);
    assert(ra == Result::ERR_AGAIN);  // Assert: new slot opened

    Result rb = buf.ingest(f1, out, T + 5000001ULL);
    assert(rb == Result::OK);         // Assert: reassembly completes (slot was reused)

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: sweep_stale prevents slot exhaustion (all slots freed, 9th fits)
// Verifies: REQ-3.2.9, REQ-3.2.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.9, REQ-3.2.3
static bool test_sweep_stale_prevents_slot_exhaustion()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p[4U] = { 0x11U, 0x22U, 0x33U, 0x44U };
    static const uint64_t OPEN_TIME = 1000000ULL;
    static const uint64_t THRESHOLD = 4000000ULL;

    MessageEnvelope out;

    // Fill all REASSEMBLY_SLOT_COUNT slots with distinct in-progress messages
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        MessageEnvelope f;
        make_fragment(f, static_cast<uint64_t>(12000U) + static_cast<uint64_t>(i),
                      static_cast<NodeId>(40U + i),
                      0U, 2U, 8U, p, 4U);
        envelope_init(out);
        Result r = buf.ingest(f, out, OPEN_TIME);
        assert(r == Result::ERR_AGAIN);  // Assert: slot opened
    }

    // Confirm all slots are taken — 9th distinct message must fail
    MessageEnvelope f_extra;
    make_fragment(f_extra, 99999ULL,
                  static_cast<NodeId>(40U + REASSEMBLY_SLOT_COUNT),
                  0U, 2U, 8U, p, 4U);
    envelope_init(out);
    Result r_full = buf.ingest(f_extra, out, OPEN_TIME);
    assert(r_full == Result::ERR_FULL);  // Assert: all slots occupied

    // Sweep stale — all slots are beyond threshold
    uint32_t freed = buf.sweep_stale(OPEN_TIME + THRESHOLD + 1ULL, THRESHOLD);
    assert(freed == REASSEMBLY_SLOT_COUNT);  // Assert: all stale slots freed

    // Inject the previously-rejected fragment — must now succeed
    envelope_init(out);
    Result r_retry = buf.ingest(f_extra, out, OPEN_TIME + THRESHOLD + 2ULL);
    assert(r_retry == Result::ERR_AGAIN);  // Assert: slot now available (waiting for frag 1)

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: sweep_expired() frees slots whose expiry_us has passed
// Verifies: REQ-3.2.3, REQ-3.3.3
//
// Scenario:
//   1. Open two multi-fragment slots with different expiry times.
//   2. Call sweep_expired() at a time between the two expiries.
//   3. The earlier-expiring slot is freed; the later-expiring slot is kept.
//   4. After sweeping, a new fragment for the freed slot's message_id opens
//      a fresh slot (slot becomes reusable).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.3
static bool test_sweep_expired_frees_expired_slot()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p4[4U] = { 0x11U, 0x22U, 0x33U, 0x44U };

    // Fragment A (message_id=20000): expiry at 1 000 000 us
    MessageEnvelope fA0;
    make_fragment(fA0, 20000ULL, 50U, 0U, 2U, 8U, p4, 4U);
    fA0.expiry_time_us = 1000000ULL;

    // Fragment B (message_id=20001): expiry at 3 000 000 us
    MessageEnvelope fB0;
    make_fragment(fB0, 20001ULL, 51U, 0U, 2U, 8U, p4, 4U);
    fB0.expiry_time_us = 3000000ULL;

    MessageEnvelope out;
    envelope_init(out);

    // Ingest first fragments of both messages (both slots open)
    Result rA = buf.ingest(fA0, out, 500000ULL);
    assert(rA == Result::ERR_AGAIN);  // Assert: slot A opened, waiting for frag 1

    Result rB = buf.ingest(fB0, out, 500000ULL);
    assert(rB == Result::ERR_AGAIN);  // Assert: slot B opened, waiting for frag 1

    // Sweep at 2 000 000 us: slot A (expiry=1 000 000) must be freed;
    // slot B (expiry=3 000 000) must survive.
    buf.sweep_expired(2000000ULL);

    // Slot A was freed: re-injecting fA0 must open a new slot (ERR_AGAIN, not ERR_FULL)
    MessageEnvelope fA0_retry;
    make_fragment(fA0_retry, 20000ULL, 50U, 0U, 2U, 8U, p4, 4U);
    fA0_retry.expiry_time_us = 9000000ULL;  // fresh expiry
    Result rA_retry = buf.ingest(fA0_retry, out, 2000001ULL);
    assert(rA_retry == Result::ERR_AGAIN);  // Assert: new slot allocated for A

    // Slot B still active: its second fragment should complete reassembly
    MessageEnvelope fB1;
    make_fragment(fB1, 20001ULL, 51U, 1U, 2U, 8U, p4, 4U);
    fB1.expiry_time_us = 3000000ULL;
    Result rB1 = buf.ingest(fB1, out, 2000001ULL);
    assert(rB1 == Result::OK);  // Assert: slot B completed (was not swept)
    assert(out.source_id == 51U);  // Assert: reassembled from correct source

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: sweep_expired() — expiry_us == 0 means never-expires (not freed)
// Verifies: REQ-3.2.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_sweep_expired_zero_expiry_never_freed()
{
    ReassemblyBuffer buf;
    buf.init();

    static const uint8_t p4[4U] = { 0xAAU, 0xBBU, 0xCCU, 0xDDU };

    // Fragment with expiry_us == 0 (never expires)
    MessageEnvelope f0;
    make_fragment(f0, 30000ULL, 60U, 0U, 2U, 8U, p4, 4U);
    f0.expiry_time_us = 0ULL;  // 0 = never expires

    MessageEnvelope out;
    envelope_init(out);

    Result r0 = buf.ingest(f0, out, 1000000ULL);
    assert(r0 == Result::ERR_AGAIN);  // Assert: slot opened

    // Sweep at a very large time — should NOT free the slot (expiry == 0)
    buf.sweep_expired(0xFFFFFFFFFFFFFFFFULL);

    // The slot must still be alive: second fragment should complete reassembly
    MessageEnvelope f1;
    make_fragment(f1, 30000ULL, 60U, 1U, 2U, 8U, p4, 4U);
    f1.expiry_time_us = 0ULL;
    Result r1 = buf.ingest(f1, out, 2000000ULL);
    assert(r1 == Result::OK);      // Assert: reassembly completed (slot survived sweep)
    assert(out.source_id == 60U);  // Assert: correct source

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

    if (!test_reassembly_short_last_frag_no_stale_bytes()) {
        printf("FAIL: test_reassembly_short_last_frag_no_stale_bytes\n"); ++failed;
    } else { printf("PASS: test_reassembly_short_last_frag_no_stale_bytes\n"); }

    if (!test_record_fragment_offset_boundary()) {
        printf("FAIL: test_record_fragment_offset_boundary\n"); ++failed;
    } else { printf("PASS: test_record_fragment_offset_boundary\n"); }

    if (!test_sweep_stale_no_op_when_threshold_zero()) {
        printf("FAIL: test_sweep_stale_no_op_when_threshold_zero\n"); ++failed;
    } else { printf("PASS: test_sweep_stale_no_op_when_threshold_zero\n"); }

    if (!test_sweep_stale_frees_open_slot()) {
        printf("FAIL: test_sweep_stale_frees_open_slot\n"); ++failed;
    } else { printf("PASS: test_sweep_stale_frees_open_slot\n"); }

    if (!test_sweep_stale_prevents_slot_exhaustion()) {
        printf("FAIL: test_sweep_stale_prevents_slot_exhaustion\n"); ++failed;
    } else { printf("PASS: test_sweep_stale_prevents_slot_exhaustion\n"); }

    if (!test_sweep_expired_frees_expired_slot()) {
        printf("FAIL: test_sweep_expired_frees_expired_slot\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_frees_expired_slot\n"); }

    if (!test_sweep_expired_zero_expiry_never_freed()) {
        printf("FAIL: test_sweep_expired_zero_expiry_never_freed\n"); ++failed;
    } else { printf("PASS: test_sweep_expired_zero_expiry_never_freed\n"); }

    return (failed > 0) ? 1 : 0;
}
