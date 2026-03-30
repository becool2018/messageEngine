## 1. Use Case Overview

Use Case: UC_05 -- Server Echo Reply

The Server process receives a DATA message from a connected client and
immediately echoes it back by swapping source_id and destination_id, copying
the payload, setting a new expiry (+10 seconds), and calling engine.send()
with the reply envelope.

The server's main loop is bounded by MAX_LOOP_ITERS = 100 000. Each iteration:
  1. Checks the stop flag (SIGINT).
  2. Calls engine.receive() to attempt to get a DATA message.
  3. If DATA received: increments counter, logs message details (ID, node, len,
     payload as string), calls send_echo_reply().
  4. Calls engine.pump_retries() (for its own outbound retries).
  5. Calls engine.sweep_ack_timeouts().

Requirement coverage: CLAUDE.md §3.3 (delivery semantics), application
requirements §3.1 (message envelope), §3.2 (messaging utilities), §7 (logging).


## 2. Entry Points

Primary entry point:
    Server.cpp :: main()  (line 130)
      Loop body starting line 207

Receive path:
    main() -> engine.receive()  (line 222)
      -> DeliveryEngine::receive()  (DeliveryEngine.cpp line 147)
        -> TcpBackend::receive_message()  (TcpBackend.cpp line 325)

Echo send path:
    main() -> send_echo_reply()  (line 234)
      -> engine.send()  (line 117 of Server.cpp)
        -> DeliveryEngine::send()  (DeliveryEngine.cpp line 75)
          -> TcpBackend::send_message()  (TcpBackend.cpp line 249)


## 3. End-to-End Control Flow (Step-by-Step)

--- Phase A: Server Initialization ---

A1. Server.cpp main() (line 130): parse argv, bind_port = 9000 (default).

A2. transport_config_default(cfg); cfg.kind=TCP, cfg.is_server=true,
    cfg.bind_port=9000, cfg.local_node_id=1, cfg.num_channels=1.
    cfg.bind_ip = "0.0.0.0" (copied char by char, bounded loop).
    cfg.channels[0]: reliability=RELIABLE_RETRY, ordering=ORDERED, max_retries=3,
    recv_timeout_ms=100.

A3. TcpBackend transport; transport.init(cfg):
      m_is_server = true
      m_recv_queue.init()
      impairment_config_default(imp_cfg); imp_cfg.enabled = cfg.channels[0].impairments_enabled
      m_impairment.init(imp_cfg)
      m_listen_fd = socket_create_tcp()
      socket_set_reuseaddr(m_listen_fd)
      socket_bind(m_listen_fd, "0.0.0.0", 9000)
      socket_listen(m_listen_fd, MAX_TCP_CONNECTIONS)
      m_open = true
      logs "Server listening on 0.0.0.0:9000"

A4. DeliveryEngine engine; engine.init(&transport, cfg.channels[0], node_id=1):
      m_ack_tracker.init(); m_retry_manager.init(); m_dedup.init(); m_id_gen.init(1)
      m_initialized = true

A5. signal(SIGINT, signal_handler): installs handler; g_stop_flag initialized to 0.

--- Phase B: Main Loop (iter = 0..MAX_LOOP_ITERS-1) ---

B1. Check stop flag: if g_stop_flag != 0 -> break (SIGINT received).

B2. now_us = timestamp_now_us()   [CLOCK_MONOTONIC]

B3. envelope_init(received)       [memset MessageEnvelope to 0; type=INVALID]

--- Phase C: engine.receive() --message arrives from client ---

C1. engine.receive(received, 100, now_us):  [DeliveryEngine.cpp line 147]
      assert(m_initialized); assert(now_us > 0)
      res = m_transport->receive_message(received, 100)

C2. TcpBackend::receive_message(received, 100):  [TcpBackend.cpp line 325]
      assert(m_open)
      res = m_recv_queue.pop(received)   [check buffered messages first]

      If queue empty (ERR_EMPTY [ASSUMPTION]):
        m_is_server = true -> accept_clients():
          m_client_count < MAX_TCP_CONNECTIONS:
            client_fd = socket_accept(m_listen_fd)
            [if client not yet connected: client_fd < 0 -> returns OK]
            [if client connects: m_client_fds[m_client_count]=client_fd;
             ++m_client_count; logs "Accepted client N"]

        poll_count = (100 + 99) / 100 = 1 (capped; 100ms timeout -> 1 iteration)
        [actually: (100+99)/100 = 1; cap at 50 -> stays 1]

        for attempt=0..0:
          for i=0..MAX_TCP_CONNECTIONS-1:
            if m_client_fds[i] < 0 -> continue
            recv_from_client(m_client_fds[i], 100):
              tcp_recv_frame(fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, 100, &out_len)
                [blocks up to 100ms waiting for framed data]
                [if data arrives: reads length prefix, reads payload bytes]
              Serializer::deserialize(m_wire_buf, out_len, env)
                [decodes wire bytes into MessageEnvelope struct]
              m_recv_queue.push(env)   [enqueues deserialized envelope]
              returns Result::OK

          res = m_recv_queue.pop(received)
          [if message in queue -> returns OK, received filled]

          [impairment delayed messages]:
            now_us2 = timestamp_now_us()
            delayed_count = m_impairment.collect_deliverable(now_us2, delayed, IMPAIR_DELAY_BUF_SIZE)
            [if any delayed -> push to m_recv_queue]
            m_recv_queue.pop(received) [check again]

        if nothing received -> return ERR_TIMEOUT

C3. Back in DeliveryEngine::receive() (line 159):
      res = OK (message received)
      assert(envelope_valid(received))
        envelope_valid: message_type != INVALID, payload_length <= MSG_MAX_PAYLOAD_BYTES,
                        source_id != NODE_ID_INVALID

C4. Expiry check (line 169):
      timestamp_expired(received.expiry_time_us, now_us)
      [client set expiry_time_us = send_time + 5s]
      If not expired: continue
      If expired: return ERR_EXPIRED (dropped; echo not sent)

C5. Control message check (line 177):
      envelope_is_control(received):
        message_type == ACK || NAK || HEARTBEAT?
      [client sends DATA -> false; skip this branch]

C6. Data path (line 196):
      assert(envelope_is_data(received))   [message_type == DATA]

C7. Duplicate suppression (line 199):
      received.reliability_class == RELIABLE_RETRY -> true
      dedup_res = m_dedup.check_and_record(received.source_id, received.message_id):
        is_duplicate(src, msg_id):
          linear scan of m_window[0..DEDUP_WINDOW_SIZE-1]
          [if found: return ERR_DUPLICATE]
          [if not found: return false]
        if not duplicate:
          record(src, msg_id):
            m_window[m_next] = {src, msg_id, valid=true}
            m_next = (m_next + 1) % DEDUP_WINDOW_SIZE
            if m_count < DEDUP_WINDOW_SIZE: ++m_count
          return Result::OK
      dedup_res == OK -> continue

C8. Log and return (line 211):
      Logger::log(INFO, "DeliveryEngine",
                  "Received data message_id=%llu from src=%u, length=%u",
                  received.message_id, received.source_id, received.payload_length)
      assert(envelope_valid(received))
      return Result::OK

--- Phase D: Server main loop processes DATA message ---

D1. Back in Server.cpp main() (line 222):
      res = engine.receive(received, 100, now_us)
      result_ok(res) && envelope_is_data(received):
        received.message_type == DATA -> true

D2. ++messages_received   [counter update]

D3. Logger::log(INFO, "Server",
                "Received msg#%llu from node %u, len %u: ",
                received.message_id, received.source_id, received.payload_length)

D4. print_payload_as_string(received.payload, received.payload_length):
      assert(payload != nullptr); assert(len <= MSG_MAX_PAYLOAD_BYTES)
      char buf[256]
      copy_len = min(len, 255)
      snprintf(buf, 256, "%.*s", copy_len, (const char*)payload)
      if snprintf ret < 0: printf("[error copying payload]\n")
      else: printf("%s\n", buf)

--- Phase E: send_echo_reply() ---

E1. echo_res = send_echo_reply(engine, received, now_us):  [Server.cpp line 90]

E2. Inside send_echo_reply():
      assert(envelope_valid(received)); assert(now_us > 0)
      MessageEnvelope reply;
      envelope_init(reply)   [memset 0, type=INVALID]

E3. Build echo reply:
      reply.message_type      = MessageType::DATA
      reply.source_id         = received.destination_id   [swapped: server's node ID = 1]
      reply.destination_id    = received.source_id        [swapped: client's node ID = 2]
      reply.priority          = received.priority         [inherited = 0]
      reply.reliability_class = received.reliability_class [inherited = RELIABLE_RETRY]
      reply.timestamp_us      = now_us
      reply.expiry_time_us    = timestamp_deadline_us(now_us, 10000U)
                              = now_us + 10 000 000 us   [10-second window]
      reply.payload_length    = received.payload_length

E4. Bounds check:
      if reply.payload_length > MSG_MAX_PAYLOAD_BYTES: return ERR_INVALID
      [for "Hello from client #N", len is small; check passes]

E5. memcpy(reply.payload, received.payload, reply.payload_length)
    [copies "Hello from client #N" into reply]

E6. engine.send(reply, now_us):  [DeliveryEngine.cpp line 75]
      assert(m_initialized); assert(now_us > 0)
      reply.source_id  = m_local_id = 1   [OVERWRITTEN: DeliveryEngine sets source_id]
      reply.message_id = m_id_gen.next()  [e.g. server assigns message_id=1]
      reply.timestamp_us = now_us

      NOTE: reply.source_id was set to received.destination_id in E3, then
      OVERWRITTEN to m_local_id (=1) by DeliveryEngine::send() line 86.
      For the server with local_id=1 and received.destination_id=1 (server),
      these values are the same. No semantic difference in this demo.

E7. send_via_transport(reply):  [DeliveryEngine.cpp line 54]
      assert(m_initialized); assert(m_transport != nullptr); assert(envelope_valid(reply))
      res = m_transport->send_message(reply)

E8. TcpBackend::send_message(reply):  [TcpBackend.cpp line 249]
      assert(m_open); assert(envelope_valid(reply))
      wire_len = 0
      res = Serializer::serialize(reply, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
        [encodes all fields + payload into m_wire_buf with length prefix]
      if !result_ok(res): log WARNING_LO, return

      now_us2 = timestamp_now_us()
      res = m_impairment.process_outbound(reply, now_us2)
        [if impairments disabled: returns OK, no drop]
        [if loss impairment active: may return ERR_IO -> silent drop]
      if res == ERR_IO: return OK   [drop]

      delayed_count = m_impairment.collect_deliverable(now_us2, delayed, IMPAIR_DELAY_BUF_SIZE)

      if m_client_count == 0: log WARNING_LO, return OK   [no connected clients]

      for i=0..MAX_TCP_CONNECTIONS-1:
        if m_client_fds[i] < 0: continue
        tcp_send_frame(m_client_fds[i], m_wire_buf, wire_len, send_timeout_ms)
          [writes length prefix + payload bytes to socket fd]
          [if write fails: logs WARNING_LO; continues to next fd]

      [send delayed messages, if any]

      assert(wire_len > 0)
      return Result::OK

E9. Back in DeliveryEngine::send():
      send_via_transport result = OK
      reply.reliability_class == RELIABLE_RETRY -> enter tracking block (line 100):
        ack_deadline = timestamp_deadline_us(now_us, recv_timeout_ms=100)
        m_ack_tracker.track(reply, ack_deadline)   [server tracks its own echo for ACK]
      reply.reliability_class == RELIABLE_RETRY -> enter retry block (line 118):
        m_retry_manager.schedule(reply, max_retries=3, backoff_ms, now_us)
        [server schedules retries for the echo reply, in case client does not ACK]

      assert(reply.source_id == 1); assert(reply.message_id != 0)
      logs "Sent message_id=N, reliability=2"
      return Result::OK

E10. Back in send_echo_reply():
       if !result_ok(res): log WARNING_LO, return
       return Result::OK

--- Phase F: Back in main loop ---

F1. if result_ok(echo_res): ++messages_sent

F2. retried = engine.pump_retries(now_us)
    [collects due RetryEntry slots; re-sends any messages whose next_retry_us <= now_us]
    [for the just-sent echo: next_retry_us = now_us (immediate) -> fires right away]

F3. if retried > 0: logs "Retried N message(s)"

F4. ack_timeouts = engine.sweep_ack_timeouts(now_us)
    [sweeps AckTracker PENDING slots past deadline (100ms)]
    [the echo reply's ACK deadline is now_us + 100ms; unlikely to have expired yet]

F5. if ack_timeouts > 0: logs WARNING_HI "Detected N ACK timeout(s)"

F6. iter increments; loop repeats from B1.


## 4. Call Tree (Hierarchical)

Server::main()
  transport.init(cfg)
    TcpBackend::TcpBackend()          [constructor, zeroes fds]
    TcpBackend::init(cfg)
      m_recv_queue.init()
      ImpairmentEngine::init()
      socket_create_tcp()
      socket_set_reuseaddr()
      socket_bind()
      socket_listen()
  engine.init(&transport, cfg.channels[0], 1)
    AckTracker::init()
    RetryManager::init()
    DuplicateFilter::init()
    MessageIdGen::init(1)
  signal(SIGINT, signal_handler)

  [loop iter=0..MAX_LOOP_ITERS-1]
    g_stop_flag check
    timestamp_now_us()
    envelope_init(received)
    engine.receive(received, 100, now_us)
      TcpBackend::receive_message(received, 100)
        m_recv_queue.pop(received)
        TcpBackend::accept_clients()
          socket_accept(m_listen_fd)
        [poll loop]:
          recv_from_client(client_fd, 100)
            tcp_recv_frame()
            Serializer::deserialize()
            m_recv_queue.push()
          m_recv_queue.pop()
          timestamp_now_us()
          ImpairmentEngine::collect_deliverable()
          m_recv_queue.push(delayed[i])
          m_recv_queue.pop()
      envelope_valid() [assertion]
      timestamp_expired(received.expiry_time_us, now_us)
      envelope_is_control(received)     [false for DATA]
      envelope_is_data(received)        [assertion]
      DuplicateFilter::check_and_record(src, msg_id)
        DuplicateFilter::is_duplicate()
        DuplicateFilter::record()
      Logger::log(INFO, "Received data...")
    [if result_ok && is_data]:
      Logger::log(INFO, "Received msg#...")
      print_payload_as_string(received.payload, received.payload_length)
        snprintf()
        printf()
      send_echo_reply(engine, received, now_us)
        envelope_init(reply)
        [fill reply fields: swap src/dst, copy payload]
        timestamp_deadline_us(now_us, 10000)
        memcpy(reply.payload, ...)
        engine.send(reply, now_us)
          DeliveryEngine::send_via_transport(reply)
            TcpBackend::send_message(reply)
              Serializer::serialize()
              timestamp_now_us()
              ImpairmentEngine::process_outbound()
              ImpairmentEngine::collect_deliverable()
              tcp_send_frame()              [per active client fd]
              [send delayed messages if any]
          timestamp_deadline_us(now_us, recv_timeout_ms)
          AckTracker::track(reply, deadline)
            envelope_copy()
          RetryManager::schedule(reply, 3, backoff_ms, now_us)
            envelope_copy()
    engine.pump_retries(now_us)
      RetryManager::collect_due(now_us, retry_buf, MSG_RING_CAPACITY)
        [per active slot: expiry, exhaustion, due checks, backoff doubling]
      [per collected]: DeliveryEngine::send_via_transport()
                         TcpBackend::send_message()
    engine.sweep_ack_timeouts(now_us)
      AckTracker::sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY)
        [PENDING past deadline -> FREE; ACKED -> FREE]

  transport.close()
    socket_close(m_listen_fd)
    [for each client fd]: socket_close(m_client_fds[i])
  signal(SIGINT, old_handler)


## 5. Key Components Involved

Component                   File                       Role in UC_05
--------------------------  -------------------------  ------------------------------
Server::main()              src/app/Server.cpp         Orchestrates init, main loop,
                            (line 130)                 stop flag, counters
send_echo_reply()           src/app/Server.cpp         Builds echo: swaps src/dst,
                            (line 90)                  copies payload, sets +10s expiry,
                                                       calls engine.send()
print_payload_as_string()   src/app/Server.cpp         Logs payload content; bounded
                            (line 64)                  snprintf + printf
signal_handler()            src/app/Server.cpp         Sets g_stop_flag on SIGINT
                            (line 55)
DeliveryEngine::receive()   src/core/DeliveryEngine.cpp Checks expiry, dedup, routes
                            (line 147)                 ACKs; returns OK + env for DATA
DeliveryEngine::send()      src/core/DeliveryEngine.cpp Assigns server message_id,
                            (line 75)                  overwrites source_id, tracks
                                                       and schedules echo reply
DuplicateFilter::           src/core/DuplicateFilter.cpp Deduplicates RELIABLE_RETRY
  check_and_record()        (line 91)                  messages from client
AckTracker::track()         src/core/AckTracker.cpp    Tracks server's echo reply
                            (line 42)                  for ACK from client
RetryManager::schedule()    src/core/RetryManager.cpp  Schedules echo reply retries
                            (line 44)                  in case client does not ACK
TcpBackend::receive_message() src/platform/TcpBackend.cpp Accepts clients, polls fds,
                            (line 325)                 deserializes, queues messages
TcpBackend::accept_clients() src/platform/TcpBackend.cpp Non-blocking accept; called
                            (line 161)                 every receive iteration
TcpBackend::send_message()  src/platform/TcpBackend.cpp Serializes echo, applies
                            (line 249)                 impairment, sends to all fds
recv_from_client()          src/platform/TcpBackend.cpp Framed TCP read, deserialize,
                            (line 193)                 push to queue; handles disconnect
timestamp_deadline_us()     src/core/Timestamp.hpp     +10s expiry for echo reply
timestamp_expired()         src/core/Timestamp.hpp     Checked before echo dispatch
Serializer::deserialize()   src/core/Serializer.hpp    Wire -> MessageEnvelope
Serializer::serialize()     src/core/Serializer.hpp    MessageEnvelope -> wire
envelope_make_ack()         src/core/MessageEnvelope.hpp [NOT called by server; server
                                                       does NOT send explicit ACK]


## 6. Branching Logic / Decision Points

Decision                      Location                   Outcomes
----------------------------  -------------------------  ----------------------------
g_stop_flag != 0              Server.cpp line 209        break loop; proceed to cleanup
recv queue empty              TcpBackend.cpp line 330    proceed to accept_clients
                                                         + poll loop
m_is_server                   TcpBackend.cpp line 336    true -> accept_clients();
                                                         false (client) -> skip
m_client_count >=             TcpBackend::accept_clients No new accept attempted;
MAX_TCP_CONNECTIONS           line 166                   return OK
socket_accept < 0             TcpBackend.cpp line 172    No pending connection;
                                                         return OK (non-blocking)
tcp_recv_frame fails          recv_from_client() line 200 Close fd, shift array,
                                                         --m_client_count, ERR_IO
Deserialize fails             recv_from_client() line 228 WARNING_LO; return error;
                                                         message not queued
recv queue full               recv_from_client() line 237 WARNING_HI; message dropped
result_ok && is_data          Server.cpp line 224        true -> log + echo;
                                                         false -> skip echo
expiry check in receive()     DeliveryEngine.cpp line 169 expired -> ERR_EXPIRED ->
                                                         echo NOT sent
duplicate check               DeliveryEngine.cpp line 201 ERR_DUPLICATE -> ERR_DUPLICATE
                                                         returned; echo NOT sent for
                                                         repeat messages
is_control check              DeliveryEngine.cpp line 177 ACK from client -> process
                                                         on_ack() pair; not echoed
payload_length >              send_echo_reply() line 111  return ERR_INVALID; no echo
MSG_MAX_PAYLOAD_BYTES
impairment drops echo         TcpBackend.cpp line 266    silent drop (ERR_IO -> OK)
m_client_count == 0           TcpBackend.cpp line 278    echo discarded; WARNING_LO
tcp_send_frame fails          TcpBackend.cpp line 290-294 WARNING_LO; continues to
                                                         next client fd


## 7. Concurrency / Threading Behavior

Single-threaded process. All loop iterations (receive, log, echo, pump,
sweep) execute sequentially in the same thread.

Signal handler: g_stop_flag is volatile sig_atomic_t (Server.cpp line 51).
The signal handler (line 55) sets g_stop_flag = 1; this is the only write.
main() reads it at line 209. This is a standard POSIX signal-safe pattern.

accept_clients() is called on each receive_message() pass (line 336-337).
It calls socket_accept() once (non-blocking via [ASSUMPTION] SOCK_NONBLOCK or
select-with-timeout). If no client is pending, it returns OK without blocking.

[ASSUMPTION]: socket_accept() is configured non-blocking so it does not hang
waiting for a connection. If it blocks, the server's receive loop would stall.
The comment "Non-blocking, ignore result" at line 337 implies this.

No mutexes, no std::thread, no std::atomic usage in Server.cpp or the
components it calls. Thread safety is trivially satisfied by single-threaded
design.

The bounded loop (MAX_LOOP_ITERS = 100 000) ensures the process eventually
terminates even without SIGINT, satisfying Power of 10 rule 2.


## 8. Memory & Ownership Semantics (C/C++ Specific)

Object                      Location           Lifetime / Ownership
--------------------------  -----------------  --------------------------------
TransportConfig cfg         stack (main)       POD struct; copied into TcpBackend
TcpBackend transport        stack (main)       Owns: m_listen_fd, m_client_fds[],
                                               m_recv_queue, m_impairment, m_wire_buf
DeliveryEngine engine       stack (main)       Owns: AckTracker, RetryManager,
                                               DuplicateFilter, MessageIdGen by value
MessageEnvelope received    stack (main loop)  Re-initialized each iteration via
                                               envelope_init(). Valid only within
                                               the if-block that processes it.
MessageEnvelope reply       stack              Allocated inside send_echo_reply()
                (send_echo_reply)              on caller's stack. Passed by ref to
                                               engine.send() which copies it
                                               into RetryManager and AckTracker
                                               slots via envelope_copy() (memcpy).
                                               Original stack copy can be destroyed
                                               after send() returns.
char buf[256]               stack              Inside print_payload_as_string();
                (print_payload_as_string)      scoped to that function; no escape.
m_wire_buf                  TcpBackend member  Shared scratch buffer; overwritten
                                               on every serialize/deserialize call.
                                               Only safe in single-threaded use.

Key copy points:
1. envelope_copy(m_slots[i].env, reply) in AckTracker::track() and
   RetryManager::schedule() -- full memcpy of sizeof(MessageEnvelope).
2. memcpy(reply.payload, received.payload, payload_length) in send_echo_reply()
   -- bounded by MSG_MAX_PAYLOAD_BYTES check.
3. envelope_init(received) at loop top -- memset zeroes the struct before reuse,
   preventing stale data from previous iterations bleeding through.

TransportInterface* m_transport in DeliveryEngine is a non-owning pointer.
TcpBackend is declared before DeliveryEngine in main(), so it is destroyed AFTER
DeliveryEngine (LIFO stack order). This is correct; m_transport remains valid
for the entire lifetime of DeliveryEngine.


## 9. Error Handling Flow

Error                         Detection                    Handling
----------------------------  ---------------------------  -------------------------
TcpBackend init fails         transport.init() != OK       FATAL log; return 1
                              Server.cpp line 178-182      (process exits)
signal() fails                assert(old_handler != SIG_ERR) Abort in debug
                              Server.cpp line 197
receive() returns timeout     result_ok(res) = false       No echo; loop continues;
                              Server.cpp line 224          pump/sweep still called
receive() returns expired     ERR_EXPIRED, result_ok=false No echo; WARNING_LO logged
                                                           in DeliveryEngine::receive
receive() returns duplicate   ERR_DUPLICATE, result_ok=false No echo; INFO logged in
                                                           DeliveryEngine::receive
recv_from_client fails        ERR_IO returned              Close fd, remove from
(connection lost)             TcpBackend.cpp line 200      active array; next iter
                                                           tries remaining clients
Deserialize fails             WARNING_LO in               Message not queued;
                              recv_from_client             loop continues
Recv queue full               WARNING_HI in               Message dropped; no
                              recv_from_client             ERR propagated up
echo payload too large        send_echo_reply() line 111   return ERR_INVALID;
                                                           ++messages_sent NOT called
ACK tracker full              AckTracker::track ERR_FULL   WARNING_HI in
                                                           DeliveryEngine::send;
                                                           send still returns OK
Retry table full              RetryManager::schedule       WARNING_HI; send still OK
                              ERR_FULL
Impairment drops echo         TcpBackend silent ERR_IO->OK Engine sees OK; no notification
tcp_send_frame fails          WARNING_LO; loop continues   Other client fds still sent
                              TcpBackend line 291
snprintf fails in             ret < 0 branch               printf("[error copying...]\n");
print_payload_as_string                                    no abort
ACK timeout on echo           sweep_ack_timeouts() -> sweep WARNING_HI "Detected N
                              _expired fires at deadline   ACK timeout(s)"; no
                                                           automatic re-echo from server
                                                           side [server relies on
                                                           RetryManager for that]


## 10. External Interactions

Interaction            API Used                    Direction   Notes
---------------------  --------------------------  ----------  --------------------
TCP accept             socket_accept(m_listen_fd)  Inbound     Called each receive
                                                               iteration; non-blocking
                                                               [ASSUMPTION]
TCP receive            tcp_recv_frame()            Inbound     Framed read; blocks up
                       -> read()/recv() [ASSUMPTION]            to 100ms per client fd
TCP send               tcp_send_frame()            Outbound    Framed write to all
                       -> write()/send() [ASSUMPTION]           active client fds
POSIX monotonic clock  clock_gettime(CLOCK_MONOTONIC Inbound   Called at loop top and
                       )                                        inside TcpBackend
POSIX signal           signal(SIGINT, handler)     Inbound     Stop signal from user
                       (volatile g_stop_flag)
Logger output          Logger::log()               Outbound    All INFO/WARNING events
                                                               [ASSUMPTION: stdout]
printf()               printf("%s\n", buf)         Outbound    Payload content printed
                       in print_payload_as_string              directly to stdout


## 11. State Changes / Side Effects

On each DATA message received and echo sent:

Component                State Field           Before           After
-----------------------  --------------------  ---------------  ---------------
messages_received        uint32_t in main()    N                N+1
messages_sent            uint32_t in main()    M                M+1 (if echo OK)
DuplicateFilter          m_window[m_next]      unset            {src, msg_id, valid}
DuplicateFilter          m_next                X                (X+1)%DEDUP_WINDOW_SIZE
DuplicateFilter          m_count               C                min(C+1, DEDUP_WINDOW_SIZE)
MessageIdGen (server)    m_counter             K                K+1 (echo gets new ID)
AckTracker (server)      m_slots[j].state      FREE             PENDING
AckTracker (server)      m_count               A                A+1
RetryManager (server)    m_slots[j].active     false            true
RetryManager (server)    m_slots[j].retry_count 0               0
RetryManager (server)    m_slots[j].next_retry_us 0             now_us (immediate)
RetryManager (server)    m_count               R                R+1
TcpBackend               m_wire_buf            prev frame       echo reply frame bytes

On g_stop_flag (SIGINT):
  signal_handler: g_stop_flag = 1 (atomic write to sig_atomic_t)
  main loop: detects g_stop_flag != 0 on next iteration, breaks

On TCP client disconnect (recv_from_client fails):
  m_client_fds[i] -> closed and set to -1
  m_client_fds shifted to compact array
  --m_client_count


## 12. Sequence Diagram (ASCII)

Client         TcpBackend(S)  DeliveryEngine(S)  Server::main  send_echo_reply  AckTracker  RetryManager  Logger
   |               |               |                  |               |              |              |          |
   |--DATA frame-->|               |                  |               |              |              |          |
   |               |--tcp_recv_frame [frame arrives]  |               |              |              |          |
   |               |--Serializer::deserialize()       |               |              |              |          |
   |               |--m_recv_queue.push(env)          |               |              |              |          |
   |               |               |                  |               |              |              |          |
   |               |         engine.receive(100ms)----+               |              |              |          |
   |               |<--receive_message()              |               |              |              |          |
   |               |   m_recv_queue.pop() -> OK       |               |              |              |          |
   |               |               |                  |               |              |              |          |
   |               |   envelope_valid(env) [assert]   |               |              |              |          |
   |               |   timestamp_expired()? NO        |               |              |              |          |
   |               |   envelope_is_control()? NO      |               |              |              |          |
   |               |   envelope_is_data()? YES        |               |              |              |          |
   |               |   dedup.check_and_record()------>|               |              |              |          |
   |               |   <-- OK (not duplicate)         |               |              |              |          |
   |               |               |------Logger: "Received data msg"-+              |              |          |
   |               |<--OK (env)----+                  |               |              |              |          |
   |               |               |                  |               |              |              |          |
   |               |               |    result_ok && is_data -> true  |              |              |          |
   |               |               |                  |--log "Received msg#..."----->|              |          |
   |               |               |                  |--print_payload_as_string()   |              |          |
   |               |               |                  |   printf("Hello from client")|              |          |
   |               |               |                  |               |              |              |          |
   |               |               |                  |--send_echo_reply(engine,env,now)            |          |
   |               |               |                  |               |              |              |          |
   |               |               |                  |    envelope_init(reply)      |              |          |
   |               |               |                  |    reply.dst = env.src (=2)  |              |          |
   |               |               |                  |    reply.src = env.dst (=1)  |              |          |
   |               |               |                  |    timestamp_deadline_us(+10s)              |          |
   |               |               |                  |    memcpy payload            |              |          |
   |               |               |                  |               |              |              |          |
   |               |               |<---------engine.send(reply,now)--+              |              |          |
   |               |               | reply.source_id = 1 (local_id)  |              |              |          |
   |               |               | reply.message_id = next()        |              |              |          |
   |               |               |--send_via_transport(reply)------>|              |              |          |
   |               |<--send_message(reply)            |               |              |              |          |
   |               |   serialize() -> m_wire_buf      |               |              |              |          |
   |               |   impairment.process_outbound()  |               |              |              |          |
   |               |   tcp_send_frame(fd, buf, len)   |               |              |              |          |
   |<--DATA frame--+               |                  |               |              |              |          |
   |               |               |<--OK-------------+              |              |              |          |
   |               |               |--track(reply, deadline)-------->|              |              |          |
   |               |               |                  |              |<--OK---------+              |          |
   |               |               |--schedule(reply, 3, backoff, now)              |              |          |
   |               |               |                  |              |       slot.active=true       |          |
   |               |               |<--OK-------------+              |              |<--OK---------+          |
   |               |               |------Logger: "Sent message_id=N"               |              |--------->|
   |               |               |<--OK (echo sent)--+             |              |              |          |
   |               |               |                  |++messages_sent              |              |          |
   |               |               |                  |               |              |              |          |
   |               |               |         pump_retries(now_us)----+              |              |          |
   |               |               |   collect_due(): echo due now   |              |              |          |
   |               |               |   send_via_transport(echo copy) |              |              |          |
   |<--DATA frame--+               |   [immediate retry of echo]     |              |              |          |
   |               |               |                  |               |              |              |          |
   |               |               |         sweep_ack_timeouts()-----+              |              |          |
   |               |               |   sweep_expired(): deadline not yet passed      |              |          |
   |               |               |   (ACK deadline = now+100ms)    |              |              |          |


## 13. Initialization vs Runtime Flow

Initialization (runs once):
  TcpBackend::init()    -- socket, bind, listen; m_open=true
  DeliveryEngine::init() -- zeroes all tracking structures; m_initialized=true
  signal()              -- installs SIGINT handler

Runtime (per loop iteration, up to 100 000 times):
  timestamp_now_us()      -- syscall (CLOCK_MONOTONIC)
  envelope_init(received) -- memset; ensures clean state
  engine.receive()        -- poll TCP socket; deserialize; expiry/dedup checks
  [if DATA]: send_echo_reply() -> engine.send() -> TcpBackend::send_message()
  engine.pump_retries()   -- O(ACK_TRACKER_CAPACITY) scan of retry slots
  engine.sweep_ack_timeouts() -- O(ACK_TRACKER_CAPACITY) scan of ACK slots

No dynamic allocation at runtime. The only runtime syscalls are:
  clock_gettime(), recv()/read(), send()/write(), accept() -- all bounded.

The loop terminates when:
  a) SIGINT sets g_stop_flag (normal shutdown), or
  b) MAX_LOOP_ITERS (100 000) is reached (Power of 10 safety bound).


## 14. Known Risks / Observations

R1. Echo reply is scheduled for immediate retry (next_retry_us = now_us).
    Just as noted in UC_03 R1, RetryManager::schedule() sets next_retry_us =
    now_us. The first pump_retries() call after send_echo_reply() will
    immediately retransmit the echo. The client will receive a duplicate DATA
    message. The client's DuplicateFilter is responsible for suppressing it.

R2. Server does not send an explicit ACK to the client's original message.
    Server.cpp never calls envelope_make_ack() or sends a MessageType::ACK.
    The client's RetryManager will keep retrying the original DATA message
    for up to 5 seconds (expiry) or 3 retry attempts, whichever comes first.
    The server's DuplicateFilter will suppress the duplicate DATA deliveries
    (if the filter window is large enough and has not been evicted).

R3. Server schedules retries for the echo reply using RELIABLE_RETRY.
    The echo inherits reliability_class from the received message (RELIABLE_RETRY).
    The server's AckTracker and RetryManager track the echo reply. If the
    client does not send an ACK for the echo, the server retries 3 times with
    exponential backoff. Since the client also does not send explicit ACKs
    (no ACK send in Client.cpp), the server's echo retries will exhaust
    (WARNING_HI "Exhausted retries") and the server's AckTracker will sweep
    the entry at deadline+100ms (WARNING_HI "ACK timeout").

R4. DeliveryEngine::send() overwrites reply.source_id with m_local_id.
    send_echo_reply() sets reply.source_id = received.destination_id (the
    server's ID = 1). DeliveryEngine::send() line 86 overwrites this with
    m_local_id (also 1 for the server). In this demo they match, so there
    is no bug. However, if a relay scenario is introduced where
    received.destination_id != m_local_id, the override would mask the
    intended routing.

R5. print_payload_as_string uses printf not Logger.
    Payload content is printed directly to stdout via printf (line 83). This
    bypasses the Logger subsystem, so payload output is not decorated with
    severity level, component name, or timestamps. This is intentional (avoid
    logging payload content by default) but inconsistent with the logging
    requirement (§7.1).

R6. MAX_LOOP_ITERS = 100 000 with recv_timeout_ms = 100 ms.
    In the worst case (no messages ever arrive), the server runs for
    100 000 * 100 ms = 10 000 seconds (~2.8 hours) before exiting. This may
    be longer than intended for a demo. No idle timeout exists.

R7. accept_clients() is called once per receive_message() call.
    With recv_timeout_ms = 100 ms, accept_clients() is called approximately
    10 times per second. This is sufficient for the demo but would not scale
    to high connection rates. Only one accept() is attempted per call.


## 15. Unknowns / Assumptions

U1. [ASSUMPTION] socket_accept() is non-blocking (or uses a very short select
    timeout). Server.cpp comment "Non-blocking, ignore result" (line 337)
    implies this. If blocking, the receive loop would stall waiting for a
    connection even when existing clients are sending data.

U2. [ASSUMPTION] tcp_recv_frame() and tcp_send_frame() use a length-prefixed
    framing protocol (e.g. 4-byte big-endian length followed by payload bytes).
    Implementation in SocketUtils.hpp/cpp not read.

U3. [ASSUMPTION] Serializer::serialize() and deserialize() produce/consume a
    deterministic, endian-safe wire format. Implementation in Serializer.hpp
    not read.

U4. [ASSUMPTION] m_recv_queue is a fixed-capacity ring buffer (consistent with
    Power of 10 no-dynamic-allocation rule). Its capacity is unknown. When
    full, recv_from_client() logs WARNING_HI and drops the incoming message.

U5. [ASSUMPTION] The client sends ACK messages in response to the echo
    (server's DATA reply). If it does not (which appears to be the case from
    Client.cpp -- no explicit ACK send is present), the server's RetryManager
    will exhaust its retries and the AckTracker will time out. This is an
    expected operational behavior in the demo.

U6. [ASSUMPTION] ImpairmentEngine is disabled by default. If enabled, the echo
    reply could be delayed or dropped, causing the client to receive ERR_TIMEOUT
    in wait_for_echo() even though the server successfully sent the reply.

U7. [ASSUMPTION] The dedup window (DEDUP_WINDOW_SIZE) is large enough to hold
    all retransmissions from the client within the 5-second expiry window.
    If not, duplicate DATA messages could slip through to the echo path,
    causing the server to send duplicate echo replies.

U8. MAX_TCP_CONNECTIONS value is not visible without TcpBackend.hpp.
    [ASSUMPTION: small value, e.g. 4 or 8]. Only one client connects in the
    demo scenario.

U9. poll_count calculation: (100 + 99) / 100 = 1 iteration of the recv loop.
    This means only one pass of recv_from_client() is attempted per
    receive_message() call for a 100ms timeout. Combined with the 100ms
    blocking read in tcp_recv_frame(), total latency per receive call is
    approximately 100ms. This is consistent with RECV_TIMEOUT_MS = 100.
```

---

The three documents have been produced in full. Here is a brief summary of what was traced and documented:

**UC_03_reliable_retry_send.md** covers the complete lifecycle of a RELIABLE_RETRY send: ID and source assignment in `DeliveryEngine::send()`, immediate transport delivery via `TcpBackend::send_message()`, parallel registration in both `AckTracker::track()` and `RetryManager::schedule()`, and all subsequent retry passes through `pump_retries()` -> `collect_due()`. The backoff doubling formula (line 187 of RetryManager.cpp), the 60 000 ms cap (line 188), the exhaustion check (line 166), the immediate-first-retry risk, and the ACK cancellation path via `RetryManager::on_ack()` are all traced explicitly.

**UC_04_expired_message_drop.md** traces both expiry paths: Path A (receive-side, `timestamp_expired()` called at DeliveryEngine.cpp line 169, returning `ERR_EXPIRED` before dedup or application processing) and Path B (retry-side, `RetryManager::collect_due()` line 156 firing before the due-check so an expired message is never retransmitted). The `timestamp_deadline_us()` computation (+5s for client sends, +10s for echo replies) and the `expiry_us = 0` permanent-message edge case are documented with the actual code path.

**UC_05_server_echo_reply.md** traces the full Server main loop: TCP accept in `accept_clients()`, framed read in `recv_from_client()`, deserialization, expiry and dedup checks in `DeliveryEngine::receive()`, the payload log via `print_payload_as_string()`, `send_echo_reply()` building the swapped-endpoint envelope, `engine.send()` overwriting `source_id` with `m_local_id`, the downstream `TcpBackend::send_message()` path, and the fact that the server schedules its own echo reply for retry (RELIABLE_RETRY inherited), while never sending an explicit ACK to the client.agentId: af62ef93043b0b026 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 78749
tool_uses: 12
duration_ms: 542133</usage>