# UC_06 — TCP inbound deserialisation — recv_from_client() called internally by TcpBackend::receive_message()

**HL Group:** System Internal — sub-function of HL-5 (Receive a Message); invoked inside
`TcpBackend::receive_message()`, which is itself called by `DeliveryEngine::receive()`.
The User never calls `recv_from_client()` directly.
**Actor:** System (internal sub-function)
**Requirement traceability:** REQ-3.2.3, REQ-4.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.3.2

---

## 1. Use Case Overview

**Trigger:** Inside `TcpBackend::receive_message()`, after `poll()` reports that a client fd
is readable, `poll_clients_once()` calls `recv_from_client(client_fd, timeout_ms)` for each
readable client fd.

**Higher-level UC that invokes this sub-function:** UC_05 (server echo reply) and any
application call to `DeliveryEngine::receive()`. `recv_from_client()` is never called
outside `TcpBackend`'s private implementation.

**Goal:** Read exactly one length-prefixed frame from the specified TCP client fd, deserialize
it into a `MessageEnvelope`, and push it onto the `TcpBackend`'s `m_recv_queue` SPSC ring
buffer so that `receive_message()` can return it to the caller.

**Success outcome:** `recv_from_client()` returns `Result::OK`. One `MessageEnvelope` has
been pushed to `m_recv_queue` and is available for the next `pop()` call.

**Error outcomes:**
- `Result::ERR_IO` — `recv_frame()` failed (connection reset, partial read that cannot be
  completed, or timeout). The client fd is removed and closed via `remove_client_fd()`.
  WARNING_LO is logged.
- `Result::ERR_INVALID` (from `Serializer::deserialize()`) — the received bytes do not form
  a valid wire frame (bad padding byte, `payload_length > MSG_MAX_PAYLOAD_BYTES`, or buffer
  too small). WARNING_LO is logged; the fd is NOT closed.
- `Result::ERR_FULL` (from `m_recv_queue.push()`) — the receive ring buffer is full
  (MSG_RING_CAPACITY = 64 messages). The deserialized envelope is discarded. WARNING_HI is
  logged. Returns `OK` (not propagated as an error from `recv_from_client()`).

---

## 2. Entry Points

```
// src/platform/TcpBackend.cpp — private method, never called from outside TcpBackend
Result TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms);

// Called internally by:
// src/platform/TcpBackend.cpp
void TcpBackend::poll_clients_once(uint32_t timeout_ms);

// Which is called by:
// src/platform/TcpBackend.cpp
Result TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms);
```

The complete inbound call chain is:
```
DeliveryEngine::receive()
  → TcpBackend::receive_message()
      → TcpBackend::poll_clients_once()
          → TcpBackend::recv_from_client(client_fd, timeout_ms)
              → ISocketOps::recv_frame()
              → Serializer::deserialize()
              → RingBuffer::push()
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. `poll_clients_once(timeout_ms)` has already called `poll()` and observed that
   `m_client_fds[i]` is readable (`POLLIN` event). It calls
   `recv_from_client(m_client_fds[i], timeout_ms)`.
2. `recv_from_client()` fires two pre-condition `NEVER_COMPILED_OUT_ASSERT`s:
   `client_fd >= 0`, `m_open`.
3. Declares `out_len = 0U` on the stack (uint32_t).
4. Calls `m_sock_ops->recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES,
   timeout_ms, &out_len)`.
   - `recv_frame()` reads a length-prefixed frame from the TCP socket into `m_wire_buf`
     (8192-byte buffer). The first N bytes encode the frame length (implementation-defined
     framing in `SocketUtils`); the subsequent bytes are the serialized envelope.
   - On success: `out_len` is set to the number of bytes in the frame (header + payload
     bytes, not including the length prefix itself), returns `true`.
   - On failure (connection closed, RST, timeout, partial frame): returns `false`.
5. If `recv_frame()` returns `false`:
   - Logs WARNING_LO: `"recv_frame failed; closing connection"`.
   - Calls `remove_client_fd(client_fd)`:
     a. Asserts `client_fd >= 0`, `m_client_count > 0`.
     b. Scans `m_client_fds[0..MAX_TCP_CONNECTIONS-1]` for the matching fd.
     c. On match at index `i`: calls `m_sock_ops->do_close(m_client_fds[i])`, sets
        `m_client_fds[i] = -1`, compacts the array by shifting entries left, sets the
        last vacated slot to -1, decrements `m_client_count`.
     d. Asserts post-condition: `m_client_count < MAX_TCP_CONNECTIONS`.
   - Returns `Result::ERR_IO`.
6. If `recv_frame()` returns `true`:
   - Declares `MessageEnvelope env` on the stack.
   - Calls `Serializer::deserialize(m_wire_buf, out_len, env)`.
7. Inside `Serializer::deserialize(buf, buf_len, env)`:
   a. Pre-condition asserts: `buf != nullptr`, `buf_len <= 0xFFFFFFFF`.
   b. Checks `buf_len < WIRE_HEADER_SIZE (44)` → returns `ERR_INVALID` if too short.
   c. Calls `envelope_init(env)` — zero-initializes output envelope.
   d. Reads header fields sequentially using `read_u8()` / `read_u32()` / `read_u64()`
      helpers:
      - offset 0: `message_type` (1 byte).
      - offset 1: `reliability_class` (1 byte).
      - offset 2: `priority` (1 byte).
      - offset 3: padding byte — if not 0, returns `ERR_INVALID`.
      - offset 4–11: `message_id` (8 bytes BE).
      - offset 12–19: `timestamp_us` (8 bytes BE).
      - offset 20–23: `source_id` (4 bytes BE).
      - offset 24–27: `destination_id` (4 bytes BE).
      - offset 28–35: `expiry_time_us` (8 bytes BE).
      - offset 36–39: `payload_length` (4 bytes BE).
      - offset 40–43: padding word — if not 0, returns `ERR_INVALID`.
   e. Asserts `offset == WIRE_HEADER_SIZE (44)`.
   f. Checks `env.payload_length > MSG_MAX_PAYLOAD_BYTES (4096)` → returns `ERR_INVALID`.
   g. Checks `buf_len < WIRE_HEADER_SIZE + env.payload_length` → returns `ERR_INVALID`.
   h. If `env.payload_length > 0`: `memcpy(env.payload, &buf[44], env.payload_length)`.
   i. Post-condition assert: `envelope_valid(env)`.
   j. Returns `OK`.
8. Back in `recv_from_client()`: checks `result_ok(res)`:
   - Non-OK (e.g., `ERR_INVALID`): logs WARNING_LO: `"Deserialize failed: ..."`. Returns
     `res` (e.g., `ERR_INVALID`). The client fd is NOT closed.
9. If `deserialize()` returned `OK`: calls `m_recv_queue.push(env)`.
10. Inside `RingBuffer::push(env)`:
    a. Loads `m_tail` (relaxed), `m_head` (acquire). Computes `cnt = t - h`.
    b. Asserts `cnt <= MSG_RING_CAPACITY`.
    c. If `cnt >= MSG_RING_CAPACITY`: returns `ERR_FULL`.
    d. Calls `envelope_copy(m_buf[t & RING_MASK], env)` — memcpy into the ring slot.
    e. Stores `m_tail = t + 1` (release).
    f. Returns `OK`.
11. Back in `recv_from_client()`: checks `result_ok(res)` from push:
    - Non-OK (`ERR_FULL`): logs WARNING_HI: `"Recv queue full; dropping message"`.
      (Does not return early here — falls through to the post-condition assert.)
12. Post-condition assert: `out_len <= SOCKET_RECV_BUF_BYTES`.
13. Returns `Result::OK` (regardless of whether `push()` succeeded — the function succeeded
    in reading the frame; queue overflow is a resource-limit event, not a read failure).

---

## 4. Call Tree

```
TcpBackend::recv_from_client(client_fd, timeout_ms)
 ├── ISocketOps::recv_frame(client_fd, m_wire_buf, 8192, timeout_ms, &out_len)
 │    └── [POSIX recv/read syscall — length-prefixed framing]
 ├── [on recv_frame failure] TcpBackend::remove_client_fd(client_fd)
 │    └── ISocketOps::do_close(client_fd)
 │         └── [POSIX close() syscall]
 ├── Serializer::deserialize(m_wire_buf, out_len, env)
 │    ├── envelope_init(env)                 [memset + INVALID sentinel]
 │    ├── read_u8() × 4                      [message_type, reliability_class, priority, padding]
 │    ├── read_u64() × 3                     [message_id, timestamp_us, expiry_time_us]
 │    ├── read_u32() × 3                     [source_id, destination_id, payload_length + padding]
 │    └── memcpy(env.payload, buf+44, len)   [payload bytes]
 └── RingBuffer::push(env)
      └── envelope_copy(m_buf[t & MASK], env) [memcpy into ring slot]
```

---

## 5. Key Components Involved

- **`TcpBackend::recv_from_client()`** — Private method. Coordinates the three-step inbound
  pipeline: read frame from socket, deserialize bytes into envelope, push envelope into
  receive queue.

- **`ISocketOps::recv_frame()`** — Abstracted POSIX recv. Reads one complete length-prefixed
  frame from the TCP socket into `m_wire_buf`. Handles partial reads internally (bounded by
  `SOCKET_RECV_BUF_BYTES`). Returns `false` on any connection-level error.

- **`Serializer::deserialize()`** — Static function. Converts raw bytes from `m_wire_buf`
  into a `MessageEnvelope`. Validates padding bytes and payload length. Uses manual bit-shift
  reads for endian-safe big-endian decoding (no `ntohs`/`ntohl`).

- **`RingBuffer::push()`** — SPSC lock-free push. Uses `std::atomic<uint32_t>` with
  acquire/release ordering to safely hand off the envelope to the consumer thread (or the
  same thread on the next `pop()` call).

- **`TcpBackend::remove_client_fd()`** — Private helper. Closes the fd and compacts the
  `m_client_fds[]` array. Called only on `recv_frame()` failure.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `recv_frame()` returns false | Log WARNING_LO, call `remove_client_fd()`, return `ERR_IO` | Continue to deserialize | — |
| `buf_len < WIRE_HEADER_SIZE` in `deserialize()` | Return `ERR_INVALID` | Continue header parse | — |
| Padding byte != 0 in `deserialize()` | Return `ERR_INVALID` | Continue | — |
| Padding word != 0 in `deserialize()` | Return `ERR_INVALID` | Continue | — |
| `payload_length > MSG_MAX_PAYLOAD_BYTES` in `deserialize()` | Return `ERR_INVALID` | Continue | payload copy |
| `buf_len < WIRE_HEADER_SIZE + payload_length` | Return `ERR_INVALID` | Continue | payload copy |
| `deserialize()` returns non-OK | Log WARNING_LO, return `res`; fd NOT closed | Call `m_recv_queue.push()` | — |
| `RingBuffer::push()` returns `ERR_FULL` | Log WARNING_HI; fall through to post-condition | Envelope successfully queued | Return OK |

---

## 7. Concurrency / Threading Behavior

- **Single-producer / single-consumer (SPSC) contract for `m_recv_queue`:**
  - **Producer:** `recv_from_client()` → `RingBuffer::push()`. In the server demo, this runs
    on the application main thread (same thread as the consumer).
  - **Consumer:** `TcpBackend::receive_message()` → `RingBuffer::pop()`. Also on the
    application main thread.
  - Because producer and consumer are the same thread in the demo, the SPSC atomic ordering
    provides stronger guarantees than strictly needed. In a multi-threaded variant, the
    producer and consumer could be different threads as long as exactly one thread pushes and
    exactly one thread pops.

- **`std::atomic` usage in `RingBuffer::push()`:**
  - `m_tail.load(std::memory_order_relaxed)` — producer reads its own tail (no sync needed).
  - `m_head.load(std::memory_order_acquire)` — producer observes freed slots from consumer.
  - `m_tail.store(t+1, std::memory_order_release)` — consumer's next acquire of `m_tail`
    synchronizes-with this store, making the written `m_buf[t & RING_MASK]` data visible.

- **`m_wire_buf` is not protected.** `recv_from_client()` writes into `m_wire_buf`, and so
  does `send_message()` (via `Serializer::serialize()`). Both are called from the application
  thread; there is no concurrent access to `m_wire_buf`.

- **`m_client_fds[]` and `m_client_count`** are not protected by any lock. They are only
  accessed from the application thread that calls `receive_message()` (and `send_message()`).

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `m_wire_buf` | `TcpBackend` member | `SOCKET_RECV_BUF_BYTES` = 8192 bytes | Written by `recv_frame()`; read by `Serializer::deserialize()`. Reused on every call. |
| `out_len` | Stack of `recv_from_client` | `uint32_t` (4 bytes) | Populated by `recv_frame()` with the frame byte count |
| `env` (local) | Stack of `recv_from_client` | `sizeof(MessageEnvelope)` ≈ 4140 bytes | Stack-allocated; populated by `deserialize()`; passed to `push()` by const-ref |
| `m_buf[t & RING_MASK]` | `RingBuffer` member array | one `MessageEnvelope` ≈ 4140 bytes | Deep-copied from `env` via `envelope_copy()` (memcpy). Owned by `RingBuffer`; consumed by `pop()`. |
| `RingBuffer m_buf[]` | `TcpBackend` member | `MSG_RING_CAPACITY * sizeof(MessageEnvelope)` = 64 × ~4140 ≈ 265 KB | Fixed; no heap. |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. `m_wire_buf` and `m_buf[]` are
fixed-size members. `env` is stack-allocated.

---

## 9. Error Handling Flow

| Condition | System state after | What caller (`poll_clients_once`) does |
|---|---|---|
| `recv_frame()` false | Client fd closed and removed from `m_client_fds[]`; `m_client_count` decremented; `ERR_IO` returned | `poll_clients_once()` loop continues to the next client fd; `receive_message()` will eventually return `ERR_TIMEOUT` if no other message |
| `deserialize()` → `ERR_INVALID` | `m_wire_buf` contains bad frame; `env` is partially initialized; fd is still open; WARNING_LO logged; `ERR_INVALID` returned | `poll_clients_once()` continues; bad frame is discarded; connection remains open |
| `push()` → `ERR_FULL` | Envelope deserialized but not queued; WARNING_HI logged; `recv_from_client()` returns `OK` | `receive_message()` will return `ERR_TIMEOUT` on this message (the frame was read from the socket and consumed, but the envelope was dropped) |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `ISocketOps::recv_frame()` → `SocketOpsImpl::recv_frame()` | TCP client fd | Blocking length-prefixed frame read. Timeout enforced by the underlying implementation (select/poll + recv). |
| `ISocketOps::do_close()` → `SocketOpsImpl::do_close()` | TCP client fd | POSIX `close()` syscall; only called on `recv_frame()` failure. |

No `clock_gettime()`, file I/O, or IPC occurs within `recv_from_client()` itself. The
`timeout_ms` parameter is passed through to `recv_frame()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|---|---|---|---|
| `m_wire_buf` | bytes [0..out_len-1] | Stale previous frame | Bytes of the newly received frame (header + payload) |
| `RingBuffer m_recv_queue` | `m_buf[t & RING_MASK]` | Stale / uninitialized | Deep copy of deserialized `env` |
| `RingBuffer m_recv_queue` | `m_tail` (atomic) | T | T+1 (release store) |
| `m_client_fds[i]` | fd value | `client_fd` (valid) | -1 (only if `recv_frame()` fails) |
| `m_client_count` | N | N-1 (only if `recv_frame()` fails) |
| Kernel TCP receive buffer | bytes | Frame bytes | Consumed (bytes removed from kernel buffer by `recv_frame()`) |

---

## 12. Sequence Diagram

```
poll_clients_once   recv_from_client   ISocketOps       Serializer        RingBuffer
      |                    |               |                  |                |
      | (poll detected     |               |                  |                |
      |  POLLIN on fd)     |               |                  |                |
      |--recv_from_client(fd, timeout)->   |                  |                |
      |                    |--recv_frame(fd,wire_buf,8192,t,&len)-->           |
      |                    |               |  [POSIX recv]    |                |
      |                    |               |<--true, out_len--|                |
      |                    |--deserialize(wire_buf, out_len, env)-->           |
      |                    |               |  envelope_init() |                |
      |                    |               |  read_u8/u32/u64 fields           |
      |                    |               |  memcpy payload  |                |
      |                    |               |<--OK, env------- |                |
      |                    |--push(env)----------------------->                |
      |                    |               |                  | envelope_copy  |
      |                    |               |                  | m_tail release |
      |                    |               |                  |<--OK-----------|
      |<--OK (or ERR_IO/ERR_INVALID)-------|                  |                |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (preconditions):**
- `TcpBackend::init()` must have completed: `m_open = true`, `m_recv_queue.init()` called
  (m_head = m_tail = 0), `m_wire_buf` zero-initialized as a member of `TcpBackend`.
- `m_client_fds[]` contains valid fds populated by prior `accept_clients()` calls (server)
  or by `connect_to_server()` (client).

**Steady-state:**
- `recv_from_client()` is called by `poll_clients_once()` for every client fd that `poll()`
  reports as readable. It is called at most once per fd per `poll_clients_once()` invocation.
- After `recv_from_client()` returns `OK`, the envelope is in `m_recv_queue` and available
  for the next `pop()` call in `receive_message()`.
- The `m_wire_buf` is reused on every call; its contents are valid only for the duration of
  the `recv_from_client()` call (until the next call overwrites it).

---

## 14. Known Risks / Observations

- **`recv_frame()` failure closes the fd but does not notify higher layers immediately.** The
  application calling `receive()` will eventually time out and return `ERR_TIMEOUT` on this
  iteration. On subsequent iterations, the fd will not appear in the poll set (it has been
  removed), so the connection loss is handled implicitly.

- **Queue overflow silently discards a deserialized message.** If `m_recv_queue` is full, the
  bytes have already been read from the TCP socket (consumed from the kernel buffer) and the
  envelope is dropped. The sender has no indication the message was lost at this stage.
  This is a capacity-limit failure that WARNING_HI logs but does not propagate as an error
  to `poll_clients_once()`.

- **`recv_from_client()` returns `OK` even when `push()` fails.** The caller
  (`poll_clients_once()`) cannot distinguish between "message queued" and "message dropped
  due to queue full" from the return code.

- **Partial-frame handling is delegated to `recv_frame()`.** `recv_from_client()` assumes
  that `recv_frame()` either delivers a complete frame or returns `false`. No partial-frame
  state is maintained in `recv_from_client()` itself.

- **`m_wire_buf` is a shared object-level buffer.** If `send_message()` is called while
  `recv_from_client()` is executing (impossible in single-thread design but possible if
  threading rules were violated), they would race on `m_wire_buf`.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `ISocketOps::recv_frame()` reads exactly one complete length-prefixed frame
  per call. The length prefix itself is consumed by `recv_frame()` and is not included in
  `out_len`. The exact framing format (prefix width, endianness) is in `SocketUtils` which
  was not read for this document.

- [ASSUMPTION] A `recv_frame()` failure always indicates a connection-level error (not just
  a timeout on data arrival), so closing the fd is the correct response. A pure timeout
  would return `false` with `out_len = 0`; the client fd would then be incorrectly closed.
  Whether `recv_frame()` distinguishes timeout from error is not confirmed from the source
  files read.

- [ASSUMPTION] `Serializer::deserialize()` is called with `m_wire_buf` as its input buffer.
  The buffer is always at least `out_len` bytes valid because `recv_frame()` populates it
  before returning. The remaining bytes in `m_wire_buf` beyond `out_len` are stale from
  previous calls but are not read (the deserializer uses `buf_len = out_len`).

- [ASSUMPTION] `envelope_valid(env)` in `Serializer::deserialize()`'s post-condition assert
  will pass for all structurally valid frames. A structurally valid frame with a semantically
  invalid field (e.g., `message_type = INVALID`) would cause the assert to fire at
  NEVER_COMPILED_OUT_ASSERT semantics.
