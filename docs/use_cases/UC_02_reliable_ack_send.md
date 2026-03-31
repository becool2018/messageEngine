# UC_02 — RELIABLE_ACK send: single transmission, ACK slot allocated in AckTracker

**HL Group:** HL-2 — Send with Acknowledgement
**Actor:** User
**Requirement traceability:** REQ-3.3.2, REQ-3.2.4, REQ-3.2.1, REQ-3.2.3, REQ-4.1.2,
REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::send()` with a `MessageEnvelope` whose
`reliability_class` is `ReliabilityClass::RELIABLE_ACK`.

**Goal:** Transmit the envelope exactly once over TCP and allocate one slot in the
`AckTracker` so the system can detect whether the remote peer sends an ACK within the
configured timeout (`recv_timeout_ms`). No automatic retry is scheduled.

**Success outcome:** `DeliveryEngine::send()` returns `Result::OK`. The serialized wire
frame has reached the TCP socket(s). An `AckTracker` slot transitions from FREE to PENDING
with `deadline_us = now_us + recv_timeout_ms * 1000`.

**Error outcomes:**
- `Result::ERR_INVALID` — engine not initialized or envelope is invalid.
- `Result::ERR_IO` from transport — serialization or socket send failure; ACK tracking is
  still attempted because tracking is a best-effort side effect.
- `Result::OK` with AckTracker `ERR_FULL` — send succeeded but ACK tracking could not be
  registered (tracker full); WARNING_HI is logged; the message is untracked.
- `Result::OK` (silent drop) — impairment loss policy dropped the message before socket
  write; ACK tracking still attempted.

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
```

The caller fills the envelope with `reliability_class = RELIABLE_ACK`, then calls
`engine.send(env, timestamp_now_us())`.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. Caller invokes `DeliveryEngine::send(env, now_us)`.
2. Pre-condition asserts: `m_initialized`, `now_us > 0`.
3. Guard check: `if (!m_initialized) return ERR_INVALID` — skipped when initialized.
4. `send()` stamps `env.source_id = m_local_id`.
5. `send()` calls `m_id_gen.next()` → stores result in `env.message_id`.
6. `send()` stamps `env.timestamp_us = now_us`.
7. `send()` calls `send_via_transport(env)`.
8. `send_via_transport()` asserts `m_initialized`, `m_transport != nullptr`,
   `envelope_valid(env)`, then calls `m_transport->send_message(env)` via virtual dispatch.
9. `TcpBackend::send_message()` executes identically to UC_01 steps 9a–12:
   serializes, applies impairment, flushes delay buffer, sends to all clients, returns `OK`
   (or an error code if something fails).
10. Control returns to `DeliveryEngine::send()` with `res` from `send_via_transport()`.
11. Transport error check: `if (res != Result::OK)` — logs WARNING_LO and returns `res`.
    For the success path, `res == OK` and execution continues.
12. Reliability class check — first branch:
    `if (env.reliability_class == RELIABLE_ACK || env.reliability_class == RELIABLE_RETRY)`:
    condition is TRUE for RELIABLE_ACK.
    a. Computes ACK deadline: `ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms)`.
       This is `now_us + (recv_timeout_ms * 1000)` in microseconds.
    b. Calls `m_ack_tracker.track(env, ack_deadline)`.
13. Inside `AckTracker::track(env, deadline_us)`:
    a. Pre-condition assert: `m_count <= ACK_TRACKER_CAPACITY`.
    b. Scans `m_slots[0..ACK_TRACKER_CAPACITY-1]` (32 slots, bounded) for a slot where
       `state == EntryState::FREE`.
    c. On finding a free slot at index `i`:
       - Calls `envelope_copy(m_slots[i].env, env)` — copies entire `MessageEnvelope`
         (memcpy, 4140 bytes).
       - Sets `m_slots[i].deadline_us = deadline_us`.
       - Sets `m_slots[i].state = EntryState::PENDING`.
       - Increments `m_count`.
       - Post-condition asserts: `m_slots[i].state == PENDING`, `m_count <= ACK_TRACKER_CAPACITY`.
       - Returns `Result::OK`.
    d. If no free slot found: asserts `m_count == ACK_TRACKER_CAPACITY`, returns `ERR_FULL`.
14. Back in `DeliveryEngine::send()`: checks `track_res`:
    - `ERR_FULL` or any non-OK → logs WARNING_HI: `"Failed to track ACK for message_id=..."`.
      Does NOT return early — tracking failure is a side effect; the send itself succeeded.
15. Second reliability check: `if (env.reliability_class == RELIABLE_RETRY)` — FALSE for
    RELIABLE_ACK. Retry scheduling block is skipped entirely.
16. Post-condition asserts: `env.source_id == m_local_id`, `env.message_id != 0`.
17. Logs INFO: `"Sent message_id=..., reliability=1"`.
18. Returns `OK`.
19. Caller receives `OK`. The `AckTracker` now has one PENDING slot for this message.

---

## 4. Call Tree

```
DeliveryEngine::send(env, now_us)
 ├── MessageIdGen::next()                        [assigns env.message_id]
 ├── DeliveryEngine::send_via_transport(env)
 │    └── TcpBackend::send_message(envelope)     [virtual dispatch]
 │         ├── Serializer::serialize(...)
 │         ├── timestamp_now_us()
 │         ├── ImpairmentEngine::process_outbound(...)
 │         └── TcpBackend::flush_delayed_to_clients(now_us)
 │              ├── ImpairmentEngine::collect_deliverable(...)
 │              ├── Serializer::serialize(delayed[i], ...)
 │              └── TcpBackend::send_to_all_clients(...)
 │                   └── ISocketOps::send_frame(fd, buf, len, timeout_ms)
 └── AckTracker::track(env, ack_deadline)        [only if res==OK]
      └── envelope_copy(m_slots[i].env, env)     [memcpy]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Orchestrates send; after a successful transport send, computes the
  ACK deadline and calls `AckTracker::track()`. Does not retry on ACK non-receipt; that is
  left to the application to detect via `sweep_ack_timeouts()`.

- **`MessageIdGen`** — Assigns `env.message_id`. The message_id stored in the AckTracker
  slot is the same value; it is used later by `AckTracker::on_ack()` to find the matching
  PENDING slot when an ACK arrives.

- **`TcpBackend`** — Same as UC_01; see UC_01 §5 for detail.

- **`AckTracker`** — Maintains a fixed table of 32 `Entry` slots (FREE / PENDING / ACKED
  state machine). `track()` places one entry in PENDING state with the computed deadline.
  The `m_count` member tracks occupancy.

- **`timestamp_deadline_us()`** — Inline function in `Timestamp.hpp`. Computes
  `now_us + duration_ms * 1000` without overflow, returning the absolute ACK deadline.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `!m_initialized` | Return `ERR_INVALID` | Continue | Step 4 |
| `send_via_transport()` returns non-OK | Log WARNING_LO, return that result | Continue to ACK tracking | Step 12 |
| `reliability_class == RELIABLE_ACK \|\| RELIABLE_RETRY` | Enter ACK-tracking block, compute deadline, call `track()` | Skip ACK block | Retry check |
| `AckTracker::track()` returns `ERR_FULL` | Log WARNING_HI; continue (no early return) | Continue | Post-condition asserts |
| `reliability_class == RELIABLE_RETRY` | Enter retry-scheduling block | Skip retry | Post-condition asserts |
| Free slot found in AckTracker scan | Fill slot, increment count, return OK | Continue scan | Next slot |
| No free slot found after full scan | Assert `m_count == ACK_TRACKER_CAPACITY`, return `ERR_FULL` | — | Back to `send()` |

---

## 7. Concurrency / Threading Behavior

- Same single-thread assumption as UC_01 for the send path.

- **`AckTracker`** is not thread-safe. It is owned by `DeliveryEngine` and accessed only
  from the application thread that calls `send()` and later `sweep_ack_timeouts()` or
  `receive()`. No locks protect it.

- **ACK receive path interaction:** When a remote ACK arrives and `DeliveryEngine::receive()`
  processes it, `m_ack_tracker.on_ack()` is called from the same application thread that
  calls `receive()`. If `send()` and `receive()` are called from different threads, there is
  a data race on `m_ack_tracker`. This is a design-level constraint (single application
  thread).

- No `std::atomic` operations are involved in the ACK-tracking path; all accesses are plain
  member reads/writes inside the engine's call chain.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `env` (caller's) | Caller's stack | ≈ 4140 bytes | `source_id`, `message_id`, `timestamp_us` stamped in-place |
| `m_wire_buf` | `TcpBackend` member | 8192 bytes | Reused each call |
| `delayed[]` in `flush_delayed_to_clients` | Stack | 32 × ~4140 ≈ 132 KB | See UC_01 §8 |
| `m_slots[i].env` in `AckTracker` | `AckTracker` member array | one `MessageEnvelope` ≈ 4140 bytes | Deep-copied from `env` via `envelope_copy()` (memcpy). AckTracker owns this copy for the duration of PENDING state. |
| `m_slots[i].deadline_us` | `AckTracker` member | `uint64_t` (8 bytes) | Absolute expiry time in microseconds |
| `m_slots` array | `AckTracker` member | `ACK_TRACKER_CAPACITY * sizeof(Entry)` = 32 × (4140 + 8 + 1 + padding) ≈ 132 KB | Fixed at construction; no heap |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. `AckTracker::track()` uses the
pre-allocated `m_slots` array, copying the envelope with `memcpy`. `envelope_copy()` is an
inline `memcpy` wrapper.

---

## 9. Error Handling Flow

| Condition | System state after | What caller should do |
|---|---|---|
| `!m_initialized` → `ERR_INVALID` | No changes | Call `init()` |
| `send_via_transport()` → non-OK | Message not sent; ACK tracking not attempted (early return) | Check Result; retry if appropriate |
| `AckTracker::track()` → `ERR_FULL` | Message sent; no PENDING slot; WARNING_HI logged | Application must handle untracked reliable sends; consider reducing in-flight message count |
| ACK never arrives (tracked but deadline expires) | `sweep_ack_timeouts()` will eventually return the expired entry as an unacknowledged message | Call `sweep_ack_timeouts()` in the event loop; handle returned expired envelopes |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | POSIX monotonic clock | Called in `timestamp_now_us()` inside `TcpBackend::send_message()` and in `timestamp_deadline_us()` |
| `ISocketOps::send_frame()` | TCP socket fd | See UC_01 §10 |

No file I/O, IPC, or hardware interaction on the UC_02 send path.

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
| `AckTracker m_slots[i]` | `env` | Zero-initialized | Deep copy of `env` |
| `AckTracker m_slots[i]` | `deadline_us` | 0 | `now_us + recv_timeout_ms * 1000` |
| `AckTracker m_count` | count | N | N+1 (if slot found) |
| Kernel TCP send buffer | bytes | Previous | Appended with length-prefixed frame |

---

## 12. Sequence Diagram

```
Caller       DeliveryEngine    MessageIdGen   TcpBackend     AckTracker    timestamp_deadline_us
  |                |                |              |               |                |
  |--send(env,now)->|               |              |               |                |
  |                |--next()------->|              |               |                |
  |                |<-message_id----|              |               |                |
  |                | (stamp env)    |              |               |                |
  |                |--send_via_transport(env)------>|               |                |
  |                |                |  [serialize + impairment + flush + send_frame] |
  |                |<--OK-----------|              |               |                |
  |                | (res==OK)       |              |               |                |
  |                |--timestamp_deadline_us(now, recv_timeout_ms)-->|               |
  |                |<--ack_deadline--|              |               |                |
  |                |--track(env, ack_deadline)----->|               |                |
  |                |                |              |  scan m_slots for FREE          |
  |                |                |              |  copy env, set PENDING          |
  |                |                |              |  ++m_count                      |
  |                |<--OK (or ERR_FULL if full)---->|               |                |
  | (RELIABLE_ACK: no retry schedule)              |               |                |
  |<--OK------------|                |              |               |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- Same as UC_01: `TcpBackend::init()` and `DeliveryEngine::init()` must have completed.
- `AckTracker::init()` is called inside `DeliveryEngine::init()`: zero-initializes
  `m_slots` (memset), sets `m_count = 0`. All 32 slots start in FREE state.

**Steady-state:**
- `AckTracker::track()` is called once per RELIABLE_ACK send.
- PENDING slots persist until either:
  - An ACK is received and `DeliveryEngine::receive()` calls `m_ack_tracker.on_ack()` →
    slot transitions to ACKED.
  - The deadline expires and `DeliveryEngine::sweep_ack_timeouts()` calls
    `m_ack_tracker.sweep_expired()` → slot transitions to FREE.
- The slot is not freed inline during UC_02; it remains PENDING until one of the above events.

---

## 14. Known Risks / Observations

- **ACK tracking silently degrades at capacity.** When `AckTracker` is full
  (`m_count == ACK_TRACKER_CAPACITY == 32`), a RELIABLE_ACK message is sent but not
  tracked. The only indication is a WARNING_HI log entry. Applications sending many
  simultaneous RELIABLE_ACK messages must call `sweep_ack_timeouts()` regularly to reclaim
  slots.

- **Tracking attempted even after loss drop.** If the impairment engine drops the message,
  `send_via_transport()` still returns `OK`, and `AckTracker::track()` is still called.
  An ACK will never arrive for a lost message, so the slot will eventually expire.

- **`envelope_copy()` is a full `sizeof(MessageEnvelope)` memcpy** (~4140 bytes) per
  tracked message. For 32 simultaneous RELIABLE_ACK messages the AckTracker copies ≈ 132 KB
  of data during `init()` or as messages arrive. This is a fixed cost, not unbounded, but
  it is worth noting for WCET analysis.

- **No retry.** RELIABLE_ACK does not auto-retry. If the remote peer never sends an ACK,
  the application learns this only via `sweep_ack_timeouts()`. There is no mid-flight
  cancellation or resend mechanism in this class.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `timestamp_deadline_us()` is called with `m_cfg.recv_timeout_ms` which is
  set by the application in `ChannelConfig::recv_timeout_ms`. For the demo application in
  `Server.cpp` and `Client.cpp`, this is 100 ms.

- [ASSUMPTION] The AckTracker slot for this message will be visited by
  `sweep_ack_timeouts()` before `AckTracker` fills up; this relies on the application
  calling `sweep_ack_timeouts()` in its event loop.

- [ASSUMPTION] The tracking failure path (WARNING_HI + continue) means the send is
  considered successful from the caller's perspective even if ACK tracking could not be
  registered. This is a deliberate design choice (tracking is a "side effect" per the
  code comment in `DeliveryEngine::send()`).

- [ASSUMPTION] `envelope_copy()` is always safe to call with valid pointers and a
  `sizeof(MessageEnvelope)` copy length. The inline implementation uses `memcpy` directly.
