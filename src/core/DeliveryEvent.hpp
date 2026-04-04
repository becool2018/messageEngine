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
 * @file DeliveryEvent.hpp
 * @brief Enumeration and struct for DeliveryEngine pull-style observability events.
 *
 * Each notable delivery-layer action is recorded as a DeliveryEvent and pushed
 * into a bounded ring (DeliveryEventRing). Callers poll events via
 * DeliveryEngine::poll_event(). This is a pull-style interface: no callbacks,
 * no virtual dispatch for event delivery.
 *
 * Rules applied:
 *   - Power of 10 Rule 3: no dynamic allocation; all fields are POD.
 *   - MISRA C++:2023: no STL, no templates.
 *   - F-Prime style: plain enum class and plain struct.
 *
 * NSC: observability-only; no effect on delivery state.
 *
 * Implements: REQ-7.2.5
 */
// Implements: REQ-7.2.5

#ifndef CORE_DELIVERY_EVENT_HPP
#define CORE_DELIVERY_EVENT_HPP

#include <cstdint>
#include "Types.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEventKind — identifies the event type
// ─────────────────────────────────────────────────────────────────────────────

/// Identifies what delivery-layer action produced this event.
/// NSC: observability label only; no operational effect.
enum class DeliveryEventKind : uint8_t {
    SEND_OK        = 0U,  ///< Message successfully submitted to transport
    SEND_FAIL      = 1U,  ///< Message send failed (transport error, expiry, full, etc.)
    ACK_RECEIVED   = 2U,  ///< ACK received for a previously sent message
    RETRY_FIRED    = 3U,  ///< Retry transmission fired by pump_retries()
    ACK_TIMEOUT    = 4U,  ///< ACK not received before deadline (sweep_ack_timeouts())
    DUPLICATE_DROP = 5U,  ///< Received message suppressed as duplicate
    EXPIRY_DROP    = 6U,  ///< Received or sent message dropped due to expiry
    MISROUTE_DROP  = 7U,  ///< Received message dropped — wrong destination_id
};

// ─────────────────────────────────────────────────────────────────────────────
// DeliveryEvent — one observability record
// ─────────────────────────────────────────────────────────────────────────────

/// A single delivery-layer observability event.
/// Pushed into DeliveryEventRing by DeliveryEngine; read via poll_event().
/// NSC: read-only record; never modifies delivery state.
struct DeliveryEvent {
    DeliveryEventKind kind;         ///< What happened
    uint64_t          message_id;   ///< Which message (source or destination, context-dep.)
    NodeId            node_id;      ///< Peer node (source or dest, context-dependent)
    uint64_t          timestamp_us; ///< When the event occurred (µs)
    Result            result;       ///< Associated result code (OK, ERR_*, etc.)
};

#endif // CORE_DELIVERY_EVENT_HPP
