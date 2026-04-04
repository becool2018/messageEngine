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
 * @file test_Serializer.cpp
 * @brief Unit tests for Serializer round-trip and edge cases.
 *
 * Tests serialization and deserialization of MessageEnvelope to/from
 * binary wire format with various payload sizes and error conditions.
 *
 * Rules applied:
 *   - Power of 10: fixed buffer sizes, bounded loops, ≥2 assertions per test.
 *   - MISRA C++: no STL, no exceptions, ≤1 pointer indirection.
 *   - F-Prime style: simple test framework using assert() and printf().
 */

// Verifies: REQ-3.2.3, REQ-3.2.8
// Verification: M1 + M2 + M4 + M5 (fault injection not required — pure logic)

#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdint>

#include "core/Types.hpp"
#include "core/MessageEnvelope.hpp"
#include "core/Serializer.hpp"
#include "core/ProtocolVersion.hpp"

// Fixed-size wire buffer for all serialization tests
// (Power of 10 rule 3: no dynamic allocation)
static uint8_t wire_buf[8192U];

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: Basic round-trip serialization and deserialization
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_serialize_deserialize_basic()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type = MessageType::DATA;
    original.message_id = 0x0102030405060708ULL;
    original.timestamp_us = 0x0A0B0C0D0E0F1011ULL;
    original.source_id = 42U;
    original.destination_id = 99U;
    original.priority = 1U;
    original.reliability_class = ReliabilityClass::RELIABLE_ACK;
    original.expiry_time_us = 500000ULL;
    original.payload_length = 5U;
    original.payload[0] = 'H';
    original.payload[1] = 'e';
    original.payload[2] = 'l';
    original.payload[3] = 'l';
    original.payload[4] = 'o';

    // Serialize
    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len == Serializer::WIRE_HEADER_SIZE + 5U);

    // Deserialize
    MessageEnvelope deserialized;
    envelope_init(deserialized);
    Result dr = Serializer::deserialize(wire_buf, out_len, deserialized);
    assert(dr == Result::OK);

    // Verify all fields match
    assert(deserialized.message_type == original.message_type);
    assert(deserialized.message_id == original.message_id);
    assert(deserialized.timestamp_us == original.timestamp_us);
    assert(deserialized.source_id == original.source_id);
    assert(deserialized.destination_id == original.destination_id);
    assert(deserialized.priority == original.priority);
    assert(deserialized.reliability_class == original.reliability_class);
    assert(deserialized.expiry_time_us == original.expiry_time_us);
    assert(deserialized.payload_length == original.payload_length);
    assert(deserialized.payload[0] == 'H');
    assert(deserialized.payload[4] == 'o');

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Serialize with buffer too small
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_serialize_buffer_too_small()
{
    MessageEnvelope env;
    envelope_init(env);
    env.message_type = MessageType::DATA;
    env.source_id = 1U;
    env.payload_length = 100U;

    uint8_t small_buf[10U];
    uint32_t out_len = 0U;

    // Buffer size (10) is less than WIRE_HEADER_SIZE (44)
    Result r = Serializer::serialize(env, small_buf, sizeof(small_buf), out_len);
    assert(r == Result::ERR_INVALID);
    assert(out_len == 0U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Serialize invalid envelope
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_serialize_invalid_envelope()
{
    // Envelope with INVALID message type
    MessageEnvelope invalid_type;
    envelope_init(invalid_type);
    invalid_type.message_type = MessageType::INVALID;
    invalid_type.source_id = 1U;

    uint32_t out_len = 0U;
    Result r1 = Serializer::serialize(invalid_type, wire_buf, sizeof(wire_buf), out_len);
    assert(r1 == Result::ERR_INVALID);

    // Envelope with invalid (zero) source_id
    MessageEnvelope invalid_src;
    envelope_init(invalid_src);
    invalid_src.message_type = MessageType::DATA;
    invalid_src.source_id = NODE_ID_INVALID;  // zero is invalid

    out_len = 0U;
    Result r2 = Serializer::serialize(invalid_src, wire_buf, sizeof(wire_buf), out_len);
    assert(r2 == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Deserialize with truncated buffer
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_deserialize_truncated()
{
    // First, create a valid serialized message
    MessageEnvelope original;
    envelope_init(original);
    original.message_type = MessageType::DATA;
    original.source_id = 5U;
    original.payload_length = 20U;
    for (uint32_t i = 0U; i < 20U; ++i) {
        original.payload[i] = static_cast<uint8_t>(i & 0xFFU);
    }

    uint32_t full_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), full_len);
    assert(sr == Result::OK);
    assert(full_len > Serializer::WIRE_HEADER_SIZE);

    // Try to deserialize with buffer size one byte short
    MessageEnvelope truncated;
    envelope_init(truncated);
    Result dr = Serializer::deserialize(wire_buf, full_len - 1U, truncated);
    assert(dr == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Serialize and deserialize maximum payload size
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_serialize_max_payload()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type = MessageType::DATA;
    original.source_id = 100U;
    original.payload_length = MSG_MAX_PAYLOAD_BYTES;

    // Fill payload with 0xAB pattern
    for (uint32_t i = 0U; i < MSG_MAX_PAYLOAD_BYTES; ++i) {
        original.payload[i] = 0xABU;
    }

    // Use large buffer for serialization
    uint8_t large_buf[4096U + 100U];
    uint32_t out_len = 0U;

    Result sr = Serializer::serialize(original, large_buf, sizeof(large_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len == Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES);

    // Deserialize
    MessageEnvelope deserialized;
    envelope_init(deserialized);
    Result dr = Serializer::deserialize(large_buf, out_len, deserialized);
    assert(dr == Result::OK);

    // Verify payload matches
    assert(deserialized.payload_length == MSG_MAX_PAYLOAD_BYTES);
    for (uint32_t i = 0U; i < MSG_MAX_PAYLOAD_BYTES; ++i) {
        assert(deserialized.payload[i] == 0xABU);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: Verify WIRE_HEADER_SIZE constant (protocol v2)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.2.8
static bool test_wire_header_size()
{
    // v2 layout (from Serializer.hpp):
    // 1 byte message_type + 1 reliability_class + 1 priority + 1 proto_version
    // + 8 message_id + 8 timestamp_us + 4 source_id + 4 destination_id
    // + 8 expiry_time_us + 4 payload_length + 2 magic + 2 reserved
    // + 4 sequence_num + 1 fragment_index + 1 fragment_count + 2 total_payload_length
    // = 1+1+1+1+8+8+4+4+8+4+2+2+4+1+1+2 = 52
    assert(Serializer::WIRE_HEADER_SIZE == 52U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7: Serialize zero-payload envelope (covers False branch of
//         'if (env.payload_length > 0U)' in serialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_serialize_zero_payload()
{
    MessageEnvelope env;
    envelope_init(env);
    env.message_type   = MessageType::DATA;
    env.source_id      = 7U;
    env.payload_length = 0U;  // zero payload — skips memcpy branch in serialize

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(env, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len == Serializer::WIRE_HEADER_SIZE);

    // Round-trip: deserialize back and verify zero payload
    MessageEnvelope deser;
    envelope_init(deser);
    Result dr = Serializer::deserialize(wire_buf, out_len, deser);
    assert(dr == Result::OK);
    assert(deser.payload_length == 0U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8: Deserialize with buffer shorter than WIRE_HEADER_SIZE (covers True
//         branch of 'if (buf_len < WIRE_HEADER_SIZE)' in deserialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_deserialize_header_too_short()
{
    // Any buffer smaller than 44 bytes must be rejected before reading header
    uint8_t tiny[10U];
    (void)memset(tiny, 0, sizeof(tiny));

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(tiny, static_cast<uint32_t>(sizeof(tiny)), env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 9: Deserialize with unrecognised protocol version (covers True branch
//         of 'if (proto_ver != PROTO_VERSION)' in deserialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.2.8
static bool test_deserialize_version_mismatch()
{
    // Serialize a valid message to get a well-formed wire buffer
    MessageEnvelope original;
    envelope_init(original);
    original.message_type  = MessageType::DATA;
    original.source_id     = 3U;
    original.payload_length = 4U;
    original.payload[0] = 'T';
    original.payload[1] = 'e';
    original.payload[2] = 's';
    original.payload[3] = 't';

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len >= Serializer::WIRE_HEADER_SIZE);

    // Overwrite version byte at wire offset 3 with an unknown version (0xFF)
    wire_buf[3U] = 0xFFU;

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 10: Deserialize with corrupted frame magic (covers True branch of
//          'if (magic_word != ...)' in deserialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.2.8
static bool test_deserialize_bad_magic()
{
    // Serialize a valid message to get a well-formed wire buffer
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 4U;
    original.payload_length = 4U;
    original.payload[0] = 'D';
    original.payload[1] = 'a';
    original.payload[2] = 't';
    original.payload[3] = 'a';

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len >= Serializer::WIRE_HEADER_SIZE);

    // Corrupt the magic high-byte at wire offset 40 (0x4D → 0x01)
    wire_buf[40U] = 0x01U;

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 11: Deserialize with payload_length field > MSG_MAX_PAYLOAD_BYTES
//          (covers True branch of 'if (env.payload_length > MSG_MAX_PAYLOAD_BYTES)'
//           in deserialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_deserialize_oversized_payload_field()
{
    // Serialize a valid message to get a clean header
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 5U;
    original.payload_length = 4U;
    original.payload[0] = 0x01U;
    original.payload[1] = 0x02U;
    original.payload[2] = 0x03U;
    original.payload[3] = 0x04U;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);

    // Overwrite payload_length field (big-endian, offset 36) with oversized value
    uint32_t oversized = MSG_MAX_PAYLOAD_BYTES + 1U;
    wire_buf[36U] = static_cast<uint8_t>((oversized >> 24U) & 0xFFU);
    wire_buf[37U] = static_cast<uint8_t>((oversized >> 16U) & 0xFFU);
    wire_buf[38U] = static_cast<uint8_t>((oversized >> 8U)  & 0xFFU);
    wire_buf[39U] = static_cast<uint8_t>((oversized >> 0U)  & 0xFFU);

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 12: Deserialize zero-payload message (covers False branch of
//          'if (env.payload_length > 0U)' in deserialize)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_deserialize_zero_payload()
{
    // Serialize a zero-payload envelope
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::ACK;
    original.source_id      = 8U;
    original.destination_id = 1U;
    original.payload_length = 0U;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len == Serializer::WIRE_HEADER_SIZE);

    // Deserialize: payload_length == 0 → skips memcpy branch in deserialize
    MessageEnvelope deser;
    envelope_init(deser);
    Result dr = Serializer::deserialize(wire_buf, out_len, deser);
    assert(dr == Result::OK);
    assert(deser.payload_length == 0U);
    assert(deser.source_id == 8U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 13: Deserialize rejects version 0 (old pre-versioning format)
//          and version 1 (old v1 format now that we are at v2)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.8
static bool test_deserialize_version_zero_rejected()
{
    // Serialize a valid message (version byte is set to PROTO_VERSION = 2)
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 10U;
    original.payload_length = 0U;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len >= Serializer::WIRE_HEADER_SIZE);

    // Overwrite version byte at offset 3 with 0 (old unversioned format)
    // — must be rejected by a v2 deserializer
    wire_buf[3U] = 0U;

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    // Also verify that v1 frames are rejected by a v2 deserializer
    wire_buf[3U] = 1U;
    MessageEnvelope env2;
    envelope_init(env2);
    Result r2 = Serializer::deserialize(wire_buf, out_len, env2);
    assert(r2 == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 14: Verify PROTO_VERSION and PROTO_MAGIC appear correctly in wire frame
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.8
static bool test_proto_version_in_wire_frame()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 11U;
    original.payload_length = 0U;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len >= Serializer::WIRE_HEADER_SIZE);

    // Wire byte 3 must equal PROTO_VERSION
    assert(wire_buf[3U] == PROTO_VERSION);

    // Wire bytes 40–41 must equal PROTO_MAGIC (big-endian: high byte, low byte)
    assert(wire_buf[40U] == static_cast<uint8_t>((PROTO_MAGIC >> 8U) & 0xFFU));
    assert(wire_buf[41U] == static_cast<uint8_t>(PROTO_MAGIC & 0xFFU));

    // Wire bytes 42–43 must be reserved zero
    assert(wire_buf[42U] == 0U);
    assert(wire_buf[43U] == 0U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 15: Serialize and deserialize v2 fragment fields (round-trip)
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3, REQ-3.2.8
static bool test_v2_fragment_fields_round_trip()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type         = MessageType::DATA;
    original.source_id            = 20U;
    original.payload_length       = 10U;
    original.sequence_num         = 42U;
    original.fragment_index       = 1U;
    original.fragment_count       = 3U;
    original.total_payload_length = 3000U;
    for (uint32_t i = 0U; i < 10U; ++i) {
        original.payload[i] = static_cast<uint8_t>(i);
    }

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);
    assert(out_len == Serializer::WIRE_HEADER_SIZE + 10U);

    MessageEnvelope deser;
    envelope_init(deser);
    Result dr = Serializer::deserialize(wire_buf, out_len, deser);
    assert(dr == Result::OK);
    assert(deser.sequence_num         == 42U);
    assert(deser.fragment_index       == 1U);
    assert(deser.fragment_count       == 3U);
    assert(deser.total_payload_length == 3000U);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 16: Deserialize rejects fragment_index >= fragment_count
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_v2_fragment_index_out_of_range_rejected()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 21U;
    original.payload_length = 4U;
    original.fragment_index = 0U;
    original.fragment_count = 2U;
    original.total_payload_length = 4U;
    original.payload[0] = 0xAAU;
    original.payload[1] = 0xBBU;
    original.payload[2] = 0xCCU;
    original.payload[3] = 0xDDU;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);

    // Overwrite fragment_index (offset 48) to equal fragment_count (2): must be rejected
    wire_buf[48U] = 2U;

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 17: Deserialize rejects fragment_count > FRAG_MAX_COUNT
// ─────────────────────────────────────────────────────────────────────────────
// Verifies: REQ-3.2.3
static bool test_v2_fragment_count_too_large_rejected()
{
    MessageEnvelope original;
    envelope_init(original);
    original.message_type   = MessageType::DATA;
    original.source_id      = 22U;
    original.payload_length = 4U;
    original.fragment_index = 0U;
    original.fragment_count = 1U;
    original.total_payload_length = 4U;
    original.payload[0] = 0x01U;
    original.payload[1] = 0x02U;
    original.payload[2] = 0x03U;
    original.payload[3] = 0x04U;

    uint32_t out_len = 0U;
    Result sr = Serializer::serialize(original, wire_buf, sizeof(wire_buf), out_len);
    assert(sr == Result::OK);

    // Overwrite fragment_count (offset 49) to exceed FRAG_MAX_COUNT (4): use 5
    wire_buf[49U] = 5U;

    MessageEnvelope env;
    envelope_init(env);
    Result r = Serializer::deserialize(wire_buf, out_len, env);
    assert(r == Result::ERR_INVALID);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main test runner
// ─────────────────────────────────────────────────────────────────────────────
int main()
{
    int failed = 0;

    if (!test_serialize_deserialize_basic()) {
        printf("FAIL: test_serialize_deserialize_basic\n");
        ++failed;
    } else {
        printf("PASS: test_serialize_deserialize_basic\n");
    }

    if (!test_serialize_buffer_too_small()) {
        printf("FAIL: test_serialize_buffer_too_small\n");
        ++failed;
    } else {
        printf("PASS: test_serialize_buffer_too_small\n");
    }

    if (!test_serialize_invalid_envelope()) {
        printf("FAIL: test_serialize_invalid_envelope\n");
        ++failed;
    } else {
        printf("PASS: test_serialize_invalid_envelope\n");
    }

    if (!test_deserialize_truncated()) {
        printf("FAIL: test_deserialize_truncated\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_truncated\n");
    }

    if (!test_serialize_max_payload()) {
        printf("FAIL: test_serialize_max_payload\n");
        ++failed;
    } else {
        printf("PASS: test_serialize_max_payload\n");
    }

    if (!test_wire_header_size()) {
        printf("FAIL: test_wire_header_size\n");
        ++failed;
    } else {
        printf("PASS: test_wire_header_size\n");
    }

    if (!test_serialize_zero_payload()) {
        printf("FAIL: test_serialize_zero_payload\n");
        ++failed;
    } else {
        printf("PASS: test_serialize_zero_payload\n");
    }

    if (!test_deserialize_header_too_short()) {
        printf("FAIL: test_deserialize_header_too_short\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_header_too_short\n");
    }

    if (!test_deserialize_version_mismatch()) {
        printf("FAIL: test_deserialize_version_mismatch\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_version_mismatch\n");
    }

    if (!test_deserialize_bad_magic()) {
        printf("FAIL: test_deserialize_bad_magic\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_bad_magic\n");
    }

    if (!test_deserialize_oversized_payload_field()) {
        printf("FAIL: test_deserialize_oversized_payload_field\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_oversized_payload_field\n");
    }

    if (!test_deserialize_zero_payload()) {
        printf("FAIL: test_deserialize_zero_payload\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_zero_payload\n");
    }

    if (!test_deserialize_version_zero_rejected()) {
        printf("FAIL: test_deserialize_version_zero_rejected\n");
        ++failed;
    } else {
        printf("PASS: test_deserialize_version_zero_rejected\n");
    }

    if (!test_proto_version_in_wire_frame()) {
        printf("FAIL: test_proto_version_in_wire_frame\n");
        ++failed;
    } else {
        printf("PASS: test_proto_version_in_wire_frame\n");
    }

    if (!test_v2_fragment_fields_round_trip()) {
        printf("FAIL: test_v2_fragment_fields_round_trip\n");
        ++failed;
    } else {
        printf("PASS: test_v2_fragment_fields_round_trip\n");
    }

    if (!test_v2_fragment_index_out_of_range_rejected()) {
        printf("FAIL: test_v2_fragment_index_out_of_range_rejected\n");
        ++failed;
    } else {
        printf("PASS: test_v2_fragment_index_out_of_range_rejected\n");
    }

    if (!test_v2_fragment_count_too_large_rejected()) {
        printf("FAIL: test_v2_fragment_count_too_large_rejected\n");
        ++failed;
    } else {
        printf("PASS: test_v2_fragment_count_too_large_rejected\n");
    }

    return (failed > 0) ? 1 : 0;
}
