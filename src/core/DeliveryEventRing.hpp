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
 * @file DeliveryEventRing.hpp
 * @brief Bounded, overwrite-on-full ring buffer for DeliveryEvent records.
 *
 * Provides a pull-style observability log: push() never blocks and never fails
 * (overwrites the oldest event when full). pop() returns ERR_EMPTY when no
 * events are available. size() and clear() are provided for diagnostics and
 * test control.
 *
 * Design constraints:
 *   - Power of 10 Rule 3: no dynamic allocation; storage is a fixed array
 *     of DELIVERY_EVENT_RING_CAPACITY elements.
 *   - Power of 10 Rule 2: all loops have statically provable upper bounds.
 *   - Power of 10 Rule 5: ≥2 assertions per function.
 *   - No STL, no templates, no exceptions (F-Prime subset).
 *   - Single-threaded access only (no locks); matches DeliveryEngine's
 *     single-threaded ownership model.
 *
 * NSC: observability infrastructure; no effect on delivery state.
 *
 * Implements: REQ-7.2.5
 */
// Implements: REQ-7.2.5

#ifndef CORE_DELIVERY_EVENT_RING_HPP
#define CORE_DELIVERY_EVENT_RING_HPP

#include <cstdint>
#include <cstring>
#include "Types.hpp"
#include "DeliveryEvent.hpp"
#include "Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEventRing
// ─────────────────────────────────────────────────────────────────────────────

/// Bounded, overwrite-on-full ring buffer holding DeliveryEvent records.
/// Capacity is DELIVERY_EVENT_RING_CAPACITY (compile-time constant in Types.hpp).
/// NSC: observability only; no effect on delivery state.
class DeliveryEventRing {
public:
    /// Zero-initialize all storage and indices. Must be called once before use.
    void init()
    {
        // Power of 10 Rule 5: ≥2 assertions
        NEVER_COMPILED_OUT_ASSERT(DELIVERY_EVENT_RING_CAPACITY > 0U);   // invariant: non-zero capacity
        // Power of 10 Rule 5: second assertion — head starts at slot zero (pre-use invariant)
        NEVER_COMPILED_OUT_ASSERT(DELIVERY_EVENT_RING_CAPACITY <= 256U); // invariant: practical upper bound

        (void)memset(m_buf, 0, sizeof(m_buf));
        m_head  = 0U;   // index of next slot to write
        m_tail  = 0U;   // index of next slot to read
        m_count = 0U;   // number of valid events in ring

        // Power of 10 Rule 5: post-condition
        NEVER_COMPILED_OUT_ASSERT(m_count == 0U);   // ring is empty after init
    }

    /// Push one event into the ring.
    /// If the ring is full, the oldest entry is overwritten (ring semantics).
    /// Never blocks; never returns an error.
    /// Power of 10 Rule 3: uses pre-allocated m_buf; no heap.
    void push(const DeliveryEvent& ev)
    {
        // Power of 10 Rule 5: ≥2 assertions
        NEVER_COMPILED_OUT_ASSERT(m_head < DELIVERY_EVENT_RING_CAPACITY);   // head in bounds
        NEVER_COMPILED_OUT_ASSERT(m_count <= DELIVERY_EVENT_RING_CAPACITY);  // count bounded

        // Write into the slot at m_head
        (void)memcpy(&m_buf[m_head], &ev, sizeof(DeliveryEvent));

        // Advance head with wrap
        m_head = (m_head + 1U) % DELIVERY_EVENT_RING_CAPACITY;

        if (m_count < DELIVERY_EVENT_RING_CAPACITY) {
            ++m_count;
        } else {
            // Ring full: oldest entry was just overwritten; advance tail to
            // keep it pointing at the new oldest entry.
            m_tail = (m_tail + 1U) % DELIVERY_EVENT_RING_CAPACITY;
        }

        // Power of 10 Rule 5: post-condition
        NEVER_COMPILED_OUT_ASSERT(m_count <= DELIVERY_EVENT_RING_CAPACITY);  // count still bounded
        NEVER_COMPILED_OUT_ASSERT(m_head < DELIVERY_EVENT_RING_CAPACITY);    // head still in bounds
    }

    /// Pop the oldest event out of the ring into @p out.
    /// @return OK on success; ERR_EMPTY if the ring is empty.
    Result pop(DeliveryEvent& out)
    {
        // Power of 10 Rule 5: ≥2 assertions
        NEVER_COMPILED_OUT_ASSERT(m_tail < DELIVERY_EVENT_RING_CAPACITY);   // tail in bounds
        NEVER_COMPILED_OUT_ASSERT(m_count <= DELIVERY_EVENT_RING_CAPACITY);  // count bounded

        if (m_count == 0U) {
            return Result::ERR_EMPTY;
        }

        (void)memcpy(&out, &m_buf[m_tail], sizeof(DeliveryEvent));
        m_tail = (m_tail + 1U) % DELIVERY_EVENT_RING_CAPACITY;
        --m_count;

        // Power of 10 Rule 5: post-condition
        NEVER_COMPILED_OUT_ASSERT(m_count < DELIVERY_EVENT_RING_CAPACITY);  // count did not overflow
        NEVER_COMPILED_OUT_ASSERT(m_tail < DELIVERY_EVENT_RING_CAPACITY);   // tail still in bounds

        return Result::OK;
    }

    /// Return the number of unread events currently in the ring.
    uint32_t size() const
    {
        // Power of 10 Rule 5: ≥2 assertions
        NEVER_COMPILED_OUT_ASSERT(m_count <= DELIVERY_EVENT_RING_CAPACITY);   // count bounded
        NEVER_COMPILED_OUT_ASSERT(m_head  <  DELIVERY_EVENT_RING_CAPACITY);   // head in bounds

        return m_count;
    }

private:
    // Power of 10 Rule 3: fixed-size array; no dynamic allocation after init.
    DeliveryEvent m_buf[DELIVERY_EVENT_RING_CAPACITY] = {};

    uint32_t m_head  = 0U;  ///< Next write index (0 … DELIVERY_EVENT_RING_CAPACITY-1)
    uint32_t m_tail  = 0U;  ///< Next read index  (0 … DELIVERY_EVENT_RING_CAPACITY-1)
    uint32_t m_count = 0U;  ///< Current number of valid events (0 … DELIVERY_EVENT_RING_CAPACITY)
};

#endif // CORE_DELIVERY_EVENT_RING_HPP
