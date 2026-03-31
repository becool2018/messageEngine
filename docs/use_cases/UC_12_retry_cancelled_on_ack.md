# UC_12 — Retry cancelled on ACK receipt (RetryManager slot freed)

**HL Group:** HL-3 — Send with Automatic Retry / HL-10 — Pump the Retry Loop
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.2.5, REQ-3.3.3

---

## 1. Use Case Overview

**Trigger:** `DeliveryEngine::receive()` is called; the transport delivers an ACK envelope for a message that has an active entry in `RetryManager` (i.e., the original send used `RELIABLE_RETRY`).

**Goal:** Cancel the scheduled retry for the acknowledged message by locating the matching `RetryManager` slot (keyed on `env.source_id` and `env.message_id`), setting `active = false`, and decrementing `m_count`. Stop all future retransmissions for that message.

**Success outcome:** `RetryManager::on_ack()` finds the matching active slot, marks it inactive, decrements `m_count`, and logs `INFO` "Cancelled retry for message_id=...". The `AckTracker` slot for the same message is also marked `ACKED` (see UC_08). Future calls to `pump_retries()` will skip this slot because `active == false`.

**Error outcomes:**
- If no matching active slot exists (e.g., the retry was already cancelled by a prior ACK, the budget was exhausted, or the message was `RELIABLE_ACK` not `RELIABLE_RETRY`): `on_ack()` logs `WARNING_LO` "No retry entry found" and returns `ERR_INVALID`. The result is suppressed with `(void)`.
- If the transport returns non-OK before the ACK is processed, `receive()` returns early and `on_ack()` is never called.

---

## 2. Entry Points

**Primary entry:** Application thread calls:
```
// src/core/DeliveryEngine.cpp, line 149
Result DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
```

The retry cancellation is a side effect of ACK processing inside `receive()`; there is no separate entry point. See UC_08 for the parallel `AckTracker` transition that occurs on the same call.

---

## 3. End-to-End Control Flow

Steps 1–12 are shared with UC_08. This document focuses on the `RetryManager::on_ack()` path (step 12 in UC_08's numbering):

1. Application calls `DeliveryEngine::receive(env, timeout_ms, now_us)`.
2. Pre-condition asserts fire: `m_initialized`, `now_us > 0`.
3. Guard `!m_initialized` — engine is initialized; continues.
4. `m_transport->receive_message(env, timeout_ms)` — virtual dispatch returns `OK` with ACK envelope.
5. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))`.
6. `timestamp_expired(env.expiry_time_us, now_us)` returns `false` (ACK has `expiry_time_us = 0`).
7. `envelope_is_control(env)` returns `true`.
8. `env.message_type == MessageType::ACK` — true.
9. `m_ack_tracker.on_ack(env.source_id, env.message_id)` — transitions AckTracker slot to ACKED (UC_08).
10. `(void)ack_res` — result suppressed.
11. `m_retry_manager.on_ack(env.source_id, env.message_id)` — **this is the focus of UC_12**:
    a. Fires `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
    b. Fires `NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID)`.
    c. Linear scan `m_slots[0..ACK_TRACKER_CAPACITY-1]` (up to 32 iterations).
    d. For each slot: tests `m_slots[i].active && m_slots[i].env.source_id == src && m_slots[i].env.message_id == msg_id`.
    e. On match:
       - `m_slots[i].active = false`.
       - `--m_count`.
       - Fires post-condition assert: `NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)`.
       - Logs `INFO` "Cancelled retry for message_id=%llu from node=%u".
       - Returns `Result::OK`.
    f. If no match after full scan: logs `WARNING_LO` "No retry entry found for message_id=%llu from node=%u". Returns `Result::ERR_INVALID`.
12. `(void)retry_res` — result suppressed.
13. `receive()` logs `INFO` "Received ACK for message_id=%llu from src=%u".
14. Returns `Result::OK`.

---

## 4. Call Tree

```
DeliveryEngine::receive()
 ├── [steps 1-10: transport receive, expiry check, AckTracker update — see UC_08]
 │
 ├── RetryManager::on_ack(env.source_id, env.message_id)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID)
 │    ├── [loop i=0..31]
 │    │    └── if active && source_id match && message_id match:
 │    │         ├── m_slots[i].active = false
 │    │         ├── --m_count
 │    │         ├── NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
 │    │         └── Logger::log(INFO, "Cancelled retry ...")
 │    ├── [if no match] Logger::log(WARNING_LO, "No retry entry found ...")
 │    └── return Result::OK | Result::ERR_INVALID
 │
 ├── (void)retry_res
 ├── Logger::log(INFO, "Received ACK ...")
 └── return Result::OK
```

---

## 5. Key Components Involved

- **`DeliveryEngine::receive()`** — drives both `AckTracker` and `RetryManager` updates on ACK receipt. Owns the ordering: `AckTracker::on_ack()` first, then `RetryManager::on_ack()`.
- **`RetryManager::on_ack()`** — the sole mechanism for cancelling a scheduled retry. Performs a linear search keyed on `(source_id, message_id)` and marks the slot inactive.
- **`RetryManager::m_slots[]`** — the fixed-capacity retry table. After cancellation, the freed slot is immediately available for a new `schedule()` call.
- **`Logger::log()`** — provides both the success `INFO` trace and the `WARNING_LO` for the no-match case.

The `AckTracker` updates are covered by UC_08 and are not repeated in depth here; they occur on the same function call and in the same sequential order.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `m_transport->receive_message()` returns OK | Continue ACK path | Return `res` immediately | ACK dispatch |
| `envelope_is_control(env)` | Enter ACK/NAK/HB block | Data-message path (not this UC) | ACK type check |
| `env.message_type == ACK` | Call both `on_ack()` methods | NAK/HEARTBEAT pass-through | Return `OK` |
| `m_slots[i].active && ids match` | Cancel slot; return `OK` | Continue scan | Next slot or `ERR_INVALID` |
| `m_count > 0` before decrement | `--m_count` (implicit via `--`) | [not guarded — relies on invariant] | Assert post-condition |
| No match found after full scan | Log `WARNING_LO`; return `ERR_INVALID` | N/A | `(void)retry_res` in `receive()` |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** Single application thread calling `receive()`. `RetryManager::on_ack()` executes in-line.
- **No mutex or atomic operations** within `on_ack()`.
- **Race with `pump_retries()`:** Both `pump_retries()` (via `collect_due()`) and `receive()` (via `on_ack()`) modify `RetryManager::m_slots`. If called from separate threads simultaneously, a data race exists on `m_slots[i].active` and `m_count`. [ASSUMPTION] Single-threaded model is assumed.
- **Window between retransmit and cancel:** If `pump_retries()` fires a retry for slot `i` and, before the next call to `pump_retries()`, `receive()` calls `on_ack()` to cancel slot `i`, no further retransmission of that message will occur. The final retry that was just sent will produce a duplicate on the receiver side; the receiver's `DuplicateFilter` should suppress it for `RELIABLE_RETRY` messages.

---

## 8. Memory & Ownership Semantics

- **`RetryManager::m_slots[i].env`** — the message copy stored inside the `RetryEntry` struct is not zeroed on cancellation. `active = false` is the only state change. The stale envelope data remains in the slot until a new `schedule()` call overwrites it.
- **`RetryManager::m_slots[ACK_TRACKER_CAPACITY]`** — fixed array of 32 `RetryEntry` structs, each containing a full `MessageEnvelope` copy. No dynamic allocation.
- **Power of 10 Rule 3 confirmation:** `on_ack()` touches only pre-allocated state. No stack buffers allocated on this path.
- **Slot reuse:** After `active = false`, the slot is immediately available for the next `schedule()` call. The slot count in `m_count` is decremented, allowing new messages to be scheduled.

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `Result::OK` | Matching active slot found and cancelled | Slot `active = false`; `m_count` decremented | Normal; `receive()` returns `OK` |
| `Result::ERR_INVALID` | No active slot matching (src, msg_id) | No state modified | Suppressed with `(void)`; `receive()` still returns `OK`; `WARNING_LO` logged |
| Transport non-OK (e.g., `ERR_TIMEOUT`) | Transport `receive_message()` fails | `receive()` returns early; `on_ack()` never called | Caller polls again |

---

## 10. External Interactions

- `m_transport->receive_message(env, timeout_ms)` — as in UC_08, the only OS/network interaction.
- `RetryManager::on_ack()` itself has no OS interactions.
- The caller obtains `now_us` via `clock_gettime(CLOCK_MONOTONIC, ...)` before calling `receive()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RetryManager::m_slots[i].active` | `true` | `false` |
| `RetryManager::m_count` | M | M-1 |
| `RetryManager::m_slots[i].env` | message copy | unchanged (not zeroed) |
| `RetryManager::m_slots[i].retry_count` | R | unchanged |
| `RetryManager::m_slots[i].next_retry_us` | T | unchanged |
| `Logger` | log output | — | `INFO` "Cancelled retry for message_id=... from node=..."; `INFO` "Received ACK for message_id=..." |

`AckTracker` state changes are covered by UC_08 and occur in the same `receive()` call.

---

## 12. Sequence Diagram

```
Application     DeliveryEngine       AckTracker       RetryManager        Logger
    |                 |                   |                 |                |
    |--receive(env,t,n)->                  |                 |                |
    |                 |--receive_message()->                 |                |
    |                 |<-- OK + ACK env ---|                 |                |
    |                 |                   |                 |                |
    |                 |--on_ack(src,msg)-->|                 |                |
    |                 |                   | slot[i]=ACKED   |                |
    |                 |<--- OK ------------|                 |                |
    |                 |                   |                 |                |
    |                 |--on_ack(src,msg)---------------------->              |
    |                 |                   |  [scan 0..31]   |                |
    |                 |                   |  slot[j].active |                |
    |                 |                   |  = false        |                |
    |                 |                   |  --m_count      |                |
    |                 |                   |                 |--log(INFO, "Cancelled retry")-->
    |                 |<----------------------- OK ----------|                |
    |                 |                   |                 |                |
    |                 |--log(INFO, "Received ACK")------------------------------>
    |<-- Result::OK --|                   |                 |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `RetryManager::init()` zeroes all 32 slots and sets `m_count = 0`, `m_initialized = true`.
- A prior `DeliveryEngine::send()` call with `RELIABLE_RETRY` called `RetryManager::schedule()`, which set `active = true`, `retry_count = 0`, `max_retries`, `backoff_ms`, `next_retry_us`, `expiry_us`, and copied the envelope.

**Runtime (this UC):**
- Steady-state: `on_ack()` fires each time an ACK arrives for a `RELIABLE_RETRY` message.
- After cancellation, subsequent calls to `pump_retries()` scan the same slot but skip it because `active == false`. The scan still runs over the slot (O(32) traversal); it just costs a `!active` check.

---

## 14. Known Risks / Observations

- **Duplicate from the last retry:** If `pump_retries()` fires a retransmission for a slot moments before `on_ack()` cancels it, the remote receiver will see a duplicate. The receiver's `DuplicateFilter` (for `RELIABLE_RETRY` messages) suppresses this, but only if it still has the original (source_id, message_id) in its dedup window.
- **`RELIABLE_ACK` always triggers `WARNING_LO`:** For messages sent with `RELIABLE_ACK` (no retry entry in `RetryManager`), every received ACK causes a `WARNING_LO` log entry from `on_ack()`. This is expected behavior but adds log noise.
- **Stale slot data:** Cancelled slots are not zeroed. If there is a logic error elsewhere that reads `active == false` slots, stale envelope data may be observed.
- **`m_count` consistency:** After cancellation `m_count` is decremented. The post-condition assert checks `m_count <= ACK_TRACKER_CAPACITY`. There is no pre-cancellation guard against `m_count == 0` being decremented (the `--m_count` is unconditional in the match branch). [ASSUMPTION] The invariant that `m_count > 0` whenever an active slot exists is maintained by the paired `schedule()` / `on_ack()` calls and the init zeroing.
- **`ACK_TRACKER_CAPACITY` reused by both `AckTracker` and `RetryManager`:** Both use the same constant (32) for their slot arrays. A `RELIABLE_RETRY` send that fills one tracker also fills the other in lock-step.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The key `(source_id, message_id)` uniquely identifies a message in-flight. If a sender reuses a message_id before the prior ACK is received, a premature cancellation could occur.
- [ASSUMPTION] Single-threaded call model; `pump_retries()` and `receive()` are not called concurrently.
- [ASSUMPTION] The ACK envelope is constructed by `envelope_make_ack()` on the remote side, setting `source_id` to the original sender's node ID and `message_id` to the original message's ID. The match logic in `on_ack()` depends on this.
- [ASSUMPTION] `receive()` always calls both `AckTracker::on_ack()` and `RetryManager::on_ack()` regardless of the message's original `reliability_class`. There is no branching inside the ACK dispatch block that would skip `RetryManager::on_ack()` for `RELIABLE_ACK` messages. The `ERR_INVALID` return is the intended "not applicable" signal.
