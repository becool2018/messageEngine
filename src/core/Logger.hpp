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
 * @brief Logger facility with injectable clock and sink interfaces.
 *
 * Provides structured log output with monotonic timestamps, wall-clock
 * variants, PID, TID, severity, module, file, and line metadata. All POSIX
 * calls are confined to src/platform (ILogClock, ILogSink implementations).
 * Logger.hpp includes no POSIX headers.
 *
 * Usage — preferred: use the LOG_* macros which inject file/line automatically.
 *
 *   LOG_INFO("MyModule",   "connected peer=%u slot=%u", peer_id, slot);
 *   LOG_WARN_HI("AckTracker", "ack timeout mid=0x%08x retry=%u", msg.id, n);
 *   LOG_FATAL("Transport", "unrecoverable error");
 *   LOG_INFO_WALL("TcpBackend", "session established peer=%u", id);
 *
 * Message-context convention (keeps output grep-friendly):
 *   Embed message-specific fields using short key=value pairs in the format
 *   string. Examples:
 *     LOG_INFO("DeliveryEngine", "send ok mid=0x%08x peer=%u chan=%u", ...);
 *     LOG_WARN_HI("AckTracker",  "ack timeout mid=0x%08x retry=%u", ...);
 *     LOG_INFO("TcpBackend",     "connected peer=%u slot=%u", ...);
 *   Cross-component correlation: grep 'mid=0x000012ab' traces a single message
 *   through the full send→impair→receive pipeline across all modules.
 *
 * Standard log format (LOG_INFO, LOG_WARN_*, LOG_FATAL):
 *   [mono_sec.mono_us][pid][severity][tid][module][func:line] message
 *
 * Wall variant format (LOG_INFO_WALL, LOG_WARN_*_WALL, LOG_FATAL_WALL):
 *   [wall_sec.wall_us][mono_sec.mono_us][pid][severity][tid][module][func:line] message
 *
 * Rules applied:
 *   - Power of 10 rule 8: macros expand to a single function call, no control flow.
 *     ##__VA_ARGS__ extension documented below.
 *   - Power of 10 Rule 9: virtual dispatch via ILogSink/ILogClock vtables —
 *     the approved mechanism. No explicit function pointers.
 *   - F-Prime style: no exceptions, no STL.
 *   - Layering: no POSIX headers included; all POSIX calls in src/platform.
 *
 * Implements: REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4
 *
 * SEC-019: rate limiting deferred — tracked for future implementation.
 * Requires a fixed-size ring buffer, per-tag counters, and monotonic timestamps
 * to suppress burst log floods without dynamic allocation. Deferred because it
 * requires significant new infrastructure that is out of scope for this series.
 */

#ifndef CORE_LOGGER_HPP
#define CORE_LOGGER_HPP

#include <cstdarg>
#include "Types.hpp"
#include "ILogClock.hpp"
#include "ILogSink.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Buffer size constants (fixed stack allocation; no dynamic allocation).
// 512U matches the old LOG_LINE_MAX — no net stack increase from prior code.
// Any change requires a STACK_ANALYSIS.md update per CLAUDE.md §15.
// ─────────────────────────────────────────────────────────────────────────────

class Logger {
public:
    // ─────────────────────────────────────────────────────────────────────
    // Buffer size constants (static constexpr; shared with Logger.cpp via
    // the class definition — no linker ODR risk).
    // ─────────────────────────────────────────────────────────────────────

    /// Total stack-allocated format buffer: header + caller message body.
    /// 512U matches the existing LOG_LINE_MAX — no net stack increase.
    static constexpr uint32_t LOG_FORMAT_BUF_SIZE = 512U;

    /// Standard header reservation (mono ts + pid + severity + tid + module +
    /// file:line + delimiters). Measured worst case ~100 bytes; 120U provides
    /// headroom for long module/file names.
    static constexpr uint32_t LOG_HEADER_MAX_SIZE = 120U;

    /// Wall-variant header adds one wall-clock field (~20 bytes worst case).
    static constexpr uint32_t LOG_HEADER_WALL_MAX_SIZE = 140U;

    /// Minimum space guaranteed for the caller's message body (standard).
    static constexpr uint32_t LOG_MSG_MAX_SIZE = LOG_FORMAT_BUF_SIZE - LOG_HEADER_MAX_SIZE;

    /// Minimum space guaranteed for the caller's message body (wall variant).
    static constexpr uint32_t LOG_MSG_WALL_MAX_SIZE = LOG_FORMAT_BUF_SIZE - LOG_HEADER_WALL_MAX_SIZE;

    // ─────────────────────────────────────────────────────────────────────
    // Initialization
    // ─────────────────────────────────────────────────────────────────────

    /// Initialize the logger. Must be called once before any log() or log_wall()
    /// call. Stores clock, sink, and min_level; captures getpid() into s_pid.
    /// Returns ERR_INVALID if clock or sink is nullptr.
    ///
    /// SECURITY: clock and sink must be trusted, application-controlled
    /// components. Logger performs no integrity verification of the supplied
    /// implementations. A malicious or compromised sink could exfiltrate all
    /// log output; a malicious clock could corrupt timestamps. Never accept
    /// clock or sink pointers from untrusted input (network data, plugins,
    /// or dynamically loaded modules without integrity verification).
    static Result init(Severity min_level, ILogClock* clock, ILogSink* sink);

    // ─────────────────────────────────────────────────────────────────────
    // Logging functions (called via LOG_* macros; not directly in src/)
    // ─────────────────────────────────────────────────────────────────────

    /// Emit a structured log line with monotonic timestamp.
    /// Format: [mono_sec.mono_us][pid][sev][tid][module][func:line] message
    ///
    /// __attribute__((format)) is a GCC/Clang extension used here as a
    /// documented, minimal deviation: it enables compile-time format/argument
    /// checking at every call site and suppresses -Wformat-nonliteral at the
    /// vsnprintf call site. More compliant, not less.
    // NOLINTNEXTLINE(readability-non-const-parameter)  -- va_list requires non-const
    static void log(Severity sev,
                    const char* func,
                    int line,
                    const char* module,
                    const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

    /// Emit a structured log line with wall clock + monotonic timestamp.
    /// Format: [wall_sec.wall_us][mono_sec.mono_us][pid][sev][tid][module][func:line] message
    // NOLINTNEXTLINE(readability-non-const-parameter)  -- va_list requires non-const
    static void log_wall(Severity sev,
                         const char* func,
                         int line,
                         const char* module,
                         const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

private:
    Logger() {}  // not instantiable

    static const char* severity_tag(Severity sev);
};

// ─────────────────────────────────────────────────────────────────────────────
// LOG_* macros — metadata-injecting wrappers for Logger::log / Logger::log_wall.
//
// __func__ (ISO C++17 §9.2.3) is a predefined string literal holding the
// undecorated name of the enclosing function. Zero runtime cost — it is a
// static const char[] initialized by the compiler. No MISRA deviation needed.
//
// The func field is formatted as %.15s in Logger::log/log_wall — truncated to
// 15 characters at format time with no padding for shorter names. This keeps
// log lines predictably bounded without any caller-side allocation or copying.
//
// MISRA C++:2023 Rule 19.3.4 deviation: ##__VA_ARGS__ is a GCC/Clang extension
// used solely to suppress the trailing comma when no variadic arguments are
// supplied. No ISO C++17 standard mechanism achieves the same effect without
// adding a dummy argument or a second overload macro. This project targets
// GCC/Clang on POSIX only; portability to non-GCC toolchains is not required.
// Reviewed and accepted by project moderator per deviation policy.
//
// Standard macros — monotonic timestamp only in header.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_INFO(mod, fmt, ...)         Logger::log(Severity::INFO,       __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_WARN_LO(mod, fmt, ...)      Logger::log(Severity::WARNING_LO, __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_WARN_HI(mod, fmt, ...)      Logger::log(Severity::WARNING_HI, __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_FATAL(mod, fmt, ...)        Logger::log(Severity::FATAL,      __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)

// Wall-clock variants — wall timestamp prepended before monotonic.
// Use when absolute time correlation with external systems is needed
// (e.g., connection establishment, partition events, fatal errors).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_INFO_WALL(mod, fmt, ...)    Logger::log_wall(Severity::INFO,       __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_WARN_LO_WALL(mod, fmt, ...) Logger::log_wall(Severity::WARNING_LO, __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_WARN_HI_WALL(mod, fmt, ...) Logger::log_wall(Severity::WARNING_HI, __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LOG_FATAL_WALL(mod, fmt, ...)   Logger::log_wall(Severity::FATAL,      __func__, __LINE__, (mod), (fmt), ##__VA_ARGS__)

#endif // CORE_LOGGER_HPP
