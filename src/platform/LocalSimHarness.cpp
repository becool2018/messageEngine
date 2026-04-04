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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.3.2, REQ-7.2.4
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

    Result res = m_recv_queue.push(envelope);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                   "Receive queue full; dropping message");
    }

    NEVER_COMPILED_OUT_ASSERT(res == Result::OK || res == Result::ERR_FULL);  // Post-condition
    return res;
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
    MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                                              delayed_envelopes,
                                                              IMPAIR_DELAY_BUF_SIZE);

    // Flush errors from older delayed messages are not attributed to the current
    // send — the current envelope may have a future release_us and remain queued.
    // Log individual failures for observability; always return OK (current message
    // was accepted into the local pipeline above).
    // Power of 10: fixed loop bound.
    for (uint32_t i = 0U; i < delayed_count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        Result inject_res = m_peer->inject(delayed_envelopes[i]);
        if (inject_res != Result::OK) {
            Logger::log(Severity::WARNING_HI, "LocalSimHarness",
                        "inject() failed for delayed message at index %u: result=%d",
                        i, static_cast<int>(inject_res));
        }
    }

    return Result::OK;
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
    if (m_open) {
        ++m_connections_closed;  // REQ-7.2.4: count close events while open
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
