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
9. [Static RAM Footprint Summary](#9-static-ram-footprint-summary)

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

### Why

4 096 bytes covers the common embedded and avionics message sizes (telemetry frames, command blocks, file-transfer chunks) without requiring heap allocation.  It is large enough that most application messages fit in a single logical envelope, yet small enough that a `MessageEnvelope` on the stack does not exhaust the thread stack budget documented in `docs/STACK_ANALYSIS.md`.  The value is exactly four times `FRAG_MAX_PAYLOAD_BYTES` (1 024 B), which gives the reassembly layer a clean 4-fragment maximum and keeps `FRAG_MAX_COUNT` at 4 — a power of two that simplifies index arithmetic and static analysis of loop bounds (Power of 10 Rule 2).

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

### Why

128 entries provides headroom for the product `MAX_RETRY_COUNT × MAX_PEER_NODES` (5 × 16 = 80) with a comfortable margin.  If the window were sized exactly to that product, a single slow peer that exhausts all retries before its ACK arrives could push its own earlier retransmissions out of the window, causing a false-negative dedup and a duplicate delivery.  Doubling the product (≈ 160) and rounding down to the next lower power of two (128) balances protection against false negatives with the 1 024-byte static memory cost.  A power of two was chosen deliberately: the age-based eviction scan (`find_evict_idx()`) iterates over exactly `DEDUP_WINDOW_SIZE` entries, and a power-of-two bound is the clearest proof of a finite loop for static analysis tools (Power of 10 Rule 2).

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

### Why

32 slots matches the typical pipeline depth of a hub-and-spoke reliable messaging system: with 16 peers and 2 concurrent reliable messages per peer in flight, 32 slots are exactly saturated.  This prevents unbounded resource growth if ACKs are delayed — when all slots are full, back-pressure is applied immediately (the caller receives `ERR_FULL`) rather than silently queuing more messages and running out of memory.  The `pump_retries()` scan is O(ACK_TRACKER_CAPACITY), so keeping the capacity modest also bounds the worst-case execution time of that safety-critical function (see `docs/WCET_ANALYSIS.md`).

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

### Why

5 attempts balances two opposing risks.  Too few retries mean a transient loss event (a single dropped packet) results in a premature failure report.  Too many retries lengthen the time-to-detect a permanently failed peer: with exponential backoff capped at 60 s, 5 retries bound worst-case detection latency at approximately 1 + 2 + 4 + 8 + 60 = 75 s — an operationally acceptable ceiling for infrastructure software.  The value also keeps `DEDUP_WINDOW_SIZE` (128) comfortably above the `MAX_RETRY_COUNT × MAX_PEER_NODES` product (80), preserving the dedup safety margin described in §2.

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

### Why

64 slots is exactly double `ACK_TRACKER_CAPACITY` (32).  The inbound ring must be able to absorb a burst of ACK and DATA messages from all peers simultaneously — if the ring were smaller than the number of in-flight reliable messages, an ACK burst could trigger ring-full drops, causing the sender to time out messages that actually succeeded.  Doubling the ACK tracker capacity gives the consumer (`DeliveryEngine::receive()`) a full ACK-tracker-worth of headroom to drain before any message is lost.  The value is also the bound used by `pump_retries()` for its pre-allocated `m_retry_buf` member array, so both structures share a single well-justified number.

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

### Why

8 connections supports the primary hub-and-spoke topology targeted by this library (`MAX_PEER_NODES` = 16 addressable nodes, but a single server hub typically handles a subset at once).  The NodeId → slot routing table (REQ-6.1.9) is scanned linearly on every outbound unicast frame; keeping the connection count in single digits bounds that scan to a negligible number of iterations.  Each additional slot also consumes one file descriptor and one 8 KB receive buffer (~65 KB total for 8 slots), so the value was chosen to stay well within the static memory budget.  Applications requiring more than 8 simultaneous clients should evaluate whether a larger value is justified against the increased scan cost and memory footprint.

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
| **Current worst case** | ~764 B / 9 frames (Chain 5: DTLS outbound send); 11 frames / ~592 B (Chain 3: retry pump) — DEF-031-1 resolved: flush-path stack ~130 KB → ~592 B |

### What it is

Because the codebase enforces Power of 10 Rule 1 (no recursion), the call graph is a directed acyclic graph.  The worst-case stack depth is the longest path in that graph, enumerated statically.

The dominant frame is `send_test_message()` in the demo client, which places a 256-byte payload buffer on the stack.  The production SC functions themselves are modest: `DeliveryEngine::send()` ≈ 64 bytes, `Serializer::serialize()` ≈ 80 bytes.

On all supported platforms (macOS / Linux) the default per-thread stack is ≥ 8 MB, giving >10 000× headroom.

### Why

Stack depth analysis is a NASA-STD-8719.13C obligation for safety-critical software.  The no-recursion rule (Power of 10 Rule 1) is what makes static worst-case analysis possible: without recursion the call graph is a DAG and the longest path can be enumerated exhaustively.  Documenting the worst case here serves two purposes: (1) it gives embedded porters a concrete number to check against their platform's stack size before deployment, and (2) it provides a regression baseline — if a future change increases the worst-case depth beyond the documented value, the `docs/STACK_ANALYSIS.md` update trigger fires and the change is explicitly reviewed before merge.

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

## 9. Static RAM Footprint Summary

All memory is allocated statically at compile time.  No heap allocation occurs after `init()`.
The dominant cost is `MessageEnvelope` (≈ 4,144 bytes each at the default `MSG_MAX_PAYLOAD_BYTES = 4096`),
which is embedded inline in every pre-allocated buffer across `DeliveryEngine` and its sub-components.

### Per-buffer breakdown (one `DeliveryEngine` + `TcpBackend` instance)

| Buffer | Count | Each | Total |
|---|---|---|---|
| `DeliveryEngine::m_retry_buf` | 64 | 4,144 B | ~265 KB |
| `AckTracker::m_slots` (envelope + deadline + state) | 32 | ~4,160 B | ~130 KB |
| `RetryManager::m_slots` (envelope + schedule metadata) | 32 | ~4,176 B | ~130 KB |
| `DeliveryEngine::m_timeout_buf` | 32 | 4,144 B | ~129 KB |
| `ReassemblyBuffer` slots | 8 | ~4,200 B | ~34 KB |
| `OrderingBuffer` slots | 8 | ~4,200 B | ~34 KB |
| `DeliveryEngine::m_ordering_freed_buf` | 8 | 4,144 B | ~32 KB |
| `DeliveryEngine::m_frag_buf` | 4 | 4,144 B | ~16 KB |
| `TcpBackend` recv buf (×8 clients) | 8 | 8,192 B | ~64 KB |
| `DuplicateFilter::m_window` | 128 | 32 B | ~4 KB |
| `DeliveryEngine::m_held_pending` | 1 | 4,144 B | ~4 KB |
| **Total** | | | **~842 KB** |

### The single dominant variable

`MSG_MAX_PAYLOAD_BYTES` multiplies across approximately 189 embedded `MessageEnvelope` instances.
Halving it from 4 096 to 2 048 reduces the total footprint by roughly 390 KB.

### Tuning for constrained targets

The three constants with the greatest impact per unit change:

| Constant | Default | Reduce to | Approx. saving |
|---|---|---|---|
| `MSG_MAX_PAYLOAD_BYTES` | 4 096 B | 2 048 B | ~390 KB |
| `MSG_RING_CAPACITY` | 64 | 16 | ~200 KB |
| `ACK_TRACKER_CAPACITY` | 32 | 8 | ~130 KB |

Reducing all three to the values above brings the total footprint below 200 KB.
Always run `make run_stress_tests` after changing any capacity constant (`CLAUDE.md §1c`).

### Public API surface

For reference: `DeliveryEngine` exposes 9 public methods (4 safety-critical, 5 NSC observability).
`TransportInterface` exposes 5 abstract methods.  The full API is documented in
`src/core/DeliveryEngine.hpp` and `src/core/TransportInterface.hpp`.

---

*This document is manually maintained.  When a constant in `src/core/Types.hpp` is added or removed, update this file to match.*
