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
  - Non-OK from transport — propagated; `RetryManager::schedule()` is NOT called.
  - `Result::ERR_FULL` from `RetryManager::schedule()` — logged at `WARNING_LO`; send returns `OK` (schedule failure is non-fatal).

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(const MessageEnvelope& envelope, uint64_t now_us);
```

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `m_id_gen.next()` assigns new `message_id` into `MessageEnvelope work` (stack copy).
4. `work.reliability_class == RELIABLE_RETRY` — true.
5. **`send_via_transport(work, now_us)`** — expiry check, then `m_transport->send_message(work)` (same TCP path as UC_01). Returns result.
6. If `send_res != Result::OK`: return `send_res` immediately; `RetryManager` not touched.
7. On success: **`m_retry_mgr.schedule(work, now_us, m_cfg.retry_backoff_ms, m_cfg.max_retries)`** is called (`RetryManager.cpp`).
8. Inside `RetryManager::schedule(env, now_us, backoff_ms, max_retries)`:
   a. `NEVER_COMPILED_OUT_ASSERT(backoff_ms > 0)` and `NEVER_COMPILED_OUT_ASSERT(max_retries > 0)`.
   b. Linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]` for first slot where `!active`.
   c. If found: `entry.env = env` (full `MessageEnvelope` copy), `entry.next_retry_us = now_us`, `entry.expiry_us = env.expiry_time_us`, `entry.retry_count = 0`, `entry.max_retries = max_retries`, `entry.backoff_ms = backoff_ms`, `entry.active = true`. Returns `Result::OK`.
   d. If not found: returns `Result::ERR_FULL`.
9. `sched_res != Result::OK` — logged at `WARNING_LO`; send returns `Result::OK`.
10. `NEVER_COMPILED_OUT_ASSERT` postcondition check.
11. Returns `Result::OK` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                          [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                       [MessageId.cpp]
 ├── DeliveryEngine::send_via_transport()       [DeliveryEngine.cpp]
 │    ├── timestamp_expired()
 │    └── TcpBackend::send_message()
 │         ├── ImpairmentEngine::process_outbound()
 │         ├── ImpairmentEngine::collect_deliverable()
 │         ├── Serializer::serialize()
 │         └── send_to_all_clients() -> ::send()
 └── RetryManager::schedule()                   [RetryManager.cpp]
      └── [linear scan for inactive slot; copy envelope]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Sequences wire send then retry registration. Retry is only registered after successful wire send to avoid tracking un-sent messages.
- **`RetryManager`** — Fixed-capacity table (32 slots). Stores the full `MessageEnvelope` copy plus retry metadata. `next_retry_us = now_us` means the first retry is due immediately on the next `pump_retries()` call.
- **`MessageIdGen`** — ID assigned here is the same ID stored in `RetryEntry.env.message_id` and used to cancel the entry when an ACK for that ID arrives.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `reliability_class == RELIABLE_RETRY` | Call schedule() after send | Not this UC |
| `send_res != OK` | Return error; skip schedule() | Call schedule() |
| Free slot found in RetryManager | Set active=true; schedule returns OK | Returns ERR_FULL |
| `sched_res != OK` | Log warning; return OK | Return OK |
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

- **Transport failure before schedule():** `RetryManager` not touched; clean state.
- **`ERR_FULL` from schedule():** Message is on the wire but will never be automatically retried. Caller has no direct indication. Logged at WARNING_LO.
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
       -> DeliveryEngine::send_via_transport()
            -> TcpBackend::send_message()
                 -> Serializer::serialize() -> ::send()
            <- Result::OK
       -> RetryManager::schedule(work, now_us, backoff_ms, max_retries)
            [scan inactive slot; copy envelope; set active=true]
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
- **RetryManager full silent failure:** Same pattern as AckTracker — `sched_res != OK` is non-fatal for the send. Message will not be retried if slot allocation failed.
- **Full envelope copy cost:** `RetryEntry::env` is a copy of the full `MessageEnvelope` (~4152 bytes). With 32 slots, `RetryManager` holds up to ~132 KB of envelope data as fixed members.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `next_retry_us = now_us` is inferred from the `RetryManager::schedule()` implementation where `entry.next_retry_us = now_us` is set directly, meaning the first retry fires on the very next `pump_retries()` call.
- `[ASSUMPTION]` The channel config values `retry_backoff_ms` and `max_retries` used here come from `m_cfg` set during `DeliveryEngine::init()`; confirmed by `test_DeliveryEngine.cpp` setting `cfg_a.channels[0].max_retries = 5` and `retry_backoff_ms = 100`.
