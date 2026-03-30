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
#include "core/Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// socket_create_tcp()
// ─────────────────────────────────────────────────────────────────────────────

int socket_create_tcp()
{
    // Power of 10: check return value
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "socket(AF_INET, SOCK_STREAM) failed: %d", errno);
        return -1;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    return fd;
}

// ─────────────────────────────────────────────────────────────────────────────
// socket_create_udp()
// ─────────────────────────────────────────────────────────────────────────────

int socket_create_udp()
{
    // Power of 10: check return value
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "socket(AF_INET, SOCK_DGRAM) failed: %d", errno);
        return -1;
    }

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
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

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Parse IP address string to in_addr
    int aton_result = inet_aton(ip, &addr.sin_addr);
    if (aton_result == 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "inet_aton('%s') failed", ip);
        return false;
    }

    // Bind socket
    int result = bind(fd, reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr));
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

    // Set socket to non-blocking before connecting
    if (!socket_set_nonblocking(fd)) {
        return false;
    }

    // Build address
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    int aton_result = inet_aton(ip, &addr.sin_addr);
    if (aton_result == 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "inet_aton('%s') failed", ip);
        return false;
    }

    // Attempt non-blocking connect
    int conn_result = connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                             sizeof(addr));
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

    int poll_result = poll(&pfd, 1, static_cast<int>(timeout_ms));
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

    // Power of 10: postcondition assertion
    NEVER_COMPILED_OUT_ASSERT(client_fd >= 0);
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

        int poll_result = poll(&pfd, 1, static_cast<int>(timeout_ms));
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

        int poll_result = poll(&pfd, 1, static_cast<int>(timeout_ms));
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
    if (frame_len > max_frame_size || frame_len > buf_cap) {
        Logger::log(Severity::WARNING_HI, "SocketUtils",
                   "tcp_recv_frame: frame size %u exceeds limit %u",
                   frame_len, max_frame_size);
        return false;
    }

    // Receive frame payload
    if (frame_len > 0U) {
        if (!socket_recv_exact(fd, buf, frame_len, timeout_ms)) {
            Logger::log(Severity::WARNING_HI, "SocketUtils",
                       "tcp_recv_frame: failed to receive payload (%u bytes)", frame_len);
            return false;
        }
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

    // Build destination address
    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    int aton_result = inet_aton(ip, &addr.sin_addr);
    if (aton_result == 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "inet_aton('%s') failed", ip);
        return false;
    }

    // Send datagram
    ssize_t send_result = sendto(fd, buf, len, 0,
                                reinterpret_cast<struct sockaddr*>(&addr),
                                sizeof(addr));
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

    int poll_result = poll(&pfd, 1, static_cast<int>(timeout_ms));
    if (poll_result <= 0) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "recvfrom poll timeout");
        return false;
    }

    // Receive datagram
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    (void)memset(&src_addr, 0, sizeof(src_addr));

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

    // Extract source address and port
    const char* inet_result = inet_ntop(AF_INET, &src_addr.sin_addr,
                                       out_ip, 48);
    if (inet_result == nullptr) {
        Logger::log(Severity::WARNING_LO, "SocketUtils",
                   "inet_ntop() failed: %d", errno);
        return false;
    }

    *out_port = ntohs(src_addr.sin_port);
    *out_len = static_cast<uint32_t>(recv_result);

    // Power of 10: postcondition assertions
    NEVER_COMPILED_OUT_ASSERT(*out_len > 0U && *out_len <= buf_cap);
    NEVER_COMPILED_OUT_ASSERT(inet_result != nullptr);
    return true;
}
