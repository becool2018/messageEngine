# UC_01 — Best-Effort Send

**HL Group:** HL-1 — User sends a fire-and-forget message
**Actor:** User
**Requirement traceability:** REQ-3.3.1, REQ-3.2.1, REQ-3.2.3, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

### Clear description of what triggers this flow

A client application populates a `MessageEnvelope` with `message_type = DATA`,
`reliability_class = BEST_EFFORT`, fills in `destination_id`, `priority`,
`expiry_time_us`, `payload`, and `payload_length`, then calls
`DeliveryEngine::send(env, now_us)`. The engine is already initialized with a
`TcpBackend` instance as its `TransportInterface`. The TCP connection to the
server peer was established during `TcpBackend::init()`.

### Expected outcome (single goal)

The envelope is stamped with `source_id` and a newly generated `message_id`,
serialized to a 44-byte header plus payload bytes in big-endian wire format,
subjected to impairment evaluation (passes through with `release_us = now_us`
when impairments are disabled by default), and the serialized bytes are sent
over the TCP socket as a length-prefixed frame via `send_to_all_clients()`.
The delay buffer is also flushed via `flush_delayed_to_clients()` immediately
afterward. `AckTracker` and `RetryManager` are never called. The call returns
`Result::OK` to the application.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point:** `DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us)`
  — `DeliveryEngine.cpp` line 77.
- **Caller:** Application code in `src/app` (e.g., `Client.cpp`) or a test
  harness. No thread is created inside this call; it executes synchronously on
  the caller's thread.
- **Pre-condition:** `DeliveryEngine::init()` has already been called (sets
  `m_initialized = true`, stores a valid `m_transport` pointer, calls
  `m_id_gen.init()`). `TcpBackend::init()` has already been called (TCP
  connection established, `m_open = true`, `m_client_fds[0]` holds valid fd,
  `m_client_count = 1`).

### Example: main(), ISR, callback, RPC handler, etc.

Application calls `DeliveryEngine::send()` from its main loop (e.g., an
application entry point calls `engine.send()` after building the envelope). No
ISR or RPC mechanism is present in the codebase.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Application** constructs a `MessageEnvelope` on the stack with
   `message_type = DATA`, `reliability_class = BEST_EFFORT`, populates payload
   fields, and calls `DeliveryEngine::send(env, now_us)`.

2. **`DeliveryEngine::send()`** (`DeliveryEngine.cpp:77`) fires two
   precondition assertions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and
   `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`. Checks `!m_initialized` guard
   at line 82 (passes). Stamps `env.source_id = m_local_id` (line 88),
   `env.message_id = m_id_gen.next()` (line 89),
   `env.timestamp_us = now_us` (line 90).

3. **`DeliveryEngine::send()`** calls `send_via_transport(env)` at line 93.

4. **`DeliveryEngine::send_via_transport()`** (`DeliveryEngine.cpp:56`) fires
   assertions: `m_initialized`, `m_transport != nullptr`,
   `envelope_valid(env)`. Calls `m_transport->send_message(env)` at line 63.
   Because `m_transport` is a `TcpBackend*`, this dispatches via virtual
   dispatch to `TcpBackend::send_message()`.

5. **`TcpBackend::send_message()`** (`TcpBackend.cpp:347`) fires assertions:
   `m_open`, `envelope_valid(envelope)`. Calls `Serializer::serialize(envelope,
   m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` at line 354.

6. **`Serializer::serialize()`** (`Serializer.cpp:117`) validates the envelope
   via `envelope_valid(env)`, checks `buf_len >= WIRE_HEADER_SIZE +
   payload_length`. Writes the 44-byte header in big-endian order using
   sequential `write_u8` / `write_u32` / `write_u64` static helper calls.
   Asserts `offset == WIRE_HEADER_SIZE` (44) after the header write. Calls
   `memcpy` for payload bytes if `payload_length > 0`. Sets
   `out_len = 44 + payload_length`. Returns `Result::OK`.

7. **`TcpBackend::send_message()`** continues at line 362. Calls
   `timestamp_now_us()` to get the current monotonic time (`now_us` local
   variable at line 362).

8. **`TcpBackend::send_message()`** calls
   `m_impairment.process_outbound(envelope, now_us)` at line 363.

9. **`ImpairmentEngine::process_outbound()`** — with impairments disabled
   (default from `impairment_config_default()`): checks
   `m_delay_count >= IMPAIR_DELAY_BUF_SIZE`; if not full, calls
   `queue_to_delay_buf(in_env, now_us)`. Inside `queue_to_delay_buf()`: scans
   `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]` for the first inactive slot,
   copies the envelope into that slot with `release_us = now_us` (deliver
   immediately), sets `active = true`, increments `m_delay_count`. Returns
   `Result::OK`.

10. **`TcpBackend::send_message()`** checks the return at line 364. Since it is
    `Result::OK` (not `ERR_IO`), execution continues. Checks
    `m_client_count == 0` at line 369 — it is 1 (connected), so execution
    passes through.

11. **`TcpBackend::send_message()`** calls `send_to_all_clients(m_wire_buf,
    wire_len)` at line 375.

12. **`TcpBackend::send_to_all_clients()`** (`TcpBackend.cpp:258`) fires
    assertions: `buf != nullptr`, `len > 0`. Enters the fixed-bound loop
    `i = 0..MAX_TCP_CONNECTIONS-1` (0..7). For `i = 0`,
    `m_client_fds[0] >= 0` is true. Calls `tcp_send_frame(m_client_fds[0], buf,
    len, m_cfg.channels[0].send_timeout_ms)` at line 268.

13. **`tcp_send_frame()`** (`SocketUtils.cpp`) fires assertions: `fd >= 0`,
    `buf != nullptr`. Builds a 4-byte big-endian length header from `len` using
    manual bit shifts into a stack `uint8_t header[4]`. Calls
    `socket_send_all(fd, header, 4U, timeout_ms)` for the length prefix.

14. **`socket_send_all()`** (`SocketUtils.cpp`) fires assertions: `fd >= 0`,
    `buf != nullptr`, `len > 0`. Enters a `while (sent < len)` loop (bound is
    `len = 4`). Calls `poll(&pfd, 1, timeout_ms)` on the socket for `POLLOUT`.
    On success (`poll_result > 0`), calls POSIX `send(fd, &buf[sent], remaining,
    0)`. Advances `sent`. Loop terminates when `sent == 4`. Returns `true`.

15. **`tcp_send_frame()`** checks the header send result. Passes. Calls
    `socket_send_all(fd, buf, len, timeout_ms)` for the serialized envelope
    bytes.

16. **`socket_send_all()`** repeats the poll-then-send loop for the serialized
    envelope bytes (`len = 44 + payload_length`). Each iteration calls `poll()`
    then `send()`, advancing `sent` until `sent == len`. Returns `true`.

17. **`tcp_send_frame()`** returns `true` to `send_to_all_clients()`.

18. **`send_to_all_clients()`** completes the client loop (slots 1..7 all have
    `m_client_fds[i] < 0`, so they are skipped via `continue`). Returns to
    `TcpBackend::send_message()`.

19. **`TcpBackend::send_message()`** calls `flush_delayed_to_clients(now_us)` at
    line 376.

20. **`TcpBackend::flush_delayed_to_clients()`** (`TcpBackend.cpp:280`) fires
    assertions: `now_us > 0`, `m_open`. Declares
    `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` on the stack. Calls
    `m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`.

21. **`ImpairmentEngine::collect_deliverable()`** iterates `m_delay_buf[]`
    (fixed bound `IMPAIR_DELAY_BUF_SIZE = 32`). The slot buffered in step 9 has
    `release_us == now_us`, so `release_us <= now_us` is true. Calls
    `envelope_copy()` to copy the envelope to `delayed[0]`, marks the slot
    inactive, decrements `m_delay_count`, increments `out_count`. Returns
    `out_count = 1`.

22. **`flush_delayed_to_clients()`** enters the fixed-bound loop
    `i = 0..count-1` (one iteration). Calls
    `Serializer::serialize(delayed[0], m_wire_buf, SOCKET_RECV_BUF_BYTES,
    delayed_len)`. On success, calls `send_to_all_clients(m_wire_buf,
    delayed_len)`. This sends the delayed envelope's serialized bytes to all
    connected clients via `tcp_send_frame()` again.

23. **`TcpBackend::send_message()`** asserts `wire_len > 0` at line 378.
    Returns `Result::OK` to `DeliveryEngine::send_via_transport()`.

24. **`DeliveryEngine::send_via_transport()`** checks `res != Result::OK` at
    line 65 — passes. Returns `Result::OK` to `DeliveryEngine::send()`.

25. **`DeliveryEngine::send()`** checks `res != Result::OK` at line 94 —
    passes. Evaluates the reliability branch at lines 102–103: condition
    `(RELIABLE_ACK || RELIABLE_RETRY)` is **false** for `BEST_EFFORT`.
    **`AckTracker::track()` is never called. `RetryManager::schedule()` is
    never called.**

26. **`DeliveryEngine::send()`** fires postcondition assertions:
    `env.source_id == m_local_id`, `env.message_id != 0ULL`. Calls
    `Logger::log(INFO, "DeliveryEngine", "Sent message_id=%llu, reliability=%u",
    ...)`. Returns `Result::OK` to the application.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::send()                         [DeliveryEngine.cpp:77]
|
+-- m_id_gen.next()                            [MessageId.hpp - inline]
|
+-- DeliveryEngine::send_via_transport()       [DeliveryEngine.cpp:56]
    |
    +-- TcpBackend::send_message()             [TcpBackend.cpp:347]
        |
        +-- Serializer::serialize()            [Serializer.cpp:117]
        |   +-- envelope_valid()               [MessageEnvelope.hpp:63 - inline]
        |   +-- Serializer::write_u8()         [Serializer.cpp:24]  (x4)
        |   +-- Serializer::write_u64()        [Serializer.cpp:50]  (x3)
        |   +-- Serializer::write_u32()        [Serializer.cpp:35]  (x3)
        |   \-- memcpy()                       [C stdlib]
        |
        +-- timestamp_now_us()                 [Timestamp.hpp:31 - inline]
        |
        +-- ImpairmentEngine::process_outbound()  [impairments disabled path]
        |   +-- envelope_valid()               [MessageEnvelope.hpp:63]
        |   \-- ImpairmentEngine::queue_to_delay_buf()
        |       \-- envelope_copy()            [MessageEnvelope.hpp:56 - inline]
        |           \-- memcpy()
        |
        +-- TcpBackend::send_to_all_clients()  [TcpBackend.cpp:258]
        |   \-- tcp_send_frame()               [SocketUtils.cpp]
        |       +-- socket_send_all()          [SocketUtils.cpp]  (4-byte length header)
        |       |   +-- poll()                 [POSIX]
        |       |   \-- send()                 [POSIX]
        |       \-- socket_send_all()          [SocketUtils.cpp]  (serialized payload)
        |           +-- poll()                 [POSIX]
        |           \-- send()                 [POSIX]
        |
        \-- TcpBackend::flush_delayed_to_clients()  [TcpBackend.cpp:280]
            +-- ImpairmentEngine::collect_deliverable()
            |   \-- envelope_copy()            [MessageEnvelope.hpp:56]
            +-- Serializer::serialize()        [Serializer.cpp:117]  (re-serialize delayed)
            \-- TcpBackend::send_to_all_clients()  [TcpBackend.cpp:258]
                \-- tcp_send_frame()           [SocketUtils.cpp]
                    +-- socket_send_all()      (header)
                    \-- socket_send_all()      (payload)

[AckTracker::track()      -- NOT CALLED (BEST_EFFORT branch skipped)]
[RetryManager::schedule() -- NOT CALLED (BEST_EFFORT branch skipped)]
```

---

## 5. Key Components Involved

### DeliveryEngine
- **Responsibility:** Top-level coordinator. Stamps `source_id`, `message_id`,
  `timestamp_us` onto the envelope; dispatches to transport; conditionally
  engages ACK tracking and retry scheduling based on `reliability_class`.
- **Why in this flow:** It is the sole external-facing send API. It owns the
  decision of whether to engage `AckTracker` and `RetryManager`; for
  `BEST_EFFORT` it skips both entirely. The conditional at
  `DeliveryEngine.cpp:102` evaluates `false` for `BEST_EFFORT`.

### MessageIdGen (m_id_gen)
- **Responsibility:** Generates monotonically increasing, unique `uint64_t`
  message IDs from a seeded counter, implemented inline in `MessageId.hpp`. On
  wraparound through 0, skips to 1.
- **Why in this flow:** Assigns `env.message_id` so the message has a unique
  identity on the wire, even though it is never tracked.

### TcpBackend
- **Responsibility:** Implements `TransportInterface`. Owns the socket file
  descriptors, the `ImpairmentEngine` instance, the `RingBuffer` receive queue,
  and the wire buffer. Serializes the envelope, runs it through the impairment
  engine, then calls private helpers `send_to_all_clients()` and
  `flush_delayed_to_clients()` to dispatch bytes.
- **Why in this flow:** It is the concrete transport bound to the
  `DeliveryEngine` at init time, bridging the message layer and the OS socket
  layer.

### Serializer (static class)
- **Responsibility:** Converts `MessageEnvelope` to/from a deterministic,
  endian-safe 44-byte header plus payload byte stream (big-endian throughout,
  manual bit shifts, no `htons`/`ntohl`). Wire header layout: 1 byte
  `message_type`, 1 byte `reliability_class`, 1 byte `priority`, 1 byte padding,
  8 bytes `message_id`, 8 bytes `timestamp_us`, 4 bytes `source_id`, 4 bytes
  `destination_id`, 8 bytes `expiry_time_us`, 4 bytes `payload_length`, 4 bytes
  padding — total 44 bytes (`WIRE_HEADER_SIZE`).
- **Why in this flow:** Called twice per outbound message — once inside
  `TcpBackend::send_message()` for the main message wire bytes, and once more
  inside `flush_delayed_to_clients()` for the delayed-envelope re-serialization.

### ImpairmentEngine
- **Responsibility:** Applies configurable network faults (loss, duplication,
  latency/jitter, partition, reordering). On outbound: `process_outbound()`
  decides fate and places the message in `m_delay_buf[]` with a `release_us`
  timestamp. `collect_deliverable()` harvests messages whose release time has
  passed.
- **Why in this flow:** Always invoked regardless of `reliability_class`. With
  impairments disabled (default, `ImpairmentConfig::enabled = false`), it acts
  as a zero-delay passthrough via `queue_to_delay_buf(env, now_us)`, placing
  the message with `release_us = now_us` for immediate collection.

### tcp_send_frame() (SocketUtils)
- **Responsibility:** Frames a serialized message for TCP by prepending a 4-byte
  big-endian length prefix and then sending all bytes via `socket_send_all()`.
- **Why in this flow:** Implements the framing layer required to separate
  discrete messages over TCP's byte stream (REQ-6.1.5). Called once per
  connected-client slot by `send_to_all_clients()`.

### socket_send_all() (SocketUtils)
- **Responsibility:** Sends exactly `len` bytes over a TCP socket, looping
  until all bytes are written. Uses `poll()` before each `send()` call to
  enforce a timeout.
- **Why in this flow:** Handles partial writes. TCP's `send()` may not accept
  all bytes in one call (REQ-6.1.6); this function retries until the full count
  is transferred.

### AckTracker
- **Responsibility:** Tracks messages awaiting ACK in a fixed-size slot table
  (`ACK_TRACKER_CAPACITY = 32`).
- **Why in this flow:** Exists in `DeliveryEngine` as a member but is **not
  invoked**. The conditional at `DeliveryEngine.cpp:102` evaluates `false` for
  `BEST_EFFORT`, so `AckTracker::track()` is never called.

### RetryManager
- **Responsibility:** Schedules and manages retry of `RELIABLE_RETRY` messages
  with exponential backoff.
- **Why in this flow:** Exists in `DeliveryEngine` as a member but is **not
  invoked**. The conditional at `DeliveryEngine.cpp:120` evaluates `false` for
  `BEST_EFFORT`, so `RetryManager::schedule()` is never called.

---

## 6. Branching Logic / Decision Points

**Branch 1: `m_initialized` guard in `DeliveryEngine::send()` (line 82)**
- Condition: `!m_initialized`
- Outcome if false (normal): execution continues.
- Outcome if true (error): returns `Result::ERR_INVALID` immediately.
- Where control goes next (normal): stamp fields, call `send_via_transport()`.

**Branch 2: `send_via_transport()` return check in `DeliveryEngine::send()`
(line 94)**
- Condition: `res != Result::OK`
- Outcome if false (normal): execution continues to reliability branch.
- Outcome if true (error): logs `WARNING_LO`, returns `res` to caller.
- Where control goes next (normal): reliability class conditional.

**Branch 3: Reliability class conditional — AckTracker (lines 102–103)**
- Condition: `env.reliability_class == RELIABLE_ACK || RELIABLE_RETRY`
- For `BEST_EFFORT`: **condition is false**. `AckTracker::track()` is skipped
  entirely.

**Branch 4: Reliability class conditional — RetryManager (line 120)**
- Condition: `env.reliability_class == RELIABLE_RETRY`
- For `BEST_EFFORT`: **condition is false**. `RetryManager::schedule()` is
  skipped entirely.

**Branch 5: Impairment `ERR_IO` check in `TcpBackend::send_message()` (line 364)**
- Condition: `res == Result::ERR_IO`
- Outcome if true: returns `Result::OK` (silent drop, loss impairment active).
- Outcome if false (default): execution continues to client count check.
- Note: `ERR_FULL` from `process_outbound()` (delay buffer full) is NOT
  intercepted here — it propagates back to the application as `ERR_FULL`.

**Branch 6: No clients connected check (line 369)**
- Condition: `m_client_count == 0`
- Outcome if true: logs `WARNING_LO`, returns `Result::OK` (message discarded).
- Outcome if false (normal): proceeds to `send_to_all_clients()`.

**Branch 7: Client fd validity in `send_to_all_clients()` (line 265)**
- Condition: `m_client_fds[i] < 0`
- Outcome if true: `continue` to next slot.
- Outcome if false: calls `tcp_send_frame()` for that slot.

**Branch 8: `tcp_send_frame()` send failure**
- Condition: `socket_send_all()` returns false for header or payload.
- Outcome if true: logs `WARNING_LO`; loop continues — other client slots are
  still attempted. Return is void; per-connection failures are not propagated
  upward.

**Branch 9: `poll()` timeout in `socket_send_all()`**
- Condition: `poll_result <= 0`
- Outcome: logs `WARNING_HI`, returns false. Propagates up through
  `tcp_send_frame()`.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. This entire send path executes synchronously on the caller's thread. No
threads are spawned inside `send()`, `TcpBackend::send_message()`, or any
called function.

### Where context switches occur
Only at OS blocking calls inside `socket_send_all()`:
- `poll(fd, 1, timeout_ms)` — blocks until the socket is writable or the
  timeout expires.
- `send(fd, ...)` — the TCP socket may be in non-blocking mode after
  `socket_connect_with_timeout()`. `send()` may return `EAGAIN` or partial
  bytes; `socket_send_all()` handles both via its `while (sent < len)` loop
  with `poll()` before each attempt.

### Synchronization primitives
None present in this send path. There is no mutex, semaphore, or `std::atomic`
protecting `m_wire_buf`, `m_client_fds[]`, `m_client_count`, or
`m_impairment` state. `RingBuffer` (the receive queue) uses
`std::atomic<uint32_t>` for its head/tail with acquire/release ordering, but
the receive queue is not touched during the send path.

- [ASSUMPTION] `TcpBackend::send_message()` and
  `TcpBackend::receive_message()` are called from the same thread. If called
  from separate threads, `m_wire_buf` (8192-byte shared buffer), `m_client_fds[]`,
  `m_client_count`, and `m_impairment.m_delay_buf[]` are all accessed without
  synchronization — a data race under the C++ memory model.

### Producer/consumer relationships
None within this send path. `m_recv_queue` (a `RingBuffer`) is written by
`recv_from_client()` and read by `receive_message()`, but it is not touched
during the send path.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory
All memory is statically allocated inside the objects themselves:
- `MessageEnvelope env` in the application: caller-owned, stack-allocated.
- `TcpBackend::m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes): member array
  of `TcpBackend`, allocated in place when `TcpBackend` is constructed.
- `TcpBackend::m_impairment` (an `ImpairmentEngine`): member of `TcpBackend`;
  its `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` (32 slots) is a member array.
- `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` (32 elements): declared on
  the stack inside `TcpBackend::flush_delayed_to_clients()`.
- `uint8_t header[4]` in `tcp_send_frame()`: stack array.
- `struct pollfd pfd` in `socket_send_all()`: stack-allocated per call.
- No `new`, `malloc`, or `delete` anywhere in this path (Power of 10 rule 3).

### Lifetime of key objects
- `MessageEnvelope& env` passed to `send()` must remain valid for the duration
  of the call (passed by reference and read inside `TcpBackend::send_message()`
  during serialization). After `send()` returns, it may go out of scope safely.
- `TcpBackend` must outlive all calls to `send_message()` — it is owned by the
  application layer that constructs and passes the `TransportInterface*` to
  `DeliveryEngine::init()`.

### Stack vs heap usage
- Every local in this entire call chain is stack-allocated or lives in object
  members.
- `delayed[IMPAIR_DELAY_BUF_SIZE]` inside `flush_delayed_to_clients()` is
  stack-allocated. `sizeof(MessageEnvelope)` is approximately
  `3×uint64_t + 3×uint32_t + 3×uint8_t + padding + uint8_t[4096] ~= 4136 bytes`.
  So `delayed[32]` is approximately 132 KB on the stack. This is a potential
  stack overflow risk on embedded or constrained targets.

### RAII usage
- `TcpBackend` destructor (`~TcpBackend()`) calls `TcpBackend::close()`, which
  closes all sockets using explicit qualified dispatch to avoid virtual dispatch
  in the destructor.
- No `std::unique_ptr`, `std::lock_guard`, or other RAII wrappers (consistent
  with no-STL rule).

### Potential leaks or unsafe patterns
- If `tcp_send_frame()` fails mid-frame (header sent but payload send fails),
  the receiving end will have consumed 4 bytes of length prefix but not the
  corresponding payload. The TCP stream is in an inconsistent framing state with
  no recovery mechanism in this path. [OBSERVATION]
- `m_wire_buf` is written by `Serializer::serialize()` for the main message,
  then read by `send_to_all_clients()`, then overwritten by `Serializer::serialize()`
  inside `flush_delayed_to_clients()`, then read again. Safe in single-threaded
  context. [OBSERVATION]

---

## 9. Error Handling Flow

### How errors propagate
- Errors inside `socket_send_all()` return `false` to `tcp_send_frame()`, which
  returns `false` to `send_to_all_clients()`.
- In `send_to_all_clients()`, a failed `tcp_send_frame()` is logged at
  `WARNING_LO` but the loop continues to other clients; the function has void
  return type, so per-connection failures are not propagated upward.
- `Serializer::serialize()` returning non-OK causes `send_message()` to return
  that result immediately; this propagates through `send_via_transport()` to
  `send()`, which logs it and returns it to the application.
- `ImpairmentEngine::process_outbound()` returning `ERR_IO` causes
  `TcpBackend::send_message()` to return `Result::OK` (silent drop at line 365).
  The application receives `OK` even though the message was dropped.
- `ImpairmentEngine::process_outbound()` returning `ERR_FULL` (delay buffer
  full) is NOT intercepted as `ERR_IO` — it propagates up through `send_message()`
  and back to the application as `ERR_FULL`.

### Retry logic
None in this path. `BEST_EFFORT` has no retry. The application receives
`Result::OK` whether or not the bytes reached the network (unless serialization
fails or the delay buffer is full).

### Fallback paths
- If `m_client_count == 0`, the function returns `Result::OK` without sending
  anything.
- If all `tcp_send_frame()` calls fail, `send_to_all_clients()` still returns
  (void), and `send_message()` still returns `Result::OK`.

---

## 10. External Interactions

### Network calls
All POSIX socket calls occur inside `socket_send_all()` (`SocketUtils.cpp`):
- `poll(pfd, 1, timeout_ms)` — waits for the TCP socket to become writable.
- `send(fd, buf, remaining, 0)` — writes bytes to the TCP stream.
These are the only points where the process enters kernel space for I/O in this
send path.

### File I/O
None in this path.

### Hardware interaction
None directly. The kernel's TCP stack handles NIC interaction transparently.

### IPC
None. The send path is entirely in-process.

---

## 11. State Changes / Side Effects

### What system state is modified

**`MessageEnvelope& env` (caller's object, passed by reference):**
- `env.source_id` set to `m_local_id` (`DeliveryEngine.cpp:88`).
- `env.message_id` set to `m_id_gen.next()` (`DeliveryEngine.cpp:89`).
- `env.timestamp_us` set to `now_us` (`DeliveryEngine.cpp:90`).

**`MessageIdGen m_id_gen` (member of `DeliveryEngine`):**
- Internal counter incremented by `next()`. This is a permanent, monotonic side
  effect — the next call to `send()` will receive a higher ID.

**`ImpairmentEngine::m_delay_buf[]` and `m_delay_count` (member of
`TcpBackend`):**
- One slot is activated in `process_outbound()` via `queue_to_delay_buf()`,
  then immediately deactivated in `collect_deliverable()` (called from
  `flush_delayed_to_clients()`). Net state after `send_message()` returns:
  same as before the call.

**`TcpBackend::m_wire_buf[8192]` (member of `TcpBackend`):**
- Overwritten first with the main-message serialization, then overwritten again
  by `flush_delayed_to_clients()` re-serialization. State persists after
  `send_message()` returns.

**TCP kernel send buffer:**
- Bytes are enqueued in the OS TCP send buffer. The kernel schedules
  transmission independently.

### Persistent changes (DB, disk, device state)
None. No disk writes, no database, no device registers.

---

## 12. Sequence Diagram using mermaid

```text
Participants:
  App  DeliveryEngine  TcpBackend  ImpairEngine  Serializer  SocketUtils  OS/Kernel

Flow:

App --send(env, now_us)--> DeliveryEngine
  stamp source_id, message_id, timestamp_us
  send_via_transport(env)
    DeliveryEngine --> TcpBackend::send_message(env)
      TcpBackend --> Serializer::serialize(env) --> wire bytes in m_wire_buf
      TcpBackend: now_us2 = timestamp_now_us()
      TcpBackend --> ImpairmentEngine::process_outbound(env, now_us2)
        [impairments disabled: queue_to_delay_buf(env, now_us2), release_us=now_us2]
        returns OK
      TcpBackend --> send_to_all_clients(m_wire_buf, wire_len)
        TcpBackend --> tcp_send_frame(fd, wire_buf, len, tmo)
          SocketUtils --> socket_send_all(fd, hdr, 4, tmo)
            poll(POLLOUT) -> OS/Kernel
            send(hdr, 4) -> OS/Kernel
          SocketUtils --> socket_send_all(fd, buf, wlen, tmo)
            poll(POLLOUT) -> OS/Kernel
            send(bytes, wlen) -> OS/Kernel
          returns true
      TcpBackend --> flush_delayed_to_clients(now_us2)
        TcpBackend --> ImpairmentEngine::collect_deliverable(now_us2)
          [slot active, release_us<=now_us2: copy out, free slot]
          returns 1
        TcpBackend --> Serializer::serialize(delayed[0]) --> m_wire_buf
        TcpBackend --> send_to_all_clients(m_wire_buf, delayed_len)
          [same poll/send sequence as above]
      asserts wire_len > 0
      returns Result::OK
    returns Result::OK
  [BEST_EFFORT: reliability check FALSE]
  [AckTracker::track()      NOT CALLED]
  [RetryManager::schedule() NOT CALLED]
  asserts env.source_id == m_local_id, env.message_id != 0
  Logger::log(INFO, "Sent message_id=N, reliability=0")
App <-- Result::OK
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

1. Application constructs `TcpBackend` — constructor (`TcpBackend.cpp:28`)
   zero-initializes `m_client_fds[]` to -1, sets `m_open = false`,
   `m_is_server = false`, using a bounded init loop.
2. Application calls `TcpBackend::init(config)` — initializes `m_recv_queue`,
   calls `impairment_config_default(imp_cfg)` (sets `enabled = false`), reads
   `config.channels[0].impairments_enabled` to override, calls
   `m_impairment.init(imp_cfg)`. For client mode calls `connect_to_server()`:
   creates TCP socket via `socket_create_tcp()`, calls `socket_set_reuseaddr()`,
   calls `socket_connect_with_timeout()`. On success, stores fd in
   `m_client_fds[0]`, sets `m_open = true`, `m_client_count = 1`.
3. Application constructs `DeliveryEngine` and calls `DeliveryEngine::init
   (transport, cfg, local_id)` — stores `m_transport`, copies `m_cfg`, calls
   `m_ack_tracker.init()`, `m_retry_manager.init()`, `m_dedup.init()`,
   `m_id_gen.init(seed = local_id)`, sets `m_initialized = true`.

### What happens during steady-state execution

Each call to `DeliveryEngine::send()` follows the path described in section 3.
No new memory is allocated. The `m_id_gen` counter increments with each send.
The `ImpairmentEngine` delay buffer is transiently written and immediately
cleared within each `send_message()` call (zero-delay path). The TCP kernel
send buffer accumulates bytes until the kernel transmits them.

---

## 14. Known Risks / Observations

### Double-send of the same message data

`TcpBackend::send_message()` serializes the envelope into `m_wire_buf` and
calls `send_to_all_clients()` with those bytes (the main send at line 375).
Then calls `flush_delayed_to_clients()` (line 376), which drains the delay
buffer. The slot placed there in `process_outbound()` with `release_us = now_us`
is immediately eligible. `flush_delayed_to_clients()` re-serializes it and
calls `send_to_all_clients()` again. **The same logical message is sent twice to
each connected client in the zero-impairment path.** This appears to be a design
defect: when impairments are disabled, the delay buffer acts as a passthrough
queue, and `flush_delayed_to_clients()` in the same call cycle immediately
delivers the queued item. [OBSERVATION - potential bug]

### Unsynchronized shared state

`TcpBackend::m_wire_buf`, `m_client_fds[]`, `m_client_count`, and
`ImpairmentEngine::m_delay_buf[]` are all modified by `send_message()` and
read/written by `receive_message()` and `recv_from_client()`. If send and
receive are called from different threads (expected usage pattern), all of
these accesses are data races under the C++ memory model. No mutex,
`std::atomic`, or other synchronization is present (except on the
`RingBuffer`'s own head/tail). [RISK]

### Stack pressure from `delayed[]` in flush_delayed_to_clients()

`MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` (32 elements) is
stack-allocated inside `flush_delayed_to_clients()`. `sizeof(MessageEnvelope)`
is approximately 4136 bytes. 32 elements is approximately 132 KB on the stack.
On platforms with small stack sizes this is a stack overflow risk. [RISK]

### tcp_send_frame() failure is silently absorbed

In `send_to_all_clients()`, if `tcp_send_frame()` returns false, a `WARNING_LO`
is logged but the loop continues and the function returns void. The
`DeliveryEngine` receives `OK` (from `send_message()`) and the application has
no way to distinguish a failed send from a successful one. For `BEST_EFFORT`
this may be intentional (fire and forget), but the same code path applies for
reliable modes. [OBSERVATION]

### Logger call at send completion

`Logger::log(INFO, ...)` is called at `DeliveryEngine.cpp:138-141` with
`message_id` and `reliability_class` on every send. If `Logger` does
synchronous file or console I/O, it adds latency to every send. [OBSERVATION]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application passes `reliability_class = BEST_EFFORT`
  correctly set in the envelope before calling `send()`. The engine does not
  validate or override this field; it trusts the caller.

- [ASSUMPTION] `TcpBackend` is used in client mode (not server mode) for this
  use case. In server mode, `send_message()` calls the same helpers;
  `send_to_all_clients()` iterates all `m_client_fds[]` slots and would send
  to every connected client.

- [ASSUMPTION] `impairment_config_default()` initializes
  `ImpairmentConfig::enabled = false`. When disabled, `process_outbound()`
  passes messages through the delay buffer with `release_us = now_us`, causing
  immediate double-send via `flush_delayed_to_clients()`.

- `MessageIdGen::next()` returns a monotonically increasing non-zero `uint64_t`
  seeded with `local_id`. On wraparound through 0, it skips to 1. Confirmed at
  `MessageId.hpp`.

- `timestamp_now_us()` returns the current `CLOCK_MONOTONIC` time in
  microseconds as a `uint64_t`. Confirmed at `Timestamp.hpp:31-45`.

- [ASSUMPTION] `RingBuffer::push()` and `RingBuffer::pop()` are SPSC lock-free
  operations using `std::atomic<uint32_t>` with acquire/release ordering.
  Capacity is `MSG_RING_CAPACITY = 64`.

- [ASSUMPTION] `DeliveryEngine::send()` is called from a single thread. No
  synchronization exists; concurrent calls would create data races on `m_id_gen`
  (shared counter) and `TcpBackend::m_wire_buf`.

- The TCP socket may remain in non-blocking mode after
  `socket_connect_with_timeout()` completes. `send()` inside `socket_send_all()`
  may return partial bytes or `EAGAIN`; the `while` loop handles both via
  `poll()` before each attempt.

- The exact struct size and padding of `MessageEnvelope` depends on the
  compiler. Estimated `sizeof(MessageEnvelope) ~= 4136` bytes
  (3×`uint64_t` + 3×`uint32_t` + 3×`uint8_t` + padding + `uint8_t[4096]`).

- [ASSUMPTION] `Logger::log()` thread-safety is not established by the source
  code reviewed. If not thread-safe, concurrent sends from multiple threads will
  produce interleaved log output.
