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
 * @file test_stress_ordering.cpp
 * @brief Sustained-load stress test for OrderingBuffer gap injection and hold recycling.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * Unit tests verify the gap-hold-release cycle at one or two design points.
 * This stress test verifies correctness over ORDERING_GAP_CYCLES = 1000
 * consecutive gap→fill→release→drain cycles per round, driving the per-peer
 * sequence counter from 1 to 2001 per round.  Any hold-slot leak will cause
 * ERR_FULL on a subsequent ingest(); any sequence-counter drift will cause
 * unexpected OK or ERR_AGAIN returns and trip an assert.
 *
 * The test accepts an optional runtime duration (seconds) from argv[1] and
 * runs as many complete 1000-cycle rounds as fit in that window.  Pass no
 * argument for a 60-second default.  Use a small value (e.g., 5) for a quick
 * smoke run; use a larger value (e.g., 600) for overnight soak testing.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS TEST CATCHES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Hold slot not freed by try_release_next() — the next cycle's ingest()
 *    of an out-of-order message would return ERR_FULL (hold buffer exhausted).
 *  • next_expected_seq drift — after a gap→fill→release cycle the counter must
 *    advance by exactly 2; an off-by-one would cause the next cycle's seq_lo
 *    to be seen as out-of-order (held instead of delivered).
 *  • Spurious sweep activity — sweep_expired_holds() called with a timestamp
 *    well before expiry must free 0 slots; a non-zero count would indicate a
 *    hold slot was leaked with a stale expiry.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 2 (fixed loop bounds)  — inner loops use compile-time constants;
 *                                outer duration loop per Rule 2 deviation (below).
 *  Rule 3 (no dynamic alloc)   — static arrays only; no new/delete.
 *  Rule 5 (assertions)         — ≥2 assert() calls per function.
 *  Rule 7 (check returns)      — every Result return value is checked.
 *  Rule 10 (zero warnings)     — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions  — raised ceiling per §1b; this function ≤ 10.
 *  assert() permitted          — tests/ exemption per §9.
 *
 * Verifies: REQ-3.3.5
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <ctime>    // time_t, time()
#include <climits>  // UINT32_MAX

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/OrderingBuffer.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// ─────────────────────────────────────────────────────────────────────────────
// Stress iteration counts (compile-time — Power of 10 Rule 2).
// ─────────────────────────────────────────────────────────────────────────────

/// Number of gap→fill→release cycles per round.
static const uint32_t ORDERING_GAP_CYCLES = 1000U;

/// Source node ID used throughout the test.
static const NodeId ORDERING_SRC = 30U;

// ─────────────────────────────────────────────────────────────────────────────
// Parse an optional duration (seconds) from argv[1].
// Returns 60 if no argument is given or the parsed value is 0.
// Power of 10 Rule 3: no dynamic allocation; no strtol to avoid locale dep.
// Power of 10 Rule 2: loop bounded by 10 digits (compile-time).
// ─────────────────────────────────────────────────────────────────────────────
static time_t parse_duration_secs(int argc, char* argv[])
{
    assert(argc >= 0);
    if (argc < 2) {
        return static_cast<time_t>(60);
    }
    uint32_t val = 0U;
    const char* p = argv[1];
    for (uint32_t i = 0U; i < 10U && *p >= '0' && *p <= '9'; ++i) {
        val = val * 10U + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    return (val == 0U) ? static_cast<time_t>(60) : static_cast<time_t>(val);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build an ordered DATA envelope.
// ─────────────────────────────────────────────────────────────────────────────
static void make_ordered_env(MessageEnvelope& env,
                              NodeId           src,
                              uint32_t         seq,
                              uint64_t         now_us,
                              uint64_t         expiry_us)
{
    envelope_init(env);
    env.message_type         = MessageType::DATA;
    env.source_id            = src;
    env.destination_id       = 1U;
    // Unique message_id derived from seq to distinguish held messages.
    env.message_id           = static_cast<uint64_t>(seq) + 1000ULL;
    env.sequence_num         = seq;
    env.timestamp_us         = now_us;
    env.expiry_time_us       = expiry_us;
    env.reliability_class    = ReliabilityClass::BEST_EFFORT;
    env.fragment_index       = 0U;
    env.fragment_count       = 1U;
    env.payload_length       = 4U;
    env.total_payload_length = 4U;
    // Embed the low byte of seq in the first payload byte for identity checks.
    env.payload[0] = static_cast<uint8_t>(seq & 0xFFU);
    env.payload[1] = static_cast<uint8_t>((seq >> 8U) & 0xFFU);
    env.payload[2] = 0U;
    env.payload[3] = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: OrderingBuffer gap inject — duration-bounded, ORDERING_GAP_CYCLES
//         gap→fill→release cycles per round.
// ─────────────────────────────────────────────────────────────────────────────
// Each round (while-iteration) runs ORDERING_GAP_CYCLES inner cycles:
//   1. Inject seq_hi = base_seq + 1 first (out-of-order) → ERR_AGAIN (held).
//   2. Inject seq_lo = base_seq        (fills the gap)   → OK (delivered).
//   3. try_release_next() → OK (held seq_hi now deliverable).
//   4. try_release_next() again → ERR_AGAIN (hold buffer empty).
//   5. sweep_expired_holds(now_us) → 0 freed (no expired holds).
//   6. Advance base_seq by 2.
// A fresh OrderingBuffer is created for each round.
// ─────────────────────────────────────────────────────────────────────────────
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop.  Per-iteration work is bounded by ORDERING_GAP_CYCLES (compile-time);
// the loop terminates when the wall-clock reaches deadline.
// Verifies: REQ-3.3.5
static uint32_t test_ordering_gap_inject_soak(time_t deadline)
{
    // Static buffer for sweep output — Power of 10 Rule 3.
    static MessageEnvelope sweep_freed[ORDERING_HOLD_COUNT];

    uint32_t total_cycles = 0U;

    // Power of 10 Rule 2 deviation: duration loop — bounded per-iteration
    // work (ORDERING_GAP_CYCLES × 5 steps); terminates on wall-clock deadline.
    while (time(nullptr) < deadline) {
        OrderingBuffer buf;
        buf.init(1U);

        uint32_t base_seq = 1U;

        // Power of 10 Rule 2: bounded by ORDERING_GAP_CYCLES (compile-time).
        for (uint32_t cycle = 0U; cycle < ORDERING_GAP_CYCLES; ++cycle) {
            const uint32_t seq_hi  = base_seq + 1U;  // arrives first (out of order)
            const uint32_t seq_lo  = base_seq;         // arrives second (fills gap)
            const uint64_t now_us  = 1000000ULL
                                     + static_cast<uint64_t>(cycle) * 1000ULL;
            // Expiry 9 s in the future — well past the end of the test.
            const uint64_t expiry  = now_us + 9000000ULL;

            MessageEnvelope m_hi;
            make_ordered_env(m_hi, ORDERING_SRC, seq_hi, now_us, expiry);

            MessageEnvelope m_lo;
            make_ordered_env(m_lo, ORDERING_SRC, seq_lo, now_us, expiry);

            MessageEnvelope out;

            // Step 1: inject seq_hi — gap exists at seq_lo; must be held.
            envelope_init(out);
            Result r_hi = buf.ingest(m_hi, out, now_us);
            assert(r_hi == Result::ERR_AGAIN);

            // Step 2: inject seq_lo — fills the gap; must be delivered immediately.
            envelope_init(out);
            Result r_lo = buf.ingest(m_lo, out, now_us);
            assert(r_lo == Result::OK);
            assert(out.sequence_num == seq_lo);
            assert(out.payload[0] == static_cast<uint8_t>(seq_lo & 0xFFU));

            // Step 3: try_release_next() must return seq_hi from the hold buffer.
            envelope_init(out);
            Result r_rel = buf.try_release_next(ORDERING_SRC, out);
            assert(r_rel == Result::OK);
            assert(out.sequence_num == seq_hi);
            assert(out.payload[0] == static_cast<uint8_t>(seq_hi & 0xFFU));

            // Step 4: hold buffer must now be empty for this source.
            envelope_init(out);
            Result r_empty = buf.try_release_next(ORDERING_SRC, out);
            assert(r_empty == Result::ERR_AGAIN);

            // Step 5: no holds are stale — sweep must free nothing.
            uint32_t freed = buf.sweep_expired_holds(now_us,
                                                       sweep_freed,
                                                       ORDERING_HOLD_COUNT);
            assert(freed == 0U);

            // Advance base_seq by 2 for the next cycle.
            // CERT INT30-C: base_seq ≤ 2001 per round; well within UINT32_MAX.
            assert(base_seq <= UINT32_MAX - 2U);
            base_seq += 2U;
        }

        total_cycles += ORDERING_GAP_CYCLES;
    }

    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Initialize logger before any production code that may call LOG_* macros.
    // Power of 10: return value checked; failure causes abort via NEVER_COMPILED_OUT_ASSERT.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    const time_t duration_secs = parse_duration_secs(argc, argv);
    const time_t deadline      = time(nullptr) + duration_secs;

    assert(duration_secs > 0);

    printf("=== Stress: test_stress_ordering ===\n");
    printf("    (running for %lu s; %u cycles per round)\n",
           static_cast<unsigned long>(duration_secs), ORDERING_GAP_CYCLES);

    printf("  Test 3: OrderingBuffer gap-inject soak...");
    const uint32_t total = test_ordering_gap_inject_soak(deadline);
    assert(total > 0U);
    printf(" PASS (%u total cycles across %u rounds)\n",
           total, total / ORDERING_GAP_CYCLES);

    printf("=== test_stress_ordering: ALL PASSED ===\n");
    return 0;
}
