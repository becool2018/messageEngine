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
 * @file ISocketOps.hpp
 * @brief Pure-virtual interface wrapping SocketUtils calls used by transport
 *        backends that cannot be triggered in a loopback test environment
 *        without mock injection.
 *
 * Design rationale:
 *   TcpBackend, UdpBackend, TlsTcpBackend, and DtlsUdpBackend contain
 *   error-handling branches for POSIX failures (socket creation, bind, listen,
 *   accept, close, send, recv) that are architecturally unreachable in a
 *   loopback test: these POSIX calls succeed unconditionally when both
 *   endpoints are on 127.0.0.1 with no resource exhaustion.
 *   By routing these calls through ISocketOps, tests can inject a mock that
 *   forces each error path, satisfying M5 (fault injection) per VVP-001 §3.
 *
 * Pattern: identical to IMbedtlsOps — each method wraps exactly one
 *   SocketUtils function. No business logic, no state, no error policy.
 *   All policy (logging, recovery, return code mapping) lives in the caller.
 *
 * Power of 10 Rule 9: no explicit function pointers in production code.
 *   ISocketOps uses virtual dispatch (vtable), which is the documented
 *   permitted exception (CLAUDE.md §2, Rule 9 exception).
 *
 * MISRA C++:2023: virtual functions used per rules on polymorphism.
 *   All vtable-dispatched functions here are purely functional adapters;
 *   they do not manage object lifetime.
 *
 * Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#ifndef PLATFORM_ISOCKET_OPS_HPP
#define PLATFORM_ISOCKET_OPS_HPP

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// ISocketOps — abstract interface for injectable socket utility calls
// ─────────────────────────────────────────────────────────────────────────────

class ISocketOps {
public:
    virtual ~ISocketOps() {}

    // ── Socket creation ───────────────────────────────────────────────────────

    /// Create a TCP socket (AF_INET, SOCK_STREAM, IPPROTO_TCP).
    /// Wraps socket_create_tcp().
    /// @return File descriptor (≥0) on success; -1 on failure.
    virtual int create_tcp() = 0;

    /// Create a UDP socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP).
    /// Wraps socket_create_udp().
    /// @return File descriptor (≥0) on success; -1 on failure.
    virtual int create_udp() = 0;

    // ── Socket options ────────────────────────────────────────────────────────

    /// Set SO_REUSEADDR on @p fd.
    /// Wraps socket_set_reuseaddr().
    /// @return true on success; false on failure.
    virtual bool set_reuseaddr(int fd) = 0;

    /// Set O_NONBLOCK on @p fd.
    /// Wraps socket_set_nonblocking().
    /// @return true on success; false on failure.
    virtual bool set_nonblocking(int fd) = 0;

    // ── Bind / listen / accept ────────────────────────────────────────────────

    /// Bind @p fd to @p ip and @p port.
    /// Wraps socket_bind().
    /// @return true on success; false on failure.
    virtual bool do_bind(int fd, const char* ip, uint16_t port) = 0;

    /// Place @p fd into the listening state with the given @p backlog.
    /// Wraps socket_listen().
    /// @return true on success; false on failure.
    virtual bool do_listen(int fd, int backlog) = 0;

    /// Accept an incoming connection on @p fd.
    /// Wraps socket_accept().
    /// @return Client file descriptor (≥0) on success; -1 on failure.
    virtual int do_accept(int fd) = 0;

    // ── Connect ───────────────────────────────────────────────────────────────

    /// Connect @p fd to @p ip:@p port with a wall-clock timeout.
    /// Wraps socket_connect_with_timeout().
    /// @return true on success; false on failure or timeout.
    virtual bool connect_with_timeout(int         fd,
                                      const char* ip,
                                      uint16_t    port,
                                      uint32_t    timeout_ms) = 0;

    // ── Close ─────────────────────────────────────────────────────────────────

    /// Close @p fd.
    /// Wraps socket_close().
    virtual void do_close(int fd) = 0;

    // ── TCP framed I/O ────────────────────────────────────────────────────────

    /// Send a length-prefixed frame over a TCP @p fd.
    /// Wraps tcp_send_frame().
    /// @return true on success; false on failure or timeout.
    virtual bool send_frame(int            fd,
                            const uint8_t* buf,
                            uint32_t       len,
                            uint32_t       timeout_ms) = 0;

    /// Receive a length-prefixed frame from a TCP @p fd.
    /// Wraps tcp_recv_frame().
    /// @return true on success; false on failure, timeout, or oversized frame.
    virtual bool recv_frame(int       fd,
                            uint8_t*  buf,
                            uint32_t  buf_cap,
                            uint32_t  timeout_ms,
                            uint32_t* out_len) = 0;

    // ── UDP datagram I/O ──────────────────────────────────────────────────────

    /// Send @p len bytes to @p ip:@p port as a UDP datagram.
    /// Wraps socket_send_to().
    /// @return true on success; false on failure.
    virtual bool send_to(int            fd,
                         const uint8_t* buf,
                         uint32_t       len,
                         const char*    ip,
                         uint16_t       port) = 0;

    /// Receive one UDP datagram from @p fd into @p buf.
    /// Wraps socket_recv_from().
    /// @return true on success; false on failure or timeout.
    virtual bool recv_from(int       fd,
                           uint8_t*  buf,
                           uint32_t  buf_cap,
                           uint32_t  timeout_ms,
                           uint32_t* out_len,
                           char*     out_ip,
                           uint16_t* out_port) = 0;
};

#endif // PLATFORM_ISOCKET_OPS_HPP
