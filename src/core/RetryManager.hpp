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
 * @file RetryManager.hpp
 * @brief Manages retry scheduling and exponential backoff for RELIABLE_RETRY messages.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §3.2 (Retry logic),
 * CLAUDE.md §3.3 (Reliable with retry and dedupe).
 *
 * Rules applied:
 *   - Power of 10 rule 3: fixed-capacity slot array (ACK_TRACKER_CAPACITY).
 *   - Power of 10 rule 5: assertions enforce capacity and state invariants.
 *   - F-Prime style: no STL, no exceptions, no templates.
 *   - No dynamic allocation after init.
 *
 * Design: Maintains a fixed table of retry entries, each tracking a message's
 * retry schedule and expiry. collect_due() applies exponential backoff and
 * removes exhausted entries.
 *
 * Implements: REQ-3.2.5, REQ-3.3.3, REQ-7.2.3
 */

#ifndef CORE_RETRY_MANAGER_HPP
#define CORE_RETRY_MANAGER_HPP

#include "Assert.hpp"
#include <cstdint>
#include <cstring>
#include "Types.hpp"
#include "MessageEnvelope.hpp"
#include "DeliveryStats.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager
// ─────────────────────────────────────────────────────────────────────────────

class RetryManager {
public:
    /// Initialize the retry manager. Called once at system startup.
    void init();

    // Safety-critical (SC): HAZ-002 — verified to M5
    /**
     * @brief Schedule a message for retry.
     *
     * Adds the message to a retry slot with the given parameters.
     * Exponential backoff starts at @p backoff_ms and doubles on each retry.
     *
     * @param[in] env        Message envelope to schedule.
     * @param[in] max_retries Maximum number of retry attempts.
     * @param[in] backoff_ms  Initial backoff interval in milliseconds.
     * @param[in] now_us     Current time in microseconds.
     * @return OK on success; ERR_FULL if all slots are occupied.
     */
    Result schedule(const MessageEnvelope& env,
                    uint32_t               max_retries,
                    uint32_t               backoff_ms,
                    uint64_t               now_us);

    // Safety-critical (SC): HAZ-002 — verified to M5
    /**
     * @brief Cancel a scheduled retry when an ACK is received.
     *
     * Searches for a retry entry matching the source and message_id,
     * then marks it inactive (for removal by garbage collection).
     *
     * @param[in] src    Source node ID of the ACK.
     * @param[in] msg_id Message ID being acknowledged.
     * @return OK if cancelled; ERR_INVALID if no entry found.
     */
    Result on_ack(NodeId src, uint64_t msg_id);

    /// Cancel an active retry slot without incrementing acks_received.
    /// Used for rollback when the associated send failed before hitting the wire.
    /// Transitions the slot to inactive and decrements m_count; no stat bump.
    /// @param src    [in] source node ID of the pending message
    /// @param msg_id [in] message ID of the pending message
    /// @return OK on success; ERR_INVALID if no matching active slot
    /// NSC: bookkeeping correction only; no delivery state change.
    Result cancel(NodeId src, uint64_t msg_id);

    // Safety-critical (SC): HAZ-002 — verified to M5
    /**
     * @brief Collect messages due for retry and apply exponential backoff.
     *
     * Examines all active retry entries, identifies those whose
     * next_retry_us <= now_us, doubles the backoff for each, increments retry_count,
     * and copies matching envelopes into the output buffer.
     *
     * Removes entries that have exhausted max_retries.
     *
     * @param[in]  now_us  Current time in microseconds.
     * @param[out] out_buf Buffer to fill with ready-to-retry envelopes.
     * @param[in]  buf_cap Capacity of out_buf (number of MessageEnvelope slots).
     * @return Number of envelopes copied to out_buf.
     */
    uint32_t collect_due(uint64_t             now_us,
                         MessageEnvelope*     out_buf,
                         uint32_t             buf_cap);

    /// Return a snapshot of retry statistics (REQ-7.2.3).
    /// NSC: read-only observability accessor.
    /// Power of 10 rule 5: ≥2 assertions enforced inside.
    const RetryStats& get_stats() const;

private:
    // Phase 1 of collect_due(): reap all expired and exhausted slots across the full table.
    // Extracted to keep collect_due() within CC ≤ 10.
    void reap_terminated_slots(uint64_t now_us);

    // ─────────────────────────────────────────────────────────────────────────
    // Retry table entry (fixed-size slot in m_slots array)
    // ─────────────────────────────────────────────────────────────────────────
    struct RetryEntry {
        MessageEnvelope env;           ///< The message to retry
        uint64_t        next_retry_us;  ///< When to schedule next retry
        uint64_t        expiry_us;      ///< Discard if now >= expiry_us
        uint32_t        retry_count;    ///< Current attempt number
        uint32_t        max_retries;    ///< Maximum allowed attempts
        uint32_t        backoff_ms;     ///< Current backoff interval
        bool            active;         ///< true if slot is in use
    };

    // Power of 10 rule 3: fixed-capacity storage, no dynamic allocation
    RetryEntry  m_slots[ACK_TRACKER_CAPACITY] = {};
    uint32_t    m_count         = 0U;    ///< Number of active entries
    bool        m_initialized   = false; ///< True after init() has been called
    RetryStats  m_stats         = {};    ///< REQ-7.2.3 observability counters
    uint64_t    m_last_collect_us = 0U;  ///< Monotonic-time enforcement: timestamp of last collect_due call.
};

#endif // CORE_RETRY_MANAGER_HPP
