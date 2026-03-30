Use Case: TcpBackend sends a length-prefixed framed message over an established TCP connection.
Goal: Trace TcpBackend::send_message() → Serializer::serialize() →
      ImpairmentEngine::process_outbound() → tcp_send_frame() → socket_send_all()
      writing 4-byte length prefix + payload bytes.

## 1. Use Case Overview

Actor:        Upper-layer application code calling TcpBackend::send_message() with a
              populated MessageEnvelope.
Precondition: TcpBackend has been initialized (m_open == true); at least one client fd
              is connected (m_client_count > 0); m_client_fds[i] >= 0 for at least one i.
Postcondition: Serialized bytes have been written to the TCP stream of all connected
               clients as a 4-byte big-endian length prefix followed by the serialized
               envelope payload. Result::OK returned to caller.
Trigger:      Application invokes send_message(envelope).
Success path: serialize → process_outbound (no drop) → collect_deliverable (0 or more
              delayed) → tcp_send_frame (4-byte header + N bytes) → all bytes sent via
              socket_send_all → Result::OK.
Failure paths:
  - envelope_valid() fails → assert fires.
  - Serializer::serialize() fails → WARNING_LO logged → ERR_INVALID returned.
  - ImpairmentEngine drops (ERR_IO) → message silently discarded → Result::OK returned
    to caller (drop is by design).
  - tcp_send_frame() fails on a client → WARNING_LO logged; other clients still attempted.

## 2. Entry Points

Primary entry: TcpBackend::send_message(const MessageEnvelope& envelope)
               File: src/platform/TcpBackend.cpp, line 249
               Precondition assertions (lines 251–252):
                   assert(m_open)
                   assert(envelope_valid(envelope))

## 3. End-to-End Control Flow (Step-by-Step)

--- Phase 1: Serialize the envelope ---

Step 1.  Entry assertion checks (TcpBackend.cpp lines 251–252):
           assert(m_open)              — must be true.
           assert(envelope_valid(env)) — envelope fields must be valid.

Step 2.  uint32_t wire_len = 0 (line 255).
         Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
         (line 256) → Serializer.cpp line 115:

         a. assert(buf != nullptr) — m_wire_buf is an inline array, always non-null.
            assert(buf_len <= 0xFFFFFFFF) — 8192 passes.

         b. envelope_valid(env) check (line 125) — if false, return ERR_INVALID.

         c. required_len = WIRE_HEADER_SIZE + env.payload_length (line 130).
            WIRE_HEADER_SIZE is a compile-time constant (inferred: 4+8+8+4+4+8+4+4 = 44
            bytes: 1 message_type + 1 reliability_class + 1 priority + 1 pad +
            8 message_id + 8 timestamp_us + 4 source_id + 4 destination_id +
            8 expiry_time_us + 4 payload_length + 4 pad = 44 bytes).
            [ASSUMPTION: WIRE_HEADER_SIZE == 44U; exact value defined in Serializer.hpp.]
            if (buf_len < required_len) return ERR_INVALID.

         d. Sequential write_u8/write_u32/write_u64 calls building the wire header
            into m_wire_buf[0..WIRE_HEADER_SIZE-1]:
              offset=0:  write_u8(buf, 0, message_type)        → offset=1
              offset=1:  write_u8(buf, 1, reliability_class)   → offset=2
              offset=2:  write_u8(buf, 2, priority)            → offset=3
              offset=3:  write_u8(buf, 3, 0U) padding          → offset=4
              offset=4:  write_u64(buf, 4, message_id)         → offset=12
              offset=12: write_u64(buf, 12, timestamp_us)      → offset=20
              offset=20: write_u32(buf, 20, source_id)         → offset=24
              offset=24: write_u32(buf, 24, destination_id)    → offset=28
              offset=28: write_u64(buf, 28, expiry_time_us)    → offset=36
              offset=36: write_u32(buf, 36, payload_length)    → offset=40
              offset=40: write_u32(buf, 40, 0U) padding        → offset=44
            assert(offset == WIRE_HEADER_SIZE) — post-condition.

         e. memcpy(&m_wire_buf[44], env.payload, env.payload_length) (line 158).
            (Only if payload_length > 0.)

         f. out_len = required_len = 44 + env.payload_length.
            assert(out_len == WIRE_HEADER_SIZE + env.payload_length).
            Returns Result::OK.

Step 3.  Back in send_message(): if !result_ok(res): log WARNING_LO, return res.

--- Phase 2: Apply impairment (outbound) ---

Step 4.  now_us = timestamp_now_us() (line 264)
         → Timestamp.hpp/cpp [ASSUMPTION: returns current monotonic time in microseconds].

Step 5.  m_impairment.process_outbound(envelope, now_us) (line 265)
         → ImpairmentEngine.cpp line 62:

         a. assert(m_initialized); assert(envelope_valid(in_env)).

         b. If !m_cfg.enabled (impairments disabled, which is the default per
            channel_config_default — impairments_enabled = false):
              Find first inactive slot in m_delay_buf[0..31].
              envelope_copy(m_delay_buf[slot].env, in_env).
              m_delay_buf[slot].release_us = now_us (deliver immediately).
              m_delay_buf[slot].active = true.
              ++m_delay_count.
              Returns Result::OK.

         c. If m_cfg.enabled (impairments ON):
              i.   is_partition_active(now_us) — checks partition state machine.
                   If partition active: log WARNING_LO, return ERR_IO (message dropped).
              ii.  Loss check: if loss_probability > 0.0:
                     loss_rand = m_prng.next_double()
                     if loss_rand < loss_probability: log WARNING_LO, return ERR_IO.
              iii. Compute latency: base_delay_us = fixed_latency_ms * 1000
                   jitter_us = m_prng.next_range(0, jitter_variance_ms) * 1000
                   release_us = now_us + base_delay_us + jitter_us.
              iv.  Find first inactive slot in m_delay_buf[]; copy envelope in;
                   set release_us; active=true; ++m_delay_count.
              v.   Duplication check: if duplication_probability > 0.0:
                     dup_rand = m_prng.next_double()
                     if dup_rand < duplication_probability:
                       find next inactive slot; copy envelope; release_us += 100us.
              vi.  Returns Result::OK.

Step 6.  Back in send_message() (line 266):
           if (res == Result::ERR_IO) — message dropped by impairment.
             return Result::OK  (drop is silent to the caller).
           Otherwise continue.

--- Phase 3: Collect delayed messages ---

Step 7.  MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]  — stack array of 32
         envelopes allocated on the stack (line 272). [NOTE: This is a large stack
         allocation — 32 * sizeof(MessageEnvelope).]
         uint32_t delayed_count = m_impairment.collect_deliverable(now_us,
                                  delayed_envelopes, IMPAIR_DELAY_BUF_SIZE) (line 273).
         → ImpairmentEngine.cpp line 174:
           Scans m_delay_buf[0..31]; for each active slot where release_us <= now_us:
             envelope_copy(out_buf[out_count], m_delay_buf[i].env)
             m_delay_buf[i].active = false; --m_delay_count; ++out_count.
           Returns out_count.

--- Phase 4: Send main message to all clients ---

Step 8.  If m_client_count == 0 (line 278):
           Logger WARNING_LO "No clients connected; discarding message".
           return Result::OK.

Step 9.  Loop i = 0..MAX_TCP_CONNECTIONS-1 (i.e., 0..7) (line 285):
           if (m_client_fds[i] < 0) continue.  — skip empty slots.
           tcp_send_frame(m_client_fds[i], m_wire_buf, wire_len,
                          m_cfg.channels[0].send_timeout_ms) (line 290)
           → SocketUtils.cpp line 391:

         a. assert(fd >= 0); assert(buf != NULL).

         b. Build 4-byte big-endian length header (lines 399–403):
              header[0] = (wire_len >> 24) & 0xFF
              header[1] = (wire_len >> 16) & 0xFF
              header[2] = (wire_len >> 8)  & 0xFF
              header[3] =  wire_len        & 0xFF

         c. socket_send_all(fd, header, 4, send_timeout_ms) (line 406)
            → SocketUtils.cpp line 290:
              sent = 0
              while (sent < 4):
                poll({fd, POLLOUT}, 1, timeout_ms)
                  → syscall: poll() waits for socket writability.
                if poll_result <= 0: log WARNING_HI, return false.
                send(fd, &header[sent], 4-sent, 0)
                  → syscall: send() writes bytes to TCP stream.
                if send_result < 0: log WARNING_HI, return false.
                sent += send_result.
              assert(sent == 4). Returns true.

         d. If socket_send_all(header) fails:
              Logger WARNING_HI "tcp_send_frame: failed to send header".
              return false.

         e. If wire_len > 0:
              socket_send_all(fd, m_wire_buf, wire_len, send_timeout_ms) (line 414)
              Same polling loop as (c), but for wire_len bytes of serialized payload.
              Each iteration: poll() → send() → advance sent counter.
              On failure: Logger WARNING_HI "failed to send payload", return false.

         f. Returns true (all bytes of header + payload sent).

Step 10. Back in main client loop (line 290–294):
           if (!tcp_send_frame(...)): Logger WARNING_LO "Send frame failed on client %u".
           Loop continues regardless — next client fd is attempted.
           (Partial failure on one client does not abort other clients.)

--- Phase 5: Send delayed messages ---

Step 11. Loop i = 0..delayed_count-1 (line 298):
           assert(i < IMPAIR_DELAY_BUF_SIZE).
           uint32_t delayed_len = 0.
           Serializer::serialize(delayed_envelopes[i], m_wire_buf,
                                 SOCKET_RECV_BUF_BYTES, delayed_len).
           if !result_ok(res): continue (skip this delayed message; no logging).
           Inner loop j = 0..MAX_TCP_CONNECTIONS-1:
             if m_client_fds[j] < 0: continue.
             (void)tcp_send_frame(m_client_fds[j], m_wire_buf, delayed_len,
                                 m_cfg.channels[0].send_timeout_ms).
             Return value explicitly cast to void (not checked). [NOTE: errors silently
             dropped for delayed messages.]

Step 12. Line 317: assert(wire_len > 0)  — post-condition (main message was non-zero).
         Returns Result::OK.

## 4. Call Tree (Hierarchical)

TcpBackend::send_message(envelope)                          [TcpBackend.cpp:249]
  Serializer::serialize(envelope, m_wire_buf, 8192, &wire_len)  [Serializer.cpp:115]
    envelope_valid(env)
    write_u8(buf, offset, ...)  [x4 — type, reliability, priority, pad]
    write_u64(buf, offset, ...) [x3 — message_id, timestamp_us, expiry_time_us]
    write_u32(buf, offset, ...) [x4 — source_id, dest_id, payload_length, pad]
    memcpy(&buf[WIRE_HEADER_SIZE], payload, payload_length)
  timestamp_now_us()                                         [Timestamp.hpp]
  ImpairmentEngine::process_outbound(envelope, now_us)       [ImpairmentEngine.cpp:62]
    is_partition_active(now_us)                              [ImpairmentEngine.cpp:279]
    m_prng.next_double()                                     [loss check, if enabled]
    m_prng.next_range(0, jitter_variance_ms)                 [jitter, if enabled]
    envelope_copy(m_delay_buf[slot].env, in_env)
    m_prng.next_double()                                     [dup check, if enabled]
    envelope_copy(m_delay_buf[dup_slot].env, in_env)         [if duplicated]
  ImpairmentEngine::collect_deliverable(now_us, delayed, 32) [ImpairmentEngine.cpp:174]
    envelope_copy(out_buf[i], m_delay_buf[j].env)            [for each deliverable]
  [Loop over MAX_TCP_CONNECTIONS clients]
    tcp_send_frame(client_fd, m_wire_buf, wire_len, timeout) [SocketUtils.cpp:391]
      socket_send_all(fd, header, 4, timeout)                [SocketUtils.cpp:290]
        poll({fd,POLLOUT}, 1, timeout_ms)                    [POSIX syscall]
        send(fd, &header[sent], remaining, 0)                [POSIX syscall]
      socket_send_all(fd, m_wire_buf, wire_len, timeout)     [SocketUtils.cpp:290]
        poll({fd,POLLOUT}, 1, timeout_ms)                    [POSIX syscall, per loop]
        send(fd, &buf[sent], remaining, 0)                   [POSIX syscall, per loop]
  [Loop over delayed_count delayed messages]
    Serializer::serialize(delayed_envelopes[i], m_wire_buf, ...)
    [Loop over MAX_TCP_CONNECTIONS clients]
      tcp_send_frame(...)                                     [same as above]

## 5. Key Components Involved

Component              File                                    Role
---------              ----                                    ----
TcpBackend             src/platform/TcpBackend.cpp             Orchestrates send logic,
                                                               owns m_wire_buf, manages
                                                               client fds.
Serializer             src/core/Serializer.cpp                 Converts MessageEnvelope
                                                               to deterministic binary
                                                               wire format.
ImpairmentEngine       src/platform/ImpairmentEngine.cpp       Applies loss/latency/
                                                               jitter/dup impairments;
                                                               buffers delayed messages.
SocketUtils            src/platform/SocketUtils.cpp            tcp_send_frame() and
                                                               socket_send_all() — raw
                                                               POSIX send path.
Timestamp              src/core/Timestamp.hpp                  now_us for impairment
                                                               timing.
Logger                 src/core/Logger.hpp                     Event logging on errors
                                                               and info events.
Types.hpp              src/core/Types.hpp                      Constants; Result enum.
ChannelConfig.hpp      src/core/ChannelConfig.hpp              send_timeout_ms from
                                                               channels[0].

## 6. Branching Logic / Decision Points

Decision Point 1: result_ok(Serializer::serialize()) (line 258)
  false → Logger WARNING_LO, return ERR_INVALID. Send aborted.
  true  → continue to impairment.

Decision Point 2: ImpairmentEngine::process_outbound() returns ERR_IO (line 266)
  true  → message dropped (partition or loss); return Result::OK to caller.
  false → continue.

Decision Point 3: m_cfg.enabled in process_outbound (ImpairmentEngine.cpp:70)
  false → message placed in delay_buf with release_us = now_us (deliver immediately).
  true  → full impairment pipeline (partition check, loss, latency, jitter, dup).

Decision Point 4: is_partition_active() (ImpairmentEngine.cpp:93)
  true  → return ERR_IO (drop).
  false → continue loss check.

Decision Point 5: loss_rand < loss_probability (ImpairmentEngine.cpp:103)
  true  → return ERR_IO (drop).
  false → compute latency and buffer.

Decision Point 6: dup_rand < duplication_probability (ImpairmentEngine.cpp:147)
  true  → attempt to add duplicate to delay_buf.
  false → no duplicate.

Decision Point 7: m_client_count == 0 (line 278)
  true  → log WARNING_LO "No clients connected"; return OK (discard).
  false → proceed to send loop.

Decision Point 8: m_client_fds[i] < 0 in client send loop (line 286)
  true  → skip slot (not connected).
  false → call tcp_send_frame().

Decision Point 9: tcp_send_frame() return value in main loop (line 290)
  false → log WARNING_LO "Send frame failed on client %u"; continue loop.
  true  → next client.

Decision Point 10: delayed_count > 0 (implicit, line 298 loop)
  0     → delayed send loop body never executes.
  >0    → each delayed message serialized and sent to all clients.

Decision Point 11: wire_len > 0 in tcp_send_frame (SocketUtils.cpp:413)
  false → payload send skipped (zero-length payload message).
  true  → socket_send_all() called for payload.

Decision Point 12: poll() result in socket_send_all (SocketUtils.cpp:309)
  <= 0 → timeout; log WARNING_HI; return false.
  > 0  → issue send().

Decision Point 13: send() result in socket_send_all (SocketUtils.cpp:318)
  < 0  → error; log WARNING_HI; return false.
  >= 0 → advance sent counter; loop continues.

## 7. Concurrency / Threading Behavior

- send_message() is single-threaded; no locks are held.
- m_wire_buf is a shared instance variable reused between main message and delayed
  messages in the same call. [NOTE: Delayed message serialization overwrites m_wire_buf
  in the delayed_count loop at line 302. This is safe because the main message has
  already been sent to all clients before the delayed loop begins.]
- ImpairmentEngine state (m_delay_buf, m_prng, etc.) is modified during
  process_outbound() and collect_deliverable(). These are not thread-safe.
  [ASSUMPTION: single-threaded use.]
- poll() and send() are blocking POSIX syscalls; if send_timeout_ms is non-zero,
  the thread may block up to send_timeout_ms per poll() call per client per
  socket_send_all() loop iteration.
- For N clients and payload of W bytes, worst-case blocking time in socket_send_all
  is N * ceil(W / MTU) * send_timeout_ms (if the TCP kernel buffer is full).
  [ASSUMPTION: send_timeout_ms is set to a reasonable value such as 1000ms per
  channel_config_default.]
- RingBuffer is not involved in send_message(). Only m_recv_queue (involved in receive)
  is a RingBuffer.

## 8. Memory & Ownership Semantics (C/C++ Specific)

- m_wire_buf[8192]: TcpBackend member; serialized bytes written here; not heap.
  Reused across calls. Valid for the lifetime of the TcpBackend object.
- wire_len (uint32_t): local variable in send_message(); passed by reference to
  Serializer::serialize() which writes the actual length.
- delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]: stack-allocated array of 32 MessageEnvelope
  objects (line 272). Each MessageEnvelope contains a uint8_t payload[MSG_MAX_PAYLOAD_BYTES]
  = payload[4096]. Total stack allocation: 32 * (sizeof header fields + 4096) bytes.
  [RISK: This is a substantial stack frame — potentially 32 * ~4140 bytes ≈ 132 KB.
  Exact sizeof(MessageEnvelope) not read; may be smaller if payload is a pointer.
  [ASSUMPTION: payload is an inline array — see Risk section.]]
- header[4]: local array in tcp_send_frame(); stack; contains the 4-byte length prefix.
  Ephemeral; exists only during tcp_send_frame() execution.
- envelope parameter to send_message(): passed by const reference; no copy made at
  entry. Serializer reads from it directly.
- envelope_copy() in process_outbound() and collect_deliverable(): deep-copies into
  m_delay_buf[i].env (ImpairmentEngine member) and out of it. No heap involved.
- All POSIX buffers (header[], m_wire_buf) are owned by this layer; no ownership
  transfer to OS (send() copies data into kernel buffers).
- Power of 10 rule 3: no malloc or new anywhere in the send path.

## 9. Error Handling Flow

Error source                 Detected at / line            Action
------------                 ------------------            ------
envelope_valid() fails       send_message() line 252       assert fires (debug abort)
Serializer::serialize ERR    send_message() line 258       Log WARNING_LO; return ERR_INVALID
Impairment: drop (ERR_IO)    send_message() line 266       Silent; return Result::OK to caller
delay_buf full (ERR_FULL)    process_outbound() line 128   Log WARNING_HI; return ERR_FULL
                             [ERR_FULL is NOT currently    (falls through to caller;
                              handled in send_message()]    send_message() does not check
                                                           for ERR_FULL from process_outbound
                                                           — only ERR_IO is special-cased.
                                                           ERR_FULL would exit the ERR_IO
                                                           check at line 266 and proceed
                                                           to the send loop, sending the
                                                           un-delayed message directly.)
                                                           [OBSERVATION: this is a subtle
                                                           behavior — see Section 14.]
poll() timeout (send)        socket_send_all() line 309    Log WARNING_HI; return false
send() error                 socket_send_all() line 318    Log WARNING_HI; return false
tcp_send_frame fails         send_message() line 290-294   Log WARNING_LO; continue to
                                                           next client (not fatal)
Delayed msg serialize fails  send_message() line 304       continue (skip delayed msg;
                                                           no log; silently dropped)
Delayed tcp_send_frame fails send_message() line 312-313   (void) cast — error silently
                                                           dropped; no log
m_client_count == 0          send_message() line 278       Log WARNING_LO; return OK

Severity classification:
  - WARNING_HI: poll timeout, send() failure (socket-level I/O errors).
  - WARNING_LO: no clients, frame send failure per client, serialize failure.

## 10. External Interactions

Interaction                   syscall / API                   When
-----------                   -------------                   ----
Get current time              timestamp_now_us()              Before impairment check
Poll socket writability       poll({fd,POLLOUT},1,timeout)    Per socket_send_all() loop
                                                              iteration (header + payload)
Write bytes to TCP stream     send(fd, buf, len, 0)           Per socket_send_all() loop
                                                              iteration
Logger output                 Logger::log()                   On errors and INFO events

Number of poll()+send() calls per send_message() invocation:
  For 1 client, 1 main message, 0 delayed:
    - 2 socket_send_all calls (header + payload).
    - Each may loop if kernel buffer partially accepts data.
    - Minimum: 2 poll() + 2 send() calls (best case, all data accepted in one send).
  For N clients and D delayed messages: N*(2+) + D*N*(2+) poll/send pairs.

## 11. State Changes / Side Effects

State before send_message():
  m_wire_buf: undefined (from prior call).
  m_impairment: m_delay_buf may have pre-existing delayed items.
  m_client_fds[]: connected clients.

State after successful send_message():
  m_wire_buf: overwritten with last message's serialized bytes (main or last delayed).
  m_impairment.m_delay_buf: main message added (active=true, release_us set); any
    deliverable messages removed (active=false, m_delay_count decremented).
  m_impairment.m_prng: advanced by N samples (loss, jitter, dup checks if enabled).
  m_impairment.m_partition_active, m_next_partition_event_us: may change if partition
    state machine transitions (inside is_partition_active()).
  OS TCP send buffer: kernel has accepted bytes for each connected client.
  No change to m_recv_queue, m_client_fds, m_listen_fd, m_open, m_client_count.

Side effects:
  - Logger events.
  - PRNG state advancement.
  - OS kernel TCP send buffers updated.
  - m_wire_buf mutated (not zeroed after use).
  - Stack: delayed_envelopes[32] allocated and freed within send_message() scope.

## 12. Sequence Diagram (ASCII)

App          TcpBackend        Serializer     ImpairmentEngine    SocketUtils       OS
---          ----------        ----------     ----------------    -----------       --
 |               |                 |                |                 |              |
 |--send_msg(env)>|                |                |                 |              |
 |               |--serialize(env,buf,8192,&len)--->|                 |              |
 |               |                |--write_u8/u32/u64 (header fields) |              |
 |               |                |--memcpy(payload)  |              |              |
 |               |<--Result::OK, wire_len=44+N        |              |              |
 |               |                |                   |              |              |
 |               |--timestamp_now_us()                |              |              |
 |               |                                    |              |              |
 |               |--process_outbound(env,now_us)------>|             |              |
 |               |                               (partition?loss?dup?latency?)      |
 |               |                               envelope_copy->delay_buf           |
 |               |<--Result::OK (or ERR_IO=drop)------|             |              |
 |               |                                    |              |              |
 |   [if ERR_IO: return OK to app — drop is silent]  |              |              |
 |               |                                    |              |              |
 |               |--collect_deliverable(now_us,buf,32)->|            |              |
 |               |                               (scan delay_buf)    |              |
 |               |<--delayed_count (0..32)------------|             |              |
 |               |                                    |              |              |
 |               | [loop i=0..7 client fds]           |              |              |
 |               |--tcp_send_frame(fd,wire_buf,len,to)-------------->|              |
 |               |                                    |  build header[4]            |
 |               |                                    |  socket_send_all(header,4)->|
 |               |                                    |              |--poll()----->|
 |               |                                    |              |<--POLLOUT----|
 |               |                                    |              |--send(hdr)-->|
 |               |                                    |              |<--4 bytes----|
 |               |                                    |  socket_send_all(payload,N)->
 |               |                                    |              |--poll()----->|
 |               |                                    |              |<--POLLOUT----|
 |               |                                    |              |--send(pay)-->|
 |               |                                    |              |<--N bytes----|
 |               |<--true---------------------------------|          |              |
 |               | [end loop]                         |              |              |
 |               |                                    |              |              |
 |               | [loop delayed_count]               |              |              |
 |               |--serialize(delayed[i])-->|         |              |              |
 |               |<--Result::OK, delayed_len|         |              |              |
 |               |--tcp_send_frame (per client)----------------------------...      |
 |               | [end loop]                         |              |              |
 |               |                                    |              |              |
 |<--Result::OK---|                                   |              |              |

## 13. Initialization vs Runtime Flow

Initialization (init()):
  - m_wire_buf declared as member; value-initialized to zero by OS/loader.
    [ASSUMPTION: POSIX: BSS/stack zeroing. No explicit memset of m_wire_buf in init().]
  - ImpairmentEngine::init() called; m_delay_buf zeroed; PRNG seeded.
  - m_recv_queue.init() called; ring buffer reset.

Runtime (send_message()):
  - m_wire_buf rewritten on every call; prior contents are overwritten.
  - ImpairmentEngine delay_buf slots allocated and freed dynamically within the
    fixed static array via active flags.
  - No heap allocation at any point.
  - The channels[0].send_timeout_ms value (from TransportConfig, set once during init)
    governs all poll() timeout values during sends.

## 14. Known Risks / Observations

Risk 1: ERR_FULL from process_outbound not handled distinctly
  If the impairment delay buffer is full, process_outbound() returns Result::ERR_FULL
  (ImpairmentEngine.cpp:128). send_message() only special-cases Result::ERR_IO at
  line 266; ERR_FULL falls through and the code proceeds to send the message directly
  (bypassing the impairment delay). This means a full delay buffer silently causes
  the message to bypass delay impairment and be sent immediately. This may or may not
  be intentional.

Risk 2: Large stack allocation for delayed_envelopes
  Line 272 allocates MessageEnvelope delayed_envelopes[32] on the stack. If
  MessageEnvelope contains a payload[4096] inline array, the stack cost is
  approximately 32 * (44 + 4096) = ~132 KB. This is likely to overflow the stack
  on embedded or constrained targets. [ASSUMPTION: MessageEnvelope.payload is inline;
  exact struct size not confirmed from files read.]

Risk 3: m_wire_buf reuse in delayed send loop
  In the delayed send loop (line 301–314), Serializer::serialize writes into m_wire_buf,
  overwriting the main message's serialized bytes. The main message has already been
  sent to all clients at this point (Step 10), so the overwrite is safe. However, the
  ordering dependency is subtle and a maintenance risk.

Risk 4: Silent discard of delayed message send errors
  In the delayed send loop, tcp_send_frame return value is cast to (void) at line 312.
  Errors are silently dropped. No logging occurs for delayed message send failures.

Risk 5: Single channel assumption
  All send operations use m_cfg.channels[0].send_timeout_ms (line 291, 313).
  The code does not select a channel based on the envelope's priority or reliability
  class. If num_channels > 1, channels[1..N-1] are ignored.

Risk 6: No per-client disconnect handling during send
  If tcp_send_frame fails on client[i], a WARNING_LO is logged and the loop continues.
  The client fd is NOT closed or removed from m_client_fds[]. The dead fd will persist
  until the next recv_from_client() detects the disconnect. Subsequent sends to the
  dead fd will keep failing.

Risk 7: PRNG state shared between send and receive paths
  m_prng is a member of ImpairmentEngine and advanced by both process_outbound()
  (called from send_message()) and process_inbound() (called from recv_from_client()
  if used). If both paths are invoked from different threads, the PRNG state would
  have a data race. [ASSUMPTION: single-threaded use.]

## 15. Unknowns / Assumptions

[ASSUMPTION A]  WIRE_HEADER_SIZE is a compile-time constant defined in Serializer.hpp
                (not read). The value 44 bytes is inferred from counting the
                write_u8/write_u32/write_u64 calls in Serializer::serialize().

[ASSUMPTION B]  MessageEnvelope struct layout (from MessageEnvelope.hpp, not read):
                contains message_type, reliability_class, priority, message_id,
                timestamp_us, source_id, destination_id, expiry_time_us, payload_length,
                and payload[MSG_MAX_PAYLOAD_BYTES] or a pointer to payload bytes.
                If payload is inline (uint8_t payload[4096]), the delayed_envelopes[]
                stack allocation is very large.

[ASSUMPTION C]  timestamp_now_us() returns a uint64_t in microseconds from a monotonic
                clock. Its implementation was not read.

[ASSUMPTION D]  m_prng type provides next_double() returning [0.0, 1.0) and
                next_range(lo, hi) returning a uniform uint32_t in [lo, hi].
                Its implementation was not read; assumed deterministic given a seed.

[ASSUMPTION E]  envelope_valid() checks that message_type != INVALID and that
                payload_length <= MSG_MAX_PAYLOAD_BYTES. Exact implementation not read.

[ASSUMPTION F]  envelope_copy() performs a deep field-by-field copy (including payload
                bytes). Exact implementation not read.

[ASSUMPTION G]  impairment_config_default() is a free function that fills ImpairmentConfig
                with safe defaults (loss_probability=0.0, enabled=false, etc.).
                Its declaration was not read directly; behavior inferred from usage.

[ASSUMPTION H]  send_message() is not re-entrant; the caller is responsible for
                ensuring only one goroutine/thread calls it at a time.