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
 * Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4
 */

#include "DeliveryEngine.hpp"
#include "Assert.hpp"
#include "AssertState.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// File-static helper: validate routing and expiry of a received envelope.
// Returns ERR_EXPIRED (stale message) or ERR_INVALID (wrong-node delivery),
// or OK. Combines both checks to keep DeliveryEngine::receive() within CC ≤ 10.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
static Result check_routing(const MessageEnvelope& env,
                             NodeId                 local_id,
                             uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID);  // pre-condition
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);                // pre-condition

    if (timestamp_expired(env.expiry_time_us, now_us)) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Dropping expired message_id=%llu from src=%u",
                    (unsigned long long)env.message_id, env.source_id);
        return Result::ERR_EXPIRED;
    }

    // Safety-critical (SC): HAZ-001 — drop messages not addressed to this node.
    // destination_id == NODE_ID_INVALID (0) is the broadcast sentinel; always accept.
    if (!envelope_addressed_to(env, local_id)) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Dropping misrouted message_id=%llu: dst=%u local=%u",
                    (unsigned long long)env.message_id,
                    env.destination_id, local_id);
        return Result::ERR_INVALID;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// File-static helper: send an ACK for a received reliable data message.
// Non-fatal on send failure (data still delivered). Extracted to keep
// DeliveryEngine::receive() within CC ≤ 10.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
static void send_data_ack(TransportInterface&    transport,
                           const MessageEnvelope& data_env,
                           NodeId                 local_id,
                           uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(data_env));  // pre-condition
    NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID); // pre-condition

    MessageEnvelope ack_env;
    envelope_make_ack(ack_env, data_env, local_id, now_us);
    Result res = transport.send_message(ack_env);
    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Failed to send ACK for message_id=%llu: result=%d",
                    (unsigned long long)data_env.message_id,
                    static_cast<int>(res));
    }
    NEVER_COMPILED_OUT_ASSERT(ack_env.message_type == MessageType::ACK);  // post-condition
}

// ─────────────────────────────────────────────────────────────────────────────
// File-static helper: process an ACK message — cancel ack_tracker and retry slots.
// Extracted to keep DeliveryEngine::receive() within CC ≤ 10.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
static void process_ack(AckTracker&          ack_tracker,
                         RetryManager&        retry_manager,
                         const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(env.message_type == MessageType::ACK);  // pre-condition
    NEVER_COMPILED_OUT_ASSERT(envelope_is_control(env));               // pre-condition

    // Look up by destination_id: ACK carries (src=remote, dst=local); tracker slot was
    // originally created with source_id = local node in send().
    Result ack_res = ack_tracker.on_ack(env.destination_id, env.message_id);
    if (ack_res == Result::ERR_INVALID) {
        Logger::log(Severity::INFO, "DeliveryEngine",
                    "ACK has no matching ack_tracker slot for message_id=%llu",
                    (unsigned long long)env.message_id);
    } else if (ack_res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Unexpected ack_tracker result=%d for message_id=%llu",
                    static_cast<int>(ack_res), (unsigned long long)env.message_id);
    }

    Result retry_res = retry_manager.on_ack(env.destination_id, env.message_id);
    if (retry_res == Result::ERR_INVALID) {
        Logger::log(Severity::INFO, "DeliveryEngine",
                    "ACK has no matching retry slot for message_id=%llu",
                    (unsigned long long)env.message_id);
    } else if (retry_res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Unexpected retry_manager result=%d for message_id=%llu",
                    static_cast<int>(retry_res), (unsigned long long)env.message_id);
    }

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Received ACK for message_id=%llu from src=%u",
                (unsigned long long)env.message_id, env.source_id);
}

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

    // REQ-7.2.1–7.2.4: zero all aggregated delivery statistics
    delivery_stats_init(m_stats);

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
Result DeliveryEngine::send_via_transport(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr); // Assert: transport valid
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);          // Assert: valid timestamp

    // Safety-critical (SC): HAZ-004 / REQ-3.3.4 — drop expired messages before I/O.
    // expiry_time_us == 0 means never-expires (handled inside timestamp_expired).
    if (timestamp_expired(env.expiry_time_us, now_us)) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Message expired before send; dropping. id=%llu",
                    (unsigned long long)env.message_id);
        return Result::ERR_EXPIRED;
    }

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
    Result res = send_via_transport(env, now_us);
    if (res != Result::OK) {
        record_send_failure(env, res);  // REQ-7.2.3: stats + log (CC-reduction helper)
        return res;
    }
    ++m_stats.msgs_sent;  // REQ-7.2.3: count successful sends

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

    // Check expiry and destination in one call (HAZ-001, HAZ-004).
    // Power of 10 rule 7: check return value.
    Result routing_res = check_routing(env, m_local_id, now_us);
    if (routing_res != Result::OK) {
        record_routing_failure(routing_res);  // REQ-7.2.3: stats (CC-reduction helper)
        return routing_res;
    }

    // Handle control messages (ACK/NAK/HEARTBEAT)
    if (envelope_is_control(env)) {
        if (env.message_type == MessageType::ACK) {
            // REQ-7.2.1: capture RTT before on_ack() releases the tracker slot
            update_latency_stats(env, now_us);
            // Power of 10 rule 7: return values checked inside process_ack().
            process_ack(m_ack_tracker, m_retry_manager, env);
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
            ++m_stats.msgs_dropped_duplicate;  // REQ-7.2.3: count duplicate drop
            Logger::log(Severity::INFO, "DeliveryEngine",
                        "Suppressed duplicate message_id=%llu from src=%u",
                        (unsigned long long)env.message_id, env.source_id);
            // Re-ACK the duplicate: the original ACK may have been lost in transit,
            // causing the sender to keep retrying. Sending ACK again stops unnecessary
            // retries without delivering the data a second time. Non-fatal on failure.
            // Safety-critical (SC): HAZ-002 — prevents spurious retry exhaustion.
            send_data_ack(*m_transport, env, m_local_id, now_us);
            return Result::ERR_DUPLICATE;
        }
        NEVER_COMPILED_OUT_ASSERT(dedup_res == Result::OK);  // Assert: check_and_record succeeded
    }

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Received data message_id=%llu from src=%u, length=%u",
                (unsigned long long)env.message_id, env.source_id,
                env.payload_length);

    // Generate and send ACK for RELIABLE_ACK and RELIABLE_RETRY messages.
    // Implements REQ-3.2.4 (automatic ACK on receive path).
    // ACK send failure is non-fatal; data is still delivered to the caller.
    if (envelope_needs_ack_response(env)) {
        send_data_ack(*m_transport, env, m_local_id, now_us);
    }

    ++m_stats.msgs_received;  // REQ-7.2.3: count successful data message delivery

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
        Result send_res = send_via_transport(m_retry_buf[i], now_us);
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

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::update_latency_stats() — REQ-7.2.1 private helper
// Called before process_ack() so the tracker slot is still PENDING.
// NSC: observability bookkeeping only.
// ─────────────────────────────────────────────────────────────────────────────

void DeliveryEngine::update_latency_stats(const MessageEnvelope& ack_env,
                                           uint64_t               now_us)
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(ack_env.message_type == MessageType::ACK);  // Assert: called for ACK only
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);  // Assert: valid timestamp

    uint64_t send_ts = 0ULL;
    // destination_id in the ACK is the local node (original sender)
    Result res = m_ack_tracker.get_send_timestamp(ack_env.destination_id,
                                                   ack_env.message_id,
                                                   send_ts);
    if (res != Result::OK) {
        // No matching slot found; skip latency update (not a tracked message)
        return;
    }

    // Guard against clock anomalies (now_us should always >= send_ts)
    if (now_us < send_ts) {
        return;
    }

    uint64_t rtt_us = now_us - send_ts;

    // Update running statistics (REQ-7.2.1)
    ++m_stats.latency_sample_count;
    m_stats.latency_sum_us += rtt_us;

    if (m_stats.latency_sample_count == 1U) {
        // First sample: initialize min and max
        m_stats.latency_min_us = rtt_us;
        m_stats.latency_max_us = rtt_us;
    } else {
        if (rtt_us < m_stats.latency_min_us) {
            m_stats.latency_min_us = rtt_us;
        }
        if (rtt_us > m_stats.latency_max_us) {
            m_stats.latency_max_us = rtt_us;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::record_send_failure() — REQ-7.2.3 private helper
// Logs a send failure warning and increments msgs_dropped_expired on ERR_EXPIRED.
// Extracted from send() to reduce its cognitive complexity to ≤ 10.
// NSC: observability bookkeeping only.
// ─────────────────────────────────────────────────────────────────────────────

void DeliveryEngine::record_send_failure(const MessageEnvelope& env, Result res)
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(res != Result::OK);      // Assert: called only on failure
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine was initialized

    if (res == Result::ERR_EXPIRED) {
        ++m_stats.msgs_dropped_expired;  // REQ-7.2.3: count expiry drops on send
    }

    Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                "Failed to send message_id=%llu: %u",
                (unsigned long long)env.message_id, static_cast<uint8_t>(res));
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::record_routing_failure() — REQ-7.2.3 private helper
// Increments the appropriate drop counter based on routing failure type.
// Extracted from receive() to reduce its cognitive complexity to ≤ 10.
// NSC: observability bookkeeping only.
// ─────────────────────────────────────────────────────────────────────────────

void DeliveryEngine::record_routing_failure(Result routing_res)
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(routing_res != Result::OK);  // Assert: called only on failure
    NEVER_COMPILED_OUT_ASSERT(m_initialized);              // Assert: engine was initialized

    if (routing_res == Result::ERR_EXPIRED) {
        ++m_stats.msgs_dropped_expired;    // REQ-7.2.3: expiry drop on receive
    } else {
        ++m_stats.msgs_dropped_misrouted;  // REQ-7.2.3: wrong destination
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::get_stats() — REQ-7.2.1, REQ-7.2.3, REQ-7.2.4
// NSC: read-only aggregation of all sub-component stats.
// ─────────────────────────────────────────────────────────────────────────────

void DeliveryEngine::get_stats(DeliveryStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine was initialized
    NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr);  // Assert: transport valid

    // Start with the local snapshot (latency, message counters)
    out = m_stats;

    // Overlay AckTracker stats (REQ-7.2.3)
    out.ack = m_ack_tracker.get_stats();

    // Overlay RetryManager stats (REQ-7.2.3)
    out.retry = m_retry_manager.get_stats();

    // Overlay transport stats (REQ-7.2.4 connections + REQ-7.2.2 impairment)
    m_transport->get_transport_stats(out.transport);

    // Copy impairment stats from transport into top-level for convenience
    out.impairment = out.transport.impairment;

    // REQ-7.2.4: fatal assertion count from process-wide global
    out.fatal_count = assert_state::get_fatal_count();
}
