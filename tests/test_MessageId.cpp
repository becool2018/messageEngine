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
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


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
// Test 5: Multiple independent generators do not share state
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_independent_generators()
{
    MessageIdGen gen_a;
    MessageIdGen gen_b;
    gen_a.init(1000ULL);
    gen_b.init(2000ULL);

    // Each generator produces its own independent sequence
    uint64_t a1 = gen_a.next();
    uint64_t b1 = gen_b.next();

    assert(a1 == 1000ULL);   // gen_a starts at its seed
    assert(b1 == 2000ULL);   // gen_b starts at its seed
    assert(a1 != b1);        // independent generators have different values

    // Interleave calls: neither should affect the other
    uint64_t a2 = gen_a.next();
    uint64_t b2 = gen_b.next();

    assert(a2 == 1001ULL);   // gen_a increments independently
    assert(b2 == 2001ULL);   // gen_b increments independently
    assert(a2 != b2);        // values remain distinct

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Re-initializing a generator resets its sequence
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.1
static bool test_reinit_resets_sequence()
{
    MessageIdGen gen;
    gen.init(500ULL);

    uint64_t id1 = gen.next();
    assert(id1 == 500ULL);    // first ID is the seed

    // Advance the generator a few steps
    (void)gen.next();  // 501
    (void)gen.next();  // 502

    // Re-initialize with the same seed: sequence resets
    gen.init(500ULL);
    uint64_t id_reset = gen.next();
    assert(id_reset == 500ULL);  // Assert: restart from seed after re-init
    assert(id_reset == id1);     // Assert: same as first call before re-init

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    // Initialize logger before any production code that may call LOG_* macros.
    // Power of 10: return value checked; failure causes abort via NEVER_COMPILED_OUT_ASSERT.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

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
        { "test_independent_generators", test_independent_generators },
        { "test_reinit_resets_sequence", test_reinit_resets_sequence },
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
