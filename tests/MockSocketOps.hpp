/**
 * @file MockSocketOps.hpp
 * @brief Test double for ISocketOps: controllable failure injection for
 *        socket-level error paths unreachable in a loopback test environment.
 *
 * Usage:
 *   MockSocketOps mock;
 *   mock.fail_create_tcp = true;           // inject create_tcp failure
 *   TcpBackend backend(mock);
 *   Result r = backend.init(cfg);
 *   assert(r == Result::ERR_IO);
 *
 * Design:
 *   - All operations default to "success" (fail_* = false).
 *   - Setting a fail_* flag to true makes the corresponding method return the
 *     failure value expected by the backend (negative fd or false).
 *   - do_close() is a safe no-op; n_do_close counts invocations.
 *   - do_accept() returns -1 by default (EAGAIN: no pending connection).
 *   - recv_frame / recv_from return false when fail_* is set; when successful
 *     they set *out_len = 0 (produces a deserialise failure in the caller,
 *     which is acceptable for tests that only exercise the socket-layer path).
 *
 * Follows the DtlsMockOps pattern from test_DtlsUdpBackend.cpp.
 *
 * Power of 10 Rule 9 exception: virtual dispatch via ISocketOps vtable
 *   (CLAUDE.md §2 Rule 9 exception — compiler-generated vtable only).
 *
 * Rules applied:
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - STL exempted in tests/ for fixture setup only; not used here.
 *   - Dynamic allocation (new/delete) not used.
 *
 * NSC: test double only; no safety-critical logic.
 */

#ifndef TESTS_MOCK_SOCKET_OPS_HPP
#define TESTS_MOCK_SOCKET_OPS_HPP

#include <cstdint>
#include <cstring>
#include "platform/ISocketOps.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MockSocketOps — concrete ISocketOps for error-path injection tests
//
// Each method returns its configurable fail_* field without calling any real
// POSIX socket function.  This allows every hard POSIX error branch in the
// transport backends that cannot be triggered in a loopback environment to be
// exercised deterministically.
// ─────────────────────────────────────────────────────────────────────────────

struct MockSocketOps : public ISocketOps {
    // ── Failure flags (false = succeed, true = fail) ─────────────────────────
    bool fail_create_tcp      = false;
    bool fail_create_udp      = false;
    bool fail_set_reuseaddr   = false;
    bool fail_set_nonblocking = false;
    bool fail_do_bind         = false;
    bool fail_do_listen       = false;
    bool fail_connect         = false;
    bool fail_send_frame      = false;
    bool fail_recv_frame      = false;
    bool fail_send_to         = false;
    bool fail_recv_from       = false;

    // ── Configurable source address returned by recv_from() ───────────────────
    // Set recv_src_ip to a different IP to simulate a wrong-source datagram.
    char     recv_src_ip[48]  = "127.0.0.1";
    uint16_t recv_src_port    = 0U;

    // ── Call counters ─────────────────────────────────────────────────────────
    int      n_do_close       = 0;    ///< Incremented on each do_close() call.
    uint32_t sent_count       = 0U;   ///< Incremented by send_to() on each successful call.
    uint32_t send_frame_count = 0U;   ///< Incremented by send_frame() on each successful call.

    // ── Fake file descriptor returned on successful create ────────────────────
    // Must be ≥ 0 to satisfy preconditions in callers; not a real OS fd.
    static const int FAKE_FD = 100;

    // ── One-shot overrides for socketpair-based tests ─────────────────────────
    // These allow a real OS fd (from socketpair()) to be used as the listen or
    // client fd so that poll() fires POLLIN and accept/recv paths execute.

    /// If >= 0, returned by the first create_tcp() call; subsequent calls
    /// return FAKE_FD as normal.  Lets a real socketpair fd become m_listen_fd
    /// so poll() returns POLLIN when the paired end is written to.
    int  create_tcp_once_fd   = -1;
    bool create_tcp_once_done = false;

    /// If >= 0, returned by the first do_accept() call; subsequent calls
    /// return -1 (EAGAIN).  Lets a real socketpair fd be accepted as a client.
    int  accept_once_fd   = -1;
    bool accept_once_done = false;

    /// When recv_frame_once_len > 0, the first recv_frame() call returns these
    /// bytes and sets recv_frame_once_done = true.  Subsequent calls use the
    /// normal fail_recv_frame / zero-len behaviour.
    uint8_t  recv_frame_once_buf[512U] = {};
    uint32_t recv_frame_once_len       = 0U;
    bool     recv_frame_once_done      = false;

    ~MockSocketOps() override {}

    // ── Socket creation ───────────────────────────────────────────────────────

    int create_tcp(bool /*ipv6*/) override
    {
        if (!create_tcp_once_done && (create_tcp_once_fd >= 0)) {
            create_tcp_once_done = true;
            return create_tcp_once_fd;
        }
        return fail_create_tcp ? -1 : FAKE_FD;
    }

    int create_udp(bool /*ipv6*/) override
    {
        return fail_create_udp ? -1 : FAKE_FD;
    }

    // ── Socket options ────────────────────────────────────────────────────────

    bool set_reuseaddr(int /*fd*/) override
    {
        return !fail_set_reuseaddr;
    }

    bool set_nonblocking(int /*fd*/) override
    {
        return !fail_set_nonblocking;
    }

    // ── Bind / listen / accept ────────────────────────────────────────────────

    bool do_bind(int /*fd*/, const char* /*ip*/, uint16_t /*port*/) override
    {
        return !fail_do_bind;
    }

    bool do_listen(int /*fd*/, int /*backlog*/) override
    {
        return !fail_do_listen;
    }

    /// Returns accept_once_fd on the first call (if configured); then -1.
    /// accept_clients() treats -1 as "no connection yet" and returns OK.
    int do_accept(int /*fd*/) override
    {
        if (!accept_once_done && (accept_once_fd >= 0)) {
            accept_once_done = true;
            return accept_once_fd;
        }
        return -1;
    }

    // ── Connect ───────────────────────────────────────────────────────────────

    bool connect_with_timeout(int /*fd*/, const char* /*ip*/,
                               uint16_t /*port*/, uint32_t /*ms*/) override
    {
        return !fail_connect;
    }

    // ── Close ─────────────────────────────────────────────────────────────────

    void do_close(int /*fd*/) override
    {
        ++n_do_close;
    }

    // ── TCP framed I/O ────────────────────────────────────────────────────────

    bool send_frame(int /*fd*/, const uint8_t* /*buf*/,
                    uint32_t /*len*/, uint32_t /*ms*/) override
    {
        if (!fail_send_frame) {
            ++send_frame_count;
            return true;
        }
        return false;
    }

    bool recv_frame(int /*fd*/, uint8_t* buf, uint32_t buf_cap,
                    uint32_t /*ms*/, uint32_t* out_len) override
    {
        // One-shot: return pre-loaded bytes on first call (e.g. a serialized HELLO).
        if (!recv_frame_once_done && (recv_frame_once_len > 0U)) {
            recv_frame_once_done = true;
            uint32_t n = (recv_frame_once_len < buf_cap) ? recv_frame_once_len : buf_cap;
            if (buf != nullptr) {
                (void)memcpy(buf, recv_frame_once_buf, static_cast<size_t>(n));
            }
            if (out_len != nullptr) {
                *out_len = n;
            }
            return true;
        }
        if (out_len != nullptr) {
            *out_len = 0U;
        }
        return !fail_recv_frame;
    }

    // ── UDP datagram I/O ──────────────────────────────────────────────────────

    bool send_to(int /*fd*/, const uint8_t* /*buf*/, uint32_t /*len*/,
                 const char* /*ip*/, uint16_t /*port*/) override
    {
        if (!fail_send_to) {
            ++sent_count;
            return true;
        }
        return false;
    }

    bool recv_from(int /*fd*/, uint8_t* /*buf*/, uint32_t /*buf_cap*/,
                   uint32_t /*ms*/, uint32_t* out_len,
                   char* out_ip, uint16_t* out_port) override
    {
        if (out_len != nullptr) {
            *out_len = 0U;
        }
        if (out_ip != nullptr) {
            (void)strncpy(out_ip, recv_src_ip, 47U);
            out_ip[47U] = '\0';
        }
        if (out_port != nullptr) {
            *out_port = recv_src_port;
        }
        return !fail_recv_from;
    }
};

#endif // TESTS_MOCK_SOCKET_OPS_HPP
