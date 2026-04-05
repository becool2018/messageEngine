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
 * @file RequestReplyHeader.hpp
 * @brief Packed 12-byte prefix embedded in MessageEnvelope::payload for
 *        request/response correlation.
 *
 * The header travels inside the existing payload field; neither
 * MessageEnvelope, Serializer, nor ProtocolVersion are modified.
 *
 * Layout (no padding — enforced by static_assert):
 *   Byte 0      : kind (0 = REQUEST, 1 = RESPONSE)
 *   Bytes 1-8   : correlation_id (uint64_t, big-endian / network byte order)
 *   Bytes 9-11  : _pad (zero; reserved for future use)
 *
 * Rules applied:
 *   - Power of 10 Rule 3: no dynamic allocation; header is stack/member only.
 *   - Power of 10 Rule 8: __attribute__((packed)) is used once here; not for
 *     control flow.
 *   - MISRA C++:2023: packed struct deviation documented at point of use.
 *   - F-Prime style: plain struct, no STL, no templates.
 *
 * Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3
 */
// Implements: REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

#ifndef CORE_REQUEST_REPLY_HEADER_HPP
#define CORE_REQUEST_REPLY_HEADER_HPP

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// RRKind — discriminates REQUEST from RESPONSE
// ─────────────────────────────────────────────────────────────────────────────
enum class RRKind : uint8_t {
    REQUEST  = 0U,
    RESPONSE = 1U
};

// ─────────────────────────────────────────────────────────────────────────────
// RRHeader — 12-byte packed header prefixed to application payload
//
// MISRA C++:2023 deviation: __attribute__((packed)) is used to guarantee
// sizeof(RRHeader) == 12 across all toolchains.  No padding bytes are
// silently inserted, so the static_assert below is a compile-time proof.
// This is the minimal, documented deviation required for binary framing.
// ─────────────────────────────────────────────────────────────────────────────
// NOLINTNEXTLINE(clang-diagnostic-unknown-attributes)
struct __attribute__((packed)) RRHeader {
    uint8_t  kind;           ///< RRKind::REQUEST or RRKind::RESPONSE (cast on use)
    uint64_t correlation_id; ///< 8 bytes: request's message_id
    uint8_t  _pad[3];        ///< zero-fill; aligns total to 12 bytes
};

static_assert(sizeof(RRHeader) == 12U, "RRHeader must be exactly 12 bytes");

/// Size of RRHeader as a typed constant for use in payload-length arithmetic.
static const uint32_t RR_HEADER_SIZE = static_cast<uint32_t>(sizeof(RRHeader));

#endif // CORE_REQUEST_REPLY_HEADER_HPP
