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
 * @file RequestReplyEngine.hpp
 * @brief Bounded request/response helper layer on top of DeliveryEngine.
 *
 * Adds request/response correlation on top of the existing DeliveryEngine
 * without modifying MessageEnvelope, Serializer, or ProtocolVersion.
 * Correlation metadata travels inside the payload as a 12-byte RRHeader prefix
 * (see RequestReplyHeader.hpp); the application only sees the bytes after it.
 *
 * Design constraints:
 *   - No dynamic allocation anywhere (Power of 10 Rule 3).
 *   - No STL, no templates, no exceptions (F-Prime subset).
 *   - MAX_PENDING_REQUESTS = 16, MAX_STASH_SIZE = 16 (bounded tables).
 *   - send_request uses RELIABLE_RETRY; send_response uses BEST_EFFORT.
 *   - pump_inbound drains up to MAX_STASH_SIZE envelopes per call.
 *   - sweep_timeouts frees expired pending-request slots.
 *
 * Rules applied:
 *   - Power of 10 Rules 1–10 throughout.
 *   - MISRA C++:2023 advisory compliance; required rules in force.
 *   - NEVER_COMPILED_OUT_ASSERT for all pre/post-conditions.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
// Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

#ifndef CORE_REQUEST_REPLY_ENGINE_HPP
#define CORE_REQUEST_REPLY_ENGINE_HPP

#include <cstdint>
#include "Types.hpp"
#include "DeliveryEngine.hpp"
#include "RequestReplyHeader.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine
// ─────────────────────────────────────────────────────────────────────────────

class RequestReplyEngine {
public:
    /// Maximum number of simultaneously outstanding requests.
    static const uint32_t MAX_PENDING_REQUESTS = 16U;
    /// Maximum number of inbound requests buffered before poll.
    static const uint32_t MAX_STASH_SIZE       = 16U;

    // Safety-critical (SC): HAZ-001, HAZ-002
    /**
     * @brief Initialize with an already-init'd DeliveryEngine and local node ID.
     *
     * Must be called once before any other method.
     *
     * @param[in] engine      Reference to an initialized DeliveryEngine.
     * @param[in] local_node  Local node ID (must not be NODE_ID_INVALID).
     */
    void init(DeliveryEngine& engine, NodeId local_node);

    // Safety-critical (SC): HAZ-001, HAZ-002
    /**
     * @brief Send a request to @p destination and record a pending slot.
     *
     * Prefixes @p app_payload with an RRHeader (kind=REQUEST, correlation_id
     * derived from the engine-assigned message_id) and sends via
     * RELIABLE_RETRY.  The caller receives the correlation_id to pass to
     * receive_response() later.
     *
     * @param[in]  destination        Target node.
     * @param[in]  app_payload        Application payload bytes.
     * @param[in]  app_payload_len    Length of @p app_payload.
     * @param[in]  timeout_us         Microseconds until this request expires.
     * @param[in]  now_us             Current time in microseconds.
     * @param[out] correlation_id_out Set to the correlation ID on OK.
     * @return OK on success; ERR_FULL if pending table is full or payload too large;
     *         ERR_INVALID if not initialized.
     */
    Result send_request(NodeId           destination,
                        const uint8_t*   app_payload,
                        uint32_t         app_payload_len,
                        uint64_t         timeout_us,
                        uint64_t         now_us,
                        uint64_t&        correlation_id_out);

    // Safety-critical (SC): HAZ-001, HAZ-003
    /**
     * @brief Poll for an inbound request addressed to this node.
     *
     * Calls pump_inbound() to drain the underlying engine, then returns the
     * oldest buffered request (if any).  Strips the RRHeader prefix so the
     * application sees only its own payload bytes.
     *
     * @param[out] app_payload_buf     Buffer to receive application payload.
     * @param[in]  buf_cap             Capacity of @p app_payload_buf.
     * @param[out] app_payload_len_out Length of payload written.
     * @param[out] src_out             Source node of the request.
     * @param[out] correlation_id_out  Correlation ID to use in send_response().
     * @param[in]  now_us              Current time in microseconds.
     * @return OK if a request was available; ERR_EMPTY if none; ERR_INVALID if
     *         not initialized.
     */
    Result receive_request(uint8_t*   app_payload_buf,
                           uint32_t   buf_cap,
                           uint32_t&  app_payload_len_out,
                           NodeId&    src_out,
                           uint64_t&  correlation_id_out,
                           uint64_t   now_us);

    // Safety-critical (SC): HAZ-001, HAZ-002
    /**
     * @brief Send a response to a previously received request.
     *
     * Uses BEST_EFFORT delivery (responses are fire-and-forget; the requester
     * times out if lost).
     *
     * @param[in] destination      Target node (original requester).
     * @param[in] correlation_id   Correlation ID from receive_request().
     * @param[in] app_payload      Application response payload.
     * @param[in] app_payload_len  Length of @p app_payload.
     * @param[in] now_us           Current time in microseconds.
     * @return OK on success; ERR_FULL if payload too large; ERR_INVALID if not
     *         initialized.
     */
    Result send_response(NodeId         destination,
                         uint64_t       correlation_id,
                         const uint8_t* app_payload,
                         uint32_t       app_payload_len,
                         uint64_t       now_us);

    // Safety-critical (SC): HAZ-001, HAZ-002
    /**
     * @brief Poll for a response matching a pending request.
     *
     * Calls pump_inbound() to drain the underlying engine, then checks whether
     * a response for @p correlation_id has been stashed.  On OK, fills
     * @p app_payload_buf and frees the pending slot.
     *
     * @param[in]  correlation_id     Correlation ID returned by send_request().
     * @param[out] app_payload_buf    Buffer to receive response payload.
     * @param[in]  buf_cap            Capacity of @p app_payload_buf.
     * @param[out] app_payload_len_out Length of payload written.
     * @param[in]  now_us             Current time in microseconds.
     * @return OK if a matching response arrived; ERR_EMPTY if not yet;
     *         ERR_INVALID if not initialized or unknown correlation_id.
     */
    Result receive_response(uint64_t   correlation_id,
                            uint8_t*   app_payload_buf,
                            uint32_t   buf_cap,
                            uint32_t&  app_payload_len_out,
                            uint64_t   now_us);

    // Safety-critical (SC): HAZ-001, HAZ-002
    /**
     * @brief Sweep timed-out pending request slots and free them.
     *
     * Call periodically (e.g., on each event-loop tick) so stale slots do not
     * prevent new requests when MAX_PENDING_REQUESTS is reached.
     *
     * @param[in] now_us  Current time in microseconds.
     * @return Number of slots freed.
     */
    uint32_t sweep_timeouts(uint64_t now_us);

private:
    // ─────────────────────────────────────────────────────────────────────────
    // Pending request table
    // ─────────────────────────────────────────────────────────────────────────
    /// Maximum app payload size = MSG_MAX_PAYLOAD_BYTES minus header overhead.
    static const uint32_t APP_PAYLOAD_CAP =
        MSG_MAX_PAYLOAD_BYTES - RR_HEADER_SIZE;

    struct PendingRequest {
        uint64_t correlation_id;             ///< == message_id of the sent request
        uint64_t expires_us;                 ///< absolute expiry timestamp
        bool     active;                     ///< slot is in use
        uint8_t  stash_payload[APP_PAYLOAD_CAP]; ///< buffered response payload
        uint32_t stash_len;                  ///< bytes valid in stash_payload
        bool     stash_ready;                ///< response arrived before poll
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Inbound request stash (requests received but not yet polled)
    // ─────────────────────────────────────────────────────────────────────────
    struct StashedRequest {
        uint64_t correlation_id;
        NodeId   src;
        uint8_t  payload[APP_PAYLOAD_CAP];
        uint32_t payload_len;
        bool     active;
    };

    // ─────────────────────────────────────────────────────────────────────────
    // Member state (Power of 10 Rule 3: all static; no heap after init)
    // ─────────────────────────────────────────────────────────────────────────
    DeliveryEngine* m_engine        = nullptr;
    NodeId          m_local_node    = 0U;
    bool            m_initialized   = false;

    /// Monotonic local counter for correlation IDs (starts at 1; wraps at UINT64_MAX).
    /// Incremented on each send_request(); independent of engine message_id.
    uint64_t        m_cid_gen       = 1U;

    PendingRequest  m_pending[MAX_PENDING_REQUESTS];
    uint32_t        m_pending_count = 0U;

    StashedRequest  m_request_stash[MAX_STASH_SIZE];
    uint32_t        m_stash_head    = 0U;  ///< FIFO read index for m_request_stash
    uint32_t        m_stash_count   = 0U;

    // REQ-3.2.4 (Issue 4 fix): stash for DATA envelopes that do not carry an RRHeader.
    // pump_inbound() consumes all frames from the shared DeliveryEngine; non-RR DATA
    // messages were silently dropped. They are now preserved here and returned via
    // receive_non_rr(). Power of 10 Rule 3: bounded FIFO; no dynamic allocation.
    MessageEnvelope m_non_rr_stash[MAX_STASH_SIZE];
    uint32_t        m_non_rr_head  = 0U;
    uint32_t        m_non_rr_count = 0U;

    // ─────────────────────────────────────────────────────────────────────────
    // Private helpers
    // ─────────────────────────────────────────────────────────────────────────

    /// Drain up to MAX_STASH_SIZE envelopes from the engine; classify each as
    /// REQUEST or RESPONSE and stash accordingly.
    /// Power of 10 Rule 2: bounded loop (≤ MAX_STASH_SIZE iterations).
    void pump_inbound(uint64_t now_us);

    /// Find an active pending slot by correlation_id.
    /// Returns index [0, MAX_PENDING_REQUESTS) or MAX_PENDING_REQUESTS if not found.
    uint32_t find_pending(uint64_t correlation_id) const;

    /// Find a free pending slot.
    /// Returns index [0, MAX_PENDING_REQUESTS) or MAX_PENDING_REQUESTS if full.
    uint32_t find_free_pending() const;

    /// Store an inbound RESPONSE envelope into the matching pending slot or drop
    /// it if the correlation_id is unknown.
    void handle_inbound_response(uint64_t       correlation_id,
                                 const uint8_t* app_payload,
                                 uint32_t       app_len);

    /// Store an inbound REQUEST into the stash, or drop if stash is full.
    void handle_inbound_request(NodeId         src,
                                uint64_t       correlation_id,
                                const uint8_t* app_payload,
                                uint32_t       app_len);

    /// Build a full wire payload (RRHeader + app_payload) in @p out_buf.
    /// Returns the total length written, or 0 on overflow.
    static uint32_t build_wire_payload(RRKind         kind,
                                       uint64_t       correlation_id,
                                       const uint8_t* app_payload,
                                       uint32_t       app_len,
                                       uint8_t*       out_buf,
                                       uint32_t       out_cap);

    /// Parse an RRHeader from the front of @p raw_payload.
    /// Returns false if @p raw_len < RR_HEADER_SIZE or kind is invalid.
    static bool parse_rr_header(const uint8_t* raw_payload,
                                uint32_t       raw_len,
                                RRHeader&      hdr_out);

    /// Dispatch one inbound DATA envelope: parse the RRHeader and route to
    /// handle_inbound_response() or handle_inbound_request() as appropriate.
    /// Extracted from pump_inbound() to keep pump_inbound() CC <= 10.
    /// Returns false if the envelope is not an RR message (caller may skip).
    bool dispatch_inbound_envelope(const MessageEnvelope& env);

    /// Save a non-RR DATA envelope to m_non_rr_stash for later retrieval via
    /// receive_non_rr(). Drops with WARNING_LO when the stash is full.
    /// NSC: bookkeeping only; no reliability or delivery impact.
    void stash_non_rr_data(const MessageEnvelope& env);

public:
    // ─────────────────────────────────────────────────────────────────────────
    // Non-RR DATA passthrough (Issue 4 fix)
    // ─────────────────────────────────────────────────────────────────────────
    /**
     * @brief Pop the oldest non-RR DATA message from the passthrough stash.
     *
     * When pump_inbound() consumes a DATA frame that does not carry a valid
     * RRHeader, it saves the full envelope here rather than dropping it.
     * The application calls receive_non_rr() to retrieve those frames.
     *
     * @param[out] env     Filled with the oldest non-RR envelope on OK.
     * @param[in]  now_us  Current time in microseconds (passed to pump_inbound()).
     * @return OK if a frame was available; ERR_EMPTY otherwise;
     *         ERR_INVALID if not initialized.
     * NSC: passthrough bookkeeping only.
     */
    Result receive_non_rr(MessageEnvelope& env, uint64_t now_us);
};

#endif // CORE_REQUEST_REPLY_ENGINE_HPP
