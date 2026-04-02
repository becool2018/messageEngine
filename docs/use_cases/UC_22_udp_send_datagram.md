# UC_22 — UDP send: envelope serialised, impairments applied, datagram sent via sendto()

**HL Group:** HL-17 — UDP Plaintext Transport
**Actor:** User
**Requirement traceability:** REQ-6.2.1, REQ-6.2.3, REQ-6.2.4, REQ-4.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send()` with a transport backed by `UdpBackend`. `UdpBackend::send_message(envelope)` is called. File: `src/platform/UdpBackend.cpp`.
- **Goal:** Serialize the envelope, apply configured impairments, and send the resulting byte payload as a single UDP datagram via `sendto()`.
- **Success outcome:** `Result::OK` returned. The datagram has been passed to the kernel for delivery.
- **Error outcomes:**
  - `Result::ERR_FULL` — impairment delay buffer full.
  - `Result::ERR_IO` — `::sendto()` fails (socket error, permission denied, etc.).
  - Impairment loss/partition silently drops without error.

---

## 2. Entry Points

```cpp
// src/platform/UdpBackend.cpp
Result UdpBackend::send_message(const MessageEnvelope& envelope) override;
```

Called via virtual dispatch from `DeliveryEngine::send_via_transport()`.

---

## 3. End-to-End Control Flow

1. `UdpBackend::send_message(envelope)` — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. `uint64_t now_us = timestamp_now_us()` — current monotonic time.
4. **`flush_delayed_messages(now_us)`** — drain any previously-delayed envelopes from the delay buffer: for each due entry, `Serializer::serialize(env, m_wire_buf, sizeof(m_wire_buf), &wire_len)`, then `m_sock_ops->send_to(m_fd, m_wire_buf, wire_len, peer_ip, peer_port)`.
5. `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, &wire_len)` — converts `envelope` to big-endian wire bytes in `m_wire_buf`. Wire frame: 44-byte header + payload.
6. **`m_impairment.process_outbound(envelope, now_us, delay_buf, &delay_count)`** — applies impairments. With no impairments: `delay_buf[0] = envelope`, `delay_count = 1`.
7. **`m_impairment.collect_deliverable(delay_buf, delay_count, now_us, out_buf, &out_count)`** — returns due entries (immediately if no latency).
8. For each `out_buf[i]`: serialize again → `m_sock_ops->send_to(m_fd, wire, wire_len, peer_ip, peer_port)` → `::sendto()`.
9. Returns `Result::OK`.

---

## 4. Call Tree

```
UdpBackend::send_message()                      [UdpBackend.cpp]
 ├── timestamp_now_us()                          [Timestamp.cpp]
 ├── flush_delayed_messages(now_us)
 │    └── ISocketOps::send_to() -> ::sendto()
 ├── Serializer::serialize()                     [Serializer.cpp]
 ├── ImpairmentEngine::process_outbound()        [ImpairmentEngine.cpp]
 ├── ImpairmentEngine::collect_deliverable()
 └── [for each out_buf[i]:]
      Serializer::serialize() -> ISocketOps::send_to() -> ::sendto()
```

---

## 5. Key Components Involved

- **`UdpBackend`** — Connectionless transport. Sends to the configured `peer_ip:peer_port` via `::sendto()`.
- **`Serializer`** — Big-endian encoding. No framing header (no length prefix) needed for UDP since each datagram is self-contained.
- **`ImpairmentEngine`** — Loss/delay/duplication applied identically to the TCP path.
- **`ISocketOps::send_to()`** — Wraps `socket_send_to()` (`SocketUtils.cpp`) → `::sendto()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| Impairment drops envelope | `out_count = 0`; no sendto | sendto called |
| `::sendto()` fails | Log WARNING_LO; return ERR_IO | Return OK |
| Delayed entries due | Flush before new send | No flush needed |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. No threads created.
- Not thread-safe (`m_fd`, `m_wire_buf`, `ImpairmentEngine` are all non-atomic).

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) — `UdpBackend` member. Reused per call.
- `delay_buf[IMPAIR_DELAY_BUF_SIZE]` — 32-slot stack array for intermediate impairment processing.
- No heap allocation.

---

## 9. Error Handling Flow

- **`::sendto()` failure:** Logged `WARNING_LO`; `ERR_IO` returned.
- **Loss/partition drop:** `process_outbound()` returns `out_count=0`; no `sendto`; `OK` returned.
- **Delay buffer full:** Entry discarded; `OK` returned.

---

## 10. External Interactions

- **POSIX `::sendto()`:** On UDP socket `m_fd`. Direction: outbound. Single syscall per datagram.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `UdpBackend` | `m_wire_buf` | stale | serialized datagram |
| UDP socket | kernel send buffer | previous | + datagram enqueued |

---

## 12. Sequence Diagram

```
DeliveryEngine::send_via_transport()
  -> UdpBackend::send_message(envelope)
       -> timestamp_now_us()
       -> flush_delayed_messages(now_us)         [drain prior delayed]
       -> Serializer::serialize(envelope, ...)
       -> ImpairmentEngine::process_outbound()   [loss/dup/delay]
       -> ImpairmentEngine::collect_deliverable() [get due entries]
       -> ISocketOps::send_to(fd, wire, len)
            -> ::sendto()                        [POSIX UDP]
       <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `UdpBackend::init(config)` called; `m_fd` is a bound UDP socket; `peer_ip` and `peer_port` configured.

**Runtime:**
- Each `send_message()` call serializes and sends one datagram independently. No connection management.

---

## 14. Known Risks / Observations

- **No framing header:** Unlike TCP, UDP has no length-prefix in the payload; the datagram size is used directly. The receiver's `recvfrom()` must read the full datagram in one call.
- **MTU fragmentation:** Datagrams larger than the network MTU (~1500 bytes for Ethernet) will be fragmented by the IP layer. For DTLS, `DTLS_MAX_DATAGRAM_BYTES=1400` is enforced explicitly (UC_40).
- **No delivery guarantee:** UDP datagrams can be lost; RELIABLE_RETRY + `DuplicateFilter` is required for reliable delivery over UDP.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `flush_delayed_messages()` is called at the top of `send_message()` to drain the impairment delay buffer before processing the new message. This is consistent with TcpBackend's `flush_delayed_to_clients()` pattern.
