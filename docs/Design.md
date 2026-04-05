# messageEngine — Software Design Document

**Version:** 1.0
**Date:** March 2026
**Classification:** Class C (NPR 7150.2D Appendix D)
**Standards:** JPL Power of 10, MISRA C++:2023, F-Prime style subset, NASA-STD-8719.13C

---

## Table of Contents

1. [Design Goals and Principles](#1-design-goals-and-principles)
2. [System Context](#2-system-context)
3. [Layered Architecture](#3-layered-architecture)
4. [Data Model](#4-data-model)
5. [Error Handling Model](#5-error-handling-model)
6. [Memory Model](#6-memory-model)
7. [Concurrency Model](#7-concurrency-model)
8. [Module Designs](#8-module-designs)
   - 8.1 [Core Layer](#81-core-layer)
   - 8.2 [Platform Layer](#82-platform-layer)
   - 8.3 [App Layer](#83-app-layer)
9. [State Machines](#9-state-machines)
10. [Wire Format](#10-wire-format)
11. [Impairment Pipeline](#11-impairment-pipeline)
12. [Safety Design](#12-safety-design)
13. [Testability Design](#13-testability-design)
14. [Compile-Time Capacity Constants](#14-compile-time-capacity-constants)
15. [Key Design Decisions and Rationale](#15-key-design-decisions-and-rationale)

---

## 1. Design Goals and Principles

### 1.1 Primary Goals

| Goal | Description |
|---|---|
| **Safety-critical correctness** | Every message delivery decision — deduplication, expiry, retry, loss — must be deterministic and verifiable. |
| **Realistic fault simulation** | The impairment engine must faithfully model real network conditions (latency, jitter, loss, duplication, reordering, partitions) with full reproducibility. |
| **Transport independence** | Application code must be completely insulated from whether the underlying link is TCP, UDP, or an in-process simulation. |
| **Static analyzability** | Code must be writable so that MISRA checkers, clang-tidy, and cppcheck can mechanically verify compliance without special instrumentation. |
| **Determinism under test** | Given the same seed and inputs, the entire message path — including impairment decisions — produces an identical, reproducible sequence. |

### 1.2 Design Principles

- **No surprises:** all errors are returned as `Result` enum values; nothing is swallowed silently.
- **No hidden state:** every piece of mutable state is owned explicitly by a named class; no globals except the signal flag in the demo server.
- **Fail loud, fail safe:** `NEVER_COMPILED_OUT_ASSERT` fires in every build; it never silently degrades.
- **Fixed resources:** all buffer sizes are compile-time constants; capacity overflow is a returned error, never undefined behavior.
- **Shallow call depth:** no recursion; worst-case call chain is 10 frames / ~764 bytes of stack (DTLS outbound send path; see `docs/STACK_ANALYSIS.md`).
- **Dependency injection over globals:** `DeliveryEngine` receives a `TransportInterface*` at `init()` time, enabling any backend — or a mock — to be substituted without changing core code.

---

## 2. System Context

```
                     ┌──────────────────────────────────────┐
                     │    Application (Server / Client)      │
                     │    src/app/Server.cpp                 │
                     │    src/app/Client.cpp                 │
                     └──────────────┬───────────────────────┘
                                    │
                     ┌──────────────▼───────────────────────┐
                     │         DeliveryEngine                │
                     │  • Assigns message IDs               │
                     │  • Applies reliability semantics      │
                     │  • Drives retry / ACK-timeout pump    │
                     └──────────────┬───────────────────────┘
                                    │ TransportInterface*
                     ┌──────────────▼───────────────────────┐
                     │   TcpBackend │ UdpBackend │ LocalSim  │
                     │   • Serialization (Serializer)        │
                     │   • Impairment simulation             │
                     │   • Inbound ring buffer               │
                     └──────────────┬───────────────────────┘
                                    │ POSIX sockets / in-process
                     ┌──────────────▼───────────────────────┐
                     │     Operating System / Network        │
                     └──────────────────────────────────────┘
```

The `DeliveryEngine` is the sole gateway between application policy and the transport. It never calls OS APIs directly; all I/O goes through the `TransportInterface` abstraction.

---

## 3. Layered Architecture

Dependencies flow strictly downward. No lower layer may include or call a higher layer; no cyclic dependencies are permitted between any two modules.

| Layer | Directory | Responsibility | Permitted Dependencies |
|---|---|---|---|
| **App** | `src/app/` | Main functions; demo programs; composes core + platform | Core, Platform |
| **Core** | `src/core/` | Message semantics, serialization, delivery, retry, dedup | `TransportInterface` (abstract), no OS APIs |
| **Platform** | `src/platform/` | Socket adapters, impairment engine, PRNG | POSIX OS APIs, Core types only |
| **OS** | system | Berkeley sockets, pthreads, POSIX clocks | — |

### 3.1 Layering Rules

- `src/core/` code may use `TransportInterface` (defined in core) but must never `#include` a concrete backend header (`TcpBackend.hpp`, `UdpBackend.hpp`, etc.).
- `src/platform/` code may `#include` core types (`Types.hpp`, `MessageEnvelope.hpp`, `Assert.hpp`, `Logger.hpp`) but must not include `DeliveryEngine.hpp`, `AckTracker.hpp`, or any core policy class.
- `src/app/` composes both layers; it is the only place where concrete backend types are instantiated.

---

## 4. Data Model

### 4.1 MessageEnvelope

The central data structure. Every message — DATA, ACK, NAK, or HEARTBEAT — is wrapped in a `MessageEnvelope` and flows through the entire stack unchanged.

| Field | Type | Size | Description |
|---|---|---|---|
| `message_type` | `MessageType` | 1 byte | DATA / ACK / NAK / HEARTBEAT / INVALID |
| `reliability_class` | `ReliabilityClass` | 1 byte | BEST_EFFORT / RELIABLE_ACK / RELIABLE_RETRY |
| `priority` | `uint8_t` | 1 byte | 0 = highest priority |
| *(padding)* | `uint8_t` | 1 byte | Reserved; must be 0 on wire |
| `message_id` | `uint64_t` | 8 bytes | Unique per sender; assigned by `MessageIdGen::next()` |
| `timestamp_us` | `uint64_t` | 8 bytes | Creation time (CLOCK_MONOTONIC microseconds) |
| `source_id` | `NodeId` (uint32_t) | 4 bytes | Sending node identifier |
| `destination_id` | `NodeId` (uint32_t) | 4 bytes | Receiving node identifier |
| `expiry_time_us` | `uint64_t` | 8 bytes | Drop-after time (microseconds); 0 = never expires |
| `payload_length` | `uint32_t` | 4 bytes | Bytes of valid data in `payload[]` |
| *(padding)* | `uint32_t` | 4 bytes | Reserved; must be 0 on wire |
| `payload` | `uint8_t[4096]` | 0–4096 bytes | Application data; opaque to all layers below App |

**Wire header total: 44 bytes.** Payload follows immediately. Maximum wire frame: 44 + 4096 = 4,140 bytes.

### 4.2 Key Enumerations

```
Result:           OK  ERR_TIMEOUT  ERR_FULL  ERR_EMPTY  ERR_INVALID
                  ERR_IO  ERR_DUPLICATE  ERR_EXPIRED  ERR_OVERRUN

MessageType:      DATA  ACK  NAK  HEARTBEAT  INVALID

ReliabilityClass: BEST_EFFORT  RELIABLE_ACK  RELIABLE_RETRY

Severity:         INFO  WARNING_LO  WARNING_HI  FATAL

TransportKind:    TCP  UDP  LOCAL_SIM
```

---

## 5. Error Handling Model

messageEngine uses an **explicit return-code model** — the F-Prime / MISRA approach. No C++ exceptions are used anywhere; the project is compiled with `-fno-exceptions`.

### 5.1 Rules

- Every non-void function that can fail returns a `Result`.
- The caller is required by Power of 10 Rule 7 to check every return value. Unchecked returns are explicitly cast to `(void)` with an accompanying justification comment.
- `ERR_IO` on the send path means a message was dropped (by impairment, socket error, or partition). For BEST_EFFORT this is expected and treated as a silent drop. For RELIABLE_RETRY, if the transport sends successfully but the network silently drops the packet (loss impairment, not ERR_IO), the retry manager will retransmit when the backoff timer fires. If `send_via_transport()` returns `ERR_IO` (socket error or partition), bookkeeping slots are rolled back by `rollback_on_transport_failure()` and no retry is scheduled — the caller receives `ERR_IO` and must decide whether to retry at the application level.
- `ERR_FULL` means a fixed-capacity buffer is at capacity; the caller must decide whether to wait, discard, or escalate.
- `ERR_DUPLICATE` is returned by `DuplicateFilter::check_and_record()` and by `DeliveryEngine::receive()` to indicate the message was already seen and should not be acted on.
- `ERR_EXPIRED` is returned by `DeliveryEngine::receive()` when the message's `expiry_time_us` has passed.

### 5.2 Logging Severity

| Severity | Meaning | Example |
|---|---|---|
| `INFO` | Normal lifecycle events | Connection established, harness initialized |
| `WARNING_LO` | Localized, recoverable | Message dropped by loss impairment, partition started |
| `WARNING_HI` | System-wide, recoverable | Delay buffer full, ACK tracker full |
| `FATAL` | Unrecoverable; component must restart | Assertion violation in production build |

The logger writes to `stderr` using a fixed 512-byte stack buffer. No heap allocation; no locking.

### 5.3 Assertion Strategy

`NEVER_COMPILED_OUT_ASSERT(cond)` is the only assertion macro used in production code. It is **never disabled** by `NDEBUG`.

| Build type | Behavior on failure |
|---|---|
| Debug / test | Logs condition + file + line, then calls `abort()` for immediate crash and stack trace |
| Production | Logs FATAL with condition + file + line, then triggers a controlled component reset |

Every function carries at least two assertions: one precondition and one postcondition or invariant.

---

## 6. Memory Model

### 6.1 No Dynamic Allocation After Init

The project strictly follows Power of 10 Rule 3. All buffers and tables are statically allocated at class scope:

| Structure | Location | Capacity |
|---|---|---|
| Message ring buffer | `RingBuffer::m_buf[]` | 64 × `MessageEnvelope` |
| ACK tracker table | `AckTracker::m_slots[]` | 32 slots |
| Retry manager table | `RetryManager::m_slots[]` | 32 slots |
| Duplicate filter window | `DuplicateFilter::m_window[]` | 128 `(NodeId, uint64_t)` pairs |
| Impairment delay buffer | `ImpairmentEngine::m_delay_buf[]` | 32 × `DelayEntry` |
| Impairment reorder buffer | `ImpairmentEngine::m_reorder_buf[]` | 32 × `MessageEnvelope` |
| TCP wire buffer | `TcpBackend::m_wire_buf[]` | 8,192 bytes |
| UDP wire buffer | `UdpBackend::m_wire_buf[]` | 8,192 bytes |
| Logger format buffer | `Logger` (stack) | 512 bytes per call |

`malloc`, `new`, and `delete` do not appear anywhere in `src/`.

### 6.2 Stack Usage

With no recursion, the call graph is a directed acyclic graph. The longest path is the outbound send chain:

```
main → run_client_iteration → send_test_message → DeliveryEngine::send
     → TcpBackend::send_message → Serializer::serialize
     → ImpairmentEngine::process_outbound → ImpairmentEngine::queue_to_delay_buf
```

**Worst case: 10 frames, ~764 bytes** (Chain 5 — DTLS outbound send, dominated by `payload_buf[256]`). Well within any POSIX default thread stack (≥ 8 MB). Full per-chain breakdown: `docs/STACK_ANALYSIS.md`.

---

## 7. Concurrency Model

### 7.1 TcpBackend Threading

`TcpBackend` spawns one dedicated **receiver thread** per accepted client connection. Each thread reads from its socket and pushes deserialized `MessageEnvelope` objects into the shared inbound `RingBuffer`.

The `RingBuffer` is a lock-free SPSC (single-producer, single-consumer) queue:
- The **receiver thread** is the sole producer (calls `push()`).
- The **application thread** is the sole consumer (calls `pop()` via `receive_message()`).

`std::atomic<uint32_t>` is used for the head/tail indices with `memory_order_acquire` / `memory_order_release` ordering, providing correct SPSC synchronization without any mutex.

> `std::atomic` is a documented carve-out from the F-Prime no-STL rule. It maps directly to a hardware primitive, has no dynamic allocation, is ISO C++17 standard, and is explicitly endorsed by MISRA C++:2023 for lock-free shared state.

### 7.2 UdpBackend Threading

Similar to `TcpBackend`: one background thread drives the `recvfrom` loop and pushes to the inbound `RingBuffer`.

### 7.3 LocalSimHarness

Fully single-threaded. No threads are created; `inject()` is called synchronously from the sender's thread context directly into the receiver's `RingBuffer`. Tests drive both sides from the same thread.

### 7.4 Thread Safety Guarantees

| Operation | Thread-safe? | Notes |
|---|---|---|
| `RingBuffer::push()` | Yes (producer side) | Exactly one producer thread |
| `RingBuffer::pop()` | Yes (consumer side) | Exactly one consumer thread |
| `DeliveryEngine::send()` | Single-threaded only | Called from application thread |
| `DeliveryEngine::receive()` | Single-threaded only | Called from application thread |
| `Logger::log()` | No internal locking | Interleaving from multiple threads may produce garbled lines |

---

## 8. Module Designs

### 8.1 Core Layer

#### 8.1.1 Types (`Types.hpp`)

Foundation header included by every file. Defines all enumerations (`Result`, `MessageType`, `ReliabilityClass`, `Severity`, `TransportKind`, `OrderingMode`), compile-time constants, the `NodeId` typedef, and the `result_ok()` inline helper.

No source file; header-only.

---

#### 8.1.2 MessageEnvelope (`MessageEnvelope.hpp`)

Plain-old-data struct plus a small set of free functions. No member functions, no inheritance, no constructors — consistent with the F-Prime POD style for embedded envelopes.

Key design choices:
- `payload[MSG_MAX_PAYLOAD_BYTES]` is a fixed inline array (no pointer, no heap). Copying a `MessageEnvelope` copies the payload.
- `envelope_valid()` is the single gate for envelope integrity — checked on every send and receive.
- `envelope_init()` zero-fills all fields to `INVALID` defaults; callers must set all meaningful fields explicitly.

---

#### 8.1.3 TransportInterface (`TransportInterface.hpp`)

Pure-virtual abstract base class. Defines the five-function contract: `init`, `send_message`, `receive_message`, `close`, `is_open`.

Virtual dispatch is the only use of virtual functions in the project. It is explicitly permitted by the MISRA C++:2023 carve-out for `TransportInterface` polymorphism (no explicit function pointer declarations; vtable is compiler-generated).

`DeliveryEngine` holds a `TransportInterface*` (not an object by value) enabling the concrete backend to be swapped at `init()` time — the primary dependency-injection seam for testing.

---

#### 8.1.4 RingBuffer (`RingBuffer.hpp`)

Lock-free SPSC queue. Design:
- Power-of-two capacity (`MSG_RING_CAPACITY = 64`) allows modular indexing with a bitmask.
- Occupancy is derived from `(m_tail - m_head)` using unsigned 32-bit arithmetic that wraps at 2^32 without explicit modulo — a standard lock-free queue idiom.
- `m_head` and `m_tail` are `std::atomic<uint32_t>`. The producer reads `m_head` with `acquire` and writes `m_tail` with `release`; the consumer does the reverse. This provides the minimal ordering needed for SPSC correctness.
- `init()` must be called before use; it zeroes both indices.

---

#### 8.1.5 Serializer (`Serializer.hpp / .cpp`)

Static-methods-only class (not instantiable). Deterministic big-endian serialization with manual bit-shift helpers (`write_u8`, `write_u32`, `write_u64`, `read_u8`, `read_u32`, `read_u64`).

Wire format choices:
- All multi-byte fields in network byte order (big-endian) for cross-platform compatibility.
- Manual bit shifts rather than `htons`/`ntohl` — eliminates implicit system-header dependency and makes byte order explicit in the code.
- Two padding fields (bytes 3 and 40–43) are validated as zero on deserialization; non-zero padding returns `ERR_INVALID`.
- `payload_length` is bounds-checked against `MSG_MAX_PAYLOAD_BYTES` before the payload copy.

Safety-critical status: `serialize` and `deserialize` are both tagged SC (HAZ-005).

---

#### 8.1.6 MessageIdGen (`MessageId.hpp / .cpp`)

Single 64-bit counter. `next()` pre-increments and skips zero (zero is `NODE_ID_INVALID`). Seed support allows deterministic test sequences. No atomics — called only from `DeliveryEngine::send()` on the application thread.

---

#### 8.1.7 Timestamp (`Timestamp.hpp / .cpp`)

Thin wrapper around `clock_gettime(CLOCK_MONOTONIC)`. Using `CLOCK_MONOTONIC` (not `CLOCK_REALTIME`) prevents wall-clock jumps from affecting expiry checks. Expiry is always evaluated relative to the time of the `receive()` call.

---

#### 8.1.8 DuplicateFilter (`DuplicateFilter.hpp / .cpp`)

Sliding-window FIFO of `DEDUP_WINDOW_SIZE = 128` entries. Each entry is a `(NodeId, uint64_t)` pair. When the window is full the oldest entry is evicted.

Design rationale:
- Window size 128 ≥ `MAX_RETRY_COUNT × MAX_PEER_NODES` under any realistic configuration; a duplicate arriving after the window has advanced would be an extremely old message and correctly dropped by the expiry check instead.
- `check_and_record()` is the single entry point — it atomically checks and records, preventing a TOCTOU race.

---

#### 8.1.9 AckTracker (`AckTracker.hpp / .cpp`)

Fixed-capacity table of `ACK_TRACKER_CAPACITY = 32` slots, each in one of three states: `FREE → PENDING → ACKED`. See [§9 State Machines](#9-state-machines).

Design rationale for the sweep model:
- `on_ack()` marks a slot `ACKED` but does not free it immediately. Freeing is deferred to `sweep_expired()`. This avoids modifying the count from two different call sites and simplifies the invariant proof.
- `sweep_expired()` must be called by the application on each main-loop iteration. This is documented as an API contract; failure to call it causes ACK slots to accumulate.

---

#### 8.1.10 RetryManager (`RetryManager.hpp / .cpp`)

Fixed-capacity table of `ACK_TRACKER_CAPACITY = 32` retry slots with exponential backoff. See [§9 State Machines](#9-state-machines).

Backoff design:
- Initial interval: configured `backoff_ms` per channel (default 100 ms).
- Doubles on every retry: 100 → 200 → 400 → 800 → 1600 ms.
- Capped at 60,000 ms (1 minute) to bound worst-case retry interval.
- First retry fires immediately (`next_retry_us = now_us` at schedule time).

---

#### 8.1.11 DeliveryEngine (`DeliveryEngine.hpp / .cpp`)

The orchestrator of the core layer. Owns by-value instances of `AckTracker`, `RetryManager`, `DuplicateFilter`, and `MessageIdGen`. Holds a `TransportInterface*` injected at `init()`.

Send path:
1. Assign `message_id` via `MessageIdGen::next()`.
2. Stamp `source_id = local_node_id`.
3. If `RELIABLE_ACK`: call `AckTracker::track()`.
4. If `RELIABLE_RETRY`: call `RetryManager::schedule()`.
5. Call `transport->send_message(env)`.

Receive path:
1. Call `transport->receive_message(env, timeout_ms)`.
2. Check `timestamp_expired(env.expiry_time_us, now_us)` → drop if expired.
3. If ACK/NAK: notify `AckTracker::on_ack()` and `RetryManager::on_ack()`.
4. If DATA + `RELIABLE_RETRY`: call `DuplicateFilter::check_and_record()` → drop if duplicate.
5. Return envelope to caller.

---

### 8.2 Platform Layer

#### 8.2.1 PrngEngine (`PrngEngine.hpp / .cpp`)

xorshift64 PRNG. Chosen for:
- Minimal state (one `uint64_t`).
- No dynamic allocation.
- Deterministic: same seed always yields the same sequence.
- Sufficient statistical quality for impairment simulation (not cryptographic).

`next_double()` maps the 64-bit output to `[0.0, 1.0)` using division by 2^64. `next_range(lo, hi)` computes `lo + (next() % (hi - lo + 1))` with a bounded loop to reject bias — consistent with Power of 10 fixed-bound requirements.

---

#### 8.2.2 ImpairmentEngine (`ImpairmentEngine.hpp / .cpp`)

Network fault injection layer. Logically sits between the transport abstraction and the socket. Applied on both outbound (send) and inbound (receive/reorder) paths.

**Outbound pipeline order:**

```
process_outbound(env, now_us)
    │
    ├─ [partition active?] → YES → return ERR_IO (drop)
    │
    ├─ [loss check] → random < loss_probability → return ERR_IO (drop)
    │
    ├─ calculate release_us = now_us + fixed_latency_ms*1000 + jitter_us
    │
    ├─ [delay buffer full?] → YES → return ERR_FULL
    │
    ├─ queue_to_delay_buf(env, release_us)
    │
    └─ [duplication_probability > 0] → apply_duplication(env, release_us)
```

**Collect path:**

`collect_deliverable(now_us, out_buf, cap)` scans all 32 delay buffer slots and returns those with `release_us ≤ now_us`. This is how latency/jitter is realized: the transport backend calls `collect_deliverable` on each send/receive cycle and injects ready messages into the peer's ring buffer.

**Inbound reordering:**

`process_inbound()` maintains a separate `reorder_buf[]` window. Messages are buffered until the window fills; when full, one is randomly selected and released, the rest shift down, and the new message is added. This models out-of-order arrival without creating spurious duplicates.

---

#### 8.2.3 SocketUtils (`SocketUtils.hpp / .cpp`)

Thin POSIX adapter layer. All functions return `bool` (success/failure) or a signed file descriptor. No `MessageEnvelope` knowledge — operates entirely on raw byte buffers.

TCP framing design:
- `tcp_send_frame()` prepends a 4-byte big-endian length prefix before the payload bytes.
- `tcp_recv_frame()` reads the 4-byte prefix, validates the declared length against a cap, then calls `socket_recv_exact()` to read exactly that many bytes.
- This handles TCP's stream nature: one `send()` does not guarantee one `recv()`.

All send/receive operations use `poll()` with a caller-supplied timeout, preventing indefinite blocking.

---

#### 8.2.4 TcpBackend (`TcpBackend.hpp / .cpp`)

`TransportInterface` implementation over TCP/IP. Two operating modes:

**Server mode** (`is_server = true`):
- Binds to `bind_ip:bind_port`, listens.
- Accepts up to `MAX_TCP_CONNECTIONS = 8` clients.
- Each accepted connection spawns a POSIX thread that loops: `tcp_recv_frame → Serializer::deserialize → RingBuffer::push`.
- `send_message()` serializes the envelope and sends to all connected clients.

**Client mode** (`is_server = false`):
- Connects to `peer_ip:peer_port` with a configurable timeout.
- One background thread handles the receive loop.
- `send_message()` serializes and sends to the single server socket.

Both modes interpose the `ImpairmentEngine` on the send path.

---

#### 8.2.5 UdpBackend (`UdpBackend.hpp / .cpp`)

`TransportInterface` implementation over UDP datagrams. Single socket bound to `bind_ip:bind_port`. Sends serialized envelopes as individual datagrams to `peer_ip:peer_port`.

One background thread loops on `recvfrom`, deserializes, and pushes to the inbound `RingBuffer`. No framing is needed since UDP preserves message boundaries.

No connection management; reliability is handled entirely by the `DeliveryEngine` above (retry, ACK, dedup).

---

#### 8.2.6 LocalSimHarness (`LocalSimHarness.hpp / .cpp`)

In-process simulation transport. Two instances are linked via `link(peer)`. When A calls `send_message(env)`:

1. `ImpairmentEngine::process_outbound()` runs (may drop or delay).
2. `ImpairmentEngine::collect_deliverable()` retrieves any delayed messages.
3. Collected messages are injected into `peer->m_recv_queue` via `peer->inject()`.
4. The main envelope is also injected into the peer's queue.

No OS sockets; no threads; fully synchronous. `receive_message()` polls the `RingBuffer` with nanosleep iterations up to `timeout_ms` milliseconds.

Primary use: deterministic unit and integration tests.

---

### 8.3 App Layer

#### 8.3.1 Server (`src/app/Server.cpp`)

TCP echo server. Main loop (bounded to 100,000 iterations):

```
while (!g_stop_flag && iteration < MAX_ITERATIONS):
    receive_message(env, RECV_TIMEOUT_MS)
    if DATA: send_echo_reply(env)
    pump_retries(now_us)
    sweep_ack_timeouts(now_us)
```

`SIGINT` sets `g_stop_flag` (a `volatile sig_atomic_t`) for clean shutdown. The `signal()` call is a documented MISRA deviation — the only way to register a signal handler in a POSIX-compliant, no-STL codebase.

#### 8.3.2 Client (`src/app/Client.cpp`)

TCP client. Sends five `RELIABLE_RETRY` messages and waits up to 2 seconds for each echo reply. Demonstrates the full round-trip: `DeliveryEngine::send → TcpBackend → network → TcpBackend → DeliveryEngine::receive`.

---

## 9. State Machines

### 9.1 AckTracker Slot State Machine

```
         track() [count < capacity]
FREE ──────────────────────────────► PENDING
 ▲                                      │  │
 │  sweep_expired() [expired]           │  │ on_ack()
 │◄──────────────────────────────────────┘  │
 │                                          ▼
 │            sweep_expired()             ACKED
 └◄──────────────────────────────────────────
```

| Transition | Guard | Action |
|---|---|---|
| FREE → PENDING | `m_count < ACK_TRACKER_CAPACITY` | Copy envelope; set deadline; increment count |
| FREE → FREE | `m_count == ACK_TRACKER_CAPACITY` | Return `ERR_FULL`; log `WARNING_HI` |
| PENDING → ACKED | ACK received with matching `(src, msg_id)` | — |
| PENDING → FREE | `now_us >= deadline_us` | Copy to expired buf; decrement count |
| ACKED → FREE | Any `sweep_expired()` call | Decrement count |

**Invariant:** `0 ≤ m_count ≤ ACK_TRACKER_CAPACITY` at all times.

---

### 9.2 RetryManager Slot State Machine

```
                    schedule()
INACTIVE ─────────────────────────► WAITING
    ▲                                  │  │
    │                                  │  │ next_retry_us ≤ now_us
    │  on_ack() OR exhausted           │  │  AND count < max_retries
    │  OR expired                      │  ▼
    └──────────────────────────────── DUE
                                       │
                                       │ collect_due() → buf full
                                       └── (stays DUE until next sweep)
```

**Backoff progression:** initial_ms → ×2 → ×2 → … → cap at 60,000 ms.

**Invariants:**
- `backoff_ms` is monotonically non-decreasing per slot.
- A slot reaches `INACTIVE` only through ACK, exhaustion, or expiry.
- Every `DUE` slot is eventually reaped (bounded by `max_retries` and `expiry_us`).

---

### 9.3 ImpairmentEngine Partition State Machine

```
              first call with partition_enabled
UNINIT ────────────────────────────────────────► NO_PARTITION
                                                     │  ▲
          now_us >= m_next_partition_event_us         │  │ now_us >= m_next_partition_event_us
          (gap elapsed → start partition)             ▼  │ (duration elapsed → end partition)
                                              PARTITION_ACTIVE
```

| State | Meaning |
|---|---|
| `NO_PARTITION` | Link up; messages pass through loss / latency / jitter / duplication pipeline |
| `PARTITION_ACTIVE` | All outbound messages dropped with `ERR_IO`; logged at `WARNING_LO` |

`is_partition_active(now_us)` drives all transitions. It is called as the **first operation** in `process_outbound()`, ensuring no message bypasses the partition check.

---

### 9.4 Delay Buffer Slot Sub-State

| Sub-state | Condition | Transition |
|---|---|---|
| `SLOT_FREE` | `!active` | → `SLOT_HELD` on `queue_to_delay_buf()` |
| `SLOT_HELD` | `active AND release_us > now_us` | → `SLOT_READY` when time advances |
| `SLOT_READY` | `active AND release_us ≤ now_us` | → `SLOT_FREE` on `collect_deliverable()` |

**Invariant:** `m_delay_count` always equals the number of slots with `active == true`.

---

## 10. Wire Format

```
Offset   Size   Field
──────   ────   ────────────────────────────────
0        1      message_type          (uint8, big-endian)
1        1      reliability_class     (uint8)
2        1      priority              (uint8)
3        1      padding1              (must be 0x00)
4        8      message_id            (uint64, big-endian)
12       8      timestamp_us          (uint64, big-endian)
20       4      source_id             (uint32, big-endian)
24       4      destination_id        (uint32, big-endian)
28       8      expiry_time_us        (uint64, big-endian)
36       4      payload_length        (uint32, big-endian)
40       4      padding2              (must be 0x00000000)
44       N      payload               (N = payload_length, 0 ≤ N ≤ 4096)
```

All multi-byte fields are **big-endian (network byte order)**. The Serializer uses manual bit shifts rather than `htons`/`ntohl` for explicit, platform-independent control.

Deserialization validates:
- `buf_len >= WIRE_HEADER_SIZE (44)` — rejects truncated headers.
- `padding1 == 0` and `padding2 == 0` — rejects malformed frames.
- `payload_length <= MSG_MAX_PAYLOAD_BYTES` — rejects oversized payloads.
- Total frame length `WIRE_HEADER_SIZE + payload_length <= buf_len` — rejects truncated payloads.

---

## 11. Impairment Pipeline

Detailed processing order for a single outbound message:

```
process_outbound(env, now_us)
│
├─ 1. GUARD: envelope_valid(env) [NEVER_COMPILED_OUT_ASSERT]
│
├─ 2. DISABLED CHECK: if !cfg.enabled
│       └─ queue_to_delay_buf(env, now_us)  [immediate release]
│           return OK
│
├─ 3. PARTITION CHECK: is_partition_active(now_us)
│       └─ YES → log WARNING_LO; return ERR_IO
│
├─ 4. LOSS CHECK: check_loss()  [PRNG roll < loss_probability]
│       └─ YES → log WARNING_LO; return ERR_IO
│
├─ 5. LATENCY + JITTER:
│       base_delay_us = fixed_latency_ms * 1000
│       if jitter_mean_ms > 0:
│           jitter_us = PRNG.next_range(0, jitter_variance_ms) * 1000
│       release_us = now_us + base_delay_us + jitter_us
│
├─ 6. BUFFER FULL CHECK: m_delay_count >= IMPAIR_DELAY_BUF_SIZE
│       └─ YES → log WARNING_HI; return ERR_FULL
│
├─ 7. QUEUE: queue_to_delay_buf(env, release_us)
│
└─ 8. DUPLICATION: if cfg.duplication_probability > 0
        └─ apply_duplication(env, release_us)
               PRNG roll < dup_probability AND buffer has space:
               → queue_to_delay_buf(env, release_us + 100µs)
```

---

## 12. Safety Design

### 12.1 Hazard Summary

| ID | Hazard | Severity | Primary Mitigation |
|---|---|---|---|
| HAZ-001 | Wrong-node delivery | Cat II | `envelope_valid()` on every send/receive; Serializer field validation |
| HAZ-002 | Retry storm | Cat II | `max_retries` enforced; exponential backoff capped at 60 s; `ACK_TRACKER_CAPACITY` hard limit |
| HAZ-003 | Duplicate execution | Cat II | `DuplicateFilter::check_and_record()` on every RELIABLE_RETRY receive |
| HAZ-004 | Stale message delivery | Cat II | `timestamp_expired()` checked before every `receive()` return |
| HAZ-005 | Silent data corruption | Cat I | Serializer bounds-check on every field; padding validation; `ERR_INVALID` on any violation |
| HAZ-006 | Resource exhaustion | Cat II | `ERR_FULL` returned and logged at `WARNING_HI`; all capacity limits are compile-time constants |
| HAZ-007 | Partition masking | Cat III | `is_partition_active()` called first in every `process_outbound()` |

### 12.2 Safety-Critical Functions

Functions directly on the send/receive/expiry/dedup/retry/impairment path are tagged `// Safety-critical (SC): HAZ-NNN` in their header declarations. SC functions include:

- `Serializer::serialize()` / `deserialize()` — HAZ-005
- `DeliveryEngine::send()` / `receive()` / `pump_retries()` / `sweep_ack_timeouts()` — HAZ-001..006
- `DuplicateFilter::check_and_record()` — HAZ-003
- `AckTracker::track()` / `on_ack()` / `sweep_expired()` — HAZ-002, HAZ-006
- `RetryManager::schedule()` / `on_ack()` / `collect_due()` — HAZ-002
- `ImpairmentEngine::process_outbound()` / `collect_deliverable()` / `process_inbound()` / `is_partition_active()` — HAZ-002..007
- `RingBuffer::push()` / `pop()` — HAZ-001, HAZ-004, HAZ-006

---

## 13. Testability Design

### 13.1 Transport Injection

`DeliveryEngine` receives a `TransportInterface*` at `init()` time. Test code can provide a `LocalSimHarness` instead of a `TcpBackend`, exercising all core-layer logic without OS sockets or real network I/O.

### 13.2 LocalSimHarness

Designed specifically for deterministic testing:
- `inject(env)` directly pushes an envelope into the harness's receive queue — enabling test setup without going through `send_message`.
- `link(peer)` wires two harnesses together for round-trip tests.
- Supports impairment simulation (if enabled through `TransportConfig`).
- No threads; no kernel involvement; fully synchronous.

### 13.3 PrngEngine Seeding

Every impairment decision is driven by `PrngEngine`. Tests pass a known seed at `ImpairmentConfig::prng_seed` to produce a fully reproducible fault sequence.

### 13.4 Test Suite

The project has 22 test binaries. The stress test (`test_stress_capacity`) is run separately from the main suite (`make run_stress_tests`); all others run under `make run_tests`.

| Test Binary | Module Under Test | Tests |
|---|---|---|
| `test_MessageEnvelope` | `MessageEnvelope` helpers | 5 |
| `test_MessageId` | `MessageId` / `MessageIdGen` | 4 |
| `test_Timestamp` | `Timestamp` / expiry logic | 6 |
| `test_Serializer` | `Serializer` | 17 |
| `test_DuplicateFilter` | `DuplicateFilter` | 5 |
| `test_AckTracker` | `AckTracker` | 14 |
| `test_RetryManager` | `RetryManager` | 21 |
| `test_DeliveryEngine` | `DeliveryEngine` | 57 |
| `test_Fragmentation` | `Fragmentation` helpers | 6 |
| `test_ReassemblyBuffer` | `ReassemblyBuffer` | 7 |
| `test_OrderingBuffer` | `OrderingBuffer` | 7 |
| `test_RequestReplyEngine` | `RequestReplyEngine` | 14 |
| `test_AssertState` | `AssertState` / `NEVER_COMPILED_OUT_ASSERT` | 8 |
| `test_ImpairmentEngine` | `ImpairmentEngine` | 24 |
| `test_ImpairmentConfigLoader` | `ImpairmentConfigLoader` | 24 |
| `test_SocketUtils` | `SocketUtils` (POSIX helpers) | 22 |
| `test_TcpBackend` | `TcpBackend` | 28 |
| `test_UdpBackend` | `UdpBackend` | 20 |
| `test_TlsTcpBackend` | `TlsTcpBackend` | 34 |
| `test_DtlsUdpBackend` | `DtlsUdpBackend` | 39 |
| `test_LocalSim` | `LocalSimHarness` | 11 |
| `test_stress_capacity` | Stress: capacity limits under load | 8 |

### 13.5 Coverage Policy

LLVM source-based branch coverage (`-fprofile-instr-generate -fcoverage-mapping`) is used.

SC functions (`DeliveryEngine::send`, `DeliveryEngine::receive`, `DuplicateFilter::check_and_record`, `Serializer::serialize`, `Serializer::deserialize`) are priority targets requiring verified branch coverage. The Serializer.cpp ceiling of 74.36% is an architectural limit caused by 20 permanently-uncoverable `NEVER_COMPILED_OUT_ASSERT` fire-branches (the `abort()` path cannot be executed without crashing the test process).

---

## 14. Compile-Time Capacity Constants

All fixed-size resource limits are defined in `src/core/Types.hpp` as `static const uint32_t`. They cannot be changed at runtime.

| Constant | Value | Controls |
|---|---|---|
| `MSG_MAX_PAYLOAD_BYTES` | 4,096 | Maximum application payload per message |
| `MSG_RING_CAPACITY` | 64 | Inbound `RingBuffer` depth per backend |
| `DEDUP_WINDOW_SIZE` | 128 | `DuplicateFilter` sliding window |
| `ACK_TRACKER_CAPACITY` | 32 | Max outstanding unACKed messages; also `RetryManager` slot count |
| `MAX_RETRY_COUNT` | 5 | Default maximum retry attempts per message |
| `MAX_CHANNELS` | 8 | Max logical channels per transport |
| `MAX_PEER_NODES` | 16 | Max peer node entries |
| `IMPAIR_DELAY_BUF_SIZE` | 32 | Impairment delay buffer slots (latency/jitter/duplication) |
| `MAX_TCP_CONNECTIONS` | 8 | Max simultaneous TCP clients in server mode |
| `SOCKET_RECV_BUF_BYTES` | 8,192 | Wire receive buffer (≥ 44 + 4096 = 4,140 required) |
| `NODE_ID_INVALID` | 0 | Sentinel value; never a valid `NodeId` |

---

## 15. Key Design Decisions and Rationale

### 15.1 Result enum over exceptions

**Decision:** All errors are communicated via `Result` return values. No C++ exceptions. Compiled with `-fno-exceptions`.

**Rationale:** MISRA C++:2023 and F-Prime both prohibit exceptions in safety-critical software. Exceptions create invisible control flow paths that defeat static analysis. `Result` values are explicit, checkable (Power of 10 Rule 7 mandates it), and produce no hidden stack unwinding.

---

### 15.2 No STL in production code

**Decision:** No `std::vector`, `std::string`, `std::map`, or any STL container or algorithm in `src/`. The sole exception is `std::atomic<uint32_t>` in `RingBuffer`.

**Rationale:** STL implementations pull in significant complexity that cannot be audited or statically analysed to MISRA/Power-of-10 depth on embedded targets. Fixed-size arrays and hand-rolled data structures give full visibility into every allocation and access. `std::atomic` is carve-out because: (a) it maps directly to a hardware primitive with no dynamic allocation, (b) it is ISO C++17 standard and portable, (c) MISRA C++:2023 explicitly endorses it for lock-free shared state.

---

### 15.3 Static allocation only after init

**Decision:** All buffers are statically allocated at class scope. `malloc`/`new`/`delete` do not appear in `src/`.

**Rationale:** Power of 10 Rule 3. Dynamic allocation in safety-critical systems introduces unbounded timing, fragmentation risk, and `nullptr` return paths. Static sizing makes worst-case memory use provable at compile time.

---

### 15.4 NEVER_COMPILED_OUT_ASSERT over standard assert()

**Decision:** `NEVER_COMPILED_OUT_ASSERT(cond)` is used instead of `assert()`. It is never disabled by `NDEBUG`.

**Rationale:** JPL Power of 10 Rule 5 requires assertions to remain active in all builds. Standard `assert()` is eliminated in production builds by `-DNDEBUG`, which would remove the safety net entirely. The custom macro keeps assertions in every build while adapting its response: crash-and-trace in debug/test, controlled reset in production.

---

### 15.5 Separate delay buffer from inbound ring buffer

**Decision:** The `ImpairmentEngine` has its own 32-slot delay buffer (`m_delay_buf`). Delayed messages are not placed directly into the transport's inbound `RingBuffer`.

**Rationale:** The ring buffer is an SPSC structure with a fixed consumer (the application). Inserting time-delayed messages from the impairment engine would violate the single-producer contract. The delay buffer is internal to the impairment engine and collected synchronously by the transport on each send/receive cycle.

---

### 15.6 Virtual dispatch for TransportInterface

**Decision:** `TransportInterface` uses C++ virtual functions. All concrete backends implement it via public inheritance.

**Rationale:** This is the only architecture that satisfies all three constraints simultaneously: (a) `DeliveryEngine` must not know the concrete transport type, (b) no function pointers in application code (Power of 10 Rule 9), (c) no templates. Virtual dispatch via compiler-generated vtables produces no explicit function pointer declarations in application code and is explicitly endorsed by MISRA C++:2023.

---

*End of Design.md*
