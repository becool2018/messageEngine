# UC_20 — TCP Send: Framed Message Transmission

**HL Group:** System Internal — sub-function of HL-1/2/3 (send); hidden inside `TcpBackend::send_message()`
**Actor:** System (internal sub-function) — the User never calls framing directly
**Requirement traceability:** REQ-4.1.2, REQ-6.1.4, REQ-6.1.5, REQ-6.1.6, REQ-6.3.2, REQ-6.3.3, REQ-6.3.5, REQ-7.1.1

---

## 1. Use Case Overview

UC_20 is a **System Internal** sub-function. The User never calls TCP framing or `send_message()` directly; the call originates from `DeliveryEngine::send_via_transport()`.

**Invoked by:** UC_01 (best-effort send), UC_02 (reliable-with-ACK send), UC_03 (reliable-with-retry send) — all of which call `DeliveryEngine::send_via_transport()` (DeliveryEngine.cpp), which calls `TransportInterface::send_message()`, which dispatches to `TcpBackend::send_message()`.

**Why it is factored out:** Length-prefix framing, the impairment decision gate, multi-client fan-out, and delayed-message flushing are all platform-layer concerns that sit below the delivery semantics. UC_20 makes the framing protocol, the impairment interaction, and the double-send risk visible without cluttering the higher-level HL-1/2/3 use cases.

**Trigger:** A caller invokes `TcpBackend::send_message(envelope)` to transmit a fully-formed `MessageEnvelope` to all currently-connected TCP clients.

**Expected outcome:** The envelope is serialized to a length-prefixed wire frame, optionally delayed or dropped by the impairment engine, then transmitted to every connected client via `tcp_send_frame()`. Any messages previously deferred in the impairment delay buffer that have matured are also flushed to all clients as a side effect of the same call.

---

## 2. Entry Points

| Entry point | File | Line |
|-------------|------|------|
| `TcpBackend::send_message(const MessageEnvelope&)` | `src/platform/TcpBackend.cpp` | 347 |

Prerequisites: `TcpBackend::init()` has been called and `m_open == true`. The impairment engine has been initialized (`m_initialized == true` on `m_impairment`). At least zero clients may be connected; if `m_client_count == 0` the message is silently discarded after impairment processing.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **`TcpBackend::send_message()`** (TcpBackend.cpp:347) is called by the application layer with a populated `MessageEnvelope`.
2. Precondition assertions fire: `m_open` must be true (line 349) and `envelope_valid(envelope)` must return true (line 350).
3. **`Serializer::serialize()`** (Serializer.cpp:117) is called (line 354) with `m_wire_buf`, capacity `SOCKET_RECV_BUF_BYTES (8192)`, and an output-length reference `wire_len`.
   - `envelope_valid()` is called again inside `serialize()` (line 127) as a second guard.
   - Header fields are written in order using sequential `write_u8`/`write_u32`/`write_u64` calls: message_type (u8), reliability_class (u8), priority (u8), padding (u8 = 0), message_id (u64), timestamp_us (u64), source_id (u32), destination_id (u32), expiry_time_us (u64), payload_length (u32), padding (u32 = 0); totalling `WIRE_HEADER_SIZE = 44U` bytes.
   - `NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE)` at Serializer.cpp:155.
   - Payload bytes (up to `MSG_MAX_PAYLOAD_BYTES = 4096U`) are `memcpy`'d directly (line 160).
   - `out_len = WIRE_HEADER_SIZE + env.payload_length` set (line 163); asserted at line 166.
   - Returns `Result::OK` on success; `Result::ERR_INVALID` if `envelope_valid` fails or buffer too small.
4. If serialize fails: log WARNING_LO (TcpBackend.cpp:357); return immediately with the error code.
5. **`timestamp_now_us()`** (Timestamp.hpp:31) called (TcpBackend.cpp:362) via `clock_gettime(CLOCK_MONOTONIC)`. Result stored in `now_us`.
6. **`ImpairmentEngine::process_outbound(envelope, now_us)`** (ImpairmentEngine.cpp:151) called (TcpBackend.cpp:363). This is the impairment decision gate:
   - **Path A — impairments disabled** (`m_cfg.enabled == false`): if delay buffer has room (`m_delay_count < IMPAIR_DELAY_BUF_SIZE`), calls `queue_to_delay_buf(in_env, now_us)` with `release_us = now_us` (immediate delivery). Returns `ERR_FULL` with WARNING_HI if buffer is full.
   - **Path B — partition active**: logs WARNING_LO "message dropped (partition active)"; returns `ERR_IO`.
   - **Path C — loss fires**: calls `m_prng.next_double()`; if result < `loss_probability`, logs WARNING_LO "message dropped (loss probability)"; returns `ERR_IO`.
   - **Path D — normal queuing**: computes `release_us = now_us + fixed_latency_us + jitter_us`. Jitter drawn via PRNG only when `jitter_mean_ms > 0U`. Buffer-full guard returns `ERR_FULL` with WARNING_HI if `m_delay_count >= IMPAIR_DELAY_BUF_SIZE`. Calls `queue_to_delay_buf(in_env, release_us)`. If `duplication_probability > 0.0`, calls `apply_duplication()` which may queue a second copy at `release_us + 100µs`. Returns `Result::OK`.
7. Back in `send_message()` (TcpBackend.cpp:364): if `process_outbound` returned `ERR_IO`, the message was dropped by loss or partition; return `Result::OK` silently (line 365). **Important:** `ERR_FULL` from `process_outbound` is NOT caught here — only `ERR_IO` is special-cased. If `process_outbound` returns `ERR_FULL`, execution falls through to the client-count check and the already-serialized frame is sent immediately, bypassing the delay buffer (see Risk 1).
8. **No-client guard** (TcpBackend.cpp:369): if `m_client_count == 0`, log WARNING_LO "No clients connected; discarding message"; return `Result::OK`.
9. **`TcpBackend::send_to_all_clients(m_wire_buf, wire_len)`** (TcpBackend.cpp:258) called (line 375). Iterates `m_client_fds[0..MAX_TCP_CONNECTIONS-1]` (bounded loop, max 8 iterations):
   - Skips slots with `fd < 0` (line 265).
   - Calls **`tcp_send_frame(fd, buf, len, send_timeout_ms)`** (SocketUtils.cpp:393) for each active client, passing `m_cfg.channels[0U].send_timeout_ms` as timeout (line 269).
     - Builds 4-byte big-endian header from `len` (SocketUtils.cpp:402–405): `header[0] = (len>>24)&0xFF`, etc.
     - Calls `socket_send_all(fd, header, 4U, timeout_ms)` (line 408) for the header.
     - If `len > 0U` (line 415): calls `socket_send_all(fd, buf, len, timeout_ms)` (line 416) for the payload.
     - `socket_send_all` (SocketUtils.cpp:292) runs a `poll(POLLOUT)` + `send()` loop until all bytes are sent. Loop is bounded by `len` bytes (each iteration makes progress or returns false). Returns false on poll timeout or `send()` error; logs WARNING_HI.
   - If `tcp_send_frame` returns false: `send_to_all_clients` logs WARNING_LO per-client (TcpBackend.cpp:270–271); continues to next client — no early exit, no disconnection.
10. **`TcpBackend::flush_delayed_to_clients(now_us)`** (TcpBackend.cpp:280) called (line 376) to drain delay-buffer entries whose `release_us <= now_us`:
    - Assertions: `now_us > 0ULL` (line 282) and `m_open` (line 283).
    - Stack-allocates `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` (32 slots, line 285) ≈ 132 KB.
    - Calls **`ImpairmentEngine::collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`**: scans `m_delay_buf[0..31]`; for each slot where `active == true` and `release_us <= now_us`, calls `envelope_copy()` into the output array, marks slot inactive, decrements `m_delay_count`. Returns count.
    - For each collected message (bounded loop, max 32 iterations, line 290):
      - `NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE)` (line 291).
      - Calls `Serializer::serialize(delayed[i], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)` (line 293). This overwrites `m_wire_buf`.
      - If serialize fails: silently `continue` (line 296) — no log entry; message permanently lost (see Risk 5).
      - Calls `send_to_all_clients(m_wire_buf, delayed_len)` (line 298).
11. Postcondition assertion at TcpBackend.cpp:378: `NEVER_COMPILED_OUT_ASSERT(wire_len > 0U)`.
12. Returns `Result::OK`.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message()                    [TcpBackend.cpp:347]
 ├── envelope_valid()                          [MessageEnvelope.hpp:63]
 ├── Serializer::serialize()                   [Serializer.cpp:117]
 │    ├── envelope_valid()                     [MessageEnvelope.hpp:63]
 │    ├── write_u8() × 4                       [Serializer.cpp:24 — inline]
 │    ├── write_u64() × 3                      [Serializer.cpp:50 — inline]
 │    ├── write_u32() × 4                      [Serializer.cpp:35 — inline]
 │    └── memcpy() for payload                 [<cstring>]
 ├── timestamp_now_us()                        [Timestamp.hpp:31]
 │    └── clock_gettime(CLOCK_MONOTONIC)        [POSIX]
 ├── ImpairmentEngine::process_outbound()      [ImpairmentEngine.cpp:151]
 │    ├── is_partition_active()                [ImpairmentEngine.cpp]
 │    ├── check_loss()                         [ImpairmentEngine.cpp]
 │    │    └── PrngEngine::next_double()       [PrngEngine.hpp:91]
 │    ├── PrngEngine::next_range()             [PrngEngine.hpp:119]  (jitter, if enabled)
 │    ├── queue_to_delay_buf()                 [ImpairmentEngine.cpp:83]
 │    │    └── envelope_copy()                 [MessageEnvelope.hpp:56]
 │    └── apply_duplication()                  [ImpairmentEngine.cpp]  (if dup_prob > 0)
 │         ├── PrngEngine::next_double()
 │         └── queue_to_delay_buf()
 ├── send_to_all_clients(m_wire_buf, wire_len) [TcpBackend.cpp:258]  (if clients > 0)
 │    └── tcp_send_frame() × (0..8 clients)   [SocketUtils.cpp:393]
 │         ├── socket_send_all() for 4-byte header [SocketUtils.cpp:292]
 │         │    └── poll(POLLOUT) + send()     [POSIX, loop bounded by 4 bytes]
 │         └── socket_send_all() for payload   [SocketUtils.cpp:292]
 │              └── poll(POLLOUT) + send()     [POSIX, loop bounded by wire_len bytes]
 └── flush_delayed_to_clients(now_us)          [TcpBackend.cpp:280]
      ├── ImpairmentEngine::collect_deliverable() [ImpairmentEngine.cpp:216]
      │    └── envelope_copy() × (0..32)       [MessageEnvelope.hpp:56]
      └── (per delayed message, 0..32 iterations):
           ├── Serializer::serialize()         [Serializer.cpp:117]
           └── send_to_all_clients()           [TcpBackend.cpp:258]
                └── tcp_send_frame() × (0..8) [SocketUtils.cpp:393]
```

---

## 5. Key Components Involved

### `TcpBackend::send_message()` (TcpBackend.cpp:347)
Orchestrates the full send pipeline: serialize, impairment decision, transmit to all clients, flush delayed messages. Sole public entry point for outbound messages. Asserts `wire_len > 0U` as a postcondition.

### `Serializer::serialize()` (Serializer.cpp:117)
Converts a `MessageEnvelope` to a deterministic big-endian wire representation. Writes a fixed 44-byte (`WIRE_HEADER_SIZE`) header followed by opaque payload bytes. Validates the envelope and checks buffer capacity before any writes. Output is written directly to `m_wire_buf`.

### `ImpairmentEngine::process_outbound()` (ImpairmentEngine.cpp:151)
The impairment decision gate for outbound messages. Applies partition drop, probabilistic loss, fixed latency, random jitter, and probabilistic duplication. All paths that do not drop the message result in a call to `queue_to_delay_buf()`. Returns `ERR_IO` for drop, `ERR_FULL` for buffer overflow, or `OK` for successful queuing.

### `ImpairmentEngine::collect_deliverable()` (ImpairmentEngine.cpp:216)
Scans all 32 delay-buffer slots; copies messages whose `release_us <= now_us` into the caller's stack array and deactivates those slots. Returns the count of collected messages.

### `TcpBackend::send_to_all_clients()` (TcpBackend.cpp:258)
Private helper. Iterates `m_client_fds[0..7]` and calls `tcp_send_frame()` for each slot with `fd >= 0`. Logs WARNING_LO per-client failure but continues.

### `TcpBackend::flush_delayed_to_clients()` (TcpBackend.cpp:280)
Private helper called at the end of every `send_message()`. Drains delay-buffer entries that have matured by re-serializing each one into `m_wire_buf` and calling `send_to_all_clients()`.

### `tcp_send_frame()` (SocketUtils.cpp:393)
Encodes the TCP framing protocol: builds a 4-byte big-endian length prefix from `len`, then calls `socket_send_all()` twice — once for the 4-byte header, once for the serialized payload bytes.

### `socket_send_all()` (SocketUtils.cpp:292)
Lowest-level send loop. Calls `poll(POLLOUT)` before each `send()` call; advances `sent` counter; loops until `sent == len` or an error occurs. The loop is bounded by `len` bytes (Power of 10 Rule 2).

---

## 6. Branching Logic / Decision Points

| Condition | Outcome | Next step |
|-----------|---------|-----------|
| `serialize()` returns non-OK | Log WARNING_LO; return error to caller | No transmission |
| `process_outbound()` returns `ERR_IO` | Message dropped (loss or partition) | Return `OK` silently (TcpBackend.cpp:365) |
| `process_outbound()` returns `ERR_FULL` | NOT caught; falls through | Serialized frame sent immediately, bypassing delay (see Risk 1) |
| `process_outbound()` returns `OK` | Message queued in delay buffer | Continue to client check |
| `m_cfg.enabled == false` in `process_outbound` | Queue with `release_us = now_us` (immediate) | Message deliverable on first `collect_deliverable()` call |
| `is_partition_active(now_us) == true` | Drop | Log WARNING_LO; return `ERR_IO` |
| `check_loss()` fires | Drop | Log WARNING_LO; return `ERR_IO` |
| `jitter_mean_ms == 0U` | Skip jitter PRNG call | `jitter_us = 0`; `release_us = now_us + fixed_latency_us` |
| `duplication_probability <= 0.0` | Skip `apply_duplication()` | No duplicate queued |
| `m_client_count == 0` | Discard | Log WARNING_LO; return `OK` |
| `m_client_fds[i] < 0` in `send_to_all_clients()` | Skip slot | Continue to next index |
| `tcp_send_frame()` returns false | Log WARNING_LO per client | Continue to next client (fd NOT removed) |
| `collect_deliverable()` returns 0 | No delayed messages | `flush_delayed_to_clients()` loop body never executes |
| `Serializer::serialize()` fails inside `flush_delayed_to_clients()` | Silently `continue` | No log; that delayed message permanently lost |
| `len > 0U` in `tcp_send_frame()` | Call `socket_send_all()` for payload | Otherwise skip payload send |
| `poll()` returns `<= 0` in `socket_send_all()` | Log WARNING_HI; return false | Propagates up to `tcp_send_frame()` |
| `send()` returns `< 0` in `socket_send_all()` | Log WARNING_HI; return false | Propagates up to `tcp_send_frame()` |

---

## 7. Concurrency / Threading Behavior

`TcpBackend` has no internal threads and no mutexes. The entire `send_message()` execution occurs on the calling thread.

`m_recv_queue` (a `RingBuffer`) uses `std::atomic<uint32_t>` head/tail with acquire/release memory ordering. However, `send_message()` does not touch `m_recv_queue`; only `receive_message()` and `flush_delayed_to_queue()` do.

`m_wire_buf` is a plain `uint8_t[8192]` array — not atomic, not mutex-protected. Concurrent calls to `send_message()` from different threads would constitute a data race on `m_wire_buf`, `m_impairment`, and `m_client_fds`.

The impairment engine `m_impairment` contains no synchronization; its `m_delay_buf`, `m_delay_count`, and `m_prng` state are modified by `process_outbound()` and read/modified by `collect_deliverable()`. Both are called from the same thread within a single `send_message()` invocation.

`poll()` and `send()` are blocking POSIX syscalls. If the kernel TCP send buffer is full, a call to `socket_send_all()` may block up to `send_timeout_ms` milliseconds per iteration. For N clients and a payload of W bytes, the worst-case blocking time is proportional to `N × ceil(W / TCP_segment_size) × send_timeout_ms`.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Stack allocations in this flow
- `wire_len` (uint32_t), `now_us` (uint64_t): locals in `send_message()`.
- `delayed[IMPAIR_DELAY_BUF_SIZE]` (32 × `MessageEnvelope`): stack-allocated in `flush_delayed_to_clients()` at TcpBackend.cpp:285. Each `MessageEnvelope` contains `uint8_t payload[4096]` inline. Total: 32 × ~4140 bytes ≈ **132 KB** (see Risk 2).
- `header[4U]`: 4-byte array in `tcp_send_frame()` (SocketUtils.cpp:401).

### No heap allocation
No `malloc` or `new` is used anywhere in this flow.

### Object lifetimes and ownership
- `m_wire_buf[8192]`: owned by `TcpBackend` instance; valid for the object's lifetime. Overwritten on every `send_message()` call and for each delayed message in `flush_delayed_to_clients()`.
- `m_delay_buf[32]` (in `ImpairmentEngine`): owned by `m_impairment`; `queue_to_delay_buf()` calls `envelope_copy()` to store a full value copy, so the original envelope passed to `send_message()` may be safely destroyed after the call returns.
- `envelope` parameter to `send_message()`: passed by const reference; the caller retains ownership. `Serializer::serialize()` and `process_outbound()` read it without storing a reference; `queue_to_delay_buf()` stores an independent copy via `envelope_copy()`.

### `m_wire_buf` aliasing within a single `send_message()` call
`m_wire_buf` is written by `Serializer::serialize()` for the primary message (line 354), then overwritten for each delayed message (line 293) inside `flush_delayed_to_clients()`. This is safe because `send_to_all_clients()` at line 375 completes full transmission before `flush_delayed_to_clients()` is called at line 376. The ordering dependency is subtle and constitutes a maintenance hazard (see Risk 6).

---

## 9. Error Handling Flow

```
send_message()
  envelope_valid() fails (assertion)     → NEVER_COMPILED_OUT_ASSERT fires; abort (debug)
                                           or controlled reset (production)
  serialize() returns non-OK             → WARNING_LO logged; return error to caller
  process_outbound() returns ERR_IO      → Silent OK return (drop is expected behavior)
  process_outbound() returns ERR_FULL    → NOT caught; falls through to send_to_all_clients()
  m_client_count == 0                    → WARNING_LO; return OK (discard)
  tcp_send_frame() fails (per client)    → WARNING_LO per client; loop continues; fd NOT removed
  flush_delayed: serialize fails         → Silent continue; no log; message permanently lost
  flush_delayed: tcp_send_frame fails    → WARNING_LO per client (via send_to_all_clients)
  poll() timeout in socket_send_all      → WARNING_HI from SocketUtils; return false
  send() error in socket_send_all        → WARNING_HI from SocketUtils; return false
```

Key observations:
- `send_message()` returns `Result::OK` for all outcomes except a serialize failure. A dropped message, a no-client discard, and a successful transmission all return the same code.
- There is no retry logic inside `send_message()`. Failed sends due to client socket errors are logged but not retried; the failing client fd is NOT removed or invalidated.
- A per-client `tcp_send_frame()` failure produces log entries at three severity levels: WARNING_HI inside `socket_send_all()`, WARNING_HI inside `tcp_send_frame()`, and WARNING_LO inside `send_to_all_clients()` — three entries for the same event.

---

## 10. External Interactions

### POSIX socket I/O
- `poll(pfd, 1, timeout_ms)` with `POLLOUT` event (SocketUtils.cpp:310): called before each `send()` in `socket_send_all()`. May block up to `channels[0].send_timeout_ms` milliseconds if the kernel send buffer is full.
- `send(fd, &buf[sent], remaining, 0)` (SocketUtils.cpp:319): called after each successful `poll()`. Returns the number of bytes actually accepted, which may be less than `remaining`.

### POSIX clock
- `clock_gettime(CLOCK_MONOTONIC, &ts)` (Timestamp.hpp:31): called once per `send_message()` invocation to obtain `now_us`. The same value is reused throughout both `process_outbound()` and `flush_delayed_to_clients()` without re-reading the clock (see Risk 8).

---

## 11. State Changes / Side Effects

| State | Modified by | Effect |
|-------|-------------|--------|
| `m_delay_buf[i].active`, `.env`, `.release_us` | `queue_to_delay_buf()` | Slot becomes active with a deep copy of the envelope and a computed release timestamp |
| `m_delay_count` | `queue_to_delay_buf()` / `collect_deliverable()` | Incremented on enqueue; decremented on collection |
| `m_prng.m_state` | `check_loss()`, `next_range()` for jitter, `apply_duplication()` | PRNG state advances by 1–3 xorshift64 steps depending on active impairments |
| `m_partition_active`, `m_next_partition_event_us` | `is_partition_active()` | Partition state machine may transition |
| `m_wire_buf[0..N-1]` | `Serializer::serialize()` | Overwritten with the serialized frame; overwritten again for each delayed message in `flush_delayed_to_clients()` |
| Kernel TCP send buffer | `send()` via `socket_send_all()` | Data enqueued in kernel for transmission to each connected peer |

No persistent disk, database, or hardware state is modified.

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant Serializer
    participant ImpairmentEngine
    participant SocketUtils
    participant OS

    Caller->>TcpBackend: send_message(envelope)
    Note over TcpBackend: assert m_open (line 349)<br/>assert envelope_valid (line 350)
    TcpBackend->>Serializer: serialize(envelope, m_wire_buf, 8192, wire_len)
    Note over Serializer: write 44-byte header + memcpy payload
    Serializer-->>TcpBackend: OK / ERR_INVALID
    alt serialize fails
        TcpBackend-->>Caller: return ERR_INVALID
    end
    TcpBackend->>OS: clock_gettime(CLOCK_MONOTONIC) → now_us
    TcpBackend->>ImpairmentEngine: process_outbound(envelope, now_us)
    alt partition active or loss fires
        ImpairmentEngine-->>TcpBackend: ERR_IO
        TcpBackend-->>Caller: return OK (silent drop)
    else delay buffer full (ERR_FULL)
        ImpairmentEngine-->>TcpBackend: ERR_FULL
        Note over TcpBackend: ERR_FULL not caught — falls through to send immediately
    else normal path
        Note over ImpairmentEngine: compute release_us = now_us + latency + jitter<br/>queue_to_delay_buf(envelope, release_us)
        ImpairmentEngine-->>TcpBackend: OK
    end
    alt m_client_count == 0
        TcpBackend-->>Caller: return OK (no clients; discard)
    end
    TcpBackend->>TcpBackend: send_to_all_clients(m_wire_buf, wire_len)
    loop for each m_client_fds[i] where fd >= 0 (0..7)
        TcpBackend->>SocketUtils: tcp_send_frame(fd, buf, wire_len, timeout_ms)
        SocketUtils->>OS: poll(POLLOUT) + send() — 4-byte header
        SocketUtils->>OS: poll(POLLOUT) + send() — wire_len payload bytes
        SocketUtils-->>TcpBackend: true / false
    end
    TcpBackend->>TcpBackend: flush_delayed_to_clients(now_us)
    TcpBackend->>ImpairmentEngine: collect_deliverable(now_us, delayed[], 32)
    Note over ImpairmentEngine: scan m_delay_buf; collect entries where release_us <= now_us
    ImpairmentEngine-->>TcpBackend: count (0..32)
    loop for each deliverable delayed[i]
        TcpBackend->>Serializer: serialize(delayed[i], m_wire_buf, 8192, len)
        TcpBackend->>TcpBackend: send_to_all_clients(m_wire_buf, len)
    end
    Note over TcpBackend: NEVER_COMPILED_OUT_ASSERT(wire_len > 0) (line 378)
    TcpBackend-->>Caller: return OK
```

---

## 13. Initialization vs Runtime Flow

### Initialization (one-time, during `TcpBackend::init()`)
- `m_wire_buf` is value-initialized to zero in the constructor (TcpBackend.cpp:29, `m_wire_buf{}`).
- `m_client_fds[0..7]` are set to -1 (TcpBackend.cpp:34–36).
- `m_impairment.init(imp_cfg)` seeds the PRNG (`cfg.prng_seed != 0 ? cfg.prng_seed : 42ULL`), `memset`s both `m_delay_buf` and `m_reorder_buf` to zero, and sets `m_initialized = true`.
- `m_open` is set to true on successful `init()` completion.
- The impairment config applied during `init()` sets `imp_cfg.enabled` only; all other impairment fields retain their `impairment_config_default()` values (`prng_seed = 42ULL`, `loss_probability = 0.0`, `fixed_latency_ms = 0U`, etc.).

### Runtime (each call to `send_message()`)
- `m_wire_buf` is overwritten with new serialized content on every call (and again per delayed message inside `flush_delayed_to_clients()`).
- `m_prng` state advances by 0–3 xorshift64 steps depending on active impairments.
- The delay buffer `m_delay_buf` gains entries via `process_outbound()` and loses entries via `collect_deliverable()` within the same `send_message()` invocation.
- Kernel TCP send buffers are written on every successful `send()` call.

---

## 14. Known Risks / Observations

### Risk 1 — `ERR_FULL` bypasses delay semantics
`send_message()` at TcpBackend.cpp:364 only checks `res == Result::ERR_IO`. If `process_outbound()` returns `ERR_FULL` (delay buffer full), `send_message()` does not return early; execution falls through and calls `send_to_all_clients()` with the already-serialized `m_wire_buf`. A message intended to be delayed (latency/jitter impairment active) is sent immediately when the buffer is full, silently violating the impairment configuration. No error is returned to the caller.

### Risk 2 — Large stack frame in `flush_delayed_to_clients()`
`MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` at TcpBackend.cpp:285 allocates 32 × `sizeof(MessageEnvelope)` bytes on the stack. With `payload[4096]` inline, each `MessageEnvelope` is approximately 4140 bytes. The array is approximately **132 KB of stack space**. On embedded or constrained targets this risks a stack overflow, particularly if `send_message()` is called from a non-trivial call depth.

### Risk 3 — Triple logging on send failure
When `socket_send_all()` fails, it logs WARNING_HI (SocketUtils.cpp:312 or 321). `tcp_send_frame` then logs WARNING_HI again (lines 409, 417). `send_to_all_clients` logs WARNING_LO at TcpBackend level (line 270). The same event produces up to three log entries at different severities, which may mislead log-level-based alerting.

### Risk 4 — Failed client fd not removed
If `tcp_send_frame()` fails for client `i`, `send_to_all_clients()` logs a warning but neither closes `m_client_fds[i]` nor marks the slot invalid. Subsequent `send_message()` calls will attempt to send to the same broken fd indefinitely.

### Risk 5 — Silent discard of delayed messages that fail re-serialization
Inside `flush_delayed_to_clients()` (TcpBackend.cpp:295–296), if `Serializer::serialize()` fails on a delayed message, the loop issues `continue` with no log entry. The message is permanently lost with no visibility to the caller or any monitoring system.

### Risk 6 — `m_wire_buf` aliased across calls within a single `send_message()`
`m_wire_buf` is used for the primary message (line 354) and then overwritten for each delayed message (line 293) inside `flush_delayed_to_clients()`. This is safe because `send_to_all_clients()` at line 375 completes full transmission before `flush_delayed_to_clients()` is called at line 376. The ordering dependency is subtle and constitutes a maintenance hazard.

### Risk 7 — Single channel assumed for all sends
`send_to_all_clients()` passes `m_cfg.channels[0U].send_timeout_ms` (TcpBackend.cpp:269) as the timeout for every client and every message. The envelope's `priority` and `reliability_class` fields are not consulted to select a different channel or timeout.

### Risk 8 — `now_us` not refreshed before `flush_delayed_to_clients()`
`now_us` is captured once at TcpBackend.cpp:362 and reused at line 376. If serialization and `send_to_all_clients()` take a significant amount of real time (e.g., due to slow TCP sends), the timestamp passed to `collect_deliverable()` may be stale, causing messages with `release_us` slightly in the future to be missed until the next `send_message()` call.

---

## 15. Unknowns / Assumptions

All facts in this document are sourced directly from the code at the stated file paths and line numbers. No assumptions were required.

**Confirmed facts:**
- `WIRE_HEADER_SIZE = 44U` (Serializer.hpp:47): 1+1+1+1+8+8+4+4+8+4+4 = 44 bytes.
- `SOCKET_RECV_BUF_BYTES = 8192U` (Types.hpp): the wire buffer capacity.
- `IMPAIR_DELAY_BUF_SIZE = 32U` (Types.hpp): maximum entries in the delay buffer.
- `MAX_TCP_CONNECTIONS = 8U` (Types.hpp): maximum simultaneous clients.
- `MSG_MAX_PAYLOAD_BYTES = 4096U` (Types.hpp): maximum payload bytes per envelope.
- `MessageEnvelope.payload` is `uint8_t payload[4096]` inline (MessageEnvelope.hpp:38).
- `envelope_copy()` uses `memcpy(&dst, &src, sizeof(MessageEnvelope))` (MessageEnvelope.hpp:56).
- `envelope_valid()` checks `message_type != INVALID`, `payload_length <= MSG_MAX_PAYLOAD_BYTES`, and `source_id != NODE_ID_INVALID` (MessageEnvelope.hpp:63).
- `impairment_config_default()` sets `prng_seed = 42ULL`.
- `TcpBackend::init()` only sets `imp_cfg.enabled`; all other impairment fields come from `impairment_config_default()` (TcpBackend.cpp:100–102).
- `channels[0U].send_timeout_ms` is the timeout used for all `tcp_send_frame()` calls (TcpBackend.cpp:269), regardless of message priority or channel count.
- `now_us` is read once at TcpBackend.cpp:362 and reused through `process_outbound()` (line 363) and `flush_delayed_to_clients()` (line 376) without refreshing the clock.
