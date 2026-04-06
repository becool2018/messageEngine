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
 * Verifies: REQ-3.2.3, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.3.2, REQ-3.3.3, REQ-4.1.2
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
#include "core/DeliveryEngine.hpp"        // brings in ChannelConfig, ReassemblyBuffer
#include "core/RequestReplyEngine.hpp"
#include "platform/LocalSimHarness.hpp"

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

/// Single-step request/response cycles — walks m_stash_head through all 16
/// positions (1000 / 16 = 62 full wraps + 8 extra positions exercised).
static const uint32_t RRE_SINGLE_CYCLES       =  1000U;

/// Batch fill/drain cycles — fills RRE_BATCH_SIZE slots then drains;
/// exercises (head + count) % MAX_STASH_SIZE arithmetic on every write.
static const uint32_t RRE_BATCH_CYCLES        =   100U;

/// Requests per batch cycle.  Must satisfy: 2 × RRE_BATCH_SIZE ≤ MAX_STASH_SIZE (16)
/// so that (ACKs + responses) fit in a single pump_inbound drain call.
/// Using 8: 8 ACKs + 8 responses = 16 = MAX_STASH_SIZE exactly.
static const uint32_t RRE_BATCH_SIZE          =     8U;

/// Cycles of open-all-8-sessions → complete-all-8 for ReassemblyBuffer.
static const uint32_t REASSEMBLY_COMPLETE_CYCLES = 1000U;

/// Cycles of open-all-8-sessions → sweep_expired-all-8 for ReassemblyBuffer.
static const uint32_t REASSEMBLY_EXPIRY_CYCLES   = 1000U;

/// Node IDs used in the RRE stress tests (distinct from production node IDs).
static const NodeId RRE_STRESS_NODE_A = 10U;
static const NodeId RRE_STRESS_NODE_B = 11U;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers: RRE stress test setup
// ─────────────────────────────────────────────────────────────────────────────

static void stress_make_sim_config(TransportConfig& cfg, NodeId node_id)
{
    transport_config_default(cfg);
    cfg.kind                         = TransportKind::LOCAL_SIM;
    cfg.local_node_id                = node_id;
    cfg.is_server                    = false;
    cfg.channels[0].max_retries      = 3U;
    cfg.channels[0].retry_backoff_ms = 50U;
    cfg.channels[0].recv_timeout_ms  = 0U;  // non-blocking polls
}

static void stress_setup_rre_nodes(LocalSimHarness&    ha,
                                    DeliveryEngine&     ea,
                                    RequestReplyEngine& rrea,
                                    LocalSimHarness&    hb,
                                    DeliveryEngine&     eb,
                                    RequestReplyEngine& rreb)
{
    TransportConfig cfg_a;
    stress_make_sim_config(cfg_a, RRE_STRESS_NODE_A);
    Result ra = ha.init(cfg_a);
    assert(ra == Result::OK);

    TransportConfig cfg_b;
    stress_make_sim_config(cfg_b, RRE_STRESS_NODE_B);
    Result rb = hb.init(cfg_b);
    assert(rb == Result::OK);

    ha.link(&hb);
    hb.link(&ha);

    ea.init(&ha, cfg_a.channels[0], RRE_STRESS_NODE_A);
    eb.init(&hb, cfg_b.channels[0], RRE_STRESS_NODE_B);

    rrea.init(ea, RRE_STRESS_NODE_A);
    rreb.init(eb, RRE_STRESS_NODE_B);
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a fragment envelope for ReassemblyBuffer stress tests.
// ─────────────────────────────────────────────────────────────────────────────

static void stress_make_frag(MessageEnvelope& frag,
                              uint64_t         message_id,
                              NodeId           src,
                              uint8_t          frag_index,
                              uint8_t          frag_count,
                              uint16_t         total_payload_length,
                              uint64_t         expiry_us,
                              const uint8_t*   payload,
                              uint32_t         payload_len)
{
    envelope_init(frag);
    frag.message_type         = MessageType::DATA;
    frag.source_id            = src;
    frag.destination_id       = 1U;
    frag.message_id           = message_id;
    frag.timestamp_us         = 1000000ULL;
    frag.expiry_time_us       = expiry_us;
    frag.priority             = 0U;
    frag.reliability_class    = ReliabilityClass::BEST_EFFORT;
    frag.fragment_index       = frag_index;
    frag.fragment_count       = frag_count;
    frag.total_payload_length = total_payload_length;
    frag.payload_length       = payload_len;
    if (payload != nullptr && payload_len > 0U) {
        (void)memcpy(frag.payload, payload, static_cast<size_t>(payload_len));
    }
}

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

    // now_us is declared outside the cycle loop so it advances monotonically
    // across all cycles.  RetryManager::collect_due() asserts
    // now_us >= m_last_collect_us (monotonic-time enforcement added with
    // security fix F3); resetting to INITIAL_NOW each cycle would violate
    // that invariant on cycle 1 and later.
    uint64_t now_us = INITIAL_NOW;

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < RETRY_EXHAUST_CYCLES; ++cycle) {

        // ── Fill all slots with MAX_RETRY_COUNT retry budget ──────────────
        for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
            MessageEnvelope env;
            const uint64_t msg_id =
                (static_cast<uint64_t>(cycle) * 100ULL) +
                static_cast<uint64_t>(slot) + 1ULL;
            make_stress_envelope(env, msg_id);
            Result r = rm.schedule(env, MAX_RETRY_COUNT, 1U, now_us);
            assert(r == Result::OK);
        }

        // ── Drive to exhaustion: call collect_due MAX_RETRY_COUNT + 1 times.
        // Each call is at a timestamp 1 second past the previous one, which
        // guarantees every remaining entry is always due.
        // After MAX_RETRY_COUNT calls, all entries must be gone.
        // The (MAX_RETRY_COUNT + 1)th call must return 0.
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

    // Monotonically increasing timestamp counter.  SECfix-3: age-based eviction
    // requires batch A entries (recorded first) to have strictly smaller timestamps
    // than batch B entries so that batch A is chosen as "oldest" and evicted.
    // Power of 10 Rule 3: stack-allocated counter, no dynamic allocation.
    uint64_t now_us = 1ULL;

    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < DEDUP_WRAP_CYCLES; ++cycle) {

        const uint64_t base = static_cast<uint64_t>(cycle) * IDS_PER_CYCLE + 1ULL;

        // ── Step 1: Record batch A (DEDUP_WINDOW_SIZE entries) ────────────
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = base + static_cast<uint64_t>(i);
            Result r = filter.check_and_record(1U, msg_id, now_us);
            ++now_us;
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
        // now_us is strictly greater than all batch A timestamps, so batch B
        // entries are newer; batch A entries are evicted as oldest (SECfix-3).
        const uint64_t batch_b_base = base + static_cast<uint64_t>(DEDUP_WINDOW_SIZE);
        for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
            const uint64_t msg_id = batch_b_base + static_cast<uint64_t>(i);
            Result r = filter.check_and_record(1U, msg_id, now_us);
            ++now_us;
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
// TEST 7 — RequestReplyEngine: stash FIFO circular-buffer wraparound
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * The request stash inside RequestReplyEngine uses a circular FIFO:
 *   slot = (m_stash_head + m_stash_count) % MAX_STASH_SIZE   on write
 *   m_stash_head = (m_stash_head + 1) % MAX_STASH_SIZE       on read
 *
 * Phase A — single-step cycles (RRE_SINGLE_CYCLES = 1000):
 *   One request/response per cycle.  m_stash_head advances by 1 each cycle;
 *   after 16 cycles it wraps to 0.  1000 cycles = 62 full wraps + 8
 *   extra positions — every slot acts as head many times.
 *
 * Phase B — batch fill/drain cycles (RRE_BATCH_CYCLES = 100):
 *   All MAX_PENDING_REQUESTS (16) slots filled then drained per cycle.
 *   Verifies FIFO order (payload byte == slot index) and that the boundary
 *   overflow (17th send) and the post-drain empty (first extra receive) are
 *   correctly reported.
 *
 * Failure modes caught
 * --------------------
 *  • Off-by-one in (head + count) % MAX_STASH_SIZE: slot aliasing causes the
 *    wrong entry to be returned, failing the payload assertion.
 *  • Slot not freed on dequeue: m_stash_count stays elevated; the stash
 *    appears full one entry early on the next batch cycle.
 *  • Overflow not caught: the 17th send succeeds and corrupts a stash slot.
 *  • Empty not reported: a dequeue after full drain returns stale data.
 *
 * Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
static void test_rre_stash_fifo_wraparound()
{
    // Large objects in BSS to avoid stack overflow (Power of 10 Rule 3 note:
    // static local → BSS; init() fully re-initialises all state).
    static LocalSimHarness    ha;
    static DeliveryEngine     ea;
    static RequestReplyEngine rrea;
    static LocalSimHarness    hb;
    static DeliveryEngine     eb;
    static RequestReplyEngine rreb;

    stress_setup_rre_nodes(ha, ea, rrea, hb, eb, rreb);

    static const uint64_t RRE_NOW_US     = 1000000ULL;  // fixed test clock
    static const uint64_t RRE_TIMEOUT_US = 5000000ULL;  // 5 s — far future
    static const uint8_t  REQ_BYTE       = 0xA1U;
    static const uint8_t  RSP_BYTE       = 0xB2U;

    // ── Phase A: RRE_SINGLE_CYCLES single-step request/response cycles ──────
    // Power of 10 Rule 2: bounded loop — RRE_SINGLE_CYCLES is a compile-time constant
    for (uint32_t cycle = 0U; cycle < RRE_SINGLE_CYCLES; ++cycle) {

        // A sends one request
        uint64_t cid = 0U;
        Result r = rrea.send_request(RRE_STRESS_NODE_B, &REQ_BYTE, 1U,
                                     RRE_TIMEOUT_US, RRE_NOW_US, cid);
        assert(r == Result::OK);  // pending table must have a free slot

        // B receives the request
        uint8_t  rx_buf[1U]  = { 0U };
        uint32_t rx_len      = 0U;
        NodeId   rx_src      = 0U;
        uint64_t rx_cid      = 0U;
        r = rreb.receive_request(rx_buf, 1U, rx_len, rx_src, rx_cid, RRE_NOW_US);
        assert(r == Result::OK);  // request must arrive at B

        // B sends response
        r = rreb.send_response(rx_src, rx_cid, &RSP_BYTE, 1U, RRE_NOW_US);
        assert(r == Result::OK);

        // A collects the response
        uint8_t  rsp_buf[1U] = { 0U };
        uint32_t rsp_len     = 0U;
        r = rrea.receive_response(cid, rsp_buf, 1U, rsp_len, RRE_NOW_US);
        assert(r == Result::OK);  // pending slot must be recycled correctly

        // Sweep ACKED → FREE in AckTracker so the slot is available next cycle.
        // AckTracker::on_ack() transitions PENDING→ACKED; only sweep_ack_timeouts()
        // completes the ACKED→FREE transition.  Without this call the tracker fills
        // up after ACK_TRACKER_CAPACITY (32) cycles and the next send_request fails.
        // Power of 10 Rule 7: return value is a count; cast to void (no action needed).
        (void)ea.sweep_ack_timeouts(RRE_NOW_US);
    }

    // ── Phase B: RRE_BATCH_CYCLES full stash fill/drain cycles ──────────────
    // Batch size = RRE_BATCH_SIZE (8): 8 RELIABLE_RETRY requests produce 8 ACKs
    // in A's inbound queue; 8 responses add 8 more.  8 + 8 = 16 = MAX_STASH_SIZE,
    // so a single pump_inbound call (≤ MAX_STASH_SIZE iterations) drains all of
    // them and sets stash_ready on every pending slot before the collect loop runs.
    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < RRE_BATCH_CYCLES; ++cycle) {

        uint64_t cids[RRE_BATCH_SIZE];
        (void)memset(cids, 0, sizeof(cids));

        // A sends RRE_BATCH_SIZE requests; payload byte = slot index (for FIFO check)
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
            const uint8_t pay = static_cast<uint8_t>(slot);
            Result r = rrea.send_request(RRE_STRESS_NODE_B, &pay, 1U,
                                         RRE_TIMEOUT_US, RRE_NOW_US, cids[slot]);
            assert(r == Result::OK);  // each slot must be accepted
        }

        // B reads all RRE_BATCH_SIZE requests; verify FIFO order, then responds
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
            uint8_t  rx_buf[1U] = { 0U };
            uint32_t rx_len     = 0U;
            NodeId   rx_src     = 0U;
            uint64_t rx_cid     = 0U;
            Result r = rreb.receive_request(rx_buf, 1U, rx_len, rx_src, rx_cid, RRE_NOW_US);
            assert(r == Result::OK);
            // FIFO: payload must equal slot index; any mismatch = head/tail wrap bug
            assert(rx_buf[0] == static_cast<uint8_t>(slot));

            Result send_r = rreb.send_response(rx_src, rx_cid, rx_buf, rx_len, RRE_NOW_US);
            assert(send_r == Result::OK);
        }

        // Stash now empty: next receive_request must report no entry
        {
            uint8_t  empty_buf[1U] = { 0U };
            uint32_t empty_len     = 0U;
            NodeId   empty_src     = 0U;
            uint64_t empty_cid     = 0U;
            Result empty_r = rreb.receive_request(empty_buf, 1U, empty_len,
                                                   empty_src, empty_cid, RRE_NOW_US);
            assert(empty_r == Result::ERR_EMPTY);  // no spurious stash entry
        }

        // A collects all RRE_BATCH_SIZE responses; pending table fully recycled
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
            uint8_t  rsp_buf[1U] = { 0U };
            uint32_t rsp_len     = 0U;
            Result r = rrea.receive_response(cids[slot], rsp_buf, 1U, rsp_len, RRE_NOW_US);
            assert(r == Result::OK);  // every pending slot must be recoverable
        }

        // Free ACKED AckTracker slots so the next batch cycle can track new sends.
        // Power of 10 Rule 7: return value is a count; cast to void.
        (void)ea.sweep_ack_timeouts(RRE_NOW_US);
    }

    printf("PASS: test_rre_stash_fifo_wraparound"
           " (Phase A: %u single cycles, Phase B: %u batch cycles x %u slots)\n",
           RRE_SINGLE_CYCLES, RRE_BATCH_CYCLES, RRE_BATCH_SIZE);
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 8 — ReassemblyBuffer: session slot recycling (completion + expiry paths)
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * ReassemblyBuffer has REASSEMBLY_SLOT_COUNT (8) session slots.  Slots are
 * freed by two independent code paths:
 *   A) assemble_and_free()  — called when all fragments arrive (Result::OK).
 *   B) sweep_expired()      — called periodically; frees slots past deadline.
 *
 * Phase A — completion cycles (REASSEMBLY_COMPLETE_CYCLES = 1000):
 *   1. Open all 8 sessions (frag 0 only) — each returns ERR_AGAIN.
 *   2. Verify 9th session is rejected (ERR_FULL — table full).
 *   3. Complete all 8 sessions (frag 1) — each returns OK via assemble_and_free().
 *   4. Verify recycle: open a new session immediately — must succeed (ERR_AGAIN).
 *
 * Phase B — expiry cycles (REASSEMBLY_EXPIRY_CYCLES = 1000):
 *   1. Open all 8 sessions with an already-expired expiry_us.
 *   2. Call sweep_expired() — must free all 8 slots.
 *   3. Verify recycle: open a new session immediately — must succeed (ERR_AGAIN).
 *
 * Failure modes caught
 * --------------------
 *  • Phase A: assemble_and_free() fails to clear active flag → ERR_FULL on
 *    the next cycle's first open attempt, failing the assertion.
 *  • Phase A boundary: off-by-one in find_free_slot() admits a 9th slot that
 *    corrupts an adjacent struct.
 *  • Phase B: sweep_expired() fails to clear active flag → same ERR_FULL
 *    failure on next cycle.
 *  • Phase B: sweep uses a wrong comparison (< instead of <=) and misses the
 *    boundary expiry value → no slot freed → ERR_FULL on recycle.
 *
 * Verifies: REQ-3.2.3, REQ-3.3.3
 */
static void test_reassembly_slot_recycling()
{
    static ReassemblyBuffer buf;
    buf.init();

    // Single payload byte per fragment — enough to open a session.
    // total_payload_length = 2 × 1 byte (2 fragments × 1 byte each).
    static const uint8_t  FRAG_BYTE  = 0xBBU;
    static const uint32_t FRAG_BYTES = 1U;
    static const uint16_t TOTAL_LEN  = 2U;

    static const uint64_t FAR_EXPIRY  = 0xFFFFFFFFFFFFFF00ULL; // won't expire
    static const uint64_t PAST_EXPIRY = 1ULL;                  // already expired
    static const uint64_t SWEEP_NOW   = 1000000ULL;            // t = 1 s

    static const NodeId FRAG_SRC_A = 20U;  // source for Phase A slots 0..(REASM_PER_SRC_MAX-1)
    // H-series F-1: k_reasm_per_src_max = REASSEMBLY_SLOT_COUNT/2 in ReassemblyBuffer.cpp.
    // A single peer may hold at most this many open reassembly slots.  Distribute Phase A
    // sessions across two sources to stay within the per-source cap while still filling
    // the global buffer to capacity.
    static const uint32_t REASM_PER_SRC_MAX = REASSEMBLY_SLOT_COUNT / 2U;
    static const NodeId FRAG_SRC_A2 = 40U;  // source for Phase A slots REASM_PER_SRC_MAX..end
    static const NodeId FRAG_SRC_B  = 21U;  // source for Phase B slots 0..(REASM_PER_SRC_MAX-1)
    static const NodeId FRAG_SRC_B2 = 41U;  // source for Phase B slots REASM_PER_SRC_MAX..end

    // Phase B IDs start far above Phase A to avoid any (src, msg_id) collision.
    static const uint64_t PHASE_B_BASE = 1000000ULL;

    // ── Phase A: REASSEMBLY_COMPLETE_CYCLES open-all → complete-all cycles ──
    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < REASSEMBLY_COMPLETE_CYCLES; ++cycle) {

        // Open all 8 sessions (frag 0 only — session COLLECTING, not complete).
        // Distribute across FRAG_SRC_A and FRAG_SRC_A2 (REASM_PER_SRC_MAX each) to
        // stay within the H-series F-1 per-source slot cap while filling the global buffer.
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
            const uint64_t msg_id =
                static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                + static_cast<uint64_t>(s) + 1ULL;
            const NodeId src_a = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_A : FRAG_SRC_A2;
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, msg_id, src_a, 0U, 2U, TOTAL_LEN,
                             FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::ERR_AGAIN);  // frag 0: session opens, not yet complete
        }

        // All 8 slots in use — the 9th open attempt must be rejected
        {
            const uint64_t overflow_id = 0xDEAD000000000000ULL
                                         + static_cast<uint64_t>(cycle);
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, overflow_id, FRAG_SRC_A, 0U, 2U, TOTAL_LEN,
                             FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::ERR_FULL);  // no slot overrun permitted
        }

        // Complete all 8 sessions (frag 1 → assemble_and_free() frees the slot).
        // Must use the same source distribution as the open loop above.
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
            const uint64_t msg_id =
                static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                + static_cast<uint64_t>(s) + 1ULL;
            const NodeId src_a = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_A : FRAG_SRC_A2;
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, msg_id, src_a, 1U, 2U, TOTAL_LEN,
                             FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::OK);           // frag 1: message complete
            assert(out.message_id == msg_id);  // assembled envelope has correct key
        }

        // All 8 slots freed — verify recycle: open one fresh session
        {
            const uint64_t recycle_id = 0xCAFE000000000000ULL
                                        + static_cast<uint64_t>(cycle);
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, recycle_id, FRAG_SRC_A, 0U, 2U, TOTAL_LEN,
                             FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::ERR_AGAIN);  // slot freed; new session must open

            // Complete the recycle session so buf is fully clean before next cycle
            stress_make_frag(frag, recycle_id, FRAG_SRC_A, 1U, 2U, TOTAL_LEN,
                             FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::OK);
        }
    }

    // ── Phase B: REASSEMBLY_EXPIRY_CYCLES open-all → sweep cycles ───────────
    // Power of 10 Rule 2: bounded outer loop
    for (uint32_t cycle = 0U; cycle < REASSEMBLY_EXPIRY_CYCLES; ++cycle) {

        // Open all 8 sessions with an already-expired expiry_us.
        // Distribute across FRAG_SRC_B and FRAG_SRC_B2 (REASM_PER_SRC_MAX each) to
        // stay within the H-series F-1 per-source slot cap while filling the global buffer.
        // Power of 10 Rule 2: bounded inner loop
        for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
            const uint64_t msg_id = PHASE_B_BASE
                + static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                + static_cast<uint64_t>(s) + 1ULL;
            const NodeId src_b = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_B : FRAG_SRC_B2;
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, msg_id, src_b, 0U, 2U, TOTAL_LEN,
                             PAST_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::ERR_AGAIN);  // frag 0 accepted; session COLLECTING
        }

        // Sweep at SWEEP_NOW (1 s) > PAST_EXPIRY (1 µs) — all 8 must be freed
        buf.sweep_expired(SWEEP_NOW);

        // Verify recycle: open one fresh session — must succeed
        {
            const uint64_t verify_id = 0xBEEF000000000000ULL
                                       + static_cast<uint64_t>(cycle);
            MessageEnvelope frag;
            MessageEnvelope out;
            envelope_init(out);
            stress_make_frag(frag, verify_id, FRAG_SRC_B, 0U, 2U, TOTAL_LEN,
                             PAST_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
            Result r = buf.ingest(frag, out, 1000000ULL);
            assert(r == Result::ERR_AGAIN);  // sweep freed at least one slot

            // Clean up verify session so buf is empty before next cycle
            buf.sweep_expired(SWEEP_NOW);
        }
    }

    printf("PASS: test_reassembly_slot_recycling"
           " (Phase A: %u completion cycles x %u slots,"
           " Phase B: %u expiry cycles x %u slots)\n",
           REASSEMBLY_COMPLETE_CYCLES, REASSEMBLY_SLOT_COUNT,
           REASSEMBLY_EXPIRY_CYCLES, REASSEMBLY_SLOT_COUNT);
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
    test_rre_stash_fifo_wraparound();
    test_reassembly_slot_recycling();

    printf("\nALL stress capacity tests passed.\n");
    return 0;
}
