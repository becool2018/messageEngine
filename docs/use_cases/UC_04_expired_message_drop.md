# UC_04 — Expired Message Dropped on Send Path

**HL Group:** HL-4 — User sends a message with a deadline
**Actor:** User
**Requirement traceability:** REQ-3.3.4, REQ-3.2.2, REQ-3.2.7, REQ-4.1.2

---

## 1. Use Case Overview

### Clear description of what triggers this flow

A client application has constructed a `MessageEnvelope` with a non-zero
`expiry_time_us` that has already elapsed before the call to
`DeliveryEngine::send()`. Alternatively, the expiry elapses while the message
sits in a retry slot and the next `pump_retries()` cycle is called.

Two distinct expiry-drop paths exist in the codebase:

**Path A — Send-path guard (DeliveryEngine::send(), NOT in current code):**
Notably, the current implementation of `DeliveryEngine::send()` does **not**
check `expiry_time_us` before sending. The envelope is transmitted regardless
of its expiry. This means an already-expired message will be sent over the wire
and only dropped on the receive side (Path B of UC_09) or in the
`RetryManager` retry table (Path C below).

**Path B — Receive-side expiry (DeliveryEngine::receive()):**
Documented in UC_09. After the transport delivers an envelope, `receive()`
checks `timestamp_expired(env.expiry_time_us, now_us)`. If expired, `receive()`
returns `ERR_EXPIRED` without delivering to the application.

**Path C — RetryManager expiry (RetryManager::collect_due()):**
On each `pump_retries()` call, `collect_due()` checks `slot_has_expired()` for
every active retry slot. If the slot's `expiry_us` has passed, the slot is
deactivated (`active = false`) and no retransmission occurs. This is the primary
send-path expiry enforcement.

This use case documents Path C in detail (the expiry mechanism on the send and
retry side) and clarifies that there is no pre-transmission expiry check in
`DeliveryEngine::send()`.

### Expected outcome (single goal)

For Path C: when `pump_retries()` is called and `now_us >= m_slots[i].expiry_us`
for an active retry slot, `collect_due()` deactivates the slot, logs
`WARNING_LO "Expired retry entry"`, does not add it to the output buffer, and
returns a count that excludes the expired entry. No retransmission occurs.
The application's `pump_retries()` call returns 0 for that slot.

For Path A (no check): the message is always sent, regardless of `expiry_time_us`.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

**Path C — retry-side expiry:**
- `DeliveryEngine::pump_retries(uint64_t now_us)` — `DeliveryEngine.cpp:227`.
  → `RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)`
  → `slot_has_expired(m_slots[i].expiry_us, now_us)` — `RetryManager.cpp:19`.

**Path B — receive-side expiry (reference only, see UC_09):**
- `DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)`
  — `DeliveryEngine.cpp:149`.
  → `timestamp_expired(env.expiry_time_us, now_us)` — `Timestamp.hpp:51`.

**Expiry time computation (at send time):**
- `timestamp_deadline_us(now_us, ttl_ms)` — `Timestamp.hpp:65`.
  Returns `now_us + ttl_ms * 1000ULL`.
- Called by the application when building the envelope; stored in
  `env.expiry_time_us`.

---

## 3. End-to-End Control Flow (Step-by-Step)

**--- Setup: How expiry_time_us is established ---**

S1. Application calls `timestamp_deadline_us(now_us, ttl_ms)` when building the
    envelope (e.g., `ttl_ms = 5000U`). `timestamp_deadline_us()` (`Timestamp.hpp:65`):
    - Fires assertions: `now_us <= 0xFFFFFFFFFFFFFFFFULL`,
      `duration_ms <= 0xFFFFFFFFUL`.
    - Returns `now_us + 5000U * 1000ULL = now_us + 5 000 000 µs`.
    - `env.expiry_time_us = now_us + 5 000 000`.

S2. `RetryManager::schedule()` at line 94 copies `env.expiry_time_us` into
    `m_slots[i].expiry_us` as a scalar field. Also preserved in
    `m_slots[i].env.expiry_time_us` via `envelope_copy()`. Both are initialized
    to the same value and cannot diverge after `schedule()`.

**--- Observation: No pre-transmission expiry check ---**

O1. `DeliveryEngine::send()` does NOT call `timestamp_expired()` before calling
    `send_via_transport()`. An envelope with `expiry_time_us` already in the
    past will be serialized and transmitted over TCP exactly as a non-expired
    envelope. The expiry field is preserved verbatim in the wire format by
    `Serializer::serialize()` as an 8-byte big-endian value. The remote
    receiver's `DeliveryEngine::receive()` will then drop it via Path B (UC_09).

**--- Path C: RetryManager::collect_due() expiry check ---**

C1. Application calls `engine.pump_retries(now_us)` (e.g., from a periodic
    maintenance loop or after a `receive()` attempt).

C2. **`DeliveryEngine::pump_retries()`** (`DeliveryEngine.cpp:227`):
    - Fires assertions: `m_initialized`, `now_us > 0`.
    - Allocates `MessageEnvelope retry_buf[MSG_RING_CAPACITY=64]` on the stack
      (~264 KB).
    - Calls `m_retry_manager.collect_due(now_us, retry_buf, MSG_RING_CAPACITY)`.

C3. **`RetryManager::collect_due()`** (`RetryManager.cpp:160`):
    - Fires assertions: `m_initialized`, `out_buf != nullptr`,
      `buf_cap <= MSG_RING_CAPACITY`.
    - Iterates `m_slots[0..ACK_TRACKER_CAPACITY-1]` (fixed bound, 32 slots).
    - For each active slot `i`:

      **Expiry check (line 177):**
      ```
      if (slot_has_expired(m_slots[i].expiry_us, now_us))
      ```

C4. **`slot_has_expired()`** (`RetryManager.cpp:19`):
    - Fires assertions: `now_us > 0ULL`, `expiry_us != ~0ULL`.
    - Returns `(expiry_us != 0ULL) && (now_us >= expiry_us)`.
    - For a message with `expiry_us = T_send + 5 000 000`:
      - If `now_us < expiry_us`: returns false → slot not expired.
      - If `now_us >= expiry_us`: returns true → slot expired.

C5. **If `slot_has_expired()` returns true (Path C fires):**
    - `m_slots[i].active = false`.
    - `--m_count`.
    - `Logger::log(WARNING_LO, "RetryManager", "Expired retry entry for message_id=%llu",
      m_slots[i].env.message_id)`.
    - `continue` — slot is NOT added to `out_buf`. `collected` is not incremented.

C6. **Back in `collect_due()`:**
    - Loop continues to next slot. Exhaustion and due checks are not reached for
      the expired slot (expiry check is first at line 177).
    - After full loop: fires postcondition assertions
      `collected <= buf_cap`, `m_count <= ACK_TRACKER_CAPACITY`.
    - Returns `collected` (which is 0 for the expired slot).

C7. **Back in `DeliveryEngine::pump_retries()`:**
    - `collected = 0` for the expired slot.
    - `NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY)` passes.
    - The retry loop `for i = 0..collected-1` does not execute (collected = 0).
    - Returns `0`.

**--- Path A edge case: expiry_time_us = 0 (never expires) ---**

E1. `slot_has_expired(0, now_us)`: `(0 != 0ULL) = false`. Returns false always.
    Slot is never expired by `collect_due()`. Only exhaustion or ACK can stop
    retries. (Due to the source_id matching bug, only exhaustion applies in
    practice — see UC_03 section 14.)

**--- Timeline for a 5-second expiry (initial backoff_ms=100, max_retries=3) ---**

T=0:      `send()`: `expiry_time_us = T + 5 000 000`, `next_retry_us = T`.
T≈0 ms:   `pump_retries()`: expiry? NO. due? YES → retransmit #1.
          Backoff: 100→200 ms, `next_retry_us = T + 200 000`.
T≈200 ms: `pump_retries()`: expiry? NO. due? YES → retransmit #2.
          Backoff: 200→400 ms, `next_retry_us = T + 400 000`.
T≈400 ms: `pump_retries()`: `retry_count = 2`. due? YES → retransmit #3.
          `retry_count = 3 >= max_retries = 3` → exhausted at next collect_due().
T≈400ms+: `pump_retries()`: exhaustion check fires → deactivate, `WARNING_HI`.
          [Expiry at T+5s never reached; exhaustion wins with these defaults.]

**--- Timeline where expiry fires before exhaustion ---**

Example: `max_retries = 10`, `retry_backoff_ms = 2000 ms`, `expiry_time_us = T + 5s`:
T=0:    retransmit #1, backoff: 2000→4000 ms, `next_retry_us = T + 4 000 000`.
T=4s:   retransmit #2, backoff: 4000→8000 ms, `next_retry_us = T + 8 000 000`.
T=5s:   `pump_retries()`: `slot_has_expired(T+5s, T+5s) = true` → deactivate,
        `WARNING_LO "Expired retry entry"`.
        `retry_count = 2 < 10` but expiry check fires first (line 177 before 187).
        No retransmit.

---

## 4. Call Tree (Hierarchical)

```
--- Path C: retry-side expiry ---

DeliveryEngine::pump_retries(now_us)               [DeliveryEngine.cpp:227]
    NEVER_COMPILED_OUT_ASSERT(m_initialized)
    NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
    MessageEnvelope retry_buf[64] on stack
    RetryManager::collect_due(now_us, retry_buf, 64) [RetryManager.cpp:160]
        NEVER_COMPILED_OUT_ASSERT(m_initialized)
        NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)
        [loop i=0..31]:
            [slot active: true]
            slot_has_expired(expiry_us, now_us)     [RetryManager.cpp:19]
                NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
                NEVER_COMPILED_OUT_ASSERT(expiry_us != ~0ULL)
                return (expiry_us != 0ULL) && (now_us >= expiry_us)  [true]
            m_slots[i].active = false
            --m_count
            Logger::log(WARNING_LO, "Expired retry entry for message_id=...")
            continue  [NOT in out_buf]
        NEVER_COMPILED_OUT_ASSERT(collected <= buf_cap)
        NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)
        return collected=0
    NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY)
    [retry loop: not entered, collected=0]
    return 0

--- Path A observation: no check in send() ---

DeliveryEngine::send(env, now_us)                  [DeliveryEngine.cpp:77]
    [NO timestamp_expired() call before send_via_transport()]
    send_via_transport(env)                        [transmits even if expired]
        TcpBackend::send_message(env)              [serializes, sends]
    ...

--- Expiry time setup ---

Application
    timestamp_deadline_us(now_us, ttl_ms)          [Timestamp.hpp:65]
        return now_us + ttl_ms * 1000ULL
    env.expiry_time_us = result

RetryManager::schedule()                           [RetryManager.cpp:94]
    m_slots[i].expiry_us = env.expiry_time_us      [scalar copy]
    envelope_copy(m_slots[i].env, env)             [also copies expiry_time_us]
```

---

## 5. Key Components Involved

### timestamp_deadline_us() (Timestamp.hpp:65)
- **Responsibility:** Computes `expiry_time_us = now_us + ttl_ms * 1000ULL`.
  Called by the application at message creation time. Deterministic, no state.
- **Why in this flow:** Establishes the expiry timestamp that both the
  `RetryManager` and `DeliveryEngine::receive()` use for expiry decisions.

### timestamp_expired() (Timestamp.hpp:51)
- **Responsibility:** Inline function. Returns
  `(expiry_us != 0ULL) && (now_us >= expiry_us)`. The `!= 0ULL` guard means
  `expiry_time_us = 0` is treated as "never expires". Used in
  `DeliveryEngine::receive()` (UC_09 / Path B).
- **Why in this flow:** Referenced for the receive-side path; not called by the
  send path in the current implementation.

### slot_has_expired() (RetryManager.cpp:19)
- **Responsibility:** File-static helper. Same semantics as `timestamp_expired()`:
  `(expiry_us != 0ULL) && (now_us >= expiry_us)`. Has two
  `NEVER_COMPILED_OUT_ASSERT` calls.
- **Why in this flow:** Called by `collect_due()` for each active retry slot.
  The primary expiry enforcement on the send/retry path.

### RetryManager::collect_due() (RetryManager.cpp:160)
- **Responsibility:** Iterates all 32 retry slots. For each active slot, checks
  expiry (line 177), exhaustion (line 187), and due-time (line 198) in that
  order. Expiry check fires before exhaustion and due checks — an expired entry
  is never retransmitted even if its due time has also passed.
- **Why in this flow:** The only place in the codebase where a scheduled retry
  is cancelled due to message expiry on the sender side.

### DeliveryEngine::pump_retries() (DeliveryEngine.cpp:227)
- **Responsibility:** Calls `collect_due()`, then re-sends each collected
  envelope. Returns the count of retried messages (0 if all were expired or
  not yet due).
- **Why in this flow:** The application-facing API for the retry pump. Its
  return value signals how many messages were actually retransmitted.

### AckTracker (not directly involved in expiry)
- **Responsibility:** Holds a separate deadline (`deadline_us`, derived from
  `recv_timeout_ms`) for each tracked message. This deadline is semantically
  different from `expiry_time_us`. `sweep_ack_timeouts()` uses it independently.
- **Why mentioned:** When a retry slot expires via Path C, the corresponding
  `AckTracker` slot is NOT notified. It remains `PENDING` until its own
  deadline triggers `sweep_ack_timeouts()` → `WARNING_HI`.

---

## 6. Branching Logic / Decision Points

**Branch 1: `slot_has_expired()` — expiry_us == 0 guard**
- Condition: `expiry_us == 0ULL`
- If true: returns `false` (never expires).
- If false: proceeds to time comparison.

**Branch 2: `slot_has_expired()` — time comparison**
- Condition: `now_us >= expiry_us`
- If true: returns `true` → slot expired, deactivated, `WARNING_LO`, `continue`.
- If false: returns `false` → expiry check passes, proceed to exhaustion check.

**Branch 3: `collect_due()` — check ordering**
- Expiry check (line 177) fires before exhaustion (line 187) before due (line 198).
- An expired message is never added to `out_buf`, even if `next_retry_us <= now_us`.

**Branch 4: No expiry check in `DeliveryEngine::send()`**
- Observation: the send path has no `timestamp_expired()` call.
- An already-expired envelope is transmitted unconditionally. The receiver drops
  it via `DeliveryEngine::receive()`.

**Branch 5: `ERR_EXPIRED` in `receive()` (Path B, documented in UC_09)**
- Condition: `timestamp_expired(env.expiry_time_us, now_us)` in
  `DeliveryEngine::receive()` at line 171.
- If true: logs `WARNING_LO "Dropping expired message_id=..."`, returns
  `ERR_EXPIRED`.
- Application receives `ERR_EXPIRED`, must not process `env`.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. All calls execute synchronously on the caller's thread.

### Where context switches occur
None in the expiry check itself. `slot_has_expired()` and `timestamp_expired()`
are pure inline functions with no I/O or blocking.

### Synchronization primitives
None. `RetryManager::m_slots[]` and `m_count` are unprotected. If
`pump_retries()` and `receive()` are called from different threads, there is a
data race. All calls are assumed to originate from the same thread.

### Timing observation
The `now_us` value is captured by the application before calling `pump_retries()`
or `receive()`. If the application captures `now_us` once per loop iteration,
the same timestamp is used for all calls within that iteration. A message that
expires between the capture and the actual expiry check will be treated as
non-expired for that iteration — this errs safely toward delivery rather than
premature drop.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Key data
`expiry_time_us` is a `uint64_t` scalar field in `MessageEnvelope`. It is:
- Set by value in the application via `timestamp_deadline_us()`.
- Bitwise copied into `RetryManager::m_slots[i].env` via `envelope_copy()`
  (memcpy of `sizeof(MessageEnvelope)`).
- Separately copied to `RetryManager::m_slots[i].expiry_us` (dedicated scalar)
  at `RetryManager.cpp:94`. This is the field checked by `slot_has_expired()`.

### Two copies in RetryManager
`RetryManager` stores `expiry_us` in two places:
1. `m_slots[i].expiry_us` — checked by `slot_has_expired()` in `collect_due()`.
2. `m_slots[i].env.expiry_time_us` — the envelope copy used for retransmission.
Both are set identically from `env.expiry_time_us` in `schedule()`. No
divergence is possible after initialization.

### Slot lifecycle after expiry deactivation
When a slot is deactivated due to expiry (`active = false`), the envelope data
remains in `m_slots[i].env` until the slot is reused by the next `schedule()`
call. The slot is considered logically free immediately (the `!m_slots[i].active`
check in `collect_due()` and `schedule()` gates on the `active` flag).

### No heap allocation
`slot_has_expired()` allocates nothing. `pump_retries()` stack-allocates
`retry_buf[64]` (~264 KB); this is the largest stack risk in the path.

---

## 9. Error Handling Flow

### How errors propagate
- **Path C expiry:** `collect_due()` returns 0 for the expired slot (entry not
  in `out_buf`). `pump_retries()` returns 0. The application knows no retries
  fired, but does not receive a direct "expired" error code from `pump_retries()`.
  The only signal is the `WARNING_LO` log. There is no callback or status API.

- **Path B expiry (UC_09):** `receive()` returns `ERR_EXPIRED` directly to the
  application. The envelope content is populated but must not be used.

### Retry logic
Path C expiry terminates the retry loop for that message permanently.
The slot is freed immediately for reuse. No further transmission occurs.

### Fallback paths
- If `expiry_time_us = 0`, neither `slot_has_expired()` nor
  `timestamp_expired()` triggers. The message is retried until exhaustion or
  ACK.

---

## 10. External Interactions

### Network calls
None during the expiry check itself. `pump_retries()` will call
`send_via_transport()` for non-expired due entries, which results in `poll()`
and `send()` syscalls as documented in UC_01.

### File I/O
None.

### Hardware interaction
None. `timestamp_now_us()` calls `clock_gettime(CLOCK_MONOTONIC)` — kernel
syscall, not direct hardware.

### IPC
None.

---

## 11. State Changes / Side Effects

### Path C (retry-side expiry)

**`RetryManager::m_slots[i]`:**
- `m_slots[i].active`: `true` → `false`.
- `m_count`: decremented by 1.
- All other fields unchanged (slot data persists until overwritten by next
  `schedule()`).

**`AckTracker` (not modified by Path C):**
- The corresponding `AckTracker` slot remains `PENDING` with its own `deadline_us`
  (typically 100 ms from `recv_timeout_ms`). It will be swept by
  `sweep_ack_timeouts()` → `sweep_expired()` on its own timeline, generating a
  `WARNING_HI "ACK timeout"` — this fires even though the message expired in
  `RetryManager` and was never re-sent.

**Logger:**
- `WARNING_LO "Expired retry entry for message_id=N"` emitted once per expired
  slot per `collect_due()` call.

### Persistent changes (DB, disk, device state)
None.

---

## 12. Sequence Diagram using mermaid

```text
--- Path C: Retry-side expiry ---

Application  DeliveryEngine  RetryManager  Logger

App --pump_retries(now_us>=expiry_us)--> DeliveryEngine
  retry_buf[64] on stack
  --> RetryManager::collect_due(now_us, retry_buf, 64)
    [slot 0: active=true, expiry_us=T+5s]
    --> slot_has_expired(T+5s, now_us)
        (expiry_us != 0) && (now_us >= expiry_us) --> true
    <-- true
    m_slots[0].active = false
    m_count--
    --> Logger::log(WARNING_LO "Expired retry entry for message_id=N")
    continue (NOT in out_buf)
    [remaining slots: all inactive, skipped]
    ASSERT(collected <= buf_cap)
    ASSERT(m_count <= 32)
    return collected=0
  ASSERT(collected <= 64)
  [retry loop: not entered]
  return 0
App <-- 0

--- Expiry setup (at send time) ---

Application --> timestamp_deadline_us(now_us, 5000)
               returns now_us + 5 000 000
env.expiry_time_us = now_us + 5 000 000
engine.send(env, now_us) --> RetryManager::schedule()
    m_slots[i].expiry_us     = env.expiry_time_us
    m_slots[i].env.expiry_time_us = env.expiry_time_us
    [both set identically, no divergence]
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

`RetryManager::init()` zeroes all `m_slots[i].expiry_us` to 0 and sets
`m_slots[i].active = false`. `slot_has_expired(0, now_us)` would return false
(zero expiry = never expires) — these slots can never be accidentally expired
before a `schedule()` call populates them.

`timestamp_deadline_us()` and `slot_has_expired()` are stateless inline/static
functions; no initialization required.

### What happens during steady-state execution

- `expiry_time_us` is set once at envelope construction time and never modified.
- `m_slots[i].expiry_us` is set once at `schedule()` time and never modified
  afterward.
- `slot_has_expired()` is called on every active slot on every `collect_due()`
  call. It performs a simple `uint64_t` comparison — O(1) per slot.
- The expiry path (Path C) fires for a given slot at most once: after it fires,
  the slot is `active = false` and is skipped on all subsequent `collect_due()`
  iterations.

---

## 14. Known Risks / Observations

### No pre-transmission expiry check in send()

`DeliveryEngine::send()` does not call `timestamp_expired()` before invoking
`send_via_transport()`. A message whose `expiry_time_us` has already passed
will be serialized, transmitted over TCP, and received by the remote peer, which
will then drop it via `receive()` → `ERR_EXPIRED`. This wastes network bandwidth
and processing time for messages that the receiver will never deliver to its
application. [OBSERVATION - potential improvement]

### AckTracker not notified of retry expiry

When Path C fires and deactivates a retry slot, the corresponding `AckTracker`
slot is not notified. It remains `PENDING` with its own `deadline_us` (typically
set to `recv_timeout_ms`, e.g., 100 ms) and will be swept by
`sweep_ack_timeouts()` on its own timeline, generating a spurious `WARNING_HI
"ACK timeout"` even for a message that the sender intentionally stopped retrying
due to TTL expiry. [OBSERVATION]

### Two independent expiry subsystems

`AckTracker` uses `deadline_us = recv_timeout_ms` (100 ms to 1 s).
`RetryManager` uses `expiry_us = env.expiry_time_us` (application-set, e.g., 5
s). These are semantically different. The `AckTracker` entry typically expires
4.9 seconds before the `RetryManager` entry for a 5-second TTL message. Both
generate independent log warnings. [OBSERVATION]

### No application API for retry expiry notification

Path C drops the message silently (only logs `WARNING_LO`). The application
receives `ERR_EXPIRED` from `receive()` (Path B) but has no API to query which
outbound retry entries have expired in `RetryManager`. No callback, status poll,
or metrics hook is present. [OBSERVATION]

### Clock discontinuity risk

`CLOCK_MONOTONIC` does not advance during system suspension on all platforms. A
process suspended after sending a message with a 5-second TTL may resume 30
seconds later and process `pump_retries()`. The `now_us` value will be 30
seconds ahead of the send time, so `slot_has_expired()` will return true
immediately. This is correct behavior — the message is expired — but the
application may be surprised by mass expiry events after a system resume.
[OBSERVATION]

---

## 15. Unknowns / Assumptions

- [CONFIRMED] `slot_has_expired(0, now_us)` returns false. Zero `expiry_us`
  means "never expires". Confirmed from `RetryManager.cpp:23`.

- [CONFIRMED] The expiry check in `collect_due()` fires at line 177, before
  the exhaustion check (line 187) and the due check (line 198). An expired entry
  is never retransmitted. Confirmed from `RetryManager.cpp:177-198`.

- [CONFIRMED] `RetryManager` stores `expiry_us` in two places: as a scalar
  member `m_slots[i].expiry_us` and inside `m_slots[i].env.expiry_time_us`.
  Both set from `env.expiry_time_us` at `schedule()` time. No divergence.
  Confirmed from `RetryManager.cpp:85,94`.

- [CONFIRMED] `DeliveryEngine::send()` does not check `expiry_time_us` before
  calling `send_via_transport()`. Confirmed by reading `DeliveryEngine.cpp:77-143`.

- [CONFIRMED] `timestamp_deadline_us(now_us, duration_ms)` returns
  `now_us + duration_ms * 1000ULL`. Confirmed from `Timestamp.hpp:65-75`.

- [CONFIRMED] `timestamp_expired(expiry_us, now_us)` returns
  `(expiry_us != 0ULL) && (now_us >= expiry_us)`. Confirmed from
  `Timestamp.hpp:51-59`.

- [ASSUMPTION] With default `max_retries = 3` and `retry_backoff_ms = 100 ms`,
  exhaustion fires at approximately T+400 ms (well before a 5-second expiry).
  The Path C expiry path only fires in practice when `max_retries` is large
  enough that the retry window extends past `expiry_time_us`.

- [ASSUMPTION] The application calls `pump_retries()` with the current monotonic
  time obtained via `timestamp_now_us()`. If `now_us` is stale (e.g., captured
  significantly before the `pump_retries()` call), `slot_has_expired()` may
  return false for a message that is actually expired by wall-clock time.
