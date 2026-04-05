# UC_25 — Serializer encode: Serializer::serialize() called internally by all backends

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System
**Requirement traceability:** REQ-3.2.3, REQ-3.2.8, REQ-6.1.5

---

## 1. Use Case Overview

- **Trigger:** Any backend calls `Serializer::serialize(envelope, buf, buf_cap, &wire_len)` on the send path. File: `src/core/Serializer.cpp`.
- **Goal:** Convert a `MessageEnvelope` struct into a deterministic big-endian wire byte sequence.
- **Success outcome:** `Result::OK` returned; `buf[0..wire_len-1]` contains the 52-byte header + payload bytes. `*wire_len = WIRE_HEADER_SIZE + envelope.payload_length`.
- **Error outcomes:**
  - `Result::ERR_INVALID` — `envelope.payload_length > MSG_MAX_PAYLOAD_BYTES`.
  - `Result::ERR_INVALID` — `WIRE_HEADER_SIZE + payload_length > buf_cap`.

---

## 2. Entry Points

```cpp
// src/core/Serializer.cpp (static)
static Result Serializer::serialize(
    const MessageEnvelope& envelope,
    uint8_t* buf, uint32_t buf_cap,
    uint32_t* wire_len);
```

Called by `TcpBackend`, `UdpBackend`, `TlsTcpBackend`, `DtlsUdpBackend` on every send.

---

## 3. End-to-End Control Flow

1. `Serializer::serialize(envelope, buf, buf_cap, wire_len)` — entry.
2. `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(wire_len != nullptr)`.
3. **Validate:** `envelope.payload_length > MSG_MAX_PAYLOAD_BYTES` → return `ERR_INVALID`.
4. **Validate:** `WIRE_HEADER_SIZE + envelope.payload_length > buf_cap` → return `ERR_INVALID`.
5. **Write header fields** using `write_u8` / `write_u32` / `write_u64` helpers (big-endian):
   - Byte 0: `message_type` (1 byte).
   - Byte 1: `reliability_class` (1 byte).
   - Byte 2: `priority` (1 byte).
   - Byte 3: `PROTO_VERSION` (from `ProtocolVersion.hpp`; receiver rejects any other value).
   - Bytes 4–11: `message_id` (8 bytes BE).
   - Bytes 12–19: `timestamp_us` (8 bytes BE).
   - Bytes 20–23: `source_id` (4 bytes BE).
   - Bytes 24–27: `destination_id` (4 bytes BE).
   - Bytes 28–35: `expiry_time_us` (8 bytes BE).
   - Bytes 36–39: `payload_length` (4 bytes BE).
   - Bytes 40–41: `PROTO_MAGIC` high then low byte (0x4D, 0x45 = 'ME'; from `ProtocolVersion.hpp`).
   - Bytes 42–43: `0x0000` reserved.
   - Bytes 44 onward: `memcpy(buf + WIRE_HEADER_SIZE, envelope.payload, payload_length)`.
6. `*wire_len = WIRE_HEADER_SIZE + envelope.payload_length`.
7. `NEVER_COMPILED_OUT_ASSERT(*wire_len <= buf_cap)`.
8. Returns `Result::OK`.

---

## 4. Call Tree

```
Serializer::serialize(envelope, buf, buf_cap, wire_len)  [Serializer.cpp]
 ├── write_u8(buf, offset, value)   [×3]
 ├── write_u64(buf, offset, value)  [×3]
 ├── write_u32(buf, offset, value)  [×3]
 └── memcpy(buf + WIRE_HEADER_SIZE, payload, length)
```

---

## 5. Key Components Involved

- **`Serializer`** — Stateless. All methods are static. Provides one canonical wire encoding used by every transport.
- **`write_u8/u32/u64`** — Private static helpers that write multi-byte values in big-endian order using byte-by-byte writes (no alignment assumptions, MISRA compliant).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `payload_length > MSG_MAX_PAYLOAD_BYTES` | Return ERR_INVALID | Continue |
| `WIRE_HEADER_SIZE + payload_length > buf_cap` | Return ERR_INVALID | Continue |

---

## 7. Concurrency / Threading Behavior

- Stateless — `buf` and `envelope` are caller-provided. No shared state.
- Thread-safe as long as callers provide non-aliasing buffers.

---

## 8. Memory & Ownership Semantics

- `buf` — caller-owned; `serialize()` only writes to it.
- No heap allocation. Power of 10 Rule 3 satisfied.
- Stack: only loop counters and the write helpers' local byte temporaries.

---

## 9. Error Handling Flow

- **`ERR_INVALID`:** Caller should not call `send()` with an oversized payload. The error is propagated up through the backend's `send_message()`.
- **No partial writes:** Either the full frame is written or `ERR_INVALID` is returned before any bytes are written.

---

## 10. External Interactions

- None — operates entirely in process memory (writes to a caller-provided buffer).

---

## 11. State Changes / Side Effects

- `buf[0..wire_len-1]` — overwritten with wire frame bytes.
- `*wire_len` — set to total frame size.
- No other state changed.

---

## 12. Sequence Diagram

```
TcpBackend::send_message()
  -> Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, &wire_len)
       -> write_u8/u32/u64 helpers   [header fields; big-endian]
       -> memcpy(payload)
       -> *wire_len = 44 + payload_length
  <- Result::OK; m_wire_buf[0..wire_len-1] populated
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Caller must provide `buf_cap >= WIRE_HEADER_SIZE + payload_length`.
- `envelope.payload_length <= MSG_MAX_PAYLOAD_BYTES`.

**Runtime:**
- Called on every outbound send. Stateless; no initialization required.

---

## 14. Known Risks / Observations

- **Safety-critical function:** `Serializer::serialize()` is listed as a target for MC/DC coverage (docs/HAZARD_ANALYSIS.md HAZ-005). Wire format errors can cause the receiver to misinterpret message type, source, or payload.
- **Big-endian byte order:** All multi-byte fields are written in network byte order (big-endian). Any deserializer must use matching byte-order logic.
- **Protocol version and magic:** Byte 3 carries `PROTO_VERSION`; bytes 40–41 carry `PROTO_MAGIC`. Bytes 42–43 are reserved zero. Bump `PROTO_VERSION` in `src/core/ProtocolVersion.hpp` on any wire format change; `PROTO_MAGIC` is fixed.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `write_u64` writes 8 bytes big-endian using byte-by-byte shifts; confirmed from reading Serializer.cpp. This avoids alignment and endianness UB that would occur with a raw pointer cast.
- `[CONFIRMED]` `PROTO_MAGIC` is written as the high 16 bits of a big-endian uint32 at offset 40: `write_u32(buf, 40, (uint32_t)PROTO_MAGIC << 16)` produces bytes 0x4D, 0x45, 0x00, 0x00 on the wire.
