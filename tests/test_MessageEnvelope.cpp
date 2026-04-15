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
 * @file test_MessageEnvelope.cpp
 * @brief Unit tests for MessageEnvelope struct and helper functions.
 *
 * Tests the initialization, validation, copying, and ACK generation
 * of the MessageEnvelope data structure.
 *
 * Rules applied:
 *   - Power of 10: fixed bounds, simple control flow, no dynamic allocation.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework with assert() and printf().
 */

// Verifies: REQ-3.1, REQ-3.2.4
// Verification: M1 + M2 + M4 + M5 (fault injection not required — pure data structure)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Logger.hpp"
#include "platform/PosixLogClock.hpp"
#include "platform/PosixLogSink.hpp"


// ─────────────────────────────────────────────────────────────────────────────
// Test 1: envelope_init() initializes all fields correctly
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1
static bool test_envelope_init()
{
    MessageEnvelope env;
    envelope_init(env);

    // Verify message_type is INVALID after init
    assert(env.message_type == MessageType::INVALID);

    // Verify payload_length is zero
    assert(env.payload_length == 0U);

    // Verify source_id is zero (invalid)
    assert(env.source_id == NODE_ID_INVALID);

    // Verify timestamp is zero
    assert(env.timestamp_us == 0ULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: envelope_valid() correctly validates envelope fields
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1
static bool test_envelope_valid()
{
    MessageEnvelope valid_env;
    envelope_init(valid_env);
    valid_env.message_type = MessageType::DATA;
    valid_env.source_id = 1U;
    valid_env.destination_id = 2U;
    valid_env.payload_length = 100U;

    // Valid envelope should return true
    assert(envelope_valid(valid_env) == true);

    // Invalid envelope: source_id == 0
    MessageEnvelope invalid_env;
    envelope_init(invalid_env);
    invalid_env.message_type = MessageType::DATA;
    invalid_env.source_id = NODE_ID_INVALID;  // invalid
    invalid_env.payload_length = 100U;
    assert(envelope_valid(invalid_env) == false);

    // Invalid envelope: message_type == INVALID
    MessageEnvelope invalid_type;
    envelope_init(invalid_type);
    invalid_type.message_type = MessageType::INVALID;
    invalid_type.source_id = 1U;
    invalid_type.payload_length = 100U;
    assert(envelope_valid(invalid_type) == false);

    // Invalid envelope: payload_length exceeds maximum
    MessageEnvelope overflow_env;
    envelope_init(overflow_env);
    overflow_env.message_type = MessageType::DATA;
    overflow_env.source_id = 1U;
    overflow_env.payload_length = MSG_MAX_PAYLOAD_BYTES + 1U;
    assert(envelope_valid(overflow_env) == false);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: envelope_copy() correctly copies all fields
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1
static bool test_envelope_copy()
{
    MessageEnvelope src;
    envelope_init(src);
    src.message_type = MessageType::DATA;
    src.message_id = 12345ULL;
    src.timestamp_us = 99999ULL;
    src.source_id = 10U;
    src.destination_id = 20U;
    src.priority = 3U;
    src.reliability_class = ReliabilityClass::RELIABLE_ACK;
    src.expiry_time_us = 500000ULL;
    src.payload_length = 5U;
    src.payload[0] = 'H';
    src.payload[1] = 'e';
    src.payload[2] = 'l';
    src.payload[3] = 'l';
    src.payload[4] = 'o';

    MessageEnvelope dst;
    envelope_copy(dst, src);

    // Verify all fields match
    assert(dst.message_type == src.message_type);
    assert(dst.message_id == src.message_id);
    assert(dst.timestamp_us == src.timestamp_us);
    assert(dst.source_id == src.source_id);
    assert(dst.destination_id == src.destination_id);
    assert(dst.priority == src.priority);
    assert(dst.reliability_class == src.reliability_class);
    assert(dst.expiry_time_us == src.expiry_time_us);
    assert(dst.payload_length == src.payload_length);
    assert(dst.payload[0] == 'H');
    assert(dst.payload[4] == 'o');

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: envelope_make_ack() creates proper ACK envelope
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.4
static bool test_envelope_make_ack()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type = MessageType::DATA;
    original.message_id = 54321ULL;
    original.source_id = 5U;
    original.destination_id = 10U;
    original.priority = 2U;
    original.reliability_class = ReliabilityClass::RELIABLE_RETRY;

    MessageEnvelope ack;
    uint64_t now_us = 111111ULL;
    NodeId my_id = 10U;

    envelope_make_ack(ack, original, my_id, now_us);

    // Verify ACK fields
    assert(ack.message_type == MessageType::ACK);
    assert(ack.message_id == original.message_id);  // ACK references original message_id
    assert(ack.source_id == my_id);                  // swapped: my_id
    assert(ack.destination_id == original.source_id);  // swapped: original source
    assert(ack.timestamp_us == now_us);
    assert(ack.priority == original.priority);  // inherited
    assert(ack.reliability_class == ReliabilityClass::BEST_EFFORT);
    assert(ack.payload_length == 0U);
    assert(ack.expiry_time_us == 0ULL);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: envelope_is_data() and envelope_is_control() type checks
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1
static bool test_is_data_control()
{
    MessageEnvelope data_env;
    envelope_init(data_env);
    data_env.message_type = MessageType::DATA;
    data_env.source_id = 1U;

    // DATA should be data, not control
    assert(envelope_is_data(data_env) == true);
    assert(envelope_is_control(data_env) == false);

    MessageEnvelope ack_env;
    envelope_init(ack_env);
    ack_env.message_type = MessageType::ACK;
    ack_env.source_id = 1U;

    // ACK should be control, not data
    assert(envelope_is_data(ack_env) == false);
    assert(envelope_is_control(ack_env) == true);

    MessageEnvelope nak_env;
    envelope_init(nak_env);
    nak_env.message_type = MessageType::NAK;
    nak_env.source_id = 1U;

    // NAK should be control
    assert(envelope_is_data(nak_env) == false);
    assert(envelope_is_control(nak_env) == true);

    MessageEnvelope hb_env;
    envelope_init(hb_env);
    hb_env.message_type = MessageType::HEARTBEAT;
    hb_env.source_id = 1U;

    // HEARTBEAT should be control
    assert(envelope_is_data(hb_env) == false);
    assert(envelope_is_control(hb_env) == true);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: envelope_is_fragment() and envelope_is_last_fragment()
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1, REQ-3.3.5
static bool test_fragment_helpers()
{
    // Unfragmented message (fragment_count == 1): must not be a fragment
    MessageEnvelope unfrag;
    envelope_init(unfrag);
    unfrag.message_type   = MessageType::DATA;
    unfrag.source_id      = 1U;
    unfrag.fragment_index = 0U;
    unfrag.fragment_count = 1U;

    assert(envelope_is_fragment(unfrag) == false);        // not a fragment
    assert(envelope_is_last_fragment(unfrag) == false);   // not last fragment either

    // First fragment of a 3-fragment message (index=0, count=3)
    MessageEnvelope first_frag;
    envelope_init(first_frag);
    first_frag.message_type   = MessageType::DATA;
    first_frag.source_id      = 1U;
    first_frag.fragment_index = 0U;
    first_frag.fragment_count = 3U;

    assert(envelope_is_fragment(first_frag) == true);        // is a fragment
    assert(envelope_is_last_fragment(first_frag) == false);  // not the last

    // Middle fragment (index=1, count=3)
    MessageEnvelope mid_frag;
    envelope_init(mid_frag);
    mid_frag.message_type   = MessageType::DATA;
    mid_frag.source_id      = 1U;
    mid_frag.fragment_index = 1U;
    mid_frag.fragment_count = 3U;

    assert(envelope_is_fragment(mid_frag) == true);        // is a fragment
    assert(envelope_is_last_fragment(mid_frag) == false);  // not the last

    // Last fragment (index=2, count=3)
    MessageEnvelope last_frag;
    envelope_init(last_frag);
    last_frag.message_type   = MessageType::DATA;
    last_frag.source_id      = 1U;
    last_frag.fragment_index = 2U;
    last_frag.fragment_count = 3U;

    assert(envelope_is_fragment(last_frag) == true);       // is a fragment
    assert(envelope_is_last_fragment(last_frag) == true);  // is the last fragment

    // Two-fragment message: first (index=0) is not last; second (index=1) is last
    MessageEnvelope two_first;
    envelope_init(two_first);
    two_first.message_type   = MessageType::DATA;
    two_first.source_id      = 2U;
    two_first.fragment_index = 0U;
    two_first.fragment_count = 2U;

    assert(envelope_is_fragment(two_first) == true);        // is a fragment
    assert(envelope_is_last_fragment(two_first) == false);  // not last

    MessageEnvelope two_last;
    envelope_init(two_last);
    two_last.message_type   = MessageType::DATA;
    two_last.source_id      = 2U;
    two_last.fragment_index = 1U;
    two_last.fragment_count = 2U;

    assert(envelope_is_fragment(two_last) == true);       // is a fragment
    assert(envelope_is_last_fragment(two_last) == true);  // is the last

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: envelope_addressed_to() — unicast and broadcast cases
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.1
static bool test_envelope_addressed_to()
{
    const NodeId THIS_NODE = 42U;
    const NodeId OTHER_NODE = 99U;

    // Unicast to this_node: addressed
    MessageEnvelope unicast_to_me;
    envelope_init(unicast_to_me);
    unicast_to_me.message_type    = MessageType::DATA;
    unicast_to_me.source_id       = OTHER_NODE;
    unicast_to_me.destination_id  = THIS_NODE;

    assert(envelope_addressed_to(unicast_to_me, THIS_NODE) == true);    // addressed to me
    assert(envelope_addressed_to(unicast_to_me, OTHER_NODE) == false);  // not addressed to other

    // Unicast to other_node: not addressed to this_node
    MessageEnvelope unicast_to_other;
    envelope_init(unicast_to_other);
    unicast_to_other.message_type   = MessageType::DATA;
    unicast_to_other.source_id      = THIS_NODE;
    unicast_to_other.destination_id = OTHER_NODE;

    assert(envelope_addressed_to(unicast_to_other, THIS_NODE) == false);  // not addressed to me
    assert(envelope_addressed_to(unicast_to_other, OTHER_NODE) == true);  // addressed to other

    // Broadcast (destination_id == NODE_ID_INVALID == 0): addressed to everyone
    MessageEnvelope broadcast;
    envelope_init(broadcast);
    broadcast.message_type   = MessageType::DATA;
    broadcast.source_id      = OTHER_NODE;
    broadcast.destination_id = NODE_ID_INVALID;  // 0 = broadcast sentinel

    assert(envelope_addressed_to(broadcast, THIS_NODE) == true);   // broadcast → addressed to me
    assert(envelope_addressed_to(broadcast, OTHER_NODE) == true);  // broadcast → addressed to other too

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: envelope_needs_ack_response()
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.4
static bool test_envelope_needs_ack_response()
{
    // BEST_EFFORT: no ACK required
    MessageEnvelope be;
    envelope_init(be);
    be.message_type      = MessageType::DATA;
    be.source_id         = 1U;
    be.reliability_class = ReliabilityClass::BEST_EFFORT;

    assert(envelope_needs_ack_response(be) == false);  // no ACK for best-effort

    // RELIABLE_ACK: ACK required
    MessageEnvelope ra;
    envelope_init(ra);
    ra.message_type      = MessageType::DATA;
    ra.source_id         = 1U;
    ra.reliability_class = ReliabilityClass::RELIABLE_ACK;

    assert(envelope_needs_ack_response(ra) == true);  // ACK required

    // RELIABLE_RETRY: ACK required
    MessageEnvelope rr;
    envelope_init(rr);
    rr.message_type      = MessageType::DATA;
    rr.source_id         = 1U;
    rr.reliability_class = ReliabilityClass::RELIABLE_RETRY;

    assert(envelope_needs_ack_response(rr) == true);  // ACK required

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    // Initialize logger before any production code that may call LOG_* macros.
    // Power of 10: return value checked; failure causes abort via NEVER_COMPILED_OUT_ASSERT.
    (void)Logger::init(Severity::INFO,
                       &PosixLogClock::instance(),
                       &PosixLogSink::instance());

    int failed = 0;

    if (!test_envelope_init()) {
        printf("FAIL: test_envelope_init\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_init\n");
    }

    if (!test_envelope_valid()) {
        printf("FAIL: test_envelope_valid\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_valid\n");
    }

    if (!test_envelope_copy()) {
        printf("FAIL: test_envelope_copy\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_copy\n");
    }

    if (!test_envelope_make_ack()) {
        printf("FAIL: test_envelope_make_ack\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_make_ack\n");
    }

    if (!test_is_data_control()) {
        printf("FAIL: test_is_data_control\n");
        ++failed;
    } else {
        printf("PASS: test_is_data_control\n");
    }

    if (!test_fragment_helpers()) {
        printf("FAIL: test_fragment_helpers\n");
        ++failed;
    } else {
        printf("PASS: test_fragment_helpers\n");
    }

    if (!test_envelope_addressed_to()) {
        printf("FAIL: test_envelope_addressed_to\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_addressed_to\n");
    }

    if (!test_envelope_needs_ack_response()) {
        printf("FAIL: test_envelope_needs_ack_response\n");
        ++failed;
    } else {
        printf("PASS: test_envelope_needs_ack_response\n");
    }

    return (failed > 0) ? 1 : 0;
}
