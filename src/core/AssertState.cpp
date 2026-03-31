/**
 * @file AssertState.cpp
 * @brief Assertion-fired flag, IResetHandler registration, and check_and_clear().
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#include "core/AssertState.hpp"
#include <cstdlib>  // ::abort()

namespace assert_state {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_fatal_fired{false};

// Power of 10 Rule 3: static storage, pointer to caller-owned object; no heap.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static IResetHandler* s_handler = nullptr;

void set_reset_handler(IResetHandler& handler) noexcept
{
    s_handler = &handler;
}

IResetHandler* get_reset_handler() noexcept
{
    return s_handler;
}

bool check_and_clear() noexcept
{
    // compare_exchange_strong: if g_fatal_fired is currently true, atomically
    // replace it with false and return true.  Uses acq_rel / acquire ordering
    // for full visibility across threads.
    bool expected = true;
    return g_fatal_fired.compare_exchange_strong(
        expected, false,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

void trigger_handler_for_test(const char* cond,
                               const char* file,
                               int         line) noexcept
{
    if (s_handler != nullptr) {
        s_handler->on_fatal_assert(cond, file, line);
    } else {
        ::abort();
    }
    // If handler returned (soft-reset path), set the flag.
    g_fatal_fired.store(true, std::memory_order_release);
}

} // namespace assert_state
