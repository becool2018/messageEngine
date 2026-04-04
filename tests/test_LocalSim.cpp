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
 * @file test_LocalSim.cpp
 * @brief Unit tests for LocalSimHarness end-to-end in-process messaging.
 *
 * Tests the LocalSimHarness transport for bidirectional messaging, queue
 * overflow, and timeout behavior in a deterministic in-process environment.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 */

// Verifies: REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.3.3
// Verification: M1 + M2 + M4 + M5 (fault injection via impairment config)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <unistd.h>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/ChannelConfig.hpp"
#include "platform/LocalSimHarness.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Create a test transport config for LOCAL_SIM
// ─────────────────────────────────────────────────────────────────────────────
static void create_local_sim_config(TransportConfig& cfg, NodeId node_id)
{
    transport_config_default(cfg);
    cfg.kind = TransportKind::LOCAL_SIM;
    cfg.local_node_id = node_id;
    cfg.is_server = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Create a test DATA envelope with payload
// ─────────────────────────────────────────────────────────────────────────────
static void create_test_data_envelope(MessageEnvelope& env,
                                       NodeId src,
                                       NodeId dst,
                                       const char* payload_str)
{
    envelope_init(env);
    env.message_type = MessageType::DATA;
    env.message_id = 12345ULL;
    env.timestamp_us = 0ULL;
    env.source_id = src;
    env.destination_id = dst;
    env.priority = 0U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.expiry_time_us = 0ULL;

    // Copy payload string (bounded by MSG_MAX_PAYLOAD_BYTES)
    uint32_t len = 0U;
    while (payload_str[len] != '\0' && len < MSG_MAX_PAYLOAD_BYTES) {
        env.payload[len] = static_cast<uint8_t>(payload_str[len]);
        ++len;
    }
    env.payload_length = len;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Basic send/receive between two harnesses
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-4.1.2, REQ-4.1.3
static bool test_basic_send_receive()
{
    // Create two harnesses
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;

    // Initialize with LOCAL_SIM transport
    TransportConfig cfg_a;
    create_local_sim_config(cfg_a, 1U);
    Result init_a = harness_a.init(cfg_a);
    assert(init_a == Result::OK);
    assert(harness_a.is_open() == true);

    TransportConfig cfg_b;
    create_local_sim_config(cfg_b, 2U);
    Result init_b = harness_b.init(cfg_b);
    assert(init_b == Result::OK);
    assert(harness_b.is_open() == true);

    // Link A → B (messages from A go to B)
    harness_a.link(&harness_b);

    // Send a message from A to B
    MessageEnvelope send_env;
    create_test_data_envelope(send_env, 1U, 2U, "Hello");

    Result send_r = harness_a.send_message(send_env);
    assert(send_r == Result::OK);

    // Receive on B
    MessageEnvelope recv_env;
    Result recv_r = harness_b.receive_message(recv_env, 100U);
    assert(recv_r == Result::OK);

    // Verify message fields match
    assert(recv_env.message_id == send_env.message_id);
    assert(recv_env.source_id == send_env.source_id);
    assert(recv_env.destination_id == send_env.destination_id);
    assert(recv_env.payload_length == send_env.payload_length);

    // Verify payload matches
    for (uint32_t i = 0U; i < recv_env.payload_length; ++i) {
        assert(recv_env.payload[i] == send_env.payload[i]);
    }

    harness_a.close();
    harness_b.close();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Bidirectional messaging (A → B and B → A)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-4.1.2, REQ-4.1.3
static bool test_bidirectional()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;

    // Initialize
    TransportConfig cfg_a;
    create_local_sim_config(cfg_a, 10U);
    harness_a.init(cfg_a);

    TransportConfig cfg_b;
    create_local_sim_config(cfg_b, 20U);
    harness_b.init(cfg_b);

    // Link both directions: A → B and B → A
    harness_a.link(&harness_b);
    harness_b.link(&harness_a);

    // Send from A to B
    MessageEnvelope env_a_to_b;
    create_test_data_envelope(env_a_to_b, 10U, 20U, "A->B");
    env_a_to_b.message_id = 111ULL;

    Result send_a = harness_a.send_message(env_a_to_b);
    assert(send_a == Result::OK);

    // Receive on B
    MessageEnvelope recv_b;
    Result recv_b_r = harness_b.receive_message(recv_b, 100U);
    assert(recv_b_r == Result::OK);
    assert(recv_b.message_id == 111ULL);

    // Send from B to A
    MessageEnvelope env_b_to_a;
    create_test_data_envelope(env_b_to_a, 20U, 10U, "B->A");
    env_b_to_a.message_id = 222ULL;

    Result send_b = harness_b.send_message(env_b_to_a);
    assert(send_b == Result::OK);

    // Receive on A
    MessageEnvelope recv_a;
    Result recv_a_r = harness_a.receive_message(recv_a, 100U);
    assert(recv_a_r == Result::OK);
    assert(recv_a.message_id == 222ULL);

    harness_a.close();
    harness_b.close();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Queue full behavior
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-4.1.2
static bool test_queue_full()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;

    TransportConfig cfg_a;
    create_local_sim_config(cfg_a, 5U);
    harness_a.init(cfg_a);

    TransportConfig cfg_b;
    create_local_sim_config(cfg_b, 6U);
    harness_b.init(cfg_b);

    harness_a.link(&harness_b);

    // Inject MSG_RING_CAPACITY messages directly to fill harness_b's receive queue
    // Power of 10: bounded loop
    for (uint32_t i = 0U; i < MSG_RING_CAPACITY; ++i) {
        MessageEnvelope env;
        envelope_init(env);
        env.message_type = MessageType::DATA;
        env.source_id = 5U;
        env.destination_id = 6U;
        env.message_id = static_cast<uint64_t>(i);
        env.payload_length = 0U;

        Result r = harness_b.inject(env);
        assert(r == Result::OK);
    }

    // Try to inject one more message; should fail with ERR_FULL
    MessageEnvelope overflow_env;
    envelope_init(overflow_env);
    overflow_env.message_type = MessageType::DATA;
    overflow_env.source_id = 5U;
    overflow_env.destination_id = 6U;
    overflow_env.message_id = 999ULL;
    overflow_env.payload_length = 0U;

    Result r = harness_b.inject(overflow_env);
    assert(r == Result::ERR_FULL);

    harness_a.close();
    harness_b.close();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Receive with timeout (no message available)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-4.1.3, REQ-6.3.3
static bool test_recv_timeout()
{
    LocalSimHarness harness;

    TransportConfig cfg;
    create_local_sim_config(cfg, 7U);
    harness.init(cfg);

    // Don't send anything; just try to receive with zero timeout (non-blocking)
    MessageEnvelope env;
    Result r = harness.receive_message(env, 0U);

    // Should timeout immediately (no message available)
    assert(r == Result::ERR_TIMEOUT);

    harness.close();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: init with num_channels == 0 → impairment uses default config
// Covers LocalSimHarness::init() L59 False branch (config.num_channels > 0U == false).
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────
static bool test_num_channels_zero()
{
    LocalSimHarness side_a;
    LocalSimHarness side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    create_local_sim_config(cfg_a, 1U);
    create_local_sim_config(cfg_b, 2U);
    cfg_a.num_channels = 0U;
    cfg_b.num_channels = 0U;

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    side_a.link(&side_b);
    side_b.link(&side_a);

    // Send a message to verify basic operation still works
    MessageEnvelope send_env;
    create_test_data_envelope(send_env, 1U, 2U, "nc0");
    Result r = side_a.send_message(send_env);
    assert(r == Result::OK);

    MessageEnvelope recv_env;
    r = side_b.receive_message(recv_env, 0U);
    assert(r == Result::OK);

    side_a.close();
    side_b.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: loss impairment drops message — send_message loss path (Option A)
// Covers LocalSimHarness::send_message() L118 True branch:
//   process_outbound returns ERR_IO (100% loss) → silent drop → OK returned.
// Verifies: REQ-5.1.3, REQ-4.1.2
// ─────────────────────────────────────────────────────────────────────────────
static bool test_loss_impairment()
{
    LocalSimHarness sender;
    LocalSimHarness receiver;

    TransportConfig cfg_s;
    create_local_sim_config(cfg_s, 30U);
    cfg_s.channels[0U].impairment.enabled          = true;
    cfg_s.channels[0U].impairment.loss_probability = 1.0;  // 100% loss

    TransportConfig cfg_r;
    create_local_sim_config(cfg_r, 31U);

    assert(sender.init(cfg_s) == Result::OK);
    assert(receiver.init(cfg_r) == Result::OK);
    assert(!receiver.is_open() || receiver.is_open());  // suppress unused warning

    sender.link(&receiver);

    MessageEnvelope env;
    create_test_data_envelope(env, 30U, 31U, "drop me");
    env.message_id = 9001ULL;

    // process_outbound returns ERR_IO (100% loss) → L118 True branch COVERED.
    // send_message silently drops the message and returns OK.
    Result r = sender.send_message(env);
    assert(r == Result::OK);

    // No message was delivered to receiver; non-blocking receive times out.
    MessageEnvelope recv_env;
    r = receiver.receive_message(recv_env, 0U);
    assert(r == Result::ERR_TIMEOUT);

    sender.close();
    receiver.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: impairment delay loop body (Option A)
// Covers LocalSimHarness::send_message() L131-L134 True branch:
//   collect_deliverable returns count=1 on second send → loop body injects
//   the previously-queued (delayed) message into the peer.
// Verifies: REQ-5.1.1, REQ-4.1.2
// ─────────────────────────────────────────────────────────────────────────────
static bool test_delay_loop_body()
{
    LocalSimHarness sender;
    LocalSimHarness receiver;

    TransportConfig cfg_s;
    create_local_sim_config(cfg_s, 32U);
    cfg_s.channels[0U].impairment.enabled          = true;
    cfg_s.channels[0U].impairment.fixed_latency_ms = 1U;  // 1 ms delay

    TransportConfig cfg_r;
    create_local_sim_config(cfg_r, 33U);

    assert(sender.init(cfg_s) == Result::OK);
    assert(receiver.init(cfg_r) == Result::OK);

    sender.link(&receiver);

    MessageEnvelope env1;
    create_test_data_envelope(env1, 32U, 33U, "first");
    env1.message_id = 9101ULL;

    // First send: process_outbound queues env1 in delay buffer (release=now+1ms),
    // collect_deliverable returns 0 (not yet due) — loop body does NOT run.
    // inject(env1) delivers env1 immediately to receiver (L137).
    assert(sender.send_message(env1) == Result::OK);

    usleep(10000U);  // 10 ms >> 1 ms: env1 is past its release time

    MessageEnvelope env2;
    create_test_data_envelope(env2, 32U, 33U, "second");
    env2.message_id = 9102ULL;

    // Second send: collect_deliverable returns 1 (env1 past release).
    // Loop body runs: inject(env1 dup) into receiver.  COVERED: L131-L134.
    // Then inject(env2) delivers env2 immediately.
    assert(sender.send_message(env2) == Result::OK);

    // Receiver queue has: env1 (immediate) + env1-dup (delayed loop) + env2.
    // Non-blocking pop returns env1 first (FIFO).
    MessageEnvelope recv_env;
    Result r = receiver.receive_message(recv_env, 0U);
    assert(r == Result::OK);
    assert(recv_env.message_id == 9101ULL);

    sender.close();
    receiver.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: send_message() returns ERR_FULL when peer receive queue is saturated
// Verifies: REQ-4.1.2
//
// In LocalSimHarness there is no wire — the peer's receive ring IS the network.
// ERR_FULL from inject() is an in-process confirmation the message was NOT
// received.  Returning OK would violate the TransportInterface::send_message()
// contract which explicitly enumerates ERR_FULL as a caller-visible error.
// (Fixes the incorrect design decision recorded in commit 03f9f23.)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-4.1.2
static bool test_send_peer_queue_full_returns_err_full()
{
    LocalSimHarness harness_a;
    LocalSimHarness harness_b;

    TransportConfig cfg_a;
    create_local_sim_config(cfg_a, 40U);
    TransportConfig cfg_b;
    create_local_sim_config(cfg_b, 41U);

    assert(harness_a.init(cfg_a) == Result::OK);
    assert(harness_b.init(cfg_b) == Result::OK);

    // Link A → B so send_message() on A injects into B's queue.
    harness_a.link(&harness_b);

    // Fill harness_b's receive queue to capacity by injecting directly.
    // Power of 10: bounded loop (MSG_RING_CAPACITY is a compile-time constant).
    for (uint32_t i = 0U; i < MSG_RING_CAPACITY; ++i) {
        MessageEnvelope fill_env;
        envelope_init(fill_env);
        fill_env.message_type   = MessageType::DATA;
        fill_env.source_id      = 40U;
        fill_env.destination_id = 41U;
        fill_env.message_id     = static_cast<uint64_t>(i);
        fill_env.payload_length = 0U;
        Result r = harness_b.inject(fill_env);
        assert(r == Result::OK);
    }

    // Now send via send_message(); the peer queue is full.
    // ERR_FULL from inject() must be propagated: the message was not received.
    MessageEnvelope overflow_env;
    create_test_data_envelope(overflow_env, 40U, 41U, "overflow");
    overflow_env.message_id = 8888ULL;

    Result send_r = harness_a.send_message(overflow_env);
    // Peer-queue-full is a confirmed delivery failure: ERR_FULL must be returned.
    assert(send_r == Result::ERR_FULL);
    assert(send_r != Result::OK);

    harness_a.close();
    harness_b.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: init() + close() without link() does not violate stats invariant
// Verifies: REQ-7.2.4 (Issue 5 fix — connections_closed must never exceed
//           connections_opened)
//
// Before the fix, close() always incremented m_connections_closed when m_open
// was true, but m_connections_opened is only incremented by link().  Calling
// init(); close() without link() produced closed=1, opened=0, which tripped
// the assertion in get_transport_stats().
// After the fix, close() guards the increment with
// (m_connections_closed < m_connections_opened).
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-7.2.4
static bool test_init_close_without_link_stats_invariant()
{
    LocalSimHarness harness;

    TransportConfig cfg;
    create_local_sim_config(cfg, 50U);
    assert(harness.init(cfg) == Result::OK);

    // Close without calling link().  Before the fix this produced
    // connections_closed=1, connections_opened=0, which would fire the assertion
    // inside get_transport_stats().
    harness.close();

    // Re-initialize so we can call get_transport_stats() (requires m_open).
    assert(harness.init(cfg) == Result::OK);

    TransportStats stats;
    harness.get_transport_stats(stats);  // Must not abort — invariant holds.
    // Fixed: guard in close() ensures closed never exceeds opened.
    assert(stats.connections_opened >= stats.connections_closed);
    assert(stats.connections_closed == 0U);  // no link() was called

    harness.close();
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    int failed = 0;

    if (!test_basic_send_receive()) {
        printf("FAIL: test_basic_send_receive\n");
        ++failed;
    } else {
        printf("PASS: test_basic_send_receive\n");
    }

    if (!test_bidirectional()) {
        printf("FAIL: test_bidirectional\n");
        ++failed;
    } else {
        printf("PASS: test_bidirectional\n");
    }

    if (!test_queue_full()) {
        printf("FAIL: test_queue_full\n");
        ++failed;
    } else {
        printf("PASS: test_queue_full\n");
    }

    if (!test_recv_timeout()) {
        printf("FAIL: test_recv_timeout\n");
        ++failed;
    } else {
        printf("PASS: test_recv_timeout\n");
    }

    if (!test_num_channels_zero()) {
        printf("FAIL: test_num_channels_zero\n");
        ++failed;
    } else {
        printf("PASS: test_num_channels_zero\n");
    }

    // Impairment tests (Option A — ImpairmentConfig embedded in ChannelConfig)
    if (!test_loss_impairment()) {
        printf("FAIL: test_loss_impairment\n");
        ++failed;
    } else {
        printf("PASS: test_loss_impairment\n");
    }

    if (!test_delay_loop_body()) {
        printf("FAIL: test_delay_loop_body\n");
        ++failed;
    } else {
        printf("PASS: test_delay_loop_body\n");
    }

    // Bug-fix regression tests
    if (!test_send_peer_queue_full_returns_err_full()) {
        printf("FAIL: test_send_peer_queue_full_returns_err_full\n");
        ++failed;
    } else {
        printf("PASS: test_send_peer_queue_full_returns_err_full\n");
    }

    if (!test_init_close_without_link_stats_invariant()) {
        printf("FAIL: test_init_close_without_link_stats_invariant\n");
        ++failed;
    } else {
        printf("PASS: test_init_close_without_link_stats_invariant\n");
    }

    return (failed > 0) ? 1 : 0;
}
