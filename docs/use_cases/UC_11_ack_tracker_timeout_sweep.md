# UC_11 — AckTracker sweep: PENDING entries past deadline_us collected as expired

**HL Group:** HL-11 — Sweep ACK Timeouts
**Actor:** User
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::sweep_ack_timeouts(now_us)`. `AckTracker::sweep_expired()` scans all `PENDING` slots for entries past their `deadline_us`. File: `src/core/DeliveryEngine.cpp`, `src/core/AckTracker.cpp`.
- **Goal:** Identify `RELIABLE_ACK` messages whose ACK deadline has passed without an ACK arriving, free those slots, and return the count of unacknowledged messages.
- **Success outcome:** Returns the count of expired slots found (>= 0). For each expired slot: state transitions `PENDING → FREE` and the slot is available for reuse.
- **Error outcomes:** None — `sweep_ack_timeouts()` always succeeds. Returns 0 if nothing has expired.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
uint32_t DeliveryEngine::sweep_ack_timeouts(uint64_t now_us);
```

Called from the User's application event loop. Synchronous in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::sweep_ack_timeouts(now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)`.
3. **`m_ack_tracker.sweep_expired(now_us, m_timeout_buf, ACK_TRACKER_CAPACITY)`** is called (`AckTracker.cpp`):
   a. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]`:
      - For each entry where `entry.state == PENDING && now_us >= entry.deadline_us`:
        - Copy envelope to `expired_buf[count]`.
        - `entry.state = FREE`.
        - `count++`.
   b. Returns `count` (number of expired entries removed) directly as `uint32_t`.
4. Back in `sweep_ack_timeouts()`: `collected = sweep_expired(...)`.
5. Log each expired entry at `WARNING_HI`: message_id and destination node.
6. `sweep_ack_timeouts()` returns `collected` to the User.

---

## 4. Call Tree

```
DeliveryEngine::sweep_ack_timeouts(now_us)                     [DeliveryEngine.cpp]
 ├── AckTracker::sweep_expired(now_us, m_timeout_buf, cap)     [AckTracker.cpp]
 │    └── [for each PENDING entry past deadline: copy to buf; FREE slot; count++]
 │    returns uint32_t count
 └── [for each expired entry in m_timeout_buf:]
      Logger::log(WARNING_HI, ...)                             [Logger.hpp]
```

---

## 5. Key Components Involved

- **`AckTracker::sweep_expired(now_us, buf, cap)`** — Iterates over all `ACK_TRACKER_CAPACITY` slots. Returns expired envelopes in caller-supplied buffer; returns count directly as `uint32_t`.
- **`DeliveryEngine::sweep_ack_timeouts()`** — Owns the pre-allocated `m_timeout_buf[ACK_TRACKER_CAPACITY]` (zero-initialized in `init()`). Loops over the returned buffer to emit one `WARNING_HI` log per timed-out entry.
- **`DeliveryEngine::sweep_ack_timeouts()`** — Thin wrapper; returns the count to the application so it can respond to unacknowledged messages.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `entry.state == PENDING` | Check deadline | Skip (FREE or ACKED) |
| `now_us >= entry.deadline_us` | Log timeout; FREE slot; increment count | Still waiting |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. No threads created.
- `AckTracker::m_entries` is a plain fixed array; no atomics.
- Must be called from the same thread as `send()` and `receive()`.

---

## 8. Memory & Ownership Semantics

- No stack buffers beyond loop variables and the `expired_count` output.
- `AckTracker::m_entries[ACK_TRACKER_CAPACITY]` — 32-slot fixed array owned by `AckTracker` value member inside `DeliveryEngine`.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- No error states. `sweep_expired()` always completes successfully.
- Freed slots become immediately available for the next `track()` call.
- The caller should treat the returned count as the number of messages that may require application-level handling (e.g., log, alert, or trigger a resend at the application layer).

---

## 10. External Interactions

- **`stderr`:** `Logger::log(WARNING_LO)` for each timed-out entry.
- No network calls, file I/O, or hardware interaction.

---

## 11. State Changes / Side Effects

Per expired entry:

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `AckTracker` | `m_entries[slot].state` | `PENDING` | `FREE` |
| `stderr` | output | — | one WARNING_LO line per timeout |

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::sweep_ack_timeouts(now_us)
       -> AckTracker::sweep_expired(now_us, m_timeout_buf, ACK_TRACKER_CAPACITY)
            [for each PENDING entry where now_us >= deadline_us:]
            [copy env to m_timeout_buf[count]; entry.state = FREE; count++]
            <- uint32_t count = N
       [for i in 0..N-1:]
       -> Logger::log(WARNING_HI, "ACK timeout for message_id=... to node=...")
  <- N
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called; `AckTracker::init()` called (all slots FREE).
- Prior `send(RELIABLE_ACK, ...)` calls have populated PENDING slots with deadlines.

**Runtime:**
- `sweep_ack_timeouts()` is called periodically. Returns 0 when nothing has timed out. Returns >= 1 when at least one `RELIABLE_ACK` message went unacknowledged past its deadline.

---

## 14. Known Risks / Observations

- **No auto-retry:** `sweep_ack_timeouts()` only frees slots and counts timeouts. It does NOT automatically resend. The application layer decides whether to resend (by calling `send()` again) or escalate the error.
- **Freed slot availability:** A slot freed by sweep can be immediately reused by the next `track()` call in `send()`. The timing between sweep and send is application-controlled.
- **`deadline_us` depends on `recv_timeout_ms`:** Set during `DeliveryEngine::init()` from `ChannelConfig::recv_timeout_ms`. A too-short timeout generates false positives; a too-long timeout delays detection of dropped messages.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `sweep_expired()` returns `uint32_t` directly and fills a caller-supplied buffer (`m_timeout_buf` in `DeliveryEngine`). The old out-pointer `&expired_count` API has been replaced.
- `[ASSUMPTION]` The returned count from `sweep_ack_timeouts()` equals the number of slots transitioned from PENDING to FREE during this call. No additional bookkeeping is performed.
