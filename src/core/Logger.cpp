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
 * @file Logger.cpp
 * @brief Logger facility implementation with injectable clock and sink.
 *
 * Static member definitions, init(), log(), log_wall(), and severity_tag().
 *
 * Stack impact: Logger::log() owns a 512-byte stack buffer (same size as the
 * prior inline buffer, but now in Logger's own frame rather than the caller's).
 * Call chain: caller → Logger::log() [buf[512] + ~50 B locals]
 *             → s_clock->now_monotonic_us() [PosixLogClock: ~16 B]
 *             → s_sink->write() [PosixLogSink: ~16 B]
 *             → do_write() [wraps ::write(2): ~16 B]
 * This chain adds 3 frames versus the prior inline-only path. See
 * docs/STACK_ANALYSIS.md for the updated worst-case analysis.
 *
 * Implements: REQ-7.1.1, REQ-7.1.2, REQ-7.1.3, REQ-7.1.4
 */

#include "core/Logger.hpp"
#include "core/Assert.hpp"  // NEVER_COMPILED_OUT_ASSERT
#include <atomic>           // std::atomic — permitted carve-out (.claude/CLAUDE.md §2.3)
#include <cstdio>           // snprintf, vsnprintf
#include <unistd.h>         // getpid()

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time invariants for buffer constants.
// These assertions are placed in the .cpp to avoid duplicate definitions from
// multiple TU inclusions of the header (ODR safety for constexpr static members
// referenced by value-requiring contexts is fine in C++17, but static_assert
// in the .cpp is cleaner and avoids any concern).
// ─────────────────────────────────────────────────────────────────────────────
static_assert(Logger::LOG_HEADER_MAX_SIZE      < Logger::LOG_FORMAT_BUF_SIZE,
              "Header reservation must be smaller than total buffer");
static_assert(Logger::LOG_HEADER_WALL_MAX_SIZE < Logger::LOG_FORMAT_BUF_SIZE,
              "Wall header reservation must be smaller than total buffer");
static_assert(Logger::LOG_MSG_MAX_SIZE         >= 256U,
              "Must leave at least 256 bytes for message body");      // 392U
static_assert(Logger::LOG_MSG_WALL_MAX_SIZE    >= 256U,
              "Must leave at least 256 bytes for wall message body"); // 372U

// ─────────────────────────────────────────────────────────────────────────────
// Static member definitions (single definition per ODR; cannot be in header).
// Power of 10 Rule 3: no dynamic allocation — all pointers are set once at
// init() time and remain valid for program lifetime.
// ─────────────────────────────────────────────────────────────────────────────
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// s_min_level is std::atomic so concurrent log() calls racing against init()
// on a multi-threaded init path see a consistent severity threshold without
// UB. std::atomic<Severity> is explicitly permitted (CLAUDE.md §1d /
// .claude/CLAUDE.md §2.3 carve-out). memory_order_relaxed is sufficient:
// no happens-before relationship with other data is required for a severity
// filter — a brief window of stale level is acceptable.
static std::atomic<Severity> s_min_level{Severity::INFO};
static ILogClock* s_clock     = nullptr;
static ILogSink*  s_sink      = nullptr;
static uint32_t   s_pid       = 0U;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ─────────────────────────────────────────────────────────────────────────────
// Microseconds per second — used for timestamp formatting.
// ─────────────────────────────────────────────────────────────────────────────
static const uint64_t LOG_US_PER_SEC = 1000000ULL;

Result Logger::init(Severity min_level, ILogClock* clock, ILogSink* sink)
{
    // Precondition checks: both pointers must be non-null.
    if (clock == nullptr) {
        return Result::ERR_INVALID;
    }
    if (sink == nullptr) {
        return Result::ERR_INVALID;
    }
    s_min_level.store(min_level, std::memory_order_relaxed);
    s_clock     = clock;
    s_sink      = sink;
    // Capture PID once at init time — zero per-call overhead.
    // CERT INT31-C: pid_t is signed; cast to uint32_t is safe because
    // valid PIDs are always positive and well within uint32_t range on
    // POSIX platforms (typically limited to 32768 or 4194304).
    s_pid = static_cast<uint32_t>(::getpid());
    return Result::OK;
}

// Logger::log() appears to call itself via NEVER_COMPILED_OUT_ASSERT → Logger::log(),
// but only when an assertion fails (precondition violated). In that case the second
// Logger::log() call uses compile-time __FILE__/__LINE__ and a literal format string,
// so it cannot trigger the same precondition assertion again. Not a true runtime
// recursion; one-level diagnostic path. NOLINT applied to function definition line.
void Logger::log(Severity sev,  // NOLINT(misc-no-recursion)
                 const char* file,
                 int line,
                 const char* module,
                 const char* fmt, ...)
{
    // Power of 10 Rule 5: precondition assertions (≥2 per function).
    NEVER_COMPILED_OUT_ASSERT(fmt    != nullptr);
    NEVER_COMPILED_OUT_ASSERT(file   != nullptr);
    NEVER_COMPILED_OUT_ASSERT(line   > 0);
    NEVER_COMPILED_OUT_ASSERT(s_clock != nullptr);
    NEVER_COMPILED_OUT_ASSERT(s_sink  != nullptr);

    // Severity filter: skip below-threshold messages.
    if (sev < s_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    const uint64_t mono_us  = s_clock->now_monotonic_us();
    const uint32_t tid      = s_clock->thread_id();
    const char*    tag      = severity_tag(sev);

    // Decompose monotonic timestamp into seconds + fractional microseconds.
    const uint64_t mono_sec = mono_us / LOG_US_PER_SEC;
    const uint64_t mono_frac = mono_us % LOG_US_PER_SEC;

    // Stack-allocated fixed buffer — Power of 10 Rule 3: no heap.
    // 512U matches the former LOG_LINE_MAX; no net stack increase.
    char buf[LOG_FORMAT_BUF_SIZE];

    // Build header: [mono_sec.mono_frac][pid][sev][tid][module][file:line]
    // F-1 / CERT INT31-C: guard both error (< 0) and truncation (>= buf size).
    const int hdr = snprintf(buf,
                             static_cast<size_t>(LOG_FORMAT_BUF_SIZE),
                             "[%07llu.%06llu][%u][%s][%u][%s][%s:%d] ",
                             static_cast<unsigned long long>(mono_sec),
                             static_cast<unsigned long long>(mono_frac),
                             s_pid,
                             tag,
                             tid,
                             module,
                             file,
                             line);
    // Power of 10 R7: check snprintf return value.
    if (hdr < 0 || static_cast<uint32_t>(hdr) >= LOG_FORMAT_BUF_SIZE) {
        return;  // header truncated or error — drop silently
    }

    va_list args;
    va_start(args, fmt);
    // Power of 10: bounded vsnprintf.
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) — false positive:
    // va_start() is called two lines above. The clang-analyzer loses track of
    // initialization when it symbolically follows the NEVER_COMPILED_OUT_ASSERT
    // macro's recursive Logger::log() call path in the outer frame.
    const int body = vsnprintf(buf + hdr,
                               static_cast<size_t>(LOG_FORMAT_BUF_SIZE
                                   - static_cast<uint32_t>(hdr)),
                               fmt, args);
    va_end(args);

    // body < 0: vsnprintf encoding error. Architecturally unreachable when
    // all callers use LOG_* macros with compile-time-validated format strings
    // (see docs/COVERAGE_CEILINGS.md — T-2.10 ceiling argument, category d-iii).
    if (body < 0) { return; }

    // Append newline. buf is guaranteed to be NUL-terminated by vsnprintf.
    // Compute total length and write in one call.
    // Find the end of the formatted string by hdr + body (clamped to buf).
    const uint32_t total = static_cast<uint32_t>(hdr)
                         + static_cast<uint32_t>(body);
    // Clamp total to avoid writing past buffer end (vsnprintf truncates body).
    const uint32_t write_len = (total < LOG_FORMAT_BUF_SIZE - 1U)
                                   ? total
                                   : LOG_FORMAT_BUF_SIZE - 2U;
    buf[write_len]     = '\n';
    buf[write_len + 1U] = '\0';

    s_sink->write(buf, write_len + 1U);
}

// Same rationale as Logger::log() above — not a true recursion.
void Logger::log_wall(Severity sev,  // NOLINT(misc-no-recursion)
                      const char* file,
                      int line,
                      const char* module,
                      const char* fmt, ...)
{
    // Power of 10 Rule 5: precondition assertions (≥2 per function).
    NEVER_COMPILED_OUT_ASSERT(fmt    != nullptr);
    NEVER_COMPILED_OUT_ASSERT(file   != nullptr);
    NEVER_COMPILED_OUT_ASSERT(line   > 0);
    NEVER_COMPILED_OUT_ASSERT(s_clock != nullptr);
    NEVER_COMPILED_OUT_ASSERT(s_sink  != nullptr);

    // Severity filter.
    if (sev < s_min_level.load(std::memory_order_relaxed)) {
        return;
    }

    const uint64_t wall_us  = s_clock->now_wall_us();
    const uint64_t mono_us  = s_clock->now_monotonic_us();
    const uint32_t tid      = s_clock->thread_id();
    const char*    tag      = severity_tag(sev);

    // Decompose timestamps.
    const uint64_t wall_sec  = wall_us  / LOG_US_PER_SEC;
    const uint64_t wall_frac = wall_us  % LOG_US_PER_SEC;
    const uint64_t mono_sec  = mono_us  / LOG_US_PER_SEC;
    const uint64_t mono_frac = mono_us  % LOG_US_PER_SEC;

    char buf[LOG_FORMAT_BUF_SIZE];

    // Build header: [wall_sec.wall_frac][mono_sec.mono_frac][pid][sev][tid][module][file:line]
    const int hdr = snprintf(buf,
                             static_cast<size_t>(LOG_FORMAT_BUF_SIZE),
                             "[%llu.%06llu][%07llu.%06llu][%u][%s][%u][%s][%s:%d] ",
                             static_cast<unsigned long long>(wall_sec),
                             static_cast<unsigned long long>(wall_frac),
                             static_cast<unsigned long long>(mono_sec),
                             static_cast<unsigned long long>(mono_frac),
                             s_pid,
                             tag,
                             tid,
                             module,
                             file,
                             line);
    if (hdr < 0 || static_cast<uint32_t>(hdr) >= LOG_FORMAT_BUF_SIZE) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) — false positive:
    // va_start() is called two lines above. Same analyzer confusion as log().
    const int body = vsnprintf(buf + hdr,
                               static_cast<size_t>(LOG_FORMAT_BUF_SIZE
                                   - static_cast<uint32_t>(hdr)),
                               fmt, args);
    va_end(args);

    if (body < 0) { return; }

    const uint32_t total = static_cast<uint32_t>(hdr)
                         + static_cast<uint32_t>(body);
    const uint32_t write_len = (total < LOG_FORMAT_BUF_SIZE - 1U)
                                   ? total
                                   : LOG_FORMAT_BUF_SIZE - 2U;
    buf[write_len]      = '\n';
    buf[write_len + 1U] = '\0';

    s_sink->write(buf, write_len + 1U);
}

const char* Logger::severity_tag(Severity sev)
{
    switch (sev) {
        case Severity::INFO:       return "INFO    ";
        case Severity::WARNING_LO: return "WARN_LO ";
        case Severity::WARNING_HI: return "WARN_HI ";
        case Severity::FATAL:      return "FATAL   ";
        default:                   return "UNKNOWN ";
    }
}
