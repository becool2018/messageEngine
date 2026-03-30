## 1. Use Case Overview

Use Case ID : UC-10
Title       : RetryManager fires a scheduled retry after backoff interval elapses
Layer       : Core (DeliveryEngine / RetryManager)
Trigger     : A periodic caller (application main loop or timer thread) invokes
              DeliveryEngine::pump_retries(now_us) when the wall-clock has
              advanced past a retry entry's next_retry_us deadline.
Preconditions:
  - DeliveryEngine::init() has been called and m_initialized == true.
  - RetryManager::init() has been called; m_initialized == true.
  - At least one RetryEntry is active (active == true) with
    retry_count < max_retries, expiry_us not yet reached, and
    next_retry_us <= now_us.
  - m_transport points to a valid, open TransportInterface (e.g., TcpBackend).
Postconditions:
  - The message envelope has been re-submitted to the transport layer.
  - retry_count has been incremented by 1.
  - backoff_ms has been doubled (capped at 60,000 ms).
  - next_retry_us has been rescheduled to now_us + new_backoff_us.
  - The number of retried messages is returned to the caller.
Requirement traceability:
  - CLAUDE.md §3.2 "Retry logic: configurable retry count and backoff"
  - CLAUDE.md §3.3 "Reliable with retry and dedupe"


## 2. Entry Points

Primary entry point (called by application or timer):
  DeliveryEngine::pump_retries(uint64_t now_us)
    File : src/core/DeliveryEngine.cpp, line 225
    Sig  : uint32_t DeliveryEngine::pump_retries(uint64_t now_us)

Prerequisite entry points (must have been called earlier):
  DeliveryEngine::init(TransportInterface*, const ChannelConfig&, NodeId)
    File : src/core/DeliveryEngine.cpp, line 15
  RetryManager::init()
    File : src/core/RetryManager.cpp, line 16
  RetryManager::schedule(env, max_retries, backoff_ms, now_us)
    File : src/core/RetryManager.cpp, line 44
    [Called by DeliveryEngine::send() when reliability_class == RELIABLE_RETRY]


## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::pump_retries, line 227]
  Assert m_initialized == true.
  Assert now_us > 0.

Step 2  [DeliveryEngine::pump_retries, line 231]
  Declare stack-allocated retry_buf[MSG_RING_CAPACITY] (64 MessageEnvelope
  slots).  No heap allocation — Power of 10 rule 3.

Step 3  [DeliveryEngine::pump_retries, line 236]
  Call m_retry_manager.collect_due(now_us, retry_buf, MSG_RING_CAPACITY).
  Execution transfers to RetryManager::collect_due().

Step 4  [RetryManager::collect_due, line 140]
  Assert m_initialized.
  Assert out_buf != nullptr.
  Assert buf_cap <= MSG_RING_CAPACITY.
  Initialize local: collected = 0.

Step 5  [RetryManager::collect_due, line 147 — outer loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 slots), while collected < buf_cap.
  Per-slot processing:

  Step 5a  [line 151]
    If m_slots[i].active == false → continue (skip inactive slot).

  Step 5b  [line 156 — expiry check]
    If expiry_us != 0 AND now_us >= expiry_us:
      - Set m_slots[i].active = false.
      - Decrement m_count.
      - Log WARNING_LO "Expired retry entry for message_id=…"
      - continue (do not add to output buffer).
    [Branching: expired entries are silently deactivated, not retried.]

  Step 5c  [line 166 — exhaustion check]
    If retry_count >= max_retries:
      - Set m_slots[i].active = false.
      - Decrement m_count.
      - Log WARNING_HI "Exhausted retries for message_id=… (count=…, max=…)"
      - continue.
    [Branching: exhausted entries are deactivated before any send attempt.]

  Step 5d  [line 177 — due check]
    If next_retry_us <= now_us:
      (This is the "retry fires" path.)

      Step 5d-i  [line 179]
        Call envelope_copy(out_buf[collected], m_slots[i].env).
        This performs a memcpy of sizeof(MessageEnvelope) bytes from the
        RetryEntry's stored envelope into the caller's stack buffer.
        collected is incremented.

      Step 5d-ii  [line 183]
        Increment m_slots[i].retry_count by 1.

      Step 5d-iii  [lines 187-191 — exponential backoff]
        Compute next_backoff_ms = (uint64_t)backoff_ms * 2.
        If next_backoff_ms > 60000 → clamp to 60000 (60-second cap).
        Store back: m_slots[i].backoff_ms = (uint32_t)next_backoff_ms.

      Step 5d-iv  [lines 194-195 — reschedule]
        backoff_us = backoff_ms * 1000.
        m_slots[i].next_retry_us = now_us + backoff_us.

      Step 5d-v  [lines 197-200]
        Log INFO "Due for retry: message_id=…, attempt=…, next_backoff_ms=…"

  If next_retry_us > now_us → slot is not yet due; skip without action.

Step 6  [RetryManager::collect_due, lines 205-206]
  Post-condition assertions:
    assert collected <= buf_cap.
    assert m_count <= ACK_TRACKER_CAPACITY.
  Return collected (uint32_t).

Step 7  [DeliveryEngine::pump_retries, line 237]
  Return value stored in local 'collected'.
  Assert collected <= MSG_RING_CAPACITY.

Step 8  [DeliveryEngine::pump_retries, lines 241-256 — resend loop]
  Bounded loop: i = 0 .. collected-1.
  Per iteration:
    Assert i < MSG_RING_CAPACITY.
    Call send_via_transport(retry_buf[i]).
    Execution transfers to DeliveryEngine::send_via_transport().

Step 9  [DeliveryEngine::send_via_transport, line 54]
  Assert m_initialized.
  Assert m_transport != nullptr.
  Assert envelope_valid(retry_buf[i])  — checks message_type != INVALID,
    payload_length <= MSG_MAX_PAYLOAD_BYTES, source_id != NODE_ID_INVALID.

Step 10  [DeliveryEngine::send_via_transport, line 61]
  Call m_transport->send_message(env).
  [For TcpBackend: execution enters TcpBackend::send_message().]

Step 11  [TcpBackend::send_message, line 249]
  Assert m_open.
  Assert envelope_valid(envelope).
  Call Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len).
  [wire_len is set to the byte count of the serialized frame.]

Step 12  [TcpBackend::send_message, line 264-268]
  Call m_impairment.process_outbound(envelope, now_us).
  If result == ERR_IO: message dropped silently by loss impairment; return OK.
  [Impairment engine may delay or drop the retry — independent of RetryManager.]

Step 13  [TcpBackend::send_message, lines 273-315]
  Collect any previously-delayed envelopes via collect_deliverable().
  Send the serialized frame to all active client fds (bounded loop over
  MAX_TCP_CONNECTIONS = 8).
  Call tcp_send_frame(m_client_fds[i], m_wire_buf, wire_len, send_timeout_ms)
  for each valid fd.

Step 14  [TcpBackend::send_message, line 317]
  Assert wire_len > 0.
  Return Result::OK (or error code from Serializer on failure).

Step 15  [DeliveryEngine::send_via_transport, lines 63-67]
  Check return value from m_transport->send_message().
  If res != OK: Log WARNING_LO "Transport send failed for message_id=…"
  Return res.

Step 16  [DeliveryEngine::pump_retries, lines 246-255]
  Check return value from send_via_transport().
  If send_res != OK:
    Log WARNING_LO "Retry send failed for message_id=… (result=…)"
  Else:
    Log INFO "Retried message_id=…"
  Loop continues to next collected entry.

Step 17  [DeliveryEngine::pump_retries, lines 259-261]
  Post-condition assertion: assert collected <= MSG_RING_CAPACITY.
  Return collected to caller.


## 4. Call Tree (Hierarchical)

DeliveryEngine::pump_retries(now_us)
  [assert m_initialized, assert now_us > 0]
  RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)
    [assert m_initialized, assert out_buf != nullptr, assert buf_cap <= MSG_RING_CAPACITY]
    [loop i = 0..31]
      [if active && !expired && !exhausted && next_retry_us <= now_us]
        envelope_copy(out_buf[collected], m_slots[i].env)
          memcpy(&dst, &src, sizeof(MessageEnvelope))   [MessageEnvelope.hpp:51]
        m_slots[i].retry_count++
        [backoff doubling, capped at 60000]
        m_slots[i].next_retry_us = now_us + backoff_us
        Logger::log(INFO, "RetryManager", "Due for retry: ...")
          snprintf / vsnprintf / fprintf(stderr)         [Logger.hpp:44-57]
    [return collected]
  [assert collected <= MSG_RING_CAPACITY]
  [loop i = 0..collected-1]
    DeliveryEngine::send_via_transport(retry_buf[i])
      [assert m_initialized, m_transport != nullptr, envelope_valid]
      m_transport->send_message(retry_buf[i])            [virtual dispatch]
        TcpBackend::send_message(envelope)
          [assert m_open, assert envelope_valid]
          Serializer::serialize(envelope, wire_buf, cap, wire_len)
          timestamp_now_us()                             [Timestamp.hpp]
          m_impairment.process_outbound(envelope, now_us)
          m_impairment.collect_deliverable(now_us, ...)
          [loop over MAX_TCP_CONNECTIONS]
            tcp_send_frame(fd, wire_buf, wire_len, timeout_ms)
          [loop over delayed envelopes]
            Serializer::serialize(delayed, wire_buf, cap, len)
            tcp_send_frame(fd, wire_buf, len, timeout_ms)
          [assert wire_len > 0]
          [return Result::OK]
      [if res != OK: Logger::log(WARNING_LO, ...)]
      [return res]
    [if send_res != OK: Logger::log(WARNING_LO, ...)]
    [else: Logger::log(INFO, "Retried message_id=...")]
  [assert collected <= MSG_RING_CAPACITY]
  [return collected]


## 5. Key Components Involved

Component             File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine        src/core/DeliveryEngine.cpp/.hpp      Coordinator; drives
                                                            the retry cycle;
                                                            owns all sub-objects.

RetryManager          src/core/RetryManager.cpp/.hpp        Maintains fixed
                                                            table of RetryEntry
                                                            slots; applies
                                                            exponential backoff;
                                                            decides which entries
                                                            are due.

RetryEntry (struct)   src/core/RetryManager.hpp:88-96       Per-message retry
                                                            state: env copy,
                                                            next_retry_us,
                                                            expiry_us,
                                                            retry_count,
                                                            max_retries,
                                                            backoff_ms, active.

MessageEnvelope       src/core/MessageEnvelope.hpp          Data carrier; copied
                                                            by value into
                                                            output buffer via
                                                            envelope_copy().

TransportInterface    src/core/TransportInterface.hpp       Abstract send/receive
                      [not read directly, inferred]         contract.

TcpBackend            src/platform/TcpBackend.cpp/.hpp      Concrete transport;
                                                            serializes envelope,
                                                            applies impairments,
                                                            writes TCP frame.

Logger                src/core/Logger.hpp                   Fixed-buffer stderr
                                                            sink; severity tags.

Types.hpp             src/core/Types.hpp                    Constants:
                                                            ACK_TRACKER_CAPACITY
                                                            (32),
                                                            MSG_RING_CAPACITY
                                                            (64),
                                                            MAX_TCP_CONNECTIONS
                                                            (8).


## 6. Branching Logic / Decision Points

Decision 1 — Is the slot active?
  Location: RetryManager::collect_due, line 151
  Condition: m_slots[i].active == false
  True  → continue (skip slot entirely)
  False → proceed to expiry/exhaustion/due checks

Decision 2 — Has the retry entry expired?
  Location: RetryManager::collect_due, line 156
  Condition: expiry_us != 0 AND now_us >= expiry_us
  True  → deactivate slot, decrement m_count, log WARNING_LO, continue
  False → proceed to exhaustion check
  Note: expiry_us == 0 means "never expire" [ASSUMPTION: based on guard
        `expiry_us != 0ULL`].

Decision 3 — Have retries been exhausted?
  Location: RetryManager::collect_due, line 166
  Condition: retry_count >= max_retries
  True  → deactivate slot, decrement m_count, log WARNING_HI, continue
  False → proceed to due check

Decision 4 — Is the retry due now?
  Location: RetryManager::collect_due, line 177
  Condition: next_retry_us <= now_us
  True  → copy envelope, update retry_count and backoff, reschedule
  False → skip; entry will be checked again on the next pump_retries call

Decision 5 — Backoff cap
  Location: RetryManager::collect_due, lines 188-190
  Condition: next_backoff_ms > 60000
  True  → clamp to 60000 ms
  False → use doubled value

Decision 6 — Transport send success
  Location: DeliveryEngine::send_via_transport, line 63
  Condition: res != Result::OK
  True  → log WARNING_LO; return error to pump_retries
  False → return OK

Decision 7 — Output buffer capacity guard
  Location: RetryManager::collect_due, line 147
  Condition: collected < buf_cap (loop continuation condition)
  Purpose: prevents overflow of the caller's stack buffer


## 7. Concurrency / Threading Behavior

The codebase does not implement its own threading in these files. There is
no mutex protecting RetryManager::m_slots or DeliveryEngine's member
objects.

Observed design intent:
  - pump_retries() is intended to be called from a single thread (the
    application's main loop or a dedicated periodic task).
  - AckTracker and RetryManager contain no synchronization primitives.
  - [ASSUMPTION] The caller is responsible for ensuring pump_retries()
    is not called concurrently with send() or receive() on the same
    DeliveryEngine instance.

std::atomic carve-out:
  - Types.hpp and the files read contain no std::atomic usage. No atomic
    operations are performed inside pump_retries or collect_due.

Race condition risk:
  - If pump_retries() and receive() (which calls RetryManager::on_ack())
    are called from different threads simultaneously, m_slots[i].active
    could be modified by on_ack() mid-sweep. There is no lock.
    [ASSUMPTION: single-threaded use is the intended execution model.]


## 8. Memory & Ownership Semantics (C/C++ Specific)

Allocation model:
  - No heap allocation anywhere in this call path. All buffers are
    stack-allocated or statically sized struct members.

Stack frame — DeliveryEngine::pump_retries():
  - retry_buf[MSG_RING_CAPACITY]: 64 x sizeof(MessageEnvelope).
    sizeof(MessageEnvelope) = ~4 + 8 + 8 + 4 + 4 + 1 + 1 + 8 + 4 +
    4096 = ~4134 bytes (layout/padding-dependent).
    Stack impact: ~264,576 bytes [ASSUMPTION on sizeof; this is large and
    worth noting — see Known Risks].
  - collected: uint32_t (4 bytes)

Stack frame — TcpBackend::send_message():
  - delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]: 32 x sizeof(MessageEnvelope),
    another ~132,288 bytes.

Envelope copying:
  - envelope_copy() is a shallow memcpy of sizeof(MessageEnvelope) bytes.
  - The payload[] array is a fixed uint8_t[4096] embedded directly in the
    struct — no pointer indirection, fully owned by value.

Ownership of retry copy:
  - RetryEntry::env is a fully owned copy of the envelope made when
    RetryManager::schedule() calls envelope_copy(m_slots[i].env, env).
  - collect_due() copies from that stored copy into out_buf[]. The
    pump_retries() stack buffer therefore holds a second copy used only
    for the duration of that call.

Lifetime:
  - retry_buf[] exists only within pump_retries(). After return, the copies
    are gone. The authoritative copy remains in RetryManager::m_slots[i].env
    until the entry is deactivated.

Fixed capacity constants (Types.hpp):
  - ACK_TRACKER_CAPACITY = 32   (RetryManager slot table size)
  - MSG_RING_CAPACITY    = 64   (pump_retries output buffer size)
  - MSG_MAX_PAYLOAD_BYTES = 4096 (payload array size per envelope)


## 9. Error Handling Flow

Error source → handling action

Transport serialization failure (Serializer::serialize returns non-OK):
  - TcpBackend::send_message() logs WARNING_LO, returns error code.
  - send_via_transport() logs WARNING_LO "Transport send failed", returns
    non-OK to pump_retries().
  - pump_retries() logs WARNING_LO "Retry send failed for message_id=…"
  - Loop continues to next due entry; failure does not abort the whole sweep.
  - RetryManager entry remains active; retry will fire again after next
    backoff period.

Impairment drop (process_outbound returns ERR_IO):
  - TcpBackend::send_message() returns Result::OK (drop is silent).
  - From DeliveryEngine's perspective, send succeeded; retry state is
    updated (count incremented, next time rescheduled).
  - [ASSUMPTION: this is intentional — impairment simulates network loss,
    so the retry machinery should still advance normally.]

No connected clients (m_client_count == 0):
  - TcpBackend::send_message() logs WARNING_LO, returns OK.
  - Same effect as impairment drop from RetryManager's perspective.

tcp_send_frame failure per-client:
  - Logged as WARNING_LO inside TcpBackend.
  - Return value is not propagated per-client (loop continues to next fd).
  - TcpBackend::send_message() returns OK overall.

Retry entry expired during collect_due:
  - Slot deactivated; WARNING_LO logged.
  - Entry is never added to out_buf; no resend attempted.

Retry entry exhausted during collect_due:
  - Slot deactivated; WARNING_HI logged.
  - Entry is never added to out_buf; no resend attempted.
  - [ASSUMPTION: upper layers are not notified of exhaustion; the WARNING_HI
    log is the only signal.]

Output buffer full (collected == buf_cap before loop finishes):
  - Loop exits early due to `collected < buf_cap` guard in line 147.
  - Remaining due entries are skipped this cycle; they will fire on the
    next pump_retries call because next_retry_us has NOT been advanced
    for skipped entries.

init() not called (m_initialized == false):
  - pump_retries() assertion at line 227 fires.
  - In debug builds: abort via assert(). In release: undefined behavior
    unless asserts are kept enabled.


## 10. External Interactions

1. TransportInterface::send_message() [virtual call]
   - Called once per due retry entry in the inner loop.
   - Concrete target (TcpBackend) performs POSIX socket I/O via
     tcp_send_frame(), which calls write()/send() on a connected fd.

2. Serializer::serialize()
   - Called inside TcpBackend::send_message() to convert the in-memory
     MessageEnvelope into a length-prefixed wire frame.
   - [ASSUMPTION: Serializer is a static-method class similar to Logger;
     no dynamic allocation.]

3. ImpairmentEngine::process_outbound() and collect_deliverable()
   - Called inside TcpBackend::send_message().
   - May drop, delay, or pass through the message.
   - collect_deliverable() may trigger sending of previously buffered
     delayed messages to the socket in the same call.

4. Logger::log() / fprintf(stderr)
   - Called multiple times: once in RetryManager::collect_due() per due
     entry (INFO), once in send_via_transport() on failure (WARNING_LO),
     once in pump_retries() per entry on success (INFO) or failure
     (WARNING_LO).
   - Uses a 512-byte stack-local buffer; writes to stderr via fprintf.
   - No file I/O, no heap.

5. timestamp_now_us() [called inside TcpBackend::send_message()]
   - Platform call to get current wall-clock time in microseconds.
   - Used for impairment timing, not for retry scheduling (retry timing
     is driven by now_us passed from the caller).


## 11. State Changes / Side Effects

Object               Field                  Before             After
─────────────────────────────────────────────────────────────────────────────
RetryEntry           active                 true               true (unchanged
                                                               unless expired
                                                               or exhausted)
RetryEntry           retry_count            N                  N + 1
RetryEntry           backoff_ms             B                  min(B*2, 60000)
RetryEntry           next_retry_us          now_us (or less)   now_us + new_B_us
RetryManager         m_count               N_active           N_active - K
                                                               (K = expired +
                                                               exhausted entries
                                                               this sweep)
DeliveryEngine       (none changed)         —                  —
AckTracker           (not touched)          —                  —
TcpBackend           m_wire_buf             stale              contains last
                                                               serialized frame
stderr               log output             —                  zero or more lines
                                                               appended


## 12. Sequence Diagram (ASCII)

Caller (app loop)         DeliveryEngine      RetryManager       TcpBackend
      |                        |                    |                  |
      |  pump_retries(now_us)  |                    |                  |
      |----------------------->|                    |                  |
      |                        |  collect_due(now_us, buf, cap)        |
      |                        |------------------->|                  |
      |                        |  [loop slots 0..31]|                  |
      |                        |  [slot i: active, not expired,        |
      |                        |   not exhausted, due]                 |
      |                        |  envelope_copy(buf[0], slot.env)      |
      |                        |  retry_count++     |                  |
      |                        |  backoff doubled   |                  |
      |                        |  next_retry reschd |                  |
      |                        |  Logger INFO       |                  |
      |                        |<-------------------|                  |
      |                        |  returns collected=1                  |
      |                        |                    |                  |
      |                        |  [loop i=0..0]     |                  |
      |                        |  send_via_transport(buf[0])           |
      |                        |---------------------------------------->
      |                        |                    |  serialize()     |
      |                        |                    |  process_outbound|
      |                        |                    |  tcp_send_frame()|
      |                        |<----------------------------------------
      |                        |  returns OK        |                  |
      |                        |  Logger INFO "Retried message_id=…"   |
      |  returns 1             |                    |                  |
      |<-----------------------|                    |                  |


## 13. Initialization vs Runtime Flow

Initialization path (called once at startup, before any pump_retries call):

  DeliveryEngine::init()           [DeliveryEngine.cpp:15]
    m_ack_tracker.init()           [AckTracker.cpp:21]  — zeros m_slots[]
    m_retry_manager.init()         [RetryManager.cpp:16] — zeros m_slots[],
                                    sets m_count=0, m_initialized=true
    m_dedup.init()
    m_id_gen.init(seed)
    m_initialized = true

  RetryManager::init() details:
    - Loops ACK_TRACKER_CAPACITY (32) times.
    - Each slot: active=false, retry_count=0, backoff_ms=0,
      next_retry_us=0, expiry_us=0, max_retries=0.
    - envelope_init(m_slots[i].env): memset to 0, message_type=INVALID.

  RetryManager::schedule() (called inside DeliveryEngine::send()):
    - Finds first inactive slot.
    - envelope_copy(slot.env, env): copies the outgoing envelope.
    - Sets next_retry_us = now_us (first retry fires immediately on next
      pump_retries call).
    - Sets expiry_us = env.expiry_time_us.
    - Sets max_retries and backoff_ms from ChannelConfig.
    - Increments m_count.

Runtime path (called periodically):
  DeliveryEngine::pump_retries(now_us) — as documented above.

Key distinction:
  - No allocation occurs at runtime. All tables were sized at
    compile-time and zeroed at init. Runtime only reads/writes fields
    within already-allocated structs.


## 14. Known Risks / Observations

Risk 1 — Large stack allocation in pump_retries()
  retry_buf[64] of MessageEnvelope, each ~4134 bytes =
  approximately 264,576 bytes of stack per call. TcpBackend::send_message()
  also allocates delayed_envelopes[32] ≈ 132,288 bytes on its own stack
  frame, which is on the same call stack. Total stack depth could exceed
  400 KB. This may be acceptable on a hosted Linux platform but would
  violate embedded stack budgets. [OBSERVATION: no stack size limit is
  documented in CLAUDE.md for this target.]

Risk 2 — No notification on retry exhaustion
  When retry_count >= max_retries, the entry is silently deactivated
  and WARNING_HI is logged. There is no callback or return-value signal
  to the application layer. An application relying on delivery confirmation
  has no programmatic way to learn a message failed permanently without
  parsing log output. [OBSERVATION: by design per F-Prime event model,
  but could be a gap for error recovery.]

Risk 3 — first_retry fires immediately
  schedule() sets next_retry_us = now_us, meaning the FIRST retry fires
  on the very next pump_retries call. The initial send (in send()) already
  transmitted the message. The first "retry" is therefore a duplicate
  send of attempt #1, potentially before the original has even arrived.
  [OBSERVATION: this may be intentional for aggressive retry, but it is
  not immediately obvious from the API.]

Risk 4 — Impairment can silently advance retry state
  If process_outbound() drops the retry packet but returns OK (via the
  ERR_IO → OK translation in TcpBackend::send_message()), the RetryManager
  advances retry_count and reschedules. The receiver never saw this
  attempt, but the sender burned a retry slot. Under heavy loss
  configuration, exhaustion happens faster than the application expects.

Risk 5 — No per-destination tracking in RetryManager
  RetryEntry stores env.source_id (which is the local node), not the
  destination. on_ack() matches by source_id and message_id. If two
  messages with the same message_id are sent from the same source to
  different destinations, they would be incorrectly cross-matched.
  [ASSUMPTION: message_id is globally unique per sender, making this
  safe in practice. Verify MessageIdGen guarantees monotone uniqueness.]

Risk 6 — Partial sweep when buf_cap < ACK_TRACKER_CAPACITY
  MSG_RING_CAPACITY (64) > ACK_TRACKER_CAPACITY (32), so the cap
  passed by pump_retries (MSG_RING_CAPACITY) is always larger than
  the table size. This means the loop will never be cut short by the
  `collected < buf_cap` guard under current constants. If constants
  change, this guard becomes critical.


## 15. Unknowns / Assumptions

[ASSUMPTION 1] Single-threaded call model
  pump_retries() and receive() (which calls on_ack()) are assumed to be
  called from the same thread. No evidence of synchronization was found.

[ASSUMPTION 2] expiry_us == 0 means "never expire"
  Based on the guard `m_slots[i].expiry_us != 0ULL` at line 156. If a
  caller passes expiry_time_us = 0 in the envelope, the entry will never
  expire, regardless of how much time passes.

[ASSUMPTION 3] Serializer has no dynamic allocation
  Serializer::serialize() is invoked inside TcpBackend::send_message()
  but its source was not read. Assumed consistent with Power of 10.

[ASSUMPTION 4] TransportInterface is a pure abstract base
  Inferred from virtual dispatch semantics. The concrete instance seen
  is TcpBackend.

[ASSUMPTION 5] now_us is monotonically increasing
  The correctness of next_retry_us scheduling assumes the caller always
  passes a non-decreasing now_us. If now_us goes backward (e.g., clock
  correction), all entries become "not due" and no retries fire.

[ASSUMPTION 6] collect_due is the only function that mutates retry_count
  on_ack() sets active=false but does not touch retry_count. schedule()
  sets retry_count=0. Only collect_due increments it. This is consistent
  with the code but not explicitly documented.

[ASSUMPTION 7] ACK_TRACKER_CAPACITY is the correct capacity for RetryManager
  RetryManager uses ACK_TRACKER_CAPACITY (32) for its slot table despite
  being distinct from AckTracker. This re-use of the same constant may
  cause confusion if the two subsystems need different capacities.
```

---