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
 * @file IPosixSyscalls.hpp
 * @brief Pure-virtual interface wrapping the 8 POSIX syscalls used inside
 *        SocketUtils.cpp that are architecturally unreachable in a loopback
 *        test environment without fault injection.
 *
 * Design rationale:
 *   SocketUtils.cpp contains error-handling branches for POSIX syscall failures
 *   (socket(), fcntl(), connect(), poll(), send(), sendto(), recvfrom(),
 *   inet_ntop()) that cannot be triggered on a loopback test environment without
 *   explicit kernel fault injection. By routing these calls through IPosixSyscalls,
 *   tests can inject a mock that forces each error path, satisfying M5 (fault
 *   injection) per VVP-001 §3.
 *
 * Pattern: identical to IMbedtlsOps and ISocketOps — each method wraps exactly
 *   one POSIX syscall. No business logic, no state, no error policy. All policy
 *   (logging, recovery, return code mapping) lives in SocketUtils.cpp callers.
 *
 * Power of 10 Rule 9: no explicit function pointers in production code.
 *   IPosixSyscalls uses virtual dispatch (vtable), which is the documented
 *   permitted exception (CLAUDE.md §2, Rule 9 exception).
 *
 * MISRA C++:2023: virtual functions used per rules on polymorphism.
 *   All vtable-dispatched functions here are purely functional adapters;
 *   they do not manage object lifetime.
 *
 * Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#ifndef PLATFORM_IPOSIX_SYSCALLS_HPP
#define PLATFORM_IPOSIX_SYSCALLS_HPP

#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// IPosixSyscalls — abstract interface for injectable POSIX syscall wrappers
// ─────────────────────────────────────────────────────────────────────────────

class IPosixSyscalls {
public:
    virtual ~IPosixSyscalls() {}

    // ── Socket creation ───────────────────────────────────────────────────────

    /// Create a socket.
    /// Wraps ::socket().
    /// @param[in] domain   AF_INET or AF_INET6.
    /// @param[in] type     SOCK_STREAM or SOCK_DGRAM.
    /// @param[in] protocol IPPROTO_TCP or IPPROTO_UDP.
    /// @return File descriptor (≥0) on success; -1 on failure.
    virtual int sys_socket(int domain, int type, int protocol) = 0;

    // ── File control ──────────────────────────────────────────────────────────

    /// Get or set file descriptor flags (F_GETFL / F_SETFL form only).
    /// Wraps ::fcntl().
    /// @param[in] fd   File descriptor.
    /// @param[in] cmd  F_GETFL or F_SETFL.
    /// @param[in] arg  Flags argument (used for F_SETFL; ignored for F_GETFL).
    /// @return Current flags (F_GETFL) or 0 on success (F_SETFL); -1 on error.
    virtual int sys_fcntl(int fd, int cmd, int arg) = 0;

    // ── Connection ────────────────────────────────────────────────────────────

    /// Initiate a connection on a socket.
    /// Wraps ::connect().
    /// @param[in] fd        File descriptor.
    /// @param[in] addr      Pointer to destination address.
    /// @param[in] addrlen   Length of address structure.
    /// @return 0 on success; -1 on error (errno set).
    virtual int sys_connect(int fd,
                            const struct sockaddr* addr,
                            socklen_t addrlen) = 0;

    // ── Poll ──────────────────────────────────────────────────────────────────

    /// Poll a set of file descriptors for I/O readiness.
    /// Wraps ::poll().
    /// @param[in,out] fds        Array of pollfd structures.
    /// @param[in]     nfds       Number of elements in fds.
    /// @param[in]     timeout_ms Timeout in milliseconds.
    /// @return Number of ready descriptors (>0), 0 on timeout, -1 on error.
    virtual int sys_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) = 0;

    // ── TCP send ──────────────────────────────────────────────────────────────

    /// Send data on a connected socket.
    /// Wraps ::send().
    /// @param[in] fd    File descriptor.
    /// @param[in] buf   Pointer to data buffer.
    /// @param[in] len   Number of bytes to send.
    /// @param[in] flags Send flags.
    /// @return Number of bytes sent (≥0); -1 on error.
    virtual ssize_t sys_send(int fd,
                             const void* buf,
                             size_t len,
                             int flags) = 0;

    // ── UDP sendto ────────────────────────────────────────────────────────────

    /// Send a datagram to a specific destination.
    /// Wraps ::sendto().
    /// @param[in] fd        File descriptor.
    /// @param[in] buf       Pointer to data buffer.
    /// @param[in] len       Number of bytes to send.
    /// @param[in] flags     Send flags.
    /// @param[in] dest_addr Destination address.
    /// @param[in] addrlen   Length of destination address.
    /// @return Number of bytes sent (≥0); -1 on error.
    virtual ssize_t sys_sendto(int fd,
                               const void* buf,
                               size_t len,
                               int flags,
                               const struct sockaddr* dest_addr,
                               socklen_t addrlen) = 0;

    // ── UDP recvfrom ──────────────────────────────────────────────────────────

    /// Receive a message and capture sender address.
    /// Wraps ::recvfrom().
    /// @param[in]  fd       File descriptor.
    /// @param[out] buf      Pointer to receive buffer.
    /// @param[in]  len      Buffer capacity.
    /// @param[in]  flags    Receive flags.
    /// @param[out] src_addr Source address (filled in on return).
    /// @param[out] addrlen  Length of src_addr (in/out).
    /// @return Number of bytes received (≥0); -1 on error.
    virtual ssize_t sys_recvfrom(int fd,
                                 void* buf,
                                 size_t len,
                                 int flags,
                                 struct sockaddr* src_addr,
                                 socklen_t* addrlen) = 0;

    // ── Address conversion ────────────────────────────────────────────────────

    /// Convert a binary network address to a presentation string.
    /// Wraps ::inet_ntop().
    /// @param[in]  af   Address family (AF_INET or AF_INET6).
    /// @param[in]  src  Pointer to binary address.
    /// @param[out] dst  Pointer to output buffer.
    /// @param[in]  size Size of output buffer.
    /// @return Pointer to dst on success; nullptr on error.
    virtual const char* sys_inet_ntop(int af,
                                      const void* src,
                                      char* dst,
                                      socklen_t size) = 0;
};

#endif // PLATFORM_IPOSIX_SYSCALLS_HPP
