# UC_06 — TCP inbound deserialisation — recv_from_client() called internally by TcpBackend::receive_message()

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System
**Requirement traceability:** REQ-6.1.5, REQ-6.1.6, REQ-3.2.3

---

## 1. Use Case Overview

- **Trigger:** `TcpBackend::recv_from_client(idx, timeout_ms)` is called internally from `TcpBackend::poll_clients_once()`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Read exactly one length-prefixed TCP frame from client socket at index `idx`, deserialize the wire bytes into a `MessageEnvelope`, apply inbound impairments, and push the result into the inbound `RingBuffer` for later delivery.
- **Success outcome:** One `MessageEnvelope` is pushed into `m_recv_queue`. Returns `Result::OK`.
- **Error outcomes:**
  - `Result::ERR_IO` — `tcp_recv_frame()` fails (connection closed, partial read, POSIX error). Client removed.
  - `Result::ERR_INVALID` — `Serializer::deserialize()` returns non-OK (malformed bytes). Client removed.
  - `Result::ERR_FULL` — `m_recv_queue.push()` returns false (ring buffer at capacity). Client NOT removed; envelope discarded.

**Invoking UC:** UC_21 (`poll_clients_once()`) calls this function for each active client slot with data pending.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (private)
Result TcpBackend::recv_from_client(uint32_t idx, uint32_t timeout_ms);
```

Called internally by the system; never called directly by User code.

---

## 3. End-to-End Control Flow

1. `poll_clients_once()` calls `recv_from_client(idx, 100)` for each client where `::poll()` signaled `POLLIN`.
2. `NEVER_COMPILED_OUT_ASSERT(idx < m_client_count)` — bounds check.
3. `NEVER_COMPILED_OUT_ASSERT(m_client_fds[idx] >= 0)` — fd validity check.
4. **`m_sock_ops->recv_frame(m_client_fds[idx], m_wire_buf, SOCKET_RECV_BUF_BYTES, timeout_ms, &wire_len)`** is called. This dispatches to `tcp_recv_frame()` (`SocketUtils.cpp`):
   a. Read 4-byte big-endian length prefix via `socket_recv_exact(fd, hdr_buf, 4, timeout_ms)` → `::recv()` retry loop.
   b. Decode `payload_len = ntohl(...)`.
   c. Validate `payload_len + 4 <= buf_cap`.
   d. Read `payload_len` more bytes: `socket_recv_exact(fd, buf + 4, payload_len, timeout_ms)`.
   e. Set `*out_len = 4 + payload_len`; return `true`.
5. If `recv_frame` returns false: `Logger::log(WARNING_LO, ...)`, `remove_client(idx)`, return `Result::ERR_IO`.
6. `NEVER_COMPILED_OUT_ASSERT(wire_len >= WIRE_HEADER_SIZE)`.
7. **`Serializer::deserialize(m_wire_buf, wire_len, env)`** (`Serializer.cpp`):
   a. Reads all 44-byte header fields with big-endian `read_u8/u32/u64` helpers.
   b. Validates `env.payload_length <= MSG_MAX_PAYLOAD_BYTES`.
   c. Validates `wire_len == WIRE_HEADER_SIZE + env.payload_length`.
   d. `memcpy(env.payload, buf + WIRE_HEADER_SIZE, payload_length)`.
   e. Returns `Result::OK`.
8. If `deserialize` fails: `Logger::log(WARNING_LO, ...)`, `remove_client(idx)`, return `Result::ERR_INVALID`.
9. **`m_impairment.process_inbound(env, now_us, inbound_delay_buf, &inbound_count)`** (`ImpairmentEngine.cpp`) — applies configured inbound impairments (reorder/partition). With default config, envelope passes straight through.
10. **`flush_delayed_to_queue(now_us)`** — for each deliverable envelope in the inbound delay buffer: `m_recv_queue.push(env)`.
11. **`m_recv_queue.push(env)`** — `RingBuffer::push()` with `memory_order_release` on `m_head`. Returns `true` on success, `false` if full.
12. If `push` returns false: `Logger::log(WARNING_LO, ...)`, return `Result::ERR_FULL`.
13. Returns `Result::OK`.

---

## 4. Call Tree

```
TcpBackend::recv_from_client(idx, timeout_ms)   [TcpBackend.cpp]
 ├── ISocketOps::recv_frame()                   [SocketOpsImpl/ISocketOps.hpp]
 │    └── tcp_recv_frame()                      [SocketUtils.cpp]
 │         └── socket_recv_exact() -> ::recv()  [POSIX]
 ├── Serializer::deserialize()                  [Serializer.cpp]
 ├── ImpairmentEngine::process_inbound()        [ImpairmentEngine.cpp]
 ├── TcpBackend::flush_delayed_to_queue()       [TcpBackend.cpp]
 │    └── RingBuffer::push()                    [RingBuffer.hpp]
 └── [TcpBackend::remove_client(idx) on error]  [TcpBackend.cpp]
```

---

## 5. Key Components Involved

- **`TcpBackend::recv_from_client()`** — Orchestrates the full inbound pipeline for one client and one frame.
- **`tcp_recv_frame()`** — Implements the length-prefix framing protocol: read 4-byte header, validate size, read payload.
- **`Serializer::deserialize()`** — Decodes big-endian wire bytes into `MessageEnvelope`; validates size invariants.
- **`ImpairmentEngine::process_inbound()`** — Applies inbound impairments (reorder, partition). Pass-through when disabled.
- **`RingBuffer::push()`** — SPSC lock-free producer write. Makes the envelope available to `receive_message() -> pop()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `recv_frame()` returns false | `remove_client(idx)`; return `ERR_IO` | Proceed to deserialize |
| `wire_len < WIRE_HEADER_SIZE` | Assert fires | Continue |
| `deserialize()` returns non-OK | `remove_client(idx)`; return `ERR_INVALID` | Proceed to impairment |
| Inbound impairment active, envelope delayed | Buffered in delay buf; not yet pushed | Pushed to ring immediately |
| `m_recv_queue.push()` returns false | Log; return `ERR_FULL` | Return `OK` |

---

## 7. Concurrency / Threading Behavior

- Executes in the application's thread (the thread calling `receive_message()`).
- `RingBuffer::push()` stores `m_head` with `memory_order_release`. The corresponding `pop()` loads `m_head` with `memory_order_acquire`. In a single-threaded TCP backend model, both push and pop run in the same thread so the atomic ordering is a correctness annotation rather than a barrier.
- Not safe to call `recv_from_client()` from multiple threads simultaneously.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) — `TcpBackend` member; reused on every call.
- `RingBuffer::m_buf[MSG_RING_CAPACITY]` — 64 × ~4152-byte `MessageEnvelope` array; owned by `TcpBackend::m_recv_queue`. No heap allocation.
- `env` — stack-allocated `MessageEnvelope` inside `recv_from_client()` for intermediate use.
- Power of 10 Rule 3: no heap allocation on this path.

---

## 9. Error Handling Flow

- **`ERR_IO`:** `remove_client(idx)` compacts the client array (last slot moves to `idx`, `m_client_count` decremented). Socket FD is closed. State consistent for remaining clients.
- **`ERR_INVALID`:** Same client removal. Wire format violation is not recoverable.
- **`ERR_FULL`:** Client remains connected; incoming envelope is discarded. Application must drain `receive_message()` faster to prevent continued loss.

---

## 10. External Interactions

- **POSIX `::recv()`:** Called via `socket_recv_exact()` on `m_client_fds[idx]`. Inbound direction. May block up to `timeout_ms` per phase.
- **`stderr`:** `Logger::log()` on error conditions.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_wire_buf` | stale | overwritten with inbound frame |
| `RingBuffer` | `m_buf[m_head % CAPACITY]` | old slot | new `MessageEnvelope` |
| `RingBuffer` | `m_head` (atomic) | H | H+1 (release store) |
| `TcpBackend` | `m_client_fds[idx]` (on error) | valid fd | closed; slot compacted |
| `TcpBackend` | `m_client_count` (on error) | N | N-1 |

---

## 12. Sequence Diagram

```
TcpBackend::poll_clients_once()
  -> TcpBackend::recv_from_client(idx, 100)
       -> ISocketOps::recv_frame()
            -> tcp_recv_frame()
                 -> socket_recv_exact() -> ::recv()  [4-byte length header]
                 -> socket_recv_exact() -> ::recv()  [payload bytes]
            <- wire_len, m_wire_buf populated
       -> Serializer::deserialize(m_wire_buf, wire_len, env)
            <- Result::OK; env populated
       -> ImpairmentEngine::process_inbound(env, ...)  [pass-through if disabled]
       -> flush_delayed_to_queue(now_us)
            -> RingBuffer::push(env)                   [atomic m_head release store]
       <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TcpBackend::init()` called; `m_client_fds[idx]` is a valid open socket.
- `RingBuffer::init()` called; `m_head = m_tail = 0`.

**Runtime:**
- Called per-client per-iteration of `poll_clients_once()` when data is available.

---

## 14. Known Risks / Observations

- **`remove_client(idx)` compaction:** The last client is moved to the removed slot. If the polling loop is iterating over indices, the caller must re-check index `idx` after removal.
- **`RingBuffer` full silently discards:** No flow-control back to TCP sender. Received bytes are consumed but the deserialized envelope is thrown away.
- **Blocking `socket_recv_exact()`:** A slow/stalled sender holds the connection open and blocks the polling loop for up to `timeout_ms` per phase. Multiple slow clients can starve the receive path.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `flush_delayed_to_queue()` is called inside `recv_from_client()` after `process_inbound()`. This is consistent with the ImpairmentEngine pattern where `process_inbound` queues into a delay buffer and `flush_delayed_to_queue` transfers due entries to the ring.
- `[ASSUMPTION]` `now_us` used for delay buffer flush is obtained from `timestamp_now_us()` at the top of `TcpBackend::receive_message()` and passed down the call chain.
