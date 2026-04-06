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
 * @file PrngEngine.hpp
 * @brief Seedable PRNG for deterministic impairment decisions.
 *
 * Uses the xorshift64 algorithm for fast, deterministic pseudorandom numbers.
 * Essential for testing: given the same seed, generates the same sequence.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, fixed state, ≥2 assertions per method.
 *   - MISRA C++: no STL, no exceptions, no templates.
 *   - Deterministic for reproducible test scenarios.
 *
 * Usage:
 *   PrngEngine prng;
 *   prng.seed(12345);
 *   double packet_loss = prng.next_double();  // [0.0, 1.0)
 *   uint32_t delay_ms = prng.next_range(10, 100);
 *
 * Implements: REQ-5.2.4, REQ-5.3.1
 */

#ifndef PLATFORM_PRNG_ENGINE_HPP
#define PLATFORM_PRNG_ENGINE_HPP

#include <cstdint>
#include "core/Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// PrngEngine: xorshift64-based PRNG
// ─────────────────────────────────────────────────────────────────────────────

class PrngEngine {
public:
    // Safety-critical (SC): HAZ-002, HAZ-007 — verified to M5
    /**
     * @brief Seed the PRNG.
     *
     * Sets the internal state. If seed is 0, it is coerced to 1 (0 is invalid
     * for xorshift64, as it would produce all zeros).
     *
     * @param[in] s Seed value (non-zero preferred; 0 is coerced to 1).
     */
    void seed(uint64_t s)
    {
        // Power of 10 rule 7: check and enforce precondition
        if (s == 0ULL) {
            m_state = 1ULL;  // xorshift64 requires non-zero state
        } else {
            m_state = s;
        }

        // Power of 10 rule 5: post-condition assertions
        NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL);  // Assert: state is non-zero
    }

    // Safety-critical (SC): HAZ-002, HAZ-007 — verified to M5
    /**
     * @brief Generate next pseudorandom number (64-bit).
     *
     * Implements xorshift64 algorithm:
     *   state ^= state << 13
     *   state ^= state >> 7
     *   state ^= state << 17
     *
     * @return Next 64-bit pseudorandom value.
     */
    uint64_t next()
    {
        // Power of 10 rule 5: precondition assertion
        NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL);  // Assert: state was initialized

        // xorshift64 transformations
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;

        // Power of 10 rule 5: postcondition assertion
        NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL);  // Assert: state remains non-zero

        return m_state;
    }

    // Safety-critical (SC): HAZ-002, HAZ-007 — verified to M5
    /**
     * @brief Generate a floating-point value in [0.0, 1.0).
     *
     * Scales next() to the range [0.0, 1.0) by dividing by UINT64_MAX.
     *
     * @return Pseudorandom double in [0.0, 1.0).
     */
    double next_double()
    {
        // Power of 10 rule 5: precondition assertion
        NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL);  // Assert: state initialized

        uint64_t raw = next();

        // Convert to double in [0.0, 1.0)
        // Power of 10 rule 9: single-level indirection (cast)
        double result = static_cast<double>(raw) / static_cast<double>(UINT64_MAX);

        // Power of 10 rule 5: postcondition assertion
        NEVER_COMPILED_OUT_ASSERT(result >= 0.0 && result < 1.0);  // Assert: result in valid range

        return result;
    }

    // Safety-critical (SC): HAZ-002, HAZ-007 — verified to M5
    /**
     * @brief Generate a pseudorandom integer in [lo, hi].
     *
     * Uses modulo arithmetic: lo + (next() % range64), where range64 is
     * computed as uint64_t to prevent unsigned wrap when hi == UINT32_MAX
     * and lo == 0 (which would produce range==0 and trigger divide-by-zero
     * UB if range were kept as uint32_t — CERT INT33-C).
     *
     * range64 is always in [1, 2^32] for valid hi >= lo inputs, so it is
     * never zero.  The modulo result fits in uint32_t: when range64 == 2^32
     * the result spans [0, UINT32_MAX], all of which fit; for smaller
     * range64 the result is strictly less than range64 <= UINT32_MAX.
     *
     * @param[in] lo Lower bound (inclusive).
     * @param[in] hi Upper bound (inclusive).
     * @return Pseudorandom value in [lo, hi].
     */
    uint32_t next_range(uint32_t lo, uint32_t hi)
    {
        // Power of 10 rule 5: precondition assertions
        NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL);  // Assert: state initialized
        NEVER_COMPILED_OUT_ASSERT(hi >= lo);          // Assert: valid range

        uint64_t raw = next();

        // CERT INT33-C: compute range as uint64_t to prevent unsigned wrap
        // when hi == UINT32_MAX and lo == 0 (uint32_t result would be 0,
        // causing divide-by-zero UB).  Cast both operands to uint64_t before
        // subtraction so no intermediate result can wrap.
        uint64_t range64 = static_cast<uint64_t>(hi)
                         - static_cast<uint64_t>(lo)
                         + 1ULL;

        // Assert: range is non-zero for all valid hi >= lo inputs (CERT INT33-C).
        NEVER_COMPILED_OUT_ASSERT(range64 > 0ULL);

        // Safe narrowing: raw % range64 < range64.  When range64 == 2^32,
        // raw % range64 is in [0, UINT32_MAX], which fits in uint32_t.
        // For all smaller range64 the value is strictly less than UINT32_MAX.
        // MISRA C++:2023: static_cast used for all conversions (no C-style casts).
        uint32_t offset = static_cast<uint32_t>(raw % range64);
        uint32_t result = lo + offset;

        // Power of 10 rule 5: postcondition assertion
        NEVER_COMPILED_OUT_ASSERT(result >= lo && result <= hi);  // Assert: result in valid range

        return result;
    }

private:
    // Power of 10 rule 6: minimal scope, single private state
    uint64_t m_state;
};

#endif // PLATFORM_PRNG_ENGINE_HPP
