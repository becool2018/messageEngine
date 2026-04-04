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
 * @file Assert.hpp
 * @brief NEVER_COMPILED_OUT_ASSERT: a safety-critical assertion macro that is
 *        never disabled by NDEBUG.
 *
 * Requirement traceability: CLAUDE.md §8 (assertions and defensive programming),
 * Power of 10 Rule 5 (assertion density — assertions must remain active in all
 * builds; NDEBUG-disabled assertions violate this rule).
 *
 * MISRA C++:2023 deviation — macros are used here because __FILE__ and __LINE__
 * cannot be captured by any other language construct in C++17. This is a
 * documented, minimal deviation per CLAUDE.md §3.
 *
 * Behavior by build type:
 *   Debug (!NDEBUG): logs condition + file + line at FATAL severity,
 *                    then calls ::abort() for immediate crash and stack trace.
 *   Production (NDEBUG): logs FATAL with condition + file + line, then calls
 *                    the registered IResetHandler::on_fatal_assert() via
 *                    virtual dispatch (Power of 10 Rule 9 vtable exception).
 *                    If no handler has been registered, falls back to
 *                    ::abort() — identical to the debug path.
 *                    If the handler returns (embedded soft-reset), sets
 *                    g_fatal_fired so callers can detect and re-init the
 *                    component.
 *
 * Handler registration: call assert_state::set_reset_handler(handler) once
 *   during system initialisation.  For POSIX builds, register
 *   AbortResetHandler::instance().  For embedded, register a target-specific
 *   IResetHandler that calls the MCU reset API.
 *
 * The condition expression is ALWAYS evaluated regardless of build type.
 *
 * NSC-infrastructure: no REQ-x.x requirement applies — this file implements
 * the CLAUDE.md §10 coding standard requirement (NEVER_COMPILED_OUT_ASSERT),
 * not an application-level REQ. Excluded from per-file Implements check.
 */
// NSC-infrastructure: CLAUDE.md §10 assertion policy; no REQ-x.x applies

#ifndef CORE_ASSERT_HPP
#define CORE_ASSERT_HPP

#include "Logger.hpp"
#include "AssertState.hpp"  // g_fatal_fired, get_reset_handler()
#include <cstdlib>          // ::abort() — fallback when no handler registered

// MISRA C++:2023 deviation: macro required to capture __FILE__ and __LINE__
// at the call site. No equivalent language construct exists in C++17.
// do-while(false) idiom prevents dangling-else and double-evaluation issues.
#ifdef NDEBUG
// Production build: log FATAL, call registered IResetHandler via vtable,
// fall back to ::abort() if no handler, set g_fatal_fired if handler returns.
// Power of 10 Rule 9 exception: virtual dispatch via IResetHandler vtable.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NEVER_COMPILED_OUT_ASSERT(cond)                                           \
    do {                                                                           \
        if (!(cond)) { /* NOLINT(readability-simplify-boolean-expr) */            \
            Logger::log(Severity::FATAL, "Assert",                                \
                        "Assertion failed: (%s) at %s:%d",                        \
                        #cond, __FILE__, static_cast<int>(__LINE__));             \
            IResetHandler* h_ = assert_state::get_reset_handler();                \
            if (h_ != nullptr) {                                                   \
                h_->on_fatal_assert(#cond, __FILE__,                              \
                                    static_cast<int>(__LINE__));                  \
            } else {                                                               \
                ::abort(); /* no handler: abort is safest fallback */             \
            }                                                                      \
            /* Reached only if handler returned (embedded soft-reset path). */    \
            assert_state::g_fatal_fired.store(true,                               \
                std::memory_order_release);                                        \
            (void)assert_state::g_fatal_count.fetch_add(1U,                       \
                std::memory_order_relaxed);                                        \
        }                                                                          \
    } while (false)
#else
// Debug/test build: log FATAL then abort() for immediate stack trace.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NEVER_COMPILED_OUT_ASSERT(cond)                                           \
    do {                                                                           \
        if (!(cond)) { /* NOLINT(readability-simplify-boolean-expr) */            \
            Logger::log(Severity::FATAL, "Assert",                                \
                        "Assertion failed: (%s) at %s:%d",                        \
                        #cond, __FILE__, static_cast<int>(__LINE__));             \
            ::abort();                                                             \
        }                                                                          \
    } while (false)
#endif

#endif // CORE_ASSERT_HPP
