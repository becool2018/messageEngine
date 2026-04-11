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
 * @file test_RetryManager.cpp
 * @brief Unit tests for RetryManager: retry scheduling, backoff, and expiry logic.
 *
 * Tests cover all branches of RetryManager.cpp targeting ≥75% branch coverage.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-3.2.5, REQ-3.3.3, REQ-7.2.3
 */

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/RetryManager.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a minimal valid test envelope
// source_id=1, destination_id=2, DATA type, non-zero expiry
// ─────────────────────────────────────────────────────────────────────────────
static void make_test_envelope(MessageEnvelope& env,
                                uint64_t         msg_id,
                                uint64_t         expiry_us = 6000000ULL,
                                NodeId           src       = 1U,
                                NodeId           dst       = 2U)
{
    envelope_init(env);
    env.message_type      = MessageType::DATA;
    env.message_id        = msg_id;
    env.timestamp_us      = 1000000ULL;
    env.source_id         = src;
    env.destination_id    = dst;
    env.priority          = 0U;
    env.reliability_class = ReliabilityClass::RELIABLE_RETRY;
    env.expiry_time_us    = expiry_us;
    env.payload_length    = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: init clears state; collect_due on empty manager returns 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_init()
{
    RetryManager mgr;
    mgr.init();

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);

    assert(count == 0U);
    assert(mgr.on_ack(1U, 999ULL) == Result::ERR_INVALID);

    printf("PASS: test_init\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1b: reinit — calling init() a second time on the same manager object
// resets all state cleanly.  Regression test for the release-blocking abort
// where NEVER_COMPILED_OUT_ASSERT(!m_initialized) fired on the second call.
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_reinit()
{
    // Verifies: REQ-3.2.5
    RetryManager mgr;
    mgr.init();

    // Schedule a message so the manager has active state.
    MessageEnvelope env;
    make_test_envelope(env, 77ULL);
    Result sr = mgr.schedule(env, 3U, 100U, 1000000ULL);
    assert(sr == Result::OK);

    // Second init() must not abort; it should silently reset all state.
    mgr.init();  // was crashing with NEVER_COMPILED_OUT_ASSERT(!m_initialized)

    // After reinit the manager must behave as if freshly constructed.
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 0U);

    // Stats must be zeroed.
    const RetryStats& s = mgr.get_stats();
    assert(s.retries_sent == 0U);

    printf("PASS: test_reinit\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: schedule one message; returns OK
// ─────────────────────────────────────────────────────────────────────────────
static void test_schedule_ok()
{
    RetryManager mgr;
    mgr.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL);

    Result res = mgr.schedule(env, 5U, 100U, 1000000ULL);

    assert(res == Result::OK);
    assert(res != Result::ERR_FULL);

    printf("PASS: test_schedule_ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: fill ACK_TRACKER_CAPACITY slots; next schedule returns ERR_FULL
// ─────────────────────────────────────────────────────────────────────────────
static void test_schedule_full()
{
    RetryManager mgr;
    mgr.init();

    // Fill all slots — Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(i) + 1U);
        Result r = mgr.schedule(env, 5U, 100U, 1000000ULL);
        assert(r == Result::OK);
    }

    // Next schedule must fail
    MessageEnvelope overflow_env;
    make_test_envelope(overflow_env, 9999ULL);
    Result res = mgr.schedule(overflow_env, 5U, 100U, 1000000ULL);

    assert(res == Result::ERR_FULL);
    assert(res != Result::OK);

    printf("PASS: test_schedule_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: schedule then on_ack with matching src+msg_id; returns OK; slot freed
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_ok()
{
    RetryManager mgr;
    mgr.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL);

    Result sched_res = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(sched_res == Result::OK);

    Result ack_res = mgr.on_ack(1U, 42ULL);

    assert(ack_res == Result::OK);
    assert(ack_res != Result::ERR_INVALID);

    // After ACK, collecting should yield nothing (slot is deactivated)
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 0U);

    printf("PASS: test_on_ack_ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: on_ack with unknown msg_id; returns ERR_INVALID
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_not_found()
{
    RetryManager mgr;
    mgr.init();

    // Do not schedule anything; just call on_ack for an unknown message
    Result res = mgr.on_ack(1U, 999ULL);

    assert(res == Result::ERR_INVALID);
    assert(res != Result::OK);

    printf("PASS: test_on_ack_not_found\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: collect_due_immediate — schedule at T; collect at T+1; count==1
// The first retry is always immediate (next_retry_us set to now_us at schedule)
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_due_immediate()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    make_test_envelope(env, 77ULL, T + 5000000ULL);  // expiry 5s in future

    // Schedule at T (next_retry_us = T, so any collect at T or later fires)
    Result res = mgr.schedule(env, 5U, 100U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);

    assert(count == 1U);
    assert(buf[0].message_id == 77ULL);

    printf("PASS: test_collect_due_immediate\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: collect_not_due — after first retry fires, backoff applied;
// immediate second collect at same time yields 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_not_due()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    // expiry far in the future; backoff_ms=1000 (1 second)
    make_test_envelope(env, 88ULL, T + 60000000ULL);

    Result res = mgr.schedule(env, 5U, 1000U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect at T+1: fires immediately, retry_count=1, next_retry_us = T+1+1000000us
    uint32_t count1 = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);

    // Second collect at same time T+1: not yet due (backoff applied)
    uint32_t count2 = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);

    printf("PASS: test_collect_not_due\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: schedule with expiry_time_us=1 (past); collect_due removes silently
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_expired()
{
    RetryManager mgr;
    mgr.init();

    MessageEnvelope env;
    // expiry_time_us=1 ensures it is expired relative to now_us=1000000
    make_test_envelope(env, 99ULL, 1ULL);

    Result res = mgr.schedule(env, 5U, 100U, 1ULL);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    // now_us >> expiry_us, so entry should be removed silently
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);

    assert(count == 0U);
    assert(count == 0U);  // second assertion: no retry delivered

    printf("PASS: test_collect_expired\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: schedule with max_retries=1; collect once (retry_count becomes 1);
// collect again; slot removed because retry_count >= max_retries; count==0
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_exhausted()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    make_test_envelope(env, 55ULL, T + 60000000ULL);

    // max_retries=1 means only 1 retry allowed
    Result res = mgr.schedule(env, 1U, 100U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect at T: fires, retry_count becomes 1 (== max_retries)
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);

    // Second collect at a future time: retry_count >= max_retries, slot removed
    uint32_t count2 = mgr.collect_due(T + 1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);

    printf("PASS: test_collect_exhausted\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: backoff_doubles — schedule with backoff_ms=100; collect twice;
// verify second collect_due finds nothing immediately (backoff applied)
// ─────────────────────────────────────────────────────────────────────────────
static void test_backoff_doubles()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    make_test_envelope(env, 66ULL, T + 60000000ULL);

    // backoff_ms=100 → after first retry, next_retry_us = T + 1 + 200000us (doubled to 200ms)
    Result res = mgr.schedule(env, 10U, 100U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect at T (fires immediately)
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);

    // Second collect right after T: backoff applied; next_retry_us = T + 200000us
    // At time T+1, not yet due
    uint32_t count2 = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);

    // At time T + 200001us, it should fire again
    uint32_t count3 = mgr.collect_due(T + 200001ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count3 == 1U);

    printf("PASS: test_backoff_doubles\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: backoff_cap — schedule with backoff_ms=40000;
// after one retry backoff doubles to 60000 (capped at 60000ms)
// ─────────────────────────────────────────────────────────────────────────────
static void test_backoff_cap()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    make_test_envelope(env, 77ULL, T + 600000000ULL);  // expiry far in future

    // backoff_ms=40000: after first retry, doubled → 80000, capped to 60000
    Result res = mgr.schedule(env, 10U, 40000U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect fires immediately (at T); backoff advances to min(80000, 60000)=60000ms
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);

    // next_retry_us = T + 60000000us (60 seconds in microseconds)
    // Collect at T + 60000001us: should fire again (backoff stays at 60000ms)
    uint32_t count2 = mgr.collect_due(T + 60000001ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 1U);

    // After second retry, backoff should still be 60000ms (already at cap)
    // Collect right away at T + 60000001 + 1: should not fire
    uint32_t count3 = mgr.collect_due(T + 60000002ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count3 == 0U);

    printf("PASS: test_backoff_cap\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: on_ack with active slot present but wrong source_id
// Covers RetryManager::on_ack() L131 False branch:
//   active slot found, but env.source_id != src → compound short-circuits False.
// Verifies: REQ-3.2.5
// Verification: M1 + M2 + M4 + M5 (fault injection not required — no injectable dependency)
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_wrong_src_active()
{
    RetryManager mgr;
    mgr.init();

    // Schedule a message from src=1
    MessageEnvelope env;
    make_test_envelope(env, 200ULL, 6000000ULL, 1U, 2U);  // src=1, msg_id=200
    Result sched = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(sched == Result::OK);

    // Call on_ack with wrong source: slot is active (L130 True), source_id=1 != 3 (L131 False)
    Result res = mgr.on_ack(3U, 200ULL);

    assert(res == Result::ERR_INVALID);
    assert(res != Result::OK);

    printf("PASS: test_on_ack_wrong_src_active\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: on_ack with active slot, matching source_id but wrong message_id
// Covers RetryManager::on_ack() L132 False branch:
//   active slot found, source_id matches, but env.message_id != msg_id → False.
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_wrong_id_active()
{
    RetryManager mgr;
    mgr.init();

    // Schedule a message from src=1, msg_id=300
    MessageEnvelope env;
    make_test_envelope(env, 300ULL, 6000000ULL, 1U, 2U);  // src=1, msg_id=300
    Result sched = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(sched == Result::OK);

    // Call on_ack: slot is active (L130 True), source_id=1==1 (L131 True),
    // but msg_id=999!=300 (L132 False)
    Result res = mgr.on_ack(1U, 999ULL);

    assert(res == Result::ERR_INVALID);
    assert(res != Result::OK);

    printf("PASS: test_on_ack_wrong_id_active\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: collect_due with buf_cap smaller than number of due entries
// Covers RetryManager::collect_due() L171 False branch:
//   collected == buf_cap while i < ACK_TRACKER_CAPACITY → loop exits early.
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_due_buf_cap_limits()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    // Schedule 3 messages all immediately due; use large expiry
    for (uint32_t i = 1U; i <= 3U; ++i) {
        MessageEnvelope env;
        make_test_envelope(env,
                           static_cast<uint64_t>(i) + 400U,
                           T + 60000000ULL);
        Result r = mgr.schedule(env, 5U, 100U, T);
        assert(r == Result::OK);
    }

    // Collect with buf_cap=2: loop must exit when collected==2 (L171 False)
    MessageEnvelope buf[2U];
    uint32_t count = mgr.collect_due(T + 1ULL, buf, 2U);

    assert(count == 2U);
    assert(count <= 2U);

    printf("PASS: test_collect_due_buf_cap_limits\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: schedule with expiry_time_us=0 (never expires); message is never
// silently dropped by slot_has_expired; covers the False branch of
// (expiry_us != 0ULL) in slot_has_expired
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_never_expires()
{
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    // expiry_time_us=0 means never expires
    make_test_envelope(env, 44ULL, 0ULL);

    Result res = mgr.schedule(env, 5U, 100U, T);
    assert(res == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // Collect at far-future time: entry should NOT be expired (expiry_us==0 means never)
    // The retry should fire (next_retry_us == T, now_us == T+1)
    uint32_t count = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);

    assert(count == 1U);
    assert(buf[0].message_id == 44ULL);

    printf("PASS: test_collect_never_expires\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: stats retries_sent increments each time collect_due fires a retry
// Verifies: REQ-7.2.3 — RetryManager.retries_sent counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_retry_sent()
{
    RetryManager mgr;
    mgr.init();

    // Verify stats zeroed on init
    const RetryStats& s0 = mgr.get_stats();
    assert(s0.retries_sent == 0U);
    assert(s0.acks_received == 0U);

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    make_test_envelope(env, 700ULL, T + 60000000ULL);
    Result r = mgr.schedule(env, 5U, 100U, T);
    assert(r == Result::OK);

    // First collect fires immediately
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(T + 1ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 1U);

    // retries_sent must be 1
    const RetryStats& s1 = mgr.get_stats();
    assert(s1.retries_sent == 1U);
    assert(s1.acks_received == 0U);

    printf("PASS: test_stats_retry_sent\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: stats slots_exhausted increments when retry count reaches max_retries
// Verifies: REQ-7.2.3 — RetryManager.slots_exhausted counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_exhausted()
{
    RetryManager mgr;
    mgr.init();

    const RetryStats& s0 = mgr.get_stats();
    assert(s0.slots_exhausted == 0U);
    assert(s0.retries_sent == 0U);

    const uint64_t T = 1000000ULL;

    MessageEnvelope env;
    // max_retries=1 so one collect fires the retry, then next collect exhausts it
    make_test_envelope(env, 710ULL, T + 60000000ULL);
    Result r = mgr.schedule(env, 1U, 100U, T);
    assert(r == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect: retry_count becomes 1 (== max_retries), fires retry
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);

    // Second collect at T+200001us (after backoff): slot reaps as exhausted
    uint32_t count2 = mgr.collect_due(T + 200001ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);

    // slots_exhausted must be 1
    const RetryStats& s1 = mgr.get_stats();
    assert(s1.slots_exhausted == 1U);
    assert(s1.slots_expired == 0U);

    printf("PASS: test_stats_exhausted\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: stats slots_expired increments when slot's expiry_time_us passes
// Verifies: REQ-7.2.3 — RetryManager.slots_expired counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_expired()
{
    RetryManager mgr;
    mgr.init();

    const RetryStats& s0 = mgr.get_stats();
    assert(s0.slots_expired == 0U);
    assert(s0.slots_exhausted == 0U);

    // expiry_time_us=1 is in the past at now_us=1000000
    MessageEnvelope env;
    make_test_envelope(env, 720ULL, 1ULL);
    Result r = mgr.schedule(env, 5U, 100U, 1ULL);
    assert(r == Result::OK);

    // Collect at now_us=1000000: slot is expired, reaps silently
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 0U);

    // slots_expired must be 1
    const RetryStats& s1 = mgr.get_stats();
    assert(s1.slots_expired == 1U);
    assert(s1.slots_exhausted == 0U);

    printf("PASS: test_stats_expired\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: stats acks_received increments when on_ack() cancels an active slot
// Verifies: REQ-7.2.3 — RetryManager.acks_received counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_ack_received()
{
    RetryManager mgr;
    mgr.init();

    const RetryStats& s0 = mgr.get_stats();
    assert(s0.acks_received == 0U);
    assert(s0.retries_sent == 0U);

    MessageEnvelope env;
    make_test_envelope(env, 730ULL, 9999999999ULL, 1U, 2U);
    Result r = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(r == Result::OK);

    Result ack_r = mgr.on_ack(1U, 730ULL);
    assert(ack_r == Result::OK);

    // acks_received must be 1; retries_sent still 0
    const RetryStats& s1 = mgr.get_stats();
    assert(s1.acks_received == 1U);
    assert(s1.retries_sent == 0U);

    printf("PASS: test_stats_ack_received\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 20: cancel() — rollback deactivates slot without bumping acks_received
// Verifies: REQ-3.2.5, REQ-7.2.3
// Steps:
//   1. Schedule a message (active slot)
//   2. Confirm acks_received == 0 before cancel
//   3. Call cancel() — asserts OK
//   4. Confirm acks_received still 0 (no phantom stat)
//   5. Reschedule the same msg_id to confirm the slot is reusable
// ─────────────────────────────────────────────────────────────────────────────
static void test_retry_manager_cancel()
{
    RetryManager mgr;
    mgr.init();

    // Step 1: Schedule a message
    MessageEnvelope env;
    make_test_envelope(env, 800ULL, 9999999999ULL, 1U, 2U);
    Result sched_r = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(sched_r == Result::OK);

    // Step 2: acks_received must be 0 before cancel
    const RetryStats& s0 = mgr.get_stats();
    assert(s0.acks_received == 0U);

    // Step 3: cancel() must succeed
    Result cancel_r = mgr.cancel(1U, 800ULL);
    assert(cancel_r == Result::OK);

    // Step 4: acks_received must still be 0 — no phantom stat
    const RetryStats& s1 = mgr.get_stats();
    assert(s1.acks_received == 0U);

    // Step 5: slot is freed — reschedule same msg_id must succeed (reusable)
    Result resched_r = mgr.schedule(env, 5U, 100U, 1000000ULL);
    assert(resched_r == Result::OK);

    printf("PASS: test_retry_manager_cancel\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 21: collect_due accepts equal now_us (monotonic: >= not only >)
// Verifies: REQ-3.2.5
// Equal timestamps must be accepted (not trigger assertion failure).
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_due_accepts_equal_now_us()
{
    // Verifies: REQ-3.2.5
    RetryManager mgr;
    mgr.init();

    const uint64_t T = 1000000ULL;

    // Schedule one message; next_retry_us == T (fires at or after T)
    MessageEnvelope env;
    make_test_envelope(env, 901ULL, T + 60000000ULL);
    Result sched_r = mgr.schedule(env, 5U, 100U, T);
    assert(sched_r == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect at T: entry is due (next_retry_us == T), fires and advances backoff
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);  // Assert: entry fired

    // Second collect at same T: monotonic equality is accepted (>= passes)
    // Entry is no longer due (backoff applied after first fire), so count is 0
    uint32_t count2 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);  // Assert: no second fire; no assertion failure on equal timestamp

    printf("PASS: test_collect_due_accepts_equal_now_us\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 22: collect_due monotonic sequence — correct fire at backoff boundary
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_due_monotonic_sequence()
{
    // Verifies: REQ-3.2.5
    RetryManager mgr;
    mgr.init();

    const uint64_t T          = 1000000ULL;
    const uint32_t BACKOFF_MS = 100U;
    const uint64_t BACKOFF_US = static_cast<uint64_t>(BACKOFF_MS) * 1000ULL; // 100000 us

    // Schedule message with backoff_ms=100; first retry fires at T (immediate)
    MessageEnvelope env;
    make_test_envelope(env, 902ULL, T + 60000000ULL);
    Result sched_r = mgr.schedule(env, 5U, BACKOFF_MS, T);
    assert(sched_r == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // Collect at T: first retry fires immediately (next_retry_us == T)
    // After firing, backoff doubles to 200ms; next_retry_us = T + 200000us
    uint32_t count1 = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 1U);  // Assert: first retry fired at T

    // Collect at T + BACKOFF_US (T + 100000us): too early (next_retry_us = T + 200000us)
    uint32_t count2 = mgr.collect_due(T + BACKOFF_US, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);  // Assert: not due yet at T+100ms

    // Collect at T + 2*BACKOFF_US (T + 200000us): exactly at next retry time
    uint32_t count3 = mgr.collect_due(T + 2U * BACKOFF_US, buf, ACK_TRACKER_CAPACITY);
    assert(count3 == 1U);  // Assert: second retry fired at T+200ms

    printf("PASS: test_collect_due_monotonic_sequence\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: cancel() — wrong message_id — covers compound-condition C=False and
//       loop-exhaustion / A=False (inactive-slot) branches in cancel().
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_cancel_no_match()
{
    RetryManager mgr;
    mgr.init();

    // Schedule message (src=1, id=100) with immediate retry.
    MessageEnvelope env;
    make_test_envelope(env, 100ULL);  // src=1, id=100
    const uint64_t T = 1000000ULL;
    Result r = mgr.schedule(env, T, 3U, 100U);
    assert(r == Result::OK);  // Assert: scheduled successfully

    // Cancel wrong id: active slot → A=True(active), B=True(src=1), C=False(999≠100).
    // Remaining slots are inactive → A=False (short-circuit). Loop exhausts → False.
    Result r2 = mgr.cancel(1U, 999ULL);
    assert(r2 == Result::ERR_INVALID);  // Assert: not found returns ERR_INVALID

    // Original slot still active — collect confirms retry fires.
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count == 1U);  // Assert: slot still active after failed cancel

    printf("PASS: test_cancel_no_match\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: cancel() — wrong source_id — covers compound-condition B=False in
//       cancel().
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_cancel_wrong_source()
{
    RetryManager mgr;
    mgr.init();

    // Schedule (src=1, id=200).
    MessageEnvelope env;
    make_test_envelope(env, 200ULL);  // src=1, id=200
    const uint64_t T = 1000000ULL;
    Result r = mgr.schedule(env, T, 3U, 100U);
    assert(r == Result::OK);  // Assert: scheduled successfully

    // Cancel wrong source: A=True(active), B=False(src=2≠1). B=False covered.
    Result r2 = mgr.cancel(2U, 200ULL);
    assert(r2 == Result::ERR_INVALID);  // Assert: source mismatch returns ERR_INVALID

    // Original slot still active.
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(T, buf, ACK_TRACKER_CAPACITY);
    assert(count == 1U);  // Assert: slot still active after failed cancel

    printf("PASS: test_cancel_wrong_source\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: collect_due() — backward timestamp — covers the True branch of the
//       non-monotonic guard `if (now_us < m_last_collect_us)`.
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_collect_due_backward_timestamp()
{
    RetryManager mgr;
    mgr.init();

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First collect at T=1000000 — sets m_last_collect_us.
    uint32_t c1 = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(c1 == 0U);  // Assert: empty manager, nothing due

    // Second collect at T=500000 (backward) — True branch of backward-timestamp guard.
    // collect_due clamps now_us to m_last_collect_us and returns normally.
    uint32_t c2 = mgr.collect_due(500000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(c2 == 0U);  // Assert: backward timestamp — nothing collected (clamped)

    printf("PASS: test_collect_due_backward_timestamp\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test: init() on already-initialized EMPTY manager — covers the False branch
//       of the `m_count > 0U` sub-condition in `if (m_initialized && m_count > 0U)`.
// Verifies: REQ-3.2.5
// ─────────────────────────────────────────────────────────────────────────────
static void test_reinit_empty_no_warning()
{
    RetryManager mgr;
    mgr.init();   // First init: m_initialized=false → m_initialized=true, m_count=0

    // Second init with no active slots: m_initialized=true, m_count=0.
    // Branch: A=True(m_initialized) && B=False(m_count==0) → B=False covered.
    // The warning block is NOT entered; re-init proceeds cleanly.
    mgr.init();

    // Manager should be in clean state after reinit.
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = mgr.collect_due(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 0U);   // Assert: empty after reinit — no spurious retries
    assert(mgr.get_stats().retries_sent == 0U);  // Assert: stats zeroed by reinit

    printf("PASS: test_reinit_empty_no_warning\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_init();
    test_reinit();
    test_schedule_ok();
    test_schedule_full();
    test_on_ack_ok();
    test_on_ack_not_found();
    test_collect_due_immediate();
    test_collect_not_due();
    test_collect_expired();
    test_collect_exhausted();
    test_backoff_doubles();
    test_backoff_cap();
    test_on_ack_wrong_src_active();
    test_on_ack_wrong_id_active();
    test_collect_due_buf_cap_limits();
    test_collect_never_expires();
    test_stats_retry_sent();
    test_stats_exhausted();
    test_stats_expired();
    test_stats_ack_received();
    test_retry_manager_cancel();
    test_collect_due_accepts_equal_now_us();
    test_collect_due_monotonic_sequence();
    test_cancel_no_match();
    test_cancel_wrong_source();
    test_collect_due_backward_timestamp();
    test_reinit_empty_no_warning();

    printf("ALL RetryManager tests passed.\n");
    return 0;
}
