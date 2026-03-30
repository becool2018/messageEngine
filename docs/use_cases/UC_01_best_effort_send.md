## 1. Use Case Overview

### Clear description of what triggers this flow

A client application populates a `MessageEnvelope` with `message_type = DATA`, `reliability_class = BEST_EFFORT`, fills in `destination_id`, `priority`, `expiry_time_us`, `payload`, and `payload_length`, then calls `DeliveryEngine::send(env, now_us)`. The engine is already initialized with a `TcpBackend` instance as its `TransportInterface`. The TCP connection to the server peer was established during `TcpBackend::init()`.

### Expected outcome

The envelope is stamped with `source_id` and a newly generated `message_id`, serialized to a 44-byte header plus payload bytes in big-endian wire format, subjected to impairment evaluation (it passes through to the delay buffer immediately since impairments are disabled by default, or is conditionally dropped/delayed if enabled), and then the ready bytes are sent over the TCP socket as a length-prefixed frame. `AckTracker` and `RetryManager` are never called. The call returns `Result::OK` to the application.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point:** `DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us)` — `DeliveryEngine.cpp` line 75.
- **Caller:** Application code in `src/app` (or a test harness). No thread is created inside this call; it executes synchronously on the caller's thread.
- **Pre-condition:** `DeliveryEngine::init()` has already been called (sets `m_initialized = true`, stores a valid `m_transport` pointer, calls `m_id_gen.init()`). `TcpBackend::init()` has already been called (TCP connection established, `m_open = true`, `m_client_fds[0]` holds valid fd).

### Example: main(), ISR, callback, RPC handler, etc.

Application calls `DeliveryEngine::send()` from its main loop or a dedicated sender thread. No ISR or RPC mechanism is present in the codebase.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Application** constructs a `MessageEnvelope` on the stack with `message_type = DATA`, `reliability_class = BEST_EFFORT`, populates payload fields, and calls `DeliveryEngine::send(env, now_us)`.

2. **`DeliveryEngine::send()`** (`DeliveryEngine.cpp:75`) fires two precondition assertions: `m_initialized` is true, `now_us > 0`. Checks `m_initialized` guard at line 80 (passes). Stamps `env.source_id = m_local_id` (line 86), `env.message_id = m_id_gen.next()` (line 87), `env.timestamp_us = now_us` (line 88).

3. **`DeliveryEngine::send()`** calls `send_via_transport(env)` at line 91.

4. **`DeliveryEngine::send_via_transport()`** (`DeliveryEngine.cpp:54`) fires assertions: `m_initialized`, `m_transport != nullptr`, `envelope_valid(env)`. Calls `m_transport->send_message(env)` at line 61. Because `m_transport` is a `TcpBackend*`, this dispatches to `TcpBackend::send_message()`.

5. **`TcpBackend::send_message()`** (`TcpBackend.cpp:249`) fires assertions: `m_open`, `envelope_valid(envelope)`. Calls `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` at line 256.

6. **`Serializer::serialize()`** (`Serializer.cpp:115`) validates the envelope (`envelope_valid(env)`), checks buffer size (`buf_len >= WIRE_HEADER_SIZE + payload_length`). Writes 44-byte header in big-endian order using sequential `write_u8` / `write_u32` / `write_u64` static helper calls. Asserts `offset == WIRE_HEADER_SIZE` (44) after header. Calls `memcpy` for payload bytes if `payload_length > 0`. Writes `out_len = 44 + payload_length`. Returns `Result::OK`.

7. **`TcpBackend::send_message()`** continues at line 264. Calls `timestamp_now_us()` to get the current wall-clock time in microseconds (stored in `now_us` local variable).

8. **`TcpBackend::send_message()`** calls `m_impairment.process_outbound(envelope, now_us)` at line 265.

9. **`ImpairmentEngine::process_outbound()`** (`ImpairmentEngine.cpp:62`) fires assertions: `m_initialized`, `envelope_valid(in_env)`. **Branch: impairments disabled** (`m_cfg.enabled == false`, the default from `impairment_config_default()`): checks `m_delay_count < IMPAIR_DELAY_BUF_SIZE`, scans the `m_delay_buf[]` array for the first inactive slot (fixed-bound loop up to `IMPAIR_DELAY_BUF_SIZE = 32`), copies the envelope into that slot with `release_us = now_us` (deliver immediately), sets `active = true`, increments `m_delay_count`. Returns `Result::OK`. [If impairments are enabled the function may return `Result::ERR_IO` for a dropped message — see section 6.]

10. **`TcpBackend::send_message()`** checks the return at line 266. Since it is `Result::OK` (not `ERR_IO`), execution continues. Calls `m_impairment.collect_deliverable(now_us, delayed_envelopes, IMPAIR_DELAY_BUF_SIZE)` at line 273 to drain any messages in the delay buffer whose `release_us <= now_us`.

11. **`ImpairmentEngine::collect_deliverable()`** (`ImpairmentEngine.cpp:174`) fires assertions: `m_initialized`, `out_buf != NULL`, `buf_cap > 0`. Iterates the `m_delay_buf[]` array (fixed bound `IMPAIR_DELAY_BUF_SIZE`). The slot just buffered in step 9 has `release_us == now_us`, so `release_us <= now_us` is true. Calls `envelope_copy()` to copy the envelope to `delayed_envelopes[0]`, marks the slot inactive, decrements `m_delay_count`, increments `out_count`. Returns `out_count = 1`.

12. **`TcpBackend::send_message()`** continues at line 278. Checks `m_client_count == 0` — it is 1 (connected in client mode), so passes. Enters the primary send loop (lines 285–295, fixed bound `MAX_TCP_CONNECTIONS = 8`). For `i = 0`, `m_client_fds[0] >= 0` is true. Calls `tcp_send_frame(m_client_fds[0], m_wire_buf, wire_len, m_cfg.channels[0].send_timeout_ms)` at line 290.

13. **`tcp_send_frame()`** (`SocketUtils.cpp:391`) fires assertions: `fd >= 0`, `buf != NULL`. Builds a 4-byte big-endian length header from `wire_len` using manual bit shifts into a stack `uint8_t header[4]`. Calls `socket_send_all(fd, header, 4U, timeout_ms)` at line 406.

14. **`socket_send_all()`** (`SocketUtils.cpp:290`) fires assertions: `fd >= 0`, `buf != NULL`, `len > 0`. Enters a `while (sent < len)` loop (bound is `len = 4`). Calls `poll(&pfd, 1, timeout_ms)` on the socket for `POLLOUT`. On success (`poll_result > 0`), calls POSIX `send(fd, &buf[sent], remaining, 0)`. Advances `sent`. Loop terminates when `sent == 4`. Asserts `sent == len`. Returns `true`.

15. **`tcp_send_frame()`** checks return from `socket_send_all()` for the header (line 406–409). Passes. Checks `wire_len > 0`. Calls `socket_send_all(fd, buf, wire_len, timeout_ms)` for the payload bytes at line 414.

16. **`socket_send_all()`** repeats the poll-then-send loop for the serialized envelope bytes (`wire_len = 44 + payload_length`). Each iteration calls `poll()` then `send()`, advancing `sent` until `sent == wire_len`. Returns `true`.

17. **`tcp_send_frame()`** returns `true` to `TcpBackend::send_message()`.

18. **`TcpBackend::send_message()`** enters the delayed-messages loop (lines 298–315). `delayed_count = 1`, so `i = 0` iteration re-serializes `delayed_envelopes[0]` via `Serializer::serialize()` into `m_wire_buf`, then calls `tcp_send_frame()` again for each connected client. This is the actual wire transmission of the original message (the delay buffer with `release_us = now_us` means immediate passthrough, not a second distinct send — see section 14 observations). Asserts `wire_len > 0`. Returns `Result::OK`.

19. **`DeliveryEngine::send_via_transport()`** checks `res != Result::OK` at line 63 — passes. Returns `Result::OK` to `DeliveryEngine::send()`.

20. **`DeliveryEngine::send()`** checks `res != Result::OK` at line 92 — passes. Evaluates the reliability branch at lines 100–101: `env.reliability_class == RELIABLE_ACK` is false, `env.reliability_class == RELIABLE_RETRY` is false. **`AckTracker::track()` is never called. `RetryManager::schedule()` is never called.**

21. **`DeliveryEngine::send()`** fires postcondition assertions: `env.source_id == m_local_id`, `env.message_id != 0`. Calls `Logger::log(INFO, ...)`. Returns `Result::OK` to the application.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::send()                         [DeliveryEngine.cpp:75]
|
+-- m_id_gen.next()                            [MessageId.hpp - inline]
|
+-- DeliveryEngine::send_via_transport()       [DeliveryEngine.cpp:54]
    |
    +-- TcpBackend::send_message()             [TcpBackend.cpp:249]
        |
        +-- Serializer::serialize()            [Serializer.cpp:115]
        |   |
        |   +-- envelope_valid()               [MessageEnvelope.hpp - inline]
        |   +-- Serializer::write_u8()         [Serializer.cpp:22]  (x4)
        |   +-- Serializer::write_u64()        [Serializer.cpp:48]  (x4)
        |   +-- Serializer::write_u32()        [Serializer.cpp:33]  (x4)
        |   \-- memcpy()                       [C stdlib]
        |
        +-- timestamp_now_us()                 [Timestamp.hpp/cpp]
        |
        +-- ImpairmentEngine::process_outbound()  [ImpairmentEngine.cpp:62]
        |   |
        |   +-- envelope_valid()               [MessageEnvelope.hpp - inline]
        |   +-- (impairments disabled branch)
        |   \-- envelope_copy()                [MessageEnvelope.hpp - inline]
        |       \-- memcpy()                   [C stdlib]
        |
        +-- ImpairmentEngine::collect_deliverable()  [ImpairmentEngine.cpp:174]
        |   \-- envelope_copy()                [MessageEnvelope.hpp - inline]
        |       \-- memcpy()                   [C stdlib]
        |
        +-- tcp_send_frame()                   [SocketUtils.cpp:391]  (main message)
        |   |
        |   +-- socket_send_all()              [SocketUtils.cpp:290]  (4-byte length header)
        |   |   +-- poll()                     [POSIX]
        |   |   \-- send()                     [POSIX]
        |   |
        |   \-- socket_send_all()              [SocketUtils.cpp:290]  (serialized payload)
        |       +-- poll()                     [POSIX]
        |       \-- send()                     [POSIX]
        |
        \-- (delayed_count=1 loop)
            +-- Serializer::serialize()        [Serializer.cpp:115]   (re-serialize)
            \-- tcp_send_frame()               [SocketUtils.cpp:391]  (delayed send)
                +-- socket_send_all()          [SocketUtils.cpp:290]  (header)
                \-- socket_send_all()          [SocketUtils.cpp:290]  (payload)

[AckTracker::track()    -- NOT CALLED (BEST_EFFORT branch skipped)]
[RetryManager::schedule() -- NOT CALLED (BEST_EFFORT branch skipped)]
```

---

## 5. Key Components Involved

### DeliveryEngine
- **Responsibility:** Top-level coordinator. Stamps `source_id`, `message_id`, `timestamp_us` onto the envelope; dispatches to transport; conditionally engages ACK tracking and retry scheduling based on `reliability_class`.
- **Why in this flow:** It is the sole external-facing send API. It owns the decision of whether to engage `AckTracker` and `RetryManager`; for `BEST_EFFORT` it skips both entirely.

### MessageIdGen (m_id_gen)
- **Responsibility:** Generates monotonically increasing, unique `uint64_t` message IDs from a seeded counter.
- **Why in this flow:** Assigns `env.message_id` so the message has a unique identity on the wire, even though it is never tracked.

### TcpBackend
- **Responsibility:** Implements `TransportInterface`. Owns the socket file descriptors, the `ImpairmentEngine` instance, and the receive queue. Serializes the envelope, runs it through impairments, and calls `tcp_send_frame()` for each connected client.
- **Why in this flow:** It is the concrete transport bound to the `DeliveryEngine` at init time. It is the single module that bridges the message layer and the OS socket layer.

### Serializer (static class)
- **Responsibility:** Converts `MessageEnvelope` to/from a deterministic, endian-safe 44-byte-header + payload byte stream (big-endian throughout, manual shifts, no `htons`/`ntohl`).
- **Why in this flow:** Called twice per outbound message — once inside `TcpBackend::send_message()` for the main message, and once more in the delayed-envelope loop. Produces the byte sequence handed to `tcp_send_frame()`.

### ImpairmentEngine
- **Responsibility:** Applies configurable network faults (loss, duplication, latency/jitter, partition, reordering). On outbound: `process_outbound()` decides fate and places the message in `m_delay_buf[]` with a `release_us` timestamp. `collect_deliverable()` harvests messages whose release time has passed.
- **Why in this flow:** Always invoked regardless of `reliability_class`. With impairments disabled (default), it acts as a zero-delay passthrough, placing the message in the delay buffer with `release_us = now_us` for immediate collection.

### tcp_send_frame() (SocketUtils)
- **Responsibility:** Frames a serialized message for TCP by prepending a 4-byte big-endian length prefix and then sending all bytes via `socket_send_all()`.
- **Why in this flow:** Implements the framing layer required to separate discrete messages over TCP's byte stream. Called once per connected-client slot.

### socket_send_all() (SocketUtils)
- **Responsibility:** Sends exactly `len` bytes over a TCP socket, looping until all bytes are written. Uses `poll()` before each `send()` call to enforce a timeout.
- **Why in this flow:** Handles partial writes. TCP's `send()` may not accept all bytes in one call; this function retries until the full count is transferred.

### AckTracker
- **Responsibility:** Tracks messages awaiting ACK in a fixed-size slot table; supports timeout sweep.
- **Why in this flow:** Exists in `DeliveryEngine` as a member but is **not invoked**. The conditional at `DeliveryEngine.cpp:100` evaluates `false` for `BEST_EFFORT`, so `AckTracker::track()` is never called.

### RetryManager
- **Responsibility:** Schedules and manages retry of `RELIABLE_RETRY` messages with exponential backoff.
- **Why in this flow:** Exists in `DeliveryEngine` as a member but is **not invoked**. The conditional at `DeliveryEngine.cpp:118` evaluates `false` for `BEST_EFFORT`, so `RetryManager::schedule()` is never called.

---

## 6. Branching Logic / Decision Points

**Branch 1: `m_initialized` guard in `DeliveryEngine::send()` (line 80)**
- Condition: `!m_initialized`
- Outcome if false (normal): execution continues.
- Outcome if true (error): returns `Result::ERR_INVALID` immediately.
- Where control goes next (normal): stamp fields, call `send_via_transport()`.

**Branch 2: `send_via_transport()` return check in `DeliveryEngine::send()` (line 92)**
- Condition: `res != Result::OK`
- Outcome if false (normal): execution continues to reliability branch.
- Outcome if true (error): logs `WARNING_LO`, returns `res` to caller.
- Where control goes next (normal): reliability class conditional.

**Branch 3: Reliability class conditional — AckTracker (lines 100–101)**
- Condition: `env.reliability_class == RELIABLE_ACK || env.reliability_class == RELIABLE_RETRY`
- For `BEST_EFFORT`: **condition is false**. `AckTracker::track()` is skipped entirely.
- For other classes: `AckTracker::track()` called.

**Branch 4: Reliability class conditional — RetryManager (line 118)**
- Condition: `env.reliability_class == RELIABLE_RETRY`
- For `BEST_EFFORT`: **condition is false**. `RetryManager::schedule()` is skipped entirely.
- For `RELIABLE_RETRY`: `RetryManager::schedule()` called.

**Branch 5: Impairments enabled/disabled in `ImpairmentEngine::process_outbound()` (line 70)**
- Condition: `!m_cfg.enabled`
- Outcome if true (default): enters immediate-passthrough branch, places in delay buffer with `release_us = now_us`, returns `Result::OK`. No loss/partition/jitter logic runs.
- Outcome if false: falls through to partition check, loss probability, jitter calculation.

**Branch 6: Partition active check in `process_outbound()` (line 93)**
- Condition: `is_partition_active(now_us)` returns true
- Outcome if true: returns `Result::ERR_IO` (message silently dropped).
- Outcome if false: continues to loss impairment.

**Branch 7: Loss probability check in `process_outbound()` (lines 100–108)**
- Condition: `m_cfg.loss_probability > 0.0` AND `loss_rand < m_cfg.loss_probability`
- Outcome if dropped: returns `Result::ERR_IO`.
- Back in `TcpBackend::send_message()` at line 266: if `res == ERR_IO`, the function returns `Result::OK` (silent drop, no error propagated upward).

**Branch 8: No clients connected check in `TcpBackend::send_message()` (line 278)**
- Condition: `m_client_count == 0`
- Outcome if true: logs `WARNING_LO`, returns `Result::OK` (message discarded).
- Outcome if false (normal): proceeds to send loop.

**Branch 9: Client fd validity in send loop (line 286)**
- Condition: `m_client_fds[i] < 0`
- Outcome if true: `continue` to next slot.
- Outcome if false: calls `tcp_send_frame()` for that slot.

**Branch 10: `tcp_send_frame()` header/payload send failure (lines 406–419)**
- Condition: `socket_send_all()` returns false for header or payload.
- Outcome if true: logs `WARNING_HI`, returns false. Back in `TcpBackend::send_message()` line 291, failure is logged but loop continues — other client slots are still attempted. Return is `Result::OK` (send failure on one connection does not fail the overall call).

**Branch 11: `poll()` timeout in `socket_send_all()` (line 309)**
- Condition: `poll_result <= 0`
- Outcome: logs `WARNING_HI`, returns false. Propagates up through `tcp_send_frame()`.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. This entire send path executes synchronously on the caller's thread. No threads are spawned inside `send()`, `TcpBackend::send_message()`, or any called function.

### Where context switches occur
Only at OS blocking calls inside `socket_send_all()`:
- `poll(fd, 1, timeout_ms)` — blocks until the socket is writable or the timeout expires.
- `send(fd, ...)` — may block if the TCP send buffer is full, depending on whether `O_NONBLOCK` is set on the socket at this point. [ASSUMPTION: In client mode, `socket_set_nonblocking()` is called only during `socket_connect_with_timeout()` to perform a non-blocking connect. After the connection is established, no code in this path restores blocking mode explicitly. The socket remains non-blocking after connect, meaning `send()` may return partial bytes or `EAGAIN`. `socket_send_all()` handles partial writes via the `while (sent < len)` loop, with `poll()` before each attempt, which handles both blocking and non-blocking sockets correctly.]

### Synchronization primitives
None present in this send path. There is no mutex, semaphore, or `std::atomic` protecting `m_wire_buf`, `m_client_fds[]`, `m_client_count`, or `m_impairment` state.

- [UNKNOWN]: Whether `TcpBackend::send_message()` and `TcpBackend::receive_message()` are called from separate threads. If so, `m_wire_buf` (8192-byte shared buffer), `m_client_fds[]`, `m_client_count`, and `m_impairment.m_delay_buf[]` are all accessed without synchronization — a data race under the C++ memory model.
- [UNKNOWN]: Whether `DeliveryEngine::send()` itself is called concurrently; no locking is present.

### Producer/consumer relationships
None within this send path. `m_recv_queue` (a `RingBuffer`) is written by `recv_from_client()` and read by `receive_message()`, but it is not touched during the send path.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory
All memory is statically allocated inside the objects themselves:
- `MessageEnvelope env` in the application: caller-owned, stack-allocated (typical usage).
- `TcpBackend::m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes): member array of `TcpBackend`, allocated in place when `TcpBackend` is constructed.
- `TcpBackend::m_impairment` (an `ImpairmentEngine`): member of `TcpBackend`; its `m_delay_buf[32]` is a member array.
- `delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]` (32 elements): declared on the stack inside `TcpBackend::send_message()` at line 272.
- `header[4]` in `tcp_send_frame()`: stack array.
- `struct pollfd pfd` in `socket_send_all()`: stack-allocated per call.
- No `new`, `malloc`, or `delete` anywhere in this path (Power of 10 rule 3 compliance).

### Lifetime of key objects
- `MessageEnvelope& env` passed to `send()` must remain valid for the duration of the call (it is passed by reference and read inside `TcpBackend::send_message()`). After `send()` returns, it may go out of scope safely.
- `TcpBackend` must outlive all calls to `send_message()` — it is owned by the application layer that constructs and passes the `TransportInterface*` to `DeliveryEngine::init()`.

### Stack vs heap usage
- Every local in this entire call chain is stack-allocated or lives in object members (which are themselves part of the `TcpBackend` object's storage).
- The `delayed_envelopes[32]` buffer inside `TcpBackend::send_message()` is notably large (~32 * sizeof(MessageEnvelope) = 32 * (44 + 4096 + metadata) bytes on the stack). [OBSERVATION: Each `MessageEnvelope` is approximately 4132 bytes (8 + 8 + 4 + 4 + 1 + 1 + 8 + 4 + 4096 bytes plus padding), so `delayed_envelopes[32]` is roughly 132 KB on the stack. This is a potential stack overflow risk on embedded or constrained targets.]

### RAII usage
- `TcpBackend` destructor (`~TcpBackend()`) calls `close()`, which closes all sockets. This is the only RAII cleanup in the path.
- No `std::unique_ptr`, `std::lock_guard`, or other RAII wrappers (consistent with no-STL rule).

### Potential leaks or unsafe patterns
- If `tcp_send_frame()` fails mid-frame (e.g., header sent but payload send fails), the receiving end will have consumed 4 bytes of length prefix but not the corresponding payload. The TCP stream is in an inconsistent framing state. No recovery mechanism exists in this path. [OBSERVATION]
- `m_wire_buf` is written by `Serializer::serialize()` (step 6) and then read by `tcp_send_frame()` in the main loop (step 12), and then written again by the second `Serializer::serialize()` call in the delayed loop (step 18), and then read again by the second `tcp_send_frame()`. For a single-client scenario this is harmless but serializes the same data twice unnecessarily. [OBSERVATION]

---

## 9. Error Handling Flow

### How errors propagate
- Errors inside `socket_send_all()` return `false` to `tcp_send_frame()`, which returns `false` to `TcpBackend::send_message()`.
- In `TcpBackend::send_message()`, the return value of `tcp_send_frame()` at line 290 is **not captured** (`if (!tcp_send_frame(...))` only logs but the outer function still proceeds and returns `Result::OK`). Send errors on individual connections do not fail the overall send call.
- `Serializer::serialize()` returning non-OK causes `send_message()` to return that result immediately; this propagates through `send_via_transport()` to `send()`, which logs it and returns it to the application.
- `ImpairmentEngine::process_outbound()` returning `ERR_IO` causes `TcpBackend::send_message()` to return `Result::OK` (silent drop, line 268). The application receives `OK` even though the message was dropped.
- `ImpairmentEngine::process_outbound()` returning `ERR_FULL` (delay buffer full) is not explicitly handled at line 265–269 — only `ERR_IO` is intercepted. Any other non-OK return from `process_outbound()` (including `ERR_FULL`) is silently discarded and execution continues to the client send loop where the main-message `m_wire_buf` bytes would be sent even though the impairment engine did not accept the message. [OBSERVATION: `ERR_FULL` from `process_outbound()` results in inconsistent state — the raw bytes are still transmitted but the delayed-delivery accounting is wrong.]

### Retry logic
None in this path. `BEST_EFFORT` has no retry. The application receives `Result::OK` whether or not the bytes reached the network.

### Fallback paths
- If `m_client_count == 0`, the function returns `Result::OK` without sending anything.
- If all `tcp_send_frame()` calls fail, the function still returns `Result::OK`.

---

## 10. External Interactions

### Network calls
All POSIX socket calls occur inside `socket_send_all()` (`SocketUtils.cpp`):
- `poll(pfd, 1, timeout_ms)` — waits for the TCP socket to become writable.
- `send(fd, buf, remaining, 0)` — writes bytes to the TCP stream.
These are the only points where the process enters kernel space for I/O in this send path.

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
- `env.source_id` set to `m_local_id` (`DeliveryEngine.cpp:86`).
- `env.message_id` set to `m_id_gen.next()` (`DeliveryEngine.cpp:87`).
- `env.timestamp_us` set to `now_us` (`DeliveryEngine.cpp:88`).

**`MessageIdGen m_id_gen` (member of `DeliveryEngine`):**
- Internal counter incremented by `next()`. This is a permanent, monotonic side effect — the next call to `send()` will receive a higher ID.

**`ImpairmentEngine::m_delay_buf[]` and `m_delay_count` (member of `TcpBackend`):**
- One slot is activated in `process_outbound()`, then immediately deactivated in `collect_deliverable()`. Net state after `send_message()` returns: same as before (slot written and then freed in the same call).

**`TcpBackend::m_wire_buf[8192]` (member of `TcpBackend`):**
- Overwritten with the serialized envelope. State persists after `send_message()` returns; it contains stale data from the last send. This is intentional (reused buffer).

**TCP kernel send buffer:**
- Bytes are enqueued in the OS TCP send buffer. The kernel schedules transmission independently.

### Persistent changes (DB, disk, device state)
None. No disk writes, no database, no device registers.

---

## 12. Sequence Diagram using ASCII

```text
## Participants:

  App          DeliveryEngine    TcpBackend      ImpairEngine     Serializer      SocketUtils       OS/Kernel
   |                 |               |                |               |                |                 |

## Flow:

   |                 |               |                |               |                |                 |
   |--send(env,t)--->|               |                |               |                |                 |
   |                 |               |                |               |                |                 |
   |         stamp source_id,        |                |               |                |                 |
   |         message_id, ts          |                |               |                |                 |
   |                 |               |                |               |                |                 |
   |     send_via_transport(env)     |                |               |                |                 |
   |                 |               |                |               |                |                 |
   |                 |--send_msg(env)->               |               |                |                 |
   |                 |               |                |               |                |                 |
   |                 |               |--serialize(env)--------------->|                |                 |
   |                 |               |               |               |                 |                 |
   |                 |               |               |   write_u8/   |                 |                 |
   |                 |               |               |   write_u32/  |                 |                 |
   |                 |               |               |   write_u64   |                 |                 |
   |                 |               |               |   + memcpy    |                 |                 |
   |                 |               |               |               |                 |                 |
   |                 |               |<-----------Result::OK, wire_len----------------|                 |
   |                 |               |               |               |                 |                 |
   |                 |               |--timestamp_now_us()           |                |                 |
   |                 |               |  (internal)   |               |                |                 |
   |                 |               |               |                |               |                 |
   |                 |               |--process_outbound(env,now)--->|                |                 |
   |                 |               |               |                |               |                 |
   |                 |               |               | [impairments   |               |                 |
   |                 |               |               |  disabled:     |               |                 |
   |                 |               |               |  buffer msg,   |               |                 |
   |                 |               |               |  release=now]  |               |                 |
   |                 |               |               |                |               |                 |
   |                 |               |<-----------Result::OK----------|               |                 |
   |                 |               |               |                |               |                 |
   |                 |               |--collect_deliverable(now)---->|                |                 |
   |                 |               |               |               |                |                 |
   |                 |               |               | [slot active,  |               |                 |
   |                 |               |               |  release<=now: |               |                 |
   |                 |               |               |  copy out,     |               |                 |
   |                 |               |               |  free slot]    |               |                 |
   |                 |               |               |                |               |                 |
   |                 |               |<-----------delayed_count=1-----|               |                 |
   |                 |               |               |                |               |                 |
   |                 |               | [m_client_count>0: enter loop] |               |                 |
   |                 |               |               |                |               |                 |
   |                 |               |--tcp_send_frame(fd,wire_buf,wire_len,tmo)------>|                |
   |                 |               |               |               |                |                 |
   |                 |               |               |               | build 4-byte   |                 |
   |                 |               |               |               | BE length hdr  |                 |
   |                 |               |               |               |                |                 |
   |                 |               |               |               |--socket_send_all(fd,hdr,4,tmo)-->|
   |                 |               |               |               |                |                 |
   |                 |               |               |               |                |--poll(POLLOUT)->|
   |                 |               |               |               |                |<--writable------|
   |                 |               |               |               |                |--send(hdr,4)--->|
   |                 |               |               |               |                |<--sent 4--------|
   |                 |               |               |               |<----true--------|                |
   |                 |               |               |               |                |                 |
   |                 |               |               |               |--socket_send_all(fd,buf,wlen,tmo)->|
   |                 |               |               |               |                |                 |
   |                 |               |               |               |                |--poll(POLLOUT)->|
   |                 |               |               |               |                |<--writable------|
   |                 |               |               |               |                |--send(bytes)--->|
   |                 |               |               |               |                | [loop until     |
   |                 |               |               |               |                |  sent==wlen]    |
   |                 |               |               |               |                |<--sent wlen-----|
   |                 |               |               |               |<----true--------|                |
   |                 |               |<-----------true----------------|               |                 |
   |                 |               |               |                |               |                 |
   |                 |               | [delayed loop: re-serialize and re-send]       |                 |
   |                 |               |--serialize(delayed_envelopes[0])-------------->|                 |
   |                 |               |<-----------Result::OK, wire_len----------------|                 |
   |                 |               |--tcp_send_frame(fd,wire_buf,wlen,tmo)--------->|                 |
   |                 |               |  [same poll/send sequence as above]            |                 |
   |                 |               |<-----------true----------------|               |                 |
   |                 |               |               |                |               |                 |
   |                 |<--Result::OK--|               |                |               |                 |
   |                 |               |               |                |               |                 |
   | [BEST_EFFORT: AckTracker::track()   NOT CALLED]                  |               |                 |
   | [BEST_EFFORT: RetryManager::schedule() NOT CALLED]               |               |                 |
   |                 |               |               |                |               |                 |
   |<--Result::OK----|               |               |                |               |                 |
   |                 |               |               |                |               |                 |
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

1. Application constructs `TcpBackend` — constructor (`TcpBackend.cpp:25`) zero-initializes `m_client_fds[]` to -1, sets `m_open = false`.
2. Application calls `TcpBackend::init(config)` — creates TCP socket via `socket_create_tcp()`, calls `socket_set_reuseaddr()`, calls `socket_connect_with_timeout()` (which calls `socket_set_nonblocking()`, `connect()`, `poll()`, `getsockopt(SO_ERROR)`). On success, stores fd in `m_client_fds[0]`, sets `m_open = true`. Initializes `m_recv_queue` and `m_impairment` (via `ImpairmentEngine::init()` which seeds PRNG, zeroes buffers, resets partition state).
3. Application constructs `DeliveryEngine` and calls `DeliveryEngine::init(transport, cfg, local_id)` — stores `m_transport`, calls `m_ack_tracker.init()`, `m_retry_manager.init()`, `m_dedup.init()`, `m_id_gen.init(seed)`, sets `m_initialized = true`.

### What happens during steady-state execution

Each call to `DeliveryEngine::send()` follows the path described in section 3. No new memory is allocated. The `m_id_gen` counter increments with each send. The `ImpairmentEngine` delay buffer is transiently written and immediately cleared within each `send_message()` call (zero-delay path). The TCP kernel send buffer accumulates bytes until the kernel transmits them.

---

## 14. Known Risks / Observations

### Double-send of the same message data

`TcpBackend::send_message()` serializes the envelope into `m_wire_buf` (step 6) and calls `tcp_send_frame()` for the main message in the `m_client_fds[]` loop at line 290. Then it calls `collect_deliverable()` (step 11), which returns `delayed_count = 1` with the same envelope that was just placed in the delay buffer. The delayed-envelope loop at lines 298–315 then re-serializes the same envelope and calls `tcp_send_frame()` again. **The same message is sent twice to each connected client in the zero-impairment path.** This appears to be a design defect: `process_outbound()` with impairments disabled acts as a passthrough buffer, and `collect_deliverable()` immediately returns those messages for a second transmit. The receiver will see two copies of every BEST_EFFORT message. This is observable as spurious duplication even without impairment-based duplication enabled. [OBSERVATION - potential bug]

### Unsynchronized shared state

`TcpBackend::m_wire_buf`, `m_client_fds[]`, `m_client_count`, and `ImpairmentEngine::m_delay_buf[]` are all modified by `send_message()` and read/written by `receive_message()` and `recv_from_client()`. If send and receive are called from different threads (which is the expected usage pattern for a message engine), all of these accesses are data races under the C++ memory model. No mutex, `std::atomic`, or other synchronization is present. [RISK]

### Stack pressure from `delayed_envelopes[]`

`delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]` (32 elements of `MessageEnvelope`) is stack-allocated inside `send_message()`. `sizeof(MessageEnvelope) = 4 + 8 + 8 + 4 + 4 + 1 + 1 + 8 + 4 + 4096 = ~4140 bytes` (exact value depends on struct padding). 32 elements is approximately 132 KB on the stack. On platforms with small stack sizes (common in embedded targets this codebase style targets), this is a stack overflow risk. [RISK]

### ERR_FULL from process_outbound() not intercepted

`TcpBackend::send_message()` lines 265–269 intercept only `Result::ERR_IO` from `process_outbound()`. If `ERR_FULL` is returned (delay buffer full), the code falls through and still calls `tcp_send_frame()` with the bytes in `m_wire_buf`. This means the message is transmitted on the wire even though the impairment engine did not accept and buffer it, making the impairment engine's state inconsistent with what was sent. [RISK - potential bug]

### tcp_send_frame() failure is silently absorbed

At `TcpBackend.cpp:290–294`, if `tcp_send_frame()` returns false (send failure), a `WARNING_LO` is logged but the loop continues and `send_message()` returns `Result::OK`. The `DeliveryEngine` receives `OK` and the application has no way to distinguish a failed send from a successful one. For `BEST_EFFORT` this may be intentional by design (fire and forget), but for `RELIABLE_ACK` or `RELIABLE_RETRY` the same code path applies, potentially misleading higher layers. [OBSERVATION]

### m_wire_buf is not re-entrant

`m_wire_buf` is a single 8192-byte member buffer shared between the main-message serialization and the delayed-message loop's re-serialization. In the delayed loop, `Serializer::serialize()` overwrites `m_wire_buf` at line 302, replacing the previously serialized main-message bytes. This is safe in the single-threaded case because the main-message `tcp_send_frame()` has already completed. However, it reinforces the single-threaded assumption and would corrupt output if `send_message()` were ever called concurrently. [OBSERVATION]

### Logger call at send completion

`Logger::log(INFO, ...)` is called at `DeliveryEngine.cpp:136–139` with `message_id` and `reliability_class`. This is in the hot path on every send. If `Logger` does synchronous file or console I/O, it adds latency to every send. [OBSERVATION]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application passes `reliability_class = BEST_EFFORT` correctly set in the envelope before calling `send()`. The engine does not validate or override this field; it trusts the caller.

- [ASSUMPTION] `TcpBackend` is used in client mode (not server mode) for this use case. In server mode, `send_message()` iterates all `m_client_fds[]` slots; behavior is similar but affects multiple connections.

- [ASSUMPTION] `impairment_config_default()` initializes `ImpairmentConfig::enabled = false`. This is inferred from the disabled-path behavior in `process_outbound()` and from the comment at `TcpBackend.cpp:62`; the actual implementation of `impairment_config_default()` is in a file not listed in the source set.

- [ASSUMPTION] `MessageIdGen::next()` returns a monotonically increasing non-zero `uint64_t`. Implementation not in the source set provided; inferred from `m_id_gen.init(seed)` and postcondition `env.message_id != 0` at `DeliveryEngine.cpp:134`.

- [ASSUMPTION] `timestamp_now_us()` returns the current wall-clock time in microseconds as a `uint64_t`. Implementation is in `Timestamp.hpp/cpp`, which was not in the source set.

- [ASSUMPTION] `RingBuffer::push()` and `RingBuffer::pop()` are bounded, non-blocking, and do not allocate memory. Implementation is in `core/RingBuffer.hpp`, not in the provided source set.

- [UNKNOWN] Whether `DeliveryEngine::send()` is called from a single thread or multiple threads. No synchronization exists; concurrent calls would create data races on `m_id_gen` (shared counter) and `TcpBackend::m_wire_buf`.

- [UNKNOWN] Whether the TCP socket remains in non-blocking mode after `socket_connect_with_timeout()` completes, or whether the backend restores blocking mode. `socket_set_nonblocking()` is called inside `socket_connect_with_timeout()` but there is no `fcntl(F_SETFL, ~O_NONBLOCK)` call after the connect. If the socket is non-blocking, `send()` inside `socket_send_all()` may return `EAGAIN` (mapped to `send_result < 0` path), causing premature failure.

- [UNKNOWN] The exact struct size and padding of `MessageEnvelope`. The stack size estimate of ~132 KB for `delayed_envelopes[32]` depends on compiler padding between fields and after `payload[4096]`.

- [UNKNOWN] Whether `Logger::log()` is thread-safe. If not, concurrent sends from multiple threads will produce interleaved or corrupted log output.

- [UNKNOWN] What `DuplicateFilter::init()` and the dedup logic in `receive()` does on the receive side for `BEST_EFFORT` messages. The send path is unaffected, but if the receiver applies dedup to BEST_EFFORT messages it may silently drop the intentional duplicate introduced by the double-send bug.

- [ASSUMPTION] `envelope_valid()` at `MessageEnvelope.hpp:55` is the definitive validity check used consistently across all assertion sites. It checks `message_type != INVALID`, `payload_length <= MSG_MAX_PAYLOAD_BYTES`, and `source_id != NODE_ID_INVALID`. Because `source_id` is stamped in `send()` before `send_via_transport()` is called, the assertion `envelope_valid(env)` in `send_via_transport()` and `send_message()` will pass even if the caller left `source_id = 0` initially.