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
 * @file test_Fragmentation.cpp
 * @brief Unit tests for the Fragmentation module.
 *
 * Tests fragment_message() and needs_fragmentation() covering:
 *   - Single-fragment (payload fits in one frame)
 *   - Multi-fragment (payload spans several frames)
 *   - Exact boundary (payload == FRAG_MAX_PAYLOAD_BYTES)
 *   - Maximum payload (payload == MSG_MAX_PAYLOAD_BYTES)
 *   - Oversized input (payload > MSG_MAX_PAYLOAD_BYTES → rejected)
 *   - Last-fragment shorter than FRAG_MAX_PAYLOAD_BYTES
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions.
 *   - F-Prime style: assert() for test verification.
 */

// Verifies: REQ-3.2.3, REQ-3.3.5
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Fragmentation.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// Fixed output buffer for all tests (Power of 10 rule 3: no dynamic allocation)
static MessageEnvelope g_out_frags[FRAG_MAX_COUNT];

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a DATA envelope with given payload length
// ─────────────────────────────────────────────────────────────────────────────
static void make_logical(MessageEnvelope& env, uint32_t payload_len, uint8_t fill)
{
    envelope_init(env);
    env.message_type         = MessageType::DATA;
    env.source_id            = 1U;
    env.destination_id       = 2U;
    env.message_id           = 42ULL;
    env.timestamp_us         = 1000000ULL;
    env.expiry_time_us       = 5000000ULL;
    env.priority             = 0U;
    env.reliability_class    = ReliabilityClass::BEST_EFFORT;
    env.sequence_num         = 7U;
    env.fragment_index       = 0U;
    env.fragment_count       = 1U;
    env.payload_length       = payload_len;
    env.total_payload_length = static_cast<uint16_t>(payload_len);

    // Fill payload with pattern
    for (uint32_t i = 0U; i < payload_len; ++i) {
        env.payload[i] = static_cast<uint8_t>((fill + i) & 0xFFU);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: needs_fragmentation — payload <= FRAG_MAX_PAYLOAD_BYTES → false
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.3.5
static bool test_frag_single_fragment()
{
    MessageEnvelope logical;
    make_logical(logical, FRAG_MAX_PAYLOAD_BYTES, 0xABU);

    // Should NOT need fragmentation
    assert(!needs_fragmentation(logical));

    // fragment_message should produce exactly 1 fragment
    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == 1U);

    // Verify the single fragment
    assert(g_out_frags[0].fragment_index == 0U);
    assert(g_out_frags[0].fragment_count == 1U);
    assert(g_out_frags[0].payload_length == FRAG_MAX_PAYLOAD_BYTES);
    assert(g_out_frags[0].total_payload_length == static_cast<uint16_t>(FRAG_MAX_PAYLOAD_BYTES));
    assert(g_out_frags[0].sequence_num == 7U);
    assert(g_out_frags[0].message_id == 42ULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Multi-fragment — 2500-byte payload → 3 fragments
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.5
static bool test_frag_multi_fragment()
{
    static const uint32_t PAYLOAD_LEN = 2500U;
    MessageEnvelope logical;
    make_logical(logical, PAYLOAD_LEN, 0x11U);

    assert(needs_fragmentation(logical));

    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    // 2500 / 1024 = 2 full + 452 remainder → 3 fragments
    assert(count == 3U);

    // Verify indices and counts
    for (uint32_t i = 0U; i < count; ++i) {
        assert(g_out_frags[i].fragment_index == static_cast<uint8_t>(i));
        assert(g_out_frags[i].fragment_count == static_cast<uint8_t>(count));
        assert(g_out_frags[i].total_payload_length == static_cast<uint16_t>(PAYLOAD_LEN));
        assert(g_out_frags[i].message_id == 42ULL);
        assert(g_out_frags[i].sequence_num == 7U);
    }

    // First two fragments full, last shorter
    assert(g_out_frags[0].payload_length == FRAG_MAX_PAYLOAD_BYTES);
    assert(g_out_frags[1].payload_length == FRAG_MAX_PAYLOAD_BYTES);
    assert(g_out_frags[2].payload_length == PAYLOAD_LEN - 2U * FRAG_MAX_PAYLOAD_BYTES);

    // Verify total bytes sum to PAYLOAD_LEN
    uint32_t total = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
        total += g_out_frags[i].payload_length;
    }
    assert(total == PAYLOAD_LEN);

    // Verify payload content: fragment 0 should start with 0x11
    assert(g_out_frags[0].payload[0] == 0x11U);
    // Fragment 1 starts at offset 1024
    assert(g_out_frags[1].payload[0] == static_cast<uint8_t>((0x11U + 1024U) & 0xFFU));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Exact boundary — payload == FRAG_MAX_PAYLOAD_BYTES → 1 fragment
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.5
static bool test_frag_exact_boundary()
{
    MessageEnvelope logical;
    make_logical(logical, FRAG_MAX_PAYLOAD_BYTES, 0x55U);

    // Exactly at boundary: NOT fragmented
    assert(!needs_fragmentation(logical));

    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == 1U);
    assert(g_out_frags[0].payload_length == FRAG_MAX_PAYLOAD_BYTES);
    assert(g_out_frags[0].fragment_count == 1U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Maximum payload — MSG_MAX_PAYLOAD_BYTES → FRAG_MAX_COUNT fragments
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.5
static bool test_frag_max_payload()
{
    MessageEnvelope logical;
    // MSG_MAX_PAYLOAD_BYTES = 4096, FRAG_MAX_PAYLOAD_BYTES = 1024 → exactly 4
    make_logical(logical, MSG_MAX_PAYLOAD_BYTES, 0xCCU);

    assert(needs_fragmentation(logical));

    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == FRAG_MAX_COUNT);

    // Verify each fragment is exactly FRAG_MAX_PAYLOAD_BYTES
    for (uint32_t i = 0U; i < count; ++i) {
        assert(g_out_frags[i].payload_length == FRAG_MAX_PAYLOAD_BYTES);
        assert(g_out_frags[i].fragment_count == static_cast<uint8_t>(FRAG_MAX_COUNT));
    }

    // Total must equal MSG_MAX_PAYLOAD_BYTES
    uint32_t total = 0U;
    for (uint32_t i = 0U; i < count; ++i) {
        total += g_out_frags[i].payload_length;
    }
    assert(total == MSG_MAX_PAYLOAD_BYTES);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Oversized payload — payload > MSG_MAX_PAYLOAD_BYTES → returns 0
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_frag_oversized_rejected()
{
    MessageEnvelope logical;
    envelope_init(logical);
    logical.message_type   = MessageType::DATA;
    logical.source_id      = 1U;
    // Manually set payload_length to overflow value (violates contract; guard must catch it)
    logical.payload_length = MSG_MAX_PAYLOAD_BYTES + 1U;

    // fragment_message should return 0 (error)
    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == 0U);

    // needs_fragmentation would assert on payload > MSG_MAX_PAYLOAD_BYTES,
    // so we only test fragment_message directly here.
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Last fragment shorter — 1100-byte payload → 2 fragments
//         First = 1024, last = 76
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.5
static bool test_frag_last_shorter()
{
    static const uint32_t PAYLOAD_LEN = 1100U;
    static const uint32_t LAST_LEN    = 76U;  // 1100 - 1024 = 76
    MessageEnvelope logical;
    make_logical(logical, PAYLOAD_LEN, 0x77U);

    assert(needs_fragmentation(logical));

    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == 2U);

    assert(g_out_frags[0].payload_length == FRAG_MAX_PAYLOAD_BYTES);
    assert(g_out_frags[1].payload_length == LAST_LEN);

    // Verify total bytes
    uint32_t total = g_out_frags[0].payload_length + g_out_frags[1].payload_length;
    assert(total == PAYLOAD_LEN);

    // Verify payload continuity: first byte of fragment 1 = 1024th byte of payload
    assert(g_out_frags[1].payload[0] == static_cast<uint8_t>((0x77U + 1024U) & 0xFFU));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Zero-payload message — fragment_message produces one empty fragment
//   Covers: line 67 True branch (frag_count = 1U path for zero payload)
//           line 97 False branch (frag_payload == 0 → memcpy skipped)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.3.5
static bool test_frag_zero_payload()
{
    // Verifies: REQ-3.2.3, REQ-3.3.5
    MessageEnvelope logical;
    envelope_init(logical);
    logical.message_type   = MessageType::DATA;
    logical.source_id      = 1U;
    logical.payload_length = 0U;  // zero payload

    // needs_fragmentation must return false for a zero-length payload
    assert(!needs_fragmentation(logical));

    // fragment_message must produce exactly one empty fragment (zero-payload path)
    uint32_t count = fragment_message(logical, g_out_frags, FRAG_MAX_COUNT);
    assert(count == 1U);  // Assert: exactly one fragment produced
    assert(g_out_frags[0].payload_length == 0U);  // Assert: fragment carries no payload
    assert(g_out_frags[0].fragment_index  == 0U);
    assert(g_out_frags[0].fragment_count  == 1U);
    assert(g_out_frags[0].total_payload_length == 0U);

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

    if (!test_frag_single_fragment()) {
        printf("FAIL: test_frag_single_fragment\n"); ++failed;
    } else { printf("PASS: test_frag_single_fragment\n"); }

    if (!test_frag_multi_fragment()) {
        printf("FAIL: test_frag_multi_fragment\n"); ++failed;
    } else { printf("PASS: test_frag_multi_fragment\n"); }

    if (!test_frag_exact_boundary()) {
        printf("FAIL: test_frag_exact_boundary\n"); ++failed;
    } else { printf("PASS: test_frag_exact_boundary\n"); }

    if (!test_frag_max_payload()) {
        printf("FAIL: test_frag_max_payload\n"); ++failed;
    } else { printf("PASS: test_frag_max_payload\n"); }

    if (!test_frag_oversized_rejected()) {
        printf("FAIL: test_frag_oversized_rejected\n"); ++failed;
    } else { printf("PASS: test_frag_oversized_rejected\n"); }

    if (!test_frag_last_shorter()) {
        printf("FAIL: test_frag_last_shorter\n"); ++failed;
    } else { printf("PASS: test_frag_last_shorter\n"); }

    if (!test_frag_zero_payload()) {
        printf("FAIL: test_frag_zero_payload\n"); ++failed;
    } else { printf("PASS: test_frag_zero_payload\n"); }

    return (failed > 0) ? 1 : 0;
}
