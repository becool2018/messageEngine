# UC_74 — Ordering Gap Sweep: OrderingBuffer::sweep_expired_holds() evicts stale held messages

**HL Group:** HL-33: Ordered Message Delivery (Sequence Gate) — System Internals sub-function
**Actor:** System (internal — called by `handle_ordering_gate()` on every data-receive call)
**Requirement traceability:** REQ-3.3.5

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::handle_ordering_gate()` calls
  `m_ordering.sweep_expired_holds(now_us, m_ordering_freed_buf, ORDERING_HOLD_COUNT)` before
  calling `OrderingBuffer::ingest()`. This proactive sweep ensures that a permanently-lost
  gap does not stall the ordering gate indefinitely for BEST_EFFORT ordered channels.
- **Goal:** Evict hold slots whose `expiry_time_us` has passed, advance `next_expected_seq`
  past the evicted slot's sequence number, and return the freed envelopes to the caller
  for expiry-drop accounting and event emission.
- **Success outcome:** All expired hold slots freed; `next_expected_seq` advanced past each
  freed sequence; returned count > 0 if any slots were freed.
- **No-op outcome:** No expired holds; returns 0.

---

## 2. Entry Points

```cpp
// src/core/OrderingBuffer.cpp — SC: HAZ-001
uint32_t OrderingBuffer::sweep_expired_holds(uint64_t         now_us,
                                              MessageEnvelope* out_freed,
                                              uint32_t         out_cap);
```

Called from `DeliveryEngine::handle_ordering_gate()` via the private member
`m_ordering_freed_buf[ORDERING_HOLD_COUNT]`.

---

## 3. End-to-End Control Flow

1. `handle_ordering_gate()` calls
   `m_ordering.sweep_expired_holds(now_us, m_ordering_freed_buf, ORDERING_HOLD_COUNT)`.
2. **`sweep_expired_holds()`:**
   a. Asserts `m_initialized`, `now_us > 0`, `out_freed != nullptr`, `out_cap > 0`.
   b. Bounded loop over all `ORDERING_HOLD_COUNT` hold slots:
      - Skip inactive slots.
      - For active slots: check `slot.env.expiry_time_us <= now_us`.
      - If expired:
        - `envelope_copy(out_freed[freed_count], slot.env)` — capture for accounting.
        - `advance_sequence(slot.env.source_id, slot.env.sequence_num + 1)` — skip the
          stale sequence to unblock subsequent messages.
        - `slot.active = false`.
        - Increment `freed_count`; check `freed_count < out_cap` guard.
   c. Returns `freed_count`.
3. `handle_ordering_gate()` calls
   `account_ordering_expiry_drops(m_ordering_freed_buf, gate_freed, now_us)` to emit
   `EXPIRY_DROP` events and increment `m_stats.msgs_dropped_expired`.

---

## 4. Call Tree

```
DeliveryEngine::handle_ordering_gate()
 └── OrderingBuffer::sweep_expired_holds(now_us, out_freed, out_cap)  [SC: HAZ-001]
      └── [loop ≤ ORDERING_HOLD_COUNT]
           └── slot expired -> advance_sequence(src, seq+1) + slot.active=false
 └── account_ordering_expiry_drops(out_freed, count, now_us)
      └── emit_event(EXPIRY_DROP, ...) per freed envelope
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `sweep_expired_holds()` | Evicts expired hold slots; advances sequence to unblock downstream |
| `advance_sequence(src, up_to_seq)` | Sets `next_expected_seq` to `up_to_seq` for the peer, skipping the lost gap |
| `m_ordering_freed_buf[]` | Caller-supplied buffer; receives copies of freed envelopes for accounting |
| `account_ordering_expiry_drops()` | Increments stats and emits `EXPIRY_DROP` events |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| Hold slot inactive | Skip |
| `expiry_time_us > now_us` | Not expired; skip |
| `expiry_time_us <= now_us` | Expired: copy to out_freed, advance_sequence, free slot |
| `freed_count >= out_cap` | Guard: no more room in out_freed; stop early |

---

## 7. Concurrency / Threading Behavior

Called exclusively from `handle_ordering_gate()` on the receive thread. No locking needed.

---

## 8. Memory & Ownership Semantics

`m_ordering_freed_buf[ORDERING_HOLD_COUNT]` is a `MessageEnvelope` array member of
`DeliveryEngine` — pre-allocated, no heap. Written by `sweep_expired_holds()`, consumed by
`account_ordering_expiry_drops()` in the same `handle_ordering_gate()` call.

---

## 9. Error Handling Flow

No error codes. If `out_cap` is exhausted before all expired slots are swept, the function
stops early — remaining expired slots will be swept on the next call.

---

## 10. External Interactions

None. In-process only.

---

## 11. State Changes / Side Effects

- Expired `HoldSlot.active` flags cleared.
- `PeerState.next_expected_seq` advanced past each evicted sequence.
- `m_stats.msgs_dropped_expired` incremented per evicted slot.
- `EXPIRY_DROP` events emitted to `DeliveryEventRing`.

---

## 12. Sequence Diagram

```
handle_ordering_gate(logical, out)
  -> sweep_expired_holds(now_us, freed_buf, cap)
       -> hold[3]: seq=7, expiry=t-5s -> advance_sequence(src, 8); active=false
       <- freed=1
  -> account_ordering_expiry_drops(freed_buf, 1, now_us)
       -> emit_event(EXPIRY_DROP, msg_id, src, now_us)
  -> OrderingBuffer::ingest(logical, out)   [now with next_expected=8, gap unblocked]
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** All hold slots are inactive at `init()`; sweep is a no-op.
- **Runtime:** Called on every DATA receive to proactively evict expired holds. Bounded
  by `ORDERING_HOLD_COUNT` (8 as of the current constants) — O(1) overhead per receive.

---

## 14. Known Risks / Observations

- **Aggressive sweep may skip valid messages:** If `expiry_time_us` is set too short,
  a message held for a minor network delay may be evicted before the gap arrives.
  Senders on ORDERED channels should set `expiry_time_us` generously.

---

## 15. Unknowns / Assumptions

- `ORDERING_HOLD_COUNT` is a compile-time constant in `src/core/Types.hpp`.
- `advance_sequence()` handles `uint32_t` wraparound (sequence 0xFFFFFFFF → 1) with a
  WARNING_HI log per `seq_next_guarded()`.
