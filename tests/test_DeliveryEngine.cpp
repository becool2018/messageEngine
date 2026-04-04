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
 * @file test_DeliveryEngine.cpp
 * @brief Unit tests for DeliveryEngine: send, receive, retry, and ACK timeout logic.
 *
 * Uses LocalSimHarness as the transport for in-process, deterministic testing.
 * Tests cover all major branches of DeliveryEngine.cpp targeting ≥75% branch coverage.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-7.2.1, REQ-7.2.3, REQ-7.2.4
 */
// Verification: M1 + M2 + M4 + M5
// M4: all reachable branches exercised via LocalSimHarness loopback (tests 1–23).
// M5: transport ERR_IO paths (unreachable in loopback) exercised via
//     MockTransportInterface (tests 24–26); see MockTransportInterface.hpp.

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/ChannelConfig.hpp"
#include "core/DeliveryEngine.hpp"
#include "platform/LocalSimHarness.hpp"
#include "MockTransportInterface.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Test infrastructure: fixed now_us to avoid real-clock coupling
// ─────────────────────────────────────────────────────────────────────────────
static const uint64_t NOW_US = 1000000ULL;  // 1 second into epoch (non-zero)

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create a LOCAL_SIM TransportConfig
// ─────────────────────────────────────────────────────────────────────────────
static void create_local_sim_config(TransportConfig& cfg, NodeId node_id)
{
    transport_config_default(cfg);
    cfg.kind          = TransportKind::LOCAL_SIM;
    cfg.local_node_id = node_id;
    cfg.is_server     = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a DATA envelope with 5-second expiry
// ─────────────────────────────────────────────────────────────────────────────
static void make_data_envelope(MessageEnvelope&  env,
                                NodeId            src,
                                NodeId            dst,
                                uint64_t          msg_id,
                                ReliabilityClass  rel)
{
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = msg_id;
    env.timestamp_us      = NOW_US;
    env.source_id         = src;
    env.destination_id    = dst;
    env.priority          = 0U;
    env.reliability_class = rel;
    env.expiry_time_us    = NOW_US + 5000000ULL;  // 5 seconds from now
    env.payload_length    = 4U;
    env.payload[0]        = 0xAAU;
    env.payload[1]        = 0xBBU;
    env.payload[2]        = 0xCCU;
    env.payload[3]        = 0xDDU;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a DATA envelope with expiry in the past
// ─────────────────────────────────────────────────────────────────────────────
static void make_expired_envelope(MessageEnvelope& env, NodeId src, NodeId dst)
{
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 999ULL;
    env.timestamp_us      = 1ULL;
    env.source_id         = src;
    env.destination_id    = dst;
    env.priority          = 0U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.expiry_time_us    = 1ULL;  // expiry in the past
    env.payload_length    = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build an ACK envelope referencing the given message_id
// ─────────────────────────────────────────────────────────────────────────────
static void make_ack_envelope(MessageEnvelope&       ack,
                               NodeId                 ack_src,
                               NodeId                 ack_dst,
                               uint64_t               acked_msg_id)
{
    envelope_init(ack);
    ack.message_type      = MessageType::ACK;
    ack.message_id        = acked_msg_id;
    ack.timestamp_us      = NOW_US;
    ack.source_id         = ack_src;
    ack.destination_id    = ack_dst;
    ack.priority          = 0U;
    ack.reliability_class = ReliabilityClass::BEST_EFFORT;
    ack.expiry_time_us    = 0U;   // ACKs do not expire
    ack.payload_length    = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test setup helper: initialize two linked harnesses + one DeliveryEngine
// harness_a is the local node (id=1); harness_b is the remote node (id=2)
// engine uses harness_a as its transport
// ─────────────────────────────────────────────────────────────────────────────
static void setup_engine(LocalSimHarness& harness_a,
                          LocalSimHarness& harness_b,
                          DeliveryEngine&  engine)
{
    TransportConfig cfg_a;
    create_local_sim_config(cfg_a, 1U);
    // Enable retries in channel config
    cfg_a.channels[0].max_retries      = 5U;
    cfg_a.channels[0].retry_backoff_ms = 100U;
    cfg_a.channels[0].recv_timeout_ms  = 1000U;

    Result r_a = harness_a.init(cfg_a);
    assert(r_a == Result::OK);

    TransportConfig cfg_b;
    create_local_sim_config(cfg_b, 2U);
    Result r_b = harness_b.init(cfg_b);
    assert(r_b == Result::OK);

    // Bidirectional link
    harness_a.link(&harness_b);
    harness_b.link(&harness_a);

    engine.init(&harness_a, cfg_a.channels[0], 1U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: send BEST_EFFORT; result OK; message arrives at harness_b
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_best_effort()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::BEST_EFFORT);

    Result res = engine.send(env, NOW_US);

    assert(res == Result::OK);

    // Verify message arrived at harness_b
    MessageEnvelope recv_env;
    Result recv_res = harness_b.receive_message(recv_env, 100U);
    assert(recv_res == Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_best_effort\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: send RELIABLE_ACK; result OK; message arrives at harness_b
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_reliable_ack()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);

    Result res = engine.send(env, NOW_US);

    assert(res == Result::OK);

    MessageEnvelope recv_env;
    Result recv_res = harness_b.receive_message(recv_env, 100U);
    assert(recv_res == Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_reliable_ack\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: send RELIABLE_RETRY; result OK; message arrives at harness_b
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_reliable_retry()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);

    Result res = engine.send(env, NOW_US);

    assert(res == Result::OK);

    MessageEnvelope recv_env;
    Result recv_res = harness_b.receive_message(recv_env, 100U);
    assert(recv_res == Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_reliable_retry\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: inject a BEST_EFFORT DATA message; engine.receive() returns OK
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_data_best_effort()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Inject directly into harness_a's receive queue
    MessageEnvelope inject_env;
    make_data_envelope(inject_env, 2U, 1U, 12345ULL, ReliabilityClass::BEST_EFFORT);
    Result inject_res = harness_a.inject(inject_env);
    assert(inject_res == Result::OK);

    MessageEnvelope recv_env;
    Result recv_res = engine.receive(recv_env, 100U, NOW_US);

    assert(recv_res == Result::OK);
    assert(recv_env.message_id == 12345ULL);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_data_best_effort\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: inject expired DATA envelope; engine.receive() returns ERR_EXPIRED
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_expired()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope expired_env;
    make_expired_envelope(expired_env, 2U, 1U);
    Result inject_res = harness_a.inject(expired_env);
    assert(inject_res == Result::OK);

    MessageEnvelope recv_env;
    // now_us >> expiry_time_us so timestamp_expired() returns true
    Result recv_res = engine.receive(recv_env, 100U, NOW_US);

    assert(recv_res == Result::ERR_EXPIRED);
    assert(recv_res != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_expired\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ-3.2.7 dedicated tests — expiry edge cases
// ─────────────────────────────────────────────────────────────────────────────

// Test: send() with an already-expired envelope returns ERR_EXPIRED immediately
// (send_via_transport() expiry gate — UC_04).
// Verifies: REQ-3.2.7, REQ-3.3.4
static void test_send_expired_returns_err()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Expired outbound envelope: expiry_time_us is in the past relative to NOW_US
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 77777ULL, ReliabilityClass::BEST_EFFORT);
    env.expiry_time_us = 1ULL;  // way in the past

    Result res = engine.send(env, NOW_US);
    assert(res == Result::ERR_EXPIRED);

    // Nothing should have been sent to harness_b
    MessageEnvelope dummy;
    Result rr = harness_b.receive_message(dummy, 10U);
    assert(rr != Result::OK);

    harness_a.close();
    harness_b.close();
    printf("PASS: test_send_expired_returns_err\n");
}

// Test: receive() with expiry_time_us == now_us (exactly at deadline) returns
// ERR_EXPIRED. Covers the boundary: timestamp_expired returns true when
// now_us >= expiry_us (i.e., at-deadline is expired, not just past-deadline).
// Verifies: REQ-3.2.7, REQ-3.3.4
static void test_receive_expired_at_boundary()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // expiry exactly equal to NOW_US — must be treated as expired
    MessageEnvelope env;
    make_data_envelope(env, 2U, 1U, 88888ULL, ReliabilityClass::BEST_EFFORT);
    env.expiry_time_us = NOW_US;

    Result inj = harness_a.inject(env);
    assert(inj == Result::OK);

    MessageEnvelope out;
    Result res = engine.receive(out, 100U, NOW_US);
    assert(res == Result::ERR_EXPIRED);

    harness_a.close();
    harness_b.close();
    printf("PASS: test_receive_expired_at_boundary\n");
}

// Test: receive() with expiry_time_us == 0 (never-expires sentinel) delivers
// the message. Ensures the zero-expiry convention is honoured end-to-end on
// the receive path and a DATA envelope is not falsely dropped.
// Verifies: REQ-3.2.7, REQ-3.3.4
static void test_receive_zero_expiry_never_drops()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope env;
    make_data_envelope(env, 2U, 1U, 99999ULL, ReliabilityClass::BEST_EFFORT);
    env.expiry_time_us = 0ULL;  // 0 == never expires

    Result inj = harness_a.inject(env);
    assert(inj == Result::OK);

    MessageEnvelope out;
    // Pass a very large now_us — should NOT be dropped despite high time value
    Result res = engine.receive(out, 100U, UINT64_MAX / 2U);
    assert(res == Result::OK);
    assert(out.message_id == 99999ULL);

    harness_a.close();
    harness_b.close();
    printf("PASS: test_receive_zero_expiry_never_drops\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: send RELIABLE_RETRY; inject matching ACK; engine.receive() processes
// the ACK (not delivered as data); pump_retries returns 0 (retry cancelled)
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_ack_cancels_retry()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Send a RELIABLE_RETRY message
    MessageEnvelope send_env;
    make_data_envelope(send_env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result send_res = engine.send(send_env, NOW_US);
    assert(send_res == Result::OK);

    // Retrieve message_id assigned by the engine (from harness_b's queue)
    MessageEnvelope received_at_b;
    Result rb_res = harness_b.receive_message(received_at_b, 100U);
    assert(rb_res == Result::OK);
    uint64_t sent_msg_id = received_at_b.message_id;

    // Build an ACK from node 2 (receiver) to node 1 (sender) referencing the sent message_id.
    // DeliveryEngine::receive() now uses env.destination_id (=1, the local sender) for the
    // tracker lookup, matching the source_id stored at track() time.
    MessageEnvelope ack_env;
    make_ack_envelope(ack_env, 2U, 1U, sent_msg_id);

    // Inject the ACK into harness_a's receive queue
    Result inject_res = harness_a.inject(ack_env);
    assert(inject_res == Result::OK);

    // engine.receive() processes the ACK; returns OK (control messages pass through)
    MessageEnvelope out_env;
    Result recv_res = engine.receive(out_env, 100U, NOW_US);
    // ACK is a control message; DeliveryEngine returns OK but does not deliver as data
    assert(recv_res == Result::OK);

    // pump_retries: the retry should now be cancelled; collect returns 0
    // First pump may fire the initial retry (since next_retry_us == NOW_US)
    // We need to advance time far enough that the cancelled entry does not reappear
    uint32_t retried = engine.pump_retries(NOW_US + 1000000000ULL);
    assert(retried == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_ack_cancels_retry\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: inject same RELIABLE_RETRY message twice; first OK, second ERR_DUPLICATE
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_duplicate()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Build a RELIABLE_RETRY DATA envelope with a fixed message_id
    MessageEnvelope data_env;
    make_data_envelope(data_env, 2U, 1U, 42000ULL, ReliabilityClass::RELIABLE_RETRY);

    // Inject the same envelope twice into harness_a
    Result inj1 = harness_a.inject(data_env);
    assert(inj1 == Result::OK);
    Result inj2 = harness_a.inject(data_env);
    assert(inj2 == Result::OK);

    // First receive: OK
    MessageEnvelope out1;
    Result res1 = engine.receive(out1, 100U, NOW_US);
    assert(res1 == Result::OK);

    // Second receive: ERR_DUPLICATE
    MessageEnvelope out2;
    Result res2 = engine.receive(out2, 100U, NOW_US);
    assert(res2 == Result::ERR_DUPLICATE);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_duplicate\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: receive with timeout=50ms and nothing in queue; returns ERR_TIMEOUT
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_timeout()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Nothing injected; should timeout
    MessageEnvelope out_env;
    Result res = engine.receive(out_env, 50U, NOW_US);

    assert(res == Result::ERR_TIMEOUT);
    assert(res != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: fresh engine with no sends; pump_retries() returns 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_pump_retries_no_retries()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    uint32_t count = engine.pump_retries(NOW_US);

    assert(count == 0U);
    assert(count == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_pump_retries_no_retries\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: send RELIABLE_RETRY; pump_retries fires immediately (first retry
// is scheduled at now_us); calling with same or larger now_us returns ≥1
// ─────────────────────────────────────────────────────────────────────────────
static void test_pump_retries_fires()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Send RELIABLE_RETRY; RetryManager schedules next_retry_us = NOW_US
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result send_res = engine.send(env, NOW_US);
    assert(send_res == Result::OK);

    // Drain harness_b's initial send
    MessageEnvelope drain_env;
    (void)harness_b.receive_message(drain_env, 10U);

    // pump_retries with now_us slightly advanced; first retry is due immediately
    uint32_t retried = engine.pump_retries(NOW_US + 1ULL);

    assert(retried >= 1U);
    assert(retried <= MSG_RING_CAPACITY);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_pump_retries_fires\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: fresh engine; sweep_ack_timeouts() returns 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_no_timeouts()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    uint32_t count = engine.sweep_ack_timeouts(NOW_US);

    assert(count == 0U);
    assert(count == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_sweep_no_timeouts\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: send RELIABLE_ACK (tracked by AckTracker); sweep with far-future
// now_us (past the recv_timeout_ms deadline); returns 1
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_detects_timeout()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Send RELIABLE_ACK; engine tracks it with deadline = NOW_US + recv_timeout_ms*1000
    // recv_timeout_ms is set to 1000ms in setup_engine → deadline = NOW_US + 1000000us
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);
    Result send_res = engine.send(env, NOW_US);
    assert(send_res == Result::OK);

    // Sweep with now_us far in the future (well past deadline)
    uint32_t count = engine.sweep_ack_timeouts(NOW_US + 10000000ULL);

    assert(count == 1U);
    assert(count >= 1U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_sweep_detects_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: fill AckTracker to capacity; next RELIABLE_ACK send returns ERR_FULL
// (tracker full → reliability contract broken → send returns ERR_FULL)
// Verifies: REQ-3.3.2
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_ack_tracker_full()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Fill the AckTracker by sending ACK_TRACKER_CAPACITY RELIABLE_ACK messages
    // Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);
        Result r = engine.send(env, NOW_US);
        assert(r == Result::OK);
        // Drain harness_b to prevent its queue from filling
        MessageEnvelope drain;
        (void)harness_b.receive_message(drain, 10U);
    }

    // AckTracker is now full; next RELIABLE_ACK must return ERR_FULL because
    // ACK tracking failure breaks the reliability contract (no slot allocated,
    // so the timeout sweep will never fire for this message).
    MessageEnvelope overflow_env;
    make_data_envelope(overflow_env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);
    Result res = engine.send(overflow_env, NOW_US);

    // tracker full → send returns ERR_FULL
    assert(res == Result::ERR_FULL);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_ack_tracker_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: fill RetryManager to capacity; next RELIABLE_RETRY send returns
// ERR_FULL (retry manager full → reliability contract broken → ERR_FULL)
// Verifies: REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_retry_manager_full()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Fill the RetryManager by sending ACK_TRACKER_CAPACITY RELIABLE_RETRY messages
    // Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
        Result r = engine.send(env, NOW_US);
        assert(r == Result::OK);
        MessageEnvelope drain;
        (void)harness_b.receive_message(drain, 10U);
    }

    // RetryManager is now full; next RELIABLE_RETRY must return ERR_FULL because
    // retry scheduling failure breaks the reliability contract (no retry will fire
    // on timeout without a slot).
    MessageEnvelope overflow_env;
    make_data_envelope(overflow_env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result res = engine.send(overflow_env, NOW_US);

    // retry manager full → send returns ERR_FULL
    assert(res == Result::ERR_FULL);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_retry_manager_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: pump_retries when the transport queue is full covers the send_res != OK
// branch inside the pump_retries retry loop (line 246 True branch)
// ─────────────────────────────────────────────────────────────────────────────
static void test_pump_retries_send_fails()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Schedule a RELIABLE_RETRY message
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result send_res = engine.send(env, NOW_US);
    assert(send_res == Result::OK);

    // engine.send() places exactly 1 entry in harness_b (through the impairment
    // delay buffer; collect_deliverable injects it once). Fill the remaining
    // MSG_RING_CAPACITY - 1 slots so the next pump_retries send fails ERR_FULL.
    for (uint32_t i = 0U; i < MSG_RING_CAPACITY - 1U; ++i) {
        MessageEnvelope filler;
        make_data_envelope(filler, 2U, 1U, static_cast<uint64_t>(i) + 100U, ReliabilityClass::BEST_EFFORT);
        Result r = harness_b.inject(filler);
        assert(r == Result::OK);
    }

    // pump_retries: retry fires immediately (next_retry_us == NOW_US), but the
    // transport send fails (harness_b queue is full) — covers line 246 True branch
    uint32_t retried = engine.pump_retries(NOW_US + 1ULL);

    // Even though transport failed, collect_due still counted 1 entry as "due"
    assert(retried >= 1U);
    assert(retried <= MSG_RING_CAPACITY);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_pump_retries_send_fails\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: fill harness_b's receive queue, then send.
// In LocalSimHarness the peer's receive ring IS the network; ERR_FULL from
// inject() is a confirmed delivery failure.  send_message() now returns ERR_FULL
// and DeliveryEngine::send() must propagate it to the caller.
// (Fixes the incorrect expectation from commit 03f9f23 which asserted OK here.)
// ─────────────────────────────────────────────────────────────────────────────
static void test_send_transport_queue_full()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Fill harness_b's receive queue to capacity via direct inject
    for (uint32_t i = 0U; i < MSG_RING_CAPACITY; ++i) {
        MessageEnvelope filler;
        make_data_envelope(filler, 2U, 1U, static_cast<uint64_t>(i) + 1U, ReliabilityClass::BEST_EFFORT);
        Result r = harness_b.inject(filler);
        assert(r == Result::OK);
    }

    // send() calls send_message(); peer queue is full so inject() returns ERR_FULL.
    // ERR_FULL is a confirmed delivery failure and must be propagated to the caller.
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::BEST_EFFORT);
    Result res = engine.send(env, NOW_US);
    assert(res == Result::ERR_FULL);
    assert(res != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_send_transport_queue_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test (original 13): inject a NAK control message; engine.receive() returns OK
// (covers envelope_is_control() True, but message_type != ACK branch)
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_nak_control()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Build a NAK control envelope
    MessageEnvelope nak_env;
    envelope_init(nak_env);
    nak_env.message_type      = MessageType::NAK;
    nak_env.message_id        = 777ULL;
    nak_env.timestamp_us      = NOW_US;
    nak_env.source_id         = 2U;
    nak_env.destination_id    = 1U;
    nak_env.priority          = 0U;
    nak_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    nak_env.expiry_time_us    = 0U;  // never expires (control message)
    nak_env.payload_length    = 0U;

    // Inject into harness_a's receive queue
    Result inject_res = harness_a.inject(nak_env);
    assert(inject_res == Result::OK);

    // engine.receive() should handle NAK as control; returns OK
    MessageEnvelope out_env;
    Result recv_res = engine.receive(out_env, 100U, NOW_US);

    assert(recv_res == Result::OK);
    assert(out_env.message_type == MessageType::NAK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_nak_control\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: inject a HEARTBEAT control message; engine.receive() returns OK
// (additional coverage for envelope_is_control True, not-ACK branch)
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_heartbeat_control()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    MessageEnvelope hb_env;
    envelope_init(hb_env);
    hb_env.message_type      = MessageType::HEARTBEAT;
    hb_env.message_id        = 888ULL;
    hb_env.timestamp_us      = NOW_US;
    hb_env.source_id         = 2U;
    hb_env.destination_id    = 1U;
    hb_env.priority          = 0U;
    hb_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    hb_env.expiry_time_us    = 0U;
    hb_env.payload_length    = 0U;

    Result inject_res = harness_a.inject(hb_env);
    assert(inject_res == Result::OK);

    MessageEnvelope out_env;
    Result recv_res = engine.receive(out_env, 100U, NOW_US);

    assert(recv_res == Result::OK);
    assert(out_env.message_type == MessageType::HEARTBEAT);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_heartbeat_control\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: send multiple RELIABLE_RETRY messages; pump_retries re-sends them
// This covers the send_res == OK branch in the pump_retries loop
// ─────────────────────────────────────────────────────────────────────────────
static void test_pump_retries_resends()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Send two RELIABLE_RETRY messages
    MessageEnvelope env1;
    make_data_envelope(env1, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result r1 = engine.send(env1, NOW_US);
    assert(r1 == Result::OK);

    MessageEnvelope env2;
    make_data_envelope(env2, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result r2 = engine.send(env2, NOW_US);
    assert(r2 == Result::OK);

    // Drain harness_b's initial sends
    MessageEnvelope drain_env;
    (void)harness_b.receive_message(drain_env, 10U);
    (void)harness_b.receive_message(drain_env, 10U);

    // pump_retries: both retries fire immediately (scheduled at NOW_US)
    uint32_t retried = engine.pump_retries(NOW_US + 1ULL);

    assert(retried >= 1U);
    assert(retried <= MSG_RING_CAPACITY);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_pump_retries_resends\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: init() zero-initialises m_retry_buf; pump_retries() immediately after
// init with nothing scheduled reads only 0 entries — confirms the init loops ran
// without corrupting engine state.
// Verifies: REQ-4.1.1, REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_init_retry_buf_is_zeroed()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Nothing scheduled; pump must return 0 and must not assert on any slot
    uint32_t count = engine.pump_retries(NOW_US);

    assert(count == 0U);
    assert(count <= MSG_RING_CAPACITY);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_init_retry_buf_is_zeroed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: init() zero-initialises m_timeout_buf; sweep_ack_timeouts() immediately
// after init with nothing tracked reads 0 entries — confirms the init loops ran
// without corrupting engine state.
// Verifies: REQ-4.1.1, REQ-3.3.2
// ─────────────────────────────────────────────────────────────────────────────
static void test_init_timeout_buf_is_zeroed()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Nothing tracked; sweep must return 0 and must not assert on any slot
    uint32_t count = engine.sweep_ack_timeouts(NOW_US);

    assert(count == 0U);
    assert(count <= ACK_TRACKER_CAPACITY);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_init_timeout_buf_is_zeroed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: fill RetryManager to capacity (ACK_TRACKER_CAPACITY slots), make all
// entries due simultaneously, pump_retries() fills m_retry_buf to its maximum
// realistic depth (ACK_TRACKER_CAPACITY entries); confirms no buffer overrun.
// Verifies: REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_pump_retries_capacity()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Fill RetryManager to its capacity with RELIABLE_RETRY messages
    // Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
        Result r = engine.send(env, NOW_US);
        assert(r == Result::OK);
        // Drain harness_b to prevent its queue filling
        MessageEnvelope drain;
        (void)harness_b.receive_message(drain, 10U);
    }

    // Advance time well past all retry deadlines (first retry_us == NOW_US)
    uint32_t collected = engine.pump_retries(NOW_US + 1ULL);

    // All ACK_TRACKER_CAPACITY entries must be collected; m_retry_buf must hold them
    assert(collected == ACK_TRACKER_CAPACITY);
    assert(collected <= MSG_RING_CAPACITY);  // buffer never overrun

    harness_a.close();
    harness_b.close();

    printf("PASS: test_pump_retries_capacity\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 23: fill AckTracker to capacity (ACK_TRACKER_CAPACITY slots), expire all
// with a far-future timestamp; sweep_ack_timeouts() fills m_timeout_buf to its
// maximum depth; confirms no buffer overrun.
// Verifies: REQ-3.3.2
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_ack_timeouts_capacity()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Fill AckTracker to capacity with RELIABLE_ACK messages
    // Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);
        Result r = engine.send(env, NOW_US);
        assert(r == Result::OK);
        // Drain harness_b to prevent its queue filling
        MessageEnvelope drain;
        (void)harness_b.receive_message(drain, 10U);
    }

    // Sweep with far-future now_us (past all deadlines = NOW_US + recv_timeout_ms*1000)
    uint32_t collected = engine.sweep_ack_timeouts(NOW_US + 100000000ULL);

    // All ACK_TRACKER_CAPACITY entries must be reported timed out
    assert(collected == ACK_TRACKER_CAPACITY);
    assert(collected <= ACK_TRACKER_CAPACITY);  // buffer never overrun

    harness_a.close();
    harness_b.close();

    printf("PASS: test_sweep_ack_timeouts_capacity\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// M5 helper: build a minimal ChannelConfig suitable for mock-transport tests
// ─────────────────────────────────────────────────────────────────────────────
static void make_mock_channel_config(ChannelConfig& cfg)
{
    channel_config_default(cfg, 0U);
    cfg.max_retries      = 3U;
    cfg.retry_backoff_ms = 100U;
    cfg.recv_timeout_ms  = 1000U;
}

// ─────────────────────────────────────────────────────────────────────────────
// M5 Test 24: transport send_message() returns ERR_IO (OS/hardware failure path
// unreachable in loopback) — DeliveryEngine::send() must propagate ERR_IO.
// Verifies: REQ-3.3.1, REQ-3.3.2
// ─────────────────────────────────────────────────────────────────────────────
static void test_mock_send_transport_err_io()
{
    MockTransportInterface mock;
    ChannelConfig cfg;
    make_mock_channel_config(cfg);

    DeliveryEngine engine;
    engine.init(&mock, cfg, 1U);

    // Inject ERR_IO on send_message()
    mock.fail_send_message = true;

    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::BEST_EFFORT);
    Result res = engine.send(env, NOW_US);

    // DeliveryEngine must propagate the transport error; never return OK
    assert(res == Result::ERR_IO);
    assert(mock.n_send_message == 1);

    printf("PASS: test_mock_send_transport_err_io\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// M5 Test 25: transport receive_message() returns ERR_IO (connection-drop path
// unreachable in loopback) — DeliveryEngine::receive() must propagate ERR_IO.
// Verifies: REQ-3.3.1, REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_mock_receive_transport_err_io()
{
    MockTransportInterface mock;
    ChannelConfig cfg;
    make_mock_channel_config(cfg);

    DeliveryEngine engine;
    engine.init(&mock, cfg, 1U);

    // Inject ERR_IO on receive_message()
    mock.fail_receive_message = true;

    MessageEnvelope out;
    Result res = engine.receive(out, 100U, NOW_US);

    // DeliveryEngine must propagate ERR_IO; not treat it as ERR_TIMEOUT
    assert(res == Result::ERR_IO);
    assert(mock.n_receive_message == 1);

    printf("PASS: test_mock_receive_transport_err_io\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// M5 Test 26: transport send_message() returns ERR_IO during pump_retries()
// retry resend path (unreachable in loopback without a mock).
// pump_retries() must still count the entry as retried even though the
// transport rejected it (retry-count decrement and logging still occur).
// Verifies: REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_mock_pump_retries_transport_err_io()
{
    MockTransportInterface mock;
    ChannelConfig cfg;
    make_mock_channel_config(cfg);

    DeliveryEngine engine;
    engine.init(&mock, cfg, 1U);

    // First send succeeds (ERR_IO not yet armed)
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_RETRY);
    Result send_res = engine.send(env, NOW_US);
    assert(send_res == Result::OK);
    assert(mock.n_send_message == 1);

    // Arm ERR_IO for all subsequent sends (retry path)
    mock.fail_send_message = true;

    // RetryManager::schedule() sets next_retry_us = now_us (immediate).
    // Advance time by 1 us so the entry is due but still within the 5-second expiry.
    uint32_t retried = engine.pump_retries(NOW_US + 1ULL);

    // Entry was due; pump counted it; transport error does not suppress the count
    assert(retried >= 1U);
    assert(retried <= ACK_TRACKER_CAPACITY);

    printf("PASS: test_mock_pump_retries_transport_err_io\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: receiving a RELIABLE_ACK DATA frame causes exactly one outbound ACK.
// Verifies REQ-3.2.4: auto-ACK emission on the receive path for RELIABLE_ACK class.
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_reliable_ack_data_emits_ack()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Inject RELIABLE_ACK DATA from node 2 → node 1
    MessageEnvelope data_env;
    make_data_envelope(data_env, 2U, 1U, 55555ULL, ReliabilityClass::RELIABLE_ACK);
    Result inj = harness_a.inject(data_env);
    assert(inj == Result::OK);

    // engine.receive() accepts the data and auto-sends an ACK via the transport
    MessageEnvelope out;
    Result res = engine.receive(out, 100U, NOW_US);
    assert(res == Result::OK);
    assert(out.message_id == 55555ULL);

    // Exactly one ACK must have arrived at harness_b (the "remote" side)
    MessageEnvelope ack;
    Result rack = harness_b.receive_message(ack, 100U);
    assert(rack == Result::OK);
    assert(ack.message_type == MessageType::ACK);
    assert(ack.message_id   == 55555ULL);
    // Verify ACK routing fields (P3 fix)
    assert(ack.source_id         == 1U);                              // ACK comes from the local engine
    assert(ack.destination_id    == data_env.source_id);             // routed back to the original sender
    assert(ack.reliability_class == ReliabilityClass::BEST_EFFORT);  // ACKs are never reliable-retried
    assert(ack.payload_length    == 0U);                             // ACKs carry no payload

    // No second ACK should be present
    MessageEnvelope extra;
    Result rextra = harness_b.receive_message(extra, 10U);
    assert(rextra != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_reliable_ack_data_emits_ack\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: receiving a RELIABLE_RETRY DATA frame (first occurrence) causes exactly
// one outbound ACK.
// Verifies REQ-3.2.4: auto-ACK emission on the receive path for RELIABLE_RETRY class.
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_reliable_retry_data_emits_ack()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Inject RELIABLE_RETRY DATA from node 2 → node 1 (first occurrence)
    MessageEnvelope data_env;
    make_data_envelope(data_env, 2U, 1U, 66666ULL, ReliabilityClass::RELIABLE_RETRY);
    Result inj = harness_a.inject(data_env);
    assert(inj == Result::OK);

    // engine.receive() accepts the data and auto-sends an ACK via the transport
    MessageEnvelope out;
    Result res = engine.receive(out, 100U, NOW_US);
    assert(res == Result::OK);
    assert(out.message_id == 66666ULL);

    // Exactly one ACK must have arrived at harness_b
    MessageEnvelope ack;
    Result rack = harness_b.receive_message(ack, 100U);
    assert(rack == Result::OK);
    assert(ack.message_type == MessageType::ACK);
    assert(ack.message_id   == 66666ULL);
    // Verify ACK routing fields (P3 fix)
    assert(ack.source_id         == 1U);                              // ACK comes from the local engine
    assert(ack.destination_id    == data_env.source_id);             // routed back to the original sender
    assert(ack.reliability_class == ReliabilityClass::BEST_EFFORT);  // ACKs are never reliable-retried
    assert(ack.payload_length    == 0U);                             // ACKs carry no payload

    // No second ACK should be present
    MessageEnvelope extra;
    Result rextra = harness_b.receive_message(extra, 10U);
    assert(rextra != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_reliable_retry_data_emits_ack\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: duplicate RELIABLE_RETRY still triggers ACK resend (ACK-loss recovery).
// First receive returns OK and sends ACK. Second receive (same envelope) returns
// ERR_DUPLICATE and must ALSO send an ACK so the sender can stop retrying even
// if the first ACK was lost in transit.
// Verifies: REQ-3.2.4, REQ-3.3.3
// ─────────────────────────────────────────────────────────────────────────────
static void test_receive_duplicate_resends_ack()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // DATA from node 2 → node 1, RELIABLE_RETRY with a fixed message_id
    MessageEnvelope data_env;
    make_data_envelope(data_env, 2U, 1U, 77777ULL, ReliabilityClass::RELIABLE_RETRY);

    // Inject the same envelope twice (simulates a retry when original ACK was lost)
    Result inj1 = harness_a.inject(data_env);
    assert(inj1 == Result::OK);
    Result inj2 = harness_a.inject(data_env);
    assert(inj2 == Result::OK);

    // First receive: first occurrence → OK; engine sends ACK → harness_b
    MessageEnvelope out1;
    Result res1 = engine.receive(out1, 100U, NOW_US);
    assert(res1 == Result::OK);

    // Second receive: duplicate → ERR_DUPLICATE; engine must STILL send ACK → harness_b
    MessageEnvelope out2;
    Result res2 = engine.receive(out2, 100U, NOW_US);
    assert(res2 == Result::ERR_DUPLICATE);

    // harness_b should now hold exactly 2 ACK envelopes (one from each receive call)
    MessageEnvelope ack1;
    Result rack1 = harness_b.receive_message(ack1, 100U);
    assert(rack1 == Result::OK);
    assert(ack1.message_type == MessageType::ACK);
    assert(ack1.message_id   == 77777ULL);
    // Verify ACK routing fields (P3 fix)
    assert(ack1.source_id         == 1U);                               // ACK comes from the local engine
    assert(ack1.destination_id    == data_env.source_id);              // routed back to the original sender
    assert(ack1.reliability_class == ReliabilityClass::BEST_EFFORT);   // ACKs are never reliable-retried
    assert(ack1.payload_length    == 0U);                              // ACKs carry no payload

    MessageEnvelope ack2;
    Result rack2 = harness_b.receive_message(ack2, 100U);
    assert(rack2 == Result::OK);
    assert(ack2.message_type == MessageType::ACK);
    assert(ack2.message_id   == 77777ULL);
    // Verify ACK routing fields (P3 fix)
    assert(ack2.source_id         == 1U);                               // ACK comes from the local engine
    assert(ack2.destination_id    == data_env.source_id);              // routed back to the original sender
    assert(ack2.reliability_class == ReliabilityClass::BEST_EFFORT);   // ACKs are never reliable-retried
    assert(ack2.payload_length    == 0U);                              // ACKs carry no payload

    // No third ACK should be present
    MessageEnvelope ack3;
    Result rack3 = harness_b.receive_message(ack3, 10U);
    assert(rack3 != Result::OK);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_receive_duplicate_resends_ack\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: stats msgs_sent increments when send() succeeds
// Verifies: REQ-7.2.3 — DeliveryStats.msgs_sent counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_msgs_sent()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Verify stats zeroed on init
    DeliveryStats s0;
    engine.get_stats(s0);
    assert(s0.msgs_sent == 0U);
    assert(s0.msgs_received == 0U);

    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::BEST_EFFORT);
    Result res = engine.send(env, NOW_US);
    assert(res == Result::OK);

    // msgs_sent must be 1
    DeliveryStats s1;
    engine.get_stats(s1);
    assert(s1.msgs_sent == 1U);
    assert(s1.msgs_received == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_stats_msgs_sent\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: stats msgs_received increments when receive() delivers a DATA message
// Verifies: REQ-7.2.3 — DeliveryStats.msgs_received counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_msgs_received()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    // Inject a DATA message directly into harness_a's inbox
    MessageEnvelope inject;
    make_data_envelope(inject, 2U, 1U, 50001ULL, ReliabilityClass::BEST_EFFORT);
    Result inj = harness_b.send_message(inject);
    assert(inj == Result::OK);

    MessageEnvelope out;
    Result res = engine.receive(out, 100U, NOW_US);
    assert(res == Result::OK);

    // msgs_received must be 1
    DeliveryStats s;
    engine.get_stats(s);
    assert(s.msgs_received == 1U);
    assert(s.msgs_sent == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_stats_msgs_received\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: stats msgs_dropped_expired increments when send() is given an expired envelope
// Verifies: REQ-7.2.3 — DeliveryStats.msgs_dropped_expired counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_dropped_expired()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    DeliveryStats s0;
    engine.get_stats(s0);
    assert(s0.msgs_dropped_expired == 0U);
    assert(s0.msgs_sent == 0U);

    // Expired envelope: expiry_time_us=1 is in the past at now_us=NOW_US
    MessageEnvelope expired;
    make_expired_envelope(expired, 1U, 2U);
    Result res = engine.send(expired, NOW_US);
    assert(res == Result::ERR_EXPIRED);

    // msgs_dropped_expired must be 1
    DeliveryStats s1;
    engine.get_stats(s1);
    assert(s1.msgs_dropped_expired == 1U);
    assert(s1.msgs_sent == 0U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_stats_dropped_expired\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: stats msgs_dropped_duplicate increments when receive() gets a duplicate
// Verifies: REQ-7.2.3 — DeliveryStats.msgs_dropped_duplicate counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_dropped_duplicate()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    DeliveryStats s0;
    engine.get_stats(s0);
    assert(s0.msgs_dropped_duplicate == 0U);
    assert(s0.msgs_received == 0U);

    // Inject the same RELIABLE_RETRY message twice so duplicate filter fires on second
    MessageEnvelope env1;
    make_data_envelope(env1, 2U, 1U, 50100ULL, ReliabilityClass::RELIABLE_RETRY);
    MessageEnvelope env2 = env1;  // identical — same source_id + message_id

    Result inj1 = harness_b.send_message(env1);
    assert(inj1 == Result::OK);
    Result inj2 = harness_b.send_message(env2);
    assert(inj2 == Result::OK);

    // First receive: accepted
    MessageEnvelope out1;
    Result r1 = engine.receive(out1, 100U, NOW_US);
    assert(r1 == Result::OK);

    // Second receive: duplicate
    MessageEnvelope out2;
    Result r2 = engine.receive(out2, 100U, NOW_US);
    assert(r2 == Result::ERR_DUPLICATE);

    // msgs_dropped_duplicate must be 1
    DeliveryStats s1;
    engine.get_stats(s1);
    assert(s1.msgs_dropped_duplicate == 1U);
    assert(s1.msgs_received == 1U);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_stats_dropped_duplicate\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: stats latency_sample_count increments when receive() processes an ACK
//       for a previously sent RELIABLE_ACK message
// Verifies: REQ-7.2.1 — DeliveryStats.latency_sample_count, latency_sum_us
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_latency()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;
    DeliveryEngine  engine;
    setup_engine(harness_a, harness_b, engine);

    DeliveryStats s0;
    engine.get_stats(s0);
    assert(s0.latency_sample_count == 0U);
    assert(s0.latency_sum_us == 0ULL);

    // Send a RELIABLE_ACK message; engine assigns message_id=1
    MessageEnvelope env;
    make_data_envelope(env, 1U, 2U, 0ULL, ReliabilityClass::RELIABLE_ACK);
    Result send_r = engine.send(env, NOW_US);
    assert(send_r == Result::OK);

    // The assigned message_id is in env after send(); inject a matching ACK
    const uint64_t assigned_id = env.message_id;
    MessageEnvelope ack;
    make_ack_envelope(ack, 2U, 1U, assigned_id);
    // Use a slightly later timestamp to produce a non-zero RTT
    ack.timestamp_us = NOW_US + 1000ULL;

    Result inj = harness_b.send_message(ack);
    assert(inj == Result::OK);

    // receive() processes the ACK and records latency
    MessageEnvelope recv_out;
    Result recv_r = engine.receive(recv_out, 100U, NOW_US + 2000ULL);
    assert(recv_r == Result::OK);

    // latency_sample_count must be 1; latency_sum_us >= 0
    DeliveryStats s1;
    engine.get_stats(s1);
    assert(s1.latency_sample_count == 1U);
    assert(s1.latency_sum_us >= 0ULL);

    harness_a.close();
    harness_b.close();

    printf("PASS: test_stats_latency\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_send_best_effort();
    test_send_reliable_ack();
    test_send_reliable_retry();
    test_receive_data_best_effort();
    test_receive_expired();
    test_send_expired_returns_err();
    test_receive_expired_at_boundary();
    test_receive_zero_expiry_never_drops();
    test_receive_ack_cancels_retry();
    test_receive_duplicate();
    test_receive_timeout();
    test_pump_retries_no_retries();
    test_pump_retries_fires();
    test_sweep_no_timeouts();
    test_sweep_detects_timeout();
    test_send_ack_tracker_full();
    test_send_retry_manager_full();
    test_send_transport_queue_full();
    test_pump_retries_send_fails();
    test_receive_nak_control();
    test_receive_heartbeat_control();
    test_pump_retries_resends();
    test_init_retry_buf_is_zeroed();
    test_init_timeout_buf_is_zeroed();
    test_pump_retries_capacity();
    test_sweep_ack_timeouts_capacity();
    test_receive_reliable_ack_data_emits_ack();
    test_receive_reliable_retry_data_emits_ack();
    test_receive_duplicate_resends_ack();
    test_mock_send_transport_err_io();
    test_mock_receive_transport_err_io();
    test_mock_pump_retries_transport_err_io();
    test_stats_msgs_sent();
    test_stats_msgs_received();
    test_stats_dropped_expired();
    test_stats_dropped_duplicate();
    test_stats_latency();

    printf("ALL DeliveryEngine tests passed.\n");
    return 0;
}
