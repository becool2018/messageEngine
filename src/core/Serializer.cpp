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

/// Read one byte from buffer at offset; return value.
inline uint8_t Serializer::read_u8(const uint8_t* buf, uint32_t offset)
{
    NEVER_COMPILED_OUT_ASSERT(buf != nullptr);             // Assert: valid buffer pointer
    NEVER_COMPILED_OUT_ASSERT(offset <= 0xFFFFFFFFUL - 1U); // Assert: offset won't overflow

    return buf[offset];
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
    offset = write_u8(buf, offset, 0U);  // padding byte must be 0

    offset = write_u64(buf, offset, env.message_id);
    offset = write_u64(buf, offset, env.timestamp_us);
    offset = write_u32(buf, offset, env.source_id);
    offset = write_u32(buf, offset, env.destination_id);
    offset = write_u64(buf, offset, env.expiry_time_us);
    offset = write_u32(buf, offset, env.payload_length);
    offset = write_u32(buf, offset, 0U);  // padding word must be 0

    // Power of 10 rule 5: assertion after header write
    NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE);  // Assert: header size matches constant

    // Copy payload bytes directly (no byte-order conversion for opaque data)
    // Power of 10 rule 3: bounded copy (envelope.payload_length already validated)
    if (env.payload_length > 0U) {
        (void)memcpy(&buf[offset], env.payload, env.payload_length);
    }

    out_len = required_len;

    // Power of 10 rule 5: post-condition assertion
    NEVER_COMPILED_OUT_ASSERT(out_len == WIRE_HEADER_SIZE + env.payload_length);  // Assert: output size correct

    return Result::OK;
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
    env.message_type = static_cast<MessageType>(read_u8(buf, offset));
    offset += 1U;

    env.reliability_class = static_cast<ReliabilityClass>(read_u8(buf, offset));
    offset += 1U;

    env.priority = read_u8(buf, offset);
    offset += 1U;

    uint8_t padding1 = read_u8(buf, offset);
    offset += 1U;
    // Power of 10 rule 5: validate padding
    if (padding1 != 0U) {
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

    uint32_t padding2 = read_u32(buf, offset);
    offset += 4U;
    // Power of 10 rule 5: validate padding
    if (padding2 != 0U) {
        return Result::ERR_INVALID;
    }

    // Power of 10 rule 5: assertion after header read
    NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE);  // Assert: header size matches constant

    // Validate payload_length against message definition
    if (env.payload_length > MSG_MAX_PAYLOAD_BYTES) {
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
