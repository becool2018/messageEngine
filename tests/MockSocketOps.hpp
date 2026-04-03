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

    // ── Call counters ─────────────────────────────────────────────────────────
    int n_do_close = 0;  ///< Incremented on each do_close() call.

    // ── Fake file descriptor returned on successful create ────────────────────
    // Must be ≥ 0 to satisfy preconditions in callers; not a real OS fd.
    static const int FAKE_FD = 100;

    ~MockSocketOps() override {}

    // ── Socket creation ───────────────────────────────────────────────────────

    int create_tcp(bool /*ipv6*/) override
    {
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

    /// Always returns -1 (EAGAIN equivalent: no pending connection).
    /// accept_clients() treats -1 as "no connection yet" and returns OK.
    int do_accept(int /*fd*/) override
    {
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
        return !fail_send_frame;
    }

    bool recv_frame(int /*fd*/, uint8_t* /*buf*/, uint32_t /*buf_cap*/,
                    uint32_t /*ms*/, uint32_t* out_len) override
    {
        if (out_len != nullptr) {
            *out_len = 0U;
        }
        return !fail_recv_frame;
    }

    // ── UDP datagram I/O ──────────────────────────────────────────────────────

    bool send_to(int /*fd*/, const uint8_t* /*buf*/, uint32_t /*len*/,
                 const char* /*ip*/, uint16_t /*port*/) override
    {
        return !fail_send_to;
    }

    bool recv_from(int /*fd*/, uint8_t* /*buf*/, uint32_t /*buf_cap*/,
                   uint32_t /*ms*/, uint32_t* out_len,
                   char* /*out_ip*/, uint16_t* /*out_port*/) override
    {
        if (out_len != nullptr) {
            *out_len = 0U;
        }
        return !fail_recv_from;
    }
};

#endif // TESTS_MOCK_SOCKET_OPS_HPP
