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
 * @file test_DuplicateFilter.cpp
 * @brief Unit tests for DuplicateFilter deduplication logic.
 *
 * Tests the sliding-window duplicate detection based on (source_id, message_id)
 * pairs, including window wraparound and eviction behavior.
 *
 * Rules applied:
 *   - Power of 10: fixed window size, no dynamic allocation, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 */

// Verifies: REQ-3.2.6
// Verification: M1 + M2 + M4 + M5 (fault injection not required — pure logic, no external dependency)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/DuplicateFilter.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Basic dedup detection
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_basic_dedup()
{
    DuplicateFilter filter;
    filter.init();

    // Record a message (source=1, id=100)
    filter.record(1U, 100ULL);

    // Check if the same message is detected as duplicate
    bool is_dup = filter.is_duplicate(1U, 100ULL);
    assert(is_dup == true);

    // Combined check_and_record should return ERR_DUPLICATE
    Result r = filter.check_and_record(1U, 100ULL);
    assert(r == Result::ERR_DUPLICATE);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Different source IDs are not duplicates
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_different_src()
{
    DuplicateFilter filter;
    filter.init();

    // Record from source 1
    filter.record(1U, 100ULL);

    // Same message_id but different source should NOT be duplicate
    bool is_dup = filter.is_duplicate(2U, 100ULL);
    assert(is_dup == false);

    // check_and_record should succeed
    Result r = filter.check_and_record(2U, 100ULL);
    assert(r == Result::OK);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Different message IDs are not duplicates
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_different_id()
{
    DuplicateFilter filter;
    filter.init();

    // Record message 100 from source 1
    filter.record(1U, 100ULL);

    // Message 101 from same source should NOT be duplicate
    bool is_dup = filter.is_duplicate(1U, 101ULL);
    assert(is_dup == false);

    // check_and_record should succeed
    Result r = filter.check_and_record(1U, 101ULL);
    assert(r == Result::OK);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Unseen messages are not duplicates
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_not_seen()
{
    DuplicateFilter filter;
    filter.init();

    // Query for a message that was never seen
    bool is_dup = filter.is_duplicate(1U, 999ULL);
    assert(is_dup == false);

    // check_and_record should succeed for unseen message
    Result r = filter.check_and_record(1U, 999ULL);
    assert(r == Result::OK);

    // Now it should be seen
    is_dup = filter.is_duplicate(1U, 999ULL);
    assert(is_dup == true);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Window wraparound and eviction of oldest entries
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_window_wraparound()
{
    DuplicateFilter filter;
    filter.init();

    // Fill the window with DEDUP_WINDOW_SIZE distinct entries from source 1
    // Power of 10: bounded loop with explicit upper bound
    for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i);
        filter.record(1U, msg_id);
    }

    // Verify all are seen
    for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i);
        assert(filter.is_duplicate(1U, msg_id) == true);
    }

    // Add 5 more messages; oldest should be evicted
    for (uint32_t i = 0U; i < 5U; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(DEDUP_WINDOW_SIZE) + static_cast<uint64_t>(i);
        filter.record(1U, msg_id);
    }

    // Verify the oldest entries (0-4) are no longer in the window
    // (they should have been evicted)
    for (uint32_t i = 0U; i < 5U; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i);
        bool is_dup = filter.is_duplicate(1U, msg_id);
        assert(is_dup == false);  // evicted, so not a duplicate anymore
    }

    // Verify the newest entries are still present
    for (uint32_t i = 5U; i < DEDUP_WINDOW_SIZE + 5U; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i);
        bool is_dup = filter.is_duplicate(1U, msg_id);
        assert(is_dup == true);  // still in window
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Interleaved entries from multiple sources in the same window
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_multiple_sources_interleaved()
{
    DuplicateFilter filter;
    filter.init();

    // Record alternating entries from two sources
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < 10U; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i) + 1ULL;
        filter.record(1U, msg_id);
        filter.record(2U, msg_id);
    }

    // All recorded entries from both sources must be recognized as duplicates
    for (uint32_t i = 0U; i < 10U; ++i) {
        uint64_t msg_id = static_cast<uint64_t>(i) + 1ULL;
        assert(filter.is_duplicate(1U, msg_id) == true);  // source 1 entry present
        assert(filter.is_duplicate(2U, msg_id) == true);  // source 2 entry present
    }

    // Same message_id from a third source must NOT be a duplicate (not recorded)
    assert(filter.is_duplicate(3U, 1ULL) == false);  // source 3 never recorded

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Exact boundary — adding one entry to a full window evicts oldest
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_single_entry_eviction()
{
    DuplicateFilter filter;
    filter.init();

    // Fill the window exactly (DEDUP_WINDOW_SIZE entries, all from source 10)
    // Power of 10 Rule 2: bounded by DEDUP_WINDOW_SIZE constant
    for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
        filter.record(10U, static_cast<uint64_t>(i));
    }

    // Entry 0 must still be present (window exactly full, no eviction yet)
    assert(filter.is_duplicate(10U, 0ULL) == true);   // Assert: oldest still visible

    // Add one more: entry 0 is now evicted (oldest entry in FIFO ring)
    filter.record(10U, static_cast<uint64_t>(DEDUP_WINDOW_SIZE));

    assert(filter.is_duplicate(10U, 0ULL) == false);  // Assert: entry 0 evicted
    assert(filter.is_duplicate(10U, static_cast<uint64_t>(DEDUP_WINDOW_SIZE)) == true);  // newest present

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: check_and_record idempotence — second call on same ID is ERR_DUPLICATE
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.6
static bool test_check_and_record_idempotent()
{
    DuplicateFilter filter;
    filter.init();

    // First call: message not seen → OK and records it
    Result r1 = filter.check_and_record(5U, 77ULL);
    assert(r1 == Result::OK);  // Assert: first call succeeds

    // Second call: same message → ERR_DUPLICATE
    Result r2 = filter.check_and_record(5U, 77ULL);
    assert(r2 == Result::ERR_DUPLICATE);  // Assert: duplicate detected

    // Third call: still ERR_DUPLICATE (idempotent duplicate detection)
    Result r3 = filter.check_and_record(5U, 77ULL);
    assert(r3 == Result::ERR_DUPLICATE);  // Assert: still a duplicate

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    int failed = 0;

    if (!test_basic_dedup()) {
        printf("FAIL: test_basic_dedup\n");
        ++failed;
    } else {
        printf("PASS: test_basic_dedup\n");
    }

    if (!test_different_src()) {
        printf("FAIL: test_different_src\n");
        ++failed;
    } else {
        printf("PASS: test_different_src\n");
    }

    if (!test_different_id()) {
        printf("FAIL: test_different_id\n");
        ++failed;
    } else {
        printf("PASS: test_different_id\n");
    }

    if (!test_not_seen()) {
        printf("FAIL: test_not_seen\n");
        ++failed;
    } else {
        printf("PASS: test_not_seen\n");
    }

    if (!test_window_wraparound()) {
        printf("FAIL: test_window_wraparound\n");
        ++failed;
    } else {
        printf("PASS: test_window_wraparound\n");
    }

    if (!test_multiple_sources_interleaved()) {
        printf("FAIL: test_multiple_sources_interleaved\n");
        ++failed;
    } else {
        printf("PASS: test_multiple_sources_interleaved\n");
    }

    if (!test_single_entry_eviction()) {
        printf("FAIL: test_single_entry_eviction\n");
        ++failed;
    } else {
        printf("PASS: test_single_entry_eviction\n");
    }

    if (!test_check_and_record_idempotent()) {
        printf("FAIL: test_check_and_record_idempotent\n");
        ++failed;
    } else {
        printf("PASS: test_check_and_record_idempotent\n");
    }

    return (failed > 0) ? 1 : 0;
}
