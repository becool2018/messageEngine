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
 * @file test_AckTracker.cpp
 * @brief Unit tests for AckTracker: ACK tracking, expiry sweep, and capacity limits.
 *
 * Tests cover all branches of AckTracker.cpp targeting ≥75% branch coverage.
 *
 * Rules applied:
 *   - Power of 10: fixed buffers, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 *
 * Verifies: REQ-3.2.4, REQ-3.3.2, REQ-7.2.3
 */
// Verification: M1 + M2 + M4 + M5 (fault injection not required — no injectable dependency)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/AckTracker.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper: build a minimal valid test envelope
// source_id=1, destination_id=2, DATA type, 5-second expiry from now=1000000
// ─────────────────────────────────────────────────────────────────────────────
static void make_test_envelope(MessageEnvelope& env,
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
    env.reliability_class = ReliabilityClass::RELIABLE_ACK;
    env.expiry_time_us    = 6000000ULL;  // 5 seconds from timestamp
    env.payload_length    = 0U;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: init clears state; sweep on empty tracker returns 0
// ─────────────────────────────────────────────────────────────────────────────
static void test_init()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = tracker.sweep_expired(1000000ULL, buf, ACK_TRACKER_CAPACITY);

    assert(count == 0U);
    assert(tracker.on_ack(1U, 999ULL) == Result::ERR_INVALID);

    printf("PASS: test_init\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: track one message; returns OK
// ─────────────────────────────────────────────────────────────────────────────
static void test_track_ok()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL);

    Result res = tracker.track(env, 9000000ULL);

    assert(res == Result::OK);
    assert(tracker.on_ack(1U, 42ULL) == Result::OK);

    printf("PASS: test_track_ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: fill all ACK_TRACKER_CAPACITY slots; next track returns ERR_FULL
// ─────────────────────────────────────────────────────────────────────────────
static void test_track_full()
{
    AckTracker tracker;
    tracker.init();

    // Fill all slots — Power of 10: bounded loop
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(i) + 1U);
        Result r = tracker.track(env, 9000000ULL);
        assert(r == Result::OK);
    }

    // Next track must fail
    MessageEnvelope overflow_env;
    make_test_envelope(overflow_env, 9999ULL);
    Result res = tracker.track(overflow_env, 9000000ULL);

    assert(res == Result::ERR_FULL);
    assert(res != Result::OK);

    printf("PASS: test_track_full\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: track then on_ack with matching src+msg_id; returns OK
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_ok()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL);

    Result track_res = tracker.track(env, 9000000ULL);
    assert(track_res == Result::OK);

    Result ack_res = tracker.on_ack(1U, 42ULL);

    assert(ack_res == Result::OK);
    assert(ack_res != Result::ERR_INVALID);

    printf("PASS: test_on_ack_ok\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: track msg_id=42 from src=1; ack src=2 msg_id=42; returns ERR_INVALID
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_wrong_src()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL, 1U, 2U);  // src=1

    Result track_res = tracker.track(env, 9000000ULL);
    assert(track_res == Result::OK);

    // ACK from wrong source (src=2 instead of src=1)
    Result ack_res = tracker.on_ack(2U, 42ULL);

    assert(ack_res == Result::ERR_INVALID);
    assert(ack_res != Result::OK);

    printf("PASS: test_on_ack_wrong_src\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: track msg_id=42 from src=1; ack src=1 msg_id=99; returns ERR_INVALID
// ─────────────────────────────────────────────────────────────────────────────
static void test_on_ack_wrong_id()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 42ULL, 1U, 2U);

    Result track_res = tracker.track(env, 9000000ULL);
    assert(track_res == Result::OK);

    // ACK with wrong message ID
    Result ack_res = tracker.on_ack(1U, 99ULL);

    assert(ack_res == Result::ERR_INVALID);
    assert(ack_res != Result::OK);

    printf("PASS: test_on_ack_wrong_id\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: track with deadline=1us (past); sweep returns it in buffer; count==1
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_expired_pending()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 55ULL);

    // Use a deadline in the past (deadline_us=1); now_us will be much larger
    Result track_res = tracker.track(env, 1ULL);
    assert(track_res == Result::OK);

    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];
    // now_us is well past deadline
    uint32_t count = tracker.sweep_expired(1000000ULL, expired_buf, ACK_TRACKER_CAPACITY);

    assert(count == 1U);
    assert(expired_buf[0].message_id == 55ULL);

    printf("PASS: test_sweep_expired_pending\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: track with far-future deadline; sweep returns count==0
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_pending_not_expired()
{
    AckTracker tracker;
    tracker.init();

    MessageEnvelope env;
    make_test_envelope(env, 77ULL);

    // Far-future deadline
    Result track_res = tracker.track(env, 0xFFFFFFFFFFFFFFFFULL - 1ULL);
    assert(track_res == Result::OK);

    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];
    uint32_t count = tracker.sweep_expired(1000000ULL, expired_buf, ACK_TRACKER_CAPACITY);

    assert(count == 0U);
    assert(expired_buf[0].message_id != 77ULL || count == 0U);

    printf("PASS: test_sweep_pending_not_expired\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: track, on_ack, sweep; slot freed (count==0); can track again
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_releases_acked()
{
    AckTracker tracker;
    tracker.init();

    // Fill all slots
    for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(i) + 1U);
        Result r = tracker.track(env, 0xFFFFFFFFFFFFFFFFULL - 1ULL);
        assert(r == Result::OK);
    }

    // ACK the first message (msg_id=1 from src=1)
    Result ack_res = tracker.on_ack(1U, 1ULL);
    assert(ack_res == Result::OK);

    // Sweep to process the ACKED state — this frees the slot
    MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];
    uint32_t expired_count = tracker.sweep_expired(1000000ULL, expired_buf, ACK_TRACKER_CAPACITY);
    // ACKED slots are freed silently; count should be 0 (not expired, just acked)
    assert(expired_count == 0U);

    // Now we can track one more message into the freed slot
    MessageEnvelope new_env;
    make_test_envelope(new_env, 9999ULL);
    Result new_res = tracker.track(new_env, 0xFFFFFFFFFFFFFFFFULL - 1ULL);

    assert(new_res == Result::OK);
    assert(expired_count == 0U);

    printf("PASS: test_sweep_releases_acked\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: add 4 expired entries; sweep with buf of size 2; count==2 (capped)
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_buf_capacity()
{
    AckTracker tracker;
    tracker.init();

    // Track 4 expired entries (deadline in the past)
    for (uint32_t i = 0U; i < 4U; ++i) {
        MessageEnvelope env;
        make_test_envelope(env, static_cast<uint64_t>(i) + 100U);
        Result r = tracker.track(env, 1ULL);  // deadline=1us (expired)
        assert(r == Result::OK);
    }

    // Sweep with buffer capacity of only 2
    MessageEnvelope small_buf[2];
    uint32_t count = tracker.sweep_expired(1000000ULL, small_buf, 2U);

    // Should return at most 2 (buf_cap), even though 4 expired
    assert(count == 2U);
    assert(count <= 2U);

    printf("PASS: test_sweep_buf_capacity\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: stats timeout counter increments when PENDING slot expires
// Verifies: REQ-7.2.3 — AckTracker.timeouts counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_timeout()
{
    AckTracker tracker;
    tracker.init();

    // Verify stats are zeroed on init
    const AckTrackerStats& s0 = tracker.get_stats();
    assert(s0.timeouts == 0U);
    assert(s0.acks_received == 0U);

    // Track one message with an expired deadline
    MessageEnvelope env;
    make_test_envelope(env, 500U);
    Result r = tracker.track(env, 1ULL);  // deadline = 1µs (already expired)
    assert(r == Result::OK);

    // Sweep at a time well past the deadline
    MessageEnvelope buf[ACK_TRACKER_CAPACITY];
    uint32_t count = tracker.sweep_expired(1000000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count == 1U);

    // timeout counter must be 1; acks_received still 0
    const AckTrackerStats& s1 = tracker.get_stats();
    assert(s1.timeouts == 1U);
    assert(s1.acks_received == 0U);

    printf("PASS: test_stats_timeout\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: stats acks_received increments when on_ack() transitions PENDING→ACKED
// Verifies: REQ-7.2.3 — AckTracker.acks_received counter
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_ack_received()
{
    AckTracker tracker;
    tracker.init();

    const AckTrackerStats& s0 = tracker.get_stats();
    assert(s0.acks_received == 0U);
    assert(s0.timeouts == 0U);

    // Track a message with a future deadline
    MessageEnvelope env;
    make_test_envelope(env, 600U);
    env.source_id = 1U;
    Result track_r = tracker.track(env, 9999999999ULL);
    assert(track_r == Result::OK);

    // ACK the message
    Result ack_r = tracker.on_ack(1U, 600U);
    assert(ack_r == Result::OK);

    // acks_received must be 1; timeouts still 0
    const AckTrackerStats& s1 = tracker.get_stats();
    assert(s1.acks_received == 1U);
    assert(s1.timeouts == 0U);

    printf("PASS: test_stats_ack_received\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: multi-round timeout accumulation does NOT fire the former time-bomb
// Regression test for the broken assertion:
//   NEVER_COMPILED_OUT_ASSERT(timeouts <= acks_received + ACK_TRACKER_CAPACITY)
// which was violated after exactly 2 × ACK_TRACKER_CAPACITY timeouts with zero ACKs.
// After 3 full rounds (3 × 32 = 96 timeouts, 0 ACKs) the old assertion would fire at
// round 2.  This test verifies get_stats() is safe across all three rounds.
//
// Verifies: REQ-7.2.3 — AckTracker.timeouts counter accumulates correctly across rounds
// ─────────────────────────────────────────────────────────────────────────────
static void test_stats_timeout_multiround()
{
    AckTracker tracker;
    tracker.init();

    // Power of 10: bounded outer loop — 3 rounds
    static const uint32_t NUM_ROUNDS = 3U;
    for (uint32_t round = 0U; round < NUM_ROUNDS; ++round) {
        // Fill all slots with already-expired deadlines (deadline_us = 1)
        // Power of 10: bounded inner loop — ACK_TRACKER_CAPACITY iterations
        for (uint32_t i = 0U; i < ACK_TRACKER_CAPACITY; ++i) {
            MessageEnvelope env;
            // Use a unique msg_id per slot per round to avoid aliasing
            uint64_t msg_id = static_cast<uint64_t>(round) * ACK_TRACKER_CAPACITY
                              + static_cast<uint64_t>(i) + 1U;
            make_test_envelope(env, msg_id);
            Result r = tracker.track(env, 1ULL);  // deadline=1µs — already expired
            assert(r == Result::OK);
        }

        // Sweep at a time well past all deadlines — expires all ACK_TRACKER_CAPACITY slots
        MessageEnvelope expired_buf[ACK_TRACKER_CAPACITY];
        uint32_t expired = tracker.sweep_expired(1000000ULL, expired_buf, ACK_TRACKER_CAPACITY);
        assert(expired == ACK_TRACKER_CAPACITY);

        // Call get_stats() — must NOT abort (regression: old assertion fired here on round 2)
        const AckTrackerStats& s = tracker.get_stats();

        // Verify cumulative counters
        uint32_t expected_timeouts = (round + 1U) * ACK_TRACKER_CAPACITY;
        assert(s.timeouts == expected_timeouts);
        assert(s.acks_received == 0U);
    }

    // Final state: 3 × ACK_TRACKER_CAPACITY (= 96) timeouts, zero ACKs
    const AckTrackerStats& final_s = tracker.get_stats();
    assert(final_s.timeouts == NUM_ROUNDS * ACK_TRACKER_CAPACITY);
    assert(final_s.acks_received == 0U);

    printf("PASS: test_stats_timeout_multiround\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: cancel() frees PENDING slot without phantom ACK stat; slot reusable
// Verifies: AckTracker::cancel() rollback path (P1b fix)
// Verifies: REQ-3.2.4
// ─────────────────────────────────────────────────────────────────────────────
static void test_ack_tracker_cancel()
{
    AckTracker tracker;
    tracker.init();

    // Capture baseline stats — all zeroed after init
    const AckTrackerStats& s0 = tracker.get_stats();
    assert(s0.acks_received == 0U);
    assert(s0.timeouts == 0U);

    // Track a message (PENDING)
    MessageEnvelope env;
    make_test_envelope(env, 77ULL, 1U, 2U);
    Result track_res = tracker.track(env, 9000000ULL);
    assert(track_res == Result::OK);

    // cancel() must free the slot directly (PENDING→FREE) without acks_received bump
    Result cancel_res = tracker.cancel(1U, 77ULL);
    assert(cancel_res == Result::OK);

    // acks_received must still be 0 (no phantom ACK stat)
    const AckTrackerStats& s1 = tracker.get_stats();
    assert(s1.acks_received == 0U);
    assert(s1.timeouts == 0U);

    // Slot must be FREE: track() with the same message_id must succeed (reusable)
    MessageEnvelope env2;
    make_test_envelope(env2, 77ULL, 1U, 2U);
    Result retrack_res = tracker.track(env2, 9000000ULL);
    assert(retrack_res == Result::OK);

    printf("PASS: test_ack_tracker_cancel\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: sweep_expired accepts equal now_us (monotonic: >= not only >)
// Verifies: REQ-3.2.4
// Equal timestamps must be accepted (not trigger assertion failure).
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_expired_accepts_equal_now_us()
{
    // Verifies: REQ-3.2.4
    AckTracker tracker;
    tracker.init();

    const uint64_t T = 1000000ULL;

    // Track one RELIABLE_ACK message with far-future deadline
    MessageEnvelope env;
    make_test_envelope(env, 901ULL);
    Result track_r = tracker.track(env, T + 500000ULL);
    assert(track_r == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // First sweep at T: message not yet expired (deadline > T)
    uint32_t count1 = tracker.sweep_expired(T, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 0U);  // Assert: not expired yet

    // Second sweep at same T: equal timestamp must be accepted (>= passes)
    uint32_t count2 = tracker.sweep_expired(T, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 0U);  // Assert: still not expired; no assertion failure

    printf("PASS: test_sweep_expired_accepts_equal_now_us\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: sweep_expired monotonic sequence — correct expiry at the right time
// Verifies: REQ-3.2.4
// ─────────────────────────────────────────────────────────────────────────────
static void test_sweep_expired_monotonic_sequence()
{
    // Verifies: REQ-3.2.4
    AckTracker tracker;
    tracker.init();

    // Track a message with deadline = 200000 us
    MessageEnvelope env;
    make_test_envelope(env, 902ULL);
    Result track_r = tracker.track(env, 200000ULL);
    assert(track_r == Result::OK);

    MessageEnvelope buf[ACK_TRACKER_CAPACITY];

    // Call at 100000: not expired (deadline=200000 > 100000)
    uint32_t count1 = tracker.sweep_expired(100000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count1 == 0U);  // Assert: not expired before deadline

    // Call at 200000: exactly at deadline; sweep_one_slot uses >= so this expires
    uint32_t count2 = tracker.sweep_expired(200000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count2 == 1U);  // Assert: expired exactly at deadline

    // Call at 300000: slot already freed; no more entries
    uint32_t count3 = tracker.sweep_expired(300000ULL, buf, ACK_TRACKER_CAPACITY);
    assert(count3 == 0U);  // Assert: slot freed after expiry; no second expiry

    printf("PASS: test_sweep_expired_monotonic_sequence\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: get_send_timestamp — matching PENDING entry returns OK and timestamp
// Verifies: REQ-7.2.1 — AckTracker.get_send_timestamp latency helper
// ─────────────────────────────────────────────────────────────────────────────
static void test_get_send_timestamp_found()
{
    // Verifies: REQ-7.2.1
    AckTracker tracker;
    tracker.init();

    // Track a message; timestamp_us is set to 1000000ULL by make_test_envelope.
    MessageEnvelope env;
    make_test_envelope(env, 42ULL);
    const uint64_t expected_ts = env.timestamp_us;  // 1000000ULL

    Result track_r = tracker.track(env, 9000000ULL);
    assert(track_r == Result::OK);  // Assert: tracking succeeded

    uint64_t out_ts = 0ULL;
    Result res = tracker.get_send_timestamp(1U, 42ULL, out_ts);
    assert(res == Result::OK);             // Assert: found matching PENDING entry
    assert(out_ts == expected_ts);         // Assert: returned timestamp matches send time

    printf("PASS: test_get_send_timestamp_found\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 18: get_send_timestamp — no matching entry returns ERR_INVALID
// Verifies: REQ-7.2.1 — AckTracker.get_send_timestamp not-found path
// ─────────────────────────────────────────────────────────────────────────────
static void test_get_send_timestamp_not_found()
{
    // Verifies: REQ-7.2.1
    AckTracker tracker;
    tracker.init();

    // Nothing tracked; any query must return ERR_INVALID.
    uint64_t out_ts = 0xDEADBEEFULL;
    Result res = tracker.get_send_timestamp(1U, 42ULL, out_ts);
    assert(res == Result::ERR_INVALID);   // Assert: ERR_INVALID when entry absent
    assert(out_ts == 0xDEADBEEFULL);      // Assert: out_ts untouched on failure

    printf("PASS: test_get_send_timestamp_not_found\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 19: get_send_timestamp — two tracked messages; each returns its own ts
// Verifies: REQ-7.2.1 — AckTracker.get_send_timestamp with multiple entries
// ─────────────────────────────────────────────────────────────────────────────
static void test_get_send_timestamp_multiple_entries()
{
    // Verifies: REQ-7.2.1
    AckTracker tracker;
    tracker.init();

    // Build two envelopes with distinct message IDs and timestamps.
    MessageEnvelope env1;
    make_test_envelope(env1, 10ULL);
    env1.timestamp_us = 111000ULL;

    MessageEnvelope env2;
    make_test_envelope(env2, 20ULL);
    env2.timestamp_us = 222000ULL;

    Result r1 = tracker.track(env1, 9000000ULL);
    Result r2 = tracker.track(env2, 9000000ULL);
    assert(r1 == Result::OK);  // Assert: first track succeeded
    assert(r2 == Result::OK);  // Assert: second track succeeded

    uint64_t ts1 = 0ULL;
    Result res1 = tracker.get_send_timestamp(1U, 10ULL, ts1);
    assert(res1 == Result::OK);       // Assert: found env1
    assert(ts1 == 111000ULL);         // Assert: correct timestamp for msg 10

    uint64_t ts2 = 0ULL;
    Result res2 = tracker.get_send_timestamp(1U, 20ULL, ts2);
    assert(res2 == Result::OK);       // Assert: found env2
    assert(ts2 == 222000ULL);         // Assert: correct timestamp for msg 20

    printf("PASS: test_get_send_timestamp_multiple_entries\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    test_init();
    test_track_ok();
    test_track_full();
    test_on_ack_ok();
    test_on_ack_wrong_src();
    test_on_ack_wrong_id();
    test_sweep_expired_pending();
    test_sweep_pending_not_expired();
    test_sweep_releases_acked();
    test_sweep_buf_capacity();
    test_stats_timeout();
    test_stats_ack_received();
    test_stats_timeout_multiround();
    test_ack_tracker_cancel();
    test_sweep_expired_accepts_equal_now_us();
    test_sweep_expired_monotonic_sequence();
    test_get_send_timestamp_found();
    test_get_send_timestamp_not_found();
    test_get_send_timestamp_multiple_entries();

    printf("ALL AckTracker tests passed.\n");
    return 0;
}
