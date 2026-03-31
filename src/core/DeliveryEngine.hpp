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
 * Implements: REQ-3.2.7, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-3.3.4
 */

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

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEngine
// ─────────────────────────────────────────────────────────────────────────────

class DeliveryEngine {
public:
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

private:
    TransportInterface* m_transport;
    ChannelConfig       m_cfg;
    NodeId              m_local_id;
    AckTracker          m_ack_tracker;
    RetryManager        m_retry_manager;
    DuplicateFilter     m_dedup;
    MessageIdGen        m_id_gen;
    bool                m_initialized;

    // Private helper: send a message envelope via transport (checked return).
    Result send_via_transport(const MessageEnvelope& env);
};

#endif // CORE_DELIVERY_ENGINE_HPP
