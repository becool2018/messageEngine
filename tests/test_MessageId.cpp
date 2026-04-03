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
 * @file test_MessageId.cpp
 * @brief Dedicated unit tests for MessageIdGen: seed, monotonic increment, wraparound.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, ≥2 assertions per test, bounded loops.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-3.2.1
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstdint>
#include <cassert>

#include "core/MessageId.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: init with valid seed — first next() returns the seed value
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_init_seed()
{
    MessageIdGen gen;
    gen.init(42ULL);

    uint64_t id = gen.next();
    assert(id == 42ULL);          // first ID must equal the seed
    assert(id != 0ULL);           // must be non-zero
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: monotonic increment — successive calls return strictly increasing IDs
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_monotonic_increment()
{
    MessageIdGen gen;
    gen.init(100ULL);

    uint64_t prev = gen.next();
    assert(prev == 100ULL);        // first value matches seed

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 16U; ++i) {
        uint64_t curr = gen.next();
        assert(curr > prev);       // each ID is strictly greater than the last
        assert(curr != 0ULL);      // all IDs must be non-zero
        prev = curr;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: wraparound recovery — counter skips 0 after overflow to UINT64_MAX+1
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_wraparound_recovery()
{
    MessageIdGen gen;
    // Seed at UINT64_MAX: next() returns UINT64_MAX, then increments to 0
    // and the wraparound guard skips 0 → 1.
    gen.init(0xFFFFFFFFFFFFFFFFULL);

    uint64_t id1 = gen.next();
    assert(id1 == 0xFFFFFFFFFFFFFFFFULL);  // returns the seed (UINT64_MAX)
    assert(id1 != 0ULL);                    // non-zero

    uint64_t id2 = gen.next();
    assert(id2 == 1ULL);    // wrapped: 0 is skipped, generator resets to 1
    assert(id2 != 0ULL);    // must never return 0

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: seed boundary — minimum valid seed (1) is accepted
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_min_valid_seed()
{
    MessageIdGen gen;
    gen.init(1ULL);

    uint64_t id = gen.next();
    assert(id == 1ULL);    // first call returns the seed
    assert(id != 0ULL);    // non-zero invariant
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
        { "test_init_seed",            test_init_seed            },
        { "test_monotonic_increment",  test_monotonic_increment  },
        { "test_wraparound_recovery",  test_wraparound_recovery  },
        { "test_min_valid_seed",       test_min_valid_seed       },
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

    (void)printf("ALL MessageId tests passed (%u/%u).\n", passed, NUM_TESTS);
    assert(passed == NUM_TESTS);
    return 0;
}
