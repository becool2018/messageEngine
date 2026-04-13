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
 * @file test_stress_reconnect.cpp
 * @brief Sustained-load stress test for peer-reconnect ordering state reset.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * PURPOSE
 * ─────────────────────────────────────────────────────────────────────────────
 * Unit tests verify that OrderingBuffer::reset_peer() and
 * DeliveryEngine::reset_peer_ordering() clear all hold slots and sequence
 * state for a reconnecting peer at a few design points.  This stress test
 * verifies correctness over thousands of consecutive reconnect cycles,
 * driving the hold buffer into and out of a partially-held state each cycle.
 *
 * Any hold slot not freed by reset_peer() will cause ERR_FULL on the next
 * cycle's out-of-order inject.  Any stale sequence state (next_expected not
 * reset to 1) will cause seq=1 to be silently discarded as a duplicate-seq
 * instead of being delivered — tripping the assert in Phase D.
 *
 * The test accepts an optional runtime duration (seconds) from argv[1] and
 * runs as many complete RECONNECT_OB_CYCLES / RECONNECT_DE_CYCLES rounds as
 * fit in that window per test.  Pass no argument for a 60-second default.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * WHAT THIS TEST CATCHES
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Hold slot not freed by reset_peer() — the next cycle's out-of-order
 *    ingest would return ERR_FULL when all 8 hold slots are exhausted.
 *  • next_expected_seq not reset — seq=1 would be treated as a stale
 *    duplicate (< next_expected) and silently discarded instead of delivered,
 *    causing the assert(rr == Result::OK) in Phase D to fire.
 *  • m_held_pending not cleared on reset — DeliveryEngine::receive() would
 *    return the stale staged envelope instead of the new seq=1 message.
 *  • Spurious holds after reset — sweep_expired_holds() returning > 0
 *    after reset indicates a hold slot was leaked with active state.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * RULES APPLIED (tests/ relaxations per CLAUDE.md §1b / §9)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Rule 2 (fixed loop bounds)  — inner loops use compile-time constants;
 *                                outer duration loop per Rule 2 deviation.
 *  Rule 3 (no dynamic alloc)   — stack/static arrays only; no new/delete.
 *  Rule 5 (assertions)         — ≥2 assert() calls per function.
 *  Rule 7 (check returns)      — every Result return value is checked.
 *  Rule 10 (zero warnings)     — builds clean under -Wall -Wextra -Wpedantic.
 *  CC ≤ 15 for test functions  — raised ceiling per §1b; each function ≤ 12.
 *  assert() permitted          — tests/ exemption per §9.
 *
 * Verifies: REQ-3.3.6
 */
// Verification: M1 + M2 + M4

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>
#include <ctime>    // time_t, time()
#include <climits>  // UINT32_MAX, UINT64_MAX

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/OrderingBuffer.hpp"
#include "core/DeliveryEngine.hpp"      // brings in ChannelConfig
#include "platform/LocalSimHarness.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time constants
// ─────────────────────────────────────────────────────────────────────────────

/// Cycles per round for Test 1 (direct OrderingBuffer reconnect storm).
static const uint32_t RECONNECT_OB_CYCLES = 500U;

/// Number of in-order messages delivered before each reconnect in Test 1.
/// Must be < ORDERING_HOLD_COUNT so Phase B still has a free hold slot.
static const uint32_t RECONNECT_ADVANCE   = 5U;

/// Source peer ID used in Test 1.
static const NodeId   RECONNECT_SRC       = 10U;

/// Cycles per round for Test 2 (DeliveryEngine reconnect storm).
static const uint32_t RECONNECT_DE_CYCLES = 100U;

/// Ordered messages delivered per cycle before reconnect in Test 2.
static const uint32_t RECONNECT_DE_MSGS   = 5U;

/// Node IDs for Test 2.
static const NodeId   RECONNECT_NODE_A    = 11U;
static const NodeId   RECONNECT_NODE_B    = 12U;

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
// Helper: build a DATA envelope with specific sequence number and message ID.
// Sets fragment_count = 1 (single fragment, bypasses reassembly).
// Sets sequence_num > 0 (ORDERED, routes through ordering gate).
// ─────────────────────────────────────────────────────────────────────────────
static void make_reconnect_env(MessageEnvelope& env,
                                NodeId           src,
                                NodeId           dst,
                                uint32_t         seq,
                                uint64_t         msg_id,
                                uint64_t         now_us)
{
    assert(seq > 0U);   // sequence_num == 0 would bypass the ordering gate
    assert(msg_id > 0U);

    envelope_init(env);
    env.message_type         = MessageType::DATA;
    env.source_id            = src;
    env.destination_id       = dst;
    env.message_id           = msg_id;
    env.sequence_num         = seq;
    env.timestamp_us         = now_us;
    env.expiry_time_us       = now_us + 9000000ULL;  // 9 s — far beyond test end
    env.reliability_class    = ReliabilityClass::BEST_EFFORT;
    env.fragment_index       = 0U;
    env.fragment_count       = 1U;
    env.payload_length       = 4U;
    env.total_payload_length = 4U;
    // Encode low 16 bits of seq in payload for identity checks.
    env.payload[0] = static_cast<uint8_t>(seq & 0xFFU);
    env.payload[1] = static_cast<uint8_t>((seq >> 8U) & 0xFFU);
    env.payload[2] = 0U;
    env.payload[3] = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: OrderingBuffer reconnect storm — duration-bounded,
//         RECONNECT_OB_CYCLES gap→hold→reset→verify cycles per round.
// ─────────────────────────────────────────────────────────────────────────────
// Each cycle:
//   A. Reset state for this cycle (idempotent no-op for cycle 0).
//   B. Deliver RECONNECT_ADVANCE in-order messages to advance next_expected.
//   C. Inject one out-of-order message (gap, so it is held in the hold buffer).
//   D. Simulate reconnect: call reset_peer(src) — must clear hold + reset seq.
//   E. Verify seq=1 now delivers (next_expected reset to 1).
//   F. Verify no stale hold slots remain (sweep returns 0; try_release ERR_AGAIN).
// ─────────────────────────────────────────────────────────────────────────────
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop.  Per-iteration work is bounded by RECONNECT_OB_CYCLES (compile-time);
// loop terminates when the wall-clock reaches the deadline.
// Verifies: REQ-3.3.6
static uint64_t test_ordering_buffer_reconnect_storm(time_t deadline)
{
    // Static arrays — Power of 10 Rule 3: no dynamic allocation.
    static MessageEnvelope sweep_freed[ORDERING_HOLD_COUNT];
    static OrderingBuffer  buf;

    // init() once; reset_peer() handles per-cycle cleanup.
    buf.init(1U);

    uint64_t total_cycles = 0ULL;

    // Power of 10 Rule 2 deviation: duration-bounded outer loop.
    while (time(nullptr) < deadline) {

        // Power of 10 Rule 2: inner loop bounded by RECONNECT_OB_CYCLES.
        for (uint32_t cycle = 0U; cycle < RECONNECT_OB_CYCLES; ++cycle) {

            const uint64_t now_us = 1000000ULL
                                    + static_cast<uint64_t>(cycle) * 1000ULL;
            // Each cycle uses a disjoint msg_id space to ensure no confusion
            // between Phase B (hold) and Phase E (reconnect) message IDs.
            // CERT INT30-C: total_cycles fits in uint64_t; product with 1 000 000
            // cannot overflow until ~1.8 × 10^13 rounds (unreachable in practice).
            assert(total_cycles <= UINT64_MAX / 1000000ULL);
            const uint64_t id_base = total_cycles * 1000000ULL
                                     + static_cast<uint64_t>(cycle) * 10000ULL;

            // ── Step A: reset state left from previous cycle (idempotent). ────
            buf.reset_peer(RECONNECT_SRC);

            // ── Phase B: deliver RECONNECT_ADVANCE in-order messages. ─────────
            // Power of 10 Rule 2: bounded by RECONNECT_ADVANCE (compile-time).
            for (uint32_t s = 1U; s <= RECONNECT_ADVANCE; ++s) {
                MessageEnvelope msg;
                MessageEnvelope out;
                make_reconnect_env(msg, RECONNECT_SRC, 1U, s, id_base + s, now_us);
                envelope_init(out);
                Result ri = buf.ingest(msg, out, now_us);
                assert(ri == Result::OK);
                assert(out.sequence_num == s);
            }
            // next_expected for RECONNECT_SRC is now RECONNECT_ADVANCE + 1.

            // ── Phase C: inject out-of-order message (gap at ADVANCE+1). ──────
            // seq_hi = ADVANCE + 2 (skips ADVANCE+1, creating a gap).
            {
                const uint32_t  seq_hi  = RECONNECT_ADVANCE + 2U;
                const uint64_t  id_hi   = id_base + static_cast<uint64_t>(seq_hi);
                MessageEnvelope msg_hi;
                MessageEnvelope out_hi;
                make_reconnect_env(msg_hi, RECONNECT_SRC, 1U, seq_hi, id_hi, now_us);
                envelope_init(out_hi);
                Result rh = buf.ingest(msg_hi, out_hi, now_us);
                assert(rh == Result::ERR_AGAIN);  // held — gap exists at ADVANCE+1
            }

            // ── Phase D: simulate reconnect — reset peer ordering state. ───────
            buf.reset_peer(RECONNECT_SRC);

            // ── Phase E: verify seq=1 delivers (reset accepted fresh connection). ─
            {
                // Use a distinct ID pool (id_base + 5000) to avoid any msg_id
                // collision with Phase B or C IDs.
                const uint64_t  reconnect_id = id_base + 5000ULL;
                MessageEnvelope msg_new;
                MessageEnvelope out_new;
                make_reconnect_env(msg_new, RECONNECT_SRC, 1U, 1U,
                                   reconnect_id, now_us);
                envelope_init(out_new);
                Result rf = buf.ingest(msg_new, out_new, now_us);
                assert(rf == Result::OK);             // reset worked: seq=1 accepted
                assert(out_new.sequence_num == 1U);
            }

            // ── Phase F: verify no stale hold slots remain. ───────────────────
            {
                MessageEnvelope rel_out;
                envelope_init(rel_out);
                Result re = buf.try_release_next(RECONNECT_SRC, rel_out);
                assert(re == Result::ERR_AGAIN);   // hold buffer empty

                uint32_t freed = buf.sweep_expired_holds(now_us,
                                                          sweep_freed,
                                                          ORDERING_HOLD_COUNT);
                assert(freed == 0U);               // no stale holds
            }
        }

        total_cycles += RECONNECT_OB_CYCLES;
    }

    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: DeliveryEngine reconnect storm — exercises reset_peer_ordering()
//         via LocalSimHarness, verifying that m_held_pending and the ordering
//         gate are both cleared for a reconnecting peer.
// ─────────────────────────────────────────────────────────────────────────────
// Each round uses a fresh LocalSimHarness pair and a fresh DeliveryEngine.
// Within each round, RECONNECT_DE_CYCLES reconnect cycles run:
//   A. Inject RECONNECT_DE_MSGS ordered messages via ha.send_message / eb.receive.
//   B. Inject one out-of-order message — held by the ordering gate.
//   C. Call eb.reset_peer_ordering(RECONNECT_NODE_A).
//   D. Inject seq=1 with a fresh message_id — must deliver (reset accepted).
// ─────────────────────────────────────────────────────────────────────────────
// Notes:
//  • Messages are crafted with sequence_num > 0 so they route through the
//    ordering gate regardless of the channel's OrderingMode config.
//  • All message_ids are monotonically increasing across rounds to avoid
//    dedup-filter rejections.
//  • ha.send_message() / eb.receive() bypass ea entirely so the test controls
//    all sequence numbers directly.
//  • LocalSimHarness::pop_hello_peer() is a no-op (returns NODE_ID_INVALID),
//    so drain_hello_reconnects() inside eb.receive() is always a no-op here.
//
// Power of 10 Rule 2 deviation: outer while loop is a duration-bounded test
// loop.  Per-iteration work is bounded by RECONNECT_DE_CYCLES (compile-time);
// loop terminates when the wall-clock reaches the deadline.
// Verifies: REQ-3.3.6
static uint64_t test_delivery_engine_reconnect_storm(time_t deadline)
{
    uint64_t total_cycles = 0ULL;
    // msg_id is monotonically increasing across ALL rounds — CERT INT30-C:
    // max = 64-bit counter, incremented at most once per envelope; no overflow risk.
    uint64_t msg_id = 1ULL;

    // Power of 10 Rule 2 deviation: duration-bounded outer loop.
    while (time(nullptr) < deadline) {

        // Fresh harness and engine pair each round for clean state isolation.
        LocalSimHarness ha;
        LocalSimHarness hb;

        TransportConfig cfg_a;
        transport_config_default(cfg_a);
        cfg_a.kind          = TransportKind::LOCAL_SIM;
        cfg_a.local_node_id = RECONNECT_NODE_A;
        cfg_a.is_server     = false;

        TransportConfig cfg_b;
        transport_config_default(cfg_b);
        cfg_b.kind          = TransportKind::LOCAL_SIM;
        cfg_b.local_node_id = RECONNECT_NODE_B;
        cfg_b.is_server     = false;

        Result ra = ha.init(cfg_a);
        assert(ra == Result::OK);
        Result rb = hb.init(cfg_b);
        assert(rb == Result::OK);

        ha.link(&hb);
        hb.link(&ha);

        // eb receives via the ordering gate (sequence_num > 0 triggers it
        // regardless of ChannelConfig::ordering setting — bypass rule only
        // fires when sequence_num == 0).
        DeliveryEngine eb;
        eb.init(&hb, cfg_b.channels[0], RECONNECT_NODE_B);

        // curr_seq: the current sequence number eb expects next for RECONNECT_NODE_A.
        // Starts at 1 each round (fresh engine).  After each cycle's reset+deliver(seq=1),
        // curr_seq becomes 2 for all subsequent cycles in the round.
        uint32_t curr_seq = 1U;

        // Power of 10 Rule 2: inner loop bounded by RECONNECT_DE_CYCLES.
        for (uint32_t cycle = 0U; cycle < RECONNECT_DE_CYCLES; ++cycle) {

            const uint64_t now_us = 1000000ULL
                                    + static_cast<uint64_t>(cycle) * 1000ULL;

            // ── Phase A: deliver RECONNECT_DE_MSGS ordered messages in sequence. ─
            // Power of 10 Rule 2: bounded by RECONNECT_DE_MSGS (compile-time).
            for (uint32_t i = 0U; i < RECONNECT_DE_MSGS; ++i) {
                // CERT INT30-C: curr_seq + i; max = 2 + 5 - 1 = 6; no overflow.
                assert(curr_seq <= UINT32_MAX - i);
                const uint32_t seq_i = curr_seq + i;

                MessageEnvelope env;
                make_reconnect_env(env, RECONNECT_NODE_A, RECONNECT_NODE_B,
                                   seq_i, msg_id, now_us);
                ++msg_id;

                Result sr = ha.send_message(env);
                assert(sr == Result::OK);

                MessageEnvelope out;
                envelope_init(out);
                Result rr = eb.receive(out, 0U, now_us);
                assert(rr == Result::OK);
                assert(out.sequence_num == seq_i);
            }
            // next_expected for RECONNECT_NODE_A = curr_seq + RECONNECT_DE_MSGS.

            // ── Phase B: inject one out-of-order message — must be held. ─────
            // Gap at curr_seq+MSGS; hold curr_seq+MSGS+1.
            {
                // CERT INT30-C: curr_seq + RECONNECT_DE_MSGS + 1 ≤ 2 + 5 + 1 = 8; no overflow.
                assert(curr_seq <= UINT32_MAX - RECONNECT_DE_MSGS - 1U);
                const uint32_t seq_ooo = curr_seq + RECONNECT_DE_MSGS + 1U;

                MessageEnvelope env_ooo;
                make_reconnect_env(env_ooo, RECONNECT_NODE_A, RECONNECT_NODE_B,
                                   seq_ooo, msg_id, now_us);
                ++msg_id;

                Result sr = ha.send_message(env_ooo);
                assert(sr == Result::OK);

                MessageEnvelope out_ooo;
                envelope_init(out_ooo);
                Result rr = eb.receive(out_ooo, 0U, now_us);
                assert(rr == Result::ERR_AGAIN);  // held — gap exists
            }

            // ── Phase C: simulate peer reconnect — reset ordering state. ─────
            eb.reset_peer_ordering(RECONNECT_NODE_A);

            // ── Phase D: inject seq=1 (fresh reconnect) — must deliver. ──────
            {
                MessageEnvelope env_new;
                make_reconnect_env(env_new, RECONNECT_NODE_A, RECONNECT_NODE_B,
                                   1U, msg_id, now_us);
                ++msg_id;

                Result sr = ha.send_message(env_new);
                assert(sr == Result::OK);

                MessageEnvelope out_new;
                envelope_init(out_new);
                Result rr = eb.receive(out_new, 0U, now_us);
                assert(rr == Result::OK);           // reset worked: seq=1 accepted
                assert(out_new.sequence_num == 1U);
            }

            // After Phase D: next_expected for RECONNECT_NODE_A = 2.
            // All subsequent cycles start Phase A at seq=2.
            curr_seq = 2U;
        }

        ha.close();
        hb.close();
        total_cycles += RECONNECT_DE_CYCLES;
    }

    return total_cycles;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    const time_t duration_secs = parse_duration_secs(argc, argv);
    assert(duration_secs > 0);

    printf("=== Stress: test_stress_reconnect ===\n");
    printf("    (running for %lu s per test; %u OB cycles/round; %u DE cycles/round)\n",
           static_cast<unsigned long>(duration_secs),
           RECONNECT_OB_CYCLES, RECONNECT_DE_CYCLES);

    printf("  Test 1: OrderingBuffer reconnect storm...");
    const uint64_t ob_total = test_ordering_buffer_reconnect_storm(
                                  time(nullptr) + duration_secs);
    assert(ob_total > 0ULL);
    printf(" PASS (%llu total cycles across %llu rounds)\n",
           static_cast<unsigned long long>(ob_total),
           static_cast<unsigned long long>(ob_total / RECONNECT_OB_CYCLES));

    printf("  Test 2: DeliveryEngine reconnect storm...");
    const uint64_t de_total = test_delivery_engine_reconnect_storm(
                                  time(nullptr) + duration_secs);
    assert(de_total > 0ULL);
    printf(" PASS (%llu total cycles across %llu rounds)\n",
           static_cast<unsigned long long>(de_total),
           static_cast<unsigned long long>(de_total / RECONNECT_DE_CYCLES));

    printf("=== test_stress_reconnect: ALL PASSED ===\n");
    return 0;
}
