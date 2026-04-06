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
 * @file test_TlsTcpBackend.cpp
 * @brief Unit and integration tests for TlsTcpBackend:
 *        TLS config, plaintext fallback, TLS init failures, and loopback roundtrip.
 *
 * Tests cover:
 *   - TlsConfig struct default values (incl. session_resumption_enabled=false)
 *   - transport_config_default() initialises the embedded TlsConfig
 *   - TlsTcpBackend server bind without TLS (tls_enabled=false)
 *   - TlsTcpBackend init failure when cert/key files are missing
 *   - TlsTcpBackend init with valid self-signed cert (server bind succeeds)
 *   - Full loopback message roundtrip (plaintext) using POSIX threads
 *   - Full loopback message roundtrip (TLS) using POSIX threads
 *   - TLS session resumption: default-off, save-after-handshake, reconnect cycle
 *
 * Test cert/key written to /tmp at test startup; cleaned up at exit.
 * POSIX threads (pthread) used for loopback tests; allowed by -lpthread.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - STL exempted in tests/ for test fixture setup only.
 *
 * Verifies: REQ-6.1.6, REQ-6.1.7, REQ-6.1.8, REQ-6.1.9, REQ-6.1.10,
 *           REQ-6.1.11, REQ-6.3.4, REQ-5.1.6
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "core/Types.hpp"
#include "core/TlsConfig.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Serializer.hpp"
#include "platform/TlsTcpBackend.hpp"
#include "MockSocketOps.hpp"
#if __has_include(<mbedtls/build_info.h>)
#  include <mbedtls/build_info.h>   // mbedTLS 3.x / 4.x
#else
#  include <mbedtls/version.h>      // mbedTLS 2.x
#endif
#include <mbedtls/psa_util.h>
#include <psa/crypto.h>

// ─────────────────────────────────────────────────────────────────────────────
// Embedded PEM test credentials (self-signed EC P-256, 10-year validity)
// Generated with:
//   openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256
//                  -pkeyopt ec_param_enc:named_curve -out test.key
//   openssl req -new -x509 -key test.key -out test.crt -days 3650
//               -subj "/CN=messageEngine-test"
// named_curve encoding required: mbedTLS 4.0 does not parse explicit params.
// This cert is used as BOTH the server cert AND the trusted CA cert,
// so verify_peer=true works against it (self-signed is its own CA).
// ─────────────────────────────────────────────────────────────────────────────

// Cert generated with basicConstraints=critical,CA:TRUE and keyUsage=keyCertSign
// so it can be used as both the server cert AND the trusted CA cert for verify_peer.
static const char* TEST_CERT_PEM =
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

static const char* TEST_KEY_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgE29ClO6C6vnGjspd\n"
    "6xcoCqV4wgBXN9h5hRTMN9aUkoGhRANCAASAUZcAAVgQtvvGoFY7cU7m6lFAV2lR\n"
    "F5GqzsQBY4RWI7ZfpqatCirfDrMj3FysnOz5w2155FGmUhUx0iJ2xIUQ\n"
    "-----END PRIVATE KEY-----\n";

static const char* TEST_CERT_FILE = "/tmp/me_test_tls.crt";
static const char* TEST_KEY_FILE  = "/tmp/me_test_tls.key";

static void write_pem_files()
{
    FILE* fp = fopen(TEST_CERT_FILE, "w");
    assert(fp != nullptr);
    assert(fputs(TEST_CERT_PEM, fp) >= 0);
    assert(fclose(fp) == 0);

    fp = fopen(TEST_KEY_FILE, "w");
    assert(fp != nullptr);
    assert(fputs(TEST_KEY_PEM, fp) >= 0);
    assert(fclose(fp) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — build a minimal TransportConfig for loopback tests
// ─────────────────────────────────────────────────────────────────────────────

static void make_transport_config(TransportConfig& cfg, bool is_server,
                                   uint16_t port, bool tls_on)
{
    transport_config_default(cfg);
    cfg.is_server  = is_server;
    cfg.bind_port  = port;
    cfg.peer_port  = port;
    cfg.local_node_id = is_server ? 1U : 2U;
    cfg.tls.tls_enabled = tls_on;
    if (tls_on) {
        cfg.tls.role        = is_server ? TlsRole::SERVER : TlsRole::CLIENT;
        cfg.tls.verify_peer = false;  // skip peer verify for self-signed test certs
        uint32_t len = static_cast<uint32_t>(sizeof(cfg.tls.cert_file) - 1U);
        (void)strncpy(cfg.tls.cert_file, TEST_CERT_FILE, len);
        cfg.tls.cert_file[len] = '\0';
        (void)strncpy(cfg.tls.key_file, TEST_KEY_FILE, len);
        cfg.tls.key_file[len] = '\0';
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg for loopback tests
// ─────────────────────────────────────────────────────────────────────────────

struct ClientThreadArg {
    uint16_t port;
    bool     tls_on;
    Result   result;
};

static void* client_thread_func(void* arg)
{
    ClientThreadArg* a = static_cast<ClientThreadArg*>(arg);
    assert(a != nullptr);

    usleep(80000U);  // 80 ms: wait for server to start listening

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    a->result = client.init(cfg);
    if (a->result != Result::OK) {
        return nullptr;
    }

    MessageEnvelope env;
    envelope_init(env);
    env.message_type   = MessageType::DATA;
    env.message_id     = 0xABCDULL;
    env.source_id      = 2U;
    env.destination_id = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    a->result = client.send_message(env);
    usleep(50000U);  // 50 ms: let server drain
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: tls_config_default() — struct field values
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_config_default()
{
    TlsConfig cfg;
    tls_config_default(cfg);

    assert(cfg.tls_enabled == false);
    assert(cfg.role == TlsRole::CLIENT);
    assert(cfg.cert_file[0] == '\0');
    assert(cfg.key_file[0] == '\0');
    assert(cfg.ca_file[0] == '\0');
    assert(cfg.verify_peer == true);

    printf("PASS: test_tls_config_default\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: transport_config_default() embeds a TlsConfig with safe defaults
// ─────────────────────────────────────────────────────────────────────────────

static void test_transport_config_includes_tls()
{
    TransportConfig cfg;
    transport_config_default(cfg);

    assert(cfg.tls.tls_enabled == false);
    assert(cfg.tls.verify_peer == true);
    // Plaintext default: no cert paths set
    assert(cfg.tls.cert_file[0] == '\0');
    assert(cfg.tls.key_file[0] == '\0');

    printf("PASS: test_transport_config_includes_tls\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: TlsTcpBackend server bind (tls_enabled=false) → OK → is_open → close
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_bind_plaintext()
{
    TlsTcpBackend backend;
    TransportConfig cfg;
    make_transport_config(cfg, true, 19870U, false);

    Result res = backend.init(cfg);
    assert(res == Result::OK);
    assert(backend.is_open() == true);

    backend.close();
    assert(backend.is_open() == false);

    printf("PASS: test_server_bind_plaintext\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: TlsTcpBackend init with missing cert file → ERR_IO
// ─────────────────────────────────────────────────────────────────────────────

static void test_init_bad_cert_path()
{
    TlsTcpBackend backend;
    TransportConfig cfg;
    make_transport_config(cfg, true, 19871U, true);
    // Override cert_file to a path that does not exist
    (void)strncpy(cfg.tls.cert_file, "/tmp/does_not_exist.crt",
                  static_cast<uint32_t>(sizeof(cfg.tls.cert_file)) - 1U);
    cfg.tls.cert_file[sizeof(cfg.tls.cert_file) - 1U] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_init_bad_cert_path\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: TlsTcpBackend server bind with valid cert (tls_enabled=true) → OK
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_bind_tls()
{
    TlsTcpBackend backend;
    TransportConfig cfg;
    make_transport_config(cfg, true, 19872U, true);

    Result res = backend.init(cfg);
    assert(res == Result::OK);
    assert(backend.is_open() == true);

    backend.close();
    assert(backend.is_open() == false);

    printf("PASS: test_server_bind_tls\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Plaintext loopback roundtrip (tls_enabled=false)
// Server thread receives message sent by client thread; verifies message_id.
// ─────────────────────────────────────────────────────────────────────────────

static void test_plaintext_loopback()
{
    static const uint16_t PORT = 19873U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    ClientThreadArg args;
    args.port   = PORT;
    args.tls_on = false;
    args.result = Result::ERR_IO;

    // TlsTcpBackend is ~550 KB; default pthread stack (512 KB) overflows.
    // Set 4 MB stack — allowed in tests/ per CLAUDE.md §9 (Rule 3 EXEMPT).
    pthread_attr_t attr;
    int pattr = pthread_attr_init(&attr);
    assert(pattr == 0);
    pattr = pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U);
    assert(pattr == 0);

    pthread_t tid;
    int pret = pthread_create(&tid, &attr, client_thread_func, &args);
    assert(pret == 0);
    (void)pthread_attr_destroy(&attr);

    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 3000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(recv_res == Result::OK);
    assert(args.result == Result::OK);
    assert(received.message_id == 0xABCDULL);

    printf("PASS: test_plaintext_loopback\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: TLS loopback roundtrip (tls_enabled=true, verify_peer=false)
// Same structure as test 6 but with TLS encryption on the wire.
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_loopback()
{
    static const uint16_t PORT = 19874U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    ClientThreadArg args;
    args.port   = PORT;
    args.tls_on = true;
    args.result = Result::ERR_IO;

    // TlsTcpBackend is ~550 KB; default pthread stack (512 KB) overflows.
    // Set 4 MB stack — allowed in tests/ per CLAUDE.md §9 (Rule 3 EXEMPT).
    pthread_attr_t attr;
    int pattr = pthread_attr_init(&attr);
    assert(pattr == 0);
    pattr = pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U);
    assert(pattr == 0);

    pthread_t tid;
    int pret = pthread_create(&tid, &attr, client_thread_func, &args);
    assert(pret == 0);
    (void)pthread_attr_destroy(&attr);

    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 5000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(recv_res == Result::OK);
    assert(args.result == Result::OK);
    assert(received.message_id == 0xABCDULL);

    printf("PASS: test_tls_loopback\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Echo-loopback thread arg (client sends then receives the server's reply)
// ─────────────────────────────────────────────────────────────────────────────

struct EchoClientArg {
    uint16_t port;
    bool     tls_on;
    Result   send_result;
    Result   recv_result;
    uint64_t echo_msg_id;
};

static void* echo_client_thread_func(void* raw_arg)
{
    EchoClientArg* a = static_cast<EchoClientArg*>(raw_arg);
    assert(a != nullptr);

    usleep(80000U);  // 80 ms: wait for server

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    a->send_result = client.init(cfg);
    if (a->send_result != Result::OK) {
        return nullptr;
    }

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xBEEFULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    a->send_result = client.send_message(env);
    if (a->send_result != Result::OK) {
        client.close();
        return nullptr;
    }

    // Receive echo reply — exercises client-mode poll_clients_once() (L607 False)
    MessageEnvelope reply;
    a->recv_result = client.receive_message(reply, 3000U);
    if (a->recv_result == Result::OK) {
        a->echo_msg_id = reply.message_id;
    }

    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: client init failure when nothing is listening on target port
//         Covers: L247 True (mbedtls_net_connect fails → ERR_IO)
// ─────────────────────────────────────────────────────────────────────────────

static void test_connect_bad_host()
{
    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, 19875U, false);
    // Port 19875 has no server → connect_to_server() fails at L247
    cfg.connect_timeout_ms = 500U;

    Result res = client.init(cfg);
    assert(res != Result::OK);
    assert(client.is_open() == false);

    printf("PASS: test_connect_bad_host\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: receive_message with no client connected → ERR_TIMEOUT (L734)
//         Also exercises the polling loop (L721-732): L724 False, L730 False
// ─────────────────────────────────────────────────────────────────────────────

static void test_receive_timeout()
{
    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, 19876U, false);

    Result init_res = server.init(cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    // receive_message with short timeout: no client → all pops fail → ERR_TIMEOUT
    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 300U);
    assert(recv_res == Result::ERR_TIMEOUT);  // L734

    server.close();
    printf("PASS: test_receive_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: server send_message with no clients connected → OK (L691 True)
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_send_no_clients()
{
    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, 19877U, false);

    Result init_res = server.init(cfg);
    assert(init_res == Result::OK);

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0x1234ULL;
    env.source_id         = 1U;
    env.destination_id    = 2U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    // send_message with m_client_count==0 → L691 True: discards, returns OK
    Result send_res = server.send_message(env);
    assert(send_res == Result::OK);
    assert(server.is_open() == true);

    server.close();
    printf("PASS: test_server_send_no_clients\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: Plaintext echo loopback
//   Client sends → server receives → server sends echo → client receives
//   Covers:
//     - send_to_all_clients loop body (m_client_count > 0)
//     - client-mode poll_clients_once() (L607 False)
//     - receive_message on client side
// ─────────────────────────────────────────────────────────────────────────────

static void test_echo_loopback_plaintext()
{
    static const uint16_t PORT = 19878U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    EchoClientArg args;
    args.port        = PORT;
    args.tls_on      = false;
    args.send_result = Result::ERR_IO;
    args.recv_result = Result::ERR_IO;
    args.echo_msg_id = 0U;

    // TlsTcpBackend is ~550 KB; use 4 MB stack (Rule 3 EXEMPT in tests/)
    pthread_attr_t attr;
    int pattr = pthread_attr_init(&attr);
    assert(pattr == 0);
    pattr = pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U);
    assert(pattr == 0);

    pthread_t tid;
    int pret = pthread_create(&tid, &attr, echo_client_thread_func, &args);
    assert(pret == 0);
    (void)pthread_attr_destroy(&attr);

    // Server: receive client message
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 3000U);
    assert(recv_res == Result::OK);

    // Server: send echo reply (covers send_to_all_clients loop with m_client_count=1)
    Result send_res = server.send_message(received);
    assert(send_res == Result::OK);

    // Wait for client to receive the echo before closing
    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.send_result == Result::OK);
    assert(args.recv_result == Result::OK);         // client received echo
    assert(args.echo_msg_id == 0xBEEFULL);

    printf("PASS: test_echo_loopback_plaintext\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — create a detached-attr thread with 4 MB stack (Rule 3 EXEMPT)
// ─────────────────────────────────────────────────────────────────────────────

static pthread_t create_thread_4mb(void* (*func)(void*), void* arg)
{
    pthread_attr_t attr;
    int pattr = pthread_attr_init(&attr);
    assert(pattr == 0);
    pattr = pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U);
    assert(pattr == 0);
    pthread_t tid;
    int pret = pthread_create(&tid, &attr, func, arg);
    assert(pret == 0);
    (void)pthread_attr_destroy(&attr);
    return tid;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread: TLS client that connects then immediately closes (sends close_notify)
// ─────────────────────────────────────────────────────────────────────────────

static void* tls_close_thread_func(void* raw_arg)
{
    ClientThreadArg* a = static_cast<ClientThreadArg*>(raw_arg);
    assert(a != nullptr);
    usleep(80000U);  // 80 ms: wait for server

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    a->result = client.init(cfg);
    if (a->result == Result::OK) {
        usleep(50000U);  // 50 ms: let handshake settle on server side
        client.close();  // sends ssl_close_notify then frees resources
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg + func: TLS client with configurable verify_peer and ca_file
// ─────────────────────────────────────────────────────────────────────────────

struct VerifyPeerClientArg {
    uint16_t    port;
    bool        tls_on;
    bool        verify_peer;
    const char* ca_file;        // nullptr or "" → no CA loaded
    const char* peer_hostname;  // nullptr or "" → no hostname set (skips mbedTLS 4.0 check)
    Result      result;
};

static void* verify_peer_client_func(void* raw_arg)
{
    VerifyPeerClientArg* a = static_cast<VerifyPeerClientArg*>(raw_arg);
    assert(a != nullptr);
    usleep(80000U);

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    cfg.tls.verify_peer = a->verify_peer;
    if (a->ca_file != nullptr && a->ca_file[0] != '\0') {
        uint32_t len = static_cast<uint32_t>(sizeof(cfg.tls.ca_file) - 1U);
        (void)strncpy(cfg.tls.ca_file, a->ca_file, len);
        cfg.tls.ca_file[len] = '\0';
    }
    if (a->peer_hostname != nullptr && a->peer_hostname[0] != '\0') {
        uint32_t len = static_cast<uint32_t>(sizeof(cfg.tls.peer_hostname) - 1U);
        (void)strncpy(cfg.tls.peer_hostname, a->peer_hostname, len);
        cfg.tls.peer_hostname[len] = '\0';
    }
    a->result = client.init(cfg);
    if (a->result == Result::OK) {
        MessageEnvelope env;
        envelope_init(env);
        env.message_type      = MessageType::DATA;
        env.message_id        = 0xCAFEULL;
        env.source_id         = 2U;
        env.destination_id    = 1U;
        env.reliability_class = ReliabilityClass::BEST_EFFORT;
        (void)client.send_message(env);
        usleep(50000U);
        client.close();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg + func: raw POSIX socket (optionally sends a garbage frame)
// Used for: no-TLS-handshake test (test 20) and garbage-frame test (test 21)
// ─────────────────────────────────────────────────────────────────────────────

struct RawSocketArg {
    uint16_t port;
    bool     send_garbage;
    Result   result;
};

static void* raw_socket_thread_func(void* raw_arg)
{
    RawSocketArg* a = static_cast<RawSocketArg*>(raw_arg);
    assert(a != nullptr);
    usleep(80000U);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { a->result = Result::ERR_IO; return nullptr; }

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(a->port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int ret = ::connect(fd,
                        reinterpret_cast<struct sockaddr*>(&addr),
                        static_cast<socklen_t>(sizeof(addr)));
    if (ret < 0) {
        (void)::close(fd);
        a->result = Result::ERR_IO;
        return nullptr;
    }

    if (a->send_garbage) {
        // 4-byte big-endian length=5 followed by 5 non-decodable bytes
        uint8_t buf[9U] = {0x00U, 0x00U, 0x00U, 0x05U,
                           0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
        (void)::write(fd, buf, sizeof(buf));
        usleep(200000U);  // 200 ms: let server read + attempt deserialize
    } else {
        usleep(20000U);   // 20 ms: brief pause before disconnect (no ClientHello)
    }
    (void)::close(fd);
    a->result = Result::OK;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg + func: client sends N messages then stays alive (two-client tests)
// ─────────────────────────────────────────────────────────────────────────────

struct MultiMsgClientArg {
    uint32_t delay_us;       // initial delay before connecting
    uint32_t num_msgs;       // messages to send (max 4)
    uint32_t stay_alive_us;  // how long to stay connected after sending
    uint16_t port;
    bool     tls_on;
    Result   result;
};

static void* multi_msg_client_func(void* raw_arg)
{
    static const uint32_t MAX_MSGS = 4U;
    MultiMsgClientArg* a = static_cast<MultiMsgClientArg*>(raw_arg);
    assert(a != nullptr);
    assert(a->num_msgs <= MAX_MSGS);
    usleep(a->delay_us);

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    a->result = client.init(cfg);
    if (a->result != Result::OK) { return nullptr; }

    for (uint32_t m = 0U; m < a->num_msgs; ++m) {
        MessageEnvelope env;
        envelope_init(env);
        env.message_type      = MessageType::DATA;
        env.message_id        = 0x3000ULL + static_cast<uint64_t>(m);
        env.source_id         = 2U;
        env.destination_id    = 1U;
        env.reliability_class = ReliabilityClass::BEST_EFFORT;
        (void)client.send_message(env);
    }
    usleep(a->stay_alive_us);
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: Second receive after echo — client already closed
// Covers: L612 False (m_client_count>0 at poll time, no listen wait)
//         L511 True  (recv_from_client fails on closed socket)
//         L375 False (remove_client plaintext — no ssl_close_notify)
//         L384 False (compact loop: single-client table, j<0 never entered)
// ─────────────────────────────────────────────────────────────────────────────

static void test_post_echo_remove_client_plaintext()
{
    static const uint16_t PORT = 19879U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    EchoClientArg args;
    args.port        = PORT;
    args.tls_on      = false;
    args.send_result = Result::ERR_IO;
    args.recv_result = Result::ERR_IO;
    args.echo_msg_id = 0U;

    pthread_t tid = create_thread_4mb(echo_client_thread_func, &args);

    // Server: receive, echo, wait for client to receive echo and close
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 3000U);
    assert(recv_res == Result::OK);
    Result send_res = server.send_message(received);
    assert(send_res == Result::OK);
    (void)pthread_join(tid, nullptr);

    // process_outbound() always stages messages in the delay buffer (even with
    // impairments disabled), and flush_delayed_to_clients() then delivers a
    // second copy.  Both the client and server therefore send their messages
    // twice on the wire.  The server may receive a duplicate 0xBEEF before
    // seeing the client's TCP FIN.  Drain that extra copy (if any), then make
    // a final receive that must fail with ERR_TIMEOUT once the socket is gone.

    // recv2: covers L612 False (m_client_count==1 → listen-poll skipped)
    // Result may be OK (extra copy) or ERR_TIMEOUT (socket already gone).
    MessageEnvelope env2;
    (void)server.receive_message(env2, 400U);

    // recv3: by now the client socket is closed; recv_from_client encounters
    // EOF or ECONNRESET → L511 True → remove_client in plaintext mode:
    // L375 False (no ssl_close_notify), L384 False (j<0 loop never entered)
    MessageEnvelope env3;
    Result recv3 = server.receive_message(env3, 400U);
    assert(recv3 == Result::ERR_TIMEOUT);

    server.close();
    assert(args.send_result == Result::OK);
    assert(args.recv_result == Result::OK);
    printf("PASS: test_post_echo_remove_client_plaintext\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: TLS client with verify_peer=true and valid ca_file
// Covers: L152 True  (verify_peer → MBEDTLS_SSL_VERIFY_REQUIRED)
//         L158:9  True  (verify_peer side of && evaluated True)
//         L158:32 True  (ca_file[0]!='\0' side of && evaluated True)
//         L160 False (x509_crt_parse_file(CA) succeeds)
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_verify_peer_with_ca()
{
    static const uint16_t PORT = 19880U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    // Client: verify_peer=true, ca_file=TEST_CERT_FILE (the server's self-signed cert)
    // The server cert IS the CA cert, so verification should succeed.
    // peer_hostname must match the cert's CN ("messageEngine-test") for mbedTLS 4.0+.
    VerifyPeerClientArg args;
    args.port          = PORT;
    args.tls_on        = true;
    args.verify_peer   = true;
    args.ca_file       = TEST_CERT_FILE;
    args.peer_hostname = "messageEngine-test";  // matches cert CN
    args.result        = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(verify_peer_client_func, &args);

    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 5000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    // The server cert IS the CA cert (self-signed), so peer verification succeeds.
    // Coverage points L152 True, L158:9 True, L158:32 True, L160 False are confirmed
    // (verify_peer=true → MBEDTLS_SSL_VERIFY_REQUIRED; CA cert loaded and parsed OK).
    assert(args.result == Result::OK);
    assert(recv_res == Result::OK);
    assert(received.message_id == 0xCAFEULL);
    printf("PASS: test_tls_verify_peer_with_ca\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: TLS client with verify_peer=true but no ca_file → handshake fails
// Covers: L158:32 False (ca_file[0]=='\0' short-circuits the && False)
//         L273 True   (ssl_handshake (client) fails: no CA to verify server cert)
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_verify_peer_no_ca()
{
    static const uint16_t PORT = 19881U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    // Client: verify_peer=true but ca_file="" → CA chain never loaded →
    // TLS handshake attempts peer verification with no trusted CA → fails.
    VerifyPeerClientArg args;
    args.port          = PORT;
    args.tls_on        = true;
    args.verify_peer   = true;
    args.ca_file       = "";    // empty: ca_file[0]=='\0' → L158:32 False
    args.peer_hostname = "messageEngine-test";  // set hostname; chain validation still fails
    args.result        = Result::OK;  // expect ERR_IO

    pthread_t tid = create_thread_4mb(verify_peer_client_func, &args);

    MessageEnvelope received;
    // Server: handshake will fail on the client side; server sees no message.
    Result recv_res = server.receive_message(received, 3000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::ERR_IO);   // L273 True: ssl_handshake (client) failed
    assert(recv_res == Result::ERR_TIMEOUT); // no message received
    printf("PASS: test_tls_verify_peer_no_ca\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: TLS init with bad private key file → pk_parse_keyfile fails
// Covers: L176 True (mbedtls_pk_parse_keyfile returns non-zero)
// No server needed — failure occurs inside setup_tls_config before bind/connect.
// ─────────────────────────────────────────────────────────────────────────────

static void test_init_bad_key_path()
{
    TlsTcpBackend backend;
    TransportConfig cfg;
    make_transport_config(cfg, true, 0U, true);   // TLS server (port irrelevant)

    // Valid cert, non-existent key file → pk_parse_keyfile fails (L176 True)
    (void)strncpy(cfg.tls.key_file, "/tmp/does_not_exist.key",
                  static_cast<uint32_t>(sizeof(cfg.tls.key_file)) - 1U);
    cfg.tls.key_file[sizeof(cfg.tls.key_file) - 1U] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);
    printf("PASS: test_init_bad_key_path\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: Server with num_channels=0 + 4-digit port (9100)
// Covers: L644 False  (config.num_channels > 0U → False in init())
//         L547 False  (m_cfg.num_channels > 0U → False in send_to_all_clients())
//         L52:41 False (port_to_str loop: val==0 at i==4, loop exits via val>0U False)
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_num_channels_zero()
{
    // 4-digit port: port_to_str(9100,...) → loop exits when val==0 at i==4,
    // i.e. the condition (val > 0U) is False before (i < 5U) is False → L52:41 False.
    static const uint16_t PORT = 9100U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    srv_cfg.num_channels = 0U;   // L644 False in init(), L547 False in send_to_all_clients()

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    ClientThreadArg args;
    args.port   = PORT;
    args.tls_on = false;
    args.result = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(client_thread_func, &args);

    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 3000U);
    assert(recv_res == Result::OK);

    // send_to_all_clients: num_channels==0 → timeout uses 1000U fallback (L547 False)
    Result send_res = server.send_message(received);
    assert(send_res == Result::OK);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_server_num_channels_zero\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: receive_message with timeout > 5000 ms + TLS client that closes
// Covers: L718 True  (poll_count = (5100+99)/100 = 51 > 50U → capped at 50)
//         L454 True  (ssl_read returns PEER_CLOSE_NOTIFY ≤ 0)
//         L455 True  (ret != MBEDTLS_ERR_SSL_WANT_READ)
//         L375 True  (remove_client with m_tls_enabled=true → ssl_close_notify)
// NOTE: timeout_ms=5100 → 50 × 100 ms poll iterations ≈ 5 s elapsed.
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_client_close_and_long_timeout()
{
    static const uint16_t PORT = 19882U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    ClientThreadArg args;
    args.port   = PORT;
    args.tls_on = true;
    args.result = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(tls_close_thread_func, &args);

    // 5100 ms timeout → poll_count = 51 > 50 → L718 True (capped to 50 iters).
    // Client connects (TLS), then closes → server reads close_notify:
    //   L454 True (ssl_read ≤ 0), L455 True (not WANT_READ) → remove_client:
    //   L375 True (tls_enabled → ssl_close_notify + ssl_free)
    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 5100U);
    assert(recv_res == Result::ERR_TIMEOUT);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_tls_client_close_and_long_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: Server bind on port 0 (OS assigns ephemeral port)
// Covers: L48 True (port_to_str: val==0U → write '0' literal branch)
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_bind_port_zero()
{
    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, 0U, false);  // port 0 → L48 True in port_to_str

    Result res = server.init(cfg);
    assert(res == Result::OK);       // OS assigns an ephemeral port
    assert(server.is_open() == true);

    server.close();
    printf("PASS: test_server_bind_port_zero\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: TLS server + raw POSIX socket that closes without TLS handshake
// Covers: L344 True (ssl_handshake (accept) fails — no ClientHello, only EOF)
// ─────────────────────────────────────────────────────────────────────────────

static void test_raw_socket_no_tls_handshake()
{
    static const uint16_t PORT = 19883U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    RawSocketArg args;
    args.port        = PORT;
    args.send_garbage = false;
    args.result      = Result::ERR_IO;

    // Thread: raw socket connects then immediately closes — no TLS ClientHello.
    // Server accept_and_handshake: ssl_handshake fails → L344 True → ERR_IO
    // (accept_and_handshake return value is discarded by poll_clients_once).
    pthread_t tid = create_thread_4mb(raw_socket_thread_func, &args);

    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 2000U);
    assert(recv_res == Result::ERR_TIMEOUT);  // handshake failed; no message queued

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_raw_socket_no_tls_handshake\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: Plaintext server + raw socket sends valid length-prefix but garbage payload
// Covers: L521 True (Serializer::deserialize fails on the 5-byte garbage payload)
// ─────────────────────────────────────────────────────────────────────────────

static void test_garbage_frame_deserialize_fail()
{
    static const uint16_t PORT = 19884U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    RawSocketArg args;
    args.port         = PORT;
    args.send_garbage = true;   // sends [0,0,0,5, 0x01..0x05]: valid length, garbage body
    args.result       = Result::ERR_IO;

    // Thread sends length=5 + 5 garbage bytes. tcp_recv_frame reads it OK, then
    // Serializer::deserialize fails → L521 True (recv_from_client returns ERR_INVALID).
    pthread_t tid = create_thread_4mb(raw_socket_thread_func, &args);

    MessageEnvelope env;
    // The deserialize error is logged; recv_from_client returns ERR_INVALID.
    // poll_clients_once discards the return; receive_message loops until timeout.
    Result recv_res = server.receive_message(env, 1500U);
    assert(recv_res == Result::ERR_TIMEOUT);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_garbage_frame_deserialize_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: Two clients — both messages pushed in one poll cycle
// Covers: L713 True (queue already has item at start of receive_message)
//
// Sequence:
//   Thread1 sends 2 msgs; Thread2 sends 1 msg; each send_message places 1 copy
//   on the wire (HAZ-003 double-send fix: process_outbound queues once; no
//   separate direct send).
//   Call A — accepts Thread1 (slot 0), reads Thread1 msg1 → push → pop → return.
//            Thread2 remains in the accept backlog.
//   Call B — accepts Thread2 (slot 1, m_client_count→2); backwards loop:
//            recv_from_client(1) → Thread2 msg1 → push;
//            recv_from_client(0) → Thread1 msg2 → push.
//            Queue = [Thread2_msg1, Thread1_msg2].  Pop Thread2_msg1 → return.
//            Thread1_msg2 remains in the recv_queue.
//   Call C — L713 True: recv_queue non-empty at entry → pop Thread1_msg2 immediately.
// ─────────────────────────────────────────────────────────────────────────────

static void test_two_client_queue_prefill()
{
    static const uint16_t PORT = 19885U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    // Thread1 sends 2 msgs; Thread2 sends 1 msg.  Each send_message places
    // exactly 1 copy on the wire (HAZ-003 double-send fix).
    MultiMsgClientArg arg1;
    arg1.port          = PORT;
    arg1.tls_on        = false;
    arg1.delay_us      = 0U;       // connect as soon as the thread is scheduled
    arg1.num_msgs      = 2U;       // 2 msgs → 2 copies on wire (1 per send)
    arg1.stay_alive_us = 500000U;  // stay alive 500 ms
    arg1.result        = Result::ERR_IO;

    MultiMsgClientArg arg2;
    arg2.port          = PORT;
    arg2.tls_on        = false;
    arg2.delay_us      = 0U;       // connect immediately
    arg2.num_msgs      = 1U;
    arg2.stay_alive_us = 500000U;
    arg2.result        = Result::ERR_IO;

    pthread_t t1 = create_thread_4mb(multi_msg_client_func, &arg1);
    pthread_t t2 = create_thread_4mb(multi_msg_client_func, &arg2);

    // Let both threads connect and send before the first receive_message call.
    // Both TCP connections will be queued in the kernel backlog.
    usleep(50000U);  // 50 ms

    // Call A: poll listen → accept Thread1 (first in backlog); read Thread1 msg1
    //         → push → pop → return.  Thread2 remains in the accept backlog.
    MessageEnvelope envA;
    Result resA = server.receive_message(envA, 3000U);
    assert(resA == Result::OK);

    // Call B: m_client_count=1; accept Thread2 (slot 1, m_client_count→2);
    //   backwards loop: recv_from_client(1) → Thread2 msg1 → push;
    //                   recv_from_client(0) → Thread1 msg2 → push.
    //   Queue = [Thread2_msg1, Thread1_msg2].  Pop Thread2_msg1 → return.
    //   Thread1_msg2 remains in the recv_queue.
    MessageEnvelope envB;
    Result resB = server.receive_message(envB, 3000U);
    assert(resB == Result::OK);

    // Call C: recv_queue is non-empty at entry → L713 True → pop Thread1_msg2
    //         immediately without polling.
    MessageEnvelope envC;
    Result resC = server.receive_message(envC, 3000U);
    assert(resC == Result::OK);   // L713 True: popped without polling

    server.close();
    (void)pthread_join(t1, nullptr);
    (void)pthread_join(t2, nullptr);

    assert(arg1.result == Result::OK);
    assert(arg2.result == Result::OK);
    printf("PASS: test_two_client_queue_prefill\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: Two clients — remove_client on slot 0 while slot 1 exists
// Covers: L384 True (compact loop: j==0 < m_client_count-1==1 → body executes)
//         L375 False (plaintext path — no ssl_close_notify)
//
// Sequence:
//   Thread1 connects at 5 ms, sends 1 msg, closes immediately.
//   TCP stream for Thread1: [msg1][FIN].
//   Thread2 connects at 50 ms, sends 2 msgs, stays alive 1 s.
//   (each send_message places 1 copy on the wire — HAZ-003 double-send fix)
//
//   Call A — accepts Thread1 (slot 0), reads Thread1 msg1 → push → pop → return.
//   Call B — main thread sleeps 100 ms so Thread2 is in the TCP backlog;
//             accepts Thread2 (slot 1, m_client_count→2);
//             backwards: recv(1)→Thread2 msg1 push;
//                        recv(0)→Thread1 FIN → remove_client(0):
//               L375 False (TLS off), L384 True (j=0 < m_client_count-1=1) →
//               compact: Thread2 shifts slot 1 → slot 0; m_client_count→1.
//             pop Thread2_msg1 → return.
//   Call C — queue empty; poll Thread2 (slot 0 after compact); reads Thread2 msg2
//            → push → pop → return.
// ─────────────────────────────────────────────────────────────────────────────

static void test_two_client_compact_loop()
{
    static const uint16_t PORT = 19886U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    // Thread1: connect at 5 ms, send 1 msg, close immediately.
    MultiMsgClientArg arg1;
    arg1.port          = PORT;
    arg1.tls_on        = false;
    arg1.delay_us      = 5000U;   // 5 ms: ensure Thread1 is accepted before Thread2
    arg1.num_msgs      = 1U;
    arg1.stay_alive_us = 0U;      // close immediately after sending
    arg1.result        = Result::ERR_IO;

    // Thread2: connect at 50 ms, send 2 msgs, stay alive 1 s.
    MultiMsgClientArg arg2;
    arg2.port          = PORT;
    arg2.tls_on        = false;
    arg2.delay_us      = 50000U;  // 50 ms: connect after Thread1 is in slot 0
    arg2.num_msgs      = 2U;      // 2 msgs; each places 1 copy on the wire
    arg2.stay_alive_us = 1000000U;
    arg2.result        = Result::ERR_IO;

    pthread_t t1 = create_thread_4mb(multi_msg_client_func, &arg1);
    pthread_t t2 = create_thread_4mb(multi_msg_client_func, &arg2);

    // Call A: Thread1 connects at 5 ms; server accepts it (slot 0); reads msg1.
    MessageEnvelope envA;
    Result resA = server.receive_message(envA, 3000U);
    assert(resA == Result::OK);

    // Pause: Thread2 connects at 50 ms. Let it reach the backlog before Call B.
    usleep(100000U);  // 100 ms

    // Call B: accept Thread2 (slot 1, m_client_count→2); backwards loop:
    //   recv(1) → Thread2 msg1 → push;
    //   recv(0) → Thread1 FIN → remove_client(0)
    //     [L375 False: TLS off; L384 True: j=0 < m_client_count-1=1 → compact].
    //   Thread2 shifts slot 1 → slot 0; m_client_count→1.
    //   Pop Thread2_msg1 → return.
    MessageEnvelope envB;
    Result resB = server.receive_message(envB, 3000U);
    assert(resB == Result::OK);

    // Call C: queue empty; poll Thread2 (slot 0 after compact); reads Thread2 msg2
    //         → push → pop → return.
    MessageEnvelope envC;
    Result resC = server.receive_message(envC, 3000U);
    assert(resC == Result::OK);   // L384 True covered in Call B

    server.close();
    (void)pthread_join(t1, nullptr);
    (void)pthread_join(t2, nullptr);

    assert(arg1.result == Result::OK);
    assert(arg2.result == Result::OK);
    printf("PASS: test_two_client_compact_loop\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 23: TLS client init with verify_peer=true and non-existent ca_file path
// Covers: L160 True (x509_crt_parse_file(CA) returns non-zero for missing file)
// No server needed — failure inside setup_tls_config before connect_to_server.
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_bad_ca_file()
{
    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, 0U, true);  // TLS client
    cfg.tls.verify_peer = true;

    // Non-existent CA file: x509_crt_parse_file fails → L160 True → ERR_IO
    (void)strncpy(cfg.tls.ca_file, "/tmp/no_such_ca.pem",
                  static_cast<uint32_t>(sizeof(cfg.tls.ca_file)) - 1U);
    cfg.tls.ca_file[sizeof(cfg.tls.ca_file) - 1U] = '\0';

    Result res = client.init(cfg);
    assert(res == Result::ERR_IO);
    assert(client.is_open() == false);
    printf("PASS: test_tls_bad_ca_file\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 24: Server bind fails because port is already in use (no SO_REUSEADDR fix)
// Covers: L211 True (mbedtls_net_bind returns non-zero → ERR_IO)
// ─────────────────────────────────────────────────────────────────────────────

static void test_bind_port_in_use()
{
    static const uint16_t PORT = 19890U;

    // Bind a raw POSIX socket to the port first (without SO_REUSEADDR so it
    // holds the port exclusively), then listen → port is taken.
    int blocker = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(blocker >= 0);

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT);
    // Bind to the same address TlsTcpBackend uses (127.0.0.1 from
    // transport_config_default); INADDR_ANY would be a different address on
    // macOS and the second bind would succeed, defeating the test.
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int ret = ::bind(blocker,
                     reinterpret_cast<struct sockaddr*>(&addr),
                     static_cast<socklen_t>(sizeof(addr)));
    assert(ret == 0);
    ret = ::listen(blocker, 1);
    assert(ret == 0);

    // Now TlsTcpBackend tries to bind the same address → mbedtls_net_bind fails
    // (EADDRINUSE even with SO_REUSEADDR on an active LISTEN socket) → L211 True
    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, PORT, false);
    Result res = server.init(cfg);
    assert(res == Result::ERR_IO);
    assert(server.is_open() == false);

    (void)::close(blocker);
    printf("PASS: test_bind_port_in_use\n");
}


// ─────────────────────────────────────────────────────────────────────────────
// Test 25: Server send_message with 0 clients pre-stages message in delay buffer;
//          receive_message calls flush_delayed_to_clients — with 0 clients the
//          delayed envelope is discarded and receive_message returns ERR_TIMEOUT.
//
// NOTE: The former version expected OK + message_id==0x4000 because
//       flush_delayed_to_queue() incorrectly looped back the outbound delayed
//       envelope into m_recv_queue (self-loop bug). After the fix,
//       flush_delayed_to_clients() is called instead: with m_client_count==0 it
//       sends to no one and the message is discarded.
//
// Sequence:
//   server.send_message with m_client_count==0: process_outbound stages msg
//   to delay buf (release_us=now_us); early return since no clients.
//   server.receive_message: L712 pop empty → poll loop:
//     poll_clients_once(100ms): m_client_count==0 → waits on listen (100ms,
//       no connection pending) → accept_and_handshake EAGAIN → 0-client loop;
//     L724 pop empty → L725 False;
//     flush_delayed_to_clients(now_us): collect_deliverable returns 1 →
//       send_to_all_clients with 0 clients → no-op; message discarded;
//     poll loop times out → return ERR_TIMEOUT.
// ─────────────────────────────────────────────────────────────────────────────

static void test_server_send_before_client_connects()
{
    static const uint16_t PORT = 19891U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    // Build a valid envelope: type!=INVALID, source_id!=0, payload_length=0
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0x4000ULL;
    env.source_id         = 10U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.timestamp_us      = 1U;
    env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;

    // send_message: process_outbound stages to delay buf (release_us=now_us);
    // m_client_count==0 → early return; delay buf retains message.
    Result send_res = server.send_message(env);
    assert(send_res == Result::OK);

    // receive_message: poll loop → flush_delayed_to_clients → 0 clients → no-op;
    // message is discarded; receive_message returns ERR_TIMEOUT (not OK).
    // Bug fix: the former self-loop behavior (flush_delayed_to_queue pushing
    // outbound delayed messages into m_recv_queue) is intentionally removed.
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 300U);
    assert(recv_res == Result::ERR_TIMEOUT);

    server.close();
    printf("PASS: test_server_send_before_client_connects\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 26: 9 simultaneous clients — accept fills MAX_TCP_CONNECTIONS (8)
// Covers: L307:9 True (m_client_count >= MAX_TCP_CONNECTIONS → table full)
//
// Sequence:
//   9 client threads connect simultaneously (all in TCP backlog).
//   Server's receive_message loop calls poll_clients_once repeatedly.
//   Each poll accepts 1 client; after 8 accepts m_client_count==8.
//   On the 9th poll: accept_and_handshake sees 8>=8 → L307 True → returns OK.
//
// Data budget: num_msgs=4 → 8 wire copies per client.  Client i (slot i) is
// polled i+1 times by the time poll 8 runs; each poll reads exactly 1 copy.
// Client 0 (polled 8 times) consumes all 8 copies in poll 8 without timing
// out, so m_client_count stays at 8 through the end of that backward loop.
// ─────────────────────────────────────────────────────────────────────────────

static void test_max_connections_exceeded()
{
    static const uint16_t PORT = 19892U;
    static const uint32_t N_CLIENTS = 9U;   // one more than MAX_TCP_CONNECTIONS

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    MultiMsgClientArg args[N_CLIENTS];
    pthread_t         tids[N_CLIENTS];

    // Launch all 9 clients concurrently; each sends 4 msgs (8 wire copies).
    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        args[k].port          = PORT;
        args[k].tls_on        = false;
        args[k].delay_us      = 0U;
        args[k].num_msgs      = 4U;       // 4 msgs × 2 copies (double-send) = 8 frames
        args[k].stay_alive_us = 500000U;  // 500 ms: stay alive while server polls
        args[k].result        = Result::ERR_IO;
        tids[k] = create_thread_4mb(multi_msg_client_func, &args[k]);
    }

    // Wait for all 9 to complete the TCP 3-way handshake (kernel backlog).
    usleep(100000U);  // 100 ms

    // Drain enough messages to accept all 8 slots and trigger L307 on slot 8.
    // accept_and_handshake is called on every poll_clients_once invocation;
    // after 8 accepts m_client_count==8 and the 9th call fires L307 True.
    // The pattern of L713-True cache hits means ~37 receive_message calls
    // are needed; 50 gives a comfortable margin.
    for (uint32_t k = 0U; k < 50U; ++k) {
        MessageEnvelope env;
        (void)server.receive_message(env, 300U);
    }

    server.close();
    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        (void)pthread_join(tids[k], nullptr);
    }

    // All clients' TCP connect() succeeds at kernel level even before accept();
    // the 9th client may not have been accepted by the server but its init() OK.
    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        assert(args[k].result == Result::OK);
    }
    printf("PASS: test_max_connections_exceeded\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread helper: raw TLS client that sends a 4-byte zero-length frame header
// ─────────────────────────────────────────────────────────────────────────────

struct TlsZeroFrameArg {
    uint16_t port;
    Result   result;
};

static void* tls_zero_frame_func(void* raw)
{
    TlsZeroFrameArg* a = static_cast<TlsZeroFrameArg*>(raw);

    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;

    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    // PSA crypto is already initialised by the server's setup_tls_config().
    // Calling it again is idempotent and safe.
    (void)psa_crypto_init();

    // Connect to server
    char port_str[8U];
    (void)snprintf(port_str, sizeof(port_str), "%u",
                   static_cast<unsigned>(a->port));
    int ret = mbedtls_net_connect(&net, "127.0.0.1", port_str,
                                  MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        a->result = Result::ERR_IO;
        mbedtls_net_free(&net);
        return nullptr;
    }

    // Configure TLS as client; no peer verification needed
    ret = mbedtls_ssl_config_defaults(&conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        a->result = Result::ERR_IO;
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
        return nullptr;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    // mbedTLS 4.0 removed mbedtls_ssl_conf_rng() — PSA RNG is auto-bound after psa_crypto_init().
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(&conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
#endif

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        a->result = Result::ERR_IO;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
        return nullptr;
    }
    mbedtls_ssl_set_bio(&ssl, &net,
                        mbedtls_net_send, mbedtls_net_recv, nullptr);

    // TLS handshake
    do {
        ret = mbedtls_ssl_handshake(&ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        a->result = Result::ERR_IO;
        (void)mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
        return nullptr;
    }

    // Send 4-byte zero-length frame header: payload_len == 0 → L466:9 True
    static const uint8_t ZERO_HDR[4U] = {0U, 0U, 0U, 0U};
    do {
        ret = mbedtls_ssl_write(&ssl, ZERO_HDR, sizeof(ZERO_HDR));
    } while (ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    usleep(200000U);  // 200 ms: let server read and reject the frame

    (void)mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&net);

    a->result = (ret == static_cast<int>(sizeof(ZERO_HDR))) ? Result::OK
                                                             : Result::ERR_IO;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 27: TLS client sends zero-length frame header after TLS handshake
// Covers: L466:9 True (payload_len == 0U → tls_recv_frame rejects and returns false)
//
// Sequence:
//   TLS server accepts client; TLS handshake completes.
//   Client calls mbedtls_ssl_write([0,0,0,0]) → server reads 4-byte header →
//   payload_len = 0 → L466:9 True → tls_recv_frame returns false →
//   recv_from_client: remove_client() → server continues.
//   receive_message: no valid message queued → ERR_TIMEOUT.
// ─────────────────────────────────────────────────────────────────────────────

static void test_tls_zero_length_frame()
{
    static const uint16_t PORT = 19893U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);  // TLS server
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);

    TlsZeroFrameArg args;
    args.port   = PORT;
    args.result = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(tls_zero_frame_func, &args);

    // Server: zero-length frame → L466:9 True → no valid message queued.
    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 2000U);
    assert(recv_res == Result::ERR_TIMEOUT);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_tls_zero_length_frame\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread helper: client connects then sends a configurable number of messages
// (no hard cap).  Used only by test_recv_queue_overflow.
// ─────────────────────────────────────────────────────────────────────────────

struct HeavySenderArg {
    uint32_t num_msgs;      // how many messages to send
    uint32_t stay_alive_us; // how long to stay alive after sending
    uint16_t port;
    Result   result;
};

static void* heavy_sender_func(void* raw_arg)
{
    HeavySenderArg* a = static_cast<HeavySenderArg*>(raw_arg);
    assert(a != nullptr);
    assert(a->num_msgs > 0U);

    usleep(80000U);  // 80 ms: wait for server to be ready

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, false);  // plaintext client
    a->result = client.init(cfg);
    if (a->result != Result::OK) { return nullptr; }

    // Power of 10 Rule 2 deviation (test loop): bounded by a->num_msgs;
    // runtime count controlled by test fixture.
    for (uint32_t m = 0U; m < a->num_msgs; ++m) {
        MessageEnvelope env;
        envelope_init(env);
        env.message_type      = MessageType::DATA;
        env.message_id        = 0x7000ULL + static_cast<uint64_t>(m);
        env.source_id         = 2U;
        env.destination_id    = 1U;
        env.reliability_class = ReliabilityClass::BEST_EFFORT;
        env.timestamp_us      = 1U;
        env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;
        (void)client.send_message(env);
    }

    usleep(a->stay_alive_us);
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 28: DI constructor TlsTcpBackend(ISocketOps&) — exercises the
//          injection-constructor loop (lines ~108-125) and verifies that
//          normal init() succeeds when the injected mock is not on the
//          mbedTLS socket path (plaintext server bind uses mbedtls_net_bind
//          directly; mock is only consulted on the plaintext send/recv path).
//
// Covers: DI constructor body including the MAX_TCP_CONNECTIONS loop.
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.1
// Verification: M1 + M2 + M4 + M5 (fault injection via ISocketOps)
static void test_di_constructor_executes()
{
    MockSocketOps mock;

    // Construct using the DI constructor — this is the branch that was never
    // called; it exercises the constructor loop (i < MAX_TCP_CONNECTIONS).
    TlsTcpBackend backend(mock);

    assert(!backend.is_open());  // precondition: not yet initialised

    // Plain server bind: mbedtls_net_bind uses POSIX sockets directly, so
    // the mock is not involved here and the bind should succeed.
    TransportConfig cfg;
    make_transport_config(cfg, true, 19894U, false);

    Result res = backend.init(cfg);
    assert(res == Result::OK);
    assert(backend.is_open() == true);

    backend.close();
    assert(backend.is_open() == false);

    printf("PASS: test_di_constructor_executes\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 29: recv_queue full — recv_from_client() push fails (line ~551 True)
//
// Strategy: connect MAX_TCP_CONNECTIONS clients so that each poll_clients_once
// call pushes up to 8 messages before receive_message pops 1.  After enough
// iterations the queue overflows (capacity = MSG_RING_CAPACITY = 64) and
// recv_queue.push() returns ERR_FULL, triggering the WARNING_HI log path.
//
// Each client sends 20 messages.  With 8 clients, net queue growth per
// receive_message call is ≈ 7 (push 8, pop 1).  After ~10 calls the ring
// is full and the 65th push fires the overflow branch.
//
// The test only verifies that the server drains all available messages
// without crashing — the overflow path is silent (WARNING_HI log only).
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.3
static void test_recv_queue_overflow()
{
    static const uint16_t PORT      = 19895U;
    static const uint32_t N_CLIENTS = 8U;    // fill all TCP slots
    // Each client sends 20 messages.  send_message() also calls
    // flush_delayed_to_clients() which re-sends from the delay buffer, so
    // each logical send_message produces 2 wire frames.  Total frames per
    // client = 40.  With 8 clients, each poll_clients_once pushes up to 8
    // frames per iteration; receive_message pops 1.  After ~10 iterations
    // the queue (capacity MSG_RING_CAPACITY = 64) overflows, triggering the
    // push-fail WARNING_HI branch in recv_from_client().
    static const uint32_t N_MSGS    = 20U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    // Launch N_CLIENTS concurrently; each sends N_MSGS messages then stays alive
    // long enough for the server to flood its recv_queue.
    HeavySenderArg args[N_CLIENTS];
    pthread_t      tids[N_CLIENTS];

    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        args[k].port          = PORT;
        args[k].num_msgs      = N_MSGS;
        args[k].stay_alive_us = 2000000U;  // 2 s — stay alive during drain
        args[k].result        = Result::ERR_IO;
        tids[k] = create_thread_4mb(heavy_sender_func, &args[k]);
    }

    // Give all clients time to connect and fill their TCP send buffers.
    usleep(300000U);  // 300 ms

    // Drain: each receive_message pops 1 but poll_clients_once may push up to
    // N_CLIENTS before that pop.  After enough iterations the queue fills and
    // the push-fail (overflow) branch is hit.  We drain with a large iteration
    // count to ensure the overflow path fires and the server handles it cleanly.
    // Power of 10 Rule 2 deviation (test loop): bounded by 400 iterations;
    // runtime termination after all messages consumed.
    for (uint32_t k = 0U; k < 400U; ++k) {
        MessageEnvelope env;
        (void)server.receive_message(env, 100U);
    }

    server.close();

    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        (void)pthread_join(tids[k], nullptr);
    }

    for (uint32_t k = 0U; k < N_CLIENTS; ++k) {
        assert(args[k].result == Result::OK);
    }

    printf("PASS: test_recv_queue_overflow\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 30: send_message() ERR_IO loss-impairment path (line ~709 True)
//
// Configure channel 0 with loss_probability=1.0 and enabled=true.
// After init(), send_message() calls process_outbound() which returns ERR_IO
// (message dropped by loss impairment).  send_message() maps ERR_IO to OK
// (silent drop).  The receiver gets nothing → ERR_TIMEOUT.
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-5.1.3
static void test_send_impairment_loss_drop()
{
    static const uint16_t PORT = 19896U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);

    // Enable loss impairment at 100% on channel 0.
    // ImpairmentEngine::init() is called with this config in
    // TlsTcpBackend::init() at line ~644-647.
    srv_cfg.channels[0].impairment.enabled          = true;
    srv_cfg.channels[0].impairment.loss_probability = 1.0;

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    // Build a valid envelope to send.
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0x5000ULL;
    env.source_id         = 1U;
    env.destination_id    = 2U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.timestamp_us      = 1U;
    env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;

    // send_message: process_outbound returns ERR_IO (loss) → L709 True →
    // returns Result::OK (silent drop).
    Result send_res = server.send_message(env);
    assert(send_res == Result::OK);  // OK means "silently dropped"

    // Verify nothing was queued: receive_message must timeout.
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 300U);
    assert(recv_res == Result::ERR_TIMEOUT);

    server.close();
    printf("PASS: test_send_impairment_loss_drop\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 31: tls_config_default() initialises new session-resumption fields.
//          Verifies backward-compatibility: session_resumption_enabled is false
//          and session_ticket_lifetime_s is 86400 (24 h) by default.
//          Also verifies that tls_config_default does NOT enable resumption so
//          existing callers that never touch the field are unaffected.
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.3.4
// Verification: M1 + M2 + M4
static void test_session_resumption_disabled_by_default()
{
    TlsConfig cfg;
    tls_config_default(cfg);

    // New fields must be off-by-default for backward compatibility.
    assert(cfg.session_resumption_enabled == false);
    assert(cfg.session_ticket_lifetime_s  == 86400U);

    // Sanity-check that the other fields are still correct.
    assert(cfg.tls_enabled  == false);
    assert(cfg.verify_peer  == true);

    printf("PASS: test_session_resumption_disabled_by_default\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg: client connects with session_resumption_enabled, sends one
// message, stores the Result so the main thread can inspect it.
// ─────────────────────────────────────────────────────────────────────────────

struct SessionClientArg {
    uint16_t port;
    Result   init_result;
    Result   send_result;
};

static void* session_client_func(void* raw_arg)
{
    SessionClientArg* a = static_cast<SessionClientArg*>(raw_arg);
    assert(a != nullptr);

    usleep(80000U);  // 80 ms: let server bind

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, true);
    // Enable session resumption on the client side.
    cfg.tls.session_resumption_enabled = true;
    a->init_result = client.init(cfg);
    if (a->init_result != Result::OK) {
        return nullptr;
    }

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xCAFEULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    a->send_result = client.send_message(env);
    usleep(50000U);  // 50 ms: let server drain
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 32: session saved after a successful TLS loopback handshake.
//          The client enables session_resumption_enabled=true.  After a full
//          TLS handshake and a successful send, tls_connect_handshake() calls
//          try_save_client_session() which should log "TLS session saved ...".
//          We verify the observable side-effect: the server receives the
//          message (proving the handshake and send both completed), which is
//          only possible if init() → tls_connect_handshake() succeeded and did
//          not abort after try_save_client_session().
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.3.4
// Verification: M1 + M2 + M4
static void test_tls_session_saved_after_handshake()
{
    static const uint16_t PORT = 19897U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    // Enable session tickets on server so it can service resuming clients.
    srv_cfg.tls.session_resumption_enabled = true;

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    SessionClientArg args;
    args.port        = PORT;
    args.init_result = Result::ERR_IO;
    args.send_result = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(session_client_func, &args);

    // Server waits up to 5 s for the message from the client.
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 5000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    // Verify the full handshake + send completed successfully on both sides.
    assert(args.init_result == Result::OK);
    assert(args.send_result == Result::OK);
    assert(recv_res == Result::OK);
    assert(received.message_id == 0xCAFEULL);

    printf("PASS: test_tls_session_saved_after_handshake\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread arg: client reconnects a second time expecting session resumption.
// ─────────────────────────────────────────────────────────────────────────────

struct ResumeClientArg {
    uint16_t port;
    Result   first_init;
    Result   second_init;
    Result   second_send;
};

static void* resume_client_func(void* raw_arg)
{
    ResumeClientArg* a = static_cast<ResumeClientArg*>(raw_arg);
    assert(a != nullptr);

    usleep(80000U);  // 80 ms: let server bind

    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, true);
    cfg.tls.session_resumption_enabled = true;

    // ── First connection: full handshake, save session ────────────────────
    // TlsTcpBackend is designed for single-lifecycle: init once, use, close.
    // Use a local scope to fully destroy the first instance before the second.
    {
        TlsTcpBackend client1;
        a->first_init = client1.init(cfg);
        if (a->first_init != Result::OK) {
            return nullptr;
        }
        MessageEnvelope env1;
        envelope_init(env1);
        env1.message_type      = MessageType::DATA;
        env1.message_id        = 0xD001ULL;
        env1.source_id         = 2U;
        env1.destination_id    = 1U;
        env1.reliability_class = ReliabilityClass::BEST_EFFORT;
        (void)client1.send_message(env1);
        usleep(50000U);  // give server time to receive
        client1.close();
    }  // client1 destructor frees all mbedTLS resources

    // ── Second connection: fresh TlsTcpBackend with session_resumption_enabled
    // A new backend instance starts with m_session_saved=false (no prior
    // session to present); it performs a full handshake and saves the new
    // session.  This verifies the reconnect path does not crash or leak.
    usleep(30000U);  // 30 ms: brief pause before reconnect
    {
        TlsTcpBackend client2;
        a->second_init = client2.init(cfg);
        if (a->second_init != Result::OK) {
            return nullptr;
        }
        MessageEnvelope env2;
        envelope_init(env2);
        env2.message_type      = MessageType::DATA;
        env2.message_id        = 0xD002ULL;
        env2.source_id         = 2U;
        env2.destination_id    = 1U;
        env2.reliability_class = ReliabilityClass::BEST_EFFORT;
        a->second_send = client2.send_message(env2);
        usleep(50000U);
        client2.close();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 33: reconnect with session_resumption_enabled cycles cleanly.
//          First connection completes a full TLS handshake.  After close(),
//          m_session_saved is reset to false.  Second init() performs a new
//          full handshake (because m_session_saved==false after close) and
//          the send succeeds.  This exercises the reconnect path end-to-end
//          without requiring actual abbreviated handshake support from the
//          test-only self-signed certificate.
//
//          Observable outcome: server receives both messages.
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.3.4
// Verification: M1 + M2 + M4
static void test_tls_session_resumption_reconnect_cycle()
{
    static const uint16_t PORT = 19898U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);
    srv_cfg.tls.session_resumption_enabled = true;

    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    ResumeClientArg args;
    args.port        = PORT;
    args.first_init  = Result::ERR_IO;
    args.second_init = Result::ERR_IO;
    args.second_send = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(resume_client_func, &args);

    // Drain: receive both messages from the two client connections.
    // Power of 10 Rule 2: bounded loop (at most 100 poll iterations).
    uint32_t received_count = 0U;
    for (uint32_t k = 0U; k < 100U && received_count < 2U; ++k) {
        MessageEnvelope env;
        Result r = server.receive_message(env, 200U);
        if (r == Result::OK) {
            ++received_count;
        }
    }

    (void)pthread_join(tid, nullptr);
    server.close();

    // Both client connections must have succeeded.
    assert(args.first_init  == Result::OK);
    assert(args.second_init == Result::OK);
    assert(args.second_send == Result::OK);
    // Both messages must have arrived.
    assert(received_count == 2U);

    printf("PASS: test_tls_session_resumption_reconnect_cycle\n");
}


// ─────────────────────────────────────────────────────────────────────────────
// test_tls_inbound_partition_drops_received (REQ-5.1.6)
//
// Configure a plaintext TlsTcpBackend server with partition_enabled=true,
// partition_gap_ms=1 (fires after 1 ms), and a long partition_duration_ms.
// A loopback client sends one message after the partition is active.
// apply_inbound_impairment() detects is_partition_active()==true and drops
// the frame.  receive_message() returns ERR_TIMEOUT.
//
// Verifies: REQ-5.1.6
// ─────────────────────────────────────────────────────────────────────────────

struct TlsPartSrvArg {
    uint16_t port;
    Result   init_result;
    Result   recv_result;
};

static void* tls_partition_srv_thread(void* raw)
{
    TlsPartSrvArg* a = static_cast<TlsPartSrvArg*>(raw);
    assert(a != nullptr);

    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, a->port, false);  // plaintext
    cfg.channels[0U].impairment.enabled              = true;
    cfg.channels[0U].impairment.partition_enabled     = true;
    cfg.channels[0U].impairment.partition_gap_ms      = 10U;     // 10 ms gap
    cfg.channels[0U].impairment.partition_duration_ms = 30000U;  // 30 s

    a->init_result = server.init(cfg);
    if (a->init_result != Result::OK) { return nullptr; }

    // Prime the partition timer: call receive_message() with a short timeout
    // so collect_deliverable() → is_partition_active() is called before the
    // client connects.  This 10 ms poll call starts the 10 ms gap timer and
    // returns ERR_TIMEOUT (no client yet).  After this returns the partition
    // is guaranteed to be active (10 ms gap < 10 ms poll duration).
    MessageEnvelope prime_env;
    (void)server.receive_message(prime_env, 10U);

    // Now wait for the client to connect and send a message.
    // The partition must already be active by the time the frame arrives.
    MessageEnvelope env;
    a->recv_result = server.receive_message(env, 500U);

    server.close();
    return nullptr;
}

// Verifies: REQ-5.1.6
static void test_tls_inbound_partition_drops_received()
{
    static const uint16_t PORT = 19899U;

    TlsPartSrvArg srv_arg;
    srv_arg.port        = PORT;
    srv_arg.init_result = Result::ERR_IO;
    srv_arg.recv_result = Result::OK;

    pthread_t srv_tid = create_thread_4mb(tls_partition_srv_thread, &srv_arg);

    usleep(50000U);  // 50 ms: give server time to bind and partition to activate

    TlsTcpBackend client;
    TransportConfig cli_cfg;
    make_transport_config(cli_cfg, false, PORT, false);  // plaintext client
    Result cli_init = client.init(cli_cfg);

    if (cli_init == Result::OK) {
        MessageEnvelope env;
        envelope_init(env);
        env.message_type      = MessageType::DATA;
        env.message_id        = 0xFEED0001ULL;
        env.source_id         = 2U;
        env.destination_id    = 1U;
        env.reliability_class = ReliabilityClass::BEST_EFFORT;
        env.timestamp_us      = 1U;
        env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;
        (void)client.send_message(env);
        client.close();
    }

    (void)pthread_join(srv_tid, nullptr);

    assert(srv_arg.init_result == Result::OK);
    // Frame dropped by inbound partition -> ERR_TIMEOUT
    assert(srv_arg.recv_result == Result::ERR_TIMEOUT);

    printf("PASS: test_tls_inbound_partition_drops_received\n");
}
// ─────────────────────────────────────────────────────────────────────────────
// Test 34: F2 — tls_recv_frame() returns false on short header read
//
// Inject a mock socket whose recv_frame returns false (EOF/error path).  The
// TLS-disabled path delegates to m_sock_ops->recv_frame(), so injecting a
// mock that always fails exercises the recv_frame false branch without needing
// a real TLS context.  The test verifies that recv_from_client() calls
// remove_client() and returns ERR_IO, and that receive_message() eventually
// returns ERR_TIMEOUT (the server never delivers a valid message).
//
// Coverage note for the TLS header-loop (hdr_received != 4U branch):
//   The loop guard condition is only reachable when m_tls_enabled=true and
//   mbedtls_ssl_read() returns a partial count.  mbedTLS always delivers a
//   full record in a single call over loopback, so the "short header" exit
//   (hdr_received!=4U) cannot be triggered by a loopback integration test
//   without a custom mbedTLS BIO mock.  The fix is verified by code review
//   (M1) and static analysis (M2); branch coverage of that specific exit is
//   architecturally gated (see docs/COVERAGE_CEILINGS.md entry for F2).
//
// Verifies: REQ-6.1.6
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.6
static void test_f2_recv_frame_short_read_via_mock()
{
    // MockSocketOps whose recv_frame always returns false (simulates EOF/error).
    // The plaintext path in tls_recv_frame() calls m_sock_ops->recv_frame();
    // returning false triggers remove_client() then ERR_IO in recv_from_client().
    MockSocketOps mock;
    mock.fail_recv_frame = true;  // configure: always return false

    TlsTcpBackend backend(mock);
    TransportConfig cfg;
    make_transport_config(cfg, true, 19900U, false);  // plaintext server

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    // Connect a plain client so m_client_count > 0; then the injected mock's
    // recv_frame returns false on the first poll → remove_client → ERR_IO.
    ClientThreadArg args;
    args.port   = 19900U;
    args.tls_on = false;
    args.result = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(client_thread_func, &args);

    // Server polls; plaintext recv_frame delegates to mock → false → ERR_IO.
    // receive_message() drains poll iterations and returns ERR_TIMEOUT.
    MessageEnvelope env;
    Result recv_res = backend.receive_message(env, 1000U);

    (void)pthread_join(tid, nullptr);
    backend.close();

    // Client connects and sends OK; server-side mock causes recv failure.
    // receive_message must not return OK (no valid message was delivered).
    assert(recv_res == Result::ERR_TIMEOUT);

    printf("PASS: test_f2_recv_frame_short_read_via_mock\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 35: F3 — BIO re-association: two simultaneous raw TLS clients;
//          remove_client(0) compacts slot 1→0; slot 0 remains usable.
//
// Both clients use the raw mbedTLS API (NOT TlsTcpBackend) to avoid calling
// mbedtls_psa_crypto_free() on destruction.  psa_crypto_free() is called
// only by the server's TlsTcpBackend destructor at the end of the test, so
// the global PSA state remains valid throughout.
//
// Design:
//   Both clients start simultaneously (80 ms delay).  Both perform the TLS
//   handshake and each writes a single valid serialised frame to the server.
//   The server accepts client 1 first (slot 0), then client 2 (slot 1).
//
//   After delivering both frames to the server's receive queue, client 1 sends
//   TLS close_notify so the server's next ssl_read on slot 0 fires the close
//   path.  The server calls remove_client(0): the F3 fix re-associates slot 0's
//   BIO to &m_client_net[0] before zeroing slot 1.
//
//   Client 2 then sends a second frame.  The server must be able to receive it
//   via slot 0 (the compacted slot, BIO re-associated) — proving the fix works.
//   Without the fix, ssl_read on slot 0 would use fd=-1 from the zeroed
//   m_client_net[1] and return an I/O error.
//
// Coverage note:
//   The compaction loop body (j < m_client_count-1) executes when
//   m_client_count==2 and remove_client(0) fires.  The F3 fix
//   (mbedtls_ssl_set_bio re-association) inside that body is exercised here.
//
// Verifies: REQ-6.1.6, REQ-6.1.7
// ─────────────────────────────────────────────────────────────────────────────

// Shared state for the two raw TLS client threads.
struct F3SharedState {
    uint16_t port;
};

struct F3RawArg {
    F3SharedState* shared;
    bool           is_client1;  // true = slot 0 client; false = slot 1 client
    Result         result;
};

// Helper: set up a raw mbedTLS client context, connect, and handshake.
// Returns 0 on success, non-zero on failure.
static int f3_raw_connect_and_handshake(mbedtls_net_context* net,
                                         mbedtls_ssl_context* ssl,
                                         mbedtls_ssl_config*  conf,
                                         uint16_t port)
{
    assert(net != nullptr);
    assert(ssl != nullptr);
    assert(conf != nullptr);

    char port_str[8U];
    (void)snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));
    int ret = mbedtls_net_connect(net, "127.0.0.1", port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) { return ret; }

    ret = mbedtls_ssl_config_defaults(conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { return ret; }
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_ssl_conf_rng(conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
#endif

    ret = mbedtls_ssl_setup(ssl, conf);
    if (ret != 0) { return ret; }
    mbedtls_ssl_set_bio(ssl, net, mbedtls_net_send, mbedtls_net_recv, nullptr);

    // Power of 10 Rule 2 deviation: handshake retry loop — bounded by WANT_READ/
    // WANT_WRITE continuations; terminates when handshake completes or fails.
    do {
        ret = mbedtls_ssl_handshake(ssl);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    return ret;
}

// Send exactly `len` bytes via a raw TLS context, looping on partial writes.
// Power of 10 Rule 2: bounded by len iterations (each sends ≥1 byte).
static bool f3_raw_write_all(mbedtls_ssl_context* ssl,
                              const uint8_t* buf, uint32_t len)
{
    assert(ssl != nullptr);
    assert(buf != nullptr);
    assert(len > 0U);

    uint32_t sent = 0U;
    for (uint32_t iter = 0U; iter < len && sent < len; ++iter) {
        int ret = mbedtls_ssl_write(ssl, buf + sent,
                                    static_cast<size_t>(len - sent));
        if (ret > 0) {
            sent += static_cast<uint32_t>(ret);
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        } else {
            return false;
        }
    }
    return (sent == len);
}

static void* f3_raw_client_thread(void* raw_arg)
{
    F3RawArg* a = static_cast<F3RawArg*>(raw_arg);
    assert(a != nullptr);
    assert(a->shared != nullptr);
    usleep(80000U);  // 80 ms: wait for server to bind

    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_net_init(&net);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    // psa_crypto_init is idempotent; server called it first.
    (void)psa_crypto_init();

    int ret = f3_raw_connect_and_handshake(&net, &ssl, &conf, a->shared->port);
    if (ret != 0) {
        a->result = Result::ERR_IO;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
        return nullptr;
    }

    // Build and send one valid serialised frame.
    // Use a known-good serialised MessageEnvelope.  The simplest approach is to
    // build the raw 4-byte-length-prefixed wire bytes using Serializer.
    // We use a stack buffer (Power of 10 Rule 3: no dynamic allocation).
    uint8_t wire[SOCKET_RECV_BUF_BYTES];
    uint32_t wire_len = 0U;
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = a->is_client1 ? 0xF301ULL : 0xF302ULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    Result ser_res = Serializer::serialize(env, wire, SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(ser_res) || wire_len == 0U) {
        a->result = Result::ERR_IO;
        (void)mbedtls_ssl_close_notify(&ssl);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_net_free(&net);
        return nullptr;
    }

    // Build 4-byte big-endian length prefix + payload (as TlsTcpBackend would).
    uint8_t hdr[4U];
    hdr[0U] = static_cast<uint8_t>((wire_len >> 24U) & 0xFFU);
    hdr[1U] = static_cast<uint8_t>((wire_len >> 16U) & 0xFFU);
    hdr[2U] = static_cast<uint8_t>((wire_len >>  8U) & 0xFFU);
    hdr[3U] = static_cast<uint8_t>( wire_len         & 0xFFU);
    bool ok = f3_raw_write_all(&ssl, hdr, 4U);
    if (ok) {
        ok = f3_raw_write_all(&ssl, wire, wire_len);
    }

    if (a->is_client1) {
        // Client 1: send close_notify quickly so server detects disconnect
        // while client 2 is still alive (triggering the compaction path).
        usleep(100000U);  // 100 ms: let server read the first message
        (void)mbedtls_ssl_close_notify(&ssl);
    } else {
        // Client 2: stay alive; send a second frame after client 1 has closed.
        // Wait for client 1 to have had time to close (200 ms extra).
        usleep(400000U);  // 400 ms

        // Send second frame (message_id 0xF303).
        env.message_id = 0xF303ULL;
        uint32_t wire2_len = 0U;
        ser_res = Serializer::serialize(env, wire, SOCKET_RECV_BUF_BYTES, wire2_len);
        if (result_ok(ser_res) && wire2_len > 0U) {
            hdr[0U] = static_cast<uint8_t>((wire2_len >> 24U) & 0xFFU);
            hdr[1U] = static_cast<uint8_t>((wire2_len >> 16U) & 0xFFU);
            hdr[2U] = static_cast<uint8_t>((wire2_len >>  8U) & 0xFFU);
            hdr[3U] = static_cast<uint8_t>( wire2_len         & 0xFFU);
            (void)f3_raw_write_all(&ssl, hdr, 4U);
            (void)f3_raw_write_all(&ssl, wire, wire2_len);
        }

        usleep(500000U);  // 500 ms: stay alive until server receives second frame
        (void)mbedtls_ssl_close_notify(&ssl);
    }

    // Free per-connection contexts; do NOT call mbedtls_psa_crypto_free().
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&net);

    a->result = ok ? Result::OK : Result::ERR_IO;
    return nullptr;
}

// Verifies: REQ-6.1.6, REQ-6.1.7
static void test_f3_bio_reassoc_after_remove_client()
{
    static const uint16_t PORT = 19901U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, true);  // TLS server
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    // Shared state between the two raw client threads.
    F3SharedState shared;
    shared.port = PORT;

    F3RawArg arg1;
    arg1.shared    = &shared;
    arg1.is_client1 = true;
    arg1.result    = Result::ERR_IO;

    F3RawArg arg2;
    arg2.shared    = &shared;
    arg2.is_client1 = false;
    arg2.result    = Result::ERR_IO;

    // Launch both threads; they wait 80ms then connect simultaneously.
    pthread_t t1 = create_thread_4mb(f3_raw_client_thread, &arg1);
    pthread_t t2 = create_thread_4mb(f3_raw_client_thread, &arg2);

    // Server drains:
    // Call A: receives client 1's msg (slot 0) → envA (msg_id 0xF301)
    // Call B: receives client 2's msg (slot 1) → envB (msg_id 0xF302)
    //         During or after Call B, server detects client 1's close_notify
    //         on slot 0 → remove_client(0) → compaction:
    //           F3 fix: ssl[0].BIO → &m_client_net[0] before zeroing slot 1.
    //         client 2 shifts to slot 0.
    // Call C: receives client 2's second msg (slot 0, BIO re-associated)
    //         → envC (msg_id 0xF303)
    //         Proves slot 0's BIO is valid after compaction.

    uint64_t ids[3U] = {0U, 0U, 0U};
    uint32_t got = 0U;
    // Power of 10 Rule 2: bounded loop (at most 80 × 200 ms = 16 s max)
    for (uint32_t k = 0U; k < 80U && got < 3U; ++k) {
        MessageEnvelope env;
        Result r = server.receive_message(env, 200U);
        if (r == Result::OK) {
            if (got < 3U) {
                ids[got] = env.message_id;
                ++got;
            }
        }
    }

    server.close();
    (void)pthread_join(t1, nullptr);
    (void)pthread_join(t2, nullptr);

    assert(arg1.result == Result::OK);
    assert(arg2.result == Result::OK);
    // Must have received all 3 frames: 0xF301, 0xF302, 0xF303.
    // Order may vary (slot 0 or slot 1 polled first).
    assert(got == 3U);
    // Verify all three expected message IDs were received (order may vary)
    bool saw_f301 = false;
    bool saw_f302 = false;
    bool saw_f303 = false;
    for (uint32_t k = 0U; k < 3U; ++k) {
        if (ids[k] == 0xF301ULL) { saw_f301 = true; }
        if (ids[k] == 0xF302ULL) { saw_f302 = true; }
        if (ids[k] == 0xF303ULL) { saw_f303 = true; }
    }
    assert(saw_f301 && saw_f302 && saw_f303);

    printf("PASS: test_f3_bio_reassoc_after_remove_client\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// HELLO registration and unicast routing tests (REQ-6.1.8, REQ-6.1.9, REQ-6.1.10)
//
// Ports 19901–19907 reserved for these tests.
// Ports 19901-19907 are within the 19870-19910 range already allocated for
// TlsTcpBackend tests and do not conflict with existing tests (≤ 19900).
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: client mode register_local_id sends HELLO on-wire (loopback)
//
// TlsTcpBackend uses mbedtls_net_connect() for connection setup — it does not
// route through ISocketOps, so MockSocketOps cannot intercept the connect path.
// Instead, we use a real plaintext loopback: start a server in a thread, connect
// a client, call register_local_id(42), and verify the call returns OK.
// Because send_hello_frame() propagates its send_frame() result back to the
// caller, a successful return proves the HELLO was serialised and written to the
// wire socket without error.  Test 3 (test_tls_hello_received_by_server_populates
// _routing_table) independently verifies the server receives and records the NodeId.
//
// Verifies: REQ-6.1.8, REQ-6.1.10
// Verification: M1 + M2 + M4 (loopback path)
// ─────────────────────────────────────────────────────────────────────────────

// Helper thread: server that accepts one client and holds the connection open.
struct TlsHello1ServerArg {
    uint16_t port;
    Result   result;
};

static void* tls_hello1_server_thread(void* raw)
{
    TlsHello1ServerArg* a = static_cast<TlsHello1ServerArg*>(raw);
    assert(a != nullptr);

    TlsTcpBackend server;
    TransportConfig cfg;
    make_transport_config(cfg, true, a->port, false);
    a->result = server.init(cfg);
    if (a->result != Result::OK) { return nullptr; }

    // Poll for 600 ms to accept and drain the HELLO; hold connection open.
    // Power of 10 Rule 2: bounded poll loop (max 12 iterations × 50 ms = 600 ms).
    static const int MAX_POLL_ITERS = 12;
    for (int i = 0; i < MAX_POLL_ITERS; ++i) {
        MessageEnvelope env;
        (void)server.receive_message(env, 50U);
    }
    server.close();
    return nullptr;
}

// Verifies: REQ-6.1.8, REQ-6.1.10
static void test_tls_register_local_id_client_sends_hello()
{
    // Note: TlsTcpBackend routes connection through mbedtls_net_connect(), not
    // ISocketOps, so a real loopback server is required for the client to connect.
    static const uint16_t PORT = 19901U;

    TlsHello1ServerArg srv_arg;
    srv_arg.port   = PORT;
    srv_arg.result = Result::ERR_IO;

    pthread_t srv_tid = create_thread_4mb(tls_hello1_server_thread, &srv_arg);
    usleep(80000U);  // 80 ms: allow server to bind and listen

    TlsTcpBackend client;
    TransportConfig cli_cfg;
    make_transport_config(cli_cfg, false, PORT, false);
    Result r = client.init(cli_cfg);
    assert(r == Result::OK);
    assert(client.is_open() == true);

    // register_local_id in client mode calls send_hello_frame(); the frame is
    // sent on the real socket; the return value propagates the send result.
    r = client.register_local_id(42U);
    assert(r == Result::OK);

    client.close();
    (void)pthread_join(srv_tid, nullptr);
    assert(srv_arg.result == Result::OK);
    printf("PASS: test_tls_register_local_id_client_sends_hello\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: server mode register_local_id stores NodeId but does NOT send HELLO
// Verifies: REQ-6.1.10
// Verification: M1 + M2 + M4 + M5 (fault injection via ISocketOps)
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.10
static void test_tls_register_local_id_server_no_hello()
{
    MockSocketOps mock;
    TlsTcpBackend backend(mock);

    // Server mode, plaintext: mock bind/listen succeed; do_accept returns -1.
    TransportConfig cfg;
    make_transport_config(cfg, true, 19902U, false);
    Result r = backend.init(cfg);
    assert(r == Result::OK);
    assert(backend.is_open() == true);

    // In server mode, register_local_id stores the id but sends no HELLO.
    uint32_t frames_before = mock.send_frame_count;
    r = backend.register_local_id(7U);
    assert(r == Result::OK);

    // Server must NOT call send_frame for its own HELLO.
    assert(mock.send_frame_count == frames_before);

    backend.close();
    printf("PASS: test_tls_register_local_id_server_no_hello\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Loopback helper: TlsTcpBackend client that registers its NodeId then waits.
// ─────────────────────────────────────────────────────────────────────────────

struct TlsHelloClientArg {
    uint16_t port;
    NodeId   local_id;
    uint32_t connect_delay_us;
    bool     tls_on;
    Result   result;
};

static void* tls_hello_client_thread(void* raw)
{
    TlsHelloClientArg* a = static_cast<TlsHelloClientArg*>(raw);
    assert(a != nullptr);

    usleep(a->connect_delay_us);

    TlsTcpBackend client;
    TransportConfig cfg;
    make_transport_config(cfg, false, a->port, a->tls_on);
    a->result = client.init(cfg);
    if (a->result != Result::OK) { return nullptr; }

    // Send HELLO to server; server routing table is populated.
    a->result = client.register_local_id(a->local_id);

    usleep(300000U);  // 300 ms: keep connection alive while server tests routing
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: server receives HELLO and populates routing table; unicast works
//
// Uses plaintext TlsTcpBackend loopback.  Client registers NodeId=55 via HELLO.
// Server drains HELLOs, then sends unicast DATA to destination_id=55.
// send_message must return OK (slot found in routing table).
//
// Verifies: REQ-6.1.8, REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.8, REQ-6.1.9
static void test_tls_hello_received_by_server_populates_routing_table()
{
    static const uint16_t PORT   = 19903U;
    static const NodeId   CLI_ID = 55U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);  // plaintext server
    Result r = server.init(srv_cfg);
    assert(r == Result::OK);
    assert(server.is_open() == true);

    TlsHelloClientArg cli_arg;
    cli_arg.port             = PORT;
    cli_arg.local_id         = CLI_ID;
    cli_arg.connect_delay_us = 80000U;
    cli_arg.tls_on           = false;
    cli_arg.result           = Result::ERR_IO;

    pthread_t cli_tid = create_thread_4mb(tls_hello_client_thread, &cli_arg);

    // Drain until HELLO is consumed (Power of 10: fixed bound, 10 iters × 100 ms)
    for (uint32_t i = 0U; i < 10U; ++i) {
        MessageEnvelope env;
        (void)server.receive_message(env, 100U);
    }

    // Unicast DATA to CLI_ID — routing table must have the slot.
    MessageEnvelope data_env;
    envelope_init(data_env);
    data_env.message_type      = MessageType::DATA;
    data_env.message_id        = 0xBEEF4001ULL;
    data_env.source_id         = 1U;
    data_env.destination_id    = CLI_ID;
    data_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    data_env.timestamp_us      = 1U;
    data_env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;

    Result send_r = server.send_message(data_env);
    assert(send_r == Result::OK);  // unicast to registered CLI_ID succeeds

    (void)pthread_join(cli_tid, nullptr);
    server.close();

    assert(cli_arg.result == Result::OK);
    printf("PASS: test_tls_hello_received_by_server_populates_routing_table\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: HELLO frame is NOT delivered to the application via receive_message
//
// Client sends HELLO.  Server's receive_message must consume it internally and
// must NOT return OK to the application (HELLO is a control frame, not data).
//
// Verifies: REQ-6.1.8
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.8
static void test_tls_hello_frame_not_delivered_to_delivery_engine()
{
    static const uint16_t PORT = 19904U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result r = server.init(srv_cfg);
    assert(r == Result::OK);

    TlsHelloClientArg cli_arg;
    cli_arg.port             = PORT;
    cli_arg.local_id         = 33U;
    cli_arg.connect_delay_us = 80000U;
    cli_arg.tls_on           = false;
    cli_arg.result           = Result::ERR_IO;

    pthread_t cli_tid = create_thread_4mb(tls_hello_client_thread, &cli_arg);

    // receive_message must NOT return OK for a HELLO frame.
    MessageEnvelope env;
    Result recv_r = server.receive_message(env, 500U);
    assert(recv_r != Result::OK);  // HELLO consumed internally; not delivered

    (void)pthread_join(cli_tid, nullptr);
    server.close();

    assert(cli_arg.result == Result::OK);
    printf("PASS: test_tls_hello_frame_not_delivered_to_delivery_engine\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: unicast routes to the registered slot only
//
// Two plaintext clients connect and register NodeId 10 and NodeId 20.
// Server sends unicast DATA to 10, then 20 — both succeed (slot found).
//
// Verifies: REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.9
static void test_tls_unicast_routes_to_registered_slot()
{
    static const uint16_t PORT = 19905U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result r = server.init(srv_cfg);
    assert(r == Result::OK);
    assert(server.is_open() == true);

    TlsHelloClientArg cli1_arg;
    cli1_arg.port             = PORT;
    cli1_arg.local_id         = 10U;
    cli1_arg.connect_delay_us = 80000U;
    cli1_arg.tls_on           = false;
    cli1_arg.result           = Result::ERR_IO;

    TlsHelloClientArg cli2_arg;
    cli2_arg.port             = PORT;
    cli2_arg.local_id         = 20U;
    cli2_arg.connect_delay_us = 160000U;
    cli2_arg.tls_on           = false;
    cli2_arg.result           = Result::ERR_IO;

    pthread_t cli1_tid = create_thread_4mb(tls_hello_client_thread, &cli1_arg);
    pthread_t cli2_tid = create_thread_4mb(tls_hello_client_thread, &cli2_arg);

    // Drain HELLOs: Power of 10 fixed bound (20 iters × 100 ms = 2 s max)
    for (uint32_t i = 0U; i < 20U; ++i) {
        MessageEnvelope env;
        (void)server.receive_message(env, 100U);
    }

    // Unicast to node 10.
    MessageEnvelope d1;
    envelope_init(d1);
    d1.message_type      = MessageType::DATA;
    d1.message_id        = 0xBEEF5001ULL;
    d1.source_id         = 1U;
    d1.destination_id    = 10U;
    d1.reliability_class = ReliabilityClass::BEST_EFFORT;
    d1.timestamp_us      = 1U;
    d1.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;
    Result s1 = server.send_message(d1);
    assert(s1 == Result::OK);

    // Unicast to node 20.
    MessageEnvelope d2;
    envelope_init(d2);
    d2.message_type      = MessageType::DATA;
    d2.message_id        = 0xBEEF5002ULL;
    d2.source_id         = 1U;
    d2.destination_id    = 20U;
    d2.reliability_class = ReliabilityClass::BEST_EFFORT;
    d2.timestamp_us      = 1U;
    d2.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;
    Result s2 = server.send_message(d2);
    assert(s2 == Result::OK);

    (void)pthread_join(cli1_tid, nullptr);
    (void)pthread_join(cli2_tid, nullptr);
    server.close();

    assert(cli1_arg.result == Result::OK);
    assert(cli2_arg.result == Result::OK);
    printf("PASS: test_tls_unicast_routes_to_registered_slot\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: broadcast when destination_id == NODE_ID_INVALID (0)
//
// Two plaintext clients connect and register NodeIds 11 and 22.
// Server sends broadcast DATA (destination_id=0) — send_message returns OK.
//
// Verifies: REQ-6.1.9
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.9
static void test_tls_broadcast_when_destination_id_zero()
{
    static const uint16_t PORT = 19906U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);
    Result r = server.init(srv_cfg);
    assert(r == Result::OK);
    assert(server.is_open() == true);

    TlsHelloClientArg cli1_arg;
    cli1_arg.port             = PORT;
    cli1_arg.local_id         = 11U;
    cli1_arg.connect_delay_us = 80000U;
    cli1_arg.tls_on           = false;
    cli1_arg.result           = Result::ERR_IO;

    TlsHelloClientArg cli2_arg;
    cli2_arg.port             = PORT;
    cli2_arg.local_id         = 22U;
    cli2_arg.connect_delay_us = 160000U;
    cli2_arg.tls_on           = false;
    cli2_arg.result           = Result::ERR_IO;

    pthread_t cli1_tid = create_thread_4mb(tls_hello_client_thread, &cli1_arg);
    pthread_t cli2_tid = create_thread_4mb(tls_hello_client_thread, &cli2_arg);

    // Drain HELLOs: Power of 10 fixed bound (20 iters × 100 ms = 2 s max)
    for (uint32_t i = 0U; i < 20U; ++i) {
        MessageEnvelope env;
        (void)server.receive_message(env, 100U);
    }

    // Broadcast to all connected clients.
    MessageEnvelope bcast;
    envelope_init(bcast);
    bcast.message_type      = MessageType::DATA;
    bcast.message_id        = 0xBEEF6001ULL;
    bcast.source_id         = 1U;
    bcast.destination_id    = NODE_ID_INVALID;  // 0 = broadcast sentinel
    bcast.reliability_class = ReliabilityClass::BEST_EFFORT;
    bcast.timestamp_us      = 1U;
    bcast.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;

    Result send_r = server.send_message(bcast);
    assert(send_r == Result::OK);  // broadcast to all connected clients succeeds

    (void)pthread_join(cli1_tid, nullptr);
    (void)pthread_join(cli2_tid, nullptr);
    server.close();

    assert(cli1_arg.result == Result::OK);
    assert(cli2_arg.result == Result::OK);
    printf("PASS: test_tls_broadcast_when_destination_id_zero\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread helper: plaintext raw socket client that sends a HELLO with
// hello_source_id, then a DATA frame with data_source_id.
// Used by the two REQ-6.1.11 source validation tests.
// ─────────────────────────────────────────────────────────────────────────────

struct SrcValidationClientArg {
    uint16_t port;
    NodeId   hello_source_id;  // source_id in the HELLO frame (registers identity)
    NodeId   data_source_id;   // source_id in the DATA frame (may mismatch)
    Result   result;
};

// Helper: write all `len` bytes of `buf` to a plain POSIX fd.
// Returns true on success, false on error.
// Power of 10 Rule 2: bounded by len iterations (each sends >=1 byte).
static bool src_val_write_all(int fd, const uint8_t* buf, uint32_t len)
{
    assert(fd >= 0);
    assert(buf != nullptr);
    assert(len > 0U);

    uint32_t sent = 0U;
    for (uint32_t iter = 0U; iter < len && sent < len; ++iter) {
        ssize_t n = ::write(fd, buf + sent, static_cast<size_t>(len - sent));
        if (n > 0) {
            sent += static_cast<uint32_t>(n);
        } else {
            return false;
        }
    }
    return (sent == len);
}

// Helper: serialize `env` and write it to `fd` as a 4-byte-length-prefixed frame.
// Returns true on success.
static bool src_val_send_frame(int fd, const MessageEnvelope& env)
{
    assert(fd >= 0);

    static uint8_t wire[SOCKET_RECV_BUF_BYTES];
    uint32_t wire_len = 0U;
    Result ser_res = Serializer::serialize(env, wire, SOCKET_RECV_BUF_BYTES, wire_len);
    if (!result_ok(ser_res) || wire_len == 0U) {
        return false;
    }

    uint8_t hdr[4U];
    hdr[0U] = static_cast<uint8_t>((wire_len >> 24U) & 0xFFU);
    hdr[1U] = static_cast<uint8_t>((wire_len >> 16U) & 0xFFU);
    hdr[2U] = static_cast<uint8_t>((wire_len >>  8U) & 0xFFU);
    hdr[3U] = static_cast<uint8_t>( wire_len         & 0xFFU);

    if (!src_val_write_all(fd, hdr, 4U)) {
        return false;
    }
    return src_val_write_all(fd, wire, wire_len);
}

static void* src_validation_client_func(void* raw_arg)
{
    SrcValidationClientArg* a = static_cast<SrcValidationClientArg*>(raw_arg);
    assert(a != nullptr);

    usleep(80000U);  // 80 ms: wait for server to bind and listen

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { a->result = Result::ERR_IO; return nullptr; }

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(a->port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int ret = ::connect(fd,
                        reinterpret_cast<struct sockaddr*>(&addr),
                        static_cast<socklen_t>(sizeof(addr)));
    if (ret < 0) {
        (void)::close(fd);
        a->result = Result::ERR_IO;
        return nullptr;
    }

    // Send HELLO frame: registers hello_source_id with the server.
    MessageEnvelope hello_env;
    envelope_init(hello_env);
    hello_env.message_type   = MessageType::HELLO;
    hello_env.message_id     = 0xAA01ULL;
    hello_env.source_id      = a->hello_source_id;
    hello_env.destination_id = NODE_ID_INVALID;
    hello_env.payload_length = 0U;

    bool ok = src_val_send_frame(fd, hello_env);
    if (!ok) {
        (void)::close(fd);
        a->result = Result::ERR_IO;
        return nullptr;
    }

    usleep(50000U);  // 50 ms: let server process the HELLO

    // Send DATA frame with data_source_id (may differ from hello_source_id).
    MessageEnvelope data_env;
    envelope_init(data_env);
    data_env.message_type      = MessageType::DATA;
    data_env.message_id        = 0xAA02ULL;
    data_env.source_id         = a->data_source_id;
    data_env.destination_id    = 1U;
    data_env.reliability_class = ReliabilityClass::BEST_EFFORT;
    data_env.timestamp_us      = 1U;
    data_env.expiry_time_us    = 0xFFFFFFFFFFFFFFFFULL;

    ok = src_val_send_frame(fd, data_env);

    usleep(200000U);  // 200 ms: let server attempt to receive
    (void)::close(fd);

    a->result = ok ? Result::OK : Result::ERR_IO;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: source_id mismatch is silently dropped (REQ-6.1.11)
//
// Client registers with HELLO source_id=1, then sends DATA with source_id=2.
// The server's validate_source_id() detects the mismatch and discards the
// frame (logs WARNING_HI).  receive_message() must NOT return OK; the spoofed
// frame must never reach the delivery engine.
//
// Verifies: REQ-6.1.11
// Verification: M1 + M2 + M4 (loopback path; mismatch discard exercised)
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.11
static void test_tls_source_id_mismatch_dropped()
{
    static const uint16_t PORT = 19907U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);  // plaintext server
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    SrcValidationClientArg args;
    args.port            = PORT;
    args.hello_source_id = 1U;   // registers as NodeId 1
    args.data_source_id  = 2U;   // spoofed: different from registered NodeId
    args.result          = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(src_validation_client_func, &args);

    // Server: the DATA frame source_id==2 mismatches registered id==1.
    // validate_source_id() returns false → recv_from_client returns ERR_INVALID.
    // receive_message() discards and continues polling until timeout.
    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 1000U);
    assert(recv_res != Result::OK);  // spoofed frame must NOT be delivered

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);  // client sent successfully
    printf("PASS: test_tls_source_id_mismatch_dropped\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test tls_: source_id match is accepted and delivered (REQ-6.1.11)
//
// Client registers with HELLO source_id=1, then sends DATA with source_id=1.
// The server's validate_source_id() confirms the match and allows the frame
// through.  receive_message() must return OK with source_id==1.
//
// Verifies: REQ-6.1.11
// Verification: M1 + M2 + M4 (loopback path; match acceptance exercised)
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.1.11
static void test_tls_source_id_match_accepted()
{
    static const uint16_t PORT = 19908U;

    TlsTcpBackend server;
    TransportConfig srv_cfg;
    make_transport_config(srv_cfg, true, PORT, false);  // plaintext server
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    SrcValidationClientArg args;
    args.port            = PORT;
    args.hello_source_id = 1U;  // registers as NodeId 1
    args.data_source_id  = 1U;  // matches registered NodeId → should be delivered
    args.result          = Result::ERR_IO;

    pthread_t tid = create_thread_4mb(src_validation_client_func, &args);

    // Server: the DATA frame source_id==1 matches registered id==1.
    // validate_source_id() returns true → frame is delivered to receive_message().
    MessageEnvelope env;
    Result recv_res = server.receive_message(env, 1000U);
    assert(recv_res == Result::OK);
    assert(env.source_id == 1U);  // correct source_id delivered

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(args.result == Result::OK);
    printf("PASS: test_tls_source_id_match_accepted\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    write_pem_files();

    test_tls_config_default();
    test_transport_config_includes_tls();
    test_server_bind_plaintext();
    test_init_bad_cert_path();
    test_server_bind_tls();
    test_plaintext_loopback();
    test_tls_loopback();
    test_connect_bad_host();
    test_receive_timeout();
    test_server_send_no_clients();
    test_echo_loopback_plaintext();
    // New coverage tests (12-25):
    test_post_echo_remove_client_plaintext();
    test_tls_verify_peer_with_ca();
    test_tls_verify_peer_no_ca();
    test_init_bad_key_path();
    test_server_num_channels_zero();
    test_tls_client_close_and_long_timeout();  // ~5 s (50 poll iterations)
    test_server_bind_port_zero();
    test_raw_socket_no_tls_handshake();
    test_garbage_frame_deserialize_fail();
    test_two_client_queue_prefill();
    test_two_client_compact_loop();
    test_tls_bad_ca_file();
    test_bind_port_in_use();
    test_server_send_before_client_connects();
    test_max_connections_exceeded();
    test_tls_zero_length_frame();
    // New coverage tests (28-30):
    test_di_constructor_executes();
    test_recv_queue_overflow();
    test_send_impairment_loss_drop();
    // Session resumption tests (31-33):
    test_session_resumption_disabled_by_default();
    test_tls_session_saved_after_handshake();
    test_tls_session_resumption_reconnect_cycle();

    // Inbound impairment tests (REQ-5.1.6)
    test_tls_inbound_partition_drops_received();

    // Security fix tests (F2 and F3):
    test_f2_recv_frame_short_read_via_mock();
    test_f3_bio_reassoc_after_remove_client();

    // HELLO registration and unicast routing tests (REQ-6.1.8, REQ-6.1.9, REQ-6.1.10)
    test_tls_register_local_id_client_sends_hello();
    test_tls_register_local_id_server_no_hello();
    test_tls_hello_received_by_server_populates_routing_table();
    test_tls_hello_frame_not_delivered_to_delivery_engine();
    test_tls_unicast_routes_to_registered_slot();
    test_tls_broadcast_when_destination_id_zero();

    // Source address validation tests (REQ-6.1.11)
    test_tls_source_id_mismatch_dropped();
    test_tls_source_id_match_accepted();

    // Cleanup
    (void)remove(TEST_CERT_FILE);
    (void)remove(TEST_KEY_FILE);

    printf("ALL TlsTcpBackend tests passed.\n");
    return 0;
}
