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
 * @file ILogClock.hpp
 * @brief Abstract clock and thread-ID interface for the Logger facility.
 *
 * Logger calls clock->now_monotonic_us() and clock->thread_id() instead of
 * calling clock_gettime or pthread_self directly, keeping all POSIX time and
 * thread calls out of src/core. Concrete implementations live in src/platform
 * (e.g., PosixLogClock). Test doubles live in tests/ (e.g., FakeLogClock).
 *
 * now_wall_us() is retained in the interface for use by other consumers (e.g.,
 * metrics, message envelope timestamping) — it is not included in the standard
 * log header format but is used by Logger::log_wall().
 *
 * Design: Power of 10 Rule 9 vtable exception — virtual dispatch is the
 * approved mechanism for interface polymorphism (MISRA C++:2023 rules on
 * virtuals). No explicit function pointers.
 *
 * Layering: src/core depends only on this abstract interface. No POSIX headers
 * are included here.
 *
 * Implements: REQ-7.1.1, REQ-7.2.1
 */

#ifndef CORE_ILOGCLOCK_HPP
#define CORE_ILOGCLOCK_HPP

#include <cstdint>

// Power of 10 Rule 9 exception: vtable-backed virtual dispatch is the approved
// architectural mechanism for interface polymorphism. No explicit function
// pointer declarations appear in application code.
class ILogClock {
public:
    // Virtual destructor required for base class with virtual methods
    // (MISRA C++:2023 Rule 15.5.2).
    virtual ~ILogClock() = default;

    /// Wall clock time in microseconds since the UNIX epoch (CLOCK_REALTIME).
    /// Returns 0 on platform error; callers must handle 0 gracefully.
    virtual uint64_t now_wall_us() const = 0;

    /// Monotonic clock time in microseconds since an arbitrary epoch
    /// (CLOCK_MONOTONIC). Returns 0 on platform error.
    virtual uint64_t now_monotonic_us() const = 0;

    /// Platform thread identifier for the calling thread.
    /// On POSIX: derived from pthread_self(), truncated to uint32_t.
    virtual uint32_t thread_id() const = 0;
};

#endif // CORE_ILOGCLOCK_HPP
