# UC_71 — Ordered Message Delivery Gate: OrderingBuffer delivers DATA messages in sequence order

**HL Group:** HL-33: Ordered Message Delivery (Sequence Gate)
**Actor:** System (internal — called by `DeliveryEngine::handle_data_path()` via `handle_ordering_gate()`)
**Requirement traceability:** REQ-3.3.5

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::handle_data_path()` calls `handle_ordering_gate(logical, out, now_us)`
  for every fully-reassembled DATA message.
- **Goal:** Enforce per-peer in-order delivery for ORDERED DATA messages (those with
  `sequence_num > 0`). In-order messages are passed through immediately; out-of-order
  messages are buffered in a bounded hold array until the missing gap arrives; duplicate
  or already-delivered messages are silently discarded.
- **Success outcome (in-order):** `Result::OK`; `out` contains the delivered message.
- **Intermediate outcome (out-of-order):** `Result::ERR_AGAIN`; message held in `m_hold[]`; caller discards the frame and polls again.
- **Bypass (UNORDERED / control):** `sequence_num == 0` or message is a control type → passed through immediately.
- **Error outcome:** `Result::ERR_FULL` — hold buffer exhausted (all `ORDERING_HOLD_COUNT` slots occupied for this peer).

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::handle_ordering_gate(const MessageEnvelope& logical,
                                             MessageEnvelope&       out,
                                             uint64_t               now_us);

// src/core/OrderingBuffer.cpp
Result OrderingBuffer::ingest(const MessageEnvelope& msg,
                               MessageEnvelope&       out,
                               uint64_t               now_us);
Result OrderingBuffer::try_release_next(NodeId src, MessageEnvelope& out);
```

---

## 3. End-to-End Control Flow

**Delivery of an in-order message:**

1. `handle_ordering_gate()` first sweeps expired holds via
   `m_ordering.sweep_expired_holds(now_us, m_ordering_freed_buf, ORDERING_HOLD_COUNT)`.
   Freed slots are accounted as expiry drops.
2. `m_ordering.ingest(logical, out, now_us)`:
   a. Control message or `sequence_num == 0` → `envelope_copy(out, msg)` → `OK`.
   b. Ordered DATA: `get_or_create_peer(msg.source_id)` — find or allocate peer state entry.
   c. `msg.sequence_num == next_expected_for(src)` → `envelope_copy(out, msg)`, `advance_next_expected()` → `OK`.
3. `handle_ordering_gate()` returns `OK`.
4. `handle_data_path()` calls `deliver_held_pending()` after ingest returns OK to drain
   any backlog of contiguous held messages that are now in sequence.

**Hold of an out-of-order message:**

1. `ingest()`: `msg.sequence_num > next_expected_for(src)`.
2. `find_held(src, seq)` — check for duplicate hold (idempotent re-transmission guard).
3. If not already held: enforce per-peer hold cap (`k_hold_per_peer_max`).
4. `find_free_hold()` — allocate a hold slot.
5. `envelope_copy(m_hold[hold_idx].env, msg)` + `m_hold[hold_idx].active = true`.
6. Log INFO; return `ERR_AGAIN`.

**Discard of a duplicate (seq < next_expected):**

1. `ingest()`: `msg.sequence_num < next_expected_for(src)` → return `ERR_AGAIN` silently.

**Drain held messages after gap fills:**

1. After `ingest()` returns `OK` for the gap-filling message, `DeliveryEngine` calls
   `m_ordering.try_release_next(src, next_held)`:
   - Finds held message with `sequence_num == next_expected`; copies to `out`; frees hold slot;
     advances `next_expected`; returns `OK`.
   - Staged in `m_held_pending` for delivery on the next `receive()` call (Issue 1 fix).

---

## 4. Call Tree

```
DeliveryEngine::handle_data_path()
 └── handle_ordering_gate(logical, out)
      ├── OrderingBuffer::sweep_expired_holds()          [clears stale holds proactively]
      └── OrderingBuffer::ingest(msg, out)
           ├── envelope_copy(out, msg)                   [bypass: control or seq==0]
           ├── get_or_create_peer(src)                   [allocate/find peer state]
           ├── advance_next_expected(peer_idx)           [on in-order delivery]
           ├── find_held(src, seq)                       [dup-hold guard]
           ├── count_holds_for_peer(src)                 [per-peer cap]
           └── find_free_hold()                          [on out-of-order hold]
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `OrderingBuffer` | Per-peer sequence tracking + bounded hold buffer |
| `PeerState.next_expected_seq` | Next sequence number expected from this source |
| `HoldSlot.env` | Buffered out-of-order envelope |
| `get_or_create_peer()` | LRU-eviction peer table management |
| `try_release_next()` | Drain held messages once the gap fills |
| `m_held_pending` (DeliveryEngine) | Single-slot staging area: held message staged for next `receive()` call |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `envelope_is_control(msg)` | Bypass: copy to out; return OK |
| `msg.sequence_num == 0` | Bypass: UNORDERED; copy to out; return OK |
| `seq < next_expected` | Duplicate: silent ERR_AGAIN |
| `seq == next_expected` | In-order: deliver + advance; return OK |
| `seq > next_expected`, already held | Dup-hold guard: silent ERR_AGAIN |
| `seq > next_expected`, per-peer cap hit | ERR_FULL; WARNING_HI logged |
| `seq > next_expected`, hold slot free | Hold + ERR_AGAIN |
| Hold buffer full | ERR_FULL; WARNING_HI logged |

---

## 7. Concurrency / Threading Behavior

`OrderingBuffer` is private to `DeliveryEngine`; only `handle_ordering_gate()` and
`drain_hello_reconnects()` touch it. Both execute on the receive-polling thread; no locking needed.

---

## 8. Memory & Ownership Semantics

- `m_hold[ORDERING_HOLD_COUNT]` — fixed array in `OrderingBuffer`; no heap.
- `m_peers[ORDERING_PEER_COUNT]` — fixed array; LRU-eviction when all peer slots used.
- `m_held_pending` — single `MessageEnvelope` member of `DeliveryEngine`; written by
  `handle_ordering_gate()`, consumed by `deliver_held_pending()` on the next receive call.

---

## 9. Error Handling Flow

```
ERR_AGAIN (hold/dup)  → handle_data_path() returns ERR_AGAIN → receive() returns ERR_AGAIN → caller polls again
ERR_FULL              → handle_data_path() returns ERR_FULL → receive() returns ERR_FULL → caller handles
```

---

## 10. External Interactions

None. `OrderingBuffer` is purely in-process.

---

## 11. State Changes / Side Effects

- `PeerState.next_expected_seq` incremented on every in-order delivery or `try_release_next()`.
- `HoldSlot.active` toggled on hold/release.
- `m_held_pending_valid` set in `DeliveryEngine` when a next-in-sequence held message is staged.

---

## 12. Sequence Diagram

```
[Wire: seq=2 arrives before seq=1]

DeliveryEngine::receive()
  -> handle_ordering_gate(seq=2)
       -> OrderingBuffer::ingest()  -> ERR_AGAIN  [seq=2 held; next_expected=1]
  -> returns ERR_AGAIN

DeliveryEngine::receive()
  -> handle_ordering_gate(seq=1)   [gap-filling arrival]
       -> OrderingBuffer::ingest() -> OK  [seq=1 delivered; next_expected=2]
       -> try_release_next(src)    -> OK  [seq=2 released from hold; staged in m_held_pending]
  -> deliver seq=1 to application

DeliveryEngine::receive()          [next call]
  -> deliver_held_pending()        -> OK  [seq=2 delivered from m_held_pending]
  -> deliver seq=2 to application
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `DeliveryEngine::init()` calls `m_ordering.init(local_id)`, which zeros
  all `HoldSlot` and `PeerState` entries and sets `m_initialized = true`.
- **Runtime:** `handle_ordering_gate()` is called on every received DATA message. The
  `sweep_expired_holds()` call at its head ensures stale holds are released on every data-receive
  tick, even for BEST_EFFORT flows that skip `sweep_ack_timeouts()`.

---

## 14. Known Risks / Observations

- **Permanent stall risk:** If the gap-filling fragment is permanently lost, the hold slot
  holds indefinitely until `sweep_expired_holds()` evicts it on expiry. Without periodic
  `receive()` calls the sweep never fires. Applications using ORDERED channels should call
  `receive()` (or `sweep_ack_timeouts()`) regularly to prevent stall.
- **Per-peer cap:** `k_hold_per_peer_max` prevents a single misbehaving source from
  exhausting all hold slots. On cap violation `ERR_FULL` is returned and WARNING_HI logged;
  the out-of-order message is dropped.

---

## 15. Unknowns / Assumptions

- `ORDERING_HOLD_COUNT` (hold slots) and `ORDERING_PEER_COUNT` (peer table size) are
  compile-time constants in `src/core/Types.hpp`.
- `sequence_num = 0` is the reserved UNORDERED sentinel; senders must start their
  per-session sequence at 1.
