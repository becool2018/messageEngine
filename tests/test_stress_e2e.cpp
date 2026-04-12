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
 * @file test_stress_e2e.cpp
 * @brief End-to-end pipeline and component-combination stress tests.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * Unit tests verify correctness at design points; these stress tests verify
 * correctness under SUSTAINED LOAD targeting failure classes that only
 * manifest after many repetitions: boundary-crossing arithmetic errors, slot
 * leaks under combined impairments, and reassembly state drift.
 *
 * Each test accepts an optional runtime duration (seconds) from argv[1] and
 * runs as many complete rounds as fit in that window.  Pass no argument for a
 * 60-second default per test.  Use a small value (e.g., 5) for a quick smoke
 * run; use a larger value (e.g., 600) for overnight soak testing.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * TESTS IN THIS FILE (Session 1 stress suite)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Test 7: DuplicateFilter — wraparound at UINT64_MAX boundary.
 *           1000 unique message IDs starting at UINT64_MAX - 500 walk through
 *           zero and into positive space.  Verifies no false positives or
 *           negatives and no arithmetic crash at the 64-bit boundary.
 *
 *  Test 1: E2E throughput soak — 10,000 BEST_EFFORT messages through a linked
 *           LocalSimHarness pair.  Verifies the ring buffer, serializer, and
 *           impairment-passthrough path do not accumulate state or degrade
 *           throughput under sustained sequential load.
 *
 *  Test 2: Fragmentation/reassembly slot recycling — 500 complete (all-4-
 *           fragment) cycles followed by 50 partial (2-of-4) cycles drained by
 *           sweep_stale().  Verifies that reassembly slots are freed correctly
 *           after completion and reclaimed correctly by the stale-sweep.
 *
 *  Test 5: ImpairmentEngine combined-fault saturation — 2000 messages through
 *           all six impairments simultaneously (loss, duplication, fixed
 *           latency, jitter, reordering, partitions).  Verifies the delay
 *           buffer drains cleanly and the total delivery count is bounded.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 2 (fixed loop bounds)  — inner loops use compile-time constants;
 *                                outer duration loops per Rule 2 deviation.
 *  Rule 3 (no dynamic alloc)   — static / stack arrays only; no new/delete.
 *  Rule 5 (assertions)         — ≥2 assert() calls per test function.
 *  Rule 7 (check returns)      — every Result return value is checked or cast
 *                                to (void) where intentionally ignored.
 *  Rule 10 (zero warnings)     — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions  — raised ceiling per §1b; all functions ≤ 12.
 *  assert() permitted          — tests/ exemption per §9.
 *
 * Verifies: REQ-3.2.3, REQ-3.2.6, REQ-3.2.9, REQ-3.3.1, REQ-3.3.3,
 *           REQ-4.1.2, REQ-4.1.3, REQ-5.1.1, REQ-5.1.2, REQ-5.1.3,
 *           REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.2.2, REQ-5.3.1
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
#include "core/ChannelConfig.hpp"
#include "core/DuplicateFilter.hpp"
#include "core/ImpairmentConfig.hpp"
#include "core/Fragmentation.hpp"
#include "core/ReassemblyBuffer.hpp"
#include "platform/LocalSimHarness.hpp"
#include "platform/ImpairmentEngine.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Stress iteration counts (compile-time — Power of 10 Rule 2).
// ─────────────────────────────────────────────────────────────────────────────

/// Number of unique message IDs walked across the UINT64_MAX boundary per round.
static const uint32_t DEDUP_WRAP_MSGS         = 1000U;

/// Number of BEST_EFFORT send→receive round-trips for the E2E soak per round.
static const uint32_t E2E_SOAK_MSGS           = 10000U;

/// Complete-reassembly cycles (all FRAG_MAX_COUNT fragments arrive) per round.
static const uint32_t FRAG_COMPLETE_CYCLES    =   500U;

/// Stale-sweep cycles (partial fragments + sweep_stale to reclaim slot) per round.
static const uint32_t FRAG_STALE_CYCLES       =    50U;

/// Messages sent through the combined-impairment saturation soak per round.
static const uint32_t IMPAIR_SOAK_MSGS        =  2000U;

/// collect_deliverable output buffer capacity for the impairment soak.
static const uint32_t IMPAIR_OUT_BUF_SIZE     =    64U;

// Node IDs used by the E2E soak — distinct from unit-test IDs.
static const NodeId SOAK_NODE_A = 20U;
static const NodeId SOAK_NODE_B = 21U;

// Source node ID used by the fragmentation soak.
static const NodeId FRAG_SOAK_SRC = 6U;

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
// Helper: build and fragment a maximum-payload message for a given message_id.
// Returns the fragment count (always FRAG_MAX_COUNT for a full payload).
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t frag_soak_make_frags(MessageEnvelope (&frags)[FRAG_MAX_COUNT],
                                      uint64_t         msg_id,
                                      uint64_t         now_us)
{
    MessageEnvelope logical;
    envelope_init(logical);
    logical.message_type         = MessageType::DATA;
    logical.source_id            = FRAG_SOAK_SRC;
    logical.destination_id       = 1U;
    logical.message_id           = msg_id;
    logical.timestamp_us         = now_us;
    logical.expiry_time_us       = now_us + 5000000ULL;
    logical.sequence_num         = 0U;
    logical.fragment_index       = 0U;
    logical.fragment_count       = 1U;
    logical.reliability_class    = ReliabilityClass::BEST_EFFORT;
    logical.payload_length       = MSG_MAX_PAYLOAD_BYTES;
    // CERT INT31-C: MSG_MAX_PAYLOAD_BYTES fits in uint16_t (4096 ≤ 65535).
    logical.total_payload_length = static_cast<uint16_t>(MSG_MAX_PAYLOAD_BYTES);

    // Fill payload with a deterministic pattern derived from msg_id.
    // Power of 10 Rule 2: loop bound is MSG_MAX_PAYLOAD_BYTES (compile-time).
    for (uint32_t i = 0U; i < MSG_MAX_PAYLOAD_BYTES; ++i) {
        logical.payload[i] = static_cast<uint8_t>((msg_id + static_cast<uint64_t>(i)) & 0xFFULL);
    }

    assert(needs_fragmentation(logical));
    return fragment_message(logical, frags, FRAG_MAX_COUNT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: DuplicateFilter wraparound at UINT64_MAX — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Each round walks DEDUP_WRAP_MSGS unique IDs starting at UINT64_MAX - 500 so
// the sequence crosses zero.  A fresh DuplicateFilter is used per round.
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop; per-iteration work is bounded by DEDUP_WRAP_MSGS (compile-time).
// Verifies: REQ-3.2.6
static uint32_t test_dedup_uint64_wraparound(time_t deadline)
{
    // Starting ID: 500 steps before UINT64_MAX so the loop crosses zero.
    static const uint64_t START_ID = static_cast<uint64_t>(UINT64_MAX) - 500ULL;

    uint32_t total_msgs = 0U;

    while (time(nullptr) < deadline) {
        DuplicateFilter filter;
        filter.init();

        // Power of 10 Rule 2: bounded by DEDUP_WRAP_MSGS (compile-time constant).
        for (uint32_t i = 0U; i < DEDUP_WRAP_MSGS; ++i) {
            // Wraps naturally: START_ID + 501 = 0, START_ID + 502 = 1, ...
            const uint64_t msg_id = START_ID + static_cast<uint64_t>(i);

            // now_us must be > 0 per DuplicateFilter::record() contract (SECfix-3
            // age-based eviction requires a non-zero timestamp).
            const uint64_t now_us = 1000000ULL + static_cast<uint64_t>(i);

            // Each ID is unique; must NOT be a duplicate before recording.
            Result r = filter.check_and_record(1U, msg_id, now_us);
            assert(r == Result::OK);

            // Immediately after recording, the same ID must be in the window.
            bool dup = filter.is_duplicate(1U, msg_id);
            assert(dup == true);
        }

        total_msgs += DEDUP_WRAP_MSGS;
    }

    return total_msgs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: E2E throughput soak — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Each round sends E2E_SOAK_MSGS BEST_EFFORT messages from harness A to B.
// Harnesses are re-created and re-linked each round for clean-state isolation.
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop; per-iteration work is bounded by E2E_SOAK_MSGS (compile-time).
// Verifies: REQ-3.3.1, REQ-4.1.2, REQ-4.1.3
static uint32_t test_e2e_throughput_soak(time_t deadline)
{
    uint32_t total_msgs = 0U;

    while (time(nullptr) < deadline) {
        LocalSimHarness ha;
        LocalSimHarness hb;

        TransportConfig cfg_a;
        transport_config_default(cfg_a);
        cfg_a.kind          = TransportKind::LOCAL_SIM;
        cfg_a.local_node_id = SOAK_NODE_A;
        cfg_a.is_server     = false;

        TransportConfig cfg_b;
        transport_config_default(cfg_b);
        cfg_b.kind          = TransportKind::LOCAL_SIM;
        cfg_b.local_node_id = SOAK_NODE_B;
        cfg_b.is_server     = false;

        Result ra = ha.init(cfg_a);
        assert(ra == Result::OK);
        Result rb = hb.init(cfg_b);
        assert(rb == Result::OK);

        ha.link(&hb);
        hb.link(&ha);

        // Power of 10 Rule 2: bounded by E2E_SOAK_MSGS (compile-time constant).
        for (uint32_t i = 0U; i < E2E_SOAK_MSGS; ++i) {
            MessageEnvelope send_env;
            envelope_init(send_env);
            send_env.message_type         = MessageType::DATA;
            send_env.source_id            = SOAK_NODE_A;
            send_env.destination_id       = SOAK_NODE_B;
            send_env.message_id           = static_cast<uint64_t>(i) + 1ULL;
            send_env.timestamp_us         = static_cast<uint64_t>(i) * 100ULL;
            send_env.reliability_class    = ReliabilityClass::BEST_EFFORT;
            send_env.fragment_index       = 0U;
            send_env.fragment_count       = 1U;
            send_env.payload_length       = 8U;
            send_env.total_payload_length = 8U;
            // Embed iteration number in the first two payload bytes for identity check.
            send_env.payload[0] = static_cast<uint8_t>(i & 0xFFU);
            send_env.payload[1] = static_cast<uint8_t>((i >> 8U) & 0xFFU);

            Result sr = ha.send_message(send_env);
            assert(sr == Result::OK);

            MessageEnvelope recv_env;
            envelope_init(recv_env);
            Result rr = hb.receive_message(recv_env, 0U);
            assert(rr == Result::OK);
            assert(recv_env.message_id == send_env.message_id);
        }

        ha.close();
        hb.close();
        total_msgs += E2E_SOAK_MSGS;
    }

    return total_msgs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Fragmentation / reassembly slot recycling — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Each round runs FRAG_COMPLETE_CYCLES complete cycles and FRAG_STALE_CYCLES
// stale-sweep cycles.  A fresh ReassemblyBuffer is used per round.
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop; per-iteration work is bounded by FRAG_COMPLETE_CYCLES + FRAG_STALE_CYCLES.
// Verifies: REQ-3.2.3, REQ-3.2.9, REQ-3.3.3
static uint32_t test_frag_reassembly_soak(time_t deadline)
{
    // Static buffer for fragments — Power of 10 Rule 3: no dynamic allocation.
    static MessageEnvelope frags[FRAG_MAX_COUNT];

    uint32_t total_cycles = 0U;

    while (time(nullptr) < deadline) {
        ReassemblyBuffer rbuf;
        rbuf.init();

        // ── Complete cycles ───────────────────────────────────────────────────
        // Power of 10 Rule 2: bounded by FRAG_COMPLETE_CYCLES.
        for (uint32_t cycle = 0U; cycle < FRAG_COMPLETE_CYCLES; ++cycle) {
            const uint64_t msg_id = static_cast<uint64_t>(cycle) + 1ULL;
            const uint64_t now_us = 1000000ULL + static_cast<uint64_t>(cycle) * 10000ULL;

            uint32_t fcount = frag_soak_make_frags(frags, msg_id, now_us);
            assert(fcount == FRAG_MAX_COUNT);

            // First (FRAG_MAX_COUNT - 1) fragments must return ERR_AGAIN.
            MessageEnvelope out;
            // Power of 10 Rule 2: bounded by FRAG_MAX_COUNT - 1U (compile-time).
            for (uint32_t fi = 0U; fi < FRAG_MAX_COUNT - 1U; ++fi) {
                envelope_init(out);
                Result r = rbuf.ingest(frags[fi], out, now_us);
                assert(r == Result::ERR_AGAIN);
            }

            // Final fragment completes the message; slot must be freed.
            envelope_init(out);
            Result last = rbuf.ingest(frags[FRAG_MAX_COUNT - 1U], out, now_us);
            assert(last == Result::OK);
            assert(out.source_id == FRAG_SOAK_SRC);
        }

        // ── Stale-sweep cycles ────────────────────────────────────────────────
        // msg_id space continues past complete-cycle IDs to avoid false hits
        // if the ReassemblyBuffer ever checked for duplicate message_ids.
        // Power of 10 Rule 2: bounded by FRAG_STALE_CYCLES.
        for (uint32_t cycle = 0U; cycle < FRAG_STALE_CYCLES; ++cycle) {
            // CERT INT30-C: FRAG_COMPLETE_CYCLES + cycle ≤ 549 < UINT32_MAX.
            const uint64_t msg_id  = static_cast<uint64_t>(FRAG_COMPLETE_CYCLES + cycle) + 1ULL;
            const uint64_t now_us  = 2000000000ULL
                                     + static_cast<uint64_t>(cycle) * 10000ULL;
            // far_us must exceed now_us + stale_threshold_us (5 s).
            const uint64_t far_us  = now_us + 10000000ULL;  // 10 s after open

            uint32_t fcount = frag_soak_make_frags(frags, msg_id, now_us);
            assert(fcount == FRAG_MAX_COUNT);

            // Ingest only 2 of 4 fragments — slot stays open (incomplete).
            // Power of 10 Rule 2: bounded by literal 2U.
            MessageEnvelope out;
            for (uint32_t fi = 0U; fi < 2U; ++fi) {
                envelope_init(out);
                Result r = rbuf.ingest(frags[fi], out, now_us);
                assert(r == Result::ERR_AGAIN);
            }

            // Advance time well past the 5 s stale threshold and reclaim the slot.
            uint32_t freed = rbuf.sweep_stale(far_us, 5000000ULL);
            assert(freed >= 1U);
        }

        total_cycles += FRAG_COMPLETE_CYCLES + FRAG_STALE_CYCLES;
    }

    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: ImpairmentEngine combined-fault saturation soak — duration-bounded.
// ─────────────────────────────────────────────────────────────────────────────
// Each round enables all six impairment types at modest levels and sends
// IMPAIR_SOAK_MSGS messages, then drains the delay buffer.
// A fresh ImpairmentEngine is configured each round.
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop; per-iteration work is bounded by IMPAIR_SOAK_MSGS + IMPAIR_DELAY_BUF_SIZE.
// Verifies: REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-5.1.5,
//           REQ-5.1.6, REQ-5.2.2, REQ-5.3.1
static uint32_t test_impairment_saturation_soak(time_t deadline)
{
    // Static output buffer — Power of 10 Rule 3: no dynamic allocation.
    static MessageEnvelope out_buf[IMPAIR_OUT_BUF_SIZE];

    uint32_t total_rounds = 0U;

    while (time(nullptr) < deadline) {
        ImpairmentEngine engine;
        ImpairmentConfig cfg;
        impairment_config_default(cfg);

        cfg.enabled                 = true;
        cfg.loss_probability        = 0.05;   // 5% loss
        cfg.duplication_probability = 0.05;   // 5% duplication
        cfg.fixed_latency_ms        = 1U;
        cfg.jitter_mean_ms          = 1U;
        cfg.jitter_variance_ms      = 1U;
        cfg.reorder_enabled         = true;
        cfg.reorder_window_size     = 4U;
        cfg.partition_enabled       = true;
        cfg.partition_duration_ms   = 1U;    // 1 ms blackout
        cfg.partition_gap_ms        = 200U;  // 200 ms between blackouts
        cfg.prng_seed               = 99999ULL;  // fixed seed → deterministic run

        engine.init(cfg);

        uint32_t total_delivered = 0U;

        // Power of 10 Rule 2: bounded by IMPAIR_SOAK_MSGS.
        for (uint32_t i = 0U; i < IMPAIR_SOAK_MSGS; ++i) {
            MessageEnvelope env;
            envelope_init(env);
            env.message_type      = MessageType::DATA;
            env.source_id         = 1U;
            env.destination_id    = 2U;
            env.message_id        = static_cast<uint64_t>(i) + 1ULL;
            env.timestamp_us      = static_cast<uint64_t>(i) * 5000ULL;  // 5 ms steps
            env.reliability_class = ReliabilityClass::BEST_EFFORT;
            env.payload_length    = 0U;
            env.fragment_index    = 0U;
            env.fragment_count    = 1U;
            env.total_payload_length = 0U;

            const uint64_t now_us = static_cast<uint64_t>(i) * 5000ULL;

            // process_outbound may return OK (accepted), ERR_IO (dropped),
            // or ERR_FULL (delay buffer full) — all are valid outcomes.
            Result r = engine.process_outbound(env, now_us);
            (void)r;  // outcome accepted: stress verifies no crash/assert

            // Collect messages that have become deliverable by now_us.
            uint32_t n = engine.collect_deliverable(now_us, out_buf, IMPAIR_OUT_BUF_SIZE);
            assert(n <= IMPAIR_OUT_BUF_SIZE);
            // CERT INT30-C: total_delivered + n ≤ IMPAIR_SOAK_MSGS * 2 (checked at end).
            total_delivered += n;
        }

        // Final drain: advance 10 s past the last send to release everything
        // still in the delay buffer (latency + jitter capped well below 10 s).
        const uint64_t drain_us = static_cast<uint64_t>(IMPAIR_SOAK_MSGS) * 5000ULL
                                  + 10000000ULL;

        // Bounded drain loop: at most IMPAIR_DELAY_BUF_SIZE messages remain.
        // Power of 10 Rule 2: bounded by IMPAIR_DELAY_BUF_SIZE (compile-time).
        for (uint32_t d = 0U; d < IMPAIR_DELAY_BUF_SIZE; ++d) {
            uint32_t n = engine.collect_deliverable(drain_us, out_buf, IMPAIR_OUT_BUF_SIZE);
            assert(n <= IMPAIR_OUT_BUF_SIZE);
            total_delivered += n;
            if (n == 0U) {
                break;
            }
        }

        // Verify sanity: no message fabrication (duplication at most doubles each).
        assert(total_delivered <= IMPAIR_SOAK_MSGS * 2U);
        // At 5% loss, expect at least 50% delivery over 2000 messages.
        assert(total_delivered >= IMPAIR_SOAK_MSGS / 2U);

        ++total_rounds;
    }

    return total_rounds;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const time_t duration_secs = parse_duration_secs(argc, argv);

    assert(duration_secs > 0);

    printf("=== Stress: test_stress_e2e ===\n");
    printf("    (each test runs for %lu s)\n",
           static_cast<unsigned long>(duration_secs));

    // Test 7: DuplicateFilter wraparound at UINT64_MAX
    printf("  Test 7: DuplicateFilter UINT64_MAX wraparound (%u IDs/round)...",
           DEDUP_WRAP_MSGS);
    uint32_t r7 = test_dedup_uint64_wraparound(time(nullptr) + duration_secs);
    assert(r7 > 0U);
    printf(" PASS (%u total IDs across %u rounds)\n",
           r7, r7 / DEDUP_WRAP_MSGS);

    // Test 1: E2E throughput soak
    printf("  Test 1: E2E throughput soak (%u messages/round)...", E2E_SOAK_MSGS);
    uint32_t r1 = test_e2e_throughput_soak(time(nullptr) + duration_secs);
    assert(r1 > 0U);
    printf(" PASS (%u total messages across %u rounds)\n",
           r1, r1 / E2E_SOAK_MSGS);

    // Test 2: Fragmentation/reassembly slot recycling
    printf("  Test 2: Frag/reassembly soak (%u complete + %u stale cycles/round)...",
           FRAG_COMPLETE_CYCLES, FRAG_STALE_CYCLES);
    uint32_t r2 = test_frag_reassembly_soak(time(nullptr) + duration_secs);
    assert(r2 > 0U);
    printf(" PASS (%u total cycles)\n", r2);

    // Test 5: ImpairmentEngine combined-fault saturation
    printf("  Test 5: ImpairmentEngine saturation soak (%u messages/round)...",
           IMPAIR_SOAK_MSGS);
    uint32_t r5 = test_impairment_saturation_soak(time(nullptr) + duration_secs);
    assert(r5 > 0U);
    printf(" PASS (%u total rounds)\n", r5);

    printf("=== test_stress_e2e: ALL PASSED ===\n");
    return 0;
}
