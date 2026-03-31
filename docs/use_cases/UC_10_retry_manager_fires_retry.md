# UC_10 — RetryManager fires a scheduled retry (backoff interval elapsed)

**HL Group:** HL-10 — Pump the Retry Loop
**Actor:** System
**Requirement traceability:** REQ-3.2.5, REQ-3.3.3

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::pump_retries(now_us)` and one or more `RetryManager` slots have `active == true`, `retry_count < max_retries`, `expiry_us` not yet reached, and `next_retry_us <= now_us`.

**Goal:** Re-transmit every due message via the transport, increment `retry_count`, double the backoff interval (capped at 60 s), update `next_retry_us`, and return the count of retransmitted messages.

**Success outcome:** Each due message is copied to a stack buffer and re-sent via `send_via_transport()`. The corresponding `RetryManager` slot is updated: `retry_count++`, `backoff_ms` doubled (or clamped to 60 000 ms), `next_retry_us` advanced to `now_us + new_backoff_us`. The return value is the number of messages retried.

**Error outcomes:**
- If a retry slot has `expiry_us` in the past (`slot_has_expired()` returns `true`): the slot is deactivated and `m_count` decremented; the message is not re-sent.
- If a retry slot has `retry_count >= max_retries`: the slot is deactivated and `m_count` decremented; the message is not re-sent (retry budget exhausted).
- If `send_via_transport()` returns non-OK for a due message: a `WARNING_LO` log entry is emitted but the `RetryManager` slot is still advanced (backoff updated, `retry_count` incremented). The retransmission is counted in the return value regardless.
- If the retry table is empty or no slots are due, the function returns 0.

---

## 2. Entry Points

**Primary entry:** Application event loop calls:
```
// src/core/DeliveryEngine.cpp, line 227
uint32_t DeliveryEngine::pump_retries(uint64_t now_us)
```

The application supplies:
- `now_us` — current monotonic time in microseconds from `timestamp_now_us()`.

`pump_retries()` must be called periodically by the application; it is not self-triggering.

---

## 3. End-to-End Control Flow

1. Application calls `DeliveryEngine::pump_retries(now_us)`.
2. `pump_retries()` fires `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(now_us > 0)`.
3. Declares stack buffer: `MessageEnvelope retry_buf[MSG_RING_CAPACITY]` (64 slots × sizeof(MessageEnvelope)).
4. Initializes `uint32_t collected = 0`.
5. Calls `RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)`.

   **Inside `RetryManager::collect_due()`:**
   5a. Fires three pre-condition asserts: `m_initialized`, `out_buf != nullptr`, `buf_cap <= MSG_RING_CAPACITY`.
   5b. Iterates over `m_slots[0..ACK_TRACKER_CAPACITY-1]` (32 slots) with loop guard `i < ACK_TRACKER_CAPACITY && collected < buf_cap`.
   5c. For each slot:
       - If `!m_slots[i].active`: `continue` (skip inactive slots).
       - If `slot_has_expired(m_slots[i].expiry_us, now_us)` returns `true` (and `expiry_us != 0` and `now_us >= expiry_us`): set `active = false`, decrement `m_count`, log `WARNING_LO` "Expired retry entry", `continue`.
       - If `m_slots[i].retry_count >= m_slots[i].max_retries`: set `active = false`, decrement `m_count`, log `WARNING_HI` "Exhausted retries", `continue`.
       - If `m_slots[i].next_retry_us <= now_us` (retry is due):
         - `envelope_copy(out_buf[collected], m_slots[i].env)` — copies the message to the output buffer.
         - `++collected`; `++m_slots[i].retry_count`.
         - `m_slots[i].backoff_ms = advance_backoff(m_slots[i].backoff_ms)` — doubles backoff, capped at 60 000 ms.
         - `backoff_us = (uint64_t)m_slots[i].backoff_ms * 1000ULL`.
         - `m_slots[i].next_retry_us = now_us + backoff_us`.
         - Logs `INFO` "Due for retry: message_id=..., attempt=..., next_backoff_ms=...".
   5d. Post-condition asserts: `collected <= buf_cap`, `m_count <= ACK_TRACKER_CAPACITY`.
   5e. Returns `collected`.

6. `pump_retries()` stores returned count in `collected`.
7. `NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY)`.
8. Iterates over `retry_buf[0..collected-1]` (Power of 10: bounded loop).
   For each `retry_buf[i]`:
   8a. Calls `DeliveryEngine::send_via_transport(retry_buf[i])`:
       - Fires three asserts: `m_initialized`, `m_transport != nullptr`, `envelope_valid(retry_buf[i])`.
       - Calls `m_transport->send_message(retry_buf[i])` via virtual dispatch.
       - If non-OK: logs `WARNING_LO` "Transport send failed for message_id=...". Returns non-OK result.
   8b. In `pump_retries()`: if `send_via_transport()` returns non-OK, logs `WARNING_LO` "Retry send failed for message_id=...". If OK, logs `INFO` "Retried message_id=...".
9. Post-condition assert: `collected <= MSG_RING_CAPACITY`.
10. Returns `collected`.

---

## 4. Call Tree

```
DeliveryEngine::pump_retries(now_us)
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0)
 ├── [stack] MessageEnvelope retry_buf[64]
 ├── RetryManager::collect_due(now_us, retry_buf, 64)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)
 │    ├── NEVER_COMPILED_OUT_ASSERT(buf_cap <= MSG_RING_CAPACITY)
 │    ├── [loop i=0..31]
 │    │    ├── slot_has_expired(expiry_us, now_us)   [static helper]
 │    │    │    └── return (expiry_us != 0) && (now_us >= expiry_us)
 │    │    ├── [if expired] active=false; --m_count; Logger::log(WARNING_LO)
 │    │    ├── [if exhausted] active=false; --m_count; Logger::log(WARNING_HI)
 │    │    └── [if due] envelope_copy(); ++collected; ++retry_count
 │    │         ├── advance_backoff(current_ms)       [static helper]
 │    │         │    └── doubled = current*2; cap at 60000; return
 │    │         ├── next_retry_us = now_us + backoff_us
 │    │         └── Logger::log(INFO, "Due for retry")
 │    └── return collected
 ├── NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY)
 ├── [loop i=0..collected-1]
 │    └── DeliveryEngine::send_via_transport(retry_buf[i])
 │         ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │         ├── NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)
 │         ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(retry_buf[i]))
 │         ├── m_transport->send_message(retry_buf[i])   [virtual dispatch]
 │         └── Logger::log(WARNING_LO, ...) [on failure]
 ├── Logger::log(INFO|WARNING_LO, "Retried|failed message_id=...")
 ├── NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY)
 └── return collected
```

---

## 5. Key Components Involved

- **`DeliveryEngine::pump_retries()`** — application-facing retry pump; coordinates collection and re-transmission.
- **`RetryManager::collect_due()`** — scans the retry table, identifies due entries, copies them to the output buffer, advances backoff, deactivates expired/exhausted entries.
- **`slot_has_expired()`** (`RetryManager.cpp`, file-static) — pure predicate; returns `true` when `expiry_us != 0` and `now_us >= expiry_us`. Guards against infinite retry of expired messages.
- **`advance_backoff()`** (`RetryManager.cpp`, file-static) — doubles current backoff in milliseconds; caps at 60 000 ms (60 s) to prevent infinite growth.
- **`envelope_copy()`** (`MessageEnvelope.hpp`) — `memcpy` of one full `MessageEnvelope` struct; used to copy the envelope from the retry slot into the stack buffer.
- **`DeliveryEngine::send_via_transport()`** — private helper; calls `m_transport->send_message()` and logs failures.
- **`TransportInterface::send_message()`** — virtual dispatch; the actual I/O operation.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `!m_slots[i].active` | `continue` (skip) | Test expiry | Expiry check |
| `slot_has_expired(expiry_us, now_us)` | Deactivate slot; `WARNING_LO` log; `continue` | Test retry budget | Budget check |
| `retry_count >= max_retries` | Deactivate slot; `WARNING_HI` log; `continue` | Test if due | Due-time check |
| `next_retry_us <= now_us` | Copy to buffer; increment count; advance backoff | No action (not yet due) | Next slot |
| `advance_backoff`: `doubled > 60000U` | Return 60 000 (cap) | Return doubled value | Assign to `backoff_ms` |
| `send_via_transport()` returns non-OK | Log `WARNING_LO` "Retry send failed" | Log `INFO` "Retried" | Next iteration |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The application event loop thread calling `pump_retries()`. All work is in-line; no context switch.
- **No mutex or atomic operations** within `collect_due()` or `advance_backoff()`.
- **[ASSUMPTION]** Single-threaded application model. Calling `pump_retries()` concurrently with `receive()` from a second thread is not safe; both can modify `RetryManager::m_slots` (via `on_ack()` in `receive()` and `collect_due()` in `pump_retries()`).
- **Stack buffer lifetime:** `retry_buf[MSG_RING_CAPACITY]` is allocated on the calling thread's stack for the duration of `pump_retries()` and freed on return. The `RetryManager` slots are the original; `collect_due()` copies envelopes out, so the retry slot remains the authoritative copy.

---

## 8. Memory & Ownership Semantics

- **`retry_buf[MSG_RING_CAPACITY]`** — stack-allocated array of 64 `MessageEnvelope` structs. Size: `64 × sizeof(MessageEnvelope)` = 64 × (3 × 8 + 4 + 4 + 4 + 4096 + …) bytes. This is the largest stack allocation on the `pump_retries()` path. [ASSUMPTION] Stack depth is sufficient; see `docs/STACK_ANALYSIS.md` for the tracked worst-case.
- **`RetryManager::m_slots[ACK_TRACKER_CAPACITY]`** — fixed array of 32 `RetryEntry` structs embedded in `RetryManager`, which is embedded in `DeliveryEngine`. Each `RetryEntry` contains a full `MessageEnvelope` copy.
- **Envelope copies:** `collect_due()` copies each due envelope into `retry_buf` via `envelope_copy()` (`memcpy`). The `RetryManager` slot retains its copy (the `active` flag is not cleared; the slot is still live for future retries unless expiry or budget is hit).
- **Power of 10 Rule 3 confirmation:** No dynamic allocation on this path. All buffers are stack or static-scope.

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `send_via_transport()` non-OK | Transport `send_message()` returns non-OK | Retry slot still active; backoff already advanced; `retry_count` already incremented — message will be retried again after next backoff | Log `WARNING_LO`; caller sees count includes the failed send |
| Slot expired (`slot_has_expired()`) | `now_us >= expiry_us` for active slot | Slot deactivated; `m_count` decremented; message dropped | Log `WARNING_LO`; slot freed silently |
| Retry budget exhausted | `retry_count >= max_retries` | Slot deactivated; `m_count` decremented; message dropped | Log `WARNING_HI`; caller has no further visibility |
| `collected > MSG_RING_CAPACITY` | Logic error in `collect_due()` | `NEVER_COMPILED_OUT_ASSERT` fires | Abort (debug) or `on_fatal_assert()` (production) |

---

## 10. External Interactions

- `m_transport->send_message(retry_buf[i])` — virtual dispatch. For `TcpBackend`: `write(2)` or `send(2)` on a socket fd. For `UdpBackend`: `sendto(2)`. For `LocalSimHarness`: in-process ring buffer push.
- The caller invokes `clock_gettime(CLOCK_MONOTONIC, ...)` via `timestamp_now_us()` before calling `pump_retries()`.
- No additional OS calls inside `collect_due()` or `advance_backoff()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RetryManager::m_slots[i]` | `retry_count` | R | R+1 (for each due slot) |
| `RetryManager::m_slots[i]` | `backoff_ms` | B | `min(B*2, 60000)` |
| `RetryManager::m_slots[i]` | `next_retry_us` | T | `now_us + new_backoff_us` |
| `RetryManager::m_slots[j]` | `active` | `true` | `false` (for expired or exhausted slots) |
| `RetryManager` | `m_count` | M | M - (count of deactivated slots) |
| `Logger` | log output | — | `INFO` "Due for retry: message_id=..." per due slot; `WARNING_LO/HI` for expired/exhausted; `INFO` or `WARNING_LO` per send attempt |

No persistent storage is modified. Changes to the transport (actual bytes on the wire) are side effects of `send_message()`.

---

## 12. Sequence Diagram

```
Application     DeliveryEngine    RetryManager        advance_backoff()   TransportInterface
    |                 |                 |                      |                  |
    |--pump_retries(now_us)->           |                      |                  |
    |                 |--collect_due(now_us, buf, 64)->        |                  |
    |                 |                 | [scan 0..31]         |                  |
    |                 |                 | slot_has_expired()?  |                  |
    |                 |                 | retry_count >= max?  |                  |
    |                 |                 | next_retry_us<=now?  |                  |
    |                 |                 |  envelope_copy()     |                  |
    |                 |                 |  ++retry_count       |                  |
    |                 |                 |--advance_backoff()-->|                  |
    |                 |                 |<-- new_backoff_ms ---|                  |
    |                 |                 |  next_retry_us=now+  |                  |
    |                 |                 |    new_backoff_us    |                  |
    |                 |<--- collected --|                      |                  |
    |                 |                 |                      |                  |
    |                 | [loop i=0..collected-1]                |                  |
    |                 |--send_via_transport(retry_buf[i])----->|                  |
    |                 |                 |                      |--send_message(env)->
    |                 |                 |                      |<--- Result::OK --|
    |                 |--Logger::log(INFO, "Retried ...")      |                  |
    |<-- collected ---|                 |                      |                  |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `RetryManager::init()` zeroes all 32 slots (`active = false`, all fields zero), sets `m_count = 0`, `m_initialized = true`.
- A prior `DeliveryEngine::send()` with `RELIABLE_RETRY` called `RetryManager::schedule()`, which set `active = true`, `retry_count = 0`, `max_retries`, `backoff_ms`, `next_retry_us = now_us` (first retry is immediate), `expiry_us = env.expiry_time_us`.

**Runtime (this UC):**
- On the first `pump_retries()` call after `schedule()`: `next_retry_us` equals the `now_us` at schedule time, so `now_us >= next_retry_us` is immediately true. The first retry fires right away.
- On subsequent calls: `next_retry_us` is `now_us_at_last_retry + backoff_us`. The message is skipped until enough time elapses.
- `collect_due()` is O(32) per call regardless of how many slots are active.

---

## 14. Known Risks / Observations

- **Large stack allocation:** `retry_buf[MSG_RING_CAPACITY]` is 64 × sizeof(MessageEnvelope). With `MSG_MAX_PAYLOAD_BYTES = 4096`, each envelope is ~4137 bytes; 64 envelopes is ~265 KB on the stack. This is the dominant stack consumer in the call chain. See `docs/STACK_ANALYSIS.md` for the tracked analysis.
- **`ACK_TRACKER_CAPACITY = 32` shared between `AckTracker` and `RetryManager`:** Both use this constant as their slot count. If 32 `RELIABLE_RETRY` messages are in-flight, the `RetryManager` is full and new sends silently drop their retry schedules.
- **Backoff advances even on transport failure:** If `send_via_transport()` fails, the backoff is still doubled and `retry_count` is still incremented (both happen inside `collect_due()` before the send). The slot is not reset to its pre-advance state.
- **No ACK-tracker cleanup on retry budget exhaustion:** When the retry budget is exhausted inside `collect_due()`, the `RetryManager` slot is freed but the corresponding `AckTracker` slot (if any) remains `PENDING` until `sweep_ack_timeouts()` times it out.
- **`MSG_RING_CAPACITY = 64` vs `ACK_TRACKER_CAPACITY = 32`:** The stack buffer is sized for 64 envelopes, but the retry table has only 32 slots. `collected` can never exceed 32. The buffer is over-provisioned by 2×.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application calls `pump_retries()` at a rate appropriate for the configured `backoff_ms`. If the call interval is longer than the shortest backoff, multiple retries may fire in a single call.
- [ASSUMPTION] Stack depth is sufficient for `retry_buf[64]`. This has been analyzed and documented in `docs/STACK_ANALYSIS.md`.
- [ASSUMPTION] Single-threaded call model. `collect_due()` modifies `RetryManager` state; concurrent `receive()` calling `RetryManager::on_ack()` on a different thread would cause a data race.
- [ASSUMPTION] `env.expiry_time_us` in the `RetryEntry` matches the value in the original envelope. If it is 0 (never-expires), `slot_has_expired()` always returns `false` and the slot is only removed by retry budget exhaustion or an explicit `on_ack()`.
- [ASSUMPTION] `advance_backoff()` receives only values `<= 60000U` because the slot's `backoff_ms` is initialized from `m_cfg.retry_backoff_ms` and is always capped on each advance. The pre-condition assert enforces this.
