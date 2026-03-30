## 1. Use Case Overview

Use Case: Receiver detects and drops a duplicate message using the sliding-window dedup filter.

A sender has retransmitted a message (reliability_class = RELIABLE_RETRY) that the receiver has already seen and recorded. The receiver's DeliveryEngine::receive() calls DuplicateFilter::check_and_record() which finds the (source_id, message_id) pair already present in its fixed-size circular window and returns ERR_DUPLICATE. DeliveryEngine propagates ERR_DUPLICATE to the caller without delivering the duplicate message to the application.

Scope: core layer (DeliveryEngine, DuplicateFilter). The platform layer has already successfully delivered a MessageEnvelope to DeliveryEngine via TcpBackend::receive_message() (traced in UC_06). This use case begins at DeliveryEngine::receive() with a valid, non-expired DATA envelope in hand.

## 2. Entry Points

Primary entry point:
    DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, lines 147-220

The deduplication path is entered at line 201:
    Result dedup_res = m_dedup.check_and_record(env.source_id, env.message_id);

Sub-entries within DuplicateFilter:
    DuplicateFilter::check_and_record(NodeId src, uint64_t msg_id)  — DuplicateFilter.cpp:91-108
    DuplicateFilter::is_duplicate(NodeId src, uint64_t msg_id)      — DuplicateFilter.cpp:38-55
    DuplicateFilter::record(NodeId src, uint64_t msg_id)            — DuplicateFilter.cpp:61-85

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller invokes DeliveryEngine::receive
    Application or test harness calls:
        engine.receive(env, timeout_ms, now_us)
    Preconditions (DeliveryEngine.cpp:151-152):
        assert(m_initialized)
        assert(now_us > 0ULL)

Step 2 — Receive from transport (DeliveryEngine.cpp:159-163)
    Result res = m_transport->receive_message(env, timeout_ms);
    This dispatches through TcpBackend (or another TransportInterface implementation).
    For this use case: assume the transport returns OK and populates env with a valid
    MessageEnvelope where message_type = DATA, reliability_class = RELIABLE_RETRY,
    and source_id / message_id match a previously processed message.

Step 3 — Validate received envelope (DeliveryEngine.cpp:166)
    assert(envelope_valid(env))
    envelope_valid checks: message_type != INVALID, payload_length <= 4096, source_id != 0.
    Passes (assumed valid for this use case).

Step 4 — Check expiry (DeliveryEngine.cpp:169-174)
    if (timestamp_expired(env.expiry_time_us, now_us))
    timestamp_expired: returns (expiry_time_us != 0ULL) && (now_us >= expiry_time_us).
    For this use case: the message is NOT expired (expiry in the future or 0 = never).
    Condition is false; execution continues.

Step 5 — Check if message is a control message (DeliveryEngine.cpp:177)
    if (envelope_is_control(env))
    envelope_is_control: true iff message_type is ACK, NAK, or HEARTBEAT.
    For this use case: message_type = DATA; condition is false. Skip ACK/NAK processing.

Step 6 — Assert it is a data message (DeliveryEngine.cpp:196)
    assert(envelope_is_data(env))
    envelope_is_data: message_type == DATA. Passes.

Step 7 — Check reliability class (DeliveryEngine.cpp:199)
    if (env.reliability_class == ReliabilityClass::RELIABLE_RETRY)
    For this use case: RELIABLE_RETRY. Condition is TRUE. Dedup is applied.

Step 8 — Call DuplicateFilter::check_and_record (DeliveryEngine.cpp:201)
    Result dedup_res = m_dedup.check_and_record(env.source_id, env.message_id);

Step 9 — Inside DuplicateFilter::check_and_record (DuplicateFilter.cpp:91-108)
    Precondition: assert(m_count <= DEDUP_WINDOW_SIZE)
    Calls: is_duplicate(src, msg_id)

Step 10 — Inside DuplicateFilter::is_duplicate (DuplicateFilter.cpp:38-55)
    Linear scan of m_window[0..DEDUP_WINDOW_SIZE-1] (DEDUP_WINDOW_SIZE = 128):
        for (uint32_t i = 0U; i < 128U; ++i)
            assert(i < 128) — loop bound assertion
            if (m_window[i].valid && m_window[i].src == src && m_window[i].msg_id == msg_id)
                return true   ← DUPLICATE FOUND
    On a first occurrence of this message: no slot matches, returns false.
    On a retransmission: a slot matches (from a prior call to record()), returns true.
    For this use case: returns true (duplicate detected).

Step 11 — is_duplicate returns true (DuplicateFilter.cpp:97-99)
    check_and_record receives true from is_duplicate.
    Immediately returns Result::ERR_DUPLICATE.
    record() is NOT called (the duplicate is not re-recorded, the window is not advanced).

Step 12 — Back in DeliveryEngine::receive (DeliveryEngine.cpp:202-207)
    if (dedup_res == Result::ERR_DUPLICATE)
        Logger::log(INFO, "DeliveryEngine", "Suppressed duplicate message_id=... from src=...")
        return Result::ERR_DUPLICATE

Step 13 — Return to caller
    DeliveryEngine::receive returns ERR_DUPLICATE.
    The MessageEnvelope in env still holds the duplicate's content, but the caller
    is told not to process it (the return code signals the drop).
    No ACK is sent (the RELIABLE_RETRY ACK path is not reached for duplicates in this flow).

Note on the first receipt path (for contrast):
    When the same message arrived for the first time:
        is_duplicate returned false.
        check_and_record called record(src, msg_id):
            m_window[m_next] = {src, msg_id, valid=true}
            m_next = (m_next + 1) % 128
            if m_count < 128: m_count++
        check_and_record returned OK.
    That first reception was delivered to the application.

## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)
    m_transport->receive_message(env, timeout_ms)       [TcpBackend, returns OK]
    assert(envelope_valid(env))                         [inline predicate]
    timestamp_expired(env.expiry_time_us, now_us)       [inline, returns false]
    envelope_is_control(env)                            [inline, returns false]
    assert(envelope_is_data(env))                       [inline predicate]
    [reliability_class == RELIABLE_RETRY: true]
    DuplicateFilter::check_and_record(src, msg_id)
        assert(m_count <= 128)
        DuplicateFilter::is_duplicate(src, msg_id)
            [loop i=0..127]
                m_window[i].valid && .src==src && .msg_id==msg_id → true
            return true
        [is_duplicate returned true]
        return ERR_DUPLICATE
    [dedup_res == ERR_DUPLICATE: true]
    Logger::log(INFO, "Suppressed duplicate ...")
    return ERR_DUPLICATE

Contrast — first receipt (not this use case):
    DuplicateFilter::check_and_record(src, msg_id)
        is_duplicate() → false
        DuplicateFilter::record(src, msg_id)
            m_window[m_next] = {src, msg_id, true}
            m_next = (m_next+1) % 128
            m_count++  [if < 128]
        return OK
    [continue to deliver]
    return OK

## 5. Key Components Involved

DeliveryEngine (src/core/DeliveryEngine.cpp)
    Owns m_dedup (DuplicateFilter instance, embedded by value).
    Orchestrates transport receive → expiry check → control handling → dedup → delivery.

DuplicateFilter (src/core/DuplicateFilter.cpp/.hpp)
    Fixed circular buffer of 128 Entry structs {NodeId src, uint64_t msg_id, bool valid}.
    m_next: write pointer (0..127), wraps via modulo.
    m_count: number of valid entries (0..128); does not decrease (no explicit eviction mechanism
    other than overwrite when window is full).
    All state is embedded by value in DuplicateFilter; no pointers to heap.

MessageEnvelope (src/core/MessageEnvelope.hpp)
    The received envelope carries source_id and message_id used as the dedup key.
    reliability_class == RELIABLE_RETRY is the gating condition for dedup.

Timestamp (src/core/Timestamp.hpp)
    timestamp_expired() — inline, called before dedup; not involved in the dedup path itself.

Types.hpp
    DEDUP_WINDOW_SIZE = 128, Result::ERR_DUPLICATE = 6.

## 6. Branching Logic / Decision Points

Decision 1 (DeliveryEngine.cpp:160-163): Did transport return OK?
    NO (ERR_TIMEOUT etc.) → return that result immediately; dedup never reached.
    YES → continue.

Decision 2 (DeliveryEngine.cpp:169-174): Is the message expired?
    YES → return ERR_EXPIRED; dedup never reached.
    NO  → continue.

Decision 3 (DeliveryEngine.cpp:177): Is the message a control message?
    YES → handled as ACK/NAK/HEARTBEAT; dedup never reached.
    NO  → continue.

Decision 4 (DeliveryEngine.cpp:199): Is reliability_class == RELIABLE_RETRY?
    NO  (BEST_EFFORT or RELIABLE_ACK) → dedup is SKIPPED; message delivered unconditionally.
    YES → dedup is applied.

Decision 5 (DuplicateFilter.cpp:42-48): Does any slot match (valid && src && msg_id)?
    YES → is_duplicate returns true.
    NO  → is_duplicate returns false (linear scan exhausted 128 slots).

Decision 6 (DuplicateFilter.cpp:97-99): Did is_duplicate return true?
    YES → return ERR_DUPLICATE immediately; record() not called.
    NO  → call record(), return OK.

Decision 7 (DuplicateFilter.cpp:78-80): Is m_count < DEDUP_WINDOW_SIZE in record()?
    YES → increment m_count.
    NO  → do not increment (cap at 128); the oldest entry is overwritten silently at m_next.

## 7. Concurrency / Threading Behavior

DuplicateFilter has no atomics and no locks. It is not thread-safe. [ASSUMPTION: accessed
from a single thread.] All methods operate on m_window[], m_next, and m_count without
any synchronization primitives.

DeliveryEngine::receive is also assumed single-threaded (same as TcpBackend).

The m_dedup object is embedded by value in DeliveryEngine, so its lifetime is tied to the
DeliveryEngine instance. No sharing between DeliveryEngine instances [ASSUMPTION].

## 8. Memory & Ownership Semantics (C/C++ Specific)

DuplicateFilter::m_window[128]:
    Static array of 128 Entry structs. Each Entry: NodeId (uint32_t) + uint64_t + bool = 13 bytes
    + padding. Total ~128 * 16 = ~2048 bytes (depending on alignment). Embedded in DuplicateFilter,
    which is embedded in DeliveryEngine.

No dynamic allocation. All operations are O(N) scan (N = 128) on a fixed array.

is_duplicate is declared const (does not modify state). read-only linear scan.

record() writes exactly one slot and updates two integer counters. No memcpy or memset after init.

The env parameter to DeliveryEngine::receive is passed by reference. On ERR_DUPLICATE return,
env still holds the duplicate message content. The caller owns env and must decide what to do with
it. No ownership transfer occurs.

## 9. Error Handling Flow

ERR_DUPLICATE (Result::ERR_DUPLICATE = 6):
    Returned by DuplicateFilter::check_and_record → DeliveryEngine::receive → caller.
    Logged at INFO severity (not WARNING; duplicates are expected in RELIABLE_RETRY flows).
    The duplicate envelope content is not cleared; the caller receives the envelope but must
    interpret the return code as "do not process this message."

No other error results are generated by the dedup path itself. All errors upstream (transport,
expiry) are returned before the dedup check is reached.

Assertion failures (debug builds):
    assert(m_count <= DEDUP_WINDOW_SIZE) in check_and_record and record: invariant guard.
    assert(m_next < DEDUP_WINDOW_SIZE) in record: pre-condition on write pointer.
    assert(i < DEDUP_WINDOW_SIZE) in is_duplicate loop: loop bound guard.

## 10. External Interactions

Transport layer (TcpBackend::receive_message):
    Called at the start of DeliveryEngine::receive. Returns the duplicate envelope.
    Not called again during dedup processing.

Logger:
    Logger::log(INFO, "DeliveryEngine", "Suppressed duplicate message_id=...") at line 204.
    This is the only external interaction during the dedup drop path.

No socket I/O, no timer queries, no impairment engine interaction occurs during dedup.

## 11. State Changes / Side Effects

DuplicateFilter state: NOT modified on a duplicate drop. m_window, m_next, and m_count
are unchanged when ERR_DUPLICATE is returned (record() is not called).

DeliveryEngine state: NOT modified beyond the transport pop (which happened inside
receive_message). m_ack_tracker and m_retry_manager are not touched on a duplicate drop.

Logger state: one log entry emitted at INFO severity.

Summary: A duplicate drop is a read-only operation on DuplicateFilter with a single log side effect.

## 12. Sequence Diagram (ASCII)

Caller        DeliveryEngine     TcpBackend      DuplicateFilter    Logger
  |                |                 |                  |               |
  |--receive()---->|                 |                  |               |
  |                |--recv_msg()---->|                  |               |
  |                |<--OK,env--------|                  |               |
  |                |                                    |               |
  |                |--timestamp_expired() [false]        |               |
  |                |--envelope_is_control() [false]      |               |
  |                |--assert(envelope_is_data) [pass]   |               |
  |                |                                    |               |
  |                | [RELIABLE_RETRY: apply dedup]       |               |
  |                |--check_and_record(src,msg_id)------>|               |
  |                |                 |    assert(m_count<=128)           |
  |                |                 |    is_duplicate(src, msg_id)      |
  |                |                 |    [scan m_window[0..127]]        |
  |                |                 |    [slot found: valid&&src&&id]   |
  |                |                 |    is_duplicate → true            |
  |                |<--ERR_DUPLICATE-|                  |               |
  |                |                                    |               |
  |                |--log("Suppressed duplicate...")---->|               |
  |                |                                    |   log emit    |
  |<--ERR_DUPLICATE|                                    |               |

## 13. Initialization vs Runtime Flow

Initialization (DuplicateFilter::init, called from DeliveryEngine::init):
    memset(m_window, 0, sizeof(m_window)) — all 128 entries zeroed.
        valid=false for all slots (since false == 0).
    m_next = 0, m_count = 0.
    Postconditions asserted.

First receipt of a message (runtime):
    check_and_record called: is_duplicate finds no match, record() writes to m_window[m_next],
    m_next advances, m_count increments.

Subsequent duplicate receipt (this use case, runtime):
    check_and_record called: is_duplicate finds the (src, msg_id) pair at some slot i,
    returns ERR_DUPLICATE. Window state unchanged.

Window full (runtime, after 128 distinct messages):
    m_count = 128, m_next wraps to 0 (or wherever it is modulo 128).
    record() overwrites the oldest entry at m_next without clearing valid; the overwritten
    entry is now lost from the window. If the sender retransmits the overwritten message
    after eviction, it will appear as a new (non-duplicate) message and be delivered again.
    This is the known trade-off of a fixed-size circular dedup window.

## 14. Known Risks / Observations

Risk 1 — Window eviction causes false negatives after 128 unique messages:
    Once 128 distinct (src, msg_id) pairs have been recorded, new entries overwrite old ones.
    A retransmission of an evicted entry would pass through as a new delivery. This is by
    design for a bounded dedup filter but must be accounted for in retry timing: the retry
    window should be shorter than the time to cycle 128 messages.

Risk 2 — O(128) linear scan per message:
    is_duplicate does a full linear scan. For RELIABLE_RETRY workloads with high message rates,
    this is a fixed overhead of 128 comparisons per received message. Not a correctness issue,
    but a performance consideration.

Risk 3 — No ACK sent on duplicate drop:
    DeliveryEngine::receive returns ERR_DUPLICATE without sending an ACK. If the sender is
    waiting for an ACK (RELIABLE_ACK or RELIABLE_RETRY mode), a duplicate drop that is not
    acknowledged causes the sender's ACK timeout to fire, which may trigger another retry.
    This could create a feedback loop: sender retries → receiver drops as duplicate → sender
    times out → retries again. The design relies on the retry count limit (MAX_RETRY_COUNT=5)
    to eventually terminate this cycle. [ASSUMPTION: application layer is expected to
    handle the duplicate drop by sending ACK separately if needed, or relying on timeout.]

Risk 4 — Dedup only applied to RELIABLE_RETRY, not RELIABLE_ACK:
    At DeliveryEngine.cpp:199, dedup is gated on RELIABLE_RETRY. A RELIABLE_ACK message
    can be delivered multiple times if the transport somehow delivers it twice (unlikely over TCP,
    but possible with multiple client connections or impairment duplication). This is consistent
    with the spec (dedup is for retry flows) but should be noted.

Risk 5 — bool valid field may not be zero-initialized portably:
    memset to 0 in init() sets valid=false because false is represented as 0. This relies on
    the platform representing bool false as byte value 0, which is guaranteed by C++17 but
    worth noting as an implicit platform assumption.

## 15. Unknowns / Assumptions

[ASSUMPTION 1] The first receipt of this message was successfully processed and record() was
    called. The use case assumes the dedup window already contains the (src, msg_id) entry.

[ASSUMPTION 2] DuplicateFilter is accessed from a single thread only.

[ASSUMPTION 3] The envelope source_id and message_id fields are not modified by TcpBackend or
    Serializer between receipt and the dedup check. Traced code confirms this is true.

[ASSUMPTION 4] No ACK is sent by DeliveryEngine on a duplicate drop. The code confirms this:
    the ACK path (on_ack and the broader ACK sending mechanism) is not triggered on ERR_DUPLICATE.

[UNKNOWN 1] Whether the application layer re-sends an ACK for known duplicates as a policy decision.
    DeliveryEngine provides no such facility in the visible code.

[UNKNOWN 2] Whether m_dedup is shared between multiple DeliveryEngine instances (e.g., per channel).
    Based on the code, each DeliveryEngine has its own m_dedup by value. Multiple channels would
    have independent windows, meaning a duplicate arriving on a different channel would not be
    caught. [ASSUMPTION: one DeliveryEngine per channel.]

[UNKNOWN 3] The dedup window does not distinguish between different sessions from the same node.
    If source_id is reused after a restart and message_ids restart from 0, old window entries
    could suppress legitimately new messages from the restarted node until evicted.