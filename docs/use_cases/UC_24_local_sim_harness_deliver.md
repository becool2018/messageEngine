================================================================================
UC_24 — LOCAL SIM HARNESS DELIVER
Flow-of-Control Document
Source files traced:
  src/platform/LocalSimHarness.cpp / .hpp
  src/platform/ImpairmentEngine.cpp
  src/core/RingBuffer.hpp
  src/core/Types.hpp
================================================================================

## 1. Use Case Overview

Name:       UC_24 — LocalSimHarness In-Process Message Delivery
Actor:      A test harness or simulation orchestrator that has created two
            LocalSimHarness instances, initialized both, and linked them via
            link().
Goal:       Trace the complete in-process message delivery path from
            sender.link(receiver) establishment through sender.send_message()
            applying impairment → receiver.inject() pushing the envelope into
            the receiver's RingBuffer → receiver.receive_message() returning
            the envelope — all without any real socket or OS network stack
            involvement.
Preconditions:
  - Both LocalSimHarness instances (call them A and B) have been constructed
    and init() has returned Result::OK on each.
  - A.link(&B) has been called: A.m_peer == &B.
  - A.m_open == true, B.m_open == true.
  - The envelope to send satisfies envelope_valid().
Postconditions (happy path):
  - The MessageEnvelope is available in B.m_recv_queue.
  - B.receive_message() returns Result::OK with the envelope populated.
  - No syscalls are issued; all delivery is through in-memory function calls.

--------------------------------------------------------------------------------

## 2. Entry Points

Setup entry point:
  LocalSimHarness::link(LocalSimHarness* peer)
    File:   src/platform/LocalSimHarness.cpp, line 73
    Caller: Test orchestrator before any send/receive.

Primary send entry point:
  LocalSimHarness::send_message(const MessageEnvelope& envelope)
    File:   src/platform/LocalSimHarness.cpp, line 104

Internal injection entry point (called from send_message()):
  LocalSimHarness::inject(const MessageEnvelope& envelope)
    File:   src/platform/LocalSimHarness.cpp, line 86
    Caller: sender.send_message() on the peer (receiver) instance.

Primary receive entry point:
  LocalSimHarness::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
    File:   src/platform/LocalSimHarness.cpp, line 142

Supporting functions:
  ImpairmentEngine::process_outbound(envelope, now_us)
    File:   src/platform/ImpairmentEngine.cpp, line 62
  ImpairmentEngine::collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)
    File:   src/platform/ImpairmentEngine.cpp, line 174
  RingBuffer::push(envelope)
    File:   src/core/RingBuffer.hpp, line 127
  RingBuffer::pop(envelope)
    File:   src/core/RingBuffer.hpp, line 159
  nanosleep(2)    — 1ms sleep inside receive_message() poll loop

--------------------------------------------------------------------------------

## 3. End-to-End Control Flow (Step-by-Step)

--- Phase 1: Setup via link() ---

Step L1 — A.link(&B) (LocalSimHarness.cpp:73)
           assert(peer != nullptr)   — must not be null
           assert(peer != this)      — must not be self-link
           A.m_peer = &B             — raw pointer assignment; no ownership transfer
           Logger::log(INFO, "LocalSimHarness", "Harness linked to peer")
           [After this, A.m_peer points to B's instance in the same process.]

--- Phase 2: send_message() on A ---

Step S1 — A.send_message(envelope) (LocalSimHarness.cpp:104)
           assert(m_open)            — A is initialized
           assert(m_peer != nullptr) — A is linked to B
           assert(envelope_valid(envelope))

Step S2 — timestamp_now_us() → now_us (LocalSimHarness.cpp:111)
           [ASSUMPTION: POSIX clock_gettime; returns uint64_t microseconds.]

Step S3 — A.m_impairment.process_outbound(envelope, now_us)
           (ImpairmentEngine.cpp:62)

           Branch A — impairments DISABLED (A.m_impairment.m_cfg.enabled == false):
             Finds first inactive A.m_impairment.m_delay_buf[i] slot.
             Copies envelope; sets release_us = now_us; active = true.
             Returns Result::OK.

           Branch B — impairments ENABLED:
             B1. is_partition_active(now_us):
                 Partition state machine — if active → log WARNING_LO → ERR_IO.
             B2. Loss check: m_prng.next_double() < loss_probability → ERR_IO.
             B3. Latency: compute release_us = now_us + fixed_latency_us + jitter_us.
             B4. Find inactive slot; copy envelope; set release_us; active=true.
             B5. Duplication: m_prng.next_double() < dup_probability → copy to
                 second slot with release_us + 100 µs.
             B6. Return Result::OK.

Step S4 — Check process_outbound result (LocalSimHarness.cpp:113)
           if (res == Result::ERR_IO) → return Result::OK (silent drop)
           [No inject() call; B never sees the message. Caller sees OK.]

Step S5 — A.m_impairment.collect_deliverable(now_us, delayed_envelopes, 32)
           (ImpairmentEngine.cpp:174)
           Scans A.m_delay_buf[0..31]; collects entries where release_us <= now_us.

           Happy path (no latency impairment, impairments disabled):
             The entry inserted in Step S3 has release_us == now_us; it IS
             immediately collectible. delayed_count = 1 (or 2 if duplicated).

           Latency impairment active:
             release_us > now_us; no entries collected. delayed_count = 0.
             The envelope will remain in A.m_delay_buf until a later call
             to collect_deliverable (next send or receive on A).

Step S6 — Inject delayed envelopes into B (LocalSimHarness.cpp:126)
           Fixed loop (Power of 10): for i in [0, delayed_count):
             assert(i < IMPAIR_DELAY_BUF_SIZE)
             (void)B.inject(delayed_envelopes[i])

           B.inject(delayed_envelopes[i]) (LocalSimHarness.cpp:86):
             assert(B.m_open)
             res = B.m_recv_queue.push(delayed_envelopes[i])
               RingBuffer::push():
                 t = m_tail.load(relaxed); h = m_head.load(acquire)
                 if (t - h >= 64): return ERR_FULL
                 envelope_copy(m_buf[t & 63], env)
                 m_tail.store(t+1, release)
                 return OK
             if (!result_ok(res)): log WARNING_HI "Receive queue full"
             assert(res == OK || res == ERR_FULL)
             return res

Step S7 — Inject primary envelope into B (LocalSimHarness.cpp:132)
           res = B.inject(envelope)
           [Identical path to Step S6 above, for the original (non-delayed)
            envelope. Note: if process_outbound buffered the message for latency,
            this inject is still called with the original envelope reference.
            This is a subtlety: with latency impairment, the message is
            injected immediately here AND stored in the delay buffer for
            later re-injection. See Section 14, RISK-1.]

           B.inject(envelope):
             assert(B.m_open)
             B.m_recv_queue.push(envelope) → envelope_copy into B.m_buf[]
             return OK (or ERR_FULL if queue saturated)

Step S8 — Post-condition and return (LocalSimHarness.cpp:134)
           assert(res == OK || res == ERR_FULL)
           return res

--- Phase 3: receive_message() on B ---

Step R1 — B.receive_message(envelope, timeout_ms) (LocalSimHarness.cpp:142)
           assert(B.m_open)

Step R2 — Fast-path pop (LocalSimHarness.cpp:147)
           res = B.m_recv_queue.pop(envelope)
           RingBuffer::pop():
             h = m_head.load(relaxed); t = m_tail.load(acquire)
             if (t - h == 0): return ERR_EMPTY
             envelope_copy(env, m_buf[h & 63])
             m_head.store(h+1, release)
             return OK

           If result_ok(res): return OK immediately.
           [This is the typical exit point when inject() was called immediately
            by send_message() and no delay is active. The message is already
            in B.m_recv_queue.]

Step R3 — Zero-timeout short-circuit (LocalSimHarness.cpp:153)
           if (timeout_ms == 0): return ERR_TIMEOUT
           [Allows non-blocking poll semantics.]

Step R4 — Compute iterations (LocalSimHarness.cpp:159)
           iterations = timeout_ms (if > 0, else 1)
           if (iterations > 5000): iterations = 5000  — cap at 5 seconds
           [Each iteration sleeps 1ms via nanosleep.]

Step R5 — Poll loop (LocalSimHarness.cpp:164)
           for (i = 0; i < iterations; i++):

  Step R5a — nanosleep(1ms) (LocalSimHarness.cpp:170)
              ts = { tv_sec=0, tv_nsec=1000000 }
              (void)nanosleep(&ts, nullptr)
              [Suspends calling thread for ~1ms. No EINTR handling:
               return value is void-cast.]

  Step R5b — m_recv_queue.pop(envelope) (LocalSimHarness.cpp:173)
              RingBuffer::pop(): acquire load of m_tail; if non-empty, copy
              and release-store m_head.
              If result_ok(res): return OK

Step R6 — Timeout (LocalSimHarness.cpp:180)
           return Result::ERR_TIMEOUT
           [Reached only if the envelope was never injected or inject failed.]

--------------------------------------------------------------------------------

## 4. Call Tree (Hierarchical)

[Setup]
A.link(&B)                                      [LocalSimHarness.cpp:73]
├── assert(peer != nullptr, peer != this)
└── m_peer = &B

[Send]
A.send_message(envelope)                        [LocalSimHarness.cpp:104]
├── assert(m_open, m_peer != nullptr, envelope_valid)
├── timestamp_now_us()                          [Timestamp.cpp — not shown]
├── A.m_impairment.process_outbound(env, now_us)
│   │                                           [ImpairmentEngine.cpp:62]
│   ├── is_partition_active(now_us)             [ImpairmentEngine.cpp:279]
│   ├── m_prng.next_double()   [loss]
│   ├── m_prng.next_range()    [jitter]
│   ├── envelope_copy() → A.m_delay_buf[slot]
│   └── m_prng.next_double()   [duplication]
├── A.m_impairment.collect_deliverable(now_us, delayed[], 32)
│   │                                           [ImpairmentEngine.cpp:174]
│   └── [loop 0..31] envelope_copy + deactivate slot
├── [loop i=0..delayed_count-1]
│   └── B.inject(delayed_envelopes[i])          [LocalSimHarness.cpp:86]
│       ├── assert(B.m_open)
│       └── B.m_recv_queue.push(delayed[i])     [RingBuffer.hpp:127]
│           ├── m_tail.load(relaxed) / m_head.load(acquire)
│           ├── envelope_copy(m_buf[t&63], env)
│           └── m_tail.store(t+1, release)
└── B.inject(envelope)                          [LocalSimHarness.cpp:86]
    ├── assert(B.m_open)
    └── B.m_recv_queue.push(envelope)           [RingBuffer.hpp:127]
        ├── m_tail.load(relaxed) / m_head.load(acquire)
        ├── envelope_copy(m_buf[t&63], env)
        └── m_tail.store(t+1, release)

[Receive]
B.receive_message(envelope, timeout_ms)         [LocalSimHarness.cpp:142]
├── assert(B.m_open)
├── B.m_recv_queue.pop(envelope)                [RingBuffer.hpp:159]  ← fast path
│   ├── m_head.load(relaxed) / m_tail.load(acquire)
│   ├── envelope_copy(env, m_buf[h&63])
│   └── m_head.store(h+1, release)
│   └── return OK                              [typical exit]
└── [if empty] poll loop up to 5000 iters
    ├── nanosleep(1ms)                          [POSIX]
    └── B.m_recv_queue.pop(envelope)            [RingBuffer.hpp:159]
        └── return OK or ERR_EMPTY → repeat

--------------------------------------------------------------------------------

## 5. Key Components Involved

Component              File(s)                         Role
─────────────────────────────────────────────────────────────────────────────
LocalSimHarness (A)    LocalSimHarness.cpp / .hpp      Sender instance: applies
                                                        impairment, holds m_peer
                                                        pointer to B, calls
                                                        B->inject().
LocalSimHarness (B)    LocalSimHarness.cpp / .hpp      Receiver instance: owns
                                                        m_recv_queue; inject()
                                                        pushes into it; receive_
                                                        message() pops from it.
ImpairmentEngine       src/platform/ImpairmentEngine.cpp
                                                        Embedded in A (sender);
                                                        decides loss/delay/dup;
                                                        stores delayed envelopes.
RingBuffer (B)         src/core/RingBuffer.hpp         SPSC lock-free FIFO owned
                                                        by B. inject() is the
                                                        producer; receive_message
                                                        is the consumer.
nanosleep(2)           POSIX / OS                       Used for the 1ms sleep
                                                        inside receive_message()
                                                        poll loop. Only external
                                                        interaction on the receive
                                                        path.
timestamp_now_us()     src/core/Timestamp.hpp           Provides wall-clock time
                                                        to process_outbound() and
                                                        collect_deliverable().
Logger                 src/core/Logger.hpp              Emits INFO on link/init;
                                                        WARNING on drop/full.
Types.hpp              src/core/Types.hpp               IMPAIR_DELAY_BUF_SIZE=32,
                                                        MSG_RING_CAPACITY=64,
                                                        Result enum.

--------------------------------------------------------------------------------

## 6. Branching Logic / Decision Points

Branch Point 1 — link() preconditions (LocalSimHarness.cpp:75)
  Condition:  peer == nullptr OR peer == this
  True path:  assert fires; program aborts (debug build) or UB (NDEBUG build).
  False path: m_peer = peer; normal return.

Branch Point 2 — Impairment drop (LocalSimHarness.cpp:113)
  Condition:  process_outbound() returns ERR_IO (loss or partition)
  True path:  Return OK silently; B.inject() never called.
  False path: Proceed to collect_deliverable and inject paths.

Branch Point 3 — Impairment disabled (ImpairmentEngine.cpp:70)
  Condition:  m_cfg.enabled == false
  True path:  Copy envelope to slot with release_us = now_us (immediate).
  False path: Apply loss → latency → duplicate chain.

Branch Point 4 — Delayed message immediately collectible (ImpairmentEngine.cpp:187)
  Condition:  m_delay_buf[i].active && m_delay_buf[i].release_us <= now_us
  True path:  Collect into out_buf; deactivate slot.
  False path: Message remains in delay buffer for future call.

Branch Point 5 — inject() queue full for delayed envelopes (LocalSimHarness.cpp:128)
  Condition:  B.m_recv_queue.push() returns ERR_FULL
  True path:  (void)-cast; delayed envelope silently dropped. No log.
  False path: B.m_recv_queue holds the delayed envelope.

Branch Point 6 — inject() queue full for primary envelope (LocalSimHarness.cpp:91)
  Condition:  B.m_recv_queue.push() returns ERR_FULL
  True path:  Log WARNING_HI "Receive queue full; dropping message";
              return ERR_FULL from inject() → returned from send_message() to caller.
  False path: Return OK.

Branch Point 7 — receive_message() fast-path hit (LocalSimHarness.cpp:148)
  Condition:  B.m_recv_queue.pop() returns OK on first try
  True path:  Return OK immediately (no nanosleep).
  False path: Proceed to zero-timeout check.

Branch Point 8 — Zero timeout (LocalSimHarness.cpp:153)
  Condition:  timeout_ms == 0
  True path:  Return ERR_TIMEOUT immediately (non-blocking semantics).
  False path: Proceed to bounded poll loop.

Branch Point 9 — Poll loop pop hit (LocalSimHarness.cpp:174)
  Condition:  B.m_recv_queue.pop() returns OK inside the nanosleep loop
  True path:  Return OK.
  False path: Continue sleeping (next iteration).

Branch Point 10 — Poll exhausted (LocalSimHarness.cpp:180)
  Condition:  i reached iterations without a successful pop
  True path:  Return ERR_TIMEOUT.

--------------------------------------------------------------------------------

## 7. Concurrency / Threading Behavior

This section is critical because LocalSimHarness is explicitly intended as a
test harness where two threads may interact.

RingBuffer SPSC contract:
  B.m_recv_queue is shared between two distinct callers:
    Producer: send_message() thread (via inject → push) — typically thread A's context.
    Consumer: receive_message() thread (via pop) — typically thread B's context.
  With exactly one producer and one consumer, the RingBuffer's acquire/release
  atomic ordering is sufficient for correctness with no mutex.
  If multiple threads call send_message() concurrently (multiple producers), or
  multiple threads call receive_message() (multiple consumers), the SPSC
  contract is violated and a data race occurs.

ImpairmentEngine (A.m_impairment):
  Owned exclusively by instance A. Only A.send_message() accesses it. If A
  is used from a single thread, there is no race on the impairment engine.

m_peer pointer (A.m_peer):
  Set once by link() before concurrent use begins. All subsequent accesses are
  read-only (A.send_message() dereferences m_peer to call inject()). Provided
  link() completes before concurrent access and close() is not called
  concurrently, m_peer is race-safe without atomics.
  [ASSUMPTION: The caller ensures link() happens-before any send_message()
   calls.]

nanosleep in receive_message():
  The calling thread is suspended for ~1ms per iteration. If send_message() is
  called from another thread while receive_message() is sleeping in nanosleep,
  the inject() → push() is non-blocking and will succeed (assuming queue not
  full). The sleeping thread will observe the new tail on its next pop()
  attempt because pop() performs an acquire load of m_tail.

close() vs. concurrent use:
  close() sets m_peer = nullptr and m_open = false. If send_message() is
  running concurrently, it will race on m_open read (line 106 assert) and
  m_peer dereference (line 132). No mutex protects close(). Callers must
  ensure close() is not called while send/receive is in progress.

--------------------------------------------------------------------------------

## 8. Memory & Ownership Semantics (C/C++ Specific)

A.m_peer (LocalSimHarness*):
  Raw non-owning pointer to B. A does NOT own B; both are typically owned by
  the test fixture. Lifetime must be managed externally: B must remain alive
  for the duration of any A.send_message() call.
  Power of 10 Rule 9 (≤1 pointer indirection): m_peer is used as
  B.inject(envelope), which is one dereference — compliant.

A.m_impairment.m_delay_buf[32] (DelayedEntry array):
  Inline member of ImpairmentEngine (which is an inline member of A).
  No heap. Holds copies of envelopes via envelope_copy(). Total size:
  32 * sizeof(DelayedEntry). [ASSUMPTION: sizeof(DelayedEntry) includes a full
  MessageEnvelope copy of ~4140 bytes + release_us + active flag; total ~133 KB.]

delayed_envelopes[32] (MessageEnvelope[], stack local in send_message()):
  Fixed compile-time size. Allocated on A's call stack frame. collect_deliverable
  copies into it; inject() copies out of it. Discarded on return.

B.m_recv_queue.m_buf[64] (MessageEnvelope array, inline in RingBuffer):
  Inline member of B. push() writes via envelope_copy; pop() reads via
  envelope_copy. Each slot is ~4140 bytes; total ~265 KB embedded in B.

envelope (const MessageEnvelope&, parameter to send_message()):
  Passed by const reference. A never stores the pointer. envelope_copy() inside
  inject() copies the fields (and inline payload) into B.m_buf[]. The original
  remains owned by the caller.

envelope (MessageEnvelope&, out-parameter to receive_message()):
  Written by RingBuffer::pop() via envelope_copy(). Caller retains ownership.

No malloc/new/delete/free anywhere in this use case. All storage is static or
stack-based. Consistent with Power of 10 Rule 3.

--------------------------------------------------------------------------------

## 9. Error Handling Flow

Error                        Source                   Handling
──────────────────────────────────────────────────────────────────────────────
Loss/partition drop          ImpairmentEngine::        ERR_IO intercepted by
                             process_outbound()         send_message(); converted
                                                        to OK (silent drop).
ERR_FULL from delay buffer   process_outbound()         Not intercepted by
                                                        send_message() (only ERR_IO
                                                        is checked). ERR_FULL falls
                                                        through; see RISK-1 in §14.
ERR_FULL from inject()       B.m_recv_queue.push()      WARNING_HI logged inside
(primary envelope)                                      inject(); ERR_FULL returned
                                                        to send_message() and then
                                                        to caller.
ERR_FULL from inject()       B.m_recv_queue.push()      (void)-cast in send_message()
(delayed envelopes)                                     loop; silent drop; no log
                                                        at the UdpBackend loop site
                                                        (but inject() logs WARNING_HI
                                                        for the primary envelope;
                                                        for delayed it is also void
                                                        cast at the inject call site
                                                        in LocalSimHarness.cpp:128).
                                                        [NOTE: Reviewing line 128:
                                                        (void)m_peer->inject(delayed)
                                                        — the inject() call itself
                                                        can log WARNING_HI if the
                                                        queue is full, but the return
                                                        value is discarded here.]
ERR_TIMEOUT                  receive_message()           Returned to caller after
                                                        all poll iterations exhaust.
link() null/self peer        link() assert              Program abort (debug).
nanosleep failure            receive_message():170       Return value void-cast;
                                                        no retry; sleep may be
                                                        shorter than requested if
                                                        EINTR; next pop attempted.

--------------------------------------------------------------------------------

## 10. External Interactions

Interaction              Details
──────────────────────────────────────────────────────────────────────────────
nanosleep(2)             The ONLY syscall on the receive path (when the queue
                         is initially empty). Called with ts={0, 1000000ns}.
                         Suspends the calling thread for ~1ms. Return value
                         cast to void; EINTR not handled explicitly.

timestamp_now_us()       [ASSUMPTION] POSIX clock_gettime(). Read-only; no
                         visible side effects on system state.

Logger::log()            [ASSUMPTION] Writes to a log sink. Called on error
                         and informational paths. Not a blocking call on the
                         critical delivery path [ASSUMPTION].

No socket syscalls (socket, bind, sendto, recvfrom, poll, close) are issued
at any point in this use case. This is the defining characteristic of the
LocalSimHarness: all communication is via C++ function calls and in-memory
data copies.

--------------------------------------------------------------------------------

## 11. State Changes / Side Effects

Object              Field                        Change
──────────────────────────────────────────────────────────────────────────────
A (sender)          m_peer                       Set to &B by link(); remains.
A.m_impairment      m_delay_buf[slot].env        Overwritten with envelope copy.
A.m_impairment      m_delay_buf[slot].active     Set to true.
A.m_impairment      m_delay_buf[slot].release_us Set to now_us (or now_us + delay).
A.m_impairment      m_delay_count                Incremented by 1 (or 2 if dup).
A.m_impairment      PRNG state                   Advanced by loss/jitter/dup draws.
A.m_impairment      m_partition_active           May change via is_partition_active.
A.m_impairment      m_delay_buf[slot].active     Set to false by collect_deliverable.
A.m_impairment      m_delay_count                Decremented by collect_deliverable.
B.m_recv_queue      m_buf[t & 63]                Written by inject()/push().
B.m_recv_queue      m_tail (atomic)              Incremented by push() (release).
B.m_recv_queue      m_head (atomic)              Incremented by pop() (release).
envelope (out)      All fields + payload         Written by receive_message pop.

NOT changed:
  B.m_impairment — receive_message() does not call process_inbound() or
  collect_deliverable() on B's impairment engine. B's engine is unused on
  the receive path.
  A.m_recv_queue — not involved in this send/receive pair.
  m_open on A or B — not modified by send or receive.
  m_fd — not applicable; LocalSimHarness has no socket.

--------------------------------------------------------------------------------

## 12. Sequence Diagram (ASCII)

  TestFixture           A (sender)           ImpairmentEngine(A)    B (receiver)         RingBuffer(B)
      |                     |                       |                    |                    |
      |--A.link(&B)-------->|                       |                    |                    |
      |                     |--m_peer=&B            |                    |                    |
      |                     |                       |                    |                    |
      |--A.send_msg(env)--->|                       |                    |                    |
      |                     |--timestamp_now_us()   |                    |                    |
      |                     |--process_outbound(env, now_us)------------>|                    |
      |                     |                       |--[loss check: pass] |                    |
      |                     |                       |--[jitter calc]      |                    |
      |                     |                       |--envelope_copy→delay_buf[0]              |
      |                     |                       |<--Result::OK        |                    |
      |                     |--collect_deliverable(now_us)-------------->|                    |
      |                     |                       |--[slot 0: release_us<=now_us → collect] |
      |                     |                       |<--delayed_count=1   |                    |
      |                     | [loop i=0..0]         |                    |                    |
      |                     |--B.inject(delayed[0])-------------------->|                    |
      |                     |                       |                    |--push(delayed[0])-->|
      |                     |                       |                    |                    |--envelope_copy
      |                     |                       |                    |                    |--m_tail++
      |                     |                       |                    |<--Result::OK--------|
      |                     |                       |                    |<--Result::OK        |
      |                     |--B.inject(envelope)----------------------->|                    |
      |                     |                       |                    |--push(envelope)---->|
      |                     |                       |                    |                    |--envelope_copy
      |                     |                       |                    |                    |--m_tail++
      |                     |                       |                    |<--Result::OK--------|
      |                     |<--Result::OK from inject()                 |                    |
      |<--Result::OK---------|                       |                    |                    |
      |                     |                       |                    |                    |
      |--B.recv_msg(out,T)->|                       |                    |                    |
      |  [called on B]      |                       |                    |--pop(out)---------->|
      |                     |                       |                    |                    |--m_tail.load(acq)
      |                     |                       |                    |                    |--envelope_copy
      |                     |                       |                    |                    |--m_head++
      |                     |                       |                    |<--Result::OK--------|
      |<--Result::OK (out populated)                |                    |                    |

  Drop path (loss impairment fires):
      |--A.send_msg(env)--->|
      |                     |--process_outbound() -> ERR_IO
      |                     |  [no inject, no push to B]
      |<--Result::OK---------|  (silent drop)

  Delayed path (fixed_latency_ms > 0, release_us > now_us):
      |--A.send_msg(env)--->|
      |                     |--process_outbound() -> OK (buffered, release_us > now_us)
      |                     |--collect_deliverable() -> delayed_count=0
      |                     |  [loop skipped]
      |                     |--B.inject(envelope) -> B.m_recv_queue now has the original
      |<--Result::OK---------|
      |  [Note: message is in B's queue immediately; the delay_buf copy
      |   will cause a DUPLICATE inject on the next send_message() call.
      |   See Section 14, RISK-1.]

--------------------------------------------------------------------------------

## 13. Initialization vs Runtime Flow

Initialization Phase:

  LocalSimHarness::LocalSimHarness() (LocalSimHarness.cpp:24):
    m_peer = nullptr
    m_open = false
    assert(!m_open)

  LocalSimHarness::init(config) (LocalSimHarness.cpp:44):
    assert(config.kind == LOCAL_SIM)
    assert(!m_open)
    m_recv_queue.init()  → m_head.store(0, relaxed), m_tail.store(0, relaxed)
    impairment_config_default(imp_cfg)
    if (config.num_channels > 0): imp_cfg.enabled = channels[0].impairments_enabled
    m_impairment.init(imp_cfg):
      m_prng.seed(seed)
      memset(m_delay_buf, 0, sizeof(m_delay_buf))
      m_delay_count = 0
      m_initialized = true
    m_open = true
    Logger::log(INFO, ...)
    assert(m_open)
    return OK

  LocalSimHarness::link(peer) (LocalSimHarness.cpp:73):
    m_peer = peer
    [Called after both instances are init()'d, before any send/receive.]

No heap allocation at any point. All storage is in-object (inline member arrays).
The RingBuffer m_buf[64] and ImpairmentEngine m_delay_buf[32] are allocated
as part of the LocalSimHarness object layout.

Runtime Phase:
  send_message() / inject() / receive_message() — no allocation, no socket I/O.
  Only in-memory copies (envelope_copy) and atomic index updates (RingBuffer).
  The single external interaction is nanosleep(1ms) in receive_message() when
  the queue is initially empty.

--------------------------------------------------------------------------------

## 14. Known Risks / Observations

RISK-1 — Double injection when latency impairment is active:
  When process_outbound() buffers a message with release_us > now_us,
  send_message() still calls B.inject(envelope) at line 132 with the
  ORIGINAL envelope reference. The message is therefore injected into B
  immediately AND a copy sits in A.m_delay_buf for later collect_deliverable().
  On the next send_message() call, collect_deliverable() will find the buffered
  copy and inject it again, producing a duplicate in B.m_recv_queue that was
  not intended by the duplication impairment.
  This is a correctness bug: the delayed copy should replace the immediate
  inject, not supplement it.

RISK-2 — B's ImpairmentEngine is never used on the receive path:
  B.m_impairment (initialized during B.init()) is never called by
  receive_message() or inject(). The inbound reordering and duplication
  suppression features of ImpairmentEngine::process_inbound() are effectively
  dead code for the LocalSimHarness receive path. Impairment is applied only
  at the sender (A) side.

RISK-3 — ERR_FULL from process_outbound not handled:
  If A.m_impairment.m_delay_buf is full (m_delay_count == 32), process_outbound
  returns ERR_FULL. The check at LocalSimHarness.cpp:113 only tests for ERR_IO.
  ERR_FULL falls through and send_message() continues to inject the envelope
  into B as if no impairment was applied, but the message was not tracked.
  This mirrors RISK-1 from UC_22.

RISK-4 — nanosleep precision and EINTR:
  nanosleep(1ms) may return early if a signal is received. The return value
  is void-cast. The next pop() will simply fail (ERR_EMPTY), and the loop
  continues sleeping. Effective sleep granularity is coarser than 1ms on
  busy systems, making the timeout behavior imprecise.

RISK-5 — Iteration cap of 5000 truncates long timeouts:
  A caller requesting timeout_ms = 10000 (10 seconds) will receive ERR_TIMEOUT
  after ~5000ms due to the cap at line 160. The truncation is silent.

RISK-6 — m_peer lifetime guarantee is caller responsibility:
  A.m_peer = &B is a raw pointer. If B is destroyed before A.send_message()
  completes, the dereference at line 128 (m_peer->inject()) is a use-after-free.
  There is no mechanism (weak pointer, reference counting) to detect this.
  This is an inherent risk of the no-STL, no-dynamic-allocation design.

RISK-7 — close() races with concurrent send/receive:
  close() sets m_peer = nullptr and m_open = false without any synchronization.
  A concurrent send_message() that has passed the m_open assert but not yet
  reached the m_peer dereference will crash if close() nullifies m_peer in
  between. Same concern for receive_message(). Test harnesses should ensure
  sequential ordering of close() with respect to in-flight calls.

RISK-8 — inject() result discarded for delayed envelopes (line 128):
  (void)m_peer->inject(delayed_envelopes[i]) — inject() can log internally
  if the queue is full, but the result is thrown away. The send_message()
  caller has no indication that some delayed messages were lost.

RISK-9 — Unidirectional linking:
  A.link(&B) means A can send to B. For bidirectional communication, B.link(&A)
  must also be called explicitly. This is not enforced or documented as a
  requirement in the API, and forgetting it will cause an assert failure
  (assert(m_peer != nullptr)) when B.send_message() is called.

--------------------------------------------------------------------------------

## 15. Unknowns / Assumptions

[ASSUMPTION-1] timestamp_now_us() returns a monotonically increasing uint64_t
  microsecond count. If it returns a wall-clock value that can go backward
  (e.g., NTP adjustment), release_us comparisons in collect_deliverable() will
  behave incorrectly.

[ASSUMPTION-2] envelope_valid() verifies basic field sanity (e.g., message_type
  != INVALID, payload_length <= MSG_MAX_PAYLOAD_BYTES). Its exact implementation
  is in MessageEnvelope.hpp (not provided).

[ASSUMPTION-3] envelope_copy() is a deep copy of all MessageEnvelope fields
  including an inline payload array of MSG_MAX_PAYLOAD_BYTES (4096 bytes).
  This is the only interpretation consistent with Power of 10 rule 3 (no
  dynamic allocation). If payload is a pointer, all copies in this use case
  would be shallow and delivery would depend on the original payload buffer
  remaining alive — a serious ownership violation.

[ASSUMPTION-4] impairment_config_default(imp_cfg) initializes imp_cfg with
  enabled=false (or all probabilities = 0.0) so that a freshly initialized
  LocalSimHarness is a transparent pass-through unless explicitly configured.
  This is inferred from the fact that tests would be broken by default
  impairment.

[ASSUMPTION-5] The test orchestrator calls A.link(&B) after both A.init() and
  B.init() have returned OK, and before any A.send_message() call. The code
  does not enforce this ordering; init() does not check m_peer, and link()
  does not check m_open.

[ASSUMPTION-6] Both LocalSimHarness instances are constructed on the same
  thread or with appropriate memory ordering before being passed between
  threads. The object construction itself (m_peer=nullptr, m_open=false)
  is not atomic.

[ASSUMPTION-7] The PRNG embedded in ImpairmentEngine (m_prng) is a
  deterministic, seedable pseudo-random number generator (e.g., LCG or
  Xorshift). Its type (SimpleRng or similar) is referenced but its
  implementation file is not included in the provided sources.

[ASSUMPTION-8] Logger::log() is either single-threaded or internally
  thread-safe. Given the no-STL rule, it likely uses a spin-lock or
  single-writer pattern. This is not visible in the provided sources.

[ASSUMPTION-9] The test scenario establishes A and B in the same OS process
  and likely the same thread for the simplest test case. The nanosleep-based
  poll loop in receive_message() suggests that multi-threaded scenarios (A's
  thread sends while B's thread waits) are also intended to work, relying on
  the SPSC RingBuffer's atomic ordering for thread safety across threads.
```

---

**Summary of what was produced:**

Three complete Flow-of-Control documents were generated, each with all 15 required sections, based entirely on actual runtime execution traced through the provided source files. Key source locations used throughout:

- `/Users/donjessup/messageEngine/src/platform/UdpBackend.cpp` — primary orchestration for UC_22 and UC_23
- `/Users/donjessup/messageEngine/src/platform/LocalSimHarness.cpp` — primary orchestration for UC_24
- `/Users/donjessup/messageEngine/src/platform/SocketUtils.cpp` — `socket_send_to()` (line 482) and `socket_recv_from()` (line 530)
- `/Users/donjessup/messageEngine/src/core/Serializer.cpp` — `serialize()` (line 115) and `deserialize()` (line 173)
- `/Users/donjessup/messageEngine/src/platform/ImpairmentEngine.cpp` — `process_outbound()` (line 62), `collect_deliverable()` (line 174)
- `/Users/donjessup/messageEngine/src/core/RingBuffer.hpp` — `push()` (line 127), `pop()` (line 159)
- `/Users/donjessup/messageEngine/src/core/Types.hpp` — all capacity constants

Notable findings flagged across the documents: a correctness bug in UC_24 where latency impairment causes double injection (RISK-1); ERR_FULL from `process_outbound()` not being caught on the send path in both UC_22 and UC_24; the delayed message send result being silently discarded; and the absence of inbound impairment processing on the receive path.agentId: acd050faad9c5dc52 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 75784
tool_uses: 9
duration_ms: 519297</usage>