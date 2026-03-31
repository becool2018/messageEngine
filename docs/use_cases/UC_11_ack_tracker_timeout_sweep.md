# UC_11 — AckTracker sweep: PENDING entries past deadline_us collected as expired

**HL Group:** HL-11 — Sweep ACK Timeouts
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-7.2.3

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::sweep_ack_timeouts(now_us)` and one or more `AckTracker` slots are in `PENDING` state with `deadline_us <= now_us`.

**Goal:** Identify all `PENDING` slots whose ACK deadline has passed, copy their envelopes to a stack buffer, transition those slots to `FREE`, decrement `m_count`, and return the count of timed-out messages. Also opportunistically free any `ACKED` slots encountered during the scan. Log each timeout at `WARNING_HI`.

**Success outcome:** All expired `PENDING` slots are transitioned to `FREE` and their envelopes are available in the stack buffer. All `ACKED` slots encountered are also transitioned to `FREE`. `m_count` is decremented for every freed slot. The return value is the count of expired `PENDING` entries. The caller can inspect the returned count and trigger application-level failure handling (e.g., report unacknowledged sends).

**Error outcomes:**
- If no slots are expired or `ACKED`, `sweep_ack_timeouts()` returns 0. No state is modified beyond the scan.
- If the output buffer (`timeout_buf`) is smaller than the number of expired slots, some expired envelopes are not copied (their slots are still freed), and the return count is capped at `buf_cap`. [ASSUMPTION] In practice `buf_cap == ACK_TRACKER_CAPACITY == 32`, so this cannot happen.

---

## 2. Entry Points

**Primary entry:** Application event loop calls:
```
// src/core/DeliveryEngine.cpp, line 267
uint32_t DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)
```

The application supplies:
- `now_us` — current monotonic time in microseconds from `timestamp_now_us()`.

`sweep_ack_timeouts()` must be called periodically by the application; it is not self-triggering.

---

## 3. End-to-End Control Flow

1. Application calls `DeliveryEngine::sweep_ack_timeouts(now_us)`.
2. `sweep_ack_timeouts()` fires `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(now_us > 0)`.
3. Declares stack buffer: `MessageEnvelope timeout_buf[ACK_TRACKER_CAPACITY]` (32 slots × sizeof(MessageEnvelope)).
4. Initializes `uint32_t collected = 0`.
5. Calls `AckTracker::sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY)`.

   **Inside `AckTracker::sweep_expired()`:**
   5a. Fires pre-condition asserts: `expired_buf != nullptr`, `m_count <= ACK_TRACKER_CAPACITY`.
   5b. Iterates over `m_slots[0..ACK_TRACKER_CAPACITY-1]` (32 slots, Power of 10 bounded).
   5c. For each slot: calls `sweep_one_slot(i, now_us, expired_buf, buf_cap, expired_count)`.

   **Inside `AckTracker::sweep_one_slot(idx, now_us, expired_buf, buf_cap, expired_count)`:**
   - Pre-condition asserts: `idx < ACK_TRACKER_CAPACITY`, `expired_buf != nullptr`.
   - **Branch A — expired PENDING:**
     - `m_slots[idx].state == PENDING` AND `now_us >= m_slots[idx].deadline_us`.
     - If `expired_count < buf_cap`: calls `envelope_copy(expired_buf[expired_count], m_slots[idx].env)` — copies envelope to output; `added = 1`.
     - Sets `m_slots[idx].state = EntryState::FREE`.
     - If `m_count > 0`: decrements `m_count`.
     - Returns `added` (1 if copied, 0 if buffer full).
   - **Branch B — ACKED slot:**
     - `m_slots[idx].state == ACKED`.
     - Sets `m_slots[idx].state = EntryState::FREE`.
     - If `m_count > 0`: decrements `m_count`.
     - Returns 0 (ACKED slots are not placed in the output buffer).
   - **Branch C — FREE or non-expired PENDING:**
     - Returns 0. No state modification.

   5d. `expired_count` is accumulated via the returned values from `sweep_one_slot()`.
   5e. Post-condition asserts: `m_count <= ACK_TRACKER_CAPACITY`, `expired_count <= buf_cap`.
   5f. Returns `expired_count`.

6. `sweep_ack_timeouts()` stores returned count in `collected`.
7. `NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY)`.
8. Iterates over `timeout_buf[0..collected-1]` (Power of 10: bounded loop). For each expired envelope, logs `WARNING_HI` "ACK timeout for message_id=%llu sent to node=%u".
9. Post-condition assert: `NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY)`.
10. Returns `collected`.

---

## 4. Call Tree

```
DeliveryEngine::sweep_ack_timeouts(now_us)
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0)
 ├── [stack] MessageEnvelope timeout_buf[32]
 ├── AckTracker::sweep_expired(now_us, timeout_buf, 32)
 │    ├── NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
 │    ├── [loop i=0..31]
 │    │    └── AckTracker::sweep_one_slot(i, now_us, buf, 32, expired_count)
 │    │         ├── NEVER_COMPILED_OUT_ASSERT(idx < ACK_TRACKER_CAPACITY)
 │    │         ├── NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr)
 │    │         ├── [PENDING + expired] envelope_copy(); state=FREE; --m_count; return 1
 │    │         ├── [ACKED]             state=FREE; --m_count; return 0
 │    │         └── [FREE or !expired]  return 0
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
 │    ├── NEVER_COMPILED_OUT_ASSERT(expired_count <= buf_cap)
 │    └── return expired_count
 ├── NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY)
 ├── [loop i=0..collected-1]
 │    └── Logger::log(WARNING_HI, "ACK timeout for message_id=...")
 ├── NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY)
 └── return collected
```

---

## 5. Key Components Involved

- **`DeliveryEngine::sweep_ack_timeouts()`** — application-facing sweep; coordinates collection and logging.
- **`AckTracker::sweep_expired()`** — outer loop driver; iterates all slots and delegates to `sweep_one_slot()`.
- **`AckTracker::sweep_one_slot()`** — private per-slot state machine; the only function that transitions slots to `FREE` and decrements `m_count`. Handles both expired-PENDING and ACKED transitions in one pass.
- **`envelope_copy()`** — `memcpy` of one `MessageEnvelope`; copies expired envelopes into the output buffer for caller inspection.
- **`Logger::log()`** — emits `WARNING_HI` per expired entry; the application's primary notification mechanism for lost ACKs.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `slot.state == PENDING && now_us >= deadline_us` | Copy to output buffer (if space); set `FREE`; `--m_count`; return 1 | Check if ACKED | ACKED check |
| `expired_count < buf_cap` (inside Branch A) | Copy envelope to output buffer; `added = 1` | Do not copy (buffer full); `added = 0` | Set `FREE`; decrement |
| `slot.state == ACKED` | Set `FREE`; `--m_count`; return 0 | No action (FREE or non-expired PENDING); return 0 | Next slot |
| `m_count > 0` (before decrement) | Decrement `m_count` | Do not decrement (guard against underflow) | Continue |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The single application event loop thread calling `sweep_ack_timeouts()`. All work is in-line.
- **No mutex or atomic operations** within `sweep_expired()` or `sweep_one_slot()`.
- **[ASSUMPTION]** Single-threaded call model. Concurrent calls to `sweep_ack_timeouts()` and `receive()` from separate threads would both modify `AckTracker` state (`on_ack()` in `receive()` sets `ACKED`; `sweep_one_slot()` sets `FREE`) creating a data race on `m_slots[i].state` and `m_count`.
- **Order dependency:** `sweep_one_slot()` is the only mechanism that returns slots to `FREE`. If `receive()` sets a slot to `ACKED` between two `sweep_ack_timeouts()` calls, that slot will be freed during the next sweep (Branch B) and will not appear in the output buffer.

---

## 8. Memory & Ownership Semantics

- **`timeout_buf[ACK_TRACKER_CAPACITY]`** — stack-allocated array of 32 `MessageEnvelope` structs on the `sweep_ack_timeouts()` frame. Size: 32 × sizeof(MessageEnvelope). Freed on function return.
- **`AckTracker::m_slots[ACK_TRACKER_CAPACITY]`** — fixed array of 32 `Entry` structs (each containing a `MessageEnvelope`, `uint64_t deadline_us`, and `EntryState`) embedded in `AckTracker`, embedded in `DeliveryEngine`. No heap allocation.
- **Envelope copies:** `sweep_one_slot()` copies expired envelopes into `timeout_buf` via `envelope_copy()`. The slot's own `env` field is not cleared on transition to `FREE` — it retains stale data until overwritten by the next `track()` call.
- **Power of 10 Rule 3 confirmation:** No dynamic allocation on this path.
- **`m_count` semantics:** Counts the number of non-FREE slots (PENDING + ACKED). After sweep it reflects only PENDING slots not yet expired plus any that arrived between this and the prior sweep.

---

## 9. Error Handling Flow

| Result code / condition | Trigger | System state after | Caller action |
|-------------------------|---------|-------------------|---------------|
| Returns 0 | No expired PENDING or ACKED slots found | No state modified | Caller continues polling |
| `expired_count < actual expired` | More expired slots than `buf_cap` (cannot occur if `buf_cap == ACK_TRACKER_CAPACITY`) | All expired slots are freed; some envelopes not copied | [ASSUMPTION] Not reachable; `buf_cap` is always 32 |
| `NEVER_COMPILED_OUT_ASSERT(collected > ACK_TRACKER_CAPACITY)` fires | Logic error in `sweep_expired()` | Abort (debug) or `on_fatal_assert()` (production) | N/A |
| `WARNING_HI` log per expired slot | Normal operation — ACK not received before deadline | Slot freed; caller sees count > 0 | Application may retry, report failure, or reset peer connection |

---

## 10. External Interactions

- No OS or network calls occur inside `sweep_expired()` or `sweep_one_slot()`.
- The caller invokes `clock_gettime(CLOCK_MONOTONIC, ...)` via `timestamp_now_us()` before calling `sweep_ack_timeouts()`.
- `Logger::log()` writes to the configured log sink (implementation-defined; [ASSUMPTION] synchronous write to stderr or a file).

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `AckTracker::m_slots[i].state` | expired PENDING slot | `EntryState::PENDING` | `EntryState::FREE` |
| `AckTracker::m_slots[j].state` | ACKED slot (opportunistic) | `EntryState::ACKED` | `EntryState::FREE` |
| `AckTracker::m_count` | per freed slot | N | N-1 for each freed slot (PENDING or ACKED) |
| `timeout_buf[k].env` (stack) | — | undefined | copy of expired envelope |
| `Logger` | log output | — | `WARNING_HI` "ACK timeout for message_id=..., sent to node=..." per expired slot |

`RetryManager` is not touched by this function. The corresponding retry slot (if the expired message had `RELIABLE_RETRY` semantics) remains active until it is naturally cleaned up by `collect_due()` in `pump_retries()`.

---

## 12. Sequence Diagram

```
Application       DeliveryEngine      AckTracker          sweep_one_slot()    Logger
    |                   |                  |                     |               |
    |--sweep_ack_timeouts(now_us)->         |                     |               |
    |                   |                  |                     |               |
    |                   |--sweep_expired(now,buf,32)->           |               |
    |                   |                  | [loop i=0..31]      |               |
    |                   |                  |--sweep_one_slot(i,now,buf,32,cnt)->  |
    |                   |                  |                     | check PENDING  |
    |                   |                  |                     | + deadline     |
    |                   |                  |                     | envelope_copy()|
    |                   |                  |                     | state=FREE     |
    |                   |                  |                     | --m_count      |
    |                   |                  |<-- return 1 --------|               |
    |                   |                  |  (repeat for all slots)             |
    |                   |<--- expired_count|                     |               |
    |                   |                  |                     |               |
    |                   | [loop i=0..expired_count-1]            |               |
    |                   |--Logger::log(WARNING_HI, "ACK timeout")->             |
    |                   |                  |                     |    (write log) |
    |<-- collected ------|                  |                     |               |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `AckTracker::init()` `memset`s all 32 slots to zero (state = `FREE`), sets `m_count = 0`. Post-condition asserts verify all slots are `FREE`.
- A prior `AckTracker::track()` call placed one or more slots into `PENDING` state with a `deadline_us`.

**Runtime (this UC):**
- Steady-state: called periodically from the application loop. O(32) linear scan each call.
- Also opportunistically frees `ACKED` slots during the same pass, keeping `m_count` accurate without a separate cleanup step.
- `sweep_one_slot()` is the only function that can transition `PENDING → FREE` or `ACKED → FREE`. All other transitions (`FREE → PENDING` via `track()`, `PENDING → ACKED` via `on_ack()`) go in the other direction.

---

## 14. Known Risks / Observations

- **Dual-purpose sweep:** `sweep_expired()` frees both expired-PENDING slots (returning them to caller) and ACKED slots (silent cleanup). A caller reading the return count sees only PENDING timeouts, not ACKED frees. This is correct behavior but can be confusing during debugging.
- **`m_count` underflow guard:** `sweep_one_slot()` guards `if (m_count > 0)` before decrementing. If `m_count` is somehow zero when a slot is freed, the decrement is silently skipped rather than wrapping. This masks a potential invariant violation.
- **Stale data in freed slots:** After `state = FREE`, the `env` field and `deadline_us` in the freed slot are not zeroed. A subsequent `track()` call overwrites them, but a bug that reads a `FREE` slot could see stale data.
- **No notification to application:** The application learns about timeouts only via the return count and `WARNING_HI` log entries. There is no callback or event mechanism. The caller must read `timeout_buf` contents to know which messages timed out.
- **RetryManager not cleaned up:** When an `AckTracker` slot for a `RELIABLE_RETRY` message expires here, the corresponding `RetryManager` slot is not freed. The retry manager will continue retrying until it too expires or exhausts its budget. This is by design (retry should outlive a single ACK timeout window) but means the two trackers can drift out of sync.
- **`ACK_TRACKER_CAPACITY = 32`:** If 32 messages time out simultaneously, `collected = 32` and the stack buffer is fully utilized. The buffer is sized exactly to `ACK_TRACKER_CAPACITY`, so no overflow is possible.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application calls `sweep_ack_timeouts()` frequently enough that the gap between sweeps is smaller than the smallest configured `recv_timeout_ms`. If sweeps are infrequent, multiple messages may pile up as expired simultaneously.
- [ASSUMPTION] Single-threaded call model. No concurrent modification of `AckTracker` state.
- [ASSUMPTION] `deadline_us` is set relative to the same `CLOCK_MONOTONIC` epoch as the `now_us` passed to this function. Cross-node deadline sharing is not supported.
- [ASSUMPTION] The caller reads `timeout_buf` and acts on the expired envelopes before calling `sweep_ack_timeouts()` again, since the stack buffer is only valid for the current call's duration.
- [ASSUMPTION] `RetryManager` is permitted to remain active for a message even after `AckTracker` has freed its slot. The two components are intentionally decoupled: retry budget and ACK timeout are independent policies.
