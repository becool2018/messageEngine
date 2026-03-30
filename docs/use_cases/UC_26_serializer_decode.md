## 1. Use Case Overview

Use Case: UC-26 — Serializer decodes a wire buffer back into a MessageEnvelope with full field validation.

Serializer::deserialize() is the inverse of serialize(). It reads a big-endian binary buffer received from the wire, validates its structure, and populates a caller-provided MessageEnvelope. The process includes: minimum-length guard, zero-initialization of the output envelope, sequential big-endian field reads, two padding-byte validations, a payload_length bounds check against MSG_MAX_PAYLOAD_BYTES, a total-size guard against the supplied buffer length, an opaque payload memcpy, and a post-condition call to envelope_valid(). All reads use manual bit shifts. No heap allocation occurs.

Scope: Serializer.cpp lines 173-257, with helper calls to read_u8 (lines 67-73), read_u32 (lines 76-89), and read_u64 (lines 92-109). Also covers envelope_init() (MessageEnvelope.hpp line 41) and envelope_valid() (MessageEnvelope.hpp line 55).

---

## 2. Entry Points

Primary entry point:
    static Result Serializer::deserialize(
        const uint8_t*  buf,      // [in]  wire buffer received from socket/harness
        uint32_t        buf_len,  // [in]  number of valid bytes in buf
        MessageEnvelope& env      // [out] populated on success
    )
    Defined: Serializer.cpp line 173.

Callers observed in codebase:
    TcpBackend (platform layer) — [ASSUMPTION] calls deserialize() after reading
    a complete framed message from the socket. Call site not visible in provided files.

Helper entry points (private, called only by deserialize()):
    Serializer::read_u8   — Serializer.cpp line 67
    Serializer::read_u32  — Serializer.cpp line 76
    Serializer::read_u64  — Serializer.cpp line 92

Functions called within deserialize():
    envelope_init()       — MessageEnvelope.hpp line 41 (zero-fill output)
    envelope_valid()      — MessageEnvelope.hpp line 55 (post-condition check)
    memcpy()              — <cstring>, payload copy (line 250)

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller presents a received byte buffer.
    The calling layer (e.g., TcpBackend [ASSUMPTION]) has read buf_len bytes from
    a socket into a static or stack buffer, determined message boundaries via
    length-prefixed framing, and now calls deserialize() to reconstruct the envelope.

Step 2 — Precondition assertions (Serializer.cpp lines 178-179).
    assert(buf != nullptr)
    assert(buf_len <= 0xFFFFFFFFUL)
    Same contract enforcement as serialize(). Fire in debug builds only.

Step 3 — Minimum length guard (lines 182-184).
    if (buf_len < WIRE_HEADER_SIZE)  i.e., if (buf_len < 44)
        return Result::ERR_INVALID
    Protects all subsequent reads from out-of-bounds access. No reads occur before
    this check passes.

Step 4 — Zero-fill output envelope (line 187).
    envelope_init(env) is called.
    envelope_init() (MessageEnvelope.hpp line 41-46):
        memset(&env, 0, sizeof(MessageEnvelope))
        env.message_type = MessageType::INVALID   (= 255)
    This ensures that if deserialize() returns early on a later error, the output
    envelope is in a known invalid state rather than containing garbage.

Step 5 — Offset cursor initialized (line 190).
    uint32_t offset = 0U;

Step 6 — Header decoding, field by field (lines 193-231).

    6a. read_u8(buf, offset=0)  -> env.message_type (cast from uint8_t to MessageType)
        Line 193. offset manually advanced: offset += 1U  (line 194).
        Note: read_u8 takes the base offset directly; caller manually advances
        unlike serialize() which uses return-value chaining. Both patterns are
        used in the codebase for the same helpers.
        [OBSERVATION: deserialize() advances offset manually with +=; serialize()
        uses return-value assignment. Both are correct. The difference is stylistic
        inconsistency within the same class.]

    6b. read_u8(buf, offset=1)  -> env.reliability_class (cast to ReliabilityClass)
        Line 196. offset += 1U (line 197). offset = 2.

    6c. read_u8(buf, offset=2)  -> env.priority (plain uint8_t, no cast)
        Line 199. offset += 1U (line 200). offset = 3.

    6d. read_u8(buf, offset=3)  -> padding1 (local variable)
        Lines 202-203. offset += 1U. offset = 4.
        Padding validation (lines 205-207):
            if (padding1 != 0U)
                return Result::ERR_INVALID
        The output envelope remains in its envelope_init() state (message_type=INVALID).
        Fields read so far (message_type, reliability_class, priority) are already
        written to env, but env is invalid because message_type may be INVALID
        after memset. [OBSERVATION: if the wire byte for message_type happens to
        be 0x00 (DATA), env.message_type will read as DATA even on an error return.
        The caller must not use env if Result != OK.]

    6e. read_u64(buf, offset=4)  -> env.message_id
        Line 209. Reconstructs 64-bit value from 8 bytes:
            val  = buf[4] << 56
            val |= buf[5] << 48
            ... (same pattern as write_u64, reversed)
        offset += 8U (line 210). offset = 12.

    6f. read_u64(buf, offset=12)  -> env.timestamp_us
        Line 212. offset += 8U (line 213). offset = 20.

    6g. read_u32(buf, offset=20)  -> env.source_id
        Line 215. Reconstructs 32-bit value from 4 bytes. offset += 4U (line 216). offset = 24.

    6h. read_u32(buf, offset=24)  -> env.destination_id
        Line 218. offset += 4U (line 219). offset = 28.

    6i. read_u64(buf, offset=28)  -> env.expiry_time_us
        Line 221. offset += 8U (line 222). offset = 36.

    6j. read_u32(buf, offset=36)  -> env.payload_length
        Line 224. offset += 4U (line 225). offset = 40.

    6k. read_u32(buf, offset=40)  -> padding2 (local variable)
        Lines 227-228. offset += 4U. offset = 44.
        Padding validation (lines 230-232):
            if (padding2 != 0U)
                return Result::ERR_INVALID

Step 7 — Header size assertion (line 235).
    assert(offset == WIRE_HEADER_SIZE)  i.e., assert(44 == 44)
    Invariant check: the read sequence must sum to exactly 44 bytes.

Step 8 — payload_length bounds check (lines 238-240).
    if (env.payload_length > MSG_MAX_PAYLOAD_BYTES)  i.e., > 4096
        return Result::ERR_INVALID
    Prevents maliciously or accidentally large payload_length from causing
    buffer overread in the subsequent memcpy.

Step 9 — Total size check (lines 243-246).
    total_size = WIRE_HEADER_SIZE + env.payload_length = 44 + payload_length
    if (buf_len < total_size)
        return Result::ERR_INVALID
    This is the second bounds guard. The first (step 3) only checked for 44 bytes.
    This check ensures the actual payload data is present in the buffer.

Step 10 — Payload copy (lines 249-251).
    if (env.payload_length > 0U):
        memcpy(env.payload, &buf[offset], env.payload_length)
    offset is 44 at this point (from step 7). &buf[44] is the first payload byte.
    env.payload is the inline array within MessageEnvelope (fixed 4096 bytes).
    The bounds check in step 8 ensures env.payload_length <= 4096, which is
    exactly the size of env.payload[].

Step 11 — Post-condition assertion (line 254).
    assert(envelope_valid(env))
    envelope_valid() checks:
        env.message_type != MessageType::INVALID  (255)
        env.payload_length <= MSG_MAX_PAYLOAD_BYTES
        env.source_id != NODE_ID_INVALID  (0)
    This will fire if a valid-looking wire message carries source_id == 0 or
    message_type == 255 (INVALID). Such a message would have passed the
    padding checks but fails the semantic post-condition. [RISK: see section 14.]

Step 12 — Return Result::OK (line 256).

---

## 4. Call Tree (Hierarchical)

Serializer::deserialize()
├── envelope_init(env)                          [MessageEnvelope.hpp:41]
│   ├── memset(&env, 0, sizeof(MessageEnvelope))
│   └── env.message_type = MessageType::INVALID
├── read_u8(buf, 0)   -> message_type           [Serializer.cpp:67]
├── read_u8(buf, 1)   -> reliability_class      [Serializer.cpp:67]
├── read_u8(buf, 2)   -> priority               [Serializer.cpp:67]
├── read_u8(buf, 3)   -> padding1               [Serializer.cpp:67]
│   └── [if padding1 != 0] return ERR_INVALID
├── read_u64(buf, 4)  -> message_id             [Serializer.cpp:92]
├── read_u64(buf, 12) -> timestamp_us           [Serializer.cpp:92]
├── read_u32(buf, 20) -> source_id              [Serializer.cpp:76]
├── read_u32(buf, 24) -> destination_id         [Serializer.cpp:76]
├── read_u64(buf, 28) -> expiry_time_us         [Serializer.cpp:92]
├── read_u32(buf, 36) -> payload_length         [Serializer.cpp:76]
├── read_u32(buf, 40) -> padding2               [Serializer.cpp:76]
│   └── [if padding2 != 0] return ERR_INVALID
├── [payload_length > 4096] return ERR_INVALID
├── [buf_len < 44 + payload_length] return ERR_INVALID
├── memcpy(env.payload, buf+44, payload_length) [<cstring>]
└── envelope_valid(env)                         [MessageEnvelope.hpp:55]
    ├── checks message_type != INVALID
    ├── checks payload_length <= 4096
    └── checks source_id != 0

---

## 5. Key Components Involved

Serializer (Serializer.cpp lines 173-257)
    Static utility class. deserialize() is its only decode method.
    WIRE_HEADER_SIZE = 44U is the sole class-level constant consulted.

MessageEnvelope (MessageEnvelope.hpp)
    Output struct, passed by non-const reference.
    Fixed total size: sizeof(MessageEnvelope) = 4 fields of various sizes + 4096-byte payload.
    [ASSUMPTION: struct layout is not packed; compiler may add padding between fields.
    This does not affect wire format because serialize/deserialize use explicit field
    reads, not struct memcpy of the entire envelope from wire bytes.]

envelope_init() (MessageEnvelope.hpp line 41)
    Inline free function. memset to 0 + INVALID type. Called unconditionally
    before any field writes.

envelope_valid() (MessageEnvelope.hpp line 55)
    Inline free function. Post-condition gate. Returns bool.

read_u8 / read_u32 / read_u64
    Private static inline helpers. Each reads from buf at the given absolute offset.
    No offset advancement: caller advances offset manually with += after each call.

memcpy (<cstring>)
    Payload copy. Bounded by payload_length which is validated before the call.

MSG_MAX_PAYLOAD_BYTES (Types.hpp line 19) = 4096U
    The authoritative payload size limit.

NODE_ID_INVALID (Types.hpp line 29) = 0U
    A source_id equal to 0 fails envelope_valid(), causing the post-condition
    assert to fire even on an otherwise correctly-formed message.

---

## 6. Branching Logic / Decision Points

Decision 1 — Minimum length guard (line 182)
    Condition: buf_len < 44
    True  -> return ERR_INVALID (no reads, env not initialized)
    False -> proceed to envelope_init

Decision 2 — Padding byte 1 check (line 205)
    Condition: padding1 != 0U  (buf[3] must be 0x00)
    True  -> return ERR_INVALID (env partially populated)
    False -> continue reading fields

Decision 3 — Padding word 2 check (line 230)
    Condition: padding2 != 0U  (buf[40..43] as big-endian uint32 must be 0)
    True  -> return ERR_INVALID (env fully populated except payload)
    False -> continue

Decision 4 — payload_length range check (line 238)
    Condition: env.payload_length > 4096U
    True  -> return ERR_INVALID
    False -> proceed

Decision 5 — Total size check (line 244)
    Condition: buf_len < (44 + env.payload_length)
    True  -> return ERR_INVALID (env header fully populated, payload not copied)
    False -> proceed to memcpy

Decision 6 — Payload copy guard (line 249)
    Condition: env.payload_length > 0U
    True  -> execute memcpy
    False -> skip (zero-payload message)

Decision 7 — Post-condition assert (line 254)
    assert(envelope_valid(env))
    This is a debug-only check. In release builds it is absent.
    If envelope_valid() returns false here, it means a logically invalid envelope
    passed all structural checks (e.g., source_id == 0 in the wire bytes).
    In a debug build this aborts the process. In release builds, Result::OK
    is returned with an envelope that would fail envelope_valid() if the caller
    checks it independently. [RISK: see section 14.]

---

## 7. Concurrency / Threading Behavior

Identical analysis to serialize(): deserialize() is a pure function with no shared
state, no class variables, no globals read or written.

Thread safety: fully re-entrant. Multiple threads can call deserialize()
simultaneously on distinct (buf, env) pairs without synchronization.

The receiving layer (TcpBackend or DeliveryEngine [ASSUMPTION]) is responsible for
ensuring that buf is not modified concurrently while deserialize() is executing,
and that env is not accessed by another thread before deserialize() returns.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

buf parameter
    const uint8_t* — read-only pointer to caller-managed buffer.
    deserialize() does not write to buf and does not retain the pointer after return.
    Lifetime must encompass the call.

env parameter
    MessageEnvelope& — non-const reference to a caller-allocated envelope.
    Ownership stays with the caller. deserialize() writes all fields of env,
    starting with the total zero-fill via envelope_init().

    On error return, env is in an indeterminate but initialized state:
        - After decision 1 (buf too short): env is NOT initialized. envelope_init
          has NOT been called. [RISK: env contains whatever garbage the caller left.]
          Wait — re-reading the code: envelope_init(env) is called at line 187,
          AFTER the buf_len < WIRE_HEADER_SIZE check at lines 182-184. So if the
          buffer is too short, envelope_init is NOT called and env is unmodified.
          [RISK: caller receives ERR_INVALID with env in an unknown state.]
        - After decisions 2-5: envelope_init has been called; env is in a defined
          but partially-populated state with message_type = INVALID at memset time,
          then overwritten up to the field where the error occurred.

    env.payload (uint8_t[4096])
        memcpy writes up to payload_length bytes. The remaining bytes of payload[]
        are still 0 from envelope_init()'s memset.

padding1 and padding2
    Local stack variables. Exist only during the function call. Not accessible to caller.

No heap allocation anywhere in this call tree.

---

## 9. Error Handling Flow

Error path 1 — Buffer too short for header (line 182)
    Trigger: buf_len < 44
    Action:  return ERR_INVALID
    env:     NOT initialized (envelope_init not yet called)
    Important: this is the only error path where env is left untouched.

Error path 2 — Padding byte 1 non-zero (line 205)
    Trigger: buf[3] != 0
    Action:  return ERR_INVALID
    env:     envelope_init called; message_type, reliability_class, priority written;
             all remaining fields are 0 from memset.

Error path 3 — Padding word 2 non-zero (line 230)
    Trigger: big-endian value of buf[40..43] != 0
    Action:  return ERR_INVALID
    env:     envelope_init called; all header fields except padding written;
             payload[] is all zeros; payload_length may be set.

Error path 4 — payload_length too large (line 238)
    Trigger: env.payload_length > 4096
    Action:  return ERR_INVALID
    env:     same as error path 3; payload_length contains the invalid value.

Error path 5 — Buffer too short for payload (line 244)
    Trigger: buf_len < 44 + env.payload_length
    Action:  return ERR_INVALID
    env:     header fully populated; payload[] all zeros.

Error path 6 — Assertion failure (line 254, debug only)
    Trigger: envelope_valid() returns false after successful parse
    Action:  assert() fires -> SIGABRT in POSIX debug builds
    In release builds this path returns Result::OK with a technically invalid envelope.

Success path
    All checks pass -> return Result::OK
    env fully populated with wire data.

The caller must check the returned Result before using env.
The DeliveryEngine [ASSUMPTION] does check the result of whatever receive path
calls deserialize(), as seen from the pattern in Client.cpp line 138-140 where
result_ok(res) is tested before using the received envelope.

---

## 10. External Interactions

envelope_init() (MessageEnvelope.hpp line 41)
    Calls memset internally. Not an OS call; a <cstring> operation.

memcpy (line 250)
    Reads from buf (incoming wire data) into env.payload.
    The only place wire bytes enter the application-level envelope payload field.

No socket calls. No file I/O. No OS calls. No logging. No globals accessed.

---

## 11. State Changes / Side Effects

Inputs consumed (read-only):
    buf[0..44+payload_length-1]  — all consumed; none modified.

Outputs written to env (on success):
    env.message_type       <- buf[0] cast to MessageType
    env.reliability_class  <- buf[1] cast to ReliabilityClass
    env.priority           <- buf[2]
    env.message_id         <- buf[4..11] big-endian 64-bit
    env.timestamp_us       <- buf[12..19] big-endian 64-bit
    env.source_id          <- buf[20..23] big-endian 32-bit
    env.destination_id     <- buf[24..27] big-endian 32-bit
    env.expiry_time_us     <- buf[28..35] big-endian 64-bit
    env.payload_length     <- buf[36..39] big-endian 32-bit
    env.payload[0..n-1]   <- buf[44..44+n-1] verbatim copy

Discarded (wire fields read but not stored):
    buf[3]    — padding1 read into local, validated, then discarded
    buf[40..43] — padding2 read into local, validated, then discarded

No global state is modified. No Serializer class state is modified.

---

## 12. Sequence Diagram (ASCII)

    Caller              Serializer::deserialize()       envelope_init()   read_u8/u32/u64    memcpy   envelope_valid()
      |                         |                             |                  |               |            |
      |--deserialize(buf,len)-->|                             |                  |               |            |
      |                         | [buf_len < 44] return ERR  |                  |               |            |
      |                         |--envelope_init(env)-------->|                  |               |            |
      |                         |                             |--memset(env,0)   |               |            |
      |                         |                             | env.type=INVALID |               |            |
      |                         |<----------------------------+                  |               |            |
      |                         | offset = 0                  |                  |               |            |
      |                         |--read_u8(buf,0)------------>|----------------->|               |            |
      |                         |<--message_type-----------------------------|    |               |            |
      |                         |--read_u8(buf,1)------------>|----------------->|               |            |
      |                         |<--reliability_class---------|                  |               |            |
      |                         |--read_u8(buf,2)------------>|----------------->|               |            |
      |                         |<--priority------------------|                  |               |            |
      |                         |--read_u8(buf,3)------------>|----------------->|               |            |
      |                         |<--padding1 (local)----------|                  |               |            |
      |                         | [padding1 != 0] ERR_INVALID |                  |               |            |
      |                         |--read_u64(buf,4)----------->|----------------->|               |            |
      |                         |<--message_id----------------|                  |               |            |
      |                         |--read_u64(buf,12)---------->|----------------->|               |            |
      |                         |<--timestamp_us--------------|                  |               |            |
      |                         |--read_u32(buf,20)---------->|----------------->|               |            |
      |                         |<--source_id-----------------|                  |               |            |
      |                         |--read_u32(buf,24)---------->|----------------->|               |            |
      |                         |<--destination_id------------|                  |               |            |
      |                         |--read_u64(buf,28)---------->|----------------->|               |            |
      |                         |<--expiry_time_us------------|                  |               |            |
      |                         |--read_u32(buf,36)---------->|----------------->|               |            |
      |                         |<--payload_length------------|                  |               |            |
      |                         |--read_u32(buf,40)---------->|----------------->|               |            |
      |                         |<--padding2 (local)----------|                  |               |            |
      |                         | [padding2 != 0] ERR_INVALID |                  |               |            |
      |                         | assert(offset==44)          |                  |               |            |
      |                         | [payload_len>4096] ERR      |                  |               |            |
      |                         | [buf_len<44+plen] ERR       |                  |               |            |
      |                         | [plen > 0]                  |                  |               |            |
      |                         |--memcpy(env.payload,buf+44,plen)-------------->|               |            |
      |                         |--assert(envelope_valid(env))-----------------------------------+----------->|
      |                         |                             |                  |               |  true/abort|
      |<--Result::OK + env------|                             |                  |               |            |

---

## 13. Initialization vs Runtime Flow

Initialization phase:
    None for Serializer itself. The input buffer (buf) is assumed to have been
    initialized by the platform layer (TcpBackend) before the call. The output
    envelope (env) may be uninitialized on entry; deserialize() zero-fills it
    (after the first bounds check).

Runtime phase:
    Called once per inbound framed message.
    Execution is strictly linear: guards -> zero-fill -> reads -> validations -> copy -> return.
    No loops, no recursion, no dynamic allocation.

---

## 14. Known Risks / Observations

Risk 1 — env uninitialized on early error (decision 1)
    If buf_len < 44, envelope_init() is NOT called (it comes after the check).
    The caller receives ERR_INVALID but env is untouched. If the caller
    accidentally reads env.message_type or any field despite the error return,
    it reads garbage. Mitigation: callers should initialize env before calling
    deserialize() (e.g., envelope_init), but this is not enforced.

Risk 2 — Partially-populated env on intermediate errors (decisions 2-5)
    envelope_init() has been called, so env is at least in a defined zero state.
    However, some fields may have been written from the wire before the error.
    For example, after a padding2 failure, env.payload_length contains the
    wire value (potentially an attacker-controlled large number). Callers must
    not use any env field on non-OK return.

Risk 3 — Post-condition assert fires for source_id == 0 (NODE_ID_INVALID)
    A wire message with source_id == 0 passes all structural checks (padding,
    length bounds) but fails envelope_valid() at line 254, causing an assert
    abort in debug builds. In release builds, Result::OK is returned with an
    envelope that envelope_valid() would reject. This is a behavioral asymmetry
    between debug and release builds.

Risk 4 — Cast from uint8_t to MessageType / ReliabilityClass
    read_u8 returns a uint8_t. It is cast directly to the enum class. A wire
    byte value not defined in the enum (e.g., MessageType = 0xAA, which is
    not DATA/ACK/NAK/HEARTBEAT/INVALID) results in an enum value outside its
    defined range — undefined behavior per C++ standard. MISRA C++:2023
    strongly discourages unvalidated enum casts. There is no range-check between
    the read_u8 call and the cast.

Risk 5 — Offset advancement by manual += versus return-value chaining
    serialize() uses return-value chaining (offset = write_u8(...)). deserialize()
    uses manual advancement (read_u8(buf, offset); offset += 1U). These produce
    the same offsets but the manual pattern is more error-prone to future edits
    (e.g., forgetting a += 4U after a read_u32 would silently misalign all
    subsequent reads).

Risk 6 — No sequence number or checksum in wire format
    The wire format has no CRC, checksum, or HMAC. A corrupted buffer that
    maintains valid padding bytes and a valid payload_length will be accepted as
    a valid message. Only application-layer corruption (wrong message_type byte
    set to 255, or payload_length > 4096) would be rejected.

---

## 15. Unknowns / Assumptions

[ASSUMPTION-1] TcpBackend calls deserialize() with a buf and buf_len that have
    already been bounded by the framing layer (length-prefix), so buf_len is the
    exact length of the framed message, not the entire socket receive buffer.

[ASSUMPTION-2] The caller initializes env before calling deserialize(), providing
    a safe fallback for the error path where envelope_init is not reached (buf_len < 44).
    This is not enforced in the code.

[ASSUMPTION-3] Enum values in the wire byte stream always correspond to defined
    enum enumerators. If a non-conforming sender transmits an undefined enum byte,
    behavior is implementation-defined (most compilers will simply store the integer
    value in the enum variable, but this is not guaranteed by C++17).

[ASSUMPTION-4] The compiler inlines read_u8/u32/u64 as with write_ variants.

[ASSUMPTION-5] assert() is the only mechanism for the post-condition check.
    There is no fallback path that converts a post-condition failure into
    ERR_INVALID in release builds. The design relies on debug-build testing
    to catch these cases before production.

Unknown-1: Whether TcpBackend validates that the payload_length field embedded in
    the wire header matches the framing-layer length before calling deserialize().
    If both layers perform this check independently, it is defense-in-depth. If
    only deserialize() performs it, a truncated payload that passes the framing
    check could cause buf_len < total_size to trigger.

Unknown-2: The concrete size of sizeof(MessageEnvelope). The struct contains
    enum class fields (uint8_t underlying type), uint64_t fields (alignment 8),
    uint32_t fields (alignment 4), a uint8_t priority, and a uint8_t[4096] array.
    Compiler-inserted padding between fields is possible, making the struct larger
    than the sum of its fields. This does not affect wire format correctness but
    affects the cost of envelope_init's memset.


---
---
---