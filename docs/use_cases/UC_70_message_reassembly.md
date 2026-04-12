# UC_70 — Message Reassembly: ReassemblyBuffer collects wire fragments and returns the logical message

**HL Group:** HL-32: Large Message Fragmentation and Reassembly
**Actor:** System (internal — called by `DeliveryEngine::receive()` via `handle_fragment_ingest()`)
**Requirement traceability:** REQ-3.2.3, REQ-3.2.9, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` calls `handle_fragment_ingest(wire_env, logical_out, now_us)`
  for every wire frame pulled from the transport ring. `handle_fragment_ingest()` forwards the
  fragment to `ReassemblyBuffer::ingest()`.
- **Goal:** Reconstruct a fragmented logical `MessageEnvelope` from its constituent wire
  fragments, keyed by `(source_id, message_id)`. Return `Result::OK` with `logical_out`
  filled only when all `fragment_count` fragments have arrived.
- **Success outcome:** All fragments received; `logical_out` contains the reassembled envelope
  with the full logical payload; slot freed; `Result::OK` returned.
- **Intermediate outcome:** `Result::ERR_AGAIN` — more fragments still needed; caller discards
  the wire frame and polls again.
- **Error outcomes:**
  - `Result::ERR_FULL` — no reassembly slot available (all `REASSEMBLY_SLOT_COUNT` slots occupied).
  - `Result::ERR_INVALID` — fragment metadata inconsistent (different `fragment_count` or
    `total_payload_length` vs prior fragment of same message; out-of-range values; assembled
    byte count mismatch).

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::handle_fragment_ingest(const MessageEnvelope& wire_env,
                                               MessageEnvelope&       logical_out,
                                               uint64_t               now_us);

// src/core/ReassemblyBuffer.cpp (called by handle_fragment_ingest)
Result ReassemblyBuffer::ingest(const MessageEnvelope& frag,
                                 MessageEnvelope&       logical_out,
                                 uint64_t               now_us);
```

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::receive()` pops a `wire_env` from the transport ring.
2. Calls `handle_fragment_ingest(wire_env, logical_out, now_us)`.
3. `handle_fragment_ingest()` asserts `m_initialized` and `envelope_valid(wire_env)`, then
   calls `m_reassembly.ingest(wire_env, logical_out, now_us)`.
4. **`ReassemblyBuffer::ingest()`:**
   a. Asserts `m_initialized` and `frag.source_id != 0`.
   b. `validate_metadata(frag)` — checks `fragment_index < fragment_count`, `fragment_count ≤ FRAG_MAX_COUNT`, `total_payload_length ≤ MSG_MAX_PAYLOAD_BYTES`. Returns `ERR_INVALID` on violation.
   c. **Single-fragment fast path:** `fragment_count ≤ 1` → `envelope_copy(logical_out, frag)` → return `OK`.
   d. **Multi-fragment path:** calls `ingest_multifrag(frag, logical_out, now_us)`.
5. **`ingest_multifrag()`:**
   a. `find_or_open_slot(frag, idx, now_us)`:
      - Searches existing active slots by `(source_id, message_id)`.
      - If not found: checks per-source slot cap (`k_reasm_per_src_max`); `find_free_slot()`; calls `open_slot(idx, frag, now_us)` to initialise from first fragment.
      - Returns `ERR_FULL` if no free slot.
      - Returns `ERR_INVALID` if existing slot has inconsistent metadata (`validate_fragment()`).
   b. **Bitmask duplicate check:** if `received_mask & (1 << fragment_index)` → duplicate; return `ERR_AGAIN`.
   c. `record_fragment(idx, frag)` — copies payload slice into `slot.buf` at correct offset; updates `received_mask` and `received_bytes`. Returns `ERR_INVALID` if offset + length would exceed `MSG_MAX_PAYLOAD_BYTES`; frees slot on error.
   d. `is_complete(idx)` — `received_mask == (1 << fragment_count) - 1`? If not → return `ERR_AGAIN`.
   e. `assemble_and_free(idx, logical_out)`:
      - Copies slot header metadata into `logical_out`.
      - Sets `logical_out.payload_length = total_length`.
      - `memcpy` assembled `buf[0..total_length-1]` → `logical_out.payload`.
      - Validates `received_bytes == total_length` (F-6 guard); returns `ERR_INVALID` if mismatch.
      - Clears `slot.active = false`.
      - Returns `OK`.
6. `handle_fragment_ingest()` logs WARNING_LO on non-OK, non-ERR_AGAIN results.
7. Returns result to `DeliveryEngine::receive()`.
8. On `ERR_AGAIN` or error: `receive()` returns without delivering an envelope.
9. On `OK`: `receive()` continues with `logical_out` through routing, dedup, and ordering.

---

## 4. Call Tree

```
DeliveryEngine::receive()
 └── handle_fragment_ingest(wire_env, logical_out)
      └── ReassemblyBuffer::ingest(frag, logical_out, now_us)
           ├── validate_metadata(frag)
           ├── [fast path] envelope_copy(logical_out, frag)    [fragment_count <= 1]
           └── ingest_multifrag(frag, logical_out, now_us)
                ├── find_or_open_slot(frag, idx, now_us)
                │    ├── find_slot(source_id, message_id)
                │    ├── find_free_slot()
                │    ├── count_open_slots_for_src(src)
                │    ├── open_slot(idx, frag, now_us)
                │    └── validate_fragment(idx, frag)
                ├── [bitmask dup check]
                ├── record_fragment(idx, frag)
                ├── is_complete(idx)
                └── assemble_and_free(idx, logical_out)
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `DeliveryEngine::handle_fragment_ingest()` | Wrapper: delegates to `ReassemblyBuffer`; logs non-OK errors |
| `ReassemblyBuffer` | Maintains `REASSEMBLY_SLOT_COUNT` fixed slots; collects fragments by `(source_id, message_id)` key |
| `ReassemblySlot.received_mask` | Bitmask of received fragment indices (up to 32 fragments per message) |
| `ReassemblySlot.buf[]` | Pre-allocated payload accumulator (`MSG_MAX_PAYLOAD_BYTES`) per slot |
| `assemble_and_free()` | Copies accumulated buf → logical_out and releases the slot |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `fragment_count ≤ 1` | Fast path: copy frag to logical_out; return OK immediately |
| `validate_metadata()` fails | ERR_INVALID; slot not allocated |
| No free slot | ERR_FULL; frame dropped |
| Existing slot metadata mismatch | ERR_INVALID; slot not freed (prior fragments still collecting) |
| Duplicate fragment (bit already set) | ERR_AGAIN; silently discarded |
| `record_fragment()` offset overflow | ERR_INVALID; slot freed |
| Not all fragments received yet | ERR_AGAIN |
| All fragments received, byte count mismatch | ERR_INVALID; slot freed |
| All fragments received, bytes match | OK; logical_out filled; slot freed |

---

## 7. Concurrency / Threading Behavior

`ReassemblyBuffer` is accessed only from `DeliveryEngine::receive()`, which is called
on the application thread (or the receive-polling thread). No locking is required because
`ReassemblyBuffer` is private to `DeliveryEngine` and not accessed concurrently.

---

## 8. Memory & Ownership Semantics

- `m_slots[REASSEMBLY_SLOT_COUNT]` — fixed array in `ReassemblyBuffer`; no heap (Power of 10 Rule 3).
- Each `ReassemblySlot.buf[MSG_MAX_PAYLOAD_BYTES]` — zero-inited on `open_slot()`; no lifetime hazard.
- `logical_out` — caller-supplied; written only on `Result::OK`; ownership stays with caller.

---

## 9. Error Handling Flow

```
validate_metadata() fail  → ERR_INVALID (no slot allocated)
find_or_open_slot() fail  → ERR_FULL or ERR_INVALID
record_fragment() fail    → ERR_INVALID (slot freed)
assemble_and_free() fail  → ERR_INVALID (slot freed)
not complete yet          → ERR_AGAIN (slot remains active)
```

---

## 10. External Interactions

None. `ReassemblyBuffer` is purely in-process; it manipulates the fixed slot array only.

---

## 11. State Changes / Side Effects

- Slot `active` flag transitions: `false → true` on first fragment; `true → false` on completion or error.
- `received_mask` updated on each fragment.
- `received_bytes` accumulated per fragment.
- Completed slot is freed and available immediately for reuse.

---

## 12. Sequence Diagram

```
DeliveryEngine::receive()
  -> handle_fragment_ingest(wire_frag_0)
       -> ReassemblyBuffer::ingest()  -> ERR_AGAIN  [slot opened, fragment 0 recorded]
  -> returns ERR_AGAIN (caller polls again)

DeliveryEngine::receive()
  -> handle_fragment_ingest(wire_frag_1)
       -> ReassemblyBuffer::ingest()  -> OK          [all frags received; logical_out filled; slot freed]
  -> routing, dedup, ordering gate, deliver to app
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `DeliveryEngine::init()` calls `m_reassembly.init()`, which sets
  `m_initialized = true` and zero-inits all slots.
- **Runtime:** `ingest()` is called on every wire frame received. For non-fragmented messages
  the fast path (`fragment_count ≤ 1`) completes in O(1). For fragmented messages each call
  is O(REASSEMBLY_SLOT_COUNT) for the slot-search helpers.

---

## 14. Known Risks / Observations

- **Slot exhaustion:** A peer that sends only the first fragment of a large message can occupy
  a slot indefinitely, starving other peers. Mitigated by: (a) per-source slot cap
  (`k_reasm_per_src_max`); (b) `sweep_expired()` reclaims slots whose `expiry_us` has passed;
  (c) `sweep_stale()` (UC_73, REQ-3.2.9) reclaims slots open longer than `recv_timeout_ms`.
- **Bitmask limit:** `received_mask` is `uint32_t`, so `FRAG_MAX_COUNT ≤ 32`. Payloads
  requiring more than 32 fragments are rejected by `validate_metadata()`.

---

## 15. Unknowns / Assumptions

- `REASSEMBLY_SLOT_COUNT`, `FRAG_MAX_COUNT`, and `MSG_MAX_PAYLOAD_BYTES` are compile-time
  constants in `src/core/Types.hpp`.
- `k_reasm_per_src_max` is a private constant inside `ReassemblyBuffer.cpp`; its exact value
  can be read from the source.
