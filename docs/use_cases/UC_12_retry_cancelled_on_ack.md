# UC_12 — Retry cancelled on ACK receipt (RetryManager slot freed)

**HL Group:** HL-3 — Send with Automatic Retry
**Actor:** System
**Requirement traceability:** REQ-3.2.5, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` processes an incoming `ACK` envelope and calls `m_retry_manager.on_ack(src, message_id)` (via `process_ack()`). File: `src/core/RetryManager.cpp`.
- **Goal:** Cancel all pending retries for the acknowledged `message_id` by setting the active `RetryEntry` to `inactive`.
- **Success outcome:** The matching active `RetryEntry` has `active` set to `false`. The slot is immediately available for a new `schedule()` call. Returns `Result::OK`.
- **Error outcomes:**
  - `Result::ERR_INVALID` — no active entry matches `(src, message_id)`. Logged at `INFO` by the caller; non-fatal. Can occur for RELIABLE_ACK sends (no retry entry exists) or if the ACK is a duplicate.

---

## 2. Entry Points

```cpp
// src/core/RetryManager.cpp (called from DeliveryEngine::receive() via process_ack())
Result RetryManager::on_ack(NodeId src, uint64_t msg_id);
```

Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::receive()` pops an `ACK` envelope from the ring buffer.
2. `envelope_is_control(raw)` — true; `raw.message_type == ACK` — true.
3. **`m_ack_tracker.on_ack(env.destination_id, env.message_id)`** is called first (see UC_08 for AckTracker side).
4. **`m_retry_manager.on_ack(env.destination_id, env.message_id)`** is called (`RetryManager.cpp`) inside `process_ack()`.
5. Inside `RetryManager::on_ack(src, msg_id)`:
   a. `NEVER_COMPILED_OUT_ASSERT(msg_id != 0)`.
   b. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]`:
      - For each slot: if `entry.active && entry.env.source_id == src && entry.env.message_id == msg_id`:
        - `entry.active = false`.
        - Returns `Result::OK`.
   c. If no match: returns `Result::ERR_INVALID`.
6. Back in `process_ack()`: result is checked; if `ERR_INVALID`: `LOG_INFO(...)` ("ACK has no matching retry slot").
7. `receive()` copies `raw` to `out`, returns `Result::OK`.

---

## 4. Call Tree

```
DeliveryEngine::receive()                            [DeliveryEngine.cpp]
 └── process_ack(m_ack_tracker, m_retry_manager, env) [DeliveryEngine.cpp]
      ├── AckTracker::on_ack(src, msg_id)            [AckTracker.cpp]   (UC_08)
      └── RetryManager::on_ack(src, msg_id)          [RetryManager.cpp]
           └── [linear scan; entry.active = false on match]
```

---

## 5. Key Components Involved

- **`RetryManager::on_ack()`** — The cancel operation. Marks the matching entry as inactive so `collect_due()` skips it.
- **`DeliveryEngine::receive()`** — Orchestrates both `AckTracker::on_ack()` and `RetryManager::on_ack()` in sequence on every received ACK.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `entry.active && entry.env.message_id == message_id` | `active = false`; return OK | Continue scan |
| No matching active entry found | Return ERR_INVALID | — |
| `on_ack()` returns ERR_INVALID | Log warning in receive() | Silent success |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. No threads created.
- `RetryManager::m_entries` — plain fixed array. Not safe to call from multiple threads.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `RetryEntry::active = false` — one bool field write; no memory freed (the slot memory remains; it is logically reusable).
- `RetryEntry::env` — the stored envelope copy is not cleared; it remains in memory until overwritten by the next `schedule()` call.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **`ERR_INVALID`:** Expected for `RELIABLE_ACK` sends (no retry was scheduled). Non-fatal; logged at INFO. State remains consistent.
- The freed slot is immediately available for the next `schedule()` call (the next RELIABLE_RETRY send).

---

## 10. External Interactions

- None — this flow operates entirely in process memory.
- `LOG_*()` writes to `stderr` only on `ERR_INVALID`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RetryManager` | `m_entries[slot].active` | `true` | `false` |

All other `RetryEntry` fields are unchanged (stale values remain until the slot is reused).

---

## 12. Sequence Diagram

```
[ACK envelope received by DeliveryEngine::receive()]
  -> process_ack(m_ack_tracker, m_retry_manager, env)
       -> AckTracker::on_ack(env.destination_id, env.message_id)   [UC_08: PENDING -> FREE]
       -> RetryManager::on_ack(env.destination_id, env.message_id)
            [scan; find active entry with matching (src, message_id)]
            [entry.active = false]
            <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `RetryManager::init()` called; all entries `active = false`.
- Prior `RELIABLE_RETRY` `send()` has set `entry.active = true` for the acknowledged message.

**Runtime:**
- Called on every received ACK. Idempotent — calling `on_ack()` for the same `message_id` twice returns `ERR_INVALID` on the second call (first call already cleared `active`).

---

## 14. Known Risks / Observations

- **Idempotency:** If two ACKs arrive for the same `message_id`, the second produces `ERR_INVALID`. This is expected and non-fatal.
- **Stale `env` data in deactivated slot:** The stored envelope copy is not zeroed when deactivated. Sensitive payload data remains in `m_entries` until overwritten. This is a minor concern for security-sensitive deployments.
- **Race between pump and ACK:** If `pump_retries()` collects an entry into `due_buf` just before `on_ack()` sets `active = false`, the entry is retransmitted once after the ACK was received. Receiver dedup handles the redundant copy.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `RetryManager::on_ack()` is always called regardless of whether a retry entry was scheduled. For RELIABLE_ACK sends, it returns `ERR_INVALID` which is non-fatal.
- `[ASSUMPTION]` The stale `env` data in deactivated slots is not zeroed. This is inferred from the `on_ack()` implementation that only sets `active = false`.
