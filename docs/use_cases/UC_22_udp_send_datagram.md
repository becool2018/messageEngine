# UC_22 — UDP send: envelope serialised, impairments applied, datagram sent via sendto()

**HL Group:** HL-17 — UDP Plaintext Transport
**Actor:** System (called internally from DeliveryEngine::send_via_transport() or directly by any caller of TransportInterface::send_message())
**Requirement traceability:** REQ-4.1.2, REQ-6.2.1, REQ-6.2.3, REQ-6.2.4, REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** A caller invokes `UdpBackend::send_message(const MessageEnvelope& envelope)` with a fully populated, validated envelope on a transport that has been successfully initialized.

**Goal:** Serialize the envelope to a fixed wire buffer, apply any configured outbound impairments (loss, partition, latency/jitter, duplication), transmit the surviving bytes as a single UDP datagram via `sendto()`, and flush any previously delayed datagrams whose `release_us` has elapsed.

**Success outcome:** The datagram is handed to the OS network stack. `Result::OK` is returned to the caller. Any delayed envelopes whose timers have elapsed are also sent in the same call.

**Error outcomes:**
- Silent drop (`Result::OK` returned) — `process_outbound()` returns `ERR_IO` due to loss probability or active partition. The caller is not notified; this is intentional impairment semantics.
- `ERR_INVALID` — `Serializer::serialize()` returns non-OK (envelope invalid or buffer too small). Logged at `WARNING_LO`.
- `ERR_IO` — `ISocketOps::send_to()` fails. Logged at `WARNING_LO` with peer IP:port.
- Assertion abort — `m_open == false`, `m_fd < 0`, or `!envelope_valid(envelope)` at entry. `NEVER_COMPILED_OUT_ASSERT` fires unconditionally; on production builds triggers `IResetHandler::on_fatal_assert()`.

---

## 2. Entry Points

```cpp
// src/platform/UdpBackend.cpp, line 114
// Declared: src/platform/UdpBackend.hpp:56
Result UdpBackend::send_message(const MessageEnvelope& envelope);
// Overrides: TransportInterface::send_message()
```

Typical callers:
- `DeliveryEngine::send_via_transport()` (`src/core/DeliveryEngine.cpp:63`) — primary caller in production flows.
- Test code via `TransportInterface*` interface pointer.

Precondition assertions (fire unconditionally regardless of NDEBUG):
- `NEVER_COMPILED_OUT_ASSERT(m_open)`
- `NEVER_COMPILED_OUT_ASSERT(m_fd >= 0)`
- `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` — checks `message_type != INVALID`, `payload_length <= 4096`, `source_id != 0`

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Entry assertions.** Three `NEVER_COMPILED_OUT_ASSERT` calls fire: `m_open`, `m_fd >= 0`, `envelope_valid(envelope)`.

2. **Serialize to wire buffer.** `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` is called. Writes a 44-byte big-endian header (WIRE_HEADER_SIZE = 44) followed by `envelope.payload_length` bytes of payload into the 8192-byte member buffer `m_wire_buf`. On failure: log `WARNING_LO "Serialize failed"`; return the error code immediately.

3. **Get current time.** `timestamp_now_us()` called inline; calls `clock_gettime(CLOCK_MONOTONIC, &ts)`; returns `uint64_t` microseconds. Result stored in `now_us`.

4. **Outbound impairment.** `m_impairment.process_outbound(envelope, now_us)` called. The ImpairmentEngine applies, in order:
   - If `m_cfg.enabled == false`: calls `queue_to_delay_buf(envelope, now_us)` with `release_us = now_us` (immediate); returns `OK`. If delay buffer full (32 entries), returns `ERR_FULL`.
   - If `m_cfg.enabled == true`:
     - `is_partition_active(now_us)`: if partition active, returns `ERR_IO`.
     - `check_loss()`: draws `m_prng.next_double()`; if less than `loss_probability`, returns `ERR_IO`.
     - Computes `release_us = now_us + fixed_latency_ms*1000 + jitter_offset`; calls `queue_to_delay_buf(envelope, release_us)`. If delay buffer full, returns `ERR_FULL`.
     - `apply_duplication()`: if `duplication_probability > 0.0`, draws PRNG; if fires, calls `queue_to_delay_buf(envelope, release_us + 100)`.
     - Returns `OK`.

5. **Drop check.** If `process_outbound()` returns `ERR_IO`: return `Result::OK` to caller (silent drop). Note: `ERR_FULL` is NOT intercepted — execution falls through. [Risk 1]

6. **Collect delayed messages.** `m_impairment.collect_deliverable(now_us, delayed_envelopes, IMPAIR_DELAY_BUF_SIZE)` called. Scans `m_delay_buf[0..31]`; for each active entry where `release_us <= now_us`, copies to the stack-allocated `delayed_envelopes[32]` array, marks slot inactive, decrements `m_delay_count`. Returns `delayed_count` (0..32).

7. **Send main datagram.** `m_sock_ops->send_to(m_fd, m_wire_buf, wire_len, m_cfg.peer_ip, m_cfg.peer_port)` called. Dispatches to `SocketOpsImpl::send_to()` → `::sendto()`. On failure: log `WARNING_LO` with peer IP:port; return `ERR_IO`.

8. **Send delayed datagrams (bounded loop).** For `i = 0..delayed_count-1`:
   - `NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE)`.
   - `Serializer::serialize(delayed_envelopes[i], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)`. If fails: `continue` (silent skip).
   - `(void)m_sock_ops->send_to(m_fd, m_wire_buf, delayed_len, m_cfg.peer_ip, m_cfg.peer_port)`. Return value discarded.

9. **Post-condition assertion.** `NEVER_COMPILED_OUT_ASSERT(wire_len > 0U)`.

10. **Return `Result::OK`.**

---

## 4. Call Tree

```
UdpBackend::send_message(envelope)
 ├── NEVER_COMPILED_OUT_ASSERT(m_open)
 ├── NEVER_COMPILED_OUT_ASSERT(m_fd >= 0)
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))
 ├── Serializer::serialize(envelope, m_wire_buf, 8192, wire_len)
 │    ├── envelope_valid(env)
 │    ├── write_u8()  ×4  [message_type, reliability_class, priority, padding=0]
 │    ├── write_u64() ×3  [message_id, timestamp_us, expiry_time_us]
 │    ├── write_u32() ×4  [source_id, destination_id, payload_length, padding=0]
 │    └── memcpy(payload)
 ├── timestamp_now_us()
 │    └── clock_gettime(CLOCK_MONOTONIC, &ts)
 ├── ImpairmentEngine::process_outbound(envelope, now_us)
 │    ├── is_partition_active(now_us)        [partition state machine]
 │    ├── check_loss()                       [m_prng.next_double() < loss_prob]
 │    ├── queue_to_delay_buf(env, release_us) [envelope_copy into m_delay_buf slot]
 │    └── apply_duplication(env, release_us) [maybe queue_to_delay_buf(env, release_us+100)]
 ├── [if ERR_IO] → return Result::OK         [silent drop]
 ├── ImpairmentEngine::collect_deliverable(now_us, delayed[32], 32)
 │    └── [loop 0..31] copy + deactivate if release_us <= now_us
 ├── ISocketOps::send_to(m_fd, m_wire_buf, wire_len, peer_ip, peer_port)
 │    └── ::sendto()                         [POSIX syscall]
 └── [loop i=0..delayed_count-1]
      ├── Serializer::serialize(delayed[i], m_wire_buf, ...)
      └── (void)ISocketOps::send_to(...)
           └── ::sendto()
```

---

## 5. Key Components Involved

- **`UdpBackend`** (`src/platform/UdpBackend.cpp/.hpp`): Owns `m_fd`, `m_wire_buf[8192]`, `m_impairment`, `m_recv_queue`, `m_cfg`. Implements `TransportInterface`. SC functions: `send_message()` (HAZ-005, HAZ-006), `receive_message()` (HAZ-004, HAZ-005).
- **`Serializer::serialize()`** (`src/core/Serializer.cpp/.hpp`): Static method. Produces a deterministic 44-byte big-endian header plus opaque payload. SC (HAZ-005). No state.
- **`ImpairmentEngine`** (`src/platform/ImpairmentEngine.hpp`): Applies loss, partition, latency, jitter, duplication to outbound messages. Owns `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` (32 slots). SC: `process_outbound()` (HAZ-002, HAZ-007), `collect_deliverable()` (HAZ-004, HAZ-006).
- **`ISocketOps` / `SocketOpsImpl`** (`src/platform/`): Interface over POSIX socket calls. Default implementation is `SocketOpsImpl::instance()` (singleton). Tests inject a mock via the injection constructor.
- **`m_wire_buf[SOCKET_RECV_BUF_BYTES]`**: 8192-byte member buffer. Reused for both main datagram and each delayed datagram in sequence (safe because serialize-then-send is sequential in each iteration).
- **`delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]`**: Stack-allocated array of 32 `MessageEnvelope` objects (~132 KB on the call stack).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `Serializer::serialize()` non-OK | Log `WARNING_LO`; return error | Continue to timestamp |
| `process_outbound()` == `ERR_IO` | Return `Result::OK` (silent drop) | Continue to `collect_deliverable()` |
| `process_outbound()` == `ERR_FULL` | Falls through (not intercepted) — send without impairment tracking [Risk 1] | (same) |
| Impairments disabled (`!m_cfg.enabled`) | Queue with `release_us = now_us` (immediate) | Full impairment chain |
| Partition active | `process_outbound` returns `ERR_IO` | Proceed to loss check |
| Loss roll fires | `process_outbound` returns `ERR_IO` | Proceed to latency/queue |
| Delay buffer full (enabled path) | Log `WARNING_HI`; return `ERR_FULL` | `queue_to_delay_buf()` |
| `send_to()` (main) returns false | Log `WARNING_LO`; return `ERR_IO` | Continue to delayed loop |
| `delayed_count > 0` | Enter loop (0..31) | Skip loop |
| `Serializer::serialize()` (delayed) non-OK | `continue` (skip iteration) | `(void)send_to()` |

---

## 7. Concurrency / Threading Behavior

**Thread context:** `send_message()` runs entirely in the calling thread. No internal threads are created.

**`m_wire_buf`**: Written by `send_message()` and also by the receive path (`recv_one_datagram()`). No mutex protects it. Concurrent `send_message()` and `receive_message()` calls on the same instance would race on `m_wire_buf`. [ASSUMPTION: callers enforce single-threaded access per instance.]

**`m_impairment`**: Mutable state (`m_delay_buf`, `m_delay_count`, `m_prng`, partition fields). Accessed from both `send_message()` (via `process_outbound()` and `collect_deliverable()`) and `receive_message()` (via `flush_delayed_to_queue()`). No mutex. Same single-thread assumption required.

**`m_recv_queue` (RingBuffer)**: Uses `std::atomic<uint32_t>` with acquire/release ordering for SPSC safety. `m_tail` stored with release by the producer (`push`); loaded with acquire by the consumer (`pop`). `m_head` stored with release by the consumer; loaded with acquire by the producer. The send path does NOT touch `m_recv_queue`.

**Delayed flush timing**: Delayed messages are flushed opportunistically only when `send_message()` or `receive_message()` is called. No background timer exists.

---

## 8. Memory & Ownership Semantics

- **`envelope` parameter**: Passed by `const&`. Not stored; serialized into `m_wire_buf` and discarded.
- **`m_wire_buf[8192]`**: Inline member of `UdpBackend`. Overwritten on every call. No heap. Reused for each delayed datagram in the loop (serialize then send immediately — safe).
- **`delayed_envelopes[32]`**: Stack-local in `send_message()`. 32 × ~4140 bytes ≈ 132 KB. Populated by `collect_deliverable()` via `envelope_copy()` (which is `memcpy(&dst, &src, sizeof(MessageEnvelope))`). Discarded on return.
- **`m_impairment.m_delay_buf[32]`**: Inline in `ImpairmentEngine`, inline in `UdpBackend`. Lifetime = same as `UdpBackend`.
- **No heap allocation** anywhere on this path. Power of 10 Rule 3 fully satisfied.
- **`UdpBackend::~UdpBackend()`**: Calls `UdpBackend::close()` as a qualified non-virtual call to avoid virtual dispatch from the destructor. Closes `m_fd`.

---

## 9. Error Handling Flow

| Error point | Result | Logged | Caller impact |
|-------------|--------|--------|---------------|
| Entry assertion fails | abort / IResetHandler | FATAL | Program terminates or reset |
| `serialize()` non-OK | Propagated | `WARNING_LO` | Caller receives error code |
| `process_outbound()` == `ERR_IO` | Converted to `OK` | None | Silent drop; caller unaware |
| `process_outbound()` == `ERR_FULL` | Falls through; send proceeds | `WARNING_HI` (inside engine) | Message sent without impairment tracking |
| `send_to()` (main) returns false | `ERR_IO` | `WARNING_LO` peer IP:port | Caller receives `ERR_IO` |
| `serialize()` delayed non-OK | `continue` | None | Delayed datagram silently skipped |
| `send_to()` (delayed) returns false | `(void)` ignored | None | No propagation |

---

## 10. External Interactions

- **`::sendto(int, const void*, size_t, int, const sockaddr*, socklen_t)`**: Called via `ISocketOps::send_to()`. `fd = m_fd`, `buf = m_wire_buf`, `len = wire_len`, `dest = sockaddr_in{peer_ip, peer_port}`. [ASSUMPTION: flags = 0.]
- **`clock_gettime(CLOCK_MONOTONIC, &ts)`**: Called via `timestamp_now_us()`. Non-blocking. Returns `tv_sec * 1000000 + tv_nsec / 1000` microseconds.
- No file I/O, no IPC, no hardware interactions.

---

## 11. State Changes / Side Effects

| Object | Field | Change |
|--------|-------|--------|
| `UdpBackend` | `m_wire_buf` | Overwritten with serialized bytes (main + each delayed) |
| `ImpairmentEngine` | `m_delay_buf[slot].env` | Written by `queue_to_delay_buf()` |
| `ImpairmentEngine` | `m_delay_buf[slot].active` | Set true (queue) / false (collect) |
| `ImpairmentEngine` | `m_delay_buf[slot].release_us` | Set to `now_us + computed delay` |
| `ImpairmentEngine` | `m_delay_count` | Incremented by queue; decremented by collect |
| `ImpairmentEngine` | PRNG state (`m_prng`) | Advances for each loss/jitter/dup roll |
| `ImpairmentEngine` | `m_partition_active`, `m_next_partition_event_us` | May transition via `is_partition_active()` |
| OS kernel | UDP send buffer | Datagram(s) enqueued for transmission |
| `UdpBackend` | `m_recv_queue` | NOT modified on send path |

---

## 12. Sequence Diagram

```
Caller              UdpBackend          Serializer      ImpairmentEngine     ISocketOps
  |                     |                   |                  |                  |
  |--send_message(env)->|                   |                  |                  |
  |                     |--serialize(env)-->|                  |                  |
  |                     |<--OK, wire_len=44+plen               |                  |
  |                     |--timestamp_now_us()                  |                  |
  |                     |--process_outbound(env, now_us)------>|                  |
  |                     |<--OK (buffered) or ERR_IO (drop)-----|                  |
  |  [ERR_IO: return OK]|                   |                  |                  |
  |                     |--collect_deliverable(now_us,32)----->|                  |
  |                     |<--delayed_count, delayed_envelopes---|                  |
  |                     |--send_to(fd, wire_buf, len, peer)----|----------------->|
  |                     |<--true/false--------------------------|---------         |
  |  [false: ERR_IO]    |                   |                  |                  |
  |                     | [loop i=0..delayed_count-1]          |                  |
  |                     |--serialize(delayed[i])-->            |                  |
  |                     |--send_to(...) (void)----|----------->|                  |
  |<--Result::OK--------|                   |                  |                  |
```

**Drop path:** `process_outbound()` → `ERR_IO` → `send_message()` returns `OK` without calling `send_to()`.

---

## 13. Initialization vs Runtime Flow

**Initialization** (`UdpBackend::init()`):
- Asserts `config.kind == TransportKind::UDP` and `!m_open`.
- `m_sock_ops->create_udp()` creates `AF_INET/SOCK_DGRAM` socket.
- `m_sock_ops->set_reuseaddr(m_fd)` sets `SO_REUSEADDR`.
- `m_sock_ops->do_bind(m_fd, bind_ip, bind_port)` binds the socket.
- `m_recv_queue.init()` resets `m_head` and `m_tail` atomics to 0 (relaxed store).
- `impairment_config_default(imp_cfg)` sets `enabled=false`, all probabilities 0.0, `prng_seed=42`. If `num_channels > 0`, overrides from `config.channels[0].impairment`.
- `m_impairment.init(imp_cfg)` seeds PRNG; zeroes delay/reorder buffers; `m_delay_count = 0`.
- `m_open = true`. Log `INFO`.

**Runtime** (this use case): No allocation. All state is in-memory member variables. `m_wire_buf` reused every call.

---

## 14. Known Risks

- **Risk 1: `ERR_FULL` not intercepted.** `process_outbound()` returns `ERR_FULL` when the delay buffer saturates. The check at `send_message()` only intercepts `ERR_IO`. An `ERR_FULL` result falls through to `collect_deliverable()` and `send_to()`, sending the datagram without impairment tracking. The caller receives `OK`.
- **Risk 2: Delayed send failure silently discarded.** `(void)m_sock_ops->send_to()` in the delayed loop. A failed delayed send produces no log and no return-code propagation.
- **Risk 3: `m_wire_buf` reuse in delayed loop.** The buffer is overwritten for each delayed envelope. Safe as written (serialize then send immediately). A refactor separating serialize from send would create aliasing.
- **Risk 4: Opportunistic delayed flush.** Delayed messages are only flushed when `send_message()` or `receive_message()` is called. If neither is called for an extended period, messages accumulate despite their `release_us` having passed.
- **Risk 5: Stack pressure from `delayed_envelopes[32]`.** Approximately 132 KB of stack in the worst case (32 × `sizeof(MessageEnvelope)`). Deep call chains could overflow on constrained platforms.
- **Risk 6: `IMPAIR_DELAY_BUF_SIZE = 32` is a hard ceiling.** When the delay buffer is full, `process_outbound()` returns `ERR_FULL` and subsequent messages are sent without delay simulation.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `ISocketOps::send_to()` calls `::sendto()` with `flags = 0`. Not confirmed from headers read.
- [ASSUMPTION] `ImpairmentEngine` is not internally thread-safe. Callers must ensure single-threaded access to `UdpBackend`.
- [ASSUMPTION] When `fixed_latency_ms == 0` and `jitter_mean_ms == 0` with impairments disabled, `process_outbound()` queues the envelope with `release_us = now_us`. `collect_deliverable()` immediately collects it (since `release_us <= now_us`), so `delayed_count = 1` and the message is sent via the delayed loop rather than the main `send_to()` call. [ASSUMPTION: The main `send_to()` at step 7 always fires when `process_outbound()` returns `OK`, regardless of whether the message was buffered. This matches the code structure: the main message is sent at step 7 unconditionally after the `ERR_IO` guard.]
- [ASSUMPTION] `Logger::log()` is safe to call from the calling thread context.
- [ASSUMPTION] The peer IP and port stored in `m_cfg` are set during `init()` and do not change at runtime. There is no per-message routing.
