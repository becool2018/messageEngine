## 1. Use Case Overview

Use Case ID : UC-11
Title       : AckTracker sweeps timed-out entries and logs unacknowledged
              messages as failures
Layer       : Core (DeliveryEngine / AckTracker)
Trigger     : A periodic caller (application main loop or timer thread)
              invokes DeliveryEngine::sweep_ack_timeouts(now_us) after the
              wall-clock has advanced past one or more entry deadline_us values.
Preconditions:
  - DeliveryEngine::init() has been called; m_initialized == true.
  - AckTracker::init() has been called; m_count may be > 0.
  - At least one AckTracker entry exists with state == PENDING and
    deadline_us <= now_us (i.e., the ACK window has expired for that message).
Postconditions:
  - All PENDING entries with deadline_us <= now_us have been transitioned
    to state FREE.
  - m_count has been decremented for each freed entry.
  - Each timed-out envelope has been logged as WARNING_HI by DeliveryEngine.
  - ACKED entries encountered during the sweep are also freed (state FREE,
    m_count decremented) as a side effect of the sweep.
  - The count of timed-out entries is returned to the caller.
Requirement traceability:
  - CLAUDE.md §3.2 "ACK handling: Tracking outstanding messages awaiting ACK"
  - CLAUDE.md §3.2 "Expiry handling: Drop or deprioritize expired messages"
  - CLAUDE.md §7.1 "Logging: Errors … with enough context to debug"
  - CLAUDE.md §3.3 "Reliable with ACK: treat missing ACK as failure"


## 2. Entry Points

Primary entry point (called by application or timer):
  DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)
    File : src/core/DeliveryEngine.cpp, line 267
    Sig  : uint32_t DeliveryEngine::sweep_ack_timeouts(uint64_t now_us)

Prerequisite entry points (must have been called earlier):
  DeliveryEngine::init(TransportInterface*, const ChannelConfig&, NodeId)
    File : src/core/DeliveryEngine.cpp, line 15
  AckTracker::init()
    File : src/core/AckTracker.cpp, line 21
  AckTracker::track(env, deadline_us)
    File : src/core/AckTracker.cpp, line 42
    [Called by DeliveryEngine::send() for RELIABLE_ACK and RELIABLE_RETRY
    messages after a successful transport send.]


## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::sweep_ack_timeouts, line 269]
  Assert m_initialized == true.
  Assert now_us > 0.

Step 2  [DeliveryEngine::sweep_ack_timeouts, line 273]
  Declare stack-allocated timeout_buf[ACK_TRACKER_CAPACITY] (32 MessageEnvelope
  slots). No heap allocation — Power of 10 rule 3.

Step 3  [DeliveryEngine::sweep_ack_timeouts, line 278]
  Call m_ack_tracker.sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY).
  Execution transfers to AckTracker::sweep_expired().

Step 4  [AckTracker::sweep_expired, line 142]
  Assert expired_buf != nullptr.
  Assert buf_cap <= 0xFFFFFFFF (trivially true for uint32_t).
  Assert m_count <= ACK_TRACKER_CAPACITY.
  Initialize local: expired_count = 0.

Step 5  [AckTracker::sweep_expired, line 155 — main sweep loop]
  Iterate i = 0 .. ACK_TRACKER_CAPACITY-1 (32 iterations, fixed bound).
  Per-iteration assertions:
    assert i < ACK_TRACKER_CAPACITY.
    assert m_count <= ACK_TRACKER_CAPACITY.

  Per-slot processing:

  Step 5a  [lines 161-176 — PENDING branch]
    If m_slots[i].state == EntryState::PENDING:
      Check deadline: if now_us >= m_slots[i].deadline_us:

        Step 5a-i  [lines 166-169 — copy to output buffer if space]
          If expired_count < buf_cap:
            Call envelope_copy(expired_buf[expired_count], m_slots[i].env).
            [memcpy of sizeof(MessageEnvelope) bytes]
            Increment expired_count.
          [If buf_cap exceeded, the entry is still freed but not copied.]

        Step 5a-ii  [lines 172-175 — free the slot]
          Set m_slots[i].state = EntryState::FREE.
          If m_count > 0: decrement m_count.

      [If now_us < deadline_us: slot is still waiting; no action.]

  Step 5b  [lines 179-184 — ACKED branch (side effect)]
    Else if m_slots[i].state == EntryState::ACKED:
      [ACK was received previously by on_ack(), state became ACKED.
       sweep_expired() is the garbage-collection path for ACKED entries.]
      Set m_slots[i].state = EntryState::FREE.
      If m_count > 0: decrement m_count.
      [ACKED entries are NOT copied to expired_buf; they are not failures.]

  Step 5c  [FREE slots]
    Else (state == FREE): no action taken.

Step 6  [AckTracker::sweep_expired, lines 189-191]
  Post-condition assertions:
    assert m_count <= ACK_TRACKER_CAPACITY.
    assert expired_count <= buf_cap.
    assert expired_count <= ACK_TRACKER_CAPACITY.
  Return expired_count.

Step 7  [DeliveryEngine::sweep_ack_timeouts, line 279]
  Return value stored in local 'collected'.
  Assert collected <= ACK_TRACKER_CAPACITY.

Step 8  [DeliveryEngine::sweep_ack_timeouts, lines 283-290 — logging loop]
  Bounded loop: i = 0 .. collected-1.
  Per iteration:
    Assert i < ACK_TRACKER_CAPACITY.
    Call Logger::log(Severity::WARNING_HI, "DeliveryEngine",
                     "ACK timeout for message_id=%llu sent to node=%u",
                     timeout_buf[i].message_id, timeout_buf[i].destination_id).

Step 9  [Logger::log (inline), Logger.hpp:37]
  Retrieve severity tag: "WARN_HI".
  Format header into 512-byte stack buf via snprintf().
  Format body via vsnprintf() with va_list.
  Write to stderr via fprintf().

Step 10  [DeliveryEngine::sweep_ack_timeouts, lines 293-295]
  Post-condition assertion: assert collected <= ACK_TRACKER_CAPACITY.
  Return collected to caller.


## 4. Call Tree (Hierarchical)

DeliveryEngine::sweep_ack_timeouts(now_us)
  [assert m_initialized, assert now_us > 0]
  AckTracker::sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY)
    [assert expired_buf != nullptr]
    [assert m_count <= ACK_TRACKER_CAPACITY]
    [loop i = 0..31]
      [if PENDING && now_us >= deadline_us]
        envelope_copy(expired_buf[expired_count], m_slots[i].env)
          memcpy(&dst, &src, sizeof(MessageEnvelope))   [MessageEnvelope.hpp:51]
        m_slots[i].state = FREE
        m_count--
      [if ACKED]
        m_slots[i].state = FREE
        m_count--
    [return expired_count]
  [assert collected <= ACK_TRACKER_CAPACITY]
  [loop i = 0..collected-1]
    Logger::log(WARNING_HI, "DeliveryEngine",
                "ACK timeout for message_id=…, sent to node=…")
      severity_tag(WARNING_HI) → "WARN_HI "   [Logger.hpp:67]
      snprintf(buf, 512, "[WARN_HI ][DeliveryEngine] ")
      vsnprintf(buf+hdr, 512-hdr, fmt, args)
      fprintf(stderr, "%s\n", buf)
  [assert collected <= ACK_TRACKER_CAPACITY]
  [return collected]


## 5. Key Components Involved

Component             File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine        src/core/DeliveryEngine.cpp/.hpp      Coordinator; drives
                                                            sweep; logs results
                                                            as WARNING_HI.

AckTracker            src/core/AckTracker.cpp/.hpp          Maintains fixed
                                                            table of 32 Entry
                                                            slots with three-
                                                            state FSM
                                                            (FREE/PENDING/ACKED).

AckTracker::Entry     src/core/AckTracker.hpp:71-75         Per-message state:
                                                            env copy,
                                                            deadline_us,
                                                            state.

AckTracker::EntryState src/core/AckTracker.hpp:64-68        Enum: FREE=0,
                                                            PENDING=1, ACKED=2.

MessageEnvelope       src/core/MessageEnvelope.hpp          Data carrier; copied
                                                            by value into
                                                            output buffer.

Logger                src/core/Logger.hpp                   Fixed-buffer stderr
                                                            sink; WARNING_HI
                                                            severity used.

Types.hpp             src/core/Types.hpp                    Constants:
                                                            ACK_TRACKER_CAPACITY
                                                            (32).


## 6. Branching Logic / Decision Points

Decision 1 — Slot state: PENDING, ACKED, or FREE?
  Location: AckTracker::sweep_expired, lines 161, 179
  Branch A (PENDING): check deadline.
  Branch B (ACKED):   free immediately; do not add to expired_buf.
  Branch C (FREE):    no action.

Decision 2 — Has the PENDING entry timed out?
  Location: AckTracker::sweep_expired, line 163
  Condition: now_us >= m_slots[i].deadline_us
  True  → copy to expired_buf (if space), free slot
  False → entry remains PENDING; not touched

Decision 3 — Is there room in expired_buf?
  Location: AckTracker::sweep_expired, line 166
  Condition: expired_count < buf_cap
  True  → envelope_copy() the entry into the output buffer
  False → slot is still freed (state→FREE, m_count--), but the envelope
          is silently dropped without being added to the output buffer.
          [RISK: callers cannot log what they do not receive.]

Decision 4 — m_count underflow guard
  Location: AckTracker::sweep_expired, lines 173, 181
  Condition: m_count > 0 before decrement
  Prevents unsigned integer underflow on m_count.

Decision 5 — WARNING_HI vs other severity
  Location: DeliveryEngine::sweep_ack_timeouts, line 286
  All timed-out messages are logged at WARNING_HI (system-wide recoverable).
  This is a fixed policy; no per-message or per-channel severity override.


## 7. Concurrency / Threading Behavior

No synchronization primitives exist in AckTracker or DeliveryEngine.

Observed design intent:
  - sweep_ack_timeouts() is intended to be called from the same thread as
    send() and receive(). [ASSUMPTION]
  - AckTracker state (m_slots[], m_count) is not protected by any mutex
    or std::atomic.

Potential races:
  - receive() calls AckTracker::on_ack() which writes m_slots[i].state =
    ACKED. If sweep_ack_timeouts() runs concurrently on another thread,
    it reads and writes the same m_slots[i].state in the loop. No
    synchronization would protect the read-modify-write sequence.
  - send() calls AckTracker::track() which writes a FREE slot to PENDING
    and increments m_count. Concurrent sweep() decrements m_count and
    sets state=FREE. Concurrent modification of m_count (uint32_t, non-
    atomic) is undefined behavior under C++17.
  - [ASSUMPTION: single-threaded operation is the correct call model for
    these components.]

Logger thread safety:
  - Logger::log() uses a stack-local buf[512] and calls fprintf(stderr).
    fprintf is typically thread-safe on POSIX platforms (locked internally),
    but concurrent calls can produce interleaved output lines. Not a
    correctness issue for this use case.


## 8. Memory & Ownership Semantics (C/C++ Specific)

Allocation model:
  - No heap allocation. All buffers are stack-allocated or embedded in
    statically-sized structs.

Stack frame — DeliveryEngine::sweep_ack_timeouts():
  - timeout_buf[ACK_TRACKER_CAPACITY]: 32 x sizeof(MessageEnvelope).
    Approximately 32 x 4134 = ~132,288 bytes. [ASSUMPTION on sizeof;
    see Known Risks.]
  - collected: uint32_t (4 bytes)

Stack frame — Logger::log():
  - buf[512]: 512 bytes for formatted log line.
  - tag pointer, hdr (int), body (int), va_list args.

AckTracker internal memory:
  - m_slots[ACK_TRACKER_CAPACITY]: 32 Entry structs.
    Each Entry: sizeof(MessageEnvelope) + sizeof(uint64_t) + sizeof(EntryState).
    deadline_us (8 bytes) + state (1 byte, uint8_t) + padding.
  - Total: ~32 x (4134 + 8 + 1 + padding) ≈ ~132,000+ bytes as a
    member of DeliveryEngine. This is statically allocated at
    DeliveryEngine object construction.

Envelope copying in sweep_expired:
  - envelope_copy() is memcpy(&dst, &src, sizeof(MessageEnvelope)).
  - Copies the full envelope including the 4096-byte payload array.
  - The payload[] is value-embedded in the struct; no pointers are copied.

Ownership transfer:
  - AckTracker::Entry::env is a deep copy made by envelope_copy() during
    track(). The RetryManager and AckTracker each hold independent copies.
  - After sweep_expired(), ownership of timed-out envelopes is logically
    transferred to the timeout_buf[] stack array in sweep_ack_timeouts().
    After loop iteration (logging), those copies go out of scope and are
    discarded. No further use is made of them.

m_count semantics:
  - m_count counts non-FREE slots. After a PENDING entry is freed, m_count
    is decremented. After an ACKED entry is freed, m_count is also
    decremented. m_count does not separately count PENDING vs ACKED.


## 9. Error Handling Flow

Error source → handling action

expired_buf == nullptr (precondition violated):
  - assert(expired_buf != nullptr) fires in sweep_expired(), line 147.
  - Debug build: abort. Release build: undefined behavior.
  - In practice, timeout_buf is always stack-allocated by
    sweep_ack_timeouts() and cannot be null.

m_count underflow (m_count == 0 when decrement attempted):
  - Guard at lines 173 and 181: `if (m_count > 0U) m_count--`.
  - Prevents unsigned underflow without assertion failure.
  - [OBSERVATION: if m_count is 0 but a slot is found in PENDING or ACKED
    state, the guard masks an internal inconsistency. This should be an
    assert, not a silent guard.]

Logger format error (snprintf returns < 0):
  - Logger::log() checks hdr < 0 and body < 0; returns silently.
  - The log line is dropped; no other action taken.

Output buffer overflow (expired_count >= buf_cap before all PENDING
expired entries are processed):
  - The affected entries are still freed (state→FREE, m_count--).
  - Their envelopes are NOT copied to the output buffer.
  - DeliveryEngine's logging loop only sees the entries that fit in the
    buffer. Lost entries are not logged at WARNING_HI.
  - In practice, buf_cap == ACK_TRACKER_CAPACITY (32) == the table size,
    so this cannot happen under current constants: the number of expired
    entries is at most the table size, which equals buf_cap.

m_initialized == false:
  - sweep_ack_timeouts() asserts m_initialized at line 269.
  - Debug: abort. Release: continues with potentially garbage state.


## 10. External Interactions

1. Logger::log() / fprintf(stderr)
   - Called once per timed-out entry inside the logging loop in
     sweep_ack_timeouts().
   - Fields logged: message_id (uint64_t) and destination_id (NodeId/uint32_t).
   - Full payload is NOT logged (per CLAUDE.md §7.1 "Avoid logging full
     payload contents by default").
   - Uses a 512-byte stack buffer; output goes to stderr.

2. No transport interaction
   - sweep_ack_timeouts() does not call the transport layer.
   - It does not send NAKs, does not notify the remote peer, and does not
     trigger retries. It only frees local tracking state and logs.
   - [OBSERVATION: if the caller wants to trigger a NAK or retry on ACK
     timeout, it must do so itself using the returned count and the
     previously stored envelopes — but since timeout_buf goes out of scope
     after sweep_ack_timeouts() returns, the information is gone. See
     Known Risks.]

3. No RetryManager interaction
   - sweep_ack_timeouts() does not call RetryManager.
   - RetryManager independently tracks retry state; it is not notified
     when AckTracker frees a PENDING entry due to timeout.


## 11. State Changes / Side Effects

Object               Field               Before                After
─────────────────────────────────────────────────────────────────────────────
AckTracker::Entry    state               PENDING (expired)     FREE
AckTracker::Entry    state               ACKED                 FREE
                     (ACKED swept as a side effect)
AckTracker           m_count             N                     N - K
                                                               K = freed entries
                                                               (PENDING expired
                                                               + ACKED swept)
DeliveryEngine       timeout_buf         uninitialized         contains copies
                     (stack local)                             of timed-out
                                                               envelopes
                                                               (lifetime:
                                                               within
                                                               sweep_ack_timeouts)
stderr               log output          —                     one WARNING_HI
                                                               line per timed-out
                                                               entry in timeout_buf


## 12. Sequence Diagram (ASCII)

Caller (app loop)      DeliveryEngine      AckTracker         Logger (stderr)
      |                      |                  |                   |
      | sweep_ack_timeouts   |                  |                   |
      | (now_us)             |                  |                   |
      |--------------------->|                  |                   |
      |                      | sweep_expired(   |                   |
      |                      |  now_us, buf, 32)|                   |
      |                      |----------------->|                   |
      |                      |                  | [loop i=0..31]    |
      |                      |                  | [slot i: PENDING, |
      |                      |                  |  now>=deadline]   |
      |                      |                  | envelope_copy(    |
      |                      |                  |  buf[0], slot.env)|
      |                      |                  | state = FREE      |
      |                      |                  | m_count--         |
      |                      |                  |                   |
      |                      |                  | [slot j: ACKED]   |
      |                      |                  | state = FREE      |
      |                      |                  | m_count--         |
      |                      |<-----------------|                   |
      |                      |  returns K (expired count)           |
      |                      |                  |                   |
      |                      | [loop i=0..K-1]  |                   |
      |                      |  Logger::log(    |                   |
      |                      |  WARNING_HI,     |                   |
      |                      |  "ACK timeout …")|                   |
      |                      |---------------------------------->    |
      |                      |                  |  [WARN_HI][…]\n   |
      |                      |                  |                   |
      |  returns K           |                  |                   |
      |<---------------------|                  |                   |


## 13. Initialization vs Runtime Flow

Initialization path (called once at startup):

  DeliveryEngine::init()          [DeliveryEngine.cpp:15]
    m_ack_tracker.init()          [AckTracker.cpp:21]
      memset(m_slots, 0, sizeof(m_slots))
      m_count = 0
      [verify loop: all slots assert EntryState::FREE]
        [EntryState::FREE == 0, so memset(0) achieves correct initial state]

Runtime setup — AckTracker::track() (called inside DeliveryEngine::send()):
  [AckTracker.cpp:42]
  - Finds first FREE slot.
  - envelope_copy(slot.env, env).
  - slot.deadline_us = ack_deadline (computed as now_us +
    recv_timeout_ms * 1000 by timestamp_deadline_us() in DeliveryEngine::send,
    line 104).
  - slot.state = PENDING.
  - m_count++.

Runtime path — sweep (called periodically):
  DeliveryEngine::sweep_ack_timeouts(now_us) — as documented above.

Key distinction:
  - AckTracker memory is statically embedded in DeliveryEngine; no
    allocation at runtime.
  - track() (adding entries) and sweep_expired() (removing entries) are
    the only two mutators of m_slots[] state, plus on_ack() which
    transitions PENDING→ACKED.
  - sweep_expired() is the only function that transitions to FREE.


## 14. Known Risks / Observations

Risk 1 — Large stack allocation in sweep_ack_timeouts()
  timeout_buf[32] of MessageEnvelope ≈ 32 x 4134 = ~132,288 bytes on
  the stack. On an embedded system with a constrained stack this could
  cause stack overflow. [OBSERVATION: consistent with pump_retries risk.]

Risk 2 — Timed-out envelopes are discarded after logging
  The timeout_buf[] is stack-local. After sweep_ack_timeouts() returns,
  the envelope copies are gone. If the application needs to re-queue,
  notify a peer, or take corrective action beyond logging, it has no way
  to access the envelopes. The return value (count) is the only signal.
  [OBSERVATION: for RELIABLE_RETRY messages, the RetryManager may still
  be tracking the same entry and will independently exhaust retries.
  For RELIABLE_ACK messages with no retry, the failure is silent after
  the log line.]

Risk 3 — AckTracker does not notify RetryManager on timeout
  When an AckTracker entry times out, RetryManager is not informed.
  Both can independently hold state for the same message_id (RELIABLE_RETRY
  path). This means a message can be declared an ACK timeout by AckTracker
  while RetryManager continues to retry it. The two subsystems may thus
  disagree on the message's status simultaneously.

Risk 4 — ACKED entries are swept silently
  sweep_expired() frees ACKED entries as a side effect without any
  corresponding log line or count contribution to the return value.
  The caller (DeliveryEngine) has no visibility into how many ACKED entries
  were garbage-collected during the sweep.

Risk 5 — m_count inconsistency guard masks bugs
  The guard `if (m_count > 0U) m_count--` at lines 173 and 181 prevents
  underflow but does not assert that m_count > 0 before decrement. A
  correct implementation should be able to assert m_count > 0 here; the
  soft guard suggests the implementation is not fully confident in the
  invariant.

Risk 6 — deadline_us == 0 semantics undefined
  If track() is called with deadline_us == 0, the entry will immediately
  appear expired on the very first sweep_expired() call where now_us >= 0
  (always true). This would cause the message to be treated as timed out
  before any real time elapses. [ASSUMPTION: callers always provide a
  valid future deadline_us.]


## 15. Unknowns / Assumptions

[ASSUMPTION 1] Single-threaded call model
  sweep_ack_timeouts(), send(), and receive() are assumed to be called
  from the same thread. No synchronization evidence found.

[ASSUMPTION 2] deadline_us computed via timestamp_deadline_us()
  DeliveryEngine::send() line 104 calls timestamp_deadline_us(now_us,
  recv_timeout_ms). This function was not read (Timestamp.hpp was not
  in the file list). Assumed to compute now_us + (recv_timeout_ms * 1000).

[ASSUMPTION 3] memset(0) correctly initializes EntryState::FREE
  AckTracker::init() uses memset(m_slots, 0, ...). EntryState::FREE == 0U
  (per AckTracker.hpp:65). This relies on the enum's underlying
  representation being 0 for FREE, which is stated explicitly in the code.

[ASSUMPTION 4] ACK_TRACKER_CAPACITY == buf_cap in all production calls
  sweep_ack_timeouts() always passes ACK_TRACKER_CAPACITY as buf_cap. The
  buffer overflow guard inside sweep_expired() is therefore never exercised
  unless constants change.

[ASSUMPTION 5] WARNING_HI is the correct severity for ACK timeout
  Per Types.hpp, WARNING_HI means "system-wide but recoverable." This
  implies a single ACK timeout is considered system-wide but the
  component can continue operating. This is consistent with CLAUDE.md
  §8 error handling philosophy.

[ASSUMPTION 6] No NAK or retry is triggered by sweep_ack_timeouts
  The function only logs and returns. It does not send NAKs and does not
  call RetryManager. Any upper-layer response to ACK timeout must be
  implemented by the caller.

[ASSUMPTION 7] ACKED entry count from sweep is not needed by callers
  The return value of sweep_expired() is expired_count (PENDING timeouts
  only). ACKED garbage-collection is a side effect with no separate count.
  Callers are assumed to not need this information.
```

---