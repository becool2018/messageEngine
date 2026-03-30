# UC_07 — Duplicate message drop

**HL Group:** HL-6 — System suppresses duplicate messages
**Actor:** System
**Requirement traceability:** REQ-3.2.6, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

A sender operating in RELIABLE_RETRY mode has retransmitted a DATA message that the receiver has already seen and processed. The receiver's DeliveryEngine::receive() pulls the retransmitted envelope from the transport layer and passes it to DuplicateFilter::check_and_record(). The filter performs a linear scan of its 128-slot circular window, finds a matching (source_id, message_id) pair, and returns Result::ERR_DUPLICATE. DeliveryEngine propagates ERR_DUPLICATE to the caller without delivering the envelope to the application.

This use case begins at DeliveryEngine::receive() after the transport layer has already successfully deserialized and delivered a valid, non-expired DATA envelope. The platform-layer framing path (UC_06) is complete before this use case starts.

### Expected outcome (single goal)

The duplicate envelope is silently suppressed — the application is not called, no state is modified in DuplicateFilter, and DeliveryEngine returns Result::ERR_DUPLICATE to the caller.

---

## 2. Entry Points

Primary entry point (called by application receive loop):

    DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
    File: src/core/DeliveryEngine.cpp, line 149
    Sig:  Result DeliveryEngine::receive(MessageEnvelope&, uint32_t, uint64_t)

The deduplication sub-path is entered at line 203:

    Result dedup_res = m_dedup.check_and_record(env.source_id, env.message_id);

Sub-entry points within DuplicateFilter (called in sequence from check_and_record):

    DuplicateFilter::check_and_record(NodeId src, uint64_t msg_id)  [DuplicateFilter.cpp:93]
    DuplicateFilter::is_duplicate(NodeId src, uint64_t msg_id)      [DuplicateFilter.cpp:40]
    DuplicateFilter::record(NodeId src, uint64_t msg_id)            [DuplicateFilter.cpp:63]
      [record() is NOT called on the duplicate path; listed for contrast only]

Prerequisite entry points (must have been called earlier for dedup to fire):

    DeliveryEngine::init()       [DeliveryEngine.cpp:17]
    DuplicateFilter::init()      [DuplicateFilter.cpp:23]
      [called from DeliveryEngine::init(); zeroes m_window[128], m_next=0, m_count=0]
    DuplicateFilter::check_and_record() — called once on first receipt, recording the entry

---

## 3. End-to-End Control Flow (Step-by-Step)

Step 1  [DeliveryEngine::receive, line 153]
  NEVER_COMPILED_OUT_ASSERT(m_initialized).
  NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL).
  Guard against calling receive() before init(). Always active; not NDEBUG-gated.

Step 2  [DeliveryEngine::receive, line 161]
  Call m_transport->receive_message(env, timeout_ms).
  Virtual dispatch to TcpBackend (or LocalSimHarness in tests).
  Returns Result::OK with env populated with the retransmitted DATA envelope.
  env.message_type          = MessageType::DATA
  env.reliability_class     = ReliabilityClass::RELIABLE_RETRY
  env.source_id             = <original sender's NodeId>
  env.message_id            = <same ID as the previously-seen first transmission>
  env.expiry_time_us        = <future value or 0 (never expires)>

Step 3  [DeliveryEngine::receive, line 162]
  Check res != OK.
  For this use case: res == OK; execution continues.

Step 4  [DeliveryEngine::receive, line 168]
  NEVER_COMPILED_OUT_ASSERT(envelope_valid(env)).
  envelope_valid checks: message_type != INVALID, payload_length <= 4096,
  source_id != NODE_ID_INVALID.
  All conditions pass for the retransmitted DATA envelope.

Step 5  [DeliveryEngine::receive, lines 171-175 — expiry check]
  Call timestamp_expired(env.expiry_time_us, now_us).
  [Timestamp.hpp:51: returns (expiry_time_us != 0ULL) && (now_us >= expiry_time_us).]
  For this use case: the message is not yet expired.
  Condition evaluates false; execution continues.

Step 6  [DeliveryEngine::receive, line 179]
  Call envelope_is_control(env).
  [MessageEnvelope.hpp:79: returns true only for ACK, NAK, or HEARTBEAT.]
  env.message_type == DATA → returns false.
  Control handling branch skipped.

Step 7  [DeliveryEngine::receive, line 198]
  NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env)).
  [MessageEnvelope.hpp:72: message_type == DATA.] Passes.

Step 8  [DeliveryEngine::receive, line 201 — reliability class gate]
  Check: env.reliability_class == ReliabilityClass::RELIABLE_RETRY.
  For this use case: TRUE. Dedup is applied.
  [BEST_EFFORT and RELIABLE_ACK messages skip dedup entirely.]

Step 9  [DeliveryEngine::receive, line 203]
  Call m_dedup.check_and_record(env.source_id, env.message_id).
  Execution transfers to DuplicateFilter::check_and_record().

Step 10  [DuplicateFilter::check_and_record, line 96]
  NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE).
  [Precondition: window count is consistent with capacity. Always passes.]

Step 11  [DuplicateFilter::check_and_record, line 99]
  Call is_duplicate(src, msg_id).
  Execution transfers to DuplicateFilter::is_duplicate().

Step 12  [DuplicateFilter::is_duplicate, line 44 — linear scan]
  Iterate i = 0 .. DEDUP_WINDOW_SIZE-1 (128 iterations, fixed bound).
  Per iteration:
    NEVER_COMPILED_OUT_ASSERT(i < DEDUP_WINDOW_SIZE).  [loop bound assertion]
    Check: m_window[i].valid && m_window[i].src == src && m_window[i].msg_id == msg_id.
    [Triple-condition: slot must be occupied AND both key fields must match.]

  Step 12a — Duplicate path (this use case):
    At slot i = K (where K is the slot where the first reception was recorded):
      m_window[K].valid   == true   (was set by record() on first receipt)
      m_window[K].src     == src    (matches)
      m_window[K].msg_id  == msg_id (matches)
    All three conditions true → return true immediately.
    Loop does not run to completion.

  Step 12b — First-receipt path (contrast, not this use case):
    No slot has a matching (src, msg_id) pair.
    Loop runs all 128 iterations without early return.
    NEVER_COMPILED_OUT_ASSERT(m_count <= DEDUP_WINDOW_SIZE)  [line 54 — post-loop check].
    Returns false.

Step 13  [DuplicateFilter::check_and_record, line 99-100]
  is_duplicate() returned true.
  Immediately returns Result::ERR_DUPLICATE.
  record() is NOT called; m_window, m_next, and m_count are not modified.

Step 14  [DeliveryEngine::receive, lines 204-208]
  dedup_res == Result::ERR_DUPLICATE → true.
  Logger::log(Severity::INFO, "DeliveryEngine",
              "Suppressed duplicate message_id=%llu from src=%u", ...)
  [Uses 512-byte stack buffer; writes formatted line to stderr.]
  return Result::ERR_DUPLICATE.

Step 15  [Caller receives ERR_DUPLICATE]
  The application's receive loop receives ERR_DUPLICATE.
  env still contains the duplicate envelope content; the caller owns it.
  The caller must not process this message.
  No ACK is generated by DeliveryEngine on a duplicate drop.

---

## 4. Call Tree (Hierarchical)

DeliveryEngine::receive(env, timeout_ms, now_us)              [DeliveryEngine.cpp:149]
  [NEVER_COMPILED_OUT_ASSERT m_initialized, now_us > 0]
  m_transport->receive_message(env, timeout_ms)               [virtual dispatch, line 161]
    TcpBackend::receive_message(env, timeout_ms)              [TcpBackend.cpp:386]
      [dequeue/poll; returns OK with retransmitted DATA envelope]
  [NEVER_COMPILED_OUT_ASSERT envelope_valid(env)]
  timestamp_expired(env.expiry_time_us, now_us)               [inline, Timestamp.hpp:51]
    → false (message not expired)
  envelope_is_control(env)                                    [inline, MessageEnvelope.hpp:79]
    → false (DATA message)
  [NEVER_COMPILED_OUT_ASSERT envelope_is_data(env)]
  [reliability_class == RELIABLE_RETRY: true]
  m_dedup.check_and_record(env.source_id, env.message_id)    [DuplicateFilter.cpp:93]
    [NEVER_COMPILED_OUT_ASSERT m_count <= 128]
    is_duplicate(src, msg_id)                                  [DuplicateFilter.cpp:40]
      [loop i=0..127]
        [NEVER_COMPILED_OUT_ASSERT i < 128]
        [at slot K: valid && src match && msg_id match → true]
      return true
    [is_duplicate returned true]
    return Result::ERR_DUPLICATE
  [dedup_res == ERR_DUPLICATE: true]
  Logger::log(INFO, "DeliveryEngine",
              "Suppressed duplicate message_id=... from src=...")
    severity_tag(INFO) → "INFO     "
    snprintf(buf, 512, "[INFO     ][DeliveryEngine] ")
    vsnprintf(buf+hdr, 512-hdr, fmt, args)
    fprintf(stderr, "%s\n", buf)
  return Result::ERR_DUPLICATE

Contrast — first receipt of the same message (not this use case):
  m_dedup.check_and_record(env.source_id, env.message_id)
    is_duplicate(src, msg_id) → false (no slot matches)
    record(src, msg_id)                                        [DuplicateFilter.cpp:63]
      [NEVER_COMPILED_OUT_ASSERT m_next < 128]
      [NEVER_COMPILED_OUT_ASSERT m_count <= 128]
      m_window[m_next] = {src, msg_id, valid=true}
      m_next = (m_next + 1) % 128
      if m_count < 128: m_count++
      [NEVER_COMPILED_OUT_ASSERT m_next < 128]
      [NEVER_COMPILED_OUT_ASSERT m_count <= 128]
    return Result::OK
  [dedup_res == OK; continue to deliver]
  Logger::log(INFO, "DeliveryEngine", "Received data message_id=…")
  return Result::OK

---

## 5. Key Components Involved

Component              File                                  Role
─────────────────────────────────────────────────────────────────────────────
DeliveryEngine         src/core/DeliveryEngine.cpp/.hpp      Orchestrates the
                                                             receive pipeline:
                                                             transport → expiry →
                                                             control handling →
                                                             dedup gate. Owns
                                                             m_dedup by value.

DuplicateFilter        src/core/DuplicateFilter.cpp/.hpp     Fixed circular
                                                             window of 128 Entry
                                                             structs. Provides
                                                             check_and_record(),
                                                             is_duplicate(), and
                                                             record() as separate
                                                             primitives.

DuplicateFilter::Entry src/core/DuplicateFilter.hpp:54       POD struct:
                                                             {NodeId src,
                                                             uint64_t msg_id,
                                                             bool valid}.
                                                             16 bytes with
                                                             natural alignment.

MessageEnvelope        src/core/MessageEnvelope.hpp          Data carrier; the
                                                             key fields
                                                             source_id and
                                                             message_id form
                                                             the dedup key.
                                                             reliability_class
                                                             gates dedup.

timestamp_expired()    src/core/Timestamp.hpp:51             Inline predicate;
                                                             gates dedup behind
                                                             expiry check.

envelope_is_control()  src/core/MessageEnvelope.hpp:79       Inline predicate;
                                                             gates dedup behind
                                                             control-message
                                                             dispatch.

Logger                 src/core/Logger.hpp                   Fixed-buffer stderr
                                                             sink; INFO severity
                                                             on duplicate drop.

Types.hpp              src/core/Types.hpp                    DEDUP_WINDOW_SIZE=128,
                                                             Result::ERR_DUPLICATE=6.

---

## 6. Branching Logic / Decision Points

Decision 1 — Did transport receive succeed?
  Location: DeliveryEngine::receive, line 162
  Condition: res != Result::OK
  True  → return res immediately (ERR_TIMEOUT etc.); dedup never reached.
  False → continue.

Decision 2 — Is the message expired?
  Location: DeliveryEngine::receive, line 171
  Condition: timestamp_expired(env.expiry_time_us, now_us)
             = (expiry_us != 0ULL) && (now_us >= expiry_us)
  True  → log WARNING_LO, return ERR_EXPIRED; dedup never reached.
  False → continue.

Decision 3 — Is the message a control message?
  Location: DeliveryEngine::receive, line 179
  Condition: envelope_is_control(env)
  True  → handle as ACK/NAK/HEARTBEAT; dedup never reached.
  False → continue.

Decision 4 — Is reliability_class == RELIABLE_RETRY?
  Location: DeliveryEngine::receive, line 201
  Condition: env.reliability_class == ReliabilityClass::RELIABLE_RETRY
  False (BEST_EFFORT or RELIABLE_ACK) → dedup skipped; message delivered.
  True  → dedup is applied. [This is the path exercised in this use case.]

Decision 5 — Does any window slot match (valid && src == src && msg_id == msg_id)?
  Location: DuplicateFilter::is_duplicate, line 48
  True  → return true immediately (early exit from loop).
  False → scan exhausts all 128 slots; return false.

Decision 6 — Did is_duplicate return true?
  Location: DuplicateFilter::check_and_record, line 99
  True  → return ERR_DUPLICATE; record() not called. [This use case.]
  False → call record(); return OK.

Decision 7 — Is m_count < DEDUP_WINDOW_SIZE inside record()?
  Location: DuplicateFilter::record, line 80
  [Only reached on first receipt, not this use case.]
  True  → increment m_count.
  False → do not increment; oldest entry is silently overwritten at m_next.

---

## 7. Concurrency / Threading Behavior

DuplicateFilter contains no mutexes, no std::atomic members, and no thread creation.
m_window[], m_next, and m_count are plain non-atomic fields.

[ASSUMPTION] DuplicateFilter is accessed from a single thread only. DeliveryEngine::receive()
is assumed single-threaded, consistent with send(), pump_retries(), and sweep_ack_timeouts().

The m_dedup object is embedded by value in DeliveryEngine. Each DeliveryEngine instance has
its own independent DuplicateFilter. No sharing between instances.

If receive() were called concurrently on the same DeliveryEngine from two threads, a data
race would exist on m_window[m_next] between check_and_record() (read in is_duplicate,
write in record) and any concurrent record() calls. No synchronization would prevent it.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

Allocation model:
  No heap allocation on this path. All state is either embedded in DeliveryEngine (m_dedup)
  or on the call stack (env reference, local Result variables).

DuplicateFilter internal memory:
  m_window[DEDUP_WINDOW_SIZE]: 128 Entry structs.
    sizeof(Entry): NodeId(4) + uint64_t(8) + bool(1) + 3 bytes padding = 16 bytes.
    sizeof(m_window) = 128 × 16 = 2048 bytes.
  m_next:  uint32_t (4 bytes).
  m_count: uint32_t (4 bytes).
  Total: ~2056 bytes, statically embedded in DeliveryEngine.

is_duplicate() is declared const (DuplicateFilter.hpp:38): read-only linear scan of
m_window[]. No writes occur on the duplicate detection path.

record() is NOT called on the duplicate drop path. m_window, m_next, and m_count are
unchanged when ERR_DUPLICATE is returned. The dedup filter state is identical before
and after a duplicate drop.

Stack frame — DeliveryEngine::receive():
  env:        MessageEnvelope& (reference; ~4135 bytes live in caller's storage).
  timeout_ms: uint32_t (4 bytes).
  now_us:     uint64_t (8 bytes).
  res, dedup_res: Result enum (uint8_t each).

Stack frame — DuplicateFilter::check_and_record():
  src:    NodeId (uint32_t).
  msg_id: uint64_t.
  No local buffers.

Stack frame — DuplicateFilter::is_duplicate():
  src, msg_id: passed by value.
  i: uint32_t loop index.
  No local buffers.

Envelope ownership:
  env is passed by reference to receive(). On ERR_DUPLICATE return, env still holds the
  duplicate message content. The caller owns env. DeliveryEngine does not clear or take
  ownership of the envelope on a duplicate drop.

---

## 9. Error Handling Flow

Result::ERR_DUPLICATE (value 6):
  Returned by is_duplicate() → check_and_record() → DeliveryEngine::receive() → caller.
  Logged at INFO severity (not WARNING — duplicates are expected in RELIABLE_RETRY flows).
  Caller receives the code and must not process the envelope as new data.

Upstream errors (returned before dedup is reached):
  ERR_TIMEOUT: transport had no message; returned at line 163; dedup never reached.
  ERR_EXPIRED: message was past its deadline; returned at line 175; dedup never reached.
  Any transport error code: returned at line 163.

NEVER_COMPILED_OUT_ASSERT failures (always active, not NDEBUG-gated):
  check_and_record line 96: m_count > 128 → fires if count invariant broken.
  is_duplicate line 46: i >= 128 → impossible given loop bound `i < 128`.
  is_duplicate line 54: m_count > 128 → fires if count invariant broken post-loop.

m_initialized == false:
  NEVER_COMPILED_OUT_ASSERT(m_initialized) at receive() line 153 aborts process in all builds.

No error recovery is attempted on ERR_DUPLICATE; it is a normal, expected code in the
RELIABLE_RETRY delivery model.

---

## 10. External Interactions

1. TransportInterface::receive_message() [virtual call → TcpBackend or LocalSimHarness]
   Called once at the start of receive(). Delivers the retransmitted envelope.
   Not called again during the dedup path.

2. Logger::log() / fprintf(stderr)
   Called once inside DeliveryEngine::receive() at line 204-207 (INFO severity).
   Fields logged: message_id (uint64_t), source_id (NodeId/uint32_t).
   Full payload is NOT logged (per CLAUDE.md §7.1).
   Uses a 512-byte stack buffer; output goes to stderr.

3. No other external interactions
   No socket writes, no OS timer calls, no impairment engine interactions.
   The dedup check is entirely in-memory on the DuplicateFilter's stack-embedded array.

---

## 11. State Changes / Side Effects

Object               Field         Before (duplicate drop)    After
─────────────────────────────────────────────────────────────────────────────
DuplicateFilter      m_window[]    contains (src, msg_id)     unchanged
DuplicateFilter      m_next        N                          unchanged
DuplicateFilter      m_count       M                          unchanged
DeliveryEngine       m_ack_tracker not touched                not touched
DeliveryEngine       m_retry_mgr   not touched                not touched
stderr               log output    —                          one INFO line:
                                                              "Suppressed
                                                               duplicate
                                                               message_id=…
                                                               from src=…"

Summary: A duplicate drop is a read-only operation on DuplicateFilter with a single
log side effect. No state is modified anywhere in the delivery engine.

---

## 12. Sequence Diagram (ASCII)

```
Caller        DeliveryEngine     TcpBackend      DuplicateFilter    Logger (stderr)
  |                |                 |                  |               |
  |--receive()---->|                 |                  |               |
  |                |--recv_msg()---->|                  |               |
  |                |                | [returns OK,      |               |
  |                |                |  DATA, dup env]   |               |
  |                |<--OK, env------|                  |               |
  |                |                                    |               |
  |                | NCOA(m_initialized)                |               |
  |                | NCOA(envelope_valid)               |               |
  |                | timestamp_expired? -> false        |               |
  |                | envelope_is_control? -> false      |               |
  |                | NCOA(envelope_is_data)             |               |
  |                | reliability_class==RETRY? -> true  |               |
  |                |                                    |               |
  |                |--check_and_record(src, msg_id)---->|               |
  |                |                 | NCOA(m_count<=128)               |
  |                |                 | is_duplicate(src, msg_id)        |
  |                |                 | [scan m_window[0..127]:          |
  |                |                 |  slot K: valid&&src&&id match]   |
  |                |                 | is_duplicate → true              |
  |                |                 | return ERR_DUPLICATE             |
  |                |<--ERR_DUPLICATE-|                  |               |
  |                |                                    |               |
  |                | Logger::log(INFO,                  |               |
  |                |  "Suppressed duplicate…") -------->|               |
  |                |                                    |  [INFO] …\n   |
  |<--ERR_DUPLICATE|                                    |               |
```

NCOA = NEVER_COMPILED_OUT_ASSERT

---

## 13. Initialization vs Runtime Flow

Initialization phase (called once at startup):

  DeliveryEngine::init()               [DeliveryEngine.cpp:17]
    m_dedup.init()                     [DuplicateFilter.cpp:23]
      (void)memset(m_window, 0, 2048)  — all 128 slots: valid=false, src=0, msg_id=0
      m_next  = 0
      m_count = 0
      NEVER_COMPILED_OUT_ASSERT(m_next == 0U)
      NEVER_COMPILED_OUT_ASSERT(m_count == 0U)

Runtime — first receipt of the message (before this use case fires):

  DeliveryEngine::receive() called with the original transmission.
  check_and_record(src, msg_id):
    is_duplicate → false (no match in window)
    record(src, msg_id):
      m_window[m_next] = {src, msg_id, valid=true}
      m_next = (m_next + 1) % 128
      if m_count < 128: m_count++
  Precondition for this use case is now satisfied: window contains the entry.

Runtime — retransmission receipt (this use case):

  DeliveryEngine::receive() called with the retransmitted duplicate.
  check_and_record(src, msg_id):
    is_duplicate → true (slot K found with matching pair)
    return ERR_DUPLICATE
  DeliveryEngine returns ERR_DUPLICATE to the caller.

Key distinction:
  No allocation occurs at runtime. The 128-slot window was sized and zeroed
  at init time. Runtime only reads and writes fields within the already-allocated
  Entry structs. The check on the duplicate path is purely read-only.

---

## 14. Known Risks / Observations

Risk 1 — Window eviction causes false negatives after DEDUP_WINDOW_SIZE messages
  Once 128 distinct (src, msg_id) pairs have been recorded, each new record()
  call overwrites the oldest slot. A retransmission of an evicted message will pass
  the is_duplicate() check (no slot found) and be delivered as if new. The retry
  window for RELIABLE_RETRY messages must therefore be shorter than the time it takes
  to cycle 128 messages, or evicted duplicates may be delivered.

Risk 2 — O(128) linear scan per message on the RELIABLE_RETRY receive path
  is_duplicate() always scans all 128 slots regardless of m_count. For high-rate
  RELIABLE_RETRY workloads, this is a fixed overhead of 128 comparisons per received
  message. Not a correctness issue, but a bounded performance cost (see WCET_ANALYSIS.md:
  DuplicateFilter::check_and_record is O(DEDUP_WINDOW_SIZE)).

Risk 3 — No ACK sent on duplicate drop
  DeliveryEngine::receive() returns ERR_DUPLICATE without sending an ACK to the remote
  peer. If the sender's ACK deadline expires before the original ACK arrives, the sender
  may retry again (creating further duplicates). The design relies on RetryManager's
  max_retries limit to eventually terminate the cycle. An application-layer strategy
  of sending an ACK for known duplicates would reduce spurious retransmissions but is
  not implemented here.

Risk 4 — Dedup only applied for RELIABLE_RETRY, not RELIABLE_ACK
  The gate at DeliveryEngine.cpp line 201 skips dedup for RELIABLE_ACK messages. A
  RELIABLE_ACK message received twice (possible via impairment duplication or multiple
  client connections) would be delivered twice to the application. This is consistent
  with the specification (dedup is for retry flows), but callers of RELIABLE_ACK should
  be aware.

Risk 5 — No thread safety
  DuplicateFilter has no synchronization. Concurrent calls to check_and_record() from
  two threads would produce data races on m_window, m_next, and m_count. The intended
  single-threaded model must be enforced by the caller.

---

## 15. Unknowns / Assumptions

[ASSUMPTION 1] The first transmission of this message was previously received and
  processed successfully. DuplicateFilter::record() was called during that first
  receipt, inserting (source_id, message_id) into the window at some slot K.

[ASSUMPTION 2] DuplicateFilter and DeliveryEngine::receive() are called from a
  single thread. No evidence of synchronization found in the codebase.

[ASSUMPTION 3] The transport (TcpBackend or LocalSimHarness) does not modify
  source_id or message_id between receipt and the dedup check. Traced code confirms
  these fields are deserialized directly from the wire frame and not modified by
  the transport layer.

[ASSUMPTION 4] DeliveryEngine does not send an ACK upon a duplicate drop. Confirmed
  by code inspection: the ACK-sending path is not triggered by ERR_DUPLICATE; the
  function returns immediately after logging.

[UNKNOWN 1] Whether the application layer re-sends an ACK for known duplicates as a
  policy decision. DeliveryEngine provides no such facility; this would be an
  application-level concern.

[UNKNOWN 2] Whether m_dedup state is shared between multiple DeliveryEngine instances.
  Based on code inspection, each DeliveryEngine has its own m_dedup by value. Multiple
  channels would have independent windows. A duplicate arriving on a different channel
  would not be caught. [ASSUMPTION: one DeliveryEngine per channel.]

[UNKNOWN 3] Session restart semantics: if source_id is reused after a node restart and
  message IDs restart from 0, window entries from the previous session could suppress
  legitimately new messages until they are evicted. No session-aware invalidation
  mechanism exists in DuplicateFilter.
