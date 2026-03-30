# UC_03 — Reliable-with-Retry Send

**HL Group:** HL-3 — User sends a message requiring guaranteed delivery
**Actor:** User
**Requirement traceability:** REQ-3.3.3, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.2.1, REQ-3.2.3, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

### Clear description of what triggers this flow

A client application populates a `MessageEnvelope` with `message_type = DATA`,
`reliability_class = RELIABLE_RETRY`, fills in `destination_id`, `priority`,
`expiry_time_us`, `payload`, and `payload_length`, then calls
`DeliveryEngine::send(env, now_us)`. The engine is initialized with a
`TcpBackend` instance and a `ChannelConfig` that supplies `recv_timeout_ms`,
`max_retries`, and `retry_backoff_ms`. The TCP connection to the server peer
was established during `TcpBackend::init()`.

This use case covers the send phase, initial transport transmission, ACK
tracking registration, retry scheduling, and the retry pump loop. The ACK
cancellation path is documented in UC_12. The deduplication path on the
receiver side is part of UC_09 / UC_07.

### Expected outcome (single goal)

The envelope is stamped, sent over TCP, registered in both `AckTracker` (as
`PENDING`) and `RetryManager` (as `active = true` with `next_retry_us = now_us`
for immediate first retry). On each subsequent `pump_retries(now_us)` call, the
`RetryManager::collect_due()` checks expiry, exhaustion, and due-time for each
active slot. Due slots are copied to an output buffer, their `retry_count` is
incremented, and the backoff is doubled via `advance_backoff()` (capped at 60
seconds). Each collected envelope is retransmitted via `send_via_transport()`.
This continues until an ACK arrives (cancels the retry slot via
`RetryManager::on_ack()`), the retry count reaches `max_retries` (exhaustion),
or `expiry_time_us` passes (expiry).

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point (initial send):** `DeliveryEngine::send(MessageEnvelope& env,
  uint64_t now_us)` — `DeliveryEngine.cpp` line 77.
- **Secondary entry point (retry pump):** `DeliveryEngine::pump_retries(uint64_t now_us)`
  — `DeliveryEngine.cpp` line 227. Called periodically by the application loop.
- **Tertiary entry point (ACK cancels retry):** `DeliveryEngine::receive(MessageEnvelope&
  env, uint32_t timeout_ms, uint64_t now_us)` — `DeliveryEngine.cpp` line 149.
  When the received envelope is an ACK, calls `RetryManager::on_ack()`.
- **Pre-condition for send:** `DeliveryEngine::init()` completed, `m_initialized = true`.
  `AckTracker` and `RetryManager` both initialized (all slots `FREE` / `inactive`).

### Example: main(), ISR, callback, RPC handler, etc.

Application calls `DeliveryEngine::send()` from its main loop, then enters a
receive/pump loop. No ISR or RPC mechanism is present in the codebase.

---

## 3. End-to-End Control Flow (Step-by-Step)

**--- Phase A: Transport send (identical to UC_01 steps 1–7) ---**

1. **Application** constructs `MessageEnvelope` with
   `reliability_class = RELIABLE_RETRY`, sets `expiry_time_us` (e.g.,
   `timestamp_deadline_us(now_us, 5000U) = now_us + 5 000 000 µs`), and calls
   `DeliveryEngine::send(env, now_us)`.

2. **`DeliveryEngine::send()`** fires precondition assertions. Stamps
   `env.source_id = m_local_id`, `env.message_id = m_id_gen.next()`,
   `env.timestamp_us = now_us`.

3. Calls `send_via_transport(env)`. This dispatches to
   `TcpBackend::send_message()`, which serializes the envelope, runs impairment
   evaluation (default: passthrough with immediate delay), sends via
   `send_to_all_clients()` + `flush_delayed_to_clients()`, and returns
   `Result::OK`. (Full detail in UC_01 steps 4–23.)

4. **`DeliveryEngine::send()`** checks `res != Result::OK` (line 94, passes).

**--- Phase B: ACK tracking registration ---**

5. Reliability class conditional at line 102: `RELIABLE_RETRY` satisfies
   `(RELIABLE_ACK || RELIABLE_RETRY)`. **Branch taken.**

6. Computes `ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms)`
   (e.g., `now_us + 100 000 µs` if `recv_timeout_ms = 100`).

7. Calls `m_ack_tracker.track(env, ack_deadline)` at line 110.

8. **`AckTracker::track()`** (`AckTracker.cpp:44`): scans `m_slots[0..31]` for
   first `FREE` slot. Calls `envelope_copy(m_slots[i].env, env)` (memcpy of
   `sizeof(MessageEnvelope)` bytes). Sets `m_slots[i].deadline_us = ack_deadline`,
   `m_slots[i].state = PENDING`, increments `m_count`. Returns `OK`.

9. **`DeliveryEngine::send()`** checks `track_res` at line 111. `ERR_FULL` is
   logged at `WARNING_HI` but does not fail the send.

**--- Phase C: Retry scheduling ---**

10. Retry class conditional at line 120: `RELIABLE_RETRY` is true. **Branch taken.**

11. Calls `m_retry_manager.schedule(env, m_cfg.max_retries, m_cfg.retry_backoff_ms,
    now_us)` at line 122.

12. **`RetryManager::schedule()`** (`RetryManager.cpp:72`): asserts `m_initialized`,
    `envelope_valid(env)`. Scans `m_slots[0..ACK_TRACKER_CAPACITY-1]` (0..31)
    for first inactive slot (`!m_slots[i].active`). On finding slot `i`:
    - `m_slots[i].active = true`.
    - `envelope_copy(m_slots[i].env, env)` (memcpy ~4136 bytes).
    - `m_slots[i].retry_count = 0U`.
    - `m_slots[i].max_retries = max_retries` (e.g., 3 or 5).
    - `m_slots[i].backoff_ms = backoff_ms` (e.g., 100 ms).
    - `m_slots[i].next_retry_us = now_us` (immediate — first retry due right away).
    - `m_slots[i].expiry_us = env.expiry_time_us` (e.g., `now_us + 5 000 000`).
    - Increments `m_count`. Fires postcondition assertions.
    - Logs `INFO "Scheduled message_id=%llu for retry (max_retries=N, backoff_ms=M)"`.
    - Returns `Result::OK`.

13. **`DeliveryEngine::send()`** checks `sched_res` at line 126. `ERR_FULL`
    logged at `WARNING_HI` but does not fail the send.

**--- Phase D: Send completion ---**

14. **`DeliveryEngine::send()`** fires postcondition assertions. Logs
    `INFO "Sent message_id=%llu, reliability=2"`. Returns `Result::OK`.

**--- Phase E: Retry pump loop ---**

15. Application calls `DeliveryEngine::pump_retries(now_us)` periodically
    (e.g., after each `receive()` attempt).

16. **`DeliveryEngine::pump_retries()`** (`DeliveryEngine.cpp:227`): asserts
    `m_initialized`, `now_us > 0`. Allocates
    `MessageEnvelope retry_buf[MSG_RING_CAPACITY]` on the stack (64 slots,
    ~264 KB). Calls `m_retry_manager.collect_due(now_us, retry_buf,
    MSG_RING_CAPACITY)`.

17. **`RetryManager::collect_due()`** (`RetryManager.cpp:160`): asserts
    `m_initialized`, `out_buf != nullptr`, `buf_cap <= MSG_RING_CAPACITY`. For
    each active slot `i` (loop bound `ACK_TRACKER_CAPACITY = 32`):

    a. **Expiry check (line 177):** `slot_has_expired(m_slots[i].expiry_us,
       now_us)` — returns `(expiry_us != 0ULL) && (now_us >= expiry_us)`. If
       true: `m_slots[i].active = false`, `--m_count`, logs `WARNING_LO
       "Expired retry entry"`, `continue`. Entry not added to `out_buf`.

    b. **Exhaustion check (line 187):** if `m_slots[i].retry_count >=
       m_slots[i].max_retries`: `m_slots[i].active = false`, `--m_count`, logs
       `WARNING_HI "Exhausted retries (count=N, max=M)"`, `continue`.

    c. **Due check (line 198):** if `m_slots[i].next_retry_us <= now_us`:
       - `envelope_copy(out_buf[collected], m_slots[i].env)`.
       - `++collected`, `++m_slots[i].retry_count`.
       - **Exponential backoff via `advance_backoff()`** (`RetryManager.cpp:30`):
         `doubled = current_ms * 2U`. If `doubled > 60000U`, returns `60000U`
         (cap). Otherwise returns `(uint32_t)doubled`.
         `m_slots[i].backoff_ms = advance_backoff(m_slots[i].backoff_ms)`.
         `backoff_us = (uint64_t)m_slots[i].backoff_ms * 1000ULL`.
         `m_slots[i].next_retry_us = now_us + backoff_us`.
       - Logs `INFO "Due for retry: message_id=%llu, attempt=%u, next_backoff_ms=%u"`.

    Returns `collected` (number of envelopes in `out_buf`).

18. **`DeliveryEngine::pump_retries()`** asserts `collected <= MSG_RING_CAPACITY`.
    For `i = 0..collected-1` (fixed-bound loop): calls
    `send_via_transport(retry_buf[i])` — dispatches to `TcpBackend::send_message()`
    (full send path as in UC_01 steps 4–23). Logs success (`INFO "Retried
    message_id=%llu"`) or failure (`WARNING_LO "Retry send failed"`). Returns
    `collected`.

**--- Phase F: Backoff progression (example: initial backoff_ms = 100) ---**

- Schedule time: `next_retry_us = now_us` (immediate).
- Attempt 0 fires: `retry_count = 1`, `backoff_ms: 100→200`, `next_retry_us = now + 200 000`.
- Attempt 1 fires: `retry_count = 2`, `backoff_ms: 200→400`, `next_retry_us = now + 400 000`.
- Attempt 2 fires: `retry_count = 3 >= max_retries = 3` → exhausted at next `collect_due()`.

Note: The first retry fires immediately because `next_retry_us = now_us` at
schedule time. Combined with the double-send from `flush_delayed_to_clients()`,
the receiver sees potentially 3 transmissions before any ACK.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::send()                             [DeliveryEngine.cpp:77]
|
+-- m_id_gen.next()                                [MessageId.hpp - inline]
|
+-- DeliveryEngine::send_via_transport()           [DeliveryEngine.cpp:56]
|   \-- TcpBackend::send_message()                 [TcpBackend.cpp:347]
|       +-- Serializer::serialize()
|       +-- timestamp_now_us()
|       +-- ImpairmentEngine::process_outbound()
|       +-- send_to_all_clients()
|       |   \-- tcp_send_frame() -> socket_send_all() [poll+send x2]
|       \-- flush_delayed_to_clients()
|           +-- ImpairmentEngine::collect_deliverable()
|           +-- Serializer::serialize()
|           \-- send_to_all_clients() [second send]
|
+-- timestamp_deadline_us()                        [Timestamp.hpp:65]
|
+-- AckTracker::track(env, deadline_us)            [AckTracker.cpp:44]
|   \-- envelope_copy() -> memcpy()
|
\-- RetryManager::schedule(env,retries,backoff,now) [RetryManager.cpp:72]
    \-- envelope_copy() -> memcpy()

DeliveryEngine::pump_retries()                     [DeliveryEngine.cpp:227]
|
\-- RetryManager::collect_due(now_us, retry_buf, 64) [RetryManager.cpp:160]
    +-- slot_has_expired()                         [RetryManager.cpp:19]
    +-- [exhaustion check]
    +-- [due check]
    +-- envelope_copy() per due entry
    \-- advance_backoff()                          [RetryManager.cpp:30]

    [for each collected entry]:
    \-- DeliveryEngine::send_via_transport()
        \-- TcpBackend::send_message() [full send path as above]
```

---

## 5. Key Components Involved

### DeliveryEngine
- **Responsibility:** Orchestrates stamp, send, ACK registration, and retry
  scheduling. For `RELIABLE_RETRY`, engages both `AckTracker` and `RetryManager`.
  `pump_retries()` is the periodic re-send pump.
- **Why in this flow:** It is the sole external-facing send and pump API. The
  conditional at lines 102–103 and line 120 selects `RELIABLE_RETRY` behavior.

### AckTracker
- **Responsibility:** Tracks that the message was sent and awaits an ACK.
  `track()` registers the `PENDING` entry with a deadline. `on_ack()` (UC_08)
  transitions to `ACKED`. `sweep_expired()` reclaims both timed-out `PENDING`
  and `ACKED` slots.
- **Why in this flow:** `RELIABLE_RETRY` messages require ACK tracking to know
  when delivery is confirmed. The `AckTracker` holds the ACK deadline
  independently of the retry schedule.

### RetryManager
- **Responsibility:** Manages the retry schedule with exponential backoff.
  `schedule()` registers the active retry entry with `next_retry_us = now_us`
  (immediate first retry). `collect_due()` harvests due entries, applies backoff
  doubling via `advance_backoff()`, and deactivates exhausted or expired entries.
  `on_ack()` cancels the retry entry when an ACK is received.
- **Why in this flow:** `RELIABLE_RETRY` semantics require automatic
  retransmission until ACK or termination condition. `RetryManager` is the sole
  component implementing this.

### advance_backoff() (static helper, RetryManager.cpp:30)
- **Responsibility:** Doubles `current_ms`, capped at `60000U` (60 seconds).
  Called once per retry attempt per due slot. Has two NEVER_COMPILED_OUT_ASSERT
  calls: pre-condition `current_ms <= 60000` and in-branch confirmation.
- **Why in this flow:** Prevents retry flooding by exponentially increasing
  inter-retry intervals.

### slot_has_expired() (static helper, RetryManager.cpp:19)
- **Responsibility:** Returns `(expiry_us != 0ULL) && (now_us >= expiry_us)`.
  A zero `expiry_us` means "never expires". Called before the due check, so
  an expired message is never retransmitted.
- **Why in this flow:** Enforces the message TTL on the retry path, independently
  of the receive-side expiry check in `DeliveryEngine::receive()`.

### TcpBackend, Serializer, ImpairmentEngine, tcp_send_frame(), socket_send_all()
- Same roles as documented in UC_01. The retry send path goes through the
  identical `send_via_transport()` → `TcpBackend::send_message()` chain.

---

## 6. Branching Logic / Decision Points

**Branch 1–4:** Same as UC_02 (send failures, reliability class gate for
`AckTracker`, `track()` `ERR_FULL`, reliability class gate for `RetryManager`).

**Branch 5 (new): `RetryManager::schedule()` return check (line 126)**
- Condition: `sched_res != Result::OK`
- `ERR_FULL` (32 active slots): logs `WARNING_HI`, continues. Send is still
  considered successful; no retry is scheduled.
- `OK`: normal continuation.

**Branch 6: `collect_due()` — expiry check (line 177)**
- Condition: `slot_has_expired(m_slots[i].expiry_us, now_us)`
- If true: deactivate slot, `WARNING_LO`, `continue` (NOT added to `out_buf`).
- If false: proceed to exhaustion check.

**Branch 7: `collect_due()` — exhaustion check (line 187)**
- Condition: `m_slots[i].retry_count >= m_slots[i].max_retries`
- If true: deactivate slot, `WARNING_HI`, `continue`.
- If false: proceed to due check.

**Branch 8: `collect_due()` — due check (line 198)**
- Condition: `m_slots[i].next_retry_us <= now_us`
- If true: copy envelope to `out_buf`, increment `retry_count`, apply backoff,
  add to collection.
- If false (not yet due): skip slot for this pump call.

**Branch 9: `advance_backoff()` — cap check (RetryManager.cpp:34)**
- Condition: `doubled > 60000U`
- If true: return `60000U`.
- If false: return `(uint32_t)doubled`.

**Branch 10: `pump_retries()` — retry send result (line 246)**
- Condition: `send_res != Result::OK`
- If true: logs `WARNING_LO "Retry send failed"`. Loop continues to next entry.
  The retry slot is NOT deactivated on a failed send; it will be retried again.
- If false: logs `INFO "Retried message_id=%llu"`.

**Branches 11+:** Impairment, client count, fd validity, frame failure — same
as UC_01.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. All operations execute synchronously on the caller's thread.

### Where context switches occur
Only at POSIX blocking calls inside `socket_send_all()`: `poll()` and `send()`.

### Synchronization primitives
`RetryManager::m_slots[]` and `m_count` have no synchronization primitives.
`AckTracker::m_slots[]` and `m_count` have no synchronization primitives. If
`pump_retries()`, `receive()`, or `sweep_ack_timeouts()` are called from
different threads, there are data races on both tables. No mutex or atomic
protects them.

`RingBuffer` uses `std::atomic<uint32_t>` for head/tail but is only relevant
to the receive path.

### Producer/consumer relationships
`RetryManager` acts as both producer (schedule fills slots) and consumer
(collect_due drains/updates them). In the single-threaded model, `schedule()`
and `collect_due()` alternate; no concurrent access occurs.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory
No heap allocation. All storage is statically allocated:
- Caller's `MessageEnvelope env`: stack, caller-owned.
- `AckTracker::m_slots[32]`: member of `AckTracker`, value member of
  `DeliveryEngine`. ~132 KB.
- `RetryManager::m_slots[32]` (`RetryEntry` structs): member of `RetryManager`,
  value member of `DeliveryEngine`. Each `RetryEntry` holds a full
  `MessageEnvelope` copy plus scalar fields (~4136 + 32 bytes). ~133 KB.
- `MessageEnvelope retry_buf[MSG_RING_CAPACITY=64]` in `pump_retries()`:
  stack-allocated. 64 × ~4136 bytes ≈ **264 KB**. This is the largest stack
  frame in the codebase and a significant embedded-target risk.
- Same `delayed[32]` stack risk in `flush_delayed_to_clients()` (~132 KB).

### Lifetime of key objects
- `RetryManager` slot holds a live copy of the envelope from `schedule()` until
  `on_ack()`, exhaustion, or expiry deactivates it.
- `AckTracker` slot holds a copy from `track()` until `on_ack()` + sweep, or
  deadline expiry + sweep.

### Copies of the same envelope
The same `MessageEnvelope` (originally stack-allocated in the application) is
copied three times at send time:
1. `TcpBackend::m_wire_buf` — serialized bytes form.
2. `AckTracker::m_slots[i].env` — via `envelope_copy()`.
3. `RetryManager::m_slots[j].env` — via `envelope_copy()`.
Each subsequent retry also copies the `RetryManager` slot's envelope into
`retry_buf[k]` in `collect_due()`, then through serialization again.

### Potential leaks or unsafe patterns
- `retry_buf[64]` on the stack (~264 KB): risk of stack overflow on targets
  with small stacks. [RISK]
- `delayed[32]` in `flush_delayed_to_clients()` (~132 KB): same risk. [RISK]

---

## 9. Error Handling Flow

### How errors propagate
- Transport send failure in `send_via_transport()` (line 94): `send()` returns
  error immediately. Neither `AckTracker::track()` nor `RetryManager::schedule()`
  is called.
- `AckTracker::track()` returns `ERR_FULL` (line 111): logged `WARNING_HI`,
  send continues returning `OK`. Message tracked for retry but not for ACK.
- `RetryManager::schedule()` returns `ERR_FULL` (line 126): logged `WARNING_HI`,
  send continues returning `OK`. Message sent but not scheduled for retry.
- Retry send failure in `pump_retries()` (line 246): logged `WARNING_LO`;
  retry loop continues to next collected entry. The failing retry slot remains
  active and will be retried again on the next `pump_retries()` call.

### Retry logic
Retry fires when `m_slots[i].next_retry_us <= now_us` AND `retry_count <
max_retries` AND `expiry_us` not passed. Backoff doubles each time via
`advance_backoff()`. Maximum backoff is 60 seconds. Retry terminates on:
- ACK receipt → `on_ack()` sets `active = false` (UC_12).
- Exhaustion → `retry_count >= max_retries` in `collect_due()`.
- Expiry → `slot_has_expired()` returns true in `collect_due()`.

### Fallback paths
Same as UC_01/UC_02 for transport-layer failures.

---

## 10. External Interactions

### Network calls
Same as UC_01: `poll()` and `send()` syscalls on each retry send, in addition
to the initial send.

### File I/O
None.

### Hardware interaction
None directly.

### IPC
None.

---

## 11. State Changes / Side Effects

### What system state is modified

**`MessageEnvelope& env` (caller's object):**
- `source_id`, `message_id`, `timestamp_us` stamped during `send()`.

**`MessageIdGen m_id_gen`:**
- Counter incremented.

**`AckTracker::m_slots[i]`:**
- `state`: `FREE` → `PENDING`.
- `env`: copy of original envelope.
- `deadline_us`: `now_us + recv_timeout_ms * 1000`.
- `m_count`: +1.

**`RetryManager::m_slots[j]`:**
- `active`: `false` → `true`.
- `env`: copy of original envelope.
- `retry_count`: 0.
- `max_retries`: from `m_cfg.max_retries`.
- `backoff_ms`: from `m_cfg.retry_backoff_ms`.
- `next_retry_us`: `now_us` (immediate).
- `expiry_us`: `env.expiry_time_us`.
- `m_count`: +1.

**After each `collect_due()` that fires a retry:**
- `m_slots[j].retry_count` incremented by 1.
- `m_slots[j].backoff_ms` doubled (capped at 60000).
- `m_slots[j].next_retry_us` = `now_us + new_backoff_ms * 1000`.

**After exhaustion or expiry:**
- `m_slots[j].active = false`, `m_count` decremented.

### Persistent changes (DB, disk, device state)
None.

---

## 12. Sequence Diagram using mermaid

```text
App  DeliveryEngine  AckTracker  RetryManager  TcpBackend  Logger

--- Initial send ---

App --send(env=RELIABLE_RETRY, now_us)--> DeliveryEngine
  stamp source_id, message_id, timestamp_us
  send_via_transport(env)
    --> TcpBackend::send_message() [serialize+impair+send+flush]
    <-- OK
  [reliability check: RELIABLE_RETRY]
  --> AckTracker::track(env, deadline)
    envelope_copy(); state=PENDING; m_count++
    <-- OK
  --> RetryManager::schedule(env, max_retries, backoff_ms, now)
    envelope_copy(); active=true; next_retry=now; expiry=env.expiry
    <-- OK
App <-- OK

--- Retry pump (called each loop iteration) ---

App --pump_retries(now_us)--> DeliveryEngine
  retry_buf[64] on stack
  --> RetryManager::collect_due(now, retry_buf, 64)
    [slot i: active, !expired, !exhausted, next_retry <= now]
    envelope_copy(retry_buf[0], slots[i].env)
    retry_count++; backoff doubled; next_retry updated
    --> advance_backoff(current_ms) [doubles, caps at 60s]
    <-- collected=1
  [loop: send_via_transport(retry_buf[0])]
    --> TcpBackend::send_message() [serialize+send]
    <-- OK
    Logger::log(INFO "Retried message_id=N")
  <-- 1 (retried count)
App <-- 1

--- After max_retries reached ---

App --pump_retries(now_us)--> DeliveryEngine
  --> RetryManager::collect_due(now, ...)
    [slot i: retry_count >= max_retries]
    active=false; m_count--
    Logger::log(WARNING_HI "Exhausted retries")
    <-- collected=0
  <-- 0
App <-- 0
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

`RetryManager::init()` (`RetryManager.cpp:45-67`): sets `m_count = 0`,
`m_initialized = true`. Loops 0..`ACK_TRACKER_CAPACITY-1` setting each slot:
`active = false`, `retry_count = 0`, `backoff_ms = 0`, `next_retry_us = 0`,
`expiry_us = 0`, `max_retries = 0`, calls `envelope_init(m_slots[i].env)`.

`AckTracker::init()`: as documented in UC_02.

All other init as in UC_01/UC_02.

### What happens during steady-state execution

Each `send()` for `RELIABLE_RETRY` consumes one `AckTracker` slot and one
`RetryManager` slot. Each `pump_retries()` call iterates all 32 `RetryManager`
slots (O(ACK_TRACKER_CAPACITY) per call). Each retry transmission is a full
`send_via_transport()` call including serialization and TCP framing. `collect_due()`
removes exhausted and expired slots automatically, making those slots available for
reuse by the next `schedule()` call.

---

## 14. Known Risks / Observations

### First retry fires immediately

`RetryManager::schedule()` sets `next_retry_us = now_us` (line 91 of
`RetryManager.cpp`). The first `collect_due()` call will fire immediately. Combined
with the double-send from `process_outbound()` + `flush_delayed_to_clients()`, the
receiver sees potentially three transmissions before any ACK can arrive:
1. Original send (direct, from `send_to_all_clients()`).
2. Original send (delayed flush, from `flush_delayed_to_clients()`).
3. First retry (from first `pump_retries()` call).
The receiver's `DuplicateFilter` suppresses re-deliveries for `RELIABLE_RETRY`
messages. [OBSERVATION]

### Source ID matching bug in both AckTracker and RetryManager

`AckTracker::on_ack()` compares `m_slots[i].env.source_id` (the local node's
ID, set during `send()`) against the incoming ACK's `source_id` (the remote
peer's ID). These are different in a two-node deployment.
`RetryManager::on_ack()` has the identical comparison at `RetryManager.cpp:131`.
Consequence: neither `on_ack()` call succeeds. Retry slots are never cancelled
by incoming ACKs. All retries run to exhaustion (or expiry). [RISK - appears to
be a latent bug]

### AckTracker deadline much shorter than RetryManager expiry

`AckTracker` deadline = `recv_timeout_ms` (default 100 ms or 1000 ms).
`RetryManager` expiry = `env.expiry_time_us` (application-set, e.g., 5 seconds).
These are semantically different intervals operating independently. `WARNING_HI
"ACK timeout"` from `sweep_ack_timeouts()` fires after the ACK deadline; the
`RetryManager` slot continues retrying for the full 5-second window. Both emit
independent warnings. [OBSERVATION]

### Retry send failure does not deactivate the slot

If `send_via_transport()` fails during a retry in `pump_retries()` (line 246),
the loop logs `WARNING_LO` and continues. The retry slot remains active. Repeated
transport errors cause the same slot to be collected and attempted on every
`pump_retries()` call until exhaustion or expiry. [OBSERVATION]

### Stack pressure

`MessageEnvelope retry_buf[MSG_RING_CAPACITY=64]` in `pump_retries()`:
~264 KB on the stack per call. `delayed[32]` in `flush_delayed_to_clients()`:
~132 KB. On embedded targets with small stacks, these allocations are a
stack overflow risk. [RISK]

### Both tracker and retry tables share ACK_TRACKER_CAPACITY = 32

Under high message rates with slow ACK receipt, both tables can fill. New
messages will be sent without tracking or retry, silently downgrading them.
[RISK]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `m_cfg.max_retries` defaults to `MAX_RETRY_COUNT = 5` via
  `channel_config_default()`. Applications may override to a smaller value
  (e.g., 3).

- [ASSUMPTION] `m_cfg.retry_backoff_ms` defaults to 100 ms via
  `channel_config_default()`. With 100 ms initial backoff and `max_retries = 3`:
  attempts fire at approximately T, T+200 ms, T+400 ms; exhaustion at next
  `collect_due()` after T+400 ms. The 5-second expiry is unreachable under
  these defaults.

- [ASSUMPTION] `advance_backoff()` doubles the backoff on every `collect_due()`
  call that fires a due retry, regardless of whether the send succeeded.
  Confirmed from `RetryManager.cpp:205`.

- [CONFIRMED] `slot_has_expired()` checks `(expiry_us != 0ULL) && (now_us >=
  expiry_us)`. Expiry check fires before exhaustion check (line 177 before
  187). Confirmed from `RetryManager.cpp:177-198`.

- [CONFIRMED] `RetryManager` stores `expiry_us` in two locations:
  `m_slots[i].expiry_us` (checked by `slot_has_expired()`) and
  `m_slots[i].env.expiry_time_us` (in the copied envelope). Both set from
  `env.expiry_time_us` at `schedule()` time (line 94 of `RetryManager.cpp`);
  no divergence possible.

- [ASSUMPTION] The application calls `pump_retries()` frequently enough to
  detect due retries before the next retry interval passes. If `pump_retries()`
  is called much less frequently than `backoff_ms` intervals, retries will still
  fire correctly (just later than scheduled); retries are not missed, only
  delayed.

- [ASSUMPTION] The application calls `sweep_ack_timeouts()` periodically to
  prevent the `AckTracker` from filling with stale `PENDING` slots.
