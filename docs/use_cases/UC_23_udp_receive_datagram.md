# UC_23 — UDP receive: datagram received via recvfrom(), deserialised, enqueued for delivery

**HL Group:** HL-17 — UDP Plaintext Transport
**Actor:** User
**Requirement traceability:** REQ-6.2.1, REQ-6.2.2, REQ-6.2.4, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** `UdpBackend::receive_message(out, timeout_ms)` is called by `DeliveryEngine::receive()`. File: `src/platform/UdpBackend.cpp`.
- **Goal:** Wait for an incoming UDP datagram, receive it via `recvfrom()`, deserialize it into a `MessageEnvelope`, and either return it directly or enqueue it for delivery.
- **Success outcome:** `Result::OK` returned; `out` is populated with the received envelope.
- **Error outcomes:**
  - `Result::ERR_TIMEOUT` — no datagram received within `timeout_ms`.
  - `Result::ERR_IO` — `::recvfrom()` error.
  - `Result::ERR_INVALID` — `Serializer::deserialize()` fails (malformed datagram).
  - Inbound impairment (partition/reorder) may cause `out_count == 0`; function loops.

---

## 2. Entry Points

```cpp
// src/platform/UdpBackend.cpp
Result UdpBackend::receive_message(MessageEnvelope& out, uint32_t timeout_ms) override;
```

Called via virtual dispatch from `DeliveryEngine::receive()`.

---

## 3. End-to-End Control Flow

1. `UdpBackend::receive_message(out, timeout_ms)` — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. Try `m_recv_queue.pop(out)` first — if data is already queued (from prior `recv_one_datagram()` call): return `OK` immediately.
4. **Bounded poll loop** (Power of 10 Rule 2: `poll_count = min(50, (timeout_ms+99)/100)` iterations of 100ms):
   a. `uint64_t now_us = timestamp_now_us()`.
   b. **`flush_delayed_to_queue(now_us)`** — drain inbound delay buffer to `m_recv_queue`.
   c. Try `m_recv_queue.pop(out)` — if successful: return `OK`.
   d. **`recv_one_datagram(100)`** — blocks up to 100ms:
      - `::poll({m_fd, POLLIN, 0}, 1, 100)` — wait for data.
      - If timeout: continue outer loop.
      - If data: `m_sock_ops->recv_from(m_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, &wire_len, src_ip, src_port)` → `::recvfrom()`. Reads one full datagram.
      - Validates source IP against configured peer (`config.peer_ip`) if `verify_peer` enabled.
      - `Serializer::deserialize(m_wire_buf, wire_len, env)` — decodes wire bytes. If fails: log `WARNING_LO`; continue loop.
      - `m_impairment.process_inbound(env, now_us, inbound_buf, &inbound_count)`.
      - `flush_delayed_to_queue(now_us)` — push deliverable inbound envelopes to ring.
   e. Try `m_recv_queue.pop(out)` — return `OK` if data available.
5. After `poll_count` iterations: return `Result::ERR_TIMEOUT`.

---

## 4. Call Tree

```
UdpBackend::receive_message(out, timeout_ms)     [UdpBackend.cpp]
 ├── RingBuffer::pop()                            [RingBuffer.hpp]
 └── [bounded loop:]
      ├── flush_delayed_to_queue(now_us)
      │    └── RingBuffer::push()
      ├── RingBuffer::pop()
      └── recv_one_datagram(100)                  [UdpBackend.cpp]
           ├── ::poll()                           [POSIX]
           ├── ISocketOps::recv_from() -> ::recvfrom()
           ├── Serializer::deserialize()          [Serializer.cpp]
           ├── ImpairmentEngine::process_inbound()
           └── flush_delayed_to_queue() -> RingBuffer::push()
```

---

## 5. Key Components Involved

- **`recv_one_datagram()`** — Reads one UDP datagram, deserializes it, applies inbound impairments, and pushes to ring.
- **`RingBuffer`** — SPSC queue for deserialized inbound envelopes.
- **`Serializer::deserialize()`** — Decodes big-endian wire bytes.
- **`ImpairmentEngine::process_inbound()`** — Partition/reorder applied to inbound datagrams.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_recv_queue.pop()` succeeds immediately | Return OK | Enter poll loop |
| `::poll()` times out (100ms) | Continue outer loop | Process datagram |
| `deserialize()` fails | Log; continue loop | Push to ring |
| Inbound impairment drops | `out_count=0`; continue | Push to ring |
| `poll_count` exhausted | Return `ERR_TIMEOUT` | Continue |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread.
- `RingBuffer::push()` — `m_head` atomic release store. `pop()` — `m_tail` atomic acquire load.
- Not thread-safe for multiple concurrent callers.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) — `UdpBackend` member.
- No heap allocation.

---

## 9. Error Handling Flow

- **`ERR_TIMEOUT`:** Normal for quiet period. Caller loops back.
- **`ERR_IO`:** Socket error; `UdpBackend` state may be inconsistent. Caller should close and reinitialize.
- **`ERR_INVALID`:** Malformed datagram discarded; no state change. Caller loops.

---

## 10. External Interactions

- **POSIX `::poll()`:** On UDP socket `m_fd`. Inbound wait.
- **POSIX `::recvfrom()`:** Reads one complete UDP datagram. Inbound.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `UdpBackend` | `m_wire_buf` | stale | received datagram |
| `RingBuffer` | `m_buf[m_head]` | old slot | deserialized `MessageEnvelope` |
| `RingBuffer` | `m_head` (atomic) | H | H+1 (release store) |

---

## 12. Sequence Diagram

```
DeliveryEngine::receive()
  -> UdpBackend::receive_message(out, timeout_ms)
       -> RingBuffer::pop()                 [empty; continue]
       [loop: poll_count iterations]
            -> ::poll(fd, POLLIN, 100ms)    <- POLLIN
            -> ISocketOps::recv_from()      -> ::recvfrom()
            -> Serializer::deserialize()    <- OK
            -> ImpairmentEngine::process_inbound()
            -> flush_delayed_to_queue() -> RingBuffer::push()
            -> RingBuffer::pop()            <- OK; out populated
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `UdpBackend::init(config)` called; `m_fd` bound.

**Runtime:**
- Each `receive_message()` call blocks for up to `timeout_ms` milliseconds (in 100ms increments).

---

## 14. Known Risks / Observations

- **One datagram per loop iteration:** If multiple datagrams arrive simultaneously, only one is processed per 100ms poll window per `recv_one_datagram()` call.
- **No framing validation:** UDP datagrams arrive as complete messages. If a datagram is truncated (rare on loopback), `deserialize()` will return an error and the datagram is discarded.
- **Source validation:** If `verify_peer` is enabled, datagrams from unexpected sources are discarded silently.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Source IP validation against `config.peer_ip` is performed in `recv_one_datagram()` when `config.verify_peer` is set. For tests without peer verification, any source is accepted.
