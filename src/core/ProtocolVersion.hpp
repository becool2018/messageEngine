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
 * @file ProtocolVersion.hpp
 * @brief Wire-format protocol version and magic number constants.
 *
 * These constants are the single source of truth for the on-wire protocol
 * version.  They are written into every serialized frame and checked by
 * every deserializer.  Bump PROTO_VERSION on any change to the wire format
 * (field added, removed, reordered, or resized).  The project version
 * (src/core/Version.hpp) is independent and must NOT be used here.
 *
 * Wire header layout (52 bytes, big-endian):
 *   offset  size  field
 *   0       1     message_type
 *   1       1     reliability_class
 *   2       1     priority
 *   3       1     PROTO_VERSION          ← this file
 *   4       8     message_id
 *   12      8     timestamp_us
 *   20      4     source_id
 *   24      4     destination_id
 *   28      8     expiry_time_us
 *   36      4     payload_length
 *   40      2     PROTO_MAGIC (BE)       ← this file
 *   42      2     reserved (must be 0)
 *   44      4     sequence_num (BE)      ← v2 addition
 *   48      1     fragment_index         ← v2 addition
 *   49      1     fragment_count         ← v2 addition
 *   50      2     total_payload_length (BE) ← v2 addition
 *
 * Version history:
 *   0  — unversioned (pre-protocol-versioning builds); rejected by v1+ deserializers.
 *   1  — initial versioned wire format (proto_version + magic fields).
 *   2  — protocol v2: sequence_num, fragment_index, fragment_count,
 *         total_payload_length; ordering enforcement; bounded fragmentation/reassembly.
 *
 * Requirement traceability: CLAUDE.md §3.2.8
 *
 * Rules applied:
 *   - Power of 10 rule 8: compile-time constants only; no macros that affect control flow.
 *   - F-Prime style: no templates, no STL.
 *
 * Implements: REQ-3.2.8
 */

#ifndef CORE_PROTOCOL_VERSION_HPP
#define CORE_PROTOCOL_VERSION_HPP

#include <cstdint>

/// Current wire-format protocol version.
/// Increment this on any change to the serialized field layout.
static const uint8_t  PROTO_VERSION = 2U;

/// Two-byte frame magic: ASCII 'M' (0x4D) followed by 'E' (0x45).
/// Written at wire bytes 40–41 (big-endian high-then-low byte order).
/// Provides a fast sanity check against misaligned reads or wrong-port data.
static const uint16_t PROTO_MAGIC   = 0x4D45U;

// NOTE: MessageType::HELLO (= 4U) was added in the transport-layer unicast routing
// fix (REQ-6.1.8). HELLO occupies the existing message_type byte at its existing
// wire offset. No wire field layout changed; PROTO_VERSION is NOT bumped for HELLO.
// Old deserializers that do not recognize value 4U will reject HELLO frames, which
// is correct: pre-registration senders should not be receiving DATA from the server.

#endif // CORE_PROTOCOL_VERSION_HPP
