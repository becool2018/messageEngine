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
 * Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-3.3.5, REQ-6.1.10, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4, REQ-7.2.5
 */
// Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-3.3.5, REQ-6.1.10, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4, REQ-7.2.5

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

    // Initialize message ID generator: mix local_id with current timestamp to
    // prevent the sequence from being trivially predictable (CERT INT30-C / S2).
    // timestamp_now_us() is available via Timestamp.hpp (included in DeliveryEngine.hpp).
    const uint64_t ts_seed = timestamp_now_us();
    uint64_t id_seed = (static_cast<uint64_t>(local_id) << 32U) ^ ts_seed;
    if (id_seed == 0ULL) {
        id_seed = 1ULL;  // Ensure non-zero (0 is reserved for invalid messages)
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

    // REQ-7.2.5: initialize the observability event ring
    m_events.init();

    // REQ-3.2.3, REQ-3.3.3: initialize reassembly buffer
    m_reassembly.init();

    // REQ-3.3.5: initialize ordering buffer
    m_ordering.init(local_id);

    // REQ-3.3.5: zero outbound sequence state (bounded loop)
    // Power of 10 Rule 2: loop bound is compile-time constant ACK_TRACKER_CAPACITY
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        m_seq_state[i].active   = false;
        m_seq_state[i].dst      = 0U;
        m_seq_state[i].next_seq = 1U;  // sequences start at 1
    }

    // REQ-3.2.3: zero fragment staging buffer (bounded loop)
    // Power of 10 Rule 2: loop bound is compile-time constant FRAG_MAX_COUNT
    for (uint32_t i = 0U; i < FRAG_MAX_COUNT; ++i) {
        envelope_init(m_frag_buf[i]);
    }

    // REQ-3.3.5: zero the held-pending ordering slot (Issue 1 fix)
    envelope_init(m_held_pending);
    m_held_pending_valid = false;

    m_initialized = true;

    // REQ-6.1.10: notify transport of our NodeId so TCP/TLS clients can send
    // the on-connect HELLO registration frame.
    Result reg_res = m_transport->register_local_id(local_id);
    if (!result_ok(reg_res)) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "register_local_id failed: %u", static_cast<uint8_t>(reg_res));
        // Non-fatal: transport is open; routing may be degraded for TCP/TLS unicast.
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: engine marked initialized
    NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr);  // Assert: transport is valid

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Initialized channel=%u, local_id=%u",
                cfg.channel_id, local_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::emit_event() — REQ-7.2.5 private helper
// Builds a DeliveryEvent and pushes it into the ring. Overwrite-on-full ring
// semantics ensure this is bounded and never fails.
// NSC: observability bookkeeping only; no effect on delivery state.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void DeliveryEngine::emit_event(DeliveryEventKind kind,
                                uint64_t          message_id,
                                NodeId            node_id,
                                uint64_t          timestamp_us,
                                Result            result)
{
    // Power of 10 Rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);       // Assert: engine initialized before emitting
    NEVER_COMPILED_OUT_ASSERT(timestamp_us > 0ULL); // Assert: valid timestamp

    DeliveryEvent ev;
    ev.kind         = kind;
    ev.message_id   = message_id;
    ev.node_id      = node_id;
    ev.timestamp_us = timestamp_us;
    ev.result       = result;

    m_events.push(ev);  // bounded overwrite-on-full ring push (REQ-7.2.5)
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::emit_routing_drop_event() — REQ-7.2.5 CC-reduction helper
// Emits EXPIRY_DROP or MISROUTE_DROP based on routing result.
// Extracted from receive() to keep its CC ≤ 10.
// NSC: observability bookkeeping only; no effect on delivery state.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void DeliveryEngine::emit_routing_drop_event(const MessageEnvelope& env,
                                              Result                 routing_res,
                                              uint64_t               now_us)
{
    // Power of 10 Rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(routing_res != Result::OK);  // Assert: called only on drop
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);              // Assert: valid timestamp

    if (routing_res == Result::ERR_EXPIRED) {
        emit_event(DeliveryEventKind::EXPIRY_DROP,
                   env.message_id, env.source_id, now_us, routing_res);
    } else {
        emit_event(DeliveryEventKind::MISROUTE_DROP,
                   env.message_id, env.source_id, now_us, routing_res);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::account_ordering_expiry_drops() — Issue 3 (Round 4) fix
// Increments msgs_dropped_expired and emits one EXPIRY_DROP event per freed
// ordering-hold envelope.  Called after sweep_expired_holds() in both
// sweep_ack_timeouts() and handle_ordering_gate().
// NSC: observability bookkeeping only; no effect on delivery state.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void DeliveryEngine::account_ordering_expiry_drops(const MessageEnvelope* freed,
                                                    uint32_t               count,
                                                    uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(count <= ORDERING_HOLD_COUNT);  // Assert: bounded count
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);                 // Assert: valid timestamp

    if (count == 0U) {
        return;  // nothing to account
    }

    // Power of 10 Rule 5: pointer is non-null whenever count > 0
    NEVER_COMPILED_OUT_ASSERT(freed != nullptr);  // Assert: caller supplied buffer

    m_stats.msgs_dropped_expired += count;  // REQ-7.2.3: count ordering expiry drops

    // Power of 10 Rule 2: bounded loop (count <= ORDERING_HOLD_COUNT).
    for (uint32_t i = 0U; i < count; ++i) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Ordering hold expired: message_id=%llu from src=%u dropped",
                    (unsigned long long)freed[i].message_id, freed[i].source_id);
        // REQ-7.2.5: emit EXPIRY_DROP event for each freed ordering hold.
        emit_event(DeliveryEventKind::EXPIRY_DROP,
                   freed[i].message_id, freed[i].source_id, now_us,
                   Result::ERR_EXPIRED);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::poll_event() — REQ-7.2.5 public observability API
// Pops one event from the ring. Returns ERR_EMPTY when no events are pending.
// NSC: observability only; no effect on delivery state.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::poll_event(DeliveryEvent& out)
{
    // Power of 10 Rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                                         // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(m_events.size() <= DELIVERY_EVENT_RING_CAPACITY);       // Assert: ring bounded

    return m_events.pop(out);
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::pending_event_count() — REQ-7.2.5 public observability API
// Returns the number of unread events in the ring.
// NSC: read-only observability; no effect on delivery.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t DeliveryEngine::pending_event_count() const
{
    // Power of 10 Rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                    // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(DELIVERY_EVENT_RING_CAPACITY > 0U); // Assert: capacity invariant

    return m_events.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::drain_events() — REQ-7.2.5 bulk observability drain API
// Pops up to buf_cap events from the ring into the caller-supplied buffer.
// Equivalent to calling poll_event() in a bounded loop.
// NSC: observability only; no effect on delivery state.
// Power of 10: fixed loop bound (buf_cap or ring size, whichever is smaller);
//   >=2 assertions; no dynamic allocation.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t DeliveryEngine::drain_events(DeliveryEvent* out_buf, uint32_t buf_cap)
{
    // Power of 10 Rule 5: >=2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr || buf_cap == 0U); // Assert: buf valid when cap > 0

    uint32_t drained = 0U;

    // Power of 10 Rule 2: bounded loop — at most buf_cap iterations,
    // and at most DELIVERY_EVENT_RING_CAPACITY events can ever be present.
    // Loop terminates when the ring is empty or buf_cap is reached.
    for (uint32_t i = 0U; i < buf_cap; ++i) {
        Result pop_res = m_events.pop(out_buf[drained]);
        if (pop_res == Result::ERR_EMPTY) {
            break;
        }
        ++drained;
    }

    // Power of 10 Rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(drained <= buf_cap);  // Assert: did not overflow caller buffer

    return drained;
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
// reserve_bookkeeping() — file-static CC-reduction helper for send().
//
// Reserves AckTracker and (for RELIABLE_RETRY) RetryManager slots BEFORE any
// I/O.  Ordering rationale: if either reservation fails we return an error and
// the peer never sees the message, preserving the at-most-once contract.
//
// Rollback on RetryManager failure: cancel() frees the already-reserved
// AckTracker slot directly (PENDING→FREE) without incrementing acks_received.
// This avoids the phantom ACK stat that on_ack() would have produced.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
static Result reserve_bookkeeping(AckTracker&            ack_tracker,
                                   RetryManager&          retry_manager,
                                   const MessageEnvelope& env,
                                   const ChannelConfig&   cfg,
                                   uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);       // Assert: valid timestamp
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: envelope fields set

    // Only reliable classes require bookkeeping slots
    if (env.reliability_class != ReliabilityClass::RELIABLE_ACK &&
        env.reliability_class != ReliabilityClass::RELIABLE_RETRY) {
        return Result::OK;
    }

    uint64_t ack_deadline = timestamp_deadline_us(now_us, cfg.recv_timeout_ms);

    // Reserve AckTracker slot.  On failure: abort before any I/O.
    // Power of 10 rule 7: check return value
    Result track_res = ack_tracker.track(env, ack_deadline);
    if (track_res != Result::OK) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "Failed to track ACK for message_id=%llu (result=%u); "
                    "aborting send to preserve at-most-once contract",
                    (unsigned long long)env.message_id, static_cast<uint8_t>(track_res));
        return track_res;
    }

    // RELIABLE_ACK: only AckTracker slot needed — done.
    if (env.reliability_class != ReliabilityClass::RELIABLE_RETRY) {
        return Result::OK;
    }

    // RELIABLE_RETRY: also reserve a RetryManager slot.
    // Power of 10 rule 7: check return value
    Result sched_res = retry_manager.schedule(env,
                                              cfg.max_retries,
                                              cfg.retry_backoff_ms,
                                              now_us);
    if (sched_res != Result::OK) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "Failed to schedule retry for message_id=%llu (result=%u); "
                    "aborting send to preserve at-most-once contract",
                    (unsigned long long)env.message_id, static_cast<uint8_t>(sched_res));
        // Roll back AckTracker reservation: cancel() frees the slot directly
        // without incrementing acks_received (no phantom ACK stat).
        // Power of 10 Rule 7 / HAZ-001: check cancel() return value; log WARNING_HI
        // on failure so a stale PENDING slot can be detected and swept later.
        Result cancel_res = ack_tracker.cancel(env.source_id, env.message_id);
        if (cancel_res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                        "cancel() failed during rollback for message_id=%llu (result=%u); "
                        "stale PENDING slot may persist until sweep",
                        (unsigned long long)env.message_id,
                        static_cast<uint8_t>(cancel_res));
        }
        return sched_res;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// rollback_on_transport_failure() — file-static CC-reduction helper for send().
//
// Called when send_via_transport() fails after bookkeeping slots have already
// been reserved AND no fragment reached the wire.  Cancels AckTracker and
// (for RELIABLE_RETRY) RetryManager slots so no spurious timeout or retry
// fires for a message never put on wire.
//
// Must NOT be called when res == ERR_IO_PARTIAL: at least one fragment is
// already on the wire and the bookkeeping must remain active for timeout/retry.
//
// Power of 10 Rule 7 / HAZ-001: both cancel() return values are checked and
// logged at WARNING_HI on failure so stale slots can be detected and swept.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
static void rollback_on_transport_failure(AckTracker&            ack_tracker,
                                          RetryManager&          retry_manager,
                                          const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));   // Assert: envelope fields set
    NEVER_COMPILED_OUT_ASSERT(                        // Assert: only called for reliable classes
        env.reliability_class == ReliabilityClass::RELIABLE_ACK ||
        env.reliability_class == ReliabilityClass::RELIABLE_RETRY);

    // Always cancel the AckTracker slot for both reliable classes.
    // cancel() frees the slot directly without incrementing acks_received
    // (avoids phantom ACK stat).
    Result ack_cancel_res = ack_tracker.cancel(env.source_id, env.message_id);
    if (ack_cancel_res != Result::OK) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "ack_tracker.cancel() failed during rollback for "
                    "message_id=%llu (result=%u); stale PENDING slot may "
                    "persist until sweep",
                    (unsigned long long)env.message_id,
                    static_cast<uint8_t>(ack_cancel_res));
    }

    // Also cancel the RetryManager slot for RELIABLE_RETRY messages.
    if (env.reliability_class == ReliabilityClass::RELIABLE_RETRY) {
        Result retry_cancel_res = retry_manager.cancel(env.source_id, env.message_id);
        if (retry_cancel_res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                        "retry_manager.cancel() failed during rollback for "
                        "message_id=%llu (result=%u); stale retry slot may "
                        "persist until sweep",
                        (unsigned long long)env.message_id,
                        static_cast<uint8_t>(retry_cancel_res));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handle_send_fragments_failure() — file-static CC-reduction helper for send().
//
// Processes the failure result from send_fragments():
//   - If res == ERR_IO_PARTIAL: at least one fragment is already on the wire;
//     rollback MUST NOT happen (bookkeeping stays active for ACK timeout/retry).
//     Returns ERR_IO so callers see a uniform transport-failure code.
//   - Otherwise (ERR_IO, ERR_INVALID): nothing on wire; rollback is safe for
//     reliable classes.  Returns res unchanged.
//
// Precondition: res != OK (caller checked).
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
static Result handle_send_fragments_failure(AckTracker&            ack_tracker,
                                            RetryManager&          retry_manager,
                                            const MessageEnvelope& env,
                                            Result                 res)
{
    NEVER_COMPILED_OUT_ASSERT(res != Result::OK);          // Assert: only called on failure
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));        // Assert: envelope fields set

    if (res == Result::ERR_IO_PARTIAL) {
        // Fragment 0..i-1 already transmitted; cannot undo.  Keep bookkeeping
        // active.  Normalise to ERR_IO for the public API.
        return Result::ERR_IO;
    }

    // No fragment reached the wire: safe to cancel bookkeeping entries.
    if (env.reliability_class == ReliabilityClass::RELIABLE_ACK ||
        env.reliability_class == ReliabilityClass::RELIABLE_RETRY) {
        rollback_on_transport_failure(ack_tracker, retry_manager, env);
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

    // Step 1: assign envelope identity fields.  These must be set before any
    // bookkeeping call because track() and schedule() read env.source_id,
    // env.message_id, and env.timestamp_us from the envelope.
    // Power of 10 rule 7: check all function returns
    env.source_id    = m_local_id;
    env.message_id   = m_id_gen.next();
    env.timestamp_us = now_us;

    // REQ-3.3.5: assign per-destination sequence number only for ORDERED DATA messages.
    // Unordered channels must leave sequence_num == 0 so the receiver bypasses the
    // ordering gate. Assigning a sequence on an UNORDERED channel would cause the
    // receiver to hold every message in the ordering buffer indefinitely (Issue 2 fix).
    if (envelope_is_data(env) &&
        env.sequence_num == 0U &&
        m_cfg.ordering == OrderingMode::ORDERED) {
        env.sequence_num = next_seq_for(env.destination_id);
    }

    // REQ-3.2.3: initialize fragment fields for a new unfragmented message.
    // Fragmentation is applied in send_fragments(); these are the logical defaults.
    env.fragment_index       = 0U;
    env.fragment_count       = 1U;
    env.total_payload_length = static_cast<uint16_t>(env.payload_length);

    // Step 2: validate and reserve bookkeeping capacity BEFORE touching the
    // transport.  Returns an error if any tracker is full; in that case we
    // return immediately without calling send_via_transport().
    // Power of 10 rule 7: check return value
    Result book_res = reserve_bookkeeping(m_ack_tracker, m_retry_manager,
                                          env, m_cfg, now_us);
    if (book_res != Result::OK) {
        record_send_failure(env, book_res);  // REQ-7.2.3: stats + log
        return book_res;
    }

    // Step 3: all bookkeeping confirmed; now commit to I/O.
    // REQ-3.2.3: use send_fragments() which handles both single-frame and
    // multi-fragment sends transparently.
    Result res = send_fragments(env, now_us);
    if (res != Result::OK) {
        // CC-reduction: rollback decision delegated to handle_send_fragments_failure().
        // That helper rolls back bookkeeping only when nothing reached the wire
        // (ERR_IO / ERR_INVALID); it preserves bookkeeping on ERR_IO_PARTIAL
        // (≥1 fragment already sent) and normalises the result to ERR_IO.
        // Power of 10 Rule 7 / HAZ-001: cancel() return values are checked
        // and logged inside rollback_on_transport_failure() (CC-reduction helper).
        Result reported = handle_send_fragments_failure(
            m_ack_tracker, m_retry_manager, env, res);
        record_send_failure(env, reported);  // REQ-7.2.3: stats + log
        return reported;
    }

    // REQ-7.2.3: count successful sends -- only after all bookkeeping AND transport succeed
    ++m_stats.msgs_sent;

    // REQ-7.2.5: emit SEND_OK observability event
    emit_event(DeliveryEventKind::SEND_OK,
               env.message_id, env.destination_id, now_us, Result::OK);

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(env.source_id == m_local_id);  // Assert: source set correctly
    NEVER_COMPILED_OUT_ASSERT(env.message_id != 0ULL);       // Assert: message_id assigned

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Sent message_id=%llu, reliability=%u",
                (unsigned long long)env.message_id,
                static_cast<uint8_t>(env.reliability_class));

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::handle_control_message() — NSC private helper
// Process an inbound ACK/NAK/HEARTBEAT. Always returns (no result code).
// Extracted from receive() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void DeliveryEngine::handle_control_message(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);               // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_is_control(env));    // Assert: is control message

    if (env.message_type == MessageType::ACK) {
        // REQ-7.2.1: capture RTT before on_ack() releases the tracker slot
        update_latency_stats(env, now_us);
        // Power of 10 rule 7: return values checked inside process_ack().
        process_ack(m_ack_tracker, m_retry_manager, env);
        // REQ-7.2.5: emit ACK_RECEIVED event
        emit_event(DeliveryEventKind::ACK_RECEIVED,
                   env.message_id, env.source_id, now_us, Result::OK);
    }
    // For NAK and HEARTBEAT, no additional action required.
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::handle_data_dedup() — SC: HAZ-003 private helper
// Apply duplicate suppression for RELIABLE_RETRY DATA messages.
// Returns ERR_DUPLICATE if suppressed; OK otherwise.
// Extracted from receive() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::handle_data_dedup(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);         // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env)); // Assert: is DATA message

    if (env.reliability_class != ReliabilityClass::RELIABLE_RETRY) {
        return Result::OK;  // dedup only applies to RELIABLE_RETRY
    }

    // Power of 10 rule 7: check return value
    Result dedup_res = m_dedup.check_and_record(env.source_id, env.message_id);
    if (dedup_res != Result::ERR_DUPLICATE) {
        NEVER_COMPILED_OUT_ASSERT(dedup_res == Result::OK);  // Assert: no other error expected
        return Result::OK;
    }

    // Duplicate detected: suppress delivery
    ++m_stats.msgs_dropped_duplicate;  // REQ-7.2.3: count duplicate drop
    Logger::log(Severity::INFO, "DeliveryEngine",
                "Suppressed duplicate message_id=%llu from src=%u",
                (unsigned long long)env.message_id, env.source_id);
    // Re-ACK the duplicate: the original ACK may have been lost in transit,
    // causing the sender to keep retrying. Sending ACK again stops unnecessary
    // retries without delivering the data a second time. Non-fatal on failure.
    // Safety-critical (SC): HAZ-002 — prevents spurious retry exhaustion.
    send_data_ack(*m_transport, env, m_local_id, now_us);
    // REQ-7.2.5: emit DUPLICATE_DROP event
    emit_event(DeliveryEventKind::DUPLICATE_DROP,
               env.message_id, env.source_id, now_us, Result::ERR_DUPLICATE);
    return Result::ERR_DUPLICATE;
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

    // REQ-3.3.5: drain any previously-held ordering message before blocking on the
    // transport. If a prior in-order delivery left a contiguous message staged in
    // m_held_pending, return it immediately so the backlog drains without waiting
    // for the next wire frame (Issue 1 fix). If the held message has since expired,
    // deliver_held_pending() returns ERR_EXPIRED and clears the slot; fall through
    // to the transport poll so the caller gets a live message or ERR_TIMEOUT.
    if (m_held_pending_valid) {
        Result held_res = deliver_held_pending(env, now_us);
        if (held_res == Result::OK) {
            return Result::OK;
        }
        // ERR_EXPIRED: held message dropped; fall through to transport receive.
    }

    // Power of 10 rule 7: check return value from transport
    // wire_env holds the raw wire frame; logical_env will hold the assembled message.
    MessageEnvelope wire_env;
    envelope_init(wire_env);
    Result res = m_transport->receive_message(wire_env, timeout_ms);
    if (res != Result::OK) {
        // Normal timeout case (ERR_TIMEOUT is expected)
        return res;
    }

    // Power of 10 rule 5: validate received envelope
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(wire_env));  // Assert: transport provides valid envelope

    // REQ-3.2.3: Reassembly gate — route through ReassemblyBuffer.
    // For single-frame messages (fragment_count == 1), this completes immediately.
    // For multi-frame messages, ERR_AGAIN is returned until all fragments arrive.
    MessageEnvelope logical_env;
    envelope_init(logical_env);
    Result frag_res = handle_fragment_ingest(wire_env, logical_env, now_us);
    if (frag_res != Result::OK) {
        // ERR_AGAIN: still collecting; not an error for the caller.
        // ERR_FULL/ERR_INVALID: fragment-level error; drop frame.
        return frag_res;
    }

    // From here on, work with the fully-assembled logical_env.
    // Check expiry and destination in one call (HAZ-001, HAZ-004).
    // Power of 10 rule 7: check return value.
    Result routing_res = check_routing(logical_env, m_local_id, now_us);
    // Assign env from the logical assembled envelope for downstream use.
    envelope_copy(env, logical_env);
    if (routing_res != Result::OK) {
        record_routing_failure(routing_res);             // REQ-7.2.3: stats (CC-reduction helper)
        emit_routing_drop_event(env, routing_res, now_us);  // REQ-7.2.5: CC-reduction helper
        return routing_res;
    }

    // Handle control messages (ACK/NAK/HEARTBEAT) — CC-reduction: delegated to helper.
    if (envelope_is_control(env)) {
        handle_control_message(env, now_us);
        return Result::OK;
    }

    // Handle data messages — CC-reduction: delegated to handle_data_path().
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env));  // Assert: is data message
    return handle_data_path(env, now_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::handle_data_path() — SC: HAZ-001, HAZ-003 private helper
// Runs the full data-message delivery path: dedup → ordering gate → ACK → stats.
// Extracted from receive() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::handle_data_path(MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env));  // Assert: caller guarantees DATA

    // Apply duplicate suppression — CC-reduction: delegated to handle_data_dedup().
    Result dedup_res = handle_data_dedup(env, now_us);
    if (dedup_res != Result::OK) {
        return dedup_res;
    }

    // REQ-3.3.5: Ordering gate — route through OrderingBuffer for DATA messages.
    // Control messages and UNORDERED (sequence_num==0) pass through in ingest().
    MessageEnvelope ordered_env;
    envelope_init(ordered_env);
    Result ord_res = handle_ordering_gate(env, ordered_env, now_us);
    if (ord_res == Result::ERR_FULL) {
        // Issue 5 fix: hard drop due to exhausted ordering resources — propagate
        // ERR_FULL so callers can distinguish a real capacity drop from a benign hold.
        ++m_stats.msgs_dropped_ordering_full;  // REQ-7.2.3: ordering resource exhaustion (not a routing error)
        return Result::ERR_FULL;
    }
    if (ord_res != Result::OK) {
        // ERR_AGAIN: held (out-of-order) or discarded (duplicate seq) — benign.
        return Result::ERR_AGAIN;
    }
    envelope_copy(env, ordered_env);

    // REQ-3.3.5: attempt to drain the next contiguous held message for this source
    // (Issue 1 fix). If a previously held message is now the next in sequence, stage
    // it in m_held_pending so the subsequent receive() call delivers it without waiting
    // for the transport. Power of 10 Rule 7: return value of try_release_next checked.
    {
        MessageEnvelope next_held;
        envelope_init(next_held);
        Result rel_res = m_ordering.try_release_next(env.source_id, next_held);
        if (rel_res == Result::OK) {
            envelope_copy(m_held_pending, next_held);
            m_held_pending_valid = true;
        }
    }

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Received data message_id=%llu from src=%u, length=%u",
                (unsigned long long)env.message_id, env.source_id,
                env.payload_length);

    // Generate and send ACK for RELIABLE_ACK and RELIABLE_RETRY messages.
    // Implements REQ-3.2.4 (automatic ACK on receive path).
    // ACK is sent per logical message (post-reassembly), not per fragment.
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

    // Re-send each (with fragmentation if needed — REQ-3.2.3)
    // Power of 10 rule 2: bounded loop (collected is ≤ MSG_RING_CAPACITY)
    for (uint32_t i = 0U; i < collected; ++i) {
        // Power of 10 rule 7: check return value
        // send_fragments() re-fragments on retry as needed (REQ-3.2.3)
        Result send_res = send_fragments(m_retry_buf[i], now_us);
        if (send_res != Result::OK) {
            Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                        "Retry send failed for message_id=%llu (result=%u)",
                        (unsigned long long)m_retry_buf[i].message_id,
                        static_cast<uint8_t>(send_res));
        } else {
            Logger::log(Severity::INFO, "DeliveryEngine",
                        "Retried message_id=%llu",
                        (unsigned long long)m_retry_buf[i].message_id);
            // REQ-7.2.5: emit RETRY_FIRED event for successful retry transmission
            emit_event(DeliveryEventKind::RETRY_FIRED,
                       m_retry_buf[i].message_id,
                       m_retry_buf[i].destination_id,
                       now_us, Result::OK);
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

    // REQ-3.2.3: sweep stale reassembly slots (Issue 3 fix).
    // m_reassembly.sweep_expired() was never called, causing fragment slots for stalled
    // senders to be held indefinitely. Housekeeping here keeps the buffer live.
    m_reassembly.sweep_expired(now_us);

    // REQ-3.2.9: sweep stale reassembly slots — prevents slot exhaustion from partial sends.
    {
        const uint64_t stale_us = static_cast<uint64_t>(m_cfg.recv_timeout_ms) * 1000ULL;
        const uint32_t stale_freed = m_reassembly.sweep_stale(now_us, stale_us);
        if (stale_freed > 0U) {
            Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                        "sweep_stale freed %u stale reassembly slot(s)", stale_freed);
        }
    }

    // REQ-3.3.5 / Issue 3 fix: advance the ordering gate past permanently lost gaps.
    // Without this, a single lost ordered message blocks all later messages from
    // that source indefinitely. sweep_expired_holds() frees any held slots whose
    // message has expired and calls advance_sequence() so the gate unblocks.
    // Issue 3 (Round 4) fix: also update stats and emit EXPIRY_DROP events.
    // Power of 10 Rule 7: return value checked (count of freed slots).
    {
        uint32_t freed_holds = m_ordering.sweep_expired_holds(
                                   now_us, m_ordering_freed_buf, ORDERING_HOLD_COUNT);
        account_ordering_expiry_drops(m_ordering_freed_buf, freed_holds, now_us);
    }

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
        // REQ-7.2.5: emit ACK_TIMEOUT event
        emit_event(DeliveryEventKind::ACK_TIMEOUT,
                   m_timeout_buf[i].message_id,
                   m_timeout_buf[i].destination_id,
                   now_us, Result::ERR_TIMEOUT);
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
// Also emits a SEND_FAIL or EXPIRY_DROP observability event (REQ-7.2.5).
// NSC: observability bookkeeping only.
// ─────────────────────────────────────────────────────────────────────────────

void DeliveryEngine::record_send_failure(const MessageEnvelope& env, Result res)
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(res != Result::OK);      // Assert: called only on failure
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine was initialized

    if (res == Result::ERR_EXPIRED) {
        ++m_stats.msgs_dropped_expired;  // REQ-7.2.3: count expiry drops on send
        // REQ-7.2.5: emit EXPIRY_DROP event for send-path expiry
        emit_event(DeliveryEventKind::EXPIRY_DROP,
                   env.message_id, env.destination_id, env.timestamp_us, res);
    } else {
        // REQ-7.2.5: emit SEND_FAIL event for non-expiry send failures
        emit_event(DeliveryEventKind::SEND_FAIL,
                   env.message_id, env.destination_id, env.timestamp_us, res);
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
// DeliveryEngine::next_seq_for() — REQ-3.3.5 private helper
// Returns the next outbound sequence number for dst, allocating a per-destination
// slot if needed. Returns 0 if all slots are in use (should not occur in practice
// given ACK_TRACKER_CAPACITY >= MAX_PEER_NODES).
// NSC: sequence counter bookkeeping only.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t DeliveryEngine::next_seq_for(NodeId dst)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);    // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(dst != 0U);         // Assert: valid destination

    // Search for existing slot
    // Power of 10 Rule 2: bounded loop — ACK_TRACKER_CAPACITY is a compile-time constant
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (m_seq_state[i].active && m_seq_state[i].dst == dst) {
            uint32_t seq = m_seq_state[i].next_seq;
            ++m_seq_state[i].next_seq;
            // Wrap: skip 0 (reserved for UNORDERED)
            if (m_seq_state[i].next_seq == 0U) {
                m_seq_state[i].next_seq = 1U;
            }
            return seq;
        }
    }

    // Allocate a new slot
    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        if (!m_seq_state[i].active) {
            m_seq_state[i].active   = true;
            m_seq_state[i].dst      = dst;
            m_seq_state[i].next_seq = 2U;  // first call returns 1
            NEVER_COMPILED_OUT_ASSERT(m_seq_state[i].active);  // Assert: slot allocated
            return 1U;
        }
    }

    // All slots in use: log and return 0 (caller treats as UNORDERED)
    Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                "Sequence state full; cannot assign sequence for dst=%u", dst);
    return 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::send_fragments() — REQ-3.2.3, REQ-3.3.5 private helper
// Fragments the logical envelope (if needed) and sends each wire frame via
// send_via_transport(). Returns OK if all frames were sent, ERR_IO if any
// frame fails.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::send_fragments(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));    // Assert: valid envelope

    if (!needs_fragmentation(env)) {
        // Single wire frame: send as-is
        return send_via_transport(env, now_us);
    }

    // Fragment and send each wire frame
    uint32_t frag_count = fragment_message(env, m_frag_buf, FRAG_MAX_COUNT);
    if (frag_count == 0U) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "fragment_message failed for message_id=%llu",
                    (unsigned long long)env.message_id);
        return Result::ERR_INVALID;
    }

    NEVER_COMPILED_OUT_ASSERT(frag_count <= FRAG_MAX_COUNT);  // Assert: bounded

    // Bug fix: track whether any fragment was successfully transmitted to the
    // wire.  The caller (send()) calls rollback_on_transport_failure() on any
    // non-OK result from this function, but rollback is only safe when zero
    // fragments reached the wire — once a fragment is sent it cannot be recalled.
    // We signal this via a distinct error code so send() can skip the rollback.
    // ERR_IO_PARTIAL is returned when at least one fragment was sent before
    // the failure; ERR_IO is returned when the very first fragment failed
    // (nothing on wire → rollback is valid).
    bool any_sent = false;

    // Power of 10 Rule 2: bounded loop — frag_count <= FRAG_MAX_COUNT
    for (uint32_t i = 0U; i < frag_count; ++i) {
        Result res = send_via_transport(m_frag_buf[i], now_us);
        if (res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                        "Fragment %u/%u send failed for message_id=%llu (result=%u)",
                        i + 1U, frag_count,
                        (unsigned long long)env.message_id,
                        static_cast<uint8_t>(res));
            // Return ERR_IO_PARTIAL when prior fragments were already sent so
            // the caller knows rollback would be incorrect.  Return plain ERR_IO
            // when nothing reached the wire (rollback is safe).
            return any_sent ? Result::ERR_IO_PARTIAL : Result::ERR_IO;
        }
        any_sent = true;
    }

    NEVER_COMPILED_OUT_ASSERT(any_sent || frag_count == 0U);  // Assert: all frags sent
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::handle_fragment_ingest() — REQ-3.2.3 private helper
// Routes the received wire envelope through the reassembly buffer.
// Returns OK when logical_out is ready; ERR_AGAIN when more fragments needed;
// ERR_FULL/ERR_INVALID on error.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::handle_fragment_ingest(const MessageEnvelope& wire_env,
                                               MessageEnvelope&       logical_out,
                                               uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);           // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(wire_env)); // Assert: valid envelope

    Result res = m_reassembly.ingest(wire_env, logical_out, now_us);

    if (res == Result::ERR_AGAIN) {
        // Normal: still collecting fragments for this message
        return Result::ERR_AGAIN;
    }

    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Reassembly failed for message_id=%llu (result=%u)",
                    (unsigned long long)wire_env.message_id,
                    static_cast<uint8_t>(res));
    }

    NEVER_COMPILED_OUT_ASSERT(res == Result::OK ||
                               res == Result::ERR_AGAIN ||
                               res == Result::ERR_FULL ||
                               res == Result::ERR_INVALID);  // Assert: known result code
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::handle_ordering_gate() — REQ-3.3.5 private helper
// Routes a fully-reassembled logical DATA message through the ordering gate.
// Returns OK (out ready), ERR_AGAIN (held or discarded), or ERR_FULL (hold full).
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::handle_ordering_gate(const MessageEnvelope& logical,
                                             MessageEnvelope&       out,
                                             uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);             // Assert: engine initialized
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(logical));    // Assert: valid envelope

    // Issue 3 fix: sweep expired ordering holds on every data-receive call so that
    // ordered BEST_EFFORT channels recover from lost gaps without requiring the caller
    // to invoke sweep_ack_timeouts() (which has ACK-specific naming and may be skipped
    // for BEST_EFFORT flows). ORDERING_HOLD_COUNT = 8 — bounded overhead per receive.
    // Issue 3 (Round 4) fix: collect freed envelopes and account for expiry drops.
    // Power of 10 Rule 7: return value checked (count of freed slots).
    {
        uint32_t gate_freed = m_ordering.sweep_expired_holds(
                                  now_us, m_ordering_freed_buf, ORDERING_HOLD_COUNT);
        account_ordering_expiry_drops(m_ordering_freed_buf, gate_freed, now_us);
    }

    Result res = m_ordering.ingest(logical, out, now_us);
    if (res == Result::ERR_FULL) {
        Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                    "Ordering hold buffer full for src=%u", logical.source_id);
    }

    NEVER_COMPILED_OUT_ASSERT(res == Result::OK ||
                               res == Result::ERR_AGAIN ||
                               res == Result::ERR_FULL);  // Assert: known result code
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine::deliver_held_pending() — REQ-3.3.5 private helper (Issue 1 fix)
// Delivers the message staged in m_held_pending, attempts to stage the next
// contiguous held message via try_release_next(), sends ACK if required, and
// increments the received-message counter.  Called from receive() when
// m_held_pending_valid is true so that ordering backlogs drain across consecutive
// receive() calls without waiting for additional wire frames.
// NSC: ordering bookkeeping on the non-reliability housekeeping path.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result DeliveryEngine::deliver_held_pending(MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_held_pending_valid);  // Assert: called only when valid
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: engine initialized

    m_held_pending_valid = false;
    envelope_copy(env, m_held_pending);

    // REQ-3.2.7 / Issue 2 fix: re-check expiry on the held message.
    // A message can be valid when first held but expire during a sequence gap.
    if (timestamp_expired(env.expiry_time_us, now_us)) {
        Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                    "Held ordered message_id=%llu from src=%u expired; dropping",
                    (unsigned long long)env.message_id, env.source_id);
        ++m_stats.msgs_dropped_expired;  // REQ-7.2.3
        emit_event(DeliveryEventKind::EXPIRY_DROP,
                   env.message_id, env.source_id, now_us, Result::ERR_EXPIRED);
        // Still attempt to drain the next held frame so the backlog advances.
        {
            MessageEnvelope next_held;
            envelope_init(next_held);
            Result rel_res = m_ordering.try_release_next(env.source_id, next_held);
            if (rel_res == Result::OK) {
                envelope_copy(m_held_pending, next_held);
                m_held_pending_valid = true;
            }
        }
        NEVER_COMPILED_OUT_ASSERT(m_stats.msgs_dropped_expired > 0U);  // Assert: counted
        return Result::ERR_EXPIRED;
    }

    // Try to stage the next contiguous held message for this source.
    // Power of 10 Rule 7: return value of try_release_next checked.
    {
        MessageEnvelope next_held;
        envelope_init(next_held);
        Result rel_res = m_ordering.try_release_next(env.source_id, next_held);
        if (rel_res == Result::OK) {
            envelope_copy(m_held_pending, next_held);
            m_held_pending_valid = true;
        }
    }

    Logger::log(Severity::INFO, "DeliveryEngine",
                "Delivered held message_id=%llu from src=%u (ordering backlog drain)",
                (unsigned long long)env.message_id, env.source_id);

    // Generate and send ACK (held messages were not ACK'd when originally held).
    if (envelope_needs_ack_response(env)) {
        send_data_ack(*m_transport, env, m_local_id, now_us);
    }

    ++m_stats.msgs_received;  // REQ-7.2.3: count successful data message delivery

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: envelope valid on exit
    return Result::OK;
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
