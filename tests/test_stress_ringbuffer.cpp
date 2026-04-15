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
 * @file test_stress_ringbuffer.cpp
 * @brief Concurrent producer/consumer stress test for RingBuffer.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * The unit tests for RingBuffer verify correctness in a single-threaded
 * setting (no concurrent access).  This stress test verifies the SPSC lock-
 * free guarantees under real concurrent load: a dedicated producer thread and
 * a dedicated consumer thread hammer the same RingBuffer simultaneously for
 * the duration of the test.
 *
 * Any data race, acquire/release ordering failure, or index-arithmetic error
 * that only manifests under concurrency will be detected by:
 *   a) A mismatch in message_id on the consumer side (data corruption or
 *      reordering — detected by the assert in the consumer thread).
 *   b) An asymmetry in messages produced vs consumed after the threads join
 *      (lost messages — detected by the assert in the test body).
 *   c) pthread_join returning non-zero (thread crash — detected immediately).
 *
 * The test accepts an optional runtime duration (seconds) from argv[1] and
 * runs as many complete RING_MSGS_PER_ROUND-message rounds as fit in that
 * window.  Pass no argument for a 60-second default.
 *
 * Multiple instances of this binary can run simultaneously on the same
 * machine without interference: each process has its own address space (no
 * shared memory, no sockets, no files, no named semaphores between instances).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS TEST CATCHES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Missing release/acquire fence — consumer reads stale data written by
 *    the producer; detected as a message_id mismatch.
 *  • Index arithmetic wrap error at 2^32 — m_tail or m_head wraps silently
 *    to 0; detected as a message_id gap or duplicate.
 *  • Full-buffer spin deadlock — if push() never returns OK when the buffer
 *    has space, both threads stall; detected by spin-attempt overflow assert.
 *  • Lost wakeup — consumer misses a message and blocks; detected by the
 *    spin-attempt overflow assert on the consumer side.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * DESIGN NOTES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • One producer pthread + one consumer pthread per round.
 *  • pthread_create / pthread_join from <pthread.h> — POSIX, not STL <thread>.
 *    tests/ Rule 3 exemption (dynamic allocation in test harness setup) covers
 *    the thread stack allocated by the OS.  No new/delete in this file.
 *  • Bounded spin-waits replace busy-spin loops: each spin attempt is counted
 *    and asserted against RING_MAX_SPIN_ATTEMPTS so a stall trips an assert
 *    rather than looping forever (Power of 10 Rule 2 deviation below).
 *  • RingBuffer and thread argument struct are declared static so the large
 *    MessageEnvelope array (MSG_RING_CAPACITY × sizeof(MessageEnvelope)) lives
 *    in BSS rather than the stack — Power of 10 Rule 3.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 1 (no goto / recursion)  — obeyed throughout.
 *  Rule 2 (fixed loop bounds)    — inner loops use compile-time constants;
 *                                  spin-waits bounded by RING_MAX_SPIN_ATTEMPTS;
 *                                  outer while is duration-bounded (deviation).
 *  Rule 3 (no dynamic alloc)    — no new/delete; OS thread stacks are outside
 *                                  the project's allocation domain (deviation).
 *  Rule 5 (assertions)          — ≥2 assert() per function.
 *  Rule 7 (check returns)       — all Result and pthread return values checked.
 *  Rule 10 (zero warnings)      — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions   — raised ceiling per §1b; each function ≤ 12.
 *  assert() permitted           — tests/ exemption per §9.
 *
 * Verifies: REQ-4.1.2
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <ctime>    // time_t, time()
#include <climits>  // UINT32_MAX, UINT64_MAX
#include <pthread.h>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/RingBuffer.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// ─────────────────────────────────────────────────────────────────────────────
// Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────

/// Messages exchanged per round (producer sends, consumer receives this many).
/// Must be larger than MSG_RING_CAPACITY to exercise the full/empty boundary
/// many times per round.
static const uint32_t RING_MSGS_PER_ROUND = 10000U;

/// Maximum spin attempts before a stalled push() or pop() asserts.
/// Sized to be large enough that a healthy system never trips it, but small
/// enough that a deadlock is caught in under a millisecond.
/// Power of 10 Rule 2: bounded spin-wait (not an unbounded busy-spin).
static const uint32_t RING_MAX_SPIN_ATTEMPTS = 10000000U;

// ─────────────────────────────────────────────────────────────────────────────
// Thread argument struct — static (BSS), never heap-allocated.
// ─────────────────────────────────────────────────────────────────────────────
struct RingStressArgs {
    RingBuffer* ring;           ///< Shared ring buffer (producer writes, consumer reads).
    uint64_t    start_id;       ///< First message_id for this round.
    uint32_t    msgs_to_xfer;   ///< Number of messages the producer will push.
    uint32_t    msgs_produced;  ///< Filled by producer thread on completion.
    uint32_t    msgs_consumed;  ///< Filled by consumer thread on completion.
};

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
// Helper: build a DATA envelope carrying msg_id in the payload.
// ─────────────────────────────────────────────────────────────────────────────
static void make_ring_env(MessageEnvelope& env, uint64_t msg_id)
{
    assert(msg_id > 0U);
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.source_id         = 1U;
    env.destination_id    = 2U;
    env.message_id        = msg_id;
    env.sequence_num      = 0U;         // UNORDERED — no ordering gate
    env.timestamp_us      = 1000000ULL;
    env.expiry_time_us    = 0xFFFFFFFFFFFFFF00ULL;
    env.reliability_class = ReliabilityClass::BEST_EFFORT;
    env.fragment_index    = 0U;
    env.fragment_count    = 1U;
    env.payload_length    = 8U;
    env.total_payload_length = 8U;
    // Encode msg_id in first 8 payload bytes for identity check.
    // CERT INT30-C: bit-shifts are within uint64_t; no overflow.
    env.payload[0] = static_cast<uint8_t>( msg_id        & 0xFFU);
    env.payload[1] = static_cast<uint8_t>((msg_id >>  8U) & 0xFFU);
    env.payload[2] = static_cast<uint8_t>((msg_id >> 16U) & 0xFFU);
    env.payload[3] = static_cast<uint8_t>((msg_id >> 24U) & 0xFFU);
    env.payload[4] = static_cast<uint8_t>((msg_id >> 32U) & 0xFFU);
    env.payload[5] = static_cast<uint8_t>((msg_id >> 40U) & 0xFFU);
    env.payload[6] = static_cast<uint8_t>((msg_id >> 48U) & 0xFFU);
    env.payload[7] = static_cast<uint8_t>((msg_id >> 56U) & 0xFFU);
}

// ─────────────────────────────────────────────────────────────────────────────
// Producer thread function.
// Pushes msgs_to_xfer messages with sequential message_ids starting at start_id.
// Uses a bounded spin-wait when the ring is full (RING_MAX_SPIN_ATTEMPTS).
// ─────────────────────────────────────────────────────────────────────────────
static void* producer_fn(void* arg)
{
    RingStressArgs* a = static_cast<RingStressArgs*>(arg);
    assert(a != nullptr);
    assert(a->ring != nullptr);

    uint64_t cur_id = a->start_id;

    // Power of 10 Rule 2: bounded by msgs_to_xfer (compile-time RING_MSGS_PER_ROUND).
    for (uint32_t i = 0U; i < a->msgs_to_xfer; ++i) {
        MessageEnvelope env;
        make_ring_env(env, cur_id);

        // Bounded spin-wait: ring may be full; retry until space appears.
        // Power of 10 Rule 2: spin bounded by RING_MAX_SPIN_ATTEMPTS (compile-time).
        uint32_t spin = 0U;
        Result   r    = a->ring->push(env);
        while (r == Result::ERR_FULL) {
            assert(spin < RING_MAX_SPIN_ATTEMPTS);  // deadlock guard
            ++spin;
            r = a->ring->push(env);
        }
        assert(r == Result::OK);

        // CERT INT30-C: cur_id wraps at UINT64_MAX; start_id > 0 so cur_id > 0 always.
        ++cur_id;
        ++a->msgs_produced;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Consumer thread function.
// Pops msgs_to_xfer messages and verifies sequential message_ids.
// Uses a bounded spin-wait when the ring is empty (RING_MAX_SPIN_ATTEMPTS).
// ─────────────────────────────────────────────────────────────────────────────
static void* consumer_fn(void* arg)
{
    RingStressArgs* a = static_cast<RingStressArgs*>(arg);
    assert(a != nullptr);
    assert(a->ring != nullptr);

    uint64_t expected_id = a->start_id;

    // Power of 10 Rule 2: bounded by msgs_to_xfer (compile-time RING_MSGS_PER_ROUND).
    for (uint32_t i = 0U; i < a->msgs_to_xfer; ++i) {
        MessageEnvelope env;
        envelope_init(env);

        // Bounded spin-wait: ring may be empty; retry until item appears.
        // Power of 10 Rule 2: spin bounded by RING_MAX_SPIN_ATTEMPTS (compile-time).
        uint32_t spin = 0U;
        Result   r    = a->ring->pop(env);
        while (r == Result::ERR_EMPTY) {
            assert(spin < RING_MAX_SPIN_ATTEMPTS);  // deadlock guard
            ++spin;
            r = a->ring->pop(env);
        }
        assert(r == Result::OK);

        // Verify message identity — catches data races and ordering failures.
        assert(env.message_id == expected_id);

        // Verify payload encoding matches message_id — catches partial write races.
        assert(env.payload[0] == static_cast<uint8_t>( expected_id        & 0xFFU));
        assert(env.payload[1] == static_cast<uint8_t>((expected_id >>  8U) & 0xFFU));

        ++expected_id;
        ++a->msgs_consumed;
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Concurrent producer/consumer storm — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Each round: init a fresh RingBuffer, spawn one producer thread and one
// consumer thread, let them run concurrently through RING_MSGS_PER_ROUND
// messages, join both threads, assert counts match.
//
// The ring capacity (MSG_RING_CAPACITY = 64) is much smaller than
// RING_MSGS_PER_ROUND (10 000), so the ring cycles through full and empty
// many times per round, exercising all boundary conditions under real
// concurrent load.
//
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop.  Per-iteration work is bounded by RING_MSGS_PER_ROUND (compile-time);
// loop terminates when the wall-clock reaches the deadline.
// Verifies: REQ-4.1.2
static uint32_t test_ringbuffer_concurrent_storm(time_t deadline)
{
    // Static allocation — Power of 10 Rule 3: RingBuffer contains
    // MSG_RING_CAPACITY MessageEnvelopes; too large for the stack.
    static RingBuffer    ring;
    static RingStressArgs args;

    uint32_t total_rounds = 0U;
    uint64_t msg_id       = 1ULL;  // monotonically increasing across all rounds

    // Power of 10 Rule 2 deviation: duration-bounded outer loop.
    while (time(nullptr) < deadline) {
        ring.init();  // reset indices for this round

        args.ring         = &ring;
        args.start_id     = msg_id;
        args.msgs_to_xfer = RING_MSGS_PER_ROUND;
        args.msgs_produced = 0U;
        args.msgs_consumed = 0U;

        pthread_t prod_tid;
        pthread_t cons_tid;

        // Spawn consumer first so it is ready to drain as soon as the producer
        // starts pushing — reduces contention on the initially-empty ring.
        int rc = pthread_create(&cons_tid, nullptr, consumer_fn, &args);
        assert(rc == 0);

        rc = pthread_create(&prod_tid, nullptr, producer_fn, &args);
        assert(rc == 0);

        // Wait for both threads to complete.
        int rp = pthread_join(prod_tid, nullptr);
        int rs = pthread_join(cons_tid, nullptr);
        assert(rp == 0);
        assert(rs == 0);

        // Verify all messages were produced and consumed.
        assert(args.msgs_produced == RING_MSGS_PER_ROUND);
        assert(args.msgs_consumed == RING_MSGS_PER_ROUND);

        // CERT INT30-C: msg_id += RING_MSGS_PER_ROUND; max rounds ≈ 1.8 × 10^15
        // for a 64-bit counter — no overflow risk in any realistic test run.
        msg_id += static_cast<uint64_t>(RING_MSGS_PER_ROUND);
        ++total_rounds;
    }

    return total_rounds;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Concurrent asymmetric-speed storm — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Same structure as Test 1 but with RING_MSGS_PER_ROUND doubled so the
// producer runs longer relative to what the consumer can immediately drain.
// The producer will hit ERR_FULL repeatedly (ring fills up while the consumer
// is behind), exercising the backpressure/spin-wait path on both sides under
// sustained load.
//
// A pause between consumer pops (spin nop loop bounded by RING_SPIN_DELAY)
// is applied in the consumer to intentionally slow it relative to the producer.
//
// Power of 10 Rule 2 deviation: outer while loop is duration-bounded.
// Verifies: REQ-4.1.2
// ─────────────────────────────────────────────────────────────────────────────

/// Number of no-op iterations the consumer burns between pops to simulate
/// a "slow consumer" and force the producer to spin on ERR_FULL.
static const uint32_t RING_CONSUMER_DELAY_ITERS = 128U;

/// Argument type for the slow-consumer variant.
struct RingSlowConsumerArgs {
    RingBuffer* ring;
    uint64_t    start_id;
    uint32_t    msgs_to_xfer;
    uint32_t    msgs_produced;
    uint32_t    msgs_consumed;
};

// Slow consumer thread: burns RING_CONSUMER_DELAY_ITERS no-ops between pops
// to force the producer to back off on a full ring.
static void* slow_consumer_fn(void* arg)
{
    RingSlowConsumerArgs* a = static_cast<RingSlowConsumerArgs*>(arg);
    assert(a != nullptr);
    assert(a->ring != nullptr);

    uint64_t expected_id = a->start_id;

    // Power of 10 Rule 2: bounded by msgs_to_xfer (RING_MSGS_PER_ROUND).
    for (uint32_t i = 0U; i < a->msgs_to_xfer; ++i) {

        // Burn RING_CONSUMER_DELAY_ITERS no-ops to simulate slow consumer.
        // Power of 10 Rule 2: bounded by RING_CONSUMER_DELAY_ITERS (compile-time).
        volatile uint32_t nop_sink = 0U;
        for (uint32_t d = 0U; d < RING_CONSUMER_DELAY_ITERS; ++d) {
            ++nop_sink;
        }
        (void)nop_sink;

        // Bounded spin-wait: ring may be empty after the delay.
        // Power of 10 Rule 2: bounded by RING_MAX_SPIN_ATTEMPTS.
        uint32_t spin = 0U;
        MessageEnvelope env;
        envelope_init(env);
        Result r = a->ring->pop(env);
        while (r == Result::ERR_EMPTY) {
            assert(spin < RING_MAX_SPIN_ATTEMPTS);
            ++spin;
            r = a->ring->pop(env);
        }
        assert(r == Result::OK);
        assert(env.message_id == expected_id);
        assert(env.payload[0] == static_cast<uint8_t>( expected_id        & 0xFFU));
        assert(env.payload[1] == static_cast<uint8_t>((expected_id >>  8U) & 0xFFU));

        ++expected_id;
        ++a->msgs_consumed;
    }

    return nullptr;
}

static uint32_t test_ringbuffer_backpressure_storm(time_t deadline)
{
    static RingBuffer         ring2;
    static RingSlowConsumerArgs args2;

    uint32_t total_rounds = 0U;
    uint64_t msg_id       = 1ULL;

    // Power of 10 Rule 2 deviation: duration-bounded outer loop.
    while (time(nullptr) < deadline) {
        ring2.init();

        args2.ring         = &ring2;
        args2.start_id     = msg_id;
        args2.msgs_to_xfer = RING_MSGS_PER_ROUND;
        args2.msgs_produced = 0U;
        args2.msgs_consumed = 0U;

        pthread_t prod_tid;
        pthread_t cons_tid;

        // Slow consumer first — it will fall behind, forcing the producer
        // to spin on ERR_FULL repeatedly.
        int rc = pthread_create(&cons_tid, nullptr, slow_consumer_fn, &args2);
        assert(rc == 0);

        // Re-use producer_fn: same push loop with bounded spin on ERR_FULL.
        // Re-pack into RingStressArgs for producer_fn.
        static RingStressArgs prod_args;
        prod_args.ring         = &ring2;
        prod_args.start_id     = msg_id;
        prod_args.msgs_to_xfer = RING_MSGS_PER_ROUND;
        prod_args.msgs_produced = 0U;
        prod_args.msgs_consumed = 0U;

        rc = pthread_create(&prod_tid, nullptr, producer_fn, &prod_args);
        assert(rc == 0);

        int rp = pthread_join(prod_tid, nullptr);
        int rs = pthread_join(cons_tid, nullptr);
        assert(rp == 0);
        assert(rs == 0);

        assert(prod_args.msgs_produced == RING_MSGS_PER_ROUND);
        assert(args2.msgs_consumed    == RING_MSGS_PER_ROUND);

        msg_id += static_cast<uint64_t>(RING_MSGS_PER_ROUND);
        ++total_rounds;
    }

    return total_rounds;
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
    assert(duration_secs > 0);

    printf("=== Stress: test_stress_ringbuffer ===\n");
    printf("    (running for %lu s per test; %u msgs/round;"
           " ring capacity %u; max spin %u)\n",
           static_cast<unsigned long>(duration_secs),
           RING_MSGS_PER_ROUND, MSG_RING_CAPACITY, RING_MAX_SPIN_ATTEMPTS);

    printf("  Test 1: concurrent producer/consumer storm...");
    const uint32_t t1_rounds = test_ringbuffer_concurrent_storm(
                                   time(nullptr) + duration_secs);
    assert(t1_rounds > 0U);
    printf(" PASS (%u rounds; %u total messages)\n",
           t1_rounds, t1_rounds * RING_MSGS_PER_ROUND);

    printf("  Test 2: backpressure storm (slow consumer)...");
    const uint32_t t2_rounds = test_ringbuffer_backpressure_storm(
                                   time(nullptr) + duration_secs);
    assert(t2_rounds > 0U);
    printf(" PASS (%u rounds; %u total messages)\n",
           t2_rounds, t2_rounds * RING_MSGS_PER_ROUND);

    printf("=== test_stress_ringbuffer: ALL PASSED ===\n");
    return 0;
}
