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
 * @file ImpairmentEngine.cpp
 * @brief Implementation of the ImpairmentEngine class.
 *
 * Applies network impairments (loss, duplication, latency, jitter, reordering, partitions)
 * to MessageEnvelope objects in a deterministic manner controlled by a seedable PRNG.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds using IMPAIR_DELAY_BUF_SIZE, no recursion,
 *     ≥2 assertions per method, all return values checked.
 *   - MISRA C++: no STL, no exceptions, no templates.
 *   - F-Prime style: Result enum return codes, Logger::log() for events.
 *
 * Implements: REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.2.2, REQ-5.2.5, REQ-5.3.1, REQ-5.3.2
 */

#include "ImpairmentEngine.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

ImpairmentEngine::ImpairmentEngine()
    : m_cfg{}, m_delay_count(0U), m_reorder_count(0U),
      m_partition_active(false), m_partition_start_us(0ULL),
      m_next_partition_event_us(0ULL), m_initialized(false)
{
    // Power of 10 rule 3: zero all statically allocated buffers
    (void)memset(m_delay_buf, 0, sizeof(m_delay_buf));
    (void)memset(m_reorder_buf, 0, sizeof(m_reorder_buf));
    // Seed PRNG to known non-zero state; init() will reseed with config value
    m_prng.seed(1ULL);
    NEVER_COMPILED_OUT_ASSERT(!m_initialized);          // Power of 10: precondition
    NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U);  // Power of 10: bounds check
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

void ImpairmentEngine::init(const ImpairmentConfig& cfg)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE);
    NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0);
    // MED-7 fix: partition_gap_ms==0 with partition enabled causes an immediate, permanent
    // partition on the very first call to is_partition_active() (documented in
    // STATE_MACHINES.md §3 "Known edge case"). Reject this at init time so the
    // configuration error is detected immediately rather than causing silent message loss.
    NEVER_COMPILED_OUT_ASSERT(!cfg.partition_enabled || cfg.partition_gap_ms > 0U);

    // Store configuration
    m_cfg = cfg;

    // Seed PRNG; use default seed 42 if cfg specifies 0
    uint64_t seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL;
    m_prng.seed(seed);

    // Zero-fill delay buffer (Power of 10: memset with fixed bounds)
    (void)memset(m_delay_buf, 0, sizeof(m_delay_buf));
    m_delay_count = 0U;

    // Zero-fill reorder buffer
    (void)memset(m_reorder_buf, 0, sizeof(m_reorder_buf));
    m_reorder_count = 0U;

    // Initialize partition state
    m_partition_active = false;
    m_partition_start_us = 0ULL;
    // First call to is_partition_active() will initialize m_next_partition_event_us
    m_next_partition_event_us = 0ULL;

    // Mark as initialized
    m_initialized = true;

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// queue_to_delay_buf() — private helper
// ─────────────────────────────────────────────────────────────────────────────

Result ImpairmentEngine::queue_to_delay_buf(const MessageEnvelope& env,
                                             uint64_t release_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                           // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE);  // Power of 10: slot available

    // Power of 10 rule 2: fixed loop bound
    for (uint32_t i = 0U; i < IMPAIR_DELAY_BUF_SIZE; ++i) {
        if (!m_delay_buf[i].active) {
            envelope_copy(m_delay_buf[i].env, env);
            m_delay_buf[i].release_us = release_us;
            m_delay_buf[i].active = true;
            ++m_delay_count;
            NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);  // Power of 10: postcondition
            return Result::OK;
        }
    }

    // Should never reach here: caller verified m_delay_count < buf size
    NEVER_COMPILED_OUT_ASSERT(false);  // Logic error: no free slot despite count check
    return Result::ERR_FULL;
}

// ─────────────────────────────────────────────────────────────────────────────
// check_loss() — private helper
// ─────────────────────────────────────────────────────────────────────────────

bool ImpairmentEngine::check_loss()
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                      // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0);      // Power of 10: valid probability

    if (m_cfg.loss_probability <= 0.0) {
        return false;
    }
    double rand_val = m_prng.next_double();
    NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0);  // Power of 10: valid random value
    return rand_val < m_cfg.loss_probability;
}

// ─────────────────────────────────────────────────────────────────────────────
// apply_duplication() — private helper
// ─────────────────────────────────────────────────────────────────────────────

void ImpairmentEngine::apply_duplication(const MessageEnvelope& env,
                                          uint64_t release_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                           // Power of 10: initialized
    NEVER_COMPILED_OUT_ASSERT(m_cfg.duplication_probability > 0.0);    // Power of 10: called only when enabled

    double dup_rand = m_prng.next_double();
    NEVER_COMPILED_OUT_ASSERT(dup_rand >= 0.0 && dup_rand < 1.0);      // Power of 10: valid random value

    if (dup_rand < m_cfg.duplication_probability) {
        if (m_delay_count < IMPAIR_DELAY_BUF_SIZE) {
            Result res = queue_to_delay_buf(env, release_us + 100ULL);
            if (result_ok(res)) {
                Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                           "message duplicated");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// process_outbound()
// ─────────────────────────────────────────────────────────────────────────────

Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env,
                                          uint64_t now_us)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env));

    // If impairments disabled, pass through immediately with zero latency
    if (!m_cfg.enabled) {
        if (m_delay_count >= IMPAIR_DELAY_BUF_SIZE) {
            Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                       "delay buffer full (disabled impairments)");
            return Result::ERR_FULL;
        }
        return queue_to_delay_buf(in_env, now_us);
    }

    // Drop if partition is active
    if (is_partition_active(now_us)) {
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "message dropped (partition active)");
        return Result::ERR_IO;
    }

    // Drop if loss impairment fires
    if (check_loss()) {
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "message dropped (loss probability)");
        return Result::ERR_IO;
    }

    // Calculate release time: fixed latency + jitter
    uint64_t base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL;
    uint64_t jitter_us = 0ULL;
    if (m_cfg.jitter_mean_ms > 0U) {
            // REQ-5.1.2: jitter is centered on mean_ms with up to variance_ms of additional
            // random delay. offset = mean + uniform(0, variance) → range [mean, mean+variance].
            // A true ±variance distribution would risk uint underflow when variance > mean;
            // the [mean, mean+variance] interpretation is the safe, uint-compatible form used here.
            // Previously jitter_mean_ms was used only as a gate; its value was never added.
            uint32_t jitter_offset_ms = m_cfg.jitter_mean_ms +
                                        m_prng.next_range(0U, m_cfg.jitter_variance_ms);
        jitter_us = static_cast<uint64_t>(jitter_offset_ms) * 1000ULL;
    }
    uint64_t release_us = now_us + base_delay_us + jitter_us;

    // Queue the message in the delay buffer
    if (m_delay_count >= IMPAIR_DELAY_BUF_SIZE) {
        Logger::log(Severity::WARNING_HI, "ImpairmentEngine",
                   "delay buffer full; dropping message");
        return Result::ERR_FULL;
    }
    Result res = queue_to_delay_buf(in_env, release_us);
    if (!result_ok(res)) {
        return res;
    }

    // Probabilistically queue a duplicate copy
    if (m_cfg.duplication_probability > 0.0) {
        apply_duplication(in_env, release_us);
    }

    // Power of 10 rule 5: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// collect_deliverable()
// ─────────────────────────────────────────────────────────────────────────────

uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us,
                                                MessageEnvelope* out_buf,
                                                uint32_t buf_cap)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);

    uint32_t out_count = 0U;

    // Power of 10: fixed loop bound using IMPAIR_DELAY_BUF_SIZE
    for (uint32_t i = 0U; i < IMPAIR_DELAY_BUF_SIZE && out_count < buf_cap; ++i) {
        if (m_delay_buf[i].active && m_delay_buf[i].release_us <= now_us) {
            // Message is ready for delivery
            envelope_copy(out_buf[out_count], m_delay_buf[i].env);
            ++out_count;

            // Mark slot as inactive and decrement count
            m_delay_buf[i].active = false;
            NEVER_COMPILED_OUT_ASSERT(m_delay_count > 0U);
            --m_delay_count;
        }
    }

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE);
    return out_count;
}

// ─────────────────────────────────────────────────────────────────────────────
// process_inbound()
// ─────────────────────────────────────────────────────────────────────────────

// cppcheck-suppress unusedFunction -- called from tests/test_ImpairmentEngine.cpp
Result ImpairmentEngine::process_inbound(const MessageEnvelope& in_env,
                                        uint64_t now_us,
                                        MessageEnvelope* out_buf,
                                        uint32_t buf_cap,
                                        uint32_t& out_count)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env));
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);
    (void)now_us;  // Parameter unused; reordering works on buffered messages

    // Initialize output count
    out_count = 0U;

    // If reordering disabled or window size is 0, pass through immediately
    if (!m_cfg.reorder_enabled || m_cfg.reorder_window_size == 0U) {
        envelope_copy(out_buf[0], in_env);
        out_count = 1U;
        NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
        return Result::OK;
    }

    // Reordering is enabled; buffer the message if window not full
    if (m_reorder_count < m_cfg.reorder_window_size) {
        // Buffer the new message
        envelope_copy(m_reorder_buf[m_reorder_count], in_env);
        ++m_reorder_count;
        // Do not deliver yet; wait until window fills or timeout
        out_count = 0U;
        NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE);
        return Result::OK;
    }

    // Window is full; randomly release one buffered message and add the new one
    // Power of 10: fixed loop bound
    uint32_t release_idx = m_prng.next_range(0U, m_reorder_count - 1U);
    NEVER_COMPILED_OUT_ASSERT(release_idx < m_reorder_count);

    if (buf_cap >= 1U) {
        // Release the randomly selected message
        envelope_copy(out_buf[0], m_reorder_buf[release_idx]);
        out_count = 1U;

        // Remove the released message from the window by shifting
        // (Power of 10: fixed loop bound)
        for (uint32_t i = release_idx; i < m_reorder_count - 1U; ++i) {
            envelope_copy(m_reorder_buf[i], m_reorder_buf[i + 1U]);
        }
        --m_reorder_count;

        // Add the new message to the back of the window
        if (m_reorder_count < m_cfg.reorder_window_size) {
            envelope_copy(m_reorder_buf[m_reorder_count], in_env);
            ++m_reorder_count;
        }
    }

    // Power of 10 rule 5: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= m_cfg.reorder_window_size);
    NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// is_partition_active()
// ─────────────────────────────────────────────────────────────────────────────

bool ImpairmentEngine::is_partition_active(uint64_t now_us)
{
    // Power of 10 rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);

    // If partitions not enabled, return false
    if (!m_cfg.partition_enabled) {
        return false;
    }

    // Initialize partition event timing on first call
    if (m_next_partition_event_us == 0ULL) {
        // Start with gap (no partition yet)
        m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_gap_ms) * 1000ULL;
        m_partition_active = false;
        NEVER_COMPILED_OUT_ASSERT(!m_partition_active);
        return false;
    }

    // State machine: transition if event time reached
    if (!m_partition_active && now_us >= m_next_partition_event_us) {
        // Start a partition
        m_partition_active = true;
        m_partition_start_us = now_us;
        m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_duration_ms) * 1000ULL;
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "partition started (duration: %u ms)",
                   m_cfg.partition_duration_ms);
        NEVER_COMPILED_OUT_ASSERT(m_partition_active);
        return true;
    }

    if (m_partition_active && now_us >= m_next_partition_event_us) {
        // End the partition
        m_partition_active = false;
        m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_gap_ms) * 1000ULL;
        Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                   "partition ended");
        NEVER_COMPILED_OUT_ASSERT(!m_partition_active);
        return false;
    }

    // Power of 10 rule 5: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL);
    return m_partition_active;
}
