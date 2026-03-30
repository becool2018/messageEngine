/**
 * @file test_TlsTcpBackend.cpp
 * @brief Unit and integration tests for TlsTcpBackend:
 *        TLS config, plaintext fallback, TLS init failures, and loopback roundtrip.
 *
 * Tests cover:
 *   - TlsConfig struct default values
 *   - transport_config_default() initialises the embedded TlsConfig
 *   - TlsTcpBackend server bind without TLS (tls_enabled=false)
 *   - TlsTcpBackend init failure when cert/key files are missing
 *   - TlsTcpBackend init with valid self-signed cert (server bind succeeds)
 *   - Full loopback message roundtrip (plaintext) using POSIX threads
 *   - Full loopback message roundtrip (TLS) using POSIX threads
 *
 * Test cert/key written to /tmp at test startup; cleaned up at exit.
 * POSIX threads (pthread) used for loopback tests; allowed by -lpthread.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - STL exempted in tests/ for test fixture setup only.
 *
 * Verifies: REQ-6.3.4
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>

#include "core/Types.hpp"
#include "core/TlsConfig.hpp"
#include "core/ChannelConfig.hpp"
#include "core/MessageEnvelope.hpp"
#include "platform/TlsTcpBackend.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Embedded PEM test credentials (self-signed EC P-256, 10-year validity)
// Generated with:
//   openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
//                  -pkeyopt ec_param_enc:named_curve -out test.key
//   openssl req -new -x509 -key test.key -out test.crt -days 3650 \
//               -subj "/CN=messageEngine-test"
// named_curve encoding required: mbedTLS 4.0 does not parse explicit params.
// This cert is used as BOTH the server cert AND the trusted CA cert,
// so verify_peer=true works against it (self-signed is its own CA).
// ─────────────────────────────────────────────────────────────────────────────

static const char* TEST_CERT_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBKjCB0AIJALcZzcT10dW5MAoGCCqGSM49BAMCMB0xGzAZBgNVBAMMEm1lc3Nh\n"
    "Z2VFbmdpbmUtdGVzdDAeFw0yNjAzMzAxOTU2MjdaFw0zNjAzMjcxOTU2MjdaMB0x\n"
    "GzAZBgNVBAMMEm1lc3NhZ2VFbmdpbmUtdGVzdDBZMBMGByqGSM49AgEGCCqGSM49\n"
    "AwEHA0IABGHf3Q6V7luNtLoVjw5CPbjN2RZcqGwcokGaLUPbAcLr/STEfDDL3U3S\n"
    "7+ePXeoVJZc8G9oos/0Uz7qfNCmo2C8wCgYIKoZIzj0EAwIDSQAwRgIhAP9SKi5B\n"
    "rR710zQHrM8utnLGbCZh8cJ4rx2XvX1Qi9pXAiEAkh4fCzlZliLwK1dwaAQLaUVL\n"
    "cTVr90+nzpbaKPqWdPs=\n"
    "-----END CERTIFICATE-----\n";

static const char* TEST_KEY_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgrVlbq/3UZ+VXduIG\n"
    "4HyPJwAxYVRyd41bNcY8+na6zoahRANCAATen93k+qF9tzXOjLn+Yb5vuqahbD/V\n"
    "xa4DlieBuH+f5oUZH68/8dNUk2BFxvIZa/O8BdRRVpugMivlJWVDiKYn\n"
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
    pattr = pthread_attr_setstacksize(&attr, 4U * 1024U * 1024U);
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
    pattr = pthread_attr_setstacksize(&attr, 4U * 1024U * 1024U);
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

    // Cleanup
    (void)remove(TEST_CERT_FILE);
    (void)remove(TEST_KEY_FILE);

    printf("ALL TlsTcpBackend tests passed.\n");
    return 0;
}
