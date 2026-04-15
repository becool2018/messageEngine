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
 * @file PosixLogClock.cpp
 * @brief POSIX implementation of ILogClock.
 *
 * Implements: REQ-7.1.1, REQ-7.2.1
 */

#include "platform/PosixLogClock.hpp"

// Microseconds per second — used in ts_to_us conversion.
static const uint64_t US_PER_SEC = 1000000ULL;
// Nanoseconds per microsecond — used in ts_to_us conversion.
static const uint64_t NS_PER_US  = 1000ULL;

// Helper: convert struct timespec to microseconds.
// Defined in the anonymous namespace to avoid external linkage (MISRA Dir 1.1).
namespace {
static uint64_t ts_to_us(const struct timespec& ts)
{
    // CERT INT30-C: validate before multiplication.
    // tv_sec is time_t; cast to uint64_t before multiply to avoid overflow.
    // tv_nsec is always in [0, 999999999] per POSIX — no overflow possible
    // after dividing by NS_PER_US.
    const uint64_t sec_us  = static_cast<uint64_t>(ts.tv_sec) * US_PER_SEC;
    const uint64_t nsec_us = static_cast<uint64_t>(ts.tv_nsec) / NS_PER_US;
    return sec_us + nsec_us;
}
} // namespace

// Power of 10: no dynamic allocation — function-local static.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PosixLogClock& PosixLogClock::instance()
{
    static PosixLogClock s_instance;  // Power of 10 Rule 3: no heap allocation
    return s_instance;
}

uint64_t PosixLogClock::now_wall_us() const
{
    struct timespec ts = {0, 0};
    const int rc = do_clock_gettime(CLOCK_REALTIME, &ts);
    if (rc != 0) {
        // clock_gettime failure: return 0 (callers handle 0 gracefully).
        return 0U;
    }
    return ts_to_us(ts);
}

uint64_t PosixLogClock::now_monotonic_us() const
{
    struct timespec ts = {0, 0};
    const int rc = do_clock_gettime(CLOCK_MONOTONIC, &ts);
    if (rc != 0) {
        // clock_gettime failure: return 0 (callers handle 0 gracefully).
        return 0U;
    }
    return ts_to_us(ts);
}

uint32_t PosixLogClock::thread_id() const
{
    // pthread_t is an opaque type (pointer on macOS, integer on Linux).
    // reinterpret_cast is required to extract a numeric value from the opaque
    // type. MISRA C++:2023 Rule 5.2.4 exception: converting a pointer-to-object
    // to an unsigned integer type for logging purposes (no memory access via the
    // resulting value). Lower 32 bits are sufficient for distinguishing threads
    // in log output (not security-sensitive). CERT INT31-C: narrowing is
    // intentional and documented.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const uintptr_t tid_val = reinterpret_cast<uintptr_t>(pthread_self());
    return static_cast<uint32_t>(tid_val);
}

int PosixLogClock::do_clock_gettime(clockid_t clk, struct timespec* ts) const
{
    // Thin POSIX wrapper — single line per NVI contract. No logic here.
    return ::clock_gettime(clk, ts);
}
