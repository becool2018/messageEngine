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
 * @file RingLogSink.hpp
 * @brief Test double for ILogSink — fixed-size ring buffer that captures log
 *        lines for in-test assertions.
 *
 * Replaces fragile stderr redirection. Used in test_logger.cpp for
 * T-2.1 through T-2.14, T-5.1 through T-5.3.
 *
 * Verifies: REQ-7.1.1, REQ-7.2.1
 * Verification: M1 + M2 + M4 + M5 (fault injection via ILogSink)
 *
 * Power of 10 Rule 9 exception: virtual dispatch via ILogSink vtable —
 * the approved mechanism. No explicit function pointers.
 */

#ifndef TESTS_RINGLOGSINK_HPP
#define TESTS_RINGLOGSINK_HPP

#include <cstdint>
#include <cstring>  // strncpy, memset
#include "core/ILogSink.hpp"

/// Capacity: number of log lines held in the ring.
static const uint32_t RING_LOG_SINK_CAPACITY = 32U;
/// Maximum bytes per captured log line (including NUL terminator).
static const uint32_t RING_LOG_SINK_LINE_MAX = 512U;

/// Fixed-size ring buffer capturing log lines for test assertions.
/// When the ring is full the oldest entry is overwritten.
/// Not thread-safe — intended for single-threaded test use only.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class RingLogSink final : public ILogSink {
public:
    RingLogSink() : m_head(0U), m_count(0U)
    {
        // Power of 10 Rule 7b: initialize at declaration.
        // Zero the entire ring buffer at construction.
        for (uint32_t i = 0U; i < RING_LOG_SINK_CAPACITY; ++i) {
            (void)memset(m_lines[i], 0, RING_LOG_SINK_LINE_MAX);
        }
    }

    ~RingLogSink() override = default;

    /// ILogSink::write — copies @p len bytes from @p buf into the next ring slot.
    /// If the ring is full, the oldest slot is overwritten.
    void write(const char* buf, uint32_t len) override
    {
        if (buf == nullptr || len == 0U) {
            return;
        }
        // Write index cycles through RING_LOG_SINK_CAPACITY slots.
        const uint32_t idx = m_head % RING_LOG_SINK_CAPACITY;

        // Copy into fixed-size slot; clamp and NUL-terminate.
        const uint32_t copy_len = (len < RING_LOG_SINK_LINE_MAX - 1U)
                                      ? len
                                      : RING_LOG_SINK_LINE_MAX - 1U;
        (void)memset(m_lines[idx], 0, RING_LOG_SINK_LINE_MAX);
        (void)strncpy(m_lines[idx], buf, static_cast<size_t>(copy_len));
        m_lines[idx][copy_len] = '\0';

        m_head++;
        if (m_count < RING_LOG_SINK_CAPACITY) {
            m_count++;
        }
    }

    /// Returns the number of lines currently held in the ring (≤ capacity).
    uint32_t count() const { return m_count; }

    /// Returns true if the ring holds at least one line.
    bool has_lines() const { return m_count > 0U; }

    /// Returns a pointer to the most recently written line, or nullptr if empty.
    const char* latest() const
    {
        if (m_count == 0U) {
            return nullptr;
        }
        // m_head points one past the last written slot.
        const uint32_t idx = (m_head - 1U) % RING_LOG_SINK_CAPACITY;
        return m_lines[idx];
    }

    /// Returns a pointer to the line at position @p pos relative to the oldest
    /// entry (0 = oldest), or nullptr if pos >= count.
    const char* at(uint32_t pos) const
    {
        if (pos >= m_count) {
            return nullptr;
        }
        // Oldest entry is at (m_head - m_count) % capacity.
        const uint32_t oldest_abs = m_head - m_count;
        const uint32_t idx = (oldest_abs + pos) % RING_LOG_SINK_CAPACITY;
        return m_lines[idx];
    }

    /// Clear all captured lines.
    void clear()
    {
        m_head  = 0U;
        m_count = 0U;
        for (uint32_t i = 0U; i < RING_LOG_SINK_CAPACITY; ++i) {
            (void)memset(m_lines[i], 0, RING_LOG_SINK_LINE_MAX);
        }
    }

private:
    // Ring buffer: RING_LOG_SINK_CAPACITY slots of RING_LOG_SINK_LINE_MAX bytes each.
    // Power of 10 Rule 3: fixed-size static allocation; no heap.
    char     m_lines[RING_LOG_SINK_CAPACITY][RING_LOG_SINK_LINE_MAX];
    uint32_t m_head;   ///< Index of next write slot (wraps modulo capacity).
    uint32_t m_count;  ///< Number of lines stored (saturates at capacity).
};

#endif // TESTS_RINGLOGSINK_HPP
