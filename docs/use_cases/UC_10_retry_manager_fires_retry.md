# UC_10 — RetryManager fires a scheduled retry (backoff interval elapsed)

**HL Group:** HL-10 — Pump the Retry Loop
**Actor:** User
**Requirement traceability:** REQ-3.2.5, REQ-3.3.3, REQ-4.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::pump_retries(now_us)`. `RetryManager::collect_due()` finds one or more active entries whose `next_retry_us <= now_us`. File: `src/core/DeliveryEngine.cpp`, `src/core/RetryManager.cpp`.
- **Goal:** Retransmit all `RELIABLE_RETRY` messages whose backoff interval has elapsed; advance their backoff for the next attempt.
- **Success outcome:** Returns the count of due entries collected. For each: the envelope is retransmitted; `retry_count` incremented; `next_retry_us` advanced with doubled backoff. Returns `uint32_t` count >= 1.
- **Error outcomes:** Returns 0 if no retries are due. Individual retransmission failures are logged at `WARNING_LO`; the count still reflects entries that were collected even if the transport send failed.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
uint32_t DeliveryEngine::pump_retries(uint64_t now_us);
```

Called from the User's application event loop. Synchronous in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::pump_retries(now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)`.
3. Pre-allocated member buffer `m_retry_buf[MSG_RING_CAPACITY]` (64 slots, zero-initialised in `init()`) is used for due entries. No stack allocation.
4. **`m_retry_mgr.collect_due(now_us, m_retry_buf, MSG_RING_CAPACITY)`** (`RetryManager.cpp`):
   a. `NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0)`.
   b. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]`:
      - Skip inactive entries (`active == false`).
      - **Expiry check:** `slot_has_expired(entry, now_us)` — if true: `entry.active = false`; skip.
      - **Backoff due check:** `now_us >= entry.next_retry_us` AND `entry.retry_count < entry.max_retries`:
        - Copy `entry.env` to `out_buf[due_count++]`.
        - `entry.retry_count++`.
        - `advance_backoff(entry, now_us)`: `entry.backoff_ms = min(entry.backoff_ms * 2, 60000UL)`, `entry.next_retry_us = now_us + entry.backoff_ms * 1000ULL`.
      - If `retry_count >= max_retries`: `entry.active = false`.
   c. Returns `due_count` directly as `uint32_t`.
5. Back in `pump_retries()`: for each `m_retry_buf[i]` (`i` in `0..due_count-1`):
   a. **`send_via_transport(due_buf[i], now_us)`** — expiry check, then `m_transport->send_message()`.
   b. If non-OK: `Logger::log(WARNING_LO, ...)` ("retry send failed"); continue.
6. Returns `due_count` to the User.

---

## 4. Call Tree

```
DeliveryEngine::pump_retries(now_us)                          [DeliveryEngine.cpp]
 ├── RetryManager::collect_due(now_us, due_buf, cap)          [RetryManager.cpp]
 │    ├── slot_has_expired()
 │    └── advance_backoff()
 └── [for each due entry:]
      DeliveryEngine::send_via_transport()          [DeliveryEngine.cpp]
       ├── timestamp_expired()
       └── TcpBackend::send_message()
            ├── Serializer::serialize()
            └── send_to_all_clients() -> ::send()
```

---

## 5. Key Components Involved

- **`RetryManager::collect_due()`** — Scans all active entries; returns due ones and advances their backoff.
- **`advance_backoff()`** — Doubles `backoff_ms` each retry, capped at 60000ms. Sets `next_retry_us = now_us + new_backoff * 1000`.
- **`slot_has_expired()`** — Returns true when `entry.expiry_us != 0 && now_us >= entry.expiry_us`.
- **`DeliveryEngine::send_via_transport()`** — Retransmits the stored envelope (same `message_id`) so receiver dedup can identify and suppress it.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `entry.active == false` | Skip | Evaluate expiry and backoff |
| `slot_has_expired(entry, now_us)` | Deactivate; skip | Check backoff |
| `now_us >= entry.next_retry_us && retry_count < max_retries` | Collect + advance backoff | Not yet due |
| `retry_count >= max_retries` | Deactivate entry | Remain active |
| `send_via_transport()` returns non-OK | Log warning; continue loop | Normal completion |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. No threads created.
- `RetryManager::m_entries` modified in `collect_due()` — plain array, no atomics.
- `pump_retries()` must be called from the same thread as `send()` and `receive()`.

---

## 8. Memory & Ownership Semantics

- `m_retry_buf[MSG_RING_CAPACITY]` — 64-slot pre-allocated member array (initialised in `DeliveryEngine::init()`). Using a member buffer avoids a ~260 KB stack frame that would otherwise be needed for the due-entries collection.
- `RetryEntry::env` is read during `collect_due()` (copied to `m_retry_buf`); not modified.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **Transport failure during retry:** Entry remains active with updated `next_retry_us`. Will retry again after next backoff elapses.
- **Budget exhausted:** `entry.active = false`. No further retries; no caller notification beyond reduced count.
- **Expiry in collect_due:** Entry deactivated silently.

---

## 10. External Interactions

- **POSIX `::send()`:** TCP socket outbound for each retried envelope.
- **`stderr`:** Logger on transport failure.

---

## 11. State Changes / Side Effects

Per due entry:

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RetryManager` | `entry.retry_count` | N | N+1 |
| `RetryManager` | `entry.backoff_ms` | B | min(B*2, 60000) |
| `RetryManager` | `entry.next_retry_us` | old | `now_us + new_backoff * 1000` |
| `RetryManager` | `entry.active` (exhausted/expired) | true | false |
| TCP socket | kernel send buffer | previous | + serialized retry frame |

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::pump_retries(now_us)
       -> RetryManager::collect_due(now_us, m_retry_buf, MSG_RING_CAPACITY)
            [scan; collect due; advance backoff; deactivate exhausted/expired]
            <- uint32_t N
       [for i in 0..N-1:]
            -> send_via_transport(due_buf[i], now_us)
                 -> TcpBackend::send_message() -> ::send()
                 <- Result::OK
  <- N
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called; `RetryManager::init()` called.
- At least one `RELIABLE_RETRY` send has been called with `next_retry_us = send_now_us`.

**Runtime:**
- Called periodically from the application event loop. First retry fires immediately (since `next_retry_us = now_us` at schedule time). Subsequent retries fire at increasing backoff intervals.

---

## 14. Known Risks / Observations

- **Pre-allocated member buffer:** `m_retry_buf[MSG_RING_CAPACITY]` (64 slots × 4144 bytes = ~260 KB) is a value member of `DeliveryEngine`, initialised in `init()`. This avoids a large stack allocation on every `pump_retries()` call. The tradeoff is that `DeliveryEngine` holds this memory permanently even when retries are rare.
- **Receiver dedup required:** Same `message_id` retransmitted. Receiver's `DuplicateFilter` must be active for RELIABLE_RETRY paths to suppress redundant copies.
- **Backoff cap at 60s:** With `max_retries=5`, worst-case total retry window is bounded.
- **Entry stays active on transport failure:** Repeated failures keep the entry alive until expiry or budget exhaustion.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `advance_backoff()` uses `now_us` (current time) as the base for `next_retry_us`, not the previous scheduled time. This means small timing drifts in `pump_retries()` calls do not accumulate; each backoff interval is measured from the actual retry time.
