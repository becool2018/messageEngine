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
 * @file IResetHandler.hpp
 * @brief Pure-virtual interface for injectable assertion-failure reset handlers.
 *
 * Design rationale:
 *   NEVER_COMPILED_OUT_ASSERT needs to trigger a "controlled component reset"
 *   in production builds (CLAUDE.md §10.1) without storing a function pointer
 *   (prohibited by Power of 10 Rule 9).  IResetHandler uses virtual dispatch
 *   (vtable) — the documented Rule 9 exception for polymorphism
 *   (CLAUDE.md §2 Rule 9 exception).
 *
 *   Concrete implementations:
 *     AbortResetHandler  — POSIX host: calls ::abort().  Honest: on a hosted
 *                          library there is no meaningful "component reset"
 *                          without longjmp/exceptions; aborting is correct.
 *     EmbeddedResetHandler (future) — embedded target: calls the MCU reset
 *                          API (e.g. NVIC_SystemReset() on Cortex-M), which
 *                          is [[noreturn]] and resets only the affected
 *                          component or the whole MCU depending on the target.
 *
 * Power of 10 Rule 9 exception: virtual dispatch (vtable) is the
 *   documented permitted exception (CLAUDE.md §2, Rule 9 exception).
 *   No explicit function pointer declarations appear in application code.
 *
 * NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#ifndef CORE_IRESET_HANDLER_HPP
#define CORE_IRESET_HANDLER_HPP

/// Abstract interface for controlled-reset behavior on assertion failure.
///
/// Register a concrete instance via assert_state::set_reset_handler() during
/// system initialisation.  The macro NEVER_COMPILED_OUT_ASSERT calls
/// on_fatal_assert() when an assertion condition is violated in a production
/// (NDEBUG) build, before setting the g_fatal_fired flag.
///
/// Thread safety: on_fatal_assert() may be called from any thread.
/// Implementations must be thread-safe.
class IResetHandler {
public:
    virtual ~IResetHandler() {}

    /// Invoked by NEVER_COMPILED_OUT_ASSERT immediately after logging FATAL.
    ///
    /// @param cond  Stringified assertion condition that evaluated to false.
    /// @param file  Source file name (__FILE__).
    /// @param line  Source line number (__LINE__).
    ///
    /// Implementations may:
    ///   - Call ::abort() (POSIX host — AbortResetHandler).
    ///   - Call a hardware reset API (embedded — [[noreturn]]).
    ///   - Record the failure and return (embedded soft-reset; caller must
    ///     detect g_fatal_fired and re-initialise the component).
    ///
    // Power of 10 Rule 9 exception: virtual dispatch via vtable.
    virtual void on_fatal_assert(const char* cond,
                                 const char* file,
                                 int         line) = 0;
};

#endif // CORE_IRESET_HANDLER_HPP
