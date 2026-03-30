# UC_26 — Serializer decode

**HL Group:** System Internal — sub-function of HL-5 (User waits for an incoming message) and HL-17 (UDP receive)

**Actor:** System (internal sub-function)

**Requirement traceability:** REQ-3.2.3, REQ-4.1.3, REQ-6.1.5, REQ-6.1.6, REQ-6.2.4, REQ-6.3.2

---

## 1. Use Case Overview

### Invoked by
This use case documents the internal mechanism implemented by `Serializer::deserialize()`. It
is invoked on every inbound message path after raw bytes have been read from the network and
before the reconstructed `MessageEnvelope` is pushed into the receive queue. Confirmed callers:

- `TcpBackend::recv_from_client()` (TcpBackend.cpp:238) — called after `tcp_recv_frame()`
  has read a complete length-prefixed frame into `m_wire_buf`; `deserialize()` converts those
  bytes to an envelope which is then pushed to `m_recv_queue`.
- `UdpBackend::recv_one_datagram()` (UdpBackend.cpp:180) — called after `socket_recv_from()`
  has filled `m_wire_buf` with a datagram; `buf_len` is the exact datagram byte count.

It is factored out as a distinct mechanism because `Serializer::deserialize()` is the single
authoritative transformation from big-endian wire bytes to the in-memory `MessageEnvelope`
representation. Both TCP and UDP inbound paths call it identically; the framing mechanism
(TCP length-prefix stripping or UDP datagram boundary) is applied by the caller before
`deserialize()` is invoked. Separating this concern means wire-format validation (padding
checks, bounds guards, post-condition assertion) is defined and maintained in exactly one
place (Serializer.cpp), independent of transport mechanics.

### Name
UC_26 — Serializer Decodes a Wire Buffer into a MessageEnvelope

### Actor
Any transport-layer caller that has received raw bytes from a transport and needs to reconstruct
a `MessageEnvelope`. Confirmed callers: `TcpBackend::recv_from_client()` (TcpBackend.cpp:238),
`UdpBackend::recv_one_datagram()` (UdpBackend.cpp:180).

### Goal
Read a big-endian binary buffer received from the wire, validate its structure, and populate a
caller-provided `MessageEnvelope`. Steps: minimum-length guard → zero-initialize output →
sequential big-endian field reads → two padding-byte validations → `payload_length` bounds
check → total-size guard → opaque payload `memcpy` → post-condition `envelope_valid()`
assertion. No heap allocation.

### Preconditions
- `buf != nullptr`.
- `buf_len` is the exact count of valid bytes in `buf` (set by socket receive function).
- `env` is a caller-provided `MessageEnvelope`; its state on entry is unspecified
  (`deserialize()` may or may not call `envelope_init()` depending on how far it gets).

### Postconditions (success path)
- `env` is fully populated from wire bytes.
- `envelope_valid(env) == true` (enforced by `NEVER_COMPILED_OUT_ASSERT` at line 256).
- `Result::OK` returned.

### Error path
- `Result::ERR_INVALID` returned; `env` in a partially initialized state (see Section 8).

---

## 2. Entry Points

Primary entry point:
```
static Result Serializer::deserialize(
    const uint8_t*   buf,       // [in]  wire bytes received from socket/harness
    uint32_t         buf_len,   // [in]  number of valid bytes in buf
    MessageEnvelope& env        // [out] populated on success
)
```
Defined: Serializer.cpp, line 175.

Confirmed callers:

| Caller | File | Line | Context |
|--------|------|------|---------|
| `TcpBackend::recv_from_client()` | `src/platform/TcpBackend.cpp` | 238 | `buf = m_wire_buf` (filled by `tcp_recv_frame()`); `buf_len = out_len` (frame byte count from length-prefix) |
| `UdpBackend::recv_one_datagram()` | `src/platform/UdpBackend.cpp` | 180 | `buf = m_wire_buf` (filled by `recvfrom` via `socket_recv_from`); `buf_len = out_len` (datagram byte count) |

Private helper entry points (called only by `deserialize()`):

| Helper | File | Line |
|--------|------|------|
| `Serializer::read_u8` | `Serializer.cpp` | 69 |
| `Serializer::read_u32` | `Serializer.cpp` | 78 |
| `Serializer::read_u64` | `Serializer.cpp` | 94 |

Functions called within `deserialize()`:

| Function | File | Line | Role |
|----------|------|------|------|
| `envelope_init()` | `MessageEnvelope.hpp` | 47 | Zero-fill output envelope |
| `envelope_valid()` | `MessageEnvelope.hpp` | 63 | Post-condition check |
| `memcpy()` | `<cstring>` | Serializer.cpp:252 | Payload copy |

---

## 3. End-to-End Control Flow (Step-by-Step)

**Step 1 — Caller presents a received byte buffer** (caller context)
The calling transport layer has received `buf_len` bytes from the transport (e.g., via
`tcp_recv_frame()` in `TcpBackend::recv_from_client()` or `recvfrom` in
`UdpBackend::recv_one_datagram()`). It calls `deserialize()` to reconstruct the envelope. The
caller passes `env` by non-const reference; its initial state is unspecified.

**Step 2 — Precondition assertions** (Serializer.cpp:180–181)
```
NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)
```
These use `NEVER_COMPILED_OUT_ASSERT`, not standard `assert()`. They fire unconditionally
regardless of NDEBUG. A null `buf` terminates the program. The `buf_len` check is a tautology
for `uint32_t` but satisfies Power of 10 assertion density.

**Step 3 — Minimum length guard** (Serializer.cpp:184–186)
```
if (buf_len < WIRE_HEADER_SIZE)  i.e., if (buf_len < 44U):
    return Result::ERR_INVALID
```
NOTE: `envelope_init()` has NOT yet been called at this point. If this branch fires, `env` is
returned in its caller-supplied state (uninitialized garbage or whatever the caller put there).
See Risk 1.

**Step 4 — Zero-fill output envelope** (Serializer.cpp:189)
`envelope_init(env)` (MessageEnvelope.hpp:47):
```
memset(&env, 0, sizeof(MessageEnvelope))
env.message_type = MessageType::INVALID  (= 255U)
```
After this call, all fields of `env` are 0 except `message_type` which is `INVALID`. If any
subsequent early return fires, `env` is in this known state.

**Step 5 — Offset cursor initialized** (Serializer.cpp:192)
`uint32_t offset = 0U`

**Step 6 — Header decoding, field by field** (Serializer.cpp:195–233)
Note: unlike `serialize()` which uses return-value chaining, `deserialize()` reads using
`read_u8`/`u32`/`u64` with the current offset, then advances `offset` manually with
`+= 1U` / `+= 4U` / `+= 8U`. The net effect on `offset` is identical to the serialize pattern.

- 6a. `env.message_type = static_cast<MessageType>(read_u8(buf, 0))`. Offset += 1U; offset = 1.
  CAUTION: if the wire byte is not in the `MessageType` enum's defined range, the cast produces
  an out-of-range enum value (implementation-defined behavior per C++17). No range check exists.
- 6b. `env.reliability_class = static_cast<ReliabilityClass>(read_u8(buf, 1))`. Offset = 2.
- 6c. `env.priority = read_u8(buf, 2)`. Offset = 3.
- 6d. `padding1 = read_u8(buf, 3)` (local variable). Offset = 4.
  Padding validation (Serializer.cpp:207–209): `if (padding1 != 0U): return Result::ERR_INVALID`
  `env` fields set in 6a–6c are written; remaining fields are 0 from `envelope_init`. Caller
  must not use `env` on `ERR_INVALID` return.
- 6e. `env.message_id = read_u64(buf, 4)`. Reconstructs 64-bit value from 8 bytes; offset = 12.
- 6f. `env.timestamp_us = read_u64(buf, 12)`. Offset = 20.
- 6g. `env.source_id = read_u32(buf, 20)`. Offset = 24.
- 6h. `env.destination_id = read_u32(buf, 24)`. Offset = 28.
- 6i. `env.expiry_time_us = read_u64(buf, 28)`. Offset = 36.
- 6j. `env.payload_length = read_u32(buf, 36)`. Offset = 40.
- 6k. `padding2 = read_u32(buf, 40)` (local variable). Offset = 44.
  Padding validation (Serializer.cpp:232–234): `if (padding2 != 0U): return Result::ERR_INVALID`

**Step 7 — Header size assertion** (Serializer.cpp:237)
```
NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE)
```
i.e., `NEVER_COMPILED_OUT_ASSERT(44 == 44)`. Always active. Invariant: the read sequence must
sum to exactly 44 bytes.

**Step 8 — payload_length bounds check** (Serializer.cpp:240–242)
```
if (env.payload_length > MSG_MAX_PAYLOAD_BYTES)  i.e., > 4096U:
    return Result::ERR_INVALID
```
Prevents a maliciously or accidentally large `payload_length` from causing a buffer overread in
the subsequent `memcpy`.

**Step 9 — Total size check** (Serializer.cpp:245–248)
```
total_size = WIRE_HEADER_SIZE + env.payload_length = 44U + payload_length
if (buf_len < total_size): return Result::ERR_INVALID
```
This is the second length guard. Step 3 only ensured 44 bytes exist. This check ensures the
actual payload data is present in the buffer.

**Step 10 — Payload copy** (Serializer.cpp:251–253)
```
if (env.payload_length > 0U):
    (void)memcpy(env.payload, &buf[44], env.payload_length)
```
`offset` is 44 at this point (from Step 7). `&buf[44]` is the first payload byte. `env.payload`
is the inline `uint8_t[4096]` array within `MessageEnvelope`. Bounds guaranteed:
`env.payload_length <= 4096U` (Step 8) and `env.payload` is exactly 4096 bytes.

**Step 11 — Post-condition assertion** (Serializer.cpp:256)
```
NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
```
`envelope_valid()` (MessageEnvelope.hpp:63) checks:
- `env.message_type != MessageType::INVALID` (255U)
- `env.payload_length <= MSG_MAX_PAYLOAD_BYTES` (4096U)
- `env.source_id != NODE_ID_INVALID` (0U)

This fires unconditionally (NEVER_COMPILED_OUT_ASSERT) if a structurally valid wire message
carries `source_id == 0` or `message_type == INVALID`. Such a message passes all structural
checks but fails the semantic post-condition. This terminates the program unconditionally.
See Risk 3.

**Step 12 — Return `Result::OK`** (Serializer.cpp:258)

---

## 4. Call Tree (Hierarchical)

```
Serializer::deserialize(buf, buf_len, env)           [Serializer.cpp:175]
├── NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
├── NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)
├── [buf_len < 44U] return ERR_INVALID               [env NOT initialized]
├── envelope_init(env)                               [MessageEnvelope.hpp:47]
│   ├── memset(&env, 0, sizeof(MessageEnvelope))
│   └── env.message_type = MessageType::INVALID
├── read_u8(buf, 0)   -> env.message_type            [Serializer.cpp:69]
├── read_u8(buf, 1)   -> env.reliability_class       [Serializer.cpp:69]
├── read_u8(buf, 2)   -> env.priority                [Serializer.cpp:69]
├── read_u8(buf, 3)   -> padding1 (local)            [Serializer.cpp:69]
│   └── [padding1 != 0U] return ERR_INVALID          [env partially populated]
├── read_u64(buf, 4)  -> env.message_id              [Serializer.cpp:94]
├── read_u64(buf, 12) -> env.timestamp_us            [Serializer.cpp:94]
├── read_u32(buf, 20) -> env.source_id               [Serializer.cpp:78]
├── read_u32(buf, 24) -> env.destination_id          [Serializer.cpp:78]
├── read_u64(buf, 28) -> env.expiry_time_us          [Serializer.cpp:94]
├── read_u32(buf, 36) -> env.payload_length          [Serializer.cpp:78]
├── read_u32(buf, 40) -> padding2 (local)            [Serializer.cpp:78]
│   └── [padding2 != 0U] return ERR_INVALID          [header populated, payload not]
├── NEVER_COMPILED_OUT_ASSERT(offset == 44U)
├── [env.payload_length > 4096U] return ERR_INVALID
├── [buf_len < 44U + env.payload_length] return ERR_INVALID
├── [payload_length > 0U] (void)memcpy(env.payload, &buf[44], payload_length)
├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))   [MessageEnvelope.hpp:63]
│   ├── env.message_type != INVALID
│   ├── env.payload_length <= 4096U
│   └── env.source_id != 0U
└── return Result::OK
```

---

## 5. Key Components Involved

| Component | File(s) | Role |
|-----------|---------|------|
| `Serializer` | `src/core/Serializer.cpp / .hpp` | Non-instantiable static utility class. All methods static. Owns `WIRE_HEADER_SIZE = 44U` (Serializer.hpp:47). |
| `MessageEnvelope` | `src/core/MessageEnvelope.hpp` | Output struct passed by non-const reference. Fixed total size; payload is `uint8_t[4096U]` inline. |
| `envelope_init()` | `MessageEnvelope.hpp:47` | Inline free function. `memset` to 0 + set `INVALID` type. Called unconditionally after the first length guard. |
| `envelope_valid()` | `MessageEnvelope.hpp:63` | Inline free function. Post-condition gate. Called via `NEVER_COMPILED_OUT_ASSERT` — always active. |
| `read_u8()` | `Serializer.cpp:69` | Private static inline helper. Returns `buf[offset]`. |
| `read_u32()` | `Serializer.cpp:78` | Private static inline helper. Reconstructs 32-bit big-endian value from 4 bytes. |
| `read_u64()` | `Serializer.cpp:94` | Private static inline helper. Reconstructs 64-bit big-endian value from 8 bytes. |
| `memcpy` | `<cstring>` | Payload copy. Bounded by `payload_length` (validated before call). Return void-cast. |
| `Types.hpp` | `src/core/Types.hpp` | `MSG_MAX_PAYLOAD_BYTES=4096U`, `NODE_ID_INVALID=0U`, `WIRE_HEADER_SIZE` referenced via `Serializer.hpp`. |

---

## 6. Branching Logic / Decision Points

| Branch | Condition | True path | False path |
|--------|-----------|-----------|------------|
| Minimum length guard | `buf_len < 44U` | Return `ERR_INVALID`; `envelope_init()` NOT called; `env` untouched | Proceed to `envelope_init` and field reads |
| Padding byte 1 nonzero | `padding1 != 0U` (`buf[3]` must be 0x00) | Return `ERR_INVALID`; `env` partial (message_type, reliability_class, priority written; remaining 0 from init) | Continue reading fields |
| Padding word 2 nonzero | `padding2 != 0U` (`buf[40..43]` as big-endian uint32 must be 0) | Return `ERR_INVALID`; all header fields except payload written; `payload[]` all zeros | Continue |
| payload_length range check | `env.payload_length > 4096U` | Return `ERR_INVALID`; header fully populated; `payload[]` all zeros | Proceed to total size check |
| Total size check | `buf_len < 44U + env.payload_length` | Return `ERR_INVALID`; header fully populated; payload not copied | Proceed to `memcpy` |
| Payload copy guard | `env.payload_length > 0U` | Execute `memcpy` of `payload_length` bytes | Skip (`zero-payload` message is valid) |
| Post-condition assertion | `!envelope_valid(env)` | `NEVER_COMPILED_OUT_ASSERT` fires unconditionally; program terminates | `envelope_valid` returns true; return `Result::OK` |

---

## 7. Concurrency / Threading Behavior

`Serializer::deserialize()` is a purely functional static method with no shared state, no
class-level mutable variables, no globals, and no static locals.

Thread safety: fully re-entrant. Multiple threads can call `deserialize()` simultaneously on
distinct `(buf, env)` pairs without synchronization.

In `TcpBackend`, `recv_from_client()` (TcpBackend.cpp:238) passes `m_wire_buf` (a shared
member array) to `deserialize()`. Since `m_wire_buf` is written by `tcp_recv_frame()` and
immediately read by `deserialize()` within the same `recv_from_client()` call, this is safe
in single-threaded execution. Concurrent calls to `recv_from_client()` on the same `TcpBackend`
instance would race on `m_wire_buf`.

For `UdpBackend`, `recv_one_datagram()` similarly passes `m_wire_buf`; the same race risk
applies under concurrent receive calls.

Within `deserialize()` itself: no atomic operations, no locks, no condition variables.

---

## 8. Memory & Ownership Semantics

| Object | Ownership | Notes |
|--------|-----------|-------|
| `buf (const uint8_t*)` | Caller | Read-only pointer to a caller-managed buffer. `deserialize()` does not write to `buf` and does not retain the pointer after return. In `TcpBackend`, `buf = m_wire_buf` (inline member, 8192 bytes, filled by `tcp_recv_frame()`). |
| `env (MessageEnvelope&)` | Caller | Non-const reference; ownership stays with caller. State on entry: unspecified. State on each error return depends on which branch fired (see Section 9). |
| `padding1` (local uint8_t) | Stack | Exists only during the call. Validated then discarded. |
| `padding2` (local uint32_t) | Stack | Exists only during the call. Validated then discarded. |
| `env.payload` (uint8_t[4096U], inline) | Caller (via `env`) | `memcpy` writes up to `payload_length` bytes. Remaining bytes are 0 from `envelope_init`. `payload_length <= 4096` (Branch Point 4) ensures no overwrite. |

No heap allocation anywhere in this call tree. No `new`, `malloc`, `delete`, `free`.

---

## 9. Error Handling Flow

```
deserialize()
  buf == nullptr              → NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
  buf_len < 44                → ERR_INVALID; envelope_init NOT called; env in caller's prior state
  padding1 != 0               → ERR_INVALID; env partial (3 header fields written; rest 0 from init)
  padding2 != 0               → ERR_INVALID; all header fields written; payload[] all zeros
  payload_length > 4096       → ERR_INVALID; same state as padding2 failure
  buf_len < 44 + payload_len  → ERR_INVALID; header fully populated; payload not copied
  offset != 44 after reads    → NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
  !envelope_valid(env)        → NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
                                (fires for source_id == 0 or message_type == INVALID in wire data)
  success                     → return OK; env fully populated and envelope_valid
```

Key observation: the caller cannot distinguish between a truncated buffer (`buf_len < 44`) and
any other structural error — all return `ERR_INVALID`. But only the `buf_len < 44` case leaves
`env` entirely uninitialized. All other error paths leave `env` at minimum zero-initialized from
`envelope_init()`.

Confirmed callers check the return value before using `env`:
- `TcpBackend::recv_from_client()` (TcpBackend.cpp:239): tests result, logs `WARNING_LO`, returns `res`.
- `UdpBackend::recv_one_datagram()` (UdpBackend.cpp:181): tests `result_ok(res)` before pushing `env`.

---

## 10. External Interactions

`envelope_init()` (MessageEnvelope.hpp:47): calls `memset` internally (`<cstring>`). Not an OS
call.

`memcpy` (Serializer.cpp:252): reads from `buf` (incoming wire data) into `env.payload`. The
only place wire bytes enter the application-level envelope payload field.

No socket calls. No file I/O. No OS calls. No logging. No globals accessed.

---

## 11. State Changes / Side Effects

### Inputs consumed (read-only)
`buf[0..44+payload_length-1]` — all consumed; not modified.

### Outputs written to `env` (success path)

| Field | Source |
|-------|--------|
| `env.message_type` | `buf[0]` cast to `MessageType` |
| `env.reliability_class` | `buf[1]` cast to `ReliabilityClass` |
| `env.priority` | `buf[2]` |
| `env.message_id` | `buf[4..11]` big-endian uint64 |
| `env.timestamp_us` | `buf[12..19]` big-endian uint64 |
| `env.source_id` | `buf[20..23]` big-endian uint32 |
| `env.destination_id` | `buf[24..27]` big-endian uint32 |
| `env.expiry_time_us` | `buf[28..35]` big-endian uint64 |
| `env.payload_length` | `buf[36..39]` big-endian uint32 |
| `env.payload[0..n-1]` | `buf[44..44+n-1]` verbatim copy |

Discarded wire fields (read, validated, not stored):
- `buf[3]` — `padding1` read into local, validated, then discarded
- `buf[40..43]` — `padding2` read into local, validated, then discarded

No global state modified. No `Serializer` class state modified.

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant Deserialize as Serializer::deserialize()
    participant EnvInit as envelope_init()
    participant ReadHelpers as read_u8/u32/u64
    participant CRT as memcpy
    participant EnvValid as envelope_valid()

    Caller->>Deserialize: deserialize(buf, buf_len, env)
    Deserialize->>Deserialize: NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
    alt buf_len < 44
        Deserialize-->>Caller: ERR_INVALID [env NOT initialized]
    else buf_len >= 44
        Deserialize->>EnvInit: envelope_init(env)
        EnvInit->>EnvInit: memset(env, 0); env.type=INVALID
        EnvInit-->>Deserialize: done
        Deserialize->>ReadHelpers: read_u8(buf, 0) -> message_type
        Deserialize->>ReadHelpers: read_u8(buf, 1) -> reliability_class
        Deserialize->>ReadHelpers: read_u8(buf, 2) -> priority
        Deserialize->>ReadHelpers: read_u8(buf, 3) -> padding1
        alt padding1 != 0
            Deserialize-->>Caller: ERR_INVALID
        else padding1 == 0
            Deserialize->>ReadHelpers: read_u64(buf, 4) -> message_id
            Deserialize->>ReadHelpers: read_u64(buf, 12) -> timestamp_us
            Deserialize->>ReadHelpers: read_u32(buf, 20) -> source_id
            Deserialize->>ReadHelpers: read_u32(buf, 24) -> destination_id
            Deserialize->>ReadHelpers: read_u64(buf, 28) -> expiry_time_us
            Deserialize->>ReadHelpers: read_u32(buf, 36) -> payload_length
            Deserialize->>ReadHelpers: read_u32(buf, 40) -> padding2
            alt padding2 != 0 or payload_length > 4096 or buf_len < 44+payload_length
                Deserialize-->>Caller: ERR_INVALID
            else valid
                Deserialize->>Deserialize: NEVER_COMPILED_OUT_ASSERT(offset == 44)
                alt payload_length > 0
                    Deserialize->>CRT: memcpy(env.payload, buf+44, payload_length)
                end
                Deserialize->>EnvValid: envelope_valid(env) [via assert]
                EnvValid-->>Deserialize: true [or assert fires if false]
                Deserialize-->>Caller: Result::OK
            end
        end
    end
```

---

## 13. Initialization vs Runtime Flow

### Initialization phase
None for `Serializer` itself. The input buffer (`buf`) is initialized by the transport layer
(`tcp_recv_frame()` fills `m_wire_buf` with framed bytes in `TcpBackend`, or `socket_recv_from()`
fills `m_wire_buf` with datagram bytes in `UdpBackend`) before `deserialize()` is called. The
output envelope (`env`) may be uninitialized on entry; `deserialize()` zero-fills it after the
first bounds check (Step 3).

### Runtime phase
Called once per inbound framed message (once per TCP frame in `TcpBackend`, once per datagram
in `UdpBackend`). Execution is strictly linear: assertions → length guard → zero-fill → reads →
validations → payload copy → post-condition → return. No loops, no recursion, no dynamic
allocation. Typical hot path: ~11 read helper invocations + conditional `memcpy` + `envelope_valid`.

---

## 14. Known Risks / Observations

### Risk 1 — `env` uninitialized on the earliest error path
If `buf_len < 44U` (Branch Point 1), `envelope_init()` is NOT called because it comes after
the check. The caller receives `ERR_INVALID` with `env` in whatever state it had before the
call. If the caller reads any `env` field despite the error return, it reads garbage.
Calling `envelope_init()` before `deserialize()` would be safer, but is not enforced.

### Risk 2 — Partially-populated `env` on intermediate error paths
After Branch Points 2–5, `envelope_init()` has been called so `env` is at least
zero-initialized. However, fields written before the error point contain wire values. For
example, after a `padding2` failure, `env.payload_length` contains the wire-supplied value
(potentially attacker-controlled). Callers must not read any `env` field on non-OK return.

### Risk 3 — `NEVER_COMPILED_OUT_ASSERT` on post-condition terminates program
The post-condition `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` at line 256 fires
unconditionally for any message with `source_id == 0` or `message_type == INVALID` in the
wire data that passes all structural checks. This terminates the program rather than returning
`ERR_INVALID`. In the TCP path, a remotely-sent frame with `source_id == 0` would crash the
receiving process. This is a safety vs. availability trade-off in the design.

### Risk 4 — Unchecked cast from `uint8_t` to enum
`read_u8` returns `uint8_t`. It is cast directly to `MessageType` or `ReliabilityClass`
without a range check. A wire byte value not defined in the enum (e.g., `MessageType = 0x77`)
results in an out-of-range enum value — implementation-defined behavior per C++17. MISRA
C++:2023 strongly discourages unvalidated enum casts; no range guard exists between the
`read_u8` call and the cast.

### Risk 5 — Offset advancement inconsistency between `serialize` and `deserialize`
`serialize()` uses return-value chaining (`offset = write_u8(...)`). `deserialize()` uses
manual advancement (`read_u8(buf, offset); offset += 1U`). Both produce the same final
offsets. The manual pattern is more error-prone to future edits: forgetting a `+= 4U` after
`read_u32()` would silently misalign all subsequent reads.

### Risk 6 — No checksum or integrity verification
The wire format has no CRC, checksum, or HMAC. A corrupted buffer that maintains valid padding
bytes and a valid `payload_length` passes all structural checks. Only the semantic
post-condition (`envelope_valid`) provides any validity signal beyond structure — and it
terminates the program rather than returning `ERR_INVALID`.

### Risk 7 — `env.payload` remaining bytes after partial payload
If `env.payload_length < MSG_MAX_PAYLOAD_BYTES`, the bytes `payload[payload_length..4095]`
remain 0 from `envelope_init()`. This is safe for the current code but creates zeroed-out
trailing bytes that could be mistaken for meaningful data if `payload_length` is not checked
by the application layer.

---

## 15. Unknowns / Assumptions

**[CONFIRMED-1]** `WIRE_HEADER_SIZE = 44U` is defined in Serializer.hpp:47 as a
`static const uint32_t`. Derivation: 1+1+1+1+8+8+4+4+8+4+4 = 44 bytes.

**[CONFIRMED-2]** `NEVER_COMPILED_OUT_ASSERT` fires unconditionally regardless of NDEBUG.
Defined in `core/Assert.hpp`. The post-condition at Serializer.cpp:256 is always active; it
terminates the program for semantically invalid messages.

**[CONFIRMED-3]** `envelope_init()` is at MessageEnvelope.hpp:47: `memset(&env, 0, sizeof(MessageEnvelope))`
followed by `env.message_type = MessageType::INVALID`.

**[CONFIRMED-4]** `envelope_valid()` is at MessageEnvelope.hpp:63: checks `message_type != INVALID (255U)`,
`payload_length <= 4096U`, `source_id != 0U`.

**[CONFIRMED-5]** `MSG_MAX_PAYLOAD_BYTES = 4096U`, `NODE_ID_INVALID = 0U` confirmed in
Types.hpp. The payload field is `uint8_t payload[MSG_MAX_PAYLOAD_BYTES]` inline.

**[CONFIRMED-6]** `read_u8`/`u32`/`u64` use `NEVER_COMPILED_OUT_ASSERT` internally
(Serializer.cpp:71–72, 80–81, 96–97). They take an absolute offset (not advancing); the caller
advances `offset` with `+=` after each call.

**[CONFIRMED-7]** `UdpBackend::recv_one_datagram()` at UdpBackend.cpp:180 is a confirmed caller.
It passes `m_wire_buf` as `buf` and `out_len` (set by `socket_recv_from`) as `buf_len`.

**[CONFIRMED-8]** `TcpBackend::recv_from_client()` at TcpBackend.cpp:238 is a confirmed caller.
It passes `m_wire_buf` as `buf` and `out_len` (the frame byte count from `tcp_recv_frame()`)
as `buf_len`. The ASSUMPTION-A label from the prior version of this document is hereby
retired — TcpBackend is a confirmed caller.

**[ASSUMPTION-B]** Enum values in the wire byte stream always correspond to defined enumerators.
If a non-conforming sender transmits an undefined enum byte, the behavior is
implementation-defined (most compilers store the integer value in the enum variable without
checking the range, but this is not guaranteed by C++17).

**[ASSUMPTION-C]** The caller initializes `env` before calling `deserialize()` or does not read
`env` on `ERR_INVALID` return, specifically for the `buf_len < 44` case where `envelope_init()`
is not called. `TcpBackend::recv_from_client()` declares `env` as a local variable and checks
the result before using it — this is safe for all error paths including Risk 1.
`UdpBackend::recv_one_datagram()` (line 179) similarly declares `env` as a local and tests
`result_ok(res)` before pushing `env` — also safe.
