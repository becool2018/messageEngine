# Capacity Reference

**Project:** messageEngine
**Source of truth:** `src/core/Types.hpp`
**Related policy:** `CLAUDE.md §1c` (stress test trigger rules), `CLAUDE.md §15` (WCET), `docs/STACK_ANALYSIS.md`

All capacity limits are compile-time constants.  No heap allocation occurs on critical paths after `init()`.  Changing any value listed here is safe only when the constraints in each section are satisfied.

---

## Table of Contents

1. [Max Payload — `MSG_MAX_PAYLOAD_BYTES`](#1-max-payload--msg_max_payload_bytes)
2. [Dedup Window — `DEDUP_WINDOW_SIZE`](#2-dedup-window--dedup_window_size)
3. [ACK Tracker — `ACK_TRACKER_CAPACITY`](#3-ack-tracker--ack_tracker_capacity)
4. [Retry Manager — `MAX_RETRY_COUNT`](#4-retry-manager--max_retry_count)
5. [Inbound Ring — `MSG_RING_CAPACITY`](#5-inbound-ring--msg_ring_capacity)
6. [Concurrent Clients — `MAX_TCP_CONNECTIONS`](#6-concurrent-clients--max_tcp_connections)
7. [Worst-Case Stack Depth](#7-worst-case-stack-depth)
8. [Related Constants](#8-related-constants)

---

## 1. Max Payload — `MSG_MAX_PAYLOAD_BYTES`

| Field | Value |
|---|---|
| **Constant** | `MSG_MAX_PAYLOAD_BYTES` |
| **Current value** | `4096U` (bytes) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `Serializer`, `MessageEnvelope`, fragmentation / reassembly |

### What it is

The maximum number of payload bytes a single logical application message may carry.  Any `send()` call whose `payload_length` exceeds this limit is rejected with `ERR_INVALID` before any serialization occurs.

On the wire, payloads larger than `FRAG_MAX_PAYLOAD_BYTES` (1 024 bytes) are automatically split into up to `FRAG_MAX_COUNT` (4) fragments, each fitting inside a single DTLS datagram.  The receiver reassembles them transparently.

### Memory impact

`Serializer` allocates no heap; it works on caller-supplied buffers.  `MessageEnvelope::payload[MSG_MAX_PAYLOAD_BYTES]` is an inline array inside each envelope struct.  Every stack or static `MessageEnvelope` instance pays this cost.

### How to change it

1. Edit `MSG_MAX_PAYLOAD_BYTES` in `src/core/Types.hpp`.
2. Verify that `FRAG_MAX_COUNT` × `FRAG_MAX_PAYLOAD_BYTES` ≥ new value (or adjust those constants too — see §8).
3. Run `make run_stress_tests` — required by `CLAUDE.md §1c` because this is a capacity constant.
4. Update `docs/WCET_ANALYSIS.md` (dominant O(MSG_MAX_PAYLOAD_BYTES) paths in `Serializer`).
5. Run `make run_tests lint`.

### Constraints

- Must satisfy: `FRAG_MAX_COUNT × FRAG_MAX_PAYLOAD_BYTES ≥ MSG_MAX_PAYLOAD_BYTES`
- Each fragment must fit inside a single DTLS datagram: `FRAG_MAX_PAYLOAD_BYTES + WIRE_HEADER_SIZE_V2 ≤ DTLS_MAX_DATAGRAM_BYTES`

---

## 2. Dedup Window — `DEDUP_WINDOW_SIZE`

| Field | Value |
|---|---|
| **Constant** | `DEDUP_WINDOW_SIZE` |
| **Current value** | `128U` (entries) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `DuplicateFilter` |

### What it is

`DuplicateFilter` maintains a sliding window of the last N `(source_id, message_id)` pairs it has seen.  When a new message arrives, it scans the window: if the pair is already present the message is silently dropped and `ERR_DUPLICATE` is returned; otherwise the pair is recorded, evicting the oldest entry if the window is full.

This protects `RELIABLE_RETRY` receivers from delivering the same logical message twice when the sender retransmits before an ACK arrives.

### Memory impact

Each slot stores two `uint32_t` values (8 bytes).  Total: `DEDUP_WINDOW_SIZE × 8` = 1 024 bytes, held in a static array inside `DuplicateFilter`.

### How to change it

1. Edit `DEDUP_WINDOW_SIZE` in `src/core/Types.hpp`.
2. Run `make run_stress_tests` — required by `CLAUDE.md §1c`.
3. Update `docs/WCET_ANALYSIS.md` (O(DEDUP_WINDOW_SIZE) scan in `check_and_record`).
4. Run `make run_tests lint`.

### Constraints

- Larger windows tolerate more in-flight retransmissions before an old entry is evicted (reduces false-negative dedup risk at high message rates).
- There is no hard minimum; setting it below `MAX_RETRY_COUNT × MAX_PEER_NODES` increases the risk of a retransmitted message escaping the window before its ACK arrives.

---

## 3. ACK Tracker — `ACK_TRACKER_CAPACITY`

| Field | Value |
|---|---|
| **Constant** | `ACK_TRACKER_CAPACITY` |
| **Current value** | `32U` (slots) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `AckTracker` |

### What it is

`AckTracker` maintains a fixed-size table of messages that have been sent but not yet ACKed.  Each slot records the `message_id`, the peer `NodeId`, the expiry timestamp, and the current state (`PENDING` / `ACKED` / `EXPIRED`).

When `send()` is called with `RELIABLE_ACK` or `RELIABLE_RETRY`, a slot is allocated in `AckTracker`.  When the matching ACK arrives (or the expiry deadline passes), the slot is freed.  If all 32 slots are occupied, `send()` returns `ERR_FULL`.

### Memory impact

Each slot is a small struct (~24 bytes).  Total: ~768 bytes in a static array inside `AckTracker`.

### How to change it

1. Edit `ACK_TRACKER_CAPACITY` in `src/core/Types.hpp`.
2. Run `make run_stress_tests` — required by `CLAUDE.md §1c`.
3. Update `docs/WCET_ANALYSIS.md` (O(ACK_TRACKER_CAPACITY) scan in `pump_retries`).
4. Run `make run_tests lint`.

### Constraints

- `ACK_TRACKER_CAPACITY` is also the number of concurrent in-flight reliable messages.  Applications that pipeline many messages at once need a value ≥ their pipeline depth.
- Increasing this increases WCET of `pump_retries()` linearly — see `docs/WCET_ANALYSIS.md`.

---

## 4. Retry Manager — `MAX_RETRY_COUNT`

| Field | Value |
|---|---|
| **Constant** | `MAX_RETRY_COUNT` |
| **Current value** | `5U` (attempts) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `RetryManager` |

### What it is

For `RELIABLE_RETRY` messages, `RetryManager` retransmits up to `MAX_RETRY_COUNT` times before declaring the message failed.  Intervals use exponential backoff: the first retry fires after the initial timeout, subsequent retries double the interval (capped at 60 s).

After `MAX_RETRY_COUNT` exhausted retransmissions without an ACK, the slot is freed and the caller receives `ERR_TIMEOUT`.

The retry manager shares the same slot table sizing as `AckTracker` (`ACK_TRACKER_CAPACITY` slots).

### How to change it

1. Edit `MAX_RETRY_COUNT` in `src/core/Types.hpp`.
2. Run `make run_stress_tests` — required by `CLAUDE.md §1c`.
3. Update `docs/WCET_ANALYSIS.md` if the dominant retry-pump path changes.
4. Run `make run_tests lint`.

### Constraints

- Higher values increase the time-to-detect a permanently lost peer (worst case: `MAX_RETRY_COUNT × 60 s`).
- The dedup window should be sized so that `DEDUP_WINDOW_SIZE ≥ MAX_RETRY_COUNT × MAX_PEER_NODES` to avoid false-negative dedup on a slow receiver (see §2).

---

## 5. Inbound Ring — `MSG_RING_CAPACITY`

| Field | Value |
|---|---|
| **Constant** | `MSG_RING_CAPACITY` |
| **Current value** | `64U` (messages) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `RingBuffer<MessageEnvelope>` inside each backend |

### What it is

Each transport backend (TCP, UDP, TLS, DTLS, `LocalSimHarness`) holds an inbound `RingBuffer` of `MSG_RING_CAPACITY` `MessageEnvelope` slots.  Received frames are deserialized and placed into this ring; `DeliveryEngine::receive()` drains from it.

If the ring is full when a new message arrives the backend drops the incoming message and emits a `WARNING_HI` log.  This is the backpressure mechanism — it prevents unbounded memory growth under a slow consumer.

### Memory impact

Each slot is a `MessageEnvelope` (~4 136 bytes including the inline payload array).  Total per backend: `MSG_RING_CAPACITY × sizeof(MessageEnvelope)` ≈ 256 KB.

### How to change it

1. Edit `MSG_RING_CAPACITY` in `src/core/Types.hpp`.
2. Run `make run_stress_tests` — required by `CLAUDE.md §1c`.
3. Run `make run_tests lint`.

### Constraints

- Reducing this below the application's burst message rate will cause drop events visible in the `DeliveryEvent` ring as `SEND_FAIL`.
- Each backend instance pays the full `MSG_RING_CAPACITY × sizeof(MessageEnvelope)` cost regardless of actual usage.

---

## 6. Concurrent Clients — `MAX_TCP_CONNECTIONS`

| Field | Value |
|---|---|
| **Constant** | `MAX_TCP_CONNECTIONS` |
| **Current value** | `8U` (connections) |
| **Defined in** | `src/core/Types.hpp` |
| **Used by** | `TcpBackend` (server mode), `TlsTcpBackend` (server mode) |

### What it is

In server mode, `TcpBackend` and `TlsTcpBackend` maintain a fixed-size slot array of `MAX_TCP_CONNECTIONS` open client connections.  Each slot holds the socket file descriptor, the registered `NodeId` (populated from the client's `HELLO` frame per REQ-6.1.8), and a small receive buffer.

When the ninth client connects while eight are already active, the server rejects the new connection with `ERR_FULL` and logs `WARNING_HI`.

In client mode this constant is not used; a client opens exactly one connection.

### Memory impact

Each slot is a struct of ~16 bytes plus a `SOCKET_RECV_BUF_BYTES` (8 192-byte) buffer.  Total server-side: `MAX_TCP_CONNECTIONS × (16 + 8192)` ≈ 65 KB.

### How to change it

1. Edit `MAX_TCP_CONNECTIONS` in `src/core/Types.hpp`.
2. Run `make run_stress_tests` — required by `CLAUDE.md §1c`.
3. Run `make run_tests lint`.

### Constraints

- Each additional slot reserves one file descriptor and one 8 KB receive buffer.
- The `NodeId → slot` routing table (REQ-6.1.9) is scanned linearly; very large values increase lookup time for each outbound unicast frame.

---

## 7. Worst-Case Stack Depth

| Field | Value |
|---|---|
| **Artifact** | `docs/STACK_ANALYSIS.md` |
| **Constant** | N/A — derived from call chain analysis |
| **Current worst case** | ~764 bytes / 10 frames (Chain 5: DTLS outbound send) |

### What it is

Because the codebase enforces Power of 10 Rule 1 (no recursion), the call graph is a directed acyclic graph.  The worst-case stack depth is the longest path in that graph, enumerated statically.

The dominant frame is `send_test_message()` in the demo client, which places a 256-byte payload buffer on the stack.  The production SC functions themselves are modest: `DeliveryEngine::send()` ≈ 64 bytes, `Serializer::serialize()` ≈ 80 bytes.

On all supported platforms (macOS / Linux) the default per-thread stack is ≥ 8 MB, giving >10 000× headroom.

### When to update the analysis

Per `CLAUDE.md §15`, update `docs/STACK_ANALYSIS.md` when:

- Any function introduces a stack-allocated buffer > 256 bytes.
- A new call chain exceeds 10 frames.
- A new thread entry point is added.

### How to reduce stack depth

The primary lever is moving large stack buffers out of intermediate functions and into the outermost caller (or into static storage at init time), thereby keeping individual frame sizes small.

---

## 8. Related Constants

These constants are not in the README capacity table but interact with the ones above.  Change them only when the constraint relationships described here are satisfied.

| Constant | Value | What it sizes | Key constraint |
|---|---|---|---|
| `FRAG_MAX_PAYLOAD_BYTES` | 1 024 B | Max bytes in one wire fragment | `+ WIRE_HEADER_SIZE_V2 ≤ DTLS_MAX_DATAGRAM_BYTES` |
| `FRAG_MAX_COUNT` | 4 | Max fragments per logical message | `× FRAG_MAX_PAYLOAD_BYTES ≥ MSG_MAX_PAYLOAD_BYTES` |
| `REASSEMBLY_SLOT_COUNT` | 8 | Concurrent in-flight fragmented messages | ≥ expected concurrent senders using large messages |
| `DTLS_MAX_DATAGRAM_BYTES` | 1 400 B | Max serialized DTLS datagram (MTU guard) | Conservative; leaves margin below 1 500 B Ethernet MTU |
| `ORDERING_PEER_COUNT` | 16 (= `MAX_PEER_NODES`) | Peers tracked by the ordering gate | Must equal `MAX_PEER_NODES` — see comment in `Types.hpp` |
| `ORDERING_HOLD_COUNT` | 8 | Out-of-order hold buffer depth | Increase if high-jitter links cause frequent head-of-line blocking |
| `IMPAIR_DELAY_BUF_SIZE` | 32 | Buffered delayed messages in impairment engine | Run stress tests if changed (`CLAUDE.md §1c`) |
| `MAX_PEER_NODES` | 16 | Total addressable peer node IDs | Drives `ORDERING_PEER_COUNT`; also affects routing table scan cost |
| `DELIVERY_EVENT_RING_CAPACITY` | 64 | Events in the observability ring | Increase if `drain_events()` is called infrequently under high traffic |
| `SOCKET_RECV_BUF_BYTES` | 8 192 B | Per-connection TCP/TLS receive buffer | Must be ≥ `MSG_MAX_PAYLOAD_BYTES + WIRE_HEADER_SIZE_V2` |

---

*This document is manually maintained.  When a constant in `src/core/Types.hpp` is added or removed, update this file to match.*
