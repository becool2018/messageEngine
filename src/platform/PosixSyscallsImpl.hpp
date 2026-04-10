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
 * @file PosixSyscallsImpl.hpp
 * @brief Concrete production implementation of IPosixSyscalls.
 *
 * Each method delegates directly to the corresponding real POSIX syscall
 * with ::-qualified names to suppress ADL. No error handling, no retry
 * logic, no policy — those live in the calling SocketUtils.cpp functions.
 *
 * Singleton pattern identical to SocketOpsImpl and MbedtlsOpsImpl: a single
 * process-wide instance is returned by instance(). SocketUtils.cpp uses this
 * by default via its 1-arg overloads; tests inject a mock via the 2-arg
 * overloads that accept IPosixSyscalls&.
 *
 * NSC: lifecycle/delegation only; no safety-critical logic.
 * Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#ifndef PLATFORM_POSIX_SYSCALLS_IMPL_HPP
#define PLATFORM_POSIX_SYSCALLS_IMPL_HPP

#include "platform/IPosixSyscalls.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// PosixSyscallsImpl — production singleton delegating to real POSIX syscalls
// ─────────────────────────────────────────────────────────────────────────────

class PosixSyscallsImpl : public IPosixSyscalls {
public:
    /// Return the process-wide singleton instance.
    static PosixSyscallsImpl& instance();

    // IPosixSyscalls overrides — each delegates to the corresponding POSIX
    // syscall. See IPosixSyscalls.hpp for parameter and return semantics.

    int     sys_socket(int domain, int type, int protocol)             override;
    int     sys_fcntl(int fd, int cmd, int arg)                        override;
    int     sys_connect(int fd,
                        const struct sockaddr* addr,
                        socklen_t addrlen)                             override;
    int     sys_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) override;
    ssize_t sys_send(int fd,
                     const void* buf,
                     size_t len,
                     int flags)                                        override;
    ssize_t sys_sendto(int fd,
                       const void* buf,
                       size_t len,
                       int flags,
                       const struct sockaddr* dest_addr,
                       socklen_t addrlen)                              override;
    ssize_t sys_recvfrom(int fd,
                         void* buf,
                         size_t len,
                         int flags,
                         struct sockaddr* src_addr,
                         socklen_t* addrlen)                           override;
    const char* sys_inet_ntop(int af,
                               const void* src,
                               char* dst,
                               socklen_t size)                         override;

private:
    PosixSyscallsImpl() = default;
};

#endif // PLATFORM_POSIX_SYSCALLS_IMPL_HPP
