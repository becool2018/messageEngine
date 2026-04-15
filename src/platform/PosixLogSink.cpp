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
 * @file PosixLogSink.cpp
 * @brief POSIX implementation of ILogSink.
 *
 * Implements: REQ-7.1.1
 */

#include "platform/PosixLogSink.hpp"

// Power of 10: no dynamic allocation — function-local static (initialized
// once, lives for program lifetime).
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
PosixLogSink& PosixLogSink::instance()
{
    static PosixLogSink s_instance;  // Power of 10 Rule 3: no heap allocation
    return s_instance;
}

void PosixLogSink::write(const char* buf, uint32_t len)
{
    if (buf == nullptr || len == 0U) {
        return;
    }
    // NVI dispatch: default calls ::write(2); test subclass may override.
    // Power of 10 R7: return value must be read. A partial or failed write
    // to stderr is silently dropped — acceptable for a logging facility.
    // The (void) cast is intentional; a comment is required per project policy.
    // NOLINTNEXTLINE(bugprone-unused-return-value)
    (void)do_write(STDERR_FILENO,
                   static_cast<const void*>(buf),
                   static_cast<size_t>(len));
    // Return value intentionally discarded: logging is best-effort; a failed
    // or partial write to stderr does not constitute a recoverable error that
    // the Logger can act on. (Power of 10 R7 satisfied by explicit read+cast.)
}

ssize_t PosixLogSink::do_write(int fd, const void* buf, size_t count)
{
    // Thin POSIX wrapper — single line per NVI contract. No logic here.
    return ::write(fd, buf, count);
}
