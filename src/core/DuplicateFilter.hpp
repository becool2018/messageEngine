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
 * @file DuplicateFilter.hpp
 * @brief Sliding-window duplicate suppression based on (source_id, message_id) pairs.
 *
 * Requirement traceability: CLAUDE.md §3.2 (duplicate suppression),
 * messageEngine/CLAUDE.md §3.3 (reliable delivery with dedupe).
 *
 * Rules applied:
 *   - Power of 10: fixed window (DEDUP_WINDOW_SIZE), no dynamic allocation,
 *     ≥2 assertions per method, loop bounds provable.
 *   - MISRA C++: no STL, no exceptions, ≤1 ptr indirection.
 *   - F-Prime style: simple ring-buffer state, age-based eviction.
 *   - SECfix-3: eviction policy changed from round-robin to oldest-first to prevent
 *     replay-after-flooding attacks (attacker floods 128 unique pairs to rotate the
 *     dedup window and re-enable a previously-recorded entry).
 *
 * Implements: REQ-3.2.6, REQ-3.3.3
 */

#ifndef CORE_DUPLICATE_FILTER_HPP
#define CORE_DUPLICATE_FILTER_HPP

#include <cstdint>
#include <cstring>
#include "Assert.hpp"
#include "Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// DuplicateFilter: sliding-window dedup based on (source_id, message_id)
// ─────────────────────────────────────────────────────────────────────────────

class DuplicateFilter {
public:
    /// Initialize the filter (zero all state).
    /// Must be called once during system initialization.
    void init();

    // Safety-critical (SC): HAZ-003 — verified to M5
    /// Check if a message (by source_id, message_id) is already in the window.
    /// Returns true if duplicate, false if new.
    bool is_duplicate(NodeId src, uint64_t msg_id) const;

    // Safety-critical (SC): HAZ-003 — verified to M5
    /// Record a message in the dedup window.
    /// If window is full (m_count == DEDUP_WINDOW_SIZE), evicts the oldest-by-time entry.
    /// @param now_us  Current time in microseconds (must be > 0); used for age-based eviction.
    /// SECfix-3: now_us enables oldest-first eviction to resist replay-after-flooding.
    void record(NodeId src, uint64_t msg_id, uint64_t now_us);

    /// Combined check-and-record in one call.
    /// Returns ERR_DUPLICATE if message is already known, OK otherwise.
    /// On OK, also records the message.
    /// @param now_us  Current time in microseconds (must be > 0); forwarded to record().
    // Safety-critical (SC): HAZ-003
    Result check_and_record(NodeId src, uint64_t msg_id, uint64_t now_us);

private:
    // Power of 10 rule 9: ≤1 pointer indirection; using fixed array, not pointers
    /// Entry in the dedup window
    struct Entry {
        NodeId    src;          // source node ID
        uint64_t  msg_id;       // message ID from that source
        uint64_t  recorded_us;  // timestamp when entry was recorded (SECfix-3: age-based eviction)
        bool      valid;        // true if this slot is occupied
    };

    // Power of 10 rule 3: fixed-size allocation at static scope
    Entry    m_window[DEDUP_WINDOW_SIZE] = {};  ///< circular buffer of entries
    uint32_t m_next  = 0U;                      ///< next write position (used on not-full path only)
    uint32_t m_count = 0U;                      ///< number of valid entries in window
};

#endif // CORE_DUPLICATE_FILTER_HPP
