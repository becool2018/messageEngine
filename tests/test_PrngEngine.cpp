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
 * @file test_PrngEngine.cpp
 * @brief Unit tests for PrngEngine: seed, next(), next_double(), next_range().
 *
 * Covers:
 *   - seed() with a valid nonzero value
 *   - seed(0) coerced to 1 (xorshift64 invariant)
 *   - Deterministic reproducibility: same seed → same sequence
 *   - next() never returns 0
 *   - next_double() always in [0.0, 1.0)
 *   - next_range(lo, hi) always in [lo, hi]
 *   - next_range(lo, lo) always returns lo (degenerate single-value range)
 *   - Two independent engines seeded identically produce the same sequence
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-5.2.4, REQ-5.3.1
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstdint>
#include <cassert>
#include <climits>   // UINT64_MAX

#include "platform/PrngEngine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: seed() with a valid nonzero value — state is set to that value
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_seed_nonzero()
{
    PrngEngine prng;
    prng.seed(12345ULL);

    // The first next() call advances state; verify we get a deterministic value
    // by comparing against a second engine seeded identically.
    PrngEngine prng2;
    prng2.seed(12345ULL);

    uint64_t v1 = prng.next();
    uint64_t v2 = prng2.next();

    assert(v1 != 0ULL);   // xorshift64 must never produce 0
    assert(v1 == v2);     // same seed → same first output

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: seed(0) is coerced to 1 — same sequence as seed(1)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_seed_zero_coerced()
{
    PrngEngine prng_zero;
    prng_zero.seed(0ULL);  // must be coerced to state=1

    PrngEngine prng_one;
    prng_one.seed(1ULL);   // explicit seed=1 for comparison

    // Both must produce the same sequence because state is the same.
    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 8U; ++i) {
        uint64_t v0 = prng_zero.next();
        uint64_t v1 = prng_one.next();
        assert(v0 != 0ULL);  // state invariant: must never be zero
        assert(v0 == v1);    // coercion makes both identical
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: next() never returns 0
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_never_zero()
{
    PrngEngine prng;
    prng.seed(42ULL);

    // Power of 10 rule 2: fixed loop bound (1024 iterations)
    for (uint32_t i = 0U; i < 1024U; ++i) {
        uint64_t v = prng.next();
        assert(v != 0ULL);   // xorshift64 invariant: state must never be 0
        (void)v;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Deterministic reproducibility — same seed produces same sequence
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4, REQ-5.3.1
static bool test_deterministic_sequence()
{
    PrngEngine a;
    PrngEngine b;
    a.seed(999999ULL);
    b.seed(999999ULL);

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 32U; ++i) {
        uint64_t va = a.next();
        uint64_t vb = b.next();
        assert(va == vb);    // must match exactly
        assert(va != 0ULL);  // state invariant
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Different seeds produce different first values
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_different_seeds_differ()
{
    PrngEngine a;
    PrngEngine b;
    a.seed(1ULL);
    b.seed(2ULL);

    uint64_t va = a.next();
    uint64_t vb = b.next();

    assert(va != 0ULL);   // state invariant
    assert(vb != 0ULL);   // state invariant
    assert(va != vb);     // distinct seeds must yield distinct first outputs

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: next_double() always in [0.0, 1.0)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_double_range()
{
    PrngEngine prng;
    prng.seed(777ULL);

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 1024U; ++i) {
        double d = prng.next_double();
        assert(d >= 0.0);   // lower bound (inclusive)
        assert(d < 1.0);    // upper bound (exclusive)
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: next_double() with seed=1 produces a value in [0.0, 1.0)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_double_seed_one()
{
    PrngEngine prng;
    prng.seed(1ULL);

    double d = prng.next_double();
    assert(d >= 0.0);  // lower bound
    assert(d < 1.0);   // upper bound

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: next_range(lo, hi) always returns a value in [lo, hi]
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_range_bounds()
{
    PrngEngine prng;
    prng.seed(314159ULL);

    static const uint32_t LO = 10U;
    static const uint32_t HI = 100U;

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 1024U; ++i) {
        uint32_t v = prng.next_range(LO, HI);
        assert(v >= LO);  // must be at or above lower bound
        assert(v <= HI);  // must be at or below upper bound
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: next_range(lo, lo) always returns lo (degenerate single-value range)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_range_degenerate()
{
    PrngEngine prng;
    prng.seed(2718281ULL);

    static const uint32_t VAL = 42U;

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 32U; ++i) {
        uint32_t v = prng.next_range(VAL, VAL);
        assert(v == VAL);  // only possible result in a single-element range
        (void)v;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: next_range with a large (but non-overflowing) span [0, UINT32_MAX-1]
//
// Note: next_range(0, UINT32_MAX) overflows the internal range computation
// (hi - lo + 1 wraps to 0) and is therefore not a valid input.  The largest
// valid call is next_range(0, UINT32_MAX - 1).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_range_wide_span()
{
    PrngEngine prng;
    prng.seed(161803398ULL);

    static const uint32_t WIDE_HI = UINT32_MAX - 1U;  // largest non-overflowing hi

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 16U; ++i) {
        uint32_t v = prng.next_range(0U, WIDE_HI);
        assert(v <= WIDE_HI);  // must stay within [0, WIDE_HI]
        (void)v;
    }

    // Verify with a large lo as well
    PrngEngine prng2;
    prng2.seed(1ULL);
    static const uint32_t LARGE_LO = 0xFFFFFFF0U;
    static const uint32_t LARGE_HI = 0xFFFFFFFEU;  // hi - lo + 1 = 15, no overflow
    uint32_t v = prng2.next_range(LARGE_LO, LARGE_HI);
    assert(v >= LARGE_LO);  // lower bound
    assert(v <= LARGE_HI);  // upper bound

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: next_range coverage — lo = 0, hi = 1 (binary range)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4
static bool test_next_range_binary()
{
    PrngEngine prng;
    prng.seed(11235813ULL);

    bool saw_zero = false;
    bool saw_one  = false;

    // Power of 10 rule 2: fixed loop bound; 64 iterations should hit both values
    for (uint32_t i = 0U; i < 64U; ++i) {
        uint32_t v = prng.next_range(0U, 1U);
        assert(v <= 1U);  // must stay in [0, 1]
        if (v == 0U) { saw_zero = true; }
        if (v == 1U) { saw_one  = true; }
    }

    // Both values must be reachable with a uniform PRNG over 64 iterations
    assert(saw_zero);
    assert(saw_one);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: Re-seeding resets the sequence
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-5.2.4, REQ-5.3.1
static bool test_reseed_resets_sequence()
{
    PrngEngine prng;
    prng.seed(500ULL);

    // Advance the state several times
    for (uint32_t i = 0U; i < 8U; ++i) {
        (void)prng.next();
    }

    // Re-seed to the same value: must reproduce the original sequence
    prng.seed(500ULL);

    PrngEngine ref;
    ref.seed(500ULL);

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < 16U; ++i) {
        uint64_t vp = prng.next();
        uint64_t vr = ref.next();
        assert(vp == vr);   // re-seed must replay the same sequence
        assert(vp != 0ULL); // state invariant
    }

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
        { "test_seed_nonzero",          test_seed_nonzero          },
        { "test_seed_zero_coerced",     test_seed_zero_coerced     },
        { "test_next_never_zero",       test_next_never_zero       },
        { "test_deterministic_sequence",test_deterministic_sequence},
        { "test_different_seeds_differ",test_different_seeds_differ},
        { "test_next_double_range",     test_next_double_range     },
        { "test_next_double_seed_one",  test_next_double_seed_one  },
        { "test_next_range_bounds",     test_next_range_bounds     },
        { "test_next_range_degenerate", test_next_range_degenerate },
        { "test_next_range_wide_span",  test_next_range_wide_span  },
        { "test_next_range_binary",     test_next_range_binary     },
        { "test_reseed_resets_sequence",test_reseed_resets_sequence},
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

    (void)printf("ALL PrngEngine tests passed (%u/%u).\n", passed, NUM_TESTS);
    assert(passed == NUM_TESTS);
    return 0;
}
