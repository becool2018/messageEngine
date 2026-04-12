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
│  TcpBackend · TlsTcpBackend · UdpBackend             │
│  DtlsUdpBackend · LocalSimHarness · ImpairmentEngine │
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
| | `enum class MessageType` (DATA, ACK, NAK, HEARTBEAT, HELLO=4, INVALID) |
| | `enum class ReliabilityClass` (BEST_EFFORT, RELIABLE_ACK, RELIABLE_RETRY) |
| | `enum class TransportKind` (TCP, UDP, LOCAL_SIM, DTLS_UDP) |
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
| | `virtual Result register_local_id(NodeId id)` — default returns OK; overridden by TCP/UDP/DTLS backends to send HELLO frame (REQ-6.1.10) |
| | `virtual NodeId pop_hello_peer()` — default returns NODE_ID_INVALID (no-op); overridden by TcpBackend and TlsTcpBackend to drain a bounded HELLO reconnect FIFO; polled by `DeliveryEngine::drain_hello_reconnects()` on every `receive()` call (REQ-3.3.6) |
| | `virtual void get_transport_stats(TransportStats&) const = 0` — pure virtual; all backends must implement (REQ-7.2.4) |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp`, `ChannelConfig.hpp`, `DeliveryStats.hpp` |
| **Used by** | `DeliveryEngine`, `TcpBackend`, `UdpBackend`, `TlsTcpBackend`, `DtlsUdpBackend`, `LocalSimHarness` |

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
| | `void record(NodeId src, uint64_t msg_id, uint64_t now_us)` |
| | `Result check_and_record(NodeId src, uint64_t msg_id, uint64_t now_us)` → `ERR_DUPLICATE` or `OK` |
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
| | `Result cancel(NodeId src, uint64_t msg_id)` |
| | `Result get_send_timestamp(NodeId src, uint64_t msg_id, uint64_t& out_ts)` |
| | `Result get_tracked_destination(NodeId src, uint64_t msg_id, NodeId& out_dest)` |
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
| | `Result cancel(NodeId src, uint64_t msg_id)` |
| | `const RetryStats& get_stats() const` |
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
| | `bool poll_event(DeliveryEvent&)` — pull one observability event from the ring |
| | `uint32_t drain_events(DeliveryEvent*, uint32_t cap)` — drain up to cap events |
| | `uint32_t pending_event_count() const` |
| | `void get_stats(DeliveryEngineStats&) const` |
| **Depends on** | `TransportInterface` (pointer; injected at `init`), `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`, `DeliveryEventRing`, `Timestamp`, `Logger` (all owned by value) |
| **Used by** | `Server` (`src/app/`), `Client` (`src/app/`) |

---

### `ProtocolVersion`
**Files:** `ProtocolVersion.hpp`

Wire-format version constants. Every serialized frame carries these two values; the deserializer rejects frames that do not match.

| | |
|---|---|
| **Key API** | `PROTO_VERSION` — current version byte (byte 3 of wire header) |
| | `PROTO_MAGIC` — 2-byte magic 0x4D45 = 'ME' (wire bytes 40–41) |
| **Depends on** | — |
| **Used by** | `Serializer`, all backends |

---

### `Fragmentation`
**Files:** `Fragmentation.hpp`, `Fragmentation.cpp`

Stateless helper that splits an oversized logical `MessageEnvelope` into N wire-fragment envelopes. Each fragment carries `fragment_index` and `fragment_count` in its header. No state — safe to call from multiple contexts.

| | |
|---|---|
| **Key API** | `static uint32_t fragment_count(payload_len, frag_payload_max)` |
| | `static Result fragment_message(const MessageEnvelope&, out_frags[], frag_cap, frag_payload_max, out_count)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp` |
| **Used by** | `Serializer` (send path), backends on large messages |

---

### `ReassemblyBuffer`
**Files:** `ReassemblyBuffer.hpp`, `ReassemblyBuffer.cpp`

Bounded reassembly table. Collects incoming fragments keyed on `(source_id, message_id)`; releases the complete logical envelope when all `fragment_count` fragments arrive. Reclaims stale slots older than `recv_timeout_ms` (REQ-3.2.9).

| | |
|---|---|
| **Key API** | `void init(uint32_t recv_timeout_ms)` |
| | `Result ingest(const MessageEnvelope& frag, MessageEnvelope& logical_out, uint64_t now_us)` → `OK` (complete, logical_out populated), `ERR_IO` (stored, more fragments pending), `ERR_DUPLICATE`, `ERR_FULL` |
| | `uint32_t sweep_stale(uint64_t now_us, uint64_t stale_threshold_us)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp` |
| **Used by** | `DeliveryEngine` (receive path) |

---

### `OrderingBuffer`
**Files:** `OrderingBuffer.hpp`, `OrderingBuffer.cpp`

Per-peer in-order delivery gate. Holds out-of-sequence DATA envelopes until the expected `sequence_num` arrives, then releases the run in order. Used for channels with `ORDERED` delivery mode.

| | |
|---|---|
| **Key API** | `void init(NodeId local_node)` |
| | `Result ingest(const MessageEnvelope&, MessageEnvelope& out, uint64_t now_us)` |
| | `Result try_release_next(NodeId src, MessageEnvelope& out)` |
| | `void reset_peer(NodeId src)` — resets `next_expected_seq` to 1 and frees held slots on peer reconnect (REQ-3.3.6) |
| | `void advance_sequence(NodeId src, uint32_t up_to_seq)` |
| | `uint32_t sweep_expired_holds(uint64_t now_us, MessageEnvelope* out_freed, uint32_t out_cap)` |
| **Depends on** | `Types.hpp`, `MessageEnvelope.hpp` |
| **Used by** | `DeliveryEngine` (receive path for ordered channels) |

---

### `RequestReplyHeader`
**Files:** `RequestReplyHeader.hpp`

Packed 12-byte prefix embedded at the start of `MessageEnvelope::payload` for request/reply correlation. Identifies the correlation ID and message kind (REQUEST or REPLY).

| | |
|---|---|
| **Key API** | `struct RequestReplyHeader { uint64_t request_id; uint32_t kind; }` |
| | `request_reply_header_serialize(hdr, buf, buf_len)` |
| | `request_reply_header_deserialize(buf, buf_len, hdr)` |
| **Depends on** | `Types.hpp` |
| **Used by** | `RequestReplyEngine` |

---

### `RequestReplyEngine`
**Files:** `RequestReplyEngine.hpp`, `RequestReplyEngine.cpp`

Bounded request/response helper built on top of `DeliveryEngine`. Manages a fixed-capacity pending-requests table; matches incoming replies by `request_id`; invokes registered per-request callbacks on match or timeout.

| | |
|---|---|
| **Key API** | `Result init(DeliveryEngine*, NodeId local_id)` |
| | `Result send_request(MessageEnvelope&, uint32_t timeout_ms, uint64_t now_us, uint64_t& out_request_id)` |
| | `Result pump(uint64_t now_us)` — matches replies and fires callbacks |
| **Depends on** | `DeliveryEngine`, `RequestReplyHeader`, `Types`, `Timestamp`, `Logger` |
| **Used by** | Applications requiring synchronized request/reply semantics |

---

### `DeliveryEvent` / `DeliveryEventRing`
**Files:** `DeliveryEvent.hpp`, `DeliveryEventRing.hpp`

Pull-style observability for `DeliveryEngine`. `DeliveryEvent` is a POD record carrying `kind`, `source_id`, `message_id`, and `timestamp_us`. `DeliveryEventRing` is a fixed-capacity overwrite-on-full ring buffer backed by `std::atomic<uint32_t>` head/tail indices. No dynamic allocation; no blocking.

| | |
|---|---|
| **Key API** | `enum class DeliveryEventKind` — SEND_OK, SEND_FAIL, ACK_RECEIVED, ACK_TIMEOUT, DUPLICATE_DROP, EXPIRY_DROP, RETRY_FIRED, MISROUTE_DROP |
| | `void push(const DeliveryEvent&)` — overwrites oldest on full |
| | `bool poll(DeliveryEvent&)` — returns false if empty |
| | `uint32_t drain(DeliveryEvent*, uint32_t cap)` |
| | `uint32_t pending_count() const` |
| **Depends on** | `Types.hpp`, `<atomic>` (permitted carve-out) |
| **Used by** | `DeliveryEngine` (owned by value) |

---

### `DeliveryStats`
**Files:** `DeliveryStats.hpp`

POD metrics structs aggregated by `AckTracker`, `RetryManager`, and backend classes. Consumed by `DeliveryEngine::get_stats()` and `TransportInterface::get_transport_stats()`.

| | |
|---|---|
| **Key API** | `struct AckTrackerStats { slots_used, ack_count, timeout_count, … }` |
| | `struct RetryManagerStats { active_count, retry_fired_count, exhausted_count }` |
| | `struct TransportStats { messages_sent, messages_received, send_errors, recv_errors, connections_opened, connections_closed }` |
| **Depends on** | `Types.hpp` |
| **Used by** | `AckTracker`, `RetryManager`, `TcpBackend`, `UdpBackend`, `TlsTcpBackend`, `DtlsUdpBackend` |

---

### `TlsConfig`
**Files:** `TlsConfig.hpp`

POD configuration struct shared by `TlsTcpBackend` and `DtlsUdpBackend`. Holds TLS/DTLS parameters: enabled flag, certificate paths, CA cert path, `verify_peer` flag, and expected `peer_hostname`.

| | |
|---|---|
| **Key API** | `struct TlsConfig { tls_enabled, cert_path[], key_path[], ca_cert_path[], verify_peer, peer_hostname[] }` |
| **Depends on** | `Types.hpp` |
| **Used by** | `TlsTcpBackend`, `DtlsUdpBackend`, `TransportConfig` |

---

### `Version`
**Files:** `Version.hpp`

Compile-time project version constants: `MESSAGE_ENGINE_VERSION_MAJOR`, `MESSAGE_ENGINE_VERSION_MINOR`, `MESSAGE_ENGINE_VERSION_PATCH`, and `MESSAGE_ENGINE_VERSION_STRING`.

| | |
|---|---|
| **Depends on** | — |
| **Used by** | App layer for version reporting |

---

### `IResetHandler` / `AssertState` / `AbortResetHandler`
**Files:** `IResetHandler.hpp`, `AssertState.hpp`, `AssertState.cpp`, `AbortResetHandler.hpp`

Assert-failure handling infrastructure. `IResetHandler` is a pure-virtual interface with one method: `on_fatal_assert()`. `AssertState` holds the global handler pointer and the `g_fatal_fired` flag; `NEVER_COMPILED_OUT_ASSERT` calls through it. `AbortResetHandler` is the POSIX concrete implementation (calls `::abort()`).

| | |
|---|---|
| **Key API** | `class IResetHandler { virtual void on_fatal_assert() = 0; }` |
| | `assert_state::set_reset_handler(IResetHandler*)` |
| | `assert_state::trigger_handler_for_test()` |
| | `AbortResetHandler::instance()` — singleton |
| **Depends on** | `Types.hpp` |
| **Used by** | `Assert.hpp` (macro expansion), production main, test fixtures |

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

### `ISocketOps` / `SocketOpsImpl`
**Files:** `ISocketOps.hpp`, `SocketOpsImpl.hpp`, `SocketOpsImpl.cpp`

Injectable interface for all POSIX socket operations used by `TcpBackend` and `UdpBackend`. `ISocketOps` is the pure-virtual seam; `SocketOpsImpl` is the singleton production implementation that delegates to real POSIX calls. In tests, `MockSocketOps` is substituted to exercise error paths (M5 fault injection).

| | |
|---|---|
| **Key API** | `virtual int create_tcp()` / `create_udp()` |
| | `virtual bool do_bind(fd, ip, port)` / `do_listen` / `do_accept` |
| | `virtual bool send_frame(fd, buf, len, ms)` / `recv_frame` |
| | `virtual bool send_to(fd, buf, len, ip, port, ms)` / `recv_from` |
| | `SocketOpsImpl& SocketOpsImpl::instance()` — singleton |
| **Depends on** | POSIX sockets, `Types.hpp` |
| **Used by** | `TcpBackend`, `UdpBackend` (inject at construction) |

---

### `TlsTcpBackend`
**Files:** `TlsTcpBackend.hpp`, `TlsTcpBackend.cpp`

Drop-in `TransportInterface` replacement for `TcpBackend` that adds optional mbedTLS 4.0 TLS encryption over TCP. When `tls_enabled=false`, operates identically to `TcpBackend` (REQ-6.3.4). Supports up to `MAX_TCP_CONNECTIONS` = 8 simultaneous TLS sessions in server mode. SC functions: `send_message` (HAZ-005, HAZ-006), `receive_message` (HAZ-004, HAZ-005).

| | |
|---|---|
| **Key API** | `TlsTcpBackend(ISocketOps& sock_ops)` — injection constructor for ISocketOps |
| | `TlsTcpBackend(ISocketOps& sock_ops, IMbedtlsOps& tls_ops)` — injection constructor for both ISocketOps and IMbedtlsOps |
| | `Result init(const TransportConfig&)` |
| | `Result send_message(const MessageEnvelope&)` |
| | `Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `void close()` / `bool is_open() const` |
| **Depends on** | `TransportInterface` (implements), `TlsConfig`, `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, mbedTLS PSA Crypto API |
| **Used by** | Secure TCP clients and servers |

---

### `DtlsUdpBackend`
**Files:** `DtlsUdpBackend.hpp`, `DtlsUdpBackend.cpp`

`TransportInterface` over DTLS-encrypted UDP (DATAGRAM transport mode, REQ-6.4.1). Implements DTLS cookie anti-replay (REQ-6.4.2), configurable retransmission timer (REQ-6.4.3), MTU enforcement at `DTLS_MAX_DATAGRAM_BYTES` = 1400 (REQ-6.4.4), and plaintext UDP fallback when `tls_enabled=false` (REQ-6.4.5). SC functions: `send_message` (HAZ-005, HAZ-006), `receive_message` (HAZ-004, HAZ-005).

| | |
|---|---|
| **Key API** | `Result init(const TransportConfig&)` |
| | `Result send_message(const MessageEnvelope&)` |
| | `Result receive_message(MessageEnvelope&, uint32_t timeout_ms)` |
| | `void close()` / `bool is_open() const` |
| | *(private)* `process_hello_or_validate(env, consumed)` — HAZ-009, HAZ-011 |
| **Depends on** | `TransportInterface` (implements), `TlsConfig`, `IMbedtlsOps` (injected), `Serializer`, `ImpairmentEngine`, `RingBuffer`, mbedTLS DTLS |
| **Used by** | Secure UDP peers |

---

### `IMbedtlsOps` / `MbedtlsOpsImpl`
**Files:** `IMbedtlsOps.hpp`, `MbedtlsOpsImpl.hpp`, `MbedtlsOpsImpl.cpp`

Injectable interface for all mbedTLS library calls used by `DtlsUdpBackend`. `MbedtlsOpsImpl` is the singleton production implementation. `MockMbedtlsOps` (in `tests/`) returns configurable error codes to exercise DTLS error paths (M5). SC functions in the interface: `ssl_handshake` (HAZ-004, HAZ-005, HAZ-006), `ssl_write` (HAZ-005, HAZ-006), `ssl_read` (HAZ-004, HAZ-005).

| | |
|---|---|
| **Key API** | `virtual int crypto_init()` |
| | `virtual int ssl_config_defaults(cfg, endpoint, transport, preset)` |
| | `virtual int x509_crt_parse_file(cert, path)` |
| | `virtual int pk_parse_keyfile(mbedtls_pk_context*, const char* path, const char* pwd)` |
| | `virtual int ssl_setup(ssl, cfg)` |
| | `virtual int ssl_handshake(ssl)` |
| | `virtual int ssl_read(ssl, buf, len)` / `ssl_write` |
| | `MbedtlsOpsImpl& MbedtlsOpsImpl::instance()` — singleton |
| **Depends on** | mbedTLS 4.0 PSA Crypto, `Types.hpp` |
| **Used by** | `DtlsUdpBackend` (inject at construction) |

---

### `IPosixSyscalls` / `PosixSyscallsImpl`
**Files:** `IPosixSyscalls.hpp`, `PosixSyscallsImpl.hpp`

Injectable interface for POSIX system calls (e.g., `getpid`, `clock_gettime`, or other OS primitives). `IPosixSyscalls` is the pure-virtual seam; `PosixSyscallsImpl` is the singleton production implementation that delegates to real POSIX calls. In tests, a mock implementation can be substituted to exercise POSIX error paths (M5 fault injection).

| | |
|---|---|
| **Key API** | `PosixSyscallsImpl& PosixSyscallsImpl::instance()` — singleton |
| **Depends on** | POSIX OS APIs, `Types.hpp` |
| **Used by** | Platform layer components requiring POSIX system call injection |

---

### `ImpairmentConfigLoader`
**Files:** `ImpairmentConfigLoader.hpp`, `ImpairmentConfigLoader.cpp`

Free function `impairment_config_load(path, cfg)`. Reads an INI-style `key=value` text file (at most `MAX_CONFIG_LINES` = 64 lines, 128 bytes/line) and populates an `ImpairmentConfig` struct. Always initialises `cfg` to safe defaults before parsing; probability fields clamped to [0.0, 1.0]; unknown keys logged at WARNING_LO. SC: HAZ-002, HAZ-007.

| | |
|---|---|
| **Key API** | `Result impairment_config_load(const char* path, ImpairmentConfig& cfg)` |
| **Depends on** | `ImpairmentConfig`, `Logger`, `Types` |
| **Used by** | Application startup / configuration loading |

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
| `DeliveryEngine` | `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`, `DeliveryEventRing`, `ReassemblyBuffer`, `OrderingBuffer` |
| `RequestReplyEngine` | Pending-requests table (fixed array), held pointer to `DeliveryEngine` |
| `TcpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[SOCKET_RECV_BUF_BYTES]` |
| `TlsTcpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[SOCKET_RECV_BUF_BYTES]`, mbedTLS ssl_context array |
| `UdpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[SOCKET_RECV_BUF_BYTES]` |
| `DtlsUdpBackend` | `ImpairmentEngine`, `RingBuffer` (recv queue), `wire_buf[SOCKET_RECV_BUF_BYTES]`, mbedTLS ssl/cfg/cert state |
| `LocalSimHarness` | `ImpairmentEngine`, `RingBuffer` (recv queue) |
| `ImpairmentEngine` | `PrngEngine`, `DelayEntry[IMPAIR_DELAY_BUF_SIZE]` (delay buffer), `MessageEnvelope[IMPAIR_DELAY_BUF_SIZE]` (reorder buffer) |
| `RingBuffer` | `MessageEnvelope[MSG_RING_CAPACITY]` (statically allocated array) |
| `DeliveryEventRing` | `DeliveryEvent[DELIVERY_EVENT_RING_CAPACITY]` (statically allocated array) |

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
| `Version` | — |
| `Logger` | `Types` |
| `MessageEnvelope` | `Types` |
| `ChannelConfig` / `TransportConfig` | `Types`, `TlsConfig` |
| `ProtocolVersion` | — |
| `DeliveryStats` | `Types` |
| `DeliveryEvent` | `Types` |
| `DeliveryEventRing` | `Types`, `DeliveryEvent`, `<atomic>` |
| `TransportInterface` | `Types`, `MessageEnvelope`, `ChannelConfig`, `DeliveryStats` |
| `RingBuffer` | `Types`, `MessageEnvelope`, `<atomic>` |
| `Serializer` | `Types`, `MessageEnvelope`, `ProtocolVersion` |
| `Fragmentation` | `Types`, `MessageEnvelope` |
| `ReassemblyBuffer` | `Types`, `MessageEnvelope` |
| `OrderingBuffer` | `Types`, `MessageEnvelope` |
| `RequestReplyHeader` | `Types` |
| `MessageIdGen` | `Types` |
| `Timestamp` (free functions) | POSIX `<ctime>`, `CLOCK_MONOTONIC` |
| `DuplicateFilter` | `Types` |
| `AckTracker` | `Types`, `MessageEnvelope`, `Timestamp`, `DeliveryStats` |
| `RetryManager` | `Types`, `MessageEnvelope`, `Timestamp`, `DeliveryStats` |
| `DeliveryEngine` | `TransportInterface`, `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`, `DeliveryEventRing`, `ReassemblyBuffer`, `OrderingBuffer`, `Timestamp`, `Logger` |
| `RequestReplyEngine` | `DeliveryEngine`, `RequestReplyHeader`, `Timestamp`, `Logger`, `Types` |
| `IResetHandler` | `Types` |
| `AssertState` | `IResetHandler`, `Types` |
| `AbortResetHandler` | `IResetHandler` |
| `TlsConfig` | `Types` |
| `PrngEngine` | `Types` |
| `ImpairmentConfig` | `Types` |
| `ImpairmentEngine` | `PrngEngine`, `ImpairmentConfig`, `MessageEnvelope`, `Timestamp`, `Logger`, `Types` |
| `ImpairmentConfigLoader` | `ImpairmentConfig`, `Logger`, `Types` |
| `ISocketOps` | `Types` |
| `SocketOpsImpl` | `ISocketOps`, POSIX sockets |
| `SocketUtils` | POSIX sockets/poll/fcntl, `Types`, `Logger` |
| `TcpBackend` | `TransportInterface` (implements), `ISocketOps`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger`, `Timestamp`, `DeliveryStats` |
| `UdpBackend` | `TransportInterface` (implements), `ISocketOps`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger`, `DeliveryStats` |
| `IMbedtlsOps` | `Types` |
| `MbedtlsOpsImpl` | `IMbedtlsOps`, mbedTLS 4.0 PSA Crypto |
| `TlsTcpBackend` | `TransportInterface` (implements), `TlsConfig`, `SocketUtils`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger`, mbedTLS |
| `DtlsUdpBackend` | `TransportInterface` (implements), `TlsConfig`, `IMbedtlsOps`, `Serializer`, `ImpairmentEngine`, `RingBuffer`, `Logger` |
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
