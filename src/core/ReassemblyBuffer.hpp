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
 * @file ReassemblyBuffer.hpp
 * @brief Bounded reassembly of fragmented logical messages.
 *
 * ReassemblyBuffer maintains REASSEMBLY_SLOT_COUNT concurrent reassembly slots,
 * each keyed by (source_id, message_id). Fragments from different messages can
 * be received interleaved; each slot tracks which fragment indices have arrived
 * via a bitmask.  When all fragments have been collected the slot is assembled
 * into a logical MessageEnvelope and the slot is freed.
 *
 * Rules applied:
 *   - Power of 10: no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: no templates, no STL, no exceptions.
 *   - F-Prime style: explicit error codes, simple state machine.
 *
 * State machine per slot:
 *   INACTIVE → COLLECTING (first fragment ingested)
 *   COLLECTING → COMPLETE (all fragments received)
 *   COLLECTING → INACTIVE (sweep_expired when slot.expiry_us < now_us)
 *
 * Implements: REQ-3.2.3, REQ-3.3.3, REQ-3.2.9, REQ-3.2.10
 */
// Implements: REQ-3.2.3, REQ-3.3.3, REQ-3.2.9, REQ-3.2.10

#ifndef CORE_REASSEMBLY_BUFFER_HPP
#define CORE_REASSEMBLY_BUFFER_HPP

#include <cstdint>
#include "Types.hpp"
#include "MessageEnvelope.hpp"

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer
// ─────────────────────────────────────────────────────────────────────────────

class ReassemblyBuffer {
public:
    /// Must be called once during system initialization before any other method.
    void init();

    // Safety-critical (SC): HAZ-003, HAZ-019 — reassembly is on the receive path;
    // a corrupt fragment could allow duplicate delivery, slot exhaustion, or integer
    // overflow via wire-supplied total_payload_length (REQ-3.2.10).
    /// Ingest one wire fragment.
    ///
    /// @param frag      [in]  Wire fragment envelope (fragment_count > 1 expected,
    ///                        but fragment_count == 1 is also accepted and completes
    ///                        immediately).
    /// @param logical_out [out] Filled with the reassembled logical envelope on
    ///                         return of Result::OK.
    /// @return OK             if this fragment completes the message (logical_out valid).
    ///         ERR_AGAIN      if more fragments are still needed.
    ///         ERR_FULL       if no reassembly slot is available for a new message.
    ///         ERR_INVALID    if fragment metadata is inconsistent (different
    ///                        fragment_count or total_payload_length from a prior
    ///                        fragment of the same message, or out-of-range values).
    Result ingest(const MessageEnvelope& frag, MessageEnvelope& logical_out, uint64_t now_us);

    /// Expire and free slots whose expiry_us <= now_us.
    /// Should be called periodically to reclaim slots from stalled senders.
    /// NSC: housekeeping only; no effect on delivery semantics.
    void sweep_expired(uint64_t now_us);

    /// Sweep and free reassembly slots that have been open longer than stale_threshold_us.
    /// Protects against slot exhaustion from peers that send only the first fragment.
    /// stale_threshold_us == 0 disables the sweep (returns 0 immediately).
    /// Returns the number of slots freed.
    /// Power of 10 Rule 2: bounded loop (REASSEMBLY_SLOT_COUNT iterations).
    // NSC: housekeeping only.
    uint32_t sweep_stale(uint64_t now_us, uint64_t stale_threshold_us);

private:
    // ─────────────────────────────────────────────────────────────────────────
    // ReassemblySlot: one per in-flight logical message
    // ─────────────────────────────────────────────────────────────────────────
    struct ReassemblySlot {
        uint64_t        message_id;                    ///< key field
        NodeId          source_id;                     ///< key field
        uint8_t         buf[MSG_MAX_PAYLOAD_BYTES];    ///< accumulated payload (no init needed)
        uint32_t        received_mask;                 ///< bitmask of received fragment_index values
        uint32_t        received_bytes;               ///< F-6: sum of payload_length across all received fragments
        uint8_t         expected_count;               ///< fragment_count from first fragment
        uint16_t        total_length;                 ///< total_payload_length from first fragment
        uint64_t        expiry_us;                     ///< expiry_time_us from first fragment
        uint64_t        open_time_us;                  ///< Timestamp when this slot was opened (for stale-sweep).
        MessageEnvelope header;                        ///< copy of first fragment (for metadata)
        bool            active;                        ///< slot in use
    };

    ReassemblySlot m_slots[REASSEMBLY_SLOT_COUNT] = {};
    bool           m_initialized = false;

    // Private helpers — extracted to keep ingest() CC ≤ 10
    // Find existing active slot for (source_id, message_id); returns REASSEMBLY_SLOT_COUNT
    // (invalid index) if not found.
    uint32_t find_slot(NodeId source_id, uint64_t message_id) const;

    // Find a free (inactive) slot; returns REASSEMBLY_SLOT_COUNT if all are in use.
    uint32_t find_free_slot() const;

    // Count active slots whose source_id matches src; used to enforce the per-source
    // slot cap (k_reasm_per_src_max) against reassembly slot exhaustion DoS (F-1).
    uint32_t count_open_slots_for_src(NodeId src) const;

    // Initialize a new slot from the first fragment received for a message.
    void open_slot(uint32_t idx, const MessageEnvelope& frag, uint64_t now_us);

    // Validate that subsequent fragment metadata is consistent with the open slot.
    // Returns ERR_INVALID on mismatch, OK otherwise.
    Result validate_fragment(uint32_t idx, const MessageEnvelope& frag) const;

    // Place this fragment's payload into the slot buffer and record it in received_mask.
    // G-2: changed from void to Result; returns ERR_INVALID if any computed offset
    // or accumulated byte count would exceed MSG_MAX_PAYLOAD_BYTES. The slot is NOT
    // freed here — ingest_multifrag() frees it on ERR_INVALID.
    Result record_fragment(uint32_t idx, const MessageEnvelope& frag);

    // Check if all fragments have been received.
    bool is_complete(uint32_t idx) const;

    // Assemble the slot into a logical envelope and free the slot.
    // F-6: returns OK on success; ERR_INVALID if received_bytes != total_length
    // (inflated wire-declared length); slot is freed in both cases.
    Result assemble_and_free(uint32_t idx, MessageEnvelope& logical_out);

    // Validate fragment-level metadata (bounds checks on fragment_count, index, total_length).
    // Returns ERR_INVALID on any violation, OK otherwise.
    // Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
    Result validate_metadata(const MessageEnvelope& frag) const;

    // Find or open the reassembly slot for frag; writes slot index to idx_out.
    // Returns ERR_FULL if no free slot is available for a new message.
    // Returns ERR_INVALID if the existing slot has inconsistent metadata.
    // Returns OK and sets idx_out on success.
    // Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
    Result find_or_open_slot(const MessageEnvelope& frag, uint32_t& idx_out, uint64_t now_us);

    // Handle the multi-fragment path of ingest(): find/open slot, bitmask
    // dedup, record, and assemble when complete.
    // Precondition: frag.fragment_count > 1 and metadata already validated.
    // Returns OK (logical_out filled), ERR_AGAIN, ERR_FULL, or ERR_INVALID.
    // Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
    Result ingest_multifrag(const MessageEnvelope& frag, MessageEnvelope& logical_out,
                             uint64_t now_us);
};

#endif // CORE_REASSEMBLY_BUFFER_HPP
