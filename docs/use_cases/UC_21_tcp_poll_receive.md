# UC_21 — TCP poll and receive

**HL Group:** System Internal — sub-function of HL-5 (User waits for an incoming message)

**Actor:** System (internal sub-function)

**Requirement traceability:** REQ-4.1.3, REQ-6.1.6, REQ-6.1.7, REQ-6.3.2, REQ-6.3.3, REQ-6.3.5

---

## 1. Use Case Overview

### Invoked by
This use case documents the internal mechanism implemented by `TcpBackend::receive_message()`,
which is called directly by `DeliveryEngine::receive()` (the HL-5 system entry point). In the
server echo application pattern (UC_05), `run_server_iteration()` at Server.cpp calls
`engine.receive()` → `TcpBackend::receive_message()` on every loop iteration.

It is factored out as a distinct mechanism because the TCP receive path involves four
cooperating sub-operations that must be understood separately from the higher-level message
delivery semantics: (1) a bounded multi-attempt poll loop with configurable timeout chunking,
(2) per-client `poll()`/`recv()` framing via `socket_recv_exact()`, (3) deserialization via
`Serializer::deserialize()` (UC_26), and (4) impairment-delay drainage via
`collect_deliverable()`. None of these mechanics are visible at the HL-5 user boundary —
`receive_message()` appears to the caller as a single blocking call returning either a
`MessageEnvelope` or `ERR_TIMEOUT`.

### Trigger
A caller invokes `TcpBackend::receive_message(envelope, timeout_ms)` to retrieve an inbound
`MessageEnvelope` from any currently-connected TCP client.

### Expected Outcome
The function polls all connected client sockets, reads a complete length-prefixed TCP frame,
deserializes it into a `MessageEnvelope`, and returns the envelope to the caller. As a side effect
it also drains any impairment-delayed messages from the delay buffer into the receive queue. If no
message is received within the allotted time, `ERR_TIMEOUT` is returned.

---

## 2. Entry Points

| Entry point | File | Line |
|-------------|------|------|
| `TcpBackend::receive_message(MessageEnvelope&, uint32_t)` | `src/platform/TcpBackend.cpp` | 386 |

Precondition assertion: `m_open` must be true (line 388).

Internal sub-entries called from `receive_message()`:

| Internal function | File | Line |
|-------------------|------|------|
| `TcpBackend::poll_clients_once(uint32_t)` | `src/platform/TcpBackend.cpp` | 326 |
| `TcpBackend::accept_clients()` | `src/platform/TcpBackend.cpp` | 167 |
| `TcpBackend::recv_from_client(int, uint32_t)` | `src/platform/TcpBackend.cpp` | 224 |
| `TcpBackend::flush_delayed_to_queue(uint64_t)` | `src/platform/TcpBackend.cpp` | 306 |

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **`TcpBackend::receive_message()`** (TcpBackend.cpp:386) is called by the application.
2. Precondition assertion: `m_open` must be true (line 388).
3. **Fast-path queue check** (line 391): `m_recv_queue.pop(envelope)` (RingBuffer.hpp:131) is
   called immediately.
   - Loads `m_head` with relaxed ordering, loads `m_tail` with acquire ordering (line 134–135).
   - If `cnt == 0` (queue empty), returns `ERR_EMPTY`; execution continues to the poll loop.
   - If `cnt > 0`, calls `envelope_copy(env, m_buf[h & RING_MASK])` (line 143), releases
     `m_head` with release store (line 146), returns `OK`. The function returns to the caller
     immediately — no polling occurs.
4. **Poll count computation** (lines 396–400):
   - `poll_count = (timeout_ms + 99U) / 100U` — ceiling division into 100ms chunks.
   - Capped at 50 (line 397–398): `if (poll_count > 50U) poll_count = 50U`.
   - `NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U)` at line 400.
   - If `timeout_ms == 0`, `poll_count == 0`; the loop body never executes; returns
     `ERR_TIMEOUT` immediately.
5. **Outer poll loop** (line 403): `for (attempt = 0; attempt < poll_count; ++attempt)`.
   Bounded by `poll_count <= 50U`.

   **5a. `poll_clients_once(100U)`** (TcpBackend.cpp:326) is called (line 404):
   - Assertions: `m_open` (line 328) and `timeout_ms <= 60000U` (line 329).
   - If `m_is_server == true` (line 331): calls `accept_clients()` (TcpBackend.cpp:167):
     - Assertions: `m_is_server` (line 169) and `m_listen_fd >= 0` (line 170).
     - Capacity guard (line 172): if `m_client_count >= MAX_TCP_CONNECTIONS`, returns `OK`.
     - Calls `socket_accept(m_listen_fd)` (SocketUtils.cpp:252): calls `accept(fd, nullptr,
       nullptr)`. Returns `client_fd >= 0` on success, `< 0` if no client pending.
     - If `client_fd < 0`: returns `OK` (no pending connection is not an error, line 180).
     - Otherwise: stores `client_fd` at `m_client_fds[m_client_count]` (line 185), increments
       `m_client_count` (line 186), logs `INFO`, asserts
       `m_client_count <= MAX_TCP_CONNECTIONS` (line 191), returns `OK`.
   - Inner loop (TcpBackend.cpp:336): `for (i = 0; i < MAX_TCP_CONNECTIONS; ++i)`:
     - Skips slots where `m_client_fds[i] < 0` (line 337).
     - Calls `recv_from_client(m_client_fds[i], timeout_ms)` (line 338) for each active slot.
       Return value is cast to `(void)` — errors are not propagated up from this call.

   **5b. Inside `recv_from_client(client_fd, 100U)`** (TcpBackend.cpp:224):
   - Assertions: `client_fd >= 0` (line 226) and `m_open` (line 227).
   - Calls **`tcp_recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, 100U, &out_len)`**
     (SocketUtils.cpp:431):
     - Assertions: `fd >= 0` (line 435), `buf != nullptr` (line 436), `buf_cap > 0` (line 437),
       `out_len != nullptr` (line 438).
     - **Read 4-byte header**: calls `socket_recv_exact(fd, header, 4U, 100U)`
       (SocketUtils.cpp:339):
       - `poll({fd, POLLIN}, 1, 100)` per iteration; if `<= 0`, logs `WARNING_HI
         "recv poll timeout"`, returns false.
       - `recv(fd, &header[received], remaining, 0)` per iteration; if `< 0`, logs
         `WARNING_HI "recv() failed: %d"`, returns false; if `== 0`, logs `WARNING_HI
         "recv() returned 0 (socket closed)"`, returns false.
       - Loop terminates when `received == 4`; asserts `received == 4` (line 385). Returns true.
     - If `socket_recv_exact` for header fails: logs `WARNING_HI "tcp_recv_frame: failed to
       receive header"` (SocketUtils.cpp:443), returns false.
     - **Parse frame length** (lines 449–452): big-endian decode of `header[0..3]` into
       `frame_len` (uint32_t).
     - **Validate frame length** (line 454–461): `max_frame_size = WIRE_HEADER_SIZE +
       MSG_MAX_PAYLOAD_BYTES = 44 + 4096 = 4140`. If `frame_len > max_frame_size ||
       frame_len > buf_cap (8192)`, logs `WARNING_HI "frame size %u exceeds limit %u"`,
       returns false.
     - **Read payload** (line 464–470): if `frame_len > 0U`, calls
       `socket_recv_exact(fd, buf, frame_len, 100U)` — same `poll/recv` loop until
       `received == frame_len`. On failure: logs `WARNING_HI "failed to receive payload
       (%u bytes)"`, returns false.
     - Sets `*out_len = frame_len` (line 472). Asserts `*out_len <= buf_cap` (line 475)
       and `*out_len <= max_frame_size` (line 476). Returns true.
   - If `tcp_recv_frame` returns false (TcpBackend.cpp:230): logs `WARNING_LO
     "recv_frame failed; closing connection"` (line 231). Calls
     **`remove_client_fd(client_fd)`** (TcpBackend.cpp:199):
     - Assertions: `client_fd >= 0` (line 201) and `m_client_count > 0U` (line 202).
     - Loops `m_client_fds[0..MAX_TCP_CONNECTIONS-1]` to find `client_fd` (line 204–216).
     - Calls `socket_close(m_client_fds[i])` (SocketUtils.cpp:274) which calls `close(fd)`.
     - Sets `m_client_fds[i] = -1` (line 207).
     - Inner compaction loop (line 209–211): shifts `m_client_fds[i+1..m_client_count-1]`
       left by one position; sets `m_client_fds[m_client_count-1] = -1` (line 212).
     - Decrements `m_client_count` (line 213). Asserts `m_client_count < MAX_TCP_CONNECTIONS`
       (line 214). Returns.
     - `recv_from_client` returns `ERR_IO` (line 233). This return code is discarded by
       `poll_clients_once()` (the `(void)` cast at line 338).
   - If `tcp_recv_frame` returns true: calls **`Serializer::deserialize(m_wire_buf, out_len, env)`**
     (Serializer.cpp:175):
     - Assertions: `buf != nullptr` (line 180) and `buf_len <= 0xFFFFFFFF` (line 181).
     - If `buf_len < WIRE_HEADER_SIZE` (44): returns `ERR_INVALID` (line 184–186).
     - Calls `envelope_init(env)` (line 189) — zeroes the output envelope (sets
       `message_type = INVALID`).
     - Reads header fields sequentially with `read_u8`/`read_u32`/`read_u64` (lines 195–234):
       message_type, reliability_class, priority, padding1, message_id, timestamp_us,
       source_id, destination_id, expiry_time_us, payload_length, padding2.
     - Validates `padding1 == 0U` (line 207); returns `ERR_INVALID` if non-zero.
     - Validates `padding2 == 0U` (line 232); returns `ERR_INVALID` if non-zero.
     - Asserts `offset == WIRE_HEADER_SIZE` (line 237).
     - Validates `payload_length <= MSG_MAX_PAYLOAD_BYTES` (line 240); returns `ERR_INVALID`
       if violated.
     - Validates `buf_len >= WIRE_HEADER_SIZE + payload_length` (line 246); returns
       `ERR_INVALID` if violated.
     - `memcpy(env.payload, &buf[offset], payload_length)` (line 252) if `payload_length > 0`.
     - Asserts `envelope_valid(env)` as postcondition (line 256). Returns `OK`.
   - If `deserialize` fails (TcpBackend.cpp:239): logs `WARNING_LO "Deserialize failed: %u"`,
     returns `res`. Return code discarded by `poll_clients_once()`.
   - If `deserialize` succeeds: calls `m_recv_queue.push(env)` (RingBuffer.hpp:98):
     - Loads `m_tail` with relaxed ordering (line 101), loads `m_head` with acquire
       ordering (line 102). Computes `cnt = t - h`.
     - Asserts `cnt <= MSG_RING_CAPACITY` (line 104).
     - If `cnt >= MSG_RING_CAPACITY (64)`: returns `ERR_FULL`.
     - Otherwise: `envelope_copy(m_buf[t & RING_MASK], env)` (line 110); release store of
       `m_tail = t + 1` (line 113); asserts postcondition (line 115); returns `OK`.
   - If `push` returns `ERR_FULL` (TcpBackend.cpp:246–247): logs `WARNING_HI "Recv queue full;
     dropping message"`. `recv_from_client` still returns `OK` (line 251) — the frame was fully
     read from the socket, but the message was dropped.
   - Asserts `out_len <= SOCKET_RECV_BUF_BYTES` (line 250). Returns `OK`.

   **5c. Back in `receive_message()` after `poll_clients_once()`** (line 406):
   `m_recv_queue.pop(envelope)` is called. If `OK`: returns to caller.

   **5d. If queue still empty** (line 411): `timestamp_now_us()` is called to get `now_us`.

   **5e. `flush_delayed_to_queue(now_us)`** (TcpBackend.cpp:306) is called (line 412):
   - Assertions: `now_us > 0ULL` (line 308) and `m_open` (line 309).
   - Stack-allocates `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` (line 311).
   - Calls `ImpairmentEngine::collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`
     (ImpairmentEngine.cpp:216): scans `m_delay_buf[0..31]` for slots where `active == true`
     and `release_us <= now_us`; calls `envelope_copy()` for each; marks slot inactive;
     decrements `m_delay_count`. Returns count.
   - Loop (line 316): `for (i = 0; i < count; ++i)`:
     - Asserts `i < IMPAIR_DELAY_BUF_SIZE` (line 317).
     - `(void)m_recv_queue.push(delayed[i])` — return value discarded; `ERR_FULL` silently
       ignored (line 318).

   **5f.** (line 414): `m_recv_queue.pop(envelope)` is called again. If `OK`: returns to caller.

   **5g.** End of outer loop body. Increments `attempt` and repeats from step 5a.

6. All `poll_count` attempts exhausted. Returns `Result::ERR_TIMEOUT` (line 420).

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::receive_message(envelope, timeout_ms)  [TcpBackend.cpp:386]
 ├── RingBuffer::pop(envelope)                      [RingBuffer.hpp:131]  — fast path
 │    └── envelope_copy()                           [MessageEnvelope.hpp:56]
 └── (loop 0..poll_count-1):
      ├── poll_clients_once(100U)                   [TcpBackend.cpp:326]
      │    ├── accept_clients()                     [TcpBackend.cpp:167]  (server mode only)
      │    │    └── socket_accept(m_listen_fd)      [SocketUtils.cpp:252]
      │    │         └── accept(fd, nullptr, nullptr) [POSIX]
      │    └── (loop 0..MAX_TCP_CONNECTIONS-1):
      │         └── recv_from_client(fd, 100U)      [TcpBackend.cpp:224]
      │              ├── tcp_recv_frame(fd, m_wire_buf, 8192, 100U, &len) [SocketUtils.cpp:431]
      │              │    ├── socket_recv_exact(fd, header, 4U, 100U)  [SocketUtils.cpp:339]
      │              │    │    └── poll(POLLIN) + recv() loop           [POSIX]
      │              │    └── socket_recv_exact(fd, buf, frame_len, 100U) [SocketUtils.cpp:339]
      │              │         └── poll(POLLIN) + recv() loop           [POSIX]
      │              ├── (on failure): remove_client_fd(fd)             [TcpBackend.cpp:199]
      │              │    └── socket_close(fd)                          [SocketUtils.cpp:274]
      │              │         └── close(fd)                            [POSIX]
      │              ├── Serializer::deserialize(m_wire_buf, len, env)  [Serializer.cpp:175]
      │              │    ├── envelope_init(env)                         [MessageEnvelope.hpp:47]
      │              │    ├── read_u8() × 4                              [Serializer.cpp, internal]
      │              │    ├── read_u64() × 3                             [Serializer.cpp, internal]
      │              │    ├── read_u32() × 4                             [Serializer.cpp, internal]
      │              │    ├── memcpy(env.payload, &buf[44], len)         [<cstring>]
      │              │    └── envelope_valid(env)                        [MessageEnvelope.hpp:63]
      │              └── RingBuffer::push(env)                           [RingBuffer.hpp:98]
      │                   └── envelope_copy()                            [MessageEnvelope.hpp:56]
      ├── RingBuffer::pop(envelope)                 [RingBuffer.hpp:131]
      ├── timestamp_now_us()                        [Timestamp.hpp:31]
      │    └── clock_gettime(CLOCK_MONOTONIC)        [POSIX]
      ├── flush_delayed_to_queue(now_us)            [TcpBackend.cpp:306]
      │    ├── ImpairmentEngine::collect_deliverable(now_us, delayed[], 32) [ImpairmentEngine.cpp:216]
      │    │    └── envelope_copy() × (0..32)       [MessageEnvelope.hpp:56]
      │    └── RingBuffer::push(delayed[i]) × (0..32) [RingBuffer.hpp:98]
      └── RingBuffer::pop(envelope)                 [RingBuffer.hpp:131]
```

---

## 5. Key Components Involved

### `TcpBackend::receive_message()` (TcpBackend.cpp:386)
Outer orchestration: fast-path queue check, bounded poll loop, delegates to `poll_clients_once()`
for socket I/O and `flush_delayed_to_queue()` for impairment delivery. Controls all timeout logic.

### `TcpBackend::poll_clients_once()` (TcpBackend.cpp:326)
Private helper. Optionally accepts a new client (server mode), then calls `recv_from_client()` for
each active fd. Returns no error code; failures are handled internally.

### `TcpBackend::recv_from_client()` (TcpBackend.cpp:224)
Private helper. Reads exactly one TCP frame from a single client fd, deserializes it, and pushes it
to `m_recv_queue`. If the frame read fails, disconnects and removes the client.

### `TcpBackend::remove_client_fd()` (TcpBackend.cpp:199)
Private helper. Closes the socket, nulls the array slot, left-shifts remaining entries to compact
the array, and decrements `m_client_count`.

### `TcpBackend::flush_delayed_to_queue()` (TcpBackend.cpp:306)
Private helper. Drains any matured delay-buffer entries from the impairment engine and pushes them
directly to `m_recv_queue`. Used only on the receive path.

### `tcp_recv_frame()` (SocketUtils.cpp:431)
Reads the 4-byte big-endian length prefix, validates the frame length against
`WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 4140U`, then reads exactly `frame_len` payload bytes.

### `socket_recv_exact()` (SocketUtils.cpp:339)
Lowest-level receive loop: calls `poll(POLLIN)` then `recv()` per iteration until `received == len`
or an error occurs. The loop is bounded by `len` bytes.

### `Serializer::deserialize()` (Serializer.cpp:175)
Converts the wire bytes in `m_wire_buf` back into a `MessageEnvelope`. Validates both padding
bytes, validates `payload_length`, and calls `envelope_valid()` as a postcondition assertion.

### `RingBuffer` (RingBuffer.hpp)
SPSC queue holding up to `MSG_RING_CAPACITY = 64` `MessageEnvelope` objects. `push()` uses
acquire/release atomics on `m_tail`; `pop()` uses acquire/release atomics on `m_head`. No mutex.

### `ImpairmentEngine::collect_deliverable()` (ImpairmentEngine.cpp:216)
Scans the 32-slot `m_delay_buf`; collects entries whose `release_us <= now_us`. Called on both the
send path (`flush_delayed_to_clients()`) and the receive path (`flush_delayed_to_queue()`).

---

## 6. Branching Logic / Decision Points

| Condition | Outcome | Next step |
|-----------|---------|-----------|
| `m_recv_queue.pop()` at entry returns `OK` | Message already queued | Return to caller immediately |
| `timeout_ms == 0` | `poll_count == 0`; loop does not execute | Return `ERR_TIMEOUT` |
| `timeout_ms > 5000U` | `poll_count` capped at 50 | Loop runs at most 50 iterations |
| `m_is_server == true` in `poll_clients_once()` | `accept_clients()` called | May add a new client fd |
| `m_client_count >= MAX_TCP_CONNECTIONS` in `accept_clients()` | Skip `accept()` | Return `OK` |
| `socket_accept()` returns `< 0` | No pending connection | Return `OK` (not an error) |
| `m_client_fds[i] < 0` in inner loop | Skip slot | Next index |
| `tcp_recv_frame()` returns false | Frame read failed | `remove_client_fd()`; return `ERR_IO` (discarded by caller) |
| `poll()` returns `<= 0` in `socket_recv_exact()` | Timeout (100ms) | Log `WARNING_HI`; return false |
| `recv()` returns `< 0` | Socket error | Log `WARNING_HI`; return false |
| `recv()` returns `0` | Peer closed connection | Log `WARNING_HI "socket closed"`; return false |
| `frame_len > 4140 or > 8192` in `tcp_recv_frame()` | Oversized frame | Log `WARNING_HI`; return false |
| `padding1 != 0` or `padding2 != 0` in `deserialize()` | Corrupt header | Return `ERR_INVALID` |
| `payload_length > MSG_MAX_PAYLOAD_BYTES` in `deserialize()` | Invalid length | Return `ERR_INVALID` |
| `buf_len < total_size` in `deserialize()` | Buffer underrun | Return `ERR_INVALID` |
| `deserialize()` returns non-OK | Frame deserialization failed | Log `WARNING_LO`; frame consumed from socket but discarded |
| `m_recv_queue.push()` returns `ERR_FULL` | Ring buffer full | Log `WARNING_HI`; deserialized message dropped; `recv_from_client()` returns `OK` |
| `m_recv_queue.pop()` after `poll_clients_once()` returns `OK` | Message found | Return to caller |
| `collect_deliverable()` returns 0 | No delayed messages ready | Loop in `flush_delayed_to_queue()` does not execute |
| `m_recv_queue.push(delayed[i])` returns `ERR_FULL` | Ring full | Silent; `(void)` cast; no log |
| `m_recv_queue.pop()` after `flush_delayed_to_queue()` returns `OK` | Delayed message delivered | Return to caller |
| All `poll_count` attempts exhausted | No message received | Return `ERR_TIMEOUT` |

---

## 7. Concurrency / Threading Behavior

`TcpBackend` has no internal threads. The entire `receive_message()` call executes on the calling
thread.

`RingBuffer` is SPSC (single-producer, single-consumer). In this flow, both the producer role
(`recv_from_client()` → `push()`) and the consumer role (`receive_message()` → `pop()`) are
executed from the **same thread** within the same call. The acquire/release atomic ordering on
`m_head` and `m_tail` is semantically correct for this single-threaded use — the ordering ensures
that `push()`'s `envelope_copy()` completes before `pop()`'s `envelope_copy()` observes the
release on `m_tail`. No data race occurs because access is strictly sequential.

If `send_message()` is called from a different thread concurrently with `receive_message()`, both
would call `collect_deliverable()` on the same `ImpairmentEngine` instance. `m_delay_buf`,
`m_delay_count`, `m_partition_active`, and `m_prng` have no synchronization — this would be a
data race.

`m_wire_buf` is written by `tcp_recv_frame()` for each client and immediately read by
`Serializer::deserialize()` before moving to the next client. This is safe in single-threaded
sequential execution.

`poll(POLLIN)` in `socket_recv_exact()` blocks the calling thread for up to 100ms per call. For a
full iteration of `poll_clients_once(100U)` with `MAX_TCP_CONNECTIONS = 8` clients all idle, the
worst-case per-attempt blocking is `8 × 100ms = 800ms`. For `poll_count = 50`, the
worst-case total blocking is `50 × 800ms = 40 seconds`, even when `timeout_ms` was 5000ms.

---

## 8. Memory & Ownership Semantics

### Stack allocations in this flow
- `poll_count` (uint32_t), `now_us` (uint64_t): locals in `receive_message()`.
- `out_len` (uint32_t), `env` (MessageEnvelope): locals in `recv_from_client()`.
  `env` holds the deserialized envelope before being copied to the ring; it is on the stack
  and freed when `recv_from_client()` returns.
- `header[4U]`: local in `tcp_recv_frame()` (SocketUtils.cpp:441); 4-byte stack array; ephemeral.
- `delayed[IMPAIR_DELAY_BUF_SIZE]` (32 × `MessageEnvelope`): stack-allocated in
  `flush_delayed_to_queue()` (TcpBackend.cpp:311). With `payload[4096]` inline
  (MessageEnvelope.hpp:38), each `MessageEnvelope` is ~4140 bytes. Total: approximately
  **132 KB on the stack** per `flush_delayed_to_queue()` call — called once per outer loop
  iteration (up to 50 times). The same stack frame is reused each time (not accumulated),
  but this single allocation alone risks a stack overflow on constrained targets (see Risk 2).

### No heap allocation
No `malloc` or `new` is used anywhere in this flow.

### Object lifetimes and ownership
- `m_wire_buf[8192]`: owned by `TcpBackend`; written by `tcp_recv_frame()` and read by
  `Serializer::deserialize()` before the next client's `recv_from_client()` call. Contents
  are the last received frame after `receive_message()` returns.
- `m_recv_queue` (RingBuffer): inline member of `TcpBackend`; holds up to 64 `MessageEnvelope`
  copies. `push()` stores a deep copy; `pop()` deep-copies out.
- `m_delay_buf[32]` (in `ImpairmentEngine`): owned by `m_impairment`; entries created by
  `process_outbound()` on the send path; consumed here by `collect_deliverable()`.
- `envelope` out-parameter: written by `pop()` on `OK` return; indeterminate on `ERR_TIMEOUT`
  (no `envelope_init()` is called at entry to `receive_message()`).
- Client fds in `m_client_fds[]`: owned by `TcpBackend`; `remove_client_fd()` calls `close()`
  and nulls the slot; ownership transfers to the OS on `close()`.

---

## 9. Error Handling Flow

```
receive_message()
  m_open assertion fails          → NEVER_COMPILED_OUT_ASSERT fires; debug abort
  queue empty on entry            → Continue; poll loop entered
  timeout_ms == 0                 → poll_count = 0; return ERR_TIMEOUT immediately
  accept_clients() fails          → (void) cast in poll_clients_once(); silently ignored
  tcp_recv_frame() fails          → WARNING_LO; remove_client_fd(); ERR_IO (discarded)
    poll() timeout 100ms          → WARNING_HI in socket_recv_exact; return false
    recv() error                  → WARNING_HI in socket_recv_exact; return false
    recv() == 0 (peer closed)     → WARNING_HI in socket_recv_exact; return false
    frame_len oversized           → WARNING_HI in tcp_recv_frame; return false
  deserialize() fails             → WARNING_LO; frame consumed from socket; message dropped
    padding non-zero              → ERR_INVALID from deserialize; propagated up
    payload_length > max          → ERR_INVALID from deserialize; propagated up
  push() ERR_FULL                 → WARNING_HI; message dropped; recv_from_client returns OK
  flush_delayed push() ERR_FULL   → (void) cast; silent; no log
  all attempts exhausted          → ERR_TIMEOUT returned to caller
```

Key observations:
- The caller cannot distinguish between "no message arrived" (true timeout) and "messages arrived
  but were all dropped due to deserialize failure or ring-full condition" — both return `ERR_TIMEOUT`
  after all attempts.
- A deserialize failure consumes the frame from the socket (the bytes have been `recv()`'d) but
  does not produce a queued message. The socket fd remains open.
- `recv_from_client()` always returns `OK` on the success path, even when `push()` returned
  `ERR_FULL` (message dropped). The return value from `poll_clients_once()` is also discarded.

---

## 10. External Interactions

### POSIX socket I/O
- `accept(m_listen_fd, nullptr, nullptr)` (SocketUtils.cpp:257): called once per
  `receive_message()` in server mode. May block if the listen socket is in blocking mode
  and no client is pending (see Risk 1).
- `poll({fd, POLLIN}, 1, 100)` (SocketUtils.cpp:357): called before each `recv()` in
  `socket_recv_exact()`. Blocks up to 100ms if no data is available.
- `recv(fd, buf, len, 0)` (SocketUtils.cpp:366): called after each successful `poll()`.
  Returns bytes received (may be less than `len`).
- `close(fd)` (SocketUtils.cpp:274): called via `remove_client_fd()` when a client disconnect
  is detected.

### POSIX clock
- `clock_gettime(CLOCK_MONOTONIC, &ts)` (Timestamp.hpp:31): called once per outer loop
  iteration (line 411) to obtain `now_us` for `flush_delayed_to_queue()`.

---

## 11. State Changes / Side Effects

| State | Modified by | Effect |
|-------|-------------|--------|
| `m_wire_buf[0..frame_len-1]` | `tcp_recv_frame()` via `socket_recv_exact()` | Overwritten with the last received frame's bytes |
| `m_recv_queue.m_tail` | `RingBuffer::push()` (release store) | Tail advances by 1 for each successfully received and deserialized message |
| `m_recv_queue.m_head` | `RingBuffer::pop()` (release store) | Head advances by 1 when a message is returned to the caller |
| `m_client_fds[i]`, `m_client_count` | `remove_client_fd()` | Slot nulled; array compacted; count decremented on disconnect |
| `m_delay_buf[i].active`, `m_delay_count` | `collect_deliverable()` | Active slots cleared; count decremented for each drained message |
| Kernel TCP receive buffer | `recv()` via `socket_recv_exact()` | Bytes consumed from kernel |
| OS file descriptor table | `close()` via `remove_client_fd()` | fd released on disconnect |

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant SocketUtils
    participant Serializer
    participant RingBuffer
    participant ImpairmentEngine
    participant OS

    Caller->>TcpBackend: receive_message(envelope, timeout_ms)
    Note over TcpBackend: assert m_open (line 388)
    TcpBackend->>RingBuffer: pop(envelope)
    alt queue not empty
        RingBuffer-->>TcpBackend: OK + envelope
        TcpBackend-->>Caller: return OK
    end
    Note over TcpBackend: compute poll_count = min(ceil(timeout/100), 50)

    loop for attempt = 0..poll_count-1
        TcpBackend->>TcpBackend: poll_clients_once(100U)
        alt m_is_server
            TcpBackend->>SocketUtils: socket_accept(listen_fd)
            SocketUtils->>OS: accept(fd, nullptr, nullptr)
            OS-->>SocketUtils: client_fd or -1
            SocketUtils-->>TcpBackend: client_fd
        end
        loop for each m_client_fds[i] where fd >= 0
            TcpBackend->>TcpBackend: recv_from_client(fd, 100U)
            TcpBackend->>SocketUtils: tcp_recv_frame(fd, m_wire_buf, 8192, 100, &len)
            SocketUtils->>OS: poll(POLLIN, 100ms) + recv() — 4-byte header
            OS-->>SocketUtils: 4 bytes
            Note over SocketUtils: parse frame_len from header; validate
            SocketUtils->>OS: poll(POLLIN, 100ms) + recv() — frame_len bytes
            OS-->>SocketUtils: frame_len bytes
            SocketUtils-->>TcpBackend: true / false
            alt tcp_recv_frame fails
                TcpBackend->>SocketUtils: socket_close(fd)
                SocketUtils->>OS: close(fd)
                Note over TcpBackend: remove_client_fd(); compact array
            else tcp_recv_frame succeeds
                TcpBackend->>Serializer: deserialize(m_wire_buf, len, env)
                Note over Serializer: envelope_init; read header fields; validate padding; memcpy payload
                Serializer-->>TcpBackend: OK / ERR_INVALID
                alt deserialize OK
                    TcpBackend->>RingBuffer: push(env)
                    Note over RingBuffer: envelope_copy into slot; release store m_tail
                    RingBuffer-->>TcpBackend: OK / ERR_FULL
                end
            end
        end
        TcpBackend->>RingBuffer: pop(envelope)
        alt queue not empty
            RingBuffer-->>TcpBackend: OK + envelope
            TcpBackend-->>Caller: return OK
        end
        TcpBackend->>OS: clock_gettime(CLOCK_MONOTONIC) → now_us
        TcpBackend->>TcpBackend: flush_delayed_to_queue(now_us)
        TcpBackend->>ImpairmentEngine: collect_deliverable(now_us, delayed[], 32)
        ImpairmentEngine-->>TcpBackend: count (0..32)
        loop for each delayed[i]
            TcpBackend->>RingBuffer: push(delayed[i])
        end
        TcpBackend->>RingBuffer: pop(envelope)
        alt queue not empty
            RingBuffer-->>TcpBackend: OK + envelope
            TcpBackend-->>Caller: return OK
        end
    end
    TcpBackend-->>Caller: return ERR_TIMEOUT
```

---

## 13. Initialization vs Runtime Flow

### Initialization (one-time, during `TcpBackend::init()`)
- `m_recv_queue.init()` (RingBuffer.hpp:78): relaxed stores of 0 to `m_head` and `m_tail`.
  `MSG_RING_CAPACITY == 64` and `(64 & 63) == 0` are asserted (power-of-two check, line 81).
- `m_client_fds[0..7]` set to `-1` in constructor (TcpBackend.cpp:34–36).
- `m_impairment.init()` seeds the PRNG and zeros `m_delay_buf` and `m_reorder_buf`.
- `m_wire_buf` is value-initialized to zero in the constructor (`m_wire_buf{}`, line 29).
- `m_open` set to `true` on successful `init()`.

### Runtime (each call to `receive_message()`)
- `poll_count` is recomputed from `timeout_ms` on every call.
- Per-iteration: `poll_clients_once(100U)` reads one frame per client per 100ms window.
- `m_wire_buf` is overwritten per client frame read.
- The RingBuffer `m_tail` advances with each `push()` (from `recv_from_client()` or delayed
  flush); `m_head` advances with each successful `pop()`.
- `now_us` is captured once per outer loop iteration (line 411); the same value drives
  `collect_deliverable()` within that iteration.

---

## 14. Known Risks / Observations

### Risk 1 — `accept()` may block indefinitely in server mode
`socket_accept()` (SocketUtils.cpp:252) calls `accept(fd, nullptr, nullptr)`. The comment at
TcpBackend.cpp:179 refers to "EAGAIN in non-blocking mode", but `socket_set_nonblocking()` is never
called on `m_listen_fd` (this was confirmed for the server setup path in UC_19). If no client is
pending, `accept()` on a blocking socket blocks indefinitely, defeating the timeout logic in
`receive_message()`. This is called once per outer loop iteration.

### Risk 2 — Large stack frame in `flush_delayed_to_queue()`
`MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` at TcpBackend.cpp:311 allocates 32 ×
`sizeof(MessageEnvelope)` on the stack. With `payload[4096]` inline (MessageEnvelope.hpp:38), each
`MessageEnvelope` is ~4140 bytes; the array is approximately **132 KB of stack space**. This is
called inside the outer loop (up to 50 times), though the same stack frame is reused, not
accumulated. On embedded targets with limited stack this single allocation risks overflow.

### Risk 3 — Actual blocking time may far exceed `timeout_ms`
`poll_count = min(ceil(timeout_ms / 100), 50)`. Per iteration, `poll_clients_once(100U)` polls all
8 connected clients sequentially, each with a 100ms `poll()` timeout. Worst-case per-attempt
blocking is `8 × 100ms = 800ms`. For `poll_count = 10` (`timeout_ms = 1000`), worst-case total
blocking is `10 × 800ms = 8 seconds` — 8× the requested timeout. The per-channel
`recv_timeout_ms` from `ChannelConfig` is never consulted; 100ms is hard-coded.

### Risk 4 — Deserialize failure silently consumes the frame
If `Serializer::deserialize()` returns an error, the TCP bytes have already been consumed from the
socket's kernel buffer by `tcp_recv_frame()`. There is no mechanism to "unread" them. The frame is
permanently lost; the connection remains open. Only a `WARNING_LO` log entry indicates this
occurred.

### Risk 5 — Delayed-push `ERR_FULL` silently dropped
In `flush_delayed_to_queue()` (TcpBackend.cpp:318), `(void)m_recv_queue.push(delayed[i])` discards
the return value. If the ring is full, delayed messages are silently dropped with no log. This
contrasts with `recv_from_client()` (line 246–247) which logs `WARNING_HI` for the same condition.

### Risk 6 — Shared delay buffer between send and receive paths
`collect_deliverable()` is called on the receive path via `flush_delayed_to_queue()`. The same
`m_impairment` delay buffer receives entries from `process_outbound()` on the send path (via
`send_message()`). A message queued by `send_message()` with a future `release_us` will be
delivered by the receive path's `collect_deliverable()` when it matures. In a loopback simulation,
this causes locally-sent messages to appear as locally-received messages — likely intentional for
testing, but surprising in production two-node deployments.

### Risk 7 — Concurrent access to `m_impairment` is not safe
`ImpairmentEngine` has no synchronization. If `send_message()` and `receive_message()` are called
from different threads, both `process_outbound()` and `collect_deliverable()` would race on
`m_delay_buf`, `m_delay_count`, `m_prng`, and `m_partition_active`.

### Risk 8 — Client compaction may skip a client in the current poll iteration
`remove_client_fd()` compacts `m_client_fds[]` by left-shifting entries. When called from within
the inner loop (index `i`), the entry at index `i+1` shifts into index `i`. Since the loop
increments `i` after the call, the (newly shifted) entry at index `i` is not processed in the
current iteration. It will be picked up in the next outer attempt.

### Risk 9 — Multiple buffered frames per client — only one read per inner-loop pass
`recv_from_client()` reads exactly one frame per call. If a client has multiple frames buffered in
the kernel, only one is consumed per inner-loop iteration per outer attempt. Additional frames are
delayed by up to `poll_count × 100ms` per extra frame.

---

## 15. Unknowns / Assumptions

All facts in this document are sourced directly from the code at the stated file paths and line
numbers. No assumptions were required.

**Confirmed facts:**
- `MSG_RING_CAPACITY = 64U` (Types.hpp): ring buffer capacity.
- `MAX_TCP_CONNECTIONS = 8U` (Types.hpp): `m_client_fds` array size and inner loop bound.
- `SOCKET_RECV_BUF_BYTES = 8192U` (Types.hpp): `m_wire_buf` capacity; wire buffer for recv.
- `IMPAIR_DELAY_BUF_SIZE = 32U` (Types.hpp): delay buffer size; `delayed[]` stack array size.
- `MSG_MAX_PAYLOAD_BYTES = 4096U` (Types.hpp): payload limit; used in `tcp_recv_frame()` to
  compute `max_frame_size = 44 + 4096 = 4140`.
- `WIRE_HEADER_SIZE = 44U` (Serializer.hpp:47): validated against deserialize field offsets.
- `MessageEnvelope.payload` is `uint8_t payload[4096]` inline (MessageEnvelope.hpp:38).
- `recv_from_client()` always returns `OK` even when `push()` returns `ERR_FULL` (line 251).
- `accept()` is called as `accept(fd, nullptr, nullptr)` with no address output (SocketUtils.cpp:257).
- `poll_clients_once()` discards `recv_from_client()` return values via `(void)` cast (line 338).
- The `poll()` timeout passed to `socket_recv_exact()` is always 100ms, hard-coded in
  `poll_clients_once()` at line 338 (passed as `timeout_ms` parameter which is 100U).
- `flush_delayed_to_queue()` discards `push()` return values via `(void)` cast (line 318).
- `envelope_valid()` at Serializer.cpp:256 is asserted as a postcondition of `deserialize()`;
  if an envelope somehow passes all other checks but fails `envelope_valid()`, the assertion
  fires at runtime.
