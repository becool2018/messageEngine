# UC_01 — Best-effort send over TCP (no ACK, no retry, no dedup)

**HL Group:** HL-1 — Fire-and-Forget Message Send
**Actor:** User
**Requirement traceability:** REQ-3.3.1, REQ-3.1, REQ-3.2.3, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

**Trigger:** The application calls `DeliveryEngine::send()` with a `MessageEnvelope` whose
`reliability_class` is `ReliabilityClass::BEST_EFFORT`.

**Goal:** Transmit the envelope exactly once over TCP with no ACK slot allocated and no retry
entry scheduled. The system serializes the envelope, applies any configured impairment, and
delivers it to all connected TCP clients.

**Success outcome:** `DeliveryEngine::send()` returns `Result::OK`. The serialized wire frame
has been placed on the TCP socket(s) of all connected clients.

**Error outcomes:**
- `Result::ERR_INVALID` — engine not initialized or envelope is invalid.
- `Result::ERR_IO` — serialization failure or socket send failure (logged as WARNING_LO).
- `Result::OK` (silent drop) — message dropped by the impairment engine's loss policy;
  `process_outbound()` returns `ERR_IO`, which `send_message()` converts to `OK`.

---

## 2. Entry Points

```
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us);

// Downstream platform entry (via virtual dispatch):
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);
```

The caller constructs a `MessageEnvelope` on the stack, sets `reliability_class =
BEST_EFFORT`, `message_type = DATA`, fills `destination_id`, `priority`, `payload`, and
`payload_length`, then calls `engine.send(env, timestamp_now_us())`. Both call sites may be
on the application main thread or any thread that owns the `DeliveryEngine` instance.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. Caller invokes `DeliveryEngine::send(env, now_us)`.
2. `send()` fires two pre-condition `NEVER_COMPILED_OUT_ASSERT`s: `m_initialized` is true,
   `now_us > 0`.
3. Guard check: `if (!m_initialized) return ERR_INVALID` — skipped when initialized.
4. `send()` stamps `env.source_id = m_local_id` (the local node ID set during `init()`).
5. `send()` calls `m_id_gen.next()`, which returns the current `m_next` and increments it
   (wrapping 0 to 1). The returned value is stored in `env.message_id`.
6. `send()` stamps `env.timestamp_us = now_us`.
7. `send()` calls `send_via_transport(env)`.
8. Inside `send_via_transport()`:
   a. Three `NEVER_COMPILED_OUT_ASSERT`s fire: `m_initialized`, `m_transport != nullptr`,
      `envelope_valid(env)`.
   b. Calls `m_transport->send_message(env)` via virtual dispatch to
      `TcpBackend::send_message()`.
9. Inside `TcpBackend::send_message()`:
   a. Pre-condition asserts: `m_open`, `envelope_valid(envelope)`.
   b. Calls `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)`.
      Serializer writes a 44-byte big-endian header followed by `payload_length` payload bytes
      into `m_wire_buf`. Returns `OK` and sets `wire_len = 44 + payload_length`.
   c. Calls `timestamp_now_us()` to obtain a fresh `now_us` for impairment timing.
   d. Calls `m_impairment.process_outbound(envelope, now_us)`.
      - Loss drop: returns `ERR_IO`; `send_message()` converts this to `OK` and returns —
        no socket write occurs.
      - Otherwise: queues the message into the impairment delay buffer with
        `release_us = now_us` (zero latency case) and returns `OK`.
   e. Checks `m_client_count == 0`: if no clients connected, logs WARNING_LO, returns `OK`.
   f. Calls `flush_delayed_to_clients(now_us)`.
10. Inside `flush_delayed_to_clients()`:
    a. Calls `m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`.
       Returns the envelope placed in step 9d (since `release_us <= now_us`).
    b. Loops up to `IMPAIR_DELAY_BUF_SIZE` (32) times:
       - Calls `Serializer::serialize(delayed[i], m_wire_buf, SOCKET_RECV_BUF_BYTES,
         delayed_len)`.
       - Calls `send_to_all_clients(m_wire_buf, delayed_len)`.
11. Inside `send_to_all_clients()`:
    - Iterates over `m_client_fds[0..MAX_TCP_CONNECTIONS-1]` (bounded, 8 slots).
    - For each valid fd (`>= 0`): calls `m_sock_ops->send_frame(fd, buf, len, send_timeout_ms)`.
    - A failure on one fd logs WARNING_LO; iteration continues to the next fd.
12. `TcpBackend::send_message()` asserts `wire_len > 0` (post-condition) and returns `OK`.
13. Control returns to `send_via_transport()`, which returns `OK`.
14. Back in `DeliveryEngine::send()`:
    a. Reliability check: `env.reliability_class == BEST_EFFORT` so neither the ACK-tracking
       branch (`RELIABLE_ACK || RELIABLE_RETRY`) nor the retry branch (`RELIABLE_RETRY`) is
       entered.
    b. Two post-condition asserts fire: `env.source_id == m_local_id`,
       `env.message_id != 0`.
    c. Logs INFO: `"Sent message_id=..., reliability=0"`.
    d. Returns `OK`.
15. Caller receives `OK`.

---

## 4. Call Tree

```
DeliveryEngine::send(env, now_us)
 ├── MessageIdGen::next()                        [assigns env.message_id]
 └── DeliveryEngine::send_via_transport(env)
      └── TcpBackend::send_message(envelope)     [virtual dispatch]
           ├── Serializer::serialize(...)         [header + payload → m_wire_buf]
           ├── timestamp_now_us()                 [CLOCK_MONOTONIC]
           ├── ImpairmentEngine::process_outbound(envelope, now_us)
           └── TcpBackend::flush_delayed_to_clients(now_us)
                ├── ImpairmentEngine::collect_deliverable(now_us, ...)
                ├── Serializer::serialize(delayed[i], ...)
                └── TcpBackend::send_to_all_clients(buf, len)
                     └── ISocketOps::send_frame(fd, buf, len, timeout_ms)
                          └── [POSIX write/send syscall]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Orchestrates the send: stamps `source_id`, `message_id`,
  `timestamp_us`, and dispatches to the transport. Decides whether ACK tracking or retry
  scheduling is needed (neither for BEST_EFFORT).

- **`MessageIdGen`** — Monotonic counter seeded at `init()` from `local_id`. Generates the
  unique `message_id` for this message. Wraps at `UINT64_MAX`, skipping 0.

- **`TcpBackend`** — Concrete `TransportInterface` implementation over TCP. Owns the wire
  buffer (`m_wire_buf`, 8192 bytes), the impairment engine, and the receive queue. Handles
  all socket fd management.

- **`Serializer::serialize()`** — Static function. Converts a `MessageEnvelope` to a 44-byte
  big-endian header + opaque payload bytes in `m_wire_buf`. No heap use. Validates envelope
  and buffer size before writing.

- **`ImpairmentEngine::process_outbound()`** — Applies the configured impairment profile
  (loss, latency, jitter, duplication). For the zero-impairment case it places the message in
  the delay buffer with `release_us = now_us` so `collect_deliverable()` returns it
  immediately.

- **`ISocketOps::send_frame()`** — Abstracted POSIX socket write. In production,
  `SocketOpsImpl` issues a length-prefixed frame to the kernel. Can be mocked in tests.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `!m_initialized` | Return `ERR_INVALID` immediately | Continue send | Step 4 |
| `process_outbound()` returns `ERR_IO` (loss drop) | Return `OK` (silent drop) | Continue to flush | Return to caller |
| `m_client_count == 0` | Log WARNING_LO, return `OK` | Continue to flush | `flush_delayed_to_clients()` |
| `collect_deliverable()` count == 0 | Loop body in `flush_delayed_to_clients()` skipped | Loop executes | Return to `send_message()` |
| `send_frame()` fails on a client fd | Log WARNING_LO, continue to next fd | Next iteration | Continue fd loop |
| `reliability_class == RELIABLE_ACK \|\| RELIABLE_RETRY` | Enter ACK-tracking block | Skip tracking | Retry check |
| `reliability_class == RELIABLE_RETRY` | Enter retry-scheduling block | Skip retry | Post-condition asserts |

---

## 7. Concurrency / Threading Behavior

`DeliveryEngine` and `TcpBackend` carry no internal mutexes. The design assumes:

- **Single-threaded send path:** `send()` and `send_via_transport()` are called from exactly
  one application thread (the caller's thread). No locks are taken.

- **`RingBuffer` (`m_recv_queue`):** The receive queue inside `TcpBackend` is a SPSC ring
  buffer using `std::atomic<uint32_t>` with acquire/release memory ordering. The send path
  does not touch `m_recv_queue`; the ring buffer's atomics are not involved in UC_01.

- **`timestamp_now_us()`:** Calls `clock_gettime(CLOCK_MONOTONIC)`. Thread-safe on all POSIX
  targets.

- **`ImpairmentEngine`:** Not thread-safe; used exclusively on the send path (single thread).

- **`m_wire_buf`:** A member of `TcpBackend` (8192 bytes). Used serially: first by
  `Serializer::serialize()`, then by `send_to_all_clients()`. No concurrent access.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `env` (caller's) | Caller's stack | `sizeof(MessageEnvelope)` ≈ 4140 bytes | Passed by reference; `source_id`, `message_id`, `timestamp_us` stamped in-place |
| `m_wire_buf` | `TcpBackend` member | `SOCKET_RECV_BUF_BYTES` = 8192 bytes | Fixed at object construction; reused each `send_message()` call |
| `delayed[]` in `flush_delayed_to_clients` | Stack frame of that function | `IMPAIR_DELAY_BUF_SIZE * sizeof(MessageEnvelope)` = 32 × ~4140 ≈ 132 KB | Stack-allocated; Power of 10 Rule 3 — no heap |
| `m_slots` (AckTracker) | `AckTracker` member inside `DeliveryEngine` | `ACK_TRACKER_CAPACITY * sizeof(Entry)` = 32 entries | Not written for BEST_EFFORT |
| `m_slots` (RetryManager) | `RetryManager` member inside `DeliveryEngine` | `ACK_TRACKER_CAPACITY * sizeof(RetryEntry)` = 32 entries | Not written for BEST_EFFORT |

**Power of 10 Rule 3 confirmation:** No `malloc`, `new`, or equivalent dynamic allocation
occurs on any path exercised by UC_01. All storage is either static members of objects
created during `init()` or stack-allocated within bounded function frames.

**Object lifetimes:** `DeliveryEngine` and `TcpBackend` are expected to be stack- or
statically-allocated by the application. The `DeliveryEngine` holds a non-owning pointer to
`TcpBackend` (`m_transport`); the backend must outlive the engine.

---

## 9. Error Handling Flow

| Condition | System state after | What caller should do |
|---|---|---|
| `!m_initialized` → `ERR_INVALID` | No state changes; send aborted at guard | Call `init()` before retrying |
| `Serializer::serialize()` → `ERR_INVALID` | `wire_len` is 0; no socket write | Retry with a valid envelope |
| `process_outbound()` → `ERR_IO` (impairment drop) | Impairment counters may increment; no socket write | `OK` returned; caller sees success (fire-and-forget semantics) |
| `m_client_count == 0` | No socket write; WARNING_LO logged | Retry after a client connects |
| `send_frame()` → false on one fd | That fd not closed; WARNING_LO logged; other fds still attempted | Monitor logs; dead clients cleaned up on receive path via `remove_client_fd()` |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | POSIX monotonic clock | Called inside `timestamp_now_us()` inside `TcpBackend::send_message()`. |
| `ISocketOps::send_frame()` → `SocketOpsImpl::send_frame()` | TCP socket fd (int, created during `init()`) | Issues a length-prefixed write on the TCP connection. Timeout is `send_timeout_ms` from `ChannelConfig`. |

No file I/O, IPC, or hardware interaction occurs on the UC_01 send path.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|---|---|---|---|
| `MessageEnvelope& env` (caller's) | `source_id` | Caller-supplied (may be 0) | `m_local_id` |
| `MessageEnvelope& env` | `message_id` | Any | Non-zero monotonic counter value |
| `MessageEnvelope& env` | `timestamp_us` | Caller-supplied | `now_us` argument |
| `MessageIdGen m_id_gen` | `m_next` | N | N+1 (or 1 if N+1 == 0) |
| `TcpBackend m_wire_buf` | bytes [0..wire_len-1] | Stale previous content | Serialized wire frame of this message |
| Impairment engine delay buffer | one slot | Empty or stale | Holds envelope with `release_us = now_us`; drained in same call by `collect_deliverable()` |
| Kernel TCP send buffer (each client fd) | bytes | Previous content | Appended with length-prefixed serialized frame |

No persistent disk, database, or device state is modified.

---

## 12. Sequence Diagram

```
Caller         DeliveryEngine     MessageIdGen   TcpBackend        ImpairmentEngine  Serializer  ISocketOps
  |                  |                |               |                    |              |             |
  |--send(env,now)-->|                |               |                    |              |             |
  |                  |--next()------->|               |                    |              |             |
  |                  |<-message_id----|               |                    |              |             |
  |                  | (stamp env)    |               |                    |              |             |
  |                  |--send_via_transport(env)------->|                    |              |             |
  |                  |                |               |--serialize(env)---->|              |             |
  |                  |                |               |<--OK, wire_len------|              |             |
  |                  |                |               |--process_outbound(env,now)-------->|             |
  |                  |                |               |<--OK (queued in delay buf)---------|             |
  |                  |                |               |--flush_delayed_to_clients(now)--->|              |
  |                  |                |               |    collect_deliverable()---------->|              |
  |                  |                |               |    <--[envelope]------------------|              |
  |                  |                |               |    serialize(delayed[0])---------->|             |
  |                  |                |               |    <--OK, delayed_len-------------|             |
  |                  |                |               |    send_to_all_clients()-----------|----send_frame->|
  |                  |                |               |                    |              |  [TCP write]  |
  |                  |<--OK-----------|               |                    |              |             |
  |  (BEST_EFFORT: no ACK track, no retry schedule)   |                    |              |             |
  |<--OK-------------|                |               |                    |              |             |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (preconditions before UC_01 can execute):**
- `TcpBackend::init(config)` must have been called: creates the TCP socket (client:
  `connect_to_server()`; server: `bind_and_listen()`), sets `m_open = true`, initializes
  `m_recv_queue` and `m_impairment`.
- `DeliveryEngine::init(transport, cfg, local_id)` must have been called: sets `m_transport`,
  calls `m_ack_tracker.init()`, `m_retry_manager.init()`, `m_dedup.init()`,
  `m_id_gen.init(seed)`, sets `m_initialized = true`.

**Steady-state (UC_01 runtime):**
- Each call to `send()` is entirely self-contained. It reads and updates `m_id_gen.m_next`
  (monotonic counter), stamps three fields on the caller's envelope, calls through to the
  transport, and returns. No background threads or timers are involved.
- The impairment engine's delay buffer is both written (`process_outbound`) and drained
  (`flush_delayed_to_clients`) within a single `send_message()` call for the zero-latency
  case.

---

## 14. Known Risks / Observations

- **`m_wire_buf` is not thread-safe.** If two threads both call `send_message()` on the same
  `TcpBackend` instance, they would race on `m_wire_buf` and the `m_impairment` state. The
  design assumes a single caller thread per backend instance.

- **`flush_delayed_to_clients()` stack frame is large.** The `delayed[]` array is
  `IMPAIR_DELAY_BUF_SIZE * sizeof(MessageEnvelope) = 32 * ~4140 ≈ 132 KB` on the stack.
  See `docs/STACK_ANALYSIS.md` for platform headroom analysis.

- **Double serialization on zero-latency path.** `Serializer::serialize()` is called once
  before `process_outbound()` (result stored in `wire_len` but `m_wire_buf` content is
  immediately overwritten by `flush_delayed_to_clients()`) and again inside
  `flush_delayed_to_clients()` for the copy retrieved from the delay buffer. The first
  serialization into `m_wire_buf` is discarded by the second call. This is a minor
  inefficiency, not a correctness issue.

- **`m_client_count == 0` returns `OK`.** A caller cannot distinguish "sent successfully"
  from "no clients to send to" using the `Result` code alone.

- **BEST_EFFORT messages are silently dropped on transport errors.** There is no feedback
  mechanism to the caller beyond a WARNING_LO log entry. This is by design (REQ-3.3.1).

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `ImpairmentEngine::process_outbound()` returns `ERR_IO` specifically for a
  loss-dropped message. Inferred from the `if (res == Result::ERR_IO) { return Result::OK; }`
  guard in `TcpBackend::send_message()`.

- [ASSUMPTION] The impairment engine's `collect_deliverable()` returns messages in FIFO order
  for the zero-latency case.

- [ASSUMPTION] `SocketOpsImpl::send_frame()` issues a length-prefixed frame (REQ-6.1.5). The
  exact framing format (e.g., 4-byte big-endian length prefix) is implemented in `SocketUtils`
  which was not part of the source files read for this document.

- [ASSUMPTION] The `TcpBackend` instance is used by a single thread. No synchronization is
  present in the implementation; concurrent use would produce data races.

- [ASSUMPTION] `timestamp_now_us()` always succeeds on the target platform. The
  `NEVER_COMPILED_OUT_ASSERT(result == 0)` would trigger the registered `IResetHandler` on
  failure.
