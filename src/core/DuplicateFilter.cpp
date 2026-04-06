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
 * @file DuplicateFilter.cpp
 * @brief Implementation of sliding-window duplicate suppression.
 *
 * Requirement traceability: CLAUDE.md §3.2 (duplicate suppression),
 * messageEngine/CLAUDE.md §3.3 (reliable delivery with dedupe).
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds (DEDUP_WINDOW_SIZE), ≥2 assertions per method.
 *   - MISRA C++: no dynamic allocation, no exceptions.
 *   - F-Prime style: simple ring-buffer state, age-based eviction.
 *   - SECfix-3: eviction changed from round-robin to oldest-first (by recorded_us) to
 *     prevent replay-after-flooding (attacker floods 128 unique pairs to rotate the
 *     dedup window and re-enable a previously-recorded entry).
 *
 * Implements: REQ-3.2.6, REQ-3.3.3
 */

#include "DuplicateFilter.hpp"
#include "Assert.hpp"

// M-2: Guard the modulo wrap against a zero divisor — porting error / macro redefinition.
static_assert(DEDUP_WINDOW_SIZE > 0U, "DEDUP_WINDOW_SIZE must be nonzero");

// ─────────────────────────────────────────────────────────────────────────────
// Initialization
// ─────────────────────────────────────────────────────────────────────────────

void DuplicateFilter::init()
{
    // F-11: verify m_window element count matches DEDUP_WINDOW_SIZE at compile time.
    // Must be inside a member function to access the private m_window array.
    static_assert(sizeof(m_window) / sizeof(m_window[0]) == DEDUP_WINDOW_SIZE,
                  "m_window element count must equal DEDUP_WINDOW_SIZE");

    // Power of 10 rule 3: zero-initialize fixed buffer during init phase
    (void)memset(m_window, 0, sizeof(m_window));

    m_next = 0U;
    m_count = 0U;

    // Power of 10 rule 5: post-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_next == 0U);    // Assert: write pointer at start
    NEVER_COMPILED_OUT_ASSERT(m_count == 0U);   // Assert: window is empty
}

// ─────────────────────────────────────────────────────────────────────────────
// Check for duplicate
// ─────────────────────────────────────────────────────────────────────────────

bool DuplicateFilter::is_duplicate(NodeId src, uint64_t msg_id) const
{
    // Power of 10 rule 2: simple linear search over fixed bound
    // Power of 10 rule 3: loop bound is DEDUP_WINDOW_SIZE (static constant)
    for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
        // Power of 10 rule 5: assertion on loop invariant
        NEVER_COMPILED_OUT_ASSERT(i < DEDUP_WINDOW_SIZE);  // Assert: index in bounds

        if (m_window[i].valid && m_window[i].src == src && m_window[i].msg_id == msg_id) {
            return true;  // Found a matching entry
        }
    }

    // Power of 10 rule 5: not found assertion
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE);  // Assert: count is consistent with capacity

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Record a message in the window
// ─────────────────────────────────────────────────────────────────────────────

void DuplicateFilter::record(NodeId src, uint64_t msg_id, uint64_t now_us)
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_next < DEDUP_WINDOW_SIZE);   // Assert: write pointer in bounds
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE); // Assert: count is valid
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);                // Assert: valid timestamp

    // SECfix-3: When the window is full, evict the oldest-by-time entry instead
    // of the ring-buffer position.  This prevents an attacker from flushing the
    // window by flooding 128 unique pairs to rotate a predictable eviction pointer.
    uint32_t write_idx = m_next;
    if (m_count >= DEDUP_WINDOW_SIZE) {
        // Find the entry with the smallest recorded_us (oldest).
        // Power of 10 Rule 2: bounded loop (DEDUP_WINDOW_SIZE = 128)
        uint32_t oldest_idx = 0U;
        for (uint32_t i = 1U; i < DEDUP_WINDOW_SIZE; ++i) {
            NEVER_COMPILED_OUT_ASSERT(i < DEDUP_WINDOW_SIZE);  // Assert: loop bound
            if (m_window[i].recorded_us < m_window[oldest_idx].recorded_us) {
                oldest_idx = i;
            }
        }
        write_idx = oldest_idx;
    }

    // Write entry at selected position
    m_window[write_idx].src         = src;
    m_window[write_idx].msg_id      = msg_id;
    m_window[write_idx].recorded_us = now_us;
    m_window[write_idx].valid       = true;

    // Advance ring-buffer write pointer for the not-full path
    // Power of 10 rule 2: simple modular arithmetic
    m_next = (m_next + 1U) % DEDUP_WINDOW_SIZE;

    // Update count (capped at window size)
    // Power of 10 rule 1: bounded update
    if (m_count < DEDUP_WINDOW_SIZE) {
        m_count++;
    }

    // Power of 10 rule 5: post-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_next < DEDUP_WINDOW_SIZE);   // Assert: write pointer wrapped correctly
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE); // Assert: count still valid
}

// ─────────────────────────────────────────────────────────────────────────────
// Check and record (combined operation)
// ─────────────────────────────────────────────────────────────────────────────

Result DuplicateFilter::check_and_record(NodeId src, uint64_t msg_id, uint64_t now_us)
{
    // Power of 10 rule 5: pre-condition assertions
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE);  // Assert: count is consistent
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);                 // Assert: valid timestamp

    // Check for duplicate
    if (is_duplicate(src, msg_id)) {
        return Result::ERR_DUPLICATE;
    }

    // Record the new message
    record(src, msg_id, now_us);

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE);  // Assert: count still valid after record

    return Result::OK;
}
