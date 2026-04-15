# UC_40 — MTU enforcement: outbound message whose serialised size exceeds DTLS_MAX_DATAGRAM_BYTES is rejected with ERR_INVALID

**HL Group:** HL-19 — DTLS-Encrypted UDP Transport
**Actor:** User
**Requirement traceability:** REQ-6.4.4, REQ-4.1.2, REQ-6.2.3

---

## 1. Use Case Overview

- **Trigger:** User calls `DtlsUdpBackend::send_message(envelope)` where `WIRE_HEADER_SIZE + envelope.payload_length > DTLS_MAX_DATAGRAM_BYTES`. File: `src/platform/DtlsUdpBackend.cpp`.
- **Goal:** Reject the oversized message before any encryption attempt to prevent IP fragmentation and ensure DTLS record size stays within the configured MTU limit.
- **Success outcome (rejection path):** `send_message()` returns `Result::ERR_INVALID`. No data is sent. No mbedTLS call is made.
- **Error outcomes:**
  - `Result::ERR_INVALID` — serialized size (WIRE_HEADER_SIZE + payload_length) exceeds `DTLS_MAX_DATAGRAM_BYTES`.

---

## 2. Entry Points

```cpp
// src/platform/DtlsUdpBackend.cpp
Result DtlsUdpBackend::send_message(const MessageEnvelope& envelope) override;
```

---

## 3. End-to-End Control Flow

1. **`DtlsUdpBackend::send_message(envelope)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. **MTU check:** compute `wire_size = WIRE_HEADER_SIZE + envelope.payload_length`.
4. If `wire_size > DTLS_MAX_DATAGRAM_BYTES`:
   a. `LOG_WARN_LO("DtlsUdpBackend", "Message too large: %u > %u", wire_size, DTLS_MAX_DATAGRAM_BYTES)`.
   b. Return `Result::ERR_INVALID`.
5. (On pass) `Serializer::serialize(envelope, m_wire_buf, sizeof(m_wire_buf), &wire_len)`.
6. If `tls_enabled`: `mbedtls_ssl_write(&m_ssl, m_wire_buf, wire_len)` — DTLS encrypted send.
7. Else: `::send(m_udp_fd, m_wire_buf, wire_len, 0)` — plaintext UDP send.
8. Check return value; on error return `ERR_IO`.
9. Returns `Result::OK`.

---

## 4. Call Tree

```
DtlsUdpBackend::send_message(envelope)             [DtlsUdpBackend.cpp]
 ├── [MTU check: WIRE_HEADER_SIZE + payload_length > DTLS_MAX_DATAGRAM_BYTES]
 │    └── LOG_WARN_LO()                  [if oversized; return ERR_INVALID]
 ├── Serializer::serialize()                        [only if size check passes]
 └── mbedtls_ssl_write() / ::send()                [DTLS or plaintext]
```

---

## 5. Key Components Involved

- **MTU size check** — arithmetic guard before any mbedTLS or socket call; prevents IP fragmentation (REQ-6.4.4).
- **`DTLS_MAX_DATAGRAM_BYTES = 1400`** — capacity constant in `src/core/Types.hpp`; the threshold enforced by this check.
- **`WIRE_HEADER_SIZE = 44`** — serialized envelope header size; added to `payload_length` to compute full wire size.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `wire_size > DTLS_MAX_DATAGRAM_BYTES` | Log WARNING_LO; return ERR_INVALID | Proceed with serialization + send |
| `m_tls_enabled` | `mbedtls_ssl_write()` | `::send()` plaintext |
| `mbedtls_ssl_write()` / `::send()` returns error | Return ERR_IO | Return OK |

---

## 7. Concurrency / Threading Behavior

- Called from the application send thread; single-threaded per-call.
- No `std::atomic` operations on this path.
- `m_ssl` is not shared across threads; only the calling thread accesses it.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[DTLS_MAX_DATAGRAM_BYTES]` (or similar fixed buffer) — stack or static member; used for the serialized wire bytes.
- No heap allocation on this path.
- Power of 10 Rule 3: no dynamic allocation (the MTU check prevents the buffer from ever being overflowed).

---

## 9. Error Handling Flow

- **Oversized message:** `ERR_INVALID` returned immediately. The envelope is not serialized or encrypted. `m_open` and session state are unaffected.
- **Serialization failure:** `Serializer::serialize()` returns non-OK; `send_message()` returns `ERR_IO`. Caller must handle.
- **mbedTLS write failure:** `mbedtls_ssl_write()` returns error; logged `WARNING_LO`; `ERR_IO` returned.

---

## 10. External Interactions

- **mbedTLS:** `mbedtls_ssl_write()` called only if MTU check passes and `tls_enabled == true`.
- **POSIX `::send()`:** called only if MTU check passes and `tls_enabled == false`.
- No file I/O or additional OS calls on the rejection path.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DtlsUdpBackend` | `m_open` | true | true (unchanged) |
| `DtlsUdpBackend` | `m_ssl` | active session | active session (unchanged) |

On the rejection path, no state is modified. The oversized envelope is dropped silently except for the WARNING_LO log entry.

---

## 12. Sequence Diagram

```
User -> DtlsUdpBackend::send_message(envelope)
  [wire_size = WIRE_HEADER_SIZE + payload_length]
  [wire_size > DTLS_MAX_DATAGRAM_BYTES?]
  -> LOG_WARN_LO("Message too large")
  <- Result::ERR_INVALID

[Pass case: wire_size <= DTLS_MAX_DATAGRAM_BYTES]
  -> Serializer::serialize()
  -> mbedtls_ssl_write() [or ::send() if plaintext]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DtlsUdpBackend::init()` has been called; `m_open == true`.
- For DTLS mode: `m_has_session == true` (handshake complete).

**Runtime:**
- Every `send_message()` call performs the MTU check before any work. This is a constant-time guard on every outbound message.

---

## 14. Known Risks / Observations

- **Silent message loss:** The caller receives `ERR_INVALID` but no notification that the message was too large is propagated up the stack beyond the return code. Application code must check the return value.
- **DTLS record overhead:** The actual maximum plaintext payload that fits in a DTLS record is slightly smaller than `DTLS_MAX_DATAGRAM_BYTES` due to DTLS record header and AEAD tag overhead. The MTU constant should account for this margin.
- **Plaintext fallback:** When `tls_enabled == false`, the same MTU limit is applied for consistency, even though UDP itself can carry larger datagrams (REQ-6.4.5 plaintext fallback).

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `DTLS_MAX_DATAGRAM_BYTES = 1400` is the constant enforced by the MTU check. Value confirmed from Types.hpp knowledge; exact constant name inferred from DtlsUdpBackend.cpp.
- `[ASSUMPTION]` The MTU check uses `WIRE_HEADER_SIZE + envelope.payload_length` before calling `Serializer::serialize()`, not after. This is the correct order to avoid unnecessary serialization work on oversized messages.
