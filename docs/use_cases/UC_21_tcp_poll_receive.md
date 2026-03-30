Use Case: TcpBackend polls multiple client sockets and receives a framed message from one.
Goal: Trace TcpBackend::receive_message() → poll() on all client fds →
      recv_from_client() → tcp_recv_frame() → Serializer::deserialize() →
      RingBuffer::push() → pop() returned to caller.

## 1. Use Case Overview

Actor:        Upper-layer application code calling TcpBackend::receive_message().
Precondition: TcpBackend initialized (m_open == true); at least one client fd connected;
              a remote peer has sent a framed TCP message (4-byte length prefix + payload).
Postcondition: A MessageEnvelope is returned to the caller via the out-parameter; the
               ring buffer temporarily holds the message between push() and pop().
Trigger:      Caller invokes receive_message(envelope, timeout_ms).
Success path: Queue empty → accept_clients() (server) → poll attempt loop →
              recv_from_client() → tcp_recv_frame() (reads 4-byte header + N bytes) →
              Serializer::deserialize() → m_recv_queue.push() → m_recv_queue.pop()
              → envelope returned → Result::OK.
Failure paths:
  - Timeout with no message → Result::ERR_TIMEOUT.
  - tcp_recv_frame() fails → connection closed/removed; Result::ERR_IO.
  - deserialize() fails → WARNING_LO; Result returned; envelope NOT queued.
  - Ring buffer full → WARNING_HI; message dropped.

## 2. Entry Points

Primary entry: TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
               File: src/platform/TcpBackend.cpp, line 325
               Precondition assertion (line 327): assert(m_open)

Secondary entry (internal): TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms)
               File: src/platform/TcpBackend.cpp, line 193
               Precondition assertions (lines 195–196):
                   assert(client_fd >= 0)
                   assert(m_open)

## 3. End-to-End Control Flow (Step-by-Step)

--- Phase 1: Check receive queue first ---

Step 1.  Entry assertion (line 327): assert(m_open).

Step 2.  m_recv_queue.pop(envelope) (line 330) → RingBuffer.hpp line 159:
           h = m_head.load(relaxed)
           t = m_tail.load(acquire)
           cnt = t - h (unsigned)
           if cnt == 0: return Result::ERR_EMPTY.
           [First call: queue is empty (no prior messages).]
           envelope_copy(env, m_buf[h & RING_MASK])
           m_head.store(h+1, release)
           return Result::OK.

Step 3.  if result_ok(res): return res immediately. (Fast path if queue had something.)
         [Assumed empty on first call; fall through.]

--- Phase 2: Try to accept new clients (server mode only) ---

Step 4.  if m_is_server (line 336):
           (void)accept_clients() — result ignored.
           → TcpBackend.cpp line 161 (described fully in UC_19).
           If a new client connects here, it is added to m_client_fds[] and becomes
           available for the poll loop below.

--- Phase 3: Poll loop over all connected clients ---

Step 5.  Compute poll_count (line 341):
           poll_count = (timeout_ms + 99) / 100  — ceiling division, gives iterations
                        of 100 ms each.
           if poll_count > 50: poll_count = 50. (Cap at 5000 ms / 5 seconds.)
           [Example: timeout_ms = 1000 → poll_count = 10 iterations.]

Step 6.  Outer loop: attempt = 0..poll_count-1 (line 346):

  Step 6a. Inner loop: i = 0..MAX_TCP_CONNECTIONS-1 (= 0..7) (line 347):
             if m_client_fds[i] < 0: continue (slot unused).
             (void)recv_from_client(m_client_fds[i], 100U) (line 351).
             Return value ignored.

             → TcpBackend::recv_from_client(client_fd, 100) [TcpBackend.cpp:193]:

             [Step 6a-i]   assert(client_fd >= 0); assert(m_open).

             [Step 6a-ii]  tcp_recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES,
                           100, &out_len) (line 199) → SocketUtils.cpp line 429:

               Substep A: assert(fd>=0); assert(buf!=NULL); assert(buf_cap>0);
                          assert(out_len!=NULL).

               Substep B: Receive 4-byte length header:
                          socket_recv_exact(fd, header, 4, 100) → SocketUtils.cpp:337:
                            received = 0
                            while (received < 4):
                              poll({fd,POLLIN}, 1, 100ms)
                                → syscall: poll() on client_fd, timeout 100 ms.
                              if poll_result <= 0:
                                log WARNING_HI "recv poll timeout"
                                return false.  [→ tcp_recv_frame returns false]
                              recv(fd, &header[received], 4-received, 0)
                                → syscall: recv(), reads up to 4-received bytes.
                              if recv_result < 0: log WARNING_HI, return false.
                              if recv_result == 0: log WARNING_HI "socket closed",
                                return false.
                              received += recv_result.
                            assert(received == 4). Return true.

               Substep C: If socket_recv_exact(header) returns false:
                          tcp_recv_frame logs WARNING_HI "failed to receive header".
                          Returns false.

               Substep D: Parse frame_len from 4-byte header (big-endian):
                          frame_len = (header[0]<<24)|(header[1]<<16)|
                                      (header[2]<<8)|header[3].

               Substep E: Validate frame_len (line 454):
                          max_frame_size = Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES
                                        = 44 + 4096 = 4140 bytes. [ASSUMPTION: see §15]
                          if frame_len > max_frame_size || frame_len > buf_cap(=8192):
                            log WARNING_HI "frame size %u exceeds limit".
                            return false.

               Substep F: Receive payload (frame_len bytes):
                          if frame_len > 0:
                            socket_recv_exact(fd, buf, frame_len, 100)
                            → Same poll/recv loop as Substep B, but for frame_len bytes.
                            Iterates until exactly frame_len bytes received or timeout/error.

               Substep G: *out_len = frame_len.
                          assert(*out_len <= buf_cap).
                          assert(*out_len <= max_frame_size).
                          Returns true.

             [Step 6a-iii] Back in recv_from_client():
               If tcp_recv_frame returns false (line 200):
                 log WARNING_LO "recv_frame failed; closing connection".
                 Find client_fd in m_client_fds[0..7] (linear search, lines 206–221):
                   socket_close(m_client_fds[i]) → syscall: close(fd).
                   m_client_fds[i] = -1.
                   Shift remaining fds left to compact array (inner loop j=i..m_client_count-2):
                     m_client_fds[j] = m_client_fds[j+1].
                   m_client_fds[m_client_count-1] = -1.
                   --m_client_count.
                   assert(m_client_count < MAX_TCP_CONNECTIONS || m_client_count == 0).
                   return Result::ERR_IO.
                 [If fd not found in array: return Result::ERR_IO.]

             [Step 6a-iv]  If tcp_recv_frame returns true (frame received):
               Serializer::deserialize(m_wire_buf, out_len, env) (line 227)
               → Serializer.cpp line 173:

                 Substep A: assert(buf!=nullptr); assert(buf_len<=0xFFFFFFFF).
                 Substep B: if buf_len < WIRE_HEADER_SIZE: return ERR_INVALID.
                 Substep C: envelope_init(env) — zero/default the output envelope.
                 Substep D: Sequential read_u8/read_u32/read_u64 calls reconstruct fields:
                              offset=0:  message_type = read_u8(buf,0);   offset=1
                              offset=1:  reliability_class = read_u8;     offset=2
                              offset=2:  priority = read_u8;              offset=3
                              offset=3:  padding1 = read_u8; if != 0 → ERR_INVALID
                              offset=4:  message_id = read_u64;           offset=12
                              offset=12: timestamp_us = read_u64;         offset=20
                              offset=20: source_id = read_u32;            offset=24
                              offset=24: destination_id = read_u32;       offset=28
                              offset=28: expiry_time_us = read_u64;       offset=36
                              offset=36: payload_length = read_u32;       offset=40
                              offset=40: padding2 = read_u32; if != 0 → ERR_INVALID
                              assert(offset == WIRE_HEADER_SIZE = 44).
                 Substep E: if payload_length > MSG_MAX_PAYLOAD_BYTES: return ERR_INVALID.
                            if buf_len < WIRE_HEADER_SIZE + payload_length: ERR_INVALID.
                 Substep F: memcpy(env.payload, &buf[44], payload_length).
                 Substep G: assert(envelope_valid(env)).
                            Return Result::OK.

             [Step 6a-v]   Back in recv_from_client():
               if !result_ok(Serializer::deserialize()) (line 228):
                 log WARNING_LO "Deserialize failed: %u".
                 return res. [NOTE: recv_from_client returns early; message NOT queued.]

             [Step 6a-vi]  m_recv_queue.push(env) (line 235) → RingBuffer.hpp:127:
               t = m_tail.load(relaxed)         — producer reads own tail.
               h = m_head.load(acquire)         — observe consumer's head.
               cnt = t - h (unsigned).
               assert(cnt <= MSG_RING_CAPACITY).
               if cnt >= 64: return ERR_FULL. [Queue is full; message dropped.]
               envelope_copy(m_buf[t & RING_MASK], env)  — copy into ring slot.
               m_tail.store(t+1, release)       — atomically publish new message.
               assert((t+1-h) <= 64).
               return Result::OK.

             [Step 6a-vii] if !result_ok(push_res) (line 236):
               log WARNING_HI "Recv queue full; dropping message".
               [recv_from_client still returns Result::OK (line 241).]

             [Step 6a-viii] assert(out_len <= SOCKET_RECV_BUF_BYTES).
                            return Result::OK.

  Step 6b.  After inner client loop:
            m_recv_queue.pop(envelope) (line 355) → RingBuffer.hpp:159:
              h = m_head.load(relaxed)
              t = m_tail.load(acquire)
              cnt = t - h.
              if cnt == 0: return ERR_EMPTY.
              envelope_copy(env, m_buf[h & RING_MASK]).
              m_head.store(h+1, release).
              return Result::OK.
            if result_ok(res): return res to caller. [SUCCESS: message delivered.]

  Step 6c.  If queue still empty: check impairment engine for delayed inbound messages.
            now_us = timestamp_now_us() (line 361).
            MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE] — stack array of 32 envelopes
            (line 362).
            delayed_count = m_impairment.collect_deliverable(now_us, delayed, 32)
            (line 363) → ImpairmentEngine.cpp:174 (same as UC_20 Step 7 description).
            Loop i=0..delayed_count-1 (line 366):
              assert(i < IMPAIR_DELAY_BUF_SIZE).
              (void)m_recv_queue.push(delayed[i]).
              [Return value cast to void — ERR_FULL silently ignored.]

  Step 6d.  m_recv_queue.pop(envelope) again (line 372).
            if result_ok(res): return res to caller. [SUCCESS from delayed delivery.]

  Step 6e.  End of attempt loop body. If no message, increment attempt, repeat Step 6.

Step 7.  All poll_count attempts exhausted with no message.
         return Result::ERR_TIMEOUT (line 379).

## 4. Call Tree (Hierarchical)

TcpBackend::receive_message(envelope, timeout_ms)        [TcpBackend.cpp:325]
  RingBuffer::pop(envelope)                              [RingBuffer.hpp:159]  — fast path
  [if server] TcpBackend::accept_clients()               [TcpBackend.cpp:161]
    socket_accept(m_listen_fd)                           [SocketUtils.cpp:250]
      accept(fd, NULL, NULL)                             [POSIX syscall]
  [loop 0..poll_count-1]
    [loop 0..MAX_TCP_CONNECTIONS-1]
      TcpBackend::recv_from_client(client_fd, 100)       [TcpBackend.cpp:193]
        tcp_recv_frame(fd, m_wire_buf, 8192, 100, &len)  [SocketUtils.cpp:429]
          socket_recv_exact(fd, header, 4, 100)          [SocketUtils.cpp:337]
            poll({fd,POLLIN}, 1, 100)                    [POSIX syscall]
            recv(fd, &header[rcvd], remaining, 0)        [POSIX syscall]
          [parse frame_len from header bytes]
          socket_recv_exact(fd, m_wire_buf, frame_len, 100) [SocketUtils.cpp:337]
            poll({fd,POLLIN}, 1, 100)                    [POSIX syscall, per loop iter]
            recv(fd, &buf[rcvd], remaining, 0)           [POSIX syscall, per loop iter]
        Serializer::deserialize(m_wire_buf, out_len, env) [Serializer.cpp:173]
          envelope_init(env)
          read_u8(buf, offset)  [x4]
          read_u64(buf, offset) [x3]
          read_u32(buf, offset) [x4]
          memcpy(env.payload, &buf[44], payload_length)
          envelope_valid(env)  [postcondition]
        RingBuffer::push(env)                            [RingBuffer.hpp:127]
          m_tail.load(relaxed)
          m_head.load(acquire)
          envelope_copy(m_buf[slot], env)
          m_tail.store(t+1, release)
        [on recv_from_client failure:]
          socket_close(client_fd)                        [SocketUtils.cpp:272]
            close(fd)                                    [POSIX syscall]
          [compact m_client_fds[] by shifting]
    RingBuffer::pop(envelope)                            [RingBuffer.hpp:159] — after clients
      m_head.load(relaxed)
      m_tail.load(acquire)
      envelope_copy(env, m_buf[slot])
      m_head.store(h+1, release)
    timestamp_now_us()
    ImpairmentEngine::collect_deliverable(now_us,buf,32) [ImpairmentEngine.cpp:174]
      [scan m_delay_buf[0..31] for release_us <= now_us]
      envelope_copy(out_buf[i], m_delay_buf[j].env)
    [loop over delayed_count]
      RingBuffer::push(delayed[i])                       [RingBuffer.hpp:127]
    RingBuffer::pop(envelope)                            [RingBuffer.hpp:159] — after delayed

## 5. Key Components Involved

Component              File                                    Role
---------              ----                                    ----
TcpBackend             src/platform/TcpBackend.cpp             Outer orchestration, client
                                                               fd management, poll loop.
RingBuffer             src/core/RingBuffer.hpp                 SPSC inbound message queue;
                                                               push from recv_from_client,
                                                               pop to caller.
Serializer             src/core/Serializer.cpp                 Deserializes wire bytes into
                                                               MessageEnvelope fields.
ImpairmentEngine       src/platform/ImpairmentEngine.cpp       collect_deliverable() drains
                                                               any delayed inbound messages.
SocketUtils            src/platform/SocketUtils.cpp            tcp_recv_frame(), socket_
                                                               recv_exact(): raw I/O.
Types.hpp              src/core/Types.hpp                      Constants: MSG_RING_CAPACITY
                                                               =64, MAX_TCP_CONNECTIONS=8,
                                                               SOCKET_RECV_BUF_BYTES=8192,
                                                               IMPAIR_DELAY_BUF_SIZE=32.
Logger                 src/core/Logger.hpp                     Logging on connection loss,
                                                               deserialize failure, etc.
Timestamp              src/core/Timestamp.hpp                  now_us for impairment timing.

## 6. Branching Logic / Decision Points

Decision Point 1: m_recv_queue.pop() at entry (line 330)
  OK (queue not empty) → return message immediately; entire poll loop skipped.
  ERR_EMPTY          → continue.

Decision Point 2: m_is_server (line 336)
  true  → call accept_clients() (may add a new client fd).
  false → skip.

Decision Point 3: poll_count computation (lines 341–344)
  timeout_ms = 0 → poll_count = 0 → outer loop body never executes → ERR_TIMEOUT.
  timeout_ms = 100 → poll_count = 1.
  timeout_ms > 5000 → poll_count capped at 50.

Decision Point 4: m_client_fds[i] < 0 in inner loop (line 348)
  true  → skip slot.
  false → call recv_from_client().

Decision Point 5: tcp_recv_frame() return in recv_from_client (line 200)
  false → close client fd; compact m_client_fds[]; return ERR_IO.
  true  → proceed to deserialize.

Decision Point 6: poll() result in socket_recv_exact (SocketUtils:355)
  <= 0 → timeout (100 ms per client per attempt); return false from socket_recv_exact.
  > 0  → read data with recv().

Decision Point 7: recv() result in socket_recv_exact (SocketUtils:365)
  < 0  → error; return false.
  == 0 → socket closed (FIN received); return false.
  > 0  → advance received counter; loop continues.

Decision Point 8: frame_len validation in tcp_recv_frame (SocketUtils:454)
  frame_len > max_frame_size or > buf_cap → WARNING_HI; return false.
  Otherwise → receive payload.

Decision Point 9: Padding byte validation in deserialize (Serializer.cpp:205)
  padding1 != 0 → return ERR_INVALID.
  padding2 != 0 → return ERR_INVALID.

Decision Point 10: payload_length validation in deserialize (Serializer.cpp:238)
  > MSG_MAX_PAYLOAD_BYTES → return ERR_INVALID.
  > buf_len - WIRE_HEADER_SIZE → return ERR_INVALID.

Decision Point 11: result_ok(Serializer::deserialize()) in recv_from_client (line 228)
  false → log WARNING_LO; return error; message NOT pushed to ring.
  true  → m_recv_queue.push().

Decision Point 12: m_recv_queue.push() (line 235)
  ERR_FULL → log WARNING_HI "Recv queue full; dropping message". recv_from_client
             still returns OK (frame was read, but message is dropped).
  OK       → message available for pop.

Decision Point 13: m_recv_queue.pop() after client loop (line 355)
  OK       → return message to caller.
  ERR_EMPTY → check impairment delayed messages.

Decision Point 14: delayed_count > 0 from collect_deliverable (line 366)
  0        → delayed push loop skipped.
  > 0      → push each into ring; try pop again.

Decision Point 15: m_recv_queue.pop() after delayed loop (line 372)
  OK       → return message to caller.
  ERR_EMPTY → next outer attempt iteration.

Decision Point 16: All attempts exhausted (line 379)
  → return Result::ERR_TIMEOUT.

## 7. Concurrency / Threading Behavior

- receive_message() is designed for single-threaded use; no internal locking.
- RingBuffer is SPSC (single-producer, single-consumer):
  - Producer role: recv_from_client() calls push(); uses acquire load of m_head and
    release store of m_tail.
  - Consumer role: receive_message() calls pop(); uses acquire load of m_tail and
    release store of m_head.
  - The SPSC contract is satisfied if recv_from_client() and receive_message() are
    called from the same thread (single-threaded) OR if one thread exclusively calls
    push and another exclusively calls pop. In single-threaded use, the relaxed/acquire
    /release ordering still provides correct sequencing with no data race.
  - [ASSUMPTION: single-threaded use; both push and pop called from the same thread
    within receive_message().]
- poll() (in socket_recv_exact, timeout 100 ms) blocks the calling thread up to
  100 ms per client per attempt. For N clients per attempt and poll_count attempts:
  worst-case blocking = poll_count * N * 100 ms (if all clients time out).
  For default: 10 attempts * 8 clients * 100 ms = 8000 ms = 8 seconds for timeout_ms=1000.
  [OBSERVATION: this is likely much longer than the specified timeout. See Section 14.]
- socket_close() in the disconnect path (SocketUtils.cpp:272) is a POSIX syscall that
  is not atomic with respect to m_client_fds[] array manipulation. Safe only with
  single-threaded use.
- m_wire_buf is shared between all clients in the inner loop. After tcp_recv_frame()
  fills m_wire_buf for client[i], Serializer::deserialize() reads it, push() copies
  it into the ring, and then the inner loop moves to client[i+1] which may overwrite
  m_wire_buf. The copy into the ring (envelope_copy in push()) completes before the
  buffer is reused. [OBSERVATION: safe due to sequential execution.]
- ImpairmentEngine::collect_deliverable() and RingBuffer::push() for delayed messages
  (lines 366–369): push return value is cast to (void). If the ring is full during
  delayed delivery, messages are silently dropped. [OBSERVATION: no log emitted here.]

## 8. Memory & Ownership Semantics (C/C++ Specific)

- m_wire_buf[8192]: TcpBackend member; used as receive buffer in tcp_recv_frame().
  Written by socket_recv_exact (into the 4-byte header[] local and into m_wire_buf
  for the payload). The header is a local 4-byte stack array inside tcp_recv_frame().
- m_recv_queue (RingBuffer): inline member of TcpBackend; contains m_buf[64] of
  MessageEnvelope statically. Ring semantics prevent overflow when cnt >= 64.
- delayed[IMPAIR_DELAY_BUF_SIZE] (line 362): stack array of 32 MessageEnvelope objects
  allocated inside receive_message() each iteration of the outer loop. [NOTE: same
  large-stack-frame risk as in UC_20 — approximately 32 * sizeof(MessageEnvelope) bytes
  per outer loop iteration. If payload is inline 4096 bytes, this is ~132 KB per iter.
  For poll_count = 50, this array is created and destroyed 50 times, but as it is on
  the stack it occupies the same stack frame space each time (no accumulation).]
- header[4] in tcp_recv_frame(): local stack array; ephemeral; freed on return.
- env (MessageEnvelope) in recv_from_client(): local variable; filled by deserialize;
  passed to push() which calls envelope_copy() into the ring buffer slot.
- Ownership of client fds: TcpBackend owns all fds in m_client_fds[]. On disconnect
  detected by recv_from_client(), close() is called and the slot is nulled and compacted.
- Power of 10 rule 3: no malloc/new at any point in the receive path.
- envelope parameter to receive_message(): passed by non-const reference; written by
  pop() on success. On ERR_TIMEOUT, the envelope value is indeterminate (envelope_init
  is not called at the entry of receive_message()).

## 9. Error Handling Flow

Error source                    Detected at / line              Action
------------                    ------------------              ------
poll() timeout (recv header)    socket_recv_exact:355-360       Log WARNING_HI; return false
                                → tcp_recv_frame returns false
                                → recv_from_client line 200     Log WARNING_LO; close fd;
                                                                compact array; ERR_IO
recv() error (<0)               socket_recv_exact:365-369       Log WARNING_HI; return false
                                                                (same path as above)
recv() == 0 (peer closed)       socket_recv_exact:372-376       Log WARNING_HI "socket closed";
                                                                return false (same path)
frame_len > max / buf_cap       tcp_recv_frame:454-459          Log WARNING_HI; return false
                                                                (same path as above)
poll() timeout (recv payload)   socket_recv_exact (payload)     Same as header timeout
recv() error (payload)          socket_recv_exact (payload)     Same as above
padding byte != 0               Serializer::deserialize:205,231 Return ERR_INVALID
payload_length > max            Serializer::deserialize:238     Return ERR_INVALID
buf_len < total_size            Serializer::deserialize:244     Return ERR_INVALID
deserialize ERR                 recv_from_client:228            Log WARNING_LO; return err
                                                                (frame consumed but dropped)
push() ERR_FULL                 recv_from_client:235-239        Log WARNING_HI; recv_from_
                                                                client returns OK; message
                                                                silently dropped
Delayed push ERR_FULL           receive_message:368             (void) cast; silently dropped
timeout (all attempts done)     receive_message:379             return ERR_TIMEOUT
accept_clients ERR              receive_message:337             (void) cast; silently ignored

Severity classification:
  - WARNING_HI: poll timeout, recv() error, frame size overrun, push queue full.
  - WARNING_LO: connection lost, deserialize failure, no clients.
  - FATAL: not used in receive path.

## 10. External Interactions

Interaction                    syscall / API                   When / Context
-----------                    -------------                   --------------
Accept new connections         accept(listen_fd, NULL, NULL)   Each receive_message() call
                                                               in server mode (Step 4).
Poll socket for data           poll({fd,POLLIN},1,100ms)       Per socket_recv_exact() loop
                                                               iteration (header + payload).
Read TCP data                  recv(fd, buf, len, 0)           Per socket_recv_exact() loop
                                                               iteration.
Close disconnected client      close(fd)                       On recv failure detection.
Get current timestamp          timestamp_now_us()              Once per outer poll attempt
                                                               (line 361).
Logger output                  Logger::log()                   Errors, WARNING events.

Minimum syscall count for success (1 client, message fits in 1 recv() call):
  - 1 poll() for header, 1 recv() for header (4 bytes).
  - 1 poll() for payload, 1 recv() for payload (N bytes).
  Total: 4 POSIX calls for the data read.
  Plus: 1 accept() if server mode (regardless of whether a client connects).

## 11. State Changes / Side Effects

State before receive_message():
  m_recv_queue: possibly has prior messages.
  m_client_fds[]: 1+ connected clients with valid fds.
  m_wire_buf: indeterminate (prior content).
  m_impairment.m_delay_buf: may have delayed inbound messages.

State after successful receive_message():
  m_recv_queue: head advanced by 1 (pop() consumed); tail unchanged by receive path.
                [push() may have incremented tail; pop() incremented head.]
  m_wire_buf: contains the last received message's serialized bytes.
  m_client_fds[]: unchanged if no disconnect; compacted if disconnect detected.
  m_client_count: decremented if disconnect detected.
  m_impairment.m_delay_buf: deliverable slots cleared; m_delay_count decremented.
  envelope (out-param): filled with the received message's fields and payload.

Side effects:
  - OS: client fd closed if disconnect detected.
  - Logger events on errors.
  - Stack: delayed[] array allocated/freed each outer loop iteration.
  - m_wire_buf: mutated; contains last frame's bytes after return.
  - RingBuffer atomic counters: m_tail (release store in push), m_head (release store
    in pop) — observable by any concurrent reader of the ring (none in single-threaded).

## 12. Sequence Diagram (ASCII)

App       TcpBackend       SocketUtils      Serializer    RingBuffer    ImpairmentEngine   OS
---       ----------       -----------      ----------    ----------    ----------------   --
 |            |                |               |              |                |            |
 |recv_msg()->|                |               |              |                |            |
 |            |--pop()-------->|               |         [empty: ERR_EMPTY]   |            |
 |            |                |               |              |                |            |
 |            |--accept_clients() [server]---->|              |                |            |
 |            |                |--accept()-----|              |                |        --->|
 |            |                |               |              |                |        <---|
 |            |                |               |              |                |            |
 |            | [attempt loop] |               |              |                |            |
 |            | [client loop]  |               |              |                |            |
 |            |--recv_from_client(fd,100)------>|             |                |            |
 |            |                |--tcp_recv_frame(fd,buf,8192,100,&len)         |            |
 |            |                |  socket_recv_exact(fd,hdr,4,100)              |            |
 |            |                |  --poll({fd,POLLIN},1,100)---|               |        --->|
 |            |                |  <--POLLIN--------------------|              |        <---|
 |            |                |  --recv(fd,hdr,4,0)-----------|              |        --->|
 |            |                |  <--4 bytes ------------------|              |        <---|
 |            |                |  [parse frame_len]            |              |            |
 |            |                |  socket_recv_exact(fd,buf,frame_len,100)     |            |
 |            |                |  --poll({fd,POLLIN},1,100)---|               |        --->|
 |            |                |  <--POLLIN--------------------|              |        <---|
 |            |                |  --recv(fd,buf,N,0)-----------|              |        --->|
 |            |                |  <--N bytes ------------------|              |        <---|
 |            |                |  [*out_len=frame_len; return true]            |            |
 |            |                |               |              |                |            |
 |            |--deserialize(buf,len,env)------>|             |                |            |
 |            |                |  read_u8/u32/u64 (header)    |              |            |
 |            |                |  memcpy(payload)             |              |            |
 |            |<--Result::OK, env-filled--------|             |              |            |
 |            |                |               |              |                |            |
 |            |--push(env)-----|               |         [release store m_tail]            |
 |            |<--Result::OK---|               |              |                |            |
 |            |                |               |              |                |            |
 |            | [end client loop]              |              |                |            |
 |            |                |               |              |                |            |
 |            |--pop(env)------|               |         [acquire load m_tail] |           |
 |            |<--Result::OK, envelope filled--|              |                |            |
 |<--Result::OK|               |               |              |                |            |
 |            |                |               |              |                |            |
 | [or: queue still empty, check delayed]      |              |                |            |
 |            |--collect_deliverable(now_us,buf,32)---------->|           [scan delay_buf]  |
 |            |<--delayed_count------------------------------ |                |            |
 |            |--push(delayed[i]) [loop]-------|              |          [release store]    |
 |            |--pop(env)------|               |              |                |            |
 |<--Result::OK [or loop again / ERR_TIMEOUT]  |              |                |            |

## 13. Initialization vs Runtime Flow

Initialization (init(), described in UC_19):
  - m_recv_queue.init(): ring reset (head=0, tail=0, cnt=0).
  - m_impairment.init(): delay_buf zeroed; PRNG seeded.
  - m_wire_buf: not explicitly zeroed at init; contains whatever the OS provides
    for stack/bss allocation. [ASSUMPTION: member of TcpBackend, zero-initialized
    by OS for global/static instances; indeterminate for stack-allocated TcpBackend.]
  - m_client_fds[]: all set to -1 in constructor.

Runtime (receive_message()):
  - recv_from_client() always writes into m_wire_buf before Serializer reads from it;
    prior content is irrelevant.
  - poll() timeout per call is hard-coded to 100 ms (line 351) regardless of the
    per-channel recv_timeout_ms. The channel config value is not used in this path.
    [OBSERVATION: see Section 14, Risk 2.]
  - The poll_count cap at 50 (line 344) limits receive_message() to a maximum of
    poll_count * MAX_TCP_CONNECTIONS * socket-blocking-time blocking time.

## 14. Known Risks / Observations

Risk 1: Timeout semantics are approximate and may be much longer than specified
  poll_count = ceil(timeout_ms / 100), capped at 50. Each iteration polls ALL connected
  clients sequentially with 100 ms timeout each. For 8 clients and poll_count=10, the
  maximum wall-clock blocking is 10 * 8 * 100 ms = 8000 ms even if timeout_ms=1000.
  If all clients have no data, the actual timeout experienced by the caller is up to 8x
  longer than requested. This is a design flaw for latency-sensitive applications.

Risk 2: Per-channel recv_timeout_ms is ignored in the receive path
  ChannelConfig.recv_timeout_ms is configured per channel (ChannelConfig.hpp:28) but
  receive_message() hard-codes 100 ms per recv_from_client() call (line 351). The
  configured channel timeout is never consulted during receive.

Risk 3: m_wire_buf shared and reused per client in the inner loop
  The inner loop (line 347) iterates over all clients. For each client, tcp_recv_frame()
  writes into m_wire_buf. Deserialize reads from m_wire_buf. push() deep-copies the
  deserialized envelope into the ring. Only THEN does the loop move to the next client,
  which overwrites m_wire_buf again. This is correct but fragile: if the loop ordering
  were changed to process results after the inner loop completed, the correctness would
  break. A code comment explaining this dependency would reduce maintenance risk.

Risk 4: Multiple messages received per client per attempt — only one popped
  recv_from_client() reads exactly one frame per call. If a client has multiple frames
  buffered in the OS receive buffer, only one is read per inner-loop iteration per
  attempt. The others are read in subsequent attempts. This is correct but may delay
  message delivery by up to poll_count * 100 ms per additional buffered message per
  client.

Risk 5: Client disconnects remove the fd mid-loop iteration
  recv_from_client() removes the disconnected client's fd from m_client_fds[] and
  compacts the array (lines 207–216). The inner loop (line 347) iterates by index i up
  to MAX_TCP_CONNECTIONS; since the slot is set to -1 before compaction and the
  shifted entries are at lower indices (already passed), the current loop iteration is
  safe. However, if client[i] disconnects and is replaced in slot i by shift from i+1,
  the new client at slot i is not processed in this iteration. It will be picked up
  in the next attempt.

Risk 6: Delayed inbound message push return value ignored
  Line 368: (void)m_recv_queue.push(delayed[i]).
  If the ring buffer is full, delayed messages are silently dropped with no log.
  This is asymmetric with push() in recv_from_client() (lines 236–239) which logs
  a WARNING_HI.

Risk 7: Large stack allocation inside polling loop
  Line 362: MessageEnvelope delayed[32] on the stack, inside the outer poll loop.
  If sizeof(MessageEnvelope) is large (e.g., 4096-byte inline payload), this is
  approximately 132 KB allocated on every outer loop iteration (up to 50 times).
  The stack frame size is fixed (not accumulated), but on embedded targets with
  limited stack (e.g., 64 KB), this allocation alone exceeds the entire stack.

Risk 8: accept_clients() blocks if listen socket is blocking
  Same as UC_19 Risk 1. Called unconditionally each receive_message() in server mode.
  If the listen socket is in blocking mode, this call will block until a client
  connects, defeating the timeout logic entirely.

Risk 9: Impairment engine collect_deliverable in receive path
  The receive path calls m_impairment.collect_deliverable(). This is the same
  ImpairmentEngine instance used on the send side. The delay_buf is shared: if
  process_outbound() was called (in send_message()) to buffer an outbound message,
  collect_deliverable() in the receive path may release that outbound message and
  push it into m_recv_queue. This would cause a locally-sent message to appear as
  a received message. [OBSERVATION: This appears to be a simulation design choice
  for loopback testing, but it could be surprising in production use.]

## 15. Unknowns / Assumptions

[ASSUMPTION A]  socket_recv_exact() with timeout_ms=100 will return false after 100 ms
                if no data is available (poll() returns 0). No data is consumed from
                the socket if poll() times out. This is the intended "non-blocking with
                short timeout" behavior. Correct per POSIX poll() semantics.

[ASSUMPTION B]  recv() in socket_recv_exact() may return fewer bytes than requested
                (partial read). The while loop handles this correctly: it loops until
                exactly `len` bytes are accumulated. The loop bound is `len` (fixed per
                Power of 10 rule 2).

[ASSUMPTION C]  WIRE_HEADER_SIZE == 44 bytes (inferred from Serializer.cpp field writes).
                max_frame_size = 44 + 4096 = 4140 bytes (< SOCKET_RECV_BUF_BYTES = 8192).
                All validated frames fit in m_wire_buf.

[ASSUMPTION D]  envelope_init(env) in Serializer::deserialize() zero-fills or sets all
                fields of env to a safe default state before the field-by-field read.
                Exact implementation not read.

[ASSUMPTION E]  The envelope out-parameter in receive_message() is indeterminate if the
                function returns ERR_TIMEOUT (no envelope_init called on entry).
                Callers must not use the envelope value on non-OK return.

[ASSUMPTION F]  The ImpairmentEngine delay_buf is used for both outbound (send path) and
                inbound (receive path) delayed message collection. This is a shared data
                structure. If both send_message() and receive_message() are called from
                different threads, there would be a data race on m_delay_buf[].
                [ASSUMPTION: single-threaded use.]

[ASSUMPTION G]  collect_deliverable() in the receive path releasing messages placed by
                process_outbound() from send_message() is intentional for simulation
                (loopback testing). In production two-node use, process_outbound() is
                only called on sends, and the delay_buf would release those outbound
                messages on the sending side's next receive poll. This may or may not
                match the intended simulation semantics.

[ASSUMPTION H]  The m_prng in ImpairmentEngine is NOT exercised by collect_deliverable()
                or the receive path; it is only advanced by process_outbound() and
                process_inbound(). collect_deliverable() uses only timestamp comparisons.

[ASSUMPTION I]  socket_accept() in accept_clients() is called with the listen socket in
                its current mode (assumed blocking, per UC_19 Risk 1). The comment at
                line 173 assumes EAGAIN, which only occurs in non-blocking mode. This
                inconsistency is carried into the receive path.
```

---

Here is a summary of what was produced and the key findings:

**Three complete Flow-of-Control documents were generated**, each with all 15 required sections, based exclusively on the actual source code read from the nine files.

**Key factual findings across all three documents:**

1. **Blocking accept() risk (UC_19, UC_21):** `socket_set_nonblocking()` is never called on `m_listen_fd`. The comment at `TcpBackend.cpp:173` says "EAGAIN in non-blocking mode" but the socket is blocking, so `accept()` will block indefinitely if no client is pending.

2. **Timeout overrun (UC_21):** The actual wall-clock blocking in `receive_message()` can be up to `poll_count * MAX_TCP_CONNECTIONS * 100ms` — potentially 8x longer than the caller's `timeout_ms` for 8 connected clients.

3. **Large stack allocation (UC_20, UC_21):** `MessageEnvelope delayed_envelopes[32]` is allocated on the stack inside `send_message()` and `receive_message()`. If `MessageEnvelope.payload` is an inline `uint8_t[4096]` array, this is approximately 132 KB per allocation — dangerous on embedded targets.

4. **ERR_FULL not special-cased in send_message() (UC_20):** `process_outbound()` can return `ERR_FULL` (delay buffer full) which is not caught at `TcpBackend.cpp:266`; only `ERR_IO` is intercepted. An `ERR_FULL` silently bypasses the impairment delay and sends the message immediately.

5. **Shared delay buffer between send and receive (UC_21):** `collect_deliverable()` in the receive poll loop will drain messages placed by `process_outbound()` on the send side, potentially causing locally-sent messages to be delivered back to the local receiver — likely intentional for simulation but surprising in production.

6. **Delayed message send errors silently dropped (UC_20):** `tcp_send_frame()` return value is `(void)`-cast for delayed messages (line 312); no warning is emitted.agentId: a533717d5c1161c54 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 76961
tool_uses: 9
duration_ms: 502644</usage>