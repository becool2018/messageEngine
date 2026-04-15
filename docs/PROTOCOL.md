# Protocol Specification — Message Envelope and Delivery Semantics

**Normative REQ IDs:** defined in `CLAUDE.md §§3.1–3.3`. This document contains the full prose specification and rationale. When a conflict exists, the REQ ID lines in `CLAUDE.md` take precedence.
**Last updated:** 2026-04-15

---

## 3.1 Message Envelope

Every logical application message must be carried inside a standard envelope. The envelope fields are:

| Field | Purpose |
|---|---|
| `message_type` | Enumerated type (DATA, ACK, HELLO, …) |
| `message_id` | Unique per sender or globally; used for deduplication |
| `timestamp` | Creation or enqueue time (microseconds) |
| `source_id` | Sending node/endpoint identifier |
| `destination_id` | Receiving node/endpoint identifier; NODE_ID_INVALID (0) = broadcast |
| `priority` | Used for channel prioritization |
| `reliability_class` | `BEST_EFFORT`, `RELIABLE_ACK`, or `RELIABLE_RETRY` |
| `expiry_time` | Absolute wall-clock or relative deadline; used to drop stale messages |
| `payload` | Opaque, serialized bytes |

All fields are defined in `src/core/MessageEnvelope.hpp`.

---

## 3.2 Messaging Utilities

### 3.2.1 Message ID Generation and Comparison
`MessageIdGen` in `src/core/MessageIdGen.hpp` assigns monotonically increasing IDs per session. Comparison helpers are provided for equality and ordering.

### 3.2.2 Timestamps and Expiry Checking
`timestamp_now_us()` reads `CLOCK_MONOTONIC` via `IPosixSyscalls`. `timestamp_deadline_us(offset_us)` computes an absolute deadline. Expiry is checked in `DeliveryEngine::send()` (outbound) and `DeliveryEngine::receive()` (inbound) by comparing `expiry_time` against the current monotonic clock.

### 3.2.3 Serialization / Deserialization
`Serializer::serialize()` and `Serializer::deserialize()` in `src/core/Serializer.hpp` convert envelopes to and from a deterministic, big-endian wire format. The wire frame layout:

```
Bytes  0– 1   frame_magic   (PROTO_MAGIC — fixed; never changed)
Byte   2       message_type
Byte   3       proto_version (PROTO_VERSION — bump on any layout change)
Bytes  4– 7   source_id
Bytes  8–11   destination_id
Bytes 12–15   message_id (low 32 bits)
Bytes 16–23   timestamp_us
Bytes 24–27   expiry_time_us (offset from timestamp)
Byte  28       reliability_class
Byte  29       priority
Bytes 30–33   payload_length
Bytes 34–37   sequence_number
Bytes 38–39   fragment_index / fragment_count
Bytes 40–41   frame_magic (second copy — PROTO_MAGIC)
Bytes 42+      payload bytes
```

Constants `PROTO_VERSION` and `PROTO_MAGIC` are defined in `src/core/ProtocolVersion.hpp`. The deserializer rejects frames whose version byte ≠ `PROTO_VERSION` or whose magic word ≠ `PROTO_MAGIC`.

### 3.2.4 ACK Handling
`AckTracker` in `src/core/AckTracker.hpp` maintains a fixed-capacity array of `ACK_TRACKER_CAPACITY` pending slots. Each slot holds the message ID and a deadline. `DeliveryEngine::receive()` feeds incoming ACK envelopes to `AckTracker::on_ack()`, which transitions the matching slot from `PENDING` to `ACKED`. `DeliveryEngine::sweep_ack_timeouts()` is called each main-loop iteration to collect expired slots.

### 3.2.5 Retry Logic
`RetryManager` in `src/core/RetryManager.hpp` maintains a fixed-capacity array of `RETRY_MANAGER_CAPACITY` retry slots. Each slot holds the envelope copy, retry count, backoff deadline, and state (`INACTIVE`, `ACTIVE`, `EXHAUSTED`). `DeliveryEngine::pump_retries()` fires due retries each tick. Slots transition: `INACTIVE → ACTIVE` on `schedule()`, `ACTIVE → INACTIVE` on ACK or exhaustion.

### 3.2.6 Duplicate Suppression
`DuplicateFilter` in `src/core/DuplicateFilter.hpp` maintains a sliding window of `DEDUP_WINDOW_SIZE` `(source_id, message_id)` pairs. `check_and_record()` returns `true` if the pair is already in the window (duplicate) and inserts it otherwise. The oldest entry is evicted when the window is full.

### 3.2.7 Expiry Handling
Outbound: `DeliveryEngine::send()` checks `expiry_time` before queuing; expired messages are dropped with an `EXPIRY_DROP` observability event.
Inbound: `DeliveryEngine::receive()` checks `expiry_time` after dequeuing; stale messages are dropped before delivery to the application.

### 3.2.8 Wire Format Version Enforcement
- `PROTO_VERSION` (byte 3): bump on any change to wire field layout. Current value: see `src/core/ProtocolVersion.hpp`.
- `PROTO_MAGIC` (bytes 40–41): fixed; never changed.
- `Serializer::deserialize()` rejects frames with mismatched version or magic and returns `ERR_INVALID`.
- Rationale: prevents silent mis-parsing of frames from incompatible builds.

### 3.2.9 Stale Reassembly Slot Reclamation
`ReassemblyBuffer::sweep_stale()` in `src/core/ReassemblyBuffer.hpp` is called from `DeliveryEngine::sweep_ack_timeouts()`. Any reassembly slot open longer than `recv_timeout_ms` without receiving all fragments is closed and its memory reclaimed. Prevents resource exhaustion from peers that send partial fragment sets.

### 3.2.10 Wire-Supplied Length Field Overflow Guard
`frame_len` (from `tcp_recv_frame()`) and `total_payload_length` (from fragment reassembly) are externally supplied and must be validated before any arithmetic. The compile-time ceiling is `WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES`. Values exceeding the ceiling cause `ERR_INVALID` + `WARNING_HI`. Rationale: CERT INT30-C unsigned wrap; HAZ-019 (CWE-190).

### 3.2.11 Constant-Time Equality for Security-Sensitive Identifiers
All `(source_id, message_id)` equality comparisons in `AckTracker`, `DuplicateFilter`, and `OrderingBuffer` must use a comparator that does not short-circuit on the first differing byte. The loop in `DuplicateFilter::is_duplicate()` must not early-exit on match. DTLS cookie comparisons in `DtlsUdpBackend` use `mbedtls_ct_memcmp()`. Rationale: HAZ-018, CWE-208 timing oracle.

---

## 3.3 Delivery Semantics

### 3.3.1 Best Effort
No retry, no ACK. The message is sent once; if it is lost or the peer is unreachable, the application is not notified. Use for high-frequency telemetry where occasional loss is acceptable.

### 3.3.2 Reliable with ACK
Single transmission. `AckTracker` allocates a slot and records the deadline. If no ACK arrives before the deadline, `sweep_ack_timeouts()` fires an `ACK_TIMEOUT` event. There is no automatic retransmission; the application decides whether to retry. Use when the application needs delivery confirmation but controls its own retry policy.

### 3.3.3 Reliable with Retry and Dedupe
`RetryManager` retransmits the message at increasing backoff intervals until an ACK arrives or the message expires. The receiver runs `DuplicateFilter` to suppress duplicates caused by retransmission. Use for fire-and-forget reliable delivery without application-level retry management.

### 3.3.4 Expiring Messages
Any message with a non-zero `expiry_time` is subject to TTL enforcement. The message is dropped (not delivered) if the current time exceeds `expiry_time` at either the send or receive point. Both paths emit `EXPIRY_DROP` observability events.

### 3.3.5 Ordered vs Unordered Delivery
Ordered channels use `OrderingBuffer` to hold out-of-sequence messages and release them in sequence-number order. Unordered channels bypass `OrderingBuffer` and deliver messages as they arrive. Channel ordering is configured via `ChannelConfig::ordering`.

### 3.3.6 Peer Reconnect — Ordering State Reset
When a peer reconnects, it starts a new connection session with sequence numbers beginning at 0. Without a reset, the receiver's `OrderingBuffer` retains the prior session's `next_expected_seq` and stalls all new messages.

Detection: a `HELLO` frame received on a channel that already has a registered `source_id` indicates a reconnect.

Reset sequence:
1. `DeliveryEngine::drain_hello_reconnects()` is called on every `receive()` iteration; it drains the `m_hello_reconnects` queue populated by the transport.
2. For each reconnecting peer, `DeliveryEngine::reset_peer_ordering()` calls `OrderingBuffer::reset_peer(source_id)` to clear the sequence state and free held messages.
3. The `m_held_pending` staging buffer in `DeliveryEngine` is discarded if it belongs to the reconnecting peer.
4. `TransportInterface::pop_hello_peer()` is the interface the transport uses to signal a reconnect to `DeliveryEngine`.
