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
 *   Production (NDEBUG): logs FATAL with condition + file + line,
 *                    then returns without aborting — allows controlled
 *                    component-level recovery by the caller.
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
#include <cstdlib>

// MISRA C++:2023 deviation: macro required to capture __FILE__ and __LINE__
// at the call site. No equivalent language construct exists in C++17.
// do-while(false) idiom prevents dangling-else and double-evaluation issues.
#ifdef NDEBUG
// Production build: log FATAL, do not abort — allow controlled recovery.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define NEVER_COMPILED_OUT_ASSERT(cond)                                           \
    do {                                                                           \
        if (!(cond)) { /* NOLINT(readability-simplify-boolean-expr) */            \
            Logger::log(Severity::FATAL, "Assert",                                \
                        "Assertion failed: (%s) at %s:%d",                        \
                        #cond, __FILE__, static_cast<int>(__LINE__));             \
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
