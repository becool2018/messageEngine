# UC_03 — RELIABLE_RETRY send: message scheduled in RetryManager with exponential backoff

**HL Group:** HL-3 — Send with Automatic Retry
**Actor:** User
**Requirement traceability:** REQ-3.3.3, REQ-3.2.5, REQ-3.2.6, REQ-4.1.2, REQ-7.1.4

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send(envelope, now_us)` with `envelope.reliability_class == ReliabilityClass::RELIABLE_RETRY`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Transmit the envelope, register it in `RetryManager` for automatic retransmission until an ACK is received, the expiry passes, or the retry budget is exhausted.
- **Success outcome:** `Result::OK` returned. The envelope is on the wire; a `RetryEntry` slot in `RetryManager` is active with `next_retry_us = now_us`, `backoff_ms` from channel config, and `max_retries` from channel config.
- **Error outcomes:**
  - `Result::ERR_EXPIRED` — expiry check fails in `send_via_transport()`.
  - `Result::ERR_FULL` from `reserve_bookkeeping()` (AckTracker or RetryManager full) — logged at `WARNING_HI`; send returns `ERR_FULL` immediately with no wire I/O.
  - Any non-OK from transport — propagated; bookkeeping slots rolled back via `rollback_on_transport_failure()`.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& envelope, uint64_t now_us);
```

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `m_id_gen.next()` assigns new `message_id` into `MessageEnvelope work` (stack copy).
4. `work.reliability_class == RELIABLE_RETRY` — true.
5. **`reserve_bookkeeping(m_ack_tracker, m_retry_manager, work, m_cfg, now_us)`** is called (static helper, `DeliveryEngine.cpp`). For `RELIABLE_RETRY` this calls both `track()` and `schedule()`:
   a. Calls `m_ack_tracker.track(work, ack_deadline)`. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]` for first `FREE` slot.
      - If no free slot: logs `WARNING_HI`; returns `ERR_FULL` immediately.
   b. On `track()` success: calls `m_retry_mgr.schedule(work, now_us, m_cfg.retry_backoff_ms, m_cfg.max_retries)`.
      - Inside `schedule()`: `NEVER_COMPILED_OUT_ASSERT(backoff_ms > 0)` and `NEVER_COMPILED_OUT_ASSERT(max_retries > 0)`. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]` for first slot where `!active`.
      - If found: `entry.env = work` (full `MessageEnvelope` copy), `entry.next_retry_us = now_us`, `entry.expiry_us = env.expiry_time_us`, `entry.retry_count = 0`, `entry.max_retries = max_retries`, `entry.backoff_ms = backoff_ms`, `entry.active = true`. Returns `Result::OK`.
      - If not found: logs `WARNING_HI`; calls `m_ack_tracker.cancel(work.source_id, work.message_id)` to free the already-reserved AckTracker slot; returns `ERR_FULL`.
6. If `book_res != OK`: `record_send_failure()` called; `send()` returns `ERR_FULL` immediately. No wire I/O occurs.
7. On `book_res == OK` (both slots reserved): **`send_via_transport(work, now_us)`** is called. Expiry check, then `m_transport->send_message(work)` (same TCP path as UC_01).
   - If `send_res != OK`: **`rollback_on_transport_failure()`** cancels both the `AckTracker` slot and the `RetryManager` slot (freeing both, no stat bump). `send()` returns the transport error.
8. On `send_res == OK`: `++m_stats.msgs_sent`.
9. Returns `Result::OK` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                          [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                       [MessageId.cpp]
 ├── reserve_bookkeeping()                      [DeliveryEngine.cpp]
 │    ├── AckTracker::track()                   [AckTracker.cpp]
 │    │    └── [linear scan for FREE slot; set PENDING]
 │    └── RetryManager::schedule()              [RetryManager.cpp]
 │         └── [linear scan for inactive slot; copy envelope; set active]
 ├── DeliveryEngine::send_via_transport()       [DeliveryEngine.cpp]
 │    ├── timestamp_expired()
 │    └── TcpBackend::send_message()
 │         ├── ImpairmentEngine::process_outbound()
 │         ├── ImpairmentEngine::collect_deliverable()
 │         ├── Serializer::serialize()
 │         └── send_to_all_clients() -> ::send()
 └── [on transport failure] rollback_on_transport_failure()
      ├── AckTracker::cancel()                  [AckTracker.cpp]
      └── RetryManager::cancel()                [RetryManager.cpp]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Sequences bookkeeping then wire send. Both `AckTracker` and `RetryManager` slots are reserved before any I/O so `ERR_FULL` is returned before the peer sees the message. If `send_via_transport()` fails after bookkeeping, `rollback_on_transport_failure()` cancels both slots so no spurious timeout or retry fires.
- **`RetryManager`** — Fixed-capacity table (32 slots). Stores the full `MessageEnvelope` copy plus retry metadata. `next_retry_us = now_us` means the first retry is due immediately on the next `pump_retries()` call.
- **`MessageIdGen`** — ID assigned here is the same ID stored in `RetryEntry.env.message_id` and used to cancel the entry when an ACK for that ID arrives.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `reliability_class == RELIABLE_RETRY` | Enter `reserve_bookkeeping()` path | Not this UC |
| AckTracker slot available (inside `reserve_bookkeeping`) | `track()` OK; proceed to `schedule()` | `track()` returns `ERR_FULL`; `send()` returns `ERR_FULL` |
| RetryManager slot available (inside `reserve_bookkeeping`) | `schedule()` OK; proceed | `schedule()` fails; `cancel()` AckTracker slot; `send()` returns `ERR_FULL` |
| `book_res != OK` | Return `ERR_FULL` without I/O | Proceed to `send_via_transport()` |
| `send_res != OK` | `rollback_on_transport_failure()` (cancel both slots); return error | Proceed; `msgs_sent++` |
| `env.expiry_time_us == 0` | Entry never expires (expiry_us=0 treated as no-expiry in collect_due) | Entry expires at expiry_us |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. No new threads.
- `RetryManager::m_entries` is a plain fixed array; not thread-safe. `pump_retries()` and `on_ack()` must be called from the same thread as `send()`.
- No `std::atomic` operations on this path.

---

## 8. Memory & Ownership Semantics

- `MessageEnvelope work` — ~4152 bytes on stack in `DeliveryEngine::send()`.
- `RetryEntry::env` — full `MessageEnvelope` copy stored inside `RetryManager::m_entries[slot]`; this is the copy that will be retransmitted by `pump_retries()`. No heap allocation.
- `RetryManager::m_entries[ACK_TRACKER_CAPACITY]` — 32 `RetryEntry` structs, owned by `RetryManager` value member inside `DeliveryEngine`.
- Power of 10 Rule 3: no heap allocation.

---

## 9. Error Handling Flow

- **`ERR_FULL` from `reserve_bookkeeping()`:** `send()` returns `ERR_FULL` before any wire I/O. If `AckTracker` is full, returns immediately. If `RetryManager` is full, the already-reserved `AckTracker` slot is rolled back via `cancel()` before returning. Caller must handle `ERR_FULL`.
- **Transport failure after bookkeeping:** `rollback_on_transport_failure()` cancels both the `AckTracker` and `RetryManager` slots (no stat bump), preventing spurious timeout sweeps or phantom retries. The transport error is returned.
- **`ERR_EXPIRED`:** Send aborted before socket; nothing allocated.

---

## 10. External Interactions

- **POSIX `::send()`:** TCP socket in `TcpBackend`, outbound. Identical to UC_01.
- No other external interactions on initial send.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `MessageIdGen` | `m_counter` | N | N+1 |
| `RetryManager` | `m_entries[slot].active` | `false` | `true` |
| `RetryManager` | `m_entries[slot].env` | undefined | copy of `work` envelope |
| `RetryManager` | `m_entries[slot].next_retry_us` | undefined | `now_us` |
| `RetryManager` | `m_entries[slot].retry_count` | undefined | `0` |
| TCP socket | kernel send buffer | previous | + serialized frame |

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::send(envelope, now_us)
       -> MessageIdGen::next()
       -> reserve_bookkeeping()
            -> AckTracker::track(work, deadline_us)
                 [scan FREE slot; set PENDING]
                 <- Result::OK
            -> RetryManager::schedule(work, now_us, backoff_ms, max_retries)
                 [scan inactive slot; copy envelope; set active=true]
                 <- Result::OK
            <- Result::OK
       -> DeliveryEngine::send_via_transport()
            -> TcpBackend::send_message()
                 -> Serializer::serialize() -> ::send()
            <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called; `RetryManager::init()` called (all entries `active=false`).
- `m_cfg.retry_backoff_ms > 0` and `m_cfg.max_retries > 0` (asserted inside `schedule()`).

**Runtime:**
- Each RELIABLE_RETRY send occupies one `RetryManager` slot until ACK received or expiry/budget exhausted.
- `pump_retries(now_us)` must be called periodically to retransmit due entries.

---

## 14. Known Risks / Observations

- **`next_retry_us = now_us`:** The first retry is due immediately. If `pump_retries()` is called before the peer has a chance to ACK, the message may be resent before the original copy is even delivered. Receiver-side deduplication (DuplicateFilter) is required to suppress redundant copies.
- **RetryManager full returns `ERR_FULL`:** When the `RetryManager` is full, `reserve_bookkeeping()` cancels the already-reserved `AckTracker` slot and returns `ERR_FULL`. `send()` returns `ERR_FULL` before any wire I/O — no longer a silent failure. Caller must handle `ERR_FULL`.
- **Full envelope copy cost:** `RetryEntry::env` is a copy of the full `MessageEnvelope` (~4152 bytes). With 32 slots, `RetryManager` holds up to ~132 KB of envelope data as fixed members.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `next_retry_us = now_us` is inferred from the `RetryManager::schedule()` implementation where `entry.next_retry_us = now_us` is set directly, meaning the first retry fires on the very next `pump_retries()` call.
- `[ASSUMPTION]` The channel config values `retry_backoff_ms` and `max_retries` used here come from `m_cfg` set during `DeliveryEngine::init()`; confirmed by `test_DeliveryEngine.cpp` setting `cfg_a.channels[0].max_retries = 5` and `retry_backoff_ms = 100`.
