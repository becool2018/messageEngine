/**
 * @file test_DtlsUdpBackend.cpp
 * @brief Unit and integration tests for DtlsUdpBackend:
 *        plaintext UDP fallback, DTLS init failures, and loopback roundtrips.
 *
 * Tests cover:
 *   - DtlsUdpBackend plaintext bind (tls_enabled=false) → OK → is_open → close
 *   - DtlsUdpBackend init failure when cert/key files are missing
 *   - Full loopback message roundtrip (plaintext UDP) using POSIX threads
 *   - Full loopback message roundtrip (DTLS) using POSIX threads
 *   - Payload exceeding DTLS_MAX_DATAGRAM_BYTES → ERR_INVALID
 *   - receive_message() returns ERR_TIMEOUT when no data arrives
 *
 * Test cert/key written to /tmp at test startup; cleaned up at exit.
 * POSIX threads used for loopback tests; server init() blocks until first
 * client datagram arrives (DTLS handshake blocks in server_wait_and_handshake).
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - STL exempted in tests/ for fixture setup only.
 *
 * Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4,
 *           REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4,
 *           REQ-6.4.5, REQ-7.1.1
 */
// Verifies: REQ-4.1.1, REQ-4.1.2, REQ-4.1.3, REQ-4.1.4, REQ-6.3.4, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3, REQ-6.4.4, REQ-6.4.5, REQ-7.1.1
// Verification: M1 + M2 + M4 + M5 (fault injection via IMbedtlsOps / ISocketOps)

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
#include "platform/DtlsUdpBackend.hpp"
#include "platform/IMbedtlsOps.hpp"
#include "MockSocketOps.hpp"
#include <psa/crypto.h>

// ─────────────────────────────────────────────────────────────────────────────
// Embedded PEM test credentials (self-signed EC P-256, 10-year validity)
// Reuses the same cert/key as test_TlsTcpBackend.cpp — DTLS and TLS use the
// same mbedTLS certificate handling.
// ─────────────────────────────────────────────────────────────────────────────

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

static const char* DTLS_TEST_CERT_FILE = "/tmp/me_dtls_test.crt";
static const char* DTLS_TEST_KEY_FILE  = "/tmp/me_dtls_test.key";

static void write_pem_files()
{
    FILE* fp = fopen(DTLS_TEST_CERT_FILE, "w");
    assert(fp != nullptr);
    assert(fputs(TEST_CERT_PEM, fp) >= 0);
    assert(fclose(fp) == 0);

    fp = fopen(DTLS_TEST_KEY_FILE, "w");
    assert(fp != nullptr);
    assert(fputs(TEST_KEY_PEM, fp) >= 0);
    assert(fclose(fp) == 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper — build a TransportConfig for DtlsUdpBackend loopback tests
// ─────────────────────────────────────────────────────────────────────────────

static void make_dtls_config(TransportConfig& cfg, bool is_server,
                              uint16_t port, bool tls_on)
{
    transport_config_default(cfg);
    cfg.kind       = TransportKind::DTLS_UDP;
    cfg.is_server  = is_server;
    cfg.bind_port  = is_server ? port : 0U;  // client: OS assigns ephemeral bind port
    cfg.peer_port  = port;                   // both sides connect to the server's port
    // loopback: both sides bind/connect to 127.0.0.1
    (void)strncpy(cfg.bind_ip, "127.0.0.1",
                  static_cast<uint32_t>(sizeof(cfg.bind_ip)) - 1U);
    cfg.bind_ip[sizeof(cfg.bind_ip) - 1U] = '\0';
    (void)strncpy(cfg.peer_ip, "127.0.0.1",
                  static_cast<uint32_t>(sizeof(cfg.peer_ip)) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';
    cfg.local_node_id        = is_server ? 1U : 2U;
    cfg.connect_timeout_ms   = 10000U;  // 10 s max handshake wait
    cfg.tls.tls_enabled      = tls_on;
    if (tls_on) {
        cfg.tls.role        = is_server ? TlsRole::SERVER : TlsRole::CLIENT;
        cfg.tls.verify_peer = false;  // self-signed cert; skip chain verify
        uint32_t path_max = static_cast<uint32_t>(sizeof(cfg.tls.cert_file)) - 1U;
        (void)strncpy(cfg.tls.cert_file, DTLS_TEST_CERT_FILE, path_max);
        cfg.tls.cert_file[path_max] = '\0';
        (void)strncpy(cfg.tls.key_file, DTLS_TEST_KEY_FILE, path_max);
        cfg.tls.key_file[path_max] = '\0';
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread argument structure for loopback tests
// ─────────────────────────────────────────────────────────────────────────────

struct DtlsClientArg {
    uint16_t port;
    bool     tls_on;
    Result   result;
};

static void* dtls_client_thread(void* arg)
{
    DtlsClientArg* a = static_cast<DtlsClientArg*>(arg);
    assert(a != nullptr);

    // Allow server thread to enter server_wait_and_handshake() before client
    // sends the first datagram (which triggers the DTLS handshake).
    usleep(150000U);  // 150 ms

    DtlsUdpBackend client;
    TransportConfig cfg;
    make_dtls_config(cfg, false, a->port, a->tls_on);

    a->result = client.init(cfg);
    assert(a->result == Result::OK);
    assert(client.is_open() == true);

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xD715ULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    a->result = client.send_message(env);
    assert(a->result == Result::OK);

    usleep(100000U);  // 100 ms: let server drain receive queue
    client.close();
    assert(client.is_open() == false);
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: plaintext bind → is_open → close
// Verifies: REQ-4.1.1, REQ-4.1.4, REQ-6.4.5
// ─────────────────────────────────────────────────────────────────────────────

static void test_plaintext_server_bind()
{
    // Verifies: REQ-4.1.1, REQ-4.1.4, REQ-6.4.5
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14580U, false);

    Result res = backend.init(cfg);
    assert(res == Result::OK);
    assert(backend.is_open() == true);

    backend.close();
    assert(backend.is_open() == false);

    printf("PASS: test_plaintext_server_bind\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: TLS init with missing cert file → ERR_IO
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_bad_cert()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14582U, true);
    // Override cert to a non-existent path
    (void)strncpy(cfg.tls.cert_file, "/tmp/does_not_exist.crt",
                  static_cast<uint32_t>(sizeof(cfg.tls.cert_file)) - 1U);
    cfg.tls.cert_file[sizeof(cfg.tls.cert_file) - 1U] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_dtls_bad_cert\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: plaintext loopback roundtrip (tls_enabled=false)
// Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.4.5
// ─────────────────────────────────────────────────────────────────────────────

static void test_plaintext_loopback()
{
    // Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.4.5
    static const uint16_t PORT = 14583U;

    // Server: plaintext UDP receives from any sender — no handshake needed
    DtlsUdpBackend server;
    TransportConfig srv_cfg;
    make_dtls_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    DtlsClientArg args;
    args.port   = PORT;
    args.tls_on = false;
    args.result = Result::ERR_IO;

    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, static_cast<size_t>(2U) * 1024U * 1024U) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, &attr, dtls_client_thread, &args) == 0);
    (void)pthread_attr_destroy(&attr);

    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 3000U);

    (void)pthread_join(tid, nullptr);
    server.close();

    assert(recv_res == Result::OK);
    assert(args.result == Result::OK);
    assert(received.message_id == 0xD715ULL);
    assert(received.source_id  == 2U);

    printf("PASS: test_plaintext_loopback\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: DTLS loopback roundtrip (tls_enabled=true)
// Server thread blocks in init() until client's DTLS ClientHello arrives.
// Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3
// ─────────────────────────────────────────────────────────────────────────────

struct DtlsLoopbackArg {
    uint16_t port;
    bool     tls_on;
    Result   init_result;
    Result   recv_result;
    uint64_t recv_message_id;
};

static void* dtls_server_thread(void* arg)
{
    DtlsLoopbackArg* a = static_cast<DtlsLoopbackArg*>(arg);
    assert(a != nullptr);

    DtlsUdpBackend server;
    TransportConfig cfg;
    make_dtls_config(cfg, true, a->port, a->tls_on);

    a->init_result = server.init(cfg);  // blocks until client connects
    if (a->init_result != Result::OK) {
        return nullptr;
    }

    MessageEnvelope received;
    a->recv_result = server.receive_message(received, 5000U);
    if (a->recv_result == Result::OK) {
        a->recv_message_id = received.message_id;
    }

    server.close();
    return nullptr;
}

static void test_dtls_loopback()
{
    // Verifies: REQ-4.1.2, REQ-4.1.3, REQ-6.4.1, REQ-6.4.2, REQ-6.4.3
    static const uint16_t PORT = 14584U;

    DtlsLoopbackArg srv_arg;
    srv_arg.port            = PORT;
    srv_arg.tls_on          = true;
    srv_arg.init_result     = Result::ERR_IO;
    srv_arg.recv_result     = Result::ERR_IO;
    srv_arg.recv_message_id = 0ULL;

    // Use a large stack: DtlsUdpBackend + mbedTLS contexts are sizable
    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U) == 0);

    pthread_t srv_tid;
    assert(pthread_create(&srv_tid, &attr, dtls_server_thread, &srv_arg) == 0);
    (void)pthread_attr_destroy(&attr);

    // Client thread starts 150 ms after server (server must be polling before client sends)
    DtlsClientArg cli_arg;
    cli_arg.port   = PORT;
    cli_arg.tls_on = true;
    cli_arg.result = Result::ERR_IO;

    pthread_attr_t cli_attr;
    assert(pthread_attr_init(&cli_attr) == 0);
    assert(pthread_attr_setstacksize(&cli_attr, static_cast<size_t>(4U) * 1024U * 1024U) == 0);

    pthread_t cli_tid;
    assert(pthread_create(&cli_tid, &cli_attr, dtls_client_thread, &cli_arg) == 0);
    (void)pthread_attr_destroy(&cli_attr);

    (void)pthread_join(srv_tid, nullptr);
    (void)pthread_join(cli_tid, nullptr);

    assert(srv_arg.init_result     == Result::OK);
    assert(srv_arg.recv_result     == Result::OK);
    assert(srv_arg.recv_message_id == 0xD715ULL);
    assert(cli_arg.result          == Result::OK);

    printf("PASS: test_dtls_loopback\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: payload exceeding DTLS_MAX_DATAGRAM_BYTES → ERR_INVALID
// Verifies: REQ-6.4.4
// ─────────────────────────────────────────────────────────────────────────────

static void test_oversized_payload_rejected()
{
    // Verifies: REQ-6.4.4
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14585U, false);

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    // Build an envelope whose serialized form exceeds DTLS_MAX_DATAGRAM_BYTES.
    // With MSG_MAX_PAYLOAD_BYTES = 4096, a max-payload message serializes to
    // ~4146 bytes (header + payload), well above DTLS_MAX_DATAGRAM_BYTES (1400).
    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0x1234ULL;
    env.source_id         = 1U;
    env.destination_id    = 2U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    // Fill payload to maximum size
    (void)memset(env.payload, 0xABU, MSG_MAX_PAYLOAD_BYTES);
    env.payload_length = MSG_MAX_PAYLOAD_BYTES;

    Result send_res = backend.send_message(env);
    assert(send_res == Result::ERR_INVALID);

    backend.close();

    printf("PASS: test_oversized_payload_rejected\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: receive_message() returns ERR_TIMEOUT when no sender
// Verifies: REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_receive_timeout()
{
    // Verifies: REQ-4.1.3
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14586U, false);

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    MessageEnvelope env;
    Result recv_res = backend.receive_message(env, 300U);  // 300 ms
    assert(recv_res == Result::ERR_TIMEOUT);

    backend.close();

    printf("PASS: test_receive_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple server thread: init() then one receive_message() call
// Used by tests that need configurable server-side receive timeout
// ─────────────────────────────────────────────────────────────────────────────

struct DtlsSimpleServerArg {
    uint16_t port;
    uint32_t recv_timeout_ms;
    Result   init_result;
    Result   recv_result;
};

static void* dtls_simple_server_thread(void* arg)
{
    DtlsSimpleServerArg* a = static_cast<DtlsSimpleServerArg*>(arg);
    assert(a != nullptr);

    DtlsUdpBackend server;
    TransportConfig cfg;
    make_dtls_config(cfg, true, a->port, true);

    a->init_result = server.init(cfg);
    if (a->init_result != Result::OK) { return nullptr; }

    MessageEnvelope env;
    a->recv_result = server.receive_message(env, a->recv_timeout_ms);

    server.close();
    return nullptr;
}

// Client thread that closes immediately after handshake (sends close_notify,
// no application data) — exercises ssl_read PEER_CLOSE_NOTIFY error path
static void* dtls_early_close_client_thread(void* arg)
{
    DtlsClientArg* a = static_cast<DtlsClientArg*>(arg);
    assert(a != nullptr);

    usleep(150000U);  // 150 ms: let server enter server_wait_and_handshake

    DtlsUdpBackend client;
    TransportConfig cfg;
    make_dtls_config(cfg, false, a->port, a->tls_on);

    a->result = client.init(cfg);
    if (a->result != Result::OK) { return nullptr; }

    usleep(50000U);  // 50 ms: let server's init() finish before close_notify
    // Sends DTLS close_notify alert — exercises ssl_read PEER_CLOSE_NOTIFY
    // error path in server's receive_message() on the next call
    client.close();
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread argument and function for raw UDP garbage sender
// ─────────────────────────────────────────────────────────────────────────────

struct DtlsGarbageSenderArg {
    uint16_t target_port;
    int      result;
};

static void* dtls_garbage_sender_thread(void* arg)
{
    DtlsGarbageSenderArg* a = static_cast<DtlsGarbageSenderArg*>(arg);
    assert(a != nullptr);

    // Let server enter server_wait_and_handshake() first
    usleep(150000U);  // 150 ms

    int raw = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw < 0) { a->result = -1; return nullptr; }

    struct sockaddr_in dest;
    (void)memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(a->target_port);
    int pton_res = inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    if (pton_res != 1) {
        (void)close(raw);
        a->result = -2;
        return nullptr;
    }

    // DTLS record with content type 0xFF (undefined) — causes a fatal handshake
    // error in mbedTLS, exercising run_dtls_handshake's fatal-error return path.
    static const uint8_t fake_record[16U] = {
        0xFFU,                                  // invalid content type
        0xFEU, 0xFDU,                           // DTLS 1.2 version
        0x00U, 0x01U,                           // epoch
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, // sequence
        0x00U, 0x01U,                           // length = 1
        0x00U,                                  // payload
        0x00U, 0x00U                            // padding
    };
    ssize_t sent = sendto(raw, fake_record, sizeof(fake_record), 0,
                          reinterpret_cast<const struct sockaddr*>(&dest),
                          static_cast<socklen_t>(sizeof(dest)));
    a->result = (sent == static_cast<ssize_t>(sizeof(fake_record))) ? 0 : -3;
    (void)close(raw);
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: DTLS init with bad key file → pk_parse_keyfile failure → ERR_IO
// Covers: setup_dtls_config() pk_parse_keyfile error branch
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_bad_key_path()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14587U, true);
    // Valid cert, non-existent key → pk_parse_keyfile returns error
    uint32_t path_max = static_cast<uint32_t>(sizeof(cfg.tls.key_file)) - 1U;
    (void)strncpy(cfg.tls.key_file, "/tmp/does_not_exist.key", path_max);
    cfg.tls.key_file[path_max] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_dtls_bad_key_path\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: DTLS init with bad CA cert file when verify_peer=true → ERR_IO
// Covers: setup_dtls_config() ca_file parse failure branch (L144-149)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_bad_ca_cert_file()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14588U, true);
    cfg.tls.verify_peer = true;
    uint32_t path_max = static_cast<uint32_t>(sizeof(cfg.tls.ca_file)) - 1U;
    (void)strncpy(cfg.tls.ca_file, "/tmp/bad_ca.crt", path_max);
    cfg.tls.ca_file[path_max] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_dtls_bad_ca_cert_file\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: DTLS server init timeout — no client → poll() returns 0 → ERR_TIMEOUT
// Covers: server_wait_and_handshake() pr <= 0 branch (L273-277)
// Verifies: REQ-4.1.2, REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_init_timeout()
{
    // Verifies: REQ-4.1.2, REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14589U, true);
    cfg.connect_timeout_ms = 250U;  // short timeout; no client will connect

    Result res = backend.init(cfg);
    assert(res == Result::ERR_TIMEOUT);
    assert(backend.is_open() == false);

    printf("PASS: test_dtls_server_init_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: DTLS client with invalid peer IP → inet_pton failure → ERR_IO
// Covers: client_connect_and_handshake() inet_pton != 1 branch (L376-379)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_client_bad_peer_ip()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    // is_server=false, tls_on=true; bind_port=0 (OS assigns ephemeral)
    make_dtls_config(cfg, false, 14590U, true);
    // Override peer_ip to an invalid address string
    (void)strncpy(cfg.peer_ip, "999.999.999.999",
                  static_cast<uint32_t>(sizeof(cfg.peer_ip)) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';

    // init() → setup_dtls_config (succeeds) → client_connect_and_handshake
    // → inet_pton("999.999.999.999") returns 0 → ERR_IO
    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_dtls_client_bad_peer_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: init with invalid bind IP → socket_bind failure → ERR_IO
// Covers: init() socket_bind failure branch (L531-537)
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_init_bad_bind_ip()
{
    // Verifies: REQ-4.1.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14591U, false);
    // Override bind_ip to an address that inet_aton() will reject
    (void)strncpy(cfg.bind_ip, "999.999.999.999",
                  static_cast<uint32_t>(sizeof(cfg.bind_ip)) - 1U);
    cfg.bind_ip[sizeof(cfg.bind_ip) - 1U] = '\0';

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(backend.is_open() == false);

    printf("PASS: test_init_bad_bind_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: DTLS server receive after client sends close_notify (no data)
// Client closes immediately after handshake → server ssl_read returns
// MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY → L441-442 fires → ERR_TIMEOUT
// Covers: recv_one_dtls_datagram() ssl_read error != WANT_READ/TIMEOUT (L441-442)
// Verifies: REQ-4.1.3, REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_recv_after_client_close()
{
    // Verifies: REQ-4.1.3, REQ-6.4.1
    static const uint16_t PORT = 14592U;

    DtlsSimpleServerArg srv_arg;
    srv_arg.port            = PORT;
    srv_arg.recv_timeout_ms = 1000U;
    srv_arg.init_result     = Result::ERR_IO;
    srv_arg.recv_result     = Result::OK;  // sentinel — expect non-OK (no data sent)

    pthread_attr_t srv_attr;
    assert(pthread_attr_init(&srv_attr) == 0);
    assert(pthread_attr_setstacksize(&srv_attr, static_cast<size_t>(4U) * 1024U * 1024U) == 0);

    pthread_t srv_tid;
    assert(pthread_create(&srv_tid, &srv_attr, dtls_simple_server_thread,
                          &srv_arg) == 0);
    (void)pthread_attr_destroy(&srv_attr);

    // Client: handshake completes, sends close_notify immediately (no app data)
    DtlsClientArg cli_arg;
    cli_arg.port   = PORT;
    cli_arg.tls_on = true;
    cli_arg.result = Result::ERR_IO;

    pthread_attr_t cli_attr;
    assert(pthread_attr_init(&cli_attr) == 0);
    assert(pthread_attr_setstacksize(&cli_attr, static_cast<size_t>(4U) * 1024U * 1024U) == 0);

    pthread_t cli_tid;
    assert(pthread_create(&cli_tid, &cli_attr, dtls_early_close_client_thread,
                          &cli_arg) == 0);
    (void)pthread_attr_destroy(&cli_attr);

    (void)pthread_join(srv_tid, nullptr);
    (void)pthread_join(cli_tid, nullptr);

    assert(srv_arg.init_result == Result::OK);
    assert(cli_arg.result      == Result::OK);
    // Client sent no data → server receive must fail (ERR_TIMEOUT or ERR_IO)
    assert(srv_arg.recv_result != Result::OK);

    printf("PASS: test_dtls_server_recv_after_client_close\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: plaintext receive of a garbage datagram → deserialize fails
// Covers: recv_one_dtls_datagram() deserialize failure branch (L459-464)
// Verifies: REQ-4.1.3, REQ-6.4.5
// ─────────────────────────────────────────────────────────────────────────────

static void test_plaintext_recv_garbage_datagram()
{
    // Verifies: REQ-4.1.3, REQ-6.4.5
    static const uint16_t PORT = 14593U;

    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, PORT, false);
    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    // Send 5 garbage bytes via raw UDP socket — Serializer::deserialize will fail
    int raw = socket(AF_INET, SOCK_DGRAM, 0);
    assert(raw >= 0);

    struct sockaddr_in dest;
    (void)memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(PORT);
    int pton_res = inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr);
    assert(pton_res == 1);

    static const uint8_t garbage[5U] = {0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U};
    ssize_t sent = sendto(raw, garbage, sizeof(garbage), 0,
                          reinterpret_cast<const struct sockaddr*>(&dest),
                          static_cast<socklen_t>(sizeof(dest)));
    assert(sent == static_cast<ssize_t>(sizeof(garbage)));
    (void)close(raw);

    // receive_message times out: recv_one_dtls_datagram logs deserialize failure
    // and returns false; no valid envelope ever arrives.
    MessageEnvelope env;
    Result recv_res = backend.receive_message(env, 400U);
    assert(recv_res == Result::ERR_TIMEOUT);

    backend.close();

    printf("PASS: test_plaintext_recv_garbage_datagram\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: DTLS server receives garbage record during handshake → ERR_IO
// Covers: run_dtls_handshake() fatal ssl_handshake error branch (L241-243)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_handshake_garbage()
{
    // Verifies: REQ-6.4.1
    static const uint16_t PORT = 14594U;

    DtlsGarbageSenderArg sender_arg;
    sender_arg.target_port = PORT;
    sender_arg.result      = -99;

    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, static_cast<size_t>(512U) * 1024U) == 0);

    pthread_t tid;
    assert(pthread_create(&tid, &attr, dtls_garbage_sender_thread,
                          &sender_arg) == 0);
    (void)pthread_attr_destroy(&attr);

    // Server waits for first datagram, gets garbage, handshake fails → ERR_IO
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, PORT, true);
    cfg.connect_timeout_ms = 3000U;

    Result init_res = backend.init(cfg);

    (void)pthread_join(tid, nullptr);

    // Garbage record causes fatal handshake error (not WANT_READ/WANT_WRITE/
    // HELLO_VERIFY_REQUIRED) → run_dtls_handshake returns ERR_IO
    assert(init_res != Result::OK);
    assert(backend.is_open() == false);
    assert(sender_arg.result == 0);

    printf("PASS: test_dtls_server_handshake_garbage\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: plaintext send to invalid peer IP → socket_send_to failure → ERR_IO
// Covers: send_message() socket_send_to error branch (L612-618)
// Verifies: REQ-4.1.2, REQ-6.4.5
// ─────────────────────────────────────────────────────────────────────────────

static void test_plaintext_send_bad_peer_ip()
{
    // Verifies: REQ-4.1.2, REQ-6.4.5
    DtlsUdpBackend backend;
    TransportConfig cfg;
    // Plaintext client: valid bind_ip, ephemeral bind_port, bad peer_ip.
    // init() (plaintext) binds the socket without using peer_ip → succeeds.
    // send_message() calls socket_send_to with the invalid peer_ip → ERR_IO.
    make_dtls_config(cfg, false, 14595U, false);
    (void)strncpy(cfg.peer_ip, "999.999.999.999",
                  static_cast<uint32_t>(sizeof(cfg.peer_ip)) - 1U);
    cfg.peer_ip[sizeof(cfg.peer_ip) - 1U] = '\0';

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xBEEFULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    Result send_res = backend.send_message(env);
    assert(send_res == Result::ERR_IO);

    backend.close();
    printf("PASS: test_plaintext_send_bad_peer_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: receive_message() with timeout_ms > 5000 → poll_count clamped to 50
// Covers: receive_message() L654 `if (poll_count > 50U)` True branch
// Verifies: REQ-4.1.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_receive_long_timeout_clamp()
{
    // Verifies: REQ-4.1.3
    static const uint16_t PORT = 14596U;

    DtlsUdpBackend server;
    TransportConfig srv_cfg;
    make_dtls_config(srv_cfg, true, PORT, false);
    Result init_res = server.init(srv_cfg);
    assert(init_res == Result::OK);
    assert(server.is_open() == true);

    // Client sends after 150 ms
    DtlsClientArg cli_arg;
    cli_arg.port   = PORT;
    cli_arg.tls_on = false;
    cli_arg.result = Result::ERR_IO;

    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, static_cast<size_t>(2U) * 1024U * 1024U) == 0);
    pthread_t cli_tid;
    assert(pthread_create(&cli_tid, &attr, dtls_client_thread, &cli_arg) == 0);
    (void)pthread_attr_destroy(&attr);

    // timeout_ms=5001 → poll_count = (5001+99)/100 = 51 > 50 → clamped to 50.
    // Message arrives at ~150 ms so receive exits on the first or second poll.
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 5001U);

    (void)pthread_join(cli_tid, nullptr);
    server.close();

    assert(recv_res == Result::OK);
    assert(cli_arg.result == Result::OK);
    assert(received.message_id == 0xD715ULL);
    printf("PASS: test_receive_long_timeout_clamp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: DTLS server, verify_peer=true but ca_file="" → skips CA loading
// Covers: setup_dtls_config() `ca_file[0] != '\0'` False branch (inner &&, L144)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_verify_peer_no_ca()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14597U, true);
    // verify_peer=true but ca_file remains "" (zero-initialized by transport_config_default).
    // setup_dtls_config: `verify_peer && ca_file[0] != '\0'` → True && False → skip CA block.
    // Cert and key parse OK; server_wait_and_handshake times out after 200 ms.
    cfg.tls.verify_peer    = true;
    cfg.connect_timeout_ms = 200U;

    Result res = backend.init(cfg);
    assert(res == Result::ERR_TIMEOUT);
    assert(backend.is_open() == false);
    printf("PASS: test_dtls_server_verify_peer_no_ca\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: DTLS server, verify_peer=true with valid ca_file → CA cert loads OK
// Covers: setup_dtls_config() CA cert parse success — `if (ret != 0)` False path
//         (mbedtls_ssl_conf_ca_chain is called at L150-151)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_valid_ca_cert_load()
{
    // Verifies: REQ-6.4.1
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14598U, true);
    // Use the self-signed test cert as both CA and server cert.
    // CA cert parse succeeds → `if (ret != 0)` False → ssl_conf_ca_chain called.
    // No client connects → server_wait_and_handshake times out after 200 ms.
    cfg.tls.verify_peer = true;
    uint32_t path_max   = static_cast<uint32_t>(sizeof(cfg.tls.ca_file)) - 1U;
    (void)strncpy(cfg.tls.ca_file, DTLS_TEST_CERT_FILE, path_max);
    cfg.tls.ca_file[path_max]  = '\0';
    cfg.connect_timeout_ms     = 200U;

    Result res = backend.init(cfg);
    assert(res == Result::ERR_TIMEOUT);
    assert(backend.is_open() == false);
    printf("PASS: test_dtls_server_valid_ca_cert_load\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: DTLS server, connect_timeout_ms=0 → wait_ms defaults to 30000
// Covers: server_wait_and_handshake() ternary False branch (L271-272):
//         `connect_timeout_ms > 0U` False → wait_ms = 30000U
// Verifies: REQ-4.1.1, REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dtls_server_zero_connect_timeout()
{
    // Verifies: REQ-4.1.1, REQ-6.4.1
    static const uint16_t PORT = 14599U;

    // Client thread waits 150 ms before connecting, giving the server time to
    // enter server_wait_and_handshake and start the 30 s poll.
    DtlsClientArg cli_arg;
    cli_arg.port   = PORT;
    cli_arg.tls_on = true;
    cli_arg.result = Result::ERR_IO;

    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, static_cast<size_t>(4U) * 1024U * 1024U) == 0);
    pthread_t cli_tid;
    assert(pthread_create(&cli_tid, &attr, dtls_client_thread, &cli_arg) == 0);
    (void)pthread_attr_destroy(&attr);

    // Server in main test body: connect_timeout_ms=0 → ternary False → wait_ms=30000U.
    // Client arrives at ~150 ms so poll() returns well before the 30 s limit.
    DtlsUdpBackend server;
    TransportConfig srv_cfg;
    make_dtls_config(srv_cfg, true, PORT, true);
    srv_cfg.connect_timeout_ms = 0U;

    Result init_res = server.init(srv_cfg);  // blocks ~150 ms while client connects

    MessageEnvelope received;
    Result recv_res = Result::ERR_IO;
    if (init_res == Result::OK) {
        recv_res = server.receive_message(received, 5000U);
        server.close();
    }

    (void)pthread_join(cli_tid, nullptr);

    assert(init_res == Result::OK);
    assert(cli_arg.result == Result::OK);
    assert(recv_res == Result::OK);
    assert(received.message_id == 0xD715ULL);
    printf("PASS: test_dtls_server_zero_connect_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// DtlsMockOps — concrete IMbedtlsOps for error-path injection tests
//
// Each method returns its configurable r_* field without calling any real
// mbedTLS or POSIX function.  This allows every hard error branch in
// DtlsUdpBackend that cannot be triggered in a loopback environment to be
// exercised deterministically.
//
// Power of 10 Rule 9 exception: virtual dispatch per CLAUDE.md §2 Rule 9
// vtable exemption; no explicit function pointer declarations here.
// ─────────────────────────────────────────────────────────────────────────────

struct DtlsMockOps : public IMbedtlsOps {
    // Configurable return values; default = success for every call.
    psa_status_t r_crypto_init                   = PSA_SUCCESS;
    int          r_ssl_config_defaults            = 0;
    int          r_x509_call1                     = 0;  ///< 1st x509_crt_parse_file
    int          r_x509_call2                     = 0;  ///< 2nd x509_crt_parse_file
    int          r_pk_parse                       = 0;
    int          r_ssl_conf_own_cert              = 0;
    int          r_ssl_cookie_setup               = 0;
    ssize_t      r_recvfrom_peek                  = 1;  ///< > 0 = success
    int          r_net_connect                    = 0;
    int          r_ssl_setup                      = 0;
    int          r_ssl_set_client_transport_id    = 0;
    int          r_inet_pton                      = 1;  ///< 1 = success
    int          r_ssl_write                      = 1;  ///< > 0 = success
    int          r_ssl_handshake                  = 0;  ///< 0 = immediate success; WANT_READ = iterate
    int          r_ssl_read                       = MBEDTLS_ERR_SSL_TIMEOUT;  ///< default: timeout

    int x509_call_count = 0;  ///< tracks x509_crt_parse_file invocation index

    ~DtlsMockOps() override {}

    psa_status_t crypto_init() override { return r_crypto_init; }

    int ssl_config_defaults(mbedtls_ssl_config* /*conf*/, int /*endpoint*/,
                            int /*transport*/, int /*preset*/) override
    {
        return r_ssl_config_defaults;
    }

    int x509_crt_parse_file(mbedtls_x509_crt* /*chain*/,
                            const char*       /*path*/) override
    {
        ++x509_call_count;
        return (x509_call_count == 1) ? r_x509_call1 : r_x509_call2;
    }

    int pk_parse_keyfile(mbedtls_pk_context* /*ctx*/, const char* /*path*/,
                         const char* /*pwd*/) override
    {
        return r_pk_parse;
    }

    int ssl_conf_own_cert(mbedtls_ssl_config* /*conf*/,
                          mbedtls_x509_crt*   /*own_cert*/,
                          mbedtls_pk_context* /*pk_key*/) override
    {
        return r_ssl_conf_own_cert;
    }

    int ssl_cookie_setup(mbedtls_ssl_cookie_ctx* /*ctx*/) override
    {
        return r_ssl_cookie_setup;
    }

    int ssl_setup(mbedtls_ssl_context* /*ssl*/,
                  mbedtls_ssl_config*  /*conf*/) override
    {
        return r_ssl_setup;
    }

    int ssl_set_client_transport_id(mbedtls_ssl_context*  /*ssl*/,
                                    const unsigned char*  /*info*/,
                                    size_t                /*ilen*/) override
    {
        return r_ssl_set_client_transport_id;
    }

    int ssl_write(mbedtls_ssl_context* /*ssl*/, const unsigned char* /*buf*/,
                  size_t /*len*/) override
    {
        return r_ssl_write;
    }

    int ssl_handshake(mbedtls_ssl_context* /*ssl*/) override
    {
        return r_ssl_handshake;
    }

    int ssl_read(mbedtls_ssl_context* /*ssl*/, unsigned char* /*buf*/,
                 size_t /*len*/) override
    {
        return r_ssl_read;
    }

    ssize_t recvfrom_peek(int /*sockfd*/, void* /*buf*/, size_t /*len*/,
                          struct sockaddr* /*src_addr*/,
                          socklen_t* /*addrlen*/) override
    {
        return r_recvfrom_peek;
    }

    int net_connect(int /*sockfd*/, const struct sockaddr* /*addr*/,
                    socklen_t /*addrlen*/) override
    {
        return r_net_connect;
    }

    int inet_pton_ipv4(const char* /*src*/, void* /*dst*/) override
    {
        return r_inet_pton;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Trigger-datagram helpers for server-mode mock tests
//
// server_wait_and_handshake() blocks in poll() until a datagram arrives.
// These helpers fire a single UDP byte at the server port after a delay so
// that poll() unblocks and the code under test (recvfrom_peek, net_connect,
// ssl_setup, ssl_set_client_transport_id) is reached.
// ─────────────────────────────────────────────────────────────────────────────

static void send_udp_trigger(uint16_t port)
{
    int raw = socket(AF_INET, SOCK_DGRAM, 0);
    if (raw < 0) { return; }

    struct sockaddr_in dest;
    (void)memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &dest.sin_addr) != 1) {
        (void)close(raw);
        return;
    }

    static const uint8_t byte = 0xAAU;
    (void)sendto(raw, &byte, sizeof(byte), 0,
                 reinterpret_cast<const struct sockaddr*>(&dest),
                 static_cast<socklen_t>(sizeof(dest)));
    (void)close(raw);
}

struct MockTriggerArg {
    uint16_t port;
    uint32_t delay_ms;
};

static void* mock_trigger_thread(void* arg)
{
    MockTriggerArg* a = static_cast<MockTriggerArg*>(arg);
    assert(a != nullptr);
    usleep(static_cast<useconds_t>(a->delay_ms) * 1000U);
    send_udp_trigger(a->port);
    return nullptr;
}

// Start a trigger thread that sends one UDP datagram to `port` after 150 ms.
// Returns the thread ID (caller must pthread_join).
static pthread_t start_trigger_thread(MockTriggerArg& trig_arg, uint16_t port)
{
    trig_arg.port     = port;
    trig_arg.delay_ms = 150U;

    pthread_attr_t attr;
    (void)pthread_attr_init(&attr);
    (void)pthread_attr_setstacksize(&attr, static_cast<size_t>(256U) * 1024U);

    pthread_t tid;
    (void)pthread_create(&tid, &attr, mock_trigger_thread, &trig_arg);
    (void)pthread_attr_destroy(&attr);
    return tid;
}

// Helper — build a TLS-enabled server TransportConfig for mock tests.
// Cert file is DTLS_TEST_CERT_FILE (written during write_pem_files()).
// The mock's x509_crt_parse_file never reads the file; path must be non-empty
// to pass the NEVER_COMPILED_OUT_ASSERT in setup_dtls_config().
static void make_mock_server_cfg(TransportConfig& cfg, uint16_t port)
{
    make_dtls_config(cfg, true, port, true);
    cfg.connect_timeout_ms = 3000U;
}

// Helper — build a TLS-enabled client TransportConfig for mock tests.
static void make_mock_client_cfg(TransportConfig& cfg, uint16_t peer_port)
{
    make_dtls_config(cfg, false, peer_port, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 1: psa_crypto_init failure → setup_dtls_config → ERR_IO
// Covers: setup_dtls_config() L132-136 (crypto_init != PSA_SUCCESS)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_crypto_init_fail()
{
    // Verifies: REQ-6.4.1
    DtlsMockOps mock;
    mock.r_crypto_init = static_cast<psa_status_t>(-1);  // any non-zero failure

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, 14600U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_crypto_init_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 2: ssl_config_defaults failure → setup_dtls_config → ERR_IO
// Covers: setup_dtls_config() L148-151 (ssl_config_defaults != 0)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_ssl_config_defaults_fail()
{
    // Verifies: REQ-6.4.1
    DtlsMockOps mock;
    mock.r_ssl_config_defaults = -1;

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, 14601U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_ssl_config_defaults_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 3: ssl_conf_own_cert failure → setup_dtls_config → ERR_IO
// Covers: setup_dtls_config() L185-189 (ssl_conf_own_cert != 0)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_ssl_conf_own_cert_fail()
{
    // Verifies: REQ-6.4.1
    DtlsMockOps mock;
    mock.r_ssl_conf_own_cert = -1;  // crypto/config_defaults/x509/pk all succeed

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, 14602U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_ssl_conf_own_cert_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 4: ssl_cookie_setup failure → setup_dtls_config → ERR_IO
// Covers: setup_dtls_config() L198-202 (ssl_cookie_setup != 0, server role)
// Verifies: REQ-6.4.1, REQ-6.4.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_ssl_cookie_setup_fail()
{
    // Verifies: REQ-6.4.1, REQ-6.4.2
    DtlsMockOps mock;
    mock.r_ssl_cookie_setup = -1;

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, 14603U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_ssl_cookie_setup_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 5: recvfrom(MSG_PEEK) failure → server_wait_and_handshake → ERR_IO
// Covers: server_wait_and_handshake() L311-315 (recvfrom_peek < 0)
// A trigger datagram unblocks poll(); mock then returns -1 from recvfrom_peek.
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_server_recvfrom_peek_fail()
{
    // Verifies: REQ-6.4.1
    static const uint16_t PORT = 14604U;

    DtlsMockOps mock;
    mock.r_recvfrom_peek = -1;  // all setup succeeds; recvfrom_peek fails

    MockTriggerArg trig_arg;
    pthread_t trig_tid = start_trigger_thread(trig_arg, PORT);

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, PORT);

    Result res = backend.init(cfg);

    (void)pthread_join(trig_tid, nullptr);

    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_server_recvfrom_peek_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 6: connect() failure → server_wait_and_handshake → ERR_IO
// Covers: server_wait_and_handshake() L320-325 (net_connect < 0)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_server_net_connect_fail()
{
    // Verifies: REQ-6.4.1
    static const uint16_t PORT = 14605U;

    DtlsMockOps mock;
    mock.r_net_connect = -1;  // recvfrom_peek succeeds; connect fails

    MockTriggerArg trig_arg;
    pthread_t trig_tid = start_trigger_thread(trig_arg, PORT);

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, PORT);

    Result res = backend.init(cfg);

    (void)pthread_join(trig_tid, nullptr);

    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_server_net_connect_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 7: ssl_setup failure (server) → server_wait_and_handshake → ERR_IO
// Covers: server_wait_and_handshake() L330-334 (ssl_setup != 0)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_server_ssl_setup_fail()
{
    // Verifies: REQ-6.4.1
    static const uint16_t PORT = 14606U;

    DtlsMockOps mock;
    mock.r_ssl_setup = -1;  // recvfrom_peek and connect succeed; ssl_setup fails

    MockTriggerArg trig_arg;
    pthread_t trig_tid = start_trigger_thread(trig_arg, PORT);

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, PORT);

    Result res = backend.init(cfg);

    (void)pthread_join(trig_tid, nullptr);

    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_server_ssl_setup_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 8: ssl_set_client_transport_id failure → server_wait_and_handshake
// Covers: server_wait_and_handshake() L354-361 (ssl_set_client_transport_id != 0)
// Verifies: REQ-6.4.1, REQ-6.4.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_server_ssl_set_transport_id_fail()
{
    // Verifies: REQ-6.4.1, REQ-6.4.2
    static const uint16_t PORT = 14607U;

    DtlsMockOps mock;
    // ssl_setup succeeds (mock); set_client_transport_id fails
    mock.r_ssl_set_client_transport_id = -1;

    MockTriggerArg trig_arg;
    pthread_t trig_tid = start_trigger_thread(trig_arg, PORT);

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_server_cfg(cfg, PORT);

    Result res = backend.init(cfg);

    (void)pthread_join(trig_tid, nullptr);

    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_server_ssl_set_transport_id_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 9: connect() failure (client) → client_connect_and_handshake → ERR_IO
// Covers: client_connect_and_handshake() L403-410 (net_connect < 0)
// No trigger datagram needed — client path has no blocking poll().
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_client_net_connect_fail()
{
    // Verifies: REQ-6.4.1
    DtlsMockOps mock;
    mock.r_net_connect = -1;  // inet_pton (mock) succeeds; connect fails

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_client_cfg(cfg, 14608U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_client_net_connect_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test 10: ssl_setup failure (client) → client_connect_and_handshake → ERR_IO
// Covers: client_connect_and_handshake() L414-418 (ssl_setup != 0)
// Verifies: REQ-6.4.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_client_ssl_setup_fail()
{
    // Verifies: REQ-6.4.1
    DtlsMockOps mock;
    // connect succeeds (mock returns 0); ssl_setup fails
    mock.r_ssl_setup = -1;

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_client_cfg(cfg, 14609U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_client_ssl_setup_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// ISocketOps mock test 1: socket_create_udp failure → init → ERR_IO
// Covers: DtlsUdpBackend::init() L559-564 (create_udp < 0)
// Verifies: REQ-6.4.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_sock_create_udp_fail()
{
    // Verifies: REQ-6.4.5, REQ-6.3.2
    MockSocketOps sock_mock;
    sock_mock.fail_create_udp = true;

    DtlsMockOps tls_mock;  // all TLS ops succeed (not called in plaintext mode)

    DtlsUdpBackend backend(sock_mock, tls_mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_dtls_config(cfg, true, 14620U, false);  // plaintext server

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_sock_create_udp_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// ISocketOps mock test 2: socket_set_reuseaddr failure → init → ERR_IO
// Covers: DtlsUdpBackend::init() L566-572 (set_reuseaddr false)
// Verifies: REQ-6.4.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_mock_sock_reuseaddr_fail()
{
    // Verifies: REQ-6.4.5, REQ-6.3.2
    MockSocketOps sock_mock;
    sock_mock.fail_set_reuseaddr = true;

    DtlsMockOps tls_mock;  // all TLS ops succeed (not called in plaintext mode)

    DtlsUdpBackend backend(sock_mock, tls_mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_dtls_config(cfg, true, 14621U, false);  // plaintext server

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());
    assert(sock_mock.n_do_close >= 1);  // do_close called after reuseaddr failure

    printf("PASS: test_mock_sock_reuseaddr_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: init() with num_channels == 0 → impairment_config_default used
// Covers: init() L554 `if (config.num_channels > 0U)` False branch
// Verifies: REQ-4.1.1
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-4.1.1
static void test_init_num_channels_zero()
{
    // Verifies: REQ-4.1.1
    // With num_channels == 0 the False branch of `if (config.num_channels > 0U)`
    // is taken and impairment_config_default() provides the ImpairmentConfig.
    DtlsUdpBackend backend;
    TransportConfig cfg;
    make_dtls_config(cfg, true, 14622U, false);
    cfg.num_channels = 0U;  // force False branch at L554

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open() == true);

    backend.close();
    assert(backend.is_open() == false);

    printf("PASS: test_init_num_channels_zero\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: send_message() with loss_probability == 1.0 → silent drop → OK
// Covers: send_message() L637 `if (res == Result::ERR_IO)` True branch
//         (impairment engine drops all outbound messages)
// Verifies: REQ-5.1.3
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-5.1.3
static void test_loss_impairment_drops_send()
{
    // Verifies: REQ-5.1.3
    // With loss_probability == 1.0 every outbound message is silently dropped
    // by the impairment engine.  send_message() should return OK (silent drop)
    // and a subsequent receive_message() with 0 ms timeout should return
    // ERR_TIMEOUT (nothing was actually transmitted).
    static const uint16_t SRV_PORT = 14623U;

    // Server (receiver): bind plaintext UDP, no impairment
    DtlsUdpBackend server;
    TransportConfig srv_cfg;
    make_dtls_config(srv_cfg, true, SRV_PORT, false);
    Result srv_init = server.init(srv_cfg);
    assert(srv_init == Result::OK);
    assert(server.is_open() == true);

    // Client (sender): loss_probability = 1.0 → every outbound message dropped
    DtlsUdpBackend client;
    TransportConfig cli_cfg;
    make_dtls_config(cli_cfg, false, SRV_PORT, false);
    cli_cfg.num_channels = 1U;
    cli_cfg.channels[0U].impairment.enabled          = true;
    cli_cfg.channels[0U].impairment.loss_probability = 1.0;

    Result cli_init = client.init(cli_cfg);
    assert(cli_init == Result::OK);
    assert(client.is_open() == true);

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xAB01ULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    // send_message returns OK (ERR_IO from impairment → silently converted to OK)
    Result send_res = client.send_message(env);
    assert(send_res == Result::OK);

    // Nothing was transmitted; server must time out immediately
    MessageEnvelope received;
    Result recv_res = server.receive_message(received, 0U);
    assert(recv_res == Result::ERR_TIMEOUT);

    client.close();
    server.close();

    printf("PASS: test_loss_impairment_drops_send\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: fixed_latency_ms impairment → flush_delayed_to_queue loop body
//          and post-flush recv_queue.pop() succeed
// Covers: flush_delayed_to_queue() L531 loop body True branch (Branch 2)
//         receive_message() L711 `if (result_ok(res))` True branch (Branch 4)
// Verifies: REQ-5.1.1
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-5.1.1
static void test_delay_impairment_flush_and_recv()
{
    // Verifies: REQ-5.1.1
    // With fixed_latency_ms == 1 the impairment engine buffers the outbound
    // message in the delay buffer.  After sleeping 10 ms the release time has
    // passed.  When the receiver calls receive_message(), flush_delayed_to_queue()
    // iterates over the deliverable entries (loop body True, Branch 2), pushes
    // the message into recv_queue, and the post-flush pop succeeds (Branch 4).
    static const uint16_t SRV_PORT = 14624U;

    // Server (receiver): no impairment — just receives
    DtlsUdpBackend server;
    TransportConfig srv_cfg;
    make_dtls_config(srv_cfg, true, SRV_PORT, false);
    Result srv_init = server.init(srv_cfg);
    assert(srv_init == Result::OK);
    assert(server.is_open() == true);

    // Client (sender): 1 ms fixed latency → message goes into delay buffer
    DtlsUdpBackend client;
    TransportConfig cli_cfg;
    make_dtls_config(cli_cfg, false, SRV_PORT, false);
    cli_cfg.num_channels = 1U;
    cli_cfg.channels[0U].impairment.enabled        = true;
    cli_cfg.channels[0U].impairment.fixed_latency_ms = 1U;

    Result cli_init = client.init(cli_cfg);
    assert(cli_init == Result::OK);
    assert(client.is_open() == true);

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xAB02ULL;
    env.source_id         = 2U;
    env.destination_id    = 1U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    // Send: goes into delay buffer (not sent to socket yet)
    Result send_res = client.send_message(env);
    assert(send_res == Result::OK);

    // Wait for the 1 ms latency to expire so the message is deliverable
    usleep(10000U);  // 10 ms

    // receive_message calls flush_delayed_to_queue which iterates loop body
    // (Branch 2) and pushes the message; then the post-flush pop succeeds (Branch 4)
    MessageEnvelope received;
    Result recv_res = client.receive_message(received, 500U);
    assert(recv_res == Result::OK);
    assert(received.message_id == 0xAB02ULL);

    client.close();
    server.close();

    printf("PASS: test_delay_impairment_flush_and_recv\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test: ssl_write failure → send_message returns ERR_IO
//
// Client mode.  All init calls return success (crypto_init, ssl_config_defaults,
// x509_crt_parse_file ×2, pk_parse_keyfile, ssl_conf_own_cert, ssl_setup all
// return 0; ssl_handshake returns 0 = immediate success).
// After init(), set r_ssl_write = -1 so the next send_message() injects the
// ssl_write error path.
//
// Covers: DtlsUdpBackend::send_message() ssl_write < 0 → ERR_IO branch
// Verifies: REQ-6.4.1, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.4.1, REQ-6.3.2
static void test_mock_dtls_ssl_write_fail()
{
    // Verifies: REQ-6.4.1, REQ-6.3.2
    DtlsMockOps mock;
    // All init paths succeed; handshake completes in one call
    mock.r_ssl_handshake = 0;

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_client_cfg(cfg, 14630U);

    Result res = backend.init(cfg);
    assert(res == Result::OK);
    assert(backend.is_open());

    // Inject ssl_write failure for the send call
    mock.r_ssl_write = -1;

    MessageEnvelope env;
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = 0xC001ULL;
    env.source_id         = 1U;
    env.destination_id    = 2U;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;

    Result send_res = backend.send_message(env);
    assert(send_res == Result::ERR_IO);

    backend.close();
    printf("PASS: test_mock_dtls_ssl_write_fail\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test: ssl_handshake iteration limit exceeded → ERR_IO
//
// Client mode.  ssl_handshake always returns MBEDTLS_ERR_SSL_WANT_READ so the
// retry loop exhausts all 32 iterations and returns ERR_IO.
//
// Covers: run_dtls_handshake() L290 `if (!done)` True → ERR_IO branch
// Verifies: REQ-6.4.1, REQ-6.4.3
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.4.1, REQ-6.4.3
static void test_mock_dtls_handshake_iteration_limit()
{
    // Verifies: REQ-6.4.1, REQ-6.4.3
    DtlsMockOps mock;
    // Always return WANT_READ so the 32-iteration limit is hit
    mock.r_ssl_handshake = MBEDTLS_ERR_SSL_WANT_READ;

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_client_cfg(cfg, 14631U);

    Result res = backend.init(cfg);
    assert(res == Result::ERR_IO);
    assert(!backend.is_open());

    printf("PASS: test_mock_dtls_handshake_iteration_limit\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock test: ssl_read returns a fatal error → recv_one_dtls_datagram returns false
//            → receive_message returns ERR_TIMEOUT
//
// Client mode.  ssl_handshake succeeds (0); ssl_read returns
// MBEDTLS_ERR_SSL_INTERNAL_ERROR (fatal, not WANT_READ / TIMEOUT) triggering the
// log_mbedtls_err() path and early return false from recv_one_dtls_datagram.
//
// Covers: recv_one_dtls_datagram() L483 `ret <= 0` True + L484 inner False branch
// Verifies: REQ-6.4.1, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

// Verifies: REQ-6.4.1, REQ-6.3.2
static void test_mock_dtls_ssl_read_error()
{
    // Verifies: REQ-6.4.1, REQ-6.3.2
    DtlsMockOps mock;
    mock.r_ssl_handshake = 0;                              // init succeeds
    mock.r_ssl_read      = MBEDTLS_ERR_SSL_INTERNAL_ERROR; // fatal read error

    DtlsUdpBackend backend(mock);
    assert(!backend.is_open());

    TransportConfig cfg;
    make_mock_client_cfg(cfg, 14632U);

    Result init_res = backend.init(cfg);
    assert(init_res == Result::OK);
    assert(backend.is_open());

    MessageEnvelope received;
    Result recv_res = backend.receive_message(received, 200U);
    assert(recv_res == Result::ERR_TIMEOUT);

    backend.close();
    printf("PASS: test_mock_dtls_ssl_read_error\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    write_pem_files();

    test_plaintext_server_bind();
    test_dtls_bad_cert();
    test_plaintext_loopback();
    test_dtls_loopback();
    test_oversized_payload_rejected();
    test_receive_timeout();
    test_dtls_bad_key_path();
    test_dtls_bad_ca_cert_file();
    test_dtls_server_init_timeout();
    test_dtls_client_bad_peer_ip();
    test_init_bad_bind_ip();
    test_dtls_server_recv_after_client_close();
    test_plaintext_recv_garbage_datagram();
    test_dtls_server_handshake_garbage();
    test_plaintext_send_bad_peer_ip();
    test_receive_long_timeout_clamp();
    test_dtls_server_verify_peer_no_ca();
    test_dtls_server_valid_ca_cert_load();
    test_dtls_server_zero_connect_timeout();

    // Mock error-path tests (dependency-injected IMbedtlsOps)
    test_mock_crypto_init_fail();
    test_mock_ssl_config_defaults_fail();
    test_mock_ssl_conf_own_cert_fail();
    test_mock_ssl_cookie_setup_fail();
    test_mock_server_recvfrom_peek_fail();
    test_mock_server_net_connect_fail();
    test_mock_server_ssl_setup_fail();
    test_mock_server_ssl_set_transport_id_fail();
    test_mock_client_net_connect_fail();
    test_mock_client_ssl_setup_fail();
    test_mock_dtls_ssl_write_fail();
    test_mock_dtls_handshake_iteration_limit();
    test_mock_dtls_ssl_read_error();

    // ISocketOps mock tests
    test_mock_sock_create_udp_fail();
    test_mock_sock_reuseaddr_fail();

    // New branch-coverage tests
    test_init_num_channels_zero();
    test_loss_impairment_drops_send();
    test_delay_impairment_flush_and_recv();

    printf("=== test_DtlsUdpBackend: ALL TESTS PASSED ===\n");
    return 0;
}
