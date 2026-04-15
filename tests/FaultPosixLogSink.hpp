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
 * @file FaultPosixLogSink.hpp
 * @brief Fault-injection subclass of PosixLogSink for testing write(2) failure
 *        paths (T-4.3, T-4.4).
 *
 * Overrides do_write() via the NVI seam to return a configurable value,
 * simulating partial writes (return 0) or errors (return -1).
 *
 * Verifies: REQ-7.1.1
 * Verification: M1 + M2 + M5 (fault injection via PosixLogSink NVI seam)
 *
 * Power of 10 Rule 9 exception: virtual dispatch via PosixLogSink NVI seam —
 * the approved mechanism. No explicit function pointers.
 */

#ifndef TESTS_FAULTPOSIXLOGSINK_HPP
#define TESTS_FAULTPOSIXLOGSINK_HPP

#include <unistd.h>  // ssize_t
#include "platform/PosixLogSink.hpp"

/// Fault-injection subclass: do_write() returns m_ret unconditionally.
/// Pass m_ret = 0 for partial-write simulation, -1 for error simulation.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class FaultPosixLogSink final : public PosixLogSink {
public:
    explicit FaultPosixLogSink(ssize_t ret) : m_ret(ret) {}
    ~FaultPosixLogSink() override = default;

protected:
    // Override NVI seam to inject write(2) failure or partial write.
    // The fd and count parameters are intentionally unused; the configured
    // m_ret is returned regardless of input.
    ssize_t do_write(int /* fd */,
                     const void* /* buf */,
                     size_t /* count */) override
    {
        return m_ret;
    }

private:
    ssize_t m_ret;  ///< Return value injected for write(2) calls.
};

#endif // TESTS_FAULTPOSIXLOGSINK_HPP
