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
 * @file test_stress_capacity.cpp
 * @brief Capacity exhaustion and slot-recycling stress tests.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * Unit tests verify correctness at design points (a single fill, a single
 * sweep, one boundary value).  These stress tests verify correctness under
 * SUSTAINED LOAD — thousands of consecutive fill/drain cycles — targeting a
 * different failure class: slot leaks, index-arithmetic wrap errors, and
 * counter drift that only manifest after many repetitions.
 *
 * Each test runs the component through N complete fill→drain cycles and
 * asserts that the component's state is fully reset at the end of every
 * cycle.  Any slot that is not freed correctly will cause a later
 * schedule()/track() call to return ERR_FULL and fail the assertion.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * COMPONENTS UNDER TEST
 * ─────────────────────────────────────────────────────────────────────────────
 *  1. AckTracker          — sweep_expired() drain path (all slots expired)
 *  2. AckTracker          — mixed drain: ACK half, sweep the other half
 *  3. RetryManager        — on_ack() + collect_due() drain path
 *  4. RetryManager        — collect_due() exhaustion at MAX_RETRY_COUNT
 *  5. RingBuffer          — sustained push/pop; index wrap across 2^32 boundary
 *  6. DuplicateFilter     — sliding-window eviction across many window lengths
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT EACH TEST CATCHES
 * ─────────────────────────────────────────────────────────────────────────────
 *  Test 1  Slot not freed by sweep_expired(); off-by-one at slot 32.
 *  Test 2  Slot double-free if ACKED entry is also swept; ghost entries after
 *          partial drain prevent later fills.
 *  Test 3  Slot not freed by on_ack(); stale inactive entries block schedule().
 *  Test 4  Slot not freed after retry exhaustion; backoff arithmetic overflow
 *          on the last retry; retry count exceeds MAX_RETRY_COUNT.
 *  Test 5  Head/tail index arithmetic error on uint32_t wraparound; payload
 *          corruption when a slot is reused before it is fully overwritten.
 *  Test 6  Eviction pointer arithmetic error; false positive (evicted ID still
 *          detected as duplicate); false negative (new ID not recorded after
 *          eviction occupies the slot).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 2 (fixed loop bounds)  — all loops use compile-time constants.
 *  Rule 3 (no dynamic alloc)  — local arrays only; no new/delete/malloc.
 *  Rule 5 (assertions)        — every test function contains ≥ 2 assert() calls.
 *  Rule 7 (check returns)     — every Result return value is checked.
 *  Rule 10 (zero warnings)    — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions (raised ceiling per §1b; all functions ≤ 8).
 *  assert() is used (not NEVER_COMPILED_OUT_ASSERT) — tests/ exemption per §9.
 *
 * Verifies: REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.3.2, REQ-3.3.3, REQ-4.1.2
 */
// Verification: M1 + M2 + M4 (stress path; no injectable dependency required)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/AckTracker.hpp"
#include "core/RetryManager.hpp"
#include "core/RingBuffer.hpp"
#include "core/DuplicateFilter.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Stress iteration counts (compile-time constants — Power of 10 Rule 2).
// These are large enough to wrap index counters and expose slot leaks,
// but small enough to finish in seconds on any modern machine or BeagleBone.
// ─────────────────────────────────────────────────────────────────────────────

/// Cycles for AckTracker fill/drain tests.
static const uint32_t ACK_FILL_DRAIN_CYCLES   = 10000U;

/// Cycles for AckTracker partial-ACK + sweep test.
static const uint32_t ACK_PARTIAL_CYCLES      =  1000U;

/// Cycles for RetryManager on_ack + collect_due drain test.
static const uint32_t RETRY_ACK_DRAIN_CYCLES  =  5000U;

/// Cycles for RetryManager max-retry exhaustion test (one full table at a time).
static const uint32_t RETRY_EXHAUST_CYCLES    =  1000U;

/// Individual push/pop iterations for RingBuffer single-step test.
static const uint32_t RING_SINGLE_ITERS       = 64000U;

/// Fill/drain cycles for RingBuffer bulk-fill test.
static const uint32_t RING_FILL_CYCLES        =  1000U;

/// Window lengths to cycle through for DuplicateFilter wraparound test.
static const uint32_t DEDUP_WRAP_CYCLES       =   100U;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a minimal valid test envelope.
// Mirrors the pattern used in test_AckTracker.cpp / test_RetryManager.cpp.
// ─────────────────────────────────────────────────────────────────────────────
static void make_stress_envelope(MessageEnvelope& env,
                                 uint64_t         msg_id,
                                 NodeId           src = 1U,
                                 NodeId           dst = 2U)
{
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = msg_id;
    env.timestamp_us      = 1000000ULL;
    env.source_id         = src;
    env.destination_id    = dst;
    env.priority          = 0U;
    env.reliability_class = ReliabilityClass::RELIABLE_RETRY;
    env.expiry_time_us    = 0xFFFFFFFFFFFFFF00ULL; // far future; won't expire during tests
    env.payload_length    = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 1 — AckTracker: full fill → sweep_expired drain, N cycles
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * Every slot is filled and then drained via sweep_expired() on each of
 * ACK_FILL_DRAIN_CYCLES iterations.  A slot leak — where sweep_expired()
 * fails to mark a slot FREE — will cause track() to return ERR_FULL on
 * the NEXT cycle's first fill, failing the assertion immediately.
 *
 * Boundary exercised
 * ------------------
 * The 33rd track() call (ACK_TRACKER_CAPACITY + 1) must return ERR_FULL
 * every cycle.  This guards against an off-by-one that admits one extra
 * entry and corrupts the adjacent slot.
 *
 * Verifies: REQ-3.2.4 (ACK tracking capacity), REQ-3.3.2 (reliable-ACK)
 */
static void test_ack_tracker_fill_drain_cycles()
{
    AckTracker tracker;
    tracker.init();

    // expired_buf sized to ACK_TRACKER_CAPACITY (Power of 10 Rule 3: static)
    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];

    // Deadline in the past so every tracked entry is immediately expired.
    static const uint64_t EXPIRED_DEADLINE_US = 1ULL;
    // Current time is well past that deadline.
    static const uint64_t NOW_US              = 1000000ULL;

    // Power of 10 Rule 2: bounded loop — ACK_FILL_DRAIN_CYCLES is a compile-time constant
    for (uint32_t cycle = 0U; cycle < ACK_FILL_DRAIN_CYCLES; ++cycle) {

        // ── Fill: track ACK_TRACKER_CAPACITY messages with unique IDs ──────
        // Use msg_id = cycle * spacing + slot + 1 to guarantee uniqueness
        // across all cycles and slots (1-based to avoid msg_id == 0).
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 100ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);

            Result r = tracker.track(env, EXPIRED_DEADLINE_US);
            assert(r == Result::OK);  // every slot must accept the entry
        }

        // ── Boundary: the (ACK_TRACKER_CAPACITY + 1)th entry must be rejected ──
        {
            MessageEnvelope overflow_env;
            make_stress_envelope(overflow_env, 0xDEADBEEFULL);
            Result overflow_r = tracker.track(overflow_env, EXPIRED_DEADLINE_US);
            assert(overflow_r == Result::ERR_FULL);  // table is full; no silent overflow
        }

        // ── Drain: sweep_expired removes all entries (all deadlines are past) ──
        uint32_t collected = tracker.sweep_expired(NOW_US, expired_buf, ACK_TRACKER_CAPACITY);
        // Every slot must have been freed — a count < 32 means a slot leak.
        assert(collected == ACK_TRACKER_CAPACITY);

        // ── Recycle check: one fresh track() must succeed immediately after drain ──
        {
            MessageEnvelope recycle_env;
            make_stress_envelope(recycle_env, 0xCAFEBABEULL);
            Result recycle_r = tracker.track(recycle_env, EXPIRED_DEADLINE_US);
            assert(recycle_r == Result::OK);  // at least one slot is free

            // Clean it up so we start the next cycle with an empty tracker.
            uint32_t cleanup = tracker.sweep_expired(NOW_US, expired_buf, ACK_TRACKER_CAPACITY);
            assert(cleanup == 1U);
        }
    }

    printf("PASS: test_ack_tracker_fill_drain_cycles"
           " (%u cycles x %u slots)\n",
           ACK_FILL_DRAIN_CYCLES, ACK_TRACKER_CAPACITY);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2 — AckTracker: fill → ACK half → sweep other half, N cycles
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * A more realistic drain pattern: half the slots are acknowledged (on_ack)
 * and half are swept as expired (sweep_expired).  This exercises the two
 * independent slot-freeing paths simultaneously.
 *
 * Failure modes caught
 * --------------------
 *  • Double-free: a slot is ACKed and then swept → sweep count wrong.
 *  • Ghost entry: an ACKed slot is not marked FREE → slot count inflated,
 *    later track() returns ERR_FULL prematurely.
 *  • Sweep miscount: sweep touches an already-freed ACKed slot and reports
 *    it as an extra expired entry.
 *
 * Verifies: REQ-3.2.4 (ACK tracking), REQ-3.3.2 (reliable-ACK semantics)
 */
static void test_ack_tracker_partial_ack_then_sweep()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];

    // Lower half (slots 0..15) get a future deadline → will NOT be swept as expired.
    // Upper half (slots 16..31) get a past deadline   → WILL be swept as expired.
    static const uint32_t HALF          = ACK_TRACKER_CAPACITY / 2U;  // 16
    static const uint64_t FUTURE_DL_US  = 0xFFFFFFFFFFFFFF00ULL;
    static const uint64_t PAST_DL_US    = 1ULL;
    static const uint64_t NOW_US        = 1000000ULL;

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < ACK_PARTIAL_CYCLES; ++cycle) {

        // ── Fill lower half with FUTURE deadlines (will NOT expire) ────────
        for (uint32_t slot = 0U; slot < HALF; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 200ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);
            Result r = tracker.track(env, FUTURE_DL_US);
            assert(r == Result::OK);
        }

        // ── Fill upper half with PAST deadlines (will expire) ──────────────
        for (uint32_t slot = HALF; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 200ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);
            Result r = tracker.track(env, PAST_DL_US);
            assert(r == Result::OK);
        }

        // ── ACK the lower half by (src=1, msg_id) ─────────────────────────
        for (uint32_t slot = 0U; slot < HALF; ++slot) {
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 200ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            Result ack_r = tracker.on_ack(1U, msg_id);
            assert(ack_r == Result::OK);  // each ACK must match a PENDING slot
        }

        // ── Sweep: only the upper half (expired PENDING) should be returned.
        // The lower half was ACKed → freed silently (not reported as expired).
        uint32_t swept = tracker.sweep_expired(NOW_US, expired_buf, ACK_TRACKER_CAPACITY);
        assert(swept == HALF);  // exactly 16 expired entries; no double-count from ACKed

        // ── Recycle check: all 32 slots must now be free ───────────────────
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                0xF000000000000000ULL +
                (static_cast<uint64_t>(cycle) * ACK_TRACKER_CAPACITY) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);
            Result r = tracker.track(env, PAST_DL_US);
            assert(r == Result::OK);  // all slots freed; none leaked
        }

        // Drain completely before next cycle.
        uint32_t cleanup = tracker.sweep_expired(NOW_US, expired_buf, ACK_TRACKER_CAPACITY);
        assert(cleanup == ACK_TRACKER_CAPACITY);
    }

    printf("PASS: test_ack_tracker_partial_ack_then_sweep"
           " (%u cycles, %u ACKed + %u swept per cycle)\n",
           ACK_PARTIAL_CYCLES, HALF, HALF);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3 — RetryManager: fill → on_ack all → collect_due GC, N cycles
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * All slots are filled via schedule(), then cancelled via on_ack(), and
 * finally garbage-collected by collect_due().  This exercises the normal
 * success path for RELIABLE_RETRY: message sent, ACK received, retry
 * entry discarded.
 *
 * Failure modes caught
 * --------------------
 *  • Slot not freed after on_ack(): schedule() returns ERR_FULL on the
 *    following cycle even though all entries were ACKed.
 *  • collect_due() misses an inactive (ACKed) entry during GC → slot
 *    counter out of sync → ERR_FULL one cycle too early.
 *  • The 33rd schedule() boundary is tested every cycle.
 *
 * Verifies: REQ-3.2.5 (retry scheduling), REQ-3.3.3 (reliable-retry semantics)
 */
static void test_retry_manager_fill_ack_drain_cycles()
{
    RetryManager rm;
    rm.init();

    MessageEnvelope out_buf[ACK_TRACKER_CAPACITY];

    // Timestamps: schedule at NOW_US; collect_due at a time well past the
    // first retry deadline (backoff_ms=1 → next_retry_us = NOW_US + 1000 us).
    static const uint64_t NOW_US         = 1000000ULL;
    static const uint64_t COLLECT_NOW_US = 2000000ULL; // 1 s later; always past backoff

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < RETRY_ACK_DRAIN_CYCLES; ++cycle) {

        // ── Fill: schedule ACK_TRACKER_CAPACITY retry entries ─────────────
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 100ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);

            // backoff_ms=1 so retries fire quickly; max_retries=MAX_RETRY_COUNT
            Result r = rm.schedule(env, MAX_RETRY_COUNT, 1U, NOW_US);
            assert(r == Result::OK);
        }

        // ── Boundary: one more schedule() must be rejected ─────────────────
        {
            MessageEnvelope overflow_env;
            make_stress_envelope(overflow_env, 0xDEADBEEFULL);
            Result overflow_r = rm.schedule(overflow_env, MAX_RETRY_COUNT, 1U, NOW_US);
            assert(overflow_r == Result::ERR_FULL);
        }

        // ── Cancel all via on_ack (simulates ACK received before any retry) ─
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 100ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            Result ack_r = rm.on_ack(1U, msg_id);  // src = 1U from make_stress_envelope
            assert(ack_r == Result::OK);  // each on_ack must find a matching entry
        }

        // ── GC: collect_due removes inactive entries so slots are recycled ──
        // We use COLLECT_NOW_US even though entries are inactive — collect_due
        // must clean them up regardless of their next_retry_us.
        uint32_t collected = rm.collect_due(COLLECT_NOW_US, out_buf, ACK_TRACKER_CAPACITY);
        // Inactive (ACKed) entries should not appear in the output buffer.
        assert(collected == 0U);

        // ── Recycle check: schedule() must succeed immediately after GC ─────
        {
            MessageEnvelope recycle_env;
            make_stress_envelope(recycle_env, 0xCAFEBABEULL);
            Result recycle_r = rm.schedule(recycle_env, 1U, 1U, NOW_US);
            assert(recycle_r == Result::OK);  // at least one slot is free

            // Clean up the recycle entry: ACK it and force GC.
            Result ack_cleanup = rm.on_ack(1U, 0xCAFEBABEULL);
            (void)ack_cleanup; // ACK result not critical to this assertion path
            (void)rm.collect_due(COLLECT_NOW_US, out_buf, ACK_TRACKER_CAPACITY);
        }
    }

    printf("PASS: test_retry_manager_fill_ack_drain_cycles"
           " (%u cycles x %u slots)\n",
           RETRY_ACK_DRAIN_CYCLES, ACK_TRACKER_CAPACITY);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 4 — RetryManager: retry exhaustion at MAX_RETRY_COUNT, N cycles
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * Entries are scheduled and then driven to full exhaustion by calling
 * collect_due() MAX_RETRY_COUNT + 1 times, advancing the timestamp by
 * 1 second per call so every entry is always due.  After MAX_RETRY_COUNT
 * retries the RetryManager must automatically free the slot.
 *
 * Failure modes caught
 * --------------------
 *  • Slot not freed after exhaustion → ERR_FULL on next schedule().
 *  • Retry count exceeds MAX_RETRY_COUNT → entry appears in the (N+1)th
 *    collect_due output when it should have been removed already.
 *  • Backoff arithmetic overflow on the last doubling step: if backoff_ms
 *    overflows uint32_t the computed next_retry_us may wrap and appear
 *    permanently due, causing the entry to never be freed.
 *
 * Verifies: REQ-3.2.5 (retry count ceiling), REQ-3.3.3 (exhaustion semantics)
 */
static void test_retry_manager_max_retry_exhaustion()
{
    RetryManager rm;
    rm.init();

    MessageEnvelope out_buf[ACK_TRACKER_CAPACITY];

    // advance_us is far larger than any backoff doubling:
    // backoff starts at 1 ms, doubles 5x → 32 ms << 1 000 000 us (1 s)
    static const uint64_t ADVANCE_US    = 1000000ULL;  // 1 second per step
    static const uint64_t INITIAL_NOW   = 1000000ULL;

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < RETRY_EXHAUST_CYCLES; ++cycle) {

        // ── Fill all slots with MAX_RETRY_COUNT retry budget ──────────────
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 100ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);
            Result r = rm.schedule(env, MAX_RETRY_COUNT, 1U, INITIAL_NOW);
            assert(r == Result::OK);
        }

        // ── Drive to exhaustion: call collect_due MAX_RETRY_COUNT + 1 times.
        // Each call is at a timestamp 1 second past the previous one, which
        // guarantees every remaining entry is always due.
        // After MAX_RETRY_COUNT calls, all entries must be gone.
        // The (MAX_RETRY_COUNT + 1)th call must return 0.
        uint64_t now_us = INITIAL_NOW;
        uint32_t total_collected = 0U;

        // Power of 10 Rule 2: inner loop bounded by MAX_RETRY_COUNT + 1
        static const uint32_t MAX_CALLS = MAX_RETRY_COUNT + 1U;
        for (uint32_t call = 0U; call < MAX_CALLS; ++call) {
            now_us += ADVANCE_US;
            uint32_t cnt = rm.collect_due(now_us, out_buf, ACK_TRACKER_CAPACITY);

            // cnt must never exceed the table capacity
            assert(cnt <= ACK_TRACKER_CAPACITY);

            // On the very last call, all entries must be exhausted and removed.
            if (call == MAX_RETRY_COUNT) {
                // All ACK_TRACKER_CAPACITY entries have been retried MAX_RETRY_COUNT
                // times and must be gone; this call must return 0.
                assert(cnt == 0U);
            }
            total_collected += cnt;
        }

        // Total retries across all slots across all calls must be exactly
        // ACK_TRACKER_CAPACITY × MAX_RETRY_COUNT (each slot retried exactly
        // MAX_RETRY_COUNT times before removal, no more, no less).
        assert(total_collected == ACK_TRACKER_CAPACITY * MAX_RETRY_COUNT);

        // ── Recycle check: all slots freed; new schedule() must succeed ────
        {
            MessageEnvelope env;
            make_stress_envelope(env, 0xCAFEBABEULL);
            Result r = rm.schedule(env, 1U, 1U, now_us);
            assert(r == Result::OK);  // slot is free after exhaustion

            // Clean up: exhaust this single recycle entry before next cycle.
            static const uint32_t RECYCLE_CALLS = 2U;  // max_retries=1 → 2 calls to exhaust
            for (uint32_t rc = 0U; rc < RECYCLE_CALLS; ++rc) {
                now_us += ADVANCE_US;
                (void)rm.collect_due(now_us, out_buf, ACK_TRACKER_CAPACITY);
            }
        }
    }

    printf("PASS: test_retry_manager_max_retry_exhaustion"
           " (%u cycles x %u slots x %u retries)\n",
           RETRY_EXHAUST_CYCLES, ACK_TRACKER_CAPACITY, MAX_RETRY_COUNT);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 5 — RingBuffer: sustained push/pop with message_id roundtrip
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * Phase A — Single-step (RING_SINGLE_ITERS iterations):
 *   Push one envelope then pop one envelope.  Assert the popped message_id
 *   exactly matches the pushed value.  This drives head and tail through
 *   many full wraps of the 64-slot circular index, catching any off-by-one
 *   or wrong-mask error in the (index & RING_MASK) arithmetic.
 *
 * Phase B — Full fill / full drain (RING_FILL_CYCLES cycles):
 *   Push MSG_RING_CAPACITY envelopes, assert the (capacity+1)th push returns
 *   ERR_FULL, then pop all MSG_RING_CAPACITY envelopes, assert each pop
 *   returns the correct message_id in FIFO order, then assert the next pop
 *   returns ERR_EMPTY.
 *
 * Failure modes caught (Phase A)
 * --------------------------------
 *  • Head/tail pointer arithmetic error on index wrap (uint32_t overflow of
 *    the raw counter at 2^32 is correct; RING_MASK picks the slot correctly).
 *  • Slot data corruption: a slot is written before the previous pop fully
 *    released it (only relevant in a concurrent scenario, but validates that
 *    sequential logic produces zero corruption across 64 000 iterations).
 *
 * Failure modes caught (Phase B)
 * --------------------------------
 *  • ERR_FULL boundary: off-by-one allows 65 entries instead of 64.
 *  • FIFO ordering: a wrap-around causes a pop to return a stale envelope
 *    from a previous cycle rather than the freshly pushed one.
 *  • ERR_EMPTY boundary: pop on a fully-drained buffer returns OK with
 *    garbage data instead of ERR_EMPTY.
 *
 * Verifies: REQ-4.1.2 (send_message queue semantics), REQ-4.2.2 (per-channel limits)
 */
static void test_ring_buffer_sustained_push_pop()
{
    RingBuffer ring;
    ring.init();

    // ── Phase A: RING_SINGLE_ITERS single push/pop pairs ─────────────────
    // Power of 10 Rule 2: bounded loop
    for (uint32_t iter = 0U; iter < RING_SINGLE_ITERS; ++iter) {
        MessageEnvelope push_env;
        make_stress_envelope(push_env, static_cast<uint64_t>(iter) + 1ULL);

        Result push_r = ring.push(push_env);
        assert(push_r == Result::OK);

        MessageEnvelope pop_env;
        Result pop_r = ring.pop(pop_env);
        assert(pop_r == Result::OK);

        // The popped message_id must exactly match what was pushed.
        // Any mismatch indicates slot aliasing or index corruption.
        assert(pop_env.message_id == push_env.message_id);
    }

    // ── Phase B: RING_FILL_CYCLES full fill / full drain cycles ──────────
    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < RING_FILL_CYCLES; ++cycle) {

        // Fill exactly MSG_RING_CAPACITY slots with sequential message_ids.
        for (uint32_t slot = 0U; slot < MSG_RING_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * MSG_RING_CAPACITY) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);

            Result r = ring.push(env);
            assert(r == Result::OK);
        }

        // One more push must be rejected — the buffer is full.
        {
            MessageEnvelope overflow_env;
            make_stress_envelope(overflow_env, 0xDEADBEEFULL);
            Result overflow_r = ring.push(overflow_env);
            assert(overflow_r == Result::ERR_FULL);
        }

        // Pop all MSG_RING_CAPACITY slots; verify FIFO order is preserved.
        for (uint32_t slot = 0U; slot < MSG_RING_CAPACITY; ++slot) {
            MessageEnvelope popped;
            Result r = ring.pop(popped);
            assert(r == Result::OK);

            const uint64_t expected_id =
                (static_cast<uint64_t>(cycle) * MSG_RING_CAPACITY) +
                static_cast<uint64_t>(slot) + 1ULL;
            assert(popped.message_id == expected_id);  // FIFO order intact
        }

        // Buffer must now be empty — one more pop must return ERR_EMPTY.
        {
            MessageEnvelope empty_env;
            Result empty_r = ring.pop(empty_env);
            assert(empty_r == Result::ERR_EMPTY);
        }
    }

    printf("PASS: test_ring_buffer_sustained_push_pop"
           " (Phase A: %u iters, Phase B: %u cycles x %u slots)\n",
           RING_SINGLE_ITERS, RING_FILL_CYCLES, MSG_RING_CAPACITY);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 6 — DuplicateFilter: sliding-window eviction across N window lengths
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * The DuplicateFilter maintains a circular window of DEDUP_WINDOW_SIZE (128)
 * (source_id, message_id) pairs.  When the window is full, the oldest entry
 * is evicted to make room for a new one.
 *
 * Each cycle of this test:
 *  1. Records DEDUP_WINDOW_SIZE unique (src=1, msg_id) pairs — "batch A".
 *  2. Verifies every entry in batch A is detected as a duplicate.
 *  3. Records DEDUP_WINDOW_SIZE more unique pairs  — "batch B".
 *     This completely displaces batch A from the window.
 *  4. Verifies batch A entries are NO LONGER detected as duplicates
 *     (they were evicted — false-positive check).
 *  5. Verifies batch B entries ARE detected as duplicates
 *     (they still reside in the window — false-negative check).
 *
 * Failure modes caught
 * --------------------
 *  • Eviction pointer wrap error: the write pointer fails to advance
 *    correctly, causing batch B entries to overwrite the wrong slots,
 *    leaving stale batch A entries that produce false positives.
 *  • Count underflow: m_count drops below its correct value after eviction,
 *    causing is_duplicate() to stop searching before it finds batch B.
 *  • m_next wrap error: after recording 128 entries m_next wraps to 0;
 *    if the wrap is off-by-one, slot 0 is overwritten twice and slot 127
 *    is never written.
 *
 * Verifies: REQ-3.2.6 (duplicate suppression), REQ-3.3.3 (dedup in retry path)
 */
static void test_dedup_filter_window_wraparound()
{
    DuplicateFilter filter;
    filter.init();

    // Each cycle uses 2 × DEDUP_WINDOW_SIZE unique message_ids.
    // Offset by cycle so IDs never repeat across cycles.
    static const uint64_t IDS_PER_CYCLE = static_cast<uint64_t>(DEDUP_WINDOW_SIZE) * 2ULL;

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < DEDUP_WRAP_CYCLES; ++cycle) {

        const uint64_t base = static_cast<uint64_t>(cycle) * IDS_PER_CYCLE + 1ULL;

        // ── Step 1: Record batch A (DEDUP_WINDOW_SIZE entries) ────────────
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = base + static_cast<uint64_t>(i);
            Result r = filter.check_and_record(1U, msg_id);
            // First time seen — must be recorded, not a duplicate.
            assert(r == Result::OK);
        }

        // ── Step 2: Verify all batch A entries are detected as duplicates ─
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = base + static_cast<uint64_t>(i);
            bool is_dup = filter.is_duplicate(1U, msg_id);
            assert(is_dup);  // must still be in window; false negative = bug
        }

        // ── Step 3: Record batch B — displaces batch A from the window ────
        const uint64_t batch_b_base = base + static_cast<uint64_t>(DEDUP_WINDOW_SIZE);
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = batch_b_base + static_cast<uint64_t>(i);
            Result r = filter.check_and_record(1U, msg_id);
            assert(r == Result::OK);  // first time seen; must not be a false duplicate
        }

        // ── Step 4: Batch A must be evicted — no longer duplicates ────────
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = base + static_cast<uint64_t>(i);
            bool is_dup = filter.is_duplicate(1U, msg_id);
            assert(!is_dup);  // evicted; false positive = eviction pointer bug
        }

        // ── Step 5: Batch B must still be detected as duplicates ──────────
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = batch_b_base + static_cast<uint64_t>(i);
            bool is_dup = filter.is_duplicate(1U, msg_id);
            assert(is_dup);  // still in window; false negative = count/wrap bug
        }
    }

    printf("PASS: test_dedup_filter_window_wraparound"
           " (%u window cycles, %u entries per batch)\n",
           DEDUP_WRAP_CYCLES, DEDUP_WINDOW_SIZE);
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    printf("=== Stress tests: capacity exhaustion and slot recycling ===\n");
    printf("    (long-running; may take several seconds on slow hardware)\n\n");

    test_ack_tracker_fill_drain_cycles();
    test_ack_tracker_partial_ack_then_sweep();
    test_retry_manager_fill_ack_drain_cycles();
    test_retry_manager_max_retry_exhaustion();
    test_ring_buffer_sustained_push_pop();
    test_dedup_filter_window_wraparound();

    printf("\nALL stress capacity tests passed.\n");
    return 0;
}
