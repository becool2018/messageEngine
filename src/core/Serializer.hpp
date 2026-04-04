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
 * @file Serializer.hpp
 * @brief Deterministic, endian-safe serialization of MessageEnvelope.
 *
 * Requirement traceability: CLAUDE.md §3.2 (serialization), CLAUDE.md §4.1 (wire format).
 *
 * Wire format uses big-endian (network byte order) for all multi-byte fields.
 * This ensures deterministic binary compatibility across platforms.
 *
 * Rules applied:
 *   - Power of 10: static methods, fixed buffer sizes, ≥2 assertions per method.
 *   - MISRA C++: no dynamic allocation, no exceptions, ≤1 ptr indirection.
 *   - F-Prime style: no STL, no templates.
 *
 * Implements: REQ-3.2.3
 */

#ifndef CORE_SERIALIZER_HPP
#define CORE_SERIALIZER_HPP

#include <cstdint>
#include <cstring>
#include "Assert.hpp"
#include "Types.hpp"
#include "MessageEnvelope.hpp"
#include "ProtocolVersion.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Serializer: deterministic, endian-safe message serialization
// ─────────────────────────────────────────────────────────────────────────────

class Serializer {
public:
    /// Wire format header size in bytes (protocol v2).
    /// Layout:
    ///   1 byte   message_type
    ///   1 byte   reliability_class
    ///   1 byte   priority
    ///   1 byte   PROTO_VERSION (from ProtocolVersion.hpp; reject if unrecognised)
    ///   8 bytes  message_id (BE)
    ///   8 bytes  timestamp_us (BE)
    ///   4 bytes  source_id (BE)
    ///   4 bytes  destination_id (BE)
    ///   8 bytes  expiry_time_us (BE)
    ///   4 bytes  payload_length (BE)
    ///   2 bytes  PROTO_MAGIC (BE, 0x4D45 = 'ME'; from ProtocolVersion.hpp)
    ///   2 bytes  reserved (must be 0x0000)
    ///   4 bytes  sequence_num (BE)         [v2 addition]
    ///   1 byte   fragment_index            [v2 addition]
    ///   1 byte   fragment_count            [v2 addition]
    ///   2 bytes  total_payload_length (BE) [v2 addition]
    /// Total: 1+1+1+1+8+8+4+4+8+4+2+2+4+1+1+2 = 52 bytes
    static const uint32_t WIRE_HEADER_SIZE = 52U;

    // Safety-critical (SC): HAZ-005 — verified to M5
    /// Serialize a MessageEnvelope to a buffer in big-endian wire format.
    /// @param env        [in] envelope to serialize
    /// @param buf        [out] destination buffer
    /// @param buf_len    [in] available bytes in buf
    /// @param out_len    [out] bytes written (header + payload)
    /// @return OK on success; ERR_INVALID if buffer too small or envelope invalid
    static Result serialize(const MessageEnvelope& env,
                           uint8_t*               buf,
                           uint32_t               buf_len,
                           uint32_t&              out_len);

    // Safety-critical (SC): HAZ-001, HAZ-005 — verified to M5
    /// Deserialize a MessageEnvelope from a buffer.
    /// @param buf        [in] source buffer
    /// @param buf_len    [in] available bytes in buf
    /// @param env        [out] deserialized envelope
    /// @return OK on success; ERR_INVALID if buffer too small or payload_length invalid
    static Result deserialize(const uint8_t*  buf,
                             uint32_t         buf_len,
                             MessageEnvelope& env);

private:
    Serializer() {}  // not instantiable; static methods only

    // Helper functions for endian-safe I/O (Power of 10 rule 4: small, single-purpose)
    // Each uses manual bit shifts (no htons/ntohl) for explicit control

    /// Write one byte to buffer at offset, return new offset.
    static uint32_t write_u8(uint8_t* buf, uint32_t offset, uint8_t val);

    /// Write 2-byte big-endian value to buffer at offset, return new offset.
    static uint32_t write_u16(uint8_t* buf, uint32_t offset, uint16_t val);

    /// Read 2-byte big-endian value from buffer at offset, return value.
    static uint16_t read_u16(const uint8_t* buf, uint32_t offset);

    /// Write 4-byte big-endian value to buffer at offset, return new offset.
    static uint32_t write_u32(uint8_t* buf, uint32_t offset, uint32_t val);

    /// Write 8-byte big-endian value to buffer at offset, return new offset.
    static uint32_t write_u64(uint8_t* buf, uint32_t offset, uint64_t val);

    /// Read one byte from buffer at offset, return value.
    static uint8_t read_u8(const uint8_t* buf, uint32_t offset);

    /// Read 4-byte big-endian value from buffer at offset, return value.
    static uint32_t read_u32(const uint8_t* buf, uint32_t offset);

    /// Read 8-byte big-endian value from buffer at offset, return value.
    static uint64_t read_u64(const uint8_t* buf, uint32_t offset);
};

#endif // CORE_SERIALIZER_HPP
