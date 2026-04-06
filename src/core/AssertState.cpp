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
 * @file AssertState.cpp
 * @brief Assertion-fired flag, IResetHandler registration, and check_and_clear().
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// Implements: REQ-7.2.4
// NSC-infrastructure: CLAUDE.md §10 assertion policy

#include "core/AssertState.hpp"
#include <cstdlib>  // ::abort()

namespace assert_state {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_fatal_fired{false};

// REQ-7.2.4: monotonically increasing fatal event count.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<uint32_t> g_fatal_count{0U};

// Power of 10 Rule 3: static storage, pointer to caller-owned object; no heap.
// std::atomic permitted per project carve-out (CLAUDE.md §3, §4 std::atomic exception).
// M-3: s_handler written by set_reset_handler() from any thread; atomic eliminates the
//      data race.  Release/acquire ordering gives the loading thread a happens-before
//      edge across the store, matching the pattern already used for g_fatal_fired.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::atomic<IResetHandler*> s_handler{nullptr};

void set_reset_handler(IResetHandler& handler) noexcept
{
    // std::atomic permitted per project carve-out (CLAUDE.md §3, §4 std::atomic exception).
    s_handler.store(&handler, std::memory_order_release);
}

IResetHandler* get_reset_handler() noexcept
{
    // std::atomic permitted per project carve-out (CLAUDE.md §3, §4 std::atomic exception).
    return s_handler.load(std::memory_order_acquire);
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
    // std::atomic permitted per project carve-out (CLAUDE.md §3, §4 std::atomic exception).
    IResetHandler* h = s_handler.load(std::memory_order_acquire);
    if (h != nullptr) {
        h->on_fatal_assert(cond, file, line);
    } else {
        ::abort();
    }
    // If handler returned (soft-reset path), set the flag and bump the count.
    g_fatal_fired.store(true, std::memory_order_release);
    // REQ-7.2.4: increment fatal event counter (relaxed: no ordering needed here)
    (void)g_fatal_count.fetch_add(1U, std::memory_order_relaxed);
}

uint32_t get_fatal_count() noexcept
{
    return g_fatal_count.load(std::memory_order_acquire);
}

} // namespace assert_state
