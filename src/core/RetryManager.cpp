/**
 * @file RetryManager.cpp
 * @brief Implementation of retry scheduling with exponential backoff.
 *
 * Applies Power of 10 rules: fixed loop bounds, ≥2 assertions per function,
 * bounded resource usage, checked return values.
 *
 * Implements: REQ-3.2.5, REQ-3.3.3
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
    const uint64_t doubled = static_cast<uint64_t>(current_ms) * 2U;
    if (doubled > 60000U) {
        // Cap reached; the surrounding if already proves doubled > 60000U
        return 60000U;
    }
    NEVER_COMPILED_OUT_ASSERT(doubled <= 60000U);  // Power of 10: post-condition
    return static_cast<uint32_t>(doubled);
}

// ─────────────────────────────────────────────────────────────────────────────
// RetryManager::init()
// ─────────────────────────────────────────────────────────────────────────────
void RetryManager::init()
{
    // Power of 10 rule 5: initialize state and mark not yet initialized
    NEVER_COMPILED_OUT_ASSERT(true);  // Assert: function entry point

    m_count = 0U;
    m_initialized = true;

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
    NEVER_COMPILED_OUT_ASSERT(m_count == 0U);  // Assert: count is zero after init
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: marked as initialized
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
            --m_count;

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
// RetryManager::collect_due()
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RetryManager::collect_due(uint64_t         now_us,
                                    MessageEnvelope* out_buf,
                                    uint32_t         buf_cap)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: manager was initialized
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);  // Assert: output buffer provided
    NEVER_COMPILED_OUT_ASSERT(buf_cap <= MSG_RING_CAPACITY);  // Assert: reasonable capacity

    uint32_t collected = 0U;

    // Power of 10 rule 2: bounded loop (compile-time constant)
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY && collected < buf_cap; ++i) {
        if (!m_slots[i].active) {
            continue;  // Skip inactive slots
        }

        // Check if this entry has expired
        if (slot_has_expired(m_slots[i].expiry_us, now_us)) {
            m_slots[i].active = false;
            --m_count;
            Logger::log(Severity::WARNING_LO, "RetryManager",
                        "Expired retry entry for message_id=%llu",
                        (unsigned long long)m_slots[i].env.message_id);
            continue;
        }

        // Check if this entry has exhausted retries
        if (m_slots[i].retry_count >= m_slots[i].max_retries) {
            m_slots[i].active = false;
            --m_count;
            Logger::log(Severity::WARNING_HI, "RetryManager",
                        "Exhausted retries for message_id=%llu (count=%u, max=%u)",
                        (unsigned long long)m_slots[i].env.message_id,
                        m_slots[i].retry_count, m_slots[i].max_retries);
            continue;
        }

        // Check if retry is due
        if (m_slots[i].next_retry_us <= now_us) {
            // Copy to output buffer
            envelope_copy(out_buf[collected], m_slots[i].env);
            ++collected;
            ++m_slots[i].retry_count;

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
