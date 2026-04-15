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
 * @file FaultPosixLogClock.hpp
 * @brief Fault-injection subclass of PosixLogClock for testing clock_gettime
 *        failure paths (T-3.4, T-3.5).
 *
 * Overrides do_clock_gettime() via the NVI seam to return -1, simulating a
 * clock_gettime failure that is otherwise unreachable in a loopback environment.
 *
 * Verifies: REQ-7.1.1, REQ-7.2.1
 * Verification: M1 + M2 + M5 (fault injection via PosixLogClock NVI seam)
 *
 * Power of 10 Rule 9 exception: virtual dispatch via PosixLogClock NVI seam —
 * the approved mechanism. No explicit function pointers.
 */

#ifndef TESTS_FAULTPOSIXLOGCLOCK_HPP
#define TESTS_FAULTPOSIXLOGCLOCK_HPP

#include <time.h>  // clockid_t, struct timespec
#include "platform/PosixLogClock.hpp"

/// Fault-injection subclass: do_clock_gettime always returns -1.
/// Tests the clock_gettime failure → return 0 branch in PosixLogClock.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class FaultPosixLogClock final : public PosixLogClock {
public:
    FaultPosixLogClock()  = default;
    ~FaultPosixLogClock() override = default;

protected:
    // Override NVI seam to inject clock_gettime failure.
    // Returns -1 unconditionally (simulates syscall error).
    int do_clock_gettime(clockid_t /* clk */,
                         struct timespec* /* ts */) const override
    {
        return -1;
    }
};

#endif // TESTS_FAULTPOSIXLOGCLOCK_HPP
