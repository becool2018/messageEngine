# UC_26 — Serializer decode: Serializer::deserialize() called internally by all backends
**HL Group:** System Internal — sub-function of HL-5 (Receive a Message) and HL-17 (UDP Plaintext Transport)
**Actor:** System (internal sub-function; never called directly by the User)
**Requirement traceability:** REQ-3.2.3

---

## 1. Use Case Overview

### Invoked by
This use case documents the internal mechanism implemented by `Serializer::deserialize()`
(Serializer.cpp:175). It is invoked on every inbound message path after raw bytes have been
read from the transport and before the reconstructed `MessageEnvelope` is pushed into the
receive queue. Confirmed callers:

- `TcpBackend::recv_from_client()` (TcpBackend.cpp:238) — called after `tcp_recv_frame()` has
  read a complete length-prefixed frame into `m_wire_buf`; `deserialize()` converts those bytes
  to an envelope which is then pushed to `m_recv_queue`.
- `UdpBackend::recv_one_datagram()` (UdpBackend.cpp:180) — called after `socket_recv_from()`
  has filled `m_wire_buf` with a datagram; `buf_len` is the exact datagram byte count.

`Serializer::deserialize()` is the single authoritative transformation from big-endian wire
bytes to the in-memory `MessageEnvelope` representation. Both TCP and UDP inbound paths call it
identically; the framing mechanism (TCP length-prefix stripping or UDP datagram boundary) is
applied by the caller before `deserialize()` is invoked.

### Name
UC_26 — Serializer Decodes a Wire Buffer into a MessageEnvelope

### Goal
Read a big-endian binary buffer received from the wire, validate its structure, and populate a
caller-provided `MessageEnvelope`. Execution path: precondition assertions → minimum-length
guard → zero-initialize output → sequential big-endian field reads → two padding validations →
`payload_length` bounds check → total-size guard → conditional `memcpy` → post-condition
assertion → `Result::OK`.

### Preconditions
- `buf != nullptr` (enforced by `NEVER_COMPILED_OUT_ASSERT` before any check).
- `buf_len` is the exact count of valid bytes in `buf` (set by the socket receive function in
  the caller).
- `env` is a caller-provided `MessageEnvelope`; its state on entry is unspecified.
  `deserialize()` calls `envelope_init()` after the first length guard.

### Postconditions (success path)
- `env` is fully populated from wire bytes.
- `envelope_valid(env) == true` (enforced by `NEVER_COMPILED_OUT_ASSERT` at Serializer.cpp:256).
- Returns `Result::OK`.

### Error path
- Returns `Result::ERR_INVALID`; `env` in a partially initialized state (see Section 9).

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

| Caller | File | Line | Context |
|--------|------|------|---------|
| `TcpBackend::recv_from_client()` | TcpBackend.cpp | 238 | `buf = m_wire_buf` (filled by `tcp_recv_frame()`); `buf_len = out_len` (frame byte count) |
| `UdpBackend::recv_one_datagram()` | UdpBackend.cpp | 180 | `buf = m_wire_buf` (filled by `recvfrom` via `socket_recv_from`); `buf_len = out_len` (datagram byte count) |

Private read helpers (called only by `deserialize()`):

| Helper | File | Line |
|--------|------|------|
| `Serializer::read_u8` | Serializer.cpp | 69 |
| `Serializer::read_u32` | Serializer.cpp | 78 |
| `Serializer::read_u64` | Serializer.cpp | 94 |

---

## 3. End-to-End Control Flow (Step-by-Step)

**Step 1 — Caller presents a received byte buffer** (caller context)
The calling transport layer has received `buf_len` bytes (e.g., via `tcp_recv_frame()` in
`TcpBackend` or `socket_recv_from` in `UdpBackend`). It calls `deserialize()` to reconstruct
the envelope. The caller passes `env` by non-const reference; its initial state is unspecified.

**Step 2 — Precondition assertions** (Serializer.cpp:180–181)
```
NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)
```
Both fire unconditionally regardless of NDEBUG. A null `buf` terminates the program. The
`buf_len` check is a tautology for `uint32_t` but satisfies Power of 10 Rule 5 assertion
density.

**Step 3 — Minimum length guard** (Serializer.cpp:184–186)
```
if (buf_len < WIRE_HEADER_SIZE)   // i.e., if (buf_len < 44U)
    return Result::ERR_INVALID
```
NOTE: `envelope_init()` has NOT been called at this point. If this branch fires, `env` is
returned in its caller-supplied state (uninitialized). See Risk 1.

**Step 4 — Zero-fill output envelope** (Serializer.cpp:189)
`envelope_init(env)` (MessageEnvelope.hpp:47) executes:
```
memset(&env, 0, sizeof(MessageEnvelope))
env.message_type = MessageType::INVALID   // = 255U
```
After this point, all fields are 0 except `message_type == INVALID`. Any subsequent early
return leaves `env` in this known zero-initialized state (with partial header fields written
in later steps).

**Step 5 — Offset cursor initialized** (Serializer.cpp:192)
`uint32_t offset = 0U`

**Step 6 — Header decoding, field by field** (Serializer.cpp:195–233)
Unlike `serialize()` which chains return values (`offset = write_u8(...)`), `deserialize()`
reads via `read_u8/u32/u64` using the current absolute offset, then manually increments `offset`.

- 6a. `env.message_type = static_cast<MessageType>(read_u8(buf, 0))`. `offset += 1U`; offset = 1.
  NOTE: if the wire byte is not a defined `MessageType` value, the cast is implementation-defined.
- 6b. `env.reliability_class = static_cast<ReliabilityClass>(read_u8(buf, 1))`. `offset += 1U`; offset = 2.
- 6c. `env.priority = read_u8(buf, 2)`. `offset += 1U`; offset = 3.
- 6d. `padding1 = read_u8(buf, 3)` (local `uint8_t`). `offset += 1U`; offset = 4.
  Validation: `if (padding1 != 0U): return Result::ERR_INVALID`.
  `env` has fields from 6a–6c written; remaining fields are 0 from `envelope_init`.
- 6e. `env.message_id = read_u64(buf, 4)`. `offset += 8U`; offset = 12.
- 6f. `env.timestamp_us = read_u64(buf, 12)`. `offset += 8U`; offset = 20.
- 6g. `env.source_id = read_u32(buf, 20)`. `offset += 4U`; offset = 24.
- 6h. `env.destination_id = read_u32(buf, 24)`. `offset += 4U`; offset = 28.
- 6i. `env.expiry_time_us = read_u64(buf, 28)`. `offset += 8U`; offset = 36.
- 6j. `env.payload_length = read_u32(buf, 36)`. `offset += 4U`; offset = 40.
- 6k. `padding2 = read_u32(buf, 40)` (local `uint32_t`). `offset += 4U`; offset = 44.
  Validation: `if (padding2 != 0U): return Result::ERR_INVALID`.

**Step 7 — Header size assertion** (Serializer.cpp:237)
```
NEVER_COMPILED_OUT_ASSERT(offset == WIRE_HEADER_SIZE)   // NEVER_COMPILED_OUT_ASSERT(44U == 44U)
```
Always active. Invariant: the field read sequence must advance `offset` to exactly 44.

**Step 8 — payload_length bounds check** (Serializer.cpp:240–242)
```
if (env.payload_length > MSG_MAX_PAYLOAD_BYTES)   // i.e., > 4096U
    return Result::ERR_INVALID
```
Prevents a large wire-supplied `payload_length` from causing a buffer overread in the subsequent
`memcpy`.

**Step 9 — Total size check** (Serializer.cpp:245–248)
```
total_size = WIRE_HEADER_SIZE + env.payload_length   // = 44U + payload_length
if (buf_len < total_size): return Result::ERR_INVALID
```
Step 3 only ensured 44 bytes exist. This check ensures the actual payload data is present.

**Step 10 — Payload copy** (Serializer.cpp:251–253)
```
if (env.payload_length > 0U):
    (void)memcpy(env.payload, &buf[44], env.payload_length)
```
`&buf[44]` is the first payload byte (offset is 44 from Step 7). `env.payload` is the inline
`uint8_t[4096]` array within `MessageEnvelope`. Bounds guaranteed: `env.payload_length <= 4096U`
(Step 8) and `env.payload` is exactly 4096 bytes.

**Step 11 — Post-condition assertion** (Serializer.cpp:256)
```
NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
```
`envelope_valid()` (MessageEnvelope.hpp:63) checks:
- `env.message_type != MessageType::INVALID` (255U)
- `env.payload_length <= MSG_MAX_PAYLOAD_BYTES` (4096U)
- `env.source_id != NODE_ID_INVALID` (0U)

Fires unconditionally if a structurally valid wire message carries `source_id == 0` or
`message_type == INVALID`. Such a message passes all structural checks but terminates the
program. See Risk 3.

**Step 12 — Return `Result::OK`** (Serializer.cpp:258)

---

## 4. Call Tree (Hierarchical)

```
Serializer::deserialize(buf, buf_len, env)            [Serializer.cpp:175]
 ├── NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
 ├── NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFFUL)
 ├── [buf_len < 44U] return ERR_INVALID               [env NOT initialized]
 ├── envelope_init(env)                               [MessageEnvelope.hpp:47]
 │    ├── memset(&env, 0, sizeof(MessageEnvelope))
 │    └── env.message_type = MessageType::INVALID
 ├── read_u8(buf, 0)  -> env.message_type             [Serializer.cpp:69]
 ├── read_u8(buf, 1)  -> env.reliability_class        [Serializer.cpp:69]
 ├── read_u8(buf, 2)  -> env.priority                 [Serializer.cpp:69]
 ├── read_u8(buf, 3)  -> padding1 (local)             [Serializer.cpp:69]
 │    └── [padding1 != 0U] return ERR_INVALID         [env partial]
 ├── read_u64(buf, 4)  -> env.message_id              [Serializer.cpp:94]
 ├── read_u64(buf, 12) -> env.timestamp_us            [Serializer.cpp:94]
 ├── read_u32(buf, 20) -> env.source_id               [Serializer.cpp:78]
 ├── read_u32(buf, 24) -> env.destination_id          [Serializer.cpp:78]
 ├── read_u64(buf, 28) -> env.expiry_time_us          [Serializer.cpp:94]
 ├── read_u32(buf, 36) -> env.payload_length          [Serializer.cpp:78]
 ├── read_u32(buf, 40) -> padding2 (local)            [Serializer.cpp:78]
 │    └── [padding2 != 0U] return ERR_INVALID         [header full; payload all zeros]
 ├── NEVER_COMPILED_OUT_ASSERT(offset == 44U)
 ├── [env.payload_length > 4096U] return ERR_INVALID
 ├── [buf_len < 44U + payload_length] return ERR_INVALID
 ├── [payload_length > 0U] memcpy(env.payload, buf+44, payload_length)
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))   [MessageEnvelope.hpp:63]
 │    ├── message_type != INVALID
 │    ├── payload_length <= 4096U
 │    └── source_id != 0U
 └── return Result::OK
```

---

## 5. Key Components Involved

| Component | File(s) | Role |
|-----------|---------|------|
| `Serializer` | src/core/Serializer.cpp / .hpp | Non-instantiable static utility class. All methods are static. Owns `WIRE_HEADER_SIZE = 44U` (Serializer.hpp:47). |
| `MessageEnvelope` | src/core/MessageEnvelope.hpp | Output struct passed by non-const reference. Fixed total size; `payload` is `uint8_t[4096U]` inline. |
| `envelope_init()` | MessageEnvelope.hpp:47 | Inline free function. `memset` to 0 then sets `message_type = INVALID`. Called unconditionally after the first length guard. |
| `envelope_valid()` | MessageEnvelope.hpp:63 | Inline free function. Post-condition gate used in `NEVER_COMPILED_OUT_ASSERT`. Always active. |
| `read_u8()` | Serializer.cpp:69 | Private static inline helper. Returns `buf[offset]`. Owns two precondition assertions. |
| `read_u32()` | Serializer.cpp:78 | Private static inline helper. Reconstructs 32-bit big-endian value from 4 bytes via manual shifts. |
| `read_u64()` | Serializer.cpp:94 | Private static inline helper. Reconstructs 64-bit big-endian value from 8 bytes via manual shifts. |
| `memcpy` | `<cstring>` | Payload copy. Bounded by `payload_length` (validated before call). Return is void-cast. |
| `Types.hpp` | src/core/Types.hpp | `MSG_MAX_PAYLOAD_BYTES=4096U`, `NODE_ID_INVALID=0U`, `WIRE_HEADER_SIZE` referenced via Serializer.hpp. |

---

## 6. Branching Logic / Decision Points

| Branch | Condition | True path | False path |
|--------|-----------|-----------|------------|
| Minimum length guard | `buf_len < 44U` | Return `ERR_INVALID`; `envelope_init()` NOT called; `env` in caller's prior state | Proceed to `envelope_init` and field reads |
| Padding byte 1 nonzero | `padding1 != 0U` (`buf[3]` must be 0x00) | Return `ERR_INVALID`; `env` partial: message_type/reliability_class/priority written; remaining fields 0 from init | Continue reading remaining header fields |
| Padding word 2 nonzero | `padding2 != 0U` (`buf[40..43]` as big-endian uint32 must be 0) | Return `ERR_INVALID`; all header fields populated; `payload[]` all zeros | Continue to payload_length check |
| payload_length range check | `env.payload_length > 4096U` | Return `ERR_INVALID`; header fully populated; `payload[]` all zeros | Proceed to total size check |
| Total size check | `buf_len < 44U + env.payload_length` | Return `ERR_INVALID`; header fully populated; payload not copied | Proceed to `memcpy` |
| Payload copy guard | `env.payload_length > 0U` | Execute `memcpy` of `payload_length` bytes | Skip (`zero-payload` message is valid) |
| Post-condition assertion | `!envelope_valid(env)` | `NEVER_COMPILED_OUT_ASSERT` fires unconditionally; program terminates in all builds | `envelope_valid` returns true; return `Result::OK` |

---

## 7. Concurrency / Threading Behavior

`Serializer::deserialize()` is a purely functional static method with no shared state, no
class-level mutable variables, no globals, and no static locals.

Thread safety: fully re-entrant. Multiple threads may call `deserialize()` simultaneously on
distinct `(buf, env)` pairs without synchronization.

In `TcpBackend`, `recv_from_client()` passes `m_wire_buf` (a shared member array) to
`deserialize()`. Since `m_wire_buf` is written by `tcp_recv_frame()` and immediately read by
`deserialize()` within the same `recv_from_client()` call, this is safe under single-threaded
execution. Concurrent calls to `recv_from_client()` on the same `TcpBackend` instance would race
on `m_wire_buf`; `deserialize()` itself is not the problem.

For `UdpBackend`, `recv_one_datagram()` similarly passes `m_wire_buf`; the same race risk applies
to concurrent receive calls on the same backend instance.

Within `deserialize()` itself: no atomic operations, no locks, no condition variables.

---

## 8. Memory & Ownership Semantics

| Object | Ownership | Notes |
|--------|-----------|-------|
| `buf (const uint8_t*)` | Caller | Read-only pointer to caller-managed buffer. `deserialize()` does not write to `buf` and does not retain the pointer after return. In `TcpBackend`, this is `m_wire_buf` (inline 8192-byte member). |
| `env (MessageEnvelope&)` | Caller | Non-const reference; ownership stays with caller. State on entry unspecified; state on each error return depends on branch fired (see Section 9). |
| `padding1` (local `uint8_t`) | Stack | Read from `buf[3]`, validated, then discarded. |
| `padding2` (local `uint32_t`) | Stack | Read from `buf[40..43]`, validated, then discarded. |
| `env.payload` (`uint8_t[4096U]`, inline) | Caller via `env` | `memcpy` writes up to `payload_length` bytes. Remaining bytes `payload[payload_length..4095]` are 0 from `envelope_init`. Bounds guaranteed by Step 8. |
| `total_size` (local `uint32_t`) | Stack | Computed as `44U + env.payload_length`. Used for one check; then discarded. |

No heap allocation anywhere in this call tree. No `new`, `malloc`, `delete`, or `free`.
`sizeof(MessageEnvelope)` ≈ `4096 + 40 + compiler padding` ≈ 4140 bytes on the caller's stack.

---

## 9. Error Handling Flow

```
deserialize()
  buf == nullptr               -> NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
  buf_len < 44                 -> ERR_INVALID; envelope_init NOT called; env in caller's prior state
  padding1 != 0 (buf[3] != 0) -> ERR_INVALID; env partial (3 header fields written; rest 0 from init)
  padding2 != 0 (buf[40-43])  -> ERR_INVALID; all header fields written; payload[] all zeros
  payload_length > 4096        -> ERR_INVALID; same env state as padding2 failure path
  buf_len < 44 + payload_len   -> ERR_INVALID; header fully populated; payload not copied
  offset != 44 after reads     -> NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
                                  (developer error: field read sequence advances offset wrong amount)
  !envelope_valid(env) after
    successful payload copy    -> NEVER_COMPILED_OUT_ASSERT fires; program terminates (all builds)
                                  (fires for source_id == 0 or message_type == INVALID in wire data)
  success                      -> return OK; env fully populated and envelope_valid
```

Confirmed callers check the return value before using `env`:
- `TcpBackend::recv_from_client()` (TcpBackend.cpp:239): tests result, logs `WARNING_LO`, returns.
- `UdpBackend::recv_one_datagram()` (UdpBackend.cpp:181): tests `result_ok(res)` before pushing `env`.

---

## 10. External Interactions

`envelope_init()` (MessageEnvelope.hpp:47): calls `memset` internally (`<cstring>`). Not an OS
call.

`memcpy` (Serializer.cpp:252): reads from `buf` (incoming wire data) into `env.payload`. This is
the only place where wire bytes enter the application-level envelope payload field.

No socket calls. No file I/O. No OS calls. No logging calls. No globals accessed or modified.

---

## 11. State Changes / Side Effects

### Inputs consumed (read-only)
`buf[0 .. 44+payload_length-1]` — all consumed; never modified.

### Outputs written to `env` (success path)

| Wire bytes | Offset | Field written |
|------------|--------|---------------|
| `buf[0]` | 0 | `env.message_type` (cast to `MessageType`) |
| `buf[1]` | 1 | `env.reliability_class` (cast to `ReliabilityClass`) |
| `buf[2]` | 2 | `env.priority` |
| `buf[3]` | 3 | padding1 (local only; validated and discarded) |
| `buf[4..11]` | 4 | `env.message_id` (big-endian uint64) |
| `buf[12..19]` | 12 | `env.timestamp_us` (big-endian uint64) |
| `buf[20..23]` | 20 | `env.source_id` (big-endian uint32) |
| `buf[24..27]` | 24 | `env.destination_id` (big-endian uint32) |
| `buf[28..35]` | 28 | `env.expiry_time_us` (big-endian uint64) |
| `buf[36..39]` | 36 | `env.payload_length` (big-endian uint32) |
| `buf[40..43]` | 40 | padding2 (local only; validated and discarded) |
| `buf[44..44+n-1]` | 44 | `env.payload[0..n-1]` via `memcpy` |

No global state modified. No `Serializer` class state modified. No `static` variables written.

---

## 12. Sequence Diagram

```
Caller (TcpBackend or UdpBackend)
  |
  |-- deserialize(buf, buf_len, env) -----------------------> Serializer::deserialize()
  |                                                              |
  |                                                              |-- NEVER_COMPILED_OUT_ASSERT(buf != nullptr)
  |                                                              |-- NEVER_COMPILED_OUT_ASSERT(buf_len <= 0xFFFFFFFF)
  |                                                              |
  |                                        [buf_len < 44] ----  |-- if (buf_len < WIRE_HEADER_SIZE)
  |<-- ERR_INVALID [env NOT initialized] <--------------------  |     return ERR_INVALID
  |                                                              |
  |                                                              |-- envelope_init(env)
  |                                                              |     memset(&env, 0, sizeof)
  |                                                              |     env.message_type = INVALID
  |                                                              |
  |                                                              |-- read_u8(buf, 0) -> env.message_type
  |                                                              |-- read_u8(buf, 1) -> env.reliability_class
  |                                                              |-- read_u8(buf, 2) -> env.priority
  |                                                              |-- read_u8(buf, 3) -> padding1 (local)
  |                                                              |
  |                                  [padding1 != 0] ---------  |-- if (padding1 != 0U)
  |<-- ERR_INVALID [env partial] <----------------------------  |     return ERR_INVALID
  |                                                              |
  |                                                              |-- read_u64(buf,  4) -> env.message_id
  |                                                              |-- read_u64(buf, 12) -> env.timestamp_us
  |                                                              |-- read_u32(buf, 20) -> env.source_id
  |                                                              |-- read_u32(buf, 24) -> env.destination_id
  |                                                              |-- read_u64(buf, 28) -> env.expiry_time_us
  |                                                              |-- read_u32(buf, 36) -> env.payload_length
  |                                                              |-- read_u32(buf, 40) -> padding2 (local)
  |                                                              |
  |                                  [padding2 != 0] ---------  |-- if (padding2 != 0U)
  |<-- ERR_INVALID [header written] <-------------------------  |     return ERR_INVALID
  |                                                              |
  |                                                              |-- NEVER_COMPILED_OUT_ASSERT(offset == 44U)
  |                                                              |
  |                              [payload_length > 4096] -----  |-- if (payload_length > MSG_MAX_PAYLOAD_BYTES)
  |<-- ERR_INVALID [header written] <-------------------------  |     return ERR_INVALID
  |                                                              |
  |                       [buf_len < 44 + payload_len] ------   |-- if (buf_len < total_size)
  |<-- ERR_INVALID [header written] <-------------------------  |     return ERR_INVALID
  |                                                              |
  |                                    [payload_length > 0] --  |-- if (payload_length > 0U)
  |                                                              |     memcpy(env.payload, buf+44, payload_length)
  |                                                              |
  |                              [!envelope_valid(env)] ------  |-- NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
  |         ABORT / IResetHandler::on_fatal_assert() <--------  |     -> program terminates (all builds)
  |                                                              |
  |<-- Result::OK [env fully populated] <---------------------  |-- return Result::OK
```

---

## 13. Initialization vs Runtime Flow

### Initialization phase
None for `Serializer` itself — it has no state and is non-instantiable. The input buffer
(`buf`) is initialized by the transport layer before `deserialize()` is called. The output
envelope (`env`) may be uninitialized on entry; `deserialize()` zero-fills it after the first
bounds check (Step 3).

### Runtime phase
Called once per inbound framed message: once per TCP frame in `TcpBackend::recv_from_client()`,
once per datagram in `UdpBackend::recv_one_datagram()`. Execution is strictly linear: assertions
→ length guard → zero-fill → 11 read helper invocations → 2 padding validations → 2 bounds
checks → conditional `memcpy` → post-condition → return. No loops, no recursion, no dynamic
allocation, no branching within the read helpers.

---

## 14. Known Risks / Observations

### Risk 1 — `env` uninitialized on the earliest error path
If `buf_len < 44U` (Branch 1), `envelope_init()` has NOT been called. The caller receives
`ERR_INVALID` with `env` in whatever state it had before the call. Both confirmed callers
(`TcpBackend::recv_from_client()` and `UdpBackend::recv_one_datagram()`) declare `env` as a
local variable and check the return value before using it — so the callers are safe. A future
caller that passes an already-used `env` and reads it on error return would receive stale data.

### Risk 2 — Partially-populated `env` on intermediate error paths
After Branch 2 (padding1) and beyond, `envelope_init()` has been called so `env` is at
minimum zero-initialized. However, fields written before the error point contain wire values.
For example, after a `padding2` failure, `env.payload_length` contains the wire-supplied value
(potentially attacker-controlled) and all other header fields are set. Callers must not read
any `env` field on non-OK return.

### Risk 3 — `NEVER_COMPILED_OUT_ASSERT` on post-condition terminates program
The post-condition `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` at Serializer.cpp:256 fires
unconditionally for any message with `source_id == 0` or `message_type == INVALID` in the wire
data that passes all structural checks. This terminates the program (abort in debug/test;
`IResetHandler::on_fatal_assert()` in production) rather than returning `ERR_INVALID`. A remote
peer sending a frame with `source_id == 0` would crash the receiving process. This is an
intentional safety-vs-availability trade-off.

### Risk 4 — Unvalidated enum cast from `uint8_t`
`read_u8` returns `uint8_t`. It is cast directly to `MessageType` or `ReliabilityClass` without
a range check. A wire byte value not defined in the enum (e.g., `message_type = 0x77`) produces
an out-of-range enum value — implementation-defined behavior per C++17 §[expr.static.cast].
MISRA C++:2023 strongly discourages unvalidated enum casts. No range guard exists between the
`read_u8` call and the `static_cast`.

### Risk 5 — Offset advancement style inconsistency with `serialize()`
`serialize()` uses return-value chaining (`offset = write_u8(...)`). `deserialize()` uses manual
`offset += N` after each read helper call. Both produce the same final offsets. The manual
pattern is more error-prone to future edits: omitting a `+= 4U` after `read_u32()` would
silently misalign all subsequent reads and corrupt the decoded envelope without triggering any
immediate error.

### Risk 6 — No checksum or integrity verification
The wire format has no CRC, checksum, or HMAC. A corrupted buffer that maintains valid padding
bytes and a valid `payload_length` passes all structural checks. Only the semantic
post-condition (`envelope_valid`) provides any validity signal beyond structure — and it
terminates the program rather than returning `ERR_INVALID`.

### Risk 7 — Trailing zero bytes in `env.payload`
If `env.payload_length < MSG_MAX_PAYLOAD_BYTES`, bytes `payload[payload_length..4095]` remain
0 from `envelope_init()`. This is safe but creates zeroed trailing bytes that could be
mistaken for meaningful data if `payload_length` is not checked by the application layer.

---

## 15. Unknowns / Assumptions

**[CONFIRMED-1]** `WIRE_HEADER_SIZE = 44U` is defined in Serializer.hpp:47 as a
`static const uint32_t`. Derivation: 1+1+1+1+8+8+4+4+8+4+4 = 44 bytes.

**[CONFIRMED-2]** `NEVER_COMPILED_OUT_ASSERT` fires unconditionally regardless of NDEBUG.
Defined in src/core/Assert.hpp. The post-condition at Serializer.cpp:256 is always active.

**[CONFIRMED-3]** `envelope_init()` is at MessageEnvelope.hpp:47: `memset(&env, 0, sizeof(MessageEnvelope))`
followed by `env.message_type = MessageType::INVALID`.

**[CONFIRMED-4]** `envelope_valid()` is at MessageEnvelope.hpp:63: checks `message_type != INVALID (255U)`,
`payload_length <= 4096U`, `source_id != 0U`.

**[CONFIRMED-5]** `MSG_MAX_PAYLOAD_BYTES = 4096U`, `NODE_ID_INVALID = 0U` confirmed in
src/core/Types.hpp. The `payload` field is `uint8_t payload[MSG_MAX_PAYLOAD_BYTES]` inline
within `MessageEnvelope`.

**[CONFIRMED-6]** `read_u8`/`u32`/`u64` use `NEVER_COMPILED_OUT_ASSERT` internally
(Serializer.cpp:71–72, 80–81, 96–97). They take an absolute offset (not self-advancing); the
caller advances `offset` with `+=` after each call.

**[CONFIRMED-7]** `UdpBackend::recv_one_datagram()` at UdpBackend.cpp:180 is a confirmed caller.
It passes `m_wire_buf` as `buf` and `out_len` (set by `socket_recv_from`) as `buf_len`.

**[CONFIRMED-8]** `TcpBackend::recv_from_client()` at TcpBackend.cpp:238 is a confirmed caller.
It passes `m_wire_buf` as `buf` and `out_len` (the frame byte count from `tcp_recv_frame()`)
as `buf_len`.

**[ASSUMPTION-A]** Enum values in the wire byte stream always correspond to defined enumerators.
If a non-conforming sender transmits an undefined enum byte, the behavior is
implementation-defined (most compilers store the integer value in the enum variable without
range-checking, but this is not guaranteed by C++17).

**[ASSUMPTION-B]** The caller initializes `env` before calling `deserialize()` or does not read
`env` on `ERR_INVALID` return, specifically for the `buf_len < 44` case where `envelope_init()`
is not called. Both confirmed callers declare `env` as a local variable and test the result
before using it — this is safe for all error paths including Risk 1.
