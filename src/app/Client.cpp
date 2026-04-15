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
 * @file Client.cpp
 * @brief Safety-critical TCP client demo.
 * @generated Produced by Claude Sonnet 4.6 (Anthropic) with human review.
 *
 * Requirement traceability: messageEngine/CLAUDE.md overall architecture.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function, no recursion,
 *     no malloc/new after init, functions ≤1 page, check all returns.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection, only static_cast.
 *   - F-Prime style: Result enum returns, Logger::log() for events.
 *
 * Usage: ./Client [host] [port]
 *   host: server hostname/IP (default "127.0.0.1")
 *   port: server TCP port (default 9000)
 *
 * Behavior:
 *   1. Connects to server via TCP
 *   2. Sends 5 test messages with payload "Hello from client #N"
 *   3. Waits up to 2 seconds for each echo reply
 *   4. Runs pump_retries() and sweep_ack_timeouts() each iteration
 *   5. Prints results and exits
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>   // SEC-020: errno for strtol error detection
#include <unistd.h>
#include "core/Assert.hpp"
#include <time.h>

#include "core/Types.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/DeliveryEngine.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"
#include "platform/TcpBackend.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants (Power of 10 rule 3: fixed bounds)
// ─────────────────────────────────────────────────────────────────────────────
static const int   NUM_MESSAGES              = 5;
static const int   MAX_RECV_RETRIES          = 20;
static const int   RECV_TIMEOUT_MS           = 100;
static const int   INTER_MESSAGE_SLEEP_MS    = 100;
static const int   PAYLOAD_BUILD_MAX         = 256;
static const int   LOCAL_CLIENT_NODE_ID      = 2U;
static const int   LOCAL_SERVER_NODE_ID      = 1U;
static const int   DEFAULT_PEER_PORT         = 9000;
static const char* DEFAULT_PEER_IP           = "127.0.0.1";

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Sleep for N milliseconds (Power of 10: bounded duration)
// ─────────────────────────────────────────────────────────────────────────────
static void sleep_ms(int milliseconds)
{
    NEVER_COMPILED_OUT_ASSERT(milliseconds >= 0);  // Assert: non-negative sleep duration
    NEVER_COMPILED_OUT_ASSERT(milliseconds <= 10000);  // Assert: reasonable sleep bound (10 seconds max)

    struct timespec ts;
    ts.tv_sec  = static_cast<time_t>(milliseconds / 1000);
    ts.tv_nsec = static_cast<long>((milliseconds % 1000) * 1000000);

    // Power of 10 rule 7: check return value
    int ret = nanosleep(&ts, nullptr);
    if (ret != 0) {
        LOG_WARN_LO("Client", "nanosleep interrupted\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Send a test message
// ─────────────────────────────────────────────────────────────────────────────
static Result send_test_message(DeliveryEngine& engine,
                                 NodeId peer_id,
                                 int msg_num,
                                 uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(msg_num >= 1 && msg_num <= NUM_MESSAGES);   // Assert: valid message number
    NEVER_COMPILED_OUT_ASSERT(now_us > 0U);                              // Assert: valid timestamp

    MessageEnvelope env;
    envelope_init(env);

    // Build payload: "Hello from client #N"
    char payload_buf[PAYLOAD_BUILD_MAX];
    int payload_len = snprintf(payload_buf, static_cast<size_t>(PAYLOAD_BUILD_MAX),
                               "Hello from client #%d", msg_num);
    if (payload_len < 0 || payload_len >= PAYLOAD_BUILD_MAX) {
        LOG_WARN_LO("Client", "Payload string construction failed\n");
        return Result::ERR_INVALID;
    }

    // Copy payload into envelope (Power of 10: bounded copy)
    uint32_t payload_bytes = static_cast<uint32_t>(payload_len);
    if (payload_bytes > MSG_MAX_PAYLOAD_BYTES) {
        return Result::ERR_INVALID;
    }
    (void)memcpy(env.payload, payload_buf, payload_bytes);
    env.payload_length = payload_bytes;

    // Configure message
    env.message_type       = MessageType::DATA;
    env.destination_id     = peer_id;
    env.priority           = 0U;
    env.reliability_class  = ReliabilityClass::RELIABLE_RETRY;
    env.timestamp_us       = now_us;
    env.expiry_time_us     = timestamp_deadline_us(now_us, 5000U);  // 5 second expiry

    // Send via delivery engine
    Result res = engine.send(env, now_us);
    if (!result_ok(res)) {
        LOG_WARN_HI("Client", "Failed to send message #%d: result=%d\n",
                    msg_num, static_cast<int>(res));
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Wait for echo reply for a single message (bounded retries)
// ─────────────────────────────────────────────────────────────────────────────
static Result wait_for_echo(DeliveryEngine& engine,
                             uint64_t send_time_us)
{
    NEVER_COMPILED_OUT_ASSERT(send_time_us > 0U);          // Assert: valid send timestamp
    NEVER_COMPILED_OUT_ASSERT(MAX_RECV_RETRIES > 0);      // Assert: retry bound is positive

    // Bounded retry loop (Power of 10 rule 2)
    for (int attempt = 0; attempt < MAX_RECV_RETRIES; ++attempt) {
        uint64_t now_us = timestamp_now_us();

        MessageEnvelope reply;
        envelope_init(reply);

        Result res = engine.receive(reply, static_cast<uint32_t>(RECV_TIMEOUT_MS), now_us);

        if (result_ok(res)) {
            if (envelope_is_data(reply)) {
                LOG_INFO("Client", "Received echo reply: msg_id=%llu, len=%u\n",
                            static_cast<unsigned long long>(reply.message_id),
                            reply.payload_length);
                return Result::OK;
            }
        }

        // Pump retries and check ACK timeouts
        (void)engine.pump_retries(now_us);
        (void)engine.sweep_ack_timeouts(now_us);
    }

    LOG_WARN_HI("Client", "Timeout waiting for echo reply\n");
    return Result::ERR_TIMEOUT;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Return peer IP string from argv[1] or the default
// ─────────────────────────────────────────────────────────────────────────────
static const char* parse_peer_ip(int argc, char* const argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Power of 10: valid argument count
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Power of 10: valid argument array

    if (argc >= 2) {
        return argv[1];
    }
    return DEFAULT_PEER_IP;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Parse peer port from argv[2] with strtol validation
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t parse_peer_port(int argc, char* const argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Power of 10: valid argument count
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Power of 10: valid argument array

    if (argc < 3) {
        return static_cast<uint16_t>(DEFAULT_PEER_PORT);
    }
    // SEC-020: set errno to 0 before strtol to detect overflow/underflow.
    // POSIX requires errno check after strtol when LONG_MIN or LONG_MAX is
    // returned — a range error that end_ptr alone cannot distinguish.
    errno = 0;
    char* end_ptr = nullptr;
    const long port_long = strtol(argv[2], &end_ptr, 10);
    // SEC-020: reject if strtol set errno (overflow/underflow) or if no digits
    // were consumed (end_ptr == argv[2]) or if trailing junk follows the number.
    if (errno != 0) {
        LOG_WARN_LO("Client", "SEC-020: strtol failed for port argument (errno=%d)", errno);
        return static_cast<uint16_t>(DEFAULT_PEER_PORT);
    }
    const int port_val = (end_ptr != argv[2] && *end_ptr == '\0')
                         ? static_cast<int>(port_long) : 0;
    if (port_val > 0 && port_val <= 65535) {
        return static_cast<uint16_t>(port_val);
    }
    return static_cast<uint16_t>(DEFAULT_PEER_PORT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Send one message and wait for its echo reply
// ─────────────────────────────────────────────────────────────────────────────
static void run_client_iteration(DeliveryEngine& engine, int msg_idx,
                                  uint32_t& messages_sent, uint32_t& echoes_received)
{
    NEVER_COMPILED_OUT_ASSERT(msg_idx >= 1 && msg_idx <= NUM_MESSAGES);   // Power of 10: valid message index
    NEVER_COMPILED_OUT_ASSERT(echoes_received <= messages_sent + 1U);      // Power of 10: echoes bounded by sent

    uint64_t now_us = timestamp_now_us();

    Result res = send_test_message(engine, static_cast<NodeId>(LOCAL_SERVER_NODE_ID), msg_idx, now_us);
    if (result_ok(res)) {
        ++messages_sent;
        LOG_INFO("Client", "Sent message #%d\n", msg_idx);
    }

    Result echo_res = wait_for_echo(engine, now_us);
    if (result_ok(echo_res)) {
        ++echoes_received;
    }

    if (msg_idx < NUM_MESSAGES) {
        sleep_ms(INTER_MESSAGE_SLEEP_MS);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main function
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Assert: argv must not be null
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Assert: argc must be at least 1

    // Initialize logger with POSIX clock and sink before any LOG_* call.
    // Failure is non-fatal here: the process can still run but logging will abort.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    const char* peer_ip   = parse_peer_ip(argc, argv);
    uint16_t    peer_port = parse_peer_port(argc, argv);

    LOG_INFO("Client", "Starting TCP client connecting to %s:%u\n",
                peer_ip, peer_port);

    // ─────────────────────────────────────────────────────────────────────────
    // Configure transport and channel
    // ─────────────────────────────────────────────────────────────────────────
    TransportConfig cfg;
    transport_config_default(cfg);

    cfg.kind               = TransportKind::TCP;
    cfg.is_server          = false;
    cfg.peer_port          = peer_port;
    cfg.connect_timeout_ms = 5000U;
    cfg.local_node_id      = LOCAL_CLIENT_NODE_ID;
    cfg.num_channels       = 1U;

    // Copy peer IP (Power of 10: strncpy with explicit null-termination)
    (void)strncpy(cfg.peer_ip, peer_ip, sizeof(cfg.peer_ip) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';

    // Configure channel 0 for reliable retry
    channel_config_default(cfg.channels[0], 0U);
    cfg.channels[0].reliability     = ReliabilityClass::RELIABLE_RETRY;
    cfg.channels[0].ordering        = OrderingMode::UNORDERED;
    cfg.channels[0].max_retries     = 3U;
    cfg.channels[0].recv_timeout_ms = static_cast<uint32_t>(RECV_TIMEOUT_MS);

    // ─────────────────────────────────────────────────────────────────────────
    // Initialize transport backend
    // ─────────────────────────────────────────────────────────────────────────
    TcpBackend transport;
    Result res = transport.init(cfg);
    if (!result_ok(res)) {
        LOG_FATAL("Client", "Failed to init TcpBackend: result=%d\n",
                    static_cast<int>(res));
        return 1;
    }
    LOG_INFO("Client", "TcpBackend initialized\n");

    // ─────────────────────────────────────────────────────────────────────────
    // Initialize delivery engine
    // ─────────────────────────────────────────────────────────────────────────
    DeliveryEngine engine;
    engine.init(&transport, cfg.channels[0], LOCAL_CLIENT_NODE_ID);
    LOG_INFO("Client", "DeliveryEngine initialized\n");

    // ─────────────────────────────────────────────────────────────────────────
    // Send/receive loop (Power of 10 rule 2: fixed bound NUM_MESSAGES)
    // ─────────────────────────────────────────────────────────────────────────
    LOG_INFO("Client", "Sending %d test messages...\n", NUM_MESSAGES);

    uint32_t messages_sent   = 0U;
    uint32_t echoes_received = 0U;

    for (int msg_idx = 1; msg_idx <= NUM_MESSAGES; ++msg_idx) {
        run_client_iteration(engine, msg_idx, messages_sent, echoes_received);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Final pump and timeout sweep
    // ─────────────────────────────────────────────────────────────────────────
    uint64_t final_now_us   = timestamp_now_us();
    uint32_t final_retried  = engine.pump_retries(final_now_us);
    uint32_t final_timeouts = engine.sweep_ack_timeouts(final_now_us);

    if (final_retried > 0U) {
        LOG_INFO("Client", "Final pump: retried %u message(s)\n",
                    final_retried);
    }
    if (final_timeouts > 0U) {
        LOG_WARN_HI("Client", "Final sweep: %u ACK timeout(s)\n",
                    final_timeouts);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Cleanup and report
    // ─────────────────────────────────────────────────────────────────────────
    transport.close();
    LOG_INFO("Client", "Client completed. Sent: %u, Echo replies received: %u\n",
                messages_sent, echoes_received);

    int exit_code = (echoes_received == static_cast<uint32_t>(NUM_MESSAGES)) ? 0 : 1;
    return exit_code;
}
