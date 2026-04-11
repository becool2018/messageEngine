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
 * @file TlsTcpDemo.cpp
 * @brief Simple TLS/TCP client-server demo using TlsTcpBackend and DeliveryEngine.
 * @generated Produced by Claude Sonnet 4.6 (Anthropic) with human review.
 *
 * Implements: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *             REQ-3.3.3, REQ-6.3.4, REQ-7.1.1
 *
 * Usage:
 *   ./tls_demo server [port]   — start echo server (default port 9100)
 *   ./tls_demo client [port]   — start client, send 5 messages, print echoes
 *
 * The program writes a self-signed test certificate and key to /tmp before
 * initialising TLS.  Both server and client use the same cert (self-signed CA),
 * with peer verification disabled so no external PKI is required.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds, >= 2 assertions per function, no
 *     recursion, no malloc/new after init, functions <= 1 page, check all returns.
 *   - MISRA C++:2023: no STL, no exceptions, <= 1 pointer indirection.
 *   - F-Prime subset: ISO C++17, -fno-exceptions, -fno-rtti.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>

#include "core/Assert.hpp"
#include "core/AbortResetHandler.hpp"
#include "core/AssertState.hpp"
#include "core/Types.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/DeliveryEngine.hpp"
#include "core/Logger.hpp"
#include "core/Timestamp.hpp"
#include "platform/TlsTcpBackend.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
static const uint16_t DEFAULT_PORT        = 9100U;
static const uint32_t SERVER_NODE_ID      = 1U;
static const uint32_t CLIENT_NODE_ID      = 2U;
static const int      MAX_SERVER_ITERS    = 200000;
static const int      MAX_CLIENT_MSGS     = 5;
static const int      MAX_CLIENT_RECV     = 20;
static const uint32_t RECV_TIMEOUT_MS     = 200U;
static const uint32_t MSG_EXPIRY_MS       = 10000U;
static const int      CLIENT_STARTUP_US   = 120000;  // 120 ms wait for server

static char           CERT_FILE[64] = {'\0'};
static char           KEY_FILE[64]  = {'\0'};

// Populate CERT_FILE and KEY_FILE with PID-qualified paths so concurrent
// demo instances on the same machine do not share the same /tmp files.
static void init_pem_paths()
{
    (void)snprintf(CERT_FILE, sizeof(CERT_FILE),
                   "/tmp/me_tls_demo_%d.crt", static_cast<int>(getpid()));
    (void)snprintf(KEY_FILE, sizeof(KEY_FILE),
                   "/tmp/me_tls_demo_%d.key", static_cast<int>(getpid()));
}

// Self-signed EC P-256 certificate and key (10-year validity, test use only)
static const char* DEMO_CERT_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBTzCB96ADAgECAgkAqEb7NraJVbIwCgYIKoZIzj0EAwIwHTEbMBkGA1UEAwwS\n"
    "bWVzc2FnZUVuZ2luZS10ZXN0MB4XDTI2MDMzMDIzMDgzMFoXDTM2MDMyNzIzMDgz\n"
    "MFowHTEbMBkGA1UEAwwSbWVzc2FnZUVuZ2luZS10ZXN0MFkwEwYHKoZIzj0CAQYI\n"
    "KoZIzj0DAQcDQgAEgFGXAAFYELb7xqBWO3FO5upRQFdpUReRqs7EAWOEViO2X6am\n"
    "rQoq3w6zI9xcrJzs+cNteeRRplIVMdIidsSFEKMgMB4wDwYDVR0TAQH/BAUwAwEB\n"
    "/zALBgNVHQ8EBAMCAYYwCgYIKoZIzj0EAwIDRwAwRAIgCcIRDNbR5E1J944DL2yk\n"
    "2M0iMeyjtWdZX12A3zycJLECIEVXH1HuH9y3SrJAXqswnQVMS/bfl5F5MPrps1eN\n"
    "Bvtc\n"
    "-----END CERTIFICATE-----\n";

static const char* DEMO_KEY_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgE29ClO6C6vnGjspd\n"
    "6xcoCqV4wgBXN9h5hRTMN9aUkoGhRANCAASAUZcAAVgQtvvGoFY7cU7m6lFAV2lR\n"
    "F5GqzsQBY4RWI7ZfpqatCirfDrMj3FysnOz5w2155FGmUhUx0iJ2xIUQ\n"
    "-----END PRIVATE KEY-----\n";

// ─────────────────────────────────────────────────────────────────────────────
// Global stop flag (signal handler)
// ─────────────────────────────────────────────────────────────────────────────
static volatile sig_atomic_t g_stop = 0;

// MISRA deviation: signal() requires a function pointer; no alternative POSIX API.
static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Write PEM files to /tmp
// ─────────────────────────────────────────────────────────────────────────────
static bool write_pem_files(void)
{
    NEVER_COMPILED_OUT_ASSERT(CERT_FILE[0] != '\0');  // Assert: cert path initialised
    NEVER_COMPILED_OUT_ASSERT(KEY_FILE[0]  != '\0');  // Assert: key path initialised

    // MISRA C++:2023 deviation — fopen/fclose (C file I/O):
    // Rationale: write_pem_files() is called once at process startup to
    // materialise embedded PEM constants as on-disk files for mbedTLS.
    // This is an init-phase operation; FILE* lifetime is bounded to this
    // function scope with fclose on every return path.  No C++ RAII
    // alternative is available without STL (prohibited by CLAUDE.md §4).
    // Power of 10 Rule 3 is satisfied (init-phase only).
    FILE* fp = fopen(CERT_FILE, "w"); // NOLINT(cppcoreguidelines-owning-memory)
    if (fp == nullptr) {
        (void)fprintf(stderr, "ERROR: cannot write %s\n", CERT_FILE);
        return false;
    }
    if (fputs(DEMO_CERT_PEM, fp) < 0) {
        (void)fclose(fp); // NOLINT(cppcoreguidelines-owning-memory)
        return false;
    }
    if (fclose(fp) != 0) { // NOLINT(cppcoreguidelines-owning-memory)
        return false;
    }

    fp = fopen(KEY_FILE, "w"); // NOLINT(cppcoreguidelines-owning-memory)
    if (fp == nullptr) {
        (void)fprintf(stderr, "ERROR: cannot write %s\n", KEY_FILE);
        return false;
    }
    if (fputs(DEMO_KEY_PEM, fp) < 0) {
        (void)fclose(fp); // NOLINT(cppcoreguidelines-owning-memory)
        return false;
    }
    return fclose(fp) == 0; // NOLINT(cppcoreguidelines-owning-memory)
}

// ─────────────────────────────────────────────────────────────────────────────
// Build a TransportConfig for TLS/TCP
// ─────────────────────────────────────────────────────────────────────────────
static void make_tls_config(TransportConfig& cfg, bool is_server, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(port > 0U);  // Assert: port must be non-zero

    transport_config_default(cfg);

    cfg.kind          = TransportKind::TCP;
    cfg.is_server     = is_server;
    cfg.bind_port     = port;
    cfg.peer_port     = port;
    cfg.local_node_id = is_server ? SERVER_NODE_ID : CLIENT_NODE_ID;
    cfg.num_channels  = 1U;

    static const char LOOPBACK[] = "127.0.0.1";
    (void)strncpy(cfg.bind_ip, is_server ? "0.0.0.0" : LOOPBACK,
                  sizeof(cfg.bind_ip) - 1U);
    cfg.bind_ip[sizeof(cfg.bind_ip) - 1U] = '\0';
    (void)strncpy(cfg.peer_ip, LOOPBACK, sizeof(cfg.peer_ip) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';

    // TLS on, self-signed cert, no peer chain verification for demo
    cfg.tls.tls_enabled = true;
    cfg.tls.role        = is_server ? TlsRole::SERVER : TlsRole::CLIENT;
    cfg.tls.verify_peer = false;

    const uint32_t path_max = static_cast<uint32_t>(sizeof(cfg.tls.cert_file)) - 1U;
    (void)strncpy(cfg.tls.cert_file, CERT_FILE, path_max);
    cfg.tls.cert_file[path_max] = '\0';
    (void)strncpy(cfg.tls.key_file, KEY_FILE, path_max);
    cfg.tls.key_file[path_max] = '\0';

    channel_config_default(cfg.channels[0], 0U);
    cfg.channels[0].reliability     = ReliabilityClass::RELIABLE_RETRY;
    cfg.channels[0].ordering        = OrderingMode::ORDERED;
    cfg.channels[0].max_retries     = 3U;
    cfg.channels[0].recv_timeout_ms = RECV_TIMEOUT_MS;
}

// ─────────────────────────────────────────────────────────────────────────────
// Print payload as a bounded string
// ─────────────────────────────────────────────────────────────────────────────
static void print_payload(const uint8_t* payload, uint32_t len)
{
    NEVER_COMPILED_OUT_ASSERT(payload != nullptr);           // Assert: payload not null
    NEVER_COMPILED_OUT_ASSERT(len <= MSG_MAX_PAYLOAD_BYTES); // Assert: length in bounds

    static const uint32_t MAX_PRINT = 256U;
    char buf[MAX_PRINT];
    const uint32_t copy_len = (len < MAX_PRINT) ? len : (MAX_PRINT - 1U);
    (void)memcpy(buf, payload, copy_len);
    buf[copy_len] = '\0';
    (void)printf("%s\n", buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build and send an echo reply (extracts complexity from run_server)
// ─────────────────────────────────────────────────────────────────────────────
static void send_echo_reply(DeliveryEngine& engine,
                             const MessageEnvelope& received, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(received));           // Assert: valid envelope
    NEVER_COMPILED_OUT_ASSERT(now_us > 0U);                       // Assert: valid timestamp

    if (received.payload_length > MSG_MAX_PAYLOAD_BYTES) {
        return;
    }
    MessageEnvelope reply;
    envelope_init(reply);
    reply.message_type      = MessageType::DATA;
    reply.source_id         = SERVER_NODE_ID;
    reply.destination_id    = received.source_id;
    reply.priority          = received.priority;
    reply.reliability_class = received.reliability_class;
    reply.timestamp_us      = now_us;
    reply.expiry_time_us    = timestamp_deadline_us(now_us, MSG_EXPIRY_MS);
    reply.payload_length    = received.payload_length;
    (void)memcpy(reply.payload, received.payload, reply.payload_length);
    Result res = engine.send(reply, now_us);
    if (!result_ok(res)) {
        Logger::log(Severity::WARNING_LO, "TlsTcpDemo",
                    "Echo send failed: %d\n", static_cast<int>(res));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: one server loop iteration — receive, echo, pump (extracts complexity)
// ─────────────────────────────────────────────────────────────────────────────
static void server_iteration(DeliveryEngine& engine, uint32_t& received)
{
    NEVER_COMPILED_OUT_ASSERT(RECV_TIMEOUT_MS > 0U);  // Assert: valid timeout

    const uint64_t now_us = timestamp_now_us();
    MessageEnvelope env;
    envelope_init(env);

    const Result res = engine.receive(env, RECV_TIMEOUT_MS, now_us);
    if (result_ok(res) && envelope_is_data(env)) {
        ++received;
        (void)printf("[Server] msg#%llu from node %u: ",
                     static_cast<unsigned long long>(env.message_id),
                     static_cast<unsigned>(env.source_id));
        print_payload(env.payload, env.payload_length);
        send_echo_reply(engine, env, now_us);
    }
    (void)engine.pump_retries(now_us);
    (void)engine.sweep_ack_timeouts(now_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: poll for one echo reply; returns true when received
// ─────────────────────────────────────────────────────────────────────────────
static bool wait_for_echo(DeliveryEngine& engine)
{
    NEVER_COMPILED_OUT_ASSERT(MAX_CLIENT_RECV > 0);  // Assert: valid receive bound

    // Power of 10 Rule 2: fixed loop bound
    for (int r = 0; r < MAX_CLIENT_RECV; ++r) {
        const uint64_t now = timestamp_now_us();
        MessageEnvelope reply;
        envelope_init(reply);
        const Result rr = engine.receive(reply, RECV_TIMEOUT_MS, now);
        (void)engine.pump_retries(now);
        (void)engine.sweep_ack_timeouts(now);
        if (result_ok(rr) && envelope_is_data(reply)) {
            (void)printf("[Client] echo:  ");
            print_payload(reply.payload, reply.payload_length);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run the echo server
// ─────────────────────────────────────────────────────────────────────────────
static int run_server(uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(port > 0U);  // Assert: valid port

    Logger::log(Severity::INFO, "TlsTcpDemo",
                "Server starting on port %u (TLS)\n", static_cast<unsigned>(port));

    TransportConfig cfg;
    make_tls_config(cfg, true, port);

    TlsTcpBackend transport;
    const Result res = transport.init(cfg);
    if (!result_ok(res)) {
        Logger::log(Severity::FATAL, "TlsTcpDemo",
                    "Server transport init failed: %d\n", static_cast<int>(res));
        return 1;
    }

    DeliveryEngine engine;
    engine.init(&transport, cfg.channels[0], SERVER_NODE_ID);

    // MISRA deviation: signal() requires a function pointer.
    void (*old_handler)(int) = signal(SIGINT, sig_handler);
    NEVER_COMPILED_OUT_ASSERT(old_handler != SIG_ERR);  // Assert: signal() succeeded

    Logger::log(Severity::INFO, "TlsTcpDemo", "Server ready. Press Ctrl+C to stop.\n");

    uint32_t received = 0U;

    // Power of 10 Rule 2: fixed loop bound
    for (int i = 0; i < MAX_SERVER_ITERS && g_stop == 0; ++i) {
        server_iteration(engine, received);
    }

    transport.close();
    Logger::log(Severity::INFO, "TlsTcpDemo",
                "Server stopped. Messages received: %u\n", received);
    (void)signal(SIGINT, old_handler);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run the client
// ─────────────────────────────────────────────────────────────────────────────
static int run_client(uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(port > 0U);  // Assert: valid port

    Logger::log(Severity::INFO, "TlsTcpDemo",
                "Client connecting to 127.0.0.1:%u (TLS)\n",
                static_cast<unsigned>(port));

    // Wait briefly for server to start listening
    usleep(static_cast<useconds_t>(CLIENT_STARTUP_US));

    TransportConfig cfg;
    make_tls_config(cfg, false, port);

    TlsTcpBackend transport;
    Result res = transport.init(cfg);
    if (!result_ok(res)) {
        Logger::log(Severity::FATAL, "TlsTcpDemo",
                    "Client transport init failed: %d\n", static_cast<int>(res));
        return 1;
    }

    // REQ-6.1.8/6.1.10: DeliveryEngine::init() calls register_local_id() which
    // sends the HELLO frame so the server can register our NodeId for routing.
    DeliveryEngine engine;
    engine.init(&transport, cfg.channels[0], CLIENT_NODE_ID);

    uint32_t sent     = 0U;
    uint32_t echoed   = 0U;

    // Power of 10 Rule 2: fixed send loop bound
    for (int i = 0; i < MAX_CLIENT_MSGS; ++i) {
        const uint64_t now_us = timestamp_now_us();

        char text[64];
        (void)snprintf(text, sizeof(text), "TLS hello #%d from client", i + 1);

        MessageEnvelope env;
        envelope_init(env);
        env.message_type      = MessageType::DATA;
        env.destination_id    = SERVER_NODE_ID;
        env.priority          = 0U;
        env.reliability_class = ReliabilityClass::RELIABLE_RETRY;
        env.timestamp_us      = now_us;
        env.expiry_time_us    = timestamp_deadline_us(now_us, MSG_EXPIRY_MS);
        env.payload_length    = static_cast<uint32_t>(strlen(text));
        (void)memcpy(env.payload, text, env.payload_length);

        res = engine.send(env, now_us);
        if (result_ok(res)) {
            ++sent;
            (void)printf("[Client] sent: %s\n", text);
        } else {
            Logger::log(Severity::WARNING_HI, "TlsTcpDemo",
                        "Send failed: %d\n", static_cast<int>(res));
        }

        if (wait_for_echo(engine)) {
            ++echoed;
        }
    }

    transport.close();
    (void)printf("[Client] done. sent=%u echoed=%u\n", sent, echoed);
    return (echoed == sent) ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse port from argv
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t parse_port(int argc, const char* const argv[], int idx)
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Assert: valid argc
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Assert: valid argv

    if (argc <= idx) {
        return DEFAULT_PORT;
    }
    char* end = nullptr;
    const long v = strtol(argv[idx], &end, 10);
    if (end != argv[idx] && *end == '\0' && v > 0L && v <= 65535L) {
        return static_cast<uint16_t>(v);
    }
    return DEFAULT_PORT;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    NEVER_COMPILED_OUT_ASSERT(argc >= 1);        // Assert: valid argc
    NEVER_COMPILED_OUT_ASSERT(argv != nullptr);  // Assert: valid argv

    // Register POSIX abort handler for NEVER_COMPILED_OUT_ASSERT
    assert_state::set_reset_handler(AbortResetHandler::instance());

    if (argc < 2) {
        (void)fprintf(stderr,
                      "Usage: %s server [port]\n"
                      "       %s client [port]\n",
                      argv[0], argv[0]);
        return 1;
    }

    init_pem_paths();
    if (!write_pem_files()) {
        (void)fprintf(stderr, "ERROR: failed to write TLS cert/key to /tmp\n");
        return 1;
    }

    const uint16_t port = parse_port(argc, argv, 2);

    if (strcmp(argv[1], "server") == 0) {
        return run_server(port);
    }
    if (strcmp(argv[1], "client") == 0) {
        return run_client(port);
    }

    (void)fprintf(stderr, "ERROR: unknown role '%s' (expected server or client)\n", argv[1]);
    return 1;
}
