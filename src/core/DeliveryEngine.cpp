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
 * @file DeliveryEngine.cpp
 * @brief Implementation of delivery engine for reliable messaging.
 *
 * Applies Power of 10 rules: fixed loop bounds, ≥2 assertions per function,
 * checked return values, bounded resource usage.
 *
 * Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-7.2.3
 */

#include "DeliveryEngine.hpp"
#include "Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::init()
// ─────────────────────────────────────────────────────────────────────────────
void DeliveryEngine::init(TransportInterface* transport,
                          const ChannelConfig& cfg,
                          NodeId local_id)
{
    NEVER_COMPILED_OUT_ASSERT(transport != nullptr);  // Assert: transport provided
    NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID);  // Assert: valid local node ID

    m_transport = transport;
    m_cfg = cfg;
    m_local_id = local_id;
    m_initialized = false;

    // Initialize sub-components
    m_ack_tracker.init();
    m_retry_manager.init();
    m_dedup.init();

    // Initialize message ID generator with a seed based on local_id
    // Power of 10: deterministic seed for testing
    uint64_t id_seed = static_cast<uint64_t>(local_id);
    if (id_seed == 0ULL) {
        id_seed = 1ULL;  // Ensure non-zero seed
    }
    m_id_gen.init(id_seed);

    // Power of 10 Rule 3: zero-initialize pre-allocated output buffers during
    // init phase so they contain no garbage before first use by pump_retries()
    // and sweep_ack_timeouts(). Bounded loops — compile-time constants.
    for (uint32_t i = 0U; i < MSG_RING_CAPACITY; ++i) {
        envelope_init(m_retry_buf[i]);
    }
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        envelope_init(m_timeout_buf[i]);
    }

    m_initialized = true;

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine marked initialized
    NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr);  // Assert: transport is valid

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Initialized channel=%u, local_id=%u",
                cfg.channel_id, local_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::send_via_transport()
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::send_via_transport(const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr);  // Assert: transport valid
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: valid envelope

    // Power of 10 rule 7: check return value from transport
    Result res = m_transport->send_message(env);

    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Transport send failed for message_id=%llu (result=%u)",
                    (unsigned long long)env.message_id, static_cast<uint8_t>(res));
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::send()
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);  // Assert: valid timestamp

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Power of 10 rule 7: check all function returns
    // Assign source_id and message_id
    env.source_id = m_local_id;
    env.message_id = m_id_gen.next();
    env.timestamp_us = now_us;

    // Send via transport
    Result res = send_via_transport(env);
    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Failed to send message_id=%llu: %u",
                    (unsigned long long)env.message_id, static_cast<uint8_t>(res));
        return res;
    }

    // Apply tracking based on reliability class
    if (env.reliability_class == ReliabilityClass::RELIABLE_ACK ||
        env.reliability_class == ReliabilityClass::RELIABLE_RETRY) {

        // Calculate ACK deadline
        uint64_t ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms);

        // Track this message for ACK
        // Power of 10 rule 7: check return value
        Result track_res = m_ack_tracker.track(env, ack_deadline);
        if (track_res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                        "Failed to track ACK for message_id=%llu (result=%u)",
                        (unsigned long long)env.message_id, static_cast<uint8_t>(track_res));
            // Do not fail the send; ACK tracking is a side effect
        }
    }

    // Schedule for retry if RELIABLE_RETRY
    if (env.reliability_class == ReliabilityClass::RELIABLE_RETRY) {
        // Power of 10 rule 7: check return value
        Result sched_res = m_retry_manager.schedule(env,
                                                     m_cfg.max_retries,
                                                     m_cfg.retry_backoff_ms,
                                                     now_us);
        if (sched_res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                        "Failed to schedule retry for message_id=%llu (result=%u)",
                        (unsigned long long)env.message_id, static_cast<uint8_t>(sched_res));
            // Do not fail the send; retry is a side effect
        }
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(env.source_id == m_local_id);  // Assert: source set correctly
    NEVER_COMPILED_OUT_ASSERT(env.message_id != 0ULL);  // Assert: message_id assigned

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Sent message_id=%llu, reliability=%u",
                (unsigned long long)env.message_id,
                static_cast<uint8_t>(env.reliability_class));

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::receive()
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::receive(MessageEnvelope& env,
                               uint32_t         timeout_ms,
                               uint64_t         now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);  // Assert: valid timestamp

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Power of 10 rule 7: check return value from transport
    Result res = m_transport->receive_message(env, timeout_ms);
    if (res != Result::OK) {
        // Normal timeout case (ERR_TIMEOUT is expected)
        return res;
    }

    // Power of 10 rule 5: validate received envelope
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: transport provides valid envelope

    // Check if message has expired
    if (timestamp_expired(env.expiry_time_us, now_us)) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Dropping expired message_id=%llu from src=%u",
                    (unsigned long long)env.message_id, env.source_id);
        return Result::ERR_EXPIRED;
    }

    // Handle control messages (ACK/NAK/HEARTBEAT)
    if (envelope_is_control(env)) {
        if (env.message_type == MessageType::ACK) {
            // Process ACK: clear from retrying, confirm in ack_tracker
            // Power of 10 rule 7: check return values
            Result ack_res = m_ack_tracker.on_ack(env.source_id, env.message_id);
            (void)ack_res;  // Side effect; not critical to fail receive

            Result retry_res = m_retry_manager.on_ack(env.source_id, env.message_id);
            (void)retry_res;  // Side effect

            Logger::log(Severity::INFO, "DeliveryEngine",
                        "Received ACK for message_id=%llu from src=%u",
                        (unsigned long long)env.message_id, env.source_id);
        }
        // For other control messages (NAK, HEARTBEAT), just pass through
        return Result::OK;
    }

    // Handle data messages
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env));  // Assert: is data message

    // Apply duplicate suppression if RELIABLE_RETRY
    if (env.reliability_class == ReliabilityClass::RELIABLE_RETRY) {
        // Power of 10 rule 7: check return value
        Result dedup_res = m_dedup.check_and_record(env.source_id, env.message_id);
        if (dedup_res == Result::ERR_DUPLICATE) {
            Logger::log(Severity::INFO, "DeliveryEngine",
                        "Suppressed duplicate message_id=%llu from src=%u",
                        (unsigned long long)env.message_id, env.source_id);
            return Result::ERR_DUPLICATE;
        }
        NEVER_COMPILED_OUT_ASSERT(dedup_res == Result::OK);  // Assert: check_and_record succeeded
    }

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Received data message_id=%llu from src=%u, length=%u",
                (unsigned long long)env.message_id, env.source_id,
                env.payload_length);

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: envelope still valid

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::pump_retries()
// ─────────────────────────────────────────────────────────────────────────────
uint32_t DeliveryEngine::pump_retries(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);  // Assert: valid timestamp

    // Power of 10 Rule 3: use pre-allocated member buffer m_retry_buf (initialized
    // in init()). collect_due() overwrites only the first 'collected' slots;
    // stale data beyond that index is never read.
    uint32_t collected = 0U;

    // Collect due retries
    // Power of 10 rule 7: check return value (it's count, always valid)
    collected = m_retry_manager.collect_due(now_us, m_retry_buf, MSG_RING_CAPACITY);
    NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY);  // Assert: bounded result

    // Re-send each
    // Power of 10 rule 2: bounded loop (collected is ≤ MSG_RING_CAPACITY)
    for (uint32_t i = 0U; i < collected; ++i) {
        // Power of 10 rule 7: check return value
        Result send_res = send_via_transport(m_retry_buf[i]);
        if (send_res != Result::OK) {
            Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                        "Retry send failed for message_id=%llu (result=%u)",
                        (unsigned long long)m_retry_buf[i].message_id,
                        static_cast<uint8_t>(send_res));
        } else {
            Logger::log(Severity::INFO, "DeliveryEngine",
                        "Retried message_id=%llu",
                        (unsigned long long)m_retry_buf[i].message_id);
        }
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY);  // Assert: result bounded

    return collected;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::sweep_ack_timeouts()
// ─────────────────────────────────────────────────────────────────────────────
uint32_t DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);  // Assert: valid timestamp

    // Power of 10 Rule 3: use pre-allocated member buffer m_timeout_buf (initialized
    // in init()). sweep_expired() overwrites only the first 'collected' slots;
    // stale data beyond that index is never read.
    uint32_t collected = 0U;

    // Sweep expired ACK entries
    // Power of 10 rule 7: check return value (it's count)
    collected = m_ack_tracker.sweep_expired(now_us, m_timeout_buf, ACK_TRACKER_CAPACITY);
    NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY);  // Assert: bounded result

    // Log each timeout as WARNING_HI (system-wide but recoverable)
    // Power of 10 rule 2: bounded loop
    for (uint32_t i = 0U; i < collected; ++i) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "ACK timeout for message_id=%llu sent to node=%u",
                    (unsigned long long)m_timeout_buf[i].message_id,
                    m_timeout_buf[i].destination_id);
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY);  // Assert: result bounded

    return collected;
}
