# UC_08 — ACK received: AckTracker slot transitions from PENDING → ACKED and is freed

**HL Group:** HL-2 — Send with Acknowledgement
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` dequeues an envelope with `message_type == MessageType::ACK` and calls `m_ack_tracker.on_ack(message_id)`. File: `src/core/AckTracker.cpp`.
- **Goal:** Locate the `PENDING` slot in `AckTracker` matching the received `message_id`, transition its state to `ACKED`, and immediately free the slot back to `FREE` (so it can be reused).
- **Success outcome:** The matching slot is found; state transitions `PENDING → ACKED → FREE`. `on_ack()` returns `Result::OK`. `receive()` also returns `Result::OK` (ACK is a control message; it is passed through to the caller, not suppressed).
- **Error outcomes:**
  - `Result::ERR_NOT_FOUND` from `on_ack()` — no `PENDING` slot matches `message_id`. Logged at `WARNING_LO`; `receive()` still returns `OK` (unexpected ACK is non-fatal).

---

## 2. Entry Points

```cpp
// src/core/AckTracker.cpp (called from DeliveryEngine::receive())
Result AckTracker::on_ack(uint64_t message_id);
```

Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::receive()` pops an `ACK` envelope from the transport ring buffer.
2. `envelope_is_control(raw)` — true (ACK is a control message).
3. **Branch:** `raw.message_type == MessageType::ACK` — true.
4. **`m_ack_tracker.on_ack(raw.message_id)`** is called (`AckTracker.cpp`).
5. Inside `AckTracker::on_ack(message_id)`:
   a. `NEVER_COMPILED_OUT_ASSERT(message_id != 0)`.
   b. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]`:
      - For each entry: if `entry.state == PENDING && entry.message_id == message_id`:
        - `entry.state = ACKED`.
        - `entry.state = FREE` (immediately freed; ACKED is transitional).
        - Returns `Result::OK`.
   c. If no match: returns `Result::ERR_NOT_FOUND`.
6. If `on_ack()` returns `ERR_NOT_FOUND`: `Logger::log(WARNING_LO, ...)` ("unexpected ACK for unknown message_id").
7. **`m_retry_mgr.on_ack(raw.message_id)`** is also called (`RetryManager.cpp`) to cancel any pending retry for the same message_id:
   a. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]`.
   b. If found with `active == true && entry.env.message_id == message_id`: `entry.active = false`.
8. `receive()` copies `raw` to `out` (the caller's envelope) and returns `Result::OK`. The ACK is delivered to the caller as a control message.

---

## 4. Call Tree

```
DeliveryEngine::receive()                       [DeliveryEngine.cpp]
 ├── [pop from RingBuffer]
 ├── envelope_is_control()                      [MessageEnvelope.hpp]
 ├── AckTracker::on_ack(message_id)             [AckTracker.cpp]
 │    └── [linear scan; PENDING -> ACKED -> FREE]
 └── RetryManager::on_ack(message_id)           [RetryManager.cpp]
      └── [linear scan; set active=false]
```

---

## 5. Key Components Involved

- **`AckTracker::on_ack()`** — Frees the PENDING slot registered by `track()` during the original `send()`. This closes the acknowledgement lifecycle.
- **`RetryManager::on_ack()`** — Cancels any active retry entry for the same message_id. Required for RELIABLE_RETRY; idempotent for RELIABLE_ACK (no retry entry exists).
- **`DeliveryEngine::receive()`** — The orchestrator; both `on_ack()` calls happen here so the ACK is fully processed before being passed to the User.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `message_type == ACK` | Call `on_ack()` | Pass through as other control message |
| Matching PENDING slot found | `PENDING -> ACKED -> FREE`; return OK | Return ERR_NOT_FOUND |
| `on_ack()` returns ERR_NOT_FOUND | Log WARNING_LO; continue | Continue normally |
| Matching active retry entry found | `active = false` (cancelled) | No action |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread.
- `AckTracker::m_entries` and `RetryManager::m_entries` are plain fixed arrays; no atomics.
- `send()` and `receive()` must be called from the same thread to avoid data races on these tables.

---

## 8. Memory & Ownership Semantics

- `AckTracker::m_entries[ACK_TRACKER_CAPACITY]` — 32-slot fixed array; freed slot is zeroed or marked FREE (no heap free needed).
- `RetryManager::m_entries[ACK_TRACKER_CAPACITY]` — 32-slot fixed array; `active = false` marks the slot as reusable.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **`ERR_NOT_FOUND`:** Stale or unexpected ACK. Logged; `receive()` still returns `OK`. This can occur if the ACK arrives after the AckTracker deadline has already swept the slot, or for a message sent without `RELIABLE_ACK` semantics.
- All errors are non-fatal; `receive()` always completes with some result.

---

## 10. External Interactions

- None — this flow operates entirely in process memory.
- `Logger::log()` writes to `stderr` on `ERR_NOT_FOUND`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `AckTracker` | `m_entries[slot].state` | `PENDING` | `FREE` |
| `AckTracker` | `m_entries[slot].message_id` | assigned ID | unchanged (logically stale) |
| `RetryManager` | `m_entries[slot].active` | `true` (if retry exists) | `false` |

---

## 12. Sequence Diagram

```
[Transport delivers ACK envelope to DeliveryEngine::receive()]
DeliveryEngine::receive()
  -> envelope_is_control()       <- true
  -> [message_type == ACK]       <- true
  -> AckTracker::on_ack(message_id)
       [scan; find PENDING slot; PENDING -> ACKED -> FREE]
       <- Result::OK
  -> RetryManager::on_ack(message_id)
       [scan; find active retry; set active=false]
  -> [copy raw to out]
  <- Result::OK   [ACK delivered to caller as control envelope]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `AckTracker::init()` called; all slots in `FREE` state.
- Prior `DeliveryEngine::send()` with `RELIABLE_ACK` or `RELIABLE_RETRY` allocated the slot now being freed.

**Runtime:**
- Each ACK arrival frees exactly one slot (or zero if unexpected). The freed slot is immediately available for the next `track()` call.

---

## 14. Known Risks / Observations

- **ACK delivered to caller:** The caller receives the ACK envelope via `receive()` return value. The caller must check `message_type == ACK` and handle it appropriately (e.g., not treat it as a DATA envelope).
- **ACKED state is transitional:** The code transitions `PENDING -> ACKED` and then immediately `ACKED -> FREE` in the same function call. The ACKED state has no observable duration; it exists only as a coding step.
- **Duplicate ACK:** If two ACKs arrive for the same `message_id`, the first frees the slot; the second finds no PENDING slot and logs `ERR_NOT_FOUND` (non-fatal, expected behavior).

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The ACK `message_id` must match the `message_id` assigned by `MessageIdGen::next()` during the original `send()` call. The peer must copy the `message_id` from the DATA envelope into the ACK envelope verbatim.
- `[ASSUMPTION]` `RetryManager::on_ack()` is called unconditionally even for `RELIABLE_ACK` sends (where no retry entry exists). This is a safe no-op: the linear scan finds no active entry and returns without side effects.
