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

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"

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
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
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

    return (failed > 0) ? 1 : 0;
}
