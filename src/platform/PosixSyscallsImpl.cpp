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
 * @file PosixSyscallsImpl.cpp
 * @brief Production implementation of IPosixSyscalls — thin delegation to
 *        real POSIX syscalls.
 *
 * Rules applied:
 *   - Power of 10 Rule 4: each function is a single delegation call.
 *   - Power of 10 Rule 5: precondition assertions on all arguments.
 *   - Power of 10 Rule 7: return values passed through unchanged.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - Power of 10 Rule 9 exception: virtual dispatch via IPosixSyscalls vtable
 *     (CLAUDE.md §2, Rule 9 exception — compiler-generated vtable only).
 *
 * NSC: lifecycle/delegation only; no safety-critical logic.
 * Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#include "platform/PosixSyscallsImpl.hpp"
#include "core/Assert.hpp"
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

PosixSyscallsImpl& PosixSyscallsImpl::instance()
{
    static PosixSyscallsImpl s_instance;
    return s_instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket creation
// ─────────────────────────────────────────────────────────────────────────────

int PosixSyscallsImpl::sys_socket(int domain, int type, int protocol)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(domain == AF_INET || domain == AF_INET6);
    NEVER_COMPILED_OUT_ASSERT(type == SOCK_STREAM || type == SOCK_DGRAM);
    return ::socket(domain, type, protocol);
}

// ─────────────────────────────────────────────────────────────────────────────
// File control
// ─────────────────────────────────────────────────────────────────────────────

int PosixSyscallsImpl::sys_fcntl(int fd, int cmd, int arg)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(cmd == F_GETFL || cmd == F_SETFL);
    return ::fcntl(fd, cmd, arg);
}

// ─────────────────────────────────────────────────────────────────────────────
// Connection
// ─────────────────────────────────────────────────────────────────────────────

int PosixSyscallsImpl::sys_connect(int fd,
                                    const struct sockaddr* addr,
                                    socklen_t addrlen)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(addr != nullptr);
    return ::connect(fd, addr, addrlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll
// ─────────────────────────────────────────────────────────────────────────────

int PosixSyscallsImpl::sys_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fds != nullptr);
    NEVER_COMPILED_OUT_ASSERT(static_cast<uint32_t>(nfds) > 0U);
    return ::poll(fds, nfds, timeout_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP send
// ─────────────────────────────────────────────────────────────────────────────

ssize_t PosixSyscallsImpl::sys_send(int fd, const void* buf, size_t len, int flags)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return ::send(fd, buf, len, flags);
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP sendto
// ─────────────────────────────────────────────────────────────────────────────

ssize_t PosixSyscallsImpl::sys_sendto(int fd,
                                       const void* buf,
                                       size_t len,
                                       int flags,
                                       const struct sockaddr* dest_addr,
                                       socklen_t addrlen)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return ::sendto(fd, buf, len, flags, dest_addr, addrlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP recvfrom
// ─────────────────────────────────────────────────────────────────────────────

ssize_t PosixSyscallsImpl::sys_recvfrom(int fd,
                                         void* buf,
                                         size_t len,
                                         int flags,
                                         struct sockaddr* src_addr,
                                         socklen_t* addrlen)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return ::recvfrom(fd, buf, len, flags, src_addr, addrlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// Address conversion
// ─────────────────────────────────────────────────────────────────────────────

const char* PosixSyscallsImpl::sys_inet_ntop(int af,
                                              const void* src,
                                              char* dst,
                                              socklen_t size)
{
    // Power of 10 Rule 5: precondition assertions
    NEVER_COMPILED_OUT_ASSERT(src != nullptr);
    NEVER_COMPILED_OUT_ASSERT(dst != nullptr);
    return ::inet_ntop(af, src, dst, size);
}
