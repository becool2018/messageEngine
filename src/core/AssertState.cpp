/**
 * @file AssertState.cpp
 * @brief Definition of the global assertion-fired flag and check_and_clear().
 *
 * The flag is set (to true) by NEVER_COMPILED_OUT_ASSERT in production builds
 * when an assertion condition is violated.  Callers query it via
 * assert_state::check_and_clear() to detect and respond to component-level
 * failures without process abort.
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#include "core/AssertState.hpp"

namespace assert_state {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_fatal_fired{false};

bool check_and_clear() noexcept
{
    // compare_exchange_strong: if g_fatal_fired is currently true, atomically
    // replace it with false and return true.  If false, leave it unchanged and
    // return false.  Uses acq_rel on success, acquire on failure for full
    // visibility guarantees across threads.
    bool expected = true;
    return g_fatal_fired.compare_exchange_strong(
        expected, false,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

} // namespace assert_state
