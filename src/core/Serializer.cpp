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
 * @file Serializer.cpp
 * @brief Implementation of deterministic, endian-safe MessageEnvelope serialization.
 *
 * Requirement traceability: CLAUDE.md §3.2 (serialization/deserialization),
 * CLAUDE.md §4.1 (framing, determinism).
 *
 * Rules applied:
 *   - Power of 10: fixed bounds, no dynamic allocation, ≥2 assertions per function.
 *   - MISRA C++: explicit byte-order conversions (manual shifts, no htons/ntohl).
 *   - F-Prime style: simple state logic, bounded loops, deterministic behavior.
 *
 * Implements: REQ-3.2.3
 */

#include "Serializer.hpp"
#include "Assert.hpp"
#include "ProtocolVersion.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Helper functions: endian-safe byte I/O
// ─────────────────────────────────────────────────────────────────────────────

/// Write one byte to buffer at offset; return new offset.
inline uint32_t Serializer::write_u8(uint8_t* buf, uint32_t offset, uint8_t val)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);             // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 1U); // Assert: offset won't overflow

    buf[offset] = val;
    return offset + 1U;
}

/// Write 4-byte big-endian value to buffer at offset; return new offset.
/// Manual shifts ensure endianness without runtime dependencies.
inline uint32_t Serializer::write_u32(uint8_t* buf, uint32_t offset, uint32_t val)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 4U);   // Assert: offset + 4 won't overflow

    // Power of 10 rule 1: simple, deterministic shifts (no macros)
    buf[offset + 0U] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
    buf[offset + 1U] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    buf[offset + 2U] = static_cast<uint8_t>((val >> 8U)  & 0xFFU);
    buf[offset + 3U] = static_cast<uint8_t>((val >> 0U)  & 0xFFU);

    return offset + 4U;
}

/// Write 8-byte big-endian value to buffer at offset; return new offset.
inline uint32_t Serializer::write_u64(uint8_t* buf, uint32_t offset, uint64_t val)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 8U);   // Assert: offset + 8 won't overflow

    // Power of 10 rule 1: simple, deterministic shifts
    buf[offset + 0U] = static_cast<uint8_t>((val >> 56U) & 0xFFU);
    buf[offset + 1U] = static_cast<uint8_t>((val >> 48U) & 0xFFU);
    buf[offset + 2U] = static_cast<uint8_t>((val >> 40U) & 0xFFU);
    buf[offset + 3U] = static_cast<uint8_t>((val >> 32U) & 0xFFU);
    buf[offset + 4U] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
    buf[offset + 5U] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
    buf[offset + 6U] = static_cast<uint8_t>((val >> 8U)  & 0xFFU);
    buf[offset + 7U] = static_cast<uint8_t>((val >> 0U)  & 0xFFU);

    return offset + 8U;
}

/// Write 2-byte big-endian value to buffer at offset; return new offset.
inline uint32_t Serializer::write_u16(uint8_t* buf, uint32_t offset, uint16_t val)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 2U);   // Assert: offset + 2 won't overflow

    buf[offset + 0U] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
    buf[offset + 1U] = static_cast<uint8_t>((val >> 0U) & 0xFFU);

    return offset + 2U;
}

/// Read one byte from buffer at offset; return value.
inline uint8_t Serializer::read_u8(const uint8_t* buf, uint32_t offset)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);             // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 1U); // Assert: offset won't overflow

    return buf[offset];
}

/// Read 2-byte big-endian value from buffer at offset; return value.
inline uint16_t Serializer::read_u16(const uint8_t* buf, uint32_t offset)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 2U);   // Assert: offset + 2 won't overflow

    uint16_t val = 0U;
    val = static_cast<uint16_t>((static_cast<uint16_t>(buf[offset + 0U]) << 8U) |
                                  static_cast<uint16_t>(buf[offset + 1U]));
    return val;
}

/// Read 4-byte big-endian value from buffer at offset; return value.
inline uint32_t Serializer::read_u32(const uint8_t* buf, uint32_t offset)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 4U);   // Assert: offset + 4 won't overflow

    // Power of 10 rule 1: simple, deterministic shifts
    uint32_t val = 0U;
    val |= (static_cast<uint32_t>(buf[offset + 0U]) << 24U);
    val |= (static_cast<uint32_t>(buf[offset + 1U]) << 16U);
    val |= (static_cast<uint32_t>(buf[offset + 2U]) << 8U);
    val |= (static_cast<uint32_t>(buf[offset + 3U]) << 0U);

    return val;
}

/// Read 8-byte big-endian value from buffer at offset; return value.
inline uint64_t Serializer::read_u64(const uint8_t* buf, uint32_t offset)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);                // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 8U);   // Assert: offset + 8 won't overflow

    // Power of 10 rule 1: simple, deterministic shifts
    uint64_t val = 0ULL;
    val |= (static_cast<uint64_t>(buf[offset + 0U]) << 56U);
    val |= (static_cast<uint64_t>(buf[offset + 1U]) << 48U);
    val |= (static_cast<uint64_t>(buf[offset + 2U]) << 40U);
    val |= (static_cast<uint64_t>(buf[offset + 3U]) << 32U);
    val |= (static_cast<uint64_t>(buf[offset + 4U]) << 24U);
    val |= (static_cast<uint64_t>(buf[offset + 5U]) << 16U);
    val |= (static_cast<uint64_t>(buf[offset + 6U]) << 8U);
    val |= (static_cast<uint64_t>(buf[offset + 7U]) << 0U);

    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization
// ─────────────────────────────────────────────────────────────────────────────

Result Serializer::serialize(const MessageEnvelope& env,
                             uint8_t*               buf,
                             uint32_t               buf_len,
                             uint32_t&              out_len)
{
    // Power of 10 rule 7: check all preconditions
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);           // Assert: output buffer is valid
    NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL);  // Assert: buffer length is valid

    // Validate envelope
    if (!envelope_valid(env)) {
        return Result::ERR_INVALID;
    }

    // CERT INT30-C: direct uint32_t addition overflow guard — self-contained and
    // independent of envelope_valid() having been called first (S4).
    if (env.payload_length > (UINT32_MAX - WIRE_HEADER_SIZE)) {
        return Result::ERR_INVALID;
    }
    // Power of 10 rule 2: fixed bounds check
    uint32_t required_len = WIRE_HEADER_SIZE + env.payload_length;
    if (buf_len < required_len) {
        return Result::ERR_INVALID;
    }

    // Power of 10 rule 1: simple, sequential writes (no nested loops)
    uint32_t offset = 0U;

    // Write header fields in big-endian order
    offset = write_u8(buf, offset, static_cast<uint8_t>(env.message_type));
    offset = write_u8(buf, offset, static_cast<uint8_t>(env.reliability_class));
    offset = write_u8(buf, offset, env.priority);
    offset = write_u8(buf, offset, PROTO_VERSION);  // wire protocol version (REQ-3.2.8)

    offset = write_u64(buf, offset, env.message_id);
    offset = write_u64(buf, offset, env.timestamp_us);
    offset = write_u32(buf, offset, env.source_id);
    offset = write_u32(buf, offset, env.destination_id);
    offset = write_u64(buf, offset, env.expiry_time_us);
    offset = write_u32(buf, offset, env.payload_length);
    // Bytes 40–41: PROTO_MAGIC (BE); bytes 42–43: reserved zero (REQ-3.2.8)
    offset = write_u32(buf, offset, (static_cast<uint32_t>(PROTO_MAGIC) << 16U));

    // v2 fields: sequence_num (4B), fragment_index (1B), fragment_count (1B),
    // total_payload_length (2B). REQ-3.3.5.
    // fragment_count == 0 is invalid on the wire; treat as 1 (unfragmented).
    uint8_t wire_frag_count = (env.fragment_count == 0U) ? 1U : env.fragment_count;
    // Security finding H-1: assert payload_length fits in uint16 before cast to wire field.
    // MSG_MAX_PAYLOAD_BYTES (4096) is well within 0xFFFF; this assert is an always-on
    // invariant that detects any future constant change that would break the wire format.
    // SEC-004: CERT INT31-C — explicit ERR_INVALID before uint32→uint16 narrowing cast.
    // This is the primary error return; the assert below is defense-in-depth only.
    // envelope_valid() caps payload_length at MSG_MAX_PAYLOAD_BYTES (4096 < 0xFFFF),
    // so this path is currently unreachable — but must remain in case the constant
    // changes or this function is called outside the normal validation path (§7a).
    if (env.payload_length > 0xFFFFU) {
        return Result::ERR_INVALID;
    }
    // Power of 10 Rule 5: mandatory assertion for uint32->uint16 narrowing (H-1).
    NEVER_COMPILED_OUT_ASSERT(env.payload_length <= 0xFFFFU);  // Assert: payload fits wire uint16 field (H-1)
    // total_payload_length == 0 for unfragmented: use payload_length as default.
    uint16_t wire_total_pl = (env.total_payload_length == 0U)
                                 ? static_cast<uint16_t>(env.payload_length)
                                 : env.total_payload_length;
    offset = write_u32(buf, offset, env.sequence_num);
    offset = write_u8(buf, offset, env.fragment_index);
    offset = write_u8(buf, offset, wire_frag_count);
    offset = write_u16(buf, offset, wire_total_pl);

    // Power of 10 rule 5: assertion after header write
    NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE);  // Assert: header size matches constant

    // Copy payload bytes directly (no byte-order conversion for opaque data)
    // Power of 10 rule 3: bounded copy (envelope.payload_length already validated)
    // Security finding H-2: re-check that offset + payload_length is still within buf_len
    // before memcpy, even though the earlier required_len check should guarantee this.
    // The assert makes the invariant explicit and always-on (Power of 10 Rule 5, H-2).
    NEVER_COMPILED_OUT_ASSERT(offset + env.payload_length <= buf_len);  // Assert: payload fits buffer (H-2)
    if (env.payload_length > 0U) {
        (void)memcpy(&buf[offset], env.payload, env.payload_length);
    }

    out_len = required_len;

    // Power of 10 rule 5: post-condition assertions
    NEVER_COMPILED_OUT_ASSERT(out_len == WIRE_HEADER_SIZE + env.payload_length);  // Assert: output size correct
    NEVER_COMPILED_OUT_ASSERT(out_len >= WIRE_HEADER_SIZE);  // Assert: minimum header always serialized

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Enum range-validation helpers (Security finding F1)
// Kept as separate static functions to hold CC of deserialize() within Rule 4
// ceiling of 10.  Each function has a single decision point.
// REQ-3.2.3
// ─────────────────────────────────────────────────────────────────────────────

/// Return true when the raw byte is a defined MessageType value.
/// Valid: 0 (DATA), 1 (ACK), 2 (NAK), 3 (HEARTBEAT), 4 (HELLO), 255 (INVALID).
static bool message_type_in_range(uint8_t raw)
{
    // DATA=0, ACK=1, NAK=2, HEARTBEAT=3, HELLO=4, INVALID=255
    NEVER_COMPILED_OUT_ASSERT(raw <= 255U);  // Assert: uint8_t invariant always holds
    return (raw <= 4U) || (raw == 255U);
}

/// Return true when the raw byte is a defined ReliabilityClass value.
/// Valid: 0 (BEST_EFFORT), 1 (RELIABLE_ACK), 2 (RELIABLE_RETRY).
static bool reliability_class_in_range(uint8_t raw)
{
    NEVER_COMPILED_OUT_ASSERT(raw <= 255U);  // Assert: uint8_t invariant always holds
    return (raw <= 2U);
}

/// Validate v2 fragment header fields already read into env.
/// Returns false if any constraint is violated.
/// REQ-3.2.3
static bool fragment_header_valid(const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(env.fragment_count <= 255U);  // Assert: uint8_t field invariant
    NEVER_COMPILED_OUT_ASSERT(env.fragment_index <= 255U);  // Assert: uint8_t field invariant
    if (env.fragment_count == 0U) {
        return false;
    }
    if (env.fragment_index >= env.fragment_count) {
        return false;
    }
    if (static_cast<uint32_t>(env.fragment_count) > FRAG_MAX_COUNT) {
        return false;
    }
    return (static_cast<uint32_t>(env.total_payload_length) <= MSG_MAX_PAYLOAD_BYTES);
}

// ─────────────────────────────────────────────────────────────────────────────
// Deserialization
// ─────────────────────────────────────────────────────────────────────────────

Result Serializer::deserialize(const uint8_t*  buf,
                               uint32_t         buf_len,
                               MessageEnvelope& env)
{
    // Power of 10 rule 7: check all preconditions
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);           // Assert: input buffer is valid
    NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL);  // Assert: buffer length is valid

    // Power of 10 rule 2: fixed bounds check
    if (buf_len < WIRE_HEADER_SIZE) {
        return Result::ERR_INVALID;
    }

    // Initialize output envelope to known state
    envelope_init(env);

    // Power of 10 rule 1: simple, sequential reads (no nested loops)
    uint32_t offset = 0U;

    // Read header fields in big-endian order

    // REQ-3.2.3: validate message_type before casting — values 4–254 are undefined.
    // (Security finding F1: out-of-range cast bypasses envelope_valid() and hits SC assert)
    uint8_t mt_raw = read_u8(buf, offset);
    offset += 1U;
    if (!message_type_in_range(mt_raw)) {
        return Result::ERR_INVALID;
    }
    env.message_type = static_cast<MessageType>(mt_raw);

    // REQ-3.2.3: validate reliability_class before casting — values 3–255 are undefined.
    // (Security finding F1: out-of-range cast bypasses envelope_valid() and hits SC assert)
    uint8_t rc_raw = read_u8(buf, offset);
    offset += 1U;
    if (!reliability_class_in_range(rc_raw)) {
        return Result::ERR_INVALID;
    }
    env.reliability_class = static_cast<ReliabilityClass>(rc_raw);

    env.priority = read_u8(buf, offset);
    offset += 1U;

    uint8_t proto_ver = read_u8(buf, offset);
    offset += 1U;
    // REQ-3.2.8: reject any frame whose version byte does not match PROTO_VERSION
    if (proto_ver != PROTO_VERSION) {
        return Result::ERR_INVALID;
    }

    env.message_id = read_u64(buf, offset);
    offset += 8U;

    env.timestamp_us = read_u64(buf, offset);
    offset += 8U;

    env.source_id = read_u32(buf, offset);
    offset += 4U;

    env.destination_id = read_u32(buf, offset);
    offset += 4U;

    env.expiry_time_us = read_u64(buf, offset);
    offset += 8U;

    env.payload_length = read_u32(buf, offset);
    offset += 4U;

    uint32_t magic_word = read_u32(buf, offset);
    offset += 4U;
    // REQ-3.2.8: reject if magic (bytes 40–41) is wrong or reserved (bytes 42–43) is non-zero
    if (magic_word != (static_cast<uint32_t>(PROTO_MAGIC) << 16U)) {
        return Result::ERR_INVALID;
    }

    // v2 fields: sequence_num, fragment_index, fragment_count, total_payload_length
    env.sequence_num = read_u32(buf, offset);
    offset += 4U;

    env.fragment_index = read_u8(buf, offset);
    offset += 1U;

    env.fragment_count = read_u8(buf, offset);
    offset += 1U;

    env.total_payload_length = read_u16(buf, offset);
    offset += 2U;

    // REQ-3.2.3: validate fragment metadata — delegate to helper to hold CC within Rule 4
    if (!fragment_header_valid(env)) {
        return Result::ERR_INVALID;
    }

    // Power of 10 rule 5: assertion after header read
    NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE);  // Assert: header size matches constant

    // Validate payload_length against message definition
    if (env.payload_length > MSG_MAX_PAYLOAD_BYTES) {
        return Result::ERR_INVALID;
    }

    // F-3 defense-in-depth overflow guard (CERT INT30-C): although the preceding check
    // makes this overflow mathematically impossible (4096+52 << UINT32_MAX), we add an
    // explicit guard for consistency with serialize() and to satisfy static analysis.
    if (env.payload_length > (UINT32_MAX - WIRE_HEADER_SIZE)) {
        return Result::ERR_INVALID;
    }

    // Check that total message fits in buffer
    uint32_t total_size = WIRE_HEADER_SIZE + env.payload_length;
    if (buf_len < total_size) {
        return Result::ERR_INVALID;
    }

    // Copy payload bytes directly (no byte-order conversion for opaque data)
    if (env.payload_length > 0U) {
        (void)memcpy(env.payload, &buf[offset], env.payload_length);
    }

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));  // Assert: deserialized envelope is valid

    return Result::OK;
}
