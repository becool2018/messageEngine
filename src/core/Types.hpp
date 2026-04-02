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
 * @file Types.hpp
 * @brief Common types, constants, and enumerations for the messageEngine.
 *
 * All production code is held to Power of 10, MISRA C++, and F-Prime
 * style subset rules.  See .claude/CLAUDE.md for full details.
 *
 * Implements: REQ-3.1, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3
 */

#ifndef CORE_TYPES_HPP
#define CORE_TYPES_HPP

#include <cstdint>
#include <cstddef>

// ─────────────────────────────────────────────────────────────────────────────
// Compile-time capacity constants
// (Power of 10: fixed bounds, no dynamic allocation after init)
// ─────────────────────────────────────────────────────────────────────────────
static const uint32_t MSG_MAX_PAYLOAD_BYTES  = 4096U;
static const uint32_t MSG_RING_CAPACITY      = 64U;   ///< per-channel queue depth
static const uint32_t DEDUP_WINDOW_SIZE      = 128U;  ///< duplicate-filter sliding window
static const uint32_t ACK_TRACKER_CAPACITY   = 32U;   ///< max outstanding unACKed messages
static const uint32_t MAX_RETRY_COUNT        = 5U;
static const uint32_t MAX_CHANNELS           = 8U;
static const uint32_t MAX_PEER_NODES         = 16U;
static const uint32_t IMPAIR_DELAY_BUF_SIZE  = 32U;   ///< max buffered delayed messages
static const uint32_t MAX_TCP_CONNECTIONS    = 8U;
static const uint32_t SOCKET_RECV_BUF_BYTES  = 8192U;
static const uint32_t NODE_ID_INVALID        = 0U;
/// Maximum DTLS datagram payload (serialized envelope) to avoid IP fragmentation.
/// Accounts for IPv4 (20 B) + UDP (8 B) + DTLS record (13 B) overhead on a 1500 B MTU.
/// DtlsUdpBackend rejects send_message() calls whose serialized length exceeds this.
/// (REQ-6.4.4)
static const uint32_t DTLS_MAX_DATAGRAM_BYTES = 1400U;

// ─────────────────────────────────────────────────────────────────────────────
// Error / severity model  (F-Prime style)
// ─────────────────────────────────────────────────────────────────────────────
enum class Severity : uint8_t {
    INFO        = 0U,  ///< Informational
    WARNING_LO  = 1U,  ///< Localized, recoverable
    WARNING_HI  = 2U,  ///< System-wide, recoverable
    FATAL       = 3U   ///< Unrecoverable; component must restart/reset
};

// ─────────────────────────────────────────────────────────────────────────────
// Message type
// ─────────────────────────────────────────────────────────────────────────────
enum class MessageType : uint8_t {
    DATA       = 0U,
    ACK        = 1U,
    NAK        = 2U,
    HEARTBEAT  = 3U,
    INVALID    = 255U
};

// ─────────────────────────────────────────────────────────────────────────────
// Delivery / reliability class
// ─────────────────────────────────────────────────────────────────────────────
enum class ReliabilityClass : uint8_t {
    BEST_EFFORT     = 0U,  ///< No retry, no ACK
    RELIABLE_ACK    = 1U,  ///< Single send, expect ACK; no auto-retry
    RELIABLE_RETRY  = 2U   ///< Retry until ACK or expiry; dedup on receiver
};

// ─────────────────────────────────────────────────────────────────────────────
// Channel ordering requirement
// ─────────────────────────────────────────────────────────────────────────────
enum class OrderingMode : uint8_t {
    ORDERED   = 0U,
    UNORDERED = 1U
};

// ─────────────────────────────────────────────────────────────────────────────
// Transport kind (selects which backend is used)
// ─────────────────────────────────────────────────────────────────────────────
enum class TransportKind : uint8_t {
    TCP       = 0U,
    UDP       = 1U,
    LOCAL_SIM = 2U,
    DTLS_UDP  = 3U   ///< DTLS-secured UDP; see DtlsUdpBackend (REQ-6.4.1)
};

// ─────────────────────────────────────────────────────────────────────────────
// Generic result type  (avoids exceptions; Power of 10 + F-Prime)
// ─────────────────────────────────────────────────────────────────────────────
enum class Result : uint8_t {
    OK             = 0U,
    ERR_TIMEOUT    = 1U,
    ERR_FULL       = 2U,
    ERR_EMPTY      = 3U,
    ERR_INVALID    = 4U,
    ERR_IO         = 5U,
    ERR_DUPLICATE  = 6U,
    ERR_EXPIRED    = 7U,
    ERR_OVERRUN    = 8U
};

// ─────────────────────────────────────────────────────────────────────────────
// Compact node/endpoint identifier
// ─────────────────────────────────────────────────────────────────────────────
typedef uint32_t NodeId;

// ─────────────────────────────────────────────────────────────────────────────
// Convenience helpers (no macros for logic; Power of 10 rule 8)
// ─────────────────────────────────────────────────────────────────────────────
inline bool result_ok(Result r) { return r == Result::OK; }

#endif // CORE_TYPES_HPP
