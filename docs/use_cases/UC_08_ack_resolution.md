# UC_08 — ACK received: AckTracker slot transitions from PENDING → ACKED and is freed

**HL Group:** HL-2 — Send with Acknowledgement
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2

---

## 1. Use Case Overview

**Trigger:** `DeliveryEngine::receive()` is called by the application and the transport delivers a message envelope whose `message_type == MessageType::ACK`.

**Goal:** Locate the matching PENDING slot in `AckTracker` (keyed on `env.source_id` and `env.message_id`), mark it ACKED so the slot is released on the next sweep, and also cancel the corresponding `RetryManager` entry if one exists. Return `Result::OK` to the caller.

**Success outcome:** The `AckTracker` slot for the acknowledged message transitions from `PENDING` to `ACKED`. `m_count` in `AckTracker` is unchanged (ACKED slots are freed only on the next `sweep_expired()` call). The `RetryManager` slot (if present) is marked inactive and `m_count` decremented. The ACK envelope is consumed; the caller receives `Result::OK` but no data payload.

**Error outcomes:**
- If no matching PENDING slot exists in `AckTracker`, `on_ack()` returns `ERR_INVALID`. The return value is discarded with `(void)` — receive still returns `OK`.
- If `RetryManager::on_ack()` finds no matching retry entry (e.g., the message was `RELIABLE_ACK`), it returns `ERR_INVALID`, also suppressed.
- If the transport returns non-OK (timeout, I/O error), `receive()` returns early before any ACK processing.

---

## 2. Entry Points

**Primary entry:** Application thread calls:
```
// src/core/DeliveryEngine.cpp, line 149
Result DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
```

The application supplies:
- `env` — caller-owned `MessageEnvelope` on the stack (output buffer).
- `timeout_ms` — how long to block in the transport receive call.
- `now_us` — current monotonic time obtained by the caller via `timestamp_now_us()`.

ACK dispatch is embedded inside `receive()`; there is no separate ACK-only entry point.

---

## 3. End-to-End Control Flow

1. Application calls `DeliveryEngine::receive(env, timeout_ms, now_us)`.
2. `receive()` fires two `NEVER_COMPILED_OUT_ASSERT`s: `m_initialized == true` and `now_us > 0`.
3. Guard `!m_initialized` — since engine is initialized, execution continues past the early return.
4. `receive()` calls `m_transport->receive_message(env, timeout_ms)` via virtual dispatch on `TransportInterface`.
5. Transport fills `env` with the next available message and returns `Result::OK`. If non-OK (e.g., `ERR_TIMEOUT`), `receive()` returns `res` immediately; no ACK processing occurs.
6. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` fires — checks `message_type != INVALID`, `payload_length <= MSG_MAX_PAYLOAD_BYTES`, `source_id != NODE_ID_INVALID`.
7. `receive()` calls `timestamp_expired(env.expiry_time_us, now_us)`. ACK envelopes are built with `expiry_time_us = 0`; the predicate returns `false` (0 = never-expires). Execution continues.
8. `receive()` calls `envelope_is_control(env)`. Returns `true` because `env.message_type == MessageType::ACK`.
9. Inner branch: `env.message_type == MessageType::ACK` — true.
10. `receive()` calls `m_ack_tracker.on_ack(env.source_id, env.message_id)`:
    a. `on_ack()` fires `NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)`.
    b. Linear scan of `m_slots[0..ACK_TRACKER_CAPACITY-1]` (up to 32 iterations).
    c. For each slot: tests `state == PENDING`, `env.source_id == src`, `env.message_id == msg_id`.
    d. On match: sets `m_slots[i].state = EntryState::ACKED`. Fires post-condition assert confirming state is `ACKED`. Returns `Result::OK`.
    e. If no match found: fires second assert confirming `m_count <= ACK_TRACKER_CAPACITY`. Returns `Result::ERR_INVALID`.
11. `ack_res` is suppressed: `(void)ack_res`.
12. `receive()` calls `m_retry_manager.on_ack(env.source_id, env.message_id)`:
    a. `on_ack()` fires asserts: `m_initialized` and `src != NODE_ID_INVALID`.
    b. Linear scan of `m_slots[0..ACK_TRACKER_CAPACITY-1]`.
    c. For each slot: tests `active == true`, `env.source_id == src`, `env.message_id == msg_id`.
    d. On match: sets `m_slots[j].active = false`, decrements `m_count`. Fires post-condition assert: `m_count <= ACK_TRACKER_CAPACITY`. Logs INFO "Cancelled retry for message_id=... from node=...". Returns `Result::OK`.
    e. If no match: logs `WARNING_LO` "No retry entry found for...". Returns `Result::ERR_INVALID`.
13. `retry_res` is suppressed: `(void)retry_res`.
14. `receive()` logs `INFO` "Received ACK for message_id=%llu from src=%u".
15. `receive()` returns `Result::OK`. `env` output buffer contains the ACK envelope (not application data).

---

## 4. Call Tree

```
DeliveryEngine::receive()
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0)
 ├── m_transport->receive_message(env, timeout_ms)      [virtual dispatch]
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
 ├── timestamp_expired(env.expiry_time_us, now_us)      [inline, returns false]
 ├── envelope_is_control(env)                           [inline, returns true]
 ├── AckTracker::on_ack(env.source_id, env.message_id)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
 │    ├── [loop i=0..31] slot state/id match check
 │    ├── m_slots[i].state = EntryState::ACKED           [on match]
 │    └── NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == ACKED)
 ├── RetryManager::on_ack(env.source_id, env.message_id)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID)
 │    ├── [loop j=0..31] active/source/message_id match
 │    ├── m_slots[j].active = false; --m_count            [on match]
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
 │    └── Logger::log(INFO, "Cancelled retry ...")
 ├── Logger::log(INFO, "Received ACK ...")
 └── return Result::OK
```

---

## 5. Key Components Involved

- **`DeliveryEngine::receive()`** — top-level receive orchestrator; dispatches ACK handling after transport receive succeeds.
- **`TransportInterface::receive_message()`** — virtual method; provides the filled `MessageEnvelope`. Concrete implementation may be `TcpBackend`, `UdpBackend`, `LocalSimHarness`, or `DtlsUdpBackend`.
- **`timestamp_expired()`** — inline predicate in `Timestamp.hpp`; gates stale message drops before ACK dispatch. Returns `false` for ACK envelopes because `expiry_time_us == 0`.
- **`envelope_is_control()`** — inline predicate in `MessageEnvelope.hpp`; routes ACK envelopes away from data-message handling.
- **`AckTracker::on_ack()`** — transitions matched slot from `PENDING` to `ACKED`; does not free the slot immediately. Fulfills the reliable-ACK contract.
- **`RetryManager::on_ack()`** — cancels an active retry schedule for the same message. Only finds an entry for `RELIABLE_RETRY` messages; harmlessly returns `ERR_INVALID` for `RELIABLE_ACK`.
- **`Logger::log()`** — emits `INFO`-level trace records for the matched ACK and the cancelled retry.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `m_transport->receive_message()` returns `OK` | Continue to expiry check | Return `res` to caller immediately | Expiry check |
| `timestamp_expired(env.expiry_time_us, now_us)` | Return `ERR_EXPIRED` | Continue to control-message check | `envelope_is_control()` |
| `envelope_is_control(env)` | Enter ACK/control dispatch | Enter data-message block (not this UC) | ACK type check |
| `env.message_type == MessageType::ACK` | Call both `on_ack()` methods | Pass through (NAK, HEARTBEAT) | Return `OK` |
| `on_ack()` loop: slot `state == PENDING` and IDs match | Mark slot `ACKED`; return `OK` | Continue scan | Return `ERR_INVALID` if exhausted |
| `RetryManager::on_ack()` loop: active and IDs match | Set `active = false`, decrement `m_count`; return `OK` | Continue scan | Return `ERR_INVALID` if exhausted |
| `ack_res` or `retry_res` is non-OK | Suppressed with `(void)` — no action taken | N/A | Continue to log and return `OK` |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The single application event/service thread calling `DeliveryEngine::receive()`. All ACK processing runs in-line on that thread — no context switch.
- **No mutex or atomic operations** are used within `AckTracker::on_ack()` or `RetryManager::on_ack()`. Both structures are owned exclusively by the `DeliveryEngine` instance.
- **SPSC contract [ASSUMPTION]:** The design assumes a single-threaded caller. Concurrent calls to `receive()` and `pump_retries()` or `sweep_ack_timeouts()` from separate threads are not safe without external synchronization.
- **`m_count` in `AckTracker`:** The ACKED transition does not decrement `m_count`. Decrement occurs only in `sweep_one_slot()` called from `sweep_expired()`, which executes in `sweep_ack_timeouts()`.

---

## 8. Memory & Ownership Semantics

- **`MessageEnvelope& env`** — caller-allocated (typically stack). `receive()` fills it via the transport. ACK processing reads `env.source_id`, `env.message_id`, `env.message_type` but does not modify `env`.
- **`AckTracker::m_slots[ACK_TRACKER_CAPACITY]`** — fixed array of 32 `Entry` structs embedded inside `AckTracker`, which is embedded inside `DeliveryEngine`. Size: 32 × (sizeof(MessageEnvelope) + sizeof(uint64_t) + sizeof(EntryState)). No heap allocation.
- **`RetryManager::m_slots[ACK_TRACKER_CAPACITY]`** — fixed array of 32 `RetryEntry` structs embedded inside `RetryManager`, embedded inside `DeliveryEngine`. No heap allocation.
- **Power of 10 Rule 3 confirmation:** No dynamic allocation on this path. All state lives in fixed arrays initialized at `init()` time.
- **Object lifetimes:** `AckTracker` and `RetryManager` are value members of `DeliveryEngine` and live for the lifetime of the engine. Slot state is reused across message cycles via state-machine transitions.

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `Result::ERR_TIMEOUT` | Transport `receive_message()` returns `ERR_TIMEOUT` | No tracker state modified | Caller retries receive on next poll cycle |
| `Result::ERR_EXPIRED` | `timestamp_expired()` returns `true` before ACK dispatch | No tracker state modified; message discarded | Caller ignores; message is gone |
| `ERR_INVALID` from `AckTracker::on_ack()` | No PENDING slot matching (src, msg_id) found | `AckTracker` state unchanged | Suppressed; receive returns `OK`; stale ACK silently ignored |
| `ERR_INVALID` from `RetryManager::on_ack()` | No active retry entry for (src, msg_id) | `RetryManager` state unchanged | Suppressed; receive returns `OK`; normal for `RELIABLE_ACK` messages |

---

## 10. External Interactions

The only external interaction on this path is the virtual dispatch into the transport backend:
- `m_transport->receive_message(env, timeout_ms)` — implementation-dependent. For `TcpBackend`: calls `poll(2)` on connected fds then reads framed bytes. For `UdpBackend`: calls `recvfrom(2)`. For `LocalSimHarness`: reads from an in-process ring buffer.
- The caller invokes `timestamp_now_us()` before entering `receive()`, which calls `clock_gettime(CLOCK_MONOTONIC, ...)`.

`AckTracker::on_ack()` and `RetryManager::on_ack()` themselves have no OS or network interactions.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `AckTracker` | `m_slots[i].state` | `EntryState::PENDING` | `EntryState::ACKED` |
| `AckTracker` | `m_count` | N | N (unchanged; ACKED does not decrement) |
| `RetryManager` | `m_slots[j].active` | `true` | `false` (if retry entry existed) |
| `RetryManager` | `m_count` | M | M-1 (if retry entry existed) |
| `Logger` | log output | — | `INFO` "Cancelled retry for message_id=..." (if retry found); `INFO` "Received ACK for message_id=..." |

No persistent storage is modified. The `AckTracker` slot remains `ACKED` until the next `sweep_ack_timeouts()` call, at which point `sweep_one_slot()` transitions it to `FREE` and decrements `m_count`.

---

## 12. Sequence Diagram

```
Application       DeliveryEngine      TransportInterface   AckTracker         RetryManager
    |                   |                    |                  |                   |
    |--receive(env,t,n)->|                    |                  |                   |
    |                   |--receive_message(env,t)->              |                   |
    |                   |<------ OK + env filled (ACK) ---------|                   |
    |                   |                    |                  |                   |
    |                   |--envelope_valid(env)                  |                   |
    |                   |--timestamp_expired() => false         |                   |
    |                   |--envelope_is_control() => true        |                   |
    |                   |--env.type == ACK => true              |                   |
    |                   |                    |                  |                   |
    |                   |--on_ack(src, msg_id)----------------->|                   |
    |                   |                    |  [scan 0..31]    |                   |
    |                   |                    |  slot[i].state   |                   |
    |                   |                    |  = ACKED         |                   |
    |                   |<------------------ Result::OK --------|                   |
    |                   |                    |                  |                   |
    |                   |--on_ack(src, msg_id)------------------------------------->|
    |                   |                    |                  |  [scan 0..31]     |
    |                   |                    |                  |  slot[j].active   |
    |                   |                    |                  |  = false; m_count--|
    |                   |<---------------------------------------- Result::OK ------|
    |                   |                    |                  |                   |
    |                   |--Logger::log(INFO, "Received ACK")    |                   |
    |<-- Result::OK -----|                    |                  |                   |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `DeliveryEngine::init()` calls `m_ack_tracker.init()`: `memset`s all 32 slots to zero (state = `FREE`), sets `m_count = 0`.
- `DeliveryEngine::init()` calls `m_retry_manager.init()`: zeroes all retry slots, sets `m_count = 0`, `m_initialized = true`.
- A prior `DeliveryEngine::send()` call with `RELIABLE_ACK` or `RELIABLE_RETRY` called `AckTracker::track()`, which found a `FREE` slot, wrote the envelope and deadline, set state to `PENDING`.

**Runtime (this UC):**
- ACK arrives via transport on a steady-state `receive()` call.
- `on_ack()` in both `AckTracker` and `RetryManager` performs O(`ACK_TRACKER_CAPACITY`) = O(32) linear scans. No allocation, no recursion.
- After this UC the AckTracker slot remains `ACKED` until the next `sweep_ack_timeouts()` sweep.

---

## 14. Known Risks / Observations

- **ACKED-slot deferral:** The slot is marked `ACKED` but `m_count` is not decremented until `sweep_expired()` is called. If `sweep_ack_timeouts()` is not driven frequently, `ACKED` slots accumulate and `track()` will return `ERR_FULL` prematurely.
- **`ACK_TRACKER_CAPACITY = 32`:** If more than 32 reliable messages are in-flight simultaneously (including those in `ACKED`-but-not-swept state), new sends log `WARNING_HI` and ACK tracking silently fails for the excess messages.
- **Return values suppressed:** Stale ACKs (for already-freed or never-tracked messages) are silently ignored. The caller has no visibility into whether the ACK matched a tracked slot.
- **O(N) × 2 on every ACK:** Both `AckTracker::on_ack()` and `RetryManager::on_ack()` scan up to 32 slots each. Deterministic and bounded, but both full scans execute for every received ACK.
- **`RELIABLE_ACK` always logs `WARNING_LO` from RetryManager:** For `RELIABLE_ACK` sends there is no retry entry, so `RetryManager::on_ack()` always fails with `ERR_INVALID` and emits a `WARNING_LO` log entry on every ACK.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application calls `sweep_ack_timeouts()` periodically; otherwise `ACKED` slots are never freed and `m_count` never decreases, filling the tracker.
- [ASSUMPTION] The remote peer constructs the ACK envelope using `envelope_make_ack()`, setting `env.source_id` to the original sender's node ID and `env.message_id` to the original message's ID. The matching logic depends on this convention.
- [ASSUMPTION] A single application thread drives both `receive()` and `sweep_ack_timeouts()`. Concurrent access is not protected.
- [ASSUMPTION] `env.expiry_time_us == 0` for all ACK envelopes, as produced by `envelope_make_ack()`. A non-zero expired ACK would be dropped by `timestamp_expired()` before reaching `on_ack()`, leaving the PENDING slot to time out in the next sweep.
- [ASSUMPTION] `MessageIdGen` is seeded differently per node so that message IDs are unique per (source, session). A collision between two senders' IDs could cause a spurious ACKED transition.
