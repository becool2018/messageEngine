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
 * Each test accepts an optional runtime duration (seconds) from argv[1] and
 * runs as many complete fixed-iteration rounds as fit in that window.  Pass
 * no argument for a 60-second default per test.  Use a small value (e.g., 5)
 * for a quick smoke run; use a larger value (e.g., 600) for overnight soak.
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
 *  Rule 2 (fixed loop bounds)  — inner loops use compile-time constants;
 *                                outer duration loops per Rule 2 deviation.
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
#include <ctime>    // time_t, time()

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

/// Cycles for AckTracker fill/drain tests per round.
static const uint32_t ACK_FILL_DRAIN_CYCLES   = 10000U;

/// Cycles for AckTracker partial-ACK + sweep test per round.
static const uint32_t ACK_PARTIAL_CYCLES      =  1000U;

/// Cycles for RetryManager on_ack + collect_due drain test per round.
static const uint32_t RETRY_ACK_DRAIN_CYCLES  =  5000U;

/// Cycles for RetryManager max-retry exhaustion test per round.
static const uint32_t RETRY_EXHAUST_CYCLES    =  1000U;

/// Individual push/pop iterations for RingBuffer single-step test per round.
static const uint32_t RING_SINGLE_ITERS       = 64000U;

/// Fill/drain cycles for RingBuffer bulk-fill test per round.
static const uint32_t RING_FILL_CYCLES        =  1000U;

/// Window lengths to cycle through for DuplicateFilter wraparound test per round.
static const uint32_t DEDUP_WRAP_CYCLES       =   100U;

/// Single-step request/response cycles per round.
static const uint32_t RRE_SINGLE_CYCLES       =  1000U;

/// Batch fill/drain cycles per round.
static const uint32_t RRE_BATCH_CYCLES        =   100U;

/// Requests per batch cycle.  Must satisfy: 2 × RRE_BATCH_SIZE ≤ MAX_STASH_SIZE (16).
static const uint32_t RRE_BATCH_SIZE          =     8U;

/// Cycles of open-all-8-sessions → complete-all-8 for ReassemblyBuffer per round.
static const uint32_t REASSEMBLY_COMPLETE_CYCLES = 1000U;

/// Cycles of open-all-8-sessions → sweep_expired-all-8 for ReassemblyBuffer per round.
static const uint32_t REASSEMBLY_EXPIRY_CYCLES   = 1000U;

/// Node IDs used in the RRE stress tests (distinct from production node IDs).
static const NodeId RRE_STRESS_NODE_A = 10U;
static const NodeId RRE_STRESS_NODE_B = 11U;

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
// TEST 1 — AckTracker: full fill → sweep_expired drain, N cycles per round
// ─────────────────────────────────────────────────────────────────────────────
/**
 * What is being tested
 * --------------------
 * Every slot is filled and then drained via sweep_expired() on each of
 * ACK_FILL_DRAIN_CYCLES iterations per round.  A slot leak — where
 * sweep_expired() fails to mark a slot FREE — will cause track() to return
 * ERR_FULL on the NEXT cycle's first fill, failing the assertion immediately.
 *
 * Verifies: REQ-3.2.4 (ACK tracking capacity), REQ-3.3.2 (reliable-ACK)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop.  Per-iteration work is bounded by ACK_FILL_DRAIN_CYCLES ×
// ACK_TRACKER_CAPACITY inner steps; terminates on wall-clock deadline.
static uint32_t test_ack_tracker_fill_drain_cycles(time_t deadline)
{
    AckTracker tracker;
    tracker.init();

    // expired_buf sized to ACK_TRACKER_CAPACITY (Power of 10 Rule 3: stack).
    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];

    // Deadline in the past so every tracked entry is immediately expired.
    static const uint64_t EXPIRED_DEADLINE_US = 1ULL;
    // Current time is well past that deadline.
    static const uint64_t NOW_US              = 1000000ULL;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Power of 10 Rule 2: bounded loop — ACK_FILL_DRAIN_CYCLES is compile-time.
        for (uint32_t cycle = 0U; cycle < ACK_FILL_DRAIN_CYCLES; ++cycle) {

            // ── Fill: track ACK_TRACKER_CAPACITY messages with unique IDs ──────
            for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 100ULL) +
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
                assert(overflow_r == Result::ERR_FULL);
            }

            // ── Drain: sweep_expired removes all entries (all deadlines are past) ──
            uint32_t collected = tracker.sweep_expired(NOW_US, expired_buf,
                                                        ACK_TRACKER_CAPACITY);
            assert(collected == ACK_TRACKER_CAPACITY);

            // ── Recycle check: one fresh track() must succeed immediately after drain ──
            {
                MessageEnvelope recycle_env;
                make_stress_envelope(recycle_env, 0xCAFEBABEULL);
                Result recycle_r = tracker.track(recycle_env, EXPIRED_DEADLINE_US);
                assert(recycle_r == Result::OK);

                uint32_t cleanup = tracker.sweep_expired(NOW_US, expired_buf,
                                                          ACK_TRACKER_CAPACITY);
                assert(cleanup == 1U);
            }
        }

        total_cycles += ACK_FILL_DRAIN_CYCLES;
    }

    printf("PASS: test_ack_tracker_fill_drain_cycles"
           " (%u total cycles x %u slots)\n",
           total_cycles, ACK_TRACKER_CAPACITY);
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 2 — AckTracker: fill → ACK half → sweep other half, N cycles per round
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.4 (ACK tracking), REQ-3.3.2 (reliable-ACK semantics)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_ack_tracker_partial_ack_then_sweep(time_t deadline)
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];

    static const uint32_t HALF          = ACK_TRACKER_CAPACITY / 2U;  // 16
    static const uint64_t FUTURE_DL_US  = 0xFFFFFFFFFFFFFF00ULL;
    static const uint64_t PAST_DL_US    = 1ULL;
    static const uint64_t NOW_US        = 1000000ULL;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < ACK_PARTIAL_CYCLES; ++cycle) {

            // ── Fill lower half with FUTURE deadlines (will NOT expire) ────────
            for (uint32_t slot = 0U; slot < HALF; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 200ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);
                Result r = tracker.track(env, FUTURE_DL_US);
                assert(r == Result::OK);
            }

            // ── Fill upper half with PAST deadlines (will expire) ─────────────
            for (uint32_t slot = HALF; slot < ACK_TRACKER_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 200ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);
                Result r = tracker.track(env, PAST_DL_US);
                assert(r == Result::OK);
            }

            // ── ACK the lower half by (src=1, msg_id) ─────────────────────────
            for (uint32_t slot = 0U; slot < HALF; ++slot) {
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 200ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                Result ack_r = tracker.on_ack(1U, msg_id);
                assert(ack_r == Result::OK);
            }

            // ── Sweep: only upper half (expired PENDING) should be returned ────
            uint32_t swept = tracker.sweep_expired(NOW_US, expired_buf,
                                                    ACK_TRACKER_CAPACITY);
            assert(swept == HALF);

            // ── Recycle check: all 32 slots must now be free ──────────────────
            for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    0xF000000000000000ULL +
                    (static_cast<uint64_t>(total_cycles + cycle) * ACK_TRACKER_CAPACITY) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);
                Result r = tracker.track(env, PAST_DL_US);
                assert(r == Result::OK);
            }

            uint32_t cleanup = tracker.sweep_expired(NOW_US, expired_buf,
                                                      ACK_TRACKER_CAPACITY);
            assert(cleanup == ACK_TRACKER_CAPACITY);
        }

        total_cycles += ACK_PARTIAL_CYCLES;
    }

    printf("PASS: test_ack_tracker_partial_ack_then_sweep"
           " (%u total cycles, %u ACKed + %u swept per cycle)\n",
           total_cycles, HALF, HALF);
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 3 — RetryManager: fill → on_ack all → collect_due GC, N cycles per round
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.5 (retry scheduling), REQ-3.3.3 (reliable-retry semantics)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_retry_manager_fill_ack_drain_cycles(time_t deadline)
{
    RetryManager rm;
    rm.init();

    MessageEnvelope out_buf[ACK_TRACKER_CAPACITY];

    static const uint64_t NOW_US         = 1000000ULL;
    static const uint64_t COLLECT_NOW_US = 2000000ULL;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < RETRY_ACK_DRAIN_CYCLES; ++cycle) {

            // ── Fill: schedule ACK_TRACKER_CAPACITY retry entries ─────────────
            for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 100ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);

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

            // ── Cancel all via on_ack ──────────────────────────────────────────
            for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 100ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                Result ack_r = rm.on_ack(1U, msg_id);
                assert(ack_r == Result::OK);
            }

            // ── GC: collect_due removes inactive entries ───────────────────────
            uint32_t collected = rm.collect_due(COLLECT_NOW_US, out_buf,
                                                 ACK_TRACKER_CAPACITY);
            assert(collected == 0U);

            // ── Recycle check ──────────────────────────────────────────────────
            {
                MessageEnvelope recycle_env;
                make_stress_envelope(recycle_env, 0xCAFEBABEULL);
                Result recycle_r = rm.schedule(recycle_env, 1U, 1U, NOW_US);
                assert(recycle_r == Result::OK);

                Result ack_cleanup = rm.on_ack(1U, 0xCAFEBABEULL);
                (void)ack_cleanup;
                (void)rm.collect_due(COLLECT_NOW_US, out_buf, ACK_TRACKER_CAPACITY);
            }
        }

        total_cycles += RETRY_ACK_DRAIN_CYCLES;
    }

    printf("PASS: test_retry_manager_fill_ack_drain_cycles"
           " (%u total cycles x %u slots)\n",
           total_cycles, ACK_TRACKER_CAPACITY);
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 4 — RetryManager: retry exhaustion at MAX_RETRY_COUNT, N cycles per round
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.5 (retry count ceiling), REQ-3.3.3 (exhaustion semantics)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_retry_manager_max_retry_exhaustion(time_t deadline)
{
    RetryManager rm;
    rm.init();

    MessageEnvelope out_buf[ACK_TRACKER_CAPACITY];

    static const uint64_t ADVANCE_US    = 1000000ULL;  // 1 second per step
    static const uint64_t INITIAL_NOW   = 1000000ULL;

    // now_us is declared before the while loop and advances monotonically
    // across all rounds.  RetryManager::collect_due() asserts monotonic time;
    // resetting to INITIAL_NOW on each round would violate that invariant.
    uint64_t now_us = INITIAL_NOW;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < RETRY_EXHAUST_CYCLES; ++cycle) {

            // ── Fill all slots with MAX_RETRY_COUNT retry budget ───────────────
            for (uint32_t slot = 0U; slot < ACK_TRACKER_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_cycles + cycle) * 100ULL) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);
                Result r = rm.schedule(env, MAX_RETRY_COUNT, 1U, now_us);
                assert(r == Result::OK);
            }

            // ── Drive to exhaustion: call collect_due MAX_RETRY_COUNT + 1 times ─
            uint32_t total_collected = 0U;

            static const uint32_t MAX_CALLS = MAX_RETRY_COUNT + 1U;
            for (uint32_t call = 0U; call < MAX_CALLS; ++call) {
                now_us += ADVANCE_US;
                uint32_t cnt = rm.collect_due(now_us, out_buf, ACK_TRACKER_CAPACITY);

                assert(cnt <= ACK_TRACKER_CAPACITY);

                if (call == MAX_RETRY_COUNT) {
                    assert(cnt == 0U);
                }
                total_collected += cnt;
            }

            assert(total_collected == ACK_TRACKER_CAPACITY * MAX_RETRY_COUNT);

            // ── Recycle check ──────────────────────────────────────────────────
            {
                MessageEnvelope env;
                make_stress_envelope(env, 0xCAFEBABEULL);
                Result r = rm.schedule(env, 1U, 1U, now_us);
                assert(r == Result::OK);

                static const uint32_t RECYCLE_CALLS = 2U;
                for (uint32_t rc = 0U; rc < RECYCLE_CALLS; ++rc) {
                    now_us += ADVANCE_US;
                    (void)rm.collect_due(now_us, out_buf, ACK_TRACKER_CAPACITY);
                }
            }
        }

        total_cycles += RETRY_EXHAUST_CYCLES;
    }

    printf("PASS: test_retry_manager_max_retry_exhaustion"
           " (%u total cycles x %u slots x %u retries)\n",
           total_cycles, ACK_TRACKER_CAPACITY, MAX_RETRY_COUNT);
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 5 — RingBuffer: sustained push/pop with message_id roundtrip
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-4.1.2 (send_message queue semantics), REQ-4.2.2 (per-channel limits)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_ring_buffer_sustained_push_pop(time_t deadline)
{
    RingBuffer ring;
    ring.init();

    uint32_t total_iters = 0U;

    while (time(nullptr) < deadline) {
        // ── Phase A: RING_SINGLE_ITERS single push/pop pairs ──────────────────
        // Power of 10 Rule 2: bounded loop
        for (uint32_t iter = 0U; iter < RING_SINGLE_ITERS; ++iter) {
            MessageEnvelope push_env;
            make_stress_envelope(push_env,
                                 static_cast<uint64_t>(total_iters + iter) + 1ULL);

            Result push_r = ring.push(push_env);
            assert(push_r == Result::OK);

            MessageEnvelope pop_env;
            Result pop_r = ring.pop(pop_env);
            assert(pop_r == Result::OK);
            assert(pop_env.message_id == push_env.message_id);
        }

        // ── Phase B: RING_FILL_CYCLES full fill / full drain cycles ───────────
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < RING_FILL_CYCLES; ++cycle) {

            for (uint32_t slot = 0U; slot < MSG_RING_CAPACITY; ++slot) {
                MessageEnvelope env;
                const uint64_t msg_id =
                    (static_cast<uint64_t>(total_iters + cycle) * MSG_RING_CAPACITY) +
                    static_cast<uint64_t>(slot) + 1ULL;
                make_stress_envelope(env, msg_id);

                Result r = ring.push(env);
                assert(r == Result::OK);
            }

            {
                MessageEnvelope overflow_env;
                make_stress_envelope(overflow_env, 0xDEADBEEFULL);
                Result overflow_r = ring.push(overflow_env);
                assert(overflow_r == Result::ERR_FULL);
            }

            for (uint32_t slot = 0U; slot < MSG_RING_CAPACITY; ++slot) {
                MessageEnvelope popped;
                Result r = ring.pop(popped);
                assert(r == Result::OK);

                const uint64_t expected_id =
                    (static_cast<uint64_t>(total_iters + cycle) * MSG_RING_CAPACITY) +
                    static_cast<uint64_t>(slot) + 1ULL;
                assert(popped.message_id == expected_id);
            }

            {
                MessageEnvelope empty_env;
                Result empty_r = ring.pop(empty_env);
                assert(empty_r == Result::ERR_EMPTY);
            }
        }

        total_iters += RING_SINGLE_ITERS + RING_FILL_CYCLES;
    }

    printf("PASS: test_ring_buffer_sustained_push_pop"
           " (Phase A: %u iters, Phase B: %u cycles x %u slots per round;"
           " total rounds: %u)\n",
           RING_SINGLE_ITERS, RING_FILL_CYCLES, MSG_RING_CAPACITY,
           total_iters / (RING_SINGLE_ITERS + RING_FILL_CYCLES));
    return total_iters;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 6 — DuplicateFilter: sliding-window eviction across N window lengths
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.6 (duplicate suppression), REQ-3.3.3 (dedup in retry path)
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_dedup_filter_window_wraparound(time_t deadline)
{
    DuplicateFilter filter;
    filter.init();

    static const uint64_t IDS_PER_CYCLE = static_cast<uint64_t>(DEDUP_WINDOW_SIZE) * 2ULL;

    // now_us advances monotonically across all rounds (SECfix-3 age-based eviction
    // requires batch A timestamps to be strictly smaller than batch B).
    uint64_t now_us = 1ULL;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < DEDUP_WRAP_CYCLES; ++cycle) {

            const uint64_t base = static_cast<uint64_t>(total_cycles + cycle)
                                  * IDS_PER_CYCLE + 1ULL;

            // ── Step 1: Record batch A ─────────────────────────────────────────
            for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
                const uint64_t msg_id = base + static_cast<uint64_t>(i);
                Result r = filter.check_and_record(1U, msg_id, now_us);
                ++now_us;
                assert(r == Result::OK);
            }

            // ── Step 2: Verify all batch A entries are duplicates ─────────────
            for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
                const uint64_t msg_id = base + static_cast<uint64_t>(i);
                bool is_dup = filter.is_duplicate(1U, msg_id);
                assert(is_dup);
            }

            // ── Step 3: Record batch B — displaces batch A ────────────────────
            const uint64_t batch_b_base = base + static_cast<uint64_t>(DEDUP_WINDOW_SIZE);
            for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
                const uint64_t msg_id = batch_b_base + static_cast<uint64_t>(i);
                Result r = filter.check_and_record(1U, msg_id, now_us);
                ++now_us;
                assert(r == Result::OK);
            }

            // ── Step 4: Batch A must be evicted ───────────────────────────────
            for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
                const uint64_t msg_id = base + static_cast<uint64_t>(i);
                bool is_dup = filter.is_duplicate(1U, msg_id);
                assert(!is_dup);
            }

            // ── Step 5: Batch B must still be detected ────────────────────────
            for (uint32_t i = 0U; i < DEDUP_WINDOW_SIZE; ++i) {
                const uint64_t msg_id = batch_b_base + static_cast<uint64_t>(i);
                bool is_dup = filter.is_duplicate(1U, msg_id);
                assert(is_dup);
            }
        }

        total_cycles += DEDUP_WRAP_CYCLES;
    }

    printf("PASS: test_dedup_filter_window_wraparound"
           " (%u total window cycles, %u entries per batch)\n",
           total_cycles, DEDUP_WINDOW_SIZE);
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 7 — RequestReplyEngine: stash FIFO circular-buffer wraparound
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_rre_stash_fifo_wraparound(time_t deadline)
{
    // Large objects in BSS to avoid stack overflow (Power of 10 Rule 3 note:
    // static local → BSS; stress_setup_rre_nodes() re-initialises all state).
    static LocalSimHarness    ha;
    static DeliveryEngine     ea;
    static RequestReplyEngine rrea;
    static LocalSimHarness    hb;
    static DeliveryEngine     eb;
    static RequestReplyEngine rreb;

    static const uint64_t RRE_NOW_US     = 1000000ULL;
    static const uint64_t RRE_TIMEOUT_US = 5000000ULL;
    static const uint8_t  REQ_BYTE       = 0xA1U;
    static const uint8_t  RSP_BYTE       = 0xB2U;

    // Initialise the static RRE objects once before the duration loop.
    // State is cleanly drained at the end of every round (all requests
    // responded to, all AckTracker slots swept), so re-initialisation is not
    // needed across rounds.  Re-calling stress_setup_rre_nodes() each round
    // would re-send HELLO frames that arrive before Phase A DATA frames and
    // cause receive_request() to see a non-DATA message on the first receive.
    stress_setup_rre_nodes(ha, ea, rrea, hb, eb, rreb);

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {

        // ── Phase A: RRE_SINGLE_CYCLES single-step request/response cycles ────
        // Power of 10 Rule 2: bounded loop
        for (uint32_t cycle = 0U; cycle < RRE_SINGLE_CYCLES; ++cycle) {

            uint64_t cid = 0U;
            Result r = rrea.send_request(RRE_STRESS_NODE_B, &REQ_BYTE, 1U,
                                         RRE_TIMEOUT_US, RRE_NOW_US, cid);
            assert(r == Result::OK);

            uint8_t  rx_buf[1U]  = { 0U };
            uint32_t rx_len      = 0U;
            NodeId   rx_src      = 0U;
            uint64_t rx_cid      = 0U;
            r = rreb.receive_request(rx_buf, 1U, rx_len, rx_src, rx_cid, RRE_NOW_US);
            assert(r == Result::OK);

            r = rreb.send_response(rx_src, rx_cid, &RSP_BYTE, 1U, RRE_NOW_US);
            assert(r == Result::OK);

            uint8_t  rsp_buf[1U] = { 0U };
            uint32_t rsp_len     = 0U;
            r = rrea.receive_response(cid, rsp_buf, 1U, rsp_len, RRE_NOW_US);
            assert(r == Result::OK);

            (void)ea.sweep_ack_timeouts(RRE_NOW_US);
        }

        // ── Phase B: RRE_BATCH_CYCLES full stash fill/drain cycles ────────────
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < RRE_BATCH_CYCLES; ++cycle) {

            uint64_t cids[RRE_BATCH_SIZE];
            (void)memset(cids, 0, sizeof(cids));

            for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
                const uint8_t pay = static_cast<uint8_t>(slot);
                Result r = rrea.send_request(RRE_STRESS_NODE_B, &pay, 1U,
                                             RRE_TIMEOUT_US, RRE_NOW_US, cids[slot]);
                assert(r == Result::OK);
            }

            for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
                uint8_t  rx_buf[1U] = { 0U };
                uint32_t rx_len     = 0U;
                NodeId   rx_src     = 0U;
                uint64_t rx_cid     = 0U;
                Result r = rreb.receive_request(rx_buf, 1U, rx_len,
                                                rx_src, rx_cid, RRE_NOW_US);
                assert(r == Result::OK);
                assert(rx_buf[0] == static_cast<uint8_t>(slot));

                Result send_r = rreb.send_response(rx_src, rx_cid,
                                                    rx_buf, rx_len, RRE_NOW_US);
                assert(send_r == Result::OK);
            }

            {
                uint8_t  empty_buf[1U] = { 0U };
                uint32_t empty_len     = 0U;
                NodeId   empty_src     = 0U;
                uint64_t empty_cid     = 0U;
                Result empty_r = rreb.receive_request(empty_buf, 1U, empty_len,
                                                       empty_src, empty_cid,
                                                       RRE_NOW_US);
                assert(empty_r == Result::ERR_EMPTY);
            }

            for (uint32_t slot = 0U; slot < RRE_BATCH_SIZE; ++slot) {
                uint8_t  rsp_buf[1U] = { 0U };
                uint32_t rsp_len     = 0U;
                Result r = rrea.receive_response(cids[slot], rsp_buf, 1U,
                                                  rsp_len, RRE_NOW_US);
                assert(r == Result::OK);
            }

            (void)ea.sweep_ack_timeouts(RRE_NOW_US);
        }

        total_cycles += RRE_SINGLE_CYCLES + RRE_BATCH_CYCLES;
    }

    printf("PASS: test_rre_stash_fifo_wraparound"
           " (Phase A: %u single cycles, Phase B: %u batch cycles x %u slots"
           " per round; total rounds: %u)\n",
           RRE_SINGLE_CYCLES, RRE_BATCH_CYCLES, RRE_BATCH_SIZE,
           total_cycles / (RRE_SINGLE_CYCLES + RRE_BATCH_CYCLES));
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// TEST 8 — ReassemblyBuffer: session slot recycling (completion + expiry paths)
// ─────────────────────────────────────────────────────────────────────────────
/**
 * Verifies: REQ-3.2.3, REQ-3.3.3
 */
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test loop.
static uint32_t test_reassembly_slot_recycling(time_t deadline)
{
    static ReassemblyBuffer buf;
    buf.init();

    static const uint8_t  FRAG_BYTE  = 0xBBU;
    static const uint32_t FRAG_BYTES = 1U;
    static const uint16_t TOTAL_LEN  = 2U;

    static const uint64_t FAR_EXPIRY  = 0xFFFFFFFFFFFFFF00ULL;
    static const uint64_t PAST_EXPIRY = 1ULL;
    static const uint64_t SWEEP_NOW   = 1000000ULL;

    static const NodeId FRAG_SRC_A  = 20U;
    static const uint32_t REASM_PER_SRC_MAX = REASSEMBLY_SLOT_COUNT / 2U;
    static const NodeId FRAG_SRC_A2 = 40U;
    static const NodeId FRAG_SRC_B  = 21U;
    static const NodeId FRAG_SRC_B2 = 41U;

    static const uint64_t PHASE_B_BASE = 1000000ULL;

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        // Re-initialise static buf at the start of each round.
        buf.init();

        // Phase A and B ID spaces shift by total_cycles to stay unique across rounds.
        const uint64_t round_offset =
            static_cast<uint64_t>(total_cycles)
            * static_cast<uint64_t>(REASSEMBLY_COMPLETE_CYCLES + REASSEMBLY_EXPIRY_CYCLES)
            * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT);

        // ── Phase A: REASSEMBLY_COMPLETE_CYCLES open-all → complete-all cycles ─
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < REASSEMBLY_COMPLETE_CYCLES; ++cycle) {

            for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
                const uint64_t msg_id =
                    round_offset +
                    static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                    + static_cast<uint64_t>(s) + 1ULL;
                const NodeId src_a = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_A : FRAG_SRC_A2;
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, msg_id, src_a, 0U, 2U, TOTAL_LEN,
                                 FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::ERR_AGAIN);
            }

            {
                const uint64_t overflow_id = 0xDEAD000000000000ULL + round_offset
                                             + static_cast<uint64_t>(cycle);
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, overflow_id, FRAG_SRC_A, 0U, 2U, TOTAL_LEN,
                                 FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::ERR_FULL);
            }

            for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
                const uint64_t msg_id =
                    round_offset +
                    static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                    + static_cast<uint64_t>(s) + 1ULL;
                const NodeId src_a = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_A : FRAG_SRC_A2;
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, msg_id, src_a, 1U, 2U, TOTAL_LEN,
                                 FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::OK);
                assert(out.message_id == msg_id);
            }

            {
                const uint64_t recycle_id = 0xCAFE000000000000ULL + round_offset
                                            + static_cast<uint64_t>(cycle);
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, recycle_id, FRAG_SRC_A, 0U, 2U, TOTAL_LEN,
                                 FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::ERR_AGAIN);

                stress_make_frag(frag, recycle_id, FRAG_SRC_A, 1U, 2U, TOTAL_LEN,
                                 FAR_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::OK);
            }
        }

        // ── Phase B: REASSEMBLY_EXPIRY_CYCLES open-all → sweep cycles ─────────
        // Power of 10 Rule 2: bounded outer loop
        for (uint32_t cycle = 0U; cycle < REASSEMBLY_EXPIRY_CYCLES; ++cycle) {

            for (uint32_t s = 0U; s < REASSEMBLY_SLOT_COUNT; ++s) {
                const uint64_t msg_id =
                    round_offset + PHASE_B_BASE
                    + static_cast<uint64_t>(cycle) * static_cast<uint64_t>(REASSEMBLY_SLOT_COUNT)
                    + static_cast<uint64_t>(s) + 1ULL;
                const NodeId src_b = (s < REASM_PER_SRC_MAX) ? FRAG_SRC_B : FRAG_SRC_B2;
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, msg_id, src_b, 0U, 2U, TOTAL_LEN,
                                 PAST_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::ERR_AGAIN);
            }

            buf.sweep_expired(SWEEP_NOW);

            {
                const uint64_t verify_id = 0xBEEF000000000000ULL + round_offset
                                           + static_cast<uint64_t>(cycle);
                MessageEnvelope frag;
                MessageEnvelope out;
                envelope_init(out);
                stress_make_frag(frag, verify_id, FRAG_SRC_B, 0U, 2U, TOTAL_LEN,
                                 PAST_EXPIRY, &FRAG_BYTE, FRAG_BYTES);
                Result r = buf.ingest(frag, out, 1000000ULL);
                assert(r == Result::ERR_AGAIN);

                buf.sweep_expired(SWEEP_NOW);
            }
        }

        total_cycles += REASSEMBLY_COMPLETE_CYCLES + REASSEMBLY_EXPIRY_CYCLES;
    }

    printf("PASS: test_reassembly_slot_recycling"
           " (Phase A: %u completion cycles x %u slots,"
           " Phase B: %u expiry cycles x %u slots per round;"
           " total rounds: %u)\n",
           REASSEMBLY_COMPLETE_CYCLES, REASSEMBLY_SLOT_COUNT,
           REASSEMBLY_EXPIRY_CYCLES, REASSEMBLY_SLOT_COUNT,
           total_cycles / (REASSEMBLY_COMPLETE_CYCLES + REASSEMBLY_EXPIRY_CYCLES));
    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    const time_t duration_secs = parse_duration_secs(argc, argv);

    assert(duration_secs > 0);

    printf("=== Stress tests: capacity exhaustion and slot recycling ===\n");
    printf("    (each test runs for %lu s; total ~%lu s)\n\n",
           static_cast<unsigned long>(duration_secs),
           static_cast<unsigned long>(duration_secs) * 8UL);

    uint32_t c1 = test_ack_tracker_fill_drain_cycles(time(nullptr) + duration_secs);
    assert(c1 > 0U);

    uint32_t c2 = test_ack_tracker_partial_ack_then_sweep(time(nullptr) + duration_secs);
    assert(c2 > 0U);

    uint32_t c3 = test_retry_manager_fill_ack_drain_cycles(time(nullptr) + duration_secs);
    assert(c3 > 0U);

    uint32_t c4 = test_retry_manager_max_retry_exhaustion(time(nullptr) + duration_secs);
    assert(c4 > 0U);

    uint32_t c5 = test_ring_buffer_sustained_push_pop(time(nullptr) + duration_secs);
    assert(c5 > 0U);

    uint32_t c6 = test_dedup_filter_window_wraparound(time(nullptr) + duration_secs);
    assert(c6 > 0U);

    uint32_t c7 = test_rre_stash_fifo_wraparound(time(nullptr) + duration_secs);
    assert(c7 > 0U);

    uint32_t c8 = test_reassembly_slot_recycling(time(nullptr) + duration_secs);
    assert(c8 > 0U);

    (void)c1; (void)c2; (void)c3; (void)c4;
    (void)c5; (void)c6; (void)c7; (void)c8;

    printf("\nALL stress capacity tests passed.\n");
    return 0;
}
