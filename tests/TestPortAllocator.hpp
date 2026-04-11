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
 * @file TestPortAllocator.hpp
 * @brief Ephemeral-port helper for test isolation.
 *
 * alloc_ephemeral_port() opens a temporary socket on port 0, asks the OS to
 * assign a free port via getsockname(), closes the socket, and returns the
 * port number.  The caller can then bind its own socket to that port.
 *
 * There is a small TOCTOU window between close() and the caller's subsequent
 * bind().  This is acceptable in the loopback-only test environment; the
 * window is a few microseconds on the local interface and no other process
 * allocates in that range simultaneously.
 *
 * Using ephemeral ports (rather than hardcoded constants) lets multiple
 * instances of the test suite run on the same machine without port conflicts
 * — e.g., a CI sanitizer run overlapping with a developer's local run.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, no recursion, all returns checked.
 *   - reinterpret_cast permitted for sockaddr ↔ sockaddr_in conversions per
 *     MISRA C++:2023 Rule 5.2.4 (pointer-to-object for OS API access).
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - MISRA advisory rules relaxed in tests/ per CLAUDE.md §9 table.
 */

#ifndef TESTS_TEST_PORT_ALLOCATOR_HPP
#define TESTS_TEST_PORT_ALLOCATOR_HPP

#include <cassert>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Returns an OS-assigned ephemeral port for the given POSIX socket type
// (SOCK_STREAM for TCP, SOCK_DGRAM for UDP).
// MISRA 5.2.4 deviation: reinterpret_cast used for OS API sockaddr conversion.
static uint16_t alloc_ephemeral_port(int sock_type)
{
    assert(sock_type == SOCK_STREAM || sock_type == SOCK_DGRAM);

    int fd = ::socket(AF_INET, sock_type, 0);
    assert(fd >= 0);

    // SO_REUSEADDR lets the caller's subsequent bind succeed even if the
    // port lingers in TIME_WAIT after the temporary socket closes.
    int opt = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                       &opt, static_cast<socklen_t>(sizeof(opt)));

    struct sockaddr_in addr;
    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0U;  // OS assigns the port

    // MISRA 5.2.4: reinterpret_cast for OS API sockaddr pointer conversion.
    int r = ::bind(fd,
                   reinterpret_cast<struct sockaddr*>(&addr),  // NOLINT
                   static_cast<socklen_t>(sizeof(addr)));
    assert(r == 0);

    struct sockaddr_in bound;
    socklen_t len = static_cast<socklen_t>(sizeof(bound));
    r = ::getsockname(fd,
                      reinterpret_cast<struct sockaddr*>(&bound),  // NOLINT
                      &len);
    assert(r == 0);

    const uint16_t port = ntohs(bound.sin_port);
    assert(port > 0U);

    (void)::close(fd);
    return port;
}

#endif  // TESTS_TEST_PORT_ALLOCATOR_HPP
