# UC_21 — TCP poll and receive — poll_clients_once() called internally by TcpBackend::receive_message()

**HL Group:** System Internal — sub-function of HL-5 (User waits for an incoming message)
**Actor:** System (internal sub-function)
**Requirement traceability:** REQ-4.1.3, REQ-6.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.3.3, REQ-6.3.5, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** The application calls `TcpBackend::receive_message(envelope, timeout_ms)`. The immediate queue check (`m_recv_queue.pop()`) returns empty. The implementation enters a polling loop.

**Goal:** Poll all connected client file descriptors (and optionally the listen fd in server mode) for incoming data, accept any pending new connections, read and deserialize exactly one framed message from each readable client fd, and push the result into the internal receive queue. The application then dequeues one envelope from that queue.

**Success outcome:** At least one framed message is received from a client, deserialized, pushed to `m_recv_queue`, and returned to the application via `m_recv_queue.pop()`. `receive_message()` returns `Result::OK` with a valid `envelope`.

**Error outcomes:**
- `ERR_TIMEOUT` — no message arrived within `timeout_ms`.
- `ERR_IO` from `recv_from_client()` — connection closed by client; client fd removed.
- Deserialization failure from `Serializer::deserialize()` — WARNING_LO logged; message discarded.
- Receive queue full (`m_recv_queue.push()` fails) — WARNING_HI logged; message discarded at the queue level.

---

## 2. Entry Points

```
// src/platform/TcpBackend.cpp
Result TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms);
```

Internal sub-functions:

```
void TcpBackend::poll_clients_once(uint32_t timeout_ms);
Result TcpBackend::accept_clients();
Result TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms);

// via ISocketOps vtable → SocketOpsImpl → SocketUtils.cpp
bool ISocketOps::recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                              uint32_t timeout_ms, uint32_t* out_len);
// resolves to:
bool tcp_recv_frame(int fd, uint8_t* buf, uint32_t buf_cap,
                    uint32_t timeout_ms, uint32_t* out_len);
bool socket_recv_exact(int fd, uint8_t* buf, uint32_t len, uint32_t timeout_ms);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Fast-path check:**

1. `TcpBackend::receive_message(envelope, timeout_ms)` is called.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. `m_recv_queue.pop(envelope)` — attempt to dequeue an already-buffered message.
   - If `OK`: return immediately (fast path; no polling needed).
   - If `ERR_EMPTY`: proceed to polling loop.

**Polling loop setup:**

4. `poll_count = (timeout_ms + 99U) / 100U` — compute number of 100 ms iterations.
5. `if (poll_count > 50U) { poll_count = 50U; }` — cap at 50 (5 seconds total).
6. `NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U)`.

**Polling loop (Power of 10 fixed bound: at most 50 iterations):**

7. For `attempt = 0..poll_count-1`:
   a. `poll_clients_once(100U)` — one 100 ms poll cycle.
   b. `m_recv_queue.pop(envelope)` — check if any message arrived.
   c. If OK: return OK.
   d. `now_us = timestamp_now_us()`.
   e. `flush_delayed_to_queue(now_us)` — flush any delayed inbound messages.
   f. `m_recv_queue.pop(envelope)` — check again after delayed flush.
   g. If OK: return OK.
8. If loop exits without a message: return `ERR_TIMEOUT`.

**Inside `poll_clients_once(100U)`:**

9. `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U)`.
10. Build `pollfd pfds[MAX_POLL_FDS]` on the stack (`MAX_POLL_FDS = MAX_TCP_CONNECTIONS + 1 = 9`).
11. `has_listen = m_is_server && (m_listen_fd >= 0) && (m_client_count < MAX_TCP_CONNECTIONS)`.
12. If `has_listen`: `pfds[0] = {m_listen_fd, POLLIN, 0}`; `nfds = 1`.
13. Loop `i = 0..MAX_TCP_CONNECTIONS-1`: for each `m_client_fds[i] >= 0`, append `{m_client_fds[i], POLLIN, 0}`; `++nfds`.
14. `NEVER_COMPILED_OUT_ASSERT(nfds <= MAX_POLL_FDS)`.
15. If `nfds == 0`: return (nothing to poll).
16. `poll_rc = poll(pfds, nfds, 100)` — POSIX poll; blocks up to 100 ms.
17. `poll_rc <= 0` (timeout or EINTR): return. (No data; outer loop will retry or time out.)
18. If `has_listen && (pfds[0].revents & POLLIN) != 0`: call `accept_clients()` (see UC_19 §3 Phase B for detail).
19. Loop `i = 0..MAX_TCP_CONNECTIONS-1`:
    - `m_client_fds[i] >= 0` — valid fd.
    - Call `recv_from_client(m_client_fds[i], 100U)`.

**Inside `recv_from_client(client_fd, 100)`:**

20. `NEVER_COMPILED_OUT_ASSERT(client_fd >= 0)`, `NEVER_COMPILED_OUT_ASSERT(m_open)`.
21. `out_len = 0U`.
22. `m_sock_ops->recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, 100, &out_len)`.
    - Virtual dispatch to `SocketOpsImpl::recv_frame()` → `tcp_recv_frame()`.
23. Inside `tcp_recv_frame(fd, buf, buf_cap=8192, timeout_ms=100, out_len)`:
    a. `NEVER_COMPILED_OUT_ASSERT(fd >= 0)`, `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)`, `NEVER_COMPILED_OUT_ASSERT(out_len != nullptr)`.
    b. Receive 4-byte header: `socket_recv_exact(fd, header, 4U, timeout_ms)`.
       - `NEVER_COMPILED_OUT_ASSERT(fd >= 0)`, `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(len > 0U)`.
       - `received = 0`. Loop while `received < 4`:
         - `poll(&pfd, 1, timeout_ms)` — wait for readability. If timeout/error: log WARNING_HI; return `false`.
         - `recv(fd, &header[received], 4 - received, 0)` — partial reads handled.
         - If `recv_result < 0`: log WARNING_HI; return `false`.
         - If `recv_result == 0`: log WARNING_HI "socket closed"; return `false`.
         - `received += recv_result`.
       - `NEVER_COMPILED_OUT_ASSERT(received == 4)`. Returns `true`.
    c. Parse frame length (big-endian): `frame_len = (header[0]<<24) | (header[1]<<16) | (header[2]<<8) | header[3]`.
    d. Validate: `max_frame_size = WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 44 + 4096 = 4140`.
       - If `frame_len > max_frame_size || frame_len > buf_cap`: log WARNING_HI; return `false`.
    e. If `frame_len > 0`: `socket_recv_exact(fd, buf, frame_len, timeout_ms)` — read payload bytes.
       - Same partial-read loop as for the header; loops until exactly `frame_len` bytes received.
    f. `*out_len = frame_len`.
    g. `NEVER_COMPILED_OUT_ASSERT(*out_len <= buf_cap)`, `NEVER_COMPILED_OUT_ASSERT(*out_len <= max_frame_size)`. Returns `true`.
24. Back in `recv_from_client()`: if `recv_frame` returns `false`:
    - Log WARNING_LO "recv_frame failed; closing connection".
    - `remove_client_fd(client_fd)` — closes the fd; compacts `m_client_fds[]`; decrements `m_client_count`.
    - Returns `ERR_IO`.
25. If `recv_frame` returns `true`:
    - `MessageEnvelope env` (stack-local).
    - `res = Serializer::deserialize(m_wire_buf, out_len, env)`.
      - Validates header size; reads all fields big-endian; copies payload bytes.
      - Returns OK or ERR_INVALID.
    - If deserialize fails: log WARNING_LO; return the error.
    - `res = m_recv_queue.push(env)`.
      - If push fails (queue full): log WARNING_HI "Recv queue full; dropping message".
    - `NEVER_COMPILED_OUT_ASSERT(out_len <= SOCKET_RECV_BUF_BYTES)`.
    - Returns `Result::OK`.

---

## 4. Call Tree

```
TcpBackend::receive_message()
 ├── RingBuffer::pop()                       [fast path; returns if message queued]
 └── [loop up to 50 times]
      ├── TcpBackend::poll_clients_once()
      │    ├── [build pollfd array]
      │    ├── poll(pfds, nfds, 100ms)        [POSIX]
      │    ├── TcpBackend::accept_clients()   [if listen fd readable; server mode]
      │    │    └── ISocketOps::do_accept()   [accept(); stores new client fd]
      │    └── [for each readable client fd]
      │         └── TcpBackend::recv_from_client()
      │              ├── ISocketOps::recv_frame()    [vtable → SocketOpsImpl]
      │              │    └── tcp_recv_frame()
      │              │         ├── socket_recv_exact(fd, header, 4)
      │              │         │    └── poll(POLLIN) + recv()  [POSIX; loop until 4B]
      │              │         └── socket_recv_exact(fd, buf, frame_len)
      │              │              └── poll(POLLIN) + recv()  [POSIX; loop until N B]
      │              ├── Serializer::deserialize()   [buf → MessageEnvelope]
      │              ├── RingBuffer::push()          [enqueue to m_recv_queue]
      │              └── [on recv_frame failure]:
      │                   └── TcpBackend::remove_client_fd()
      ├── RingBuffer::pop()                   [check after poll]
      ├── timestamp_now_us()
      ├── TcpBackend::flush_delayed_to_queue()
      │    └── ImpairmentEngine::collect_deliverable()
      │         └── [for each deliverable]: RingBuffer::push()
      └── RingBuffer::pop()                   [check after delayed flush]
```

---

## 5. Key Components Involved

- **`TcpBackend::receive_message()`**: Outer control loop with bounded iteration count (max 50 × 100 ms = 5 seconds). Tries the queue first (fast path), polls, then checks the queue again. Also flushes inbound delayed messages via `flush_delayed_to_queue()`.

- **`TcpBackend::poll_clients_once()`**: Builds the `pollfd` set, calls `poll()`, dispatches `accept_clients()` (server) and `recv_from_client()` (all clients). Core of the polling mechanism.

- **`TcpBackend::recv_from_client()`**: Receives exactly one framed message from one client. Calls `recv_frame()`, deserializes, and pushes to `m_recv_queue`. On connection loss, removes the client fd.

- **`tcp_recv_frame()`** (`src/platform/SocketUtils.cpp`): Length-prefixed frame reader. Reads a 4-byte big-endian header, parses frame length, validates bounds, then reads exactly that many payload bytes. Each read step uses `socket_recv_exact()` with a poll timeout.

- **`socket_recv_exact()`** (`src/platform/SocketUtils.cpp`): Partial-read loop. Loops until `len` bytes received, with `poll(POLLIN)` before each `recv()` to enforce the timeout. Handles partial reads and connection closure (recv returns 0).

- **`Serializer::deserialize()`** (`src/core/Serializer.hpp`): Reconstructs a `MessageEnvelope` from the big-endian wire bytes stored in `m_wire_buf`. Validates size constraints and populates all envelope fields.

- **`RingBuffer::push()` / `pop()`** (`src/core/RingBuffer.hpp`): Fixed-capacity ring buffer (capacity `MSG_RING_CAPACITY = 64`) used as the inbound message queue. `push()` fails with ERR_FULL if all 64 slots are occupied.

- **`TcpBackend::remove_client_fd()`**: Removes a disconnected client fd from `m_client_fds[]`, closes the fd, and compacts the array by shifting remaining entries left.

- **`TcpBackend::flush_delayed_to_queue()`**: Drains the ImpairmentEngine delay buffer for inbound impairment (if enabled) and pushes released envelopes into `m_recv_queue`. [ASSUMPTION: in the current `TcpBackend` implementation, `process_inbound()` is not called; `flush_delayed_to_queue()` is used for the outbound delay buffer on the client-side receive path, not a separate inbound reorder buffer.]

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_recv_queue.pop()` on fast path | Return OK immediately | Enter polling loop |
| `poll_count > 50` | Clamp to 50 | Use computed value |
| `nfds == 0` in `poll_clients_once()` | Return immediately | Call `poll()` |
| `poll_rc <= 0` (timeout or error) | Return from `poll_clients_once()` | Dispatch accept/receive |
| `has_listen && pfds[0].revents & POLLIN` | Call `accept_clients()` | Skip accept |
| `m_client_fds[i] >= 0` | Call `recv_from_client(fd, 100)` | Skip slot |
| `recv_frame()` returns `false` | Log WARNING_LO; `remove_client_fd()`; return ERR_IO | Proceed to deserialize |
| `Serializer::deserialize()` returns non-OK | Log WARNING_LO; return error | Push to recv queue |
| `m_recv_queue.push()` fails (queue full) | Log WARNING_HI; continue (message dropped) | Message enqueued |
| `m_recv_queue.pop()` after poll | Return OK | Continue loop |
| `flush_delayed_to_queue()` produces messages | They are pushed to recv queue | No effect |
| `m_recv_queue.pop()` after delayed flush | Return OK | Next loop iteration |
| Loop exhausted (50 iterations) | Return `ERR_TIMEOUT` | (unreachable) |
| `socket_recv_exact()` poll timeout | Log WARNING_HI; return `false` | Call `recv()` |
| `recv()` returns 0 | Log WARNING_HI "socket closed"; return `false` | Advance `received` |
| `recv()` returns -1 | Log WARNING_HI; return `false` | Advance `received` |
| `frame_len > max_frame_size \|\| frame_len > buf_cap` | Log WARNING_HI; return `false` | Read payload |

---

## 7. Concurrency / Threading Behavior

- `receive_message()` runs on the calling thread. All sub-calls are synchronous and sequential.
- `poll()` is a blocking POSIX call; it relinquishes the CPU for up to 100 ms per iteration.
- `m_client_fds[]` and `m_client_count` are accessed by both `recv_from_client()` (via `remove_client_fd()`) and the fd-iteration loop in `poll_clients_once()`. If `recv_from_client()` calls `remove_client_fd()` which compacts the array, and the outer loop in `poll_clients_once()` then tries a now-invalid index, it will see `-1` (compacted) or a different fd.

  Inside `poll_clients_once()`, after `accept_clients()`, the loop `for i = 0..MAX_TCP_CONNECTIONS-1` reads `m_client_fds[i]` and calls `recv_from_client()`. If `recv_from_client()` removes and compacts `m_client_fds[]`, subsequent iterations of this same loop may attempt to receive from the same fd that was just removed (if the compaction shifted it to a lower index that was already visited). [ASSUMPTION: this is a minor correctness concern; the worst outcome is a spurious recv attempt on a closed fd, which returns -1 and is handled.]

- `m_recv_queue` is a `RingBuffer`. [ASSUMPTION: `RingBuffer` is not thread-safe; it is accessed from the same thread as `receive_message()`.]

- No `std::atomic` variables are used in this flow.

---

## 8. Memory & Ownership Semantics

**Stack allocations:**

| Variable | Declared in | Approximate size |
|----------|-------------|-----------------|
| `pfds[MAX_POLL_FDS]` | `poll_clients_once()` | 9 × 8 = 72 bytes |
| `env` (MessageEnvelope) | `recv_from_client()` | ~4120 bytes [ASSUMPTION] |
| `header[4]` | `tcp_recv_frame()` | 4 bytes |
| `pfd` (pollfd) | `socket_recv_exact()` | 8 bytes |
| `delayed[IMPAIR_DELAY_BUF_SIZE]` | `flush_delayed_to_queue()` | ~132 KB [ASSUMPTION] |

**Key member buffers:**

| Member | Owner | Capacity |
|--------|-------|----------|
| `m_wire_buf[SOCKET_RECV_BUF_BYTES]` | `TcpBackend` | 8192 bytes |
| `m_recv_queue` (RingBuffer) | `TcpBackend` | `MSG_RING_CAPACITY = 64` messages |
| `m_client_fds[MAX_TCP_CONNECTIONS]` | `TcpBackend` | 8 ints |

**Ownership:**

- `recv_from_client()` writes to `m_wire_buf` (owned by `TcpBackend`). The local `env` on the stack is a temporary deserialization target; its contents are copied into `m_recv_queue` by `RingBuffer::push()`.
- `remove_client_fd()` closes the fd via `m_sock_ops->do_close()`. After closing, the slot is set to -1. The kernel fd is released.

**Power of 10 Rule 3 confirmation:** No dynamic allocation. All buffers are either fixed member arrays or stack-locals of known bounded size.

---

## 9. Error Handling Flow

| Error | Trigger | State after | Caller effect |
|-------|---------|-------------|---------------|
| `recv_frame()` returns `false` | Poll timeout, `recv()` error, or socket closed | Client fd closed and removed via `remove_client_fd()`; `m_client_count` decremented | `recv_from_client()` returns ERR_IO; `poll_clients_once()` discards the result; loop continues |
| `Serializer::deserialize()` fails | Malformed wire bytes | `m_wire_buf` unchanged; `env` partially populated | Log WARNING_LO; `recv_from_client()` returns error; poll loop continues |
| `m_recv_queue.push()` fails | Ring buffer full (64 messages) | Message deserialized but not queued; lost | Log WARNING_HI; `recv_from_client()` continues; message is dropped |
| `poll()` returns <= 0 | Timeout or EINTR | No state change | `poll_clients_once()` returns; outer loop may retry |
| Frame size validation fails | `frame_len > 4140` | Header bytes consumed from stream; stream is now corrupted | Log WARNING_HI; `tcp_recv_frame()` returns `false` → client fd removed |
| `receive_message()` loop exhausted | No message in `timeout_ms` | No state change | Returns `ERR_TIMEOUT` to application |

---

## 10. External Interactions

**POSIX calls in this flow:**

- `poll(pfds, nfds, 100)` with `POLLIN` — waits up to 100 ms for any fd to become readable. `nfds` is at most `MAX_TCP_CONNECTIONS + 1 = 9`.
- `accept(listen_fd, nullptr, nullptr)` — in `accept_clients()`, non-blocking (EAGAIN if no pending connection).
- `poll(&pfd, 1, timeout_ms)` — inside `socket_recv_exact()` before each `recv()`.
- `recv(fd, &buf[received], remaining, 0)` — partial reads; called inside `socket_recv_exact()` until the exact byte count is satisfied.
- `close(fd)` — via `remove_client_fd()` → `m_sock_ops->do_close()` → `socket_close()`.

**Clock:**

- `timestamp_now_us()` — called once per outer loop iteration to pass to `flush_delayed_to_queue()`. [ASSUMPTION: wraps `clock_gettime(CLOCK_MONOTONIC, ...)`.]

---

## 11. State Changes / Side Effects

**On successful receive and push:**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_wire_buf` | previous contents | Received frame bytes (up to 4140 bytes) |
| `TcpBackend` | `m_recv_queue` | N items | N + 1 items (envelope enqueued) |

**On client disconnect (recv_frame fails):**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_client_fds[i]` | disconnected fd | -1 (closed and zeroed) |
| `TcpBackend` | `m_client_count` | N | N - 1 |
| OS kernel | fd entry | open socket | Released |

**On `flush_delayed_to_queue()`:**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_delay_buf[i].active` | `true` | `false` (if release_us elapsed) |
| `ImpairmentEngine` | `m_delay_count` | N | N - count |
| `TcpBackend` | `m_recv_queue` | N items | N + count items |

---

## 12. Sequence Diagram

```
Application     TcpBackend          ISocketOps(impl)   OS Kernel   RingBuffer
  |                  |                     |               |             |
  |--receive_msg()-->|                     |               |             |
  |                  |--pop()-------------------------------------------->|
  |                  |<--ERR_EMPTY----------------------------------------|
  |                  |                     |               |             |
  |  [attempt=0..49] |                     |               |             |
  |                  |--poll_clients_once(100ms)           |             |
  |                  |  [build pfds]       |               |             |
  |                  |--poll(pfds,nfds,100)->             |               |
  |                  |                     |  (POSIX poll) |             |
  |                  |  POLLIN on client   |               |             |
  |                  |  fd=6               |               |             |
  |                  |--recv_from_client(6,100)            |             |
  |                  |--recv_frame(6,buf,  |               |             |
  |                  |  8192,100,&len)---->|               |             |
  |                  |                     |--socket_recv_ |             |
  |                  |                     |  exact(6,hdr,4,100)        |
  |                  |                     |  poll(POLLIN)-->            |
  |                  |                     |  recv(6,hdr,4)->           |
  |                  |                     |  (4 bytes read)             |
  |                  |                     |--parse frame_len=4100       |
  |                  |                     |--socket_recv_ |             |
  |                  |                     |  exact(6,buf, |             |
  |                  |                     |  4100, 100)   |             |
  |                  |                     |  poll(POLLIN)-->            |
  |                  |                     |  recv(loop)-->             |
  |                  |                     |  (4100B read)               |
  |                  |<--true, len=4100----|               |             |
  |                  |--deserialize(buf,4100,env)          |             |
  |                  |--push(env)---------------------------------->      |
  |                  |  (enqueued)         |               |             |
  |                  |--pop(envelope)--------------------------------------------->|
  |                  |<--OK + envelope----|               |             |
  |<--OK + envelope--|                    |               |             |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**

- `m_recv_queue.init()` is called during `TcpBackend::init()`. Resets the ring buffer to empty state.
- `m_wire_buf` is zero-initialized in the constructor via `m_wire_buf{}`.

**Steady-state runtime:**

- `receive_message()` may be called repeatedly by the application. Each call checks the queue first (O(1)), then polls for up to `timeout_ms`.
- `poll_clients_once()` is called up to 50 times per `receive_message()` invocation, each with a 100 ms wait. In practice the first readable fd causes early return from the loop.
- `flush_delayed_to_queue()` is called once per poll iteration to release any delayed messages from the impairment engine into the receive queue. [ASSUMPTION: in the normal `TcpBackend` receive path, the delay buffer holds only inbound-impaired messages if `process_inbound()` were used; in the current implementation, this appears to be a guard for a future path.]
- The receive queue (`m_recv_queue`, capacity 64) decouples the arrival rate from the application's dequeue rate. If the application does not call `receive_message()` frequently enough, the queue fills and messages are dropped at `push()` time with WARNING_HI.

---

## 14. Known Risks / Observations

- **Stream corruption on frame-size validation failure.** If `tcp_recv_frame()` rejects a frame because `frame_len > max_frame_size`, it has already consumed the 4-byte header from the stream. The payload bytes remain in the kernel buffer. The stream is now misaligned: subsequent reads will interpret the payload bytes as the next length header, producing cascading deserialization failures. The client fd is removed by `recv_from_client()` on the `recv_frame()` failure, which discards the connection entirely. This is the correct recovery but results in connection loss.

- **Note on the `poll_clients_once()` client loop and `remove_client_fd()`.** Inside `poll_clients_once()`, after calling `recv_from_client()` for a client that disconnects, `remove_client_fd()` compacts `m_client_fds[]`. The outer `for i = 0..MAX_TCP_CONNECTIONS-1` loop continues, but the compacted array means some client fds may be visited twice or skipped. The worst outcome is a recv attempt on fd -1 (skipped by the `m_client_fds[i] >= 0` guard) or on a previously-visited fd that shifted down (producing a duplicate recv). The duplicate recv produces a frame-not-ready result (EAGAIN) or an extra message in the queue.

- **`MSG_RING_CAPACITY = 64` hard ceiling.** If messages arrive faster than the application dequeues them, the ring buffer fills and messages are silently dropped (WARNING_HI). There is no backpressure to the sender.

- **`poll()` does not indicate which client sent data.** The fd-iteration loop in `poll_clients_once()` calls `recv_from_client()` for all client fds after a positive `poll()` return, not just the readable ones. `revents` from the `pfds` array are not checked per client fd in the current implementation. [ASSUMPTION: the current code reads from all client fds after any `poll()` return. For fds that are not readable, `tcp_recv_frame()` will call `socket_recv_exact()` which will call `poll()` with `timeout_ms=100` and timeout immediately, returning `false`. This causes a spurious WARNING_HI per poll cycle per non-readable fd.]

- **Bounded receive timeout is 5 seconds max (`poll_count` capped at 50).** Callers requesting `timeout_ms > 5000` will only wait 5 seconds. The cap is a deliberate Power of 10 rule compliance mechanism.

- **Large stack frame in `flush_delayed_to_queue()`.** Same 132 KB concern as UC_15 and UC_20.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The `recv_from_client()` loop in `poll_clients_once()` does not check `pfds[n].revents & POLLIN` per client fd. It calls `recv_from_client()` for every valid fd unconditionally after `poll()` returns a positive count. This causes spurious recv attempts on fds that poll() did not flag as readable. The `socket_recv_exact()` inner `poll()` with 100 ms timeout will then fire for each such fd.
- [ASSUMPTION] `flush_delayed_to_queue()` in the receive path is intended for a scenario where the ImpairmentEngine has inbound-delayed messages (e.g., from a future `process_inbound()` integration). In the current TcpBackend, the delay buffer holds outbound messages; this call appears architectural.
- [ASSUMPTION] `RingBuffer` is a non-thread-safe single-producer single-consumer (or single-threaded) queue. All push and pop calls originate from the same thread.
- [ASSUMPTION] `socket_recv_exact()` returning `false` on `recv_result == 0` (remote close) triggers `recv_from_client()` → `remove_client_fd()`. The TCP FIN has been acknowledged at the OS level; the fd is closed cleanly.
- [ASSUMPTION] `sizeof(MessageEnvelope)` ≈ 4120 bytes. The stack-local `env` in `recv_from_client()` consumes approximately this much stack space per call.
- [ASSUMPTION] `tcp_recv_frame()` validates `frame_len <= WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 4140`. The `m_wire_buf` is 8192 bytes, providing headroom. If `MSG_MAX_PAYLOAD_BYTES` increases beyond `SOCKET_RECV_BUF_BYTES - WIRE_HEADER_SIZE`, the validation guard would allow a write past the end of `m_wire_buf`. These two constants must be kept consistent.
