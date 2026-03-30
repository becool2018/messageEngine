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

    return (failed > 0) ? 1 : 0;
}
