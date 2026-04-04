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
 * @file Fragmentation.cpp
 * @brief Implementation of stateless message fragmentation.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, ≥2 assertions per function,
 *     fixed loop bounds, CC ≤ 10.
 *   - MISRA C++:2023: explicit casts, no UB.
 *   - F-Prime style: simple, deterministic logic.
 *
 * Implements: REQ-3.2.3, REQ-3.3.5
 */
// Implements: REQ-3.2.3, REQ-3.3.5

#include "Fragmentation.hpp"
#include "Assert.hpp"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// needs_fragmentation
// ─────────────────────────────────────────────────────────────────────────────
bool needs_fragmentation(const MessageEnvelope& env)
{
    NEVER_COMPILED_OUT_ASSERT(env.payload_length <= MSG_MAX_PAYLOAD_BYTES);  // Assert: bounded
    NEVER_COMPILED_OUT_ASSERT(FRAG_MAX_PAYLOAD_BYTES > 0U);                  // Assert: positive divisor

    return env.payload_length > FRAG_MAX_PAYLOAD_BYTES;
}

// ─────────────────────────────────────────────────────────────────────────────
// fragment_message
// ─────────────────────────────────────────────────────────────────────────────
uint32_t fragment_message(const MessageEnvelope& logical,
                          MessageEnvelope*       out_frags,
                          uint32_t               out_cap)
{
    NEVER_COMPILED_OUT_ASSERT(out_frags != nullptr);                          // Assert: output valid
    NEVER_COMPILED_OUT_ASSERT(out_cap >= FRAG_MAX_COUNT);                     // Assert: output capacity

    // Guard: payload too large to fragment (cannot exceed MSG_MAX_PAYLOAD_BYTES)
    if (logical.payload_length > MSG_MAX_PAYLOAD_BYTES) {
        return 0U;
    }

    // Guard: output capacity insufficient
    if (out_cap < FRAG_MAX_COUNT) {
        return 0U;
    }

    // Compute fragment count: ceiling division
    // Power of 10: bounded loop — frag_count <= FRAG_MAX_COUNT
    uint32_t frag_count = 0U;
    if (logical.payload_length == 0U) {
        frag_count = 1U;  // zero-payload: one empty fragment
    } else {
        frag_count = (logical.payload_length + FRAG_MAX_PAYLOAD_BYTES - 1U) /
                     FRAG_MAX_PAYLOAD_BYTES;
    }

    NEVER_COMPILED_OUT_ASSERT(frag_count >= 1U);               // Assert: at least one fragment
    NEVER_COMPILED_OUT_ASSERT(frag_count <= FRAG_MAX_COUNT);   // Assert: bounded fragment count

    // Produce each fragment
    // Power of 10 Rule 2: bounded loop — frag_count <= FRAG_MAX_COUNT (constant)
    uint32_t offset = 0U;
    for (uint32_t i = 0U; i < frag_count; ++i) {
        // Compute this fragment's payload slice
        uint32_t remaining   = logical.payload_length - offset;
        uint32_t frag_payload = (remaining > FRAG_MAX_PAYLOAD_BYTES)
                                    ? FRAG_MAX_PAYLOAD_BYTES
                                    : remaining;

        // Copy shared header fields from the logical envelope
        envelope_copy(out_frags[i], logical);

        // Set fragment-specific fields
        out_frags[i].fragment_index       = static_cast<uint8_t>(i);
        out_frags[i].fragment_count       = static_cast<uint8_t>(frag_count);
        out_frags[i].total_payload_length = static_cast<uint16_t>(logical.payload_length);
        out_frags[i].payload_length       = frag_payload;

        // Copy payload slice (Power of 10 Rule 3: bounded copy)
        if (frag_payload > 0U) {
            (void)memcpy(out_frags[i].payload, &logical.payload[offset], frag_payload);
        }

        offset += frag_payload;
    }

    NEVER_COMPILED_OUT_ASSERT(offset == logical.payload_length);  // Assert: all bytes covered

    return frag_count;
}
