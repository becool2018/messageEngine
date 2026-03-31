/**
 * @file AssertState.hpp
 * @brief Global atomic assertion-fired flag: production component-reset hook
 *        for NEVER_COMPILED_OUT_ASSERT (CLAUDE.md §10, option a).
 *
 * In production builds (NDEBUG), NEVER_COMPILED_OUT_ASSERT sets g_fatal_fired
 * to true when an assertion condition is violated.  Library callers may poll
 * assert_state::check_and_clear() after any API call to detect a component-level
 * safety-assertion failure without process abort.
 *
 * Design rationale — option (a) chosen over alternatives:
 *   (b) user-registered callback: requires storing a function pointer in
 *       production code, violating Power of 10 Rule 9.
 *   (c) std::abort() in both builds: explicitly prohibited by CLAUDE.md §10.1
 *       ("Does not call abort() or terminate the process" in production).
 *
 * std::atomic<bool> carve-out: permitted per CLAUDE.md §3 — std::atomic<T>
 * for integral types; no dynamic allocation, maps directly to a hardware
 * primitive, ISO C++17 standard, portable across all conforming toolchains.
 *
 * Thread safety: g_fatal_fired stores with memory_order_release; callers load
 * (via check_and_clear) with memory_order_acquire, ensuring visibility across
 * threads.
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#ifndef CORE_ASSERT_STATE_HPP
#define CORE_ASSERT_STATE_HPP

#include <atomic>   // std::atomic — permitted carve-out (CLAUDE.md §3)

namespace assert_state {

/// Latching flag set to true by NEVER_COMPILED_OUT_ASSERT in production builds
/// when an assertion condition is violated.  Remains true until explicitly
/// cleared by check_and_clear().
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern std::atomic<bool> g_fatal_fired;

/// Atomically read and clear the fatal-assertion flag.
///
/// Returns true if at least one NEVER_COMPILED_OUT_ASSERT has fired since the
/// last call.  Clears the flag atomically so subsequent calls return false
/// unless another assertion fires.
///
/// Thread-safe: uses compare_exchange_strong with acquire/release ordering.
bool check_and_clear() noexcept;

} // namespace assert_state

#endif // CORE_ASSERT_STATE_HPP
