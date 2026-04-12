# UC_73 — Stale Reassembly Slot Reclamation: ReassemblyBuffer::sweep_stale() frees abandoned partial fragment sets

**HL Group:** HL-35: Stale Reassembly Slot Reclamation
**Actor:** System (internal — called periodically from `DeliveryEngine::sweep_ack_timeouts()`)
**Requirement traceability:** REQ-3.2.9

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::sweep_ack_timeouts()` calls
  `m_reassembly.sweep_stale(now_us, stale_threshold_us)` periodically. The threshold
  is derived from the transport's `recv_timeout_ms` configuration.
- **Goal:** Free `ReassemblyBuffer` slots that have been open longer than `stale_threshold_us`
  without completing, preventing slot exhaustion from peers that send only a partial
  fragment set (e.g., crash after sending fragment 0 of a 3-fragment message).
- **Success outcome:** All stale open slots freed; count of freed slots returned.
- **No-op outcome (disabled):** If `stale_threshold_us == 0`, the sweep is a no-op and
  returns 0 immediately.

---

## 2. Entry Points

```cpp
// src/core/ReassemblyBuffer.cpp
uint32_t ReassemblyBuffer::sweep_stale(uint64_t now_us, uint64_t stale_threshold_us);

// Caller: src/core/DeliveryEngine.cpp
// Called from DeliveryEngine::sweep_ack_timeouts()
```

---

## 3. End-to-End Control Flow

1. Application calls `DeliveryEngine::sweep_ack_timeouts(now_us)` (or the engine calls
   it internally from `handle_ordering_gate()`).
2. `sweep_ack_timeouts()` computes `stale_threshold_us` from `m_config.recv_timeout_ms * 1000`.
3. Calls `m_reassembly.sweep_stale(now_us, stale_threshold_us)`.
4. **`sweep_stale()`:**
   a. Asserts `m_initialized` and `REASSEMBLY_SLOT_COUNT > 0`.
   b. If `stale_threshold_us == 0` → return 0 (disabled).
   c. Bounded loop over all `REASSEMBLY_SLOT_COUNT` slots:
      - Skip inactive slots.
      - For active slots: compute `age_us = now_us - slot.open_time_us`.
      - If `age_us >= stale_threshold_us`:
        - Set `slot.active = false`; clear `slot.received_mask`, `slot.expected_count`,
          `slot.received_bytes`.
        - Log WARNING_LO: stale slot freed with `source_id`, `message_id`.
        - Increment `freed` counter.
   d. Returns `freed` (number of slots reclaimed).
5. `sweep_ack_timeouts()` logs the freed count if > 0.

---

## 4. Call Tree

```
DeliveryEngine::sweep_ack_timeouts()
 └── ReassemblyBuffer::sweep_stale(now_us, stale_threshold_us)
      └── [loop over REASSEMBLY_SLOT_COUNT slots]
           └── slot.active && age >= threshold -> slot.active = false
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `ReassemblyBuffer::sweep_stale()` | Reclaims slots open longer than `stale_threshold_us` |
| `ReassemblySlot.open_time_us` | Timestamp recorded when `open_slot()` allocates the slot |
| `stale_threshold_us` | Derived from `recv_timeout_ms`; 0 disables the sweep |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `stale_threshold_us == 0` | Sweep disabled; return 0 immediately |
| Slot inactive | Skip |
| `now_us - open_time_us < stale_threshold_us` | Not yet stale; skip |
| `now_us - open_time_us >= stale_threshold_us` | Stale: free slot; log WARNING_LO |

---

## 7. Concurrency / Threading Behavior

`sweep_stale()` is called from `sweep_ack_timeouts()` on the application thread.
`ReassemblyBuffer` is private to `DeliveryEngine`; no concurrent access.

---

## 8. Memory & Ownership Semantics

No heap allocation. Freeing a slot is a flag clear (`active = false`) and counter reset;
the `buf[]` contents are left in place (overwritten on next `open_slot()`).

---

## 9. Error Handling Flow

`sweep_stale()` returns a `uint32_t` count; it never fails. Stale-slot events are logged
at WARNING_LO (not FATAL) because stale slots indicate misbehaving peers, not local faults.

---

## 10. External Interactions

None. The sweep is entirely in-process.

---

## 11. State Changes / Side Effects

- Stale `ReassemblySlot.active` flags cleared.
- Freed slots immediately available for reuse by `find_free_slot()`.

---

## 12. Sequence Diagram

```
Application
  -> DeliveryEngine::sweep_ack_timeouts(now_us)
       -> ReassemblyBuffer::sweep_stale(now_us, threshold)
            -> [slot 2]: age=45s >= threshold=10s -> freed; log WARNING_LO
            -> [slot 5]: age=3s  <  threshold     -> skip
       <- freed=1
  <- sweep complete
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `init()` sets all `open_time_us` to 0 and `active = false`.
- **Runtime:** `sweep_stale()` is O(REASSEMBLY_SLOT_COUNT) on each call. Applications
  should call `sweep_ack_timeouts()` at a frequency appropriate to their reassembly timeout
  (e.g., once per second for a 10-second threshold).

---

## 14. Known Risks / Observations

- **Trade-off with legitimate slow senders:** A threshold that is too short frees slots from
  peers that are legitimately slow between fragments (e.g., due to network congestion). Choose
  `recv_timeout_ms` conservatively.
- **No event emitted:** Unlike `AckTracker` timeout events, stale-reassembly frees are not
  recorded in the `DeliveryEventRing`. They are logged at WARNING_LO only.

---

## 15. Unknowns / Assumptions

- `stale_threshold_us` source in `DeliveryEngine` is `m_config.recv_timeout_ms * 1000`.
  If `recv_timeout_ms == 0`, sweep is permanently disabled.
- `REASSEMBLY_SLOT_COUNT` is a compile-time constant in `src/core/Types.hpp`.
