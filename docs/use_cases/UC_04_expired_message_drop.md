## 1. Use Case Overview

Use Case: UC_04 -- Expired Message Drop

A message's expiry_time_us is set at send time via timestamp_deadline_us().
Two distinct expiry paths exist in this codebase:

Path A (Receiver-side): DeliveryEngine::receive() calls timestamp_expired()
on the received envelope's expiry_time_us field. If expired, the message is
discarded and ERR_EXPIRED is returned to the caller before any dedup or
application-level processing occurs.

Path B (RetryManager-side): RetryManager::collect_due() checks the stored
retry slot's expiry_us against now_us at every pump_retries() cycle. If the
message has expired, the slot is deactivated and no retransmission occurs;
the message is silently removed from the retry table.

Both paths are purely time-based (no external signal), using the monotonic
clock. expiry_time_us = 0 is treated as "never expires" by timestamp_expired().

Requirement coverage: CLAUDE.md §3.2 (Expiry handling), §3.3 (Expiring
messages), application_requirements §3.2 (expiry_time), §5.1.


## 2. Entry Points

Path A -- receive-side expiry:
    Client.cpp :: wait_for_echo() (line 125)
      └─ engine.receive(reply, 100, now_us)  (line 138)
           └─ DeliveryEngine::receive()  (DeliveryEngine.cpp line 147)
                └─ timestamp_expired(env.expiry_time_us, now_us)  (Timestamp.hpp line 47)

Path B -- retry-side expiry:
    Client.cpp :: wait_for_echo() (line 151)  [or Server.cpp loop line 243]
      └─ engine.pump_retries(now_us)
           └─ DeliveryEngine::pump_retries()  (DeliveryEngine.cpp line 225)
                └─ RetryManager::collect_due(now_us, ...)  (RetryManager.cpp line 136)
                     [expiry check at line 156]

Expiry time computation:
    Client.cpp :: send_test_message() (line 109)
      └─ timestamp_deadline_us(now_us, 5000)  (Timestamp.hpp line 60)


## 3. End-to-End Control Flow (Step-by-Step)

--- Setup: How expiry_time_us is set ---

S1. In send_test_message() (Client.cpp line 109):
      env.expiry_time_us = timestamp_deadline_us(now_us, 5000U)

    timestamp_deadline_us() (Timestamp.hpp line 60):
      assert(now_us <= 0xFFFFFFFFFFFFFFFFULL)
      assert(duration_ms <= 0xFFFFFFFFUL)
      duration_us = (uint64_t)5000 * 1000ULL  = 5 000 000 us
      return now_us + 5 000 000
    Result: expiry_time_us = send_time + 5 seconds

S2. envelope_copy() in RetryManager::schedule() (RetryManager.cpp line 59) and
    AckTracker::track() (AckTracker.cpp line 56) both copy the full envelope via
    memcpy, so m_slots[i].expiry_us inherits env.expiry_time_us.
    RetryManager::schedule() line 68: m_slots[i].expiry_us = env.expiry_time_us

S3. Server.cpp send_echo_reply() (line 107) sets its own expiry:
      reply.expiry_time_us = timestamp_deadline_us(now_us, 10000U) = +10 seconds
    This is the expiry of the echo reply as seen on the RECEIVER side (the client).

--- Path A: DeliveryEngine::receive() expiry check ---

A1. Client receives a data frame from TcpBackend (after ACK or echo arrives late).
    TcpBackend::receive_message() returns OK, envelope populated.

A2. DeliveryEngine::receive() (line 147):
      assert(m_initialized); assert(now_us > 0)
      res = m_transport->receive_message(env, timeout_ms)
        returns Result::OK, env filled

A3. assert(envelope_valid(env))  -- checks message_type != INVALID,
    payload_length <= MSG_MAX_PAYLOAD_BYTES, source_id != NODE_ID_INVALID

A4. Expiry check (line 169):
      if (timestamp_expired(env.expiry_time_us, now_us)):

    timestamp_expired(expiry_us, now_us) (Timestamp.hpp line 47):
      assert(expiry_us <= 0xFFFFFFFFFFFFFFFFULL)
      assert(now_us    <= 0xFFFFFFFFFFFFFFFFULL)
      return (expiry_us != 0ULL) && (now_us >= expiry_us)

    Case 1: expiry_time_us = 0 -> timestamp_expired returns false -> message passes
    Case 2: now_us < expiry_time_us -> false -> message passes
    Case 3: now_us >= expiry_time_us -> true -> message is EXPIRED

A5. If expired (Case 3):
      Logger::log(WARNING_LO, "DeliveryEngine",
                  "Dropping expired message_id=%llu from src=%u", msg_id, src)
      return Result::ERR_EXPIRED

A6. Control returns to wait_for_echo() (Client.cpp line 138):
      res = engine.receive(reply, 100, now_us)
      res == ERR_EXPIRED -> result_ok(res) is false
      [no echo counted; loop continues to next attempt]

A7. If all MAX_RECV_RETRIES (20) attempts produce ERR_EXPIRED or ERR_TIMEOUT:
      logs "Timeout waiting for echo reply"
      return ERR_TIMEOUT

--- Path B: RetryManager::collect_due() expiry check ---

B1. Each call to engine.pump_retries(now_us) (after ~5 seconds have elapsed):

B2. DeliveryEngine::pump_retries() (line 225):
      MessageEnvelope retry_buf[MSG_RING_CAPACITY]
      collected = m_retry_manager.collect_due(now_us, retry_buf, MSG_RING_CAPACITY)

B3. RetryManager::collect_due() (line 136):
      for i = 0..ACK_TRACKER_CAPACITY-1:
        if !m_slots[i].active -> continue

        -- EXPIRY CHECK (line 156) --
        if (m_slots[i].expiry_us != 0ULL && now_us >= m_slots[i].expiry_us):
          m_slots[i].active = false
          --m_count
          Logger::log(WARNING_LO, "RetryManager",
                      "Expired retry entry for message_id=%llu", msg_id)
          continue   <-- slot cleared, message NOT added to out_buf

        -- exhaustion check (line 166) --
        if (m_slots[i].retry_count >= m_slots[i].max_retries): ...

        -- due check (line 177) --
        if (m_slots[i].next_retry_us <= now_us): ... collect for retransmit

B4. Because the expiry check fires before the due check, an expired message
    is NEVER retransmitted even if next_retry_us <= now_us.

B5. collected returns 0 for the expired slot (it was skipped).
    No retransmit occurs. pump_retries() returns 0.

--- Timeline for a 5-second expiry ---

T=0:         send_test_message(): expiry_time_us = T + 5 000 000 us
T=0:         RetryManager::schedule(): next_retry_us = T (immediate)
T=0.001s:    pump_retries(): expiry? NO. due? YES -> retransmit #1
             backoff_ms: 1000->2000, next_retry_us = T + 2 000 000 us
T=2.001s:    pump_retries(): expiry? NO (now=2.001 < 5). due? YES -> retransmit #2
             backoff_ms: 2000->4000, next_retry_us = T + 6 000 000 us
T=5.0s:      pump_retries(): expiry? YES (now=5.0 >= 5.0) -> deactivate, NO retransmit
             [Even though retry_count=2 < max_retries=3, expiry wins]

Note: The expiry check fires before the due check, so even if a retry was
scheduled for T+6s but the message expired at T+5s, it is dropped at T+5s.

--- Edge Case: expiry_time_us = 0 ---

If a caller sets env.expiry_time_us = 0:
  timestamp_expired(0, now_us) -> (0 != 0) is false -> returns false
  -> message never expires in receive()
  RetryManager: m_slots[i].expiry_us = 0 -> condition
    (0 != 0ULL && ...) is false -> expiry check skipped entirely
  -> only exhaustion or ACK can stop retries


## 4. Call Tree (Hierarchical)

--- Path A: receive-side ---
DeliveryEngine::receive(env, timeout_ms, now_us)
  m_transport->receive_message(env, timeout_ms)
    TcpBackend::receive_message(env, timeout_ms)
      m_recv_queue.pop(env)                    [queue check first]
      TcpBackend::accept_clients()             [server mode only]
      [poll loop]:
        recv_from_client(fd, 100)
          tcp_recv_frame()                     [blocking read, 100ms]
          Serializer::deserialize()
          m_recv_queue.push(env)
        m_recv_queue.pop(env)                  [check after each recv]
        timestamp_now_us()                     [for impairment delayed msgs]
        m_impairment.collect_deliverable()
        m_recv_queue.push(delayed[i])
        m_recv_queue.pop(env)
  envelope_valid(env)                          [assertion]
  timestamp_expired(env.expiry_time_us, now_us)
    [if true]:
      Logger::log(WARNING_LO, "Dropping expired message_id=...")
      return ERR_EXPIRED
  envelope_is_control(env)                    [ACK/NAK/HEARTBEAT branch]
  envelope_is_data(env)                       [data branch -> dedup]
  return OK

--- Path B: retry-side ---
DeliveryEngine::pump_retries(now_us)
  RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)
    [for each active slot]:
      expiry check: now_us >= m_slots[i].expiry_us
        [if true]:
          m_slots[i].active = false
          --m_count
          Logger::log(WARNING_LO, "Expired retry entry for message_id=...")
          continue                             [slot NOT added to out_buf]
      exhaustion check
      due check + backoff doubling
  [for each collected, non-expired envelope]:
    send_via_transport(retry_buf[i])
  return collected

--- Expiry setup ---
send_test_message()
  timestamp_deadline_us(now_us, 5000U)         [Timestamp.hpp]
    return now_us + 5_000_000 us
  env.expiry_time_us = result

RetryManager::schedule()
  m_slots[i].expiry_us = env.expiry_time_us    [copied from envelope]

AckTracker::track()
  envelope_copy() -> m_slots[j].env.expiry_time_us inherited


## 5. Key Components Involved

Component                     File                       Role in UC_04
----------------------------  -------------------------  ----------------------------
timestamp_deadline_us()       src/core/Timestamp.hpp     Computes expiry_time_us at
                              (line 60)                  send time; converts ms -> us
timestamp_expired()           src/core/Timestamp.hpp     Tests now_us >= expiry_us;
                              (line 47)                  handles zero = never-expire
DeliveryEngine::receive()     src/core/DeliveryEngine.cpp Calls timestamp_expired();
                              (line 147)                 returns ERR_EXPIRED on drop;
                                                         logs WARNING_LO
RetryManager::collect_due()   src/core/RetryManager.cpp  Checks expiry_us before due;
                              (line 156)                 deactivates slot on expiry;
                                                         logs WARNING_LO
send_test_message()           src/app/Client.cpp         Sets expiry_time_us = +5s
                              (line 109)
send_echo_reply()             src/app/Server.cpp         Sets expiry_time_us = +10s
                              (line 107)                 on echo reply envelope
TcpBackend::receive_message() src/platform/TcpBackend.cpp Delivers envelope to
                                                         DeliveryEngine; expiry
                                                         checked AFTER transport
                                                         returns
envelope_valid()              src/core/MessageEnvelope.hpp Validates before expiry
                                                         check (asserted)
AckTracker::sweep_expired()   src/core/AckTracker.cpp    Separately sweeps ACK
                                                         deadline (recv_timeout_ms,
                                                         100ms); NOT the same as
                                                         message expiry


## 6. Branching Logic / Decision Points

Decision                      Location                   Outcomes
----------------------------  -------------------------  ----------------------------
expiry_us == 0                timestamp_expired()        Returns false immediately;
                              Timestamp.hpp line 54      message never expires
now_us < expiry_us            timestamp_expired()        Returns false; message valid
now_us >= expiry_us           timestamp_expired()        Returns true; message expired
Expired in receive()          DeliveryEngine.cpp         Log WARNING_LO + return
                              line 169-174               ERR_EXPIRED; application
                                                         never sees the message
Expired in collect_due()      RetryManager.cpp           active=false; --m_count;
                              line 156-163               log WARNING_LO; NOT added
                                                         to out_buf; no retransmit
Expiry fires before due       RetryManager.cpp           Order of checks: expiry
                              lines 156, 166, 177        checked FIRST; a message
                                                         due for retry but expired
                                                         is dropped, NOT retransmitted
receive() result check        Client.cpp line 140        result_ok(ERR_EXPIRED) ->
                              wait_for_echo()            false; loop continues; no
                                                         echo counted
expiry_us != 0 guard in       RetryManager.cpp line 156  Prevents incorrect expiry
RetryManager                                             when expiry_us=0 (permanent)


## 7. Concurrency / Threading Behavior

Single-threaded. No synchronization required.

timestamp_now_us() and timestamp_expired() are pure functions (no shared state).
clock_gettime(CLOCK_MONOTONIC) is POSIX-async-signal-safe and thread-safe, but
is not called from a signal handler in this codebase (the SIGINT handler only
sets g_stop_flag).

The now_us value passed to pump_retries() is captured once at the top of the
loop iteration and reused for all three calls (receive, pump_retries,
sweep_ack_timeouts). This means a message could expire between the timestamp
capture and the actual check -- but since now_us only monotonically increases,
the worst case is a false "not yet expired" result (the check runs slightly
early), which errs on the safe side by NOT dropping the message prematurely.

[ASSUMPTION]: If multiple messages are in-flight simultaneously, the expiry
check in collect_due() iterates all ACK_TRACKER_CAPACITY slots, so all
expired entries are cleaned up in a single collect_due() pass.


## 8. Memory & Ownership Semantics (C/C++ Specific)

The expiry_time_us field is a uint64_t scalar member of MessageEnvelope.
It is:
  - Set by value in send_test_message() (Client.cpp line 109)
  - Bitwise copied into RetryManager::m_slots[i].env and AckTracker::m_slots[j].env
    via envelope_copy() (memcpy of sizeof(MessageEnvelope))
  - Separately stored in RetryManager::m_slots[i].expiry_us (line 68 of RetryManager.cpp)
    as a scalar copy -- this is the field checked in collect_due()

Note: RetryManager stores expiry_us in TWO places:
  1. m_slots[i].env.expiry_time_us  (inside the copied envelope, used if retransmitting)
  2. m_slots[i].expiry_us           (dedicated scalar, checked in collect_due() line 156)

Both are set from env.expiry_time_us in schedule() (line 68), so they are
always equal at schedule time. No divergence can occur after initialization.

When a slot is deactivated due to expiry (active = false), the envelope data
remains in m_slots[i].env but is ignored (active check gates all access).
The slot is available for reuse by the next schedule() call.

No heap allocation. All expiry state is in fixed-size value members.


## 9. Error Handling Flow

Error/Condition           Detection                    Response
------------------------  ---------------------------  ------------------------------
Message expired on        timestamp_expired() in       Log WARNING_LO; return
receive path              DeliveryEngine::receive()    ERR_EXPIRED to caller;
                          line 169                     envelope contents unchanged
                                                       but caller must not use them
Caller receives           wait_for_echo() line 140     result_ok(ERR_EXPIRED) = false;
ERR_EXPIRED               Client.cpp                   loop iteration continues;
                                                       pump_retries() still called;
                                                       no echo counted
Message expired in        RetryManager::collect_due()  Slot deactivated silently;
retry table               line 156-163                 WARNING_LO logged; no retransmit;
                                                       no notification to AckTracker
AckTracker not notified   [no cross-call]              AckTracker slot remains PENDING
of retry expiry                                        until sweep_expired() fires on
                                                       its own deadline (100ms);
                                                       WARNING_HI logged by
                                                       sweep_ack_timeouts()
Expiry = 0 (permanent)    timestamp_expired()          Returns false always; message
                          RetryManager line 156        will only stop via exhaustion
                                                       or ACK
expiry_us set in past     [possible if clock skew      timestamp_expired() fires
                          or very long processing]     immediately on first receive;
                                                       message dropped


## 10. External Interactions

Interaction           API Used                       Direction   Notes
--------------------  -----------------------------  ----------  --------------------
Monotonic clock       clock_gettime(CLOCK_MONOTONIC  Inbound     Used in
                      )                                          timestamp_now_us();
                                                                 called at loop start
                                                                 in main loops,
                                                                 and in TcpBackend
                                                                 during send
Logger output         Logger::log(WARNING_LO, ...)   Outbound    On every expired
                                                                 message in both paths
TCP socket read       tcp_recv_frame()               Inbound     Delivers potentially
                                                                 expired message;
                                                                 expiry not checked
                                                                 in TcpBackend itself


## 11. State Changes / Side Effects

Path A (receive-side expiry):
  - No state changes in AckTracker or RetryManager.
  - The RetryManager slot for the expired message_id remains ACTIVE and will
    continue attempting retries until Path B catches it.
  - DeliveryEngine::receive() returns ERR_EXPIRED; no dedup record is written
    (DuplicateFilter::check_and_record is never reached for expired messages).

Path B (retry-side expiry):
  - RetryManager::m_slots[i].active = false
  - RetryManager::m_count decremented by 1
  - The slot becomes FREE for reuse on the next schedule() call.
  - AckTracker slot is NOT modified; it will eventually be cleaned up by
    sweep_ack_timeouts() -> sweep_expired() independently.

Combined effect for a 5-second message:
  At T+5s: RetryManager entry deactivated (Path B).
  The AckTracker entry (with deadline = now + 100ms from send time)
  will have been swept much earlier by sweep_ack_timeouts() -- at T+0.1s
  -- and logged as an ACK timeout (WARNING_HI).


## 12. Sequence Diagram (ASCII)

--- Path A: Receive-side expiry ---

Client::main  wait_for_echo  DeliveryEngine  Timestamp     TcpBackend    Logger
    |              |               |              |              |            |
    |--pump loop-->|               |              |              |            |
    |              |--receive(100ms,now_us)------>|              |            |
    |              |               |--recv_message(env,100)----->|            |
    |              |               |              |        [reads stale       |
    |              |               |              |         envelope from     |
    |              |               |              |         wire/queue]       |
    |              |               |<--OK (env)-----------------+            |
    |              |               |--envelope_valid(env)? YES  |            |
    |              |               |--timestamp_expired(expiry, now_us)      |
    |              |               |              |<--true-------+            |
    |              |               |              |              |--WARNING_LO|
    |              |               |<--ERR_EXPIRED+              |            |
    |              |<--ERR_EXPIRED-+              |              |            |
    |              | result_ok? NO                |              |            |
    |              | loop continues               |              |            |

--- Path B: Retry-side expiry (in pump_retries) ---

Client::main  pump_retries  DeliveryEngine  RetryManager  Logger
    |              |               |              |            |
    |--T>=5s------>|               |              |            |
    |              |--pump_retries(now_us)-------->|           |
    |              |               |--collect_due(now_us, buf, cap)
    |              |               |              |            |
    |              |               |  [slot 0: active=true]    |
    |              |               |  expiry_us=T+5s           |
    |              |               |  now_us >= expiry_us: YES |
    |              |               |  active=false; --m_count  |
    |              |               |              |--WARNING_LO|
    |              |               |              | "Expired   |
    |              |               |              |  retry     |
    |              |               |              |  entry"    |
    |              |               |  continue (NOT in out_buf)|
    |              |               |<--collected=0+            |
    |              |               | [no retransmit loop runs] |
    |              |<--0-----------+              |            |
    |<--0----------+               |              |            |

--- Expiry setup at send time ---

send_test_message()  timestamp_deadline_us()
    |                       |
    |--now_us=T0----------->|
    |  duration_ms=5000     |
    |                       | duration_us = 5000 * 1000 = 5_000_000
    |                       | return T0 + 5_000_000
    |<--expiry = T0+5s------+
    | env.expiry_time_us = T0+5s


## 13. Initialization vs Runtime Flow

Initialization:
  - No expiry-specific initialization required.
  - timestamp_now_us() and timestamp_expired() are stateless inline functions;
    they require only that clock_gettime(CLOCK_MONOTONIC) is available.
  - RetryManager::init() zero-fills all expiry_us fields to 0.
  - AckTracker::init() zero-fills all deadline_us fields to 0.

Runtime:
  - expiry_time_us set once at envelope creation time.
  - Checked on every receive call (Path A) and every pump_retries call (Path B).
  - Expiry is a monotonic, write-once, read-many field per message.
  - No update or refresh of expiry_time_us occurs after initial set.
  - RetryManager::schedule() copies it at schedule time; thereafter it is
    immutable in the slot (no reset on retry).


## 14. Known Risks / Observations

R1. Receive-side expiry does not cancel the retry entry.
    When a received message is dropped via Path A (ERR_EXPIRED in receive()),
    the corresponding RetryManager slot (if this is a locally-sent message
    whose own expiry elapsed) is not notified. The two paths are completely
    independent. A message can be expired on the wire AND still queued for
    retry on the sender -- it will just keep sending until Path B catches it.

R2. AckTracker and RetryManager use different expiry intervals.
    AckTracker deadline = now + recv_timeout_ms (100 ms) -- very short.
    RetryManager expiry = env.expiry_time_us (5 seconds from send_test_message).
    These are semantically different: AckTracker tracks ACK receipt latency;
    RetryManager tracks message lifetime. They are not synchronized.
    The AckTracker entry expires (WARNING_HI "ACK timeout") 100 ms after send,
    while the RetryManager continues retrying for up to 5 seconds.

R3. No notification to application on expiry.
    Both Path A and Path B drop messages silently (only log messages). The
    application layer (Client main loop) receives ERR_EXPIRED from receive()
    but has no API to query which outbound retry entries have expired in
    RetryManager. There is no callback or status API.

R4. Clock discontinuity risk.
    CLOCK_MONOTONIC is used, which is immune to NTP wall-clock adjustments.
    However, if the process is suspended (e.g. VM pause, container stop),
    CLOCK_MONOTONIC may or may not advance (platform-dependent). A large gap
    would cause all messages to appear simultaneously expired.
    [ASSUMPTION: Platform is not subject to long suspensions in normal use.]

R5. expiry_time_us set at envelope BUILD time, not SEND time.
    In send_test_message(), timestamp_now_us() is called at the top of the
    loop iteration (Client.cpp line 241), then passed to send_test_message()
    as now_us. The expiry is based on this now_us. If snprintf or memcpy
    incur measurable latency, the actual transmission time may be slightly
    after the expiry baseline. In practice with small payloads this is
    negligible.


## 15. Unknowns / Assumptions

U1. [ASSUMPTION] MSG_MAX_PAYLOAD_BYTES and MSG_RING_CAPACITY values not visible
    without Types.hpp. Assumed to be small (e.g. 256 and 16 respectively).

U2. [ASSUMPTION] The impairment engine can add latency to messages, potentially
    causing a message to be in the delay buffer past its expiry. When
    collect_deliverable() releases the delayed message into m_recv_queue,
    it will be picked up by receive_message() and passed to DeliveryEngine,
    which will then catch the expiry via timestamp_expired(). The impairment
    engine does not check expiry before releasing. [ASSUMPTION: this is the
    intended behavior -- expiry is enforced at the DeliveryEngine layer.]

U3. [ASSUMPTION] The Server's echo reply uses expiry = +10 seconds. If the
    round-trip time plus any impairment delay exceeds 10 seconds, the client
    will receive ERR_EXPIRED for the echo reply. Under normal LAN conditions
    this will not occur.

U4. The condition in RetryManager::collect_due() line 156 checks
    m_slots[i].expiry_us != 0ULL before comparing with now_us. This is correct
    for the "never expire" case. However, it means a message with expiry_us
    set to exactly 1 (or any tiny non-zero value) will expire instantly. There
    is no minimum expiry validation at the API boundary. [ASSUMPTION: callers
    always set expiry to a meaningful future time or 0.]

U5. [ASSUMPTION] AckTracker::sweep_expired() is the mechanism that clears
    AckTracker entries after expiry; it is called via
    engine.sweep_ack_timeouts() in each loop iteration. The AckTracker expiry
    (recv_timeout_ms = 100 ms) is completely separate from the RetryManager
    expiry (5 seconds). This is by design but creates the risk described in R2.

U6. No source for ImpairmentConfig or ImpairmentEngine internals was read.
    [ASSUMPTION]: imp_cfg.enabled = false by default, so no impairment delay
    is applied in standard runs, and messages are not held in the delay buffer
    past their expiry.