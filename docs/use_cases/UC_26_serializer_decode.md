# UC_26 — Serializer decode: Serializer::deserialize() called internally by all backends

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System
**Requirement traceability:** REQ-3.2.3, REQ-3.2.8, REQ-6.1.5, REQ-6.1.6

---

## 1. Use Case Overview

- **Trigger:** Any backend calls `Serializer::deserialize(buf, wire_len, &envelope)` on the receive path after reading a raw frame. File: `src/core/Serializer.cpp`.
- **Goal:** Convert raw wire bytes back into a `MessageEnvelope` struct, validating all size constraints.
- **Success outcome:** `Result::OK` returned; `envelope` is fully populated from the wire bytes.
- **Error outcomes:**
  - `Result::ERR_INVALID` — `wire_len < WIRE_HEADER_SIZE` (frame too short to contain header).
  - `Result::ERR_INVALID` — byte 3 (`proto_version`) ≠ `PROTO_VERSION` (version mismatch; REQ-3.2.8).
  - `Result::ERR_INVALID` — bytes 40–43 (`magic_word`) ≠ `PROTO_MAGIC << 16` (bad magic or non-zero reserved; REQ-3.2.8).
  - `Result::ERR_INVALID` — decoded `payload_length > MSG_MAX_PAYLOAD_BYTES`.
  - `Result::ERR_INVALID` — `wire_len < WIRE_HEADER_SIZE + payload_length` (frame size inconsistent with declared payload length).

**Invoking UC:** UC_06 (TCP inbound deserialize), UC_23 (UDP receive), TlsTcpBackend, DtlsUdpBackend receive paths.

---

## 2. Entry Points

```cpp
// src/core/Serializer.cpp (static)
static Result Serializer::deserialize(
    const uint8_t* buf, uint32_t wire_len,
    MessageEnvelope& envelope);
```

Called internally by all backends on every received frame.

---

## 3. End-to-End Control Flow

1. `Serializer::deserialize(buf, wire_len, envelope)` — entry.
2. `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`.
3. **Validate:** `wire_len < WIRE_HEADER_SIZE` → return `ERR_INVALID`.
4. **Read header fields** using `read_u8` / `read_u32` / `read_u64` helpers (big-endian):
   - Byte 0: `envelope.message_type = (MessageType)read_u8(buf, 0)`.
   - Byte 1: `envelope.reliability_class = (ReliabilityClass)read_u8(buf, 1)`.
   - Byte 2: `envelope.priority = read_u8(buf, 2)`.
   - Byte 3: `proto_ver = read_u8(buf, 3)` — if `proto_ver != PROTO_VERSION` → return `ERR_INVALID`.
   - Bytes 4–11: `envelope.message_id = read_u64(buf, 4)` (8 bytes BE).
   - Bytes 12–19: `envelope.timestamp_us = read_u64(buf, 12)`.
   - Bytes 20–23: `envelope.source_id = read_u32(buf, 20)`.
   - Bytes 24–27: `envelope.destination_id = read_u32(buf, 24)`.
   - Bytes 28–35: `envelope.expiry_time_us = read_u64(buf, 28)`.
   - Bytes 36–39: `envelope.payload_length = read_u32(buf, 36)`.
   - Bytes 40–43: `magic_word = read_u32(buf, 40)` — if `magic_word != (PROTO_MAGIC << 16)` → return `ERR_INVALID`.
5. **Validate:** `envelope.payload_length > MSG_MAX_PAYLOAD_BYTES` → return `ERR_INVALID`.
6. **Validate:** `wire_len < WIRE_HEADER_SIZE + envelope.payload_length` → return `ERR_INVALID`. (Frames with trailing data beyond the declared payload are accepted; only undersize frames are rejected.)
7. **`memcpy(envelope.payload, buf + WIRE_HEADER_SIZE, envelope.payload_length)`**.
8. `NEVER_COMPILED_OUT_ASSERT(envelope.payload_length <= MSG_MAX_PAYLOAD_BYTES)`.
9. Returns `Result::OK`.

---

## 4. Call Tree

```
Serializer::deserialize(buf, wire_len, envelope)   [Serializer.cpp]
 ├── read_u8(buf, offset)    [×3]
 ├── read_u64(buf, offset)   [×3]
 ├── read_u32(buf, offset)   [×3]
 └── memcpy(envelope.payload, ...)
```

---

## 5. Key Components Involved

- **`Serializer`** — Stateless. Mirrors `serialize()` exactly in reverse. Any change to the wire format must update both.
- **`read_u8/u32/u64`** — Private static helpers; big-endian byte assembly via shifts.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `wire_len < WIRE_HEADER_SIZE` | Return ERR_INVALID | Read header |
| `proto_ver != PROTO_VERSION` | Return ERR_INVALID | Continue reading header |
| `magic_word != (PROTO_MAGIC << 16)` | Return ERR_INVALID | Continue |
| `payload_length > MSG_MAX_PAYLOAD_BYTES` | Return ERR_INVALID | Continue |
| `wire_len < WIRE_HEADER_SIZE + payload_length` | Return ERR_INVALID (checked via `payload_length_valid()` helper) | memcpy payload |

---

## 7. Concurrency / Threading Behavior

- Stateless — all inputs are passed by argument.
- Thread-safe as long as callers provide non-aliasing buffers.

---

## 8. Memory & Ownership Semantics

- `buf` — caller-provided (e.g., `m_wire_buf`); read-only during deserialize.
- `envelope` — caller-provided output; written to on success.
- No heap allocation.

---

## 9. Error Handling Flow

- **`ERR_INVALID`:** No partial state written to `envelope` (validation occurs before `memcpy`). Caller discards the frame and (for TCP) removes the client.
- Any intermediate `ERR_INVALID` returns before the `memcpy` call, so `envelope` is partially initialized (header fields may be written before the final size check fires). Caller should not use `envelope` on non-OK returns.

---

## 10. External Interactions

- None — operates entirely in process memory.

---

## 11. State Changes / Side Effects

- `envelope` — populated from wire bytes on success.
- No other state changes.

---

## 12. Sequence Diagram

```
TcpBackend::recv_from_client()
  -> Serializer::deserialize(m_wire_buf, wire_len, env)
       -> read_u8/u32/u64   [header fields]
       -> [validate sizes]
       -> memcpy(env.payload, ...)
  <- Result::OK; env populated
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `buf` contains a valid wire frame (produced by `serialize()` on the sender side).
- `wire_len` is the exact byte count of the received frame.

**Runtime:**
- Called on every inbound frame. Stateless; no initialization required.

---

## 14. Known Risks / Observations

- **Safety-critical function:** `Serializer::deserialize()` is in the MC/DC target list (HAZ-001, HAZ-005 in docs/HAZARD_ANALYSIS.md). A malformed wire frame that bypasses validation can corrupt `envelope` fields and cause incorrect dispatch.
- **`message_type` and `reliability_class` are cast from raw bytes:** No range validation beyond payload size. An invalid `message_type` value not in the `MessageType` enum will produce an unexpected `envelope_is_data()` / `envelope_is_control()` result downstream.
- **Partial write on ERR_INVALID:** Header fields may be written before the final size validation. Callers must not use `envelope` on non-OK return.
- **Version and magic checked before field reads:** The `proto_version` check (byte 3) fires after reading only 4 header bytes; the magic check fires after reading 44 header bytes. A version-mismatched frame from an incompatible sender is rejected before any envelope field assignment occurs.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` Byte 3 is `proto_version` (not a discarded padding byte). It is read and validated; ERR_INVALID is returned if it doesn't match `PROTO_VERSION`.
- `[CONFIRMED]` Bytes 40–43 are `magic_word` (not discarded padding). The full 4-byte word is read via `read_u32` and checked against `(PROTO_MAGIC << 16)`; this validates both the 2-byte magic (0x4D45) and the 2-byte reserved field (0x0000) in a single comparison.
- `[ASSUMPTION]` `message_type` and `reliability_class` enum casts from `uint8_t` are safe for values in the defined enum range. Out-of-range values are legal C++ but produce undefined `envelope.message_type` behavior in downstream dispatch.
