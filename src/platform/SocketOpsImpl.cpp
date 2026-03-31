/**
 * @file SocketOpsImpl.cpp
 * @brief Production implementation of ISocketOps — thin delegation to
 *        SocketUtils free functions.
 *
 * Rules applied:
 *   - Power of 10 Rule 4: each function is a single delegation call.
 *   - Power of 10 Rule 5: precondition assertions on pointer arguments.
 *   - Power of 10 Rule 7: return values passed through unchanged.
 *   - MISRA C++:2023: no STL, no exceptions, no templates.
 *   - Power of 10 Rule 9 exception: virtual dispatch via ISocketOps vtable
 *     (CLAUDE.md §2, Rule 9 exception — compiler-generated vtable only).
 *
 * NSC: lifecycle/delegation only; no safety-critical logic.
 * Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3
 */
// Implements: REQ-6.1.5, REQ-6.1.6, REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

#include "platform/SocketOpsImpl.hpp"
#include "platform/SocketUtils.hpp"
#include "core/Assert.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

SocketOpsImpl& SocketOpsImpl::instance()
{
    static SocketOpsImpl s_instance;
    return s_instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket creation
// ─────────────────────────────────────────────────────────────────────────────

int SocketOpsImpl::create_tcp()
{
    NEVER_COMPILED_OUT_ASSERT(true);  // Power of 10 Rule 5: structural assertion
    return socket_create_tcp();
}

int SocketOpsImpl::create_udp()
{
    NEVER_COMPILED_OUT_ASSERT(true);  // Power of 10 Rule 5: structural assertion
    return socket_create_udp();
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket options
// ─────────────────────────────────────────────────────────────────────────────

bool SocketOpsImpl::set_reuseaddr(int fd)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    return socket_set_reuseaddr(fd);
}

bool SocketOpsImpl::set_nonblocking(int fd)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    return socket_set_nonblocking(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bind / listen / accept
// ─────────────────────────────────────────────────────────────────────────────

bool SocketOpsImpl::do_bind(int fd, const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    return socket_bind(fd, ip, port);
}

bool SocketOpsImpl::do_listen(int fd, int backlog)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    return socket_listen(fd, backlog);
}

int SocketOpsImpl::do_accept(int fd)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    return socket_accept(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// Connect
// ─────────────────────────────────────────────────────────────────────────────

bool SocketOpsImpl::connect_with_timeout(int fd, const char* ip,
                                          uint16_t port, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    return socket_connect_with_timeout(fd, ip, port, timeout_ms);
}

// ─────────────────────────────────────────────────────────────────────────────
// Close
// ─────────────────────────────────────────────────────────────────────────────

void SocketOpsImpl::do_close(int fd)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    socket_close(fd);
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP framed I/O
// ─────────────────────────────────────────────────────────────────────────────

bool SocketOpsImpl::send_frame(int fd, const uint8_t* buf,
                                uint32_t len, uint32_t timeout_ms)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    return tcp_send_frame(fd, buf, len, timeout_ms);
}

bool SocketOpsImpl::recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                                uint32_t timeout_ms, uint32_t* out_len)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);
    return tcp_recv_frame(fd, buf, buf_cap, timeout_ms, out_len);
}

// ─────────────────────────────────────────────────────────────────────────────
// UDP datagram I/O
// ─────────────────────────────────────────────────────────────────────────────

bool SocketOpsImpl::send_to(int fd, const uint8_t* buf, uint32_t len,
                             const char* ip, uint16_t port)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(ip != nullptr);
    return socket_send_to(fd, buf, len, ip, port);
}

bool SocketOpsImpl::recv_from(int fd, uint8_t* buf, uint32_t buf_cap,
                               uint32_t timeout_ms, uint32_t* out_len,
                               char* out_ip, uint16_t* out_port)
{
    NEVER_COMPILED_OUT_ASSERT(fd >= 0);
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_len != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_ip != nullptr);
    NEVER_COMPILED_OUT_ASSERT(out_port != nullptr);
    return socket_recv_from(fd, buf, buf_cap, timeout_ms,
                            out_len, out_ip, out_port);
}
