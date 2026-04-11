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
 * @file test_AssertState.cpp
 * @brief Unit tests for AssertState: check_and_clear(), IResetHandler
 *        registration, and trigger_handler_for_test() dispatch.
 *
 * AssertState is NSC-infrastructure (CLAUDE.md §10 assertion policy).
 * No REQ-x.x applies; the "Verifies:" tag is intentionally omitted to
 * prevent an orphan reference in the traceability check.
 *
 * Branches covered:
 *   check_and_clear() False path: flag false → returns false, flag stays false.
 *   check_and_clear() True  path: flag true  → returns true,  flag cleared.
 *   get_reset_handler(): nullptr before registration; pointer after.
 *   trigger_handler_for_test(): dispatches to MockResetHandler; sets flag.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * NOTE: NEVER_COMPILED_OUT_ASSERT is NOT called here.  In debug/test builds
 * it calls ::abort(), terminating the process.  Handler dispatch is tested
 * via trigger_handler_for_test() which exercises the same dispatch path
 * without going through the macro.
 * The "no handler → ::abort()" fallback is not tested because triggering
 * abort() would terminate the test process; it is verified by code inspection.
 */
// Verification: M1 + M2 + M3 (NSC infrastructure)
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#include <cstdio>
#include <cassert>
#include <cstring>  // strcmp
#include <atomic>   // std::atomic — permitted carve-out (CLAUDE.md §3)

#include "core/AssertState.hpp"
#include "core/IResetHandler.hpp"
#include "core/AbortResetHandler.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MockResetHandler — records on_fatal_assert invocations without aborting.
// Used to verify handler dispatch; never registered for production use.
// Power of 10 Rule 9 exemption: test code (CLAUDE.md §9 table).
// ─────────────────────────────────────────────────────────────────────────────

struct MockResetHandler : public IResetHandler {
    bool        called     = false;
    const char* last_cond  = nullptr;
    const char* last_file  = nullptr;
    int         last_line  = 0;

    ~MockResetHandler() override {}

    void on_fatal_assert(const char* cond,
                         const char* file,
                         int         line) override
    {
        called    = true;
        last_cond = cond;
        last_file = file;
        last_line = line;
        // Does NOT abort — allows trigger_handler_for_test to set g_fatal_fired.
    }
};

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
// Test 4 — Handler initially null:
//   get_reset_handler() must return nullptr before any set_reset_handler call.
//   This test must run first (before any other test registers a handler).
// ─────────────────────────────────────────────────────────────────────────────
static void test_get_handler_initially_null()
{
    // No set_reset_handler has been called yet in this process.
    IResetHandler* h = assert_state::get_reset_handler();

    assert(h == nullptr);
    // Double-check: nullptr is falsy
    assert(!h);

    printf("PASS: test_get_handler_initially_null\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — set_reset_handler / get_reset_handler round-trip:
//   After registration, get_reset_handler() returns exactly the registered ptr.
// ─────────────────────────────────────────────────────────────────────────────
static void test_set_and_get_handler()
{
    MockResetHandler mock;
    assert_state::set_reset_handler(mock);

    IResetHandler* h = assert_state::get_reset_handler();

    assert(h != nullptr);
    assert(h == &mock);

    printf("PASS: test_set_and_get_handler\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — trigger_handler_for_test dispatches correctly:
//   MockResetHandler receives the exact cond, file, and line passed to the hook.
// ─────────────────────────────────────────────────────────────────────────────
static void test_trigger_dispatches_to_handler()
{
    MockResetHandler mock;
    assert_state::set_reset_handler(mock);
    assert_state::g_fatal_fired.store(false, std::memory_order_release);

    assert_state::trigger_handler_for_test("x > 0", "test.cpp", 42);

    assert(mock.called == true);
    assert(std::strcmp(mock.last_cond, "x > 0") == 0);
    assert(std::strcmp(mock.last_file, "test.cpp") == 0);
    assert(mock.last_line == 42);

    printf("PASS: test_trigger_dispatches_to_handler\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7 — trigger_handler_for_test sets g_fatal_fired:
//   After the handler returns, g_fatal_fired must be true.
//   This verifies the soft-reset flag path is exercised when a handler returns.
// ─────────────────────────────────────────────────────────────────────────────
static void test_trigger_sets_fatal_flag()
{
    MockResetHandler mock;
    assert_state::set_reset_handler(mock);
    assert_state::g_fatal_fired.store(false, std::memory_order_release);

    assert_state::trigger_handler_for_test("p != nullptr", "file.cpp", 99);

    assert(assert_state::g_fatal_fired.load(std::memory_order_acquire) == true);
    // Clean up so subsequent tests start with flag clear.
    assert_state::g_fatal_fired.store(false, std::memory_order_release);

    printf("PASS: test_trigger_sets_fatal_flag\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8 — AbortResetHandler is a valid IResetHandler:
//   AbortResetHandler::instance() must be upcastable to IResetHandler* and
//   survive a registration round-trip.  on_fatal_assert() is NOT called here
//   because it calls ::abort() — that path is verified by code inspection per
//   the POSIX rationale in AbortResetHandler.hpp.
// ─────────────────────────────────────────────────────────────────────────────
static void test_abort_handler_is_ireset_handler()
{
    // Implicit upcast: if AbortResetHandler does not publicly inherit IResetHandler,
    // this assignment would not compile.
    IResetHandler* h = &AbortResetHandler::instance();

    assert(h != nullptr);

    // Registration round-trip
    assert_state::set_reset_handler(AbortResetHandler::instance());
    assert(assert_state::get_reset_handler() == h);

    printf("PASS: test_abort_handler_is_ireset_handler\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9 — get_fatal_count() returns the cumulative fatal-event counter:
//   After trigger_handler_for_test() fires, g_fatal_count must be non-zero.
//   This exercises lines 86–88 (get_fatal_count body) which were previously
//   uncovered; the function is NSC-infrastructure (no REQ-x.x).
// ─────────────────────────────────────────────────────────────────────────────
static void test_get_fatal_count_nonzero_after_trigger()
{
    MockResetHandler mock;
    assert_state::set_reset_handler(mock);
    // Reset flag and count to a known baseline.
    assert_state::g_fatal_fired.store(false, std::memory_order_release);
    assert_state::g_fatal_count.store(0U, std::memory_order_release);

    assert_state::trigger_handler_for_test("count_test", "test.cpp", 1);

    // get_fatal_count() must return the incremented counter.
    uint32_t c = assert_state::get_fatal_count();
    assert(c >= 1U);  // Assert: counter incremented by trigger

    // Restore flag to avoid interfering with subsequent tests.
    assert_state::g_fatal_fired.store(false, std::memory_order_release);

    printf("PASS: test_get_fatal_count_nonzero_after_trigger\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    // Test 4 must run first — verifies nullptr before any registration.
    test_get_handler_initially_null();

    test_set_and_get_handler();
    test_trigger_dispatches_to_handler();
    test_trigger_sets_fatal_flag();
    test_abort_handler_is_ireset_handler();
    test_get_fatal_count_nonzero_after_trigger();

    // check_and_clear tests do not depend on handler state.
    test_check_and_clear_false();
    test_check_and_clear_true();
    test_check_and_clear_idempotent();

    printf("ALL AssertState tests passed.\n");
    return 0;
}
