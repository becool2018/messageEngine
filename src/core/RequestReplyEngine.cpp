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
 * @file RequestReplyEngine.cpp
 * @brief Implementation of bounded request/response helper layer.
 *
 * Correlation IDs are assigned by a local monotonic counter (m_cid_gen)
 * that is incremented on each send_request() call and embedded in the
 * RRHeader of the outgoing payload.  The responder echoes this ID back in
 * the RRHeader of the RESPONSE message; send_request() therefore does NOT
 * depend on knowing the DeliveryEngine-assigned message_id.
 *
 * All functions comply with:
 *   - Power of 10: no recursion, fixed loop bounds, ≥2 assertions per
 *     function, checked return values, CC ≤ 10, no dynamic allocation.
 *   - F-Prime style: no STL, no templates, no exceptions.
 *   - MISRA C++:2023 required rules.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
// Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

#include "RequestReplyEngine.hpp"
#include "Assert.hpp"
#include "Logger.hpp"
#include "MessageEnvelope.hpp"
#include "Timestamp.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::init()
// Safety-critical (SC): HAZ-001, HAZ-002
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void RequestReplyEngine::init(DeliveryEngine& engine, NodeId local_node)
{
    // pre: local_node is valid; engine reference is always non-null (C++ guarantee)
    NEVER_COMPILED_OUT_ASSERT(local_node != NODE_ID_INVALID); // pre: valid node ID
    NEVER_COMPILED_OUT_ASSERT(local_node <= 0xFFFFFFFFU);     // pre: fits in NodeId

    m_engine        = &engine;
    m_local_node    = local_node;
    m_initialized   = false;
    m_pending_count = 0U;
    m_stash_count   = 0U;
    m_cid_gen       = 1U;   // start at 1; 0 is the sentinel "no ID"

    // Power of 10 Rule 3: zero-initialize all static tables during init phase.
    // Bounded loops — compile-time constants.
    for (uint32_t i = 0U; i < MAX_PENDING_REQUESTS; ++i) {
        m_pending[i].correlation_id = 0U;
        m_pending[i].expires_us     = 0U;
        m_pending[i].active         = false;
        m_pending[i].stash_len      = 0U;
        m_pending[i].stash_ready    = false;
        (void)memset(m_pending[i].stash_payload, 0,
                     static_cast<size_t>(APP_PAYLOAD_CAP));
    }

    for (uint32_t j = 0U; j < MAX_STASH_SIZE; ++j) {
        m_request_stash[j].correlation_id = 0U;
        m_request_stash[j].src            = NODE_ID_INVALID;
        m_request_stash[j].payload_len    = 0U;
        m_request_stash[j].active         = false;
        (void)memset(m_request_stash[j].payload, 0,
                     static_cast<size_t>(APP_PAYLOAD_CAP));
    }

    // REQ-3.2.4: zero the non-RR passthrough stash (Issue 4 fix)
    // Power of 10 Rule 2: bounded loop — compile-time constant MAX_STASH_SIZE
    m_non_rr_head  = 0U;
    m_non_rr_count = 0U;
    for (uint32_t k = 0U; k < MAX_STASH_SIZE; ++k) {
        envelope_init(m_non_rr_stash[k]);
    }

    m_initialized = true;

    NEVER_COMPILED_OUT_ASSERT(m_initialized);       // post: engine marked ready
    NEVER_COMPILED_OUT_ASSERT(m_engine != nullptr); // post: engine pointer set

    Logger::log(Severity::INFO, "RequestReplyEngine",
                "Initialized, local_node=%u", local_node);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::build_wire_payload() — static helper
// Copies RRHeader + app_payload into out_buf.
// Returns total bytes written, or 0 on overflow.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RequestReplyEngine::build_wire_payload(RRKind         kind,
                                                uint64_t       correlation_id,
                                                const uint8_t* app_payload,
                                                uint32_t       app_len,
                                                uint8_t*       out_buf,
                                                uint32_t       out_cap)
{
    NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr);       // pre: buffer provided
    NEVER_COMPILED_OUT_ASSERT(out_cap >= RR_HEADER_SIZE); // pre: room for header

    const uint32_t total = RR_HEADER_SIZE + app_len;
    if (total > out_cap) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "build_wire_payload: payload too large (%u > %u)",
                    total, out_cap);
        return 0U;
    }

    // Issue 5 fix: write RRHeader bytes manually in big-endian order so the wire
    // format is platform-independent (matches the main Serializer convention).
    // Layout: byte 0 = kind; bytes 1-8 = correlation_id BE; bytes 9-11 = zero pad.
    // Avoids memcpy of RRHeader struct (which stored correlation_id in native order).
    out_buf[0U] = static_cast<uint8_t>(kind);
    // correlation_id: 8 bytes, big-endian (MSB first)
    out_buf[1U] = static_cast<uint8_t>((correlation_id >> 56U) & 0xFFU);
    out_buf[2U] = static_cast<uint8_t>((correlation_id >> 48U) & 0xFFU);
    out_buf[3U] = static_cast<uint8_t>((correlation_id >> 40U) & 0xFFU);
    out_buf[4U] = static_cast<uint8_t>((correlation_id >> 32U) & 0xFFU);
    out_buf[5U] = static_cast<uint8_t>((correlation_id >> 24U) & 0xFFU);
    out_buf[6U] = static_cast<uint8_t>((correlation_id >> 16U) & 0xFFU);
    out_buf[7U] = static_cast<uint8_t>((correlation_id >> 8U)  & 0xFFU);
    out_buf[8U] = static_cast<uint8_t>((correlation_id >> 0U)  & 0xFFU);
    // pad bytes
    out_buf[9U]  = 0U;
    out_buf[10U] = 0U;
    out_buf[11U] = 0U;

    if (app_len > 0U) {
        NEVER_COMPILED_OUT_ASSERT(app_payload != nullptr); // pre: ptr valid when len>0
        (void)memcpy(out_buf + RR_HEADER_SIZE, app_payload,
                     static_cast<size_t>(app_len));
    }

    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::parse_rr_header() — static helper
// Reads the first RR_HEADER_SIZE bytes of raw_payload into hdr_out.
// Returns false on under-size or invalid kind.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
bool RequestReplyEngine::parse_rr_header(const uint8_t* raw_payload,
                                         uint32_t       raw_len,
                                         RRHeader&      hdr_out)
{
    NEVER_COMPILED_OUT_ASSERT(raw_payload != nullptr); // pre: pointer valid
    NEVER_COMPILED_OUT_ASSERT(raw_len > 0U);           // pre: non-empty buffer

    if (raw_len < RR_HEADER_SIZE) {
        return false;
    }

    // Issue 5 fix: read correlation_id in big-endian order (matches build_wire_payload).
    // Do NOT use memcpy of the full RRHeader struct — that writes correlation_id in
    // native byte order, breaking cross-endian interoperability.
    hdr_out.kind = raw_payload[0U];

    // correlation_id: 8 bytes, big-endian (MSB first)
    hdr_out.correlation_id =
        (static_cast<uint64_t>(raw_payload[1U]) << 56U) |
        (static_cast<uint64_t>(raw_payload[2U]) << 48U) |
        (static_cast<uint64_t>(raw_payload[3U]) << 40U) |
        (static_cast<uint64_t>(raw_payload[4U]) << 32U) |
        (static_cast<uint64_t>(raw_payload[5U]) << 24U) |
        (static_cast<uint64_t>(raw_payload[6U]) << 16U) |
        (static_cast<uint64_t>(raw_payload[7U]) << 8U)  |
        (static_cast<uint64_t>(raw_payload[8U]) << 0U);

    hdr_out._pad[0] = raw_payload[9U];
    hdr_out._pad[1] = raw_payload[10U];
    hdr_out._pad[2] = raw_payload[11U];

    // Accept only known kind values.
    bool kind_ok = (hdr_out.kind == static_cast<uint8_t>(RRKind::REQUEST)) ||
                   (hdr_out.kind == static_cast<uint8_t>(RRKind::RESPONSE));
    if (!kind_ok) {
        return false;
    }

    // Issue 2 fix: reject frames with a zero correlation_id; handle_inbound_response()
    // and handle_inbound_request() both NEVER_COMPILED_OUT_ASSERT(correlation_id != 0U).
    // A peer can synthesise a valid-kind / zero-cid frame and trigger an always-on abort.
    if (hdr_out.correlation_id == 0U) {
        return false;
    }

    NEVER_COMPILED_OUT_ASSERT(hdr_out.correlation_id != 0U);  // post: safe to pass on
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::find_pending() — private helper
// Linear scan for a matching active pending slot.
// Returns slot index or MAX_PENDING_REQUESTS if not found.
// Power of 10: ≥2 assertions, CC ≤ 10, bounded loop.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RequestReplyEngine::find_pending(uint64_t correlation_id) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);        // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(correlation_id != 0U); // pre: valid correlation ID

    // Power of 10 Rule 2: bounded by MAX_PENDING_REQUESTS (compile-time constant).
    for (uint32_t i = 0U; i < MAX_PENDING_REQUESTS; ++i) {
        if (m_pending[i].active &&
            (m_pending[i].correlation_id == correlation_id)) {
            return i;
        }
    }
    return MAX_PENDING_REQUESTS; // sentinel: not found
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::find_free_pending() — private helper
// Power of 10: ≥2 assertions, CC ≤ 10, bounded loop.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RequestReplyEngine::find_free_pending() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);             // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(MAX_PENDING_REQUESTS > 0U); // pre: capacity valid

    // Power of 10 Rule 2: bounded by MAX_PENDING_REQUESTS.
    for (uint32_t i = 0U; i < MAX_PENDING_REQUESTS; ++i) {
        if (!m_pending[i].active) {
            return i;
        }
    }
    return MAX_PENDING_REQUESTS; // sentinel: full
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::find_free_stash() — private helper
// Power of 10: ≥2 assertions, CC ≤ 10, bounded loop.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RequestReplyEngine::find_free_stash() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);        // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(MAX_STASH_SIZE > 0U);  // pre: capacity valid

    // Power of 10 Rule 2: bounded by MAX_STASH_SIZE.
    for (uint32_t i = 0U; i < MAX_STASH_SIZE; ++i) {
        if (!m_request_stash[i].active) {
            return i;
        }
    }
    return MAX_STASH_SIZE; // sentinel: full
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::handle_inbound_response() — private helper
// Matches correlation_id to pending table; stashes payload if found.
// Silently drops if unknown (may be an unmatched or duplicate response).
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void RequestReplyEngine::handle_inbound_response(uint64_t       correlation_id,
                                                 const uint8_t* app_payload,
                                                 uint32_t       app_len)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);        // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(correlation_id != 0U); // pre: non-zero ID

    uint32_t idx = find_pending(correlation_id);
    if (idx >= MAX_PENDING_REQUESTS) {
        // Unknown correlation_id — silently drop (test 5 requirement).
        Logger::log(Severity::INFO, "RequestReplyEngine",
                    "Dropped response for unknown correlation_id=%llu",
                    (unsigned long long)correlation_id);
        return;
    }

    if (m_pending[idx].stash_ready) {
        // Duplicate response — discard the later copy.
        return;
    }

    uint32_t copy_len = app_len;
    if (copy_len > APP_PAYLOAD_CAP) {
        copy_len = APP_PAYLOAD_CAP;
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "Response payload truncated to %u bytes",
                    APP_PAYLOAD_CAP);
    }

    if (copy_len > 0U) {
        NEVER_COMPILED_OUT_ASSERT(app_payload != nullptr); // pre: ptr valid when len>0
        (void)memcpy(m_pending[idx].stash_payload, app_payload,
                     static_cast<size_t>(copy_len));
    }
    m_pending[idx].stash_len   = copy_len;
    m_pending[idx].stash_ready = true;

    Logger::log(Severity::INFO, "RequestReplyEngine",
                "Stashed response correlation_id=%llu len=%u",
                (unsigned long long)correlation_id, copy_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::handle_inbound_request() — private helper
// Stashes an inbound request; drops with WARNING_LO if stash is full.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void RequestReplyEngine::handle_inbound_request(NodeId         src,
                                                uint64_t       correlation_id,
                                                const uint8_t* app_payload,
                                                uint32_t       app_len)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);        // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(correlation_id != 0U); // pre: valid ID

    uint32_t slot = find_free_stash();
    if (slot >= MAX_STASH_SIZE) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "Request stash full; dropping correlation_id=%llu from src=%u",
                    (unsigned long long)correlation_id, src);
        return;
    }

    uint32_t copy_len = app_len;
    if (copy_len > APP_PAYLOAD_CAP) {
        copy_len = APP_PAYLOAD_CAP;
    }

    if (copy_len > 0U) {
        NEVER_COMPILED_OUT_ASSERT(app_payload != nullptr); // pre: ptr valid when len>0
        (void)memcpy(m_request_stash[slot].payload, app_payload,
                     static_cast<size_t>(copy_len));
    }
    m_request_stash[slot].payload_len    = copy_len;
    m_request_stash[slot].correlation_id = correlation_id;
    m_request_stash[slot].src            = src;
    m_request_stash[slot].active         = true;
    ++m_stash_count;

    Logger::log(Severity::INFO, "RequestReplyEngine",
                "Stashed request correlation_id=%llu from src=%u len=%u",
                (unsigned long long)correlation_id, src, copy_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::dispatch_inbound_envelope() — private CC-reduction helper
// Parses the RRHeader from a DATA envelope and routes it to
// handle_inbound_response() or handle_inbound_request().
// Extracted from pump_inbound() to keep pump_inbound() CC ≤ 10.
// Returns false if the envelope is not a valid RR message (caller skips it).
// Power of 10: single-purpose, ≤1 page, ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
bool RequestReplyEngine::dispatch_inbound_envelope(const MessageEnvelope& env)
{
    // Power of 10 Rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env));    // pre: caller guards control msgs
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // pre: engine ready

    RRHeader hdr;
    bool ok = parse_rr_header(env.payload, env.payload_length, hdr);
    if (!ok) {
        return false;  // Not an RR message or malformed; caller may skip.
    }

    const uint8_t* app_start = env.payload + RR_HEADER_SIZE;
    uint32_t       app_len   = (env.payload_length > RR_HEADER_SIZE)
                               ? (env.payload_length - RR_HEADER_SIZE)
                               : 0U;

    if (hdr.kind == static_cast<uint8_t>(RRKind::RESPONSE)) {
        handle_inbound_response(hdr.correlation_id, app_start, app_len);
    } else {
        handle_inbound_request(env.source_id, hdr.correlation_id,
                               app_start, app_len);
    }

    // Power of 10 Rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(ok);  // post: parse succeeded (always true here)
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::pump_inbound() — private helper
// Drains up to MAX_STASH_SIZE envelopes from the engine per call.
// Power of 10 Rule 2: bounded loop (≤ MAX_STASH_SIZE iterations).
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void RequestReplyEngine::pump_inbound(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);       // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(m_engine != nullptr); // pre: engine pointer valid

    // Power of 10 Rule 2: bounded by MAX_STASH_SIZE per-call drain limit.
    for (uint32_t iter = 0U; iter < MAX_STASH_SIZE; ++iter) {
        MessageEnvelope env;
        envelope_init(env);

        // Power of 10 Rule 7: check return value.
        // timeout_ms=0: non-blocking poll.
        Result res = m_engine->receive(env, 0U, now_us);
        if (res != Result::OK) {
            // ERR_TIMEOUT / ERR_EMPTY = no more messages; stop draining.
            break;
        }

        // Only DATA messages carry RR payload; skip control messages.
        if (!envelope_is_data(env)) {
            continue;
        }

        // Dispatch: parse RRHeader and route to response or request handler.
        // Extracted to dispatch_inbound_envelope() to keep CC ≤ 10.
        // Power of 10 Rule 7: return value checked; non-RR frames are stashed.
        bool dispatched = dispatch_inbound_envelope(env);
        if (!dispatched) {
            // Issue 4 fix: preserve non-RR DATA frames so the application can
            // retrieve them via receive_non_rr(). Prior behaviour silently dropped them.
            stash_non_rr_data(env);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::stash_non_rr_data() — NSC private helper (Issue 4 fix)
// Saves a non-RR DATA envelope to the m_non_rr_stash FIFO for later retrieval
// via receive_non_rr(). Drops with WARNING_LO when the stash is full.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
void RequestReplyEngine::stash_non_rr_data(const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // Assert: engine ready
    NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env));    // Assert: DATA frames only

    if (m_non_rr_count >= MAX_STASH_SIZE) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "Non-RR stash full; dropping message_id=%llu from src=%u",
                    (unsigned long long)env.message_id, env.source_id);
        return;
    }

    uint32_t idx = (m_non_rr_head + m_non_rr_count) % MAX_STASH_SIZE;
    envelope_copy(m_non_rr_stash[idx], env);
    ++m_non_rr_count;

    NEVER_COMPILED_OUT_ASSERT(m_non_rr_count <= MAX_STASH_SIZE);  // Assert: bounded
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::receive_non_rr() — NSC public (Issue 4 fix)
// Pops the oldest non-RR DATA envelope from m_non_rr_stash.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result RequestReplyEngine::receive_non_rr(MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                       // Assert: engine ready
    NEVER_COMPILED_OUT_ASSERT(m_non_rr_count <= MAX_STASH_SIZE);    // Assert: valid count

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Issue 4 fix: drain the underlying engine so fresh non-RR frames are
    // captured even when the application calls only receive_non_rr() and never
    // calls receive_request() or receive_response().
    pump_inbound(now_us);

    if (m_non_rr_count == 0U) {
        return Result::ERR_EMPTY;
    }

    envelope_copy(env, m_non_rr_stash[m_non_rr_head]);
    m_non_rr_head = (m_non_rr_head + 1U) % MAX_STASH_SIZE;
    --m_non_rr_count;

    NEVER_COMPILED_OUT_ASSERT(m_non_rr_count < MAX_STASH_SIZE);    // Assert: decremented
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::send_request()
// Safety-critical (SC): HAZ-001, HAZ-002
//
// Assigns a local correlation_id from m_cid_gen, embeds it in the RRHeader,
// and sends via RELIABLE_RETRY.  The responder echoes this ID back in its
// RESPONSE RRHeader; we do not rely on the engine-assigned message_id.
//
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result RequestReplyEngine::send_request(NodeId           destination,
                                        const uint8_t*   app_payload,
                                        uint32_t         app_payload_len,
                                        uint64_t         timeout_us,
                                        uint64_t         now_us,
                                        uint64_t&        correlation_id_out)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);       // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(m_engine != nullptr); // pre: engine pointer valid

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Guard: pending table must have a free slot BEFORE building the envelope.
    uint32_t slot = find_free_pending();
    if (slot >= MAX_PENDING_REQUESTS) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "send_request: pending table full");
        return Result::ERR_FULL;
    }

    // Assign correlation_id from local counter (wraps at UINT64_MAX; 1 is reserved
    // as the first valid value; 0 is the sentinel "none").
    uint64_t cid = m_cid_gen;
    m_cid_gen    = (m_cid_gen == UINT64_MAX) ? 1U : (m_cid_gen + 1U);

    // Build wire payload: RRHeader(REQUEST, cid) + app_payload.
    // Power of 10 Rule 3: stack buffer, bounded size.
    uint8_t  wire_buf[MSG_MAX_PAYLOAD_BYTES];
    uint32_t wire_len = build_wire_payload(RRKind::REQUEST, cid,
                                           app_payload, app_payload_len,
                                           wire_buf, MSG_MAX_PAYLOAD_BYTES);
    if (wire_len == 0U) {
        return Result::ERR_FULL;
    }

    // Construct envelope.
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.destination_id    = destination;
    env.priority          = 0U;
    env.reliability_class = ReliabilityClass::RELIABLE_RETRY; // REQ-3.3.3
    env.expiry_time_us    = (timeout_us > 0U) ? (now_us + timeout_us) : 0U;
    env.payload_length    = wire_len;
    (void)memcpy(env.payload, wire_buf, static_cast<size_t>(wire_len));

    // Power of 10 Rule 7: check return value from engine.send().
    Result res = m_engine->send(env, now_us);
    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "send_request: engine.send failed: result=%u",
                    static_cast<uint8_t>(res));
        return res;
    }

    // Record pending slot.
    m_pending[slot].correlation_id = cid;
    m_pending[slot].expires_us     = (timeout_us > 0U) ? (now_us + timeout_us) : 0U;
    m_pending[slot].active         = true;
    m_pending[slot].stash_len      = 0U;
    m_pending[slot].stash_ready    = false;
    ++m_pending_count;

    correlation_id_out = cid;

    NEVER_COMPILED_OUT_ASSERT(m_pending[slot].active);   // post: slot is active
    NEVER_COMPILED_OUT_ASSERT(correlation_id_out != 0U); // post: non-zero cid

    Logger::log(Severity::INFO, "RequestReplyEngine",
                "Sent request correlation_id=%llu to dst=%u",
                (unsigned long long)cid, destination);

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::receive_request()
// Safety-critical (SC): HAZ-001, HAZ-003
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result RequestReplyEngine::receive_request(uint8_t*   app_payload_buf,
                                           uint32_t   buf_cap,
                                           uint32_t&  app_payload_len_out,
                                           NodeId&    src_out,
                                           uint64_t&  correlation_id_out,
                                           uint64_t   now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);              // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(app_payload_buf != nullptr); // pre: buffer valid

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Drain any newly arrived envelopes first.
    pump_inbound(now_us);

    // Return the oldest active stash entry.
    // Power of 10 Rule 2: bounded by MAX_STASH_SIZE.
    for (uint32_t i = 0U; i < MAX_STASH_SIZE; ++i) {
        if (!m_request_stash[i].active) {
            continue;
        }

        uint32_t copy_len = m_request_stash[i].payload_len;
        if (copy_len > buf_cap) {
            copy_len = buf_cap;
        }
        if (copy_len > 0U) {
            (void)memcpy(app_payload_buf, m_request_stash[i].payload,
                         static_cast<size_t>(copy_len));
        }
        app_payload_len_out = copy_len;
        src_out             = m_request_stash[i].src;
        correlation_id_out  = m_request_stash[i].correlation_id;

        // Free the slot.
        m_request_stash[i].active = false;
        if (m_stash_count > 0U) {
            --m_stash_count;
        }

        NEVER_COMPILED_OUT_ASSERT(correlation_id_out != 0U);       // post: valid cid
        NEVER_COMPILED_OUT_ASSERT(src_out != NODE_ID_INVALID);     // post: valid src

        return Result::OK;
    }

    return Result::ERR_EMPTY;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::send_response()
// Safety-critical (SC): HAZ-001, HAZ-002
// Responses are BEST_EFFORT: fire-and-forget; requester times out if lost.
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result RequestReplyEngine::send_response(NodeId         destination,
                                         uint64_t       correlation_id,
                                         const uint8_t* app_payload,
                                         uint32_t       app_payload_len,
                                         uint64_t       now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);        // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(m_engine != nullptr);  // pre: engine pointer valid

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Build wire payload: RRHeader(RESPONSE, correlation_id) + app_payload.
    uint8_t  wire_buf[MSG_MAX_PAYLOAD_BYTES];
    uint32_t wire_len = build_wire_payload(RRKind::RESPONSE, correlation_id,
                                           app_payload, app_payload_len,
                                           wire_buf, MSG_MAX_PAYLOAD_BYTES);
    if (wire_len == 0U) {
        return Result::ERR_FULL;
    }

    // Construct envelope.
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.destination_id    = destination;
    env.priority          = 0U;
    // REQ-3.3.2: BEST_EFFORT — requester times out if response lost.
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.expiry_time_us    = now_us + 5000000ULL; // 5-second default expiry
    env.payload_length    = wire_len;
    (void)memcpy(env.payload, wire_buf, static_cast<size_t>(wire_len));

    // Power of 10 Rule 7: check return value.
    Result res = m_engine->send(env, now_us);
    if (res != Result::OK) {
        Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                    "send_response: engine.send failed: result=%u",
                    static_cast<uint8_t>(res));
    } else {
        Logger::log(Severity::INFO, "RequestReplyEngine",
                    "Sent response correlation_id=%llu to dst=%u",
                    (unsigned long long)correlation_id, destination);
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::receive_response()
// Safety-critical (SC): HAZ-001, HAZ-002
// Power of 10: ≥2 assertions, CC ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result RequestReplyEngine::receive_response(uint64_t   correlation_id,
                                            uint8_t*   app_payload_buf,
                                            uint32_t   buf_cap,
                                            uint32_t&  app_payload_len_out,
                                            uint64_t   now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);              // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(app_payload_buf != nullptr); // pre: buffer valid

    if (!m_initialized) {
        return Result::ERR_INVALID;
    }

    // Drain any newly arrived envelopes first.
    pump_inbound(now_us);

    // Look up the pending slot.
    uint32_t idx = find_pending(correlation_id);
    if (idx >= MAX_PENDING_REQUESTS) {
        Logger::log(Severity::INFO, "RequestReplyEngine",
                    "receive_response: unknown correlation_id=%llu",
                    (unsigned long long)correlation_id);
        return Result::ERR_INVALID;
    }

    if (!m_pending[idx].stash_ready) {
        return Result::ERR_EMPTY;
    }

    // Copy response payload to caller.
    uint32_t copy_len = m_pending[idx].stash_len;
    if (copy_len > buf_cap) {
        copy_len = buf_cap;
    }
    if (copy_len > 0U) {
        (void)memcpy(app_payload_buf, m_pending[idx].stash_payload,
                     static_cast<size_t>(copy_len));
    }
    app_payload_len_out = copy_len;

    // Free the pending slot.
    m_pending[idx].active      = false;
    m_pending[idx].stash_ready = false;
    if (m_pending_count > 0U) {
        --m_pending_count;
    }

    NEVER_COMPILED_OUT_ASSERT(!m_pending[idx].active);     // post: slot freed
    NEVER_COMPILED_OUT_ASSERT(idx < MAX_PENDING_REQUESTS); // post: valid index

    Logger::log(Severity::INFO, "RequestReplyEngine",
                "Received response correlation_id=%llu len=%u",
                (unsigned long long)correlation_id, copy_len);

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestReplyEngine::sweep_timeouts()
// Safety-critical (SC): HAZ-001, HAZ-002
// Power of 10: ≥2 assertions, CC ≤ 10, bounded loop.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t RequestReplyEngine::sweep_timeouts(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized); // pre: engine ready
    NEVER_COMPILED_OUT_ASSERT(now_us > 0U);   // pre: valid timestamp

    uint32_t freed = 0U;

    // Power of 10 Rule 2: bounded by MAX_PENDING_REQUESTS.
    for (uint32_t i = 0U; i < MAX_PENDING_REQUESTS; ++i) {
        if (!m_pending[i].active) {
            continue;
        }
        // expires_us == 0 means never-expires.
        if (m_pending[i].expires_us == 0U) {
            continue;
        }
        if (now_us >= m_pending[i].expires_us) {
            Logger::log(Severity::WARNING_LO, "RequestReplyEngine",
                        "sweep_timeouts: correlation_id=%llu expired",
                        (unsigned long long)m_pending[i].correlation_id);
            m_pending[i].active      = false;
            m_pending[i].stash_ready = false;
            if (m_pending_count > 0U) {
                --m_pending_count;
            }
            ++freed;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(freed <= MAX_PENDING_REQUESTS);         // post: bounded
    NEVER_COMPILED_OUT_ASSERT(m_pending_count <= MAX_PENDING_REQUESTS); // post: count valid

    return freed;
}
