# UC_03 — RELIABLE_RETRY send: message scheduled in RetryManager with exponential backoff

**HL Group:** HL-3 — Send with Automatic Retry
**Actor:** User
**Requirement traceability:** REQ-3.3.3, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.2.1,
REQ-3.2.3, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::send()` with a `MessageEnvelope` whose
`reliability_class` is `ReliabilityClass::RELIABLE_RETRY`.

**Goal:** Transmit the envelope once over TCP, allocate one slot in `AckTracker` (for ACK
detection), and also register the message in `RetryManager` for automatic exponential-backoff
retransmission. The system will retransmit the message on each call to `pump_retries()` until
an ACK is received, the retry budget (`max_retries`) is exhausted, or the message's
`expiry_time_us` passes.

**Success outcome:** `DeliveryEngine::send()` returns `Result::OK`. The wire frame has
reached the TCP socket(s). One `AckTracker` slot is PENDING. One `RetryManager` slot is
active with `next_retry_us = now_us` (due immediately on first `pump_retries()` call).

**Error outcomes:**
- `Result::ERR_INVALID` — engine not initialized or invalid envelope.
- Non-OK from transport (e.g., `ERR_IO`) — WARNING_LO logged; ACK tracking and retry
  scheduling are still attempted (best-effort side effects; `send()` returns the transport
  error without attempting tracking/scheduling).
- `Result::OK` with `AckTracker::track()` returning `ERR_FULL` — send succeeded; ACK
  tracking failed; WARNING_HI logged; no early return.
- `Result::OK` with `RetryManager::schedule()` returning `ERR_FULL` — send succeeded; retry
  not scheduled; WARNING_HI logged; no early return.

---

## 2. Entry Points

```
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us);

// Downstream platform entry (via virtual dispatch):
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);

// ACK tracking:
// src/core/AckTracker.cpp
Result AckTracker::track(const MessageEnvelope& env, uint64_t deadline_us);

// Retry scheduling:
// src/core/RetryManager.cpp
Result RetryManager::schedule(const MessageEnvelope& env,
                              uint32_t max_retries,
                              uint32_t backoff_ms,
                              uint64_t now_us);
```

The caller fills the envelope with `reliability_class = RELIABLE_RETRY` and sets
`expiry_time_us` to a future timestamp (e.g., `timestamp_deadline_us(now_us, 5000U)` for a
5-second expiry as used in `Client.cpp`). Then calls `engine.send(env, now_us)`.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. Caller invokes `DeliveryEngine::send(env, now_us)`.
2. Pre-condition asserts: `m_initialized`, `now_us > 0`.
3. Guard: `if (!m_initialized) return ERR_INVALID` — skipped when initialized.
4. `send()` stamps `env.source_id = m_local_id`.
5. `send()` calls `m_id_gen.next()` → stores non-zero result in `env.message_id`.
6. `send()` stamps `env.timestamp_us = now_us`.
7. `send()` calls `send_via_transport(env)` → `TcpBackend::send_message()` (same as UC_01
   steps 9–12). Returns `OK` on success.
8. Transport result check: if non-OK, logs WARNING_LO and returns that result. Send aborts;
   no tracking or scheduling is attempted.
9. First reliability branch — `if (reliability_class == RELIABLE_ACK || RELIABLE_RETRY)`:
   TRUE for RELIABLE_RETRY.
   a. `ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms)`.
   b. `m_ack_tracker.track(env, ack_deadline)` — see UC_02 step 13 for internals.
      On `ERR_FULL`: logs WARNING_HI, continues (no early return).
10. Second reliability branch — `if (reliability_class == RELIABLE_RETRY)`: TRUE.
    a. Calls `m_retry_manager.schedule(env, m_cfg.max_retries, m_cfg.retry_backoff_ms,
       now_us)`.
11. Inside `RetryManager::schedule(env, max_retries, backoff_ms, now_us)`:
    a. Pre-condition asserts: `m_initialized`, `envelope_valid(env)`.
    b. Scans `m_slots[0..ACK_TRACKER_CAPACITY-1]` (32 slots) for a slot where
       `active == false`.
    c. On finding a free slot at index `i`:
       - `m_slots[i].active = true`.
       - `envelope_copy(m_slots[i].env, env)` — full memcpy of the envelope.
       - `m_slots[i].retry_count = 0`.
       - `m_slots[i].max_retries = max_retries` (e.g., 3 in the demo apps).
       - `m_slots[i].backoff_ms = backoff_ms` (e.g., 100 ms initial backoff from
         `ChannelConfig::retry_backoff_ms`).
       - `m_slots[i].next_retry_us = now_us` — due immediately (first retry on next
         `pump_retries()` call).
       - `m_slots[i].expiry_us = env.expiry_time_us` — copied from the envelope.
       - Increments `m_count`.
       - Post-condition asserts: `m_count <= ACK_TRACKER_CAPACITY`,
         `m_slots[i].active == true`.
       - Logs INFO: `"Scheduled message_id=... for retry (max_retries=..., backoff_ms=...)"`.
       - Returns `Result::OK`.
    d. If no free slot: asserts `m_count == ACK_TRACKER_CAPACITY`, logs WARNING_LO,
       returns `ERR_FULL`.
12. Back in `DeliveryEngine::send()`: checks `sched_res`:
    - Non-OK → logs WARNING_HI: `"Failed to schedule retry for message_id=..."`.
      Does NOT return early.
13. Post-condition asserts: `env.source_id == m_local_id`, `env.message_id != 0`.
14. Logs INFO: `"Sent message_id=..., reliability=2"`.
15. Returns `OK`.
16. Caller receives `OK`.
    - `AckTracker` has one PENDING slot for this message.
    - `RetryManager` has one active slot with `next_retry_us = now_us`.

---

## 4. Call Tree

```
DeliveryEngine::send(env, now_us)
 ├── MessageIdGen::next()
 ├── DeliveryEngine::send_via_transport(env)
 │    └── TcpBackend::send_message(envelope)        [virtual dispatch]
 │         ├── Serializer::serialize(...)
 │         ├── ImpairmentEngine::process_outbound(...)
 │         └── TcpBackend::flush_delayed_to_clients(now_us)
 │              └── ISocketOps::send_frame(fd, buf, len, timeout_ms)
 ├── AckTracker::track(env, ack_deadline)            [if res==OK, RELIABLE_ACK||RETRY]
 │    └── envelope_copy(m_slots[i].env, env)
 └── RetryManager::schedule(env, max_retries,        [if res==OK, RELIABLE_RETRY only]
                            backoff_ms, now_us)
      └── envelope_copy(m_slots[i].env, env)
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Coordinates three distinct operations in sequence: (1) transport
  send, (2) ACK-tracking registration, (3) retry scheduling. Failures in (2) and (3) are
  non-fatal to the caller.

- **`MessageIdGen`** — Same as UC_01 and UC_02. The message_id stamped here is stored in
  both the `AckTracker` and `RetryManager` copies of the envelope.

- **`TcpBackend`** — Same as UC_01.

- **`AckTracker`** — Same as UC_02. Registers the PENDING slot so that incoming ACKs can be
  matched and the PENDING state cleared.

- **`RetryManager`** — Maintains a fixed table of 32 `RetryEntry` slots. `schedule()` fills
  one slot. Each entry holds a full copy of the envelope, the retry counter, the max retry
  limit, the current backoff interval, the next-retry timestamp, and the expiry time.
  `pump_retries()` (UC_10) later reads and retransmits due entries.

- **`timestamp_deadline_us()`** — Used to compute the ACK deadline from `recv_timeout_ms`.
  The retry expiry (`expiry_us`) is taken directly from `env.expiry_time_us`, not computed
  here.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `!m_initialized` | Return `ERR_INVALID` | Continue | Step 4 |
| `send_via_transport()` non-OK | Log WARNING_LO, return result | Continue | ACK-track branch |
| `reliability_class == RELIABLE_ACK \|\| RELIABLE_RETRY` | Compute deadline, call `track()` | Skip | Retry branch |
| `AckTracker::track()` non-OK | Log WARNING_HI, continue | — | Retry branch |
| `reliability_class == RELIABLE_RETRY` | Call `RetryManager::schedule()` | Skip | Post-condition asserts |
| `RetryManager::schedule()` non-OK | Log WARNING_HI, continue | — | Post-condition asserts |
| Free slot found in RetryManager | Fill slot, set `active=true`, return OK | Continue scan | Return to `send()` |
| No free slot in RetryManager | Assert full, log WARNING_LO, return `ERR_FULL` | — | Back to `send()` |

---

## 7. Concurrency / Threading Behavior

- Same single-thread assumption as UC_01 and UC_02 for the send path.

- **`RetryManager`** is not thread-safe. It is owned by `DeliveryEngine` and accessed only
  from the application thread calling `send()` and `pump_retries()`.

- **`AckTracker`** is accessed from the same thread for `track()` (here) and
  `on_ack()` / `sweep_expired()` (from the receive/sweep path). No concurrent access is
  permitted.

- No `std::atomic` accesses occur in `RetryManager::schedule()` or `AckTracker::track()`.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `env` (caller's) | Caller's stack | ≈ 4140 bytes | `source_id`, `message_id`, `timestamp_us` stamped |
| `m_wire_buf` | `TcpBackend` member | 8192 bytes | Reused each call |
| `delayed[]` in `flush_delayed_to_clients` | Stack | 32 × ~4140 ≈ 132 KB | See UC_01 §8 |
| `AckTracker::m_slots[i].env` | `AckTracker` member | ≈ 4140 bytes | Deep-copied via `envelope_copy()` |
| `RetryManager::m_slots[i].env` | `RetryManager` member | ≈ 4140 bytes | Deep-copied via `envelope_copy()`. **Second full copy of the envelope.** |
| `RetryManager::m_slots[i]` fields | `RetryManager` member | `RetryEntry` struct | `retry_count=0`, `max_retries`, `backoff_ms`, `next_retry_us=now_us`, `expiry_us=env.expiry_time_us` |
| `RetryManager::m_slots` array | `RetryManager` member | 32 × sizeof(RetryEntry) | Fixed at construction; no heap |

**Note on copies:** For a single RELIABLE_RETRY send, the envelope is copied three times:
(1) from caller's stack into `AckTracker::m_slots[i].env`, (2) from caller's stack into
`RetryManager::m_slots[i].env`, and (3) by the serializer's read of the envelope
during `send_message()`. Each copy is ≈ 4140 bytes. This is bounded by the fixed capacity
constants and incurs no heap allocation.

**Power of 10 Rule 3 confirmation:** No dynamic allocation. Both `AckTracker` and
`RetryManager` use pre-allocated fixed arrays.

---

## 9. Error Handling Flow

| Condition | System state after | What caller should do |
|---|---|---|
| `!m_initialized` → `ERR_INVALID` | No state changes | Call `init()` |
| `send_via_transport()` non-OK | No tracking or scheduling; WARNING_LO logged | Inspect result; retry at application level |
| `AckTracker::track()` → `ERR_FULL` | Message sent, not ACK-tracked, retry scheduled; WARNING_HI logged | Reduce in-flight count; call `sweep_ack_timeouts()` to reclaim slots |
| `RetryManager::schedule()` → `ERR_FULL` | Message sent, ACK-tracked, not retry-scheduled; WARNING_HI logged | Reduce in-flight count; message will not be automatically retried |
| Message expires in RetryManager | `collect_due()` (called from `pump_retries()`) deactivates the slot | Call `pump_retries()` regularly in the event loop |
| Retry budget exhausted | `collect_due()` deactivates the slot | Call `pump_retries()` regularly |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | POSIX monotonic clock | In `timestamp_now_us()` (inside `send_message()`) and `timestamp_deadline_us()` |
| `ISocketOps::send_frame()` | TCP socket fd | See UC_01 §10 |

No file I/O, IPC, or hardware interaction on the UC_03 send path.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|---|---|---|---|
| `MessageEnvelope& env` | `source_id` | Any | `m_local_id` |
| `MessageEnvelope& env` | `message_id` | Any | Non-zero monotonic value |
| `MessageEnvelope& env` | `timestamp_us` | Any | `now_us` |
| `MessageIdGen m_id_gen` | `m_next` | N | N+1 (or 1 on wrap) |
| `TcpBackend m_wire_buf` | bytes | Stale | Serialized frame |
| `AckTracker m_slots[i]` | `state` | `FREE` | `PENDING` |
| `AckTracker m_slots[i]` | `env`, `deadline_us` | Zeroed | Filled |
| `AckTracker m_count` | N | N+1 |
| `RetryManager m_slots[j]` | `active` | `false` | `true` |
| `RetryManager m_slots[j]` | `env` | Zeroed | Deep copy of `env` |
| `RetryManager m_slots[j]` | `retry_count` | 0 | 0 |
| `RetryManager m_slots[j]` | `max_retries` | 0 | `m_cfg.max_retries` |
| `RetryManager m_slots[j]` | `backoff_ms` | 0 | `m_cfg.retry_backoff_ms` |
| `RetryManager m_slots[j]` | `next_retry_us` | 0 | `now_us` (due immediately) |
| `RetryManager m_slots[j]` | `expiry_us` | 0 | `env.expiry_time_us` |
| `RetryManager m_count` | M | M+1 |
| Kernel TCP send buffer | bytes | Previous | Appended with length-prefixed frame |

---

## 12. Sequence Diagram

```
Caller     DeliveryEngine  MessageIdGen  TcpBackend    AckTracker   RetryManager
  |               |              |             |              |              |
  |--send(env,t)->|              |             |              |              |
  |               |--next()----->|             |              |              |
  |               |<-message_id--|             |              |              |
  |               | (stamp env)  |             |              |              |
  |               |--send_via_transport(env)--->|              |              |
  |               |              |  [serialize+impairment+flush+send_frame]  |
  |               |<--OK---------|             |              |              |
  |               |--track(env, deadline)------>|              |              |
  |               |              |             |  fill PENDING slot          |
  |               |<--OK (or ERR_FULL)---------|              |              |
  |               |--schedule(env, max, backoff, now)--------->|              |
  |               |              |             |              | fill slot    |
  |               |              |             |              | next_retry=t |
  |               |<--OK (or ERR_FULL)---------|              |              |
  |<--OK----------|              |             |              |              |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `RetryManager::init()` is called inside `DeliveryEngine::init()`: iterates all 32 slots
  and zeroes them (`active = false`, `retry_count = 0`, etc.). `m_count = 0`.
  `m_initialized = true`.

**Steady-state:**
- `RetryManager::schedule()` is called once per RELIABLE_RETRY send. The slot remains active
  until one of:
  1. ACK received → `RetryManager::on_ack()` (called from `DeliveryEngine::receive()` when
     a matching ACK envelope arrives) sets `active = false`, decrements `m_count`.
  2. Retry budget exhausted → `RetryManager::collect_due()` (called from `pump_retries()`)
     checks `retry_count >= max_retries`, deactivates slot.
  3. Expiry → `collect_due()` checks `slot_has_expired(expiry_us, now_us)`, deactivates slot.
- Each call to `pump_retries(now_us)` collects all slots where `next_retry_us <= now_us`,
  doubles `backoff_ms` (capped at 60 000 ms), and re-sends via `send_via_transport()`. This
  is UC_10 (not detailed here).

---

## 14. Known Risks / Observations

- **Three copies of the envelope per RELIABLE_RETRY send:** One in `AckTracker`, one in
  `RetryManager`, one implied by serialization. For large payloads (up to 4096 bytes each),
  32 active RELIABLE_RETRY messages consume ≈ 256 KB in AckTracker + ≈ 256 KB in
  RetryManager = ≈ 512 KB of fixed stack/static storage. This is a known design trade-off
  (Power of 10 Rule 3: no heap, fixed bounds).

- **`next_retry_us = now_us` means the first retry fires immediately.** On the very next
  call to `pump_retries()` the message will be retransmitted, even if it was just sent
  milliseconds ago. Applications should either call `pump_retries()` with appropriate
  temporal spacing or accept the immediate first retry.

- **`RetryManager` and `AckTracker` have the same capacity (32 slots each, both use
  `ACK_TRACKER_CAPACITY`).** A single RELIABLE_RETRY message occupies one slot in each.
  The effective limit is `min(32, 32) = 32` concurrent RELIABLE_RETRY messages.

- **Retry scheduling failure is not surfaced to the caller.** If `RetryManager::schedule()`
  returns `ERR_FULL`, the caller's `send()` still returns `OK`. The application receives no
  indication that automatic retry was not registered.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `m_cfg.retry_backoff_ms` is the initial backoff interval. It is set from
  `ChannelConfig::retry_backoff_ms` (100 ms default in `channel_config_default()`). The
  first retry fires at `now_us` (immediately); subsequent backoffs double: 100 ms, 200 ms,
  400 ms, ..., capped at 60 000 ms.

- [ASSUMPTION] `m_cfg.max_retries` is `ChannelConfig::max_retries`. In `Client.cpp` and
  `Server.cpp` this is set to 3.

- [ASSUMPTION] The `RetryManager` slot index `j` may differ from the `AckTracker` slot
  index `i` for the same message. Both are scanned independently and are not linked by index.

- [ASSUMPTION] `envelope_copy()` is safe to call with overlapping source/destination only if
  they are the same object; in this UC the source is always the caller's `env` and the
  destinations are distinct array entries, so there is no overlap.
