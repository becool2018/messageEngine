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
 * @file Fragmentation.hpp
 * @brief Stateless fragmentation of logical MessageEnvelopes into wire fragments.
 *
 * A logical message whose payload_length exceeds FRAG_MAX_PAYLOAD_BYTES is split
 * into up to FRAG_MAX_COUNT wire fragments. Each fragment carries a slice of the
 * logical payload plus fragment_index / fragment_count metadata so the receiver
 * can reassemble the original message.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: no templates, no STL, no exceptions.
 *   - F-Prime style: simple, single-purpose free functions.
 *
 * Implements: REQ-3.2.3, REQ-3.3.5
 */
// Implements: REQ-3.2.3, REQ-3.3.5

#ifndef CORE_FRAGMENTATION_HPP
#define CORE_FRAGMENTATION_HPP

#include <cstdint>
#include "Types.hpp"
#include "MessageEnvelope.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// Fragmentation: split a logical envelope into wire-fragment envelopes
// ─────────────────────────────────────────────────────────────────────────────

/// Return true when the envelope's payload exceeds FRAG_MAX_PAYLOAD_BYTES and
/// must be split into multiple wire fragments before sending.
/// NSC: predicate only; no side effects.
bool needs_fragmentation(const MessageEnvelope& env);

/// Fragment one logical MessageEnvelope into up to FRAG_MAX_COUNT wire envelopes.
///
/// Each output fragment carries:
///   - same message_id, source_id, destination_id, reliability_class, priority,
///     expiry_time_us, sequence_num, timestamp_us
///   - fragment_index = 0..N-1
///   - fragment_count = N
///   - total_payload_length = logical payload_length
///   - payload_length = length of this fragment's slice
///   - payload = slice of the logical payload
///
/// @param logical  [in]  The logical envelope to fragment. payload_length must be
///                       <= MSG_MAX_PAYLOAD_BYTES. message_type must be valid.
/// @param out_frags [out] Output array; caller must provide capacity >= FRAG_MAX_COUNT.
/// @param out_cap  [in]  Number of elements in out_frags (must be >= FRAG_MAX_COUNT).
/// @return Number of fragments produced (>= 1 on success), or 0 on error
///         (payload too large or out_cap too small).
///
/// Safety-critical (SC): HAZ-005 — fragment splitting is a wire-format operation.
uint32_t fragment_message(const MessageEnvelope& logical,
                          MessageEnvelope*       out_frags,
                          uint32_t               out_cap);

#endif // CORE_FRAGMENTATION_HPP
