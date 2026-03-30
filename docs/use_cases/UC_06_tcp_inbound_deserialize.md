# UC_06 — TCP Inbound Deserialization

**HL Group:** System Internal — sub-function of HL-5 (receive); invoked inside `TcpBackend::recv_from_client()`
**Actor:** System (internal sub-function) — the User never calls this directly
**Requirement traceability:** REQ-3.2.3, REQ-4.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.3.2

---

## 1. Use Case Overview

UC_06 is a **System Internal** sub-function. The User never calls it directly; it is invisible at the User → System boundary.

**Invoked by:** UC_21 (TCP poll and receive) → `TcpBackend::recv_from_client()` (TcpBackend.cpp:224). That function calls `tcp_recv_frame()` to read raw bytes from the kernel socket buffer, then immediately calls `Serializer::deserialize()` to reconstruct a `MessageEnvelope`. UC_06 documents the deserialization step and its surrounding context within `recv_from_client()`.

**Why it is factored out:** `Serializer::deserialize()` is a reusable static method shared by both the TCP receive path (here) and the UDP receive path (`UdpBackend::recv_one_datagram()`). The deserializer has no knowledge of transport type; it operates only on a byte buffer and a length. Documenting it as a distinct mechanism makes its contract, failure modes, and safety risks (particularly the `NEVER_COMPILED_OUT_ASSERT` postcondition) visible without re-stating them in every transport that uses it.

**Trigger:** `recv_from_client()` has just successfully returned from `tcp_recv_frame()` with `out_len` bytes written into `TcpBackend::m_wire_buf`. Control passes to `Serializer::deserialize(m_wire_buf, out_len, env)`.

**Expected outcome:** `m_wire_buf[0..out_len-1]` is interpreted as a 44-byte big-endian wire header followed by opaque payload bytes. A fully-populated `MessageEnvelope` is produced and pushed to `m_recv_queue` (RingBuffer). The transport layer returns `OK` from `recv_from_client()`.

**Scope:** `TcpBackend::recv_from_client()` (platform layer), `tcp_recv_frame()` / `socket_recv_exact()` (SocketUtils), `Serializer::deserialize()` (core layer), `RingBuffer::push()` (core layer).

---

## 2. Entry Points

| Entry point | File | Line |
|-------------|------|------|
| `TcpBackend::recv_from_client(int, uint32_t)` | `src/platform/TcpBackend.cpp` | 224 |

Called from `TcpBackend::poll_clients_once()` (TcpBackend.cpp:338), which is called from `TcpBackend::receive_message()` (TcpBackend.cpp:404).

Internal sub-entries invoked in sequence within `recv_from_client()`:

| Function | File | Line |
|----------|------|------|
| `tcp_recv_frame()` | `src/platform/SocketUtils.cpp` | 431 |
| `socket_recv_exact()` (×2) | `src/platform/SocketUtils.cpp` | 339 |
| `Serializer::deserialize()` | `src/core/Serializer.cpp` | 175 |
| `envelope_init()` | `src/core/MessageEnvelope.hpp` | 47 |
| `read_u8()` / `read_u32()` / `read_u64()` | `src/core/Serializer.cpp` | 69 / 78 / 94 |
| `envelope_valid()` | `src/core/MessageEnvelope.hpp` | 63 |
| `RingBuffer::push()` | `src/core/RingBuffer.hpp` | 98 |

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase A: Entry into recv_from_client()

A1. `recv_from_client(client_fd, timeout_ms)` (TcpBackend.cpp:224) is called by `poll_clients_once()` with `timeout_ms = 100U`.
A2. Precondition assertions: `NEVER_COMPILED_OUT_ASSERT(client_fd >= 0)` (line 226); `NEVER_COMPILED_OUT_ASSERT(m_open)` (line 227).
A3. `uint32_t out_len = 0U` initialized.

### Phase B: tcp_recv_frame() — read raw bytes from socket

B1. `tcp_recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, timeout_ms, &out_len)` (SocketUtils.cpp:431) is called:
    - Assertions: `fd >= 0` (line 435), `buf != nullptr` (line 436), `buf_cap > 0U` (line 437), `out_len != nullptr` (line 438).

B2. **Read 4-byte length header.** `socket_recv_exact(fd, header, 4U, 100U)` (SocketUtils.cpp:339):
    - Initializes `received = 0`.
    - Loop while `received < 4`:
      - `poll({fd, POLLIN}, 1, 100)` — blocks up to 100ms.
      - If `poll_result <= 0`: log WARNING_HI "recv poll timeout"; return false.
      - `recv(fd, &header[received], remaining, 0)`.
      - If `recv_result < 0`: log WARNING_HI "recv() failed"; return false.
      - If `recv_result == 0`: log WARNING_HI "recv() returned 0 (socket closed)"; return false.
      - `received += recv_result`.
    - Assert `received == 4` (line 385). Returns true.
    - If `socket_recv_exact` returns false: `tcp_recv_frame` logs WARNING_HI "failed to receive header" (line 443); returns false.

B3. **Parse frame_len** (SocketUtils.cpp:449–452): big-endian decode from `header[0..3]` into `frame_len` (uint32_t):
    ```
    frame_len = (header[0]<<24) | (header[1]<<16) | (header[2]<<8) | header[3]
    ```

B4. **Validate frame_len** (SocketUtils.cpp:454–461):
    - `max_frame_size = WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 44 + 4096 = 4140`.
    - If `frame_len > 4140` or `frame_len > buf_cap (8192)`: log WARNING_HI "frame size N exceeds limit 4140"; return false.

B5. **Read payload bytes.** If `frame_len > 0U`: `socket_recv_exact(fd, buf, frame_len, 100U)` — same poll+recv loop until `received == frame_len`. On failure: log WARNING_HI "failed to receive payload (N bytes)"; return false.

B6. `*out_len = frame_len` (line 472). Assert `*out_len <= buf_cap` (line 475) and `*out_len <= max_frame_size` (line 476). Returns true.

B7. If `tcp_recv_frame` returned false (TcpBackend.cpp:230): log WARNING_LO "recv_frame failed; closing connection". Call `remove_client_fd(client_fd)`:
    - Assert `client_fd >= 0` and `m_client_count > 0U`.
    - Find `client_fd` in `m_client_fds[0..7]`; `socket_close(fd)`; set slot to -1; left-shift remaining entries; `--m_client_count`.
    - Assert `m_client_count < MAX_TCP_CONNECTIONS`.
    - `recv_from_client` returns `ERR_IO`.

### Phase C: Serializer::deserialize() — reconstruct envelope

C1. If `tcp_recv_frame` returned true: `Serializer::deserialize(m_wire_buf, out_len, env)` (Serializer.cpp:175) is called with `env` declared as a stack-local `MessageEnvelope` (TcpBackend.cpp:237).

C2. **Precondition assertions** (Serializer.cpp:180–181): `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)`.

C3. **Minimum length guard** (Serializer.cpp:184): if `buf_len < 44U` → return `ERR_INVALID`. At this point `envelope_init()` has NOT been called; `env` is in its uninitialized stack state.

C4. **Zero-fill** (Serializer.cpp:189): `envelope_init(env)` — `memset(&env, 0, sizeof(MessageEnvelope))`; `env.message_type = MessageType::INVALID (255U)`.

C5. **Header field reads** (Serializer.cpp:195–233), sequentially:
    - `env.message_type = static_cast<MessageType>(read_u8(buf, 0))` → offset 1
    - `env.reliability_class = static_cast<ReliabilityClass>(read_u8(buf, 1))` → offset 2
    - `env.priority = read_u8(buf, 2)` → offset 3
    - `padding1 = read_u8(buf, 3)` → offset 4; if `padding1 != 0U` → return `ERR_INVALID`
    - `env.message_id = read_u64(buf, 4)` → offset 12
    - `env.timestamp_us = read_u64(buf, 12)` → offset 20
    - `env.source_id = read_u32(buf, 20)` → offset 24
    - `env.destination_id = read_u32(buf, 24)` → offset 28
    - `env.expiry_time_us = read_u64(buf, 28)` → offset 36
    - `env.payload_length = read_u32(buf, 36)` → offset 40
    - `padding2 = read_u32(buf, 40)` → offset 44; if `padding2 != 0U` → return `ERR_INVALID`
    - `NEVER_COMPILED_OUT_ASSERT(offset == 44U)` — always-active invariant.

C6. **Payload length bounds check** (Serializer.cpp:240): if `env.payload_length > MSG_MAX_PAYLOAD_BYTES (4096U)` → return `ERR_INVALID`.

C7. **Total size check** (Serializer.cpp:246): `total_size = 44U + env.payload_length`. If `buf_len < total_size` → return `ERR_INVALID`.

C8. **Payload copy** (Serializer.cpp:251–253): if `env.payload_length > 0U`: `(void)memcpy(env.payload, &buf[44], env.payload_length)`.

C9. **Post-condition assertion** (Serializer.cpp:256): `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))`. This is always-active; fires unconditionally if `env.message_type == INVALID` or `env.source_id == 0` or `env.payload_length > 4096`. A wire message with `source_id == 0` passes all structural checks but terminates the process here (see Risk 3).

C10. Returns `Result::OK`.

### Phase D: Error handling from deserialize()

D1. If `Serializer::deserialize()` returns non-OK (TcpBackend.cpp:239): log WARNING_LO "Deserialize failed: N"; `recv_from_client` returns the error result. The return value is discarded by `poll_clients_once()` (via `(void)` cast at line 338). The TCP bytes have already been consumed from the kernel socket buffer. The frame is permanently lost; the fd remains open.

### Phase E: RingBuffer::push()

E1. If deserialization succeeds: `m_recv_queue.push(env)` (RingBuffer.hpp:98):
    - Load `m_tail` (relaxed), load `m_head` (acquire). Compute `cnt = t - h`.
    - Assert `cnt <= MSG_RING_CAPACITY (64)`.
    - If `cnt >= 64` → return `ERR_FULL`.
    - `envelope_copy(m_buf[t & RING_MASK], env)` — full `memcpy` of `sizeof(MessageEnvelope)`.
    - Release store `m_tail = t + 1`.
    - Returns `OK`.

E2. If `push()` returns `ERR_FULL` (TcpBackend.cpp:246): log WARNING_HI "Recv queue full; dropping message". `recv_from_client` still returns `OK` — the frame was read successfully; the overflow is a policy decision, not an I/O error.

E3. Assert `out_len <= SOCKET_RECV_BUF_BYTES` (TcpBackend.cpp:250). `recv_from_client` returns `OK`.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::recv_from_client(client_fd, 100U)        [TcpBackend.cpp:224]
 ├── tcp_recv_frame(fd, m_wire_buf, 8192, 100, &len)  [SocketUtils.cpp:431]
 │    ├── socket_recv_exact(fd, header, 4U, 100U)     [SocketUtils.cpp:339]
 │    │    └── poll(POLLIN, 100ms) + recv() loop       [POSIX, bounded by 4 bytes]
 │    ├── [parse frame_len from header[0..3] BE]
 │    ├── [validate frame_len <= 4140 and <= 8192]
 │    └── socket_recv_exact(fd, buf, frame_len, 100U) [SocketUtils.cpp:339]
 │         └── poll(POLLIN, 100ms) + recv() loop       [POSIX, bounded by frame_len]
 ├── (on failure): remove_client_fd(client_fd)        [TcpBackend.cpp:199]
 │    └── socket_close(fd)                            [SocketUtils.cpp:274]
 │         └── close(fd)                              [POSIX]
 ├── Serializer::deserialize(m_wire_buf, len, env)    [Serializer.cpp:175]
 │    ├── envelope_init(env)                          [MessageEnvelope.hpp:47]
 │    │    └── memset(&env, 0, sizeof(MessageEnvelope))
 │    ├── read_u8(buf, 0)   → env.message_type        [Serializer.cpp:69]
 │    ├── read_u8(buf, 1)   → env.reliability_class   [Serializer.cpp:69]
 │    ├── read_u8(buf, 2)   → env.priority            [Serializer.cpp:69]
 │    ├── read_u8(buf, 3)   → padding1 [validate == 0] [Serializer.cpp:69]
 │    ├── read_u64(buf, 4)  → env.message_id          [Serializer.cpp:94]
 │    ├── read_u64(buf, 12) → env.timestamp_us        [Serializer.cpp:94]
 │    ├── read_u32(buf, 20) → env.source_id           [Serializer.cpp:78]
 │    ├── read_u32(buf, 24) → env.destination_id      [Serializer.cpp:78]
 │    ├── read_u64(buf, 28) → env.expiry_time_us      [Serializer.cpp:94]
 │    ├── read_u32(buf, 36) → env.payload_length      [Serializer.cpp:78]
 │    ├── read_u32(buf, 40) → padding2 [validate == 0] [Serializer.cpp:78]
 │    ├── NEVER_COMPILED_OUT_ASSERT(offset == 44U)
 │    ├── [env.payload_length > 4096U] → ERR_INVALID
 │    ├── [buf_len < 44 + payload_length] → ERR_INVALID
 │    ├── memcpy(env.payload, buf+44, payload_length) [if > 0]
 │    └── NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
 └── RingBuffer::push(env)                            [RingBuffer.hpp:98]
      ├── m_tail.load(relaxed), m_head.load(acquire)
      ├── envelope_copy(m_buf[t & RING_MASK], env)    [MessageEnvelope.hpp:56]
      │    └── memcpy(&dst, &src, sizeof(MessageEnvelope))
      └── m_tail.store(t+1, release)
```

---

## 5. Key Components Involved

### `TcpBackend::recv_from_client()` (TcpBackend.cpp:224)
Orchestrates the inbound deserialization pipeline for a single client fd. Handles the connection-loss path (`remove_client_fd`) and the queue-full path (logs but returns OK). Owns `m_wire_buf`, which is the shared receive buffer for all clients.

### `tcp_recv_frame()` (SocketUtils.cpp:431)
Implements TCP framing: reads 4 bytes of big-endian length prefix, validates the frame length against `max_frame_size = 4140`, then reads exactly `frame_len` payload bytes. Uses `socket_recv_exact()` twice — once for the 4-byte header and once for the variable-length payload.

### `socket_recv_exact()` (SocketUtils.cpp:339)
Lowest-level receive primitive. Calls `poll(POLLIN)` before each `recv()` to enforce the per-call timeout (100ms). Loops until `received == len` or an error/timeout/close occurs. Loop bound is `len` bytes (Power of 10 Rule 2).

### `Serializer::deserialize()` (Serializer.cpp:175)
Stateless static method. Reconstructs a `MessageEnvelope` from a big-endian byte buffer using 11 sequential read-helper calls (`read_u8`, `read_u32`, `read_u64`). Validates two padding fields and both length bounds. Post-condition assertion always active: `envelope_valid(env)` must be true.

### `envelope_init()` (MessageEnvelope.hpp:47)
Inline helper. `memset(&env, 0, sizeof(MessageEnvelope))` followed by `env.message_type = INVALID`. Called immediately after the first length guard passes, so all intermediate error returns leave `env` in a known zero-filled state (except for the `buf_len < 44` path where it is not called).

### `RingBuffer::push()` (RingBuffer.hpp:98)
SPSC ring buffer producer path. Uses acquire/release atomics on `m_tail` and `m_head`. Stores a deep copy of `env` via `envelope_copy()`. Returns `ERR_FULL` if the 64-slot queue is exhausted.

### `remove_client_fd()` (TcpBackend.cpp:199)
Closes the socket, nulls the `m_client_fds` slot, left-shifts remaining entries to compact the array, and decrements `m_client_count`. Called only when `tcp_recv_frame()` fails.

---

## 6. Branching Logic / Decision Points

| Condition | Location | Outcome |
|-----------|----------|---------|
| `poll()` returns `<= 0` in `socket_recv_exact()` | SocketUtils.cpp:357 | Log WARNING_HI "recv poll timeout"; `socket_recv_exact` returns false |
| `recv()` returns `< 0` | SocketUtils.cpp:368 | Log WARNING_HI "recv() failed"; return false |
| `recv()` returns `0` (peer closed) | SocketUtils.cpp:374 | Log WARNING_HI "socket closed"; return false |
| `frame_len > 4140` or `frame_len > 8192` | SocketUtils.cpp:456 | Log WARNING_HI "frame size exceeds limit"; return false |
| `tcp_recv_frame()` returns false | TcpBackend.cpp:230 | Log WARNING_LO; `remove_client_fd()`; return `ERR_IO` |
| `buf_len < 44U` in `deserialize()` | Serializer.cpp:184 | Return `ERR_INVALID`; `env` NOT initialized (see Risk 1) |
| `padding1 != 0U` | Serializer.cpp:207 | Return `ERR_INVALID`; `env` partial (message_type/class/priority written) |
| `padding2 != 0U` | Serializer.cpp:232 | Return `ERR_INVALID`; all header fields written; payload not copied |
| `env.payload_length > 4096U` | Serializer.cpp:240 | Return `ERR_INVALID`; header fully written |
| `buf_len < 44U + env.payload_length` | Serializer.cpp:246 | Return `ERR_INVALID`; header fully written; payload not copied |
| `env.payload_length == 0U` | Serializer.cpp:251 | Skip `memcpy`; zero-payload message is valid |
| `!envelope_valid(env)` post-condition | Serializer.cpp:256 | `NEVER_COMPILED_OUT_ASSERT` fires; process terminates unconditionally |
| `deserialize()` returns non-OK | TcpBackend.cpp:239 | Log WARNING_LO; frame consumed; return error (discarded by caller) |
| `push()` returns `ERR_FULL` | TcpBackend.cpp:246 | Log WARNING_HI; deserialized `env` dropped; `recv_from_client` returns OK |

---

## 7. Concurrency / Threading Behavior

`TcpBackend` has no internal threads. The entire `recv_from_client()` call executes on the calling thread.

`RingBuffer` is SPSC. In this flow, the producer (`push()`) and consumer (`pop()` in `receive_message()`) both execute on the same thread in sequential order: `push()` is called inside `recv_from_client()`, and `pop()` is called in the outer `receive_message()` loop body after `poll_clients_once()` returns. The acquire/release ordering on `m_tail` and `m_head` is semantically correct.

`m_wire_buf` is a plain `uint8_t[8192]` member — not atomic, not mutex-protected. The inner loop in `poll_clients_once()` processes clients sequentially; `m_wire_buf` is overwritten for each client before `deserialize()` reads it. This is safe in single-threaded use. Concurrent calls to `recv_from_client()` from different threads would race on `m_wire_buf`.

`socket_recv_exact()` blocks the calling thread for up to 100ms per `poll()` call. For `MAX_TCP_CONNECTIONS = 8` clients all idle, the per-attempt blocking is `8 × 100ms = 800ms`.

`Serializer::deserialize()` is a fully re-entrant static method with no shared state. Multiple threads can call it concurrently with distinct `(buf, env)` pairs without synchronization.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

| Object | Location | Ownership / Lifetime |
|--------|----------|----------------------|
| `m_wire_buf[8192]` | Member of `TcpBackend` | Owned by `TcpBackend`; valid for object lifetime. Overwritten by `tcp_recv_frame()` per client per call. Contents are the last received frame after `recv_from_client` returns. |
| `out_len` | Stack local in `recv_from_client()` | Holds byte count written by `tcp_recv_frame()`. Discarded on return. |
| `header[4U]` | Stack local in `tcp_recv_frame()` | 4-byte ephemeral array for the length prefix. |
| `env` | Stack local in `recv_from_client()` | Populated by `deserialize()`; passed by const-ref to `push()`. Destroyed when `recv_from_client()` returns. Its content is immortalized in `m_buf[slot]` before destruction. |
| `m_recv_queue.m_buf[64]` | Inline member of `RingBuffer` | Owns 64 deep-copied `MessageEnvelope` objects. Written by `push()` (release store), read by `pop()` (acquire load). |
| Client fd in `m_client_fds[i]` | Owned by `TcpBackend` | Closed in `remove_client_fd()` on failure or in `TcpBackend::close()` on teardown. |

**Copy chain per received message:**
1. `socket_recv_exact()` writes raw bytes into `TcpBackend::m_wire_buf`.
2. `Serializer::deserialize()` reads `m_wire_buf` and writes fields into `env` (stack local in `recv_from_client()`).
3. `RingBuffer::push()` calls `envelope_copy()` to deep-copy `env` into `m_buf[slot]` — `sizeof(MessageEnvelope)` ≈ 4140 bytes.
4. Later, `RingBuffer::pop()` calls `envelope_copy()` to deep-copy `m_buf[slot]` into the caller's envelope — a third `sizeof(MessageEnvelope)` copy.

No heap allocation (`new`, `malloc`) occurs anywhere in this path. All buffers are statically sized.

---

## 9. Error Handling Flow

```
recv_from_client()
  tcp_recv_frame() fails:
    poll() timeout (100ms)           → WARNING_HI in socket_recv_exact; return false
    recv() error                     → WARNING_HI in socket_recv_exact; return false
    recv() == 0 (peer closed)        → WARNING_HI in socket_recv_exact; return false
    frame_len oversized              → WARNING_HI in tcp_recv_frame; return false
  → TcpBackend: WARNING_LO; remove_client_fd(); return ERR_IO (discarded by caller)

  deserialize() fails:
    buf_len < 44                     → ERR_INVALID; env NOT initialized (Risk 1)
    padding1 != 0                    → ERR_INVALID; env partial
    padding2 != 0                    → ERR_INVALID; env partial
    payload_length > 4096            → ERR_INVALID; header populated; payload not copied
    buf_len < 44 + payload_length    → ERR_INVALID; header populated; payload not copied
  → TcpBackend: WARNING_LO; return error (discarded by poll_clients_once)
  Note: TCP bytes consumed from kernel; frame permanently lost; fd remains open.

  NEVER_COMPILED_OUT_ASSERT at Serializer.cpp:256:
    source_id == 0 or message_type == INVALID in wire data → process terminates (Risk 3)

  push() ERR_FULL                    → WARNING_HI; deserialized env dropped; OK returned
```

Key observation: `poll_clients_once()` at TcpBackend.cpp:338 discards `recv_from_client()` return values via `(void)` cast. Errors from deserialization and `ERR_IO` from connection loss are both swallowed. The only externally visible signal is the log entry.

---

## 10. External Interactions

### POSIX socket I/O
- `poll({fd, POLLIN}, 1, 100)` (SocketUtils.cpp:357): called before each `recv()` in `socket_recv_exact()`. Blocks up to 100ms if no data is available.
- `recv(fd, buf, len, 0)` (SocketUtils.cpp:366): called after each successful `poll()`. May return fewer bytes than requested.
- `close(fd)` (SocketUtils.cpp:280): called via `remove_client_fd()` on connection loss.

### memcpy (standard library)
- In `envelope_init()`: `memset(&env, 0, sizeof(MessageEnvelope))`.
- In `Serializer::deserialize()`: `memcpy(env.payload, &buf[44], env.payload_length)` for the opaque payload bytes.
- In `envelope_copy()` inside `RingBuffer::push()`: `memcpy(&dst, &src, sizeof(MessageEnvelope))`.

### Logger
`Logger::log()` called at WARNING_LO and WARNING_HI severity for various error conditions. No logging occurs on the happy path inside `Serializer::deserialize()` itself.

---

## 11. State Changes / Side Effects

| State | Modified by | Effect |
|-------|-------------|--------|
| `m_wire_buf[0..frame_len-1]` | `socket_recv_exact()` via `tcp_recv_frame()` | Overwritten with the received frame bytes |
| `m_client_fds[i]`, `m_client_count` | `remove_client_fd()` | Slot nulled; array compacted; count decremented on connection loss |
| `m_recv_queue.m_buf[slot]` | `RingBuffer::push()` (via `envelope_copy`) | Deep copy of deserialized `env` stored |
| `m_recv_queue.m_tail` | `RingBuffer::push()` (release store) | Tail advances by 1 on successful enqueue |
| Kernel TCP receive buffer | `recv()` in `socket_recv_exact()` | `frame_len + 4` bytes consumed from kernel |
| OS file descriptor table | `close()` via `remove_client_fd()` | fd released on connection loss |

`Serializer::deserialize()` has no class-level state. `SocketUtils` functions are stateless free functions. No persistent disk, database, or hardware state is modified.

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant PollOnce as poll_clients_once
    participant RecvClient as recv_from_client
    participant SockUtils as SocketUtils
    participant OS as POSIX kernel
    participant Deser as Serializer::deserialize
    participant Ring as RingBuffer

    PollOnce->>RecvClient: recv_from_client(client_fd, 100U)
    Note over RecvClient: assert fd>=0, m_open

    RecvClient->>SockUtils: tcp_recv_frame(fd, m_wire_buf, 8192, 100, &out_len)
    SockUtils->>OS: poll(POLLIN, 100ms) + recv() — 4 bytes
    OS-->>SockUtils: header[0..3]
    Note over SockUtils: frame_len = BE decode(header); validate <= 4140
    SockUtils->>OS: poll(POLLIN, 100ms) + recv() — frame_len bytes
    OS-->>SockUtils: frame_len bytes into m_wire_buf
    SockUtils-->>RecvClient: true / false

    alt tcp_recv_frame fails
        RecvClient->>SockUtils: socket_close(fd) via remove_client_fd()
        SockUtils->>OS: close(fd)
        RecvClient-->>PollOnce: ERR_IO (discarded via void cast)
    else tcp_recv_frame succeeds
        RecvClient->>Deser: deserialize(m_wire_buf, out_len, env)
        Note over Deser: assert buf!=nullptr; buf_len >= 44
        Deser->>Deser: envelope_init(env) — memset + INVALID
        Deser->>Deser: read_u8 × 3 (type, class, priority)
        Deser->>Deser: read_u8 → padding1; validate == 0
        Deser->>Deser: read_u64 × 3 (msg_id, timestamp, expiry)
        Deser->>Deser: read_u32 × 3 (src_id, dst_id, payload_len)
        Deser->>Deser: read_u32 → padding2; validate == 0
        Deser->>Deser: assert offset == 44
        Deser->>Deser: validate payload_length <= 4096
        Deser->>Deser: validate buf_len >= 44+payload_length
        Deser->>Deser: memcpy(env.payload, buf+44, payload_length)
        Deser->>Deser: NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
        Deser-->>RecvClient: OK / ERR_INVALID

        alt deserialize fails
            RecvClient-->>PollOnce: error (discarded via void cast)
        else deserialize OK
            RecvClient->>Ring: push(env)
            Note over Ring: envelope_copy into m_buf[slot]; release store m_tail
            Ring-->>RecvClient: OK / ERR_FULL
            alt ERR_FULL
                Note over RecvClient: WARNING_HI; env dropped
            end
            RecvClient-->>PollOnce: OK (void cast)
        end
    end
```

---

## 13. Initialization vs Runtime Flow

### Initialization (one-time, during `TcpBackend::init()`)
- `m_recv_queue.init()` (RingBuffer.hpp:78): relaxed stores of 0 to `m_head` and `m_tail`. Asserts `MSG_RING_CAPACITY == 64` and `(64 & 63) == 0` (power-of-two check).
- `m_client_fds[0..7]` set to -1 in constructor (TcpBackend.cpp:34–36).
- `m_wire_buf` value-initialized to zero (`m_wire_buf{}`, TcpBackend.cpp:29).
- `m_open` set to true on successful `init()`.

### Runtime (each call to `recv_from_client()`)
- `m_wire_buf` is overwritten by `tcp_recv_frame()` on every successful call.
- `env` (stack local) is freshly initialized by `envelope_init()` each call.
- `m_recv_queue.m_tail` advances by 1 per successful `push()`.
- `m_client_fds` and `m_client_count` are modified only on connection loss.

---

## 14. Known Risks / Observations

### Risk 1 — `env` uninitialized on the earliest `deserialize()` error path
If `buf_len < 44U` (Serializer.cpp:184), `envelope_init()` has not been called. The `env` local variable in `recv_from_client()` is returned in its uninitialized stack state. `recv_from_client()` then checks `result_ok(res)` (line 239) and returns the error without touching `env`. The `push()` is not called. The caller (`poll_clients_once`) discards the return value. In practice no code reads the invalid `env` in this path, but it is an implicit contract: callers of `deserialize()` must not access `env` when `ERR_INVALID` is returned from the first guard.

### Risk 2 — Deserialize failure silently consumes the TCP frame
If any `ERR_INVALID` condition fires in `deserialize()`, the TCP bytes have already been consumed from the kernel socket buffer by `tcp_recv_frame()`. There is no mechanism to "unread" them. The frame is permanently lost; the connection remains open. Only a WARNING_LO log entry indicates this. No flow-control signal is sent to the peer.

### Risk 3 — Post-condition assertion terminates the process for `source_id == 0`
`NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` at Serializer.cpp:256 fires unconditionally if `env.source_id == 0` (= `NODE_ID_INVALID`) or `env.message_type == INVALID (255U)`. A remotely-sent TCP frame with a zero source_id passes all structural validation but causes process termination rather than returning `ERR_INVALID`. This is a safety-vs-availability trade-off: the design prioritizes detecting malformed frames at the assert level rather than tolerating them.

### Risk 4 — Shared `m_wire_buf` across all clients
`TcpBackend` has one `m_wire_buf[8192]` shared across all client connections. The inner loop in `poll_clients_once()` processes clients sequentially; each call to `recv_from_client()` overwrites the same buffer before moving to the next client. This is safe in single-threaded use but would become a data race if the loop were parallelized.

### Risk 5 — `poll_clients_once()` discards `recv_from_client()` return values
At TcpBackend.cpp:338, `recv_from_client()` is called with `(void)`. Both `ERR_IO` (connection loss) and deserialization errors are swallowed. The outer `receive_message()` loop has no visibility into whether a frame was received and dropped vs. no frame arrived at all. Both cases look identical: the `m_recv_queue.pop()` after `poll_clients_once()` returns `ERR_EMPTY`.

### Risk 6 — `RingBuffer::push()` `ERR_FULL` drops deserialized message silently (from caller's view)
WARNING_HI is logged, but the return value from `push()` is checked and `recv_from_client()` returns `OK` regardless. The outer loop continues without knowing a message was dropped. A sustained ring-full condition produces repeated WARNING_HI entries with no backpressure mechanism.

### Risk 7 — Three full `MessageEnvelope` copies per received message
Each received message is copied three times: (1) wire bytes → `deserialize()` builds `env` on stack; (2) `push()` copies `env` into `m_buf[slot]`; (3) `pop()` copies `m_buf[slot]` into the caller's envelope. Each copy is `sizeof(MessageEnvelope)` ≈ 4140 bytes.

### Risk 8 — Unchecked enum cast in `deserialize()`
`static_cast<MessageType>(read_u8(buf, 0))` produces an out-of-range `MessageType` value if the wire byte is not in `{0, 1, 2, 3, 255}`. The post-condition assertion catches `MessageType::INVALID (255)` but not other out-of-range values. An unrecognized byte produces implementation-defined behavior (C++17 scoped enum with out-of-range value). No range guard exists between the `read_u8` call and the `static_cast`.

---

## 15. Unknowns / Assumptions

All facts below are sourced directly from the code at the stated file paths and line numbers.

**[CONFIRMED]** `WIRE_HEADER_SIZE = 44U` (Serializer.hpp:47). Derivation: 1+1+1+1+8+8+4+4+8+4+4 = 44 bytes.

**[CONFIRMED]** `SOCKET_RECV_BUF_BYTES = 8192U` (Types.hpp): `m_wire_buf` capacity.

**[CONFIRMED]** `MSG_MAX_PAYLOAD_BYTES = 4096U` (Types.hpp): max payload bytes; `max_frame_size = 44 + 4096 = 4140`.

**[CONFIRMED]** `MSG_RING_CAPACITY = 64U` (Types.hpp): RingBuffer slot count.

**[CONFIRMED]** `MAX_TCP_CONNECTIONS = 8U` (Types.hpp): `m_client_fds` array bound.

**[CONFIRMED]** `NODE_ID_INVALID = 0U` (Types.hpp): `envelope_valid()` rejects `source_id == 0`.

**[CONFIRMED]** `envelope_init()` at MessageEnvelope.hpp:47: `memset` + `env.message_type = INVALID`.

**[CONFIRMED]** `envelope_valid()` at MessageEnvelope.hpp:63: checks `message_type != INVALID`, `payload_length <= 4096`, `source_id != 0`.

**[CONFIRMED]** `NEVER_COMPILED_OUT_ASSERT` is always active, independent of `NDEBUG`. Defined in `src/core/Assert.hpp`.

**[CONFIRMED]** `poll_clients_once()` discards `recv_from_client()` return value via `(void)` cast at TcpBackend.cpp:338.

**[CONFIRMED]** `recv_from_client()` returns `OK` even when `push()` returns `ERR_FULL` (TcpBackend.cpp:251).

**[ASSUMPTION]** `Logger::log()` is called from a single thread (no concurrent access to the Logger sink from `TcpBackend`). Logger implementation was not read.

**[ASSUMPTION]** The `recv_from_client()` timeout parameter (100ms) is always the value passed by `poll_clients_once()`; the `ChannelConfig::recv_timeout_ms` field is not consulted here.
