# UC_25 — Serializer encode: Serializer::serialize() called internally by all backends

**HL Group:** System Internal — sub-function of HL-1 (best-effort send), HL-2 (reliable-ACK send), HL-3 (reliable-retry send), HL-17 (UDP send)
**Actor:** System (internal sub-function; never called directly by the User)
**Requirement traceability:** REQ-3.2.3

---

## 1. Use Case Overview

**Invoked by:** `Serializer::serialize()` is the single authoritative transformation from the in-memory `MessageEnvelope` representation to the big-endian binary wire format. It is invoked on every outbound message path before bytes are written to the transport. Confirmed callers:
- `UdpBackend::send_message()` (`src/platform/UdpBackend.cpp:122`) — encodes the main outbound message before `send_to()`.
- `UdpBackend::send_message()` delayed-envelope loop (`UdpBackend.cpp:157`) — re-encodes delayed messages drained from the impairment engine.
- `TcpBackend::send_message()` (`src/platform/TcpBackend.cpp`) — encodes message before `tcp_send_frame()`.
- `TcpBackend::flush_delayed_to_clients()` (`src/platform/TcpBackend.cpp`) — re-encodes delayed envelopes when their `release_us` has elapsed.

**Goal:** Convert an in-memory `MessageEnvelope` struct into a fixed-layout, big-endian binary wire buffer. Produce a 44-byte header followed by the payload bytes. No heap allocation. Fully deterministic: the same envelope always produces the same bit pattern regardless of host endianness.

**Preconditions:**
- `buf != nullptr`.
- `buf_len >= WIRE_HEADER_SIZE + env.payload_length` (44 + payload_length).
- `envelope_valid(env) == true`.

**Success outcome:** `buf[0..44+payload_length-1]` is written with the wire-format envelope; `out_len` is set to `44 + payload_length`; `Result::OK` returned.

**Error outcome:** `Result::ERR_INVALID` returned — envelope invalid or buffer too small; `buf` unmodified; `out_len` not written.

---

## 2. Entry Points

```cpp
// src/core/Serializer.cpp, line 117
// Declared: src/core/Serializer.hpp:56
static Result Serializer::serialize(
    const MessageEnvelope& env,   // [in]  envelope to encode
    uint8_t*               buf,   // [out] caller-provided destination buffer
    uint32_t               buf_len,  // [in]  available bytes in buf
    uint32_t&              out_len   // [out] bytes written on success
);
```

Private helper entry points (called only by `serialize()`):
- `Serializer::write_u8(buf, offset, val)` — `Serializer.cpp:24`
- `Serializer::write_u32(buf, offset, val)` — `Serializer.cpp:35`
- `Serializer::write_u64(buf, offset, val)` — `Serializer.cpp:50`

Supporting function:
- `envelope_valid()` — `src/core/MessageEnvelope.hpp:63`

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Precondition assertions** (`Serializer.cpp:123-124`):
   - `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)`.
   - `NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)` — tautology for `uint32_t`; satisfies Power of 10 assertion density.

2. **Envelope validation gate** (`Serializer.cpp:127-129`):
   - `if (!envelope_valid(env))`: checks `message_type != INVALID (255U)`, `payload_length <= 4096U`, `source_id != 0U`. If false: return `ERR_INVALID`; `buf` unmodified; `out_len` not written.

3. **Buffer size check** (`Serializer.cpp:132-135`):
   - `required_len = WIRE_HEADER_SIZE + env.payload_length` (44 + payload_length).
   - If `buf_len < required_len`: return `ERR_INVALID`.

4. **Offset cursor initialized** (`Serializer.cpp:138`): `uint32_t offset = 0U`.

5. **Header encoding, field by field** (`Serializer.cpp:141-152`). All writes use private helpers. Each helper returns the new offset, assigned back:
   - `write_u8(buf, 0, message_type)` → buf[0]; offset = 1. Values: DATA=0x00, ACK=0x01, NAK=0x02, HEARTBEAT=0x03.
   - `write_u8(buf, 1, reliability_class)` → buf[1]; offset = 2. Values: BEST_EFFORT=0x00, RELIABLE_ACK=0x01, RELIABLE_RETRY=0x02.
   - `write_u8(buf, 2, priority)` → buf[2]; offset = 3.
   - `write_u8(buf, 3, 0U)` → buf[3] = 0x00 (padding); offset = 4.
   - `write_u64(buf, 4, message_id)` → buf[4..11] big-endian; offset = 12.
   - `write_u64(buf, 12, timestamp_us)` → buf[12..19]; offset = 20.
   - `write_u32(buf, 20, source_id)` → buf[20..23]; offset = 24.
   - `write_u32(buf, 24, destination_id)` → buf[24..27]; offset = 28.
   - `write_u64(buf, 28, expiry_time_us)` → buf[28..35]; offset = 36.
   - `write_u32(buf, 36, payload_length)` → buf[36..39]; offset = 40.
   - `write_u32(buf, 40, 0U)` → buf[40..43] = 0x00000000 (padding word); offset = 44.

6. **Header size assertion** (`Serializer.cpp:155`): `NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE)` — i.e., `assert(44 == 44)`. Always active. Guards against `WIRE_HEADER_SIZE` changes without updating the write sequence.

7. **Payload copy** (`Serializer.cpp:159-161`):
   - If `env.payload_length > 0U`: `(void)memcpy(&buf[44], env.payload, env.payload_length)`. `(void)` cast is correct: `memcpy`'s return value (dst pointer) carries no error information.
   - If `payload_length == 0`: skip; zero-payload messages are valid.

8. **out_len assignment** (`Serializer.cpp:163`): `out_len = required_len = 44 + payload_length`. Only written on the success path.

9. **Post-condition assertion** (`Serializer.cpp:166`): `NEVER_COMPILED_OUT_ASSERT(out_len == WIRE_HEADER_SIZE + env.payload_length)`. Redundant by construction; required by Power of 10 Rule 5 (≥2 assertions per function).

10. **Return `Result::OK`.**

---

## 4. Call Tree

```
Serializer::serialize(env, buf, buf_len, out_len)
 ├── NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
 ├── NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)
 ├── envelope_valid(env)
 │    ├── env.message_type != INVALID (255U)
 │    ├── env.payload_length <= 4096U
 │    └── env.source_id != 0U
 │    [if false] → return ERR_INVALID
 ├── [buf_len < 44 + payload_length] → return ERR_INVALID
 ├── write_u8(buf, 0, message_type)      → buf[0]; return 1
 ├── write_u8(buf, 1, reliability_class) → buf[1]; return 2
 ├── write_u8(buf, 2, priority)          → buf[2]; return 3
 ├── write_u8(buf, 3, 0U)               → buf[3]=0; return 4
 ├── write_u64(buf, 4, message_id)       → buf[4..11] BE; return 12
 ├── write_u64(buf, 12, timestamp_us)    → buf[12..19]; return 20
 ├── write_u32(buf, 20, source_id)       → buf[20..23]; return 24
 ├── write_u32(buf, 24, destination_id)  → buf[24..27]; return 28
 ├── write_u64(buf, 28, expiry_time_us)  → buf[28..35]; return 36
 ├── write_u32(buf, 36, payload_length)  → buf[36..39]; return 40
 ├── write_u32(buf, 40, 0U)             → buf[40..43]=0; return 44
 ├── NEVER_COMPILED_OUT_ASSERT(offset == 44U)
 ├── [payload_length > 0U] (void)memcpy(&buf[44], env.payload, payload_length)
 ├── out_len = 44 + payload_length
 └── NEVER_COMPILED_OUT_ASSERT(out_len == 44 + payload_length)
     return Result::OK
```

---

## 5. Key Components Involved

- **`Serializer`** (`src/core/Serializer.cpp/.hpp`): Non-instantiable static utility class. All methods static. `WIRE_HEADER_SIZE = 44U` defined at `.hpp:47`. SC (HAZ-005).
- **`MessageEnvelope`** (`src/core/MessageEnvelope.hpp`): Input struct passed by const reference. Payload is `uint8_t payload[4096U]` inline — fixed size.
- **`envelope_valid()`** (`src/core/MessageEnvelope.hpp:63`): Inline free function. SC (HAZ-001, HAZ-005). Three-condition precondition gate.
- **`write_u8()`** (`Serializer.cpp:24`): Private static inline helper. Single store; returns new offset.
- **`write_u32()`** (`Serializer.cpp:35`): Private static inline helper. Four manual big-endian bit-shift stores; returns new offset.
- **`write_u64()`** (`Serializer.cpp:50`): Private static inline helper. Eight manual big-endian bit-shift stores; returns new offset.
- **`memcpy`** (`<cstring>`): Bounded opaque payload copy. `(void)` cast on return value.

---

## 6. Branching Logic / Decision Points

| Branch | Condition | True path | False path |
|--------|-----------|-----------|------------|
| Envelope validation | `!envelope_valid(env)` | Return `ERR_INVALID`; no writes | Proceed to buffer size check |
| Buffer size | `buf_len < 44 + payload_length` | Return `ERR_INVALID`; no writes | Proceed to header encoding |
| Payload copy guard | `payload_length > 0U` | Execute `memcpy` of `payload_length` bytes | Skip `memcpy` (zero-length payload is valid) |

No other branches on the success path. The 11 write calls are unconditional sequential statements. No loops, no switches, no recursion.

---

## 7. Concurrency / Threading Behavior

`Serializer::serialize()` is a purely functional static method with no shared state, no class-level mutable variables, no globals, and no static locals.

**Thread safety:** Fully re-entrant. Multiple threads can call `serialize()` simultaneously on distinct `(env, buf)` pairs without synchronization.

**Shared buffer risk:** In `UdpBackend`, both the main datagram and each delayed datagram reuse the same `m_wire_buf` member. Concurrent `send_message()` calls on the same `UdpBackend` instance would race on `m_wire_buf`. This is a caller-side concern; `serialize()` itself is safe.

Within `serialize()` itself: no atomic operations, no locks, no condition variables.

---

## 8. Memory & Ownership Semantics

| Object | Ownership | Notes |
|--------|-----------|-------|
| `env (const MessageEnvelope&)` | Caller | Borrowed read-only; pointer not retained after return |
| `buf (uint8_t*)` | Caller | Written but not owned by `Serializer`; at `UdpBackend` this is `m_wire_buf[8192]` |
| `out_len (uint32_t&)` | Caller | Written exactly once at line 163; not written on error paths |
| `required_len` (local uint32_t) | Stack | `44 + payload_length`; also used as final `out_len` |
| `offset` (local uint32_t) | Stack | Cursor; advances 0→44; discarded on return |

No heap allocation anywhere. `WIRE_HEADER_SIZE = 44U` is a `static const` class member; no runtime storage.

---

## 9. Error Handling Flow

```
serialize()
  buf == nullptr            → NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
  !envelope_valid(env)      → return ERR_INVALID; buf unmodified; out_len not written
  buf_len < required_len    → return ERR_INVALID; buf unmodified; out_len not written
  offset != 44 after writes → NEVER_COMPILED_OUT_ASSERT fires; program terminates
  out_len mismatch          → NEVER_COMPILED_OUT_ASSERT fires; program terminates
  success                   → return OK; buf[0..out_len-1] written; out_len set
```

Callers check the return value: `UdpBackend::send_message()` tests `result_ok(res)` at line 124 before calling `send_to()`.

---

## 10. External Interactions

`memcpy` (`<cstring>`, `Serializer.cpp:160`): the only external function called. Used only for the opaque payload copy. All header field writes are done with explicit array subscripts and manual bit shifts.

No file I/O, no socket calls, no OS calls, no dynamic memory, no global variables accessed, no logging within `serialize()` itself.

---

## 11. State Changes / Side Effects

### Byte-level wire format written on success

| Byte range | Content |
|------------|---------|
| `buf[0]` | `message_type` (1 byte) |
| `buf[1]` | `reliability_class` (1 byte) |
| `buf[2]` | `priority` (1 byte) |
| `buf[3]` | padding = 0x00 (1 byte; validated by deserialize) |
| `buf[4..11]` | `message_id` (big-endian uint64) |
| `buf[12..19]` | `timestamp_us` (big-endian uint64) |
| `buf[20..23]` | `source_id` (big-endian uint32) |
| `buf[24..27]` | `destination_id` (big-endian uint32) |
| `buf[28..35]` | `expiry_time_us` (big-endian uint64) |
| `buf[36..39]` | `payload_length` (big-endian uint32) |
| `buf[40..43]` | padding = 0x00000000 (4 bytes; validated by deserialize) |
| `buf[44..44+n-1]` | verbatim payload bytes (if `payload_length > 0`) |
| `out_len` | set to `44 + payload_length` |

No state changes within `Serializer` or any other object. Input envelope not modified.

---

## 12. Sequence Diagram

```
Caller          Serializer::serialize()       envelope_valid()    write_u8/u32/u64    memcpy
  |                      |                          |                    |               |
  |--serialize(env,buf,len,out_len)-->               |                    |               |
  |                      |--NEVER_COMPILED_OUT_ASSERT(buf != nullptr)    |               |
  |                      |--envelope_valid(env)----->|                   |               |
  |                      |<--true/false-------------|                    |               |
  |  [false: ERR_INVALID]|                          |                    |               |
  |  [buf_len < required: ERR_INVALID]              |                    |               |
  |                      |--write_u8(message_type)-->                    |               |
  |                      |<--offset=1----------------|                   |               |
  |                      | ... (10 more field writes) ...               |               |
  |                      |--write_u32(0U) [padding word]                 |               |
  |                      |<--offset=44                                   |               |
  |                      |--NEVER_COMPILED_OUT_ASSERT(offset == 44)      |               |
  |                      |--memcpy(buf+44, env.payload, n)-------------->|               |
  |                      | out_len = 44 + payload_length                 |               |
  |                      |--NEVER_COMPILED_OUT_ASSERT(out_len == 44+n)   |               |
  |<--Result::OK---------|                          |                    |               |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:** No initialization required. `Serializer` has no constructor, no mutable state, and no `init()` method. `WIRE_HEADER_SIZE = 44U` is initialized at compile time.

**Runtime:** `serialize()` is called once per outbound message. Execution is strictly linear: assertions → validation → 11 sequential field writes → conditional `memcpy` → return. No persistent state between calls.

---

## 14. Known Risks

- **Risk 1: Padding validation asymmetry.** `serialize()` writes padding bytes as hard-coded 0. `deserialize()` validates them and returns `ERR_INVALID` if non-zero. A sender that writes non-zero padding is rejected on the receive side — intentional defense-in-depth, but creates implicit coupling.
- **Risk 2: `buf_len` upper bound check is a tautology.** `NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)` is always true for `uint32_t`. If a caller passes a `buf_len` larger than the actual buffer, the `required_len` check may pass and `memcpy` may overwrite beyond the buffer.
- **Risk 3: No protocol version field.** The wire header has no version byte. Wire format changes cannot be detected by the receiver via a version mismatch mechanism.
- **Risk 4: Unchecked enum casts in write helpers.** `message_type` and `reliability_class` are cast to `uint8_t` before `write_u8()`. If the in-memory enum value is out of range (e.g., due to memory corruption), an arbitrary byte is written to the wire.
- **Risk 5: `NEVER_COMPILED_OUT_ASSERT` is always active.** A null `buf` terminates the program unconditionally in all build configurations. This is the intended safety posture.

---

## 15. Unknowns / Assumptions

- [CONFIRMED] `WIRE_HEADER_SIZE = 44U` is `static const uint32_t` in `Serializer.hpp:47`. Derivation: 1+1+1+1+8+8+4+4+8+4+4 = 44 bytes.
- [CONFIRMED] `NEVER_COMPILED_OUT_ASSERT` fires unconditionally. Defined in `src/core/Assert.hpp`.
- [CONFIRMED] `envelope_valid()` at `MessageEnvelope.hpp:63`: `message_type != INVALID (255U)`, `payload_length <= 4096U`, `source_id != 0U`.
- [CONFIRMED] `MSG_MAX_PAYLOAD_BYTES = 4096U`, `NODE_ID_INVALID = 0U` from `Types.hpp`.
- [CONFIRMED] `write_u8`/`u32`/`u64` use `NEVER_COMPILED_OUT_ASSERT` internally and perform manual big-endian bit-shift stores.
- [ASSUMPTION-B] The compiler inlines `write_u8`/`u32`/`u64` under -O1 or higher. The `inline` keyword is a hint; at -O0 these are actual function calls.
- [ASSUMPTION-C] The caller initializes `out_len` to 0 before calling `serialize()` so that on `ERR_INVALID` return (where `out_len` is not written) the caller does not act on garbage.
