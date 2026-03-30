## 1. Use Case Overview

Use Case: Receiver drops an expired message before delivering it to the application.

A DATA message has arrived at the receiver via TCP transport, was deserialized successfully, and was queued in RingBuffer. When DeliveryEngine::receive() pops the envelope, it checks the message's expiry_time_us against the current monotonic clock. The message's expiry time has already passed, so timestamp_expired() returns true, and DeliveryEngine returns ERR_EXPIRED without delivering the message to the application and without sending an ACK.

Scope: core layer (DeliveryEngine, Timestamp). Assumes the platform layer (TcpBackend, Serializer, RingBuffer) has already successfully completed the inbound deserialization path from UC_06. This use case begins at the moment DeliveryEngine::receive() has a valid envelope from the transport.

## 2. Entry Points

Primary entry point:
    DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, lines 147-220

Expiry check at line 169:
    if (timestamp_expired(env.expiry_time_us, now_us))

Key inline function:
    timestamp_expired(uint64_t expiry_us, uint64_t now_us) — Timestamp.hpp:47-55

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller invokes DeliveryEngine::receive
    engine.receive(env, timeout_ms, now_us)
    now_us is supplied by the caller. In a typical application loop:
        uint64_t now_us = timestamp_now_us();
        Result res = engine.receive(env, timeout_ms, now_us);
    Preconditions: assert(m_initialized), assert(now_us > 0ULL).

Step 2 — Init check (DeliveryEngine.cpp:154-156)
    if (!m_initialized) return Result::ERR_INVALID;
    For this use case: m_initialized == true. Passes.

Step 3 — Receive from transport (DeliveryEngine.cpp:159-163)
    Result res = m_transport->receive_message(env, timeout_ms);
    TcpBackend::receive_message() is called. It pops a MessageEnvelope from m_recv_queue.
    The envelope has:
        message_type = DATA,
        expiry_time_us = T_expiry (some time in the past relative to now_us),
        source_id = valid (non-zero),
        payload_length <= 4096.
    Returns OK.
    If res != OK: return immediately (timeout/IO error). Skip all following steps.

Step 4 — Validate received envelope (DeliveryEngine.cpp:166)
    assert(envelope_valid(env))
    envelope_valid(env): message_type(DATA=0) != INVALID(255) ✓,
    payload_length <= 4096 ✓, source_id != 0 ✓.
    Assertion passes (envelope is structurally valid; expiry is a semantic property, not a
    structural one, so envelope_valid is satisfied regardless of expiry state).

Step 5 — Expiry check (DeliveryEngine.cpp:169-174) ← CRITICAL DECISION
    if (timestamp_expired(env.expiry_time_us, now_us))

    Inside timestamp_expired (Timestamp.hpp:47-55):
        assert(expiry_us <= 0xFFFFFFFFFFFFFFFFULL)  [trivially true]
        assert(now_us    <= 0xFFFFFFFFFFFFFFFFULL)  [trivially true]
        return (expiry_us != 0ULL) && (now_us >= expiry_us)

        For an expired message:
            expiry_time_us = T_expiry > 0 → (T_expiry != 0ULL) is true.
            now_us >= T_expiry → true (message is stale).
        Returns true.

Step 6 — Expiry branch taken (DeliveryEngine.cpp:170-174)
    Logger::log(Severity::WARNING_LO, "DeliveryEngine",
                "Dropping expired message_id=%llu from src=%u",
                env.message_id, env.source_id);
    return Result::ERR_EXPIRED;

Step 7 — Return to caller
    DeliveryEngine::receive returns ERR_EXPIRED.
    env still holds the expired envelope's content (not cleared).
    The caller receives ERR_EXPIRED and must not process the envelope.

    No ACK is sent.
    No dedup recording occurs (expiry check precedes dedup check).
    No AckTracker or RetryManager interaction occurs.

Special case — expiry_time_us == 0 (never-expires):
    timestamp_expired returns false when expiry_time_us == 0.
    ACK messages always have expiry_time_us = 0 (set by envelope_make_ack).
    DATA messages with no deadline should also set expiry_time_us = 0.

Special case — now_us is zero:
    assert(now_us > 0ULL) fires in debug builds.
    In production (no assert), now_us = 0 would cause timestamp_expired to return false for any
    non-zero expiry_us (since 0 < expiry_us for any future time). All messages would be treated
    as not-yet-expired. This would be a silent correctness failure.

## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)
    [m_initialized check]
    m_transport->receive_message(env, timeout_ms)           [TcpBackend, returns OK]
        RingBuffer::pop(envelope)                           [pops expired DATA message]
    assert(envelope_valid(env))                             [inline, passes]
    timestamp_expired(env.expiry_time_us, now_us)           [inline]
        assert bounds on both args
        return (expiry_us != 0ULL) && (now_us >= expiry_us)  → true
    [branch: expired == true]
    Logger::log(WARNING_LO, "Dropping expired message_id=...")
    return ERR_EXPIRED

## 5. Key Components Involved

DeliveryEngine (src/core/DeliveryEngine.cpp)
    The single active component in this use case after the transport delivers the envelope.
    Performs the expiry check and returns ERR_EXPIRED.

timestamp_expired (src/core/Timestamp.hpp:47-55)
    Inline function. Two boolean comparisons on uint64_t values. No state.

timestamp_now_us (src/core/Timestamp.hpp:28-42)
    Called by the caller (application) to produce now_us. Uses clock_gettime(CLOCK_MONOTONIC).
    Not called inside DeliveryEngine::receive() itself — now_us is a parameter.

TcpBackend / RingBuffer:
    Already completed their role (UC_06). The expired message was deserialized and queued
    normally; expiry is not checked at the transport level in TcpBackend.

MessageEnvelope:
    expiry_time_us field is the key datum: a uint64_t microsecond wall-clock value set by
    the sender at message creation. The receiver never modifies it.

Logger (src/core/Logger.hpp — not read):
    Called at WARNING_LO severity with message_id and source_id metadata.

Types.hpp:
    Result::ERR_EXPIRED = 7.

## 6. Branching Logic / Decision Points

Decision 1 (DeliveryEngine.cpp:154-156): m_initialized?
    NO → return ERR_INVALID immediately.
    YES → continue.

Decision 2 (DeliveryEngine.cpp:160-163): transport returned OK?
    NO → return the transport's error (ERR_TIMEOUT, ERR_IO, etc.).
    YES → continue with the received envelope.

Decision 3 (DeliveryEngine.cpp:169) [PRIMARY]: timestamp_expired?
    Sub-decision A: expiry_time_us == 0?
        YES → never expires; return false immediately.
        NO  → continue to comparison.
    Sub-decision B: now_us >= expiry_time_us?
        YES (expired) → return true → DeliveryEngine drops and returns ERR_EXPIRED.
        NO  (not yet) → return false → DeliveryEngine continues to control/dedup/deliver.

Decision 4 (reached only if NOT expired):
    envelope_is_control? → ACK resolution path (UC_08).
    reliability_class == RELIABLE_RETRY? → dedup path (UC_07).
    Otherwise: deliver to application.

For this use case, Decision 3 returns true and all subsequent decisions are bypassed.

## 7. Concurrency / Threading Behavior

timestamp_expired is a pure inline function; no shared state, no threading concerns.

timestamp_now_us uses clock_gettime(CLOCK_MONOTONIC). CLOCK_MONOTONIC is per-process (or
per-thread on some platforms); concurrent calls from different threads return independent
monotonic values. The returned uint64_t is passed by value as now_us into receive().

DeliveryEngine::receive() is assumed single-threaded (no mutex on m_ack_tracker, m_dedup, etc.).

The expired envelope sits in the caller's stack variable (env), which is not shared.

Timing race: if now_us is captured immediately before receive() is called, and receive() blocks
inside TcpBackend::receive_message() for up to timeout_ms milliseconds, the now_us value
passed in may be stale by the time the expiry check runs. A message with expiry_time_us just
slightly in the future could appear non-expired at the start but actually expire during the
transport wait. This is not detected by the current design.

[ASSUMPTION: Callers capture now_us immediately before calling receive() and the latency
between capture and the expiry check is negligible (bounded by transport receive time).]

## 8. Memory & Ownership Semantics (C/C++ Specific)

env (DeliveryEngine::receive parameter, MessageEnvelope&):
    Passed by non-const reference. On ERR_EXPIRED return, env is populated with the expired
    message's content. The caller owns the storage. DeliveryEngine does not clear env on error.
    If the caller inspects env after ERR_EXPIRED, it will see the expired message's fields.
    There is no convention in the visible code requiring env to be zeroed on error return.

expiry_time_us (uint64_t, field of MessageEnvelope):
    Read-only inside timestamp_expired. Not modified anywhere on the receive path.
    Set by the sender during message construction and preserved exactly through serialization
    (8-byte big-endian field in the wire format, read by Serializer::deserialize at offset 28).

now_us (uint64_t, parameter to receive()):
    Passed by value. A snapshot of CLOCK_MONOTONIC at the time of the call (or slightly before).
    Used only in timestamp_expired comparison; not stored.

timestamp_expired stack frame:
    No heap allocation. Two uint64_t parameters, two asserts, one comparison. O(1).

## 9. Error Handling Flow

ERR_EXPIRED (Result::ERR_EXPIRED = 7):
    Generated at DeliveryEngine.cpp:173.
    Logged at WARNING_LO (localized, recoverable).
    Returned to DeliveryEngine's caller (application layer or test harness).
    The caller must handle ERR_EXPIRED gracefully:
        Do not process env.
        Increment an expiry drop counter (if the application maintains metrics).
        Continue polling for the next message.

No cleanup beyond the log:
    No ACK is sent (expired message is silently dropped without acknowledgment).
    No retry cancellation (if the message was tracked by RetryManager on the sender side,
    the retry will continue until max_retries or the RetryManager's own expiry check).
    DuplicateFilter::record() is NOT called for expired messages. If the same message arrives
    again (duplicate retransmission of an expired message), it will NOT be recognized as a
    duplicate; it will be re-evaluated for expiry at the next receive() call.

assert(now_us > 0ULL) (DeliveryEngine.cpp:152):
    Guards against a zero-timestamp caller bug. Fires in debug; silent in production.

## 10. External Interactions

Transport layer (TcpBackend::receive_message):
    Called at the start, delivers the expired envelope. The transport has no knowledge of
    expiry; it simply provides whatever was deserialized from the wire.

Logger:
    Logger::log(WARNING_LO, "DeliveryEngine", "Dropping expired message_id=... from src=...")
    at line 171-173. This is the only external interaction during an expiry drop.

clock_gettime(CLOCK_MONOTONIC):
    Called by the application (not by DeliveryEngine itself) to produce now_us before calling
    receive(). This is the OS clock interaction that drives the expiry decision.

No socket I/O, no DuplicateFilter, no AckTracker, no RetryManager interaction.

## 11. State Changes / Side Effects

DeliveryEngine: no state changes (m_ack_tracker, m_dedup, m_retry_manager untouched).

TcpBackend / RingBuffer:
    The expired envelope has already been popped from m_recv_queue (inside receive_message).
    m_head was incremented (release) by the pop. This consumption is permanent; the expired
    message cannot be replayed from the queue.

Logger: one WARNING_LO log entry with message_id and source_id.

Summary: The expiry drop is effectively stateless at the DeliveryEngine level. The only
permanent side effect is the consumption of the envelope from the RingBuffer (which happened
inside transport receive_message) and the log emission.

## 12. Sequence Diagram (ASCII)

Application     DeliveryEngine     TcpBackend      RingBuffer    timestamp_expired    Logger
    |                 |                 |               |               |                |
    |--now_us=       |                 |               |               |                |
    | timestamp_      |                 |               |               |                |
    | now_us()        |                 |               |               |                |
    |                 |                 |               |               |                |
    |--receive(env,   |                 |               |               |                |
    |  timeout, now)->|                 |               |               |                |
    |                 |--recv_msg()---->|               |               |                |
    |                 |                 |--pop(env)---->|               |                |
    |                 |                 |   [expired    |               |                |
    |                 |                 |   DATA env]   |               |                |
    |                 |                 |<--OK,env------|               |                |
    |                 |<--OK,env--------|               |               |                |
    |                 |                                 |               |                |
    |                 |--assert(envelope_valid) [pass]  |               |                |
    |                 |                                 |               |                |
    |                 |--timestamp_expired(expiry, now)---------------->|                |
    |                 |    expiry != 0 → true           |               |                |
    |                 |    now >= expiry → true         |               |                |
    |                 |<--true--------------------------|-----------<---|                |
    |                 |                                 |               |                |
    |                 |--log("Dropping expired msg...")------------------------------------>|
    |                 |                                 |               |   [WARNING_LO] |
    |<--ERR_EXPIRED---|                                 |               |                |
    |                 |                                 |               |                |
    [Application must NOT process env; may log metrics or discard]

## 13. Initialization vs Runtime Flow

Initialization:
    No expiry-specific initialization. timestamp_expired is a pure function with no state.
    The expiry_time_us field in each MessageEnvelope is set by the sender at message creation
    time, stored in the wire format, and preserved through deserialization.

Runtime — message creation (sender side):
    Application sets env.expiry_time_us = timestamp_now_us() + (ttl_ms * 1000ULL)
    or uses timestamp_deadline_us(now_us, ttl_ms). [ASSUMPTION: no factory function visible
    in the read code; application constructs envelopes directly.]
    env.expiry_time_us = 0 for messages that should never expire.

Runtime — message transit:
    Serializer::serialize writes expiry_time_us at offset 28 as an 8-byte big-endian field.
    Serializer::deserialize reconstructs it identically at the receiver.
    The value is not modified by TcpBackend, RingBuffer, ImpairmentEngine, or any transport layer.

Runtime — expiry check (this use case):
    Performed once, inline, at the very start of DeliveryEngine::receive()'s processing logic,
    before dedup, control handling, or delivery.

Impact of impairment-induced latency:
    If ImpairmentEngine applies fixed latency (e.g., 3 seconds) to a message, the message may
    arrive at the receiver after its expiry_time_us has passed. The receiver's expiry check
    will drop it. This is the correct and intended behavior per the application requirements
    (CLAUDE.md §3.2: "Expiry handling: Drop or deprioritize expired messages before delivery").

## 14. Known Risks / Observations

Risk 1 — now_us is caller-supplied, not captured inside receive():
    The caller captures now_us before calling receive(), then receive() blocks inside
    TcpBackend::receive_message() for up to timeout_ms milliseconds before the expiry check
    runs. If a message's expiry_time_us is within the polling window, it may be accepted
    (now_us < expiry at the time of capture but now_us >= expiry if re-captured after the
    transport wait). The current design cannot detect this race. Messages with very tight
    TTLs may slip through unexpired even though they expire during the transport wait.
    Mitigation: recapture now_us inside DeliveryEngine::receive() before the timestamp_expired
    call. [This is a design gap; the current code does not do so.]

Risk 2 — Expired envelope content visible to caller:
    On ERR_EXPIRED return, env is populated with the expired message. If the caller accidentally
    processes env (e.g., ignores the return code), it may act on stale data. No defensive
    zeroing of env is performed on expiry drop.

Risk 3 — No ACK sent on expiry drop:
    The sender's RetryManager will continue retrying an expired-drop message until retry count
    exhaustion or until the sender's own expiry check (RetryManager::collect_due() checks
    expiry_us at RetryManager.cpp:156). The receiver silently drops without ACK, causing the
    sender to retry. This is consistent with the spec ("drop expired messages") but means
    the sender may waste retries on a receiver that will always drop them.

Risk 4 — Dedup window NOT updated on expiry drop:
    If the same message arrives multiple times (retransmissions) and it is expired at arrival,
    each retransmission is dropped via expiry without being recorded in DuplicateFilter.
    This is correct (dedup is for non-expired messages), but means the expiry-drop log entry
    will appear multiple times for the same message_id under retransmission. Metrics may need
    to de-duplicate warning logs.

Risk 5 — CLOCK_MONOTONIC vs wall-clock mismatch:
    timestamp_now_us() uses CLOCK_MONOTONIC (not CLOCK_REALTIME). If expiry_time_us is set
    by the sender using CLOCK_REALTIME (wall clock), and the receiver compares it against
    CLOCK_MONOTONIC, the comparison is between two different clock domains and the expiry
    decision will be incorrect. [ASSUMPTION: both sender and receiver use CLOCK_MONOTONIC
    consistently, or now_us at the receiver is derived from the same clock as the sender's
    expiry_time_us. The code uses timestamp_now_us() = CLOCK_MONOTONIC throughout, but there
    is no enforcement that the sender also uses CLOCK_MONOTONIC for expiry_time_us.]

Risk 6 — Monotonic clock does not account for system suspension:
    On embedded or desktop targets, CLOCK_MONOTONIC pauses during system sleep/suspension.
    A message that was non-expired when the system suspended may appear non-expired when it
    resumes (because CLOCK_MONOTONIC did not advance during sleep). This could cause messages
    to be delivered well past their intended TTL after a sleep/wake cycle.

## 15. Unknowns / Assumptions

[ASSUMPTION 1] The caller (application) calls timestamp_now_us() immediately before each
    call to receive() and passes the result as now_us. DeliveryEngine never re-queries the clock.

[ASSUMPTION 2] The sender uses timestamp_now_us() (CLOCK_MONOTONIC) when setting
    expiry_time_us, ensuring clock consistency between sender and receiver on the same host.
    For inter-host scenarios, clock synchronization (NTP, PTP) would be required for correct
    cross-host expiry behavior.

[ASSUMPTION 3] expiry_time_us is set by the application layer, not by TcpBackend or Serializer.
    Serializer preserves it verbatim. This is confirmed by the code.

[ASSUMPTION 4] The application layer handles ERR_EXPIRED by not processing the envelope.
    The DeliveryEngine API does not enforce this (env is not cleared on error).

[UNKNOWN 1] Whether the application layer maintains a counter of expiry drops for metrics
    purposes. No metrics hooks are visible in DeliveryEngine (only Logger calls).

[UNKNOWN 2] Whether impairment-induced delay is accounted for when setting message TTLs.
    If an impairment engine adds 5 seconds of latency and the message TTL is 3 seconds,
    all messages will be expired on arrival. The design requires that TTLs be set
    generously enough to accommodate maximum configured impairment delay.

[UNKNOWN 3] Whether DeliveryEngine::receive() re-queries the clock internally in any path.
    Confirmed: it does NOT. timestamp_now_us() is called in receive_message() (TcpBackend level)
    only for impairment collection, not for expiry. The expiry now_us is purely from the
    caller-supplied parameter.agentId: a58af85a6fa4394c1 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 88297
tool_uses: 15
duration_ms: 488234</usage>