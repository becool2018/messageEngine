# UC_20 — TCP send framed message — send_frame() / send_to_all_clients() called internally by TcpBackend::send_message()

**HL Group:** System Internal — sub-function of HL-1/2/3/4 (all send paths); hidden inside `TcpBackend::send_message()`
**Actor:** System (internal sub-function) — the User never calls framing directly
**Requirement traceability:** REQ-4.1.2, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.3.2, REQ-6.3.3, REQ-6.3.5, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** `TcpBackend::send_message(envelope)` is called by the application. The message passes through the impairment engine (no loss, no partition), is queued in the delay buffer, and `flush_delayed_to_clients()` determines it is immediately deliverable (release_us <= now_us, e.g., zero latency).

**Goal:** Serialize the `MessageEnvelope` into a length-prefixed binary frame and transmit it reliably over all active TCP connections. Framing ensures that the receiver can reassemble the exact byte boundaries of each message over the TCP stream, which provides no inherent message boundaries.

**Success outcome:** All connected client fds receive the 4-byte big-endian length header followed by the serialized envelope payload. `send_message()` returns `Result::OK`.

**Error outcomes:**
- `ERR_IO` from `Serializer::serialize()` if the envelope is invalid or the buffer is too small.
- Per-client send failure (WARNING_LO logged; that client skipped; other clients still receive the message).
- `ERR_FULL` from the impairment engine if the delay buffer is full.
- `ERR_IO` from `process_outbound()` if message is dropped by loss or partition (treated as OK by `send_message()`).

---

## 2. Entry Points

```
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);
```

Internal sub-functions:

```
void TcpBackend::flush_delayed_to_clients(uint64_t now_us);
void TcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len);

// via ISocketOps vtable → SocketOpsImpl → SocketUtils.cpp
bool ISocketOps::send_frame(int fd, const uint8_t* buf, uint32_t len,
                             uint32_t timeout_ms);
// resolves to:
bool tcp_send_frame(int fd, const uint8_t* buf, uint32_t len, uint32_t timeout_ms);
bool socket_send_all(int fd, const uint8_t* buf, uint32_t len, uint32_t timeout_ms);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. `TcpBackend::send_message(envelope)` is called.
2. Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. **Serialize to wire buffer:**
   - `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)`.
   - Inside `Serializer::serialize()` [ASSUMPTION: implementation tracing from header; not read for this UC]:
     - Validates `buf_len >= WIRE_HEADER_SIZE + payload_length`.
     - Writes 44-byte header: `message_type` (1B), `reliability_class` (1B), `priority` (1B), pad (1B), `message_id` (8B BE), `timestamp_us` (8B BE), `source_id` (4B BE), `destination_id` (4B BE), `expiry_time_us` (8B BE), `payload_length` (4B BE), pad (4B).
     - Copies `payload[0..payload_length-1]` after the header.
     - Sets `out_len = WIRE_HEADER_SIZE + payload_length`.
     - Returns `OK` or `ERR_INVALID`.
   - If serialize fails: log WARNING_LO; return the error.
4. **Apply impairment:**
   - `now_us = timestamp_now_us()`.
   - `res = m_impairment.process_outbound(envelope, now_us)`.
   - If `res == ERR_IO` (message dropped by loss or partition): return `Result::OK` (silent drop).
   - If `res != OK && res != ERR_IO`: return `res` (e.g., ERR_FULL).
5. **Client count check:**
   - `m_client_count == 0U` — no connected clients: log WARNING_LO; return `OK` (discard).
6. **Flush deliverable messages:**
   - `flush_delayed_to_clients(now_us)` is called.
7. Inside `flush_delayed_to_clients(now_us)`:
   a. `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`, `NEVER_COMPILED_OUT_ASSERT(m_open)`.
   b. `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` — stack-allocated output buffer.
   c. `count = m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`.
      - Scans `m_delay_buf[]` for entries with `active == true` and `release_us <= now_us`.
      - Copies them to `delayed[]`; marks slots inactive; decrements `m_delay_count`.
      - Returns the number of deliverable messages.
   d. For each `i` in `[0, count)`:
      - `NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE)`.
      - `delayed_len = 0`.
      - `res = Serializer::serialize(delayed[i], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)`.
        - Overwrites `m_wire_buf` with the serialized form of `delayed[i]`.
        - Returns `OK` or `ERR_INVALID`.
      - If serialize fails: `continue` — skip this delayed message silently.
      - `send_to_all_clients(m_wire_buf, delayed_len)`.
8. Inside `send_to_all_clients(buf, len)`:
   a. `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(len > 0U)`.
   b. Power of 10 loop: `for (uint32_t i = 0; i < MAX_TCP_CONNECTIONS; ++i)`.
   c. `m_client_fds[i] < 0` — skip (no client at this slot).
   d. `m_sock_ops->send_frame(m_client_fds[i], buf, len, m_cfg.channels[0U].send_timeout_ms)`.
      - Virtual dispatch to `SocketOpsImpl::send_frame()` → `tcp_send_frame()`.
   e. `tcp_send_frame(fd, buf, len, timeout_ms)`:
      - Constructs 4-byte big-endian length header:
        - `header[0] = (len >> 24) & 0xFF`
        - `header[1] = (len >> 16) & 0xFF`
        - `header[2] = (len >> 8) & 0xFF`
        - `header[3] = len & 0xFF`
      - `socket_send_all(fd, header, 4U, timeout_ms)` — sends the 4-byte header.
      - If header send fails: log WARNING_HI; return `false`.
      - `len > 0U` check: `socket_send_all(fd, buf, len, timeout_ms)` — sends the payload.
      - If payload send fails: log WARNING_HI; return `false`.
      - Returns `true`.
   f. `socket_send_all(fd, buf, len, timeout_ms)`:
      - `NEVER_COMPILED_OUT_ASSERT(fd >= 0)`, `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(len > 0U)`.
      - `sent = 0`. Loop while `sent < len`:
        - `poll(&pfd, 1, timeout_ms)` — check writability. If timeout/error: log WARNING_HI; return `false`.
        - `send(fd, &buf[sent], len - sent, 0)` — partial send allowed.
        - If `send_result < 0`: log WARNING_HI; return `false`.
        - `sent += send_result`.
      - `NEVER_COMPILED_OUT_ASSERT(sent == len)`. Returns `true`.
   g. Back in `send_to_all_clients()`: if `send_frame()` returns `false`: log WARNING_LO "Send frame failed on client %u". Continue to next client.
9. Back in `send_message()`: `NEVER_COMPILED_OUT_ASSERT(wire_len > 0U)`. Returns `Result::OK`.

---

## 4. Call Tree

```
TcpBackend::send_message()
 ├── Serializer::serialize()                   [envelope → m_wire_buf; 44B hdr + payload]
 ├── timestamp_now_us()
 ├── ImpairmentEngine::process_outbound()      [may drop or queue with delay]
 │    ├── is_partition_active()
 │    ├── check_loss()
 │    └── queue_to_delay_buf()
 └── TcpBackend::flush_delayed_to_clients()
      ├── ImpairmentEngine::collect_deliverable()
      └── [for each deliverable message]
           ├── Serializer::serialize()         [delayed[i] → m_wire_buf]
           └── TcpBackend::send_to_all_clients()
                └── [for each client fd >= 0]
                     └── ISocketOps::send_frame()  [vtable → SocketOpsImpl]
                          └── tcp_send_frame()
                               ├── socket_send_all(fd, header, 4)
                               │    └── poll(POLLOUT) + send()  [POSIX; loop until 4B sent]
                               └── socket_send_all(fd, buf, len)
                                    └── poll(POLLOUT) + send()  [POSIX; loop until N B sent]
```

---

## 5. Key Components Involved

- **`TcpBackend::send_message()`**: Orchestrates the full send path: validate → serialize → impair → flush. Entry point for all outbound messages.

- **`Serializer::serialize()`** (`src/core/Serializer.hpp`): Produces a deterministic, endian-safe, length-delimited byte sequence in `m_wire_buf`. Wire format: 44-byte header + payload bytes. Called twice per delayed message (once before queuing, once before transmitting).

- **`TcpBackend::flush_delayed_to_clients()`**: Collects all delay-buffer entries that have elapsed their `release_us`, re-serializes each, and hands them to `send_to_all_clients()`.

- **`TcpBackend::send_to_all_clients()`**: Iterates all active client fds (bounded by `MAX_TCP_CONNECTIONS = 8`) and calls `send_frame()` on each. A per-client failure does not abort delivery to other clients.

- **`tcp_send_frame()`** (`src/platform/SocketUtils.cpp`): Prepends a 4-byte big-endian length header to the serialized payload, then calls `socket_send_all()` twice — first for the header, then for the payload. This is the framing layer that satisfies REQ-6.1.5.

- **`socket_send_all()`** (`src/platform/SocketUtils.cpp`): Handles partial writes. Loops until all `len` bytes are sent, using `poll(POLLOUT)` before each `send()` call to honor the per-channel `send_timeout_ms`. Satisfies REQ-6.1.6 (partial write handling).

- **`ISocketOps`** / **`SocketOpsImpl`**: Virtual dispatch adapter. `send_frame()` in `ISocketOps` maps to `tcp_send_frame()` in `SocketUtils.cpp`.

- **`m_wire_buf[SOCKET_RECV_BUF_BYTES]`**: Shared serialization scratch buffer in `TcpBackend`. Holds at most one serialized frame at a time (8192 bytes; max frame = 44 + 4096 = 4140 bytes, well within capacity).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `Serializer::serialize()` returns non-OK in `send_message()` | Log WARNING_LO; return error | Continue to impairment |
| `process_outbound()` returns `ERR_IO` | Silent drop; return OK | Continue |
| `process_outbound()` returns other non-OK | Return error to caller | Continue |
| `m_client_count == 0` | Log WARNING_LO "No clients"; return OK (discard) | Call `flush_delayed_to_clients()` |
| `collect_deliverable()` returns 0 | Loop body does not execute | Execute serialize + send for each |
| `Serializer::serialize()` returns non-OK in `flush_delayed_to_clients()` | `continue` — skip that delayed message | Call `send_to_all_clients()` |
| `m_client_fds[i] < 0` in `send_to_all_clients()` | Skip slot | Call `send_frame()` on this fd |
| `send_frame()` returns `false` | Log WARNING_LO "Send frame failed"; continue to next client | No action (success) |
| `poll(POLLOUT)` returns <= 0 in `socket_send_all()` | Log WARNING_HI; return `false` | Call `send()` |
| `send()` returns < 0 | Log WARNING_HI; return `false` | Advance `sent` counter; re-poll if needed |
| `sent == len` | Loop exits; return `true` | Continue looping (partial write) |

---

## 7. Concurrency / Threading Behavior

- `send_to_all_clients()` is called synchronously from `flush_delayed_to_clients()` on the thread calling `send_message()`.
- `m_client_fds[]` is read inside `send_to_all_clients()` (looking for valid fds). If a concurrent thread calls `receive_message()` → `recv_from_client()` → `remove_client_fd()` while `send_to_all_clients()` is iterating, a concurrent modification of `m_client_fds[]` could cause a send attempt on a closed or repurposed fd.
- `m_wire_buf` is written by both the initial `Serializer::serialize()` call and the one inside `flush_delayed_to_clients()`. The two calls are sequential on the same thread, so there is no race. However, if a re-entrant call to `send_message()` were possible (e.g., via a signal handler), `m_wire_buf` could be corrupted.

[ASSUMPTION: single-threaded use, or external serialization of all TcpBackend method calls.]

- `socket_send_all()` loop: the `poll()` + `send()` sequence is not atomic. Another thread could close `fd` between `poll()` returning writable and the `send()` call. This would produce `send()` returning -1 with EBADF, which is handled as a WARNING_HI + return false.

---

## 8. Memory & Ownership Semantics

**Stack allocations:**

| Variable | Declared in | Approximate size |
|----------|-------------|-----------------|
| `delayed[IMPAIR_DELAY_BUF_SIZE]` | `flush_delayed_to_clients()` | 32 × ~4120 bytes ≈ 132 KB [ASSUMPTION] |
| `header[4]` | `tcp_send_frame()` | 4 bytes |
| `pfd` (pollfd) | `socket_send_all()` | 8 bytes |

**Key member buffers:**

| Member | Owner | Capacity | Max frame bytes |
|--------|-------|----------|-----------------|
| `m_wire_buf[SOCKET_RECV_BUF_BYTES]` | `TcpBackend` | 8192 bytes | 44 + 4096 = 4140 bytes maximum |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. `delayed[]` is a fixed-size stack array. `m_wire_buf` is a fixed member. The loop in `socket_send_all()` uses no heap.

**Wire frame layout (as produced by `tcp_send_frame()`):**

```
Byte 0     : (frame_len >> 24) & 0xFF
Byte 1     : (frame_len >> 16) & 0xFF
Byte 2     : (frame_len >> 8) & 0xFF
Byte 3     : frame_len & 0xFF
Bytes 4..N : serialized envelope (WIRE_HEADER_SIZE=44 bytes + payload_length bytes)
```

Total on-wire bytes = 4 + WIRE_HEADER_SIZE + payload_length = 4 + 44 + payload_length.

---

## 9. Error Handling Flow

| Error | Location | Behavior | Upstream effect |
|-------|----------|----------|-----------------|
| `Serializer::serialize()` fails | `send_message()` | Log WARNING_LO; return error to application | Application sees non-OK result |
| `process_outbound()` ERR_IO | `send_message()` | Silent drop; return OK | Application sees OK (loss is intentional) |
| `process_outbound()` ERR_FULL | `send_message()` | Return ERR_FULL to application | Application must handle backpressure |
| `m_client_count == 0` | `send_message()` | Log WARNING_LO; return OK | Application sees OK; message discarded |
| `Serializer::serialize()` fails | `flush_delayed_to_clients()` | `continue` — delayed message silently lost | Message is dequeued from delay buffer but never transmitted |
| `send_frame()` fails per client | `send_to_all_clients()` | Log WARNING_LO; continue to next client | That client does not receive the message; others do |
| `poll(POLLOUT)` timeout | `socket_send_all()` | Log WARNING_HI; return `false` | `send_frame()` returns `false` → send_to_all_clients logs WARNING_LO |
| `send()` returns -1 | `socket_send_all()` | Log WARNING_HI; return `false` | Same as above |

---

## 10. External Interactions

**POSIX calls in this flow:**

- `poll(pfd, 1, timeout_ms)` with `POLLOUT` — checks writability of the client fd before each `send()`. Timeout is `m_cfg.channels[0U].send_timeout_ms`. [ASSUMPTION: `send_timeout_ms` defaults to a value set in `TransportConfig`.]
- `send(fd, &buf[sent], remaining, 0)` — sends bytes on the TCP socket. May send fewer than `remaining` bytes (partial write). `socket_send_all()` loops until all bytes are sent or an error/timeout occurs.

The 4-byte header and the payload are sent as two separate `socket_send_all()` calls. The kernel may coalesce them in the TCP send buffer, but the framing protocol requires the receiver to read the header first and then exactly `frame_len` payload bytes.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_wire_buf` | previous contents | Serialized envelope bytes (44B header + payload) |
| `ImpairmentEngine` | `m_delay_buf[i].active` | `true` | `false` (after `collect_deliverable`) |
| `ImpairmentEngine` | `m_delay_count` | N | N - count (where count = deliverable messages) |
| OS kernel | TCP send buffer for each client fd | previous data | New frame bytes enqueued for transmission |
| Network | in-flight bytes | none | 4 + WIRE_HEADER_SIZE + payload_length bytes per client per message |

---

## 12. Sequence Diagram

```
Application      TcpBackend           ImpairmentEngine  ISocketOps(impl)   OS Kernel
  |                   |                      |                 |                |
  |--send_message()-->|                      |                 |                |
  |                   |--serialize(env,      |                 |                |
  |                   |  m_wire_buf)         |                 |                |
  |                   |  wire_len = 44+plen  |                 |                |
  |                   |--timestamp_now_us()  |                 |                |
  |                   |--process_outbound()->|                 |                |
  |                   |  (latency=0; slot    |                 |                |
  |                   |  queued; rel_us=now) |                 |                |
  |                   |<--OK-----------------|                 |                |
  |                   |--flush_delayed_      |                 |                |
  |                   |  to_clients(now_us)  |                 |                |
  |                   |  collect_deliverable |                 |                |
  |                   |  → count=1           |                 |                |
  |                   |  slot freed          |                 |                |
  |                   |--serialize(delayed[0]|                 |                |
  |                   |  m_wire_buf)         |                 |                |
  |                   |--send_to_all_clients(|                 |                |
  |                   |  m_wire_buf, len)    |                 |                |
  |                   |  [for each client fd]|                 |                |
  |                   |--send_frame(fd, buf, |                 |                |
  |                   |  len, timeout)------>|                 |                |
  |                   |                      |--tcp_send_frame()|               |
  |                   |                      |  header[4]=BE(len)               |
  |                   |                      |--socket_send_all(header,4)       |
  |                   |                      |  poll(POLLOUT)-->|               |
  |                   |                      |  send(header,4)->|               |
  |                   |                      |  (sent==4)       |               |
  |                   |                      |--socket_send_all(buf, len)       |
  |                   |                      |  poll(POLLOUT)-->|               |
  |                   |                      |  send(buf,len)-->|               |
  |                   |                      |  (may loop for   |               |
  |                   |                      |  partial writes) |               |
  |                   |                      |--return true---->|               |
  |                   |<--true---------------|                  |               |
  |<--OK--------------|                      |                  |               |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**

- `m_wire_buf` is zero-initialized via `m_wire_buf{}` aggregate initialization in the `TcpBackend` constructor. No initialization of `send_frame` state is required; the framing logic is stateless (header is computed each call).

**Steady-state runtime:**

- Every `send_message()` call re-computes the wire frame from scratch. `m_wire_buf` is overwritten on each call. There is no incremental framing.
- The `socket_send_all()` loop is bounded by `len` (the frame size). For a 4140-byte frame, the worst case is 4140 loop iterations if the kernel accepts exactly 1 byte per `send()`. In practice, TCP sends large chunks; the loop typically completes in one or two iterations.
- `send_timeout_ms` per-channel configuration limits how long the loop blocks in aggregate. A timeout mid-frame leaves the receiver with a partial frame; the receiver's `tcp_recv_frame()` will fail on the subsequent poll timeout and close the connection.

---

## 14. Known Risks / Observations

- **Double serialization per delayed message.** Every message passing through the delay buffer is serialized twice: once in `send_message()` (before impairment, bytes discarded) and once in `flush_delayed_to_clients()` (before actual transmission). This is redundant CPU work.

- **`m_wire_buf` size (8192 bytes).** The maximum frame size is `4 + 44 + 4096 = 4144` bytes, comfortably within 8192. However, `SOCKET_RECV_BUF_BYTES` is used as the capacity for both the serialize buffer and the receive buffer. Changing `MSG_MAX_PAYLOAD_BYTES` without updating `SOCKET_RECV_BUF_BYTES` could cause overflow.

- **Partial-write loop (`socket_send_all()`) has no finite iteration bound.** The loop terminates only when `sent == len`. If `send()` consistently returns `0` (connection closed without -1), `sent` never advances and the loop runs forever. The `send()` returning 0 case is handled for `recv()` but not explicitly for `send()`. [ASSUMPTION: `send()` returning 0 is treated by the OS as a no-op on a closed connection and will eventually return -1 or EPIPE; the `poll(POLLOUT)` step before each `send()` provides the timeout bound.]

- **One-message-at-a-time framing.** `send_to_all_clients()` sends one frame per call. If `collect_deliverable()` returns N > 1 messages (burst release after latency), the outer loop in `flush_delayed_to_clients()` sends N frames serially to all clients. Each frame uses a separate header + payload pair.

- **Per-client send failure is silent to the application.** If client 3 of 5 fails to receive a message, `send_message()` still returns OK. There is no mechanism to report partial delivery failure. Retry at the `ReliabilityClass::RELIABLE_RETRY` level must detect the missing ACK.

- **Large stack frame in `flush_delayed_to_clients()`.** The 132 KB `delayed[]` stack allocation is a concern on constrained platforms (see also UC_15 §14).

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `m_cfg.channels[0U].send_timeout_ms` is the timeout used by `send_to_all_clients()` for all clients regardless of how many channels are configured. No per-client timeout.
- [ASSUMPTION] `Serializer::serialize()` implementation matches the header documentation: 44-byte wire header + raw payload bytes = `WIRE_HEADER_SIZE + payload_length` bytes total.
- [ASSUMPTION] `socket_send_all()` will not loop infinitely because `poll(POLLOUT)` provides a timeout bound on each iteration. A consistent timeout terminates the loop via the `poll_result <= 0` check.
- [ASSUMPTION] The `delayed[]` stack allocation in `flush_delayed_to_clients()` is safe given the platform stack size. On typical POSIX systems (8 MB stack), 132 KB is acceptable.
- [ASSUMPTION] Only `channels[0]` impairment and send timeout are used regardless of the envelope's channel or priority. `TcpBackend` does not implement per-channel routing.
- [ASSUMPTION] The initial `Serializer::serialize()` call in `send_message()` (step 3) is used for envelope validation purposes; the serialized bytes are not directly used in the transmission path when impairment adds latency.
