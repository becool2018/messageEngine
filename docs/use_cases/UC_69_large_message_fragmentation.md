# UC_69 — Large Message Fragmentation: DeliveryEngine::send_fragments() splits payload into wire fragments

**HL Group:** HL-32: Large Message Fragmentation and Reassembly
**Actor:** System (internal — triggered by `DeliveryEngine::send()` when payload exceeds FRAG_MAX_PAYLOAD_BYTES)
**Requirement traceability:** REQ-3.2.3, REQ-3.3.5

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::send()` calls `send_fragments(env, now_us)` for every outbound
  logical envelope. If `env.payload_length > FRAG_MAX_PAYLOAD_BYTES`, `send_fragments` invokes
  `fragment_message()` to split the payload into up to `FRAG_MAX_COUNT` wire fragments and
  transmits each one individually via `send_via_transport()`.
- **Goal:** Transparently fragment an oversized logical message into bounded wire frames so the
  transport never sees a payload larger than `FRAG_MAX_PAYLOAD_BYTES`.
- **Success outcome:** All fragments transmitted; `Result::OK` returned to `send()`.
- **Error outcomes:**
  - `Result::ERR_INVALID` — `fragment_message()` returns 0 (payload too large or `out_cap` too small); logged at WARNING_HI.
  - `Result::ERR_IO` — the very first fragment failed to send; nothing reached the wire; rollback in `send()` is safe.
  - `Result::ERR_IO_PARTIAL` — at least one fragment was sent before a failure; rollback is skipped because partial transmission cannot be recalled.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp — called by send() and pump_retries()
Result DeliveryEngine::send_fragments(const MessageEnvelope& env, uint64_t now_us);

// src/core/Fragmentation.cpp — free function called by send_fragments()
uint32_t fragment_message(const MessageEnvelope& logical,
                          MessageEnvelope*       out_frags,
                          uint32_t               out_cap);
```

`send_fragments()` is also called from `pump_retries()` when re-transmitting a reliable-retry
message that was originally fragmented.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::send(env, now_us)` serialises, assigns message_id, etc., then calls
   `send_fragments(env, now_us)`.
2. `send_fragments()` asserts `m_initialized` and `envelope_valid(env)`.
3. **Fast path:** `needs_fragmentation(env)` returns `false` (payload ≤ `FRAG_MAX_PAYLOAD_BYTES`)
   → calls `send_via_transport(env, now_us)` directly and returns its result.
4. **Fragmentation path:** `needs_fragmentation(env)` returns `true`.
5. `fragment_message(env, m_frag_buf, FRAG_MAX_COUNT)` is called:
   - Computes `N = ceil(payload_length / FRAG_MAX_PAYLOAD_BYTES)`.
   - Fills `m_frag_buf[0..N-1]`, each carrying: same `message_id`, `source_id`,
     `destination_id`, `reliability_class`, `priority`, `expiry_time_us`, `sequence_num`,
     `timestamp_us`; plus `fragment_index`, `fragment_count = N`,
     `total_payload_length`, and its slice of the logical payload.
   - Returns `N` (≥ 1) on success; 0 on error.
6. If `fragment_message()` returns 0 → log WARNING_HI; return `ERR_INVALID`.
7. Asserts `frag_count <= FRAG_MAX_COUNT`.
8. **Fragment send loop** (bounded by `frag_count ≤ FRAG_MAX_COUNT`):
   - For each `i` in `[0, frag_count)`:
     - `send_via_transport(m_frag_buf[i], now_us)`.
     - On failure: log WARNING_HI with fragment index; return `ERR_IO_PARTIAL` if any earlier
       fragment succeeded, or `ERR_IO` if this is the first.
     - On success: set `any_sent = true`.
9. Returns `Result::OK` after all fragments are sent.

---

## 4. Call Tree

```
DeliveryEngine::send()
 └── DeliveryEngine::send_fragments()
      ├── needs_fragmentation()           [Fragmentation.hpp — predicate]
      ├── fragment_message()              [Fragmentation.cpp — free function]
      │    └── (fills m_frag_buf[0..N-1] with payload slices)
      └── send_via_transport() × N       [calls m_transport->send_message()]
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `DeliveryEngine::send_fragments()` | Orchestrates fragmentation and per-fragment transmission |
| `fragment_message()` (`Fragmentation.cpp`) | Stateless splitter: slices payload into fixed-size fragments, copies metadata |
| `m_frag_buf[FRAG_MAX_COUNT]` | Pre-allocated static array of `MessageEnvelope` in `DeliveryEngine` — no heap |
| `send_via_transport()` | Calls `m_transport->send_message()` with the impairment engine in the path |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `!needs_fragmentation(env)` | Single-frame path: `send_via_transport(env)` directly |
| `fragment_message()` returns 0 | ERR_INVALID logged at WARNING_HI; returned to `send()` |
| First fragment send fails | `ERR_IO` returned (nothing on wire; rollback safe) |
| Fragment `i > 0` send fails | `ERR_IO_PARTIAL` returned (partial; rollback skipped) |
| All fragments succeed | `Result::OK` |

---

## 7. Concurrency / Threading Behavior

`send_fragments()` is called on the caller's thread (same as `send()`). The static
`m_frag_buf` array is owned by the `DeliveryEngine` instance; concurrent sends from
multiple threads to the same `DeliveryEngine` are not supported (single-caller model,
matching the overall `DeliveryEngine` threading contract).

---

## 8. Memory & Ownership Semantics

- `m_frag_buf[FRAG_MAX_COUNT]` is a `MessageEnvelope` array declared as a member of
  `DeliveryEngine` — stack/BSS allocated at construction, no heap (Power of 10 Rule 3).
- `fragment_message()` is stateless: it reads `logical` and writes `out_frags`; no
  allocation, no retained state.
- Each `m_frag_buf[i]` lifetime: valid only during the `send_fragments()` call; overwritten
  on the next call.

---

## 9. Error Handling Flow

```
fragment_message() == 0    → ERR_INVALID (log WARNING_HI) → returned to send()
first fragment ERR_IO      → ERR_IO → send() calls rollback_on_transport_failure()
later fragment ERR_IO      → ERR_IO_PARTIAL → send() skips rollback
```

---

## 10. External Interactions

- `m_transport->send_message()` (via `send_via_transport()`) — the transport backend
  (TcpBackend, TlsTcpBackend, UdpBackend, DtlsUdpBackend, or LocalSimHarness) serialises
  each fragment and transmits it on the wire.

---

## 11. State Changes / Side Effects

- `m_frag_buf[]` is overwritten on each call.
- No `DeliveryEngine` counters are incremented here; `send()` handles stats accounting.
- Transport-level send counters are incremented once per fragment by the backend.

---

## 12. Sequence Diagram

```
DeliveryEngine::send()
  -> send_fragments(env)
       -> needs_fragmentation(env)         [true if payload > FRAG_MAX_PAYLOAD_BYTES]
       -> fragment_message(env, m_frag_buf) [fills N fragment envelopes]
       -> send_via_transport(m_frag_buf[0]) [fragment 0 → transport → wire]
       -> send_via_transport(m_frag_buf[1]) [fragment 1 → transport → wire]
       -> ...
       -> send_via_transport(m_frag_buf[N-1])
  <- Result::OK
DeliveryEngine::send()
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `m_frag_buf` is zero-initialized at `DeliveryEngine` construction.
  No per-call init needed.
- **Runtime:** Called on every `send()` and every `pump_retries()` retry. For the vast
  majority of messages (payload ≤ `FRAG_MAX_PAYLOAD_BYTES`) the fast path is taken and
  `fragment_message()` is never invoked.

---

## 14. Known Risks / Observations

- **Partial delivery is unrecoverable:** Once `ERR_IO_PARTIAL` is returned, fragments
  already on the wire cannot be recalled. The receiver will eventually expire the partial
  reassembly slot via `sweep_stale()` (UC_73). The sender's retry logic (if RELIABLE_RETRY)
  will re-fragment and retransmit the entire message — the receiver's bitmask dedup inside
  `ReassemblyBuffer` will silently drop duplicated fragments.
- **Re-fragmentation on retry:** `pump_retries()` calls `send_fragments()` with the full
  logical `m_retry_buf[i]` envelope. Re-fragmentation produces identical fragment boundaries
  for the same payload, so the receiver's slot (if still open) will continue collecting.

---

## 15. Unknowns / Assumptions

- `FRAG_MAX_COUNT` and `FRAG_MAX_PAYLOAD_BYTES` are compile-time constants in `src/core/Types.hpp`.
- `fragment_message()` guarantees identical fragment boundaries for the same input — no
  randomisation or ordering within the fragment array.
