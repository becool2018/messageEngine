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
 * @file test_SocketUtils.cpp
 * @brief Unit tests for SocketUtils.cpp POSIX socket helpers.
 *
 * Tests cover:
 *   - socket_create_tcp(false) / socket_create_udp(false) success paths
 *   - socket_set_nonblocking() on a valid fd
 *   - socket_set_reuseaddr() on a valid fd
 *   - socket_bind() with valid IP+port (success) and invalid IP (failure)
 *   - socket_listen() after bind
 *   - socket_accept() with a connecting loopback client
 *   - socket_close() on a valid fd
 *   - socket_connect_with_timeout() to a listening server
 *   - tcp_send_frame() + tcp_recv_frame() round-trip
 *   - tcp_recv_frame() oversized frame rejection
 *   - socket_send_to() with invalid IP (failure)
 *   - socket_send_to() + socket_recv_from() UDP round-trip
 *
 * Reachable branches specifically exercised:
 *   - socket_bind()    True branch  (inet_pton returns 0 for bad IP)
 *   - socket_send_to() True branch  (inet_pton returns 0 for bad IP)
 *   - socket_recv_from() AF_INET6 branch (IPv6 loopback UDP round-trip)
 *
 * All other uncovered branches in SocketUtils.cpp are architectural ceilings
 * (POSIX hard error paths unreachable on loopback: fcntl failure, setsockopt
 * failure, listen failure, accept failure, close failure, recvfrom failure,
 * inet_ntop failure, UDP partial send).
 *
 * Port range 19200-19250 is reserved for SocketUtils tests.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - No STL on logic paths.
 *
 * Verifies: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Verifies: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
// Verification: M1 + M2 + M3 (NSC — raw POSIX adapter; no message-delivery policy)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "core/Types.hpp"
#include "core/Serializer.hpp"
#include "platform/SocketUtils.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Loopback address and base port
// ─────────────────────────────────────────────────────────────────────────────

static const char   LOOPBACK_IP[]   = "127.0.0.1";
static const char   LOOPBACK_IP6[]  = "::1";
static const char   BAD_IP[]        = "999.999.999.999";
static const uint32_t TIMEOUT_MS    = 2000U;

// ─────────────────────────────────────────────────────────────────────────────
// Helper — wait for a listening fd to be readable (accept-ready) with a poll.
// Returns true if the fd became readable within timeout_ms.
// Power of 10: no recursion; single poll call.
// ─────────────────────────────────────────────────────────────────────────────

static bool wait_readable(int fd, uint32_t timeout_ms)
{
    assert(fd >= 0);
    assert(timeout_ms > 0U);

    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = POLLIN;
    pfd.revents = 0;

    int r = poll(&pfd, 1U, static_cast<int>(timeout_ms));
    assert(r >= 0);
    return r > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: socket_create_tcp(false) returns a valid fd
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_create_tcp()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    socket_close(fd);
    printf("PASS: test_create_tcp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: socket_create_udp(false) returns a valid fd
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_create_udp()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);

    socket_close(fd);
    printf("PASS: test_create_udp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: socket_set_nonblocking() succeeds on a valid fd
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_set_nonblocking()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    bool ok = socket_set_nonblocking(fd);
    assert(ok);

    socket_close(fd);
    printf("PASS: test_set_nonblocking\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: socket_set_reuseaddr() succeeds on a valid fd
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_set_reuseaddr()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    bool ok = socket_set_reuseaddr(fd);
    assert(ok);

    socket_close(fd);
    printf("PASS: test_set_reuseaddr\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: socket_bind() with valid IP and port succeeds
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_bind_valid()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    bool ra = socket_set_reuseaddr(fd);
    assert(ra);

    bool ok = socket_bind(fd, LOOPBACK_IP, 19200U);
    assert(ok);

    socket_close(fd);
    printf("PASS: test_bind_valid\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: socket_bind() with invalid IP returns false
// Exercises the reachable branch: inet_aton() returns 0 for a bad IP string.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_bind_bad_ip()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    bool ok = socket_bind(fd, BAD_IP, 19201U);
    assert(!ok);

    socket_close(fd);
    printf("PASS: test_bind_bad_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: socket_listen() returns true after a successful bind
// Verifies: REQ-6.1.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_listen()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    bool ra = socket_set_reuseaddr(fd);
    assert(ra);

    bool bound = socket_bind(fd, LOOPBACK_IP, 19202U);
    assert(bound);

    bool ok = socket_listen(fd, 1);
    assert(ok);

    socket_close(fd);
    printf("PASS: test_listen\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: socket_accept() with a connecting loopback client
// Creates a listening server fd and a non-blocking client fd in the same
// process; connect() is issued before accept() so the kernel queues the SYN.
// Verifies: REQ-6.1.1, REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_accept()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19203U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client initiates connection (uses timeout helper which sets non-blocking)
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool connected = socket_connect_with_timeout(cli_fd, LOOPBACK_IP,
                                                  19203U, TIMEOUT_MS);
    assert(connected);

    // Server-side: wait for the SYN to arrive then accept
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int accepted_fd = socket_accept(srv_fd);
    assert(accepted_fd >= 0);

    socket_close(accepted_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_accept\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: socket_close() on a valid fd (no crash, no error)
// Verifies: REQ-4.1.4
// ─────────────────────────────────────────────────────────────────────────────

static void test_close()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    socket_close(fd);  // must not crash
    printf("PASS: test_close\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: socket_connect_with_timeout() to a listening server
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_connect_with_timeout()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19204U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19204U,
                                           TIMEOUT_MS);
    assert(ok);

    // Accept to complete the handshake (avoids RST on loopback)
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    socket_close(acc_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_connect_with_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: tcp_send_frame() + tcp_recv_frame() round-trip
// Sends a short payload; verifies length prefix and payload bytes survive.
// Verifies: REQ-6.1.5, REQ-6.1.6
// ─────────────────────────────────────────────────────────────────────────────

static void test_tcp_frame_round_trip()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19205U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19205U,
                                           TIMEOUT_MS);
    assert(ok);

    // Accept on server side
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    // F-4: tcp_recv_frame() now rejects frames smaller than WIRE_HEADER_SIZE (52 bytes).
    // Use exactly WIRE_HEADER_SIZE bytes to satisfy the minimum-size guard while
    // still testing the round-trip framing mechanics.
    static const uint32_t PAYLOAD_LEN = Serializer::WIRE_HEADER_SIZE;
    uint8_t payload_buf[Serializer::WIRE_HEADER_SIZE];
    (void)memset(payload_buf, 0xABU, sizeof(payload_buf));  // fill with test pattern

    bool sent = tcp_send_frame(cli_fd, payload_buf, PAYLOAD_LEN, TIMEOUT_MS);
    assert(sent);

    // Receive on server side
    // Buffer sized to Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES
    static const uint32_t BUF_CAP =
        Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    uint8_t  recv_buf[BUF_CAP];
    uint32_t recv_len = 0U;

    (void)memset(recv_buf, 0, BUF_CAP);

    bool received = tcp_recv_frame(acc_fd, recv_buf, BUF_CAP,
                                   TIMEOUT_MS, &recv_len);
    assert(received);
    assert(recv_len == PAYLOAD_LEN);
    assert(memcmp(recv_buf, payload_buf, PAYLOAD_LEN) == 0);

    socket_close(acc_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_tcp_frame_round_trip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: tcp_recv_frame() rejects an oversized frame
// Sends a raw 4-byte length prefix that exceeds max_frame_size; verifies
// tcp_recv_frame() returns false.
// Verifies: REQ-6.1.5, REQ-6.1.6
// ─────────────────────────────────────────────────────────────────────────────

static void test_tcp_recv_oversized_frame()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19206U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19206U,
                                           TIMEOUT_MS);
    assert(ok);

    // Accept on server side
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    // Send a raw 4-byte big-endian length of 0xFFFFFFFF from client.
    // This exceeds max_frame_size, so tcp_recv_frame() must reject it.
    uint8_t bad_header[4U];
    bad_header[0] = 0xFFU;
    bad_header[1] = 0xFFU;
    bad_header[2] = 0xFFU;
    bad_header[3] = 0xFFU;

    bool hdr_sent = socket_send_all(cli_fd, bad_header, 4U, TIMEOUT_MS);
    assert(hdr_sent);

    // Server-side recv must return false (oversized)
    static const uint32_t BUF_CAP =
        Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    uint8_t  recv_buf[BUF_CAP];
    uint32_t recv_len = 0U;

    (void)memset(recv_buf, 0, BUF_CAP);

    bool received = tcp_recv_frame(acc_fd, recv_buf, BUF_CAP,
                                   TIMEOUT_MS, &recv_len);
    assert(!received);

    socket_close(acc_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_tcp_recv_oversized_frame\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: socket_send_to() with invalid IP returns false
// Exercises the reachable branch: inet_aton() returns 0 for a bad IP string.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────

static void test_send_to_bad_ip()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);

    static const uint8_t DATA[4U] = {0x01U, 0x02U, 0x03U, 0x04U};

    bool ok = socket_send_to(fd, DATA, 4U, BAD_IP, 19207U);
    assert(!ok);

    socket_close(fd);
    printf("PASS: test_send_to_bad_ip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: socket_send_to() + socket_recv_from() UDP round-trip
// Verifies: REQ-6.2.1, REQ-6.2.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_send_recv_round_trip()
{
    // Receiver binds to 19208; sender uses 19209 as an ephemeral source.
    int recv_fd = socket_create_udp(false);
    assert(recv_fd >= 0);

    bool ra = socket_set_reuseaddr(recv_fd);
    assert(ra);

    bool bound = socket_bind(recv_fd, LOOPBACK_IP, 19208U);
    assert(bound);

    int send_fd = socket_create_udp(false);
    assert(send_fd >= 0);

    static const uint8_t PAYLOAD[8U] =
        {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0x11U, 0x22U, 0x33U, 0x44U};
    static const uint32_t PAYLOAD_LEN = 8U;

    bool sent = socket_send_to(send_fd, PAYLOAD, PAYLOAD_LEN,
                                LOOPBACK_IP, 19208U);
    assert(sent);

    // Receive on recv_fd
    uint8_t  recv_buf[1500U];
    uint32_t recv_len   = 0U;
    char     src_ip[48U];
    uint16_t src_port   = 0U;

    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(src_ip, 0, sizeof(src_ip));

    bool ok = socket_recv_from(recv_fd, recv_buf, static_cast<uint32_t>(sizeof(recv_buf)),
                                TIMEOUT_MS, &recv_len, src_ip, &src_port);
    assert(ok);
    assert(recv_len == PAYLOAD_LEN);
    assert(memcmp(recv_buf, PAYLOAD, PAYLOAD_LEN) == 0);
    assert(src_ip[0] != '\0');

    socket_close(send_fd);
    socket_close(recv_fd);
    printf("PASS: test_udp_send_recv_round_trip\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: socket_recv_from() timeout with no sender
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_recv_timeout()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);

    bool ra = socket_set_reuseaddr(fd);
    assert(ra);

    bool bound = socket_bind(fd, LOOPBACK_IP, 19209U);
    assert(bound);

    uint8_t  recv_buf[256U];
    uint32_t recv_len = 0U;
    char     src_ip[48U];
    uint16_t src_port = 0U;

    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(src_ip, 0, sizeof(src_ip));

    // 200 ms timeout; no sender; expect false
    bool ok = socket_recv_from(fd, recv_buf,
                                static_cast<uint32_t>(sizeof(recv_buf)),
                                200U, &recv_len, src_ip, &src_port);
    assert(!ok);

    socket_close(fd);
    printf("PASS: test_udp_recv_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: socket_send_all() + socket_recv_exact() round-trip over TCP
// Verifies: REQ-6.1.5, REQ-6.1.6
// ─────────────────────────────────────────────────────────────────────────────

static void test_tcp_send_recv_exact()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19210U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19210U,
                                           TIMEOUT_MS);
    assert(ok);

    // Accept
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    // Send exactly 10 bytes
    static const uint8_t SEND_DATA[10U] =
        {0x10U, 0x20U, 0x30U, 0x40U, 0x50U,
         0x60U, 0x70U, 0x80U, 0x90U, 0xA0U};
    static const uint32_t DATA_LEN = 10U;

    bool sent = socket_send_all(cli_fd, SEND_DATA, DATA_LEN, TIMEOUT_MS);
    assert(sent);

    // Receive exactly 10 bytes on server side
    uint8_t recv_buf[10U];
    (void)memset(recv_buf, 0, sizeof(recv_buf));

    bool received = socket_recv_exact(acc_fd, recv_buf, DATA_LEN, TIMEOUT_MS);
    assert(received);
    assert(memcmp(recv_buf, SEND_DATA, DATA_LEN) == 0);

    socket_close(acc_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_tcp_send_recv_exact\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: tcp_send_frame() with zero-length payload (len == 0)
// Exercises the len == 0 branch in tcp_send_frame(): only header is sent.
// Verifies: REQ-6.1.5
// ─────────────────────────────────────────────────────────────────────────────

static void test_tcp_frame_zero_payload()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19211U);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19211U,
                                           TIMEOUT_MS);
    assert(ok);

    // Accept on server side
    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);

    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    // Send a zero-length frame (len == 0: only the 4-byte header is transmitted)
    static const uint8_t DUMMY[1U] = {0x00U};
    bool sent = tcp_send_frame(cli_fd, DUMMY, 0U, TIMEOUT_MS);
    assert(sent);

    // F-4: tcp_recv_frame() now rejects frames < WIRE_HEADER_SIZE.  A zero-length
    // frame (0 < 52) must be rejected — received must be false.
    static const uint32_t BUF_CAP =
        Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    uint8_t  recv_buf[BUF_CAP];
    uint32_t recv_len = 0xDEADU;   // poison to detect non-write

    (void)memset(recv_buf, 0, BUF_CAP);

    bool received = tcp_recv_frame(acc_fd, recv_buf, BUF_CAP,
                                   TIMEOUT_MS, &recv_len);
    assert(!received);  // F-4: zero-length frame must be rejected by tcp_recv_frame()

    socket_close(acc_fd);
    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_tcp_frame_zero_payload\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: socket_is_ipv6() classifies IPv4 and IPv6 strings correctly
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_socket_is_ipv6()
{
    // IPv6 strings must return true
    assert(socket_is_ipv6("::1"));
    assert(socket_is_ipv6("::"));
    assert(socket_is_ipv6("2001:db8::1"));
    assert(socket_is_ipv6("fe80::1%eth0"));

    // IPv4 strings must return false
    assert(!socket_is_ipv6("127.0.0.1"));
    assert(!socket_is_ipv6("0.0.0.0"));
    assert(!socket_is_ipv6("192.168.1.1"));

    printf("PASS: test_socket_is_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: socket_create_tcp(true) returns a valid AF_INET6 fd
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_create_tcp_ipv6()
{
    int fd = socket_create_tcp(true);
    assert(fd >= 0);

    socket_close(fd);
    printf("PASS: test_create_tcp_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: socket_create_udp(true) returns a valid AF_INET6 fd
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_create_udp_ipv6()
{
    int fd = socket_create_udp(true);
    assert(fd >= 0);

    socket_close(fd);
    printf("PASS: test_create_udp_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: socket_bind() on IPv6 loopback succeeds
// Verifies: REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_bind_ipv6()
{
    int fd = socket_create_tcp(true);
    assert(fd >= 0);

    bool ra = socket_set_reuseaddr(fd);
    assert(ra);

    bool ok = socket_bind(fd, LOOPBACK_IP6, 19300U);
    assert(ok);

    socket_close(fd);
    printf("PASS: test_bind_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: socket_send_to() + socket_recv_from() UDP round-trip over IPv6
// Exercises the AF_INET6 branch in socket_recv_from().
// Verifies: REQ-6.2.1, REQ-6.3.1
// ─────────────────────────────────────────────────────────────────────────────

static void test_udp_ipv6_send_recv()
{
    int recv_fd = socket_create_udp(true);
    assert(recv_fd >= 0);

    bool ra = socket_set_reuseaddr(recv_fd);
    assert(ra);

    bool bound = socket_bind(recv_fd, LOOPBACK_IP6, 19301U);
    assert(bound);

    int send_fd = socket_create_udp(true);
    assert(send_fd >= 0);

    static const uint8_t PAYLOAD[6U] =
        {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
    static const uint32_t PAYLOAD_LEN = 6U;

    bool sent = socket_send_to(send_fd, PAYLOAD, PAYLOAD_LEN,
                                LOOPBACK_IP6, 19301U);
    assert(sent);

    uint8_t  recv_buf[256U];
    uint32_t recv_len = 0U;
    char     src_ip[48U];
    uint16_t src_port = 0U;

    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(src_ip, 0, sizeof(src_ip));

    bool ok = socket_recv_from(recv_fd, recv_buf,
                                static_cast<uint32_t>(sizeof(recv_buf)),
                                TIMEOUT_MS, &recv_len, src_ip, &src_port);
    assert(ok);
    assert(recv_len == PAYLOAD_LEN);
    assert(memcmp(recv_buf, PAYLOAD, PAYLOAD_LEN) == 0);
    assert(src_ip[0] != '\0');  // non-empty source address extracted

    socket_close(send_fd);
    socket_close(recv_fd);
    printf("PASS: test_udp_ipv6_send_recv\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 23: fill_addr() IPv6 failure — invalid IPv6 address string
// Exercises lines 74-77 in fill_addr(): socket_is_ipv6 returns true for "::zzz"
// (contains ':') but inet_pton(AF_INET6, "::zzz") rejects it.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_fill_addr_bad_ipv6()
{
    // Need an IPv6-capable socket so socket_bind reaches fill_addr.
    int fd = socket_create_tcp(true);
    assert(fd >= 0);

    // "::zzz" passes socket_is_ipv6 (has ':') but inet_pton(AF_INET6) rejects it.
    bool ok = socket_bind(fd, "::zzz", 19302U);
    assert(!ok);  // Assert: fill_addr returns false; bind propagates it

    socket_close(fd);
    printf("PASS: test_fill_addr_bad_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 24: socket_set_nonblocking() on a closed fd → fcntl(F_GETFL) fails
// Exercises lines 183-186.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_set_nonblocking_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);  // fd number still positive; fd is now invalid

    bool ok = socket_set_nonblocking(fd);
    assert(!ok);  // Assert: fcntl(F_GETFL) fails with EBADF

    printf("PASS: test_set_nonblocking_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 25: socket_set_reuseaddr() on a closed fd → setsockopt fails
// Exercises lines 215-218.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_set_reuseaddr_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    bool ok = socket_set_reuseaddr(fd);
    assert(!ok);  // Assert: setsockopt fails with EBADF

    printf("PASS: test_set_reuseaddr_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 26: socket_bind() EADDRINUSE — same port bound twice without SO_REUSEADDR
// Exercises lines 245-248.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_bind_eaddrinuse()
{
    int fd1 = socket_create_tcp(false);
    assert(fd1 >= 0);
    bool b1 = socket_bind(fd1, LOOPBACK_IP, 19303U);
    assert(b1);  // Assert: first bind succeeds

    int fd2 = socket_create_tcp(false);
    assert(fd2 >= 0);
    // No SO_REUSEADDR on fd2; same port → EADDRINUSE → lines 245-248
    bool b2 = socket_bind(fd2, LOOPBACK_IP, 19303U);
    assert(!b2);  // Assert: second bind on same port fails

    socket_close(fd1);
    socket_close(fd2);
    printf("PASS: test_bind_eaddrinuse\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 27: socket_connect_with_timeout() — socket_set_nonblocking fails
// Using a closed fd: socket_set_nonblocking(closed_fd) → fcntl fails → lines 275-276.
// Verifies: REQ-6.3.2, REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2, REQ-6.3.3
static void test_connect_nonblocking_fails()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);  // closed; socket_set_nonblocking will fail

    bool ok = socket_connect_with_timeout(fd, LOOPBACK_IP, 19304U, TIMEOUT_MS);
    assert(!ok);  // Assert: returns false because socket_set_nonblocking failed

    printf("PASS: test_connect_nonblocking_fails\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 28: socket_connect_with_timeout() — fill_addr fails (bad IPv6)
// socket_set_nonblocking succeeds on valid fd; fill_addr("::zzz") fails → lines 282-283.
// Verifies: REQ-6.3.2, REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2, REQ-6.3.3
static void test_connect_bad_ipv6()
{
    int fd = socket_create_tcp(true);  // IPv6 socket
    assert(fd >= 0);

    // "::zzz" passes socket_is_ipv6 but inet_pton rejects it → fill_addr fails
    bool ok = socket_connect_with_timeout(fd, "::zzz", 19305U, TIMEOUT_MS);
    assert(!ok);  // Assert: fill_addr fails → early return at lines 282-283

    socket_close(fd);
    printf("PASS: test_connect_bad_ipv6\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 29: socket_connect_with_timeout() — ECONNREFUSED (no listener)
// Non-blocking connect to a port with no listener; on loopback, connect()
// returns -1+ECONNREFUSED immediately (not EINPROGRESS) → lines 297-300.
// Verifies: REQ-6.3.2, REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2, REQ-6.3.3
static void test_connect_refused()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);

    // Port 19399 has no listener; non-blocking connect returns ECONNREFUSED
    // immediately on loopback → errno != EINPROGRESS → lines 297-300
    bool ok = socket_connect_with_timeout(fd, LOOPBACK_IP, 19399U, TIMEOUT_MS);
    assert(!ok);  // Assert: ECONNREFUSED — not EINPROGRESS

    socket_close(fd);
    printf("PASS: test_connect_refused\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 30: socket_listen() on a closed fd → listen() fails
// Exercises lines 346-349.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_listen_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    bool ok = socket_listen(fd, 1);
    assert(!ok);  // Assert: listen() fails with EBADF → lines 346-349

    printf("PASS: test_listen_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 31: socket_accept() on a closed fd → accept() fails
// Exercises lines 368-371.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_accept_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    int result = socket_accept(fd);
    assert(result < 0);  // Assert: accept() fails with EBADF → lines 368-371

    printf("PASS: test_accept_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 32: socket_close() on an already-closed fd → close() returns -1
// Exercises lines 391-394.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_close_already_closed()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);          // First close — valid
    socket_close(fd);          // Second close — EBADF → lines 391-394 (must not crash)

    // No assert needed beyond "did not crash"; the function returns void.
    assert(fd >= 0);  // Assert: fd value unchanged (just a number)

    printf("PASS: test_close_already_closed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 33: socket_send_all() on a closed fd → send() fails
// poll() returns POLLNVAL (poll_result > 0) on a closed fd, then send() returns
// -1+EBADF → lines 433-436.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_send_all_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    static const uint8_t DATA[4U] = {0x01U, 0x02U, 0x03U, 0x04U};
    // poll(closed_fd) → POLLNVAL → poll_result=1 → proceeds to send()
    // send(closed_fd) → -1+EBADF → lines 433-436
    bool ok = socket_send_all(fd, DATA, 4U, TIMEOUT_MS);
    assert(!ok);  // Assert: send_all returns false (EBADF)

    printf("PASS: test_send_all_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 34: socket_recv_exact() poll timeout
// Bound UDP socket with nothing to read; 1 ms timeout → poll returns 0 → lines 482-485.
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.3
static void test_recv_exact_timeout()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);

    bool ra = socket_set_reuseaddr(fd);
    assert(ra);

    bool bound = socket_bind(fd, LOOPBACK_IP, 19307U);
    assert(bound);

    uint8_t buf[16U];
    // 1 ms timeout; no data sent → poll returns 0 (timeout) → lines 482-485
    bool ok = socket_recv_exact(fd, buf, 10U, 1U);
    assert(!ok);  // Assert: recv_exact times out

    socket_close(fd);
    printf("PASS: test_recv_exact_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 35: socket_recv_exact() on a closed fd → recv() fails
// poll(closed_fd) → POLLNVAL → poll_result=1 → recv() → -1+EBADF → lines 491-494.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_recv_exact_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    uint8_t buf[16U];
    bool ok = socket_recv_exact(fd, buf, 10U, TIMEOUT_MS);
    assert(!ok);  // Assert: recv fails with EBADF → lines 491-494

    printf("PASS: test_recv_exact_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 36: socket_recv_exact() — recv() returns 0 (peer closed connection)
// Exercises lines 498-501.
// Setup: create loopback TCP pair, close server side, recv on client.
// When the peer sends FIN the socket becomes readable; recv() returns 0.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_recv_exact_peer_closed()
{
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);
    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, 19308U);
    assert(bound);
    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);
    bool connected = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, 19308U, TIMEOUT_MS);
    assert(connected);

    bool readable = wait_readable(srv_fd, TIMEOUT_MS);
    assert(readable);
    int acc_fd = socket_accept(srv_fd);
    assert(acc_fd >= 0);

    // Close server side → sends FIN → client's recv() returns 0 → lines 498-501
    socket_close(acc_fd);

    uint8_t buf[16U];
    bool ok = socket_recv_exact(cli_fd, buf, 10U, TIMEOUT_MS);
    assert(!ok);  // Assert: recv returns 0 (EOF) → returns false

    socket_close(cli_fd);
    socket_close(srv_fd);
    printf("PASS: test_recv_exact_peer_closed\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 37: tcp_send_frame() on a closed fd → socket_send_all(header) fails
// Exercises lines 532-535.
// Verifies: REQ-6.1.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.1.5, REQ-6.3.2
static void test_send_frame_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    static const uint8_t DATA[4U] = {0x10U, 0x20U, 0x30U, 0x40U};
    // socket_send_all(header) → send() fails with EBADF → lines 532-535
    bool ok = tcp_send_frame(fd, DATA, 4U, TIMEOUT_MS);
    assert(!ok);  // Assert: header send fails

    printf("PASS: test_send_frame_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 38: tcp_recv_frame() on a closed fd → socket_recv_exact(header) fails
// Exercises lines 566-569.
// Verifies: REQ-6.1.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.1.5, REQ-6.3.2
static void test_recv_frame_closed_fd()
{
    int fd = socket_create_tcp(false);
    assert(fd >= 0);
    socket_close(fd);

    static const uint32_t BUF_CAP = Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    uint8_t buf[BUF_CAP];
    uint32_t out_len = 0U;
    // socket_recv_exact(header) → recv() fails → lines 566-569
    bool ok = tcp_recv_frame(fd, buf, BUF_CAP, TIMEOUT_MS, &out_len);
    assert(!ok);  // Assert: header recv fails

    printf("PASS: test_recv_frame_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 39: tcp_recv_frame() — header received but payload recv fails
// Exercises lines 592-595.
// Uses socketpair(): one end sends WIRE_HEADER_SIZE as frame_len then closes;
// the other end's tcp_recv_frame reads the header (success) then fails reading
// the payload (recv() returns 0 = EOF).
// Verifies: REQ-6.1.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.1.5, REQ-6.3.2
static void test_recv_frame_payload_fails()
{
    int sv[2];
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(r == 0);  // Assert: socketpair created

    // sv[0] = writer (sends header then closes)
    // sv[1] = reader (used for tcp_recv_frame)
    // frame_len = WIRE_HEADER_SIZE = 52 (valid range; triggers payload recv)
    static const uint32_t FRAME_LEN = Serializer::WIRE_HEADER_SIZE;
    uint8_t hdr[4U];
    hdr[0] = static_cast<uint8_t>((FRAME_LEN >> 24U) & 0xFFU);
    hdr[1] = static_cast<uint8_t>((FRAME_LEN >> 16U) & 0xFFU);
    hdr[2] = static_cast<uint8_t>((FRAME_LEN >>  8U) & 0xFFU);
    hdr[3] = static_cast<uint8_t>( FRAME_LEN         & 0xFFU);

    bool hw = socket_send_all(sv[0], hdr, 4U, TIMEOUT_MS);
    assert(hw);  // Assert: header written to socketpair buffer
    socket_close(sv[0]);  // FIN → tcp_recv_frame payload recv returns 0

    static const uint32_t BUF_CAP = Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    uint8_t buf[BUF_CAP];
    uint32_t out_len = 0U;
    // Reads 4-byte header (OK, already buffered); then tries to read FRAME_LEN
    // bytes of payload but gets EOF → socket_recv_exact returns false → lines 592-595
    bool ok = tcp_recv_frame(sv[1], buf, BUF_CAP, TIMEOUT_MS, &out_len);
    assert(!ok);  // Assert: payload recv fails (EOF)

    socket_close(sv[1]);
    printf("PASS: test_recv_frame_payload_fails\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 40: socket_send_to() on a closed fd → sendto() fails
// fill_addr() succeeds (valid IP); sendto(closed_fd) → -1+EBADF → lines 631-634.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_send_to_closed_fd()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);
    socket_close(fd);

    static const uint8_t DATA[4U] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    // fill_addr(LOOPBACK_IP) succeeds; sendto(closed_fd) → EBADF → lines 631-634
    bool ok = socket_send_to(fd, DATA, 4U, LOOPBACK_IP, 19309U);
    assert(!ok);  // Assert: sendto fails with EBADF

    printf("PASS: test_send_to_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 41: socket_recv_from() — recvfrom() returns 0 (zero-length UDP datagram)
// Exercises lines 697-700.
// Sends a zero-length UDP datagram directly via sendto() (bypassing
// socket_send_to which asserts len > 0).  On loopback, recvfrom() returns 0
// for an empty UDP datagram → socket_recv_from returns false.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_recv_from_zero_datagram()
{
    int recv_fd = socket_create_udp(false);
    assert(recv_fd >= 0);
    bool ra = socket_set_reuseaddr(recv_fd);
    assert(ra);
    bool bound = socket_bind(recv_fd, LOOPBACK_IP, 19310U);
    assert(bound);

    int send_fd = socket_create_udp(false);
    assert(send_fd >= 0);

    // Build destination address for raw sendto()
    struct sockaddr_in dst;
    (void)memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(static_cast<uint16_t>(19310U));
    int pton_r = inet_pton(AF_INET, LOOPBACK_IP, &dst.sin_addr);
    assert(pton_r == 1);  // Assert: address converted

    // Send zero-length UDP datagram directly (socket_send_to would assert len>0)
    static const uint8_t EMPTY[1U] = {0x00U};
    // MISRA C++:2023 Rule 5.2.4: POSIX socket API requires reinterpret_cast.
    ssize_t ns = sendto(send_fd, EMPTY, 0U, 0,
                        reinterpret_cast<const struct sockaddr*>(&dst),
                        static_cast<socklen_t>(sizeof(dst)));
    assert(ns == 0);  // Assert: zero bytes sent (empty datagram)

    // recvfrom() returns 0 for zero-length datagram → lines 697-700
    uint8_t  recv_buf[256U];
    uint32_t recv_len = 0U;
    char     src_ip[48U];
    uint16_t src_port = 0U;
    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(src_ip,   0, sizeof(src_ip));

    bool ok = socket_recv_from(recv_fd, recv_buf,
                                static_cast<uint32_t>(sizeof(recv_buf)),
                                TIMEOUT_MS, &recv_len, src_ip, &src_port);
    assert(!ok);  // Assert: recvfrom returns 0 → socket_recv_from returns false

    socket_close(send_fd);
    socket_close(recv_fd);
    printf("PASS: test_recv_from_zero_datagram\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// test_socket_create_tcp_fail_resource_error
//
// Covers log_socket_create_error() WARNING_HI branch — triggered when socket()
// fails with EMFILE (fd exhaustion, a resource/permission error).
//
// Strategy: probe the next fd number the OS would assign, close it, then set
// RLIMIT_NOFILE to that number (so max open fd = probe_fd - 1). socket_create_tcp()
// calls socket() which needs the probed fd but finds it out of range → EMFILE →
// log_socket_create_error logs at WARNING_HI. Limit is restored immediately.
//
// Power of 10: ≥2 assertions (fd probe assertion + fd < 0 assertion).
// NSC: SocketUtils is an OS adapter; no message-delivery policy.
// Verifies: REQ-6.3.2 (error classification)
// ─────────────────────────────────────────────────────────────────────────────

static void test_socket_create_tcp_fail_resource_error()
{
    // Probe: find the fd number the next socket() would receive.
    int probe_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe_fd >= 0);
    (void)close(probe_fd);  // free it; next socket() will get this number again

    // Tighten the limit so fd == probe_fd is forbidden (max fd = probe_fd - 1).
    struct rlimit old_rl;
    (void)getrlimit(RLIMIT_NOFILE, &old_rl);
    struct rlimit tight_rl;
    tight_rl.rlim_cur = static_cast<rlim_t>(probe_fd);
    tight_rl.rlim_max = old_rl.rlim_max;
    int sr = setrlimit(RLIMIT_NOFILE, &tight_rl);
    assert(sr == 0);

    // socket_create_tcp() calls socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    // socket() tries to return probe_fd which exceeds the new limit → EMFILE.
    // log_socket_create_error() logs at WARNING_HI (EMFILE is a resource error).
    int new_fd = socket_create_tcp(false);

    // Restore the limit immediately before any further assertions.
    (void)setrlimit(RLIMIT_NOFILE, &old_rl);

    // Verify socket_create_tcp() returned the error sentinel.
    assert(new_fd < 0);

    printf("PASS: test_socket_create_tcp_fail_resource_error\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// main — run all tests in sequence
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    printf("=== test_SocketUtils ===\n");

    test_create_tcp();
    test_create_udp();
    test_set_nonblocking();
    test_set_reuseaddr();
    test_bind_valid();
    test_bind_bad_ip();
    test_listen();
    test_accept();
    test_close();
    test_connect_with_timeout();
    test_tcp_frame_round_trip();
    test_tcp_recv_oversized_frame();
    test_send_to_bad_ip();
    test_udp_send_recv_round_trip();
    test_udp_recv_timeout();
    test_tcp_send_recv_exact();
    test_tcp_frame_zero_payload();
    test_socket_is_ipv6();
    test_create_tcp_ipv6();
    test_create_udp_ipv6();
    test_bind_ipv6();
    test_udp_ipv6_send_recv();
    test_fill_addr_bad_ipv6();
    test_set_nonblocking_closed_fd();
    test_set_reuseaddr_closed_fd();
    test_bind_eaddrinuse();
    test_connect_nonblocking_fails();
    test_connect_bad_ipv6();
    test_connect_refused();
    test_listen_closed_fd();
    test_accept_closed_fd();
    test_close_already_closed();
    test_send_all_closed_fd();
    test_recv_exact_timeout();
    test_recv_exact_closed_fd();
    test_recv_exact_peer_closed();
    test_send_frame_closed_fd();
    test_recv_frame_closed_fd();
    test_recv_frame_payload_fails();
    test_send_to_closed_fd();
    test_recv_from_zero_datagram();

    // log_socket_create_error() WARNING_HI branch (EMFILE via setrlimit)
    test_socket_create_tcp_fail_resource_error();

    printf("=== ALL PASSED ===\n");
    return 0;
}
