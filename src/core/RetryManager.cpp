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
 * @file RetryManager.cpp
 * @brief Implementation of retry scheduling with exponential backoff.
 *
 * Applies Power of 10 rules: fixed loop bounds, ≥2 assertions per function,
 * bounded resource usage, checked return values.
 *
 * Implements: REQ-3.2.5, REQ-3.3.3, REQ-7.2.3
 */

#include "RetryManager.hpp"
#include "Logger.hpp"
#include "Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// File-static helper: return true when a retry entry's expiry has passed.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
static bool slot_has_expired(uint64_t expiry_us, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);      // Power of 10: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(expiry_us != ~0ULL); // Power of 10: sentinel not passed
    return (expiry_us != 0ULL) && (now_us >= expiry_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// File-static helper: advance exponential backoff, capped at 60 seconds.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t advance_backoff(uint32_t current_ms)
{
    NEVER_COMPILED_OUT_ASSERT(current_ms <= 60000U);  // Power of 10: pre-condition
    // Cap before multiply: structurally prevents overflow even if
    // the precondition is weakened on a future embedded port.
    const uint32_t capped = (current_ms > 30000U) ? 30000U : current_ms;
    const uint32_t doubled = capped * 2U;
    // CERT INT30-C: enforce 1 ms minimum backoff — prevents zero-delay retry storm
    // when schedule() is called with backoff_ms=0 (sec eng review finding #8).
    const uint32_t result  = (doubled == 0U) ? 1U : doubled;
    NEVER_COMPILED_OUT_ASSERT(result <= 60000U);   // Power of 10: post-condition
    NEVER_COMPILED_OUT_ASSERT(result > 0U);        // Assert: minimum 1 ms enforced
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::init()
// ─────────────────────────────────────────────────────────────────────────────
void RetryManager::init()
{
    // If called on an already-initialized manager (e.g. engine restart or test re-use),
    // emit a warning if active slots exist — those entries will be discarded — then
    // fall through and reset all state to a clean baseline.  Aborting here is too
    // strict: a re-initializing caller is not misusing the API; it explicitly wants a
    // fresh manager, which is the correct contract for engine restart paths.
    // Power of 10 Rule 7: m_count access is safe because m_initialized only becomes
    // true inside this function, so the value is always valid here.
    if (m_initialized && (m_count > 0U)) {
        Logger::log(Severity::WARNING_HI, "RetryManager",
                    "re-init with %u active retry slot(s); entries discarded", m_count);
    }
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count bounded

    m_count = 0U;
    m_initialized = true;
    m_last_collect_us = 0U;
    retry_stats_init(m_stats);  // REQ-7.2.3: zero all observability counters

    // Power of 10 rule 2: bounded loop (compile-time constant)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        m_slots[i].active = false;
        m_slots[i].retry_count = 0U;
        m_slots[i].backoff_ms = 0U;
        m_slots[i].next_retry_us = 0ULL;
        m_slots[i].expiry_us = 0ULL;
        m_slots[i].max_retries = 0U;
        envelope_init(m_slots[i].env);
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(m_count == 0U);          // Assert: count is zero after init
    NEVER_COMPILED_OUT_ASSERT(m_initialized);           // Assert: marked as initialized
    NEVER_COMPILED_OUT_ASSERT(m_stats.retries_sent == 0U);  // Assert: stats zeroed
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::schedule()
// ─────────────────────────────────────────────────────────────────────────────
Result RetryManager::schedule(const MessageEnvelope& env,
                              uint32_t               max_retries,
                              uint32_t               backoff_ms,
                              uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: valid envelope

    // Power of 10 rule 2: bounded loop (compile-time constant)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (!m_slots[i].active) {
            // Found a free slot; use it
            m_slots[i].active = true;
            envelope_copy(m_slots[i].env, env);
            m_slots[i].retry_count = 0U;
            m_slots[i].max_retries = max_retries;
            m_slots[i].backoff_ms = backoff_ms;

            // Schedule first retry immediately
            m_slots[i].next_retry_us = now_us;

            // Expiry is message's configured expiry time
            m_slots[i].expiry_us = env.expiry_time_us;

            ++m_count;

            // Power of 10 rule 5: post-condition assertion
            NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count bounded
            NEVER_COMPILED_OUT_ASSERT(m_slots[i].active);  // Assert: slot marked active

            Logger::log(Severity::INFO, "RetryManager",
                        "Scheduled message_id=%llu for retry (max_retries=%u, backoff_ms=%u)",
                        (unsigned long long)env.message_id, max_retries, backoff_ms);

            return Result::OK;
        }
    }

    // No free slots
    NEVER_COMPILED_OUT_ASSERT(m_count == ACK_TRACKER_CAPACITY);  // Assert: all slots full

    Logger::log(Severity::WARNING_LO, "RetryManager",
                "Retry table full; cannot schedule message_id=%llu",
                (unsigned long long)env.message_id);

    return Result::ERR_FULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::on_ack()
// ─────────────────────────────────────────────────────────────────────────────
Result RetryManager::on_ack(NodeId src, uint64_t msg_id)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID);  // Assert: valid source node

    // Power of 10 rule 2: bounded loop (compile-time constant)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (m_slots[i].active &&
            m_slots[i].env.source_id == src &&
            m_slots[i].env.message_id == msg_id) {

            // Cancel this retry entry
            m_slots[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_count > 0U);  // CERT INT30-C: guard against uint32_t underflow
            --m_count;
            ++m_stats.acks_received;  // REQ-7.2.3: record ACK-cancelled retry

            // Power of 10 rule 5: post-condition assertion
            NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count decremented

            Logger::log(Severity::INFO, "RetryManager",
                        "Cancelled retry for message_id=%llu from node=%u",
                        (unsigned long long)msg_id, src);

            return Result::OK;
        }
    }

    // Entry not found
    Logger::log(Severity::WARNING_LO, "RetryManager",
                "No retry entry found for message_id=%llu from node=%u",
                (unsigned long long)msg_id, src);

    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::cancel()
// Rollback helper: deactivates a retry slot without bumping acks_received.
// Called when a send fails before hitting the wire so no phantom stat is recorded.
// NSC: bookkeeping correction only; no delivery state change.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
Result RetryManager::cancel(NodeId src, uint64_t msg_id)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);           // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID);  // Assert: valid source node

    // Power of 10 rule 2: bounded loop (compile-time constant)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (m_slots[i].active &&
            m_slots[i].env.source_id == src &&
            m_slots[i].env.message_id == msg_id) {

            // Deactivate the slot without recording an ACK stat
            m_slots[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_count > 0U);  // CERT INT30-C: guard against uint32_t underflow
            --m_count;

            // Power of 10 rule 5: post-condition assertion
            NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count decremented

            Logger::log(Severity::INFO, "RetryManager",
                        "Cancelled (rollback) retry for message_id=%llu from node=%u",
                        (unsigned long long)msg_id, src);

            return Result::OK;
        }
    }

    // Entry not found — this is expected when no slot was reserved for this message
    Logger::log(Severity::WARNING_LO, "RetryManager",
                "No retry entry found to cancel for message_id=%llu from node=%u",
                (unsigned long long)msg_id, src);

    return Result::ERR_INVALID;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::reap_terminated_slots()
// Phase 1 of collect_due(): remove expired and exhausted entries from the table.
// Extracted to keep collect_due() within CC ≤ 10.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
void RetryManager::reap_terminated_slots(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);   // Assert: valid timestamp

    // Power of 10 rule 2: fully bounded loop (compile-time constant).
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (!m_slots[i].active) {
            continue;
        }
        if (slot_has_expired(m_slots[i].expiry_us, now_us)) {
            m_slots[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_count > 0U);  // CERT INT30-C: guard against uint32_t underflow
            --m_count;
            ++m_stats.slots_expired;  // REQ-7.2.3: record expiry event
            Logger::log(Severity::WARNING_LO, "RetryManager",
                        "Expired retry entry for message_id=%llu",
                        (unsigned long long)m_slots[i].env.message_id);
            continue;
        }
        if (m_slots[i].retry_count >= m_slots[i].max_retries) {
            m_slots[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_count > 0U);  // CERT INT30-C: guard against uint32_t underflow
            --m_count;
            ++m_stats.slots_exhausted;  // REQ-7.2.3: record exhaustion event
            Logger::log(Severity::WARNING_HI, "RetryManager",
                        "Exhausted retries for message_id=%llu (count=%u, max=%u)",
                        (unsigned long long)m_slots[i].env.message_id,
                        m_slots[i].retry_count, m_slots[i].max_retries);
        }
    }

    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count bounded
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::collect_due()
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RetryManager::collect_due(uint64_t         now_us,
                                    MessageEnvelope* out_buf,
                                    uint32_t         buf_cap)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);  // Assert: output buffer provided
    NEVER_COMPILED_OUT_ASSERT(buf_cap <= MSG_RING_CAPACITY);  // Assert: reasonable capacity

    // Monotonic-time contract: callers must supply non-decreasing now_us (CLOCK_MONOTONIC).
    NEVER_COMPILED_OUT_ASSERT(now_us >= m_last_collect_us);  // Assert: monotonic time contract
    m_last_collect_us = now_us;

    uint32_t collected = 0U;

    // MED-3 fix: separate reaping from collecting to avoid skipping tail slots.
    // Previously a single loop with `&& collected < buf_cap` exited before visiting
    // remaining slots when the output buffer was full, leaving expired/exhausted slots
    // unreaped until the next call. Under HAZ-002 (retry storm), a full buffer leads to
    // unbounded slot accumulation in the worst case.
    //
    // Phase 1: reap expired and exhausted slots across all indices (full sweep).
    reap_terminated_slots(now_us);

    // Phase 2: collect due retries into output buffer.
    // Power of 10 rule 2: bounded by compile-time constant; early exit when buffer full.
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY && collected < buf_cap; ++i) {
        if (!m_slots[i].active) {
            continue;  // Skip inactive slots (including those just reaped in Phase 1)
        }

        // Check if retry is due
        if (m_slots[i].next_retry_us <= now_us) {
            // Copy to output buffer
            envelope_copy(out_buf[collected], m_slots[i].env);
            ++collected;
            ++m_slots[i].retry_count;
            ++m_stats.retries_sent;  // REQ-7.2.3: record retry transmission

            // Apply exponential backoff via helper (capped at 60 s)
            m_slots[i].backoff_ms = advance_backoff(m_slots[i].backoff_ms);
            uint64_t backoff_us = static_cast<uint64_t>(m_slots[i].backoff_ms) * 1000ULL;
            m_slots[i].next_retry_us = now_us + backoff_us;

            Logger::log(Severity::INFO, "RetryManager",
                        "Due for retry: message_id=%llu, attempt=%u, next_backoff_ms=%u",
                        (unsigned long long)m_slots[i].env.message_id,
                        m_slots[i].retry_count, m_slots[i].backoff_ms);
        }
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(collected <= buf_cap);  // Assert: collected within output capacity
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count bounded

    return collected;
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::get_stats() — REQ-7.2.3 observability accessor
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

const RetryStats& RetryManager::get_stats() const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY);  // Assert: count bounded
    return m_stats;
}
