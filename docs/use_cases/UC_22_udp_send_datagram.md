================================================================================
UC_22 — UDP SEND DATAGRAM
Flow-of-Control Document
Source files traced:
  src/platform/UdpBackend.cpp / .hpp
  src/platform/SocketUtils.cpp
  src/core/Serializer.cpp
  src/platform/ImpairmentEngine.cpp
  src/core/Types.hpp
================================================================================

## 1. Use Case Overview

Name:       UC_22 — UDP Send Datagram
Actor:      Any caller holding a UdpBackend* (e.g., the application layer or a
            test harness) that has successfully called UdpBackend::init().
Goal:       Serialize a fully populated MessageEnvelope into a wire-format byte
            array, optionally impair it (drop, delay, duplicate), and transmit
            surviving bytes as a single UDP datagram to the configured peer
            IP:port via the sendto(2) syscall.
Preconditions:
  - UdpBackend::init() has returned Result::OK.
  - m_open == true and m_fd >= 0.
  - The supplied MessageEnvelope satisfies envelope_valid() (non-null payload
    pointer if payload_length > 0, valid message_type, etc.).
  - m_cfg.peer_ip and m_cfg.peer_port identify a reachable UDP endpoint.
Postconditions (happy path):
  - At least one UDP datagram containing the serialized envelope has been handed
    to the OS network stack via sendto().
  - Result::OK is returned to the caller.
  - If impairment causes a drop, Result::OK is returned with no datagram sent.

--------------------------------------------------------------------------------

## 2. Entry Points

Primary entry point:
  UdpBackend::send_message(const MessageEnvelope& envelope)
    File:   src/platform/UdpBackend.cpp, line 100
    Caller: Any code that holds a TransportInterface* pointing to a UdpBackend.

Supporting function signatures reached during this call:
  Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
    File:   src/core/Serializer.cpp, line 115

  timestamp_now_us()
    File:   src/core/Timestamp.hpp / .cpp [ASSUMPTION: POSIX clock_gettime]

  ImpairmentEngine::process_outbound(envelope, now_us)
    File:   src/platform/ImpairmentEngine.cpp, line 62

  ImpairmentEngine::collect_deliverable(now_us, delayed_envelopes,
                                         IMPAIR_DELAY_BUF_SIZE)
    File:   src/platform/ImpairmentEngine.cpp, line 174

  socket_send_to(fd, buf, len, ip, port)
    File:   src/platform/SocketUtils.cpp, line 482

  sendto(2) — POSIX syscall
    Invoked inside socket_send_to()

--------------------------------------------------------------------------------

## 3. End-to-End Control Flow (Step-by-Step)

Step 1  —  UdpBackend::send_message() entry (UdpBackend.cpp:100)
           assert(m_open)         — verifies transport is initialized
           assert(m_fd >= 0)      — verifies socket file descriptor is valid
           assert(envelope_valid) — verifies envelope fields are sane

Step 2  —  Serializer::serialize() (Serializer.cpp:115)
           a. assert(buf != nullptr) and assert(buf_len valid)
           b. envelope_valid(env) checked; returns ERR_INVALID if false
           c. required_len = WIRE_HEADER_SIZE + env.payload_length
              If buf_len < required_len → return ERR_INVALID
           d. Sequential big-endian writes into m_wire_buf via write_u8/u32/u64:
              offset 0:  message_type   (1 byte)
              offset 1:  reliability_class (1 byte)
              offset 2:  priority        (1 byte)
              offset 3:  padding = 0     (1 byte)
              offset 4:  message_id      (8 bytes)
              offset 12: timestamp_us    (8 bytes)
              offset 20: source_id       (4 bytes)
              offset 24: destination_id  (4 bytes)
              offset 28: expiry_time_us  (8 bytes)
              offset 36: payload_length  (4 bytes)
              offset 40: padding = 0     (4 bytes)
              offset 44: [payload bytes] (payload_length bytes)
           e. assert(offset == WIRE_HEADER_SIZE) — structure integrity check
           f. memcpy payload into buf[WIRE_HEADER_SIZE]
           g. out_len = required_len; assert(out_len == WIRE_HEADER_SIZE + payload_length)
           h. Returns Result::OK; wire_len is set in UdpBackend::send_message()
           [Control returns to send_message() at UdpBackend.cpp:110]

Step 3  —  Check Serializer result (UdpBackend.cpp:110)
           if (!result_ok(res)) → log WARNING_LO, return res  [early exit]

Step 4  —  timestamp_now_us() (UdpBackend.cpp:116)
           Obtains current wall-clock time in microseconds.
           [ASSUMPTION: implemented via clock_gettime(CLOCK_MONOTONIC) or
            CLOCK_REALTIME; not shown in provided sources.]

Step 5  —  ImpairmentEngine::process_outbound(envelope, now_us)
           (ImpairmentEngine.cpp:62)
           a. assert(m_initialized) and assert(envelope_valid)

           Branch A — impairments DISABLED (m_cfg.enabled == false):
             Finds first inactive slot in m_delay_buf[] (fixed loop, up to
             IMPAIR_DELAY_BUF_SIZE = 32 iterations).
             Copies envelope into slot; sets release_us = now_us (immediate).
             Returns Result::OK.

           Branch B — impairments ENABLED:
             B1. is_partition_active(now_us):
                 If partition state machine returns true → log, return ERR_IO
                 [Interpreted by send_message() as silent drop → return OK]
             B2. Loss check: if loss_probability > 0.0
                 Draw m_prng.next_double(); if < loss_probability → ERR_IO
                 [Silent drop]
             B3. Latency calculation:
                 base_delay_us = fixed_latency_ms * 1000
                 jitter_us     = prng.next_range(0, jitter_variance_ms) * 1000
                 release_us    = now_us + base_delay_us + jitter_us
             B4. Find first inactive m_delay_buf[] slot; copy envelope;
                 set active = true; increment m_delay_count.
             B5. Duplication check: if duplication_probability > 0.0
                 Draw prng.next_double(); if < probability and buffer not full:
                 copy envelope into next inactive slot with release_us + 100 µs.
             B6. Return Result::OK.

Step 6  —  Check process_outbound result (UdpBackend.cpp:118)
           if (res == ERR_IO) → message dropped by impairment → return OK
           [No datagram is sent; caller sees Result::OK (silent drop)]

Step 7  —  ImpairmentEngine::collect_deliverable(now_us, delayed_envelopes,
            IMPAIR_DELAY_BUF_SIZE) (ImpairmentEngine.cpp:174)
           Iterates m_delay_buf[0..31]; for each active slot where
           release_us <= now_us: copies envelope to out_buf, marks slot inactive,
           decrements m_delay_count.
           Returns delayed_count (0 to IMPAIR_DELAY_BUF_SIZE).
           [On first call with non-zero latency, delayed_count will be 0
            because release_us > now_us; the message remains buffered.]

Step 8  —  Send the primary message (UdpBackend.cpp:130)
           socket_send_to(m_fd, m_wire_buf, wire_len,
                          m_cfg.peer_ip, m_cfg.peer_port)
           (SocketUtils.cpp:482):
             a. assert(fd >= 0), assert(buf != NULL), assert(len > 0),
                assert(ip != NULL)
             b. Build sockaddr_in: AF_INET, htons(port), inet_aton(ip)
                If inet_aton fails → log WARNING_LO, return false
             c. sendto(fd, buf, len, 0, &addr, sizeof(addr))
                If sendto returns < 0 → log WARNING_LO, return false
                If send_result != len → log WARNING_HI, return false
             d. assert(send_result == len)
             e. return true
           If socket_send_to returns false → log WARNING_LO, return ERR_IO

Step 9  —  Send delayed messages (UdpBackend.cpp:139)
           Fixed loop: for i in [0, delayed_count):
             assert(i < IMPAIR_DELAY_BUF_SIZE)
             Re-serialize delayed_envelopes[i] into m_wire_buf
             (void)socket_send_to(...) — result intentionally discarded
             [Note: result void-cast; a WARNING_LO silent failure is possible
              without notification to the caller. This is a known design
              trade-off for delayed delivery.]

Step 10 —  Post-condition and return (UdpBackend.cpp:153)
           assert(wire_len > 0)
           return Result::OK

--------------------------------------------------------------------------------

## 4. Call Tree (Hierarchical)

UdpBackend::send_message(envelope)             [UdpBackend.cpp:100]
├── assert(m_open, m_fd >= 0, envelope_valid)
├── Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
│   │                                           [Serializer.cpp:115]
│   ├── envelope_valid(env)
│   ├── write_u8()  ×4                          [Serializer.cpp:22]
│   ├── write_u64() ×3                          [Serializer.cpp:48]
│   ├── write_u32() ×4                          [Serializer.cpp:33]
│   ├── assert(offset == WIRE_HEADER_SIZE)
│   └── memcpy(payload)
├── timestamp_now_us()                          [Timestamp.cpp — not shown]
├── ImpairmentEngine::process_outbound(env, now_us)
│   │                                           [ImpairmentEngine.cpp:62]
│   ├── assert(m_initialized, envelope_valid)
│   ├── [if disabled] — find slot, copy, set release_us=now_us, return OK
│   └── [if enabled]
│       ├── is_partition_active(now_us)         [ImpairmentEngine.cpp:279]
│       │   └── partition state machine transitions
│       ├── m_prng.next_double()  [loss check]
│       ├── m_prng.next_range()   [jitter]
│       ├── envelope_copy() → m_delay_buf[slot]
│       └── m_prng.next_double()  [duplication check]
│           └── envelope_copy() → m_delay_buf[slot+1] (conditional)
├── ImpairmentEngine::collect_deliverable(now_us, delayed_envelopes, cap)
│   │                                           [ImpairmentEngine.cpp:174]
│   └── [loop 0..31] envelope_copy() if slot.active && release_us <= now_us
├── socket_send_to(m_fd, m_wire_buf, wire_len, peer_ip, peer_port)
│   │                                           [SocketUtils.cpp:482]
│   ├── memset(&addr)
│   ├── inet_aton(ip, &addr.sin_addr)
│   ├── sendto(fd, buf, len, 0, &addr, sizeof(addr))   [syscall]
│   └── assert(send_result == len)
└── [loop 0..delayed_count-1]  (delayed message flush)
    ├── Serializer::serialize(delayed_envelopes[i], ...)
    └── socket_send_to(...)
        └── sendto()                                    [syscall]

--------------------------------------------------------------------------------

## 5. Key Components Involved

Component              File(s)                         Role
─────────────────────────────────────────────────────────────────────────────
UdpBackend             UdpBackend.cpp / .hpp           Top-level orchestrator
                                                        of the send path; owns
                                                        m_wire_buf, m_fd,
                                                        m_impairment, m_cfg.
Serializer             src/core/Serializer.cpp         Converts MessageEnvelope
                                                        to deterministic big-
                                                        endian byte array.
ImpairmentEngine       src/platform/ImpairmentEngine.cpp
                                                        Applies loss, latency,
                                                        jitter, duplication, and
                                                        partition impairments.
                                                        Owns m_delay_buf[32].
socket_send_to()       src/platform/SocketUtils.cpp    Thin POSIX wrapper that
                                                        calls sendto(2).
MessageEnvelope        src/core/MessageEnvelope.hpp    Data structure being
                                                        transmitted (not shown
                                                        but referenced).
RingBuffer             src/core/RingBuffer.hpp         Not directly used on the
                                                        send path; relevant on
                                                        receive path only.
Types.hpp              src/core/Types.hpp              Constants (SOCKET_RECV_
                                                        BUF_BYTES=8192,
                                                        IMPAIR_DELAY_BUF_SIZE=32,
                                                        Result enum).
Logger                 src/core/Logger.hpp             Observability; called on
                                                        every error/warning path.

--------------------------------------------------------------------------------

## 6. Branching Logic / Decision Points

Branch Point 1 — Serializer failure (UdpBackend.cpp:110)
  Condition:  !result_ok(res) from Serializer::serialize()
  True path:  Log WARNING_LO "Serialize failed"; return res to caller.
  False path: Continue to timestamp and impairment.

Branch Point 2 — Impairment disabled (ImpairmentEngine.cpp:70)
  Condition:  m_cfg.enabled == false
  True path:  Slot-search loop; copy envelope with release_us = now_us; return OK.
  False path: Apply full impairment chain (partition → loss → latency → dup).

Branch Point 3 — Partition active (ImpairmentEngine.cpp:93)
  Condition:  is_partition_active(now_us) == true
  True path:  Log WARNING_LO; return ERR_IO → silent drop in send_message().
  False path: Continue to loss check.

Branch Point 4 — Packet loss (ImpairmentEngine.cpp:103)
  Condition:  loss_rand < m_cfg.loss_probability
  True path:  Log WARNING_LO; return ERR_IO → silent drop.
  False path: Calculate latency; buffer message.

Branch Point 5 — process_outbound returns ERR_IO (UdpBackend.cpp:118)
  Condition:  res == Result::ERR_IO
  True path:  Return OK to caller (silent drop is not an error to the sender).
  False path: Proceed to collect_deliverable and send.

Branch Point 6 — Delay buffer full (ImpairmentEngine.cpp:125)
  Condition:  m_delay_count >= IMPAIR_DELAY_BUF_SIZE (32)
  True path:  Log WARNING_HI; return ERR_FULL.
  False path: Normal buffering path.

Branch Point 7 — Duplication (ImpairmentEngine.cpp:147)
  Condition:  dup_rand < m_cfg.duplication_probability AND buffer not full
  True path:  Copy envelope to second slot with +100 µs extra delay; log WARNING_LO.
  False path: No duplicate generated.

Branch Point 8 — socket_send_to failure (UdpBackend.cpp:133)
  Condition:  socket_send_to() returns false
  True path:  Log WARNING_LO; return ERR_IO.
  False path: Continue to delayed message loop.

Branch Point 9 — Delayed message re-serialization failure (UdpBackend.cpp:145)
  Condition:  !result_ok(res) in the delayed loop
  True path:  continue (skip this delayed envelope; it is silently lost).
  False path: Attempt socket_send_to(); result is void-cast (not checked).

Branch Point 10 — inet_aton failure (SocketUtils.cpp:498)
  Condition:  inet_aton(ip) == 0 (invalid IP string in m_cfg.peer_ip)
  True path:  Log WARNING_LO; return false → ERR_IO propagates up.
  False path: Build sockaddr_in and call sendto.

Branch Point 11 — sendto byte count mismatch (SocketUtils.cpp:515)
  Condition:  send_result != len
  True path:  Log WARNING_HI; return false.
  False path: assert(send_result == len); return true.

--------------------------------------------------------------------------------

## 7. Concurrency / Threading Behavior

The UdpBackend is designed for a single-thread-per-direction model consistent
with the F-Prime style subset. No locks are used anywhere in the send path.

m_wire_buf (uint8_t[8192]) is a member of UdpBackend, so concurrent calls to
send_message() from multiple threads would race on this buffer. The design
relies on the caller guaranteeing single-threaded access per UdpBackend
instance. [ASSUMPTION: the caller serializes access.]

m_impairment is an ImpairmentEngine owned by UdpBackend. It maintains mutable
state (m_delay_buf, m_delay_count, PRNG state). Concurrent calls would corrupt
it. Same single-caller assumption applies.

m_recv_queue (RingBuffer) uses std::atomic<uint32_t> with acquire/release
ordering for SPSC safety. The send path does NOT touch m_recv_queue directly.

The ImpairmentEngine::collect_deliverable() call during send_message() is the
only point where delayed messages from prior calls are flushed; this means
delayed message delivery is opportunistic and tied to the next send_message()
invocation, not to a background timer thread.

--------------------------------------------------------------------------------

## 8. Memory & Ownership Semantics (C/C++ Specific)

m_wire_buf  — Fixed-size stack-member array (uint8_t[8192]). Owned by the
              UdpBackend instance. No heap allocation. Overwritten on every
              send_message() call. The delayed message loop reuses the same
              buffer for re-serialization, so delayed_envelopes[i] data must
              be consumed before the next iteration writes to m_wire_buf.

m_delay_buf — Fixed-size array of DelayedEntry[IMPAIR_DELAY_BUF_SIZE] (32
              entries) inside ImpairmentEngine. Owned by ImpairmentEngine
              (which is a value-member of UdpBackend). No heap. Entries are
              copied (via envelope_copy()) not pointed to, ensuring value
              semantics throughout. [ASSUMPTION: envelope_copy performs a flat
              memcpy of all MessageEnvelope fields including the payload array.]

delayed_envelopes[] — Declared as a VLA-like local array on the stack of
              send_message():
                MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE];
              (Power of 10 rule 3: size is compile-time constant 32, so the
              stack allocation is fixed and bounded; this is acceptable.)

MessageEnvelope passed to send_message() — passed by const reference. The
              function does not take ownership; it reads fields and copies the
              payload via memcpy inside serialize(). The caller retains
              ownership of the original envelope.

WIRE_HEADER_SIZE — compile-time constant (44 bytes based on field layout:
              4 bytes fixed fields + 8 + 8 + 4 + 4 + 8 + 4 + 4 = 44).
              [ASSUMPTION: derived from Serializer.cpp field ordering; not
              explicitly confirmed from a header constant shown in files.]

--------------------------------------------------------------------------------

## 9. Error Handling Flow

Error                     Source                    Handling
──────────────────────────────────────────────────────────────────────────────
ERR_INVALID               Serializer::serialize()   Logged WARNING_LO; returned
                                                     immediately to caller.
ERR_IO (loss/partition)   process_outbound()        Intercepted at UdpBackend.cpp:118;
                                                     converted to Result::OK (silent
                                                     drop by design).
ERR_FULL (delay buf full) process_outbound()        [ASSUMPTION: propagated as ERR_FULL
                                                     to caller — not explicitly tested
                                                     in send_message(); the check at
                                                     line 118 only tests ERR_IO.]
                                                     [RISK: ERR_FULL from process_outbound
                                                     would fall through to socket_send_to
                                                     and send the datagram even though
                                                     the engine cannot track it. See
                                                     Section 14.]
socket_send_to() = false  SocketUtils.cpp           Logged WARNING_LO; returned ERR_IO
                                                     to caller.
inet_aton() failure       SocketUtils.cpp:498        Logged WARNING_LO; false returned
                                                     from socket_send_to().
sendto() partial write    SocketUtils.cpp:515        Logged WARNING_HI; false returned.
Delayed re-serialize fail UdpBackend.cpp:145         continue (silently skip that
                                                     delayed envelope).
Delayed socket_send_to()  UdpBackend.cpp:149         Return value void-cast; no error
                                                     reported.

Severity mapping (F-Prime model):
  WARNING_LO — localized, recoverable (e.g., one packet dropped).
  WARNING_HI — system-wide but recoverable (e.g., queue full, partial send).
  FATAL      — not emitted on the send path; reserved for init failures.

--------------------------------------------------------------------------------

## 10. External Interactions

Interaction                Details
──────────────────────────────────────────────────────────────────────────────
sendto(2) syscall          The only true external interaction on the send path.
                           Parameters: fd=m_fd, buf=m_wire_buf, len=wire_len,
                           flags=0, dest_addr=sockaddr_in{peer_ip, peer_port},
                           addrlen=sizeof(sockaddr_in).
                           Returns ssize_t bytes sent, or -1 on error.

OS UDP socket              m_fd is an AF_INET/SOCK_DGRAM socket created in init().
                           The kernel may silently drop the datagram if the send
                           buffer is full or the network is unreachable.

inet_aton(3) / htons(3)   POSIX library calls inside socket_send_to(); not
                           syscalls but platform-specific functions.

clock_gettime / gettimeofday  [ASSUMPTION] Called inside timestamp_now_us()
                           to obtain current time for impairment scheduling.

Logger::log()              Calls some underlying platform log sink
                           [ASSUMPTION: writes to stderr or a ring buffer;
                           not a syscall visible in the send hot path].

--------------------------------------------------------------------------------

## 11. State Changes / Side Effects

Object            Field                Change
──────────────────────────────────────────────────────────────────────────────
ImpairmentEngine  m_delay_buf[i].env   Overwritten with copy of envelope.
ImpairmentEngine  m_delay_buf[i].active Set to true (new message buffered).
ImpairmentEngine  m_delay_buf[i].release_us Set to now_us + computed delay.
ImpairmentEngine  m_delay_count        Incremented by 1 (or 2 if duplicated).
ImpairmentEngine  m_prng (internal)    Advances PRNG state for loss/jitter/dup.
ImpairmentEngine  m_partition_active   May transition via is_partition_active().
ImpairmentEngine  m_next_partition_event_us  Updated on partition transitions.
UdpBackend        m_wire_buf           Overwritten with serialized bytes.
UdpBackend        m_recv_queue         NOT modified on the send path.
UdpBackend        m_open, m_fd         NOT modified.
OS Kernel         UDP send buffer      Datagram enqueued for transmission.
ImpairmentEngine  m_delay_buf[i].active Set to false (collect_deliverable).
ImpairmentEngine  m_delay_count        Decremented for each collected entry.

--------------------------------------------------------------------------------

## 12. Sequence Diagram (ASCII)

  Caller              UdpBackend           Serializer       ImpairmentEngine    SocketUtils      OS Kernel
    |                     |                    |                   |                 |               |
    |--send_message(env)-->|                   |                   |                 |               |
    |                     |--serialize(env)--->|                   |                 |               |
    |                     |                   |--write fields----->|                 |               |
    |                     |                   |<--Result::OK-------|                 |               |
    |                     |--timestamp_now_us()|                   |                 |               |
    |                     |<--now_us-----------|                   |                 |               |
    |                     |--process_outbound(env, now_us)-------->|                 |               |
    |                     |                   |   [check partition, loss, jitter]    |               |
    |                     |<--Result::OK (buffered in delay_buf)---|                 |               |
    |                     |--collect_deliverable(now_us)---------->|                 |               |
    |                     |<--delayed_count=0 (none ready yet)-----|                 |               |
    |                     |--socket_send_to(fd,buf,len,ip,port)--->|                 |               |
    |                     |                   |                   |--sendto()------->|               |
    |                     |                   |                   |                 |--sendto(2)---->|
    |                     |                   |                   |                 |<--ssize_t------|
    |                     |                   |                   |<--true----------|               |
    |                     |<--true------------|                   |                 |               |
    |                     |  [no delayed msgs; loop skipped]      |                 |               |
    |<--Result::OK---------|                  |                   |                 |               |

  Drop path (loss impairment active):
    |--send_message(env)-->|
    |                     |--serialize()----->|  (OK)
    |                     |--process_outbound()-->| (ERR_IO = drop)
    |<--Result::OK --------|  (silent drop; no sendto called)

--------------------------------------------------------------------------------

## 13. Initialization vs Runtime Flow

Initialization (UdpBackend::init(), UdpBackend.cpp:45):
  - socket_create_udp() → socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
  - socket_set_reuseaddr() → setsockopt(SO_REUSEADDR)
  - socket_bind() → inet_aton + bind(2)
  - m_recv_queue.init() → atomic store of head=0, tail=0
  - impairment_config_default(imp_cfg) → fill ImpairmentConfig with defaults
  - m_impairment.init(imp_cfg) → seed PRNG, zero m_delay_buf, set m_initialized=true
  - m_open = true
  All allocations are static (member arrays). No malloc/new anywhere.

Runtime (send_message()):
  - No allocation. m_wire_buf reused every call.
  - delayed_envelopes[] is a stack frame local (32 * sizeof(MessageEnvelope)).
  - All state mutation is in-place within member objects.

The distinction matters for Power of 10 rule 3: heap is never touched after
the init phase is complete.

--------------------------------------------------------------------------------

## 14. Known Risks / Observations

RISK-1 — ERR_FULL not filtered at send_message():
  process_outbound() can return ERR_FULL when the delay buffer is saturated.
  UdpBackend::send_message() only checks for ERR_IO (line 118). If ERR_FULL is
  returned, execution falls through to collect_deliverable() and socket_send_to(),
  sending the datagram without any impairment tracking. The message will appear
  on the wire but will not be in the delay buffer, breaking impairment
  accounting. This is a logic gap.

RISK-2 — Delayed message send result discarded (UdpBackend.cpp:149):
  (void)socket_send_to(...) for delayed envelopes. A network failure during
  delayed message flush is silently swallowed. The caller receives Result::OK
  regardless. This is a deliberate design trade-off (delayed delivery is
  best-effort) but is worth flagging for safety reviews.

RISK-3 — m_wire_buf reuse within the delayed loop:
  The delayed message loop re-serializes into the same m_wire_buf. The loop
  processes one delayed envelope at a time, serializing and immediately sending
  it before moving to the next, so this is safe. However, if a future refactor
  splits these two steps, a buffer aliasing bug could occur.

RISK-4 — Opportunistic delayed flush timing:
  Delayed messages accumulate in m_delay_buf and are only flushed when
  send_message() or receive_message() is called. If no sends or receives occur
  for an extended period, delayed messages will not be delivered until the next
  call. This is acceptable for simulation but may cause unexpected timing
  behavior in production-like scenarios.

RISK-5 — No IPv6 support:
  socket_send_to() hardcodes AF_INET and uses inet_aton(), which does not
  support IPv6. The CLAUDE.md requirement §6.3 notes "support IPv4, and
  optionally IPv6." IPv6 is not implemented.

RISK-6 — inet_aton vs inet_pton:
  inet_aton() is used throughout SocketUtils.cpp. POSIX marks inet_aton as
  obsolescent; inet_pton is preferred. This is a portability concern.

--------------------------------------------------------------------------------

## 15. Unknowns / Assumptions

[ASSUMPTION-1] timestamp_now_us() is implemented via POSIX clock_gettime()
  or similar. Its implementation file is not provided. The function returns
  a uint64_t microsecond timestamp.

[ASSUMPTION-2] envelope_valid(envelope) checks that message_type is not
  INVALID, payload pointer is non-null if payload_length > 0, and other
  basic field sanity. Its definition is in MessageEnvelope.hpp (not provided).

[ASSUMPTION-3] envelope_copy() performs a flat struct copy (memcpy or
  field-by-field assignment) including the inline payload array. If
  MessageEnvelope contains a pointer to an external payload buffer, envelope_copy
  would only copy the pointer, not the data — a critical safety concern.
  Given the Power of 10 rule (no dynamic allocation), the payload is assumed
  to be an inline array of MSG_MAX_PAYLOAD_BYTES.

[ASSUMPTION-4] WIRE_HEADER_SIZE = 44 bytes, computed from the sequential
  field writes in Serializer.cpp: 4 (fixed bytes) + 8+8+4+4+8+4+4 = 44.
  This constant's definition file (Serializer.hpp) is not shown.

[ASSUMPTION-5] impairment_config_default() zero-fills or sets safe defaults
  (e.g., loss_probability=0.0, enabled=false by default if not configured)
  so that the impairment engine is a pass-through unless explicitly configured.

[ASSUMPTION-6] The caller guarantees single-threaded access to any given
  UdpBackend instance. The code has no mutex protecting m_wire_buf or
  m_impairment.

[ASSUMPTION-7] m_prng is a deterministic LCG or Xorshift PRNG seeded via
  ImpairmentEngine::init(). Its type and implementation are not provided.

[ASSUMPTION-8] Logger::log() is thread-safe or is only called from a single
  thread context. No mutex is visible at its call sites.
```

---