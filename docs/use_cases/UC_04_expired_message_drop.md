# UC_04 — Expired message detected and dropped at send time

**HL Group:** HL-4 — Send with Expiry Deadline
**Actor:** User
**Requirement traceability:** REQ-3.3.4, REQ-3.2.2, REQ-3.2.7, REQ-4.1.2

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::send()` with a `MessageEnvelope` whose
`expiry_time_us` is non-zero and whose deadline has already passed (i.e.,
`now_us >= expiry_time_us`).

**Goal:** Detect the stale message before it reaches the transport layer and drop it without
any network I/O.

**Success outcome (expiry detection):** `DeliveryEngine::send()` returns ... wait.

**Important clarification:** Examining `DeliveryEngine::send()` (src/core/DeliveryEngine.cpp),
there is **no expiry check on the send path** inside `send()` itself. The expiry check
`timestamp_expired(env.expiry_time_us, now_us)` appears only in `DeliveryEngine::receive()`
(line 171). The send path in `DeliveryEngine::send()` does not call `timestamp_expired()` or
otherwise reject an expired envelope — it proceeds directly to `send_via_transport()` after
stamping `source_id`, `message_id`, and `timestamp_us`.

This means that an expired message submitted via `send()` will be transmitted over the wire.
The expiry drop that is documented in REQ-3.2.7 and REQ-3.3.4 is enforced on the **receive
side**, not the send side, within the current implementation.

This UC documents the actual runtime behavior: an expired envelope submitted to `send()` is
NOT dropped on the send path in the current code. The document also traces what would happen
at the receive end.

**Actual error outcome for an expired envelope at send time:**
- `DeliveryEngine::send()` returns `Result::OK` — the message is transmitted.
- The receiver's `DeliveryEngine::receive()` will later detect the expiry and return
  `Result::ERR_EXPIRED`.

---

## 2. Entry Points

```
// src/core/DeliveryEngine.cpp — send path (no expiry check here)
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us);

// src/core/DeliveryEngine.cpp — receive path (expiry checked here)
Result DeliveryEngine::receive(MessageEnvelope& env,
                               uint32_t timeout_ms,
                               uint64_t now_us);

// src/core/Timestamp.hpp — expiry predicate used on receive path
inline bool timestamp_expired(uint64_t expiry_us, uint64_t now_us);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

### Send side (no drop occurs)

1. Caller sets `env.expiry_time_us` to a past timestamp (e.g., `now_us - 1`) and calls
   `DeliveryEngine::send(env, now_us)`.
2. Pre-condition asserts: `m_initialized`, `now_us > 0`.
3. Guard: `if (!m_initialized) return ERR_INVALID` — passes.
4. `send()` stamps `env.source_id`, `env.message_id`, `env.timestamp_us`.
5. `send()` calls `send_via_transport(env)`.
6. Inside `send_via_transport()`: asserts fire, `m_transport->send_message(env)` dispatches
   to `TcpBackend::send_message()`.
7. `TcpBackend::send_message()` calls `Serializer::serialize()`. The serializer writes
   `expiry_time_us` into the wire frame as-is (big-endian, 8 bytes at offset 28 of the
   header). **No expiry check in the serializer.**
8. Impairment, flush, and socket send proceed as in UC_01.
9. `send()` completes reliability-class branching (ACK tracking / retry scheduling if
   applicable), logs INFO, returns `OK`.
10. **Caller receives `OK`. The expired message has been transmitted.**

### Receive side (drop occurs here)

11. The remote peer's `TcpBackend` receives the frame via `recv_from_client()` →
    `Serializer::deserialize()` → `m_recv_queue.push()`.
12. The remote `DeliveryEngine::receive(env, timeout_ms, now_us)` pops the envelope.
13. Pre-condition asserts in `receive()`: `m_initialized`, `now_us > 0`.
14. `m_transport->receive_message(env, timeout_ms)` returns `OK`.
15. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` fires (passes — the envelope is
    structurally valid; expiry is a semantic concern, not a structural one).
16. `timestamp_expired(env.expiry_time_us, now_us)` is evaluated:
    - `expiry_time_us != 0` (it was set by the sender) AND `now_us >= expiry_time_us` →
      returns `true`.
17. `receive()` logs WARNING_LO: `"Dropping expired message_id=... from src=..."`.
18. `receive()` returns `Result::ERR_EXPIRED`.
19. **Caller of `receive()` gets `ERR_EXPIRED`. The message is discarded without delivery.**

---

## 4. Call Tree

```
[Send side — no drop]
DeliveryEngine::send(env, now_us)
 ├── MessageIdGen::next()
 └── DeliveryEngine::send_via_transport(env)
      └── TcpBackend::send_message(envelope)
           ├── Serializer::serialize(...)      [writes expiry_time_us as-is]
           └── TcpBackend::flush_delayed_to_clients(now_us)
                └── ISocketOps::send_frame(fd, buf, len, timeout_ms)

[Receive side — drop occurs]
DeliveryEngine::receive(env, timeout_ms, now_us)
 ├── m_transport->receive_message(env, timeout_ms)
 │    └── TcpBackend::receive_message(...)
 │         └── [pop from m_recv_queue or poll+recv]
 └── timestamp_expired(env.expiry_time_us, now_us)   [TRUE → return ERR_EXPIRED]
```

---

## 5. Key Components Involved

- **`DeliveryEngine::send()`** — Does NOT check expiry. Transmits the message regardless of
  `expiry_time_us`.

- **`Serializer::serialize()`** — Encodes `expiry_time_us` verbatim (big-endian 8 bytes).
  No semantic validation of the expiry value.

- **`DeliveryEngine::receive()`** — Performs the expiry check immediately after a successful
  `receive_message()` call but before dedup, ACK processing, or returning the envelope to
  the caller.

- **`timestamp_expired(expiry_us, now_us)`** — Inline function. Returns `true` if
  `expiry_us != 0 && now_us >= expiry_us`. The `expiry_us == 0` sentinel means "never
  expires."

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `expiry_time_us == 0` in `timestamp_expired()` | Returns `false` (never expires) | — | Next expiry check |
| `now_us >= expiry_time_us` (and `expiry_time_us != 0`) | Returns `true` | Returns `false` | `receive()` checks return value |
| `timestamp_expired()` returns `true` in `receive()` | Log WARNING_LO, return `ERR_EXPIRED` | Continue to control/data message handling | — |

On the **send path**, there are no expiry-related branches — expiry is not evaluated.

---

## 7. Concurrency / Threading Behavior

- Same as UC_01 on the send side: single application thread, no locks.
- On the receive side: `receive()` is called from the application thread. `timestamp_now_us()`
  is thread-safe (CLOCK_MONOTONIC). The `m_dedup`, `m_ack_tracker`, and `m_retry_manager`
  are accessed on the receive thread without synchronization.
- No `std::atomic` accesses in the expiry-check path; only plain timestamp comparisons.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `env` (send caller's) | Caller's stack | ≈ 4140 bytes | Transmitted with its expired `expiry_time_us` field intact |
| `m_wire_buf` | `TcpBackend` member | 8192 bytes | Encodes `expiry_time_us` into bytes 28–35 of the wire frame (big-endian) |
| `env` (receive caller's) | Receive caller's stack | ≈ 4140 bytes | Populated by `receive_message()`, then found to be expired; contents are valid but caller gets `ERR_EXPIRED` and should discard |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. Expiry check is a pure arithmetic
comparison with no side effects on storage.

---

## 9. Error Handling Flow

| Condition | System state after | What caller should do |
|---|---|---|
| Expired message submitted to `send()` | Message transmitted; no drop; returns `OK` | [ASSUMPTION] Application should pre-check expiry before calling `send()` if it wants to avoid sending stale data |
| `receive()` returns `ERR_EXPIRED` | Envelope is structurally valid but semantically stale; `m_recv_queue` item consumed; WARNING_LO logged | Discard the envelope; do not process its payload; continue calling `receive()` for the next message |
| `expiry_time_us == 0` | Message never expires; `timestamp_expired()` returns `false` on both send and receive paths | Normal delivery path |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | POSIX monotonic clock | `now_us` is supplied by the caller; `timestamp_now_us()` is called by `TcpBackend::send_message()` internally. On the receive side, `now_us` is supplied by the application calling `receive()`. |

No file I/O, IPC, or hardware interaction is specific to the expiry-check path.

---

## 11. State Changes / Side Effects

**Send side (expired message transmitted):**
- Same state changes as UC_01 (source_id, message_id, timestamp_us stamped; wire buffer
  written; TCP socket written). No additional state changes due to expiry.
- If `reliability_class == RELIABLE_RETRY`, a RetryManager slot is created. That slot will
  be active even for an already-expired message; `collect_due()` will detect the expiry via
  `slot_has_expired(expiry_us, now_us)` and deactivate it on the first `pump_retries()` call.

**Receive side (drop):**
| Object | Member | Before | After |
|---|---|---|---|
| `RingBuffer m_recv_queue` | head index | H | H+1 (item consumed by `pop()`) |
| Logger | output stream | — | WARNING_LO entry appended |

No other state changes. The receive caller's `env` output parameter is populated by
`receive_message()` but should be discarded because `ERR_EXPIRED` is returned.

---

## 12. Sequence Diagram

```
Send Caller   DeliveryEngine (sender)  TcpBackend     Network      TcpBackend (receiver)  DeliveryEngine (receiver)
    |                  |                    |              |                |                       |
    |--send(env,now)-->|                    |              |                |                       |
    |  [expiry in past]|                    |              |                |                       |
    |                  |--send_via_transport->            |                |                       |
    |                  |                    |--serialize-->|                |                       |
    |                  |                    |--send_frame->|                |                       |
    |                  |                    |    [TCP]     |                |                       |
    |<--OK-------------|                    |              |                |                       |
    |                  |                    |              |--recv_frame--->|                       |
    |                  |                    |              |                |--deserialize-->       |
    |                  |                    |              |                |--push(env)-->         |
    |                  |                    |              |                |                       |
    |             Recv Caller               |              |                |--receive()----------->|
    |                  |                    |              |                |                       |--receive_message()
    |                  |                    |              |                |                       |  pop(env) from queue
    |                  |                    |              |                |                       |--timestamp_expired()
    |                  |                    |              |                |                       |  [TRUE: now >= expiry]
    |                  |                    |              |                |                       |--log WARNING_LO
    |             <--ERR_EXPIRED------------|              |                |                       |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- Same as UC_01: `TcpBackend::init()` and `DeliveryEngine::init()` must have completed.
- `Timestamp.hpp` functions are inline and require no initialization.

**Steady-state:**
- The expiry check in `receive()` is always active. Every received envelope with a non-zero
  `expiry_time_us` is checked against the `now_us` passed to `receive()`. There is no
  configuration knob to disable expiry checking on the receive path.
- The send path has no expiry check; this is an intentional design choice (or a gap,
  depending on interpretation — see §14).

---

## 14. Known Risks / Observations

- **Expiry is not enforced on the send path.** REQ-3.2.7 states "Drop or deprioritize expired
  messages before delivery," but the current `DeliveryEngine::send()` implementation does not
  call `timestamp_expired()` before calling `send_via_transport()`. An expired message will
  consume network bandwidth and be dropped only on the receiver's side.

- **RetryManager will schedule retries for an expired message.** If `reliability_class ==
  RELIABLE_RETRY` and the envelope is already expired at send time, a RetryManager slot is
  still allocated. On the first `pump_retries()` call, `collect_due()` will detect
  `slot_has_expired(expiry_us, now_us)` and deactivate the slot — but the initial
  transmission has already occurred.

- **AckTracker slot is allocated for expired RELIABLE_ACK messages.** The slot will expire
  during the next `sweep_ack_timeouts()` call but consumes capacity in the meantime.

- **`now_us` on the receive side is provided by the caller.** If the application uses a stale
  `now_us` (e.g., sampled before a long blocking operation), it may fail to detect genuinely
  expired messages.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The intended behavior per REQ-3.2.7 is that expired messages should be dropped
  before transmission. The current send path in `DeliveryEngine::send()` does not implement
  this. Whether this constitutes a requirements gap or an intentional design decision
  (receiver-side enforcement only) is not documented in the source files read.

- [ASSUMPTION] `timestamp_expired(0, now_us)` always returns `false` because of the
  `expiry_us != 0` guard. A value of 0 means "never expires." This is relied upon by ACK
  envelopes which set `expiry_time_us = 0` in `envelope_make_ack()`.

- [ASSUMPTION] `now_us` passed to `receive()` is the current monotonic time at the moment of
  the call. The application in `Server.cpp` captures `now_us = timestamp_now_us()` at the
  start of each loop iteration and passes it to both `receive()` and `pump_retries()`.
