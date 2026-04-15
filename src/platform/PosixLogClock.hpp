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
 * @file PosixLogClock.hpp
 * @brief POSIX implementation of ILogClock.
 *
 * Uses the Non-Virtual Interface (NVI) idiom to expose a protected virtual
 * seam (do_clock_gettime) that test subclasses can override to inject
 * clock_gettime failures without modifying production code.
 *
 * Note: 'final' is intentionally omitted to allow the NVI test seam subclass
 * (FaultPosixLogClock) to override do_clock_gettime(). This is a deliberate
 * deviation from the preferred 'final' pattern; documented here per deviation
 * policy.
 *
 * Power of 10 Rule 9 exception: vtable-backed virtual dispatch via ILogClock
 * and the NVI do_clock_gettime() seam — no explicit function pointers.
 *
 * Implements: REQ-7.1.1, REQ-7.2.1
 */

#ifndef PLATFORM_POSIXLOGCLOCK_HPP
#define PLATFORM_POSIXLOGCLOCK_HPP

#include <cstdint>
#include <time.h>     // clock_gettime, clockid_t, struct timespec
#include <pthread.h>  // pthread_self
#include "core/ILogClock.hpp"

// 'final' omitted: NVI seam requires subclassing for fault injection
// (FaultPosixLogClock in tests/). Documented deviation from preferred 'final'.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class PosixLogClock : public ILogClock {
public:
    // Virtual destructor inherited from ILogClock (MISRA C++:2023 Rule 15.5.2).
    ~PosixLogClock() override = default;

    /// Returns CLOCK_REALTIME in microseconds; 0 on clock_gettime failure.
    uint64_t now_wall_us() const override;

    /// Returns CLOCK_MONOTONIC in microseconds; 0 on clock_gettime failure.
    uint64_t now_monotonic_us() const override;

    /// Returns a uint32_t derived from pthread_self().
    uint32_t thread_id() const override;

    /// Singleton accessor — returns the one shared instance.
    static PosixLogClock& instance();

protected:
    // NVI seam: default implementation calls ::clock_gettime(clk, ts).
    // Overridden in FaultPosixLogClock (tests/) to inject clock_gettime failure.
    // Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
    virtual int do_clock_gettime(clockid_t clk, struct timespec* ts) const;

protected:
    // Protected constructor: not instantiable by external clients (use instance()).
    // Protected (not private) to allow NVI test-seam subclasses (FaultPosixLogClock)
    // to construct without going through the singleton accessor.
    PosixLogClock() = default;
};

#endif // PLATFORM_POSIXLOGCLOCK_HPP
