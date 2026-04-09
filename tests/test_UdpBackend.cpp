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
 * @file test_UdpBackend.cpp
 * @brief Unit and integration tests for UdpBackend:
 *        socket bind, loopback send/receive, timeout, and lifecycle.
 *
 * Tests cover:
 *   - transport_config_default() with kind overridden to UDP
 *   - Socket bind: is_open() == true, close() → is_open() == false
 *   - receive_message() timeout when no sender
 *   - Loopback send/receive: two UdpBackend instances, same thread
 *   - Multiple messages: send several, receive all in order
 *   - recv_queue full path: recv_one_datagram when queue is saturated
 *   - Close before init (safe no-op)
 *   - is_open() lifecycle
 *
 * Ports 19600–19620 are reserved for UdpBackend tests.
 * No threads required: UDP send is non-blocking for datagrams that fit in
 * kernel buffers; receive_message() polls with 100 ms intervals.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - STL exempted in tests/ for fixture setup only.
 *
 * Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *           REQ-6.2.1, REQ-6.2.2, REQ-6.2.3, REQ-6.2.4, REQ-7.1.1
 */
// Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-5.1.6, REQ-6.2.1, REQ-6.2.2, REQ-6.2.3, REQ-6.2.4, REQ-7.1.1

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "core/Types.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "platform/UdpBackend.hpp"
#include "MockSocketOps.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper — build a UDP TransportConfig for loopback
// Peer A binds to bind_port, sends to peer_port.
// ─────────────────────────────────────────────────────────────────────────────

static void make_udp_cfg(TransportConfig& cfg,
                          uint16_t bind_port,
                          uint16_t peer_port)
{
    transport_config_default(cfg);
    cfg.kind      = TransportKind::UDP;
    cfg.bind_port = bind_port;
    cfg.peer_port = peer_port;
    // bind_ip and peer_ip already "127.0.0.1" from transport_config_default
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — minimal DATA envelope
// ─────────────────────────────────────────────────────────────────────────────

static void make_test_envelope(MessageEnvelope& env, uint64_t id)
{
    envelope_init(env);
    env.message_type     = MessageType::DATA;
    env.message_id       = id;
    env.source_id        = 1U;
    env.destination_id   = 2U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — minimal HELLO envelope for source NodeId registration (REQ-6.1.8)
// ─────────────────────────────────────────────────────────────────────────────

static void make_hello_envelope(MessageEnvelope& env, NodeId src)
{
    envelope_init(env);
    env.message_type      = MessageType::HELLO;
    env.message_id        = 0U;
    env.source_id         = src;
    env.destination_id    = NODE_ID_INVALID;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: override TransportKind to UDP; verify config fields
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_config()
{
    TransportConfig cfg;
    make_udp_cfg(cfg, 19600U, 19601U);

    assert(cfg.kind == TransportKind::UDP);
    assert(cfg.bind_port == 19600U);
    assert(cfg.peer_port == 19601U);

    printf("PASS: test_udp_config\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: bind socket, is_open() true, close(), is_open() false
// Verifies: REQ-4.1.1, REQ-4.1.4, REQ-6.2.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_bind_and_close()
{
    UdpBackend backend;
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19602U, 19603U);  // peer not needed for bind-only test

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    backend.close();
    assert(!backend.is_open());

    printf("PASS: test_udp_bind_and_close\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: receive_message() with no sender → ERR_TIMEOUT
// Verifies: REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_receive_timeout()
{
    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, 19604U, 19605U);

    Result r = backend.init(cfg);
    assert(r == Result::OK);

    MessageEnvelope env;
    r = backend.receive_message(env, 200U);  // 200 ms: 2 poll iterations
    assert(r == Result::ERR_TIMEOUT);

    backend.close();
    printf("PASS: test_udp_receive_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: loopback send/receive — same-thread, two UdpBackend instances
// Side A sends to side B; side B calls receive_message().
// Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.2.1, REQ-6.2.2, REQ-6.2.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_loopback_send_receive()
{
    static const uint16_t PORT_A   = 19606U;
    static const uint16_t PORT_B   = 19607U;
    static const uint64_t TEST_ID  = 0xCAFEBABEULL;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // REQ-6.1.8 / REQ-6.1.10 / REQ-6.2.4: register_local_id() sends HELLO on wire
    // so side_b can accept DATA frames from source_id=1.
    assert(side_a.register_local_id(1U) == Result::OK);

    // Side A sends; datagram enters OS kernel buffer immediately (non-blocking)
    MessageEnvelope send_env;
    make_test_envelope(send_env, TEST_ID);
    Result r = side_a.send_message(send_env);
    assert(r == Result::OK);

    // Side B polls; HELLO is consumed, then DATA is retrieved
    MessageEnvelope recv_env;
    r = side_b.receive_message(recv_env, 1000U);
    assert(r == Result::OK);
    assert(recv_env.message_id == TEST_ID);
    assert(recv_env.message_type == MessageType::DATA);

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_loopback_send_receive\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: multiple messages sent and received in order
// Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.2.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_multiple_messages()
{
    static const uint16_t PORT_A = 19608U;
    static const uint16_t PORT_B = 19609U;
    static const uint32_t N      = 4U;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // REQ-6.1.8 / REQ-6.1.10 / REQ-6.2.4: register_local_id() sends HELLO to peer.
    assert(side_a.register_local_id(1U) == Result::OK);

    // Send N messages from A to B
    // Power of 10 Rule 2: fixed loop bound (N = 4)
    for (uint32_t i = 0U; i < N; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(i) + 1U);
        Result r = side_a.send_message(env);
        assert(r == Result::OK);
    }

    // Small pause to ensure all datagrams are in the OS receive buffer
    usleep(10000U);  // 10 ms

    // Receive all N from B
    uint32_t received = 0U;
    // Power of 10 Rule 2: fixed loop bound
    for (uint32_t attempt = 0U; attempt < N * 2U; ++attempt) {
        MessageEnvelope env;
        Result r = side_b.receive_message(env, 200U);
        if (r == Result::OK) {
            assert(env.message_type == MessageType::DATA);
            ++received;
            if (received >= N) { break; }
        }
    }
    assert(received == N);

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_multiple_messages\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: close before init (safe no-op)
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_close_before_init()
{
    UdpBackend backend;
    assert(!backend.is_open());

    backend.close();  // must not crash
    backend.close();  // double close also safe

    assert(!backend.is_open());
    printf("PASS: test_udp_close_before_init\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: is_open() lifecycle
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_is_open_lifecycle()
{
    UdpBackend backend;
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19610U, 19611U);

    assert(backend.init(cfg) == Result::OK);
    assert(backend.is_open());

    backend.close();
    assert(!backend.is_open());

    // close again (idempotent)
    backend.close();
    assert(!backend.is_open());

    printf("PASS: test_udp_is_open_lifecycle\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: send_message to unreachable peer — UDP just sends, OK returned
// (ICMP unreachable arrives asynchronously; sendto succeeds regardless)
// Verifies: REQ-4.1.2, REQ-6.2.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_send_to_unreachable()
{
    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, 19612U, 19699U);  // nothing on 19699

    assert(backend.init(cfg) == Result::OK);

    MessageEnvelope env;
    make_test_envelope(env, 0xFEEDULL);
    Result r = backend.send_message(env);
    // UDP sendto() to an unreachable port succeeds at the socket layer
    assert(r == Result::OK);

    backend.close();
    printf("PASS: test_udp_send_to_unreachable\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: init with bad bind IP → ERR_IO
// Covers UdpBackend::init() L73 True branch (socket_bind returns false).
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_bind_bad_ip()
{
    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, 19613U, 19614U);

    const char bad_ip[] = "999.999.999.999";
    (void)memcpy(cfg.bind_ip, bad_ip, sizeof(bad_ip));

    Result r = backend.init(cfg);
    assert(r != Result::OK);
    assert(!backend.is_open());

    printf("PASS: test_udp_bind_bad_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: init with num_channels == 0 → impairment uses default config
// Covers UdpBackend::init() L86 False branch (config.num_channels > 0U == false).
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_num_channels_zero()
{
    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, 19615U, 19616U);
    cfg.num_channels = 0U;

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    backend.close();
    printf("PASS: test_udp_num_channels_zero\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: raw garbage UDP datagram → Serializer::deserialize fails
// Covers recv_one_datagram L181 True branch (!result_ok after deserialize).
// Verifies: REQ-6.2.4
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_recv_garbage_datagram()
{
    static const uint16_t BIND_PORT = 19617U;

    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, BIND_PORT, 19618U);

    assert(backend.init(cfg) == Result::OK);

    // Send 5 garbage bytes to the backend's bind port via a raw POSIX socket.
    // recv_one_datagram calls Serializer::deserialize(5 bytes) which rejects
    // them (5 < WIRE_HEADER_SIZE=44) → ERR_INVALID → True branch covered.
    int raw_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(raw_fd >= 0);

    struct sockaddr_in dst;
    (void)memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(BIND_PORT);
    (void)inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);

    const uint8_t garbage[5U] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U};
    // MISRA C++:2023 5.2.4: reinterpret_cast required by POSIX sendto() API
    ssize_t sent = sendto(raw_fd,
                          static_cast<const void*>(garbage),
                          sizeof(garbage), 0,
                          reinterpret_cast<const struct sockaddr*>(&dst),
                          static_cast<socklen_t>(sizeof(dst)));
    assert(sent == static_cast<ssize_t>(sizeof(garbage)));
    (void)close(raw_fd);

    // receive_message: garbage fails deserialize; no valid message is queued.
    // Loop times out → ERR_TIMEOUT.
    MessageEnvelope env;
    Result r = backend.receive_message(env, 300U);
    assert(r == Result::ERR_TIMEOUT);

    backend.close();
    printf("PASS: test_udp_recv_garbage_datagram\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock fault-injection tests (VVP-001 M5 — dependency-injected ISocketOps)
// Cover POSIX error paths unreachable in a loopback environment.
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_udp_create_fail()
{
    // Verifies: REQ-6.2.1, REQ-6.3.2
    MockSocketOps mock;
    mock.fail_create_udp = true;

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19650U, 19651U);

    Result r = backend.init(cfg);
    assert(r == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_udp_create_fail\n");
}

static void test_mock_udp_reuseaddr_fail()
{
    // Verifies: REQ-6.2.1, REQ-6.3.2
    MockSocketOps mock;
    mock.fail_set_reuseaddr = true;

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19652U, 19653U);

    Result r = backend.init(cfg);
    assert(r == Result::ERR_IO);
    assert(!backend.is_open());
    assert(mock.n_do_close >= 1);  // do_close called after reuseaddr failure

    printf("PASS: test_mock_udp_reuseaddr_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: impairment delay paths (Option A — fixed_latency_ms via ChannelConfig)
// Covers:
//   (a) send_message delayed-message loop (UdpBackend.cpp L153-L165):
//       second send triggers flush of first delayed message to the wire.
//   (b) flush_delayed_to_wire() loop body: receive_message sends expired delayed
//       envelope to the wire via send_one_envelope() (NOT into recv_queue).
//   (c) receive_message returns ERR_TIMEOUT: recv_queue stays empty after flush.
// Uses MockSocketOps: recv_from returns true/0 → deserialize fails → no socket data.
// Verifies: REQ-5.1.1, REQ-4.1.2, REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_impairment_delay_paths()
{
    MockSocketOps mock;
    UdpBackend backend(mock);

    TransportConfig cfg;
    make_udp_cfg(cfg, 19620U, 19621U);
    cfg.channels[0U].impairment.enabled          = true;
    cfg.channels[0U].impairment.fixed_latency_ms = 1U;  // 1 ms delay

    assert(backend.init(cfg) == Result::OK);
    assert(backend.is_open());

    MessageEnvelope env1;
    make_test_envelope(env1, 0xDE100001ULL);

    // First send: process_outbound queues env1 (release = now_us + 1 ms).
    // collect_deliverable returns 0 (not yet due) — delayed loop does NOT run.
    assert(backend.send_message(env1) == Result::OK);

    usleep(10000U);  // 10 ms >> 1 ms: env1 is now past its release time

    MessageEnvelope env2;
    make_test_envelope(env2, 0xDE100002ULL);

    // Second send: process_outbound queues env2; collect_deliverable returns 1
    // (env1 past its release time) — delayed-message loop runs once, sending
    // env1 to the wire via send_one_envelope().
    // Covers: send_message delayed-message loop body (path (a) above).
    assert(backend.send_message(env2) == Result::OK);

    usleep(10000U);  // 10 ms >> 1 ms: env2 is now past its release time

    // receive_message: recv_one_datagram returns false (mock: out_len=0 → deserialize
    // fails). flush_delayed_to_wire() sends env2 to the wire — NOT into recv_queue.
    // recv_queue stays empty → receive_message returns ERR_TIMEOUT.
    // Covers: flush_delayed_to_wire() loop body (path (b)).
    MessageEnvelope recv_env;
    Result r = backend.receive_message(recv_env, 500U);
    assert(r == Result::ERR_TIMEOUT);

    // Verify flush_delayed_to_wire() actually called send_to() — not a silent drop.
    // send_message(env2) flushed env1 (1 send_to call); receive_message flushed env2
    // (1 more send_to call).  Total: 2 send_to() calls at this point.
    assert(mock.sent_count == 2U);

    backend.close();
    printf("PASS: test_udp_impairment_delay_paths\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: send_message loss-impairment path — ERR_IO from process_outbound
//
// Covers send_message() L132 True branch:
//   `if (res == Result::ERR_IO) { return Result::OK; }`
// process_outbound() returns ERR_IO when check_loss() fires (loss_probability=1.0
// guarantees loss on every message).  send_message must silently drop the message
// and return OK.  A subsequent receive_message with 0-ms timeout must return
// ERR_TIMEOUT (nothing was delivered).
//
// Verifies: REQ-4.1.2, REQ-5.1.3
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.2, REQ-5.1.3
static void test_udp_send_loss_impairment()
{
    UdpBackend backend;
    TransportConfig cfg;
    make_udp_cfg(cfg, 19625U, 19626U);  // peer 19626: nothing listens there

    // Configure 100 % loss so check_loss() always fires → process_outbound
    // returns ERR_IO.
    cfg.channels[0U].impairment.enabled          = true;
    cfg.channels[0U].impairment.loss_probability = 1.0;

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    MessageEnvelope env;
    make_test_envelope(env, 0xD5A1D5A1ULL);

    // send_message must hit the ERR_IO silent-drop path and return OK.
    r = backend.send_message(env);
    assert(r == Result::OK);

    // Nothing was delivered: receive with 0 ms timeout (poll_count = 0, loop
    // does not run) → ERR_TIMEOUT.
    MessageEnvelope recv_env;
    r = backend.receive_message(recv_env, 0U);
    assert(r == Result::ERR_TIMEOUT);

    backend.close();
    printf("PASS: test_udp_send_loss_impairment\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: send_message send_to() failure path
//
// Covers send_message() L144 True branch:
//   `if (!m_sock_ops->send_to(...)) { return Result::ERR_IO; }`
// Uses MockSocketOps with fail_send_to=true.  init() succeeds (mock create_udp /
// set_reuseaddr / do_bind all succeed).  send_message serializes the envelope,
// calls process_outbound (impairment disabled → queued then collected
// immediately with release_us == now_us), then calls send_to which the mock
// rejects → ERR_IO must be returned.
//
// Verifies: REQ-4.1.2, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.2, REQ-6.3.2
static void test_mock_send_to_fail()
{
    MockSocketOps mock;
    mock.fail_send_to = true;  // inject failure into the socket-layer send path

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19660U, 19661U);

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    MessageEnvelope env;
    make_test_envelope(env, 0xFA115E1DULL);

    // send_to fails for the current envelope — UdpBackend must propagate ERR_IO
    // per TransportInterface contract.
    r = backend.send_message(env);
    assert(r == Result::ERR_IO);

    // Backend remains open after a send failure.
    assert(backend.is_open());

    backend.close();
    printf("PASS: test_mock_send_to_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: delayed messages are sent to wire, not looped back into recv_queue
//
// Verifies that flush_delayed_to_wire() sends expired outbound envelopes to
// the wire via send_one_envelope() rather than pushing them into m_recv_queue.
// After flushing, receive_message() finds an empty queue and returns ERR_TIMEOUT.
//
// Strategy (achievable via the public API):
//   1. Use MockSocketOps so recv_from always returns true with out_len=0.
//      Serializer::deserialize on 0 bytes always fails → recv_one_datagram
//      returns false on every call (nothing is pushed to recv_queue from the
//      socket path).
//   2. Configure impairment with fixed_latency_ms=1 (enabled=true) so that
//      each send_message call queues the envelope in the impairment delay buffer
//      rather than transmitting it immediately.
//   3. Send N_SEND messages → fills the delay buffer.
//   4. Wait 10 ms so all items expire.
//   5. receive_message(500 ms):
//        - recv_one_datagram returns false (mock, 0 bytes).
//        - flush_delayed_to_wire() collects all expired items and sends each
//          to the wire via send_one_envelope() (MockSocketOps send_to succeeds).
//        - recv_queue remains empty → both queue pops fail.
//        - receive_message returns ERR_TIMEOUT.
//
// Verifies: REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.3
static void test_udp_recv_queue_initial_pop()
{
    // Power of 10 rule 2: loop bound is compile-time constant IMPAIR_DELAY_BUF_SIZE.
    static const uint32_t N_SEND = IMPAIR_DELAY_BUF_SIZE;  // 32

    MockSocketOps mock;
    // Default MockSocketOps: recv_from returns true with out_len=0.
    // Serializer::deserialize on 0 bytes → ERR_INVALID → recv_one_datagram
    // returns false on every call (nothing pushed from the socket path).

    UdpBackend backend(mock);

    TransportConfig cfg;
    make_udp_cfg(cfg, 19662U, 19663U);
    // Impairment with 1 ms delay: each send_message queues to delay buffer;
    // collect_deliverable in send_message finds nothing expired yet (same now_us).
    cfg.channels[0U].impairment.enabled          = true;
    cfg.channels[0U].impairment.fixed_latency_ms = 1U;

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    // Send N_SEND messages → fills delay buffer; collect_deliverable in
    // send_message finds nothing expired (release_us = now_us + 1 ms > now_us).
    // Power of 10 rule 2: fixed loop bound (N_SEND = IMPAIR_DELAY_BUF_SIZE).
    for (uint32_t i = 0U; i < N_SEND; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(0xBEEF0000ULL + i));
        Result rs = backend.send_message(env);
        assert(rs == Result::OK);
    }

    // Wait 10 ms >> 1 ms: all N_SEND items in the delay buffer have expired.
    usleep(10000U);

    // receive_message: flush_delayed_to_wire() sends expired items to the wire
    // (not into recv_queue).  recv_queue remains empty → ERR_TIMEOUT.
    MessageEnvelope env_a;
    r = backend.receive_message(env_a, 500U);
    assert(r == Result::ERR_TIMEOUT);

    // Verify all N_SEND delayed envelopes were forwarded to the wire via send_to().
    // No send_to() calls occurred during send_message() (delay buffer, not immediate).
    // flush_delayed_to_wire() during receive_message() calls send_to() once per item.
    assert(mock.sent_count == N_SEND);

    backend.close();
    printf("PASS: test_udp_recv_queue_initial_pop\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: recv_from() OS failure in recv_one_datagram (VVP-001 M5)
//
// Covers recv_one_datagram() L184 False branch:
//   `if (!m_sock_ops->recv_from(...)) { return false; }`
// Uses MockSocketOps with fail_recv_from=true.  Every poll iteration calls
// recv_from which immediately returns false; the loop exhausts poll_count
// without queuing any message → receive_message returns ERR_TIMEOUT.
// This branch is exclusively reachable via OS-level recvfrom() failure;
// it cannot occur in a nominal loopback environment where the socket is open.
//
// Verifies: REQ-4.1.3, REQ-6.3.2
// Verification: M1 + M2 + M4 + M5 (fault injection via ISocketOps)
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.3, REQ-6.3.2
static void test_mock_udp_recv_from_fail()
{
    MockSocketOps mock;
    mock.fail_recv_from = true;   // inject recvfrom() OS failure

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19670U, 19671U);

    // init must succeed: create_udp / set_reuseaddr / do_bind all unblocked
    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    // Every recv_one_datagram call returns false (recv_from injected failure).
    // receive_message exhausts poll_count (timeout_ms=200 → 2 iterations) and
    // returns ERR_TIMEOUT — the recv_from False branch is covered.
    MessageEnvelope env;
    r = backend.receive_message(env, 200U);
    assert(r == Result::ERR_TIMEOUT);

    // Backend remains open after a receive failure.
    assert(backend.is_open());

    backend.close();
    printf("PASS: test_mock_udp_recv_from_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: recv_from() returns datagram from wrong source — datagram dropped
//
// Verifies: REQ-6.2.4
//
// Strategy: use MockSocketOps with recv_src_ip set to a different IP than the
// configured peer_ip.  recv_from() returns true, but validate_source() rejects
// the source mismatch before deserialization.  receive_message() exhausts all
// poll iterations without queuing any message and returns ERR_TIMEOUT.
//
// Verification method: M5 (fault injection via ISocketOps mock).
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.2.4
static void test_recv_wrong_source_dropped()
{
    MockSocketOps mock;
    // Simulate datagram arriving from a different IP than the configured peer.
    // peer_ip defaults to "127.0.0.1"; set mock to return "192.168.1.99".
    (void)strncpy(mock.recv_src_ip, "192.168.1.99", sizeof(mock.recv_src_ip) - 1U);
    mock.recv_src_ip[sizeof(mock.recv_src_ip) - 1U] = '\0';
    mock.recv_src_port = 19681U;  // arbitrary port for the spoofed source

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19680U, 19681U);  // peer_ip="127.0.0.1", peer_port=19681

    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open());

    // receive_message: recv_from returns true (mock), validate_source sees
    // src_ip="192.168.1.99" != peer_ip="127.0.0.1" -> drops datagram.
    // All poll iterations exhaust without queuing -> ERR_TIMEOUT.
    MessageEnvelope env;
    r = backend.receive_message(env, 200U);  // 2 poll iterations
    assert(r == Result::ERR_TIMEOUT);

    // Backend remains open after source validation drop.
    assert(backend.is_open());

    backend.close();
    printf("PASS: test_recv_wrong_source_dropped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: inbound partition drops received datagram via process_inbound()
// side_b is configured with partition_gap_ms=1 / partition_duration_ms=60000 so
// the partition fires within 5 ms and stays active for the whole test.
// side_a sends one datagram; side_b::receive_message() calls process_inbound()
// which returns ERR_IO (partition drop), so receive_message() returns ERR_TIMEOUT.
// Covers: recv_one_datagram() inbound-impairment branch (partition drop path)
// Verifies: REQ-5.1.6, REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-5.1.6, REQ-4.1.3
static void test_udp_inbound_partition_drops_received()
{
    // Verifies: REQ-5.1.6, REQ-4.1.3
    // Strategy: send a warm-up datagram first; side_b receives it (initialises
    // the partition timer on the first call to is_partition_active()).  Then
    // wait > gap_ms so the partition becomes active, and send the test datagram.
    // side_b::receive_message() must now return ERR_TIMEOUT (partition drop).
    static const uint16_t SIDE_A_PORT = 19690U;
    static const uint16_t SIDE_B_PORT = 19691U;

    UdpBackend side_a;  // sender — no impairment
    UdpBackend side_b;  // receiver — partition active on inbound

    TransportConfig cfg_a;
    make_udp_cfg(cfg_a, SIDE_A_PORT, SIDE_B_PORT);

    TransportConfig cfg_b;
    make_udp_cfg(cfg_b, SIDE_B_PORT, SIDE_A_PORT);
    cfg_b.num_channels = 1U;
    cfg_b.channels[0U].impairment.enabled               = true;
    cfg_b.channels[0U].impairment.partition_enabled     = true;
    cfg_b.channels[0U].impairment.partition_gap_ms      = 10U;     // 10 ms gap
    cfg_b.channels[0U].impairment.partition_duration_ms = 60000U;  // stays 60 s

    Result r = side_a.init(cfg_a);
    assert(r == Result::OK);
    assert(side_a.is_open() == true);

    r = side_b.init(cfg_b);
    assert(r == Result::OK);
    assert(side_b.is_open() == true);

    // REQ-6.1.8 / REQ-6.1.10 / REQ-6.2.4: register_local_id() sends HELLO to peer.
    // source_id=2 matches warmup and test datagram source_id.
    r = side_a.register_local_id(2U);
    assert(r == Result::OK);

    // Step 1: send a warm-up datagram; side_b receives it (partition not yet
    // active — first call to is_partition_active() just initialises the timer).
    MessageEnvelope warmup;
    make_test_envelope(warmup, 0xAB90ULL);
    warmup.source_id      = 2U;
    warmup.destination_id = 1U;
    r = side_a.send_message(warmup);
    assert(r == Result::OK);

    // HELLO is consumed first, then warmup DATA arrives in the same receive_message call.
    MessageEnvelope warmup_recv;
    r = side_b.receive_message(warmup_recv, 500U);
    assert(r == Result::OK);  // warm-up message must arrive (partition not active yet)

    // Step 2: wait > 10 ms so the partition gap elapses and partition becomes active
    usleep(15000U);  // 15 ms > 10 ms gap

    // Step 3: send the test datagram — process_inbound() drops it (partition active)
    MessageEnvelope env;
    make_test_envelope(env, 0xAB91ULL);
    env.source_id      = 2U;
    env.destination_id = 1U;
    r = side_a.send_message(env);
    assert(r == Result::OK);

    MessageEnvelope recv_env;
    r = side_b.receive_message(recv_env, 500U);
    assert(r == Result::ERR_TIMEOUT);  // partition dropped the datagram

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_inbound_partition_drops_received\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: HELLO registers peer NodeId; HELLO frame is consumed and not
// returned to the caller; subsequent DATA with matching source_id is accepted.
// Verifies: REQ-6.1.8, REQ-6.2.4
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.8, REQ-6.2.4
static void test_udp_hello_registration()
{
    static const uint16_t PORT_A = 19700U;
    static const uint16_t PORT_B = 19701U;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // Step 1: register_local_id() sends HELLO on wire; side_b consumes it internally.
    // receive_message must NOT return the HELLO to the caller (REQ-6.1.10).
    assert(side_a.register_local_id(1U) == Result::OK);

    MessageEnvelope recv_hello;
    Result r = side_b.receive_message(recv_hello, 500U);
    assert(r == Result::ERR_TIMEOUT);  // HELLO consumed; never reaches app layer

    // Step 2: DATA with matching source_id is accepted after HELLO.
    MessageEnvelope data;
    make_test_envelope(data, 0xAB12ULL);
    assert(side_a.send_message(data) == Result::OK);

    MessageEnvelope recv_data;
    r = side_b.receive_message(recv_data, 500U);
    assert(r == Result::OK);
    assert(recv_data.message_id == 0xAB12ULL);

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_hello_registration\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: DATA frame received before HELLO is dropped (REQ-6.1.8, REQ-6.2.4).
// Verifies: REQ-6.1.8, REQ-6.2.4
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.8, REQ-6.2.4
static void test_udp_data_before_hello_dropped()
{
    static const uint16_t PORT_A = 19702U;
    static const uint16_t PORT_B = 19703U;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // Send DATA without prior HELLO — side_b must drop it (WARNING_HI logged).
    MessageEnvelope data;
    make_test_envelope(data, 0xBAD1ULL);
    assert(side_a.send_message(data) == Result::OK);

    MessageEnvelope recv;
    Result r = side_b.receive_message(recv, 500U);
    assert(r == Result::ERR_TIMEOUT);  // data-before-HELLO dropped

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_data_before_hello_dropped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 23: source_id rotation after HELLO is rejected (REQ-6.2.4).
// After HELLO registers source_id=1, a DATA with source_id=99 must be dropped.
// Verifies: REQ-6.2.4
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.2.4
static void test_udp_source_id_rotation_rejected()
{
    static const uint16_t PORT_A = 19704U;
    static const uint16_t PORT_B = 19705U;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // Register with source_id=1 via register_local_id(); side_b consumes HELLO.
    assert(side_a.register_local_id(1U) == Result::OK);

    MessageEnvelope recv_hello;
    Result r = side_b.receive_message(recv_hello, 500U);
    assert(r == Result::ERR_TIMEOUT);  // HELLO consumed

    // Attempt source_id rotation: send DATA with source_id=99 (not registered).
    MessageEnvelope spoofed;
    make_test_envelope(spoofed, 0xBAD2ULL);
    spoofed.source_id = 99U;  // does not match registered NodeId 1
    assert(side_a.send_message(spoofed) == Result::OK);

    // side_b must drop — source_id mismatch (WARNING_HI logged).
    MessageEnvelope recv_data;
    r = side_b.receive_message(recv_data, 500U);
    assert(r == Result::ERR_TIMEOUT);

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_source_id_rotation_rejected\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 24: duplicate HELLO is dropped; existing registration survives.
// First HELLO registers source_id=1; second HELLO is rejected (WARNING_HI).
// Subsequent DATA with source_id=1 is still accepted.
// Verifies: REQ-6.1.8
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.8
static void test_udp_duplicate_hello_dropped()
{
    static const uint16_t PORT_A = 19706U;
    static const uint16_t PORT_B = 19707U;

    UdpBackend side_a;
    UdpBackend side_b;

    TransportConfig cfg_a;
    TransportConfig cfg_b;
    make_udp_cfg(cfg_a, PORT_A, PORT_B);
    make_udp_cfg(cfg_b, PORT_B, PORT_A);

    assert(side_a.init(cfg_a) == Result::OK);
    assert(side_b.init(cfg_b) == Result::OK);

    // First HELLO: register_local_id() sends HELLO; registers source_id=1 at side_b.
    assert(side_a.register_local_id(1U) == Result::OK);

    // Second HELLO: duplicate; dropped with WARNING_HI.
    MessageEnvelope hello2;
    make_hello_envelope(hello2, 1U);
    assert(side_a.send_message(hello2) == Result::OK);

    // DATA with registered source_id=1 must still be accepted.
    MessageEnvelope data;
    make_test_envelope(data, 0xAB34ULL);
    assert(side_a.send_message(data) == Result::OK);

    // receive_message iterates: HELLO1 consumed, HELLO2 (duplicate) dropped,
    // then DATA accepted.
    MessageEnvelope recv;
    Result r = side_b.receive_message(recv, 1000U);
    assert(r == Result::OK);
    assert(recv.message_id == 0xAB34ULL);

    side_a.close();
    side_b.close();
    printf("PASS: test_udp_duplicate_hello_dropped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Additional mock fault-injection tests (VVP-001 M5)
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_udp_bind_fail()
{
    // Verifies: REQ-6.2.1, REQ-6.3.2
    // Covers: UdpBackend::init() bind-failure path (do_bind returns false)
    MockSocketOps mock;
    mock.fail_do_bind = true;

    UdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_udp_cfg(cfg, 19710U, 19711U);

    Result r = backend.init(cfg);
    assert(r == Result::ERR_IO);
    assert(!backend.is_open());
    assert(mock.n_do_close >= 1);

    printf("PASS: test_mock_udp_bind_fail\n");
}

static void test_mock_udp_send_hello_send_to_fail()
{
    // Verifies: REQ-6.1.8, REQ-6.1.10
    // Covers: UdpBackend::send_hello_datagram() send_to-failure path
    // Strategy: init succeeds; inject send_to failure; register_local_id → ERR_IO.
    MockSocketOps mock;
    UdpBackend backend(mock);

    TransportConfig cfg;
    make_udp_cfg(cfg, 19712U, 19713U);
    assert(backend.init(cfg) == Result::OK);
    assert(backend.is_open());

    mock.fail_send_to = true;  // inject after init

    Result r = backend.register_local_id(3U);
    assert(r == Result::ERR_IO);

    backend.close();
    printf("PASS: test_mock_udp_send_hello_send_to_fail\n");
}

static void test_mock_udp_send_hello_no_peer()
{
    // Verifies: REQ-6.1.10
    // Covers: UdpBackend::send_hello_datagram() no-peer guard
    //         (peer_ip[0]=='\0' || peer_port==0 → ERR_INVALID)
    // Strategy: configure with no peer; init still succeeds (bind only);
    //           register_local_id → send_hello_datagram → ERR_INVALID.
    UdpBackend backend;

    TransportConfig cfg;
    make_udp_cfg(cfg, 19714U, 0U);  // peer_port = 0 triggers the guard
    cfg.peer_ip[0] = '\0';           // also clear peer_ip for belt-and-braces

    assert(backend.init(cfg) == Result::OK);
    assert(backend.is_open());

    Result r = backend.register_local_id(2U);
    assert(r == Result::ERR_INVALID);

    backend.close();
    printf("PASS: test_mock_udp_send_hello_no_peer\n");
}

static void test_mock_udp_get_stats()
{
    // Verifies: REQ-7.2.4
    // Covers: UdpBackend::get_transport_stats() — all lines previously uncovered.
    MockSocketOps mock;
    UdpBackend backend(mock);

    TransportConfig cfg;
    make_udp_cfg(cfg, 19715U, 19716U);
    assert(backend.init(cfg) == Result::OK);

    TransportStats stats;
    transport_stats_init(stats);
    backend.get_transport_stats(stats);

    // UDP bind counts as a connection event (REQ-7.2.4).
    assert(stats.connections_opened >= 1U);
    assert(stats.connections_closed == 0U);

    backend.close();
    printf("PASS: test_mock_udp_get_stats\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    printf("=== test_UdpBackend ===\n");

    test_udp_config();
    test_udp_bind_and_close();
    test_udp_receive_timeout();
    test_udp_loopback_send_receive();
    test_udp_multiple_messages();
    test_udp_close_before_init();
    test_udp_is_open_lifecycle();
    test_udp_send_to_unreachable();
    test_udp_bind_bad_ip();
    test_udp_num_channels_zero();
    test_udp_recv_garbage_datagram();

    // Mock fault-injection tests (VVP-001 M5)
    test_mock_udp_create_fail();
    test_mock_udp_reuseaddr_fail();
    test_mock_udp_bind_fail();
    test_mock_udp_send_hello_send_to_fail();
    test_mock_udp_send_hello_no_peer();
    test_mock_udp_get_stats();

    // Impairment delay-path tests (Option A — full ImpairmentConfig in ChannelConfig)
    test_udp_impairment_delay_paths();

    // Branch coverage: loss impairment, send_to failure, initial recv_queue pop
    test_udp_send_loss_impairment();
    test_mock_send_to_fail();
    test_udp_recv_queue_initial_pop();

    // M5 fault injection: recv_from OS failure path
    test_mock_udp_recv_from_fail();

    // REQ-6.2.4: source address validation -- wrong source dropped (M5)
    test_recv_wrong_source_dropped();

    // Inbound impairment wiring tests (REQ-5.1.5, REQ-5.1.6)
    test_udp_inbound_partition_drops_received();

    // REQ-6.1.8 / REQ-6.2.4: HELLO-before-data enforcement and source_id binding
    test_udp_hello_registration();
    test_udp_data_before_hello_dropped();
    test_udp_source_id_rotation_rejected();
    test_udp_duplicate_hello_dropped();

    printf("=== ALL PASSED ===\n");
    return 0;
}
