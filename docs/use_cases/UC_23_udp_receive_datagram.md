# UC_23 — UDP receive: datagram received via recvfrom(), deserialised, enqueued for delivery

**HL Group:** HL-17 — UDP Plaintext Transport
**Actor:** System (called from DeliveryEngine::receive() or directly by any caller of TransportInterface::receive_message())
**Requirement traceability:** REQ-4.1.3, REQ-6.2.1, REQ-6.2.2, REQ-6.2.4, REQ-3.2.6, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** A caller invokes `UdpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)` on an initialized transport.

**Goal:** Return one valid `MessageEnvelope` to the caller. The function first checks the internal ring buffer, then polls the UDP socket for new datagrams in a bounded loop (up to 50 iterations × 100 ms each = ≤5 seconds). On each iteration it also flushes any delayed messages from the impairment engine whose `release_us` has elapsed.

**Success outcome:** A valid `MessageEnvelope` is written into `envelope` and `Result::OK` is returned.

**Error outcomes:**
- `ERR_TIMEOUT` — no message arrived within the bounded poll window.
- `ERR_EMPTY` — fast-path ring buffer check found nothing (transient; loop not entered on `timeout_ms = 0`).
- Assertion abort — `m_open == false` or `m_fd < 0` at entry.

---

## 2. Entry Points

```cpp
// src/platform/UdpBackend.cpp, line 230
// Declared: src/platform/UdpBackend.hpp:58
Result UdpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms);
// Overrides: TransportInterface::receive_message()
```

Internal helpers entered from `receive_message()`:
```cpp
// src/platform/UdpBackend.cpp, line 175
bool UdpBackend::recv_one_datagram(uint32_t timeout_ms);

// src/platform/UdpBackend.cpp, line 210
void UdpBackend::flush_delayed_to_queue(uint64_t now_us);
```

Precondition assertions at entry of `receive_message()`:
- `NEVER_COMPILED_OUT_ASSERT(m_open)`
- `NEVER_COMPILED_OUT_ASSERT(m_fd >= 0)`

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Entry assertions.** `NEVER_COMPILED_OUT_ASSERT(m_open)` and `NEVER_COMPILED_OUT_ASSERT(m_fd >= 0)`.

2. **Fast-path ring buffer pop.** `m_recv_queue.pop(envelope)`:
   - Loads `m_head` with `memory_order_relaxed`, `m_tail` with `memory_order_acquire`.
   - If `m_tail - m_head == 0`: returns `ERR_EMPTY`; proceed to step 3.
   - Else: `envelope_copy(envelope, m_buf[m_head & RING_MASK])`, stores new `m_head` with `memory_order_release`; returns `OK`.
   - If `OK`: return `OK` immediately (message was pre-queued from a prior poll).

3. **Compute bounded poll count.** `poll_count = (timeout_ms + 99U) / 100U` (ceiling division, converts to 100 ms units). If `poll_count > 50U`, cap at 50. `NEVER_COMPILED_OUT_ASSERT(poll_count <= 50U)` confirms bound.

4. **Bounded poll loop.** For `attempt = 0..poll_count-1` (Power of 10 Rule 2: fixed bound):

   **4a. `recv_one_datagram(100U)` (private helper):**
   - Entry assertions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(m_fd >= 0)`.
   - Declares stack locals: `uint32_t out_len = 0U`, `char src_ip[48]`, `uint16_t src_port = 0U`.
   - `m_sock_ops->recv_from(m_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, 100U, &out_len, src_ip, &src_port)`:
     - Internally: `poll(m_fd, POLLIN, 100)` — blocks up to 100 ms waiting for a datagram.
     - If `poll` returns ≤0 (timeout or error): returns false from `recv_from`.
     - `recvfrom(fd, m_wire_buf, 8192, 0, &src_addr, &src_len)` — reads one UDP datagram.
     - If `recv_result < 0` or `== 0`: returns false from `recv_from`.
     - `inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, 48)` — fills source IP string.
     - Sets `*out_len = (uint32_t)recv_result`, `*src_port = ntohs(src_addr.sin_port)`.
     - Returns true with `out_len` bytes in `m_wire_buf`.
   - If `recv_from` returns false: `recv_one_datagram` returns false; no envelope queued.
   - If true: `Serializer::deserialize(m_wire_buf, out_len, env)`:
     - Validates 44-byte header; reads all fields; validates padding bytes (buf[3] must be 0, buf[40..43] as uint32 must be 0); bounds-checks `payload_length <= 4096`; copies payload via `memcpy`.
     - If non-OK: log `WARNING_LO "Deserialize failed: %u"`; return false.
   - `m_recv_queue.push(env)`:
     - Loads `m_tail` relaxed, `m_head` acquire; checks `t - h < MSG_RING_CAPACITY (64)`.
     - If full: log `WARNING_HI "Recv queue full; dropping datagram from %s:%u"`; push returns `ERR_FULL`. `recv_one_datagram` still returns true (datagram received but envelope dropped).
     - If space: `envelope_copy(m_buf[t & RING_MASK], env)`, stores new `m_tail` with release; returns `OK`.
   - Returns true.

   **4b. Pop ring buffer.** `m_recv_queue.pop(envelope)`. If `OK`: return `OK`.

   **4c. Get current time.** `now_us = timestamp_now_us()`.

   **4d. `flush_delayed_to_queue(now_us)` (private helper):**
   - Entry assertions: `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`, `NEVER_COMPILED_OUT_ASSERT(m_open)`.
   - Declares stack-local `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` (32 slots).
   - `m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`: scans `m_delay_buf[0..31]`; for each active entry where `release_us <= now_us`, copies to `delayed[]`, marks inactive, decrements `m_delay_count`. Returns count (0..32).
   - Bounded loop `i = 0..count-1`: `NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE)`; `(void)m_recv_queue.push(delayed[i])` — `ERR_FULL` result discarded.

   **4e. Pop ring buffer again.** `m_recv_queue.pop(envelope)`. If `OK`: return `OK`. Else: continue to next iteration.

5. **Timeout.** Loop exhausted without a message. Return `Result::ERR_TIMEOUT`. The `envelope` reference is unmodified.

---

## 4. Call Tree

```
UdpBackend::receive_message(envelope, timeout_ms)
 ├── NEVER_COMPILED_OUT_ASSERT(m_open, m_fd >= 0)
 ├── m_recv_queue.pop(envelope)                          [fast path]
 │    ├── m_head.load(relaxed); m_tail.load(acquire)
 │    └── [if non-empty] envelope_copy(); m_head.store(release)
 ├── [compute poll_count = min((timeout_ms+99)/100, 50)]
 └── [loop 0..poll_count-1]
      ├── recv_one_datagram(100U)
      │    ├── NEVER_COMPILED_OUT_ASSERT(m_open, m_fd >= 0)
      │    ├── ISocketOps::recv_from(m_fd, m_wire_buf, 8192, 100, &out_len, src_ip, &src_port)
      │    │    ├── poll(m_fd, POLLIN, 100)              [POSIX syscall]
      │    │    ├── recvfrom(fd, m_wire_buf, 8192, ...)  [POSIX syscall]
      │    │    └── inet_ntop(AF_INET, ...)
      │    ├── Serializer::deserialize(m_wire_buf, out_len, env)
      │    │    ├── envelope_init(env)                   [memset + set INVALID]
      │    │    ├── read_u8()×4, read_u64()×3, read_u32()×3
      │    │    ├── padding validations (buf[3]==0, buf[40..43]==0)
      │    │    ├── payload_length bounds check
      │    │    └── memcpy(env.payload, buf+44, payload_length)
      │    └── m_recv_queue.push(env)
      │         ├── m_tail.load(relaxed); m_head.load(acquire)
      │         ├── envelope_copy(m_buf[t & RING_MASK], env)
      │         └── m_tail.store(t+1, release)
      ├── m_recv_queue.pop(envelope)
      ├── timestamp_now_us()
      │    └── clock_gettime(CLOCK_MONOTONIC, &ts)
      ├── flush_delayed_to_queue(now_us)
      │    ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0, m_open)
      │    ├── ImpairmentEngine::collect_deliverable(now_us, delayed[32], 32)
      │    │    └── [loop 0..31] copy + deactivate if release_us <= now_us
      │    └── [loop 0..count-1] (void)m_recv_queue.push(delayed[i])
      └── m_recv_queue.pop(envelope)
```

---

## 5. Key Components Involved

- **`UdpBackend`** (`src/platform/UdpBackend.cpp/.hpp`): Orchestrator. Owns `m_fd`, `m_wire_buf[8192]`, `m_recv_queue`, `m_impairment`. SC: `receive_message()` (HAZ-004, HAZ-005).
- **`UdpBackend::recv_one_datagram()`**: Private helper. Polls socket once (100 ms), receives one datagram, deserializes, pushes to ring buffer.
- **`UdpBackend::flush_delayed_to_queue()`**: Private helper. Calls `collect_deliverable()` and pushes each deliverable envelope to `m_recv_queue`.
- **`ISocketOps::recv_from()`** (`src/platform/`): Abstraction over `poll()` + `recvfrom()`. Returns datagram bytes and source address. Default: `SocketOpsImpl`.
- **`Serializer::deserialize()`** (`src/core/Serializer.cpp/.hpp`): Reads 44-byte big-endian header; validates padding; bounds-checks `payload_length`; copies payload. SC (HAZ-001, HAZ-005).
- **`RingBuffer` (m_recv_queue)** (`src/core/RingBuffer.hpp`): SPSC lock-free FIFO. Capacity `MSG_RING_CAPACITY = 64`. `push()` is producer; `pop()` is consumer. `std::atomic<uint32_t>` head/tail with acquire/release ordering.
- **`ImpairmentEngine::collect_deliverable()`** (`src/platform/ImpairmentEngine.hpp`): Scans `m_delay_buf[32]` for entries whose `release_us <= now_us`; copies them out. SC (HAZ-004, HAZ-006).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| Fast-path `pop()` returns `OK` | Return `OK` immediately | Enter poll loop |
| `poll()` returns ≤0 (timeout/error) | `recv_from` returns false; no datagram | Proceed to `recvfrom()` |
| `recvfrom()` returns < 0 or 0 | `recv_from` returns false | Proceed to `inet_ntop` |
| `inet_ntop()` returns null | `recv_from` returns false | Set src_ip, out_len; return true |
| `deserialize()` non-OK | Log `WARNING_LO`; `recv_one_datagram` returns false | `push(env)` |
| `push()` returns `ERR_FULL` | Log `WARNING_HI`; envelope dropped; `recv_one_datagram` still returns true | Envelope queued |
| Pop after `recv_one_datagram` returns `OK` | Return `OK` immediately | Continue to timestamp + flush |
| `collect_deliverable` returns count > 0 | Push delayed envelopes to ring buffer | Skip inner loop |
| Push of delayed envelope returns `ERR_FULL` | `(void)` — silently dropped | Envelope queued |
| Pop after flush returns `OK` | Return `OK` | Continue to next iteration |
| Loop exhausted | Return `ERR_TIMEOUT` | — |

---

## 7. Concurrency / Threading Behavior

**Thread context:** `receive_message()` runs entirely in the calling thread. No background threads are created.

**`poll()` blocking:** The only blocking call on this path. Each `recv_from()` call blocks up to 100 ms. The outer loop can block up to `poll_count × 100 ms` (≤5 seconds). A `SIGINT` during `poll()` returns -1 with `errno = EINTR`; `recv_from` treats this as timeout and returns false.

**RingBuffer SPSC contract:**
- Producer role: `recv_one_datagram()` and `flush_delayed_to_queue()` call `m_recv_queue.push()`.
- Consumer role: `receive_message()` calls `m_recv_queue.pop()`.
- In the single-threaded model, both roles run in the same thread. `m_tail` is stored with release by push; loaded with acquire by pop. `m_head` is stored with release by pop; loaded with acquire by push.

**`m_wire_buf`**: Written by `recv_from()` on each datagram and by `send_message()` on the send path. No mutex. Concurrent `send_message()` and `receive_message()` calls on the same instance would race. [ASSUMPTION: single-threaded access per instance.]

**`m_impairment`**: Accessed from both `send_message()` and `receive_message()`. No mutex. Same single-thread assumption.

---

## 8. Memory & Ownership Semantics

- **`m_wire_buf[8192]`**: Inline member. Overwritten on each `recv_from()` call. No heap.
- **`m_recv_queue.m_buf[64]`**: Inline in `RingBuffer`, inline in `UdpBackend`. Each slot is one full `MessageEnvelope` (~4140 bytes). 64 slots ≈ 265 KB embedded.
- **`delayed[32]`** in `flush_delayed_to_queue()`: Stack-local. 32 × ~4140 bytes ≈ 132 KB stack usage.
- **`src_ip[48]`**: Stack-local in `recv_one_datagram()`.
- **`envelope` parameter**: Caller owns. Written via `envelope_copy()` (memcpy of full `MessageEnvelope`) on successful `pop()`. Unmodified on `ERR_TIMEOUT`.
- **`envelope_copy()`** is `memcpy(&dst, &src, sizeof(MessageEnvelope))` — deep copy including inline `payload[4096]`.
- **No heap allocation** anywhere on this path.
- **`UdpBackend::~UdpBackend()`**: Calls `UdpBackend::close()` (qualified, non-virtual) to close `m_fd`.

---

## 9. Error Handling Flow

| Error | Source | Handling |
|-------|--------|----------|
| `poll()` timeout (returns 0) | `ISocketOps::recv_from()` | Returns false; iteration continues |
| `poll()` error (returns -1) | `ISocketOps::recv_from()` | Treated same as timeout; returns false |
| `recvfrom()` returns < 0 | `ISocketOps::recv_from()` | Log `WARNING_LO`; returns false |
| `recvfrom()` returns 0 | `ISocketOps::recv_from()` | Log `WARNING_LO`; returns false |
| `inet_ntop()` returns null | `ISocketOps::recv_from()` | Log `WARNING_LO`; returns false |
| `deserialize()` non-OK | `recv_one_datagram()` | Log `WARNING_LO "Deserialize failed: %u"`; returns false |
| `push()` returns `ERR_FULL` | `recv_one_datagram()` | Log `WARNING_HI "Recv queue full; dropping datagram from %s:%u"`; datagram dropped; `recv_one_datagram` still returns true |
| Delayed `push()` returns `ERR_FULL` | `flush_delayed_to_queue()` | `(void)` cast; silently dropped |
| Loop exhausted | `receive_message()` | Return `ERR_TIMEOUT` |

**Severity mapping:** `WARNING_LO` — localized, recoverable (single datagram lost). `WARNING_HI` — system-wide, recoverable (queue saturated; sender has no backpressure).

---

## 10. External Interactions

- **`poll(int fd, int events=POLLIN, int timeout_ms=100)`**: Called via `ISocketOps::recv_from()`. Blocks up to 100 ms per call.
- **`recvfrom(int fd, void* buf, size_t len, int flags=0, sockaddr* src_addr, socklen_t* src_len)`**: Reads one UDP datagram into `m_wire_buf`. Returns datagram byte count or -1.
- **`inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, 48)`**: Converts source address to string.
- **`clock_gettime(CLOCK_MONOTONIC, &ts)`**: Via `timestamp_now_us()`. Used for delayed-message flush comparison.
- **OS UDP receive buffer**: Kernel buffers incoming datagrams. Datagrams arriving when the kernel buffer is full are silently dropped at the OS level before `recvfrom()` is called.
- No TLS, no TCP, no connection state.

---

## 11. State Changes / Side Effects

| Object | Field | Change |
|--------|-------|--------|
| `UdpBackend` | `m_wire_buf` | Overwritten with raw bytes from `recvfrom()` on each datagram |
| `RingBuffer` | `m_tail` (atomic) | Incremented on each successful `push()` (release store) |
| `RingBuffer` | `m_head` (atomic) | Incremented on each successful `pop()` (release store) |
| `RingBuffer` | `m_buf[tail % 64]` | Written with deserialized `MessageEnvelope` on push |
| `ImpairmentEngine` | `m_delay_buf[i].active` | Set to false when `release_us <= now_us` in `collect_deliverable` |
| `ImpairmentEngine` | `m_delay_count` | Decremented for each delivered delayed message |
| `envelope` (caller) | All fields | Overwritten on `OK`; unmodified on `ERR_TIMEOUT` |
| OS kernel | UDP receive buffer | Datagram consumed by `recvfrom()`; removed from kernel buffer |

---

## 12. Sequence Diagram

```
Caller           UdpBackend         ISocketOps          Serializer       ImpairmentEngine
  |                  |                   |                    |                  |
  |--receive(env)--->|                   |                    |                  |
  |                  |--pop(env)         |                    |                  |
  |                  | [ERR_EMPTY]       |                    |                  |
  |                  | [compute poll_count]                   |                  |
  |                  | [loop attempt 0..N]                    |                  |
  |                  |--recv_from(fd,wire_buf,8192,100,...)-->|                  |
  |                  | poll(fd,POLLIN,100)                    |                  |
  |                  | recvfrom(fd,...)                       |                  |
  |                  |<--true, out_len, src_ip, src_port------|                  |
  |                  |--deserialize(wire_buf, out_len, env)-->|                  |
  |                  |<--Result::OK, env populated-----------  |                  |
  |                  |--push(env) [m_recv_queue]              |                  |
  |                  |--pop(env)  → OK                        |                  |
  |<--Result::OK-----|                   |                    |                  |

Timeout path:
  | [each iteration]
  |                  |--recv_from() → false (poll timeout)    |                  |
  |                  |--pop() → ERR_EMPTY                     |                  |
  |                  |--collect_deliverable(now_us,32)--------|----------------->|
  |                  |<--0 (nothing to flush)-----------------|                  |
  |                  |--pop() → ERR_EMPTY                     |                  |
  | [loop exhausted]
  |<--ERR_TIMEOUT----|
```

---

## 13. Initialization vs Runtime Flow

**Initialization** (`UdpBackend::init()`): Creates and binds UDP socket; calls `m_recv_queue.init()` (resets atomics to 0); initializes impairment engine (PRNG seeded, delay buffer zeroed). Sets `m_open = true`. See UC_22 §13 for full details.

**Runtime** (this use case): No allocation. `m_wire_buf` reused every call. `delayed[32]` is a bounded stack local in `flush_delayed_to_queue()`. Up to `50 × 100ms = 5s` maximum blocking per call. Delayed messages accumulated from prior `send_message()` impairment calls are flushed on each iteration of the receive loop.

---

## 14. Known Risks

- **Risk 1: Delayed message delivery is opportunistic.** Delayed messages are flushed only when `receive_message()` or `send_message()` is called. No background timer exists. Long gaps in calls cause messages to accumulate past their `release_us`.
- **Risk 2: Ring buffer full silently drops datagrams.** When `m_recv_queue` is at capacity (64 messages), newly deserialized envelopes are dropped at `push()` with only a `WARNING_HI` log. No backpressure to the sender.
- **Risk 3: Source address not validated against configured peer.** `recvfrom()` accepts datagrams from any source address. REQ-6.2.4 ("Validate source address") is partially met by wire-format deserialization but not by IP/port source filtering.
- **Risk 4: Delayed flush `push()` result discarded.** `(void)m_recv_queue.push(delayed[i])` in `flush_delayed_to_queue()`. Delayed envelopes dropped when queue is full produce no log.
- **Risk 5: `poll()` interrupted by signal.** `SIGINT` during `poll()` returns -1 (`EINTR`). Treated as timeout; loop continues. The receive path has no stop-flag awareness.
- **Risk 6: Stack pressure in `flush_delayed_to_queue()`.** `delayed[32]` ≈ 132 KB on the stack frame. See docs/STACK_ANALYSIS.md for the current worst-case analysis.
- **Risk 7: `MSG_RING_CAPACITY = 64` is the hard ceiling.** Once 64 messages are pending and the caller is slow to pop, all new datagrams are deserialized and dropped.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `ISocketOps::recv_from()` calls `poll()` with `POLLIN` and a 100 ms timeout, then `recvfrom()` with `flags = 0`, then `inet_ntop()`. Internally returns false for both genuine errors and innocent timeouts; no distinction is made in `recv_one_datagram()`.
- [ASSUMPTION] `Serializer::deserialize()` validates wire format including padding bytes and `payload_length` bounds check before `memcpy`. Confirmed by reading `Serializer.cpp`.
- [ASSUMPTION] `envelope_copy()` is `memcpy(&dst, &src, sizeof(MessageEnvelope))`. Confirmed in `MessageEnvelope.hpp`.
- [ASSUMPTION] `timestamp_now_us()` uses `clock_gettime(CLOCK_MONOTONIC)`. Returns monotonically increasing microseconds. Not affected by NTP.
- [ASSUMPTION] `Logger::log()` is safe to call from the calling thread context.
- [ASSUMPTION] `UdpBackend` is accessed from a single thread at a time. No internal synchronization beyond the SPSC ring buffer atomics.
- [UNKNOWN] Whether `recvfrom()` can return a zero-length UDP datagram in practice. POSIX allows it; the code correctly handles it by returning false from `recv_from`.
