# messageEngine — Class Architecture

**Version 1.0 | March 2026**
*Safety-critical C++ networking library*

---

## Table of Contents

1. [Purpose and Scope](#1-purpose-and-scope)
2. [Layered Architecture](#2-layered-architecture)
3. [Core Layer Classes](#3-core-layer-classes)
4. [Platform Layer Classes](#4-platform-layer-classes)
5. [App Layer](#5-app-layer)
6. [Key Object Relationships](#6-key-object-relationships)
7. [Key Data Flows](#7-key-data-flows)
8. [Dependency Matrix](#8-dependency-matrix)
9. [Coding Standard Compliance Summary](#9-coding-standard-compliance-summary)

---

## 1. Purpose and Scope

**messageEngine** is a safety-critical C++ networking library that models realistic communication problems — latency, jitter, packet loss, duplication, reordering, and link partitions — while providing consistent, reusable messaging utilities across three transport backends (TCP, UDP, and an in-process simulation harness).

All production code complies with **JPL Power of 10**, **MISRA C++**, and an **F-Prime style subset**: no exceptions, no STL, no templates, no RTTI, and no dynamic allocation on critical paths after initialisation.

---

## 2. Layered Architecture

Dependencies flow downward only — no lower layer may reference a higher one, and no cyclic dependencies are permitted.

| Layer | Directory | Responsibility |
|---|---|---|
| **App Layer** | `src/app/` | Application orchestration and demo programs. Composes core + platform objects; contains `main()`. |
| **Core Layer** | `src/core/` | Transport-agnostic message layer. Envelope definition, serialization, delivery semantics, ACK tracking, retry, and duplicate suppression. No OS APIs — depends only on abstract interfaces. |
| **Platform Layer** | `src/platform/` | OS and socket adapters. TCP, UDP, and in-process transports; impairment engine. Implements `TransportInterface`; no application policy. |

### 2.1 Dependency Diagram

```
┌─────────────────────────────────────────────────────┐
│         App Layer  (src/app/)   Server · Client      │
└─────────────────────┬───────────────────────────────┘
                      │  uses
┌─────────────────────▼───────────────────────────────┐
│  Core Layer  (src/core/)                             │
│  DeliveryEngine · Serializer · AckTracker · …        │
└─────────────────────┬───────────────────────────────┘
                      │  implements TransportInterface
┌─────────────────────▼───────────────────────────────┐
│  Platform Layer  (src/platform/)                     │
│  TcpBackend · UdpBackend · LocalSimHarness           │
│  ImpairmentEngine                                    │
└─────────────────────┬───────────────────────────────┘
                      │  POSIX sockets / OS APIs
┌─────────────────────▼───────────────────────────────┐
│  OS  (Berkeley sockets, POSIX clocks, pthreads)      │
└─────────────────────────────────────────────────────┘
```

Dependency direction: App → Core → Platform → OS/sockets. The Core layer depends only on `TransportInterface` (an abstract class it defines) and never on concrete backend implementations.

---

## 3. Core Layer Classes

### `Types`
**Files:** `Types.hpp`

Shared enumerations, compile-time constants, and `NodeId` typedef. Foundation for the entire codebase.

| | |
|---|---|
| **Key API** | `enum class Result` (OK, ERR_TIMEOUT, ERR_FULL, …) |
| | `enum class Severity` (INFO, WARNING_LO, WARNING_HI, FATAL) |
| | `enum class MessageType` (DATA, ACK, NAK, HEARTBEAT, INVALID) |
| | `enum class ReliabilityClass` (BEST_EFFORT, RELIABLE_ACK, RELIABLE_RETRY) |
| | `enum class TransportKind` (TCP, UDP, LOCAL_SIM) |
| | Constants: `MSG_MAX_PAYLOAD_BYTES`, `MSG_RING_CAPACITY`, `DEDUP_WINDOW_SIZE`, … |
| **Depends on** | — (no dependencies) |
| **Used by** | All files |

---

### `Logger`
**Files:** `Logger.hpp`

Header-only, heap-free static logger. Formats severity-tagged lines and writes to `stderr` using a fixed 512-byte stack buffer.

| | |
|---|---|
| **Key API** | `Logger::log(Severity, const char* module, const char* fmt, …)` |
| **Depends on** | `Types.hpp` |
| **Used by** | All layers |

---

### `MessageEnvelope`
**Files:** `MessageEnvelope.hpp`

The canonical wire envelope struct. Carries message type, unique ID, timestamps, source/destination node IDs, priority, reliability class, expiry time, and an opaque payload (up to 4096 bytes).

| | |
|---|---|
| **Key API** | `struct MessageEnvelope { … uint8_t payload[4096]; }` |
| | `envelope_init(env)` |
| | `envelope_copy(dst, src)` |
| | `envelope_valid(env) → bool` |
| | `envelope_make_ack(ack, original, my_id, now_us)` |
| | `envelope_is_data(env)` / `envelope_is_control(env)` |
| **Depends on** | `Types.hpp` |
| **Used by** | Every class that sends or receives messages |

---

### `TransportInterface`
**Files:** `TransportInterface.hpp`

Pure-virtual abstract base class defining the single transport API. All concrete backends implement this; the core and app layers use only this interface — never raw sockets.

| | |
|---|---|
| **Key API** | `virtual Result init(const TransportConfig&)` |
| | `virtual Result send_message(const MessageEnvelope&)` |
| | `virtual Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `virtual void close()` |
| | `virtual bool is_open() const` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp`, `ChannelConfig.hpp` |
| **Used by** | `DeliveryEngine`, `TcpBackend`, `UdpBackend`, `LocalSimHarness` |

---

### `ChannelConfig` / `TransportConfig`
**Files:** `ChannelConfig.hpp`

POD configuration structs. `ChannelConfig` holds per-channel settings (priority, reliability mode, queue depth, timeouts). `TransportConfig` holds transport-level settings (kind, bind IP/port, peer address, node ID, channel array).

| | |
|---|---|
| **Key API** | `struct ChannelConfig { channel_id, priority, reliability, … }` |
| | `struct TransportConfig { kind, bind_ip, bind_port, channels[], … }` |
| | `transport_config_default(cfg)` |
| | `channel_config_default(cfg, id)` |
| **Depends on** | `Types.hpp` |
| **Used by** | `TransportInterface`, `DeliveryEngine`, all backends |

---

### `RingBuffer`
**Files:** `RingBuffer.hpp`

Header-only, statically allocated, power-of-two SPSC (single-producer, single-consumer) lock-free FIFO for `MessageEnvelope`. Capacity is fixed at `MSG_RING_CAPACITY` (64). Used by backends as an inbound message queue.

**Memory ordering:** Uses `std::atomic<uint32_t>` with `std::memory_order_acquire` / `std::memory_order_release` to provide correct SPSC ordering. `m_count` has been eliminated; occupancy is derived from `(m_tail - m_head)` using unsigned 32-bit arithmetic, which wraps correctly at 2^32. `std::atomic` is an explicit carve-out from the F-Prime no-STL rule: it is ISO standard (C++11/17), has no dynamic allocation, maps directly to a hardware primitive, and is portable across all conforming toolchains — satisfying F-Prime's portability principle and endorsed by MISRA C++:2023.

**Thread-safety contract:**
- Exactly one thread calls `push()` / `full()` (producer role).
- Exactly one thread calls `pop()` / `peek()` / `empty()` (consumer role).
- `size()` is an approximate snapshot safe to call from either side for diagnostics only.

| | |
|---|---|
| **Key API** | `void init()` |
| | `Result push(const MessageEnvelope&)` — producer only |
| | `Result pop(MessageEnvelope&)` — consumer only |
| | `Result peek(MessageEnvelope&)` — consumer only |
| | `bool empty()` / `bool full()` / `uint32_t size()` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp`, `<atomic>` (permitted carve-out) |
| **Used by** | `TcpBackend`, `UdpBackend`, `LocalSimHarness` |

---

### `Serializer`
**Files:** `Serializer.hpp`, `Serializer.cpp`

Static-only class providing deterministic big-endian (network byte order) serialization and deserialization of `MessageEnvelope`. Uses manual bit-shift I/O — no `htons`/`ntohl` — for explicit endian control. Wire header is always 52 bytes.

| | |
|---|---|
| **Key API** | `static const uint32_t WIRE_HEADER_SIZE = 52` |
| | `static Result serialize(env, buf, buf_len, out_len)` |
| | `static Result deserialize(buf, buf_len, env)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp` |
| **Used by** | `TcpBackend`, `UdpBackend` |

---

### `MessageIdGen`
**Files:** `MessageId.hpp`, `MessageId.cpp`

Monotonically incrementing 64-bit message ID generator. Never returns 0 (reserved as invalid). Supports seeding for deterministic test sequences.

| | |
|---|---|
| **Key API** | `void init(uint64_t seed)` |
| | `uint64_t next()` |
| **Depends on** | `Types.hpp` |
| **Used by** | `DeliveryEngine` |

---

### `Timestamp` (free functions)
**Files:** `Timestamp.hpp`, `Timestamp.cpp`

Inline utility functions for monotonic timestamps (`CLOCK_MONOTONIC`). Used for message creation times, retry deadlines, and expiry checking.

| | |
|---|---|
| **Key API** | `uint64_t timestamp_now_us()` |
| | `bool timestamp_expired(expiry_us, now_us)` |
| | `uint64_t timestamp_deadline_us(now_us, duration_ms)` |
| **Depends on** | `<ctime>` (POSIX) |
| **Used by** | `DeliveryEngine`, `AckTracker`, `RetryManager`, `ImpairmentEngine`, app layer |

---

### `DuplicateFilter`
**Files:** `DuplicateFilter.hpp`, `DuplicateFilter.cpp`

Sliding-window duplicate suppression. Maintains a ring of 128 `(source_id, message_id)` pairs; when the window is full the oldest entry is evicted. Prevents re-delivery of retransmitted messages at the receiver.

| | |
|---|---|
| **Key API** | `void init()` |
| | `bool is_duplicate(NodeId src, uint64_t msg_id)` |
| | `void record(NodeId src, uint64_t msg_id)` |
| | `Result check_and_record(NodeId, uint64_t)` → `ERR_DUPLICATE` or `OK` |
| **Depends on** | `Types.hpp` |
| **Used by** | `DeliveryEngine` |

---

### `AckTracker`
**Files:** `AckTracker.hpp`, `AckTracker.cpp`

Fixed-capacity (32-slot) table tracking messages awaiting acknowledgement. Each entry records the full envelope, a deadline, and a state (`FREE → PENDING → ACKED`). Supports deadline sweeping to detect timed-out ACKs.

| | |
|---|---|
| **Key API** | `void init()` |
| | `Result track(const MessageEnvelope&, uint64_t deadline_us)` |
| | `Result on_ack(NodeId src, uint64_t msg_id)` |
| | `bool is_pending(NodeId src, uint64_t msg_id)` |
| | `uint32_t sweep_expired(now_us, expired_buf[], buf_cap)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp`, `Timestamp.hpp` |
| **Used by** | `DeliveryEngine` |

---

### `RetryManager`
**Files:** `RetryManager.hpp`, `RetryManager.cpp`

Retry scheduler with per-message exponential backoff (initial interval doubles each attempt, capped at 60 s). Entries are cancelled when an ACK arrives. Expired envelopes are removed automatically.

| | |
|---|---|
| **Key API** | `void init()` |
| | `Result schedule(env, max_retries, backoff_ms, now_us)` |
| | `Result on_ack(NodeId src, uint64_t msg_id)` |
| | `uint32_t collect_due(now_us, out_buf[], buf_cap)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp`, `Timestamp.hpp` |
| **Used by** | `DeliveryEngine` |

---

### `DeliveryEngine`
**Files:** `DeliveryEngine.hpp`, `DeliveryEngine.cpp`

Per-channel coordinator. Owns and orchestrates `AckTracker`, `RetryManager`, `DuplicateFilter`, and `MessageIdGen`. Assigns message IDs, applies the configured reliability class on send, checks expiry and dedup on receive, and drives the retry/ACK-timeout pump.

| | |
|---|---|
| **Key API** | `void init(TransportInterface*, const ChannelConfig&, NodeId local_id)` |
| | `Result send(MessageEnvelope&, uint64_t now_us)` |
| | `Result receive(MessageEnvelope&, uint32_t timeout_ms, uint64_t now_us)` |
| | `uint32_t pump_retries(uint64_t now_us)` |
| | `uint32_t sweep_ack_timeouts(uint64_t now_us)` |
| **Depends on** | `TransportInterface` (pointer; injected at `init`), `AckTracker`, `RetryManager`, `DuplicateFilter` (owned by value), `MessageIdGen`, `Timestamp`, `Logger` |
| **Used by** | `Server` (`src/app/`), `Client` (`src/app/`) |

---

## 4. Platform Layer Classes

### `PrngEngine`
**Files:** `PrngEngine.hpp`, `PrngEngine.cpp`

Seedable xorshift64 PRNG. Given the same seed and input sequence, always produces the same output — essential for reproducible test scenarios.

| | |
|---|---|
| **Key API** | `void seed(uint64_t s)` |
| | `uint64_t next()` |
| | `double next_double()` — `[0.0, 1.0)` |
| | `uint32_t next_range(uint32_t lo, uint32_t hi)` |
| **Depends on** | `Types.hpp` |
| **Used by** | `ImpairmentEngine` |

---

### `ImpairmentConfig`
**Files:** `ImpairmentConfig.hpp`

POD configuration struct for the impairment engine. Holds all tunable parameters: fixed latency, jitter mean/variance, loss probability, duplication probability, reorder window, partition duration/gap, and PRNG seed.

| | |
|---|---|
| **Key API** | `struct ImpairmentConfig { enabled, fixed_latency_ms, jitter_*, loss_probability, … }` |
| | `impairment_config_default(cfg)` — disables all impairments |
| **Depends on** | `Types.hpp` |
| **Used by** | `ImpairmentEngine`, `TcpBackend`, `UdpBackend`, `LocalSimHarness` |

---

### `ImpairmentEngine`
**Files:** `ImpairmentEngine.hpp`, `ImpairmentEngine.cpp`

Network fault injection engine. Sits logically between the transport socket and the message queue. Applies loss, duplication, fixed latency, random jitter, packet reordering, and link partitions. All decisions are driven by a seeded `PrngEngine` for determinism.

| | |
|---|---|
| **Key API** | `void init(const ImpairmentConfig&)` |
| | `Result process_outbound(env, now_us)` — `ERR_IO` = drop |
| | `uint32_t collect_deliverable(now_us, out_buf[], cap)` |
| | `Result process_inbound(env, now_us, out_buf[], cap, out_count)` |
| | `bool is_partition_active(now_us)` |
| **Depends on** | `PrngEngine`, `ImpairmentConfig`, `MessageEnvelope`, `Timestamp`, `Logger` |
| **Used by** | `TcpBackend`, `UdpBackend`, `LocalSimHarness` |

---

### `SocketUtils` (free functions)
**Files:** `SocketUtils.hpp`, `SocketUtils.cpp`

Thin POSIX socket adapter layer. Provides bounded, poll-based send and receive, length-prefixed TCP framing, UDP sendto/recvfrom, and connect-with-timeout. Returns `bool`/`int` — never throws. Used exclusively by `TcpBackend` and `UdpBackend`.

| | |
|---|---|
| **Key API** | `int socket_create_tcp()` / `socket_create_udp()` |
| | `bool socket_set_reuseaddr(fd)` / `socket_set_nonblocking(fd)` |
| | `bool socket_bind(fd, ip, port)` |
| | `bool socket_connect_with_timeout(fd, ip, port, timeout_ms)` |
| | `bool socket_send_all(fd, buf, len, timeout_ms)` |
| | `bool socket_recv_exact(fd, buf, len, timeout_ms)` |
| | `bool tcp_send_frame(fd, buf, len, timeout_ms)` — 4-byte BE length prefix |
| | `bool tcp_recv_frame(fd, buf, cap, timeout_ms, out_len)` |
| | `bool socket_send_to` / `socket_recv_from` (UDP) |
| **Depends on** | POSIX: `<sys/socket.h>`, `<poll.h>`, `<fcntl.h>`, `<arpa/inet.h>`, `Types.hpp`, `Logger.hpp` |
| **Used by** | `TcpBackend`, `UdpBackend` |

---

### `TcpBackend`
**Files:** `TcpBackend.hpp`, `TcpBackend.cpp`

`TransportInterface` implementation over TCP/IP. Supports both server mode (listens, accepts up to `MAX_TCP_CONNECTIONS` = 8 clients) and client mode (single connection). Frames messages with a 4-byte big-endian length prefix. Owns an `ImpairmentEngine` and an inbound `RingBuffer`.

| | |
|---|---|
| **Key API** | `Result init(const TransportConfig&)` |
| | `Result send_message(const MessageEnvelope&)` |
| | `Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `void close()` |
| | `bool is_open() const` |
| | *(private)* `connect_to_server()`, `accept_clients()`, `recv_from_client(fd, ms)` |
| **Depends on** | `TransportInterface` (implements), `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger`, `Timestamp` |
| **Used by** | `Server` (`src/app/`), `Client` (`src/app/`) |

---

### `UdpBackend`
**Files:** `UdpBackend.hpp`, `UdpBackend.cpp`

`TransportInterface` implementation over UDP datagrams. Binds a single socket; sends serialized envelopes as individual datagrams to a configured peer address. Owns an `ImpairmentEngine` and an inbound `RingBuffer`. No connection management — loss is handled at the message layer.

| | |
|---|---|
| **Key API** | `Result init(const TransportConfig&)` |
| | `Result send_message(const MessageEnvelope&)` |
| | `Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `void close()` |
| | `bool is_open() const` |
| **Depends on** | `TransportInterface` (implements), `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger` |
| **Used by** | Applications requiring datagram transport |

---

### `LocalSimHarness`
**Files:** `LocalSimHarness.hpp`, `LocalSimHarness.cpp`

In-process simulation transport. Two instances are linked via `link()`; messages sent on one appear directly in the peer's `RingBuffer` after impairment simulation — no OS sockets involved. Designed for deterministic unit and integration testing.

| | |
|---|---|
| **Key API** | `void link(LocalSimHarness* peer)` |
| | `Result inject(const MessageEnvelope&)` — called by linked peer |
| | `Result init(const TransportConfig&)` |
| | `Result send_message(const MessageEnvelope&)` |
| | `Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `void close()` |
| | `bool is_open() const` |
| **Depends on** | `TransportInterface` (implements), `ImpairmentEngine`, `RingBuffer`, `Logger`, `Types` |
| **Used by** | `test_LocalSim.cpp`, any test needing a controlled transport |

---

## 5. App Layer

### `Server` (main)
**Files:** `src/app/Server.cpp`

TCP echo server. Reads an optional port from `argv`, configures a `TransportConfig`, initialises `TcpBackend` and `DeliveryEngine`, then runs a bounded main loop (100,000 iterations). Each iteration: receive → echo reply; pump retries; sweep ACK timeouts. `SIGINT` sets a volatile flag to exit cleanly.

| | |
|---|---|
| **Key API** | `main(int argc, char* argv[]) → int` |
| | `static void signal_handler(int)` — sets `g_stop_flag` |
| | `static void print_payload_as_string(payload, len)` |
| | `static Result send_echo_reply(engine, received, now_us)` |
| **Depends on** | `TcpBackend`, `DeliveryEngine`, `ChannelConfig`, `Logger`, `Timestamp` |

---

### `Client` (main)
**Files:** `src/app/Client.cpp`

TCP client. Reads optional host/port from `argv`, connects to the server, and sends five `RELIABLE_RETRY` messages with payloads `"Hello from client #N"`. Waits up to 2 s per message for an echo reply. Prints results and exits.

| | |
|---|---|
| **Key API** | `main(int argc, char* argv[]) → int` |
| | `static void sleep_ms(int milliseconds)` |
| | `static Result send_test_message(engine, peer_id, msg_num, now_us)` |
| | `static Result wait_for_echo(engine, send_time_us)` |
| **Depends on** | `TcpBackend`, `DeliveryEngine`, `ChannelConfig`, `Logger`, `Timestamp` |

---

## 6. Key Object Relationships

### 6.1 Ownership (contains-by-value)

All sub-components are statically allocated at class scope — no heap after `init()`.

| Owner | Owned Sub-components |
|---|---|
| `DeliveryEngine` | `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen` |
| `TcpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[8192]` |
| `UdpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[8192]` |
| `LocalSimHarness` | `ImpairmentEngine`, `RingBuffer` (recv queue) |
| `ImpairmentEngine` | `PrngEngine`, `DelayEntry[32]` (delay buffer), `MessageEnvelope[32]` (reorder buffer) |
| `RingBuffer` | `MessageEnvelope[64]` (statically allocated array) |

### 6.2 Dependency Injection (pointer / reference)

`DeliveryEngine` receives a `TransportInterface*` at `init()` time. This decouples message-layer policy from transport implementation and allows any backend — or a mock — to be swapped in without changing core code.

### 6.3 LocalSimHarness Peer Link

Two `LocalSimHarness` instances are connected by calling `link()` on each, passing a pointer to the other. A message sent on instance A is injected directly into B's `RingBuffer` (after impairment simulation), and vice versa. This models a full-duplex channel without any OS involvement.

---

## 7. Key Data Flows

### 7.1 Outbound Message (send path)

| Component | Action |
|---|---|
| App | Fills `MessageEnvelope` fields (type, payload, reliability class, expiry, destination). |
| `DeliveryEngine::send()` | Assigns `message_id` via `MessageIdGen`; stamps `source_id`. Based on `reliability_class`: tracks in `AckTracker` (RELIABLE_ACK/RETRY) and schedules retry in `RetryManager` (RELIABLE_RETRY). |
| `TransportInterface::send_message()` | Dispatches to the concrete backend. |
| `TcpBackend` / `UdpBackend` | Serialises envelope via `Serializer::serialize()` into a wire buffer. |
| `ImpairmentEngine::process_outbound()` | May drop (loss), duplicate, or add latency/jitter. Dropped messages return `ERR_IO`; caller treats this as a silent drop for best-effort. |
| `SocketUtils` / OS | Sends the wire bytes over the TCP frame or UDP datagram. |

### 7.2 Inbound Message (receive path)

| Component | Action |
|---|---|
| `SocketUtils` / OS | Receives raw bytes from socket; assembles the full frame. |
| `Serializer::deserialize()` | Converts wire bytes back to a `MessageEnvelope` struct. |
| `ImpairmentEngine::process_inbound()` | Optionally buffers for reordering; releases when window is satisfied. |
| `RingBuffer::push()` | Backend enqueues the received envelope. |
| `TransportInterface::receive_message()` | Pops the envelope and returns it to the caller. |
| `DeliveryEngine::receive()` | Checks expiry (`timestamp_expired`). If ACK/NAK: updates `AckTracker` and `RetryManager`. If DATA + RELIABLE_RETRY: runs `DuplicateFilter::check_and_record()`. Returns `OK` or `ERR_EXPIRED`/`ERR_DUPLICATE`. |
| App | Processes the delivered DATA envelope (echo, log, etc.). |

### 7.3 Retry Pump

The application calls `DeliveryEngine::pump_retries(now_us)` periodically (typically each main-loop iteration). The method calls `RetryManager::collect_due()` to retrieve messages whose `next_retry_us ≤ now_us`, then re-sends each via the transport. The retry interval doubles each attempt (exponential backoff) and entries are removed when their `max_retries` is exhausted or an ACK is received.

---

## 8. Dependency Matrix

| Class | Depends On |
|---|---|
| `Types` | — (no project dependencies) |
| `Logger` | `Types` |
| `MessageEnvelope` | `Types` |
| `ChannelConfig` / `TransportConfig` | `Types` |
| `TransportInterface` | `Types`, `MessageEnvelope`, `ChannelConfig` |
| `RingBuffer` | `Types`, `MessageEnvelope` |
| `Serializer` | `Types`, `MessageEnvelope` |
| `MessageIdGen` | `Types` |
| `Timestamp` (free functions) | POSIX `<ctime>`, `CLOCK_MONOTONIC` |
| `DuplicateFilter` | `Types` |
| `AckTracker` | `Types`, `MessageEnvelope`, `Timestamp` |
| `RetryManager` | `Types`, `MessageEnvelope`, `Timestamp` |
| `DeliveryEngine` | `TransportInterface`, `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`, `Timestamp`, `Logger` |
| `PrngEngine` | `Types` |
| `ImpairmentConfig` | `Types` |
| `ImpairmentEngine` | `PrngEngine`, `ImpairmentConfig`, `MessageEnvelope`, `Timestamp`, `Logger`, `Types` |
| `SocketUtils` | POSIX sockets/poll/fcntl, `Serializer`, `Types`, `Logger` |
| `TcpBackend` | `TransportInterface` (implements), `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger`, `Timestamp` |
| `UdpBackend` | `TransportInterface` (implements), `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger` |
| `LocalSimHarness` | `TransportInterface` (implements), `ImpairmentEngine`, `RingBuffer`, `Logger` |
| `Server` (main) | `TcpBackend`, `DeliveryEngine`, `ChannelConfig`, `Logger`, `Timestamp` |
| `Client` (main) | `TcpBackend`, `DeliveryEngine`, `ChannelConfig`, `Logger`, `Timestamp` |

---

## 9. Coding Standard Compliance Summary

| Requirement | How It Is Met |
|---|---|
| **Power of 10 — no recursion** | No function calls itself directly or indirectly; verified by code review. |
| **Power of 10 — fixed loop bounds** | Every loop uses a compile-time constant (`MSG_RING_CAPACITY`, `ACK_TRACKER_CAPACITY`, `IMPAIR_DELAY_BUF_SIZE`, etc.) as its upper bound. |
| **Power of 10 — no dynamic allocation after init** | All buffers (`RingBuffer`, `AckTracker` slots, delay buffer) are statically sized at class scope. No `malloc`/`new` on critical paths. |
| **Power of 10 — small functions** | All functions are ≤ 1 printed page; typically 20–50 lines. |
| **Power of 10 — ≥ 2 assertions per function** | Enforced throughout: preconditions, postconditions, invariants. Verified by inspection. |
| **Power of 10 — check all returns** | Every non-void call result is explicitly checked; unchecked calls are cast to `(void)` with justification. |
| **Power of 10 — zero warnings** | Built with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; zero warnings confirmed. |
| **MISRA — no STL in production** | No `std::vector`, `std::string`, `std::map`, or STL containers/algorithms used in `src/`. `std::atomic<uint32_t>` is a documented carve-out: it has no dynamic allocation, maps to a hardware primitive, is ISO standard, and is portable across all conforming toolchains — satisfying F-Prime's portability principle and endorsed by MISRA C++:2023. |
| **F-Prime — no exceptions** | Compiled with `-fno-exceptions`; all errors propagated via `Result` enum. |
| **F-Prime — no RTTI** | Compiled with `-fno-rtti`; no `dynamic_cast`, no `typeid`. |
| **F-Prime — no templates** | No template functions or classes in production code. |
| **F-Prime — no function pointers** | No raw function pointer variables. `signal()` use in `Server.cpp` is documented as a minimal MISRA deviation. |
| **Architecture — no upward deps** | Platform never includes Core headers for application logic; Core never calls OS APIs directly. |
| **Determinism** | `ImpairmentEngine` driven by a seedable `PrngEngine`; same seed → same impairment sequence every run. |
