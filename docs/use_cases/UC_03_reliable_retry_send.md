## 1. Use Case Overview

Use Case: UC_03 — Reliable-with-Retry Send with Exponential Backoff

A Client process sends a DATA message carrying reliability_class = RELIABLE_RETRY.
The message is transmitted immediately, registered in both the AckTracker and
RetryManager, and then retransmitted by pump_retries() on each main-loop pass
until either an ACK is received (cancelling the retry entry) or the message
expires / exhausts its retry budget.

Exponential backoff: the initial backoff interval (cfg.channels[0].retry_backoff_ms,
read from ChannelConfig) is doubled on every collect_due() pass, capped hard at
60 000 ms (60 seconds). All time values are in microseconds internally.

Requirement coverage: CLAUDE.md §3.2 (Retry logic), §3.3 (Reliable with retry
and dedupe), application_requirements §5 (Impairment engine), §3.2 (Messaging
utilities).


## 2. Entry Points

Primary entry point:
    Client.cpp :: main()  (line 162)
      └─ send_test_message()  (line 74)
           └─ DeliveryEngine::send()  (DeliveryEngine.cpp line 75)

Secondary / recurring entry point (retry pump):
    Client.cpp :: wait_for_echo()  (line 125)
      └─ engine.pump_retries()  (line 151)
           └─ DeliveryEngine::pump_retries()  (DeliveryEngine.cpp line 225)

Tertiary entry point (ACK receipt cancels retry):
    Client.cpp :: wait_for_echo()
      └─ engine.receive()  (line 138)
           └─ DeliveryEngine::receive()  (DeliveryEngine.cpp line 147)
                └─ RetryManager::on_ack()  (RetryManager.cpp line 97)


## 3. End-to-End Control Flow (Step-by-Step)

--- Phase A: Initialization (runs once before send/receive loop) ---

A1. Client.cpp main() parses argv; sets peer_ip="127.0.0.1", peer_port=9000.
A2. transport_config_default(cfg) zero-fills TransportConfig.
A3. cfg.channels[0] configured: reliability=RELIABLE_RETRY, max_retries=3,
    recv_timeout_ms=100. retry_backoff_ms is set by channel_config_default()
    [ASSUMPTION: channel_config_default sets retry_backoff_ms to a non-zero value,
    e.g. 1000 ms; exact value not visible without ChannelConfig.cpp].
A4. TcpBackend transport; transport.init(cfg):
      socket_create_tcp() -> fd
      socket_set_reuseaddr(fd)
      socket_connect_with_timeout(fd, "127.0.0.1", 9000, 5000ms)
      m_client_fds[0] = fd, m_client_count=1, m_open=true
A5. DeliveryEngine engine; engine.init(&transport, cfg.channels[0], node_id=2):
      m_ack_tracker.init()    -- zeroes ACK_TRACKER_CAPACITY slots
      m_retry_manager.init()  -- zeroes ACK_TRACKER_CAPACITY RetryEntry slots
      m_dedup.init()          -- zeroes DEDUP_WINDOW_SIZE window entries
      m_id_gen.init(seed=2)
      m_initialized = true

--- Phase B: First Send (msg_idx=1 of 5) ---

B1. now_us = timestamp_now_us()  [clock_gettime(CLOCK_MONOTONIC)]
B2. send_test_message(engine, peer_id=1, msg_num=1, now_us):
      envelope_init(env)       -- memset to 0, message_type=INVALID
      snprintf(payload_buf, "Hello from client #1")
      memcpy(env.payload, payload_buf, len)
      env.payload_length = len
      env.message_type   = DATA
      env.destination_id = 1
      env.priority       = 0
      env.reliability_class = RELIABLE_RETRY
      env.timestamp_us   = now_us
      env.expiry_time_us = timestamp_deadline_us(now_us, 5000)
                         = now_us + 5 000 000 us   (5-second window)

B3. engine.send(env, now_us):  [DeliveryEngine.cpp line 75]
      assert(m_initialized); assert(now_us > 0)
      env.source_id  = 2               (m_local_id)
      env.message_id = m_id_gen.next() (e.g. 1)
      env.timestamp_us = now_us

      -- B3a: Immediate transport send --
      send_via_transport(env):
        assert(m_initialized); assert(m_transport != nullptr); assert(envelope_valid(env))
        res = m_transport->send_message(env)
          --> TcpBackend::send_message(env):
                Serializer::serialize(env, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
                now_us2 = timestamp_now_us()
                m_impairment.process_outbound(env, now_us2)
                  [if impairments disabled -> returns OK; no drop]
                m_impairment.collect_deliverable(now_us2, ...) -> delayed_count=0
                tcp_send_frame(m_client_fds[0], m_wire_buf, wire_len, send_timeout_ms)
                returns Result::OK
        returns Result::OK

      -- B3b: ACK tracking --
      env.reliability_class == RELIABLE_RETRY -> enters if block (line 100-101)
      ack_deadline = timestamp_deadline_us(now_us, recv_timeout_ms=100)
                   = now_us + 100 000 us
      m_ack_tracker.track(env, ack_deadline):
        finds first FREE slot (slot 0 after init)
        envelope_copy(m_slots[0].env, env)
        m_slots[0].deadline_us = ack_deadline
        m_slots[0].state = PENDING
        m_count = 1
        returns Result::OK

      -- B3c: Retry scheduling --
      env.reliability_class == RELIABLE_RETRY -> enters if block (line 118)
      m_retry_manager.schedule(env, max_retries=3, backoff_ms=cfg.retry_backoff_ms,
                                now_us):
        finds first inactive slot (slot 0 after init)
        m_slots[0].active       = true
        envelope_copy(m_slots[0].env, env)
        m_slots[0].retry_count  = 0
        m_slots[0].max_retries  = 3
        m_slots[0].backoff_ms   = cfg.retry_backoff_ms  [ASSUMPTION: 1000 ms]
        m_slots[0].next_retry_us = now_us   (immediate -- first retry due right away)
        m_slots[0].expiry_us    = env.expiry_time_us (now_us + 5 000 000 us)
        m_count = 1
        logs "Scheduled message_id=1 for retry (max_retries=3, backoff_ms=1000)"
        returns Result::OK

      assert(env.source_id == 2); assert(env.message_id != 0)
      logs "Sent message_id=1, reliability=2"
      returns Result::OK

--- Phase C: wait_for_echo() -- retry pump loop ---

C1. for attempt=0..MAX_RECV_RETRIES-1 (0..19):
      now_us = timestamp_now_us()
      engine.receive(reply, timeout_ms=100, now_us):
        [see UC_05 for ACK processing; if server sends ACK, RetryManager::on_ack cancels]
        [if no message or timeout -> returns ERR_TIMEOUT]

      if receive returns OK and reply is DATA -> success path (skip retries)

      engine.pump_retries(now_us):  [DeliveryEngine.cpp line 225]
        assert(m_initialized); assert(now_us > 0)
        MessageEnvelope retry_buf[MSG_RING_CAPACITY]  (stack allocation)
        collected = m_retry_manager.collect_due(now_us, retry_buf, MSG_RING_CAPACITY)

--- Phase D: RetryManager::collect_due() -- core backoff logic ---

D1. RetryManager::collect_due(now_us, out_buf, buf_cap):
      assert(m_initialized); assert(out_buf != nullptr); assert(buf_cap <= MSG_RING_CAPACITY)
      collected = 0

      Loop i=0..ACK_TRACKER_CAPACITY-1:
        if !m_slots[i].active -> continue

        -- Expiry check --
        if m_slots[i].expiry_us != 0 AND now_us >= m_slots[i].expiry_us:
          m_slots[i].active = false; --m_count
          logs "Expired retry entry for message_id=1"
          continue  (entry removed, no retransmit)

        -- Exhaustion check --
        if m_slots[i].retry_count >= m_slots[i].max_retries:
          m_slots[i].active = false; --m_count
          logs "Exhausted retries for message_id=1"
          continue

        -- Due check --
        if m_slots[i].next_retry_us <= now_us:
          envelope_copy(out_buf[collected], m_slots[i].env)
          ++collected
          ++m_slots[i].retry_count   (e.g. becomes 1 after first retry)

          -- Exponential backoff doubling --
          next_backoff_ms = (uint64_t)m_slots[i].backoff_ms * 2
          if next_backoff_ms > 60000 -> next_backoff_ms = 60000   (cap)
          m_slots[i].backoff_ms = (uint32_t)next_backoff_ms

          backoff_us = next_backoff_ms * 1000
          m_slots[i].next_retry_us = now_us + backoff_us

          logs "Due for retry: message_id=1, attempt=1, next_backoff_ms=2000"

      assert(collected <= buf_cap); assert(m_count <= ACK_TRACKER_CAPACITY)
      return collected

Backoff progression (assuming initial backoff_ms = 1000):
  Attempt 0 -> retry_count=1, new backoff_ms=2000,  next_retry at now+2 000 000 us
  Attempt 1 -> retry_count=2, new backoff_ms=4000,  next_retry at now+4 000 000 us
  Attempt 2 -> retry_count=3, new backoff_ms=8000   [but max_retries=3, so exhausted]

After attempt 2 the exhaustion check fires: retry_count(3) >= max_retries(3) -> slot deactivated.

Cap behaviour (if initial backoff were 32 000 ms):
  Attempt N: 32000*2=64000 > 60000 -> capped to 60000 ms, indefinitely thereafter.

--- Phase E: Retry retransmit ---

E1. Back in DeliveryEngine::pump_retries() (line 241):
      for i=0..collected-1:
        send_res = send_via_transport(retry_buf[i])
          --> TcpBackend::send_message() again (same path as B3a)
        if send_res != OK -> logs WARNING_LO "Retry send failed for message_id=1"
        else              -> logs INFO "Retried message_id=1"
      return collected

--- Phase F: ACK received -- cancels retry ---

F1. If server responds with an ACK envelope (message_type=ACK):
      DeliveryEngine::receive() calls (line 181-185):
        m_ack_tracker.on_ack(src=server_node, msg_id=1)
          finds slot 0: state=PENDING, source_id matches, message_id matches
          m_slots[0].state = ACKED
          returns OK
        m_retry_manager.on_ack(src=server_node, msg_id=1)
          finds slot 0: active=true, env.source_id matches, env.message_id matches
          m_slots[0].active = false; --m_count
          logs "Cancelled retry for message_id=1 from node=1"
          returns OK

F2. Next call to collect_due() finds slot 0 inactive -> skips it.
    No further retransmissions occur for message_id=1.


## 4. Call Tree (Hierarchical)

Client::main()
  transport.init(cfg)
    TcpBackend::connect_to_server()
      socket_create_tcp()
      socket_set_reuseaddr()
      socket_connect_with_timeout()
  engine.init(&transport, cfg.channels[0], 2)
    AckTracker::init()
    RetryManager::init()
    DuplicateFilter::init()
    MessageIdGen::init()

  [loop msg_idx=1..5]
    timestamp_now_us()
    send_test_message(engine, peer_id=1, msg_num, now_us)
      envelope_init(env)
      snprintf / memcpy
      timestamp_deadline_us(now_us, 5000)
      engine.send(env, now_us)
        DeliveryEngine::send_via_transport(env)
          TcpBackend::send_message(env)
            Serializer::serialize()
            timestamp_now_us()
            ImpairmentEngine::process_outbound()
            ImpairmentEngine::collect_deliverable()
            tcp_send_frame()
        timestamp_deadline_us(now_us, recv_timeout_ms)
        AckTracker::track(env, ack_deadline)
          envelope_copy()
        RetryManager::schedule(env, max_retries, backoff_ms, now_us)
          envelope_copy()

    wait_for_echo(engine, now_us)
      [loop attempt=0..19]
        timestamp_now_us()
        engine.receive(reply, 100, now_us)
          TcpBackend::receive_message(reply, 100)
            m_recv_queue.pop()
            TcpBackend::accept_clients()     [server side only; client skips]
            recv_from_client(fd, 100)
              tcp_recv_frame()
              Serializer::deserialize()
              m_recv_queue.push()
            m_recv_queue.pop()
          [if ACK]:
            AckTracker::on_ack()
            RetryManager::on_ack()
        engine.pump_retries(now_us)
          RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)
            [per active slot]:
              expiry check
              exhaustion check
              due check -> envelope_copy(), backoff doubling, next_retry_us update
          [for each collected envelope]:
            DeliveryEngine::send_via_transport()
              TcpBackend::send_message()
                Serializer::serialize()
                tcp_send_frame()
        engine.sweep_ack_timeouts(now_us)
          AckTracker::sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY)

  engine.pump_retries(final_now_us)    [final sweep]
  engine.sweep_ack_timeouts(final_now_us)
  transport.close()


## 5. Key Components Involved

Component               File                      Role in UC_03
----------------------  ------------------------  ------------------------------------
Client::main()          src/app/Client.cpp        Orchestrates init, send, receive loop
send_test_message()     src/app/Client.cpp        Builds RELIABLE_RETRY envelope
wait_for_echo()         src/app/Client.cpp        Drives pump_retries() each attempt
DeliveryEngine::send()  src/core/DeliveryEngine.cpp  Assigns IDs, calls transport, tracks
DeliveryEngine::        src/core/DeliveryEngine.cpp  Collects due entries, resends
  pump_retries()
DeliveryEngine::        src/core/DeliveryEngine.cpp  Handles ACK, cancels retry
  receive()
RetryManager::          src/core/RetryManager.cpp Stores message, sets next_retry_us=now
  schedule()
RetryManager::          src/core/RetryManager.cpp Expiry/exhaustion/due checks; backoff
  collect_due()           doubling, caps at 60 000 ms
RetryManager::on_ack()  src/core/RetryManager.cpp Deactivates slot on ACK receipt
AckTracker::track()     src/core/AckTracker.cpp   Records PENDING entry with deadline
AckTracker::on_ack()    src/core/AckTracker.cpp   Transitions slot PENDING->ACKED
AckTracker::            src/core/AckTracker.cpp   Sweeps PENDING past deadline -> FREE
  sweep_expired()
MessageIdGen::next()    src/core/MessageId.hpp    [ASSUMPTION] Returns monotonic uint64
timestamp_deadline_us() src/core/Timestamp.hpp    Computes absolute deadline in us
timestamp_now_us()      src/core/Timestamp.hpp    CLOCK_MONOTONIC via clock_gettime()
TcpBackend::            src/platform/TcpBackend.cpp  Serializes, impairs, frames, sends
  send_message()
Serializer::serialize() src/core/Serializer.hpp   Encodes envelope to wire bytes
ImpairmentEngine        src/platform/ [ASSUMPTION]  Optionally drops/delays messages
tcp_send_frame()        src/platform/SocketUtils  Framed write with length prefix


## 6. Branching Logic / Decision Points

Decision                     Location                      Branch Outcomes
---------------------------  ----------------------------  ---------------------------
reliability_class check      DeliveryEngine.cpp line 100   RELIABLE_RETRY -> track ACK
                                                           + schedule retry;
                                                           BEST_EFFORT -> neither
send_via_transport result    DeliveryEngine.cpp line 92    != OK -> log WARNING_LO,
                                                           return early (no tracking)
track() result               DeliveryEngine.cpp line 109   ERR_FULL -> log WARNING_HI,
                                                           continue (send still OK)
schedule() result            DeliveryEngine.cpp line 124   ERR_FULL -> log WARNING_HI,
                                                           continue (send still OK)
slot active check            RetryManager.cpp line 151     inactive -> skip
expiry check                 RetryManager.cpp line 156     expired -> deactivate, log,
                                                           skip (no retransmit)
exhaustion check             RetryManager.cpp line 166     retry_count>=max_retries ->
                                                           deactivate, log WARNING_HI
due check                    RetryManager.cpp line 177     next_retry_us > now_us ->
                                                           skip this pass
backoff cap                  RetryManager.cpp line 188     next_backoff_ms > 60000 ->
                                                           clamp to 60000
impairment drop              TcpBackend.cpp line 266       ERR_IO -> return OK (silent)
client_count == 0            TcpBackend.cpp line 278       no clients -> discard, OK
ACK type check               DeliveryEngine.cpp line 178   ACK -> call on_ack() pair;
                                                           NAK/HEARTBEAT -> pass through
send_res in pump loop        DeliveryEngine.cpp line 246   != OK -> WARNING_LO, continue


## 7. Concurrency / Threading Behavior

The codebase is single-threaded at the application level. No threads, mutexes,
or std::atomic usage appear in Client.cpp, DeliveryEngine, RetryManager,
AckTracker, or DuplicateFilter. The F-Prime style no-STL, no-thread constraint
is enforced.

All operations (send, pump_retries, receive, sweep_ack_timeouts) are called
sequentially within the Client main() loop. The retry pump executes within the
same thread as the send operation; there is no background timer thread.

TcpBackend internally calls timestamp_now_us() during send_message() to check
impairment timing. This is a read of CLOCK_MONOTONIC -- safe in single-threaded
context.

Signal handler (Server.cpp only): g_stop_flag is volatile sig_atomic_t. Not
relevant to Client-side retry behavior.

[ASSUMPTION]: If TcpBackend were to be used across threads in a future
configuration, m_recv_queue, m_client_fds[], and m_client_count would require
protection. No such protection currently exists.


## 8. Memory & Ownership Semantics (C/C++ Specific)

All storage is static (global), stack, or value-type member fields.
No heap allocation occurs after system init (Power of 10 rule 3).

Object                          Location         Ownership/Lifetime
------------------------------  ---------------  --------------------------------
TransportConfig cfg             stack (main)     POD struct; copied into TcpBackend
TcpBackend transport            stack (main)     Owns m_listen_fd, m_client_fds[],
                                                 m_recv_queue, m_impairment,
                                                 m_wire_buf
DeliveryEngine engine           stack (main)     Owns AckTracker, RetryManager,
                                                 DuplicateFilter, MessageIdGen by value
MessageEnvelope env             stack            Built in send_test_message(), passed
(in send_test_message)                           by ref to engine.send(), which copies
                                                 it into RetryManager slot and
                                                 AckTracker slot via envelope_copy()
                                                 (memcpy of fixed-size struct)
retry_buf[]                     stack            Temporary array in pump_retries();
(in pump_retries)                                MSG_RING_CAPACITY elements;
                                                 filled by collect_due() via
                                                 envelope_copy(); discarded after loop
RetryManager::m_slots[]         value member     Fixed array of ACK_TRACKER_CAPACITY
                                of RetryManager  RetryEntry structs; each holds a
                                                 full MessageEnvelope copy
AckTracker::m_slots[]           value member     Fixed array of ACK_TRACKER_CAPACITY
                                of AckTracker    AckEntry structs with copy of envelope
m_wire_buf                      member of        Fixed-size scratch buffer,
                                TcpBackend       SOCKET_RECV_BUF_BYTES; reused each
                                                 call to send_message/receive_message
TransportInterface* m_transport pointer member   Non-owning pointer; pointed-to object
                                of DeliveryEngine (TcpBackend on stack) must outlive
                                                 DeliveryEngine

Key copy semantics:
- envelope_copy() uses memcpy(&dst, &src, sizeof(MessageEnvelope)).
  The entire envelope including the payload[] array is bitwise copied.
- RetryManager stores a full copy in each slot -- the original envelope
  in the caller can be destroyed after schedule() returns.
- AckTracker likewise stores a full copy.

Risks:
- sizeof(MessageEnvelope) may be large (payload[MSG_MAX_PAYLOAD_BYTES]);
  stack frame in pump_retries() allocates MSG_RING_CAPACITY copies. If
  MSG_MAX_PAYLOAD_BYTES and MSG_RING_CAPACITY are both large this could
  exhaust stack space. [ASSUMPTION: constants are sized conservatively]


## 9. Error Handling Flow

Error                  Detection point              Handling
---------------------  ---------------------------  ----------------------------------
Transport send fails   send_via_transport() returns Log WARNING_LO; send() returns
                       != OK                        the error code to caller
ACK tracker full       AckTracker::track() returns  Log WARNING_HI; send() continues
                       ERR_FULL                     to return OK (ACK tracking is
                                                    best-effort side effect)
Retry table full       RetryManager::schedule()     Log WARNING_HI; send() continues
                       returns ERR_FULL             to return OK (retry is side effect)
Message expired in     collect_due() line 156-163   Slot deactivated, log WARNING_LO;
retry table                                         NOT retransmitted
Retries exhausted      collect_due() line 166-173   Slot deactivated, log WARNING_HI;
                                                    no further transmission
Retry send fails       pump_retries() line 246-250  Log WARNING_LO; loop continues
                                                    to next message
Impairment drops msg   TcpBackend::send_message()   Returns Result::OK silently;
                       line 266-268                 upper layers are not notified
                                                    of the drop
Serialization fails    TcpBackend::send_message()   Log WARNING_LO; return error
                                                    to DeliveryEngine::send_via_transport
No clients connected   TcpBackend::send_message()   Log WARNING_LO; return OK
                       line 278-282                 (message is discarded)
socket write fails     tcp_send_frame()             Log WARNING_LO; loop continues
                                                    to next client fd
ACK not found          RetryManager::on_ack()       Log WARNING_LO; return ERR_INVALID
                       line 126                     (ignored by receive())
snprintf fails         send_test_message() line 89  Return ERR_INVALID; msg not sent


## 10. External Interactions

Interaction            API Used                     Direction   Notes
---------------------  ---------------------------  ----------  --------------------
TCP socket write       tcp_send_frame()             Outbound    Framed with length
                       -> write()/send() [ASSUMPTION]           prefix; blocks up to
                                                                send_timeout_ms
TCP socket read        tcp_recv_frame()             Inbound     Framed read; blocks
                                                                100 ms per attempt
POSIX clock            clock_gettime(CLOCK_MONOTONIC Inbound    Called at send time,
                       )                                        retry time, receive
                                                                time; monotonic
POSIX nanosleep        nanosleep() in sleep_ms()    Self        Between messages,
                                                                INTER_MESSAGE_SLEEP_MS
                                                                = 100 ms
Logger output          Logger::log()                Outbound    stdout/stderr
                                                                [ASSUMPTION]


## 11. State Changes / Side Effects

Component             State Field           Before send()   After send()       After on_ack()
--------------------  --------------------  --------------  -----------------  ---------------
MessageIdGen          m_counter             N               N+1                N+1
AckTracker slot 0     state                 FREE            PENDING            ACKED
AckTracker            m_count               0               1                  1 (freed on
                                                                               sweep_expired)
RetryManager slot 0   active                false           true               false
RetryManager slot 0   retry_count           0               0                  0
RetryManager slot 0   backoff_ms            0               cfg.retry_backoff  N/A (inactive)
RetryManager slot 0   next_retry_us         0               now_us (immediate) N/A
RetryManager          m_count               0               1                  0
TcpBackend            m_wire_buf            undefined       last serialized    unchanged
                                                            frame bytes

After each collect_due() that fires a retry:
  m_slots[i].retry_count   incremented by 1
  m_slots[i].backoff_ms    doubled (capped at 60000)
  m_slots[i].next_retry_us set to now_us + new_backoff_us

After exhaustion (retry_count >= max_retries):
  m_slots[i].active = false; RetryManager::m_count decremented


## 12. Sequence Diagram (ASCII)

Client::main    send_test_msg   DeliveryEngine    AckTracker   RetryManager   TcpBackend
    |                |               |                |              |              |
    |--msg_idx=1---->|               |                |              |              |
    |                |--engine.send->|                |              |              |
    |                |               |--send_via_tr-->|              |              |
    |                |               |                |              |        send_message()
    |                |               |                |              |        serialize->tcp_send_frame
    |                |               |<--OK-----------+              |              |
    |                |               |--track(env, deadline)-------->|              |
    |                |               |                |<--OK---------+              |
    |                |               |--schedule(env, 3, backoff_ms, now)---------->|
    |                |               |                |              |<--OK---------+
    |                |<--OK----------+                |              |              |
    |<--OK-----------+               |                |              |              |
    |                                |                |              |              |
    |--wait_for_echo---------------->|                |              |              |
    |   [attempt=0]                  |                |              |              |
    |                |--engine.receive(100ms)-------->|              |              |
    |                |               |--recv_message->+              |              |
    |                |               |<--ERR_TIMEOUT--+              |              |
    |                |--pump_retries(now_us)--------->|              |              |
    |                |               |--collect_due(now_us)--------->|              |
    |                |               |   expiry? NO                  |              |
    |                |               |   exhausted? NO               |              |
    |                |               |   due? YES (next_retry<=now)  |              |
    |                |               |   backoff: 1000->2000ms       |              |
    |                |               |<--[env copy, collected=1]-----+              |
    |                |               |--send_via_transport(retry_buf[0])----------->|
    |                |               |                |              |        serialize->tcp_send_frame
    |                |               |<--OK-----------+              |              |
    |   [attempt=1..N, retries continue until ACK or expiry]         |              |
    |                                |                |              |              |
    |   [ACK received from server]   |                |              |              |
    |                |--engine.receive(100ms)-------->|              |              |
    |                |               |--recv_message: ACK envelope   |              |
    |                |               |--ack_tracker.on_ack(1,1)----->|              |
    |                |               |                |<--OK---------+              |
    |                |               |--retry_manager.on_ack(1,1)--->|              |
    |                |               |                |       slot.active=false      |
    |                |               |<--OK-----------+              |              |
    |<--OK-----------+               |                |              |              |


## 13. Initialization vs Runtime Flow

Initialization (runs once, at startup):
- TcpBackend::init() -- socket creation, TCP connect
- DeliveryEngine::init() -- sub-component init calls, no allocation
- AckTracker::init() -- memset + loop clearing all slots
- RetryManager::init() -- memset + loop clearing all RetryEntry slots
- DuplicateFilter::init() -- memset + zero pointers
- MessageIdGen::init(seed) -- seeds counter

Runtime (repeats per message, per loop iteration):
- timestamp_now_us() called multiple times per iteration
- engine.send() -- assigns IDs, calls transport, fills one AckTracker slot +
                   one RetryManager slot
- engine.pump_retries() -- iterates all ACK_TRACKER_CAPACITY RetryEntry slots
                           every call; O(ACK_TRACKER_CAPACITY) per pump
- engine.sweep_ack_timeouts() -- iterates all ACK_TRACKER_CAPACITY AckTracker
                                  slots every call; O(ACK_TRACKER_CAPACITY)
- engine.receive() -- may trigger on_ack() freeing slots

No dynamic allocation occurs at runtime. All data structures are fixed-size
arrays initialized at startup.


## 14. Known Risks / Observations

R1. First retry is immediate.
    RetryManager::schedule() sets next_retry_us = now_us (line 65), which means
    the very first collect_due() call will immediately retransmit the message
    even if the original send just succeeded. This causes a guaranteed duplicate
    transmission on every RELIABLE_RETRY message unless an ACK arrives before
    the first pump_retries() call. The DuplicateFilter on the receiver side is
    designed to absorb this.

R2. ACK tracking deadline is very short.
    recv_timeout_ms = 100 ms is used for both the receive poll interval and the
    ACK deadline in AckTracker. With pump_retries() calling collect_due() every
    100 ms and the ACK deadline also 100 ms, sweep_ack_timeouts() may mark ACK
    as timed out even when a retry is still in progress. These two subsystems
    (RetryManager and AckTracker) have independent expiry logic and can diverge.

R3. RetryManager and AckTracker use different expiry mechanisms.
    AckTracker uses deadline_us = now_us + recv_timeout_ms (100 ms window).
    RetryManager uses env.expiry_time_us (5-second window from send_test_message).
    A message can be in PENDING state in AckTracker (timed out) while still
    actively retrying in RetryManager -- they are not coordinated.

R4. Stack usage in pump_retries().
    MessageEnvelope retry_buf[MSG_RING_CAPACITY] is allocated on the stack.
    If MSG_MAX_PAYLOAD_BYTES is large (e.g. 1024) and MSG_RING_CAPACITY is
    also large (e.g. 16), this is 16 KB+ on the stack per pump call.

R5. send_via_transport() return ignored in pump loop is partial.
    Retry send failures are logged but the retry slot is NOT deactivated on
    transport failure. The slot will be retried again on the next pump cycle.
    This could cause repeated transport-layer errors to accumulate.

R6. Backoff doubling starts from the configured backoff_ms, but that value is
    applied to the *second* retry, not the first. The first retry fires
    immediately (next_retry_us = now_us at schedule time). So the effective
    sequence is: T+0, T+backoff, T+3*backoff, T+7*backoff, etc.


## 15. Unknowns / Assumptions

U1. [ASSUMPTION] cfg.channels[0].retry_backoff_ms default value: not visible
    without ChannelConfig.cpp / channel_config_default(). Documents assume 1000 ms.

U2. [ASSUMPTION] ACK_TRACKER_CAPACITY and MSG_RING_CAPACITY values: referenced
    as compile-time constants in Types.hpp (not read). Documents assume small
    values (e.g. 16 and 16).

U3. [ASSUMPTION] MessageIdGen::next() returns a monotonically increasing uint64_t
    starting from 1. Implementation not read (MessageId.hpp/cpp not in scope).

U4. [ASSUMPTION] ImpairmentEngine is disabled by default (imp_cfg.enabled = false
    from impairment_config_default()). If enabled, process_outbound() may return
    ERR_IO, silently dropping the retry packet.

U5. [ASSUMPTION] tcp_send_frame() implements a length-prefixed framing protocol
    matching the deserialization side. Implementation in SocketUtils not read.

U6. [ASSUMPTION] Logger::log() is thread-safe or not called from multiple
    threads simultaneously. In single-threaded context this is trivially true.

U7. The server may send a DATA echo rather than a separate ACK envelope.
    If the server echoes via engine.send() with RELIABLE_RETRY (as seen in
    Server.cpp send_echo_reply()), it does NOT send a bare ACK. The client's
    DeliveryEngine::receive() only cancels retry when it sees message_type=ACK.
    Whether the server also sends an explicit ACK is not visible from Server.cpp.
    [ASSUMPTION] The client's retry for message_id=1 is only cancelled if an
    ACK envelope is received; the DATA echo reply alone does NOT cancel it.

U8. DEDUP_WINDOW_SIZE value unknown. If smaller than max_retries, the duplicate
    filter may evict earlier entries before all retries are received, allowing
    duplicates through on the receiver side.