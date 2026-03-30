## 1. Use Case Overview

Use Case: UC-25 — Serializer encodes a MessageEnvelope into a deterministic big-endian wire buffer.

The Serializer class provides a pure static method, Serializer::serialize(), that converts an in-memory MessageEnvelope struct into a fixed-layout, big-endian binary wire buffer. The method produces a 44-byte header followed by the payload bytes. No heap allocation is performed at any point. The output is fully deterministic: the same envelope always produces the same bit pattern, regardless of host endianness. This use case covers the complete execution path from call entry through field-by-field encoding to return.

Scope: Serializer.cpp lines 115-167, with helper calls to write_u8 (lines 22-29), write_u32 (lines 33-45), and write_u64 (lines 48-64).

---

## 2. Entry Points

Primary entry point:
    static Result Serializer::serialize(
        const MessageEnvelope& env,   // [in]  envelope to encode
        uint8_t*               buf,   // [out] caller-provided destination buffer
        uint32_t               buf_len,  // [in]  available bytes in buf
        uint32_t&              out_len   // [out] bytes written on success
    )
    Defined: Serializer.cpp line 115.

Callers observed in the codebase:
    TcpBackend (platform layer) — [ASSUMPTION] TcpBackend calls Serializer::serialize() before writing bytes to the socket. The call site is not visible in the files provided; this is inferred from the architecture requirement that the transport framing layer owns wire format.

Helper entry points (private, called only by serialize()):
    Serializer::write_u8   — Serializer.cpp line 22
    Serializer::write_u32  — Serializer.cpp line 33
    Serializer::write_u64  — Serializer.cpp line 48

Prerequisite functions called before serialize():
    envelope_valid()       — MessageEnvelope.hpp line 55 (validation gate)
    memcpy()               — <cstring>, payload copy (line 158)

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller prepares a MessageEnvelope.
    The calling layer (e.g., TcpBackend [ASSUMPTION]) populates a MessageEnvelope
    on the stack or in a static buffer, then calls Serializer::serialize() passing
    a destination buffer pointer and its capacity.

Step 2 — Precondition assertions (Serializer.cpp lines 121-122).
    assert(buf != nullptr)
    assert(buf_len <= 0xFFFFFFFFUL)
    Both fire in debug builds if violated. In release builds these compile away
    [ASSUMPTION: standard NDEBUG convention used].

Step 3 — Envelope validation gate (lines 125-127).
    envelope_valid(env) is called.
    envelope_valid() (MessageEnvelope.hpp line 55) checks three conditions:
        a. env.message_type != MessageType::INVALID  (value 255)
        b. env.payload_length <= MSG_MAX_PAYLOAD_BYTES  (4096)
        c. env.source_id != NODE_ID_INVALID  (0)
    If any condition fails: serialize() returns Result::ERR_INVALID immediately.
    No bytes are written to buf.

Step 4 — Buffer size check (lines 130-133).
    required_len = WIRE_HEADER_SIZE + env.payload_length
                 = 44 + payload_length
    If buf_len < required_len: returns Result::ERR_INVALID immediately.
    No bytes are written to buf.

Step 5 — Offset cursor initialized (line 136).
    uint32_t offset = 0U;
    This cursor advances with each helper call. It is the sole state variable
    for the encoding pass.

Step 6 — Header encoding, field by field (lines 139-150).
    All writes use the private helper functions. Each helper returns the new
    offset, which is immediately assigned back.

    6a. write_u8(buf, offset=0, env.message_type cast to uint8_t)
        Writes 1 byte at buf[0]. offset becomes 1.
        Possible values: DATA=0x00, ACK=0x01, NAK=0x02, HEARTBEAT=0x03.

    6b. write_u8(buf, offset=1, env.reliability_class cast to uint8_t)
        Writes 1 byte at buf[1]. offset becomes 2.
        Possible values: BEST_EFFORT=0x00, RELIABLE_ACK=0x01, RELIABLE_RETRY=0x02.

    6c. write_u8(buf, offset=2, env.priority)
        Writes 1 byte at buf[2]. offset becomes 3.
        0 = highest priority; no maximum enforced here beyond uint8_t range.

    6d. write_u8(buf, offset=3, 0U)   -- padding byte, hard-coded 0
        Writes 0x00 at buf[3]. offset becomes 4.

    6e. write_u64(buf, offset=4, env.message_id)
        Big-endian decomposition via 8 manual right-shifts:
            buf[4]  = (message_id >> 56) & 0xFF  -- most significant byte
            buf[5]  = (message_id >> 48) & 0xFF
            buf[6]  = (message_id >> 40) & 0xFF
            buf[7]  = (message_id >> 32) & 0xFF
            buf[8]  = (message_id >> 24) & 0xFF
            buf[9]  = (message_id >> 16) & 0xFF
            buf[10] = (message_id >>  8) & 0xFF
            buf[11] = (message_id >>  0) & 0xFF  -- least significant byte
        offset becomes 12.

    6f. write_u64(buf, offset=12, env.timestamp_us)
        Same 8-shift big-endian pattern into buf[12..19]. offset becomes 20.

    6g. write_u32(buf, offset=20, env.source_id)
        4-shift big-endian:
            buf[20] = (source_id >> 24) & 0xFF
            buf[21] = (source_id >> 16) & 0xFF
            buf[22] = (source_id >>  8) & 0xFF
            buf[23] = (source_id >>  0) & 0xFF
        offset becomes 24.

    6h. write_u32(buf, offset=24, env.destination_id)
        Same pattern into buf[24..27]. offset becomes 28.

    6i. write_u64(buf, offset=28, env.expiry_time_us)
        8-shift big-endian into buf[28..35]. offset becomes 36.

    6j. write_u32(buf, offset=36, env.payload_length)
        4-shift big-endian into buf[36..39]. offset becomes 40.

    6k. write_u32(buf, offset=40, 0U)  -- padding word, hard-coded 0
        Writes 0x00 0x00 0x00 0x00 into buf[40..43]. offset becomes 44.

Step 7 — Header size assertion (line 153).
    assert(offset == WIRE_HEADER_SIZE)  i.e., assert(44 == 44)
    This is a mandatory invariant check. If WIRE_HEADER_SIZE is ever changed
    without updating the write sequence (or vice versa), this fires.

Step 8 — Payload copy (lines 157-159).
    If env.payload_length > 0:
        memcpy(&buf[44], env.payload, env.payload_length)
    If payload_length == 0: the branch is skipped entirely; no memcpy call.
    The payload bytes are copied verbatim, with no byte-order transformation,
    because payload is declared opaque (application-defined serialization).

Step 9 — out_len assignment (line 161).
    out_len = required_len = WIRE_HEADER_SIZE + env.payload_length
    This is the only write to the caller's out_len reference variable.

Step 10 — Post-condition assertion (line 164).
    assert(out_len == WIRE_HEADER_SIZE + env.payload_length)
    Redundant by construction but required by Power of 10 rule 5 (>=2 assertions
    per function).

Step 11 — Return Result::OK (line 166).

---

## 4. Call Tree (Hierarchical)

Serializer::serialize()
├── envelope_valid(env)                         [MessageEnvelope.hpp:55]
│   ├── checks env.message_type != INVALID
│   ├── checks env.payload_length <= 4096
│   └── checks env.source_id != 0
├── write_u8(buf, 0, message_type)              [Serializer.cpp:22]
├── write_u8(buf, 1, reliability_class)         [Serializer.cpp:22]
├── write_u8(buf, 2, priority)                  [Serializer.cpp:22]
├── write_u8(buf, 3, 0U)                        [Serializer.cpp:22]
├── write_u64(buf, 4, message_id)               [Serializer.cpp:48]
├── write_u64(buf, 12, timestamp_us)            [Serializer.cpp:48]
├── write_u32(buf, 20, source_id)               [Serializer.cpp:33]
├── write_u32(buf, 24, destination_id)          [Serializer.cpp:33]
├── write_u64(buf, 28, expiry_time_us)          [Serializer.cpp:48]
├── write_u32(buf, 36, payload_length)          [Serializer.cpp:33]
├── write_u32(buf, 40, 0U)                      [Serializer.cpp:33]
└── memcpy(&buf[44], env.payload, payload_len)  [<cstring>]

All helpers are declared inline. The compiler is expected to inline them,
eliminating function-call overhead entirely [ASSUMPTION: compiler honors inline
keyword under -O1 or higher].

---

## 5. Key Components Involved

Serializer (Serializer.hpp/cpp)
    Non-instantiable utility class (private constructor, line 68).
    All methods are static. No instance state.
    Owns the WIRE_HEADER_SIZE constant = 44U.

MessageEnvelope (MessageEnvelope.hpp)
    Plain struct, stack-allocatable. Fixed-size: 4096 + fixed header fields.
    Passed by const reference; serialize() does not modify it.

envelope_valid() (MessageEnvelope.hpp line 55)
    Inline free function. Acts as a precondition gate.

Types.hpp
    Defines MSG_MAX_PAYLOAD_BYTES = 4096U, MessageType enum, ReliabilityClass enum,
    Result enum, NodeId typedef.

write_u8 / write_u32 / write_u64
    Private static inline helpers. Single-purpose, zero side effects, no allocation.
    Each takes (buf, offset, value) and returns new_offset.

memcpy (<cstring>)
    Used for bounded opaque payload copy. The (void) cast discards the return value;
    this is consistent with Power of 10 rule 7 because memcpy's return (dst pointer)
    carries no error information.

---

## 6. Branching Logic / Decision Points

Decision 1 — envelope_valid() gate (line 125)
    Condition: !envelope_valid(env)
    True  -> return ERR_INVALID immediately (no writes to buf)
    False -> continue to buffer size check

Decision 2 — Buffer size check (line 131)
    Condition: buf_len < (WIRE_HEADER_SIZE + env.payload_length)
    True  -> return ERR_INVALID immediately
    False -> continue to header encoding

Decision 3 — Payload copy guard (line 157)
    Condition: env.payload_length > 0U
    True  -> execute memcpy of payload_length bytes
    False -> skip memcpy entirely (zero-length payload is valid)

No other branches exist in the happy path. The 11 write calls are unconditional
sequential statements. There are no loops, no switches, and no recursion.

---

## 7. Concurrency / Threading Behavior

Serializer::serialize() is a purely functional static method with no shared state.
It has no class-level variables, no globals, and no static locals.

Thread safety: serialize() is fully re-entrant and can be called from multiple
threads simultaneously without synchronization, provided each call uses a distinct
(buf, env) pair.

The DeliveryEngine or TcpBackend layer above Serializer [ASSUMPTION] is responsible
for ensuring that a single MessageEnvelope is not mutated while serialize() is
operating on it.

There are no atomic operations, no locks, and no condition variables within this
function.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

env parameter
    Passed as const reference. Serializer borrows it read-only. Lifetime must
    exceed the duration of the serialize() call. Caller retains ownership.

buf parameter
    Raw pointer to a caller-allocated buffer (likely stack or static array at the
    TcpBackend [ASSUMPTION] level). Serializer writes to it but does not own or
    free it. Lifetime must exceed the call.

out_len parameter
    Non-const reference to a uint32_t. Serializer writes exactly once (line 161).
    Caller owns the variable.

No heap allocation occurs. No new, no malloc, no delete, no free anywhere in
this call tree.

payload within MessageEnvelope
    uint8_t payload[MSG_MAX_PAYLOAD_BYTES] — a fixed-size inline array.
    memcpy reads from env.payload[0..payload_length-1].
    Buffer overread is prevented by the envelope_valid() check and the
    payload_length <= 4096 bound enforced before memcpy is reached.

WIRE_HEADER_SIZE = 44U is a static const class member (Serializer.hpp line 45).
It has no storage beyond the compilation unit [ASSUMPTION: compiler treats it as
a compile-time constant, not an addressable object].

---

## 9. Error Handling Flow

All errors are returned via Result enum values. No exceptions. No errno consulted.

Error path 1 — Invalid envelope
    Trigger: envelope_valid() returns false
    Action:  return Result::ERR_INVALID
    out_len: not written (indeterminate / caller's prior value)
    buf:     not written

Error path 2 — Buffer too small
    Trigger: buf_len < WIRE_HEADER_SIZE + payload_length
    Action:  return Result::ERR_INVALID
    out_len: not written
    buf:     not written

Error path 3 — Assertion failure (debug builds only)
    Trigger: buf == nullptr, or offset overflow condition in a helper
    Action:  assert() aborts the process (SIGABRT in POSIX)
    These represent programming errors (contract violations), not runtime errors.
    In production/release builds these assertions are compiled out [ASSUMPTION].

Success path
    All checks pass -> return Result::OK
    out_len holds the total bytes written: 44 + payload_length

The caller is responsible for checking the return value (Power of 10 rule 7).
The Client.cpp and Server.cpp files demonstrate this pattern for engine.send()
returns, which ultimately propagates from serialize().

---

## 10. External Interactions

memcpy (line 158)
    The only external function called. Sourced from <cstring>. Used for the
    opaque payload copy only. All other data movement is done with explicit
    array subscript writes.

No file I/O. No socket calls. No OS calls. No dynamic memory. No global variables
read or written. No logging calls within serialize() itself.

The function is entirely self-contained at the core layer, with no dependency on
platform or app layers, consistent with the architecture rules in CLAUDE.md.

---

## 11. State Changes / Side Effects

Outputs written:
    buf[0..43]                      — 44-byte header in big-endian wire format
    buf[44..44+payload_length-1]    — verbatim payload copy (if payload_length > 0)
    out_len                         — set to 44 + payload_length on success only

No state changes within Serializer or any other object. serialize() has no side
effects beyond writing to its output parameters. The input envelope is untouched.

---

## 12. Sequence Diagram (ASCII)

    Caller                  Serializer::serialize()       envelope_valid()    write_u8/u32/u64    memcpy
      |                            |                            |                   |                |
      |---serialize(env,buf,len)-->|                            |                   |                |
      |                            |---envelope_valid(env)----->|                   |                |
      |                            |<---true/false--------------+                   |                |
      |                            |  [false] return ERR_INVALID                   |                |
      |                            |  [true] check buf_len                          |                |
      |                            |  [too small] return ERR_INVALID               |                |
      |                            |  [ok] offset = 0                              |                |
      |                            |---write_u8(message_type)-------------------->|                |
      |                            |<--offset=1------------------------------------|                |
      |                            |---write_u8(reliability_class)--------------->|                |
      |                            |<--offset=2------------------------------------|                |
      |                            |---write_u8(priority)------------------------>|                |
      |                            |<--offset=3------------------------------------|                |
      |                            |---write_u8(0U, padding)-------------------->|                |
      |                            |<--offset=4------------------------------------|                |
      |                            |---write_u64(message_id)-------------------->|                |
      |                            |<--offset=12-----------------------------------|                |
      |                            |---write_u64(timestamp_us)------------------>|                |
      |                            |<--offset=20-----------------------------------|                |
      |                            |---write_u32(source_id)--------------------->|                |
      |                            |<--offset=24-----------------------------------|                |
      |                            |---write_u32(destination_id)--------------->|                |
      |                            |<--offset=28-----------------------------------|                |
      |                            |---write_u64(expiry_time_us)--------------->|                |
      |                            |<--offset=36-----------------------------------|                |
      |                            |---write_u32(payload_length)--------------->|                |
      |                            |<--offset=40-----------------------------------|                |
      |                            |---write_u32(0U, padding word)------------->|                |
      |                            |<--offset=44-----------------------------------|                |
      |                            |  assert(offset == 44)                         |                |
      |                            |  [payload_length > 0]                         |                |
      |                            |---memcpy(buf+44, payload, length)------------------------------> |
      |                            |  out_len = 44 + payload_length                |                |
      |                            |  assert(out_len == 44 + payload_length)       |                |
      |<--Result::OK + out_len-----|                            |                   |                |

---

## 13. Initialization vs Runtime Flow

Initialization phase:
    No initialization is required for Serializer. The class has no constructor,
    no static mutable state, and no init() method. WIRE_HEADER_SIZE is a
    compile-time constant.

    The TransportConfig and ChannelConfig objects (covering buffer sizes, ports,
    etc.) are initialized by the app layer (Client.cpp / Server.cpp) before any
    encode/decode takes place, but that initialization is not within Serializer's
    scope.

Runtime phase:
    serialize() is called once per outbound message.
    Execution is strictly linear: validation -> header writes -> payload copy -> return.
    There is no persistent state between calls.
    Typical hot path (valid envelope, sufficient buffer): ~15 inline function calls
    reduced to ~50 store instructions by the compiler [ASSUMPTION].

---

## 14. Known Risks / Observations

Risk 1 — Padding validation asymmetry
    serialize() writes padding bytes as hard-coded 0. deserialize() validates them
    and returns ERR_INVALID if non-zero. This is correct behavior, but it means
    any producer that writes non-zero padding (e.g., a buggy third-party tool)
    will be rejected. This is intentional defense-in-depth.

Risk 2 — payload_length not re-validated against MSG_MAX_PAYLOAD_BYTES inside serialize()
    envelope_valid() checks payload_length <= 4096. If envelope_valid() is somehow
    bypassed (e.g., by a future caller that checks the result but ignores it), the
    buffer size check (step 4) still protects buf from overrun, but only if buf_len
    is passed accurately. If a caller passes a fraudulently large buf_len, an
    overrun is possible. The assert on buf_len only checks the uint32_t range, not
    an application-level maximum.

Risk 3 — Release-build assertion removal
    The two assertions in serialize() and all helper assertions are standard
    assert() calls. Under NDEBUG they vanish. In a production/flight build, the
    buf==nullptr and offset overflow checks disappear, relying entirely on callers
    to maintain the contract.

Risk 4 — (void) cast on memcpy
    The return value of memcpy is explicitly discarded with (void). This is
    correct per MISRA C++:2023 because memcpy's return (the dst pointer) carries
    no error information. However, static analysis tools that enforce "check all
    returns" may flag this; the (void) cast is the conventional suppression.

Risk 5 — No WIRE_HEADER_SIZE version field
    There is no version byte in the wire header. If the format changes, there is
    no mechanism for a receiver to detect a version mismatch. The padding bytes
    serve as a limited sanity check, but this is not a versioning scheme.

---

## 15. Unknowns / Assumptions

[ASSUMPTION-1] TcpBackend calls Serializer::serialize() before performing a socket
    write. This is architecturally required (platform layer owns framing) but the
    TcpBackend source file was not provided.

[ASSUMPTION-2] Assertions are compiled away under NDEBUG in production builds.
    The codebase does not show a custom ASSERT macro that would remain active.

[ASSUMPTION-3] The compiler inlines write_u8/u32/u64. They are declared inline
    in Serializer.cpp and are small enough to be inlined under any optimization
    level above -O0.

[ASSUMPTION-4] The maximum value of payload_length that reaches serialize() is
    bounded by the envelope_valid() check. No separate re-check exists inside
    serialize() for the case where the caller passes env.payload_length > 4096
    while also passing a buf_len large enough to pass the size guard.

[ASSUMPTION-5] The out_len output parameter is always initialized to 0 or some
    prior valid value by the caller before calling serialize(), so that on error
    return (where out_len is not written) the caller does not act on garbage.
    No enforcement of this contract is visible in serialize() itself.

Unknown-1: Whether TcpBackend uses a static wire buffer of size
    WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 44 + 4096 = 4140 bytes, or a
    larger buffer. This determines the effective maximum message wire size.

Unknown-2: The endianness of the host platform. The code is explicitly
    endianness-independent (manual shifts), so this does not affect correctness,
    but it is unknown for completeness.


---
---
---