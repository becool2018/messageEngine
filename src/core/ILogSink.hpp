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
 * @file ILogSink.hpp
 * @brief Abstract output sink for the Logger facility.
 *
 * Logger calls sink->write() instead of calling write(2) or fprintf directly,
 * keeping all POSIX output calls out of src/core. Concrete implementations
 * live in src/platform (e.g., PosixLogSink). Test doubles live in tests/
 * (e.g., RingLogSink, FaultPosixLogSink).
 *
 * Design: Power of 10 Rule 9 vtable exception — virtual dispatch is the
 * approved mechanism for interface polymorphism (MISRA C++:2023 rules on
 * virtuals). No explicit function pointers.
 *
 * Layering: src/core depends only on this abstract interface. No POSIX headers
 * are included here.
 *
 * Implements: REQ-7.1.1
 */

#ifndef CORE_ILOGSINK_HPP
#define CORE_ILOGSINK_HPP

#include <cstdint>

// Power of 10 Rule 9 exception: vtable-backed virtual dispatch is the approved
// architectural mechanism for interface polymorphism. No explicit function
// pointer declarations appear in application code.
class ILogSink {
public:
    // Virtual destructor required for base class with virtual methods
    // (MISRA C++:2023 Rule 15.5.2).
    virtual ~ILogSink() = default;

    /// Write @p len bytes from @p buf to the sink.
    /// Implementations must not throw (F-Prime: -fno-exceptions).
    /// Partial writes and errors are silently dropped — acceptable for a
    /// logging facility (Power of 10 R7: return value must be read by impl).
    virtual void write(const char* buf, uint32_t len) = 0;
};

#endif // CORE_ILOGSINK_HPP
