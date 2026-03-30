# UC_11 — AckTracker timeout sweep

**HL Group:** HL-11 — User sweeps ACK timeouts
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2

---

## 1. Use Case Overview

### What triggers this flow

A periodic caller (application main loop or timer thread) invokes
DeliveryEngine::sweep_ack_timeouts(now_us) after the wall-clock has advanced past
one or more AckTracker entry deadline_us values. At least one entry has
state == PENDING and deadline_us <= now_us (its ACK window has expired).
The call sweeps all slots, frees expired PENDING entries, frees any accumulated
ACKED entries as a side effect, logs each timeout as WARNING_HI, and returns the
count of expired PENDING entries.

### Expected outcome (single goal)

All PENDING entries whose ACK deadline has passed are transitioned to FREE.
Each timed-out envelope is logged as a WARNING_HI event. The count of timed-out
entries is returned to the caller. ACKED entries encountered during the sweep are
also freed silently (no count, no log) as a garbage-collection side effect.

---

## 2. Entry Points

Primary entry point (called by application or timer):

    DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, line 267
    Sig:  uint32_t DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)

Prerequisite entry points (must have been called earlier):

    DeliveryEngine::init(TransportInterface*, const ChannelConfig&, NodeId)
      [DeliveryEngine.cpp:17]
    AckTracker::init()
      [AckTracker.cpp:23]
    AckTracker::track(env, deadline_us)
      [AckTracker.cpp:44]
      [Called by DeliveryEngine::send() for RELIABLE_ACK and RELIABLE_RETRY
      messages after a successful transport send.]

Private helper called inside sweep_expired():

    AckTracker::sweep_one_slot(idx, now_us, expired_buf, buf_cap, expired_count)
      [AckTracker.cpp:112]
      Processes one slot: copies expired PENDING envelopes to the output buffer,
      releases ACKED slots silently.
      Returns 1 if an expired PENDING envelope was added to the buffer, 0 otherwise.

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::sweep_ack_timeouts, line 269]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL).
  Guard against calling sweep_ack_timeouts() before init(). Always active.

Step 2  [DeliveryEngine::sweep_ack_timeouts, line 273]
  Declare stack-allocated timeout_buf[ACK_TRACKER_CAPACITY] (32 MessageEnvelope slots).
  No heap allocation — Power of 10 rule 3.
  Stack impact: 32 × sizeof(MessageEnvelope) ≈ 132 KB (see Known Risks).

Step 3  [DeliveryEngine::sweep_ack_timeouts, line 278]
  Call m_ack_tracker.sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY).
  Execution transfers to AckTracker::sweep_expired().

Step 4  [AckTracker::sweep_expired, line 152]
  NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr).
  NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).
  Initialize local: expired_count = 0.

Step 5  [AckTracker::sweep_expired, line 158 — main sweep loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Per iteration:
    expired_count += sweep_one_slot(i, now_us, expired_buf, buf_cap, expired_count).

  Step 5a — Inside sweep_one_slot() [AckTracker.cpp:112]:
    NEVER_COMPILED_OUT_ASSERT(idx < ACK_TRACKER_CAPACITY).
    NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr).

    Branch A — PENDING and deadline passed [lines 121-132]:
      If m_slots[idx].state == EntryState::PENDING
      AND now_us >= m_slots[idx].deadline_us:
        uint32_t added = 0U.
        If expired_count < buf_cap:
          envelope_copy(expired_buf[expired_count], m_slots[idx].env).
          [memcpy of sizeof(MessageEnvelope) bytes; includes full 4096-byte payload.]
          added = 1U.
        [If buf_cap exceeded: slot is still freed but envelope is NOT copied to output.]
        m_slots[idx].state = EntryState::FREE.
        if (m_count > 0U): m_count--.
        return added.  [1 if copied, 0 if buffer was full]

    Branch B — ACKED [lines 134-138]:
      If m_slots[idx].state == EntryState::ACKED:
        [ACK was received previously by on_ack(); state became ACKED.
         sweep_one_slot is the GC path for ACKED entries.]
        m_slots[idx].state = EntryState::FREE.
        if (m_count > 0U): m_count--.
        return 0U.  [ACKED entries NOT counted as expired; not failures]

    Branch C — FREE or not-yet-expired PENDING:
      return 0U.  [no action; deadline has not yet passed]

Step 6  [AckTracker::sweep_expired, lines 162-164]
  NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY).
  NEVER_COMPILED_OUT_ASSERT(expired_count <= buf_cap).
  Return expired_count.

Step 7  [DeliveryEngine::sweep_ack_timeouts, line 279]
  Return value stored in local 'collected'.
  NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY).

Step 8  [DeliveryEngine::sweep_ack_timeouts, lines 283-288 — logging loop]
  Bounded loop: i = 0 .. collected-1.
  Per iteration:
    Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                "ACK timeout for message_id=%llu sent to node=%u",
                timeout_buf[i].message_id, timeout_buf[i].destination_id).

Step 9  [Logger::log (inline)]
  severity_tag(WARNING_HI) → "WARN_HI ".
  snprintf + vsnprintf into 512-byte stack buffer.
  fprintf(stderr, "%s\n", buf).

Step 10  [DeliveryEngine::sweep_ack_timeouts, lines 290-293]
  NEVER_COMPILED_OUT_ASSERT(collected <= ACK_TRACKER_CAPACITY).
  Return collected to caller.

---

## 4. Call Tree (Hierarchical)

DeliveryEngine::sweep_ack_timeouts(now_us)                     [DeliveryEngine.cpp:267]
  [NEVER_COMPILED_OUT_ASSERT m_initialized, now_us > 0]
  AckTracker::sweep_expired(now_us, timeout_buf, 32)           [AckTracker.cpp:147]
    [NEVER_COMPILED_OUT_ASSERT expired_buf!=nullptr, m_count<=32]
    [loop i=0..31]
      sweep_one_slot(i, now_us, expired_buf, buf_cap, expired_count)
        [AckTracker.cpp:112]
        [NEVER_COMPILED_OUT_ASSERT idx<32, expired_buf!=nullptr]
        [if PENDING && now_us >= deadline_us]  — Branch A
          [if expired_count < buf_cap]
            envelope_copy(expired_buf[expired_count], m_slots[idx].env)
              memcpy(&dst, &src, sizeof(MessageEnvelope))      [MessageEnvelope.hpp:56]
          m_slots[idx].state = FREE
          if m_count > 0: m_count--
          return 1 (or 0 if buf full)
        [if ACKED]  — Branch B
          m_slots[idx].state = FREE
          if m_count > 0: m_count--
          return 0
        [else (FREE or not-yet-expired PENDING)]  — Branch C
          return 0
    [NEVER_COMPILED_OUT_ASSERT m_count<=32, expired_count<=32]
    return expired_count
  [NEVER_COMPILED_OUT_ASSERT collected <= 32]
  [loop i=0..collected-1]
    Logger::log(WARNING_HI, "DeliveryEngine",
                "ACK timeout for message_id=…, sent to node=…")
      severity_tag(WARNING_HI) → "WARN_HI "
      snprintf(buf, 512, "[WARN_HI ][DeliveryEngine] ")
      vsnprintf(buf+hdr, 512-hdr, fmt, args)
      fprintf(stderr, "%s\n", buf)
  [NEVER_COMPILED_OUT_ASSERT collected <= 32]
  return collected

---

## 5. Key Components Involved

Component              File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine         src/core/DeliveryEngine.cpp/.hpp      Coordinator: drives
                                                             the sweep; receives
                                                             expired envelope
                                                             copies; logs results
                                                             as WARNING_HI.

AckTracker             src/core/AckTracker.cpp/.hpp          Maintains 32-slot
                                                             fixed table with
                                                             three-state FSM
                                                             (FREE/PENDING/ACKED).
                                                             Delegates per-slot
                                                             logic to private
                                                             sweep_one_slot().

sweep_one_slot()       src/core/AckTracker.cpp:112           Private helper:
                                                             single-slot sweep
                                                             logic; Power of 10
                                                             single-purpose
                                                             function.

AckTracker::Entry      src/core/AckTracker.hpp:79            Per-message state:
                                                             env copy (value),
                                                             deadline_us,
                                                             state enum.

AckTracker::EntryState src/core/AckTracker.hpp:72            Enum: FREE=0,
                                                             PENDING=1, ACKED=2.

MessageEnvelope        src/core/MessageEnvelope.hpp          Data carrier; copied
                                                             by value into
                                                             timeout_buf[].

Logger                 src/core/Logger.hpp                   Fixed-buffer stderr
                                                             sink; WARNING_HI
                                                             severity used for
                                                             each expired entry.

Types.hpp              src/core/Types.hpp                    ACK_TRACKER_CAPACITY
                                                             (32).

---

## 6. Branching Logic / Decision Points

Decision 1 — Slot state: PENDING, ACKED, or FREE?
  Location: AckTracker::sweep_one_slot, lines 121, 134
  Branch A (PENDING): check deadline.
  Branch B (ACKED):   free immediately; return 0 (not a failure event).
  Branch C (FREE or non-expired PENDING): return 0; no action.

Decision 2 — Has the PENDING entry timed out?
  Location: AckTracker::sweep_one_slot, line 122
  Condition: now_us >= m_slots[idx].deadline_us
  True  → copy to expired_buf (if space), free slot, return 1 (or 0 if buf full).
  False → entry remains PENDING; not touched; return 0.

Decision 3 — Is there room in expired_buf?
  Location: AckTracker::sweep_one_slot, line 125
  Condition: expired_count < buf_cap
  True  → envelope_copy() the entry into the output buffer; added = 1.
  False → slot is still freed (state → FREE, m_count--), but the envelope
          is NOT added to the output buffer.
          [RISK: the caller cannot log what it does not receive.]

Decision 4 — m_count underflow guard
  Location: AckTracker::sweep_one_slot, lines 130, 137
  Condition: m_count > 0U before decrement.
  Prevents unsigned integer underflow on m_count.
  [OBSERVATION: a correct implementation should be able to assert m_count > 0
  here; the soft guard suggests the implementation is not fully confident in
  the invariant. See Known Risks.]

Decision 5 — WARNING_HI severity
  Location: DeliveryEngine::sweep_ack_timeouts, line 284
  All timed-out messages are logged at WARNING_HI (system-wide, recoverable).
  Fixed policy; no per-message or per-channel severity override.

---

## 7. Concurrency / Threading Behavior

No synchronization primitives exist in AckTracker or DeliveryEngine.

[ASSUMPTION] sweep_ack_timeouts() is intended to be called from the same thread
as send() and receive(). AckTracker state (m_slots[], m_count) is not protected
by any mutex or std::atomic.

Potential races:
  Race A — receive() calls AckTracker::on_ack() which writes m_slots[i].state = ACKED.
  If sweep_ack_timeouts() runs concurrently on another thread, both read and write
  the same m_slots[i].state in sweep_one_slot(). No synchronization prevents it.
  Concurrent write-write on the same field is undefined behavior under C++17.

  Race B — send() calls AckTracker::track() which writes a FREE slot to PENDING
  and increments m_count. Concurrent sweep() decrements m_count and sets state=FREE.
  Concurrent modification of non-atomic uint32_t m_count is undefined behavior.

Logger concurrency:
  Logger::log() uses a stack-local buf[512]; no shared mutable state.
  fprintf(stderr) is typically thread-safe on POSIX (locked internally), but
  concurrent calls may produce interleaved output lines.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

Allocation model:
  No heap allocation. All buffers are stack-allocated or embedded in statically-
  sized structs.

Stack frame — DeliveryEngine::sweep_ack_timeouts():
  timeout_buf[ACK_TRACKER_CAPACITY]: 32 × sizeof(MessageEnvelope) ≈ 132 KB.
  [OBSERVATION: large stack allocation; see Known Risks.]
  collected: uint32_t (4 bytes).

Stack frame — AckTracker::sweep_one_slot():
  idx, expired_count, added: uint32_t each. Minimal.

Stack frame — Logger::log():
  buf[512]: 512 bytes for formatted log line.
  tag pointer, hdr (int), body (int), va_list args.

AckTracker internal memory (statically embedded in DeliveryEngine):
  m_slots[ACK_TRACKER_CAPACITY]: 32 Entry structs.
    Each Entry: sizeof(MessageEnvelope) + sizeof(uint64_t) + sizeof(EntryState)
    = ~4135 + 8 + 1 + padding bytes ≈ ~4144 bytes per slot.
  Total: ~32 × 4144 ≈ ~133 KB, statically allocated as a member of DeliveryEngine.

Envelope copying in sweep_one_slot():
  envelope_copy() is memcpy(&dst, &src, sizeof(MessageEnvelope)).
  Copies the full envelope including the 4096-byte payload[] array.
  payload[] is value-embedded in the struct; no pointer indirection.

Ownership transfer:
  AckTracker::Entry::env is a deep copy made by envelope_copy() during track().
  After sweep_one_slot(), ownership of timed-out envelopes is logically transferred
  to the timeout_buf[] stack array in sweep_ack_timeouts(). After the logging loop,
  those copies go out of scope and are discarded.

m_count semantics:
  m_count counts non-FREE slots. After a PENDING entry is freed, m_count is
  decremented. After an ACKED entry is freed, m_count is also decremented.
  m_count does not separately count PENDING vs ACKED.

---

## 9. Error Handling Flow

expired_buf == nullptr (precondition violated):
  NEVER_COMPILED_OUT_ASSERT(expired_buf != nullptr) fires in sweep_expired() at
  line 152, and in sweep_one_slot() at line 119. Always active; aborts process.
  In practice, timeout_buf is always stack-allocated by sweep_ack_timeouts() and
  cannot be null.

m_count underflow (m_count == 0 when decrement attempted):
  Guard in sweep_one_slot(): `if (m_count > 0U) { --m_count; }` at lines 130/137.
  Prevents unsigned underflow without an assertion failure.
  [OBSERVATION: if m_count is 0 but a slot is found in PENDING or ACKED state,
  the guard masks an internal inconsistency. A correct implementation should
  assert m_count > 0 before decrement rather than silently skipping it.]

Logger format error (snprintf returns < 0):
  Logger::log() checks hdr < 0 and body < 0; returns silently.
  The log line is dropped; no other action taken.

Output buffer overflow (expired_count >= buf_cap before all expired PENDING
entries are processed):
  Affected entries are still freed (state → FREE, m_count--).
  Their envelopes are NOT copied to the output buffer.
  DeliveryEngine's logging loop only sees entries that fit in the buffer.
  In practice, buf_cap == ACK_TRACKER_CAPACITY (32) == the table size, so this
  cannot happen under current constants.

m_initialized == false:
  NEVER_COMPILED_OUT_ASSERT(m_initialized) at sweep_ack_timeouts() line 269 —
  always active; aborts process.

---

## 10. External Interactions

1. Logger::log() / fprintf(stderr)
   Called once per timed-out entry inside the logging loop in
   sweep_ack_timeouts() at lines 284-287 (WARNING_HI severity).
   Fields logged: message_id (uint64_t) and destination_id (NodeId/uint32_t).
   Full payload is NOT logged (per CLAUDE.md §7.1).
   Uses a 512-byte stack buffer; output goes to stderr.

2. No transport interaction
   sweep_ack_timeouts() does not call the transport layer.
   It does not send NAKs, does not notify the remote peer, and does not trigger
   retries. It only frees local tracking state and logs.
   [OBSERVATION: if the caller wants to trigger a NAK or retry on ACK timeout,
   it must do so itself using the returned count — but the timeout_buf containing
   envelope copies goes out of scope after sweep_ack_timeouts() returns, so the
   information is lost. The caller only receives the count.]

3. No RetryManager interaction
   sweep_ack_timeouts() does not call RetryManager. RetryManager independently
   tracks retry state; it is not notified when AckTracker frees a PENDING entry
   due to timeout. Both can independently hold state for the same message_id.

---

## 11. State Changes / Side Effects

Object               Field               Before                After
─────────────────────────────────────────────────────────────────────────────
AckTracker::Entry    state               PENDING (expired)     FREE
AckTracker::Entry    state               ACKED                 FREE
                                         (ACKED swept as GC side effect;
                                         not counted in return value)
AckTracker           m_count             N                     N - K
                                                               K = freed entries
                                                               (expired PENDING
                                                               + ACKED swept)
DeliveryEngine       timeout_buf         uninitialized         contains copies
                     (stack local)                             of timed-out
                                                               envelopes
                                                               (lifetime: within
                                                               sweep_ack_timeouts)
stderr               log output          —                     one WARNING_HI
                                                               line per timed-out
                                                               PENDING entry

---

## 12. Sequence Diagram (ASCII)

```
Caller (app loop)    DeliveryEngine      AckTracker           Logger (stderr)
      |                    |                  |                    |
      | sweep_ack_timeouts |                  |                    |
      | (now_us)           |                  |                    |
      |------------------->|                  |                    |
      |                    | sweep_expired(   |                    |
      |                    |  now_us, buf, 32)|                    |
      |                    |----------------->|                    |
      |                    |                  | [loop i=0..31]     |
      |                    |                  | sweep_one_slot()   |
      |                    |                  |                    |
      |                    |                  | [PENDING, expired] |
      |                    |                  | envelope_copy(     |
      |                    |                  |  buf[0], slot.env) |
      |                    |                  | state = FREE       |
      |                    |                  | m_count--          |
      |                    |                  | return 1           |
      |                    |                  |                    |
      |                    |                  | [ACKED slot]       |
      |                    |                  | state = FREE       |
      |                    |                  | m_count--          |
      |                    |                  | return 0           |
      |                    |                  |                    |
      |                    |<-----------------|                    |
      |                    | returns K (expired PENDING count)     |
      |                    |                  |                    |
      |                    | [loop i=0..K-1]  |                    |
      |                    | Logger::log(     |                    |
      |                    |  WARNING_HI,     |                    |
      |                    |  "ACK timeout…") |                    |
      |                    |-------------------------------------------->
      |                    |                  |   [WARN_HI][…]\n   |
      | returns K          |                  |                    |
      |<-------------------|                  |                    |
```

---

## 13. Initialization vs Runtime Flow

Initialization path (called once at startup):

  DeliveryEngine::init()               [DeliveryEngine.cpp:17]
    m_ack_tracker.init()               [AckTracker.cpp:23]
      memset(m_slots, 0, sizeof(m_slots))  [line 26]
        [32 × Entry zeroed: env=0, deadline_us=0, state=0 (== FREE)]
        [EntryState::FREE == 0U; memset(0) is correct for this enum.]
      m_count = 0
      NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
      [verify loop: all 32 slots assert state == FREE]

Runtime setup — AckTracker::track() (called inside DeliveryEngine::send()):
  [AckTracker.cpp:44]
  Finds first FREE slot.
  envelope_copy(slot.env, env) — deep copy of the outgoing envelope.
  slot.deadline_us = timestamp_deadline_us(now_us, recv_timeout_ms)
    [Timestamp.hpp:65: = now_us + (recv_timeout_ms * 1000ULL).]
  slot.state = PENDING.
  m_count++.

Runtime path — sweep (called periodically):
  DeliveryEngine::sweep_ack_timeouts(now_us) — as documented above.

Key distinction:
  AckTracker memory is statically embedded in DeliveryEngine; no allocation at
  runtime. track() (adding entries) and sweep_one_slot() (removing entries) are
  the only two mutators of m_slots[] state, plus on_ack() which transitions
  PENDING → ACKED. sweep_one_slot() is the only function that transitions
  any slot to FREE.

---

## 14. Known Risks / Observations

Risk 1 — Large stack allocation in sweep_ack_timeouts()
  timeout_buf[32] of MessageEnvelope ≈ 32 × 4135 bytes ≈ 132 KB on the stack.
  On an embedded system with a constrained stack this could cause stack overflow.
  See docs/STACK_ANALYSIS.md for analysis of the worst-case call chain.

Risk 2 — Timed-out envelopes are discarded after logging
  The timeout_buf[] is stack-local. After sweep_ack_timeouts() returns, the
  envelope copies are gone. If the application needs to re-queue, notify a peer,
  or take corrective action beyond logging, it has no way to access the envelopes.
  The return value (count) is the only signal.
  [OBSERVATION: for RELIABLE_RETRY messages, RetryManager may still be tracking
  the same entry and will independently exhaust retries. For RELIABLE_ACK messages
  with no retry, the failure is silent after the log line.]

Risk 3 — AckTracker does not notify RetryManager on timeout
  When an AckTracker entry times out, RetryManager is not informed. Both can
  independently hold state for the same message_id (RELIABLE_RETRY path).
  A message can be declared an ACK timeout by AckTracker while RetryManager
  continues to retry it.

Risk 4 — ACKED entries are swept silently
  sweep_one_slot() frees ACKED entries as a side effect, returning 0 for them
  (not counted in expired_count). The caller (DeliveryEngine) has no visibility
  into how many ACKED entries were garbage-collected during the sweep.

Risk 5 — m_count inconsistency guard masks bugs
  The guard `if (m_count > 0U) { --m_count; }` in sweep_one_slot() at lines 130
  and 137 prevents underflow but does not assert that m_count > 0. A correct
  implementation should be able to assert m_count > 0 here; the soft guard
  suggests the implementation is not fully confident in the invariant.

Risk 6 — deadline_us == 0 causes immediate expiry
  If track() is called with deadline_us == 0, the entry will immediately appear
  expired on the very first sweep_expired() call (now_us >= 0 is always true).
  This would cause the message to be treated as timed out before any real time
  elapses. [ASSUMPTION: callers always provide a valid future deadline_us.]

---

## 15. Unknowns / Assumptions

[ASSUMPTION 1] Single-threaded call model
  sweep_ack_timeouts(), send(), and receive() are assumed to be called from the
  same thread. No synchronization evidence found in the codebase.

[ASSUMPTION 2] deadline_us computed via timestamp_deadline_us()
  DeliveryEngine::send() line 106 calls timestamp_deadline_us(now_us, recv_timeout_ms).
  Confirmed in Timestamp.hpp:65: returns now_us + (duration_ms * 1000ULL).

[ASSUMPTION 3] memset(0) correctly initializes EntryState::FREE
  AckTracker::init() uses memset(m_slots, 0, ...). EntryState::FREE == 0U
  (per AckTracker.hpp:73). This relies on the enum's underlying representation
  being 0 for FREE, which is explicitly stated in the code.

[ASSUMPTION 4] ACK_TRACKER_CAPACITY == buf_cap in all production calls
  sweep_ack_timeouts() always passes ACK_TRACKER_CAPACITY as buf_cap. The buffer
  overflow guard inside sweep_one_slot() is therefore never exercised unless
  constants change.

[ASSUMPTION 5] WARNING_HI is the correct severity for ACK timeout
  Per Types.hpp, WARNING_HI means "system-wide but recoverable." A single ACK
  timeout is considered system-wide but the component can continue operating.
  This is consistent with CLAUDE.md §8 error handling philosophy.

[ASSUMPTION 6] No NAK or retry is triggered by sweep_ack_timeouts()
  The function only logs and returns. It does not send NAKs and does not call
  RetryManager. Any upper-layer response to ACK timeout must be implemented by
  the caller using the returned count.

[ASSUMPTION 7] ACKED entry count from sweep is not needed by callers
  The return value of sweep_expired() is expired_count (expired PENDING only).
  ACKED garbage-collection is a side effect with no separate count. Callers are
  assumed not to need this information.
