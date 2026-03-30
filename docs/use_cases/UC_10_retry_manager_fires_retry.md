# UC_10 — RetryManager fires scheduled retry

**HL Group:** HL-10 — User pumps the retry loop
**Actor:** System
**Requirement traceability:** REQ-3.2.5, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

A periodic caller (application main loop or timer thread) invokes
DeliveryEngine::pump_retries(now_us) when the wall-clock has advanced past a retry
entry's next_retry_us deadline. The RetryManager holds at least one active RetryEntry
where active == true, retry_count < max_retries, expiry_us has not been reached, and
next_retry_us <= now_us. The call causes the due message to be re-transmitted through
the transport layer, and the retry slot's backoff to be doubled and rescheduled.

### Expected outcome (single goal)

The due message envelope is re-submitted to the transport layer. The RetryEntry's
retry_count is incremented, backoff_ms is doubled (capped at 60,000 ms), and
next_retry_us is rescheduled to now_us + new_backoff_us. The number of retried
messages is returned to the caller.

---

## 2. Entry Points

Primary entry point (called by application or timer):

    DeliveryEngine::pump_retries(uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, line 227
    Sig:  uint32_t DeliveryEngine::pump_retries(uint64_t now_us)

Prerequisite entry points (must have been called earlier):

    DeliveryEngine::init(TransportInterface*, const ChannelConfig&, NodeId)
      [DeliveryEngine.cpp:17]
    RetryManager::init()
      [RetryManager.cpp:45]
    RetryManager::schedule(env, max_retries, backoff_ms, now_us)
      [RetryManager.cpp:72]
      [Called by DeliveryEngine::send() when reliability_class == RELIABLE_RETRY]

File-static helpers called within collect_due():

    slot_has_expired(uint64_t expiry_us, uint64_t now_us)  [RetryManager.cpp:19]
      Returns (expiry_us != 0ULL) && (now_us >= expiry_us).
    advance_backoff(uint32_t current_ms)                    [RetryManager.cpp:30]
      Returns min(current_ms * 2, 60000).

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::pump_retries, line 229]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL).
  Guard against calling pump_retries() before init(). Always active; not NDEBUG-gated.

Step 2  [DeliveryEngine::pump_retries, line 233]
  Declare stack-allocated retry_buf[MSG_RING_CAPACITY] (64 MessageEnvelope slots).
  No heap allocation — Power of 10 rule 3.
  Stack impact: 64 × sizeof(MessageEnvelope) ≈ 264 KB (see Known Risks).

Step 3  [DeliveryEngine::pump_retries, line 238]
  Call m_retry_manager.collect_due(now_us, retry_buf, MSG_RING_CAPACITY).
  Execution transfers to RetryManager::collect_due().

Step 4  [RetryManager::collect_due, line 164]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr).
  NEVER_COMPILED_OUT_ASSERT(buf_cap <= MSG_RING_CAPACITY).
  Initialize local: collected = 0.

Step 5  [RetryManager::collect_due, line 171 — main loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 slots, fixed bound)
  while collected < buf_cap.
  Per-slot processing:

  Step 5a  [line 172]
    If m_slots[i].active == false → continue (skip inactive slot).

  Step 5b  [line 177 — expiry check]
    Call slot_has_expired(m_slots[i].expiry_us, now_us):
      NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
      NEVER_COMPILED_OUT_ASSERT(expiry_us != ~0ULL)
      Returns (expiry_us != 0ULL) && (now_us >= expiry_us).
    If true (entry expired):
      m_slots[i].active = false.
      Decrement m_count.
      Logger::log(WARNING_LO, "RetryManager",
                  "Expired retry entry for message_id=…")
      continue (do not add to output buffer; no resend).
    [Note: expiry_us == 0 means never-expire due to the (expiry_us != 0ULL) guard.]

  Step 5c  [line 187 — exhaustion check]
    If m_slots[i].retry_count >= m_slots[i].max_retries:
      m_slots[i].active = false.
      Decrement m_count.
      Logger::log(WARNING_HI, "RetryManager",
                  "Exhausted retries for message_id=… (count=…, max=…)")
      continue (deactivate before any send attempt).
    [Exhausted entries are deactivated without retrying.]

  Step 5d  [line 198 — due check]
    If m_slots[i].next_retry_us <= now_us:
      [This is the "retry fires" path.]

      Step 5d-i  [line 200]
        Call envelope_copy(out_buf[collected], m_slots[i].env).
        memcpy of sizeof(MessageEnvelope) bytes from the RetryEntry's stored
        envelope into the caller's stack buffer. Increment collected.

      Step 5d-ii  [line 202]
        Increment m_slots[i].retry_count by 1.

      Step 5d-iii  [line 205 — exponential backoff]
        Call advance_backoff(m_slots[i].backoff_ms):
          NEVER_COMPILED_OUT_ASSERT(current_ms <= 60000U)
          doubled = (uint64_t)current_ms * 2U
          If doubled > 60000: NEVER_COMPILED_OUT_ASSERT(doubled > 60000),
            return 60000U.
          Else: NEVER_COMPILED_OUT_ASSERT(doubled <= 60000),
            return (uint32_t)doubled.
        Store result: m_slots[i].backoff_ms = advance_backoff(m_slots[i].backoff_ms).

      Step 5d-iv  [lines 206-207 — reschedule]
        backoff_us = (uint64_t)new_backoff_ms * 1000ULL.
        m_slots[i].next_retry_us = now_us + backoff_us.

      Step 5d-v  [lines 209-212]
        Logger::log(INFO, "RetryManager",
                    "Due for retry: message_id=…, attempt=…, next_backoff_ms=…")

    If next_retry_us > now_us: slot is not yet due; skip without action.

Step 6  [RetryManager::collect_due, lines 217-218]
  NEVER_COMPILED_OUT_ASSERT(collected <= buf_cap).
  NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).
  Return collected (uint32_t).

Step 7  [DeliveryEngine::pump_retries, line 239]
  Return value stored in local 'collected'.
  NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY).

Step 8  [DeliveryEngine::pump_retries, lines 243-256 — resend loop]
  Bounded loop: i = 0 .. collected-1.
  Per iteration:
    Call DeliveryEngine::send_via_transport(retry_buf[i]).

Step 9  [DeliveryEngine::send_via_transport, line 58]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr).
  NEVER_COMPILED_OUT_ASSERT(envelope_valid(retry_buf[i])).

Step 10  [DeliveryEngine::send_via_transport, line 63]
  Call m_transport->send_message(env).
  Virtual dispatch to TcpBackend::send_message() (or LocalSimHarness in tests).

Step 11  [TcpBackend::send_message]
  Serializer::serialize(envelope, wire_buf, cap, wire_len).
  timestamp_now_us() for impairment timing.
  m_impairment.process_outbound(envelope, now_us).
    [May drop, delay, or pass through the retry packet.]
  If impairment result == ERR_IO: return OK (drop transparent to DeliveryEngine).
  If m_client_count == 0: log WARNING_LO, return OK.
  Else: send_to_all_clients(wire_buf, wire_len).
        flush_delayed_to_clients(now_us).
  Return Result::OK.

Step 12  [DeliveryEngine::send_via_transport, lines 65-69]
  If res != OK: Logger::log(WARNING_LO, "Transport send failed for message_id=…")
  Return res.

Step 13  [DeliveryEngine::pump_retries, lines 246-255]
  If send_res != OK:
    Logger::log(WARNING_LO, "Retry send failed for message_id=… (result=…)")
  Else:
    Logger::log(INFO, "Retried message_id=…")
  Loop continues to next collected entry.

Step 14  [DeliveryEngine::pump_retries, lines 259-261]
  NEVER_COMPILED_OUT_ASSERT(collected <= MSG_RING_CAPACITY).
  Return collected to caller.

---

## 4. Call Tree (Hierarchical)

DeliveryEngine::pump_retries(now_us)                           [DeliveryEngine.cpp:227]
  [NEVER_COMPILED_OUT_ASSERT m_initialized, now_us > 0]
  RetryManager::collect_due(now_us, retry_buf, 64)             [RetryManager.cpp:160]
    [NEVER_COMPILED_OUT_ASSERT m_initialized, out_buf!=nullptr, buf_cap<=64]
    [loop i=0..31, while collected < 64]
      [if !active → continue]
      slot_has_expired(m_slots[i].expiry_us, now_us)           [RetryManager.cpp:19]
        [if true → deactivate, WARNING_LO, continue]
      [if retry_count >= max_retries → deactivate, WARNING_HI, continue]
      [if next_retry_us <= now_us: RETRY FIRES]
        envelope_copy(out_buf[collected], m_slots[i].env)      [MessageEnvelope.hpp:56]
          memcpy(&dst, &src, sizeof(MessageEnvelope))
        ++m_slots[i].retry_count
        advance_backoff(m_slots[i].backoff_ms)                 [RetryManager.cpp:30]
          → min(backoff_ms * 2, 60000)
        m_slots[i].backoff_ms = new_backoff_ms
        m_slots[i].next_retry_us = now_us + new_backoff_us
        Logger::log(INFO, "RetryManager", "Due for retry: …")
    [NEVER_COMPILED_OUT_ASSERT collected<=64, m_count<=32]
    return collected
  [NEVER_COMPILED_OUT_ASSERT collected <= 64]
  [loop i=0..collected-1]
    DeliveryEngine::send_via_transport(retry_buf[i])           [DeliveryEngine.cpp:56]
      [NEVER_COMPILED_OUT_ASSERT m_initialized, transport, envelope_valid]
      m_transport->send_message(retry_buf[i])                  [virtual dispatch]
        TcpBackend::send_message(envelope)
          Serializer::serialize(envelope, wire_buf, cap, wire_len)
          timestamp_now_us()                                   [Timestamp.hpp:31]
          m_impairment.process_outbound(envelope, now_us)      [ImpairmentEngine]
            check_loss()
              m_prng.next_double()
          [if ERR_IO → return OK (loss transparent)]
          [if m_client_count == 0 → return OK]
          send_to_all_clients(wire_buf, wire_len)
            [loop over MAX_TCP_CONNECTIONS fds]
              tcp_send_frame(fd, buf, len, timeout_ms)
          flush_delayed_to_clients(now_us)
            m_impairment.collect_deliverable(now_us, …)
            [loop over matured delayed envelopes]
              Serializer::serialize + send_to_all_clients
          return Result::OK
      [if res != OK: Logger::log(WARNING_LO, "Transport send failed …")]
      return res
    [if send_res != OK: Logger::log(WARNING_LO, "Retry send failed …")]
    [else: Logger::log(INFO, "Retried message_id=…")]
  [NEVER_COMPILED_OUT_ASSERT collected <= 64]
  return collected

---

## 5. Key Components Involved

Component              File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine         src/core/DeliveryEngine.cpp/.hpp      Coordinator: drives
                                                             the retry cycle via
                                                             pump_retries(); owns
                                                             all sub-objects by
                                                             value.

RetryManager           src/core/RetryManager.cpp/.hpp        Maintains 32-slot
                                                             fixed table of
                                                             RetryEntry structs;
                                                             applies exponential
                                                             backoff; decides
                                                             which entries are due.

RetryEntry (struct)    src/core/RetryManager.hpp:93          Per-message retry
                                                             state: env copy,
                                                             next_retry_us,
                                                             expiry_us,
                                                             retry_count,
                                                             max_retries,
                                                             backoff_ms, active.

advance_backoff()      src/core/RetryManager.cpp:30          File-static helper:
                                                             returns
                                                             min(ms*2, 60000);
                                                             asserts pre/post
                                                             conditions.

slot_has_expired()     src/core/RetryManager.cpp:19          File-static helper:
                                                             expiry predicate:
                                                             (expiry_us != 0) &&
                                                             (now >= expiry_us).

MessageEnvelope        src/core/MessageEnvelope.hpp          Data carrier; copied
                                                             by value into
                                                             retry_buf[] via
                                                             envelope_copy().

TcpBackend             src/platform/TcpBackend.cpp/.hpp      Concrete transport;
                                                             serializes, applies
                                                             impairments, sends
                                                             via
                                                             send_to_all_clients()
                                                             and
                                                             flush_delayed_to_
                                                             clients().

Logger                 src/core/Logger.hpp                   Fixed-buffer stderr
                                                             sink; INFO on due
                                                             entry; WARNING_LO on
                                                             expiry; WARNING_HI
                                                             on exhaustion.

Types.hpp              src/core/Types.hpp                    ACK_TRACKER_CAPACITY
                                                             (32),
                                                             MSG_RING_CAPACITY
                                                             (64).

---

## 6. Branching Logic / Decision Points

Decision 1 — Is the slot active?
  Location: RetryManager::collect_due, line 172
  Condition: !m_slots[i].active
  True  → continue (skip slot entirely).
  False → proceed to expiry/exhaustion/due checks.

Decision 2 — Has the retry entry expired?
  Location: RetryManager::collect_due, line 177
  Condition: slot_has_expired(expiry_us, now_us)
             = (expiry_us != 0ULL) && (now_us >= expiry_us)
  True  → deactivate slot, decrement m_count, log WARNING_LO, continue.
  False → proceed to exhaustion check.
  Note: expiry_us == 0 means "never expire" (guard: expiry_us != 0ULL).

Decision 3 — Have retries been exhausted?
  Location: RetryManager::collect_due, line 187
  Condition: retry_count >= max_retries
  True  → deactivate slot, decrement m_count, log WARNING_HI, continue.
  False → proceed to due check.

Decision 4 — Is the retry due now?
  Location: RetryManager::collect_due, line 198
  Condition: next_retry_us <= now_us
  True  → copy envelope, increment retry_count, advance backoff, reschedule.
           [This is the "retry fires" path exercised by this use case.]
  False → skip; entry will be checked again on the next pump_retries call.

Decision 5 — Backoff cap inside advance_backoff()
  Location: RetryManager.cpp, line 34
  Condition: doubled > 60000U
  True  → NEVER_COMPILED_OUT_ASSERT(doubled > 60000), return 60000U (cap).
  False → NEVER_COMPILED_OUT_ASSERT(doubled <= 60000), return (uint32_t)doubled.

Decision 6 — Transport send success
  Location: DeliveryEngine::send_via_transport, line 65
  Condition: res != Result::OK
  True  → log WARNING_LO; return error to pump_retries.
  False → return OK.

Decision 7 — Impairment drop
  Location: TcpBackend::send_message
  Condition: process_outbound returns ERR_IO
  True  → return OK; drop is transparent to DeliveryEngine.
           [retry_count was incremented; retry state advances even though the
           packet was not actually sent on the wire.]
  False → proceed to client send.

Decision 8 — Output buffer capacity guard
  Location: RetryManager::collect_due, line 171
  Condition: collected < buf_cap (loop continuation condition)
  Purpose: prevents overflow of the caller's stack buffer; remaining due entries
           are skipped this cycle and will fire on the next pump_retries call.

---

## 7. Concurrency / Threading Behavior

No synchronization primitives exist in RetryManager or DeliveryEngine.

[ASSUMPTION] pump_retries() is intended to be called from a single thread — the
application's main loop or a dedicated periodic task. AckTracker and RetryManager
contain no synchronization primitives.

Race condition risk:
  If pump_retries() and receive() (which calls RetryManager::on_ack()) run from
  different threads simultaneously, m_slots[i].active could be written by on_ack()
  mid-sweep in collect_due(). Both are modifying the same RetryEntry fields with
  no lock. The resulting data race is undefined behavior under C++17.

  Similarly, send() may call RetryManager::schedule() concurrently with
  collect_due(), producing a race on m_slots[i] fields and m_count.

  [ASSUMPTION: single-threaded use is the intended and only correct call model.]

Logger thread safety:
  Logger::log() uses a stack-local buf[512]; no shared mutable state.
  fprintf(stderr) is typically thread-safe on POSIX (locked internally) but
  may produce interleaved output lines under concurrent calls.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

Allocation model:
  No heap allocation on this path. All buffers are stack-allocated or statically
  sized struct members embedded in DeliveryEngine.

Stack frame — DeliveryEngine::pump_retries():
  retry_buf[MSG_RING_CAPACITY]: 64 × sizeof(MessageEnvelope).
    sizeof(MessageEnvelope): 8+8+8 (uint64_t fields) + 4+4+4 (NodeId/uint32_t) +
    1+1+1 (enum/uint8_t fields) + padding + 4096 (payload[]) ≈ 4135 bytes.
    Stack impact: ~264 KB.  [OBSERVATION: large; see Known Risks.]
  collected: uint32_t (4 bytes).

Stack frame — TcpBackend::flush_delayed_to_clients():
  delayed[IMPAIR_DELAY_BUF_SIZE]: 32 × sizeof(MessageEnvelope) ≈ 132 KB.
  Called from send_message(), which is on the same call stack during the resend
  loop. Combined peak stack usage can exceed 400 KB.

Envelope copying:
  envelope_copy() is a memcpy of sizeof(MessageEnvelope) bytes.
  The payload[] array (uint8_t[4096]) is embedded directly in the struct;
  no pointer indirection; fully owned by value.

Ownership of retry copy:
  RetryEntry::env is a deep copy made by RetryManager::schedule() when the
  message is first scheduled. collect_due() copies from that stored copy into
  retry_buf[]. The pump_retries() stack buffer holds a second copy used only
  for the duration of that call. After return, the stack copy is gone; the
  authoritative copy remains in m_slots[i].env until the entry is deactivated.

---

## 9. Error Handling Flow

Transport serialization failure (Serializer::serialize returns non-OK):
  TcpBackend::send_message() logs WARNING_LO; returns error.
  send_via_transport() logs WARNING_LO "Transport send failed for message_id=…".
  pump_retries() logs WARNING_LO "Retry send failed for message_id=…".
  Loop continues to the next due entry; a single-send failure does not abort the sweep.
  RetryManager entry remains active; retry will fire again after the next backoff period.
  [Note: retry_count was already incremented and next_retry_us already advanced.]

Impairment drop (process_outbound returns ERR_IO):
  TcpBackend::send_message() converts ERR_IO to Result::OK.
  DeliveryEngine treats the send as successful; retry state is updated normally.
  [ASSUMPTION: this is intentional — impairment simulates network loss;
  the retry machinery advances regardless to mirror real-world retry behavior.]

No connected clients (m_client_count == 0):
  TcpBackend::send_message() logs WARNING_LO; returns OK.
  Same effect as an impairment drop from RetryManager's perspective.

Retry entry expired during collect_due:
  slot_has_expired() returns true → slot deactivated; WARNING_LO logged.
  Entry is never added to out_buf; no resend attempted.

Retry entry exhausted during collect_due:
  retry_count >= max_retries → slot deactivated; WARNING_HI logged.
  Entry is never added to out_buf; no resend attempted.
  [No upper-layer notification beyond the log; see Known Risks.]

Output buffer full (collected == buf_cap before loop finishes):
  Loop exits early due to collected < buf_cap guard.
  Remaining due entries are skipped this cycle; next_retry_us has NOT been advanced
  for those entries, so they will fire on the next pump_retries call.

m_initialized == false:
  NEVER_COMPILED_OUT_ASSERT(m_initialized) at pump_retries line 229 aborts the
  process in all builds (NEVER_COMPILED_OUT_ASSERT is not NDEBUG-gated).

---

## 10. External Interactions

1. TransportInterface::send_message() [virtual call → TcpBackend]
   Called once per due retry entry in the inner resend loop.
   Concrete target (TcpBackend) performs POSIX socket I/O via tcp_send_frame()
   inside send_to_all_clients(), which calls write/send on connected fds.

2. Serializer::serialize()
   Called inside TcpBackend::send_message() to convert the in-memory MessageEnvelope
   into a length-prefixed wire frame.
   Also called inside flush_delayed_to_clients() for delayed envelopes.

3. ImpairmentEngine::process_outbound() and flush_delayed_to_clients()
   process_outbound() called inside TcpBackend::send_message().
   May drop, delay, or pass through the retry packet.
   flush_delayed_to_clients() triggers transmission of previously-buffered delayed
   messages in the same call.

4. Logger::log() / fprintf(stderr)
   Called in RetryManager::collect_due() per due entry (INFO),
   optionally WARNING_LO (expired) or WARNING_HI (exhausted).
   Called in send_via_transport() on failure (WARNING_LO).
   Called in pump_retries() per entry on success (INFO) or failure (WARNING_LO).
   Uses a 512-byte stack-local buffer; writes to stderr via fprintf.

5. timestamp_now_us() [called inside TcpBackend::send_message()]
   Platform call via clock_gettime(CLOCK_MONOTONIC).
   Used for impairment timing only; retry scheduling is driven by the now_us
   passed from the caller of pump_retries().

---

## 11. State Changes / Side Effects

Object               Field               Before               After
─────────────────────────────────────────────────────────────────────────────
RetryEntry           active              true                 true (unchanged
                                                              unless expired
                                                              or exhausted)
RetryEntry           retry_count         N                    N + 1
RetryEntry           backoff_ms          B                    min(B*2, 60000)
RetryEntry           next_retry_us       <= now_us            now_us + new_B_us
RetryManager         m_count             N_active             N_active - K
                                                              (K = expired +
                                                               exhausted this
                                                               sweep)
DeliveryEngine       (none changed)      —                    —
AckTracker           (not touched)       —                    —
TcpBackend           m_wire_buf          stale                contains last
                                                              serialized frame
stderr               log output          —                    INFO lines per
                                                              retried entry;
                                                              optional
                                                              WARNING_LO/HI

---

## 12. Sequence Diagram (ASCII)

```
Caller (app loop)     DeliveryEngine     RetryManager          TcpBackend
      |                     |                  |                    |
      | pump_retries(now_us)|                  |                    |
      |-------------------->|                  |                    |
      |                     | collect_due(     |                    |
      |                     |  now_us,buf,64)  |                    |
      |                     |----------------->|                    |
      |                     |                  | [loop i=0..31]     |
      |                     |                  | [slot i: active,   |
      |                     |                  |  not expired,      |
      |                     |                  |  not exhausted,    |
      |                     |                  |  due]              |
      |                     |                  | envelope_copy(     |
      |                     |                  |  buf[0], slot.env) |
      |                     |                  | retry_count++      |
      |                     |                  | advance_backoff()  |
      |                     |                  | reschedule         |
      |                     |                  | Logger INFO        |
      |                     |<-----------------|                    |
      |                     | returns 1        |                    |
      |                     |                  |                    |
      |                     | [loop i=0..0]    |                    |
      |                     | send_via_        |                    |
      |                     | transport(buf[0])|                    |
      |                     |-------------------------------------------->
      |                     |                  |    serialize()     |
      |                     |                  |    process_outbound|
      |                     |                  |    send_to_all_    |
      |                     |                  |    clients()       |
      |                     |                  |    flush_delayed_  |
      |                     |                  |    to_clients()    |
      |                     |<--------------------------------------------
      |                     | returns OK       |                    |
      |                     | Logger INFO      |                    |
      |                     | "Retried …"      |                    |
      | returns 1            |                  |                    |
      |<--------------------|                  |                    |
```

---

## 13. Initialization vs Runtime Flow

Initialization path (called once at startup, before any pump_retries call):

  DeliveryEngine::init()               [DeliveryEngine.cpp:17]
    m_retry_manager.init()             [RetryManager.cpp:45]
      m_count = 0; m_initialized = true
      [loop ACK_TRACKER_CAPACITY=32 times]
        m_slots[i].active       = false
        m_slots[i].retry_count  = 0
        m_slots[i].backoff_ms   = 0
        m_slots[i].next_retry_us = 0
        m_slots[i].expiry_us    = 0
        m_slots[i].max_retries  = 0
        envelope_init(m_slots[i].env)  [memset 0; message_type=INVALID]
      NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
      NEVER_COMPILED_OUT_ASSERT(m_initialized)

  RetryManager::schedule() [RetryManager.cpp:72]
  (Called inside DeliveryEngine::send() for RELIABLE_RETRY messages):
    Finds first inactive slot.
    envelope_copy(slot.env, env): deep copy of the outgoing envelope.
    slot.active        = true
    slot.next_retry_us = now_us   [first retry fires immediately on next pump call]
    slot.expiry_us     = env.expiry_time_us
    slot.max_retries   = cfg.max_retries
    slot.backoff_ms    = cfg.retry_backoff_ms
    slot.retry_count   = 0
    m_count++

Runtime path (called periodically):
  DeliveryEngine::pump_retries(now_us) — as documented above.

Key distinction:
  No allocation occurs at runtime. All tables were sized at compile-time and
  zeroed at init. Runtime only reads and writes fields within already-allocated
  structs. The retry_buf[] stack array is the only per-call allocation.

---

## 14. Known Risks / Observations

Risk 1 — Large stack allocation in pump_retries()
  retry_buf[64] of MessageEnvelope ≈ 264 KB on the call stack.
  flush_delayed_to_clients() (called from TcpBackend::send_message()) allocates
  delayed[32] (32 × ~4135 ≈ 132 KB) on its own frame, on the same call stack
  during the resend loop. Combined peak stack usage can exceed 400 KB. Acceptable
  on a hosted Linux platform but would violate embedded stack budgets.
  See docs/STACK_ANALYSIS.md for the full analysis.

Risk 2 — No upper-layer notification on retry exhaustion
  When retry_count >= max_retries, the entry is silently deactivated and
  WARNING_HI is logged. There is no callback or return-value signal to the
  application. The return value of pump_retries() is the count of messages
  retried this cycle, not the count of exhausted entries.

Risk 3 — First retry fires immediately
  RetryManager::schedule() sets next_retry_us = now_us, meaning the first retry
  fires on the very next pump_retries call. The initial send() already transmitted
  the message. The first "retry" is therefore a duplicate send of attempt #1,
  potentially before the original has even arrived at the peer.

Risk 4 — Impairment can silently advance retry state
  If process_outbound() drops the retry packet but TcpBackend converts ERR_IO to
  OK, RetryManager advances retry_count and reschedules. The receiver never saw
  this attempt, but the sender consumed a retry slot.

Risk 5 — No per-destination tracking in RetryManager
  RetryEntry stores env.source_id (the local node's ID), not the destination.
  on_ack() matches by source_id and message_id. If two messages with the same
  message_id were sent to different destinations, they would be cross-matched.
  [ASSUMPTION: message_id is globally unique per sender, making this safe.]

Risk 6 — advance_backoff precondition: backoff_ms must be <= 60000
  If schedule() is called with a backoff_ms > 60000 (outside the valid range),
  the NEVER_COMPILED_OUT_ASSERT in advance_backoff() will abort the process on
  the next collect_due() call.

---

## 15. Unknowns / Assumptions

[ASSUMPTION 1] Single-threaded call model
  pump_retries() and receive() (which calls on_ack()) are assumed to be called
  from the same thread. No synchronization evidence found in the codebase.

[ASSUMPTION 2] expiry_us == 0 means "never expire"
  Based on the guard in slot_has_expired(): `expiry_us != 0ULL`. A caller that
  passes expiry_time_us = 0 in the envelope will produce an entry that never
  expires, regardless of how much time passes.

[ASSUMPTION 3] TransportInterface is a pure abstract base
  Inferred from virtual dispatch semantics. Concrete instance used in production
  is TcpBackend; tests may use LocalSimHarness.

[ASSUMPTION 4] now_us is monotonically increasing
  Correctness of next_retry_us scheduling assumes the caller always passes a
  non-decreasing now_us. If now_us goes backward (clock correction), all entries
  become "not due" and no retries fire until now_us catches up.

[ASSUMPTION 5] collect_due is the only function that mutates retry_count
  on_ack() sets active=false but does not touch retry_count.
  schedule() sets retry_count=0. Only collect_due increments it.

[ASSUMPTION 6] ACK_TRACKER_CAPACITY is the correct capacity for RetryManager
  RetryManager uses ACK_TRACKER_CAPACITY (32) for its slot table, despite being
  distinct from AckTracker. This re-use of the same constant may cause confusion
  if the two subsystems need different capacities in the future.
