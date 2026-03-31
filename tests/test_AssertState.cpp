/**
 * @file test_AssertState.cpp
 * @brief Unit tests for assert_state::check_and_clear() and g_fatal_fired.
 *
 * AssertState is NSC-infrastructure (CLAUDE.md §10 assertion policy).
 * No REQ-x.x applies; the "Verifies:" tag is intentionally omitted to
 * prevent an orphan reference in the traceability check.
 *
 * Branches covered in check_and_clear():
 *   False path: g_fatal_fired is false → returns false, flag stays false.
 *   True  path: g_fatal_fired is true  → returns true,  flag reset to false.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * NOTE: NEVER_COMPILED_OUT_ASSERT is NOT called here.  In debug/test builds
 * it calls ::abort(), which would terminate the test process.  Instead,
 * g_fatal_fired is manipulated directly to exercise both branches of
 * check_and_clear() without triggering abort().
 */
// Verification: M1 + M2 + M3 (NSC infrastructure)
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#include <cstdio>
#include <cassert>
#include <atomic>   // std::atomic — permitted carve-out (CLAUDE.md §3)

#include "core/AssertState.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — False path:
//   g_fatal_fired starts false; check_and_clear() returns false; flag stays false.
// ─────────────────────────────────────────────────────────────────────────────
static void test_check_and_clear_false()
{
    // Ensure flag is false before the test (prior tests may have left it clear)
    assert_state::g_fatal_fired.store(false, std::memory_order_release);

    // Under test: False path — flag was not set
    bool result = assert_state::check_and_clear();

    assert(result == false);
    // Flag must remain false after a false-path call
    assert(assert_state::g_fatal_fired.load(std::memory_order_acquire) == false);

    printf("PASS: test_check_and_clear_false\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — True path:
//   g_fatal_fired is set to true directly; check_and_clear() returns true and
//   atomically resets the flag to false.
// ─────────────────────────────────────────────────────────────────────────────
static void test_check_and_clear_true()
{
    // Simulate a fired assertion by setting the flag directly.
    // This avoids calling NEVER_COMPILED_OUT_ASSERT, which calls ::abort() in
    // debug/test builds (CLAUDE.md §10.1).
    assert_state::g_fatal_fired.store(true, std::memory_order_release);

    // Under test: True path — flag was set
    bool result = assert_state::check_and_clear();

    assert(result == true);
    // Flag must be atomically cleared after a true-path call
    assert(assert_state::g_fatal_fired.load(std::memory_order_acquire) == false);

    printf("PASS: test_check_and_clear_true\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — Idempotent clear:
//   After check_and_clear() returns true, a second call returns false,
//   confirming the flag is cleared and is not re-armed by the query itself.
// ─────────────────────────────────────────────────────────────────────────────
static void test_check_and_clear_idempotent()
{
    // Arm the flag
    assert_state::g_fatal_fired.store(true, std::memory_order_release);

    // First call clears and returns true
    bool first = assert_state::check_and_clear();
    assert(first == true);

    // Second call must return false — flag already cleared
    bool second = assert_state::check_and_clear();
    assert(second == false);

    // Flag remains false after second call
    assert(assert_state::g_fatal_fired.load(std::memory_order_acquire) == false);

    printf("PASS: test_check_and_clear_idempotent\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_check_and_clear_false();
    test_check_and_clear_true();
    test_check_and_clear_idempotent();

    printf("ALL AssertState tests passed.\n");
    return 0;
}
