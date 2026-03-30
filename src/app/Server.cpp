/**
 * @file Server.cpp
 * @brief Safety-critical TCP server demo.
 *
 * Requirement traceability: messageEngine/CLAUDE.md overall architecture.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, ≥2 assertions per function, no recursion,
 *     no malloc/new after init, functions ≤1 page, check all returns.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection, only static_cast.
 *   - F-Prime style: Result enum returns, Logger::log() for events.
 *
 * Usage: ./Server [port]
 *   port: optional TCP bind port (default 9000)
 *
 * Behavior:
 *   1. Initializes TCP server on 0.0.0.0:port
 *   2. Runs a bounded main loop (MAX_LOOP_ITERS = 100000) receiving and echoing messages
 *   3. Calls pump_retries() and sweep_ack_timeouts() each iteration
 *   4. On SIGINT, sets stop flag and exits cleanly
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3, REQ-7.1.1
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include "core/Assert.hpp"

#include "core/Types.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/DeliveryEngine.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "platform/TcpBackend.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants (Power of 10 rule 3: fixed bounds)
// ─────────────────────────────────────────────────────────────────────────────
static const int   MAX_LOOP_ITERS          = 100000;
static const int   PAYLOAD_PRINT_MAX       = 256;
static const int   RECV_TIMEOUT_MS         = 100;
static const int   LOCAL_SERVER_NODE_ID    = 1U;
static const int   DEFAULT_BIND_PORT       = 9000;
static const char* DEFAULT_BIND_IP         = "0.0.0.0";

// ─────────────────────────────────────────────────────────────────────────────
// Global state (volatile for signal handler)
// ─────────────────────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_stop_flag = 0;

/// Signal handler for SIGINT
/// MISRA deviation: signal() requires a function pointer; no alternative POSIX API available.
static void signal_handler(int sig)
{
    (void)sig;  // unused parameter
    g_stop_flag = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Print payload as a null-terminated string (bounded)
// ─────────────────────────────────────────────────────────────────────────────
static void print_payload_as_string(const uint8_t* payload, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(payload != nullptr);  // Assert: payload must not be null
    NEVER_COMPILED_OUT_ASSERT(len <= MSG_MAX_PAYLOAD_BYTES);  // Assert: bounded payload length

    // Power of 10 rule 3: fixed-size buffer
    char buf[PAYLOAD_PRINT_MAX];
    uint32_t copy_len = len;
    if (copy_len >= static_cast<uint32_t>(PAYLOAD_PRINT_MAX)) {
        copy_len = static_cast<uint32_t>(PAYLOAD_PRINT_MAX) - 1U;
    }

    // Power of 10 rule 7: checked return
    int ret = snprintf(buf, static_cast<size_t>(PAYLOAD_PRINT_MAX),
                       "%.*s", static_cast<int>(copy_len),
                       reinterpret_cast<const char*>(payload));
    if (ret < 0) {
        (void)printf("[error copying payload]\n");
    } else {
        (void)printf("%s\n", buf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Build and send an echo reply (swaps src/dst, copies payload)
// ─────────────────────────────────────────────────────────────────────────────
static Result send_echo_reply(DeliveryEngine& engine,
                               const MessageEnvelope& received,
                               uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(received));          // Assert: received envelope is valid
    NEVER_COMPILED_OUT_ASSERT(now_us > 0U);                      // Assert: valid timestamp

    MessageEnvelope reply;
    envelope_init(reply);

    // Build echo reply: swap source and destination
    reply.message_type       = MessageType::DATA;
    reply.source_id          = received.destination_id;
    reply.destination_id     = received.source_id;
    reply.priority           = received.priority;
    reply.reliability_class  = received.reliability_class;
    reply.timestamp_us       = now_us;
    reply.expiry_time_us     = timestamp_deadline_us(now_us, 10000U);  // 10 second expiry
    reply.payload_length     = received.payload_length;

    // Power of 10 rule 7: check memcpy bounds
    if (reply.payload_length > MSG_MAX_PAYLOAD_BYTES) {
        return Result::ERR_INVALID;
    }
    (void)memcpy(reply.payload, received.payload, reply.payload_length);

    // Send the reply via delivery engine
    Result res = engine.send(reply, now_us);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "Server",
                    "Failed to send echo reply: result=%d\n",
                    static_cast<int>(res));
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Parse bind port from argv[1] with strtol validation
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t parse_server_port(int argc, char* const argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Power of 10: valid argument count
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Power of 10: valid argument array

    if (argc < 2) {
        return static_cast<uint16_t>(DEFAULT_BIND_PORT);
    }
    // bugprone-unchecked-string-to-number-conversion: strtol detects non-numeric input
    char* end_ptr = nullptr;
    const long port_long = strtol(argv[1], &end_ptr, 10);
    const int port_val = (end_ptr != argv[1] && *end_ptr == '\0')
                         ? static_cast<int>(port_long) : 0;
    if (port_val > 0 && port_val <= 65535) {
        return static_cast<uint16_t>(port_val);
    }
    return static_cast<uint16_t>(DEFAULT_BIND_PORT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Run one receive/echo/retry/sweep iteration of the server loop
// ─────────────────────────────────────────────────────────────────────────────
static void run_server_iteration(DeliveryEngine& engine,
                                  uint32_t& messages_received,
                                  uint32_t& messages_sent)
{
    NEVER_COMPILED_OUT_ASSERT(RECV_TIMEOUT_MS > 0);                     // Power of 10: valid timeout constant
    NEVER_COMPILED_OUT_ASSERT(messages_sent <= messages_received + 1U); // Power of 10: echoes bounded by receives

    uint64_t now_us = timestamp_now_us();

    MessageEnvelope received;
    envelope_init(received);

    Result res = engine.receive(received, static_cast<uint32_t>(RECV_TIMEOUT_MS), now_us);

    if (result_ok(res) && envelope_is_data(received)) {
        ++messages_received;
        Logger::log(Severity::INFO, "Server",
                    "Received msg#%llu from node %u, len %u: ",
                    static_cast<unsigned long long>(received.message_id),
                    static_cast<unsigned>(received.source_id),
                    received.payload_length);
        print_payload_as_string(received.payload, received.payload_length);

        Result echo_res = send_echo_reply(engine, received, now_us);
        if (result_ok(echo_res)) {
            ++messages_sent;
        }
    }

    uint32_t retried = engine.pump_retries(now_us);
    if (retried > 0U) {
        Logger::log(Severity::INFO, "Server", "Retried %u message(s)\n", retried);
    }

    uint32_t ack_timeouts = engine.sweep_ack_timeouts(now_us);
    if (ack_timeouts > 0U) {
        Logger::log(Severity::WARNING_HI, "Server",
                    "Detected %u ACK timeout(s)\n", ack_timeouts);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main function
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Assert: argv must not be null
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Assert: argc must be at least 1

    uint16_t bind_port = parse_server_port(argc, argv);
    Logger::log(Severity::INFO, "Server", "Starting TCP server on port %u\n", bind_port);

    // ─────────────────────────────────────────────────────────────────────────
    // Configure transport and channel
    // ─────────────────────────────────────────────────────────────────────────
    TransportConfig cfg;
    transport_config_default(cfg);

    cfg.kind            = TransportKind::TCP;
    cfg.is_server       = true;
    cfg.bind_port       = bind_port;
    cfg.local_node_id   = LOCAL_SERVER_NODE_ID;
    cfg.num_channels    = 1U;

    // Copy bind IP (Power of 10: strncpy with explicit null-termination)
    (void)strncpy(cfg.bind_ip, DEFAULT_BIND_IP, sizeof(cfg.bind_ip) - 1U);
    cfg.bind_ip[sizeof(cfg.bind_ip) - 1U] = '\0';

    // Configure channel 0 for reliable retry
    channel_config_default(cfg.channels[0], 0U);
    cfg.channels[0].reliability     = ReliabilityClass::RELIABLE_RETRY;
    cfg.channels[0].ordering        = OrderingMode::ORDERED;
    cfg.channels[0].max_retries     = 3U;
    cfg.channels[0].recv_timeout_ms = static_cast<uint32_t>(RECV_TIMEOUT_MS);

    // ─────────────────────────────────────────────────────────────────────────
    // Initialize transport backend
    // ─────────────────────────────────────────────────────────────────────────
    TcpBackend transport;
    Result res = transport.init(cfg);
    if (!result_ok(res)) {
        Logger::log(Severity::FATAL, "Server", "Failed to init TcpBackend: result=%d\n",
                    static_cast<int>(res));
        return 1;
    }
    Logger::log(Severity::INFO, "Server", "TcpBackend initialized\n");

    // ─────────────────────────────────────────────────────────────────────────
    // Initialize delivery engine
    // ─────────────────────────────────────────────────────────────────────────
    DeliveryEngine engine;
    engine.init(&transport, cfg.channels[0], LOCAL_SERVER_NODE_ID);
    Logger::log(Severity::INFO, "Server", "DeliveryEngine initialized\n");

    // ─────────────────────────────────────────────────────────────────────────
    // Install signal handler
    // ─────────────────────────────────────────────────────────────────────────
    // MISRA deviation: signal() requires a function pointer; no alternative POSIX API available.
    void (*old_handler)(int) = signal(SIGINT, signal_handler);
    NEVER_COMPILED_OUT_ASSERT(old_handler != SIG_ERR);  // Assert: signal() succeeded

    // ─────────────────────────────────────────────────────────────────────────
    // Main receive/echo loop (Power of 10 rule 2: fixed bound)
    // ─────────────────────────────────────────────────────────────────────────
    Logger::log(Severity::INFO, "Server", "Entering main loop. Press Ctrl+C to exit.\n");

    uint32_t messages_received = 0U;
    uint32_t messages_sent     = 0U;

    for (int iter = 0; iter < MAX_LOOP_ITERS; ++iter) {
        if (g_stop_flag != 0) {
            Logger::log(Severity::INFO, "Server", "Stop flag set; exiting loop\n");
            break;
        }
        run_server_iteration(engine, messages_received, messages_sent);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Cleanup
    // ─────────────────────────────────────────────────────────────────────────
    transport.close();
    Logger::log(Severity::INFO, "Server",
                "Server stopped. Messages received: %u, sent: %u\n",
                messages_received, messages_sent);
    (void)signal(SIGINT, old_handler);

    return 0;
}
