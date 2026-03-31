/**
 * @file AssertState.hpp
 * @brief Assert-fired flag and IResetHandler registration for
 *        NEVER_COMPILED_OUT_ASSERT (CLAUDE.md §10).
 *
 * In production builds (NDEBUG), NEVER_COMPILED_OUT_ASSERT:
 *   1. Logs FATAL with condition, file, and line.
 *   2. Calls the registered IResetHandler::on_fatal_assert() via virtual
 *      dispatch (Power of 10 Rule 9 vtable exception).  If no handler has
 *      been registered, falls back to ::abort() — identical to the debug path.
 *   3. Sets g_fatal_fired (if the handler returned without aborting), so
 *      callers can detect a soft-reset scenario on embedded targets.
 *
 * Design rationale:
 *   Previous option (a) — flag only — let execution continue past a violated
 *   precondition into undefined behaviour.  The new design calls a handler
 *   *before* returning, which either aborts (POSIX) or resets the MCU
 *   (embedded).  The flag is retained as a secondary mechanism for embedded
 *   targets where the handler performs a soft reset and returns.
 *
 *   Option (b) previously rejected because it required a raw function pointer
 *   (Power of 10 Rule 9 violation).  IResetHandler uses virtual dispatch
 *   (vtable) — the documented Rule 9 exception — resolving the conflict.
 *
 * Registration:
 *   Call assert_state::set_reset_handler(handler) once during init, before
 *   any component is used.  For POSIX builds, register AbortResetHandler.
 *   The handler is stored as a raw pointer to a statically-allocated object
 *   (Power of 10 Rule 3: no heap allocation).
 *
 * std::atomic<bool> carve-out: permitted per CLAUDE.md §3.
 * Thread safety: g_fatal_fired uses release/acquire ordering.
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#ifndef CORE_ASSERT_STATE_HPP
#define CORE_ASSERT_STATE_HPP

#include "core/IResetHandler.hpp"
#include <atomic>   // std::atomic — permitted carve-out (CLAUDE.md §3)

namespace assert_state {

/// Latching flag set to true by NEVER_COMPILED_OUT_ASSERT when an assertion
/// condition is violated and the registered handler returns (soft-reset path).
/// Remains true until cleared by check_and_clear().
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern std::atomic<bool> g_fatal_fired;

/// Register the handler invoked by NEVER_COMPILED_OUT_ASSERT in production
/// (NDEBUG) builds.  Must be called once during system initialisation.
/// @p handler must outlive all uses of NEVER_COMPILED_OUT_ASSERT (i.e. the
/// process lifetime).  Pass AbortResetHandler::instance() for POSIX builds.
/// Power of 10 Rule 3: stores a pointer to a caller-owned static object;
///   no heap allocation.
void set_reset_handler(IResetHandler& handler) noexcept;

/// Return the currently registered handler, or nullptr if none is set.
/// Used by the NEVER_COMPILED_OUT_ASSERT macro and by tests.
IResetHandler* get_reset_handler() noexcept;

/// Atomically read and clear the fatal-assertion flag.
/// Returns true if at least one assertion has fired since the last call.
/// Thread-safe: uses compare_exchange_strong with acquire/release ordering.
bool check_and_clear() noexcept;

/// Test hook: directly invoke the registered handler (or abort if none) with
/// the supplied arguments, then set g_fatal_fired if the handler returns.
/// Allows tests to verify handler dispatch without triggering the macro.
void trigger_handler_for_test(const char* cond,
                               const char* file,
                               int         line) noexcept;

} // namespace assert_state

#endif // CORE_ASSERT_STATE_HPP
