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
 * @file AbortResetHandler.hpp
 * @brief Concrete IResetHandler for POSIX-hosted builds: calls ::abort().
 *
 * Rationale:
 *   On a hosted POSIX library without longjmp (Power of 10 Rule 1) or
 *   exceptions (-fno-exceptions), there is no mechanism to unwind the call
 *   stack after a violated precondition without terminating the process.
 *   ::abort() is therefore the correct and honest "reset" for this platform.
 *   It produces a SIGABRT, a core dump (on most systems), and a full stack
 *   trace — the best possible diagnostic for a safety assertion failure.
 *
 *   For an embedded port, replace this with a target-specific handler that
 *   calls the MCU reset API (e.g. NVIC_SystemReset() on Cortex-M).  The
 *   on_fatal_assert() method should be marked [[noreturn]] on targets where
 *   the reset API is unconditionally non-returning.
 *
 * Registration:
 *   Call assert_state::set_reset_handler(AbortResetHandler::instance())
 *   once during system initialisation (before any other component is used).
 *   AbortResetHandler is a stateless singleton backed by static storage
 *   (Power of 10 Rule 3: no heap allocation).
 *
 * Power of 10 Rule 9 exception: virtual dispatch via vtable per CLAUDE.md §2.
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#ifndef CORE_ABORT_RESET_HANDLER_HPP
#define CORE_ABORT_RESET_HANDLER_HPP

#include "core/IResetHandler.hpp"
#include <cstdlib>  // ::abort()

/// POSIX-host concrete reset handler: terminates via ::abort().
///
/// Stateless singleton; backed by static storage (Power of 10 Rule 3).
class AbortResetHandler : public IResetHandler {
public:
    ~AbortResetHandler() override {}

    /// Return the process-wide singleton instance.
    static AbortResetHandler& instance() { return s_instance; }

    /// Terminate the process immediately via ::abort().
    ///
    /// On POSIX this raises SIGABRT, writes a core dump (if enabled), and
    /// produces a stack trace in the debugger.  This is the correct behavior
    /// for a hosted library with no hardware reset API.
    ///
    // Power of 10 Rule 9 exception: virtual dispatch via vtable.
    void on_fatal_assert(const char* /*cond*/,
                         const char* /*file*/,
                         int         /*line*/) override
    {
        ::abort();
    }

private:
    AbortResetHandler() = default;
    // Power of 10 Rule 3: static storage, no heap allocation.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static AbortResetHandler s_instance;
};

// Out-of-line definition of static member (required in C++17 for ODR).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline AbortResetHandler AbortResetHandler::s_instance{};

#endif // CORE_ABORT_RESET_HANDLER_HPP
