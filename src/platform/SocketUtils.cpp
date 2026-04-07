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
 * @file SocketUtils.cpp
 * @brief Implementation of POSIX socket utility functions.
 *
 * All functions follow Power of 10 rules: checked returns, fixed loop bounds,
 * ≥2 assertions per non-trivial function, no recursion.
 *
 * Error handling uses Logger::log() with WARNING_LO for recoverable errors
 * and WARNING_HI for serious (connection/IO) errors.
 *
 * Rules applied:
 *   - Power of 10: fixed bounds (timeout, loop iterations), ≥2 assertions.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: Result/bool return codes, explicit error logging.
 *
 * Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */

#include "SocketUtils.hpp"
#include "core/Serializer.hpp"
#include <cerrno>
#include <climits>
#include "core/Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// fill_addr() — private helper: fill sockaddr_storage for IPv4 or IPv6
// ─────────────────────────────────────────────────────────────────────────────

/// Fill @p addr for @p ip:@p port. Detects IPv4/IPv6 from the address string.
/// Strips any zone-ID suffix (e.g. "%eth0") from IPv6 before calling inet_pton.
/// Power of 10 Rule 2: zone-strip loop bounded to 47 iterations.
/// @pre ip != nullptr; addr != nullptr; addr_len != nullptr
/// @return true on success; false if inet_pton() rejects the address string.
static bool fill_addr(const char* ip, uint16_t port,
                      struct sockaddr_storage* addr, socklen_t* addr_len)
{
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    NEVER_COMPILED_OUT_ASSERT(addr != nullptr);
    NEVER_COMPILED_OUT_ASSERT(addr_len != nullptr);

    (void)memset(addr, 0, sizeof(*addr));

    if (socket_is_ipv6(ip)) {
        // Strip optional zone-ID suffix (inet_pton rejects "%eth0" suffixes).
        // Power of 10 Rule 2: bounded to 47 characters.
        char stripped[48];
        uint32_t i = 0U;
        while (i < 47U && ip[i] != '\0' && ip[i] != '%') {
            stripped[i] = ip[i];
            ++i;
        }
        stripped[i] = '\0';

        // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
        // sockaddr_in6* is the POSIX-standard address-family polymorphism idiom.
        struct sockaddr_in6* a6 = reinterpret_cast<struct sockaddr_in6*>(addr);
        a6->sin6_family = AF_INET6;
        a6->sin6_port   = htons(port);
        if (inet_pton(AF_INET6, stripped, &a6->sin6_addr) != 1) {
            Logger::log(Severity::WARNING_LO, "SocketUtils",
                       "inet_pton(AF_INET6, '%s') failed", ip);
            return false;
        }
        *addr_len = static_cast<socklen_t>(sizeof(struct sockaddr_in6));
    } else {
        // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
        // sockaddr_in* is the POSIX-standard address-family polymorphism idiom.
        struct sockaddr_in* a4 = reinterpret_cast<struct sockaddr_in*>(addr);
        a4->sin_family = AF_INET;
        a4->sin_port   = htons(port);
        if (inet_pton(AF_INET, ip, &a4->sin_addr) != 1) {
            Logger::log(Severity::WARNING_LO, "SocketUtils",
                       "inet_pton(AF_INET, '%s') failed", ip);
            return false;
        }
        *addr_len = static_cast<socklen_t>(sizeof(struct sockaddr_in));
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(*addr_len > 0U);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// log_socket_create_error() — file-local CC-reduction helper (F-10)
// Logs socket() failure at WARNING_HI for resource/permission errors (EMFILE,
// ENFILE, EACCES, EPERM) and WARNING_LO for all other errors.  Extracted from
// socket_create_tcp/udp to keep their cognitive complexity within Rule 4.
// Power of 10: single-purpose, ≤1 page, ≥2 assertions.
// ─────────────────────────────────────────────────────────────────────────────

static void log_socket_create_error(const char* proto, const char* family_str, int err)
{
    NEVER_COMPILED_OUT_ASSERT(proto      != nullptr);  // pre-condition: non-null proto string
    NEVER_COMPILED_OUT_ASSERT(family_str != nullptr);  // pre-condition: non-null family string

    // F-10: EMFILE/ENFILE (fd exhaustion) and EACCES/EPERM (permission denied)
    // are system-wide; escalate to WARNING_HI.  All other errors stay WARNING_LO.
    bool is_resource_err = (err == EMFILE) || (err == ENFILE) ||
                           (err == EACCES) || (err == EPERM);
    Severity sev = is_resource_err ? Severity::WARNING_HI : Severity::WARNING_LO;
    if (is_resource_err) {
        Logger::log(sev, "SocketUtils",
                    "socket(%s, %s) failed (system resource/permission): %d",
                    proto, family_str, err);
    } else {
        Logger::log(sev, "SocketUtils",
                    "socket(%s, %s) failed: %d",
                    proto, family_str, err);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_create_tcp()
// ─────────────────────────────────────────────────────────────────────────────

int socket_create_tcp(bool ipv6)
{
    const int family = ipv6 ? AF_INET6 : AF_INET;
    NEVER_COMPILED_OUT_ASSERT(family == AF_INET || family == AF_INET6);  // Pre-condition

    // Power of 10: check return value
    int fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        log_socket_create_error("SOCK_STREAM", ipv6 ? "AF_INET6" : "AF_INET", errno);
        return -1;
    }

    // F-18: fd 0, 1, 2 are stdin/stdout/stderr; a new socket must be > 2.
    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd > 2);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_create_udp()
// ─────────────────────────────────────────────────────────────────────────────

int socket_create_udp(bool ipv6)
{
    const int family = ipv6 ? AF_INET6 : AF_INET;
    NEVER_COMPILED_OUT_ASSERT(family == AF_INET || family == AF_INET6);  // Pre-condition

    // Power of 10: check return value
    int fd = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        log_socket_create_error("SOCK_DGRAM", ipv6 ? "AF_INET6" : "AF_INET", errno);
        return -1;
    }

    // F-18: fd 0, 1, 2 are stdin/stdout/stderr; a new socket must be > 2.
    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd > 2);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_set_nonblocking()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_set_nonblocking(int fd)
{
    // Power of 10: precondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);

    // Get current flags
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "fcntl(F_GETFL) failed: %d", errno);
        return false;
    }

    // Set O_NONBLOCK
    flags |= O_NONBLOCK;
    int result = fcntl(fd, F_SETFL, flags);
    if (result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "fcntl(F_SETFL, O_NONBLOCK) failed: %d", errno);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(result == 0);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_set_reuseaddr()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_set_reuseaddr(int fd)
{
    // Power of 10: precondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);

    int optval = 1;
    int result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                           &optval, sizeof(optval));
    if (result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "setsockopt(SO_REUSEADDR) failed: %d", errno);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(result == 0);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_bind()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_bind(int fd, const char* ip, uint16_t port)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);

    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    if (!fill_addr(ip, port, &addr, &addr_len)) {
        return false;
    }

    // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
    // sockaddr* is the POSIX-standard socket API polymorphism idiom.
    int result = bind(fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len);
    if (result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "bind(%s:%u) failed: %d", ip, port, errno);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(result == 0);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_connect_with_timeout()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_connect_with_timeout(int fd, const char* ip, uint16_t port,
                                  uint32_t timeout_ms)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    // SEC-022: a zero timeout makes poll() return immediately (POSIX: poll with
    // timeout=0 is a non-blocking check), which causes connect() to appear to
    // time out on every call. Reject it early with a defensive guard.
    // SEC-022: timeout_ms == 0 makes poll() a non-blocking check, so connect()
    // always appears to time out. The assert enforces the precondition; no
    // additional runtime check is needed (cppcheck: if-check would be always-false).
    NEVER_COMPILED_OUT_ASSERT(timeout_ms > 0U);  // SEC-022: must have positive timeout

    // Set socket to non-blocking before connecting
    if (!socket_set_nonblocking(fd)) {
        return false;
    }

    // Build address (IPv4 or IPv6 detected from ip string)
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    if (!fill_addr(ip, port, &addr, &addr_len)) {
        return false;
    }

    // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
    // sockaddr* is the POSIX-standard socket API polymorphism idiom.
    int conn_result = connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                             addr_len);
    if (conn_result == 0) {
        // Immediate success
        NEVER_COMPILED_OUT_ASSERT(conn_result == 0);
        return true;
    }

    // Check for EINPROGRESS (expected for non-blocking)
    if (errno != EINPROGRESS) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "connect(%s:%u) failed: %d", ip, port, errno);
        return false;
    }

    // Wait for socket to become writable (using poll)
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    // CERT INT31-C: poll() takes int; clamp uint32_t to INT_MAX before narrowing.
    // Values > INT_MAX cast to negative, which POSIX defines as "block indefinitely".
    static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
    const uint32_t clamped_ms = (timeout_ms > k_poll_max_ms) ? k_poll_max_ms : timeout_ms;
    int poll_result = poll(&pfd, 1, static_cast<int>(clamped_ms));
    if (poll_result <= 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "connect(%s:%u) timeout", ip, port);
        return false;
    }

    // Check for connection error via SO_ERROR
    int opt_err = 0;
    socklen_t opt_len = sizeof(opt_err);
    int getsock_result = getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                   &opt_err, &opt_len);
    if (getsock_result < 0 || opt_err != 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "connect(%s:%u) failed after poll: %d", ip, port, opt_err);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(getsock_result == 0 && opt_err == 0);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_listen()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_listen(int fd, int backlog)
{
    // Power of 10: precondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);

    int result = listen(fd, backlog);
    if (result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "listen(backlog=%d) failed: %d", backlog, errno);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(result == 0);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_accept()
// ─────────────────────────────────────────────────────────────────────────────

int socket_accept(int fd)
{
    // Power of 10: precondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);

    // Accept without address information
    int client_fd = accept(fd, nullptr, nullptr);
    if (client_fd < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "accept() failed: %d", errno);
        return -1;
    }

    // G-6: a successfully accepted fd is a new socket — it must be > 2 (not stdin/stdout/stderr).
    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(client_fd > 2);
    return client_fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_close()
// ─────────────────────────────────────────────────────────────────────────────

void socket_close(int fd)
{
    // Power of 10: precondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);

    // Close the socket; ignore EINTR
    int result = close(fd);
    if (result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "close(%d) failed: %d", fd, errno);
        // Still consider it a success; the fd is likely closed
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_send_all()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_send_all(int fd, const uint8_t* buf, uint32_t len,
                     uint32_t timeout_ms)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    uint32_t sent = 0U;

    // Power of 10: fixed loop bound is len
    while (sent < len) {
        // Poll before send to check if writable (timeout)
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        // CERT INT31-C: clamp before narrowing uint32_t → int for poll().
        static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
        const uint32_t clamped_ms = (timeout_ms > k_poll_max_ms) ? k_poll_max_ms : timeout_ms;
        int poll_result = poll(&pfd, 1, static_cast<int>(clamped_ms));
        if (poll_result <= 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "send poll timeout (sent %u of %u bytes)", sent, len);
            return false;
        }

        // Send remaining bytes
        uint32_t remaining = len - sent;
        ssize_t send_result = send(fd, &buf[sent], remaining, 0);
        if (send_result < 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "send() failed: %d", errno);
            return false;
        }
        // F-4: send() returning 0 means the peer closed the connection.
        // Without this guard the loop spins indefinitely without advancing `sent`.
        if (send_result == 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "send() returned 0 (peer closed); aborting (sent %u of %u)",
                       sent, len);
            return false;
        }

        // Advance sent counter
        sent += static_cast<uint32_t>(send_result);
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(sent == len);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_recv_exact()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_recv_exact(int fd, uint8_t* buf, uint32_t len,
                       uint32_t timeout_ms)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);

    uint32_t received = 0U;

    // Power of 10: fixed loop bound is len
    while (received < len) {
        // Poll before recv to check if readable (timeout)
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        // CERT INT31-C: clamp before narrowing uint32_t → int for poll().
        static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
        const uint32_t clamped_ms = (timeout_ms > k_poll_max_ms) ? k_poll_max_ms : timeout_ms;
        int poll_result = poll(&pfd, 1, static_cast<int>(clamped_ms));
        if (poll_result <= 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "recv poll timeout (received %u of %u bytes)", received, len);
            return false;
        }

        // Receive remaining bytes
        uint32_t remaining = len - received;
        ssize_t recv_result = recv(fd, &buf[received], remaining, 0);
        if (recv_result < 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "recv() failed: %d", errno);
            return false;
        }

        // Check for socket closure (recv returns 0)
        if (recv_result == 0) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "recv() returned 0 (socket closed)");
            return false;
        }

        // Advance received counter
        received += static_cast<uint32_t>(recv_result);
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(received == len);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tcp_send_frame()
// ─────────────────────────────────────────────────────────────────────────────

bool tcp_send_frame(int fd, const uint8_t* buf, uint32_t len,
                    uint32_t timeout_ms)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);

    // Create 4-byte big-endian length header
    uint8_t header[4U];
    header[0] = static_cast<uint8_t>((len >> 24U) & 0xFFU);
    header[1] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
    header[2] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
    header[3] = static_cast<uint8_t>(len & 0xFFU);

    // Send header
    if (!socket_send_all(fd, header, 4U, timeout_ms)) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "tcp_send_frame: failed to send header");
        return false;
    }

    // Send payload
    if (len > 0U) {
        if (!socket_send_all(fd, buf, len, timeout_ms)) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "tcp_send_frame: failed to send payload (%u bytes)", len);
            return false;
        }
    }

    // Power of 10: postcondition assertion
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// tcp_recv_frame()
// ─────────────────────────────────────────────────────────────────────────────

bool tcp_recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                    uint32_t timeout_ms, uint32_t* out_len)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);

    // Receive 4-byte big-endian length header
    uint8_t header[4U];
    if (!socket_recv_exact(fd, header, 4U, timeout_ms)) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "tcp_recv_frame: failed to receive header");
        return false;
    }

    // Parse length from header (big-endian)
    uint32_t frame_len = (static_cast<uint32_t>(header[0]) << 24U) |
                        (static_cast<uint32_t>(header[1]) << 16U) |
                        (static_cast<uint32_t>(header[2]) << 8U) |
                        (static_cast<uint32_t>(header[3]));

    // Validate frame length
    uint32_t max_frame_size = Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES;
    // F-4: a valid framed message must be at least WIRE_HEADER_SIZE bytes;
    // zero-length or truncated frames cannot contain a valid envelope header.
    if (frame_len < Serializer::WIRE_HEADER_SIZE ||
        frame_len > max_frame_size || frame_len > buf_cap) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "tcp_recv_frame: invalid frame size %u (min=%u, max=%u)",
                   frame_len, static_cast<unsigned>(Serializer::WIRE_HEADER_SIZE),
                   max_frame_size);
        return false;
    }

    // Receive frame payload (frame_len >= WIRE_HEADER_SIZE > 0 guaranteed above).
    if (!socket_recv_exact(fd, buf, frame_len, timeout_ms)) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "tcp_recv_frame: failed to receive payload (%u bytes)", frame_len);
        return false;
    }

    *out_len = frame_len;

    // Power of 10: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(*out_len <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(*out_len <= max_frame_size);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_send_to()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_send_to(int fd, const uint8_t* buf, uint32_t len,
                    const char* ip, uint16_t port)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(len > 0U);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);

    // Build destination address (IPv4 or IPv6 detected from ip string)
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    if (!fill_addr(ip, port, &addr, &addr_len)) {
        return false;
    }

    // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
    // sockaddr* is the POSIX-standard socket API polymorphism idiom.
    ssize_t send_result = sendto(fd, buf, len, 0,
                                reinterpret_cast<struct sockaddr*>(&addr),
                                addr_len);
    if (send_result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "sendto(%s:%u) failed: %d", ip, port, errno);
        return false;
    }

    // Verify all bytes sent (UDP should be atomic, but check anyway)
    if (static_cast<uint32_t>(send_result) != len) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "sendto() sent %ld of %u bytes", send_result, len);
        return false;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(static_cast<uint32_t>(send_result) == len);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_recv_from()
// ─────────────────────────────────────────────────────────────────────────────

bool socket_recv_from(int fd, uint8_t* buf, uint32_t buf_cap,
                      uint32_t timeout_ms, uint32_t* out_len,
                      char* out_ip, uint16_t* out_port)
{
    // Power of 10: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_ip != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_port != nullptr);

    // Poll for readability with timeout
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    // CERT INT31-C: clamp before narrowing uint32_t → int for poll().
    static const uint32_t k_poll_max_ms = static_cast<uint32_t>(INT_MAX);
    const uint32_t clamped_ms = (timeout_ms > k_poll_max_ms) ? k_poll_max_ms : timeout_ms;
    int poll_result = poll(&pfd, 1, static_cast<int>(clamped_ms));
    if (poll_result <= 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "recvfrom poll timeout");
        return false;
    }

    // Receive datagram — sockaddr_storage handles both IPv4 and IPv6.
    struct sockaddr_storage src_addr;
    socklen_t src_len = static_cast<socklen_t>(sizeof(src_addr));
    (void)memset(&src_addr, 0, sizeof(src_addr));

    // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
    // sockaddr* is the POSIX-standard socket API polymorphism idiom.
    ssize_t recv_result = recvfrom(fd, buf, buf_cap, 0,
                                  reinterpret_cast<struct sockaddr*>(&src_addr),
                                  &src_len);
    if (recv_result < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "recvfrom() failed: %d", errno);
        return false;
    }

    if (recv_result == 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "recvfrom() returned 0 bytes");
        return false;
    }

    // Extract source address and port — family-aware.
    const char* ntop_result = nullptr;
    if (src_addr.ss_family == AF_INET6) {
        // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
        // sockaddr_in6* is the POSIX-standard address-family polymorphism idiom.
        const struct sockaddr_in6* a6 =
            reinterpret_cast<const struct sockaddr_in6*>(&src_addr);
        ntop_result = inet_ntop(AF_INET6, &a6->sin6_addr, out_ip, 48);
        *out_port = ntohs(a6->sin6_port);
    } else {
        // MISRA C++:2023 Rule 5.2.4: reinterpret_cast from sockaddr_storage* to
        // sockaddr_in* is the POSIX-standard address-family polymorphism idiom.
        const struct sockaddr_in* a4 =
            reinterpret_cast<const struct sockaddr_in*>(&src_addr);
        ntop_result = inet_ntop(AF_INET, &a4->sin_addr, out_ip, 48);
        *out_port = ntohs(a4->sin_port);
    }

    if (ntop_result == nullptr) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "inet_ntop() failed: %d", errno);
        return false;
    }

    *out_len = static_cast<uint32_t>(recv_result);

    // Power of 10: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(*out_len > 0U && *out_len <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(ntop_result != nullptr);
    return true;
}
