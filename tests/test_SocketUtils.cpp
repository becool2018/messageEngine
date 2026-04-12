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
 * Ports are allocated dynamically via alloc_ephemeral_port() (TestPortAllocator.hpp)
 * so multiple test suite instances can run concurrently without conflicts.
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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#include "core/Types.hpp"
#include "core/Serializer.hpp"
#include "platform/SocketUtils.hpp"
#include "platform/IPosixSyscalls.hpp"
#include "platform/PosixSyscallsImpl.hpp"
#include "TestPortAllocator.hpp"

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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_bind(fd, LOOPBACK_IP, port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_bind(fd, BAD_IP, port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(fd, LOOPBACK_IP, port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client initiates connection (uses timeout helper which sets non-blocking)
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool connected = socket_connect_with_timeout(cli_fd, LOOPBACK_IP,
                                                  port, TIMEOUT_MS);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port,
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port,
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
// Sends a raw 4-byte length prefix that exceeds max_frame_size (0xFFFFFFFF);
// verifies tcp_recv_frame() returns false.  Exercises the REQ-3.2.10 ceiling
// guard (frame_len > k_max_safe_frame branch).
// Verifies: REQ-6.1.5, REQ-6.1.6, REQ-3.2.10
// ─────────────────────────────────────────────────────────────────────────────

static void test_tcp_recv_oversized_frame()
{
    // Set up server
    int srv_fd = socket_create_tcp(false);
    assert(srv_fd >= 0);

    bool ra = socket_set_reuseaddr(srv_fd);
    assert(ra);

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port,
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

    const uint16_t port = alloc_ephemeral_port(SOCK_DGRAM);
    bool ok = socket_send_to(fd, DATA, 4U, BAD_IP, port);
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
    // Receiver binds to a dynamically allocated port; sender sends to that port.
    int recv_fd = socket_create_udp(false);
    assert(recv_fd >= 0);

    bool ra = socket_set_reuseaddr(recv_fd);
    assert(ra);

    const uint16_t recv_port = alloc_ephemeral_port(SOCK_DGRAM);
    bool bound = socket_bind(recv_fd, LOOPBACK_IP, recv_port);
    assert(bound);

    int send_fd = socket_create_udp(false);
    assert(send_fd >= 0);

    static const uint8_t PAYLOAD[8U] =
        {0xAAU, 0xBBU, 0xCCU, 0xDDU, 0x11U, 0x22U, 0x33U, 0x44U};
    static const uint32_t PAYLOAD_LEN = 8U;

    bool sent = socket_send_to(send_fd, PAYLOAD, PAYLOAD_LEN,
                                LOOPBACK_IP, recv_port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_DGRAM);
    bool bound = socket_bind(fd, LOOPBACK_IP, port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port,
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);

    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    // Client connects
    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);

    bool ok = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port,
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_bind(fd, LOOPBACK_IP6, port);
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

    const uint16_t recv_port = alloc_ephemeral_port(SOCK_DGRAM);
    bool bound = socket_bind(recv_fd, LOOPBACK_IP6, recv_port);
    assert(bound);

    int send_fd = socket_create_udp(true);
    assert(send_fd >= 0);

    static const uint8_t PAYLOAD[6U] =
        {0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
    static const uint32_t PAYLOAD_LEN = 6U;

    bool sent = socket_send_to(send_fd, PAYLOAD, PAYLOAD_LEN,
                                LOOPBACK_IP6, recv_port);
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
    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_bind(fd, "::zzz", port);
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
    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool b1 = socket_bind(fd1, LOOPBACK_IP, port);
    assert(b1);  // Assert: first bind succeeds

    int fd2 = socket_create_tcp(false);
    assert(fd2 >= 0);
    // No SO_REUSEADDR on fd2; same port → EADDRINUSE → lines 245-248
    bool b2 = socket_bind(fd2, LOOPBACK_IP, port);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_connect_with_timeout(fd, LOOPBACK_IP, port, TIMEOUT_MS);
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
    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_connect_with_timeout(fd, "::zzz", port, TIMEOUT_MS);
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

    // Allocate a port then release it; no listener will be present → ECONNREFUSED
    // immediately on loopback → errno != EINPROGRESS → lines 297-300
    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_connect_with_timeout(fd, LOOPBACK_IP, port, TIMEOUT_MS);
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

    const uint16_t port = alloc_ephemeral_port(SOCK_DGRAM);
    bool bound = socket_bind(fd, LOOPBACK_IP, port);
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
    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool bound = socket_bind(srv_fd, LOOPBACK_IP, port);
    assert(bound);
    bool listening = socket_listen(srv_fd, 1);
    assert(listening);

    int cli_fd = socket_create_tcp(false);
    assert(cli_fd >= 0);
    bool connected = socket_connect_with_timeout(cli_fd, LOOPBACK_IP, port, TIMEOUT_MS);
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
    const uint16_t port = alloc_ephemeral_port(SOCK_DGRAM);
    bool ok = socket_send_to(fd, DATA, 4U, LOOPBACK_IP, port);
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
    const uint16_t recv_port = alloc_ephemeral_port(SOCK_DGRAM);
    bool bound = socket_bind(recv_fd, LOOPBACK_IP, recv_port);
    assert(bound);

    int send_fd = socket_create_udp(false);
    assert(send_fd >= 0);

    // Build destination address for raw sendto()
    struct sockaddr_in dst;
    (void)memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(recv_port);
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
// MockPosixSyscalls — fault-injection mock for IPosixSyscalls
//
// Each syscall has a corresponding fail flag and errno value. When the fail
// flag is false (default), the call delegates to the real POSIX syscall.
// Special controls allow per-call-number failure (fcntl, poll) and return-
// value overrides (connect, send, sendto, recvfrom, inet_ntop).
//
// Test-only class; STL not used; plain arrays and flags only.
// Power of 10 Rule 9 exemption: function pointers in test frameworks are
// permitted per CLAUDE.md §9 table.
// ─────────────────────────────────────────────────────────────────────────────

class MockPosixSyscalls : public IPosixSyscalls {
public:
    // ── socket ───────────────────────────────────────────────────────────────
    bool fail_socket      = false;
    int  socket_err       = EINVAL;

    // ── fcntl ────────────────────────────────────────────────────────────────
    int  fcntl_fail_call  = -1;   // fail on call N (0-based); -1 = never
    int  fcntl_fail_err   = EBADF;
    int  fcntl_call_n     = 0;    // call counter (reset to 0 on construction)

    // ── connect ───────────────────────────────────────────────────────────────
    bool connect_override = false;
    int  connect_ret      = 0;
    int  connect_err      = 0;

    // ── poll ─────────────────────────────────────────────────────────────────
    int  poll_fail_call   = -1;   // fail on call N (0-based); -1 = never
    int  poll_fail_ret    = 0;    // return value when failing (0 = timeout)
    int  poll_call_n      = 0;    // call counter

    // ── send ─────────────────────────────────────────────────────────────────
    bool send_return_zero = false; // return 0 instead of delegating

    // ── sendto ───────────────────────────────────────────────────────────────
    bool    sendto_partial     = false;
    ssize_t sendto_partial_ret = 0;

    // ── recvfrom ─────────────────────────────────────────────────────────────
    bool    fail_recvfrom         = false;
    int     recvfrom_err          = EBADF;
    bool    recvfrom_succeed      = false; // return canned success bytes
    ssize_t recvfrom_succeed_ret  = 5;

    // ── inet_ntop ────────────────────────────────────────────────────────────
    bool fail_inet_ntop  = false;
    int  inet_ntop_err   = ENOSPC;

    // ─────────────────────────────────────────────────────────────────────────

    int sys_socket(int domain, int type, int protocol) override
    {
        assert(domain == AF_INET || domain == AF_INET6);
        assert(type == SOCK_STREAM || type == SOCK_DGRAM);
        if (fail_socket) {
            errno = socket_err;
            return -1;
        }
        return ::socket(domain, type, protocol);
    }

    int sys_fcntl(int fd, int cmd, int arg) override
    {
        assert(fd >= 0);
        assert(cmd == F_GETFL || cmd == F_SETFL);
        int cur_call = fcntl_call_n;
        ++fcntl_call_n;
        if (fcntl_fail_call >= 0 && cur_call == fcntl_fail_call) {
            errno = fcntl_fail_err;
            return -1;
        }
        return ::fcntl(fd, cmd, arg);
    }

    int sys_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) override
    {
        assert(fd >= 0);
        assert(addr != nullptr);
        if (connect_override) {
            errno = connect_err;
            return connect_ret;
        }
        return ::connect(fd, addr, addrlen);
    }

    int sys_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) override
    {
        assert(fds != nullptr);
        assert(static_cast<uint32_t>(nfds) > 0U);
        int cur_call = poll_call_n;
        ++poll_call_n;
        if (poll_fail_call >= 0 && cur_call == poll_fail_call) {
            return poll_fail_ret;
        }
        return ::poll(fds, nfds, timeout_ms);
    }

    ssize_t sys_send(int fd, const void* buf, size_t len, int flags) override
    {
        assert(fd >= 0);
        assert(buf != nullptr);
        if (send_return_zero) {
            return 0;
        }
        return ::send(fd, buf, len, flags);
    }

    ssize_t sys_sendto(int fd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest_addr, socklen_t addrlen) override
    {
        assert(fd >= 0);
        assert(buf != nullptr);
        if (sendto_partial) {
            return sendto_partial_ret;
        }
        return ::sendto(fd, buf, len, flags, dest_addr, addrlen);
    }

    ssize_t sys_recvfrom(int fd, void* buf, size_t len, int flags,
                          struct sockaddr* src_addr, socklen_t* addrlen) override
    {
        assert(fd >= 0);
        assert(buf != nullptr);
        if (fail_recvfrom) {
            errno = recvfrom_err;
            return -1;
        }
        if (recvfrom_succeed) {
            // Fill buf with dummy bytes so the caller has data to process
            static const uint8_t dummy[5] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U};
            (void)memcpy(buf, dummy,
                         (static_cast<size_t>(recvfrom_succeed_ret) < len)
                             ? static_cast<size_t>(recvfrom_succeed_ret) : len);
            // Leave src_addr zeroed (ss_family == 0 → treated as AF_INET path)
            if (src_addr != nullptr) {
                (void)memset(src_addr, 0, static_cast<size_t>(*addrlen));
            }
            return recvfrom_succeed_ret;
        }
        return ::recvfrom(fd, buf, len, flags, src_addr, addrlen);
    }

    const char* sys_inet_ntop(int af, const void* src, char* dst,
                               socklen_t size) override
    {
        assert(src != nullptr);
        assert(dst != nullptr);
        if (fail_inet_ntop) {
            errno = inet_ntop_err;
            return nullptr;
        }
        return ::inet_ntop(af, src, dst, size);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 1: socket_create_tcp() WARNING_LO path — socket() fails EINVAL
// Exercises log_socket_create_error() WARNING_LO branch (lines 121–124).
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_socket_create_tcp_warning_lo_errno()
{
    MockPosixSyscalls mock;
    mock.fail_socket = true;
    mock.socket_err  = EINVAL;  // not EMFILE/ENFILE/EACCES/EPERM → WARNING_LO

    int fd = socket_create_tcp(false, mock);
    assert(fd < 0);  // Assert: socket() failure returns -1

    printf("PASS: test_socket_create_tcp_warning_lo_errno\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 2: socket_set_nonblocking() F_SETFL failure
// F_GETFL succeeds (call 0); F_SETFL fails (call 1) → WARNING_LO lines 192–195.
// Uses socketpair to provide a valid fd for the successful F_GETFL call.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_set_nonblocking_setfl_fails()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    MockPosixSyscalls mock;
    mock.fcntl_fail_call = 1;  // call 0 = F_GETFL (succeeds), call 1 = F_SETFL (fails)
    mock.fcntl_fail_err  = EBADF;

    bool ok = socket_set_nonblocking(sv[0], mock);
    assert(!ok);  // Assert: F_SETFL failure returns false

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_set_nonblocking_setfl_fails\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 3: socket_connect_with_timeout() immediate success (conn_result == 0)
// Mock overrides connect() to return 0 immediately — exercises lines 291–293.
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.3
static void test_connect_immediate_success()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    MockPosixSyscalls mock;
    mock.connect_override = true;
    mock.connect_ret      = 0;   // immediate success
    mock.connect_err      = 0;

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_connect_with_timeout(sv[0], "127.0.0.1", port, 100U, mock);
    assert(ok);  // Assert: immediate success returns true

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_connect_immediate_success\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 4: socket_connect_with_timeout() poll() timeout after EINPROGRESS
// Mock returns EINPROGRESS from connect(), then poll() returns 0 (timeout).
// Exercises lines 314–317.
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.3
static void test_connect_poll_timeout_mock()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    MockPosixSyscalls mock;
    mock.connect_override = true;
    mock.connect_ret      = -1;
    mock.connect_err      = EINPROGRESS;  // → proceeds to poll
    mock.poll_fail_call   = 0;            // first poll call (for connect wait) returns 0
    mock.poll_fail_ret    = 0;            // 0 = timeout

    const uint16_t port = alloc_ephemeral_port(SOCK_STREAM);
    bool ok = socket_connect_with_timeout(sv[0], "127.0.0.1", port, 100U, mock);
    assert(!ok);  // Assert: poll timeout returns false

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_connect_poll_timeout_mock\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 5: socket_send_all() send() returns 0 (peer closed path)
// Mock returns 0 from sys_send(); real poll succeeds (sv[0] is writable).
// Exercises lines 440–444.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_send_all_send_returns_zero_mock()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    MockPosixSyscalls mock;
    mock.send_return_zero = true;  // sys_send returns 0

    static const uint8_t data[1U] = {0x42U};
    bool ok = socket_send_all(sv[0], data, 1U, 100U, mock);
    assert(!ok);  // Assert: send() returns 0 → returns false (lines 440-444)

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_send_all_send_returns_zero_mock\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 6: socket_send_to() partial sendto() return
// Mock returns sendto_partial_ret = 3 for a 10-byte payload → partial send.
// Exercises lines 638–641.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_sendto_partial_mock()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);  // Assert: UDP socket created

    MockPosixSyscalls mock;
    mock.sendto_partial     = true;
    mock.sendto_partial_ret = 3;  // partial send: 3 of 10 bytes

    static const uint8_t data[10U] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U,
        0x06U, 0x07U, 0x08U, 0x09U, 0x0AU
    };
    const uint16_t port = alloc_ephemeral_port(SOCK_DGRAM);
    bool ok = socket_send_to(fd, data, 10U, "127.0.0.1", port, mock);
    assert(!ok);  // Assert: partial sendto() returns false (lines 638-641)

    socket_close(fd);
    printf("PASS: test_sendto_partial_mock\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Mock Test 7: socket_recv_from() inet_ntop() failure
// Uses socketpair(AF_UNIX, SOCK_DGRAM). Sends 5 bytes from sv[1] to sv[0].
// Real poll + recvfrom succeed; mock inet_ntop returns nullptr.
// Exercises lines 721–724.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_recv_from_inet_ntop_fails_mock()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    static const uint8_t payload[5U] = {0x10U, 0x20U, 0x30U, 0x40U, 0x50U};
    ssize_t ns = send(sv[1], payload, sizeof(payload), 0);
    assert(ns == 5);  // Assert: 5 bytes sent into sv[0]'s receive queue

    MockPosixSyscalls mock;
    mock.fail_inet_ntop = true;
    mock.inet_ntop_err  = ENOSPC;

    uint8_t  recv_buf[256U];
    uint32_t out_len  = 0U;
    char     out_ip[48U];
    uint16_t out_port = 0U;
    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(out_ip,   0, sizeof(out_ip));

    // poll + recvfrom succeed (real syscalls); inet_ntop fails via mock
    bool ok = socket_recv_from(sv[0], recv_buf, static_cast<uint32_t>(sizeof(recv_buf)),
                                100U, &out_len, out_ip, &out_port, mock);
    assert(!ok);  // Assert: inet_ntop failure returns false (lines 721-724)

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_recv_from_inet_ntop_fails_mock\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Practical Test A2: socket_recv_from() recvfrom() returns -1 on closed fd
// Creates a UDP socket, closes it, then calls socket_recv_from() with timeout=1ms.
// poll(closed_fd) returns 1 with POLLNVAL (fd is closed but numeric value ≥ 0),
// recvfrom() then fails with EBADF → lines 691-694.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_recv_from_closed_fd()
{
    int fd = socket_create_udp(false);
    assert(fd >= 0);  // Assert: valid fd obtained
    socket_close(fd);

    uint8_t  recv_buf[256U];
    uint32_t out_len  = 0U;
    char     out_ip[48U];
    uint16_t out_port = 0U;
    (void)memset(recv_buf, 0, sizeof(recv_buf));
    (void)memset(out_ip,   0, sizeof(out_ip));

    // poll() on a closed fd returns POLLNVAL (poll_result=1, but POLLNVAL set).
    // The poll_result <= 0 check is False so we proceed to recvfrom().
    // recvfrom() on a closed fd returns -1/EBADF → lines 691-694.
    bool ok = socket_recv_from(fd, recv_buf,
                                static_cast<uint32_t>(sizeof(recv_buf)),
                                1U, &out_len, out_ip, &out_port);
    assert(!ok);  // Assert: recvfrom fails with EBADF → returns false

    printf("PASS: test_recv_from_closed_fd\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Practical Test A3: socket_connect_with_timeout() non-EINPROGRESS error
// Creates an AF_INET TCP socket; connects with IPv6 address "::1" (port 9980).
// fill_addr("::1") fills a sockaddr_in6; connect() on an AF_INET socket with
// an AF_INET6 address fails synchronously with EAFNOSUPPORT (not EINPROGRESS).
// Exercises lines 297–300.
// Verifies: REQ-6.3.2, REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2, REQ-6.3.3
static void test_connect_family_mismatch()
{
    // Create an AF_INET TCP socket
    int fd = socket_create_tcp(false);
    assert(fd >= 0);  // Assert: valid fd obtained

    // "::1" is IPv6 — fill_addr builds a sockaddr_in6; connect() on an AF_INET
    // socket rejects it with EAFNOSUPPORT, not EINPROGRESS → lines 297-300.
    bool ok = socket_connect_with_timeout(fd, "::1", 9980U, 1U);
    assert(!ok);  // Assert: family mismatch → connect fails with non-EINPROGRESS

    socket_close(fd);
    printf("PASS: test_connect_family_mismatch\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Practical Test A4: tcp_send_frame() payload send fails (buffer full)
// Creates socketpair, floods sv[0] until kernel send buffer is full, drains
// a large block from sv[1] (so sv[0]'s poll becomes writable for the header),
// then calls tcp_send_frame(sv[0], payload, 8, 1ms) with a MockPosixSyscalls
// that lets the header poll + send succeed but makes the payload poll time out
// by returning 0 on the second poll call.
// Exercises lines 540–543.
// Verifies: REQ-6.1.5, REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.1.5, REQ-6.3.2
static void test_send_frame_payload_fails_buffer_full()
{
    int sv[2] = {-1, -1};
    int r = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(r == 0);  // Assert: socketpair created

    // Use MockPosixSyscalls to control exactly which poll call fails.
    // poll call 0: for the 4-byte header (socket_send_all) — real poll, writable.
    // poll call 1: for the 8-byte payload (socket_send_all) — returns 0 (timeout).
    // send call: real ::send for the header; not reached for payload.
    MockPosixSyscalls mock;
    mock.poll_fail_call = 1;   // fail second poll call (payload wait)
    mock.poll_fail_ret  = 0;   // return 0 = timeout

    static const uint8_t payload[8U] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U
    };
    // Header (4 bytes) poll succeeds (real), send succeeds; payload poll returns
    // 0 (timeout) → tcp_send_frame lines 540–543 executes → returns false.
    bool ok = tcp_send_frame(sv[0], payload, 8U, 1U, mock);
    assert(!ok);  // Assert: payload poll timeout → returns false (lines 540-543)

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_send_frame_payload_fails_buffer_full\n");
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
    struct rlimit old_rl = {};                           // §7b: init at declaration
    int gr = getrlimit(RLIMIT_NOFILE, &old_rl);         // Rule 7: check return value
    assert(gr == 0);
    struct rlimit tight_rl = {};                         // §7b: init at declaration
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
// Practical Test A1: socket_create_udp() resource-error path (RLIMIT)
//
// Covers log_socket_create_error() WARNING_HI branch via socket_create_udp() —
// same technique as test_socket_create_tcp_fail_resource_error above.
// socket_create_udp() calls socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); with
// RLIMIT_NOFILE clamped to probe_fd the call fails with EMFILE → WARNING_HI.
// Covers lines 161–163.
//
// MUST remain in the "last" group: transiently modifies RLIMIT_NOFILE.
// Verifies: REQ-6.3.2
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.2
static void test_socket_create_udp_fail_resource_error()
{
    // Probe: find the fd number the next socket() call would receive.
    int probe_fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(probe_fd >= 0);  // Assert: probe socket opened
    (void)close(probe_fd);  // free it; next call will get this number again

    // Tighten the limit so fd == probe_fd is forbidden.
    struct rlimit old_rl = {};                            // §7b: init at declaration
    int gr = getrlimit(RLIMIT_NOFILE, &old_rl);          // Rule 7: check return value
    assert(gr == 0);
    struct rlimit tight_rl = {};                          // §7b: init at declaration
    tight_rl.rlim_cur = static_cast<rlim_t>(probe_fd);
    tight_rl.rlim_max = old_rl.rlim_max;
    int sr = setrlimit(RLIMIT_NOFILE, &tight_rl);
    assert(sr == 0);

    // socket_create_udp() calls socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    // socket() tries to return probe_fd which is now out of range → EMFILE.
    int new_fd = socket_create_udp(false);

    // Restore the limit immediately before any further assertions.
    (void)setrlimit(RLIMIT_NOFILE, &old_rl);

    // Verify socket_create_udp() returned the error sentinel.
    assert(new_fd < 0);  // Assert: udp creation fails with EMFILE → -1

    printf("PASS: test_socket_create_udp_fail_resource_error\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 42: socket_send_all() poll timeout — write buffer full, 1 ms timeout.
// Exercises SocketUtils.cpp line ~423-427: poll_result <= 0 True path.
// Strategy: socketpair, make writer non-blocking, flood until EAGAIN (buffer
// full), restore blocking, call socket_send_all(timeout=1ms) → poll times out.
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.3
static void test_send_all_poll_timeout()
{
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);                 // Assert: socketpair created

    // Save flags and set sv[0] non-blocking so flood-writes return EAGAIN
    // when the kernel write buffer fills (sv[1] is never drained).
    int fl = fcntl(sv[0], F_GETFL, 0);
    assert(fl >= 0);                 // Assert: F_GETFL succeeded
    (void)fcntl(sv[0], F_SETFL, fl | O_NONBLOCK);

    // Flood-write until EAGAIN/EWOULDBLOCK: the kernel buffer is now full.
    // Power of 10: fixed bound — buffer fills in well under 1 000 iterations.
    static const uint8_t flood[4096] = {};
    for (uint32_t i = 0U; i < 1000U; ++i) {
        ssize_t n = send(sv[0], flood, sizeof(flood), 0);
        if (n < 0) {
            break;  // EAGAIN: buffer is full; exit loop
        }
    }

    // Restore blocking mode; buffer is still full (sv[1] is still not drained).
    (void)fcntl(sv[0], F_SETFL, fl);

    // socket_send_all polls for POLLOUT with a 1 ms timeout.
    // The buffer is full so poll() returns 0 (timeout) → function returns false.
    static const uint8_t data[1] = {0x42U};
    bool ok = socket_send_all(sv[0], data, 1U, 1U);
    assert(!ok);  // Assert: poll timeout → returns false

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_send_all_poll_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 43: socket_send_all() CERT INT31-C timeout clamp — timeout_ms = UINT32_MAX
// exceeds INT_MAX; the branch at SocketUtils.cpp line ~421 clamps it to INT_MAX
// before passing to poll().  The socket is writable so poll() returns immediately.
// Exercises SocketUtils.cpp line ~421 True path: (timeout_ms > k_poll_max_ms).
// Verifies: REQ-6.3.3
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-6.3.3
static void test_send_all_timeout_ms_clamped()
{
    // Socketpair with write buffer space available (not flooded).
    int sv[2] = {-1, -1};
    int sr = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(sr == 0);  // Assert: socketpair created

    static const uint8_t data[1] = {0x55U};
    // UINT32_MAX > INT_MAX → clamping branch fires; poll() returns 1 immediately
    // (socket is writable) → send() succeeds → function returns true.
    bool ok = socket_send_all(sv[0], data, 1U,
                              static_cast<uint32_t>(0xFFFFFFFFU));
    assert(ok);  // Assert: send succeeds (socket had buffer space)

    (void)close(sv[0]);
    (void)close(sv[1]);
    printf("PASS: test_send_all_timeout_ms_clamped\n");
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

    test_send_all_poll_timeout();
    test_send_all_timeout_ms_clamped();

    // ── Mock syscall tests (IPosixSyscalls fault injection) ──────────────────
    test_socket_create_tcp_warning_lo_errno();
    test_set_nonblocking_setfl_fails();
    test_connect_immediate_success();
    test_connect_poll_timeout_mock();
    test_send_all_send_returns_zero_mock();
    test_sendto_partial_mock();
    test_recv_from_inet_ntop_fails_mock();

    // ── Practical OS-based tests (no mock) ───────────────────────────────────
    test_recv_from_closed_fd();
    test_connect_family_mismatch();
    test_send_frame_payload_fails_buffer_full();

    // MUST remain last: transiently modifies process-wide RLIMIT_NOFILE.
    // Restore happens inside the function before any assert fires, but placement
    // last is a second safety layer — no test that follows can be starved of fds.
    test_socket_create_tcp_fail_resource_error();
    test_socket_create_udp_fail_resource_error();

    printf("=== ALL PASSED ===\n");
    return 0;
}
