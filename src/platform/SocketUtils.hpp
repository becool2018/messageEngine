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
 * @file SocketUtils.hpp
 * @brief POSIX socket utility functions for TCP and UDP communication.
 *
 * Provides a thin wrapper over raw socket operations with error checking,
 * timeout support, and framing helpers. No C++ STL, no exceptions.
 *
 * Requirement traceability: messageEngine/CLAUDE.md §6 (Platform/network backend).
 *
 * Rules applied:
 *   - Power of 10: all return values checked, no recursion, fixed timeouts.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection per expression.
 *   - F-Prime style: return bool for success/fail; use Logger for errors.
 *
 * Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */

#ifndef PLATFORM_SOCKET_UTILS_HPP
#define PLATFORM_SOCKET_UTILS_HPP

#include <cstdint>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include "core/Types.hpp"
#include "core/Logger.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// TCP Socket Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Create a TCP socket (AF_INET, SOCK_STREAM).
/// @return File descriptor (>= 0) on success, -1 on error.
int socket_create_tcp();

/// Create a UDP socket (AF_INET, SOCK_DGRAM).
/// @return File descriptor (>= 0) on success, -1 on error.
int socket_create_udp();

/// Set a socket to non-blocking mode (O_NONBLOCK).
/// @param[in] fd File descriptor.
/// @return true on success, false on error.
bool socket_set_nonblocking(int fd);

/// Enable SO_REUSEADDR on the socket (avoids TIME_WAIT issues).
/// @param[in] fd File descriptor.
/// @return true on success, false on error.
bool socket_set_reuseaddr(int fd);

/// Bind a socket to an IP address and port.
/// @param[in] fd   File descriptor.
/// @param[in] ip   IPv4 address string (e.g., "127.0.0.1" or "0.0.0.0").
/// @param[in] port Port number (host byte order).
/// @return true on success, false on error.
bool socket_bind(int fd, const char* ip, uint16_t port);

/// Connect to a remote address with timeout (non-blocking connect via poll).
/// @param[in] fd          File descriptor.
/// @param[in] ip          IPv4 address string.
/// @param[in] port        Port number (host byte order).
/// @param[in] timeout_ms  Timeout in milliseconds.
/// @return true on success (connected), false on error or timeout.
bool socket_connect_with_timeout(int fd, const char* ip, uint16_t port,
                                  uint32_t timeout_ms);

/// Put a socket in listening mode.
/// @param[in] fd       File descriptor.
/// @param[in] backlog  Maximum pending connections.
/// @return true on success, false on error.
bool socket_listen(int fd, int backlog);

/// Accept an incoming connection.
/// @param[in] fd File descriptor of listening socket.
/// @return File descriptor of accepted client socket (>= 0), or -1 on error.
int socket_accept(int fd);

/// Close a socket and release the file descriptor.
/// @param[in] fd File descriptor.
void socket_close(int fd);

// ─────────────────────────────────────────────────────────────────────────────
// TCP Data Transfer Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Send all bytes on a TCP socket with timeout (handles partial writes).
/// Power of 10: loop bound is len (fixed by input).
///
/// @param[in] fd          File descriptor.
/// @param[in] buf         Pointer to data to send.
/// @param[in] len         Number of bytes to send.
/// @param[in] timeout_ms  Timeout per write operation (milliseconds).
/// @return true if all bytes sent successfully, false on error or timeout.
bool socket_send_all(int fd, const uint8_t* buf, uint32_t len,
                     uint32_t timeout_ms);

/// Receive exactly len bytes on a TCP socket with timeout (handles partial reads).
/// Power of 10: loop bound is len (fixed by input).
///
/// @param[in] fd          File descriptor.
/// @param[out] buf        Pointer to receive buffer.
/// @param[in] len         Number of bytes to receive.
/// @param[in] timeout_ms  Timeout per read operation (milliseconds).
/// @return true if all bytes received, false on error, timeout, or socket closed.
bool socket_recv_exact(int fd, uint8_t* buf, uint32_t len,
                       uint32_t timeout_ms);

/// Send a TCP frame: 4-byte big-endian length prefix + payload.
/// @param[in] fd          File descriptor.
/// @param[in] buf         Pointer to frame payload.
/// @param[in] len         Length of payload (must be < 2^31).
/// @param[in] timeout_ms  Timeout per write operation (milliseconds).
/// @return true if frame sent completely, false on error.
bool tcp_send_frame(int fd, const uint8_t* buf, uint32_t len,
                    uint32_t timeout_ms);

/// Receive a TCP frame: read 4-byte big-endian length prefix + payload.
/// Validates that the received length is reasonable (≤ max message size).
///
/// @param[in] fd          File descriptor.
/// @param[out] buf        Pointer to receive buffer.
/// @param[in] buf_cap     Capacity of receive buffer.
/// @param[in] timeout_ms  Timeout per read operation (milliseconds).
/// @param[out] out_len    Number of payload bytes read (excluding length header).
/// @return true if frame received completely, false on error or invalid length.
bool tcp_recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                    uint32_t timeout_ms, uint32_t* out_len);

// ─────────────────────────────────────────────────────────────────────────────
// UDP Functions
// ─────────────────────────────────────────────────────────────────────────────

/// Send a UDP datagram to a remote address.
/// @param[in] fd    File descriptor of UDP socket.
/// @param[in] buf   Pointer to data to send.
/// @param[in] len   Number of bytes to send.
/// @param[in] ip    Destination IPv4 address string.
/// @param[in] port  Destination port (host byte order).
/// @return true on success, false on error.
bool socket_send_to(int fd, const uint8_t* buf, uint32_t len,
                    const char* ip, uint16_t port);

/// Receive a UDP datagram with timeout.
/// @param[in] fd          File descriptor of UDP socket.
/// @param[out] buf        Pointer to receive buffer.
/// @param[in] buf_cap     Capacity of receive buffer (max UDP packet size).
/// @param[in] timeout_ms  Timeout in milliseconds.
/// @param[out] out_len    Number of bytes received (datagram size).
/// @param[out] out_ip     IPv4 source address string (caller provides 48-char buffer).
/// @param[out] out_port   Source port (host byte order).
/// @return true on success (datagram received), false on error or timeout.
bool socket_recv_from(int fd, uint8_t* buf, uint32_t buf_cap,
                      uint32_t timeout_ms, uint32_t* out_len,
                      char* out_ip, uint16_t* out_port);

#endif // PLATFORM_SOCKET_UTILS_HPP
