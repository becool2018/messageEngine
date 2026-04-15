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
 * @file FakeLogClock.hpp
 * @brief Test double for ILogClock — returns caller-controlled values.
 *
 * Enables deterministic log assertions without relying on real POSIX clock.
 * Used by test_logger.cpp for T-2.4, T-2.5, T-2.8W, T-2.10W, T-6.1–T-6.3.
 *
 * Verifies: REQ-7.1.1, REQ-7.2.1
 * Verification: M1 + M2 + M4 + M5 (fault injection via ILogClock)
 *
 * Power of 10 Rule 9 exception: virtual dispatch via ILogClock vtable —
 * the approved mechanism. No explicit function pointers.
 */

#ifndef TESTS_FAKELOGCLOCK_HPP
#define TESTS_FAKELOGCLOCK_HPP

#include <cstdint>
#include "core/ILogClock.hpp"

/// Test double for ILogClock that returns caller-settable values.
/// Not thread-safe — intended for single-threaded test use only.
// Power of 10 Rule 9 exception: vtable-backed virtual dispatch, no function pointers.
class FakeLogClock final : public ILogClock {
public:
    FakeLogClock() :
        m_wall_us(0U),
        m_mono_us(0U),
        m_thread_id(0U)
    {}

    ~FakeLogClock() override = default;

    // Setters — called by tests to control returned values.
    void set_wall_us(uint64_t v)    { m_wall_us    = v; }
    void set_mono_us(uint64_t v)    { m_mono_us    = v; }
    void set_thread_id(uint32_t v)  { m_thread_id  = v; }

    // ILogClock interface.
    uint64_t now_wall_us()      const override { return m_wall_us;   }
    uint64_t now_monotonic_us() const override { return m_mono_us;   }
    uint32_t thread_id()        const override { return m_thread_id; }

private:
    uint64_t m_wall_us;
    uint64_t m_mono_us;
    uint32_t m_thread_id;
};

#endif // TESTS_FAKELOGCLOCK_HPP
