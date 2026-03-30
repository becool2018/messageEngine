================================================================================
UC_23 — UDP RECEIVE DATAGRAM
Flow-of-Control Document
Source files traced:
  src/platform/UdpBackend.cpp / .hpp
  src/platform/SocketUtils.cpp
  src/core/Serializer.cpp
  src/platform/ImpairmentEngine.cpp
  src/core/RingBuffer.hpp
  src/core/Types.hpp
================================================================================

## 1. Use Case Overview

Name:       UC_23 — UDP Receive Datagram
Actor:      Any caller holding a UdpBackend* that has successfully called
            UdpBackend::init().
Goal:       Block (with bounded timeout) for an incoming UDP datagram on the
            bound socket, deserialize the raw bytes into a MessageEnvelope,
            flush any delayed envelopes from the impairment engine, push and
            pop through the RingBuffer, and return the first available envelope
            to the caller.
Preconditions:
  - UdpBackend::init() has returned Result::OK.
  - m_open == true and m_fd >= 0.
  - The socket is bound and may already have datagrams in the OS receive buffer.
Postconditions (happy path):
  - A valid MessageEnvelope is written into the caller's envelope reference.
  - Result::OK is returned.
  - The received datagram has been consumed from the OS socket buffer.
Timeout path:
  - No datagram arrived within timeout_ms milliseconds.
  - Result::ERR_TIMEOUT is returned; envelope is unmodified.

--------------------------------------------------------------------------------

## 2. Entry Points

Primary entry point:
  UdpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
    File:   src/platform/UdpBackend.cpp, line 161

Supporting functions reached during this call:
  RingBuffer::pop(envelope)
    File:   src/core/RingBuffer.hpp, line 159

  socket_recv_from(fd, buf, cap, timeout_ms, &out_len, src_ip, &src_port)
    File:   src/platform/SocketUtils.cpp, line 530
    POSIX:  poll(2), then recvfrom(2)

  Serializer::deserialize(buf, out_len, envelope)
    File:   src/core/Serializer.cpp, line 173

  timestamp_now_us()
    File:   src/core/Timestamp.hpp / .cpp [ASSUMPTION]

  ImpairmentEngine::collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)
    File:   src/platform/ImpairmentEngine.cpp, line 174

  RingBuffer::push(delayed[i])
    File:   src/core/RingBuffer.hpp, line 127

  RingBuffer::pop(envelope) — called again after push
    File:   src/core/RingBuffer.hpp, line 159

--------------------------------------------------------------------------------

## 3. End-to-Step Control Flow (Step-by-Step)

Step 1  —  UdpBackend::receive_message() entry (UdpBackend.cpp:161)
           assert(m_open)   — transport is initialized
           assert(m_fd >= 0) — socket is valid

Step 2  —  Queue fast-path check (UdpBackend.cpp:167)
           m_recv_queue.pop(envelope)
           If result_ok(res): return res immediately.
           [This handles the case where a previous call left a message in the
            RingBuffer (e.g., after a push/pop sequence where push succeeded
            but a second pop was not reached) — unusual but safe.]

Step 3  —  Compute poll iteration count (UdpBackend.cpp:174)
           poll_count = (timeout_ms + 99) / 100    — ceiling division: 100ms/iter
           If poll_count > 50: poll_count = 50      — cap: max 5000ms total
           [Power of 10 rule 2: fixed upper bound enforced here.]

Step 4  —  Poll loop (UdpBackend.cpp:179, outer bound = poll_count)
           for (attempt = 0; attempt < poll_count; attempt++)

  Step 4a — socket_recv_from() (SocketUtils.cpp:530)
             Internally:
               a. Build pollfd: fd=m_fd, events=POLLIN
               b. poll(&pfd, 1, 100) — 100ms timeout per iteration
                  If poll_result <= 0: log WARNING_LO "recvfrom poll timeout"
                  Return false → continue outer loop (next attempt)
               c. Build sockaddr_in for source address
               d. recvfrom(fd, buf, buf_cap, 0, &src_addr, &src_len)
                  If recv_result < 0: log WARNING_LO, return false → continue
                  If recv_result == 0: log WARNING_LO, return false → continue
               e. inet_ntop(AF_INET, &src_addr.sin_addr, out_ip, 48)
                  If inet_ntop == NULL: log WARNING_LO, return false → continue
               f. *out_port = ntohs(src_addr.sin_port)
               g. *out_len = (uint32_t)recv_result
               h. assert(*out_len > 0 && *out_len <= buf_cap)
               i. return true
             [m_wire_buf (UdpBackend::m_wire_buf) is passed as buf;
              socket_recv_from writes datagram bytes directly into it.]

  Step 4b — Deserialize (UdpBackend.cpp:191)
             Serializer::deserialize(m_wire_buf, out_len, envelope)
             (Serializer.cpp:173):
               a. assert(buf != nullptr), assert(buf_len valid)
               b. If buf_len < WIRE_HEADER_SIZE: return ERR_INVALID
               c. envelope_init(env)  — zero/reset destination envelope
               d. Sequential big-endian reads via read_u8/read_u32/read_u64:
                  offset 0:  env.message_type      (1 byte)
                  offset 1:  env.reliability_class  (1 byte)
                  offset 2:  env.priority           (1 byte)
                  offset 3:  padding1 (must be 0)   (1 byte)
                  offset 4:  env.message_id         (8 bytes)
                  offset 12: env.timestamp_us        (8 bytes)
                  offset 20: env.source_id           (4 bytes)
                  offset 24: env.destination_id      (4 bytes)
                  offset 28: env.expiry_time_us      (8 bytes)
                  offset 36: env.payload_length      (4 bytes)
                  offset 40: padding2 (must be 0)    (4 bytes)
               e. assert(offset == WIRE_HEADER_SIZE)
               f. Validate env.payload_length <= MSG_MAX_PAYLOAD_BYTES (4096)
               g. Check buf_len >= WIRE_HEADER_SIZE + payload_length
               h. memcpy(env.payload, &buf[WIRE_HEADER_SIZE], payload_length)
               i. assert(envelope_valid(env))
               j. return Result::OK
             If !result_ok(res): log WARNING_LO; continue outer loop.

  Step 4c — Impairment engine delayed flush (UdpBackend.cpp:199)
             timestamp_now_us() → now_us
             m_impairment.collect_deliverable(now_us, delayed[0..31], 32)
             (ImpairmentEngine.cpp:174):
               Iterates m_delay_buf[0..31]; for each active slot where
               release_us <= now_us: copies env to out_buf, deactivates slot,
               decrements m_delay_count.
               Returns delayed_count.

  Step 4d — Push delayed envelopes to ring buffer (UdpBackend.cpp:204)
             for (i = 0; i < delayed_count; i++):
               assert(i < IMPAIR_DELAY_BUF_SIZE)
               (void)m_recv_queue.push(delayed[i])
               [push return is void-cast; queue-full silently drops the
                delayed envelope.]

  Step 4e — Push freshly-received envelope (UdpBackend.cpp:210)
             res = m_recv_queue.push(envelope)
             If !result_ok(res): log WARNING_HI "Recv queue full; dropping"
             [Even on push failure, execution continues to pop.]

  Step 4f — Pop and return (UdpBackend.cpp:218)
             res = m_recv_queue.pop(envelope)
             If result_ok(res): return res  [SUCCESS — exits function]
             [If push failed (queue was full), pop may return ERR_EMPTY if
              the envelope was not actually stored; unusual corner case.]

Step 5  —  Post-loop delayed flush (UdpBackend.cpp:225)
           Executed after all poll attempts are exhausted with no datagram:
           timestamp_now_us() → now_us
           collect_deliverable(now_us, delayed[32], 32)
           for (i in 0..delayed_count): push(delayed[i]) → m_recv_queue
           res = m_recv_queue.pop(envelope)
           If result_ok(res): return res
           [This handles the case where delayed messages become deliverable
            by the time the timeout elapses.]

Step 6  —  Timeout return (UdpBackend.cpp:241)
           return Result::ERR_TIMEOUT

--------------------------------------------------------------------------------

## 4. Call Tree (Hierarchical)

UdpBackend::receive_message(envelope, timeout_ms)    [UdpBackend.cpp:161]
├── assert(m_open, m_fd >= 0)
├── RingBuffer::pop(envelope)                        [RingBuffer.hpp:159]  [fast path]
│   ├── m_head.load(relaxed) / m_tail.load(acquire)
│   └── envelope_copy(env, m_buf[h & RING_MASK])    [on hit]
├── [compute poll_count: cap to 50]
└── for attempt = 0..poll_count-1
    ├── socket_recv_from(fd, m_wire_buf, 8192, 100, &out_len, src_ip, &src_port)
    │   │                                            [SocketUtils.cpp:530]
    │   ├── poll(&pfd, 1, 100)                       [syscall]
    │   ├── recvfrom(fd, buf, cap, 0, &src_addr, &src_len)  [syscall]
    │   ├── inet_ntop(AF_INET, ...)
    │   └── *out_len = (uint32_t)recv_result
    ├── Serializer::deserialize(m_wire_buf, out_len, envelope)
    │   │                                            [Serializer.cpp:173]
    │   ├── envelope_init(env)
    │   ├── read_u8()  ×4                            [Serializer.cpp:67]
    │   ├── read_u64() ×3                            [Serializer.cpp:92]
    │   ├── read_u32() ×4                            [Serializer.cpp:76]
    │   ├── assert(offset == WIRE_HEADER_SIZE)
    │   └── memcpy(env.payload, ...)
    ├── timestamp_now_us()
    ├── ImpairmentEngine::collect_deliverable(now_us, delayed, 32)
    │   │                                            [ImpairmentEngine.cpp:174]
    │   └── [loop 0..31] envelope_copy + deactivate slot
    ├── [loop i=0..delayed_count] (void)RingBuffer::push(delayed[i])
    │   └── RingBuffer::push()                       [RingBuffer.hpp:127]
    │       ├── m_tail.load(relaxed) / m_head.load(acquire)
    │       ├── envelope_copy(m_buf[t & RING_MASK], env)
    │       └── m_tail.store(t+1, release)
    ├── RingBuffer::push(envelope)                   [RingBuffer.hpp:127]
    │   └── [same as above]
    └── RingBuffer::pop(envelope)                    [RingBuffer.hpp:159]  → return OK
        ├── m_head.load(relaxed) / m_tail.load(acquire)
        ├── envelope_copy(env, m_buf[h & RING_MASK])
        └── m_head.store(h+1, release)

[post-loop if timeout]
├── timestamp_now_us()
├── ImpairmentEngine::collect_deliverable(...)
├── [loop] RingBuffer::push(delayed[i])
└── RingBuffer::pop(envelope)   → return OK or ERR_TIMEOUT

--------------------------------------------------------------------------------

## 5. Key Components Involved

Component              File(s)                         Role
─────────────────────────────────────────────────────────────────────────────
UdpBackend             UdpBackend.cpp / .hpp           Orchestrates receive:
                                                        owns poll loop, m_wire_buf,
                                                        m_recv_queue, m_impairment.
socket_recv_from()     src/platform/SocketUtils.cpp    POSIX wrapper: poll + recvfrom;
                                                        delivers raw bytes into
                                                        m_wire_buf.
poll(2)                OS kernel                        Multiplexed I/O wait; returns
                                                        when socket is readable or
                                                        100ms elapses.
recvfrom(2)            OS kernel                        Copies one UDP datagram from
                                                        the kernel receive buffer into
                                                        m_wire_buf.
Serializer             src/core/Serializer.cpp         Interprets big-endian wire
                                                        bytes into MessageEnvelope
                                                        fields; validates padding.
ImpairmentEngine       src/platform/ImpairmentEngine.cpp  Flushes messages whose
                                                        simulated delay has elapsed.
RingBuffer             src/core/RingBuffer.hpp         SPSC lock-free FIFO; buffers
                                                        ready envelopes; mediates
                                                        push (receive path) → pop
                                                        (caller).
MessageEnvelope        src/core/MessageEnvelope.hpp    Data structure populated by
                                                        Serializer::deserialize().
Types.hpp              src/core/Types.hpp              SOCKET_RECV_BUF_BYTES=8192,
                                                        IMPAIR_DELAY_BUF_SIZE=32,
                                                        MSG_RING_CAPACITY=64.
Logger                 src/core/Logger.hpp             Records timeouts, errors,
                                                        queue-full events.

--------------------------------------------------------------------------------

## 6. Branching Logic / Decision Points

Branch Point 1 — Queue fast-path hit (UdpBackend.cpp:168)
  Condition:  m_recv_queue.pop() returns Result::OK
  True path:  Return immediately; no socket interaction needed.
  False path: Proceed to poll loop.

Branch Point 2 — poll() timeout (SocketUtils.cpp:549)
  Condition:  poll_result <= 0 (timeout or interrupted)
  True path:  Log WARNING_LO; return false from socket_recv_from()
              → UdpBackend continues to next attempt.
  False path: Proceed to recvfrom().

Branch Point 3 — recvfrom() error (SocketUtils.cpp:563)
  Condition:  recv_result < 0
  True path:  Log WARNING_LO; return false → continue outer loop.
  False path: Check for zero-byte datagram.

Branch Point 4 — Zero-byte datagram (SocketUtils.cpp:569)
  Condition:  recv_result == 0
  True path:  Log WARNING_LO; return false → continue.
  False path: Proceed to inet_ntop.

Branch Point 5 — inet_ntop failure (SocketUtils.cpp:578)
  Condition:  inet_result == NULL
  True path:  Log WARNING_LO; return false → continue outer loop.
  False path: Extract port; set out_len; return true.

Branch Point 6 — Deserialize failure (UdpBackend.cpp:192)
  Condition:  !result_ok(res) from Serializer::deserialize()
  True path:  Log WARNING_LO; continue outer loop (discard malformed datagram).
  False path: Proceed to impairment flush and push.

Branch Point 7 — Padding validation failure (Serializer.cpp:205, 230)
  Condition:  padding1 != 0 or padding2 != 0 in the wire stream
  True path:  Return ERR_INVALID → branch point 6 true path.
  False path: Continue field parsing.

Branch Point 8 — payload_length oversize (Serializer.cpp:238)
  Condition:  env.payload_length > MSG_MAX_PAYLOAD_BYTES (4096)
  True path:  Return ERR_INVALID → branch point 6 true path.
  False path: Validate total size and memcpy payload.

Branch Point 9 — RingBuffer push failure for delayed envelopes (UdpBackend.cpp:206)
  Condition:  m_recv_queue.push(delayed[i]) returns ERR_FULL
  True path:  (void)-cast; silently dropped; no log, no error propagation.
  False path: Slot written; tail incremented.

Branch Point 10 — RingBuffer push failure for received envelope (UdpBackend.cpp:211)
  Condition:  !result_ok(m_recv_queue.push(envelope))
  True path:  Log WARNING_HI "Recv queue full; dropping datagram"; continue to pop.
  False path: Slot written; attempt pop.

Branch Point 11 — Pop after push (UdpBackend.cpp:219)
  Condition:  result_ok(m_recv_queue.pop(envelope))
  True path:  Return OK to caller.
  False path: Continue to next poll attempt (unusual; means push also failed).

Branch Point 12 — Post-loop delayed message available (UdpBackend.cpp:236)
  Condition:  result_ok(m_recv_queue.pop(envelope)) after final collect_deliverable
  True path:  Return OK.
  False path: Return ERR_TIMEOUT.

--------------------------------------------------------------------------------

## 7. Concurrency / Threading Behavior

RingBuffer — SPSC design (single-producer, single-consumer):
  Producer role in this context: receive_message() itself calls push().
  Consumer role: receive_message() itself calls pop().
  Within a single call to receive_message() on a single thread, all push/pop
  operations are sequential; there is no concurrent producer. The atomic
  acquire/release ordering in RingBuffer is therefore strictly stronger than
  required for single-threaded use, but causes no harm.

  If a background thread were also to call send_message() on the same UdpBackend
  instance, it would call collect_deliverable() which modifies m_delay_buf —
  this would race with the receive_message() path's collect_deliverable() call.
  No mutex protects m_delay_buf. [RISK: see Section 14.]

poll(2) — blocking per iteration with a 100ms timeout. The function does not
  use select or epoll; it issues one poll() syscall per outer loop iteration.
  This means the calling thread is blocked inside poll for up to 100ms per
  attempt, then returns to the C++ level, then re-enters poll.

No threads are spawned by receive_message(). There is no background receive
thread; the caller's thread does all the work.

The SPSC contract of RingBuffer.hpp states:
  - Exactly one thread may push() / full()
  - Exactly one thread may pop() / peek() / empty()
  In this implementation both roles are performed by the same thread, which is
  a subset of the SPSC contract and is safe.

--------------------------------------------------------------------------------

## 8. Memory & Ownership Semantics (C/C++ Specific)

m_wire_buf (uint8_t[8192], member of UdpBackend):
  Passed by pointer to socket_recv_from(); the OS writes directly into it via
  recvfrom(). Then passed by const pointer to Serializer::deserialize(). No
  allocation; the 8192-byte region is static within the object. Overwritten on
  every successful recvfrom.

out_len (uint32_t, local in receive_message()):
  Set by socket_recv_from(); used as the buf_len argument to
  Serializer::deserialize(). Stack variable; no heap.

src_ip[48] (char[], local in receive_message()):
  Stack buffer. inet_ntop writes at most 48 bytes (INET6_ADDRSTRLEN).
  Populated by socket_recv_from() and used only for logging in UdpBackend.
  [Note: src_ip and src_port are extracted by socket_recv_from but the
   UdpBackend receive_message() passes them to socket_recv_from but does
   not subsequently use src_ip/src_port for any routing logic — they are
   available only for the WARNING_HI log message at line 213.]

envelope (MessageEnvelope&, out-parameter from caller):
  Written by Serializer::deserialize() during field parsing.
  Also written by RingBuffer::pop() via envelope_copy() on return.
  Caller retains ownership throughout; UdpBackend never takes a pointer to it
  beyond the duration of receive_message().

delayed[] (MessageEnvelope[32], stack local in receive_message()):
  Fixed-size stack frame within receive_message().
  collect_deliverable() writes into it via envelope_copy().
  Entries are then pushed into m_recv_queue via envelope_copy() (another copy).
  The local array is discarded when the stack frame exits.

m_recv_queue (RingBuffer, member of UdpBackend):
  Inline object containing MessageEnvelope m_buf[64] (fixed, static).
  No heap allocation. push/pop perform in-place envelope_copy into/from m_buf.

m_impairment.m_delay_buf (DelayedEntry[32], member of ImpairmentEngine):
  Inline, no heap. collect_deliverable() reads and deactivates entries.

--------------------------------------------------------------------------------

## 9. Error Handling Flow

Error                         Source                   Handling
──────────────────────────────────────────────────────────────────────────────
poll timeout                  socket_recv_from/poll()  WARNING_LO logged;
                                                        continue outer loop.
recvfrom() < 0                socket_recv_from()        WARNING_LO logged;
                                                        continue outer loop.
recvfrom() == 0               socket_recv_from()        WARNING_LO logged;
                                                        continue outer loop.
inet_ntop() NULL              socket_recv_from()        WARNING_LO logged;
                                                        continue outer loop.
buf_len < WIRE_HEADER_SIZE    Serializer::deserialize() ERR_INVALID;
                                                        WARNING_LO logged;
                                                        continue outer loop.
padding1 or padding2 != 0     Serializer::deserialize() ERR_INVALID;
                                                        continue outer loop.
payload_length > 4096         Serializer::deserialize() ERR_INVALID;
                                                        continue outer loop.
m_recv_queue push fail        UdpBackend.cpp:211        WARNING_HI logged;
(main envelope)                                         pop attempted anyway;
                                                        likely returns ERR_EMPTY;
                                                        continue outer loop.
m_recv_queue push fail        UdpBackend.cpp:206        (void)-cast; silent drop.
(delayed envelope)
All poll attempts exhausted   UdpBackend.cpp:241        ERR_TIMEOUT returned.

No error on this path escalates to FATAL. All recoverable: WARNING_LO or
WARNING_HI per the F-Prime severity model.

--------------------------------------------------------------------------------

## 10. External Interactions

Interaction              Details
──────────────────────────────────────────────────────────────────────────────
poll(2) syscall          Called inside socket_recv_from() per outer loop iter.
                         Args: pfd={m_fd, POLLIN, 0}, nfds=1, timeout=100 (ms).
                         Blocks calling thread for up to 100ms.
                         Returns: >0 (readable), 0 (timeout), <0 (error/signal).

recvfrom(2) syscall      Called after poll signals readability.
                         Args: fd=m_fd, buf=m_wire_buf, len=8192, flags=0,
                               src_addr=&src_addr, addrlen=&src_len.
                         Returns: bytes received (1..8192) or negative on error.
                         Copies exactly one UDP datagram per call.

inet_ntop(3)             Converts binary src_addr.sin_addr to dotted-decimal
                         ASCII in out_ip[48]. Used only for diagnostic logging.

OS UDP receive buffer    Kernel-side buffer for the bound socket. Datagrams
                         accumulate here until recvfrom() drains them. If the
                         OS buffer fills (e.g., during a receive_message()
                         timeout), subsequent datagrams are dropped by the OS
                         with no notification to the application.

Logger::log()            [ASSUMPTION] Writes to platform log sink. Called on
                         WARNING_LO/HI paths; not called on the happy path.

--------------------------------------------------------------------------------

## 11. State Changes / Side Effects

Object            Field                     Change
──────────────────────────────────────────────────────────────────────────────
UdpBackend        m_wire_buf                Overwritten with datagram bytes
                                            by recvfrom() via socket_recv_from.
ImpairmentEngine  m_delay_buf[i].active     Set to false for delivered entries.
ImpairmentEngine  m_delay_count             Decremented for each delivered entry.
RingBuffer        m_buf[t & RING_MASK]      Written by envelope_copy on push.
RingBuffer        m_tail (atomic)           Incremented by push (release store).
RingBuffer        m_head (atomic)           Incremented by pop (release store).
envelope (out)    All fields                Populated by Serializer::deserialize
                                            and then overwritten by pop's
                                            envelope_copy.
OS socket buffer  datagram bytes            Consumed by recvfrom; removed from
                                            kernel buffer.

NOT changed:
  m_open, m_fd, m_cfg — read-only on the receive path.
  m_impairment PRNG state — collect_deliverable does not call the PRNG.
  m_impairment m_delay_buf content — entries are deactivated, not erased
  (active flag set to false; stale data remains in the slot until it is
  overwritten by process_outbound on the next impaired send).

--------------------------------------------------------------------------------

## 12. Sequence Diagram (ASCII)

  Caller         UdpBackend       RingBuffer      SocketUtils       Serializer      ImpairmentEngine  OS Kernel
    |                |                |               |                  |                 |              |
    |--recv_msg()-->|                |               |                  |                 |              |
    |               |--pop()-------->|               |                  |                 |              |
    |               |<--ERR_EMPTY---|               |                  |                 |              |
    |               | [compute poll_count = cap(ceil(timeout/100), 50)] |               |              |
    |               | attempt=0..poll_count-1        |                  |                 |              |
    |               |--socket_recv_from(fd,buf,8192,100ms)------------>|                 |              |
    |               |               |               |--poll(fd,POLLIN,100)-------------->|              |
    |               |               |               |               |                 |<-poll(2)------->|
    |               |               |               |               |                 |<--POLLIN evt----|
    |               |               |               |<--1 (ready)---|                 |              |
    |               |               |               |--recvfrom(fd,buf,8192)--------->|              |
    |               |               |               |               |                 |<-recvfrom(2)->|
    |               |               |               |               |                 |<--N bytes-----|
    |               |               |               |--inet_ntop()->|                 |              |
    |               |               |               |<--true (out_len=N)              |              |
    |               |<--true,out_len=N               |               |                 |              |
    |               |--deserialize(buf, N, env)------|-->            |                 |              |
    |               |               |               |  [read fields, memcpy payload]  |              |
    |               |               |               |<--Result::OK   |                 |              |
    |               |--timestamp_now_us()            |               |                 |              |
    |               |--collect_deliverable(now_us)-->|               |                 |              |
    |               |               |               |               |                 |              |
    |               |<--delayed_count=0(no due msgs)|               |                 |              |
    |               |--push(envelope)--------------->|               |                 |              |
    |               |               |--envelope_copy->m_buf[]        |                 |              |
    |               |               |--m_tail.store(t+1, release)    |                 |              |
    |               |               |<--Result::OK   |               |                 |              |
    |               |--pop(envelope)--------------->|                |                 |              |
    |               |               |--m_tail.load(acquire)          |                 |              |
    |               |               |--envelope_copy env<-m_buf[]    |                 |              |
    |               |               |--m_head.store(h+1, release)    |                 |              |
    |               |               |<--Result::OK   |               |                 |              |
    |<--Result::OK--|               |               |                |                 |              |

  Timeout path (no datagram during all attempts):
    |--recv_msg()-->|
    |               | [loop poll_count times: poll 100ms each → timeout each]
    |               |--collect_deliverable(final now_us)
    |               |--pop() → ERR_EMPTY
    |<--ERR_TIMEOUT-|

--------------------------------------------------------------------------------

## 13. Initialization vs Runtime Flow

Initialization (UdpBackend::init(), UdpBackend.cpp:45):
  - socket_create_udp() creates AF_INET/SOCK_DGRAM socket; fd stored in m_fd.
  - socket_set_reuseaddr(), socket_bind() configure the socket so recvfrom can
    receive datagrams on config.bind_port.
  - m_recv_queue.init() resets atomic m_head=0, m_tail=0.
  - m_impairment.init() seeds PRNG, zeros m_delay_buf.
  - m_open = true.

Runtime (receive_message()):
  - No allocation. m_wire_buf is a member array; delayed[] is a stack local.
  - poll()/recvfrom() are OS-blocking calls; their return values drive the
    control flow.
  - The RingBuffer push/pop is used even for a single message because the
    design allows delayed messages to arrive first (FIFO order via push then
    immediate pop). The push+pop round-trip adds a copy but preserves
    consistent semantics across all enqueue paths.

The RingBuffer m_buf[64] is fully allocated in the object's memory from
construction; init() only resets head/tail indices. No runtime allocation.

--------------------------------------------------------------------------------

## 14. Known Risks / Observations

RISK-1 — Push/pop redundancy for the single-message case:
  In the common case (no delayed messages, queue empty), receive_message()
  performs: push(envelope) then pop(envelope). This is two envelope_copy()
  operations for no benefit. The design appears to be intended to maintain a
  single dequeueing path, but it introduces extra latency and memory writes
  on every receive.

RISK-2 — Delayed envelope push result discarded (UdpBackend.cpp:206):
  The (void)m_recv_queue.push(delayed[i]) call does not check the result. If
  the ring buffer is nearly full, delayed envelopes from the impairment engine
  are silently dropped with no log and no metric increment. This can cause
  impairment simulation results to differ from expectations in tests.

RISK-3 — Race on m_delay_buf between send and receive paths:
  send_message() calls collect_deliverable() and process_outbound().
  receive_message() also calls collect_deliverable().
  Both mutate m_delay_buf and m_delay_count without any lock. If one thread
  calls send_message() while another calls receive_message() on the same
  UdpBackend instance, a data race occurs on ImpairmentEngine state.
  [The SPSC RingBuffer is race-safe, but the ImpairmentEngine is not.]

RISK-4 — poll_count computation for sub-100ms timeouts:
  For timeout_ms < 100, poll_count = ceil(timeout_ms/100) = 1 iteration with
  a 100ms poll timeout. A caller requesting a 10ms timeout will actually wait
  up to 100ms. The cap of 50 iterations (5000ms) also means a 10-second
  timeout_ms is silently truncated to 5 seconds.

RISK-5 — No inbound impairment processing:
  receive_message() does not call ImpairmentEngine::process_inbound(). The
  inbound reordering logic (ImpairmentEngine.cpp:209) exists but is never
  invoked on the UDP receive path. This means reorder_enabled and
  reorder_window_size have no effect on received datagrams.

RISK-6 — src_ip only used for logging:
  The source IP and port of the received datagram are extracted but are only
  used in the WARNING_HI log message when the queue is full. There is no
  source validation (CLAUDE.md §6.2 "Validate source address"). Any peer can
  send datagrams to the bound port and they will be deserialized and delivered.

RISK-7 — OS buffer saturation during long receive timeouts:
  When poll blocks for up to 5000ms with no traffic, the OS UDP receive buffer
  may fill with incoming datagrams. Excess datagrams are silently dropped by
  the kernel. There is no mechanism to detect or log this condition.

--------------------------------------------------------------------------------

## 15. Unknowns / Assumptions

[ASSUMPTION-1] timestamp_now_us() returns a uint64_t monotonic microsecond
  timestamp. Implementation not provided; assumed POSIX clock_gettime().

[ASSUMPTION-2] envelope_init(env) zero-initializes all fields of the
  MessageEnvelope struct, including the payload array. Definition in
  MessageEnvelope.hpp (not provided).

[ASSUMPTION-3] envelope_copy() performs a complete value copy, including the
  inline payload array of MSG_MAX_PAYLOAD_BYTES (4096 bytes). If the payload is
  a pointer, this would be a shallow copy — a critical correctness concern.
  Power of 10 rule 3 (no dynamic allocation) strongly suggests the payload is
  an inline array, making envelope_copy() a deep copy.

[ASSUMPTION-4] MSG_RING_CAPACITY = 64 (from Types.hpp). The ring buffer can
  hold at most 64 MessageEnvelope objects simultaneously.

[ASSUMPTION-5] sizeof(MessageEnvelope) includes an inline payload of
  MSG_MAX_PAYLOAD_BYTES (4096 bytes) plus the header fields (~44 bytes).
  Each RingBuffer entry therefore occupies approximately 4140+ bytes; m_buf[64]
  consumes approximately 265 KB of object memory.

[ASSUMPTION-6] The caller's timeout_ms intention for sub-100ms timeouts is
  not honored exactly (see RISK-4). This is an implicit contract gap.

[ASSUMPTION-7] No signal handling is assumed. If recvfrom or poll is
  interrupted by a signal (EINTR), the current code treats recv_result < 0 as
  an error and logs a WARNING_LO, then continues the outer loop. This is
  acceptable behavior.

[ASSUMPTION-8] The RingBuffer SPSC contract is satisfied because, in the
  current code structure, push and pop are always called from the same thread
  (receive_message()). A future refactor that separates a background receive
  thread from the consumer thread would need to revisit this.
```

---