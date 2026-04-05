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
 * @file DeliveryEngine.hpp
 * @brief Top-level coordinator tying transport, ACK tracking, retry, and dedup together.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §2 (layered architecture),
 * CLAUDE.md §3.2 and §3.3 (messaging utilities and delivery semantics).
 *
 * Rules applied:
 *   - Power of 10: no recursion, fixed bounds, assertions, checked returns.
 *   - F-Prime style: no STL, no exceptions, no templates.
 *   - Architecture: bridges message/transport layers; hides reliability details.
 *
 * Design: Owns AckTracker, RetryManager, DuplicateFilter. Applies all delivery
 * semantics based on message envelope reliability_class field.
 *
 * Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-3.3.5, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4, REQ-7.2.5
 */
// Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4, REQ-3.3.5, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4, REQ-7.2.5

#ifndef CORE_DELIVERY_ENGINE_HPP
#define CORE_DELIVERY_ENGINE_HPP

#include "Assert.hpp"
#include <cstdint>
#include "Types.hpp"
#include "MessageEnvelope.hpp"
#include "TransportInterface.hpp"
#include "ChannelConfig.hpp"
#include "AckTracker.hpp"
#include "RetryManager.hpp"
#include "DuplicateFilter.hpp"
#include "MessageId.hpp"
#include "Timestamp.hpp"
#include "Logger.hpp"
#include "DeliveryStats.hpp"
#include "AssertState.hpp"
#include "DeliveryEvent.hpp"
#include "DeliveryEventRing.hpp"
#include "ReassemblyBuffer.hpp"
#include "OrderingBuffer.hpp"
#include "Fragmentation.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine
// ─────────────────────────────────────────────────────────────────────────────

class DeliveryEngine {
public:
    /// Default constructor: zero-initializes m_initialized so that sub-components
    /// (which assert !m_initialized in their own init()) get a clean false value
    /// even when DeliveryEngine is stack-allocated without value-initialization.
    /// Power of 10 Rule 3: no allocation; just sets a sentinel boolean to false.
    DeliveryEngine() : m_transport(nullptr), m_local_id(0U), m_initialized(false) {}

    /**
     * @brief Initialize the delivery engine.
     *
     * Sets up the transport, ACK tracker, retry manager, and duplicate filter
     * for a single logical channel. Must be called once during system init.
     *
     * @param[in] transport   Pointer to initialized TransportInterface instance.
     * @param[in] cfg         Channel configuration (priority, reliability, timeouts).
     * @param[in] local_id    Local node ID for identifying ourselves as the sender.
     */
    void init(TransportInterface* transport, const ChannelConfig& cfg, NodeId local_id);

    // Safety-critical (SC): HAZ-001, HAZ-002, HAZ-006 — verified to M5
    /**
     * @brief Send a message through the delivery engine.
     *
     * Assigns a message_id, sets source_id, enqueues to transport.
     * Depending on reliability_class:
     *   - BEST_EFFORT: send once, do not track.
     *   - RELIABLE_ACK: track for ACK, do not auto-retry.
     *   - RELIABLE_RETRY: track and schedule for retry.
     *
     * @param[in,out] env      Message to send (modified with source_id, message_id).
     * @param[in]     now_us   Current time in microseconds.
     * @return OK on success; ERR_INVALID if not initialized; other codes from transport.
     */
    Result send(MessageEnvelope& env, uint64_t now_us);

    // Safety-critical (SC): HAZ-001, HAZ-003, HAZ-004, HAZ-005 — verified to M5
    /**
     * @brief Receive a message through the delivery engine.
     *
     * Calls transport->receive_message(). Applies impairments and dedup logic.
     * If DATA and RELIABLE_RETRY, runs duplicate_filter.check_and_record().
     * If ACK, calls ack_tracker.on_ack() and retry_manager.on_ack().
     * Drops expired messages.
     *
     * @param[out]    env         Received message (filled on success).
     * @param[in]     timeout_ms  Receive timeout in milliseconds.
     * @param[in]     now_us      Current time in microseconds.
     * @return OK on success; ERR_TIMEOUT if no message arrived;
     *         ERR_EXPIRED if message has expired; other codes from transport.
     */
    Result receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us);

    // Safety-critical (SC): HAZ-002 — verified to M5
    /**
     * @brief Process outstanding retries.
     *
     * Calls retry_manager.collect_due() to gather messages ready for retry,
     * then re-sends each via transport. Logs each retry.
     *
     * @param[in] now_us  Current time in microseconds.
     * @return Number of messages retried.
     */
    uint32_t pump_retries(uint64_t now_us);

    /**
     * @brief Populate @p out with aggregated delivery statistics.
     *
     * Combines local message counters and latency tracking with sub-component
     * stats from AckTracker, RetryManager, and the transport's get_transport_stats().
     * Also reads the global fatal assert count (REQ-7.2.4).
     *
     * NSC: read-only observability; no effect on delivery.
     * Power of 10 rule 5: ≥2 assertions enforced inside.
     *
     * @param[out] out DeliveryStats struct to fill.
     */
    void get_stats(DeliveryStats& out) const;

    // Safety-critical (SC): HAZ-002, HAZ-006 — verified to M5
    /**
     * @brief Check for ACK timeouts and process them.
     *
     * Calls ack_tracker.sweep_expired() to identify messages whose ACK deadline
     * has passed. Logs each timeout as WARNING_HI.
     *
     * @param[in] now_us  Current time in microseconds.
     * @return Number of timed-out ACKs detected.
     */
    uint32_t sweep_ack_timeouts(uint64_t now_us);

    // REQ-7.2.5: pull-style observability event polling API
    /**
     * @brief Pop the oldest pending observability event.
     *
     * Returns OK and fills @p out when an event is available.
     * Returns ERR_EMPTY when no events are pending.
     * Non-blocking; never modifies delivery state.
     * NSC: observability only.
     *
     * @param[out] out  Filled with the oldest event on OK.
     * @return OK on success; ERR_EMPTY if no events pending.
     */
    Result poll_event(DeliveryEvent& out);

    /**
     * @brief Return the number of unread observability events in the ring.
     *
     * NSC: read-only observability; no effect on delivery.
     *
     * @return Number of pending events (0 … DELIVERY_EVENT_RING_CAPACITY).
     */
    uint32_t pending_event_count() const;

    // REQ-7.2.5: bulk drain of the observability event ring into a caller-supplied buffer.
    /**
     * @brief Drain up to buf_cap events from the ring into out_buf.
     *
     * Events are removed in FIFO order (oldest first). Stops when the ring is
     * empty or buf_cap events have been written. Non-blocking; never modifies
     * delivery state. Equivalent to calling poll_event() in a bounded loop.
     * NSC: observability only.
     *
     * @param[out] out_buf  Caller-supplied array of at least buf_cap elements.
     * @param[in]  buf_cap  Maximum number of events to drain (must be <= buf_cap
     *                      elements allocated in out_buf).
     * @return Number of events written to out_buf (0 ... min(pending, buf_cap)).
     */
    uint32_t drain_events(DeliveryEvent* out_buf, uint32_t buf_cap);

private:
    TransportInterface* m_transport;
    ChannelConfig       m_cfg = {};
    NodeId              m_local_id;
    AckTracker          m_ack_tracker;
    RetryManager        m_retry_manager;
    DuplicateFilter     m_dedup;
    MessageIdGen        m_id_gen;
    bool                m_initialized;

    // Power of 10 Rule 3: pre-allocated output buffers for pump_retries() and
    // sweep_ack_timeouts(). Zero-initialized in init(). Single-threaded access
    // only; each buffer is owned exclusively by its corresponding function.
    MessageEnvelope m_retry_buf[MSG_RING_CAPACITY] = {};
    MessageEnvelope m_timeout_buf[ACK_TRACKER_CAPACITY] = {};

    // REQ-7.2.1–7.2.4: aggregated delivery statistics (zero-init in init())
    DeliveryStats   m_stats = {};

    // REQ-7.2.5: bounded pull-style observability event ring (init'd in init())
    // Power of 10 Rule 3: fixed-capacity ring; no dynamic allocation.
    DeliveryEventRing m_events;

    // REQ-3.2.3, REQ-3.3.3: bounded fragment reassembly buffer (init'd in init())
    // Power of 10 Rule 3: fixed REASSEMBLY_SLOT_COUNT slots; no dynamic allocation.
    ReassemblyBuffer m_reassembly;

    // REQ-3.3.5: per-peer in-order delivery gate (init'd in init())
    // Power of 10 Rule 3: fixed ORDERING_HOLD_COUNT slots; no dynamic allocation.
    OrderingBuffer m_ordering;

    // REQ-3.3.5: per-destination outbound sequence counter for ORDERED DATA messages.
    // Power of 10 Rule 3: bounded by ACK_TRACKER_CAPACITY (max peers).
    struct SeqState {
        NodeId   dst;
        uint32_t next_seq;
        bool     active;
    };
    SeqState m_seq_state[ACK_TRACKER_CAPACITY] = {};

    // Power of 10 Rule 3: fragment staging buffer for send/retry paths.
    // Holds up to FRAG_MAX_COUNT wire fragments for one logical message at a time.
    MessageEnvelope m_frag_buf[FRAG_MAX_COUNT] = {};

    // Issue 3 fix: pre-allocated output buffer for sweep_expired_holds() calls.
    // Sized to ORDERING_HOLD_COUNT (max holds that can expire in one sweep).
    // Power of 10 Rule 3: fixed-capacity; no heap after init.
    MessageEnvelope m_ordering_freed_buf[ORDERING_HOLD_COUNT] = {};

    // REQ-3.3.5: single-slot staging area for held ordering messages (Issue 1 fix).
    // When handle_data_path() delivers an in-order message, try_release_next() checks
    // whether the next sequential message is already held; if so it is staged here and
    // returned on the subsequent receive() call rather than being stuck forever.
    // Power of 10 Rule 3: single pre-allocated envelope; no heap after init.
    MessageEnvelope m_held_pending = {};
    bool            m_held_pending_valid = false;

    // Issue 3 fix: update msgs_dropped_expired stats and emit EXPIRY_DROP events
    // for holds freed by sweep_expired_holds().  Extracted to keep callers CC ≤ 10.
    // NSC: observability bookkeeping only.
    void account_ordering_expiry_drops(const MessageEnvelope* freed,
                                        uint32_t               count,
                                        uint64_t               now_us);

    // Private helper: push one DeliveryEvent into m_events (REQ-7.2.5).
    // Bounded overwrite-on-full ring push; never blocks, never fails.
    // NSC: observability bookkeeping only.
    void emit_event(DeliveryEventKind kind,
                    uint64_t          message_id,
                    NodeId            node_id,
                    uint64_t          timestamp_us,
                    Result            result);

    // Private helper: emit EXPIRY_DROP or MISROUTE_DROP event based on routing result.
    // Extracted from receive() to reduce its cognitive complexity to ≤ 10 (REQ-7.2.5).
    // NSC: observability bookkeeping only.
    void emit_routing_drop_event(const MessageEnvelope& env,
                                 Result                 routing_res,
                                 uint64_t               now_us);

    // Private helper: expiry-gate then send a message envelope via transport.
    // Returns ERR_EXPIRED without touching the transport if the message has expired.
    Result send_via_transport(const MessageEnvelope& env, uint64_t now_us);

    // Private helper: record a send-failure stat and log a warning.
    // Increments msgs_dropped_expired on ERR_EXPIRED; logs at WARNING_LO.
    // Extracted from send() to reduce its cognitive complexity (CC ≤ 10).
    // NSC: observability bookkeeping only.
    void record_send_failure(const MessageEnvelope& env, Result res);

    // Private helper: update routing-failure stats (msgs_dropped_expired or
    // msgs_dropped_misrouted) based on the routing result code.
    // Extracted from receive() to reduce its cognitive complexity (CC ≤ 10).
    // NSC: observability bookkeeping only.
    void record_routing_failure(Result routing_res);

    // Private helper: update latency stats from a received ACK.
    // Looks up the original send timestamp and computes RTT (REQ-7.2.1).
    // NSC: observability bookkeeping only.
    void update_latency_stats(const MessageEnvelope& ack_env, uint64_t now_us);

    // Private helper: get or assign the next outbound sequence number for dst.
    // Returns the sequence number; 0 on failure (no peer slot available).
    // REQ-3.3.5: per-destination outbound sequence counter.
    uint32_t next_seq_for(NodeId dst);

    // Private helper: send a logical envelope, fragmenting if necessary.
    // Returns OK when all wire frames were sent; ERR_IO if any frame failed
    // (partial send is treated as failure — bookkeeping not rolled back here,
    // that is the caller's responsibility).
    // REQ-3.2.3, REQ-3.3.5.
    Result send_fragments(const MessageEnvelope& env, uint64_t now_us);

    // Private helper: handle fragment ingest during receive().
    // Returns OK when the full logical message is ready in logical_out.
    // Returns ERR_AGAIN when more fragments are needed (caller should return).
    // Returns ERR_INVALID/ERR_FULL on error (caller should return the error).
    // REQ-3.2.3.
    Result handle_fragment_ingest(const MessageEnvelope& wire_env,
                                   MessageEnvelope&       logical_out);

    // Private helper: run a logical DATA message through the ordering gate.
    // Returns OK when the message is ready in out.
    // Returns ERR_AGAIN if held (caller should return ERR_AGAIN).
    // Returns ERR_FULL if the hold buffer is full.
    // REQ-3.3.5.
    Result handle_ordering_gate(const MessageEnvelope& logical,
                                 MessageEnvelope&       out,
                                 uint64_t               now_us);

    // Private helper: apply duplicate suppression to a RELIABLE_RETRY DATA message.
    // Returns ERR_DUPLICATE if suppressed (caller should return ERR_DUPLICATE).
    // Returns OK if the message passes (not a duplicate, or not RELIABLE_RETRY).
    // Extracted from receive() to reduce its cognitive complexity to ≤ 10.
    // Safety-critical (SC): HAZ-003 — prevents spurious duplicate delivery.
    Result handle_data_dedup(const MessageEnvelope& env, uint64_t now_us);

    // Private helper: run the full data-message delivery path after routing.
    // Applies dedup, ordering gate, ACK generation, stats increment.
    // Returns OK on successful delivery; ERR_DUPLICATE if suppressed;
    // ERR_AGAIN if held by the ordering gate.
    // Extracted from receive() to reduce its cognitive complexity to ≤ 10.
    // Safety-critical (SC): HAZ-001, HAZ-003 — on the reliable delivery path.
    Result handle_data_path(MessageEnvelope& env, uint64_t now_us);

    // Private helper: deliver the staged m_held_pending envelope (REQ-3.3.5 Issue 1 fix).
    // Called at the top of receive() when m_held_pending_valid is true.
    // Copies the held message to env, checks expiry, attempts to stage the next held
    // message via try_release_next(), sends ACK if required, and increments stats.
    // Returns OK on successful delivery; ERR_EXPIRED if the held message has expired
    // (receive() must loop so the caller gets a live message or ERR_TIMEOUT).
    // SC: HAZ-001, HAZ-004 — expiry gate on the ordering backlog drain path.
    Result deliver_held_pending(MessageEnvelope& env, uint64_t now_us);

    // Private helper: process an inbound control message (ACK/NAK/HEARTBEAT).
    // Always returns OK (control messages are never an error condition).
    // Extracted from receive() to reduce its cognitive complexity to ≤ 10.
    // NSC: bookkeeping and ACK processing only.
    void handle_control_message(const MessageEnvelope& env, uint64_t now_us);
};

#endif // CORE_DELIVERY_ENGINE_HPP
