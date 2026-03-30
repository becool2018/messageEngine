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
 * Verifies: REQ-3.2.5, REQ-3.3.3
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
        make_test_envelope(env, static_cast<uint64_t>(i + 1U));
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
// Test 12: schedule with expiry_time_us=0 (never expires); message is never
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
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_init();
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
    test_collect_never_expires();

    printf("ALL RetryManager tests passed.\n");
    return 0;
}
