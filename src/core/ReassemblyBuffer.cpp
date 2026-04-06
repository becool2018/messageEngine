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
 * @file ReassemblyBuffer.cpp
 * @brief Implementation of bounded fragment reassembly.
 *
 * Rules applied:
 *   - Power of 10: fixed loop bounds (REASSEMBLY_SLOT_COUNT / FRAG_MAX_COUNT),
 *     no dynamic allocation, ≥2 assertions per function, CC ≤ 10.
 *   - MISRA C++:2023: checked casts, no UB.
 *   - F-Prime style: explicit error codes.
 *
 * Implements: REQ-3.2.3, REQ-3.3.3, REQ-3.2.9
 */
// Implements: REQ-3.2.3, REQ-3.3.3, REQ-3.2.9

#include "ReassemblyBuffer.hpp"
#include "Assert.hpp"
#include "Logger.hpp"
#include <cstring>

// F-1: Per-source cap — a single peer may hold at most half the total slots open
// simultaneously. This prevents one misbehaving or malicious peer from exhausting
// the entire reassembly table with partial fragment sets.
// REQ-3.2.9: stale slot reclamation prevents indefinite slot hold; the per-source
// cap prevents exhaustion before the stale sweep fires.
static const uint32_t k_reasm_per_src_max = REASSEMBLY_SLOT_COUNT / 2U;

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::init
// ─────────────────────────────────────────────────────────────────────────────
void ReassemblyBuffer::init()
{
    NEVER_COMPILED_OUT_ASSERT(REASSEMBLY_SLOT_COUNT > 0U);  // Assert: capacity constant valid
    NEVER_COMPILED_OUT_ASSERT(FRAG_MAX_COUNT > 0U);         // Assert: fragment cap valid

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        m_slots[i].active        = false;
        m_slots[i].message_id    = 0U;
        m_slots[i].source_id     = 0U;
        m_slots[i].received_mask = 0U;
        m_slots[i].expected_count = 0U;
        m_slots[i].total_length  = 0U;
        m_slots[i].expiry_us     = 0U;
        m_slots[i].open_time_us  = 0U;
        envelope_init(m_slots[i].header);
    }

    m_initialized = true;

    NEVER_COMPILED_OUT_ASSERT(m_initialized);  // Assert: init complete
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::find_slot (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ReassemblyBuffer::find_slot(NodeId source_id, uint64_t message_id) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(source_id != 0U);        // Assert: valid source

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (m_slots[i].active &&
            m_slots[i].source_id == source_id &&
            m_slots[i].message_id == message_id) {
            return i;
        }
    }

    return REASSEMBLY_SLOT_COUNT;  // sentinel: not found
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::find_free_slot (private)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ReassemblyBuffer::find_free_slot() const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);           // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(REASSEMBLY_SLOT_COUNT > 0U);  // Assert: has capacity

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (!m_slots[i].active) {
            return i;
        }
    }

    return REASSEMBLY_SLOT_COUNT;  // sentinel: all in use
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::count_open_slots_for_src (private)
// Returns the number of active slots whose source_id matches src.
// Used by find_or_open_slot() to enforce k_reasm_per_src_max (F-1).
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ReassemblyBuffer::count_open_slots_for_src(NodeId src) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(src != 0U);                // Assert: valid source

    uint32_t count = 0U;
    // Power of 10 Rule 2: bounded loop at REASSEMBLY_SLOT_COUNT
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (m_slots[i].active && m_slots[i].source_id == src) {
            ++count;
        }
    }
    NEVER_COMPILED_OUT_ASSERT(count <= REASSEMBLY_SLOT_COUNT);  // Assert: result bounded
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::open_slot (private)
// ─────────────────────────────────────────────────────────────────────────────
void ReassemblyBuffer::open_slot(uint32_t idx, const MessageEnvelope& frag, uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(idx < REASSEMBLY_SLOT_COUNT);   // Assert: valid index
    NEVER_COMPILED_OUT_ASSERT(!m_slots[idx].active);           // Assert: slot is free

    m_slots[idx].active          = true;
    m_slots[idx].message_id      = frag.message_id;
    m_slots[idx].source_id       = frag.source_id;
    m_slots[idx].received_mask   = 0U;
    m_slots[idx].received_bytes  = 0U;   // F-6: zero byte accumulator at slot open
    m_slots[idx].expected_count  = frag.fragment_count;
    m_slots[idx].total_length    = frag.total_payload_length;
    m_slots[idx].expiry_us      = frag.expiry_time_us;
    m_slots[idx].open_time_us   = now_us;
    envelope_copy(m_slots[idx].header, frag);

    // Bug fix: zero the accumulation buffer so that any fragment slice shorter
    // than FRAG_MAX_PAYLOAD_BYTES does not expose stale bytes from a prior
    // message that occupied this slot.  assemble_and_free copies total_length
    // bytes out, which may span gaps never written by short payload fragments.
    (void)memset(m_slots[idx].buf, 0, sizeof(m_slots[idx].buf));

    NEVER_COMPILED_OUT_ASSERT(m_slots[idx].active);  // Assert: slot is now active
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::validate_fragment (private)
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::validate_fragment(uint32_t idx, const MessageEnvelope& frag) const
{
    NEVER_COMPILED_OUT_ASSERT(idx < REASSEMBLY_SLOT_COUNT);   // Assert: valid index
    NEVER_COMPILED_OUT_ASSERT(m_slots[idx].active);            // Assert: slot is active

    // fragment_count must match what we opened with
    if (frag.fragment_count != m_slots[idx].expected_count) {
        return Result::ERR_INVALID;
    }

    // total_payload_length must match
    if (frag.total_payload_length != m_slots[idx].total_length) {
        return Result::ERR_INVALID;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::record_fragment (private)
// G-2: changed from void to Result; assert-on-bounds replaced with defensive
// return ERR_INVALID so that a malformed fragment (e.g. crafted payload_length
// that passes validate_metadata but overruns the accumulation buffer via
// arithmetic) does not trigger abort() in a running server.
// Slot is NOT freed here; ingest_multifrag() frees it on ERR_INVALID.
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::record_fragment(uint32_t idx, const MessageEnvelope& frag)
{
    NEVER_COMPILED_OUT_ASSERT(idx < REASSEMBLY_SLOT_COUNT);          // Assert: valid index (programmer error)
    NEVER_COMPILED_OUT_ASSERT(frag.fragment_index < FRAG_MAX_COUNT); // Assert: validated by caller

    // Compute byte offset for this fragment's slice
    // Power of 10 Rule 2: constant; FRAG_MAX_PAYLOAD_BYTES is compile-time constant
    uint32_t byte_offset = static_cast<uint32_t>(frag.fragment_index) * FRAG_MAX_PAYLOAD_BYTES;

    // G-2: defensive bounds check — replaced NEVER_COMPILED_OUT_ASSERT to avoid
    // abort on a malformed fragment that reaches this path.
    if (byte_offset > MSG_MAX_PAYLOAD_BYTES) {
        Logger::log(Severity::WARNING_HI, "ReassemblyBuffer",
                    "record_fragment: byte_offset %u exceeds MSG_MAX_PAYLOAD_BYTES; dropping",
                    byte_offset);
        return Result::ERR_INVALID;
    }

    // Place payload into the accumulation buffer
    if (frag.payload_length > 0U) {
        // G-2: defensive overrun check — replaced assert with return.
        if (frag.payload_length > MSG_MAX_PAYLOAD_BYTES - byte_offset) {
            Logger::log(Severity::WARNING_HI, "ReassemblyBuffer",
                        "record_fragment: fragment overruns buffer "
                        "(offset=%u len=%u max=%u); dropping",
                        byte_offset, frag.payload_length, MSG_MAX_PAYLOAD_BYTES);
            return Result::ERR_INVALID;
        }
        (void)memcpy(&m_slots[idx].buf[byte_offset], frag.payload, frag.payload_length);
    }

    // F-6: accumulate received byte count for integrity check at assembly.
    // G-2: CERT INT30-C overflow guard — replaced assert with defensive return.
    if (frag.payload_length > MSG_MAX_PAYLOAD_BYTES - m_slots[idx].received_bytes) {
        Logger::log(Severity::WARNING_HI, "ReassemblyBuffer",
                    "record_fragment: received_bytes overflow for slot %u; dropping", idx);
        return Result::ERR_INVALID;
    }
    m_slots[idx].received_bytes += frag.payload_length;

    // Record this fragment in the bitmask
    m_slots[idx].received_mask |= (1U << frag.fragment_index);

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::is_complete (private)
// ─────────────────────────────────────────────────────────────────────────────
bool ReassemblyBuffer::is_complete(uint32_t idx) const
{
    NEVER_COMPILED_OUT_ASSERT(idx < REASSEMBLY_SLOT_COUNT);  // Assert: valid index
    NEVER_COMPILED_OUT_ASSERT(m_slots[idx].active);           // Assert: slot active

    // Build the expected full bitmask: bits 0..(expected_count-1) all set
    uint8_t cnt = m_slots[idx].expected_count;
    NEVER_COMPILED_OUT_ASSERT(cnt >= 1U && cnt <= static_cast<uint8_t>(FRAG_MAX_COUNT));  // Assert: valid count

    // (1U << cnt) - 1U produces the all-ones mask for cnt bits
    uint32_t expected_mask = (1U << static_cast<uint32_t>(cnt)) - 1U;

    return (m_slots[idx].received_mask & expected_mask) == expected_mask;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::assemble_and_free (private)
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::assemble_and_free(uint32_t idx, MessageEnvelope& logical_out)
{
    NEVER_COMPILED_OUT_ASSERT(idx < REASSEMBLY_SLOT_COUNT);  // Assert: valid index
    NEVER_COMPILED_OUT_ASSERT(m_slots[idx].active);           // Assert: slot active

    // F-6: integrity check — sum of received fragment bytes must equal the
    // wire-declared total_payload_length from the first fragment. A mismatch
    // means the sender inflated total_payload_length; discard the slot to avoid
    // handing a logically inconsistent envelope to the DeliveryEngine.
    if (m_slots[idx].received_bytes != static_cast<uint32_t>(m_slots[idx].total_length)) {
        Logger::log(Severity::WARNING_HI, "ReassemblyBuffer",
                    "assemble_and_free: received_bytes=%u != total_length=%u; "
                    "discarding (src=%u msg_id=%llu)",
                    m_slots[idx].received_bytes,
                    static_cast<uint32_t>(m_slots[idx].total_length),
                    static_cast<unsigned>(m_slots[idx].source_id),
                    static_cast<unsigned long long>(m_slots[idx].message_id));
        m_slots[idx].active         = false;
        m_slots[idx].received_mask  = 0U;
        m_slots[idx].expected_count = 0U;
        m_slots[idx].received_bytes = 0U;
        NEVER_COMPILED_OUT_ASSERT(!m_slots[idx].active);  // Assert: slot freed on mismatch
        return Result::ERR_INVALID;
    }

    // Start from the header envelope (has all metadata)
    envelope_copy(logical_out, m_slots[idx].header);

    // Overwrite payload fields with reassembled data
    logical_out.payload_length       = static_cast<uint32_t>(m_slots[idx].total_length);
    logical_out.fragment_index       = 0U;
    logical_out.fragment_count       = 1U;  // reassembled: appears as single message
    logical_out.total_payload_length = m_slots[idx].total_length;

    // Copy assembled payload
    if (logical_out.payload_length > 0U) {
        (void)memcpy(logical_out.payload, m_slots[idx].buf, logical_out.payload_length);
    }

    // Free the slot
    m_slots[idx].active         = false;
    m_slots[idx].received_mask  = 0U;
    m_slots[idx].expected_count = 0U;
    m_slots[idx].received_bytes = 0U;

    NEVER_COMPILED_OUT_ASSERT(!m_slots[idx].active);  // Assert: slot freed
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::validate_metadata (private)
// Bounds-checks fragment-level metadata fields.
// Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::validate_metadata(const MessageEnvelope& frag) const
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(frag.source_id != 0U);   // Assert: valid source

    // fragment_count == 0 is treated as 1 (unfragmented) for backward
    // compatibility with code that does not set fragment fields explicitly
    // (e.g. inject() paths that bypass serialization). The deserializer already
    // guards against fragment_count == 0 on the wire path.
    if (frag.fragment_index >= ((frag.fragment_count == 0U) ? 1U : frag.fragment_count)) {
        return Result::ERR_INVALID;
    }
    if (static_cast<uint32_t>(frag.fragment_count) > FRAG_MAX_COUNT) {
        return Result::ERR_INVALID;
    }
    if (static_cast<uint32_t>(frag.total_payload_length) > MSG_MAX_PAYLOAD_BYTES) {
        return Result::ERR_INVALID;
    }

    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::find_or_open_slot (private)
// Finds an existing reassembly slot or opens a new one for frag.
// Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::find_or_open_slot(const MessageEnvelope& frag, uint32_t& idx_out,
                                            uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(frag.source_id != 0U);   // Assert: valid source

    uint32_t idx = find_slot(frag.source_id, frag.message_id);

    if (idx == REASSEMBLY_SLOT_COUNT) {
        // New message: enforce per-source cap before allocating a slot (F-1).
        // A single peer may hold at most k_reasm_per_src_max (= REASSEMBLY_SLOT_COUNT/2)
        // open slots to prevent reassembly table exhaustion via partial fragment sets.
        if (count_open_slots_for_src(frag.source_id) >= k_reasm_per_src_max) {
            Logger::log(Severity::WARNING_HI, "ReassemblyBuffer",
                        "per-source slot cap reached for source=%u; dropping fragment",
                        static_cast<unsigned>(frag.source_id));
            return Result::ERR_FULL;
        }
        // Allocate a free slot.
        idx = find_free_slot();
        if (idx == REASSEMBLY_SLOT_COUNT) {
            return Result::ERR_FULL;
        }
        open_slot(idx, frag, now_us);
    } else {
        // Existing slot: validate consistency
        Result vr = validate_fragment(idx, frag);
        if (vr != Result::OK) {
            return vr;
        }
    }

    idx_out = idx;
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::ingest
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::ingest(const MessageEnvelope& frag, MessageEnvelope& logical_out,
                                 uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);          // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(frag.source_id != 0U);   // Assert: valid source

    // Validate fragment metadata bounds (CC-reduction: delegated to helper).
    Result meta_res = validate_metadata(frag);
    if (meta_res != Result::OK) {
        return meta_res;
    }

    // Fast path: single-fragment message (or legacy fragment_count == 0) completes immediately.
    // fragment_count == 0 is treated as unfragmented for backward compatibility.
    if (frag.fragment_count <= 1U) {
        envelope_copy(logical_out, frag);
        return Result::OK;
    }

    // Multi-fragment path — CC-reduction: delegated to ingest_multifrag().
    return ingest_multifrag(frag, logical_out, now_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::ingest_multifrag (private)
// Handles the multi-fragment reassembly path: slot allocation, bitmask
// duplicate detection, payload recording, and assembly on completion.
// Precondition: frag.fragment_count > 1 and metadata already validated.
// Extracted from ingest() to reduce its cognitive complexity to ≤ 10.
// ─────────────────────────────────────────────────────────────────────────────
Result ReassemblyBuffer::ingest_multifrag(const MessageEnvelope& frag,
                                           MessageEnvelope&       logical_out,
                                           uint64_t               now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);            // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(frag.fragment_count > 1U); // Assert: caller guarantees multi-frag

    // Find or open a reassembly slot (CC-reduction: delegated to helper).
    uint32_t idx = 0U;
    Result slot_res = find_or_open_slot(frag, idx, now_us);
    if (slot_res != Result::OK) {
        return slot_res;
    }

    // Check for duplicate fragment (already received this index).
    uint32_t bit = 1U << static_cast<uint32_t>(frag.fragment_index);
    if ((m_slots[idx].received_mask & bit) != 0U) {
        // Duplicate fragment: silently discard; wait for remaining.
        return Result::ERR_AGAIN;
    }

    // Record the fragment payload and update bitmask.
    // G-2: check return value; on ERR_INVALID free the slot and propagate.
    Result rec_r = record_fragment(idx, frag);
    if (rec_r != Result::OK) {
        m_slots[idx].active         = false;
        m_slots[idx].received_mask  = 0U;
        m_slots[idx].expected_count = 0U;
        m_slots[idx].received_bytes = 0U;
        NEVER_COMPILED_OUT_ASSERT(!m_slots[idx].active);  // Assert: slot freed on error
        return rec_r;
    }

    // Check if all fragments have arrived.
    if (!is_complete(idx)) {
        return Result::ERR_AGAIN;
    }

    // All fragments received: assemble into logical envelope and free slot.
    // F-6: assemble_and_free returns ERR_INVALID if received_bytes != total_length;
    // the slot is freed inside assemble_and_free in both success and error cases.
    Result asm_r = assemble_and_free(idx, logical_out);
    if (asm_r != Result::OK) {
        return asm_r;  // ERR_INVALID; caller must not use logical_out
    }

    NEVER_COMPILED_OUT_ASSERT(logical_out.payload_length <= MSG_MAX_PAYLOAD_BYTES);  // Assert: valid output
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::sweep_stale
// Frees slots that have been open longer than stale_threshold_us.
// Prevents slot exhaustion from peers that send only the first fragment.
// stale_threshold_us == 0 disables the sweep (returns 0 immediately).
// REQ-3.2.9: stale reassembly slot reclamation.
// NSC: housekeeping only.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ReassemblyBuffer::sweep_stale(uint64_t now_us, uint64_t stale_threshold_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);                          // Assert: buffer ready
    NEVER_COMPILED_OUT_ASSERT(REASSEMBLY_SLOT_COUNT > 0U);             // Assert: capacity valid

    if (stale_threshold_us == 0U) {
        return 0U;
    }

    uint32_t freed = 0U;
    // Power of 10 Rule 2: bounded by REASSEMBLY_SLOT_COUNT.
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (!m_slots[i].active) {
            continue;
        }
        if (now_us >= m_slots[i].open_time_us &&
            (now_us - m_slots[i].open_time_us) >= stale_threshold_us) {
            // Log stale slot expiry.
            Logger::log(Severity::WARNING_LO, "ReassemblyBuffer",
                        "stale slot freed: src=%u msg_id=%llu age_ms=%llu",
                        static_cast<unsigned>(m_slots[i].source_id),
                        static_cast<unsigned long long>(m_slots[i].message_id),
                        static_cast<unsigned long long>(
                            (now_us - m_slots[i].open_time_us) / 1000ULL));
            m_slots[i].active         = false;
            m_slots[i].received_mask  = 0U;
            m_slots[i].expected_count = 0U;
            m_slots[i].open_time_us   = 0U;
            ++freed;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(freed <= REASSEMBLY_SLOT_COUNT);  // Assert: cannot free more than capacity
    return freed;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReassemblyBuffer::sweep_expired
// ─────────────────────────────────────────────────────────────────────────────
void ReassemblyBuffer::sweep_expired(uint64_t now_us)
{
    NEVER_COMPILED_OUT_ASSERT(m_initialized);   // Assert: initialized
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL);   // Assert: valid timestamp

    // Power of 10 Rule 2: bounded loop
    for (uint32_t i = 0U; i < REASSEMBLY_SLOT_COUNT; ++i) {
        if (!m_slots[i].active) {
            continue;
        }
        // expiry_us == 0 means never-expires
        if (m_slots[i].expiry_us != 0ULL && m_slots[i].expiry_us <= now_us) {
            m_slots[i].active        = false;
            m_slots[i].received_mask = 0U;
            m_slots[i].expected_count = 0U;
        }
    }

    NEVER_COMPILED_OUT_ASSERT(REASSEMBLY_SLOT_COUNT > 0U);  // Assert: capacity invariant
}
