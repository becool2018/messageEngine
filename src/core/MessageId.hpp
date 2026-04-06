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
 * @file MessageId.hpp
 * @brief Simple, deterministic message ID generator for sender-scoped uniqueness.
 *
 * Requirement traceability: CLAUDE.md §3.1 (message_id generation).
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, fixed bounds, ≥2 assertions per method.
 *   - MISRA C++: no STL, no exceptions, no templates.
 *   - F-Prime style: simple state machine, deterministic behavior.
 *
 * Design note: Each sender maintains its own MessageIdGen to ensure unique IDs.
 * We use a simple counter starting from a seed, never returning 0 (which is
 * reserved for invalid/uninitialized messages).
 *
 * The seed passed to init() must be derived from a cryptographically unpredictable
 * source (OS entropy via arc4random_buf / getrandom) as required by REQ-5.2.4.
 * A fixed literal seed or a time-only seed is prohibited in production — see
 * DeliveryEngine::init() for the compliant seed-construction path.
 *
 * Implements: REQ-3.2.1, REQ-5.2.4
 */

#ifndef CORE_MESSAGE_ID_HPP
#define CORE_MESSAGE_ID_HPP

#include <cstdint>
#include "Assert.hpp"
#include "Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Message ID generator (deterministic, seedable for testing)
// ─────────────────────────────────────────────────────────────────────────────

class MessageIdGen {
public:
    // Safety-critical (SC): HAZ-003 — verified to M5
    /// Initialize the generator with a seed value.
    /// Seed must be non-zero (0 is reserved for invalid messages).
    /// Called once during system initialization.
    void init(uint64_t seed)
    {
        // Power of 10 rule 7: assertion on input
        NEVER_COMPILED_OUT_ASSERT(seed != 0ULL);  // Assert: seed must be non-zero (0 reserved)

        m_next = seed;

        // Power of 10 rule 5: double-check post-condition
        NEVER_COMPILED_OUT_ASSERT(m_next == seed);  // Assert: initialization succeeded
    }

    // Safety-critical (SC): HAZ-003 — verified to M5
    /// Generate and return the next unique message ID.
    /// Returns a non-zero value; increments internal counter.
    /// Called for each outgoing message that requires a unique ID.
    uint64_t next()
    {
        // Power of 10 rule 5: pre-condition assertion
        NEVER_COMPILED_OUT_ASSERT(m_next != 0ULL);  // Assert: generator was initialized

        uint64_t result = m_next;

        // Increment; if we ever hit 0, skip to 1 (recovery from wraparound)
        // Power of 10 rule 2: simple, deterministic logic
        m_next++;
        if (m_next == 0ULL) {
            m_next = 1ULL;
        }

        // Power of 10 rule 5: post-condition assertion
        NEVER_COMPILED_OUT_ASSERT(result != 0ULL);  // Assert: returned ID is non-zero
        NEVER_COMPILED_OUT_ASSERT(m_next != 0ULL);   // Assert: next ID is valid

        return result;
    }

private:
    // Power of 10 rule 6: minimal scope, private member
    uint64_t m_next = 0ULL;
};

#endif // CORE_MESSAGE_ID_HPP
