## 1. Use Case Overview

Use Case: Receiver receives an ACK and resolves the corresponding pending entry in the AckTracker.

An ACK message has arrived over the transport. The receiver's DeliveryEngine::receive() detects message_type == ACK, calls AckTracker::on_ack() to transition the corresponding slot from PENDING to ACKED, then calls RetryManager::on_ack() to cancel the retry schedule for that message. The overall state transition is PENDING → ACKED in AckTracker and active → inactive in RetryManager.

Context: this is the sender side calling receive() to process the in-band ACK coming back from the remote peer. The "receiver" here is the node that originally sent the data message and is now receiving the ACK response.

Scope: core layer (DeliveryEngine, AckTracker, RetryManager). Platform layer is assumed to have already delivered the ACK envelope via TcpBackend::receive_message().

## 2. Entry Points

Primary entry point:
    DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, lines 147-220

ACK processing begins at line 178:
    if (env.message_type == MessageType::ACK)

ACK resolution sub-entries:
    AckTracker::on_ack(NodeId src, uint64_t msg_id)    — AckTracker.cpp:81-110
    RetryManager::on_ack(NodeId src, uint64_t msg_id)  — RetryManager.cpp:97-131

Note: AckTracker and RetryManager are independent and both called sequentially.

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller invokes DeliveryEngine::receive
    engine.receive(env, timeout_ms, now_us)
    Preconditions: assert(m_initialized), assert(now_us > 0ULL).

Step 2 — Receive from transport (DeliveryEngine.cpp:159-163)
    Result res = m_transport->receive_message(env, timeout_ms);
    TcpBackend::receive_message pops a MessageEnvelope from m_recv_queue (see UC_06).
    The envelope has: message_type = ACK, source_id = remote peer's NodeId,
    message_id = the message_id of the original DATA message being ACKed.
    Returns OK.

Step 3 — Validate received envelope (DeliveryEngine.cpp:166)
    assert(envelope_valid(env))
    envelope_valid: message_type (ACK = 1) != INVALID (255) ✓,
    payload_length = 0 <= 4096 ✓, source_id != 0 ✓.
    Passes.

Step 4 — Check expiry (DeliveryEngine.cpp:169-174)
    timestamp_expired(env.expiry_time_us, now_us)
    An ACK envelope has expiry_time_us = 0 (set by envelope_make_ack at MessageEnvelope.hpp:90).
    timestamp_expired: (0 != 0ULL) is false → returns false.
    Message is NOT expired. Continue.

Step 5 — Check if control message (DeliveryEngine.cpp:177)
    envelope_is_control(env):
        (ACK || NAK || HEARTBEAT) — ACK is true.
    Condition is TRUE.

Step 6 — Check if specifically ACK (DeliveryEngine.cpp:178)
    if (env.message_type == MessageType::ACK) → true.

Step 7 — Call AckTracker::on_ack (DeliveryEngine.cpp:181-182)
    Result ack_res = m_ack_tracker.on_ack(env.source_id, env.message_id);
    (void)ack_res;   [result is noted but discarded]

Step 8 — Inside AckTracker::on_ack (AckTracker.cpp:81-110)
    Precondition: assert(m_count <= ACK_TRACKER_CAPACITY)   [ACK_TRACKER_CAPACITY = 32]
    Linear scan of m_slots[0..31]:
        for (uint32_t i = 0U; i < 32U; ++i)
            assert(i < 32)
            if (m_slots[i].state == EntryState::PENDING
                && m_slots[i].env.source_id == src     [src = remote peer who sent ACK]
                && m_slots[i].env.message_id == msg_id)
                    ← MATCH FOUND
                m_slots[i].state = EntryState::ACKED
                assert(m_slots[i].state == EntryState::ACKED)
                return Result::OK

    Note on source_id matching: The slot was populated by AckTracker::track() during send(),
    where env.source_id = m_local_id (the local node). The incoming ACK has source_id =
    the remote peer (the ACK sender). Therefore the comparison m_slots[i].env.source_id == src
    compares local_id vs peer_id. These will only match if src == local_id.
    [See Observation in section 14 — this is a potential matching bug.]
    [ASSUMPTION: the ACK's source_id carries the original data message_id and the comparison
    is designed to match on message_id alone in practice, or src == local_id in loopback tests.]

    If no match found:
        assert(m_count <= 32)
        return ERR_INVALID

Step 9 — Call RetryManager::on_ack (DeliveryEngine.cpp:184-185)
    Result retry_res = m_retry_manager.on_ack(env.source_id, env.message_id);
    (void)retry_res;   [result discarded]

Step 10 — Inside RetryManager::on_ack (RetryManager.cpp:97-131)
    Preconditions: assert(m_initialized), assert(src != NODE_ID_INVALID)
    Linear scan of m_slots[0..31]:
        for (uint32_t i = 0U; i < 32U; ++i)
            assert(i < 32)
            if (m_slots[i].active
                && m_slots[i].env.source_id == src
                && m_slots[i].env.message_id == msg_id)
                    ← MATCH FOUND
                m_slots[i].active = false
                --m_count
                assert(m_count <= 32)
                Logger::log(INFO, "RetryManager", "Cancelled retry for message_id=... from node=...")
                return Result::OK

    If no match:
        Logger::log(WARNING_LO, "RetryManager", "No retry entry found...")
        return ERR_INVALID

Step 11 — Log ACK receipt (DeliveryEngine.cpp:187-190)
    Logger::log(INFO, "DeliveryEngine", "Received ACK for message_id=... from src=...")

Step 12 — Return from control message handler (DeliveryEngine.cpp:192)
    return Result::OK;
    Note: ACK messages return OK to the caller. The caller may need to distinguish ACK from
    DATA, but the return code itself is OK. The envelope still carries message_type = ACK
    which the caller can inspect.

## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)
    m_transport->receive_message(env, timeout_ms)       [TcpBackend, returns OK with ACK env]
    assert(envelope_valid(env))                         [inline, passes]
    timestamp_expired(env.expiry_time_us=0, now_us)    [inline, returns false]
    envelope_is_control(env)                            [inline, returns true: ACK]
        [env.message_type == ACK: true]
        AckTracker::on_ack(env.source_id, env.message_id)
            assert(m_count <= 32)
            [loop i=0..31]
                assert(i < 32)
                [match: PENDING && src && msg_id]
                m_slots[i].state = ACKED
                assert(m_slots[i].state == ACKED)
                return OK
        (void)ack_res
        RetryManager::on_ack(env.source_id, env.message_id)
            assert(m_initialized)
            assert(src != NODE_ID_INVALID)
            [loop i=0..31]
                assert(i < 32)
                [match: active && src && msg_id]
                m_slots[i].active = false
                --m_count
                assert(m_count <= 32)
                Logger::log(INFO, "Cancelled retry...")
                return OK
        (void)retry_res
        Logger::log(INFO, "Received ACK for ...")
        return OK

## 5. Key Components Involved

DeliveryEngine (src/core/DeliveryEngine.cpp)
    Owns m_ack_tracker (AckTracker, embedded by value) and m_retry_manager (RetryManager,
    embedded by value). Orchestrates: transport receive → expiry → control dispatch → ACK resolution.

AckTracker (src/core/AckTracker.cpp)
    Tracks up to 32 outstanding messages awaiting ACK.
    Each slot: {MessageEnvelope env, uint64_t deadline_us, EntryState state}.
    EntryState: FREE (0) → PENDING → ACKED → FREE (via sweep_expired).
    on_ack performs PENDING → ACKED transition.

RetryManager (src/core/RetryManager.cpp)
    Tracks up to 32 messages scheduled for retry (ACK_TRACKER_CAPACITY = 32).
    Each slot: {MessageEnvelope env, bool active, uint32_t retry_count, uint32_t max_retries,
    uint32_t backoff_ms, uint64_t next_retry_us, uint64_t expiry_us}.
    on_ack sets active = false, decrements m_count — cancels retry.

MessageEnvelope (src/core/MessageEnvelope.hpp)
    envelope_make_ack: used by sender to construct ACK envelopes.
        Sets message_type = ACK, message_id = original.message_id, source_id = my_id,
        destination_id = original.source_id, expiry_time_us = 0.
    envelope_is_control: inline predicate checking for ACK/NAK/HEARTBEAT.

Types.hpp
    ACK_TRACKER_CAPACITY = 32, Result::ERR_INVALID = 4, MessageType::ACK = 1.

## 6. Branching Logic / Decision Points

Decision 1 (DeliveryEngine.cpp:160-163): transport returns OK?
    NO → return error; ACK processing never reached.

Decision 2 (DeliveryEngine.cpp:169-174): Is message expired?
    YES → return ERR_EXPIRED; ACK processing never reached.
    For ACK: expiry_time_us = 0, so timestamp_expired always returns false for ACKs.

Decision 3 (DeliveryEngine.cpp:177): Is message a control message?
    NO  → proceed to DATA handling (dedup, delivery).
    YES → enter control message handler.

Decision 4 (DeliveryEngine.cpp:178): Is message_type specifically ACK?
    YES → call on_ack on both AckTracker and RetryManager.
    NO  (NAK or HEARTBEAT) → skip ACK resolution; fall through to return OK.

Decision 5 (AckTracker.cpp:93-95): Does any PENDING slot match (src, msg_id)?
    YES → transition to ACKED, return OK.
    NO  → return ERR_INVALID (ACK for unknown or already-resolved message).
    Note: ERR_INVALID from on_ack is discarded by DeliveryEngine (via (void)ack_res).

Decision 6 (RetryManager.cpp:106-108): Does any active slot match (src, msg_id)?
    YES → deactivate slot, return OK.
    NO  → return ERR_INVALID, log WARNING_LO.
    Note: ERR_INVALID from retry on_ack is also discarded.

Decision 7 — Independent results:
    AckTracker::on_ack and RetryManager::on_ack are called unconditionally regardless of
    each other's result. A match in one but not the other is not an error at the DeliveryEngine
    level (both results are discarded with (void)). This handles the case where a message
    is RELIABLE_ACK (tracked by AckTracker but not RetryManager) or RELIABLE_RETRY (both).

## 7. Concurrency / Threading Behavior

No concurrency primitives in AckTracker or RetryManager. Both are assumed single-threaded.

DeliveryEngine::pump_retries() and sweep_ack_timeouts() also access m_retry_manager and
m_ack_tracker. If called concurrently with receive(), data races would occur on the slot
arrays. [ASSUMPTION: all DeliveryEngine methods are called from the same thread.]

AckTracker state (m_slots[], m_count): no atomics. Not thread-safe.
RetryManager state (m_slots[], m_count): no atomics. Not thread-safe.

## 8. Memory & Ownership Semantics (C/C++ Specific)

AckTracker::m_slots[32]:
    Array of 32 structs. Each struct contains a full MessageEnvelope copy (≈4128 bytes) plus
    deadline_us (8 bytes) and EntryState (1 byte + padding). Total ≈ 32 * ~4140 = ~132 KB.
    Embedded in AckTracker, which is embedded in DeliveryEngine.

RetryManager::m_slots[32]:
    Array of 32 structs. Each struct contains a MessageEnvelope copy plus active (bool),
    retry_count, max_retries, backoff_ms (uint32_t each), next_retry_us, expiry_us (uint64_t each).
    Total ≈ 32 * ~4160 = ~133 KB. Embedded in RetryManager, embedded in DeliveryEngine.

on_ack in AckTracker: only modifies m_slots[i].state. No memcpy. The envelope data in the
slot is retained (not cleared) until sweep_expired() sets state back to FREE.

on_ack in RetryManager: only sets m_slots[i].active = false and decrements m_count.
The envelope data in the slot is retained but the slot is logically free for reuse by schedule().

The incoming ACK envelope (env parameter to receive()) is read-only during ACK resolution.
Its source_id and message_id are used as lookup keys. No data is copied from it.

## 9. Error Handling Flow

ERR_INVALID from AckTracker::on_ack:
    Means: no PENDING slot found for (src, msg_id).
    Possible causes: ACK for a message that was never tracked (BEST_EFFORT message getting ACKed),
    ACK for a message already ACKED (duplicate ACK), ACK after the slot was swept as expired.
    Action: logged at assert level only. DeliveryEngine discards with (void). receive() still
    returns OK (the ACK was processed; it just had no effect).

ERR_INVALID from RetryManager::on_ack:
    Means: no active retry slot for (src, msg_id).
    Possible causes: message was RELIABLE_ACK (not RELIABLE_RETRY), so no retry was scheduled.
    Or: retry slot already expired/exhausted. Or: message was never sent with retry.
    Action: logged at WARNING_LO by RetryManager. DeliveryEngine discards with (void).

Both ERR_INVALID results are intentionally discarded. DeliveryEngine returns OK to its caller
regardless, because receiving an ACK (even for an unknown message) is not a failure condition.

Assertion failures (debug builds):
    assert(m_slots[i].state == EntryState::ACKED) after state assignment: sanity check.
    assert(m_count <= ACK_TRACKER_CAPACITY) throughout.
    assert(src != NODE_ID_INVALID) in RetryManager::on_ack: guards against zero source_id.

## 10. External Interactions

Transport layer (TcpBackend::receive_message):
    Called at the start. Delivers the ACK envelope from the wire.

Logger:
    RetryManager::on_ack logs INFO ("Cancelled retry...") on success.
    RetryManager::on_ack logs WARNING_LO ("No retry entry found...") on miss.
    DeliveryEngine logs INFO ("Received ACK for message_id=...") after both on_ack calls.

No socket I/O, no timer queries (timestamp_now_us not called), no DuplicateFilter interaction
on the ACK processing path.

sweep_ack_timeouts() (called separately by pump): also releases ACKED slots by transitioning
them from ACKED → FREE. This is not part of the receive() path.

## 11. State Changes / Side Effects

AckTracker state:
    m_slots[i].state: PENDING → ACKED. This is the primary state transition.
    m_count: unchanged by on_ack. Count is decremented only in sweep_expired() when the ACKED
    slot is collected and transitioned back to FREE.

RetryManager state:
    m_slots[i].active: true → false.
    m_count: decremented by 1.
    All other slot fields (retry_count, backoff_ms, etc.) retained but irrelevant (slot is dead).

Logger: one INFO log from RetryManager, one INFO log from DeliveryEngine.

No changes to DuplicateFilter, RingBuffer, or any transport-layer state.

## 12. Sequence Diagram (ASCII)

Caller       DeliveryEngine    TcpBackend    AckTracker      RetryManager     Logger
  |               |                |               |               |              |
  |--receive()-->|                 |               |               |              |
  |               |--recv_msg()-->|                |               |              |
  |               |<--OK,ACK env--|                |               |              |
  |               |                                |               |              |
  |               |--timestamp_expired(0) [false]  |               |              |
  |               |--envelope_is_control() [true]  |               |              |
  |               |  [message_type == ACK: true]   |               |              |
  |               |                                |               |              |
  |               |--on_ack(src, msg_id)---------->|               |              |
  |               |               [loop 0..31]     |               |              |
  |               |               [PENDING match]  |               |              |
  |               |               state = ACKED    |               |              |
  |               |<--OK---------------------------|               |              |
  |               | (void) ack_res                 |               |              |
  |               |                                |               |              |
  |               |--on_ack(src, msg_id)-------------------------->|              |
  |               |                                [loop 0..31]    |              |
  |               |                                [active match]  |              |
  |               |                                active = false  |              |
  |               |                                m_count--       |              |
  |               |                                |---log("Cancelled retry")---->|
  |               |<--OK-------------------------------------------|              |
  |               | (void) retry_res               |               |              |
  |               |                                                               |
  |               |--log("Received ACK for message_id=...")-------------------->|
  |               |                                                               |
  |<--OK----------|                                |               |              |

ACKED slot lifecycle (separate sweep path, not part of receive()):
  Caller         DeliveryEngine    AckTracker
    |                  |               |
    |--sweep_ack_---->|               |
    |  timeouts()      |               |
    |                  |--sweep_expired(now, buf, cap)->|
    |                  |               [loop 0..31]     |
    |                  |               [ACKED slot]     |
    |                  |               state = FREE     |
    |                  |               m_count--        |
    |                  |<--expired_count=0 (ACKED → FREE, not "expired")
    |<--0             |               |

## 13. Initialization vs Runtime Flow

Initialization (AckTracker::init, called from DeliveryEngine::init):
    memset(m_slots, 0, sizeof(m_slots)) — all states set to FREE (0).
    m_count = 0.
    Postcondition loop: asserts all slots are EntryState::FREE.

Initialization (RetryManager::init, called from DeliveryEngine::init):
    m_count = 0, m_initialized = true.
    Loop: sets all slots to active=false, zeroes all fields, calls envelope_init(env).

Send path (prior to this use case, establishing the PENDING state):
    DeliveryEngine::send() calls:
        AckTracker::track(env, deadline_us) → slot[i] transitions FREE → PENDING.
        RetryManager::schedule(env, max_retries, backoff_ms, now_us) → slot[j].active = true.

ACK resolution (this use case):
    AckTracker::on_ack → PENDING → ACKED.
    RetryManager::on_ack → active = false.

Slot cleanup (separate periodic call):
    DeliveryEngine::sweep_ack_timeouts() → AckTracker::sweep_expired():
        ACKED → FREE (slot reclaimed).
        PENDING with expired deadline → FREE (slot reclaimed, envelope reported as timeout).

## 14. Known Risks / Observations

Risk 1 — Source ID matching semantics in AckTracker::on_ack:
    In AckTracker::track() (called from DeliveryEngine::send()), the envelope stored in the slot
    has env.source_id = m_local_id (the local sender, assigned at DeliveryEngine.cpp:86).
    In AckTracker::on_ack(), the lookup key src = env.source_id where env is the INCOMING ACK.
    The incoming ACK has source_id = the remote peer (set by envelope_make_ack at
    MessageEnvelope.hpp:85: ack.source_id = my_id, where my_id is the ACK sender's local ID).
    Therefore: AckTracker slot has env.source_id = local_id; incoming ACK has source_id = peer_id.
    These are NOT the same node in a typical two-node setup.
    The comparison (m_slots[i].env.source_id == src) will fail in non-loopback scenarios.
    ONLY message_id matching would succeed. In practice, if all message_ids are globally unique,
    the source_id check is the discriminator that silently causes ERR_INVALID every time on_ack
    is called in a real two-node scenario. [This appears to be a latent bug.]
    RetryManager::on_ack has the same pattern and the same issue.

Risk 2 — Both on_ack results discarded:
    If on_ack fails (ERR_INVALID), the PENDING slot is never transitioned to ACKED and remains
    PENDING until sweep_expired() fires its deadline. This means the ACK timeout callback
    (sweep_ack_timeouts → WARNING_HI) will fire even for messages that were actually ACKed,
    if the source_id matching issue in Risk 1 is present.

Risk 3 — ACKED slots not immediately reclaimed:
    After on_ack sets state = ACKED, the slot is not freed until sweep_ack_timeouts() runs.
    If sweep is not called frequently enough and many messages are ACKed without sweep,
    the tracker can fill up with ACKED slots, causing track() to return ERR_FULL.

Risk 4 — RetryManager and AckTracker share ACK_TRACKER_CAPACITY = 32:
    Both have a fixed 32-slot table. Under RELIABLE_RETRY with high message rates, both can
    fill simultaneously. A full RetryManager means new messages cannot be scheduled for retry;
    a full AckTracker means new messages cannot be tracked. Both conditions produce ERR_FULL
    which is logged at WARNING_HI but does not fail the send.

## 15. Unknowns / Assumptions

[ASSUMPTION 1] The ACK envelope arriving at DeliveryEngine::receive() was created by
    envelope_make_ack() at the remote sender and correctly carries the original data
    message's message_id in its own message_id field (per MessageEnvelope.hpp:84).

[ASSUMPTION 2] DeliveryEngine::receive() and pump_retries()/sweep_ack_timeouts() are
    called from the same thread, so no concurrent access to AckTracker or RetryManager occurs.

[ASSUMPTION 3] AckTracker slot matching works correctly in production. Given Risk 1 above,
    this assumption requires that src in on_ack equals env.source_id in the stored slot.
    This would hold in loopback/self-test scenarios but likely fails in two-node deployments.

[UNKNOWN 1] Whether sweep_ack_timeouts() is called periodically from a maintenance loop.
    If not called, ACKED slots accumulate and the tracker will fill.

[UNKNOWN 2] The AckTracker and RetryManager do not share state directly. They are updated
    independently. It is not verified whether a message can be in AckTracker but not RetryManager
    or vice versa (e.g., for RELIABLE_ACK messages, no retry is scheduled, so RetryManager
    on_ack would always return ERR_INVALID, which is expected).

[UNKNOWN 3] Whether envelope_make_ack is the only mechanism for generating ACK messages.
    If a transport or impairment path generates synthetic ACKs, the source_id may differ.