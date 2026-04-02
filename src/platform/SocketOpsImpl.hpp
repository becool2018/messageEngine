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
 * @file SocketOpsImpl.hpp
 * @brief Concrete production implementation of ISocketOps.
 *
 * Each method delegates directly to the corresponding SocketUtils free
 * function. No error handling, no retry logic, no policy — those live in
 * the calling backend.
 *
 * Singleton pattern identical to MbedtlsOpsImpl: a single process-wide
 * instance is returned by instance(). Transport backends use this by
 * default; tests inject a mock via the ISocketOps& constructor parameter.
 *
 * NSC: lifecycle/delegation only; no safety-critical logic.
 * Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#ifndef PLATFORM_SOCKET_OPS_IMPL_HPP
#define PLATFORM_SOCKET_OPS_IMPL_HPP

#include "platform/ISocketOps.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// SocketOpsImpl — production singleton delegating to SocketUtils
// ─────────────────────────────────────────────────────────────────────────────

class SocketOpsImpl : public ISocketOps {
public:
    /// Return the process-wide singleton instance.
    static SocketOpsImpl& instance();

    // ISocketOps overrides — each delegates to the corresponding SocketUtils
    // free function. See ISocketOps.hpp for parameter and return semantics.

    int  create_tcp()                                              override;
    int  create_udp()                                             override;
    bool set_reuseaddr(int fd)                                    override;
    bool set_nonblocking(int fd)                                  override;
    bool do_bind(int fd, const char* ip, uint16_t port)           override;
    bool do_listen(int fd, int backlog)                           override;
    int  do_accept(int fd)                                        override;
    bool connect_with_timeout(int fd, const char* ip,
                              uint16_t port,
                              uint32_t timeout_ms)                override;
    void do_close(int fd)                                         override;
    bool send_frame(int fd, const uint8_t* buf,
                    uint32_t len, uint32_t timeout_ms)            override;
    bool recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                    uint32_t timeout_ms, uint32_t* out_len)       override;
    bool send_to(int fd, const uint8_t* buf, uint32_t len,
                 const char* ip, uint16_t port)                   override;
    bool recv_from(int fd, uint8_t* buf, uint32_t buf_cap,
                   uint32_t timeout_ms, uint32_t* out_len,
                   char* out_ip, uint16_t* out_port)              override;

private:
    SocketOpsImpl() = default;
};

#endif // PLATFORM_SOCKET_OPS_IMPL_HPP
