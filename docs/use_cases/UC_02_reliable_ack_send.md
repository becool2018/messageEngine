## 1. Use Case Overview

Use Case: UC-02 — Reliable-ACK Send and ACK Resolution over TCP
Initiator: Application caller (client role).
Trigger: Application populates a MessageEnvelope with message_type=DATA, reliability_class=RELIABLE_ACK, payload, destination_id, priority, expiry_time_us, then calls DeliveryEngine::send(). Later, the remote peer sends an ACK, and the application calls DeliveryEngine::receive() on the same engine.
Success End State:
  - Phase A (send): The serialized envelope is on the wire and AckTracker has a PENDING entry with a deadline. RetryManager is NOT engaged (RELIABLE_ACK does not auto-retry).
  - Phase B (receive ACK): AckTracker transitions the entry from PENDING to ACKED. receive() returns Result::OK to the caller.
Failure End States:
  - ERR_INVALID if engine not initialized.
  - ERR_IO / ERR_INVALID if serialization or transport send fails.
  - ERR_FULL if AckTracker has no free slots (m_count == ACK_TRACKER_CAPACITY = 32).
  - ERR_TIMEOUT from receive() if no ACK arrives within timeout_ms.
  - ERR_EXPIRED if the incoming ACK envelope has expired (unlikely for ACKs since expiry_time_us=0, but checked).
Scope: Covers the full send + ACK-receive cycle. Retry loop (RELIABLE_RETRY) and duplicate suppression are out of scope.

---

## 2. Entry Points

Phase A — Send:
  DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us)
  File: src/core/DeliveryEngine.cpp, line 75.
  Pre-conditions same as UC-01 except reliability_class=RELIABLE_ACK (0x01).

Phase B — Receive ACK:
  DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
  File: src/core/DeliveryEngine.cpp, line 147.
  Called by the application in a poll loop or after the send returns.
  env is an uninitialized or reused output buffer.

---

## 3. End-to-End Control Flow (Step-by-Step)

--- PHASE A: SEND ---

Step A1 — DeliveryEngine::send() entry (DeliveryEngine.cpp:75-82)
  Same as UC-01 Steps 1-2: assertions, guard check, envelope stamping.
  env.source_id = m_local_id
  env.message_id = m_id_gen.next()  [counter advances; value N stored for later ACK matching]
  env.timestamp_us = now_us

Step A2 — send_via_transport() (DeliveryEngine.cpp:91)
  Identical to UC-01 Steps 3-11. The envelope is serialized and transmitted over TCP.
  Result res = send_via_transport(env)
  If res != OK: log WARNING_LO, return res. Phase A ends in failure.

Step A3 — Reliability class gate: RELIABLE_ACK branch entered (DeliveryEngine.cpp:100-115)
  Condition: env.reliability_class == RELIABLE_ACK — TRUE.
  Compute ACK deadline:
    ack_deadline = timestamp_deadline_us(now_us, m_cfg.recv_timeout_ms)
    [ASSUMPTION: timestamp_deadline_us() = now_us + (recv_timeout_ms * 1000). Implementation in Timestamp.hpp not read.]

Step A4 — AckTracker::track() called (DeliveryEngine.cpp:108)
  m_ack_tracker.track(env, ack_deadline)
  File: AckTracker.cpp:42.

  AckTracker::track() internals:
    Pre-condition: assert(m_count <= ACK_TRACKER_CAPACITY)
    Linear scan of m_slots[0..31] for first entry where state == EntryState::FREE.
      [Power of 10 rule 2: fixed loop bound = ACK_TRACKER_CAPACITY = 32]
    On first FREE slot found (say index k):
      envelope_copy(m_slots[k].env, env)   — memcpy of full MessageEnvelope (~4140 bytes)
      m_slots[k].deadline_us = ack_deadline
      m_slots[k].state = EntryState::PENDING   (transition: FREE -> PENDING)
      m_count++
    Post-conditions:
      assert(m_slots[k].state == PENDING)
      assert(m_count <= ACK_TRACKER_CAPACITY)
    Returns Result::OK.

  If no FREE slot (m_count == 32):
    assert(m_count == ACK_TRACKER_CAPACITY)
    Returns Result::ERR_FULL.

  Back in DeliveryEngine::send() (DeliveryEngine.cpp:109-114):
    track_res = OK or ERR_FULL.
    If ERR_FULL: logs WARNING_HI "Failed to track ACK for message_id=..."
    Regardless: send is NOT failed. ACK tracking failure is treated as a non-fatal side effect.
    [Comment at line 113: "Do not fail the send; ACK tracking is a side effect"]

Step A5 — RetryManager branch NOT entered (DeliveryEngine.cpp:118)
  Condition: env.reliability_class == RELIABLE_RETRY — FALSE for RELIABLE_ACK.
  RetryManager::schedule() is NOT called. RetryManager remains untouched.

Step A6 — Post-conditions and return (DeliveryEngine.cpp:133-141)
  assert(env.source_id == m_local_id)
  assert(env.message_id != 0ULL)
  Logger::log(INFO, "Sent message_id=N, reliability=1")
  Returns Result::OK.
  AckTracker now holds one PENDING entry for (source_id=m_local_id, message_id=N, deadline=ack_deadline).

--- PHASE B: RECEIVE ACK ---

Step B1 — DeliveryEngine::receive() entry (DeliveryEngine.cpp:147-156)
  assert(m_initialized)
  assert(now_us > 0ULL)
  Guard: if (!m_initialized) return ERR_INVALID.

Step B2 — Transport receive (DeliveryEngine.cpp:159)
  res = m_transport->receive_message(env, timeout_ms)
  Dispatches to TcpBackend::receive_message(envelope, timeout_ms) (TcpBackend.cpp:325).

  TcpBackend::receive_message() internals (TcpBackend.cpp:325-380):
    Step B2a — Queue check:
      res = m_recv_queue.pop(envelope)
      If OK: return immediately (a previously buffered message is delivered).
      [ASSUMPTION: MessageRingBuffer::pop() is a FIFO dequeue operation]

    Step B2b — Accept new clients (server mode only):
      if (m_is_server): accept_clients() — non-blocking.
      In client mode: skipped.

    Step B2c — Poll loop (TcpBackend.cpp:341-376):
      poll_count = min((timeout_ms + 99) / 100, 50)   [cap at 50 iterations = 5 seconds]
      for attempt = 0 to poll_count-1:
        For each active client fd:
          recv_from_client(m_client_fds[i], 100)  [100 ms per-attempt timeout]
            tcp_recv_frame(fd, m_wire_buf, 8192, 100, &out_len)  (SocketUtils.cpp:429)
              socket_recv_exact(fd, header, 4, timeout_ms) — reads 4-byte length prefix.
              Parses frame_len from header.
              Validates frame_len <= WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES.
              socket_recv_exact(fd, m_wire_buf, frame_len, timeout_ms) — reads body.
            Serializer::deserialize(m_wire_buf, out_len, env)  (Serializer.cpp:173)
              Reads header fields in order (message_type, reliability_class, priority, padding, message_id, timestamp_us, source_id, destination_id, expiry_time_us, payload_length, padding).
              Validates padding bytes == 0.
              Validates payload_length <= MSG_MAX_PAYLOAD_BYTES.
              memcpy payload into env.payload[].
              assert(envelope_valid(env))
              Returns OK.
            m_recv_queue.push(env)  — enqueues deserialized envelope.

        res = m_recv_queue.pop(envelope)  — attempt to dequeue.
        If OK: return.

        Check ImpairmentEngine for delayed messages (collect_deliverable).
        Push any ready delayed envelopes to m_recv_queue.
        res = m_recv_queue.pop(envelope) again.
        If OK: return.

      If all iterations exhausted: return ERR_TIMEOUT.

  The ACK message from the remote peer is a MessageEnvelope with:
    message_type = MessageType::ACK  (0x01)
    message_id   = N  (the original message_id being acknowledged)
    source_id    = remote peer's NodeId
    reliability_class = BEST_EFFORT (ACKs carry no further reliability; envelope_make_ack() at MessageEnvelope.hpp:77 sets this)
    expiry_time_us = 0  (ACKs do not expire)

Step B3 — Receive result check (DeliveryEngine.cpp:160-163)
  if (res != OK): return res immediately.
  ERR_TIMEOUT: application's poll loop must retry.
  On success: env now holds the ACK envelope.

Step B4 — Expiry check (DeliveryEngine.cpp:169-174)
  timestamp_expired(env.expiry_time_us, now_us) called.
  For ACK envelopes: expiry_time_us = 0.
  [ASSUMPTION: timestamp_expired() returns false when expiry_time_us == 0, treating 0 as "never expires." This is the only logical interpretation given the comment "ACKs do not expire" in envelope_make_ack().]
  Normal path: not expired; continue.

Step B5 — Control message detection (DeliveryEngine.cpp:177)
  envelope_is_control(env) called (MessageEnvelope.hpp:69):
    Returns true if message_type == ACK || NAK || HEARTBEAT.
  For our ACK envelope: message_type == ACK — returns TRUE.

Step B6 — ACK type check and AckTracker::on_ack() (DeliveryEngine.cpp:178-189)
  if (env.message_type == MessageType::ACK) — TRUE.

  m_ack_tracker.on_ack(env.source_id, env.message_id) called (AckTracker.cpp:81).
    Note: env.source_id here is the REMOTE PEER's NodeId. env.message_id is N.

  AckTracker::on_ack() internals:
    Pre-condition: assert(m_count <= ACK_TRACKER_CAPACITY)
    Linear scan of m_slots[0..31]:
      Condition: state == PENDING AND env.source_id == src AND env.message_id == msg_id.
      Match logic: m_slots[k].env.source_id == src (remote peer id) AND m_slots[k].env.message_id == N.
      [CRITICAL OBSERVATION: The slot's env.source_id was set to m_local_id in Phase A Step A1. But on_ack() matches against env.source_id from the INCOMING ACK, which is the remote peer's id. These will NOT match unless the local node IS the peer, i.e., round-trip where source_id in the tracked slot == remote peer's source_id.
       This appears to be a semantic issue: AckTracker stores the original outgoing envelope (source_id = local node). The incoming ACK has source_id = remote peer. The match on source_id will fail. See Section 14 Risk 7 and Section 15 Unknown-3 for full analysis.]

    Assuming the match succeeds (or under the alternative interpretation):
      m_slots[k].state = EntryState::ACKED  (transition: PENDING -> ACKED)
      Post-condition: assert(m_slots[k].state == ACKED)
      Returns Result::OK.
    If no match:
      Returns Result::ERR_INVALID.

  Back in DeliveryEngine::receive() (DeliveryEngine.cpp:182):
    ack_res = on_ack() result.
    (void)ack_res — result is DISCARDED. ACK tracking is a side effect here too.

Step B7 — RetryManager::on_ack() called (DeliveryEngine.cpp:184-185)
  m_retry_manager.on_ack(env.source_id, env.message_id) called (RetryManager.cpp:97).
  For RELIABLE_ACK, RetryManager::schedule() was never called (Step A5), so no slot is active.
  on_ack() scans all 32 slots; none match; returns ERR_INVALID.
  Result is (void)retry_res — discarded.
  Logs WARNING_LO "No retry entry found for message_id=N from node=..." inside RetryManager::on_ack().

Step B8 — Control message early return (DeliveryEngine.cpp:192)
  return Result::OK.
  The ACK envelope is returned to the caller in env. The caller can inspect env.message_id to confirm which message was acknowledged.

--- ACKED SLOT CLEANUP ---

Step C1 — Next call to sweep_ack_timeouts() (DeliveryEngine.cpp:267)
  Called by the application's maintenance loop (not immediately after receive).
  m_ack_tracker.sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY) (AckTracker.cpp:142).

  AckTracker::sweep_expired() scans all 32 slots:
    PENDING slots with now_us >= deadline_us: copied to timeout_buf, state set to FREE, m_count--.
    ACKED slots: state set directly to FREE, m_count--.  (AckTracker.cpp:179-183)

  The slot for message N (state == ACKED) is released: state -> FREE, m_count--.
  expired_count does NOT include ACKED entries — only truly timed-out PENDING entries.
  The slot is now available for reuse.

---

## 4. Call Tree (Hierarchical)

PHASE A:
DeliveryEngine::send(env, now_us)                             [DeliveryEngine.cpp:75]
  m_id_gen.next()                                             [MessageId — ASSUMPTION]
  DeliveryEngine::send_via_transport(env)                     [DeliveryEngine.cpp:54]
    envelope_valid(env)                                       [MessageEnvelope.hpp:55]
    m_transport->send_message(env) --> TcpBackend::send_message(envelope)  [TcpBackend.cpp:249]
      Serializer::serialize(...)                              [Serializer.cpp:115]
        write_u8/write_u32/write_u64 helpers
        memcpy(payload)
      timestamp_now_us()
      ImpairmentEngine::process_outbound(...)                 [ImpairmentEngine.cpp:62]
        is_partition_active(now_us)                           [ImpairmentEngine.cpp:279]
        envelope_copy() into m_delay_buf[]
      ImpairmentEngine::collect_deliverable(...)              [ImpairmentEngine.cpp:174]
      tcp_send_frame(fd, wire_buf, len, timeout)              [SocketUtils.cpp:391]
        socket_send_all(fd, header, 4, ...)                   [SocketUtils.cpp:290]
          poll() + send()
        socket_send_all(fd, body, len, ...)                   [SocketUtils.cpp:290]
          poll() + send()
  [reliability_class == RELIABLE_ACK: branch taken]
  timestamp_deadline_us(now_us, recv_timeout_ms)              [Timestamp — ASSUMPTION]
  m_ack_tracker.track(env, ack_deadline)                      [AckTracker.cpp:42]
    envelope_copy(m_slots[k].env, env)                        [MessageEnvelope.hpp:49]
    m_slots[k].state = PENDING
    m_count++
  [reliability_class != RELIABLE_RETRY: RetryManager::schedule() NOT called]
  Logger::log(INFO, "Sent ...")

PHASE B:
DeliveryEngine::receive(env, timeout_ms, now_us)              [DeliveryEngine.cpp:147]
  m_transport->receive_message(env, timeout_ms) --> TcpBackend::receive_message(...)  [TcpBackend.cpp:325]
    m_recv_queue.pop(envelope)                                [ASSUMPTION: MessageRingBuffer]
    [if server: accept_clients()]
    [poll loop, up to 50 iterations:]
      recv_from_client(client_fd, 100)                        [TcpBackend.cpp:193]
        tcp_recv_frame(fd, wire_buf, 8192, 100, &out_len)     [SocketUtils.cpp:429]
          socket_recv_exact(fd, header, 4, ...)               [SocketUtils.cpp:337]
            poll() + recv() loop
          socket_recv_exact(fd, body, frame_len, ...)         [SocketUtils.cpp:337]
            poll() + recv() loop
        Serializer::deserialize(wire_buf, out_len, env)       [Serializer.cpp:173]
          read_u8/read_u32/read_u64 helpers
          memcpy(payload)
          envelope_valid(env)
        m_recv_queue.push(env)
      m_recv_queue.pop(envelope)
      ImpairmentEngine::collect_deliverable(...)
      m_recv_queue.pop(envelope)
  timestamp_expired(env.expiry_time_us, now_us)               [Timestamp — ASSUMPTION]
  envelope_is_control(env)                                    [MessageEnvelope.hpp:69]
  [env.message_type == ACK: branch taken]
  m_ack_tracker.on_ack(env.source_id, env.message_id)         [AckTracker.cpp:81]
    [scan m_slots[] for PENDING match]
    m_slots[k].state = ACKED                                  [PENDING -> ACKED transition]
  m_retry_manager.on_ack(env.source_id, env.message_id)       [RetryManager.cpp:97]
    [scan finds no entry; returns ERR_INVALID; logged; result voided]
  Logger::log(INFO, "Received ACK for message_id=N...")
  return Result::OK

PHASE C (deferred cleanup):
DeliveryEngine::sweep_ack_timeouts(now_us)                    [DeliveryEngine.cpp:267]
  m_ack_tracker.sweep_expired(now_us, timeout_buf, 32)        [AckTracker.cpp:142]
    [ACKED slot found: state -> FREE, m_count--]

---

## 5. Key Components Involved

DeliveryEngine (src/core/DeliveryEngine.cpp/.hpp)
  Central orchestrator for both phases. Owns AckTracker, RetryManager. Decides reliability class handling.

AckTracker (src/core/AckTracker.cpp/.hpp)
  Fixed array of 32 Entry structs (each: MessageEnvelope copy + deadline_us + EntryState).
  State machine: FREE -> PENDING (on track()) -> ACKED (on on_ack()) -> FREE (on sweep_expired()).
  Key invariant: m_count tracks non-FREE slots. Linear scan is O(ACK_TRACKER_CAPACITY) = O(32).

RetryManager (src/core/RetryManager.cpp)
  Involved only via on_ack() during receive, which finds no matching entry (schedule() was never called) and returns ERR_INVALID. Its result is voided. No functional role for RELIABLE_ACK.

TcpBackend (src/platform/TcpBackend.cpp)
  Handles both outbound send and inbound receive. Owns m_recv_queue and m_impairment.

ImpairmentEngine (src/platform/ImpairmentEngine.cpp)
  Applied on the outbound path (process_outbound). On receive path, collect_deliverable() is checked in the poll loop but process_inbound() is not called by TcpBackend (inbound impairment/reordering is not exercised in this trace).

Serializer (src/core/Serializer.cpp)
  Used in both directions: serialize() on send, deserialize() on receive.

SocketUtils (src/platform/SocketUtils.cpp)
  tcp_send_frame() on send, tcp_recv_frame() + socket_recv_exact() on receive.

MessageEnvelope (src/core/MessageEnvelope.hpp)
  envelope_make_ack() utility builds the ACK envelope on the REMOTE side (not traced here).
  envelope_is_control() and envelope_valid() used during receive processing.

---

## 6. Branching Logic / Decision Points

Decision 1 — Send-path guard: m_initialized (DeliveryEngine.cpp:80)
  Same as UC-01.

Decision 2 — Transport send success (DeliveryEngine.cpp:92-97)
  Same as UC-01.

Decision 3 — Reliability class gate at send: RELIABLE_ACK (DeliveryEngine.cpp:100)
  Condition TRUE: AckTracker::track() is called.
  This is the key divergence from BEST_EFFORT.

Decision 4 — AckTracker::track() slot availability (AckTracker.cpp:54)
  If FREE slot found (normal case): transition state to PENDING, return OK.
  If no FREE slot (tracker full, m_count==32): return ERR_FULL.
  DeliveryEngine treats ERR_FULL as a non-fatal warning — send proceeds anyway. The message is sent but ACK will never be detected if it arrives (no slot to match against).

Decision 5 — RetryManager gate at send: NOT RELIABLE_RETRY (DeliveryEngine.cpp:118)
  Condition FALSE: RetryManager::schedule() NOT called.
  RELIABLE_ACK relies on the caller to handle unacknowledged messages via sweep_ack_timeouts().

Decision 6 — Receive: transport result (DeliveryEngine.cpp:160)
  If ERR_TIMEOUT: caller's poll loop continues. AckTracker state unchanged.
  If OK: envelope delivered for inspection.

Decision 7 — Receive: expiry check (DeliveryEngine.cpp:169)
  For ACK envelopes (expiry_time_us == 0): assumed to never be expired.
  For data envelopes: drop if expired.

Decision 8 — Receive: control vs data (DeliveryEngine.cpp:177)
  envelope_is_control() returns true for ACK: takes the ACK processing branch.
  Falls through to data processing if not control.

Decision 9 — Receive: message_type == ACK (DeliveryEngine.cpp:178)
  TRUE for our ACK: calls m_ack_tracker.on_ack() and m_retry_manager.on_ack().
  NAK or HEARTBEAT: passes through without calling either tracker.

Decision 10 — AckTracker::on_ack() match (AckTracker.cpp:93-95)
  Scans for slot where state==PENDING AND source_id==src AND message_id==msg_id.
  Match: state transitions PENDING -> ACKED, returns OK.
  No match: returns ERR_INVALID (result voided by caller).
  The source_id matching semantics are a concern (see Risk 7, Unknown-3).

Decision 11 — Receive: duplicate suppression gate (DeliveryEngine.cpp:199)
  if (env.reliability_class == RELIABLE_RETRY) — only for RELIABLE_RETRY.
  RELIABLE_ACK ACK envelopes carry reliability_class=BEST_EFFORT (set by envelope_make_ack()).
  Dedup check NOT invoked.

---

## 7. Concurrency / Threading Behavior

The RELIABLE_ACK use case introduces a temporal gap between send() and receive() that has concurrency implications:

Scenario: two threads — one calling send() in a loop, one calling receive() (or pump_retries()/sweep_ack_timeouts()) in a maintenance loop.
  - m_ack_tracker.m_slots[] is accessed for write in track() (Phase A) and on_ack() / sweep_expired() (Phase B/C) without any mutex or atomic guard.
  - A data race exists if send() and receive() execute concurrently: track() increments m_count while sweep_expired() decrements it; both scan m_slots[].
  - No std::atomic on m_count (it is uint32_t, not std::atomic<uint32_t>).

AckTracker::on_ack() also accesses m_slots[].env.source_id, m_slots[].env.message_id, m_slots[].state — all plain struct fields; no atomic protection.

RetryManager::on_ack() similarly has unprotected access to m_slots[].active.

TcpBackend::receive_message() runs the poll loop and may call recv_from_client() which writes into m_wire_buf[] — shared with send_message(). Concurrent send and receive through the same TcpBackend instance are unsafe.

The design intent (F-Prime model, Power of 10) implies single-threaded operation per component. The application layer (src/app) is responsible for serializing access. This is an architectural invariant, not a code-level enforcement.

timestamp_now_us() called inside TcpBackend::receive_message() for collect_deliverable(). Two clock reads occur across the poll loop iterations; their relationship to now_us passed in from the caller is not defined.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

Phase A send path — same as UC-01 Section 8 for the transport portion.

AckTracker slot memory:
  m_slots[ACK_TRACKER_CAPACITY] = 32 entries.
  Each Entry = sizeof(MessageEnvelope) + sizeof(uint64_t) + sizeof(EntryState) = ~4140 + 8 + 1 = ~4149 bytes + padding.
  Total AckTracker static footprint: 32 * ~4149 = ~133 KB of BSS/data segment (member of DeliveryEngine).
  envelope_copy() in track() = memcpy of ~4140 bytes into the slot — full copy semantics. The tracked envelope is independent of the caller's original.

Phase B receive — env parameter:
  receive(MessageEnvelope& env, ...) — output parameter, not const.
  Filled by TcpBackend via Serializer::deserialize() which calls envelope_init() first (memset to zero) then populates fields.
  ACK envelope returned in env is the deserialized ACK from the wire; it is NOT the copy stored in AckTracker. The AckTracker copy (m_slots[k].env) remains in memory, state=ACKED, until sweep_expired() frees it.

Stack allocations in receive path (TcpBackend.cpp:362):
  delayed[IMPAIR_DELAY_BUF_SIZE] — same ~132 KB concern as UC-01.
  m_wire_buf used for wire bytes (member buffer, not stack).

No heap allocation anywhere in the traced path. All allocation is static (member arrays) or stack.

Pointer aliasing:
  m_transport in DeliveryEngine is a non-owning raw pointer to TcpBackend. TcpBackend must outlive all calls to DeliveryEngine::send() and receive().
  env parameter passed by reference in both send() and receive() — caller owns the envelope.

---

## 9. Error Handling Flow

Phase A — Send errors:

ERR_INVALID (DeliveryEngine not initialized):
  Returned immediately; no side effects.

Serialization / transport failure:
  send_via_transport() returns ERR_IO or ERR_INVALID.
  send() logs WARNING_LO and returns the error.
  AckTracker::track() is NOT reached (early return at line 96-97).
  No PENDING slot is created.

ERR_FULL from AckTracker::track() (tracker at capacity):
  Logged as WARNING_HI.
  send() continues and returns OK.
  The message IS sent to the wire but no ACK slot exists to receive the ACK.
  The application layer must handle unacknowledged messages via sweep_ack_timeouts() or by not sending beyond ACK_TRACKER_CAPACITY concurrent messages.

Phase B — Receive errors:

ERR_TIMEOUT from TcpBackend::receive_message():
  Returned directly to caller from receive() at line 162.
  AckTracker state unchanged.
  Application must call receive() again or call sweep_ack_timeouts() to detect expired PENDING entries.

ERR_EXPIRED (envelope has passed expiry_time_us):
  For ACK envelopes with expiry_time_us=0: should not occur if timestamp_expired(0, now_us) correctly returns false.
  For data envelopes accidentally caught here: dropped, ERR_EXPIRED returned.
  AckTracker not updated in this case.

ERR_INVALID from AckTracker::on_ack() (no matching PENDING slot):
  Result is (void)ack_res — ignored by DeliveryEngine.
  Causes: message was never tracked (send was non-tracked due to ERR_FULL), slot already swept, duplicate ACK, or source_id mismatch. No external signal to the caller.

WARNING_LO logged by RetryManager::on_ack() when it finds no matching entry:
  This occurs on every RELIABLE_ACK receive path because RetryManager was never given a slot. The log is spurious noise for RELIABLE_ACK use case.

ACK timeout (not an error in receive() itself):
  Detected by sweep_ack_timeouts() when now_us >= deadline_us.
  Logged as WARNING_HI per timed-out message.
  Slot state: PENDING -> FREE (swept).
  Application must check sweep_ack_timeouts() return count and react (re-send, abort, escalate).

---

## 10. External Interactions

Phase A:
  Same as UC-01 Section 10 for the outbound socket writes.

Phase B:
  poll(fd, POLLIN, 100) — waits up to 100 ms per attempt for socket readability.
  recv(fd, buf, remaining, 0) — kernel TCP receive from stream.
  timestamp_now_us() — one or more POSIX clock reads during the poll loop.
  Logger::log() — called for INFO "Received ACK...", and WARNING_LO from RetryManager (spurious for RELIABLE_ACK).

Remote peer (out of scope for this trace):
  The remote application calls envelope_make_ack() or equivalent to build an ACK envelope with message_type=ACK, message_id=N, source_id=remote_id, destination_id=local_id, expiry_time_us=0, reliability_class=BEST_EFFORT.
  The remote serializes and sends it back over the TCP connection to our listening/client fd.
  This arrives in the kernel receive buffer and is read by tcp_recv_frame() in Step B2c.

---

## 11. State Changes / Side Effects

Phase A state changes:

MessageEnvelope env (caller-owned):
  env.source_id stamped = m_local_id
  env.message_id stamped = N (next from m_id_gen)
  env.timestamp_us stamped = now_us

m_id_gen:
  Internal counter advanced by 1.

AckTracker m_slots[k] (k = first FREE slot):
  state: FREE -> PENDING
  env: copy of the outgoing envelope (memcpy ~4140 bytes)
  deadline_us: now_us + recv_timeout_ms * 1000
m_ack_tracker.m_count: incremented by 1.

TcpBackend m_wire_buf[]:
  Overwritten with serialized bytes of env.
ImpairmentEngine m_delay_buf[]:
  One slot activated (as in UC-01).

RetryManager:
  NO state change. m_count unchanged. All slots remain inactive.

Phase B state changes:

AckTracker m_slots[k]:
  state: PENDING -> ACKED
m_ack_tracker.m_count: unchanged (slot is non-FREE in both PENDING and ACKED states).

TcpBackend m_wire_buf[]:
  Overwritten with received bytes from tcp_recv_frame().
m_recv_queue:
  An entry was pushed by recv_from_client() and then popped by receive_message().

Phase C (sweep) state changes:
AckTracker m_slots[k]:
  state: ACKED -> FREE
m_ack_tracker.m_count: decremented by 1.

---

## 12. Sequence Diagram (ASCII)

APP        DeliveryEngine      AckTracker     TcpBackend        ImpairmentEngine    SocketUtils    Remote Peer
 |               |                  |              |                    |                 |              |
 |--send(env)--->|                  |              |                   |                 |              |
 |               |--stamp(src,id,ts)|              |                   |                 |              |
 |               |--send_via_xport->|              |                   |                 |              |
 |               |              send_message(env)->|                   |                 |              |
 |               |                  |         serialize(env,wire_buf)  |                 |              |
 |               |                  |         process_outbound(env)-->|                 |              |
 |               |                  |              | copy to delay_buf |                 |              |
 |               |                  |              |<--OK--------------|                 |              |
 |               |                  |         collect_deliverable()-->|                 |              |
 |               |                  |         tcp_send_frame(fd,...)  |         poll/send(header+body)->|
 |               |                  |              |<--true            |                 |              |
 |               |                  |         Result::OK              |                 |              |
 |               |<--Result::OK-    |              |                   |                 |              |
 |               |                  |              |                   |                 |              |
 |               |--track(env,deadline)            |                   |                 |              |
 |               |----------------->|              |                   |                 |              |
 |               |   FREE->PENDING  |              |                   |                 |              |
 |               |   m_count++      |              |                   |                 |              |
 |               |<--Result::OK-----|              |                   |                 |              |
 |               |                  |              |                   |                 |              |
 |               | [RELIABLE_RETRY? NO]            |                   |                 |              |
 |               | [RetryManager::schedule() NOT called]               |                 |              |
 |               |                  |              |                   |                 |              |
 |<--Result::OK--|                  |              |                   |                 |              |
 |               |                  |              |         [... time passes ...]       |              |
 |               |                  |              |                   |      <--ACK arrives from remote peer
 |               |                  |              |                   |         in kernel TCP buffer   |
 |               |                  |              |                   |                 |              |
 |--receive(env,timeout,now_us)     |              |                   |                 |              |
 |-------------->|                  |              |                   |                 |              |
 |               |--receive_message(env,timeout)-->|                   |                 |              |
 |               |                  |  m_recv_queue.pop() [empty]     |                 |              |
 |               |                  |  recv_from_client(fd,100)       |                 |              |
 |               |                  |       tcp_recv_frame(fd,...)--->|    poll(POLLIN)->|              |
 |               |                  |                                 |    recv(header) <-              |
 |               |                  |                                 |    recv(body) <--              |
 |               |                  |  deserialize(wire_buf)->ACK_env |                 |              |
 |               |                  |  recv_queue.push(ACK_env)       |                 |              |
 |               |                  |  recv_queue.pop(ACK_env)->OK    |                 |              |
 |               |<--Result::OK, ACK_env in envelope                  |                 |              |
 |               |                  |              |                   |                 |              |
 |               |--timestamp_expired(0, now_us) => false             |                 |              |
 |               |--envelope_is_control(ACK_env) => true              |                 |              |
 |               |--env.message_type==ACK => true                     |                 |              |
 |               |                  |              |                   |                 |              |
 |               |--on_ack(src_id, msg_id=N)       |                  |                 |              |
 |               |----------------->|              |                   |                 |              |
 |               |   scan PENDING slots            |                   |                 |              |
 |               |   match slot k: PENDING->ACKED  |                   |                 |              |
 |               |<--Result::OK-----|              |                   |                 |              |
 |               |  (ack_res voided)|              |                   |                 |              |
 |               |                  |              |                   |                 |              |
 |               |--retry_mgr.on_ack(src,N): no slot, ERR_INVALID (logged, voided)      |              |
 |               |                  |              |                   |                 |              |
 |               |--Logger(INFO, "Received ACK for N")                |                 |              |
 |<--Result::OK--|                  |              |                   |                 |              |
 |               |                  |              |                   |                 |              |
 | [... later: application calls sweep_ack_timeouts() ...]            |                 |              |
 |--sweep_ack_timeouts(now_us)      |              |                   |                 |              |
 |-------------->|--sweep_expired(now_us,buf,32)-> |                   |                 |              |
 |               |----------------->|              |                   |                 |              |
 |               |   slot k: ACKED->FREE, m_count--|                   |                 |              |
 |               |<--expired_count=0               |                   |                 |              |
 |<--0-----------|                  |              |                   |                 |              |

---

## 13. Initialization vs Runtime Flow

Initialization (same as UC-01, Section 13) plus:

AckTracker::init() (called from DeliveryEngine::init()):
  memset(m_slots, 0, sizeof(m_slots)) — all 32 Entry structs zeroed.
  m_count = 0.
  Loop verifies all slots are FREE (EntryState::FREE == 0, which matches zero-init).

RetryManager::init() (called from DeliveryEngine::init()):
  m_count = 0, m_initialized = true.
  All slots: active=false, retry_count=0, backoff_ms=0, next_retry_us=0, expiry_us=0, max_retries=0.
  envelope_init() called on each slot's env.

Runtime Phase A (first send):
  AckTracker has 0 PENDING entries before the first call. After: 1 PENDING entry.
  m_id_gen counter is at N after generating message_id N. Subsequent sends increment N.

Runtime Phase B (first receive after ACK arrives):
  AckTracker transitions from 1 PENDING to 0 PENDING + 1 ACKED.
  After sweep_ack_timeouts(): 0 PENDING, 0 ACKED, slot FREE again.

Capacity management over time:
  Maximum concurrent in-flight RELIABLE_ACK messages = 32 (ACK_TRACKER_CAPACITY).
  Exceeding this causes silent ACK tracking failure (ERR_FULL, send proceeds).
  The application must call sweep_ack_timeouts() periodically to free ACKED and timed-out PENDING slots.

---

## 14. Known Risks / Observations

Risk 1 through Risk 6 from UC-01 apply equally here.

Risk 7 — AckTracker source_id matching semantics mismatch (CRITICAL).
  In Phase A, AckTracker::track() stores a copy of the outgoing envelope. That envelope has:
    env.source_id = m_local_id  (the sender, i.e., this node)
  In Phase B, AckTracker::on_ack(env.source_id, env.message_id) is called where env is the incoming ACK envelope. The incoming ACK was built by envelope_make_ack() on the remote side:
    ack.source_id = my_id  (the REMOTE peer's node ID)
    ack.message_id = original.message_id  (= N)
  So on_ack() searches for a slot where:
    m_slots[i].env.source_id == remote_peer_id  AND  m_slots[i].env.message_id == N
  But the slot stored source_id = local_id (this node). Unless local_id == remote_peer_id (impossible — different nodes have different IDs), the scan will NEVER find a match. The PENDING slot will remain until it is swept as timed-out by sweep_ack_timeouts(). The ACK is received and acknowledged via Logger, but the AckTracker state never advances to ACKED. This is a functional bug unless the intent is that env.source_id in the ACK carries the ORIGINAL sender's id (i.e., local_id), which would contradict envelope_make_ack()'s assignment of ack.source_id = my_id (the responder).

Risk 8 — RetryManager::on_ack() spurious WARNING_LO log on every RELIABLE_ACK receive.
  For RELIABLE_ACK, RetryManager was never engaged. Every ACK receipt triggers a WARNING_LO "No retry entry found..." log from RetryManager. In a system sending hundreds of RELIABLE_ACK messages per second, this would flood the log. The guard should check whether RetryManager has an active slot before calling on_ack() for non-RELIABLE_RETRY messages.

Risk 9 — ACK tracking failure is silent to the caller.
  If AckTracker::track() returns ERR_FULL, the caller's send() still returns OK. The application has no way to know that ACK tracking was skipped without checking the log. A higher-severity return code or an out-parameter for tracking status would improve observability.

Risk 10 — ACKED slot not reclaimed until sweep_ack_timeouts() is called.
  After on_ack() transitions a slot to ACKED, m_count is NOT decremented at that point. The slot remains non-FREE and counts toward the 32-slot limit until sweep_expired() runs. If the application sends many RELIABLE_ACK messages rapidly and calls sweep_ack_timeouts() infrequently, the tracker can exhaust capacity even when all messages have been acknowledged.

Risk 11 — timestamp_expired(0, now_us) behavior for ACK envelopes.
  envelope_make_ack() sets expiry_time_us=0. The implementation of timestamp_expired() is not read. If it returns true when expiry_time_us == 0 (treating 0 as "immediately expired"), all incoming ACK envelopes would be silently dropped at DeliveryEngine::receive() line 169-173, and the PENDING slot would never be resolved. This would be a silent correctness failure.

---

## 15. Unknowns / Assumptions

All UC-01 unknowns apply here ([ASSUMPTION-1] through [ASSUMPTION-8], [UNKNOWN-1], [UNKNOWN-2]).

[ASSUMPTION-9] timestamp_deadline_us(now_us, recv_timeout_ms) computes now_us + (uint64_t)recv_timeout_ms * 1000ULL. Implementation not read (Timestamp.hpp not in file list).

[ASSUMPTION-10] timestamp_expired(expiry_time_us, now_us) returns false when expiry_time_us == 0, treating zero as "never expires." This is the only semantics consistent with the design intent for ACK envelopes. If the opposite is true, the receive path is broken for all ACKs.

[ASSUMPTION-11] MessageRingBuffer (m_recv_queue in TcpBackend) is a FIFO ring buffer of MessageEnvelope structs with a fixed capacity. pop() returns ERR_EMPTY when empty, OK when an item is dequeued. Its implementation (MessageRingBuffer.hpp/cpp) was not read.

[UNKNOWN-3] The AckTracker::on_ack() source_id matching is the most significant open question. Two interpretations exist:
  Interpretation A (as coded): The stored slot contains the local node's source_id; the incoming ACK contains the remote's source_id. These will never match. The PENDING slot always times out. This is a bug.
  Interpretation B (possible alternative design intent): The ACK envelope is constructed on the receiver side with source_id = original.source_id (i.e., the local node's id) and destination_id = original.source_id, meaning the ACK is "on behalf of" the original sender. envelope_make_ack() as written does NOT do this — it sets ack.source_id = my_id (the ACK sender, i.e., the remote peer). So Interpretation B is not supported by the current code.
  Resolution requires either a test or design clarification from the authors.

[UNKNOWN-4] Whether DeliveryEngine::receive() is called by the same entity that called send(), or by a separate consumer. The API does not enforce coupling. In a symmetric design (client both sends and receives on the same engine), the receive() call on the sender's DeliveryEngine handles the incoming ACK. In an asymmetric design, the ACK might arrive on a different engine instance that has no tracked slot, making on_ack() always return ERR_INVALID.

[UNKNOWN-5] sweep_ack_timeouts() is not called by send() or receive() — it is a separate maintenance call. The application's call frequency determines how long ACKED and timed-out PENDING slots stay alive. No guidance on minimum sweep frequency is visible in the code.

---

The two documents above are the complete Flow-of-Control traces. Here is a summary of the key findings:

**What was confirmed directly from source:**

- BEST_EFFORT send bypasses AckTracker and RetryManager entirely via explicit `reliability_class` guards at DeliveryEngine.cpp:100 and :118. Both gates are simple `if` conditionals; no function pointer dispatch, no virtual call for that logic.
- RELIABLE_ACK causes AckTracker::track() to fire, creating a FREE->PENDING transition in the fixed 32-slot array. RetryManager::schedule() is explicitly skipped because the outer condition checks for RELIABLE_RETRY specifically.
- ACK resolution in receive() follows: deserialize -> expiry check -> envelope_is_control() -> on_ack() -> PENDING->ACKED. The ACKED state is not freed in place; it waits for sweep_ack_timeouts().
- The ImpairmentEngine interposes on every outbound message regardless of reliability class, with silent drop (ERR_IO mapped to OK) for loss/partition.

**Critical finding (Risk 7 / Unknown-3):** The source_id field used to match an incoming ACK against a tracked PENDING slot appears to be semantically mismatched — the slot stores the local node's source_id, but the incoming ACK's source_id is the remote peer's id. This would cause every ACK to fail its AckTracker match, leaving all RELIABLE_ACK slots to time out rather than resolve to ACKED.agentId: ae924a360e2f250f9 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 78271
tool_uses: 11
duration_ms: 413490</usage>