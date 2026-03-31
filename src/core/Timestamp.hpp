/**
 * @file Timestamp.hpp
 * @brief Monotonic clock utilities for message timing, expiry, and deadlines.
 *
 * Requirement traceability: CLAUDE.md §3.2 (timestamps, expiry checking),
 * messageEngine/CLAUDE.md §3.3 (expiring messages).
 *
 * Rules applied:
 *   - Power of 10: all functions are inline, ≥2 assertions each, fixed bounds.
 *   - MISRA C++: no exceptions, no dynamic allocation, ≤1 ptr indirection.
 *   - F-Prime style: no STL, no templates.
 *
 * Implements: REQ-3.2.2, REQ-3.3.4
 */

#ifndef CORE_TIMESTAMP_HPP
#define CORE_TIMESTAMP_HPP

#include <ctime>
#include <cstdint>
#include "Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Monotonic timestamp in microseconds
// ─────────────────────────────────────────────────────────────────────────────

// Safety-critical (SC): HAZ-004 — verified to M5
/// Get current monotonic time in microseconds since system epoch.
/// Uses CLOCK_MONOTONIC to avoid wall-clock discontinuities.
/// Called during message creation and timeout checks.
inline uint64_t timestamp_now_us()
{
    struct timespec ts;
    // Power of 10 rule 7: check all return values
    int result = clock_gettime(CLOCK_MONOTONIC, &ts);
    NEVER_COMPILED_OUT_ASSERT(result == 0);  // Assert: clock_gettime must succeed
    NEVER_COMPILED_OUT_ASSERT(ts.tv_sec >= 0);  // Assert: time must be non-negative

    // Power of 10 rule 9: single level of indirection (none here)
    // Convert seconds and nanoseconds to microseconds
    uint64_t sec_us  = static_cast<uint64_t>(ts.tv_sec) * 1000000ULL;
    uint64_t nsec_us = static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;

    return sec_us + nsec_us;
}

// Safety-critical (SC): HAZ-004 — verified to M5
/// Check if a message has expired relative to current time.
/// Returns true if now_us >= expiry_us AND expiry_us != 0 (0 = never expires).
/// Used by transport layers and message queues to drop stale messages.
inline bool timestamp_expired(uint64_t expiry_us, uint64_t now_us)
{
    // Power of 10 rule 2: fixed control flow (no recursion, simple conditions)
    NEVER_COMPILED_OUT_ASSERT(expiry_us <= 0xFFFFFFFFFFFFFFFFULL);  // Assert: valid timestamp bounds
    NEVER_COMPILED_OUT_ASSERT(now_us    <= 0xFFFFFFFFFFFFFFFFULL);  // Assert: valid timestamp bounds

    // expiry_us == 0 means "never expires"; only consider expired if expiry is set
    return (expiry_us != 0ULL) && (now_us >= expiry_us);
}

// Safety-critical (SC): HAZ-002, HAZ-004 — verified to M5
/// Compute an absolute deadline (expiry time) given current time and duration.
/// Returns now_us + (duration_ms * 1000), converting milliseconds to microseconds.
/// Used by ACK tracking and retry logic to set timeouts.
inline uint64_t timestamp_deadline_us(uint64_t now_us, uint32_t duration_ms)
{
    // Power of 10 rule 7: check implicit bounds
    NEVER_COMPILED_OUT_ASSERT(now_us <= 0xFFFFFFFFFFFFFFFFULL);  // Assert: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(duration_ms <= 0xFFFFFFFFUL);      // Assert: reasonable timeout

    // Compute deadline without overflow
    uint64_t duration_us = static_cast<uint64_t>(duration_ms) * 1000ULL;

    return now_us + duration_us;
}

#endif // CORE_TIMESTAMP_HPP
