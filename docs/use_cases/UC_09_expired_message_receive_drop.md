# UC_09 — Expired Message Dropped on Receive Path

**HL Group:** HL-5 — User waits for an incoming message
**Actor:** System
**Requirement traceability:** REQ-3.3.4, REQ-3.2.7, REQ-3.2.2, REQ-4.1.3

---

## 1. Use Case Overview

### Clear description of what triggers this flow

A `DATA` message has arrived at the receiver via TCP transport, was successfully
deserialized by `Serializer::deserialize()`, and was pushed into the `RingBuffer`
receive queue by `recv_from_client()`. When the application calls
`DeliveryEngine::receive()`, the transport pops the envelope and returns it to
`DeliveryEngine`. `DeliveryEngine::receive()` then checks
`timestamp_expired(env.expiry_time_us, now_us)`. The message's `expiry_time_us`
field has already passed (i.e., `now_us >= expiry_time_us` and
`expiry_time_us != 0`), so the expiry check returns `true`. `DeliveryEngine`
logs `WARNING_LO "Dropping expired message_id=..."` and returns `ERR_EXPIRED`
to the application without delivering the message.

### Expected outcome (single goal)

`DeliveryEngine::receive()` returns `Result::ERR_EXPIRED`. The `env` output
parameter is populated with the expired envelope's fields, but the application
must not process it. No ACK is sent. No dedup entry is recorded.
`DuplicateFilter`, `AckTracker`, and `RetryManager` are not touched on this path.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point:** `DeliveryEngine::receive(MessageEnvelope& env,
  uint32_t timeout_ms, uint64_t now_us)` — `DeliveryEngine.cpp` line 149.
- **Expiry check:** line 171:
  `if (timestamp_expired(env.expiry_time_us, now_us))`
- **Key inline function:** `timestamp_expired(uint64_t expiry_us, uint64_t now_us)`
  — `Timestamp.hpp:51-59`.
- **Pre-condition:** `DeliveryEngine::init()` completed (`m_initialized = true`).
  `TcpBackend` has a `MessageEnvelope` with `expiry_time_us > 0` and already
  elapsed queued in `m_recv_queue`.

### Example: main(), ISR, callback, RPC handler, etc.

The application calls `engine.receive(env, timeout_ms, now_us)` from its main
loop. `now_us` is typically captured by the application just before the call:
```
uint64_t now_us = timestamp_now_us();
Result res = engine.receive(env, timeout_ms, now_us);
```
No ISR or RPC mechanism is present.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Step 1 — Application invokes DeliveryEngine::receive()**

`engine.receive(env, timeout_ms, now_us)` called. `now_us` is a snapshot of
`CLOCK_MONOTONIC` captured before or at the call.

Precondition assertions at `DeliveryEngine.cpp:153-154`:
- `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
- `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`.

`m_initialized` guard at line 156-158: passes for this use case.

**Step 2 — Transport receive (DeliveryEngine.cpp:161)**

```
Result res = m_transport->receive_message(env, timeout_ms);
```

Virtual dispatch to `TcpBackend::receive_message()` (`TcpBackend.cpp:386`):

1. Calls `m_recv_queue.pop(envelope)` — the SPSC ring buffer. If a message is
   present (the expired `DATA` envelope was already deserialized and pushed by
   `recv_from_client()`), `pop()` succeeds immediately with `Result::OK` and
   fills `envelope`. The `RingBuffer::pop()` uses atomic acquire-load on `m_head`
   and release-store on `m_tail`.

2. If the queue is empty on first try, `TcpBackend::receive_message()` enters
   a polling loop capped at `poll_count` iterations (at most 50, covering up to
   5 seconds): `poll_clients_once(100)` → `recv_from_client()` →
   `tcp_recv_frame()` (reads length-prefixed frame from socket) →
   `Serializer::deserialize()` → `m_recv_queue.push()`, then `m_recv_queue.pop()`.
   Also checks `flush_delayed_to_queue()` for impairment-delayed messages.

3. Returns `OK` with `envelope` populated with the expired `DATA` message.

If `res != OK` (e.g., `ERR_TIMEOUT` from a genuinely empty queue): `receive()`
returns `res` immediately. This use case assumes `res == OK`.

**Step 3 — Envelope validation (DeliveryEngine.cpp:168)**

```
NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));
```

`envelope_valid()` (`MessageEnvelope.hpp:63`):
- `message_type != INVALID`: `DATA (0) != INVALID (255)` ✓.
- `payload_length <= MSG_MAX_PAYLOAD_BYTES`: any valid value ≤ 4096 ✓.
- `source_id != NODE_ID_INVALID`: valid sender node ✓.

The assertion passes. Expiry is a semantic property; `envelope_valid()` does not
check `expiry_time_us`. A structurally valid but semantically expired envelope
passes this assertion.

**Step 4 — Expiry check (DeliveryEngine.cpp:171-175) — CRITICAL DECISION**

```
if (timestamp_expired(env.expiry_time_us, now_us))
```

**Inside `timestamp_expired()`** (`Timestamp.hpp:51-59`):
- Fires assertions:
  - `NEVER_COMPILED_OUT_ASSERT(expiry_us <= 0xFFFFFFFFFFFFFFFFULL)` (trivially
    true for `uint64_t`).
  - `NEVER_COMPILED_OUT_ASSERT(now_us <= 0xFFFFFFFFFFFFFFFFULL)` (trivially
    true).
- Returns: `(expiry_us != 0ULL) && (now_us >= expiry_us)`.

For an expired message:
- `expiry_time_us = T_expiry > 0` → `(T_expiry != 0ULL) = true`.
- `now_us >= T_expiry` → true (clock has passed the expiry).
- Returns `true`.

**Step 5 — Expiry branch taken (DeliveryEngine.cpp:172-175)**

```
Logger::log(Severity::WARNING_LO, "DeliveryEngine",
            "Dropping expired message_id=%llu from src=%u",
            (unsigned long long)env.message_id, env.source_id);
return Result::ERR_EXPIRED;
```

`Logger::log()` at `WARNING_LO` severity: writes `message_id` and `source_id`
metadata to the log output (no payload content per REQ-7.1.4).

`return Result::ERR_EXPIRED` (`Result::ERR_EXPIRED = 7` from `Types.hpp:91`).

**Step 6 — Return to application**

`DeliveryEngine::receive()` returns `ERR_EXPIRED`. `env` is populated with the
expired envelope's fields (not cleared). The caller receives `ERR_EXPIRED` and
must not process `env`.

**What does NOT happen:**
- No ACK is sent. The sender's `RetryManager` slot remains active and may send
  retransmissions, which will also be dropped on arrival.
- No call to `m_dedup.check_and_record()`. `DuplicateFilter` is not consulted
  for expired messages. If the same message_id arrives again (retransmission),
  it will not be recognized as a duplicate; it will be re-evaluated for expiry.
- No call to `AckTracker::on_ack()` or `RetryManager::on_ack()`. These are
  reached only for `ACKED` control messages or after the expiry check passes.

**--- Special case: expiry_time_us = 0 (never expires) ---**

`timestamp_expired(0, now_us)`: `(0 != 0ULL) = false`. Returns false. The
expiry branch is not taken. The message passes through to control/dedup/deliver.
ACK envelopes always have `expiry_time_us = 0` (set by `envelope_make_ack()`
at `MessageEnvelope.hpp:101`). Data messages with no TTL requirement should
also use `expiry_time_us = 0`.

**--- Special case: now_us = 0 ---**

`NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)` at `DeliveryEngine.cpp:154` fires.
This assertion is `NEVER_COMPILED_OUT` — it aborts (debug) or triggers component
reset (production) in all builds. The application must never pass `now_us = 0`.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::receive(env, timeout_ms, now_us)        [DeliveryEngine.cpp:149]
|
+-- NEVER_COMPILED_OUT_ASSERT(m_initialized)
+-- NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
+-- [m_initialized guard: passes]
+-- m_transport->receive_message(env, timeout_ms)       [virtual dispatch, line 161]
|   \-- TcpBackend::receive_message()                   [TcpBackend.cpp:386]
|       +-- m_recv_queue.pop(envelope)                  [RingBuffer - SPSC]
|       |   [if queue non-empty: returns OK immediately]
|       +-- [if empty, polling loop up to 50 iterations]:
|       |   +-- poll_clients_once(100)                  [TcpBackend.cpp:326]
|       |   |   \-- recv_from_client(fd, 100)           [TcpBackend.cpp:224]
|       |   |       +-- tcp_recv_frame()                [SocketUtils.cpp]
|       |   |       |   +-- socket_recv_exact(4-byte len hdr)
|       |   |       |   |   \-- poll() + recv()         [POSIX]
|       |   |       |   \-- socket_recv_exact(payload)
|       |   |       |       \-- poll() + recv()         [POSIX]
|       |   |       +-- Serializer::deserialize()       [Serializer.cpp:175]
|       |   |       |   +-- reads 44-byte header
|       |   |       |   \-- memcpy() payload
|       |   |       \-- m_recv_queue.push(env)          [RingBuffer]
|       |   +-- m_recv_queue.pop(envelope)
|       |   +-- timestamp_now_us()                      [Timestamp.hpp:31]
|       |   +-- flush_delayed_to_queue(now_us)          [TcpBackend.cpp:306]
|       |   \-- m_recv_queue.pop(envelope)
|       \-- return OK (expired DATA envelope)
|
+-- NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))      [inline, line 168, passes]
+-- timestamp_expired(env.expiry_time_us, now_us)       [Timestamp.hpp:51, inline]
|   +-- NEVER_COMPILED_OUT_ASSERT(bounds - trivially true)
|   \-- return (expiry_us != 0ULL) && (now_us >= expiry_us)  [true]
+-- [branch: expired == true]
+-- Logger::log(WARNING_LO, "Dropping expired message_id=...")  [line 172]
\-- return ERR_EXPIRED                                  [line 175]

[envelope_is_control()   -- NOT REACHED]
[DuplicateFilter         -- NOT REACHED]
[AckTracker::on_ack()    -- NOT REACHED]
[RetryManager::on_ack()  -- NOT REACHED]
```

---

## 5. Key Components Involved

### DeliveryEngine (src/core/DeliveryEngine.cpp)
- **Responsibility:** The sole active component after the transport delivers the
  envelope. Checks validity, runs expiry, routes to ACK or dedup/deliver path.
  For an expired message, returns `ERR_EXPIRED` after logging.
- **Why in this flow:** It holds the expiry gate that all received messages must
  pass before delivery to the application.

### timestamp_expired() (Timestamp.hpp:51-59)
- **Responsibility:** Inline pure function. Takes `expiry_us` and `now_us` as
  `uint64_t` parameters. Returns `(expiry_us != 0ULL) && (now_us >= expiry_us)`.
  The `!= 0ULL` guard means zero expiry is treated as "never expires".
- **Why in this flow:** The single point of expiry decision. Called at line 171
  of `DeliveryEngine::receive()`, before any other application-level processing.

### timestamp_now_us() (Timestamp.hpp:31-45)
- **Responsibility:** Wraps `clock_gettime(CLOCK_MONOTONIC)`, returns `uint64_t`
  microseconds. Called by the application (not by `DeliveryEngine::receive()`
  itself) to produce `now_us`.
- **Why in this flow:** Supplies the reference time for the expiry comparison.
  `DeliveryEngine` does not re-query the clock internally during `receive()`.

### TcpBackend / RingBuffer
- **Responsibility:** Deserializes wire bytes and queues envelopes; pops them
  on `receive_message()`. The expired envelope was already deserialized
  (faithfully preserving `expiry_time_us`) and queued. TcpBackend does not
  check expiry; it delivers whatever was deserialized.
- **Why in this flow:** Provides the expired envelope to `DeliveryEngine`.

### Serializer::deserialize() (Serializer.cpp:175)
- **Responsibility:** Reads the 44-byte big-endian wire header, reads
  `expiry_time_us` as an 8-byte field at wire offset 32 (after
  `source_id` and `destination_id` at offsets 24 and 28), copies it verbatim
  into `env.expiry_time_us`. No expiry check or modification.
- **Why in this flow:** Preserves `expiry_time_us` exactly from the wire format
  as set by the sender at message creation time.

### Logger
- **Responsibility:** Emits `WARNING_LO "Dropping expired message_id=N from src=M"`.
  Uses a 512-byte stack buffer; writes to stderr.
- **Why in this flow:** The only external-observable side effect of the expiry
  drop.

### DuplicateFilter (not reached)
- **Why mentioned:** For `RELIABLE_RETRY` messages, `check_and_record()` would
  be called at line 203 if the expiry check passed. For expired messages, the
  expiry check at line 171 fires first and returns before reaching line 201.
  Expired retransmissions will NOT be recorded in the dedup window.

---

## 6. Branching Logic / Decision Points

**Branch 1: `m_initialized` guard (line 156-158)**
- Condition: `!m_initialized`
- Normal: continue. Error: returns `ERR_INVALID`.

**Branch 2: transport returns OK? (line 162-164)**
- Condition: `res != OK`
- Normal: continue with envelope. Error: returns transport error (`ERR_TIMEOUT`,
  `ERR_IO`, etc.). Expiry check not reached.

**Branch 3: NEVER_COMPILED_OUT_ASSERT(envelope_valid) (line 168)**
- An expired envelope still satisfies `envelope_valid()` (expiry is semantic,
  not structural). This assertion never fails on a well-formed expired message.

**Branch 4: timestamp_expired() — zero guard (Timestamp.hpp:58)**
- Sub-condition A: `expiry_time_us == 0ULL`
- If true: returns false immediately → message never expires.
- If false: proceeds to comparison.

**Branch 5: timestamp_expired() — time comparison (Timestamp.hpp:58)**
- Sub-condition B: `now_us >= expiry_time_us`
- If true (expired): returns true → `receive()` drops and returns `ERR_EXPIRED`.
- If false (not yet): returns false → `receive()` continues to control/dedup/deliver.

**Branch 6 (reached only if NOT expired): control message detection (line 179)**
- `envelope_is_control(env)`: routes ACK to `on_ack()` pair (UC_08).
- Not taken for this use case (message is `DATA`).

**Branch 7 (reached only if NOT expired): dedup check (line 201-210)**
- `reliability_class == RELIABLE_RETRY`: calls `DuplicateFilter::check_and_record()`.
- Not taken for this use case.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. `receive()` executes synchronously on the caller's thread.

### Where context switches occur
Inside `TcpBackend::receive_message()` at `poll()` and `recv()` calls within
`tcp_recv_frame()`. These may block for up to `timeout_ms` per polling iteration.
The expiry check itself (`timestamp_expired()`) is non-blocking.

### Synchronization primitives
`timestamp_expired()` and `timestamp_now_us()` are stateless inline/static
functions. `clock_gettime(CLOCK_MONOTONIC)` is POSIX async-signal-safe and
thread-safe, but is not called inside `receive()` for the expiry check — `now_us`
is a parameter.

`RingBuffer::pop()` uses atomic acquire-load on `m_head` and release-store on
`m_tail` for SPSC correctness.

`DeliveryEngine` members (`m_ack_tracker`, `m_dedup`, `m_retry_manager`) are
not touched during an expiry drop. Even if these were accessed concurrently
from another thread, an expiry drop does not interact with them.

### Timing race on now_us
The caller captures `now_us` before calling `receive()`, then `receive()` may
block inside `TcpBackend::receive_message()` for up to `poll_count * 100 ms`
(maximum 5000 ms, capped at 50 iterations in `TcpBackend.cpp:396-400`). If a
message's `expiry_time_us` falls within the polling window, it may be accepted
as non-expired at the time of the `now_us` capture but actually expire during
the transport wait. The current code does not re-capture `now_us` after the
transport returns. This errs toward delivering messages that borderline-expire
during the receive wait. [OBSERVATION]

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### env (DeliveryEngine::receive() parameter, MessageEnvelope&)
Passed by non-const reference. On `ERR_EXPIRED` return, `env` is populated with
the expired message's full content (header + payload). The caller owns the
storage. `DeliveryEngine` does not clear `env` on an expiry drop. If the caller
inspects `env` after receiving `ERR_EXPIRED`, it will see the expired message's
fields — the payload data is present and accessible. The API contract requires
the caller to not process `env` on `ERR_EXPIRED`, but no enforcement exists.

### expiry_time_us (uint64_t, field of MessageEnvelope)
Read-only during `timestamp_expired()`. Not modified anywhere on the receive
path. Set by the sender during message construction; preserved exactly through
`Serializer::serialize()` (8-byte big-endian at wire offset 32) and
`Serializer::deserialize()` (reads the same 8-byte field back as a `uint64_t`
via `read_u64()`).

### now_us (uint64_t, parameter to receive())
Passed by value. A snapshot of `CLOCK_MONOTONIC` at the time of the call (or
slightly before). Used only in `timestamp_expired()` comparison; not stored by
`DeliveryEngine`.

### RingBuffer consumption
The expired envelope was already popped from `m_recv_queue` inside
`TcpBackend::receive_message()`. `m_head` was advanced (atomic release-store).
This consumption is permanent; the expired message is no longer in the queue.

### No heap allocation
`timestamp_expired()` allocates nothing. `DeliveryEngine::receive()` allocates
nothing beyond what `TcpBackend::receive_message()` allocates on its stack.

---

## 9. Error Handling Flow

### ERR_EXPIRED (Result::ERR_EXPIRED = 7)
- Generated at `DeliveryEngine.cpp:175`.
- Logged at `WARNING_LO` (localized, recoverable) at lines 172-174.
- Returned to `DeliveryEngine`'s caller.
- The caller must handle `ERR_EXPIRED` by not processing `env`. Typically the
  application increments a drop counter (if it maintains metrics), logs the
  event, and continues polling.

### No cleanup beyond the log
- No ACK is sent. No dedup recording. No tracker interaction.
- If the sender used `RELIABLE_RETRY`, it will continue retransmitting until
  its `RetryManager` slot expires or exhausts retries. The receiver will drop
  each retransmission via the same expiry path, generating one `WARNING_LO`
  per retransmission.

### NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
Always active. Guards against a zero-timestamp caller programming error.

---

## 10. External Interactions

### Network calls
Inside `TcpBackend::receive_message()` during the polling loop:
- `poll(pfd, 1, timeout_ms)` — waits for socket readability.
- `recv(fd, buf, len, flags)` (inside `tcp_recv_frame()` → `socket_recv_exact()`)
  — reads exactly the framed bytes.
These are the only kernel entries. The expiry check itself has no kernel calls.

### clock_gettime(CLOCK_MONOTONIC)
Called by the application (not by `receive()` internally) to produce `now_us`
before calling `receive()`. `timestamp_now_us()` wraps this call
(`Timestamp.hpp:31-45`).

### Logger
`Logger::log(WARNING_LO, "DeliveryEngine", "Dropping expired message_id=%llu
from src=%u", ...)` at `DeliveryEngine.cpp:172-174`. This is the only external
interaction during an expiry drop.

### File I/O, hardware, IPC
None.

---

## 11. State Changes / Side Effects

### DeliveryEngine
No state changes. `m_ack_tracker`, `m_dedup`, `m_retry_manager` are untouched.

### TcpBackend / RingBuffer
The expired envelope has already been popped from `m_recv_queue` inside
`receive_message()`. `m_head` was atomically incremented. This consumption is
permanent and cannot be reversed.

### Logger
One `WARNING_LO` log entry with `message_id` and `source_id`.

### Summary
The expiry drop is effectively stateless at the `DeliveryEngine` level. The only
permanent side effects are the queue consumption (inside transport receive) and
the log emission. No application-visible state is changed.

---

## 12. Sequence Diagram using mermaid

```text
Application     DeliveryEngine     TcpBackend      RingBuffer    timestamp_expired    Logger
    |                 |                 |               |               |                |
    |--now_us =       |                 |               |               |                |
    |  timestamp_     |                 |               |               |                |
    |  now_us()       |                 |               |               |                |
    |                 |                 |               |               |                |
    |--receive(env,   |                 |               |               |                |
    |  timeout, now)->|                 |               |               |                |
    |                 |--recv_msg()---->|               |               |                |
    |                 |                 |--pop(env)---->|               |                |
    |                 |                 |   [expired    |               |                |
    |                 |                 |   DATA env]   |               |                |
    |                 |                 |<--OK, env-----|               |                |
    |                 |<--OK, env-------|               |               |                |
    |                 |                                 |               |                |
    |                 |--ASSERT(envelope_valid) [pass]  |               |                |
    |                 |                                 |               |                |
    |                 |--timestamp_expired(expiry, now)---------------->|                |
    |                 |    (expiry_us != 0) && (now >= expiry_us)       |                |
    |                 |<--true--------------------------|-----------<---|                |
    |                 |                                 |               |                |
    |                 |--log("Dropping expired message_id=N from src=M")--------------->|
    |                 |                                 [WARNING_LO]    |                |
    |<--ERR_EXPIRED---|                                 |               |                |
    |                 |                                 |               |                |
    [Application does NOT process env]
    [envelope_is_control()    NOT REACHED]
    [DuplicateFilter          NOT REACHED]
    [AckTracker               NOT REACHED]
    [RetryManager             NOT REACHED]
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

No expiry-specific initialization. `timestamp_expired()` is a pure function
with no state. The `expiry_time_us` field in each `MessageEnvelope` is set by
the sender at message creation time, stored in the wire format, and preserved
through `Serializer::deserialize()`.

### What happens during steady-state execution

**Message creation (sender side):**
The application sets `env.expiry_time_us = timestamp_deadline_us(now_us, ttl_ms)`.
`timestamp_deadline_us()` returns `now_us + ttl_ms * 1000ULL`. For a message
that never expires, the application sets `env.expiry_time_us = 0`.

**Message transit:**
`Serializer::serialize()` writes `expiry_time_us` as an 8-byte big-endian field
at offset 32 in the wire format. `Serializer::deserialize()` reads it back via
`read_u64()`. The value is not modified by `TcpBackend`, `RingBuffer`,
`ImpairmentEngine`, or any transport layer.

**Expiry check (this use case):**
Performed once per received message, inline, at `DeliveryEngine.cpp:171`,
immediately after `envelope_valid()` and before any control/dedup/deliver
routing. The check is O(1) with two `uint64_t` comparisons.

**Impact of impairment-induced latency:**
If `ImpairmentEngine` applies fixed latency or jitter and the message is held
in `m_delay_buf[]` past its `expiry_time_us`, it will still be released by
`collect_deliverable()` (the impairment engine does not check expiry), pushed
to `m_recv_queue`, popped by `receive_message()`, and then dropped by
`DeliveryEngine::receive()` via the expiry check. This is the correct and
intended behavior: expiry is enforced at the delivery gate, not at the
impairment buffer.

---

## 14. Known Risks / Observations

### Risk 1 — now_us is caller-supplied, not captured inside receive()

The caller captures `now_us` before calling `receive()`. `receive()` may block
inside `TcpBackend::receive_message()` for up to `poll_count * 100 ms` (max
5000 ms per `TcpBackend.cpp:396-400`) before the expiry check runs. A message
with `expiry_time_us` within the polling window may appear non-expired at the
time of the `now_us` capture but actually expire during the transport wait. The
expiry check will then pass (false negative), and the message will be delivered
to the application after its intended TTL has elapsed.

Mitigation: re-capture `now_us` inside `DeliveryEngine::receive()` immediately
before `timestamp_expired()`. The current code does not do this. [RISK]

### Risk 2 — Expired envelope content visible to caller

On `ERR_EXPIRED` return, `env` is fully populated with the expired message.
If the caller ignores the return code and processes `env`, it will act on stale
data. No defensive zeroing of `env` is performed on expiry drop. [OBSERVATION]

### Risk 3 — No ACK sent on expiry drop

The sender's `RetryManager` will continue retrying the expired message until
retry count exhaustion or until `slot_has_expired()` fires in
`RetryManager::collect_due()`. The receiver silently drops without ACK, causing
the sender to waste retries. Each retransmission generates a fresh
`WARNING_LO` log entry on the receiver. [OBSERVATION]

### Risk 4 — Dedup window NOT updated on expiry drop

If the same message arrives multiple times (retransmissions of a
`RELIABLE_RETRY` message) and each arrival is expired, none are recorded in
`DuplicateFilter`. The expiry check at line 171 fires before the dedup check
at line 201. Each retransmission generates a fresh `WARNING_LO` log for the
same `message_id`. [OBSERVATION]

### Risk 5 — Clock domain consistency

`timestamp_now_us()` uses `CLOCK_MONOTONIC`. If the sender set `expiry_time_us`
using a different clock source (e.g., `CLOCK_REALTIME`) or a different host's
monotonic clock, the comparison is between incompatible clock domains and the
expiry decision will be incorrect. The codebase uses `timestamp_now_us()`
consistently within a single process; cross-host expiry requires clock
synchronization (NTP, PTP, or GPS). [OBSERVATION]

### Risk 6 — CLOCK_MONOTONIC behavior during system suspension

On desktop targets, `CLOCK_MONOTONIC` pauses during system sleep/suspension.
A message sent with a 5-second TTL on a system that is then suspended for 30
seconds will appear non-expired when the system resumes (the monotonic clock
advanced by only ~0 seconds during the suspension). This would deliver messages
well past their intended wall-clock TTL. [OBSERVATION]

---

## 15. Unknowns / Assumptions

- [CONFIRMED] `timestamp_expired(0, now_us)` returns false. Zero `expiry_time_us`
  means "never expires". ACK envelopes always have `expiry_time_us = 0` (set by
  `envelope_make_ack()` at `MessageEnvelope.hpp:101`). Confirmed from
  `Timestamp.hpp:51-59`.

- [CONFIRMED] The expiry check fires at `DeliveryEngine.cpp:171`, before
  `envelope_is_control()` (line 179) and before `DuplicateFilter::check_and_record()`
  (line 203). Confirmed from `DeliveryEngine.cpp:149-222`.

- [CONFIRMED] `DeliveryEngine::receive()` does not re-query the clock internally
  for the expiry check. `now_us` is the parameter passed by the caller.
  `TcpBackend::receive_message()` independently calls `timestamp_now_us()` at
  `TcpBackend.cpp:411` for `flush_delayed_to_queue()`, but this is not used for
  the expiry gate. Confirmed from `DeliveryEngine.cpp` and `TcpBackend.cpp`.

- [ASSUMPTION 1] The caller captures `now_us` via `timestamp_now_us()` immediately
  before calling `receive()`. If `now_us` is significantly stale, messages that
  have expired since the capture will be accepted as non-expired.

- [ASSUMPTION 2] The sender used `timestamp_deadline_us()` (which uses
  `CLOCK_MONOTONIC` via `timestamp_now_us()`) when setting `expiry_time_us`.
  This ensures clock consistency between sender and receiver on the same host.
  For inter-host deployments, clock synchronization is required.

- [ASSUMPTION 3] `expiry_time_us` is set by the application layer, not by
  `TcpBackend` or `Serializer`. `Serializer` preserves it verbatim.

- [ASSUMPTION 4] The application handles `ERR_EXPIRED` by not processing `env`.
  The `DeliveryEngine` API does not enforce this by clearing `env`.

- [UNKNOWN 1] Whether the application layer maintains a counter of expiry drops
  for metrics purposes. No metrics hooks are visible in `DeliveryEngine` (only
  `Logger` calls).

- [UNKNOWN 2] Whether impairment-induced delay is accounted for when setting
  message TTLs. If `ImpairmentEngine` adds 5 seconds of latency and the message
  TTL is 3 seconds, all messages will be expired on arrival. TTLs must be set
  generously enough to accommodate the maximum configured impairment delay.
