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
 * Implements: REQ-3.2.1
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
    // Safety-critical (SC): HAZ-003
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

    // Safety-critical (SC): HAZ-003
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
    uint64_t m_next;
};

#endif // CORE_MESSAGE_ID_HPP
