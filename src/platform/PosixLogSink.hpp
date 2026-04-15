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
 * @file PosixLogSink.hpp
 * @brief POSIX implementation of ILogSink: writes to stderr via write(2).
 *
 * Uses the Non-Virtual Interface (NVI) idiom to expose a protected virtual
 * seam (do_write) that test subclasses can override to inject write(2)
 * failures without modifying production code.
 *
 * Note: 'final' is intentionally omitted to allow the NVI test seam subclass
 * (FaultPosixLogSink) to override do_write(). This is a deliberate deviation
 * from the preferred 'final' pattern; documented here per deviation policy.
 *
 * Power of 10 Rule 9 exception: vtable-backed virtual dispatch via ILogSink
 * and the NVI do_write() seam — no explicit function pointers.
 *
 * Implements: REQ-7.1.1
 */

#ifndef PLATFORM_POSIXLOGSINK_HPP
#define PLATFORM_POSIXLOGSINK_HPP

#include <cstdint>
#include <unistd.h>   // STDERR_FILENO, write(2), ssize_t
#include "core/ILogSink.hpp"

// 'final' omitted: NVI seam requires subclassing for fault injection
// (FaultPosixLogSink in tests/). Documented deviation from preferred 'final'.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class PosixLogSink : public ILogSink {
public:
    // Virtual destructor inherited from ILogSink (MISRA C++:2023 Rule 15.5.2).
    ~PosixLogSink() override = default;

    /// Write @p len bytes from @p buf to stderr via do_write().
    /// Power of 10 R7: write(2) return value is explicitly read inside
    /// do_write() and suppressed with (void) + comment — partial/failed
    /// writes to stderr are silently dropped (acceptable for logging).
    void write(const char* buf, uint32_t len) override;

    /// Singleton accessor — returns the one shared instance.
    /// Initialized on first call (no dynamic allocation: function-local static).
    static PosixLogSink& instance();

protected:
    // NVI seam: default implementation calls ::write(STDERR_FILENO, buf, count).
    // Overridden in FaultPosixLogSink (tests/) to inject write(2) failures.
    // Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
    virtual ssize_t do_write(int fd, const void* buf, size_t count);

protected:
    // Protected constructor: not instantiable by external clients (use instance()).
    // Protected (not private) to allow NVI test-seam subclasses (FaultPosixLogSink)
    // to construct without going through the singleton accessor.
    PosixLogSink() = default;
};

#endif // PLATFORM_POSIXLOGSINK_HPP
