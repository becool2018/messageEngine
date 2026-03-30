# UC_12 — Retry cancelled on ACK receipt

**HL Group:** HL-10 — User pumps the retry loop (termination path)
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.2.5, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

The remote peer sends an ACK message acknowledging receipt of a DATA message that was
previously sent with reliability_class == RELIABLE_RETRY. The ACK arrives on the TCP
socket, is deserialized by TcpBackend, and is queued in the receive ring buffer.
The application calls DeliveryEngine::receive(), which pulls the ACK envelope from the
transport, recognizes it as a control message (envelope_is_control → true), and
dispatches to both AckTracker::on_ack() and RetryManager::on_ack() in sequence.

### Expected outcome (single goal)

The AckTracker slot for the acknowledged message transitions from PENDING to ACKED.
The RetryManager slot is immediately deactivated (active = false, m_count decremented).
Both events are logged. DeliveryEngine::receive() returns Result::OK. Future calls to
pump_retries() will skip the cancelled RetryManager slot, ending the retry cycle for
this message.

---

## 2. Entry Points

Primary entry point (called by application receive loop):

    DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, line 149
    Sig:  Result DeliveryEngine::receive(MessageEnvelope&, uint32_t, uint64_t)

The receive() call triggers the secondary entry points:

    AckTracker::on_ack(NodeId src, uint64_t msg_id)
      [AckTracker.cpp:80]
    RetryManager::on_ack(NodeId src, uint64_t msg_id)
      [RetryManager.cpp:123]

Prerequisite entry points (must have been called earlier):

    DeliveryEngine::init()           [DeliveryEngine.cpp:17]
    AckTracker::init()               [AckTracker.cpp:23]
    RetryManager::init()             [RetryManager.cpp:45]
    DeliveryEngine::send()           [DeliveryEngine.cpp:77]
      → AckTracker::track()          [AckTracker.cpp:44]
      → RetryManager::schedule()     [RetryManager.cpp:72]

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::receive, line 153]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL).

Step 2  [DeliveryEngine::receive, line 161]
  Call m_transport->receive_message(env, timeout_ms).
  Virtual dispatch to TcpBackend::receive_message().
  TcpBackend pops from its internal receive queue (or polls the TCP socket if the
  queue is empty). The deserialized ACK envelope is returned in env.
  Returns Result::OK.
  env.message_type      = MessageType::ACK
  env.message_id        = <original data message's message_id>
  env.source_id         = <ACK sender's NodeId (the remote peer's own ID)>
  env.destination_id    = <original sender's NodeId (local node)>
  env.expiry_time_us    = 0  (ACKs never expire; per envelope_make_ack() line 101)
  env.reliability_class = BEST_EFFORT

Step 3  [DeliveryEngine::receive, lines 162-164]
  Check res != OK → false (OK was returned); execution continues.

Step 4  [DeliveryEngine::receive, line 168]
  NEVER_COMPILED_OUT_ASSERT(envelope_valid(env)).
  Checks: message_type != INVALID, payload_length <= 4096, source_id != 0.

Step 5  [DeliveryEngine::receive, lines 171-175 — expiry check]
  Call timestamp_expired(env.expiry_time_us, now_us).
  [Timestamp.hpp:51: returns (expiry_us != 0ULL) && (now_us >= expiry_us).]
  env.expiry_time_us == 0 → returns false (never expires).
  Condition false; execution continues.

Step 6  [DeliveryEngine::receive, line 179]
  Call envelope_is_control(env).
  [MessageEnvelope.hpp:79: returns true for ACK, NAK, or HEARTBEAT.]
  env.message_type == ACK → returns true.
  Execution enters the control-message handling branch.

Step 7  [DeliveryEngine::receive, line 180]
  Check env.message_type == MessageType::ACK → true.
  Execution enters the ACK-specific handling sub-branch.

Step 8  [DeliveryEngine::receive, line 183 — AckTracker::on_ack]
  Call m_ack_tracker.on_ack(env.source_id, env.message_id).
  [env.source_id here is the ACK sender's own ID (remote peer's ID), as set by
  envelope_make_ack() at MessageEnvelope.hpp:97: ack.source_id = my_id.
  The AckTracker slot has slot.env.source_id = m_local_id (the original sender,
  i.e., the local node). These will differ in a two-node scenario.
  See Known Risks.]
  Execution transfers to AckTracker::on_ack().

Step 9  [AckTracker::on_ack, line 83]
  NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).

Step 10  [AckTracker::on_ack, lines 88-100 — search loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Per slot, check:
    m_slots[i].state           == EntryState::PENDING
    AND m_slots[i].env.source_id == src
    AND m_slots[i].env.message_id == msg_id.

  Match found (Step 10a):
    Set m_slots[i].state = EntryState::ACKED.  [PENDING → ACKED transition]
    NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == EntryState::ACKED).
    Return Result::OK.

  No match found (Step 10b) — source_id mismatch or late ACK:
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).
    Return Result::ERR_INVALID.
    [Most likely cause: source_id mismatch bug (see Known Risks).]

Step 11  [DeliveryEngine::receive, line 184]
  Store ack_res; cast to void: `(void)ack_res;`
  [Comment: "Side effect; not critical to fail receive."]
  Failure is silently ignored; execution always continues to RetryManager::on_ack().

Step 12  [DeliveryEngine::receive, line 186 — RetryManager::on_ack]
  Call m_retry_manager.on_ack(env.source_id, env.message_id).
  Execution transfers to RetryManager::on_ack().

Step 13  [RetryManager::on_ack, lines 125-126]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID).

Step 14  [RetryManager::on_ack, lines 129-147 — search loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Per slot, check:
    m_slots[i].active              == true
    AND m_slots[i].env.source_id   == src
    AND m_slots[i].env.message_id  == msg_id.

  Match found (Step 14a):
    Set m_slots[i].active = false.
    [Slot is now logically freed; collect_due() will skip it on all future calls
    due to the `if (!m_slots[i].active) continue` guard at RetryManager.cpp:172.]
    Decrement m_count.
    NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).
    Logger::log(INFO, "RetryManager",
                "Cancelled retry for message_id=%llu from node=%u", ...)
    Return Result::OK.

  No match found (Step 14b) — source_id mismatch or entry already expired:
    Logger::log(WARNING_LO, "RetryManager",
                "No retry entry found for message_id=%llu from node=%u", ...)
    Return Result::ERR_INVALID.

Step 15  [DeliveryEngine::receive, line 187]
  Store retry_res; cast to void: `(void)retry_res;`
  [Comment: "Side effect."]
  Failure is silently ignored.

Step 16  [DeliveryEngine::receive, lines 189-191]
  Logger::log(INFO, "DeliveryEngine",
              "Received ACK for message_id=%llu from src=%u", ...)

Step 17  [DeliveryEngine::receive, line 194]
  Return Result::OK.
  [The ACK is consumed. The caller's env still contains the ACK envelope.
  The caller can inspect env.message_type == ACK and env.message_id to determine
  which message was acknowledged. receive() returns OK, not a special "ACK seen" code.]

---

## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)               [DeliveryEngine.cpp:149]
  [NEVER_COMPILED_OUT_ASSERT m_initialized, now_us > 0]
  m_transport->receive_message(env, timeout_ms)                [virtual dispatch, line 161]
    TcpBackend::receive_message(env, timeout_ms)               [TcpBackend.cpp:386]
      m_recv_queue.pop(env)                                    [ring buffer pop]
      [if empty: poll loop over client fds]
        poll_clients_once(100U)
          recv_from_client(fd, timeout_ms)
            tcp_recv_frame(fd, wire_buf, cap, timeout, &len)
            Serializer::deserialize(wire_buf, len, env)        [deserializes ACK]
            m_recv_queue.push(env)
          m_recv_queue.pop(env)
        flush_delayed_to_queue(now_us)
      [returns OK with ACK envelope in env]
  [NEVER_COMPILED_OUT_ASSERT envelope_valid(env)]
  timestamp_expired(env.expiry_time_us=0, now_us)              [inline, Timestamp.hpp:51]
    → false (expiry_us == 0 means never-expire)
  envelope_is_control(env)                                     [inline, MessageEnvelope.hpp:79]
    → true (message_type == ACK)
  [branch: message_type == ACK]
  m_ack_tracker.on_ack(env.source_id, env.message_id)          [AckTracker.cpp:80]
    [NEVER_COMPILED_OUT_ASSERT m_count <= 32]
    [loop i=0..31]
      [if PENDING && source_id match && message_id match]
        m_slots[i].state = ACKED
        [NEVER_COMPILED_OUT_ASSERT state == ACKED]
        return OK
    [if not found: return ERR_INVALID]
  (void)ack_res
  m_retry_manager.on_ack(env.source_id, env.message_id)        [RetryManager.cpp:123]
    [NEVER_COMPILED_OUT_ASSERT m_initialized]
    [NEVER_COMPILED_OUT_ASSERT src != NODE_ID_INVALID]
    [loop i=0..31]
      [if active && source_id match && message_id match]
        m_slots[i].active = false
        m_count--
        [NEVER_COMPILED_OUT_ASSERT m_count <= 32]
        Logger::log(INFO, "RetryManager", "Cancelled retry …")
          snprintf / vsnprintf / fprintf(stderr)
        return OK
    [if not found]
      Logger::log(WARNING_LO, "RetryManager", "No retry entry found …")
      return ERR_INVALID
  (void)retry_res
  Logger::log(INFO, "DeliveryEngine", "Received ACK for message_id=…")
    snprintf / vsnprintf / fprintf(stderr)
  return OK

---

## 5. Key Components Involved

Component              File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine         src/core/DeliveryEngine.cpp/.hpp      Coordinator: detects
                                                             the ACK control
                                                             message; dispatches
                                                             to AckTracker and
                                                             RetryManager; logs
                                                             the event.

AckTracker             src/core/AckTracker.cpp/.hpp          Transitions PENDING
                                                             entry to ACKED state.
                                                             Slot will be freed
                                                             on the next
                                                             sweep_expired() call
                                                             via sweep_one_slot().

AckTracker::EntryState src/core/AckTracker.hpp:72            FSM: PENDING(1) →
                                                             ACKED(2) here;
                                                             ACKED → FREE on
                                                             next sweep.

RetryManager           src/core/RetryManager.cpp/.hpp        Sets active=false on
                                                             the matching
                                                             RetryEntry; decrements
                                                             m_count. Slot is
                                                             immediately available
                                                             for reuse.

RetryEntry (struct)    src/core/RetryManager.hpp:93          Contains active bool;
                                                             after on_ack,
                                                             active=false stops
                                                             collect_due() from
                                                             ever retrying.

TcpBackend             src/platform/TcpBackend.cpp/.hpp      Delivers the inbound
                                                             ACK envelope via
                                                             receive_message().

envelope_make_ack()    src/core/MessageEnvelope.hpp:88       Inline helper used by
                                                             the remote peer to
                                                             build the ACK:
                                                             sets source_id = my_id
                                                             (peer's own ID).

Logger                 src/core/Logger.hpp                   INFO on successful
                                                             cancellation;
                                                             WARNING_LO if no entry
                                                             found.

Types.hpp              src/core/Types.hpp                    MessageType::ACK (1),
                                                             ACK_TRACKER_CAPACITY
                                                             (32),
                                                             NODE_ID_INVALID (0).

---

## 6. Branching Logic / Decision Points

Decision 1 — Did transport receive succeed?
  Location: DeliveryEngine::receive, line 162
  Condition: res != Result::OK
  True  → return res immediately (ERR_TIMEOUT etc.); ACK handling skipped.
  False → continue; ACK envelope is in env.

Decision 2 — Has the ACK envelope expired?
  Location: DeliveryEngine::receive, line 171
  Condition: timestamp_expired(env.expiry_time_us, now_us)
  For ACKs: expiry_time_us == 0 → returns false (never expires per Timestamp.hpp:58).
  True  → log WARNING_LO, return ERR_EXPIRED (not the normal ACK path).
  False → continue.

Decision 3 — Is this a control message?
  Location: DeliveryEngine::receive, line 179
  Condition: envelope_is_control(env)
             = (message_type == ACK || NAK || HEARTBEAT)
  True  → enter control handling branch.
  False → fall through to data handling (dedup, delivery).

Decision 4 — Is the control message specifically an ACK?
  Location: DeliveryEngine::receive, line 180
  Condition: env.message_type == MessageType::ACK
  True  → call on_ack() on both subsystems.
  False → (NAK or HEARTBEAT) pass through without ACK handling; return OK.
  [NAK and HEARTBEAT are not specifically handled in this code path.]

Decision 5 — Did AckTracker find a matching PENDING entry?
  Location: AckTracker::on_ack, loop at line 88
  Match condition: state == PENDING && source_id == src && message_id == msg_id
  True  → state = ACKED; return OK.
  False (loop exhausted) → return ERR_INVALID.
  [DeliveryEngine discards this return value with (void)ack_res.
  Most likely failure cause: source_id mismatch; see Known Risks.]

Decision 6 — Did RetryManager find a matching active entry?
  Location: RetryManager::on_ack, loop at line 129
  Match condition: active == true && source_id == src && message_id == msg_id
  True  → active = false; m_count--; log INFO; return OK.
  False (loop exhausted) → log WARNING_LO; return ERR_INVALID.
  [DeliveryEngine discards this return value with (void)retry_res.
  Most likely failure cause: source_id mismatch; see Known Risks.]

Decision 7 — Order of operations
  AckTracker::on_ack() is always called before RetryManager::on_ack().
  Both are called unconditionally; the result of the first does not gate the second.

---

## 7. Concurrency / Threading Behavior

No synchronization primitives exist in AckTracker, RetryManager, or DeliveryEngine.

Race A — pump_retries() vs receive():
  pump_retries() calls RetryManager::collect_due() which reads and writes
  m_slots[i].active, retry_count, backoff_ms, next_retry_us.
  receive() calls RetryManager::on_ack() which writes m_slots[i].active and reads
  m_slots[i].env.source_id and message_id.
  If these run on different threads: concurrent read-write on the same RetryEntry
  fields → data race → undefined behavior under C++17.

Race B — sweep_ack_timeouts() vs receive():
  sweep_ack_timeouts() → AckTracker::sweep_one_slot() writes m_slots[i].state.
  receive() → AckTracker::on_ack() writes m_slots[i].state.
  If both run concurrently on the same slot: unsynchronized write-write →
  undefined behavior.

Logger concurrency:
  Logger uses a stack-local buffer per call; no shared mutable state.
  fprintf(stderr) is thread-safe on POSIX but may interleave lines.

[ASSUMPTION: the intended execution model is single-threaded. All three functions —
receive(), pump_retries(), and sweep_ack_timeouts() — must be called from the same thread.]

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

No heap allocation on this call path.

Stack frames:
  DeliveryEngine::receive():
    env: MessageEnvelope& (reference; ~4135 bytes in caller's storage).
    timeout_ms: uint32_t (4 bytes).
    now_us: uint64_t (8 bytes).
    res, ack_res, retry_res: Result enum (uint8_t each).

  AckTracker::on_ack():
    src: NodeId/uint32_t.
    msg_id: uint64_t.
    Loop variable i: uint32_t.
    No local buffers.

  RetryManager::on_ack():
    src: NodeId/uint32_t.
    msg_id: uint64_t.
    Loop variable i: uint32_t.
    No local buffers.

  Logger::log():
    buf[512]: 512-byte stack buffer for formatted output.

ACK envelope ownership:
  The ACK envelope arrives in the caller's env argument (passed by reference).
  AckTracker::on_ack() does NOT copy env; it reads msg_id and src only.
  RetryManager::on_ack() does NOT copy env; it reads msg_id and src only.
  Neither subsystem stores the ACK envelope.

Stored data after on_ack():
  AckTracker::m_slots[i].env: the deep copy of the original DATA message. Not
    modified by on_ack(); only the state field is changed PENDING → ACKED.
  RetryManager::m_slots[j].env: the deep copy of the original DATA message. Not
    modified by on_ack(); only active is changed to false.
  Both copies persist in memory until cleaned up by sweep_expired() (AckTracker)
  or until reused by a future schedule() call (RetryManager).

State asymmetry:
  RetryManager frees slots immediately on on_ack() (active=false; logically free
  from collect_due's perspective at line 172 immediately after this call).
  AckTracker transitions to an intermediate ACKED state; sweep_one_slot() (called
  by sweep_expired()) is the GC mechanism that finally transitions to FREE.
  This asymmetry is intentional; see Section 13.

---

## 9. Error Handling Flow

Transport receive returns non-OK (e.g., ERR_TIMEOUT):
  DeliveryEngine::receive() returns the error code immediately.
  AckTracker::on_ack() and RetryManager::on_ack() are NOT called.
  This is the normal "no message yet" path.

NEVER_COMPILED_OUT_ASSERT(envelope_valid(env)) fails:
  DeliveryEngine::receive() line 168: fires if transport delivers an envelope with
  INVALID message_type, payload_length > MSG_MAX_PAYLOAD_BYTES, or source_id == 0.
  Always active; aborts process in all builds.

AckTracker::on_ack() returns ERR_INVALID (no matching entry):
  Occurs if the ACK arrives after the entry was already freed by sweep_expired()
  (late ACK), or if source_id mismatch prevents the comparison from succeeding
  (see Known Risks Risk 1).
  AckTracker emits no log on ERR_INVALID.
  DeliveryEngine discards with (void)ack_res; execution always continues to
  RetryManager::on_ack().

RetryManager::on_ack() returns ERR_INVALID (no matching entry):
  Occurs if the entry expired or was exhausted by collect_due(), or if the message
  was RELIABLE_ACK (not scheduled for retry), or if source_id mismatch.
  Logged as WARNING_LO inside RetryManager::on_ack().
  DeliveryEngine discards with (void)retry_res; receive() still returns OK.

Both return ERR_INVALID simultaneously:
  The ACK is consumed (receive() returns OK), but both subsystems had nothing to
  cancel. Only RetryManager logs WARNING_LO; AckTracker is silent.

ACK with non-zero expiry_time_us (unexpected):
  If expiry_time_us != 0 on an ACK and the deadline has passed, receive() returns
  ERR_EXPIRED without calling either on_ack() function. RetryManager continues to
  retry; AckTracker entry remains PENDING.
  [ASSUMPTION: envelope_make_ack() always sets expiry_time_us = 0; this case is
  unreachable in practice.]

---

## 10. External Interactions

1. TransportInterface::receive_message() [virtual call → TcpBackend]
   Polls the internal receive queue via m_recv_queue.pop().
   On queue empty: enter poll loop (up to 50 iterations of 100ms each), calling
   poll_clients_once() and flush_delayed_to_queue() per iteration.
   Deserializes the wire frame via Serializer::deserialize().
   Returns the ACK envelope in the caller's env reference.

2. timestamp_expired() [inline, Timestamp.hpp:51]
   Pure function; returns false for ACKs (expiry_time_us == 0).

3. envelope_is_control() [inline, MessageEnvelope.hpp:79]
   Pure function; returns true for ACK/NAK/HEARTBEAT. No I/O.

4. Logger::log() / fprintf(stderr)
   Called in the ACK path:
   a. RetryManager::on_ack() — INFO: "Cancelled retry …" (success path)
      OR WARNING_LO: "No retry entry found …" (miss path)
   b. DeliveryEngine::receive() — INFO: "Received ACK for message_id=…"
   Uses 512-byte stack buffer; writes to stderr.

5. No transport write operations
   DeliveryEngine::receive() does not send anything as a result of receiving an ACK.
   ACKs have reliability_class == BEST_EFFORT (per envelope_make_ack() line 100)
   and are not themselves ACKed or retried.

---

## 11. State Changes / Side Effects

Object               Field                Before        After
─────────────────────────────────────────────────────────────────────────────
AckTracker::Entry[i] state                PENDING       ACKED
RetryManager::Entry[j] active             true          false
RetryManager         m_count              N             N - 1

Deferred state changes (after next sweep_expired call):
AckTracker::Entry[i] state                ACKED         FREE
AckTracker           m_count              M             M - 1

DeliveryEngine       (no member fields changed; env is caller's buffer)
stderr               log output           —             1-2 INFO lines +
                                                        optional WARNING_LO

---

## 12. Sequence Diagram (ASCII)

```
Remote Peer    TcpBackend       DeliveryEngine     AckTracker     RetryManager  Logger
     |              |                 |                 |               |          |
     | [sends ACK]  |                 |                 |               |          |
     |--tcp_frame-->|                 |                 |               |          |
     |              | deserialize     |                 |               |          |
     |              | recv_queue.push |                 |               |          |
     |              |                 |                 |               |          |
     |    [app calls receive()]       |                 |               |          |
     |              |<-- recv_msg(env,to) --------------|               |          |
     |              | recv_queue.pop  |                 |               |          |
     |              |---------------->|                 |               |          |
     |              | returns OK,     |                 |               |          |
     |              | env=ACK         |                 |               |          |
     |                               |                 |               |          |
     |                               | NCOA(valid)     |               |          |
     |                               | timestamp_exp?->false           |          |
     |                               | is_control? -> true (ACK)       |          |
     |                               | type==ACK? -> true              |          |
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
     |                               |                 | [loop: match] |          |
     |                               |                 | active=false  |          |
     |                               |                 | m_count--     |          |
     |                               |                 | log INFO      |--------->|
     |                               |                 | return OK     | [INFO]\n |
     |                               |<--------------------------------|          |
     |                               | (void)retry_res |               |          |
     |                               |                 |               |          |
     |                               | Logger INFO     |               |          |
     |                               | "Received ACK…" |               |          |
     |                               |------------------------------------------------>|
     |                               | return OK       |               | [INFO]\n |
     |     [app receives OK]         |                 |               |          |
```

NCOA = NEVER_COMPILED_OUT_ASSERT

---

## 13. Initialization vs Runtime Flow

Initialization path (called once at startup):

  DeliveryEngine::init()
    m_ack_tracker.init()    → m_slots[] zeroed; all state = FREE; m_count = 0
    m_retry_manager.init()  → m_slots[] zeroed; all active = false; m_count = 0

Runtime setup — earlier send for RELIABLE_RETRY message (precondition for this UC):

  DeliveryEngine::send()
    AckTracker::track(env, deadline_us)
      → Finds FREE slot; copies env; slot.state = PENDING; m_count++
    RetryManager::schedule(env, max_retries, backoff_ms, now_us)
      → Finds inactive slot; copies env; slot.active = true; m_count++
      → slot.next_retry_us = now_us (first retry fires immediately)

  [Zero or more calls to pump_retries() may have fired before the ACK arrives,
  incrementing retry_count and rescheduling next_retry_us each time.]

Runtime — ACK receipt (this use case):
  DeliveryEngine::receive() → AckTracker::on_ack() + RetryManager::on_ack()
  AckTracker: slot transitions PENDING → ACKED.
  RetryManager: active = false; m_count decremented.

Post-ACK state asymmetry (intentional design):
  RetryManager: slot is immediately logically free (active=false). Future
    collect_due() calls skip it at line 172. No GC phase needed.
  AckTracker: slot is ACKED, not FREE. The slot remains occupied until the next
    sweep_expired() call, which transitions it to FREE via sweep_one_slot().
    If ACKs arrive faster than sweep_expired() is called, ACKED slots accumulate
    and may prevent new track() calls from finding free slots (see Known Risks).

---

## 14. Known Risks / Observations

Risk 1 — Source ID semantics mismatch (latent identity bug)
  envelope_make_ack() (MessageEnvelope.hpp:88) sets:
    ack.source_id      = my_id  (the peer who is sending the ACK — the REMOTE peer)
    ack.destination_id = original.source_id (back to the original sender — LOCAL node)
    ack.message_id     = original.message_id
  DeliveryEngine::receive() calls on_ack(env.source_id, env.message_id).
  AckTracker and RetryManager search for entries where:
    slot.env.source_id == src
  The original tracked message stored slot.env.source_id = LOCAL node ID
  (assigned at DeliveryEngine::send() line 88: env.source_id = m_local_id).
  In a two-node scenario: src passed to on_ack is the REMOTE peer's ID,
  but the stored slot.env.source_id is the LOCAL node's ID.
  These will NOT match. Both on_ack() functions will return ERR_INVALID.
  The retry entry will never be cancelled via this path; the retry will run
  until max_retries is exhausted or expiry_us passes.
  [RISK: this is a latent identity mismatch bug that renders the ACK-cancellation
  mechanism ineffective in a standard two-node deployment. Requires careful review
  and likely a fix in how on_ack() matches entries.]

Risk 2 — Both return values silently discarded
  (void)ack_res and (void)retry_res mean on_ack() failures are not propagated.
  If the ACK is for an unknown or already-cancelled message, the caller receives
  Result::OK with no indication the ACK was redundant or unexpected.

Risk 3 — AckTracker slot not immediately freed on ACK
  After on_ack(), the AckTracker slot is ACKED, not FREE. The slot remains
  occupied until the next sweep_expired() call. If ACKs arrive faster than
  sweep_expired() is called, ACKED slots accumulate and may block new track()
  calls (ERR_FULL). Bounded by ACK_TRACKER_CAPACITY (32).

Risk 4 — RetryManager m_count decremented but slot not zeroed
  active is set to false; other fields (retry_count, backoff_ms, next_retry_us,
  env copy) retain their last values. They will be overwritten by the next
  schedule() call. Stale values could be misleading if slot inspection is added.

Risk 5 — No upper-layer notification of successful ACK
  receive() returns OK with the ACK envelope in env. The caller can inspect
  env.message_type == ACK and env.message_id. No separate "ACK received"
  result code is provided; callers must inspect env after an OK return.

Risk 6 — NAK and HEARTBEAT are not handled
  DeliveryEngine::receive() dispatches to the ACK block only for ACK messages.
  NAK and HEARTBEAT fall through to `return Result::OK` at line 194 without any
  specific handling. No NAK-triggered retry and no heartbeat response mechanism
  exist.

---

## 15. Unknowns / Assumptions

[ASSUMPTION 1] Source ID matching is only correct in single-node loopback scenarios
  AckTracker and RetryManager search using src == slot.env.source_id.
  This matches only if src (the ACK sender's ID) equals the local node's ID stored
  in the slot. In a two-node scenario this will fail. See Risk 1.

[ASSUMPTION 2] timestamp_expired(0, now_us) returns false
  ACK envelopes have expiry_time_us == 0 (MessageEnvelope.hpp:101).
  Confirmed: timestamp_expired (Timestamp.hpp:58) returns false when
  expiry_us == 0 due to the `(expiry_us != 0ULL)` guard.

[ASSUMPTION 3] Serializer::deserialize is deterministic and allocation-free
  Called inside TcpBackend::recv_from_client(). Assumed correct per Power of 10.

[ASSUMPTION 4] The ACK is not itself ACKed
  ACKs have reliability_class == BEST_EFFORT. receive() only schedules
  retry/ack tracking for RELIABLE_ACK and RELIABLE_RETRY messages. ACKs
  therefore do not generate new tracking entries.

[ASSUMPTION 5] Single-threaded call model
  No mutexes protect AckTracker or RetryManager state. receive(),
  pump_retries(), and sweep_ack_timeouts() must be called from the same thread.

[ASSUMPTION 6] RetryManager entry for RELIABLE_RETRY always coexists with AckTracker entry
  DeliveryEngine::send() creates both entries for RELIABLE_RETRY messages. An edge
  case exists where AckTracker::track() succeeds but RetryManager::schedule() fails
  (ERR_FULL). In that case, on_ack() succeeds in AckTracker but finds no entry in
  RetryManager (returns ERR_INVALID, logged as WARNING_LO).
