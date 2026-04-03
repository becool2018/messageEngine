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
 * @file test_Timestamp.cpp
 * @brief Dedicated unit tests for Timestamp utilities: expired true/false,
 *        expiry==0 never-expires policy, and deadline_us arithmetic.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, ≥2 assertions per test, bounded loops.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-3.2.2, REQ-3.3.4
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstdint>
#include <cassert>

#include "core/Timestamp.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: not expired when now < expiry
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2
static bool test_expired_false_before_deadline()
{
    uint64_t expiry_us = 1000000ULL;  // 1 second
    uint64_t now_us    =  999999ULL;  // 1 µs before expiry

    bool result = timestamp_expired(expiry_us, now_us);
    assert(!result);              // should NOT be expired
    assert(now_us < expiry_us);   // sanity: now is before expiry
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: expired when now == expiry (boundary: exactly at deadline)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2
static bool test_expired_true_at_deadline()
{
    uint64_t expiry_us = 1000000ULL;
    uint64_t now_us    = 1000000ULL;  // exactly at expiry

    bool result = timestamp_expired(expiry_us, now_us);
    assert(result);               // should be expired (now >= expiry)
    assert(now_us == expiry_us);  // sanity
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: expired when now > expiry
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2
static bool test_expired_true_past_deadline()
{
    uint64_t expiry_us = 1000000ULL;
    uint64_t now_us    = 1000001ULL;  // 1 µs past expiry

    bool result = timestamp_expired(expiry_us, now_us);
    assert(result);               // should be expired
    assert(now_us > expiry_us);   // sanity
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: expiry == 0 means "never expires" regardless of now
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2, REQ-3.3.4
static bool test_expiry_zero_never_expires()
{
    // now_us = very large value; expiry = 0 (sentinel: never expires)
    uint64_t expiry_us = 0ULL;
    uint64_t now_us    = 0xFFFFFFFFFFFFFFFFULL;

    bool result = timestamp_expired(expiry_us, now_us);
    assert(!result);              // expiry==0 must NEVER be considered expired
    assert(expiry_us == 0ULL);    // sanity: zero expiry is the sentinel
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: timestamp_deadline_us computes now + duration_ms * 1000
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2
static bool test_deadline_arithmetic()
{
    uint64_t now_us      = 5000000ULL;  // 5 s
    uint32_t duration_ms =    2000U;    // 2 s = 2 000 000 µs

    uint64_t deadline = timestamp_deadline_us(now_us, duration_ms);
    assert(deadline == 7000000ULL);               // 5 s + 2 s = 7 s
    assert(deadline > now_us);                    // deadline is in the future
    assert(deadline == now_us + 2000000ULL);      // exact value check
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: deadline with zero duration == now (degenerate case)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.2
static bool test_deadline_zero_duration()
{
    uint64_t now_us      = 9999999ULL;
    uint32_t duration_ms = 0U;

    uint64_t deadline = timestamp_deadline_us(now_us, duration_ms);
    assert(deadline == now_us);    // zero duration → deadline == now
    assert(deadline != 0ULL);      // sanity: non-zero result
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    struct TestCase {
        const char* name;
        bool (*fn)();
    };

    // Power of 10 rule 2: fixed-size table; loop bound is array length
    static const TestCase tests[] = {
        { "test_expired_false_before_deadline", test_expired_false_before_deadline },
        { "test_expired_true_at_deadline",      test_expired_true_at_deadline      },
        { "test_expired_true_past_deadline",    test_expired_true_past_deadline    },
        { "test_expiry_zero_never_expires",     test_expiry_zero_never_expires     },
        { "test_deadline_arithmetic",           test_deadline_arithmetic           },
        { "test_deadline_zero_duration",        test_deadline_zero_duration        },
    };
    static const uint32_t NUM_TESTS =
        static_cast<uint32_t>(sizeof(tests) / sizeof(tests[0]));

    uint32_t passed = 0U;
    for (uint32_t i = 0U; i < NUM_TESTS; ++i) {
        bool ok = tests[i].fn();
        if (ok) {
            (void)printf("PASS: %s\n", tests[i].name);
            ++passed;
        } else {
            (void)printf("FAIL: %s\n", tests[i].name);
        }
    }

    (void)printf("ALL Timestamp tests passed (%u/%u).\n", passed, NUM_TESTS);
    assert(passed == NUM_TESTS);
    return 0;
}
