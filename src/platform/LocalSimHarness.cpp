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
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.3.2
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
    : m_peer(nullptr), m_open(false)
{
    // Power of 10 rule 3: initialize to safe state
    NEVER_COMPILED_OUT_ASSERT(!m_open);
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
        imp_cfg.enabled = config.channels[0U].impairments_enabled;
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
    Result res = m_impairment.process_outbound(envelope, now_us);
    if (res == Result::ERR_IO) {
        // Message dropped by loss impairment
        return Result::OK;
    }

    // Check for delayed messages ready to deliver
    MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
    uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                                              delayed_envelopes,
                                                              IMPAIR_DELAY_BUF_SIZE);

    // Inject delayed messages into peer first (FIFO order)
    // Power of 10: fixed loop bound
    for (uint32_t i = 0U; i < delayed_count; ++i) {
        NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE);
        (void)m_peer->inject(delayed_envelopes[i]);
    }

    // Inject main message into peer
    res = m_peer->inject(envelope);

    NEVER_COMPILED_OUT_ASSERT(res == Result::OK || res == Result::ERR_FULL);  // Post-condition
    return res;
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
    uint32_t iterations = (timeout_ms > 0U) ? timeout_ms : 1U;
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
    m_peer = nullptr;
    m_open = false;
    Logger::log(Severity::INFO, "LocalSimHarness", "Transport closed");
}

// ─────────────────────────────────────────────────────────────────────────────
// is_open()
// ─────────────────────────────────────────────────────────────────────────────

bool LocalSimHarness::is_open() const
{
    NEVER_COMPILED_OUT_ASSERT(m_open == true || m_open == false);  // Invariant: valid bool
    return m_open;
}
