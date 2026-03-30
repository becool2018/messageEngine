## 1. Use Case Overview

Use Case ID : UC-12
Title       : RetryManager cancels a retry entry upon receiving an ACK for
              the tracked message
Layer       : Core (DeliveryEngine / AckTracker / RetryManager)
Trigger     : The remote peer sends an ACK message. DeliveryEngine::receive()
              pulls the ACK from the transport, recognizes it as a control
              message, and dispatches it to both AckTracker and RetryManager.
Preconditions:
  - DeliveryEngine::init() has been called; m_initialized == true.
  - A RELIABLE_RETRY message was previously sent via DeliveryEngine::send().
  - AckTracker has a PENDING entry for (source_id, message_id).
  - RetryManager has an active RetryEntry for (source_id, message_id).
  - The transport delivers an inbound ACK envelope with:
      message_type == MessageType::ACK
      message_id == the original message_id
      source_id  == the local node (our original source_id stored in the entry)
Postconditions:
  - AckTracker entry for (source_id, message_id) transitions PENDING → ACKED.
  - RetryManager entry for (source_id, message_id) transitions active → false
    (entry effectively freed; retry scheduling ceases).
  - RetryManager::m_count is decremented by 1.
  - Both operations are logged at INFO severity.
  - receive() returns Result::OK to the caller (ACK is consumed as a control
    message; it is not delivered as data).
Requirement traceability:
  - CLAUDE.md §3.2 "ACK handling: Generating ACK/NAK messages … Tracking
    outstanding messages awaiting ACK"
  - CLAUDE.md §3.2 "Retry logic: retry until ACK or expiry"
  - CLAUDE.md §3.3 "Reliable with retry and dedupe"


## 2. Entry Points

Primary entry point (called by application receive loop):
  DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms,
                          uint64_t now_us)
    File : src/core/DeliveryEngine.cpp, line 147
    Sig  : Result DeliveryEngine::receive(MessageEnvelope&, uint32_t, uint64_t)

The receive() call triggers the secondary entry points:
  AckTracker::on_ack(NodeId src, uint64_t msg_id)
    File : src/core/AckTracker.cpp, line 81

  RetryManager::on_ack(NodeId src, uint64_t msg_id)
    File : src/core/RetryManager.cpp, line 97

Prerequisite entry points (must have been called earlier):
  DeliveryEngine::init()          [DeliveryEngine.cpp:15]
  AckTracker::init()              [AckTracker.cpp:21]
  RetryManager::init()            [RetryManager.cpp:16]
  DeliveryEngine::send()          [DeliveryEngine.cpp:75]
    → AckTracker::track()         [AckTracker.cpp:42]
    → RetryManager::schedule()    [RetryManager.cpp:44]


## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::receive, line 151]
  Assert m_initialized == true.
  Assert now_us > 0.

Step 2  [DeliveryEngine::receive, line 159]
  Call m_transport->receive_message(env, timeout_ms).
  [TcpBackend::receive_message() polls its receive queue. An ACK frame
  has been deserialized and queued from the underlying TCP socket.]
  Returns Result::OK with env populated with the ACK envelope.

Step 3  [DeliveryEngine::receive, lines 160-163]
  Check return value: res != OK → return res immediately.
  [In the ACK path, receive_message succeeded; execution continues.]

Step 4  [DeliveryEngine::receive, line 166]
  Assert envelope_valid(env).
  Check: message_type != INVALID, payload_length <= MSG_MAX_PAYLOAD_BYTES,
  source_id != NODE_ID_INVALID.

Step 5  [DeliveryEngine::receive, lines 169-174 — expiry check]
  Call timestamp_expired(env.expiry_time_us, now_us).
  ACK envelopes are built with expiry_time_us = 0 (per envelope_make_ack()
  in MessageEnvelope.hpp:90). [ASSUMPTION: timestamp_expired() returns
  false when expiry_time_us == 0, treating zero as "never expires".]
  If expired → log WARNING_LO, return ERR_EXPIRED. (Not taken for ACK.)

Step 6  [DeliveryEngine::receive, line 177]
  Call envelope_is_control(env).
  Returns true because env.message_type == MessageType::ACK.
  [envelope_is_control checks for ACK, NAK, or HEARTBEAT.]

Step 7  [DeliveryEngine::receive, line 178]
  Check env.message_type == MessageType::ACK → true.
  Execution enters the ACK-handling branch.

Step 8  [DeliveryEngine::receive, line 181 — AckTracker::on_ack]
  Call m_ack_tracker.on_ack(env.source_id, env.message_id).
  [Note: env.source_id is the LOCAL node's ID — the node that originally
  sent the data message. The ACK sender (remote peer) is stored in
  env.destination_id would be local; env.source_id in the received ACK
  envelope should be the original sender's ID as echoed by
  envelope_make_ack(). See Known Risks for the ID semantics discussion.]
  Execution transfers to AckTracker::on_ack().

Step 9  [AckTracker::on_ack, line 83]
  Assert m_count <= ACK_TRACKER_CAPACITY.

Step 10  [AckTracker::on_ack, lines 89-103 — search loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Assert i < ACK_TRACKER_CAPACITY per iteration.

  Per slot:
    If m_slots[i].state == PENDING
    AND m_slots[i].env.source_id == src
    AND m_slots[i].env.message_id == msg_id:
      Found the matching entry.

      Step 10a  [line 97]
        Set m_slots[i].state = EntryState::ACKED.
        [State machine transition: PENDING → ACKED]

      Step 10b  [line 100]
        Assert m_slots[i].state == EntryState::ACKED. (post-condition)

      Step 10c  [line 102]
        Return Result::OK.

  [If no matching PENDING entry is found — e.g., if the ACK arrives after
  sweep_expired() already freed it — the loop completes without a match.]

Step 11  [AckTracker::on_ack, lines 107-109]
  (Only if no match found)
  Assert m_count <= ACK_TRACKER_CAPACITY.
  Return Result::ERR_INVALID.

Step 12  [DeliveryEngine::receive, line 182]
  Store ack_res = result of m_ack_tracker.on_ack().
  Cast to void: `(void)ack_res;`
  [Return value is deliberately discarded — per comment: "Side effect;
  not critical to fail receive".]

Step 13  [DeliveryEngine::receive, line 184 — RetryManager::on_ack]
  Call m_retry_manager.on_ack(env.source_id, env.message_id).
  Execution transfers to RetryManager::on_ack().

Step 14  [RetryManager::on_ack, line 99]
  Assert m_initialized.
  Assert src != NODE_ID_INVALID.

Step 15  [RetryManager::on_ack, lines 103-121 — search loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Assert i < ACK_TRACKER_CAPACITY per iteration.

  Per slot:
    If m_slots[i].active == true
    AND m_slots[i].env.source_id == src
    AND m_slots[i].env.message_id == msg_id:
      Found the matching retry entry.

      Step 15a  [line 111]
        Set m_slots[i].active = false.
        [Entry is now logically freed; it will be skipped on all future
        collect_due() calls due to the `if (!m_slots[i].active) continue`
        guard.]

      Step 15b  [line 112]
        Decrement m_count.

      Step 15c  [line 115]
        Assert m_count <= ACK_TRACKER_CAPACITY. (post-condition)

      Step 15d  [lines 117-119]
        Log INFO "Cancelled retry for message_id=… from node=…"

      Step 15e  [line 121]
        Return Result::OK.

  [If no matching active entry is found:]

Step 16  [RetryManager::on_ack, lines 126-130]
  Log WARNING_LO "No retry entry found for message_id=… from node=…"
  Return Result::ERR_INVALID.

Step 17  [DeliveryEngine::receive, line 185]
  Store retry_res = result of m_retry_manager.on_ack().
  Cast to void: `(void)retry_res;`
  [Return value deliberately discarded — "Side effect".]

Step 18  [DeliveryEngine::receive, lines 187-189]
  Log INFO "Received ACK for message_id=… from src=…"

Step 19  [DeliveryEngine::receive, line 192]
  Return Result::OK.
  [The ACK is consumed. The caller's env still contains the ACK envelope
  fields but receive() returns OK, not a separate "was control" code.
  It is up to the caller to inspect env.message_type if needed.]


## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)
  [assert m_initialized, assert now_us > 0]
  m_transport->receive_message(env, timeout_ms)            [virtual dispatch]
    TcpBackend::receive_message(env, timeout_ms)
      m_recv_queue.pop(env)                               [ring buffer pop]
      [if empty: accept_clients() if server]
      [if empty: poll loop over client fds]
        recv_from_client(fd, 100ms)
          tcp_recv_frame(fd, wire_buf, cap, timeout, &len)
          Serializer::deserialize(wire_buf, len, env)
          m_recv_queue.push(env)
        m_recv_queue.pop(env)
        m_impairment.collect_deliverable(now_us, ...)
        m_recv_queue.pop(env)
      [returns OK with ACK envelope]
  [assert envelope_valid(env)]
  timestamp_expired(env.expiry_time_us, now_us)            [inline, Timestamp.hpp]
    → false for ACK (expiry_time_us == 0)
  envelope_is_control(env)                                 [inline, MessageEnvelope.hpp]
    → true (message_type == ACK)
  [branch: message_type == ACK]
  m_ack_tracker.on_ack(env.source_id, env.message_id)
    [assert m_count <= ACK_TRACKER_CAPACITY]
    [loop i=0..31]
      [if PENDING && source_id match && message_id match]
        m_slots[i].state = ACKED
        [assert state == ACKED]
        [return OK]
    [if not found: return ERR_INVALID]
  (void)ack_res
  m_retry_manager.on_ack(env.source_id, env.message_id)
    [assert m_initialized]
    [assert src != NODE_ID_INVALID]
    [loop i=0..31]
      [if active && source_id match && message_id match]
        m_slots[i].active = false
        m_count--
        [assert m_count <= ACK_TRACKER_CAPACITY]
        Logger::log(INFO, "RetryManager", "Cancelled retry …")
          snprintf / vsnprintf / fprintf(stderr)
        [return OK]
    [if not found]
      Logger::log(WARNING_LO, "RetryManager", "No retry entry found …")
      [return ERR_INVALID]
  (void)retry_res
  Logger::log(INFO, "DeliveryEngine", "Received ACK for message_id=…")
    snprintf / vsnprintf / fprintf(stderr)
  [return OK]


## 5. Key Components Involved

Component             File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine        src/core/DeliveryEngine.cpp/.hpp      Coordinator; detects
                                                            ACK control message;
                                                            dispatches to
                                                            AckTracker and
                                                            RetryManager; logs.

AckTracker            src/core/AckTracker.cpp/.hpp          Transitions PENDING
                                                            entry to ACKED state;
                                                            slot will be freed
                                                            on next
                                                            sweep_expired() call.

AckTracker::EntryState src/core/AckTracker.hpp:64-68        FSM: PENDING(1)
                                                            → ACKED(2) here;
                                                            ACKED → FREE on
                                                            next sweep.

RetryManager          src/core/RetryManager.cpp/.hpp        Sets active=false on
                                                            the matching
                                                            RetryEntry;
                                                            decrements m_count.

RetryEntry (struct)   src/core/RetryManager.hpp:88-96       Contains active bool,
                                                            env copy,
                                                            retry_count, etc.

TransportInterface /  src/platform/TcpBackend.cpp           Provides the inbound
TcpBackend                                                  ACK envelope via
                                                            receive_message().

MessageEnvelope       src/core/MessageEnvelope.hpp          Data type for ACK
                                                            message; built by
                                                            envelope_make_ack()
                                                            on the remote side.

Logger                src/core/Logger.hpp                   Logs INFO (successful
                                                            cancellation) and
                                                            WARNING_LO (if no
                                                            entry found).

Types.hpp             src/core/Types.hpp                    Constants and enums:
                                                            MessageType::ACK,
                                                            ACK_TRACKER_CAPACITY.


## 6. Branching Logic / Decision Points

Decision 1 — Did transport receive succeed?
  Location: DeliveryEngine::receive, line 160
  Condition: res != Result::OK
  True  → return res immediately (ERR_TIMEOUT if no message).
  False → continue; ACK envelope is in env.

Decision 2 — Has the ACK envelope expired?
  Location: DeliveryEngine::receive, line 169
  Condition: timestamp_expired(env.expiry_time_us, now_us)
  For ACK: expiry_time_us == 0 → assumed to mean "never expire".
  True  → log WARNING_LO, return ERR_EXPIRED. (Not the normal ACK path.)
  False → continue.

Decision 3 — Is this a control message?
  Location: DeliveryEngine::receive, line 177
  Condition: envelope_is_control(env)
             == (message_type == ACK || NAK || HEARTBEAT)
  True  → enter control handling branch.
  False → fall through to data handling (dedup, delivery).

Decision 4 — Is the control message specifically an ACK?
  Location: DeliveryEngine::receive, line 178
  Condition: env.message_type == MessageType::ACK
  True  → call on_ack() on both subsystems.
  False → (NAK or HEARTBEAT) pass through without ACK handling;
          return OK immediately.

Decision 5 — Did AckTracker find a matching PENDING entry?
  Location: AckTracker::on_ack, loop at line 89
  Match condition: state == PENDING && source_id == src && message_id == msg_id
  True  → state = ACKED; return OK.
  False (loop exhausted) → return ERR_INVALID.
  [DeliveryEngine discards this return value with (void)ack_res.]

Decision 6 — Did RetryManager find a matching active entry?
  Location: RetryManager::on_ack, loop at line 103
  Match condition: active == true && source_id == src && message_id == msg_id
  True  → active = false; m_count--; log INFO; return OK.
  False (loop exhausted) → log WARNING_LO; return ERR_INVALID.
  [DeliveryEngine discards this return value with (void)retry_res.]

Decision 7 — Order of operations between AckTracker and RetryManager
  AckTracker::on_ack() is always called before RetryManager::on_ack().
  Both are called unconditionally; the result of the first does not gate
  the second.


## 7. Concurrency / Threading Behavior

No synchronization primitives present in any of these components.

Concurrent access risks:

Race A — pump_retries() vs receive():
  - pump_retries() calls RetryManager::collect_due() which reads and
    writes m_slots[i].active, retry_count, backoff_ms, next_retry_us.
  - receive() calls RetryManager::on_ack() which writes m_slots[i].active
    and reads m_slots[i].env.source_id and message_id.
  - If these run on different threads: concurrent read-write on the same
    RetryEntry fields → data race → undefined behavior under C++17.

Race B — sweep_ack_timeouts() vs receive():
  - sweep_ack_timeouts() calls AckTracker::sweep_expired() which writes
    m_slots[i].state.
  - receive() calls AckTracker::on_ack() which writes m_slots[i].state.
  - If both run concurrently on the same slot: unsynchronized write-write
    → undefined behavior.

Logger concurrency:
  - Logger uses a stack-local buffer per call; no shared mutable state.
  - fprintf(stderr) is thread-safe on POSIX but may interleave lines.

[ASSUMPTION: the intended execution model is single-threaded. All three
functions — receive(), pump_retries(), and sweep_ack_timeouts() — must
be called from the same thread.]


## 8. Memory & Ownership Semantics (C/C++ Specific)

No heap allocation in this call path.

Stack frames:
  - DeliveryEngine::receive(): env (MessageEnvelope, ~4134 bytes),
    timeout_ms (uint32_t), now_us (uint64_t), res (Result), ack_res
    (Result), retry_res (Result).
    The env reference is caller-provided; the local references are small.

  - AckTracker::on_ack(): src (NodeId), msg_id (uint64_t), loop var i.
    No local buffers.

  - RetryManager::on_ack(): src (NodeId), msg_id (uint64_t), loop var i.
    No local buffers.

  - Logger::log(): buf[512] local stack buffer.

ACK envelope ownership:
  - The ACK envelope arrives in the caller's env argument.
  - AckTracker::on_ack() does NOT copy env; it only reads msg_id and src.
  - RetryManager::on_ack() does NOT copy env; it reads msg_id and src
    to match against stored RetryEntry::env fields.
  - The ACK envelope itself is never stored by either subsystem.

Stored data:
  - AckTracker::m_slots[i].env is the copy of the ORIGINAL DATA message.
    It is not modified by on_ack(); only the state field is changed.
  - RetryManager::m_slots[i].env is similarly a copy of the original data
    message. It is not modified by on_ack(); only active is changed to
    false.

State after on_ack():
  - AckTracker::m_slots[i].state == ACKED. The slot is still occupied;
    the env copy persists in memory until the next sweep_expired() call
    transitions it to FREE and potentially reads it (for ACKED entries,
    sweep_expired does NOT copy to the output buffer — it just frees).
  - RetryManager::m_slots[i].active == false. The slot is logically free.
    retry_count, backoff_ms, next_retry_us, and env remain in memory with
    their last values but are unreachable via normal code paths (all loops
    check `if (!m_slots[i].active) continue`). They will be overwritten
    when the slot is reused by a future schedule() call.


## 9. Error Handling Flow

Error source → handling action

Transport receive returns non-OK (e.g., ERR_TIMEOUT):
  - DeliveryEngine::receive() returns the error code immediately.
  - AckTracker::on_ack() and RetryManager::on_ack() are NOT called.
  - This is the normal "no message yet" path; no error logging occurs.

envelope_valid(env) assertion fails:
  - DeliveryEngine::receive() line 166: assert(envelope_valid(env)).
  - The assertion fires if the transport delivers an envelope with
    INVALID message_type, payload_length > MSG_MAX_PAYLOAD_BYTES, or
    source_id == 0.
  - Debug build: abort. Release build: continue with potentially invalid
    envelope.
  - [ASSUMPTION: the transport is trusted to provide valid envelopes;
    this is an internal consistency check, not an external validation.]

AckTracker::on_ack() returns ERR_INVALID (no matching entry):
  - Occurs if the ACK arrives after the entry was already freed by
    sweep_expired() (late ACK) or if the message was never tracked
    (BEST_EFFORT or RELIABLE_ACK with no retry entry).
  - DeliveryEngine discards with (void)ack_res.
  - No log line from DeliveryEngine for this case (AckTracker itself
    does not log on ERR_INVALID return in on_ack()).
  - Execution continues to RetryManager::on_ack().

RetryManager::on_ack() returns ERR_INVALID (no matching entry):
  - Occurs if the ACK arrives after the retry entry expired or was
    exhausted by collect_due(), or if the message was RELIABLE_ACK
    (not scheduled for retry).
  - Logged as WARNING_LO inside RetryManager::on_ack() line 126:
    "No retry entry found for message_id=… from node=…"
  - DeliveryEngine discards with (void)retry_res.
  - receive() still returns OK.

Both return ERR_INVALID simultaneously:
  - The ACK is consumed (receive() returns OK), but both subsystems had
    nothing to cancel. This is the "late ACK / orphan ACK" case.
  - Only RetryManager logs WARNING_LO; AckTracker is silent.
  - [OBSERVATION: DeliveryEngine does not log at WARNING for this
    compound failure; a diagnostic log at INFO would aid debugging.]

timestamp_expired on ACK (unlikely but possible):
  - If expiry_time_us != 0 on an ACK and the deadline has passed:
    receive() returns ERR_EXPIRED.
  - Neither on_ack() function is called.
  - RetryManager will continue to retry; AckTracker entry remains PENDING.
  - [ASSUMPTION: envelope_make_ack() always sets expiry_time_us = 0,
    making this case unreachable in practice.]


## 10. External Interactions

1. TransportInterface::receive_message() [virtual call → TcpBackend]
   - Polls the internal receive queue, then optionally accepts new TCP
     clients and reads from connected fds.
   - Deserializes the wire frame via Serializer::deserialize().
   - Returns the ACK envelope filled into the caller's env reference.

2. timestamp_expired() [called in DeliveryEngine::receive()]
   - Platform or inline utility function (Timestamp.hpp, not read).
   - Determines if the envelope's expiry_time_us is in the past.
   - For ACKs with expiry_time_us == 0: assumed to return false.

3. envelope_is_control() [inline, MessageEnvelope.hpp:69]
   - Pure function; no external I/O.
   - Returns true for ACK/NAK/HEARTBEAT.

4. Logger::log() / fprintf(stderr)
   - Called three times in the ACK path:
     a. RetryManager::on_ack(), INFO: "Cancelled retry …"  (success path)
        OR WARNING_LO: "No retry entry found …" (miss path)
     b. DeliveryEngine::receive(), INFO: "Received ACK for message_id=…"
   - Uses 512-byte stack buffer; writes to stderr.

5. No transport write operations
   - DeliveryEngine::receive() does not send anything as a result of
     receiving an ACK. [ASSUMPTION: ACKs are not themselves ACKed.
     They have reliability_class == BEST_EFFORT per envelope_make_ack().]


## 11. State Changes / Side Effects

Object               Field                Before        After
─────────────────────────────────────────────────────────────────────────────
AckTracker::Entry[i] state                PENDING       ACKED
                                          (for matching entry)
RetryManager::Entry[i] active             true          false
RetryManager         m_count              N             N - 1

AckTracker::Entry[i] state                —             FREE
                     (will change on next sweep_expired call)
AckTracker           m_count              M             M - 1
                     (will change on next sweep_expired call)

DeliveryEngine       (no member fields changed; env is caller's buffer)
stderr               log output           —             1-2 INFO lines +
                                                        optional WARNING_LO


## 12. Sequence Diagram (ASCII)

Remote Peer    TcpBackend       DeliveryEngine     AckTracker     RetryManager  Logger
     |              |                 |                 |               |          |
     |  [sends ACK] |                 |                 |               |          |
     |--tcp_frame-->|                 |                 |               |          |
     |              | deserialize     |                 |               |          |
     |              | recv_queue.push |                 |               |          |
     |              |                 |                 |               |          |
     |    [app calls receive()]       |                 |               |          |
     |              |<-- receive_message(env,to) -------|               |          |
     |              |  recv_queue.pop |                 |               |          |
     |              |---------------->|                 |               |          |
     |              |  returns OK,    |                 |               |          |
     |              |  env=ACK        |                 |               |          |
     |                               |                 |               |          |
     |                               | envelope_valid? |               |          |
     |                               | timestamp_expired? (no)         |          |
     |                               | envelope_is_control? (yes, ACK) |          |
     |                               |                 |               |          |
     |                               | on_ack(src, id) |               |          |
     |                               |---------------->|               |          |
     |                               |                 | [loop: match] |          |
     |                               |                 | state=ACKED   |          |
     |                               |                 | return OK     |          |
     |                               |<----------------|               |          |
     |                               | (void)ack_res   |               |          |
     |                               |                 |               |          |
     |                               | on_ack(src, id) |               |          |
     |                               |--------------------------------->|          |
     |                               |                 |  [loop: match]|          |
     |                               |                 |  active=false |          |
     |                               |                 |  m_count--    |          |
     |                               |                 |  log INFO     |--------->|
     |                               |                 |               | [WARN_HI]| (to stderr)
     |                               |                 |  return OK    |          |
     |                               |<--------------------------------|          |
     |                               | (void)retry_res |               |          |
     |                               |                 |               |          |
     |                               | Logger INFO     |               |          |
     |                               | "Received ACK…" |               |          |
     |                               |------------------------------------------------>|
     |                               |                 |               |  [INFO to stderr]
     |                               | return OK       |               |          |
     |     [app receives OK]         |                 |               |          |


## 13. Initialization vs Runtime Flow

Initialization path (called once at startup):

  DeliveryEngine::init()
    m_ack_tracker.init()     → m_slots[] zeroed; all state = FREE; m_count = 0
    m_retry_manager.init()   → m_slots[] zeroed; all active = false; m_count = 0

  DeliveryEngine::send() (earlier runtime call for RELIABLE_RETRY message):
    AckTracker::track(env, deadline_us)
      → Finds FREE slot; copies env; slot.state = PENDING; m_count++
    RetryManager::schedule(env, max_retries, backoff_ms, now_us)
      → Finds inactive slot; copies env; slot.active = true; m_count++

Runtime path — ACK receipt (documented above):
  DeliveryEngine::receive() → AckTracker::on_ack() + RetryManager::on_ack()

Post-ACK state:
  - AckTracker: slot is ACKED (not yet FREE; awaits next sweep_expired).
  - RetryManager: slot is inactive (active=false; effectively freed
    immediately from collect_due's perspective without waiting for a sweep).

This asymmetry is intentional:
  - RetryManager frees slots immediately on on_ack() (no separate GC phase).
  - AckTracker transitions to an intermediate ACKED state; sweep_expired()
    is the GC mechanism.


## 14. Known Risks / Observations

Risk 1 — Source ID semantics in ACK envelope
  envelope_make_ack() (MessageEnvelope.hpp:77) sets:
    ack.source_id      = my_id  (the peer who is sending the ACK)
    ack.destination_id = original.source_id (back to the original sender)
    ack.message_id     = original.message_id
  DeliveryEngine::receive() calls on_ack(env.source_id, env.message_id).
  But AckTracker and RetryManager search for entries where:
    slot.env.source_id == src (src = ACK.source_id = the REMOTE peer's id)
  The original tracked message stored slot.env.source_id = LOCAL node id.
  These would NOT match unless the ACK's source_id equals the original
  sender's source_id, which is only true if the remote peer echoes back
  the original sender's ID in ack.source_id rather than its own.
  [OBSERVATION: this is a potential identity mismatch. The code at
  receive() line 181 passes env.source_id from the ACK envelope, but the
  tracker stores the original message's source_id (the local node). For
  the match to work, the ACK's source_id must equal the local node's ID.
  This would be the case if the remote sends an ACK with source_id set
  to the original message's source_id — but envelope_make_ack() sets
  ack.source_id = my_id (the ACK sender). This deserves careful review.]

Risk 2 — Both return values silently discarded
  (void)ack_res and (void)retry_res mean failures in on_ack() are not
  propagated. If the ACK is for an unknown or already-cancelled message,
  the caller gets Result::OK with no indication that the ACK was redundant
  or unexpected.

Risk 3 — AckTracker slot not immediately freed on ACK
  After on_ack(), the AckTracker slot is ACKED, not FREE. The slot remains
  occupied until the next sweep_expired() call. If ACKs arrive faster than
  sweep_expired() is called, ACKED slots accumulate and may prevent new
  track() calls from finding free slots (ERR_FULL from track() in a future
  send). This is bounded by ACK_TRACKER_CAPACITY (32).

Risk 4 — RetryManager m_count decremented but slot not zeroed
  active is set to false in RetryManager::on_ack(), but the other fields
  (retry_count, backoff_ms, next_retry_us, env copy) retain their values.
  The slot will be overwritten by the next schedule() call. If inspection
  or logging of inactive slots is ever added, stale values could be
  misleading.

Risk 5 — No upper-layer notification of successful ACK
  receive() returns OK with the ACK envelope in env. The caller can
  inspect env.message_type == ACK and env.message_id to know which message
  was acknowledged. However, the API contract in DeliveryEngine.hpp (line 71)
  says "If ACK, calls ack_tracker.on_ack() and retry_manager.on_ack()" but
  does not specify that the ACK is returned to the caller as data.
  The caller receives OK and the env filled with ACK fields. This is usable
  but may surprise callers who only check the return code.

Risk 6 — NAK and HEARTBEAT are not handled
  DeliveryEngine::receive() dispatches to the ACK block only for
  MessageType::ACK. NAK and HEARTBEAT fall through to `return Result::OK`
  at line 192 without any specific handling. No NAK-triggered retry and
  no heartbeat response mechanism exists in this code.


## 15. Unknowns / Assumptions

[ASSUMPTION 1] Source ID matching is correct
  AckTracker and RetryManager search using src == slot.env.source_id.
  This works only if the src passed from DeliveryEngine::receive()
  (env.source_id from the ACK) equals the local node's ID that was
  stored when the original message was tracked. This depends on how the
  remote peer constructs the ACK's source_id field. See Risk 1.

[ASSUMPTION 2] timestamp_expired(0, now_us) returns false
  ACK envelopes have expiry_time_us == 0. The behavior of timestamp_expired
  with a zero expiry was not verified (Timestamp.hpp was not read). Assumed
  to treat zero as "no expiry."

[ASSUMPTION 3] Serializer::deserialize is deterministic and allocation-free
  Called inside TcpBackend::recv_from_client(). Source not read. Assumed
  consistent with Power of 10.

[ASSUMPTION 4] envelope_is_control returns true only for ACK/NAK/HEARTBEAT
  Confirmed by reading MessageEnvelope.hpp:69-74. No assumption needed here.

[ASSUMPTION 5] The ACK is not itself ACKed
  ACKs have reliability_class == BEST_EFFORT (per envelope_make_ack()).
  receive() only schedules retry/ack tracking for RELIABLE_ACK and
  RELIABLE_RETRY messages. ACKs therefore do not generate new tracking
  entries. Confirmed by the `envelope_is_control()` early return at line 192
  which exits before any dedup or reliability handling.

[ASSUMPTION 6] Single-threaded call model
  No mutexes protect AckTracker or RetryManager state. receive(),
  pump_retries(), and sweep_ack_timeouts() must be called from the same
  thread.

[ASSUMPTION 7] RetryManager entry for RELIABLE_RETRY always coexists with
  AckTracker entry
  DeliveryEngine::send() creates both entries for RELIABLE_RETRY messages.
  It is possible (edge case) for AckTracker::track() to succeed but
  RetryManager::schedule() to fail (ERR_FULL). In that case, on_ack()
  would succeed in AckTracker but find no entry in RetryManager
  (returning ERR_INVALID, logged as WARNING_LO).
```

---

Three Flow-of-Control documents have been produced, each covering all 15 required sections. Here is a summary of what was traced:

**UC-10 (RetryManager fires retry):** Traced `pump_retries()` → `collect_due()` inner loop with its three-guard sequence (expiry check, exhaustion check, due check), the exponential backoff doubling capped at 60,000 ms, `next_retry_us` rescheduling, and the resend path through `send_via_transport()` → `TcpBackend::send_message()` including impairment engine interaction.

**UC-11 (AckTracker timeout sweep):** Traced `sweep_ack_timeouts()` → `sweep_expired()` three-branch FSM sweep (PENDING/expired → FREE with copy to output buffer, ACKED → FREE silently, FREE → no action), the WARNING_HI logging loop in DeliveryEngine, and the side-effect ACKED garbage collection. Key risk identified: ACKED entries silently freed without contributing to the returned expired count.

**UC-12 (Retry cancelled on ACK):** Traced `receive()` → expiry check → `envelope_is_control()` → ACK branch → `AckTracker::on_ack()` (PENDING → ACKED state transition) → `RetryManager::on_ack()` (active → false, m_count decremented) → both return values discarded with `(void)`. Key risk identified at Risk 1: a potential source_id identity mismatch where the ACK envelope's `source_id` (set to the remote peer's ID by `envelope_make_ack`) may not match the stored `source_id` (the local node's ID) in the tracker/retry slots.agentId: a8cc36c2b5949f283 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 67266
tool_uses: 10
duration_ms: 461170</usage>