/**
 * @file MockTransportInterface.hpp
 * @brief Test double for TransportInterface: controllable failure injection for
 *        transport-level error paths unreachable in a loopback test environment.
 *
 * Usage:
 *   MockTransportInterface mock;
 *   mock.fail_send_message = true;          // inject ERR_IO on next send
 *   DeliveryEngine engine;
 *   engine.init(&mock, cfg, local_id);
 *   Result r = engine.send(env, now_us);
 *   assert(r == Result::ERR_IO);
 *
 * Design:
 *   - All operations default to nominal success / ERR_TIMEOUT.
 *   - Setting fail_send_message = true makes send_message() return ERR_IO.
 *   - Setting fail_receive_message = true makes receive_message() return ERR_IO.
 *   - Default receive_message() returns ERR_TIMEOUT (no message available).
 *   - init() always returns OK; close() / is_open() are no-ops.
 *
 * Follows the MockSocketOps pattern from MockSocketOps.hpp.
 *
 * Power of 10 Rule 9 exception: virtual dispatch via TransportInterface vtable
 *   (CLAUDE.md §2 Rule 9 exception — compiler-generated vtable only).
 *
 * Rules applied:
 *   - Raw assert() permitted in tests/ per CLAUDE.md §9 table.
 *   - No STL, no dynamic allocation.
 *
 * NSC: test double only; no safety-critical logic.
 */

#ifndef TESTS_MOCK_TRANSPORT_INTERFACE_HPP
#define TESTS_MOCK_TRANSPORT_INTERFACE_HPP

#include <cstdint>
#include "core/TransportInterface.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// MockTransportInterface — concrete TransportInterface for M5 fault injection
//
// Injects ERR_IO from send_message() or receive_message() without any real
// socket operation, exercising DeliveryEngine error-handling paths that are
// architecturally unreachable in a LocalSimHarness loopback environment.
// ─────────────────────────────────────────────────────────────────────────────

struct MockTransportInterface : public TransportInterface {
    // ── Failure flags (false = nominal, true = return ERR_IO) ────────────────
    bool fail_send_message    = false;
    bool fail_receive_message = false;

    // ── Call counters ─────────────────────────────────────────────────────────
    int n_send_message    = 0;   ///< Incremented on each send_message() call.
    int n_receive_message = 0;   ///< Incremented on each receive_message() call.

    ~MockTransportInterface() override {}

    // ── Lifecycle (always succeed; DeliveryEngine::init takes an already-init'd ptr)

    Result init(const TransportConfig& /*config*/) override
    {
        return Result::OK;
    }

    void close() override {}

    bool is_open() const override { return true; }

    // ── Send ──────────────────────────────────────────────────────────────────

    Result send_message(const MessageEnvelope& /*envelope*/) override
    {
        ++n_send_message;
        return fail_send_message ? Result::ERR_IO : Result::OK;
    }

    // ── Receive ───────────────────────────────────────────────────────────────

    Result receive_message(MessageEnvelope& /*envelope*/,
                           uint32_t         /*timeout_ms*/) override
    {
        ++n_receive_message;
        if (fail_receive_message) {
            return Result::ERR_IO;
        }
        return Result::ERR_TIMEOUT;  // default: no message available
    }
};

#endif // TESTS_MOCK_TRANSPORT_INTERFACE_HPP
