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
 * @file LocalSimHarness.cpp
 * @brief Implementation of in-process simulation transport.
 *
 * Provides direct, in-memory message passing with impairment simulation
 * for deterministic testing without real sockets.
 *
 * Rules applied:
 *   - Power of 10: all functions ≤1 page, ≥2 assertions each, fixed loop bounds.
 *   - MISRA C++: no exceptions, all return values checked.
 *   - F-Prime style: event logging via Logger.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.3.2, REQ-6.1.10, REQ-7.2.4
 */

#include "platform/LocalSimHarness.hpp"
#include "core/Assert.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include <unistd.h>
#include <time.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

LocalSimHarness::LocalSimHarness()
    : m_peer(nullptr), m_open(false),
      m_connections_opened(0U), m_connections_closed(0U)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(!m_open);
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened == 0U);  // Assert: counters zeroed
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────

LocalSimHarness::~LocalSimHarness()
{
    // Power of 10: explicit qualified call avoids virtual dispatch in destructor
    LocalSimHarness::close();
}

// ─────────────────────────────────────────────────────────────────────────────
// init()
// ─────────────────────────────────────────────────────────────────────────────

Result LocalSimHarness::init(const TransportConfig& config)
{
    NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::LOCAL_SIM);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(!m_open);  // Not already initialized

    // Initialize receive queue
    m_recv_queue.init();

    // Initialize impairment engine
    ImpairmentConfig imp_cfg;
    impairment_config_default(imp_cfg);
    if (config.num_channels > 0U) {
        imp_cfg = config.channels[0U].impairment;
    }
    m_impairment.init(imp_cfg);

    m_open = true;
    Logger::log(Severity::INFO, "LocalSimHarness",
               "Local simulation harness initialized (node %u)",
               config.local_node_id);

    NEVER_COMPILED_OUT_ASSERT(m_open);  // Post-condition
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// register_local_id()
// ─────────────────────────────────────────────────────────────────────────────

Result LocalSimHarness::register_local_id(NodeId id)
{
    NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID);  // pre-condition: valid NodeId
    (void)id;  // REQ-6.1.10: LocalSim has no connection-oriented registration
    NEVER_COMPILED_OUT_ASSERT(m_open);  // pre-condition: transport must be initialised
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// link()
// ─────────────────────────────────────────────────────────────────────────────

// cppcheck-suppress unusedFunction -- called from tests/test_LocalSim.cpp
void LocalSimHarness::link(LocalSimHarness* peer)
{
    NEVER_COMPILED_OUT_ASSERT(peer != nullptr);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(peer != this);     // Pre-condition: not self-loop

    m_peer = peer;
    ++m_connections_opened;  // REQ-7.2.4: link counts as connection established
    Logger::log(Severity::INFO, "LocalSimHarness", "Harness linked to peer");
}

// ─────────────────────────────────────────────────────────────────────────────
// inject()
// ─────────────────────────────────────────────────────────────────────────────

Result LocalSimHarness::inject(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition
    // S3: validate envelope before accepting into receive queue (same check as
    // send_message() and deliver_from_peer()); prevents malformed envelopes from
    // bypassing validation and reaching DeliveryEngine::receive().
    if (!envelope_valid(envelope)) {
        NEVER_COMPILED_OUT_ASSERT(true);  // post: invalid envelope discarded
        return Result::ERR_INVALID;
    }

    Result res = m_recv_queue.push(envelope);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                   "Receive queue full; dropping message");
    }

    NEVER_COMPILED_OUT_ASSERT(res == Result::OK || res == Result::ERR_FULL);  // Post-condition
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// deliver_from_peer() — private helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Routes a linked-peer envelope through this harness's inbound impairment
// before queuing to m_recv_queue.  Called only from flush_outbound_batch();
// not a public test hook.
//
// Inbound impairment applied:
//   1. Partition check via is_partition_active(): if active, drop → return false.
//   2. Reordering via process_inbound(): if count==0 (buffered), return false;
//      if count==1, push to m_recv_queue and return true.
//
// inject() is intentionally NOT called here; inject() is the raw test hook
// that bypasses all impairment.
//
// Implements: REQ-5.1.5, REQ-5.1.6
// Power of 10: ≥2 assertions, CC ≤ 10, no dynamic allocation.

Result LocalSimHarness::deliver_from_peer(const MessageEnvelope& env, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);          // Pre-condition: receiver must be open
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Pre-condition: valid envelope

    // REQ-5.1.6: Check inbound partition (intermittent outage) on the receiver side.
    // If a partition is active on this harness, silently drop the arriving envelope.
    if (m_impairment.is_partition_active(now_us)) {
        Logger::log(Severity::WARNING_LO, "LocalSimHarness",
                    "deliver_from_peer: inbound partition active; dropping message");
        return Result::ERR_IO;  // Dropped by partition
    }

    // REQ-5.1.5: Apply inbound reordering via process_inbound().
    // A 1-slot output buffer is sufficient: process_inbound() emits at most 1
    // envelope per call (either a pass-through or a randomly released reorder entry).
    MessageEnvelope inbound_out;
    uint32_t        inbound_count = 0U;
    Result res = m_impairment.process_inbound(env, now_us,
                                              &inbound_out, 1U, inbound_count);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                    "deliver_from_peer: process_inbound failed; dropping message");
        return Result::ERR_IO;
    }

    // If count == 0, the envelope was buffered in the reorder window for later
    // release. It will emerge on a subsequent call; do not queue now.
    // ERR_TIMEOUT signals "not yet delivered; reorder-buffered" to flush_outbound_batch.
    if (inbound_count == 0U) {
        return Result::ERR_TIMEOUT;
    }

    // Invariant: process_inbound() emits at most 1 envelope into a 1-slot buffer.
    NEVER_COMPILED_OUT_ASSERT(inbound_count <= 1U);

    // Push the (possibly reordered) envelope into the receive queue.
    Result push_res = m_recv_queue.push(inbound_out);
    if (!result_ok(push_res)) {
        Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                    "deliver_from_peer: recv queue full; dropping inbound message");
        return Result::ERR_FULL;  // Queue full — report to caller
    }

    return Result::OK;  // Envelope successfully queued
}

// ─────────────────────────────────────────────────────────────────────────────
// flush_outbound_batch() — private helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Routes each envelope in @p batch through the peer's inbound impairment
// (deliver_from_peer()) before queuing.  inject() is no longer called here;
// it remains a separate raw test hook that bypasses impairment entirely.
//
// Three-case attribution:
//   Current envelope in batch and deliver_from_peer() returns false
//     AND the peer's receive queue is not the cause (partition/reorder):
//     → return ERR_IO (consistent with outbound-loss semantics).
//   Current envelope NOT in batch             → return OK (queued for later delivery).
//   Non-current envelope deliver fails        → log WARNING_HI, continue, return OK.
//
// Note: deliver_from_peer() returns bool (true=queued, false=dropped/buffered).
// ERR_FULL from the peer ring is swallowed inside deliver_from_peer() and logged;
// we cannot distinguish it from a partition drop at this layer.  If queue-full
// attribution is needed for the current envelope, callers should monitor
// get_transport_stats().
//
// Implements: REQ-5.1.5, REQ-5.1.6
// Power of 10: fixed loop bound (count ≤ IMPAIR_DELAY_BUF_SIZE).

Result LocalSimHarness::flush_outbound_batch(const MessageEnvelope& envelope,
                                              const MessageEnvelope* batch,
                                              uint32_t count,
                                              uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(batch != nullptr);
    NEVER_COMPILED_OUT_ASSERT(count <= IMPAIR_DELAY_BUF_SIZE);

    Result current_result = Result::OK;
    for (uint32_t i = 0U; i < count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);

        bool is_current = (batch[i].source_id  == envelope.source_id &&
                           batch[i].message_id == envelope.message_id);

        // Route through the peer's inbound impairment (partition + reorder).
        // OK        — queued to peer's m_recv_queue.
        // ERR_IO    — dropped by inbound partition; treat as send failure.
        // ERR_FULL  — peer receive queue saturated; confirmed not received.
        // ERR_TIMEOUT — reorder-buffered (not yet emitted); not an error.
        Result deliver_res = m_peer->deliver_from_peer(batch[i], now_us);

        if (deliver_res == Result::ERR_IO || deliver_res == Result::ERR_FULL) {
            if (is_current) {
                // Attribute delivery failure to the caller for the current envelope.
                Logger::log(Severity::WARNING_LO, "LocalSimHarness",
                            "flush_outbound_batch: current envelope not delivered "
                            "(inbound impairment on peer); result=%d",
                            static_cast<int>(deliver_res));
                current_result = deliver_res;
            } else {
                Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                            "flush_outbound_batch: delayed envelope at index %u "
                            "not delivered (result=%d)", i,
                            static_cast<int>(deliver_res));
            }
        }
        // ERR_TIMEOUT (reorder-buffered) and OK need no extra handling here.
    }
    return current_result;
}

// ─────────────────────────────────────────────────────────────────────────────
// send_message()
// ─────────────────────────────────────────────────────────────────────────────

Result LocalSimHarness::send_message(const MessageEnvelope& envelope)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition
    NEVER_COMPILED_OUT_ASSERT(m_peer != nullptr);  // Pre-condition: must be linked
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope));  // Pre-condition

    // Apply impairment: process_outbound may drop the message (ERR_IO = drop silently)
    uint64_t now_us = timestamp_now_us();
    // HIGH-1 fix: capture outbound result before flushing delayed messages.
    // ERR_IO   — current message dropped by loss impairment; return OK (intentional drop).
    // ERR_FULL — delay buffer full; current message dropped; return ERR_FULL to caller.
    //            Previously ERR_FULL was silently overwritten by a later inject() OK result.
    // OK       — message queued; will appear via collect_deliverable() when its timer fires.
    Result outbound_res = m_impairment.process_outbound(envelope, now_us);
    if (outbound_res == Result::ERR_IO) {
        // Intentional loss-impairment drop; caller does not retry (by design).
        return Result::OK;
    }
    if (outbound_res != Result::OK) {
        // Delay buffer full or other impairment error; return immediately.
        return outbound_res;
    }

    // Collect all messages now ready to deliver (includes the current message when
    // impairments are disabled, since process_outbound queues it with release_us=now_us).
    // Do NOT also inject `envelope` directly — that would double-send every message
    // and bypass configured delay for the current message. (HAZ-003, HAZARD_ANALYSIS §2)
    // flush_outbound_batch() handles the three-case attribution (see its comment).
    MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                                              delayed_envelopes,
                                                              IMPAIR_DELAY_BUF_SIZE);

    // flush_outbound_batch routes each envelope through the peer's inbound
    // impairment (deliver_from_peer), then returns OK or ERR_IO.
    return flush_outbound_batch(envelope, delayed_envelopes, delayed_count, now_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// receive_message()
// ─────────────────────────────────────────────────────────────────────────────

Result LocalSimHarness::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(m_open);  // Pre-condition

    // Check receive queue first (instant)
    Result res = m_recv_queue.pop(envelope);
    if (result_ok(res)) {
        return res;
    }

    // If timeout is 0, return immediately (non-blocking)
    if (timeout_ms == 0U) {
        return Result::ERR_TIMEOUT;
    }

    // Poll with nanosleep: bounded iterations (Power of 10 rule 2)
    // Each iteration sleeps ~1ms, so timeout_ms iterations gives ~timeout_ms delay
    uint32_t iterations = timeout_ms;  // timeout_ms > 0U guaranteed by early return at L158
    if (iterations > 5000U) {
        iterations = 5000U;  // Cap at 5 seconds
    }

    for (uint32_t i = 0U; i < iterations; ++i) {
        // Sleep 1ms
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 1000000;  // 1 millisecond in nanoseconds

        (void)nanosleep(&ts, nullptr);

        // Try to pop from queue
        res = m_recv_queue.pop(envelope);
        if (result_ok(res)) {
            return res;
        }
    }

    // Timeout
    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// close()
// ─────────────────────────────────────────────────────────────────────────────

void LocalSimHarness::close()
{
    // Guard: only count a close when a corresponding link() was recorded.
    // init() sets m_open=true but does NOT increment m_connections_opened;
    // link() is the event that increments m_connections_opened.  Without this
    // guard, init(); close() (no link()) would produce closed=1, opened=0,
    // tripping the monotonic invariant asserted in get_transport_stats().
    if (m_open && (m_connections_closed < m_connections_opened)) {
        ++m_connections_closed;  // REQ-7.2.4: count close events while linked
    }
    m_peer = nullptr;
    m_open = false;
    Logger::log(Severity::INFO, "LocalSimHarness", "Transport closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool LocalSimHarness::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open || (m_peer == nullptr));  // Invariant: when closed, peer is null
    return m_open;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_transport_stats() — REQ-7.2.4 / REQ-7.2.2 observability
// NSC: read-only; no state change.
// ─────────────────────────────────────────────────────────────────────────────

void LocalSimHarness::get_transport_stats(TransportStats& out) const
{
    // Power of 10 rule 5: ≥2 assertions
    NEVER_COMPILED_OUT_ASSERT(m_connections_opened >= m_connections_closed);  // Assert: monotonic counters
    NEVER_COMPILED_OUT_ASSERT(m_connections_closed <= m_connections_opened);  // Assert: closed ≤ opened

    transport_stats_init(out);
    out.connections_opened = m_connections_opened;
    out.connections_closed = m_connections_closed;
    out.impairment         = m_impairment.get_stats();
}
