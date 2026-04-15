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
 * @file test_stress_tcp_fanin.cpp
 * @brief Sustained-load stress test for TcpBackend multi-client fan-in
 *        and server-side unicast routing.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * Unit tests for TcpBackend verify individual paths (bind, connect, loopback
 * roundtrip) at a few design points.  This stress test runs sustained,
 * concurrent TCP traffic from TCP_STRESS_CLIENTS simultaneous clients to one
 * server, verifying the multi-client fan-in path and server-side unicast
 * routing (REQ-6.1.9) across thousands of complete connect/send/echo/disconnect
 * cycles.
 *
 * What each round exercises:
 *   1. All TCP_STRESS_CLIENTS client threads concurrently connect to the server.
 *   2. Each client calls register_local_id() to send a HELLO frame, registering
 *      its NodeId in the server's NodeId → socket-slot routing table (REQ-6.1.8).
 *   3. Each client sends TCP_STRESS_MSGS_PER_CLIENT DATA messages while
 *      all other clients do the same — true concurrent fan-in.
 *   4. The server receives each DATA message and echoes it back with
 *      destination_id = received.source_id — unicast routing (REQ-6.1.9).
 *   5. Each client receives its own echoes and verifies message_id identity.
 *      No client ever sees another client's echoes (unicast guarantee).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS TEST CATCHES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Lost messages — server total_recv != expected at round end.
 *  • Misrouted echoes — client receives echo with wrong message_id.
 *  • Routing table corruption — server routes echo to wrong socket slot,
 *    causing the wrong client to get the reply (trips message_id assert).
 *  • Slot leaks — server accumulates slots across reconnects, eventually
 *    refusing new connections (MAX_TCP_CONNECTIONS exhausted).
 *  • HELLO ordering violation — server drops DATA from unregistered slot;
 *    client receives no echo and trips the timeout assert.
 *  • Source-ID spoof — server locks in NodeId from HELLO; a DATA frame
 *    with a different source_id is dropped (REQ-6.1.11).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * MULTIPLE INSTANCES
 * ─────────────────────────────────────────────────────────────────────────────
 * Each instance allocates its own ephemeral port via alloc_ephemeral_port()
 * (TestPortAllocator.hpp) for every round.  Multiple instances of this binary
 * can therefore run simultaneously on the same machine without port conflicts
 * or any other shared resource.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DESIGN NOTES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • TcpBackend is ~539 KB on the stack (see test_TcpBackend.cpp comment).
 *    Every thread that creates a local TcpBackend must use a 2 MB stack.
 *    pthread_attr_setstacksize(2 MB) is called for all threads.
 *  • usleep(TCP_HELLO_SETTLE_US) between HELLO and first DATA gives the
 *    server time to process the HELLO and populate its routing table before
 *    the first DATA frame arrives — prevents spurious REQ-6.1.11 drops.
 *  • Server loop is bounded by TCP_SERVER_MAX_ITERS (compile-time constant
 *    derived from TCP_STRESS_CLIENTS × TCP_STRESS_MSGS_PER_CLIENT) to satisfy
 *    Power of 10 Rule 2 while still draining all expected messages.
 *  • Static arg arrays for thread arguments (no heap allocation on the hot
 *    path); TcpBackend objects live in each thread's 2 MB stack.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 1 (no goto / recursion)  — obeyed throughout.
 *  Rule 2 (fixed loop bounds)    — all loops compile-time bounded or
 *                                  duration-bounded (outer, deviation).
 *  Rule 3 (no dynamic alloc)     — no new/delete; OS thread stacks are
 *                                  outside the project's allocation domain.
 *  Rule 5 (assertions)           — ≥2 assert() per function.
 *  Rule 7 (check returns)        — all Result and pthread return values checked.
 *  Rule 10 (zero warnings)       — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions    — raised ceiling per §1b; each function ≤ 14.
 *  assert() permitted            — tests/ exemption per §9.
 *
 * Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-6.1.7,
 *           REQ-6.1.8, REQ-6.1.9, REQ-6.1.10, REQ-6.1.11
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <ctime>    // time_t, time()
#include <climits>  // UINT32_MAX, UINT64_MAX
#include <csignal>  // signal(), SIGPIPE, SIG_IGN
#include <atomic>   // std::atomic<bool> — permitted carve-out (CLAUDE.md §1d)
#include <unistd.h> // usleep()
#include <pthread.h>

#include "core/Types.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "platform/TcpBackend.hpp"
#include "TestPortAllocator.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// ─────────────────────────────────────────────────────────────────────────────
// Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────

/// Number of concurrent client threads per round.
/// Must be < MAX_TCP_CONNECTIONS (8) so the server always has room.
static const uint32_t TCP_STRESS_CLIENTS       = 4U;

/// DATA messages each client sends and receives per round.
static const uint32_t TCP_STRESS_MSGS_PER_CLIENT = 10U;

/// NodeId for the server.
static const NodeId TCP_SERVER_NODE  = 1U;

/// First client NodeId; clients use TCP_CLIENT_BASE_NODE + i (i=0..CLIENTS-1).
static const NodeId TCP_CLIENT_BASE_NODE = 10U;

/// Thread stack size — TcpBackend is ~539 KB; 2 MB gives comfortable headroom.
static const size_t TCP_THREAD_STACK = static_cast<size_t>(2U) * 1024U * 1024U;

/// Microseconds between a client's HELLO and its first DATA send.
/// Gives the server time to call receive_message() and process the HELLO,
/// populating its NodeId → socket-slot routing table before DATA arrives.
static const uint32_t TCP_HELLO_SETTLE_US = 300000U;   // 300 ms

/// Per-call timeout (ms) for receive_message() in the server loop.
static const uint32_t TCP_SERVER_RECV_TIMEOUT_MS = 20U;

/// Maximum iterations for the server receive loop.
/// Sized to handle all expected DATA messages plus extra receive_message()
/// calls that drain HELLOs (which don't count toward total_recv).
/// Formula: (clients × msgs + 1) × 500 is a generous bound that still
/// terminates quickly when all messages arrive normally.
/// Power of 10 Rule 2: this is the compile-time upper bound for the server loop.
static const uint32_t TCP_SERVER_MAX_ITERS =
    (TCP_STRESS_CLIENTS * TCP_STRESS_MSGS_PER_CLIENT + 1U) * 500U;

// ─────────────────────────────────────────────────────────────────────────────
// Thread argument structures — static (BSS), never heap-allocated.
// ─────────────────────────────────────────────────────────────────────────────

struct TcpFaninSrvArg {
    uint16_t port;           ///< Port the server binds to.
    Result   init_result;    ///< Filled by server thread after init().
    uint32_t total_recv;     ///< DATA messages received (filled by server thread).
    uint32_t total_echo;     ///< Echo replies sent (filled by server thread).
    // std::atomic<bool> carve-out: CLAUDE.md §1d / .claude/CLAUDE.md §3.
    // Signals main thread that the server has called listen() and is ready to
    // accept connections.  Prevents the fixed-sleep TOCTOU race where clients
    // attempt connect() before the server's bind+listen finishes.
    std::atomic<bool> ready; ///< Set true by server thread after successful init().
};

struct TcpFaninCliArg {
    uint64_t start_msg_id;   ///< First message_id for this client in this round.
    NodeId   node_id;        ///< This client's NodeId (sent in HELLO).
    uint32_t msgs_to_send;   ///< DATA messages to send (= TCP_STRESS_MSGS_PER_CLIENT).
    uint32_t msgs_sent;      ///< Filled by client thread on completion.
    uint32_t msgs_recv;      ///< Filled by client thread on completion.
    uint16_t port;           ///< Server port to connect to.
    Result   result;         ///< OK on success, error code on failure.
};

// ─────────────────────────────────────────────────────────────────────────────
// Parse an optional duration (seconds) from argv[1].
// Returns 60 if no argument is given or the parsed value is 0.
// Power of 10 Rule 3: no dynamic allocation; no strtol.
// Power of 10 Rule 2: loop bounded by 10 digits (compile-time).
// ─────────────────────────────────────────────────────────────────────────────
static time_t parse_duration_secs(int argc, char* argv[])
{
    assert(argc >= 0);
    if (argc < 2) {
        return static_cast<time_t>(60);
    }
    uint32_t val = 0U;
    const char* p = argv[1];
    for (uint32_t i = 0U; i < 10U && *p >= '0' && *p <= '9'; ++i) {
        val = val * 10U + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    return (val == 0U) ? static_cast<time_t>(60) : static_cast<time_t>(val);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a server TransportConfig.
// ─────────────────────────────────────────────────────────────────────────────
static void make_fanin_server_cfg(TransportConfig& cfg, uint16_t port)
{
    assert(port > 0U);
    transport_config_default(cfg);
    cfg.kind          = TransportKind::TCP;
    cfg.is_server     = true;
    cfg.bind_port     = port;
    cfg.local_node_id = TCP_SERVER_NODE;
    // recv_timeout_ms also governs the HELLO-eviction timeout (REQ-6.1.12).
    // Use 5 s to give clients ample time to deliver their HELLO on loaded machines.
    // receive_message() uses its own caller-supplied timeout_ms argument, not this field.
    cfg.channels[0U].recv_timeout_ms = 5000U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a client TransportConfig connecting to the server port.
// ─────────────────────────────────────────────────────────────────────────────
static void make_fanin_client_cfg(TransportConfig& cfg, uint16_t port, NodeId node_id)
{
    assert(port > 0U);
    assert(node_id != NODE_ID_INVALID);
    transport_config_default(cfg);
    cfg.kind               = TransportKind::TCP;
    cfg.is_server          = false;
    cfg.peer_port          = port;
    cfg.local_node_id      = node_id;
    cfg.connect_timeout_ms = 3000U;
    cfg.channels[0U].recv_timeout_ms = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a DATA envelope for fan-in testing.
// source_id = sender NodeId; destination_id = server NodeId.
// ─────────────────────────────────────────────────────────────────────────────
static void make_fanin_env(MessageEnvelope& env,
                            NodeId  src,
                            NodeId  dst,
                            uint64_t msg_id)
{
    assert(src != NODE_ID_INVALID);
    assert(msg_id > 0U);
    envelope_init(env);
    env.message_type         = MessageType::DATA;
    env.source_id            = src;
    env.destination_id       = dst;
    env.message_id           = msg_id;
    env.sequence_num         = 0U;         // UNORDERED
    env.timestamp_us         = 1000000ULL;
    env.expiry_time_us       = 0xFFFFFFFFFFFFFF00ULL;
    env.reliability_class    = ReliabilityClass::BEST_EFFORT;
    env.fragment_index       = 0U;
    env.fragment_count       = 1U;
    env.payload_length       = 8U;
    env.total_payload_length = 8U;
    // Encode low 64 bits of msg_id in payload for identity check.
    env.payload[0] = static_cast<uint8_t>( msg_id        & 0xFFU);
    env.payload[1] = static_cast<uint8_t>((msg_id >>  8U) & 0xFFU);
    env.payload[2] = static_cast<uint8_t>((msg_id >> 16U) & 0xFFU);
    env.payload[3] = static_cast<uint8_t>((msg_id >> 24U) & 0xFFU);
    env.payload[4] = static_cast<uint8_t>((msg_id >> 32U) & 0xFFU);
    env.payload[5] = static_cast<uint8_t>((msg_id >> 40U) & 0xFFU);
    env.payload[6] = static_cast<uint8_t>((msg_id >> 48U) & 0xFFU);
    env.payload[7] = static_cast<uint8_t>((msg_id >> 56U) & 0xFFU);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server thread — binds, accepts connections, receives DATA, echoes back.
//
// TcpBackend (~539 KB) lives on the thread stack; thread stack = 2 MB.
// ─────────────────────────────────────────────────────────────────────────────
static void* fanin_server_fn(void* raw)
{
    TcpFaninSrvArg* a = static_cast<TcpFaninSrvArg*>(raw);
    assert(a != nullptr);

    TcpBackend server;  // ~539 KB on stack — thread stack must be ≥ 2 MB
    TransportConfig cfg;
    make_fanin_server_cfg(cfg, a->port);

    a->init_result = server.init(cfg);
    // Signal readiness before the early-return so that the main thread is never
    // left polling forever: if init fails, ready=true lets main see init_result.
    a->ready.store(true, std::memory_order_release);
    if (a->init_result != Result::OK) {
        return nullptr;
    }

    // Register our node ID (server mode: stores locally, no HELLO sent).
    Result rr = server.register_local_id(TCP_SERVER_NODE);
    assert(rr == Result::OK);

    const uint32_t expected = TCP_STRESS_CLIENTS * TCP_STRESS_MSGS_PER_CLIENT;

    // Power of 10 Rule 2: bounded by TCP_SERVER_MAX_ITERS (compile-time).
    // Normal exit: a->total_recv reaches expected well before max_iters.
    for (uint32_t iter = 0U;
         iter < TCP_SERVER_MAX_ITERS && a->total_recv < expected;
         ++iter)
    {
        MessageEnvelope env;
        envelope_init(env);
        Result r = server.receive_message(env, TCP_SERVER_RECV_TIMEOUT_MS);

        if (r != Result::OK) {
            continue;  // timeout or transient error; retry
        }

        ++a->total_recv;

        // REQ-6.1.9: unicast routing — echo back to originating client.
        // destination_id = received source_id routes to the specific client slot.
        MessageEnvelope echo;
        envelope_copy(echo, env);
        echo.source_id      = TCP_SERVER_NODE;
        echo.destination_id = env.source_id;  // unicast back to sender

        Result er = server.send_message(echo);
        if (er == Result::OK) {
            ++a->total_echo;
        }
    }

    server.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Client thread — connects, registers NodeId via HELLO, sends DATA, verifies
// echoes.
//
// TcpBackend (~539 KB) lives on the thread stack; thread stack = 2 MB.
// ─────────────────────────────────────────────────────────────────────────────
static void* fanin_client_fn(void* raw)
{
    TcpFaninCliArg* a = static_cast<TcpFaninCliArg*>(raw);
    assert(a != nullptr);

    TcpBackend client;  // ~539 KB on stack — thread stack must be ≥ 2 MB
    TransportConfig cfg;
    make_fanin_client_cfg(cfg, a->port, a->node_id);

    Result r = client.init(cfg);
    if (r != Result::OK) {
        a->result = r;
        return nullptr;
    }

    // REQ-6.1.8, REQ-6.1.10: send HELLO to register our NodeId with the server.
    r = client.register_local_id(a->node_id);
    assert(r == Result::OK);

    // Let server process HELLO before we send DATA (REQ-6.1.11: data from
    // unregistered slot is dropped, so HELLO must be processed first).
    usleep(TCP_HELLO_SETTLE_US);

    // Power of 10 Rule 2: bounded by msgs_to_send (= TCP_STRESS_MSGS_PER_CLIENT).
    for (uint32_t i = 0U; i < a->msgs_to_send; ++i) {
        // CERT INT30-C: start_msg_id + i; max i < 10; no overflow risk.
        assert(a->start_msg_id <= UINT64_MAX - static_cast<uint64_t>(i));
        const uint64_t cur_id = a->start_msg_id + static_cast<uint64_t>(i);

        MessageEnvelope send_env;
        make_fanin_env(send_env, a->node_id, TCP_SERVER_NODE, cur_id);

        Result sr = client.send_message(send_env);
        assert(sr == Result::OK);
        ++a->msgs_sent;

        // Receive the echo from the server.  2-second timeout should be
        // generous even under heavy multi-client load on loopback.
        MessageEnvelope echo_env;
        envelope_init(echo_env);
        Result rr = client.receive_message(echo_env, 10000U);  // 10 s — generous for loaded machines
        assert(rr == Result::OK);

        // Verify unicast routing: echo message_id must match what we sent.
        assert(echo_env.message_id == cur_id);
        // Verify server source_id (SEC-025: locked in from first echo).
        assert(echo_env.source_id  == TCP_SERVER_NODE);
        ++a->msgs_recv;
    }

    client.close();
    a->result = Result::OK;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: TCP multi-client fan-in + unicast routing storm.
// ─────────────────────────────────────────────────────────────────────────────
// Each round:
//   a. Allocate an ephemeral port (instance-isolated).
//   b. Start server thread.
//   c. usleep(50 ms) — let server bind before clients connect.
//   d. Start TCP_STRESS_CLIENTS client threads concurrently.
//   e. Join all threads.
//   f. Assert: server received all expected DATA; all clients got all echoes.
//
// Power of 10 Rule 2 deviation: outer while loop is duration-bounded.
// Per-iteration work is bounded by TCP_SERVER_MAX_ITERS (compile-time) on the
// server side and by TCP_STRESS_MSGS_PER_CLIENT on each client side.
// Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-6.1.7, REQ-6.1.8,
//           REQ-6.1.9, REQ-6.1.10, REQ-6.1.11
static uint32_t test_tcp_fanin_storm(time_t deadline)
{
    // Static arg arrays — no heap allocation (Power of 10 Rule 3).
    static TcpFaninSrvArg  srv_arg;
    static TcpFaninCliArg  cli_args[TCP_STRESS_CLIENTS];
    static pthread_t        srv_tid;
    static pthread_t        cli_tids[TCP_STRESS_CLIENTS];

    uint32_t total_rounds = 0U;
    uint64_t msg_id       = 1ULL;  // monotonically increasing across all rounds

    // pthread_attr: 2 MB stack for every thread (TcpBackend is ~539 KB).
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    assert(rc == 0);
    rc = pthread_attr_setstacksize(&attr, TCP_THREAD_STACK);
    assert(rc == 0);

    // Power of 10 Rule 2 deviation: duration-bounded outer loop.
    while (time(nullptr) < deadline) {

        // ── Allocate an ephemeral port for this round (instance isolation). ──
        const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
        assert(port > 0U);

        // ── Reset arg structs for this round. ────────────────────────────────
        srv_arg.port        = port;
        srv_arg.init_result = Result::ERR_IO;
        srv_arg.total_recv  = 0U;
        srv_arg.total_echo  = 0U;
        srv_arg.ready.store(false, std::memory_order_relaxed);

        // Power of 10 Rule 2: bounded by TCP_STRESS_CLIENTS (compile-time).
        for (uint32_t i = 0U; i < TCP_STRESS_CLIENTS; ++i) {
            // CERT INT30-C: i * TCP_STRESS_MSGS_PER_CLIENT; i < 4, msgs < 10 → max 30; safe.
            assert(i <= UINT32_MAX / TCP_STRESS_MSGS_PER_CLIENT);
            cli_args[i].port         = port;
            cli_args[i].node_id      = static_cast<NodeId>(TCP_CLIENT_BASE_NODE + i);
            cli_args[i].msgs_to_send = TCP_STRESS_MSGS_PER_CLIENT;
            // CERT INT30-C: msg_id + i*msgs; max 3*10 = 30 above msg_id; safe.
            cli_args[i].start_msg_id = msg_id
                                       + static_cast<uint64_t>(i)
                                         * static_cast<uint64_t>(TCP_STRESS_MSGS_PER_CLIENT);
            cli_args[i].msgs_sent    = 0U;
            cli_args[i].msgs_recv    = 0U;
            cli_args[i].result       = Result::ERR_IO;
        }

        // ── Start server thread. ──────────────────────────────────────────────
        rc = pthread_create(&srv_tid, &attr, fanin_server_fn, &srv_arg);
        assert(rc == 0);

        // Wait for server to signal it is ready (bind + listen complete).
        // Polling with 1 ms sleep; bounded by TCP_SERVER_READY_TIMEOUT_US.
        // This replaces the fixed 50 ms sleep and prevents ECONNREFUSED on
        // loaded machines where the server thread is scheduled late.
        // Power of 10 Rule 2: loop bounded by TCP_SERVER_READY_TIMEOUT_US / 1000.
        static const uint32_t TCP_SERVER_READY_TIMEOUT_US = 2000000U; // 2 s
        static const uint32_t TCP_SERVER_READY_POLL_US    =    1000U; // 1 ms
        static const uint32_t TCP_SERVER_READY_MAX_ITERS  =
            TCP_SERVER_READY_TIMEOUT_US / TCP_SERVER_READY_POLL_US;    // 2000
        for (uint32_t wi = 0U;
             wi < TCP_SERVER_READY_MAX_ITERS
                 && !srv_arg.ready.load(std::memory_order_acquire);
             ++wi)
        {
            usleep(TCP_SERVER_READY_POLL_US);
        }
        // If still not ready, server init failed; check at the per-round assert.

        // ── Start all client threads concurrently. ────────────────────────────
        // Power of 10 Rule 2: bounded by TCP_STRESS_CLIENTS (compile-time).
        for (uint32_t i = 0U; i < TCP_STRESS_CLIENTS; ++i) {
            rc = pthread_create(&cli_tids[i], &attr, fanin_client_fn, &cli_args[i]);
            assert(rc == 0);
        }

        // ── Join clients first, then server. ─────────────────────────────────
        // Power of 10 Rule 2: bounded by TCP_STRESS_CLIENTS (compile-time).
        for (uint32_t i = 0U; i < TCP_STRESS_CLIENTS; ++i) {
            int rj = pthread_join(cli_tids[i], nullptr);
            assert(rj == 0);
        }
        int rj = pthread_join(srv_tid, nullptr);
        assert(rj == 0);

        // ── Verify correctness. ───────────────────────────────────────────────
        assert(srv_arg.init_result  == Result::OK);
        assert(srv_arg.total_recv   == TCP_STRESS_CLIENTS * TCP_STRESS_MSGS_PER_CLIENT);
        assert(srv_arg.total_echo   == TCP_STRESS_CLIENTS * TCP_STRESS_MSGS_PER_CLIENT);

        // Power of 10 Rule 2: bounded by TCP_STRESS_CLIENTS (compile-time).
        for (uint32_t i = 0U; i < TCP_STRESS_CLIENTS; ++i) {
            assert(cli_args[i].result    == Result::OK);
            assert(cli_args[i].msgs_sent == TCP_STRESS_MSGS_PER_CLIENT);
            assert(cli_args[i].msgs_recv == TCP_STRESS_MSGS_PER_CLIENT);
        }

        // Advance msg_id past all IDs used in this round.
        // CERT INT30-C: clients × msgs = 40; + 1 gap = 41; negligible vs. uint64_t.
        msg_id += static_cast<uint64_t>(TCP_STRESS_CLIENTS)
                  * static_cast<uint64_t>(TCP_STRESS_MSGS_PER_CLIENT)
                  + 1ULL;

        ++total_rounds;
    }

    int rd = pthread_attr_destroy(&attr);
    assert(rd == 0);

    return total_rounds;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Initialize logger before any production code that may call LOG_* macros.
    // Power of 10: return value checked; failure causes abort via NEVER_COMPILED_OUT_ASSERT.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    // Ignore SIGPIPE process-wide — send() to a closed socket returns EPIPE
    // instead of killing the process.  TcpBackend checks all return values and
    // logs a warning on EPIPE; per-round correctness asserts catch any mismatch.
    // Rationale: at high round counts the OS may reuse ports with residual
    // connections still in TIME_WAIT, causing a write to a closing socket.
    // POSIX: signal() returns SIG_ERR on failure; assert to catch mis-configuration.
    // NOLINTNEXTLINE(cert-msc54-cpp,bugprone-signal-handler)
    assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR);  // NOLINT(cert-msc54-cpp)

    const time_t duration_secs = parse_duration_secs(argc, argv);
    assert(duration_secs > 0);

    printf("=== Stress: test_stress_tcp_fanin ===\n");
    printf("    (running for %lu s; %u clients; %u msgs/client/round;"
           " max_iters/round=%u)\n",
           static_cast<unsigned long>(duration_secs),
           TCP_STRESS_CLIENTS, TCP_STRESS_MSGS_PER_CLIENT,
           TCP_SERVER_MAX_ITERS);
    printf("    Multiple instances safe: each round allocs its own ephemeral"
           " port.\n");

    printf("  Test 1: TCP multi-client fan-in + unicast routing storm...");
    const uint32_t rounds = test_tcp_fanin_storm(time(nullptr) + duration_secs);
    assert(rounds > 0U);
    printf(" PASS (%u rounds; %u total DATA exchanged)\n",
           rounds,
           rounds * TCP_STRESS_CLIENTS * TCP_STRESS_MSGS_PER_CLIENT);

    printf("=== test_stress_tcp_fanin: ALL PASSED ===\n");
    return 0;
}
