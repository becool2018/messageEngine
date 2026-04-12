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
 * @file ConstantTime.hpp
 * @brief Timing-safe equality helpers for security-sensitive identifiers.
 *
 * These helpers replace plain == comparisons for (source_id, message_id) pairs
 * to prevent timing-oracle attacks (CWE-208) on dedup and ACK tracking paths.
 *
 * Technique: volatile XOR accumulator.  The volatile qualifier prevents the
 * compiler from short-circuiting evaluation or eliminating the XOR of each
 * field before the zero-test.  The XOR of all fields is accumulated into a
 * single result; equality iff result == 0.
 *
 * This is a project-internal approximation of mbedtls_ct_memcmp() that obeys
 * the src/core layering rule (core must not depend on platform-layer mbedTLS).
 * Any platform layer that can call mbedtls_ct_memcmp() directly should prefer
 * it over these helpers (see CLAUDE.md §7d).
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: no templates, no STL, no exceptions.
 *   - F-Prime style: pure inline helpers, no side effects.
 *
 * Implements: REQ-3.2.11
 */
// Implements: REQ-3.2.11

#ifndef CORE_CONSTANT_TIME_HPP
#define CORE_CONSTANT_TIME_HPP

#include <cstdint>
#include "Types.hpp"
#include "Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ct_node_id_equal — constant-time NodeId (uint32_t) equality
// HAZ-018: timing-oracle mitigation for source_id comparisons.
// ─────────────────────────────────────────────────────────────────────────────

/// Compare two NodeId values without early-exit on the first differing bit.
/// Returns true iff a == b.
// Safety-critical (SC): HAZ-018 — source_id equality on security-sensitive paths.
static inline bool ct_node_id_equal(NodeId a, NodeId b)
{
    NEVER_COMPILED_OUT_ASSERT(sizeof(NodeId) == 4U);  // Assert: NodeId is 32 bits (4 bytes)
    // volatile prevents the compiler from short-circuiting the XOR before the
    // zero-test (CWE-208 timing oracle mitigation; CLAUDE.md §7d).
    volatile uint32_t diff = static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b);
    // Assert 2: postcondition — result is consistent with the XOR accumulator.
    NEVER_COMPILED_OUT_ASSERT((diff == 0U) == (static_cast<uint32_t>(a) == static_cast<uint32_t>(b)));
    return (diff == 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// ct_id_pair_equal — constant-time (source_id, message_id) pair equality
// HAZ-018: primary mitigation for AckTracker and DuplicateFilter timing oracles.
// ─────────────────────────────────────────────────────────────────────────────

/// Compare (source_id, message_id) pairs without early-exit on any field.
/// Returns true iff src_a == src_b AND id_a == id_b.
// Safety-critical (SC): HAZ-018 — (source_id, message_id) pair equality on
// dedup and ACK-tracking paths.  Both fields are ORed into a single result so
// neither comparison can leak information through execution time (CWE-208).
static inline bool ct_id_pair_equal(NodeId src_a, NodeId src_b,
                                    uint64_t id_a, uint64_t id_b)
{
    NEVER_COMPILED_OUT_ASSERT(sizeof(NodeId) == 4U);  // Assert: NodeId is 32 bits (4 bytes)
    // Both fields are XORed and ORed into a single volatile accumulator so
    // the compiler cannot skip either comparison even when src already differs.
    volatile uint32_t src_diff = static_cast<uint32_t>(src_a) ^ static_cast<uint32_t>(src_b);
    volatile uint64_t id_diff  = id_a ^ id_b;
    volatile uint64_t combined = static_cast<uint64_t>(src_diff) | id_diff;
    // Assert 3: postcondition — combined == 0 iff both fields matched.
    NEVER_COMPILED_OUT_ASSERT((combined == 0ULL) == ((src_diff == 0U) && (id_diff == 0ULL)));
    return (combined == 0ULL);
}

#endif // CORE_CONSTANT_TIME_HPP
