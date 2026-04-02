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
 * @file Logger.hpp
 * @brief Minimal, dependency-free logging facility.
 *
 * Writes to stderr with a severity prefix.  No heap allocation, no
 * function pointers, no STL.  Inline so it can be used from any layer.
 *
 * Rules applied:
 *   - Power of 10 rule 8: no macros for control flow.
 *   - F-Prime style: no exceptions, no STL.
 *   - MISRA Dir 4.9: prefer inline functions over macros.
 *
 * Implements: REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4
 */

#ifndef CORE_LOGGER_HPP
#define CORE_LOGGER_HPP

#include <cstdio>
#include <cstdarg>
#include "Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Maximum log-line length (fixed buffer; no dynamic allocation)
// ─────────────────────────────────────────────────────────────────────────────
static const int LOG_LINE_MAX = 512;

class Logger {
public:
    /// Write a formatted log line to stderr.
    ///
    /// __attribute__((format)) is a GCC/Clang extension used here as a
    /// documented, minimal deviation: it is a pure type-safety annotation
    /// (no control flow, no allocation), it tells the compiler that `fmt`
    /// is a printf-style format string so -Wformat-nonliteral is suppressed
    /// at the vsnprintf call site, and it enables compile-time format/argument
    /// checking at every call site.  This is more compliant, not less.
    // NOLINTNEXTLINE(readability-non-const-parameter)  -- va_list requires non-const
    static void log(Severity sev, const char* module, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)))
    {
        const char* tag = severity_tag(sev);
        char buf[LOG_LINE_MAX];

        // Power of 10: bounds-checked write, return value checked
        int hdr = snprintf(buf, static_cast<size_t>(LOG_LINE_MAX),
                           "[%s][%s] ", tag, module);
        if (hdr < 0) { return; }   // snprintf error; fail silently

        va_list args;
        va_start(args, fmt);
        // Power of 10: bounded vsnprintf
        int body = vsnprintf(buf + hdr,
                             static_cast<size_t>(LOG_LINE_MAX - hdr),
                             fmt, args);
        va_end(args);
        if (body < 0) { return; }

        (void)fprintf(stderr, "%s\n", buf);
    }

private:
    Logger() {}  // not instantiable

    static const char* severity_tag(Severity sev)
    {
        switch (sev) {
            case Severity::INFO:       return "INFO    ";
            case Severity::WARNING_LO: return "WARN_LO ";
            case Severity::WARNING_HI: return "WARN_HI ";
            case Severity::FATAL:      return "FATAL   ";
            default:                   return "UNKNOWN ";
        }
    }
};

#endif // CORE_LOGGER_HPP
