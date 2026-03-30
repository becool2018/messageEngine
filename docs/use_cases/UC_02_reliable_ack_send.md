# UC_02 — Reliable-with-ACK Send

**HL Group:** HL-2 — User sends a message requiring confirmation
**Actor:** User
**Requirement traceability:** REQ-3.3.2, REQ-3.2.4, REQ-3.2.1, REQ-3.2.3, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

### Clear description of what triggers this flow

A client application populates a `MessageEnvelope` with `message_type = DATA`,
`reliability_class = RELIABLE_ACK`, fills in `destination_id`, `priority`,
`expiry_time_us`, `payload`, and `payload_length`, then calls
`DeliveryEngine::send(env, now_us)`. The engine is initialized with a
`TcpBackend` instance and a `ChannelConfig` that supplies `recv_timeout_ms`
(used as the ACK deadline duration). The TCP connection to the server peer was
established during `TcpBackend::init()`.

This use case covers only the send phase. The ACK resolution phase (when the
remote peer's ACK arrives and is processed) is documented in UC_08.

### Expected outcome (single goal)

The envelope is stamped with `source_id` and a newly generated `message_id`,
serialized to wire format, subjected to impairment evaluation, and sent over
TCP as a length-prefixed frame. After a successful transport send, the engine
computes an ACK deadline (`now_us + recv_timeout_ms * 1000`) and calls
`AckTracker::track()` to register a `PENDING` entry. `RetryManager::schedule()`
is **not** called because `RELIABLE_ACK` does not auto-retry. The call returns
`Result::OK` to the application. The `AckTracker` slot remains `PENDING` until
UC_08 (ACK resolution) or `sweep_ack_timeouts()` clears it.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point:** `DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us)`
  — `DeliveryEngine.cpp` line 77.
- **Caller:** Application code in `src/app` or a test harness. No thread is
  created inside this call; it executes synchronously on the caller's thread.
- **Pre-condition:** `DeliveryEngine::init()` has been called successfully
  (`m_initialized = true`). `TcpBackend::init()` has been called (`m_open = true`,
  `m_client_count >= 1`). `m_ack_tracker` has been initialized with all slots
  `FREE`, `m_count = 0`.

### Example: main(), ISR, callback, RPC handler, etc.

Application calls `DeliveryEngine::send()` from its main loop after populating
an envelope with `RELIABLE_ACK`. No ISR or RPC mechanism is present in the
codebase.

---

## 3. End-to-End Control Flow (Step-by-Step)

**--- Phase A: Transport send (identical to UC_01 steps 1–24) ---**

1. **Application** constructs a `MessageEnvelope` on the stack with
   `message_type = DATA`, `reliability_class = RELIABLE_ACK`, and calls
   `DeliveryEngine::send(env, now_us)`.

2. **`DeliveryEngine::send()`** (`DeliveryEngine.cpp:77`) fires precondition
   assertions `m_initialized` and `now_us > 0`. Checks `!m_initialized` guard.
   Stamps `env.source_id = m_local_id` (line 88), `env.message_id =
   m_id_gen.next()` (line 89), `env.timestamp_us = now_us` (line 90).

3. Calls `send_via_transport(env)` at line 93.

4. **`send_via_transport()`** fires assertions, then calls
   `m_transport->send_message(env)` via virtual dispatch to
   `TcpBackend::send_message()`.

5. **`TcpBackend::send_message()`** (`TcpBackend.cpp:347`):
   - Calls `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES,
     wire_len)` — produces 44-byte big-endian header plus payload bytes in
     `m_wire_buf`.
   - Calls `timestamp_now_us()` for the impairment engine.
   - Calls `m_impairment.process_outbound(envelope, now_us)` — with impairments
     disabled (default), queues the envelope in the delay buffer with
     `release_us = now_us`. Returns `Result::OK`.
   - Checks `m_client_count == 0` (false, client connected). Calls
     `send_to_all_clients(m_wire_buf, wire_len)` — calls `tcp_send_frame()` for
     each connected fd, which calls `socket_send_all()` twice (4-byte length
     header, then serialized body), using `poll()` + `send()` syscalls.
   - Calls `flush_delayed_to_clients(now_us)` — drains the delay buffer slot
     placed above, re-serializes, sends again (same double-send observation as
     UC_01). Returns `Result::OK`.

6. **`send_via_transport()`** returns `Result::OK` to `send()`.

7. **`DeliveryEngine::send()`** checks `res != Result::OK` at line 94 (passes).

**--- Phase B: ACK tracking registration ---**

8. **`DeliveryEngine::send()`** evaluates reliability class at line 102:
   `RELIABLE_ACK` satisfies `(env.reliability_class == RELIABLE_ACK ||
   env.reliability_class == RELIABLE_RETRY)`. **Branch taken.**

9. Computes `ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms)`
   at line 106. `timestamp_deadline_us()` returns `now_us + recv_timeout_ms *
   1000ULL`. Default `recv_timeout_ms` is 1000 ms (configurable);
   `ack_deadline = now_us + 1 000 000 µs`.

10. Calls `m_ack_tracker.track(env, ack_deadline)` at line 110.

11. **`AckTracker::track()`** (`AckTracker.cpp:44`):
    - Fires precondition assertion: `m_count <= ACK_TRACKER_CAPACITY`.
    - Iterates `m_slots[0..ACK_TRACKER_CAPACITY-1]` (fixed bound, 32 slots) for
      the first `EntryState::FREE` slot.
    - On finding slot `i`: calls `envelope_copy(m_slots[i].env, env)` (memcpy of
      `sizeof(MessageEnvelope)` bytes, approximately 4136 bytes). Sets
      `m_slots[i].deadline_us = ack_deadline`. Sets
      `m_slots[i].state = EntryState::PENDING`. Increments `m_count`.
    - Fires postcondition assertions: `m_slots[i].state == PENDING`,
      `m_count <= ACK_TRACKER_CAPACITY`.
    - Returns `Result::OK`.

12. **`DeliveryEngine::send()`** checks `track_res != Result::OK` at line 111.
    For success: passes. For `ERR_FULL` (all 32 slots occupied): logs
    `WARNING_HI` ("Failed to track ACK for message_id=...") and continues
    without failing the send. ACK tracking failure does not fail the send.

**--- Phase C: Retry scheduling skipped ---**

13. **`DeliveryEngine::send()`** evaluates the retry conditional at line 120:
    `env.reliability_class == RELIABLE_RETRY` is **false** for `RELIABLE_ACK`.
    **`RetryManager::schedule()` is never called.**

**--- Phase D: Completion ---**

14. **`DeliveryEngine::send()`** fires postcondition assertions:
    `env.source_id == m_local_id`, `env.message_id != 0ULL`. Logs
    `INFO "Sent message_id=%llu, reliability=1"`. Returns `Result::OK`.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::send()                         [DeliveryEngine.cpp:77]
|
+-- m_id_gen.next()                            [MessageId.hpp - inline]
|
+-- DeliveryEngine::send_via_transport()       [DeliveryEngine.cpp:56]
|   |
|   +-- TcpBackend::send_message()             [TcpBackend.cpp:347]
|       +-- Serializer::serialize()            [Serializer.cpp:117]
|       |   +-- envelope_valid()               [MessageEnvelope.hpp:63]
|       |   +-- write_u8() x4, write_u64() x3, write_u32() x3
|       |   \-- memcpy()
|       +-- timestamp_now_us()                 [Timestamp.hpp:31]
|       +-- ImpairmentEngine::process_outbound()
|       |   \-- queue_to_delay_buf()
|       |       \-- envelope_copy() -> memcpy()
|       +-- send_to_all_clients()              [TcpBackend.cpp:258]
|       |   \-- tcp_send_frame()               [SocketUtils.cpp]
|       |       +-- socket_send_all()          (4-byte length header)
|       |       |   \-- poll() + send()        [POSIX]
|       |       \-- socket_send_all()          (serialized payload)
|       |           \-- poll() + send()        [POSIX]
|       \-- flush_delayed_to_clients()         [TcpBackend.cpp:280]
|           +-- ImpairmentEngine::collect_deliverable()
|           |   \-- envelope_copy()
|           +-- Serializer::serialize()
|           \-- send_to_all_clients()
|               \-- tcp_send_frame() [repeat as above]
|
+-- timestamp_deadline_us()                    [Timestamp.hpp:65 - inline]
|
+-- AckTracker::track(env, deadline_us)        [AckTracker.cpp:44]
|   \-- envelope_copy()                        [MessageEnvelope.hpp:56]
|       \-- memcpy()
|
[RetryManager::schedule() -- NOT CALLED (RELIABLE_ACK branch skipped)]
```

---

## 5. Key Components Involved

### DeliveryEngine
- **Responsibility:** Stamps the envelope, dispatches to transport, then
  selectively engages ACK tracking and retry scheduling based on
  `reliability_class`. For `RELIABLE_ACK`, engages `AckTracker` but not
  `RetryManager`.
- **Why in this flow:** It is the sole send API. The conditional at lines
  102–103 distinguishes `RELIABLE_ACK` from `BEST_EFFORT` and `RELIABLE_RETRY`.

### AckTracker
- **Responsibility:** Maintains a fixed-size slot table (`ACK_TRACKER_CAPACITY
  = 32`) of `Entry` structs, each holding a full `MessageEnvelope` copy, a
  deadline, and a state (`FREE`, `PENDING`, `ACKED`). `track()` registers a new
  `PENDING` entry. `on_ack()` (called from `receive()` in UC_08) transitions it
  to `ACKED`. `sweep_expired()` reclaims `ACKED` and timed-out `PENDING` slots.
- **Why in this flow:** `RELIABLE_ACK` semantics require the engine to know
  whether the remote peer ACKed the message; this tracking is the mechanism.

### timestamp_deadline_us()
- **Responsibility:** Inline function in `Timestamp.hpp:65`. Computes
  `now_us + duration_ms * 1000ULL`. Used here to compute the ACK deadline.
- **Why in this flow:** The ACK deadline is the absolute time after which
  `sweep_ack_timeouts()` will log a timeout and reclaim the slot.

### TcpBackend, Serializer, ImpairmentEngine, tcp_send_frame(), socket_send_all()
- Same roles as documented in UC_01. See UC_01 section 5 for full descriptions.

### RetryManager
- **Responsibility:** Schedules and manages retry of `RELIABLE_RETRY` messages.
- **Why in this flow:** Exists as a `DeliveryEngine` member but is **not
  invoked**. The conditional at `DeliveryEngine.cpp:120` evaluates `false` for
  `RELIABLE_ACK`, so `RetryManager::schedule()` is never called.

---

## 6. Branching Logic / Decision Points

**Branch 1: `m_initialized` guard (line 82)**
- Condition: `!m_initialized`
- Normal: continue. Error: returns `ERR_INVALID`.

**Branch 2: `send_via_transport()` return check (line 94)**
- Condition: `res != Result::OK`
- Normal: continue to ACK tracking. Error: logs `WARNING_LO`, returns `res`.
  Note: if the transport send fails, `AckTracker::track()` is not called.

**Branch 3: Reliability class conditional — AckTracker (lines 102–103)**
- Condition: `RELIABLE_ACK || RELIABLE_RETRY`
- For `RELIABLE_ACK`: **condition is true**. `AckTracker::track()` is called.
- For `BEST_EFFORT`: false — no tracking.

**Branch 4: `AckTracker::track()` return check (line 111)**
- Condition: `track_res != Result::OK`
- Normal (`OK`): continue.
- `ERR_FULL` (32 slots occupied): logs `WARNING_HI` ("Failed to track ACK"),
  continues. The send is still considered successful; ACK is not tracked.

**Branch 5: Reliability class conditional — RetryManager (line 120)**
- Condition: `RELIABLE_RETRY`
- For `RELIABLE_ACK`: **condition is false**. `RetryManager::schedule()` is
  never called.

**Branch 6: AckTracker slot search — FREE slot found vs not found**
- `AckTracker::track()` scans 0..31 for `EntryState::FREE`.
- Found: copies envelope, sets `PENDING`, increments `m_count`, returns `OK`.
- Not found: asserts `m_count == ACK_TRACKER_CAPACITY`, returns `ERR_FULL`.

**Branches 7–11: Same as UC_01 branches 5–9** (impairment, client count,
fd validity, send frame failure, poll timeout).

---

## 7. Concurrency / Threading Behavior

### Threads created
None. The entire send path executes synchronously on the caller's thread.

### Where context switches occur
Only at POSIX blocking calls inside `socket_send_all()`: `poll()` and `send()`.

### Synchronization primitives
None in the `AckTracker`. `m_slots[]` and `m_count` are plain member variables
with no atomics or mutex. If `DeliveryEngine::receive()` is called concurrently
with `send()` (which calls `AckTracker::track()`), there is a data race on
`m_slots[]` and `m_count`. No synchronization is present.

The `RingBuffer` receive queue uses `std::atomic<uint32_t>` for head/tail but
is not accessed during the send path.

### Producer/consumer relationships
`AckTracker` acts as a producer (slots filled by `track()`) and consumer (slots
cleared by `on_ack()` and `sweep_one_slot()`). In the single-threaded model,
there is no concurrent access.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory
- `MessageEnvelope& env` in the application: caller-owned, stack-allocated.
- `AckTracker::m_slots[ACK_TRACKER_CAPACITY]` (32 entries): member array of
  `AckTracker`, which is a member of `DeliveryEngine` by value. Each entry
  holds a full `MessageEnvelope` copy (~4136 bytes). Total for `m_slots`: ~132 KB.
- `AckTracker::track()` copies the envelope via `envelope_copy()` (which calls
  `memcpy(&dst, &src, sizeof(MessageEnvelope))`). The stored copy is independent
  of the caller's stack envelope.
- All other memory is as documented in UC_01 (no heap allocation anywhere).

### Lifetime of key objects
- The `AckTracker` slot holds a live copy of the envelope from the moment
  `track()` returns until `on_ack()` or `sweep_expired()` reclaims it.
- `TcpBackend` and `DeliveryEngine` must outlive all send/receive calls.

### Stack vs heap usage
Same as UC_01. No heap allocation. `AckTracker::track()` adds negligible stack
(only loop index and return value).

### RAII usage
Same as UC_01. `TcpBackend` destructor calls `TcpBackend::close()`.

### Potential leaks or unsafe patterns
- If `AckTracker` is full (`ERR_FULL`) and the send returns `OK`, the message
  was transmitted but will never be confirmed or cleaned up. The application
  has no signal that ACK tracking was skipped. [OBSERVATION]
- Same double-send risk from `flush_delayed_to_clients()` as documented in
  UC_01. [OBSERVATION - potential bug]

---

## 9. Error Handling Flow

### How errors propagate
- Transport send failure (step 5) causes `send()` to return the error code
  before `AckTracker::track()` is ever called. The ACK tracker is untouched.
- `AckTracker::track()` returning `ERR_FULL` (step 12) is logged at
  `WARNING_HI` but does not fail `send()`. The application receives `OK` but
  the message is not being tracked for ACK.
- All other error paths are identical to UC_01 (see UC_01 section 9).

### Retry logic
None. `RELIABLE_ACK` does not auto-retry. If the ACK does not arrive before the
`deadline_us`, `sweep_ack_timeouts()` will log `WARNING_HI` and release the
slot. No retransmission occurs.

### Fallback paths
- If `m_client_count == 0`, `send_message()` returns `OK` without sending.
  `AckTracker::track()` would still have been called if `send_via_transport()`
  returned `OK`.
- Note: `send_via_transport()` returns `OK` even when `send_to_all_clients()`
  silently absorbs all per-client send failures (void return). In that case,
  `AckTracker::track()` is called despite the message never reaching the peer.

---

## 10. External Interactions

### Network calls
Same as UC_01: `poll()` and `send()` syscalls inside `socket_send_all()`.

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
- `env.source_id`, `env.message_id`, `env.timestamp_us` stamped as in UC_01.

**`MessageIdGen m_id_gen` (member of `DeliveryEngine`):**
- Counter incremented by `next()`.

**`AckTracker::m_slots[i]` (member of `DeliveryEngine`):**
- `m_slots[i].state`: `FREE` → `PENDING`.
- `m_slots[i].env`: populated with `envelope_copy()` from `env`.
- `m_slots[i].deadline_us`: set to `now_us + recv_timeout_ms * 1000`.
- `AckTracker::m_count`: incremented by 1.

**`ImpairmentEngine::m_delay_buf[]` and `m_wire_buf`:**
- Transiently modified and restored within `send_message()`, as in UC_01.

**TCP kernel send buffer:**
- Bytes enqueued for transmission.

### Persistent changes (DB, disk, device state)
None.

---

## 12. Sequence Diagram using mermaid

```text
Participants:
  App  DeliveryEngine  TcpBackend  AckTracker  RetryManager  Serializer  SocketUtils

Flow (Phase A: transport send):

App --send(env=RELIABLE_ACK, now_us)--> DeliveryEngine
  stamp source_id, message_id, timestamp_us
  send_via_transport(env)
    DeliveryEngine --> TcpBackend::send_message(env)
      Serializer::serialize(env) --> m_wire_buf
      ImpairmentEngine::process_outbound() [disabled: queue+immediate]
      send_to_all_clients(m_wire_buf, wire_len)
        tcp_send_frame(fd, buf, len)
          socket_send_all(length_header) --> poll()+send()
          socket_send_all(payload) --> poll()+send()
      flush_delayed_to_clients() [re-serialize + send again]
      returns OK
    returns OK

Flow (Phase B: ACK tracking):

  ack_deadline = timestamp_deadline_us(now_us, recv_timeout_ms)
  DeliveryEngine --> AckTracker::track(env, ack_deadline)
    [scan slots 0..31 for FREE]
    envelope_copy(slots[i].env, env)   [memcpy ~4136 bytes]
    slots[i].deadline_us = ack_deadline
    slots[i].state = PENDING
    m_count++
    returns OK
  [track_res == OK: continue]

Flow (Phase C: retry skip):

  [RELIABLE_ACK: retry conditional FALSE]
  [RetryManager::schedule() NOT CALLED]

Flow (Phase D: completion):

  asserts source_id, message_id
  Logger::log(INFO, "Sent message_id=N, reliability=1")
App <-- Result::OK
[AckTracker slot i: state=PENDING, deadline=now+timeout]
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

Same as UC_01 for `TcpBackend` and `DeliveryEngine`.

`AckTracker::init()` (`AckTracker.cpp:23-38`): calls `memset(m_slots, 0,
sizeof(m_slots))` (zeroes all 32 `Entry` structs including `state = FREE = 0`),
sets `m_count = 0`, then loops 0..31 asserting each slot is `EntryState::FREE`.

### What happens during steady-state execution

Each `DeliveryEngine::send()` call for `RELIABLE_ACK` consumes one
`AckTracker` slot. Slots accumulate until either:
- `on_ack()` transitions them to `ACKED` (UC_08), and
- `sweep_ack_timeouts()` calls `sweep_expired()` to reclaim `ACKED` slots.

Or until the deadline passes without an ACK, at which point `sweep_expired()`
reports the timeout at `WARNING_HI` and reclaims the slot. The application must
call `sweep_ack_timeouts()` periodically to prevent the 32-slot table from
filling.

---

## 14. Known Risks / Observations

### AckTracker full silently degrades reliability

If `AckTracker` is full (all 32 slots `PENDING`), `track()` returns `ERR_FULL`.
`DeliveryEngine::send()` logs `WARNING_HI` and continues, returning `OK` to
the application. The message was sent over the wire, but the application has
no ACK tracking for it. This silently downgrades `RELIABLE_ACK` to
`BEST_EFFORT` behavior for that message. [RISK]

### ACK deadline based on send-time now_us, not transport completion time

`ack_deadline = timestamp_deadline_us(now_us, recv_timeout_ms)` where `now_us`
is the time passed to `send()` by the application, not the time when the
transport actually transmitted the bytes. Any processing latency between the
application's `timestamp_now_us()` capture and the moment TCP delivers the bytes
to the kernel reduces the effective ACK window. For small payloads over fast
links this is negligible; for large payloads or congested links it could matter.
[OBSERVATION]

### Double-send from delay buffer passthrough

Same issue as UC_01: with impairments disabled, `flush_delayed_to_clients()`
sends the same envelope a second time in the same `send_message()` call.
[OBSERVATION - potential bug]

### Source ID matching issue in AckTracker::on_ack()

When `on_ack()` is called (UC_08), it matches `m_slots[i].env.source_id`
(the local node's ID, set during `send()`) against the incoming ACK's
`source_id` (the remote peer's ID). In a two-node deployment these are
different values. The slot will not match, `on_ack()` returns `ERR_INVALID`,
and the slot remains `PENDING` until `sweep_expired()` fires its timeout.
The `WARNING_HI` for ACK timeout will fire even for messages the remote peer
successfully acknowledged. [RISK - appears to be a latent bug]

### Unsynchronized shared state

Same as UC_01: no mutex protects `m_ack_tracker`, `m_wire_buf`, `m_client_fds[]`,
or `m_impairment`. [RISK]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `m_cfg.recv_timeout_ms` is set by the `ChannelConfig` passed to
  `DeliveryEngine::init()`. Default via `channel_config_default()` is 1000 ms.
  The application may override this in its channel configuration.

- [ASSUMPTION] `AckTracker` has capacity `ACK_TRACKER_CAPACITY = 32`.
  Confirmed from `Types.hpp:24`. Under high message rates, this capacity can
  be exhausted if `sweep_ack_timeouts()` is not called frequently enough.

- [ASSUMPTION] The application calls `sweep_ack_timeouts()` periodically to
  prevent the `AckTracker` from filling with stale `PENDING` or `ACKED` slots.
  If not called, the tracker fills within 32 sent messages.

- `timestamp_deadline_us(now_us, duration_ms)` returns
  `now_us + duration_ms * 1000ULL`. Confirmed from `Timestamp.hpp:65-75`.

- [ASSUMPTION] The remote peer generates ACK messages using `envelope_make_ack()`
  (`MessageEnvelope.hpp:88`), which sets `ack.message_id = original.message_id`
  and `ack.source_id = my_id` (the peer's node ID). Due to the source_id
  matching issue described in section 14, `AckTracker::on_ack()` will fail to
  match the slot in a standard two-node setup.

- [ASSUMPTION] `send_via_transport()` returns `OK` even when
  `send_to_all_clients()` silently absorbs all per-connection send failures
  (because `send_to_all_clients()` returns void). In that scenario,
  `AckTracker::track()` is still called for a message that never left the
  process.
