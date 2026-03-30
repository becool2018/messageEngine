Use Case 30: Test Harness Runs a Full Send/Receive Round-Trip Through LocalSimHarness
======================================================================================

## 1. Use Case Overview

Actor:        test_basic_send_receive() in tests/test_LocalSim.cpp.
Trigger:      main() calls test_basic_send_receive() as the first test case (line 252).
Precondition: No real network exists. Two LocalSimHarness objects are stack-allocated.
              No threads have been started. The system is single-threaded during this test.
Postcondition: harness_a successfully delivered a "Hello" DATA envelope to harness_b.
               All envelope fields (message_id, source_id, destination_id, payload_length,
               payload bytes) compare equal between sent and received copies.
               Both harnesses are closed. Function returns true.
Scope:        In-process simulation only. No sockets, no OS network stack.

This use case traces the complete path from test setup, through the LocalSimHarness
send/receive mechanism, through the ImpairmentEngine, through the RingBuffer, and
back out to the test assertion loop. The impairment engine is initialized with
default config (enabled=false), so all messages pass through immediately.


## 2. Entry Points

Test function:
  test_basic_send_receive()                           tests/test_LocalSim.cpp, line 65.

Helper functions called from within the test:
  create_local_sim_config(cfg, node_id)               tests/test_LocalSim.cpp, line 27.
  create_test_data_envelope(env, src, dst, payload)   tests/test_LocalSim.cpp, line 38.

TransportInterface implementations:
  LocalSimHarness::init(config)                       src/platform/LocalSimHarness.cpp, line 44.
  LocalSimHarness::link(peer)                         src/platform/LocalSimHarness.cpp, line 73.
  LocalSimHarness::send_message(envelope)             src/platform/LocalSimHarness.cpp, line 104.
  LocalSimHarness::inject(envelope)                   src/platform/LocalSimHarness.cpp, line 86.
  LocalSimHarness::receive_message(envelope, timeout) src/platform/LocalSimHarness.cpp, line 142.
  LocalSimHarness::close()                            src/platform/LocalSimHarness.cpp, line 187.

Impairment engine (called from send_message):
  ImpairmentEngine::process_outbound(env, now_us)     src/platform/ImpairmentEngine.cpp, line 62.
  ImpairmentEngine::collect_deliverable(...)          src/platform/ImpairmentEngine.cpp, line 174.

Ring buffer (used by inject and receive_message):
  RingBuffer::push(env)                               src/core/RingBuffer.hpp, line 127.
  RingBuffer::pop(env)                                src/core/RingBuffer.hpp, line 159.

Platform call:
  timestamp_now_us()                                  src/core/Timestamp.hpp (not read; inferred).
  nanosleep()                                         POSIX, called from receive_message().


## 3. End-to-End Control Flow (Step-by-Step)

-- Phase 1: Object Construction --

Step 1.1  -- LocalSimHarness harness_a; is constructed on the stack. [test line 68]
             Constructor [LocalSimHarness.cpp:24]:
               m_peer = nullptr
               m_open = false
               assert(!m_open)   [line 28]

Step 1.2  -- LocalSimHarness harness_b; is constructed on the stack. [test line 69]
             Same as Step 1.1 for harness_b.

-- Phase 2: Initialize harness_a --

Step 2.1  -- create_local_sim_config(cfg_a, 1U) is called. [test line 73]
             Inside create_local_sim_config() [test line 27]:
               transport_config_default(cfg)       -- sets safe defaults
               cfg.kind = TransportKind::LOCAL_SIM
               cfg.local_node_id = 1U
               cfg.is_server = false

Step 2.2  -- harness_a.init(cfg_a) is called. [test line 74]
             Inside LocalSimHarness::init(config) [LocalSimHarness.cpp:44]:
               a. assert(config.kind == TransportKind::LOCAL_SIM)  [line 46]
               b. assert(!m_open)                                   [line 47]
               c. m_recv_queue.init()   -- calls RingBuffer::init() [line 50]
                    RingBuffer::init() [RingBuffer.hpp:76]:
                      assert(MSG_RING_CAPACITY > 0U)
                      assert((MSG_RING_CAPACITY & (MSG_RING_CAPACITY-1)) == 0)  [power-of-two check]
                      m_head.store(0U, relaxed)
                      m_tail.store(0U, relaxed)
               d. ImpairmentConfig imp_cfg declared.               [line 53]
               e. impairment_config_default(imp_cfg) called.       [line 54]
                    Sets imp_cfg.enabled = false, prng_seed = 42ULL, all others zeroed/false.
               f. if (config.num_channels > 0U): imp_cfg.enabled = config.channels[0].impairments_enabled
                  [ASSUMPTION: num_channels==0 in default config; imp_cfg.enabled stays false.]
               g. m_impairment.init(imp_cfg)                       [line 58]
                    ImpairmentEngine::init(imp_cfg):
                      asserts, m_cfg=imp_cfg, seed=42, m_prng.seed(42),
                      memset buffers, m_initialized=true.
               h. m_open = true                                    [line 60]
               i. Logger::log(INFO, "LocalSimHarness", "...initialized (node 1)")  [lines 61-63]
               j. assert(m_open)                                   [line 65]
             Returns Result::OK.

Step 2.3  -- assert(init_a == Result::OK)                          [test line 75]
Step 2.4  -- assert(harness_a.is_open() == true)                   [test line 76]
             is_open() [LocalSimHarness.cpp:198]:
               assert(m_open == true || m_open == false)
               return m_open   --> true

-- Phase 3: Initialize harness_b --

Step 3.1  -- create_local_sim_config(cfg_b, 2U) [test line 79]  -- same as Step 2.1 with node_id=2.
Step 3.2  -- harness_b.init(cfg_b) [test line 80]               -- same as Step 2.2 with node_id=2.
Step 3.3  -- assert(init_b == Result::OK)                        [test line 81]
Step 3.4  -- assert(harness_b.is_open() == true)                 [test line 82]

-- Phase 4: Link --

Step 4.1  -- harness_a.link(&harness_b) [test line 85]
             Inside LocalSimHarness::link(peer) [LocalSimHarness.cpp:73]:
               assert(peer != nullptr)   [line 75]
               assert(peer != this)      [line 76]
               m_peer = &harness_b       [line 78]
               Logger::log(INFO, "LocalSimHarness", "Harness linked to peer")
             NOTE: harness_b.m_peer is NOT set here. This is a unidirectional link.
             Messages from harness_a go to harness_b, but NOT vice versa (not needed
             for test_basic_send_receive).

-- Phase 5: Build the Envelope --

Step 5.1  -- MessageEnvelope send_env declared (stack).            [test line 88]
Step 5.2  -- create_test_data_envelope(send_env, 1U, 2U, "Hello") [test line 89]
             Inside create_test_data_envelope() [test line 38]:
               envelope_init(env)             -- zeroes/initializes envelope fields
               env.message_type = DATA
               env.message_id   = 12345ULL    -- hardcoded test ID
               env.timestamp_us = 0ULL
               env.source_id    = 1U
               env.destination_id = 2U
               env.priority     = 0U
               env.reliability_class = BEST_EFFORT
               env.expiry_time_us = 0ULL
               Payload copy loop (bounded by MSG_MAX_PAYLOAD_BYTES):
                 copies 'H','e','l','l','o' (5 bytes) into env.payload[]
               env.payload_length = 5U

-- Phase 6: Send --

Step 6.1  -- harness_a.send_message(send_env) is called.           [test line 91]
             Inside LocalSimHarness::send_message(envelope) [LocalSimHarness.cpp:104]:

  Step 6.1a -- Precondition assertions:
                 assert(m_open)                    [line 106]  -- true
                 assert(m_peer != nullptr)          [line 107]  -- &harness_b
                 assert(envelope_valid(envelope))   [line 108]

  Step 6.1b -- now_us = timestamp_now_us()          [line 111]
               [ASSUMPTION: returns current monotonic time in microseconds via clock_gettime or similar.]

  Step 6.1c -- m_impairment.process_outbound(envelope, now_us) is called. [line 112]
               Inside ImpairmentEngine::process_outbound(in_env, now_us) [ImpairmentEngine.cpp:62]:

                 Precondition assertions:
                   assert(m_initialized)          [line 66]  -- true
                   assert(envelope_valid(in_env)) [line 67]

                 Check m_cfg.enabled:              [line 70]
                   m_cfg.enabled == false   (set during init with imp_cfg.enabled=false)
                   --> TAKES THE DISABLED PATH

                 Disabled path [lines 71-90]:
                   Check m_delay_count >= IMPAIR_DELAY_BUF_SIZE: [line 71]
                     m_delay_count == 0, so NOT full.

                   Find first inactive slot loop (bounded by IMPAIR_DELAY_BUF_SIZE):
                     i=0: m_delay_buf[0].active == false (zeroed at init)
                       --> slot 0 is chosen
                       envelope_copy(m_delay_buf[0].env, in_env)   [line 79]
                       m_delay_buf[0].release_us = now_us            [line 80] -- immediate delivery
                       m_delay_buf[0].active = true                  [line 81]
                       ++m_delay_count  --> m_delay_count = 1        [line 82]
                       assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE) [line 83]
                       return Result::OK                              [line 84]

                 process_outbound() returns Result::OK.

  Step 6.1d -- Check process_outbound result: [line 113]
                 res == Result::OK (NOT ERR_IO)
                 --> Message was NOT dropped. Execution continues.

  Step 6.1e -- Collect delayed messages: [lines 119-122]
                 MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE] (stack array)
                 delayed_count = m_impairment.collect_deliverable(now_us,
                                                                  delayed_envelopes,
                                                                  IMPAIR_DELAY_BUF_SIZE)

                 Inside ImpairmentEngine::collect_deliverable(now_us, out_buf, buf_cap)
                   [ImpairmentEngine.cpp:174]:

                   Precondition assertions:
                     assert(m_initialized)    [line 179]
                     assert(out_buf != NULL)  [line 180]
                     assert(buf_cap > 0U)     [line 181]

                   Loop (bounded by IMPAIR_DELAY_BUF_SIZE): [line 186]
                     i=0: m_delay_buf[0].active == true
                          m_delay_buf[0].release_us == now_us (from Step 6.1c)
                          Condition: active && release_us <= now_us
                            release_us == now_us -> true (<=)
                          --> slot 0 is ready for delivery:
                            envelope_copy(out_buf[0], m_delay_buf[0].env) [line 189]
                            ++out_count --> out_count = 1                  [line 190]
                            m_delay_buf[0].active = false                  [line 193]
                            assert(m_delay_count > 0U)                    [line 194]
                            --m_delay_count --> m_delay_count = 0          [line 195]
                     i=1..N: m_delay_buf[i].active == false -> skip.

                   Postcondition assertions:
                     assert(out_count <= buf_cap)              [line 200]
                     assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE) [line 201]

                   Returns out_count = 1.

                 delayed_count = 1.

  Step 6.1f -- Inject delayed messages into peer (bounded loop): [lines 126-129]
                 for i=0 (< delayed_count=1):
                   assert(i < IMPAIR_DELAY_BUF_SIZE)                [line 127]
                   m_peer->inject(delayed_envelopes[0])              [line 128]
                   --> harness_b.inject(delayed_envelopes[0]) called.

                 Inside harness_b.inject(envelope) [LocalSimHarness.cpp:86]:
                   assert(m_open)                                    [line 88]
                   res = m_recv_queue.push(envelope)

                   Inside RingBuffer::push(env) [RingBuffer.hpp:127]:
                     t = m_tail.load(relaxed) --> 0
                     h = m_head.load(acquire) --> 0
                     cnt = t - h = 0
                     assert(cnt <= MSG_RING_CAPACITY)
                     cnt < MSG_RING_CAPACITY: NOT full
                     envelope_copy(m_buf[0 & RING_MASK], env)  -- copies into slot 0
                     m_tail.store(1U, release)                  -- advances tail to 1
                     assert((1 - 0) <= MSG_RING_CAPACITY)
                     Returns Result::OK.

                   result_ok(res) == true -> no warning logged.
                   assert(res == OK || res == ERR_FULL)           [line 96]
                   Returns Result::OK.

  Step 6.1g -- Inject the MAIN message into peer (the original envelope): [line 132]
                 res = m_peer->inject(envelope)
                 --> harness_b.inject(send_env) called.

                 Inside harness_b.inject(send_env) [LocalSimHarness.cpp:86]:
                   assert(m_open)
                   res = m_recv_queue.push(send_env)

                   Inside RingBuffer::push(send_env) [RingBuffer.hpp:127]:
                     t = m_tail.load(relaxed) --> 1   (tail advanced in Step 6.1f)
                     h = m_head.load(acquire) --> 0
                     cnt = 1 - 0 = 1
                     NOT full (cnt < MSG_RING_CAPACITY)
                     envelope_copy(m_buf[1 & RING_MASK], send_env)  -- slot 1
                     m_tail.store(2U, release)
                     Returns Result::OK.

                   Returns Result::OK.

  Step 6.1h -- send_message() postcondition: [line 134]
                 assert(res == OK || res == ERR_FULL)
               Returns Result::OK.

Step 6.2  -- assert(send_r == Result::OK)                          [test line 92]

-- Phase 7: Receive --

Step 7.1  -- MessageEnvelope recv_env declared (stack).            [test line 95]
Step 7.2  -- harness_b.receive_message(recv_env, 100U) called.    [test line 96]
             Inside LocalSimHarness::receive_message(envelope, timeout_ms)
               [LocalSimHarness.cpp:142]:

  Step 7.2a -- assert(m_open)                                      [line 144]

  Step 7.2b -- First attempt to pop from queue (instant path): [line 147]
                 res = m_recv_queue.pop(envelope)

                 Inside RingBuffer::pop(envelope) [RingBuffer.hpp:159]:
                   h = m_head.load(relaxed) --> 0
                   t = m_tail.load(acquire) --> 2   (tail=2 after two pushes)
                   cnt = 2 - 0 = 2
                   assert(cnt <= MSG_RING_CAPACITY)
                   cnt != 0: NOT empty
                   envelope_copy(env, m_buf[0 & RING_MASK])    -- copies slot 0
                   m_head.store(1U, release)                    -- advances head to 1
                   assert((2 - 1) < MSG_RING_CAPACITY)
                   Returns Result::OK.

  Step 7.2c -- result_ok(res) == true                              [line 148]
                 Immediate return with Result::OK.                  [line 149]
               NOTE: The delayed_envelopes[0] copy (from Step 6.1f) is returned first
               because it was pushed into slot 0 first. The original send_env is in slot 1
               and will be returned on the NEXT pop().

  NOTE ON INJECTION ORDER: Step 6.1f injected the delayed_envelopes copy first,
  then Step 6.1g injected the original send_env second. Both are identical copies
  of the envelope (same message_id, payload, etc.), so the test assertions do not
  distinguish between them. The test only calls receive_message() once and checks
  the first envelope available, which is the delayed_envelopes[0] copy.
  [OBSERVATION: This means harness_b's receive queue still holds one unretrieved copy
  of send_env (in slot 1) when the test ends.]

Step 7.3  -- assert(recv_r == Result::OK)                          [test line 97]

-- Phase 8: Verify Envelope Fields --

Step 8.1  -- assert(recv_env.message_id == send_env.message_id)          [test line 100]
             12345ULL == 12345ULL  -> pass

Step 8.2  -- assert(recv_env.source_id == send_env.source_id)             [test line 101]
             1U == 1U  -> pass

Step 8.3  -- assert(recv_env.destination_id == send_env.destination_id)   [test line 102]
             2U == 2U  -> pass

Step 8.4  -- assert(recv_env.payload_length == send_env.payload_length)   [test line 103]
             5U == 5U  -> pass

Step 8.5  -- Payload byte-by-byte verification loop (bounded by recv_env.payload_length=5):
             for i in [0, 5):
               assert(recv_env.payload[i] == send_env.payload[i])          [test line 107]
             'H'=='H', 'e'=='e', 'l'=='l', 'l'=='l', 'o'=='o'  -> all pass

-- Phase 9: Cleanup --

Step 9.1  -- harness_a.close() [test line 110]
             LocalSimHarness::close() [LocalSimHarness.cpp:187]:
               m_peer = nullptr
               m_open = false
               Logger::log(INFO, "LocalSimHarness", "Transport closed")

Step 9.2  -- harness_b.close() [test line 111]
             Same as Step 9.1 for harness_b.

Step 9.3  -- return true                                                   [test line 113]

Step 9.4  -- Destructors fire as harness_a and harness_b go out of scope.
             ~LocalSimHarness() [LocalSimHarness.cpp:35]:
               calls close() again -> close() is idempotent (m_open was already false).


## 4. Call Tree (Hierarchical)

main()
  |
  +-- test_basic_send_receive()                          [test_LocalSim.cpp:65]
        |
        +-- LocalSimHarness::LocalSimHarness()  x2       [LocalSimHarness.cpp:24]
        |
        +-- create_local_sim_config(cfg_a, 1U)           [test:27]
        |     +-- transport_config_default(cfg)
        |     +-- cfg.kind = LOCAL_SIM, local_node_id=1
        |
        +-- harness_a.init(cfg_a)                        [LocalSimHarness.cpp:44]
        |     +-- m_recv_queue.init()                    [RingBuffer.hpp:76]
        |     |     +-- m_head.store(0, relaxed)
        |     |     +-- m_tail.store(0, relaxed)
        |     |
        |     +-- impairment_config_default(imp_cfg)     [ImpairmentConfig.hpp:77]
        |     |     +-- imp_cfg.enabled = false
        |     |     +-- imp_cfg.prng_seed = 42
        |     |
        |     +-- m_impairment.init(imp_cfg)             [ImpairmentEngine.cpp:23]
        |           +-- m_prng.seed(42)                  [PrngEngine.hpp:40]
        |           +-- memset(delay_buf), memset(reorder_buf)
        |           +-- m_initialized = true
        |
        +-- create_local_sim_config(cfg_b, 2U)           [test:27]
        +-- harness_b.init(cfg_b)                        [same as harness_a.init]
        |
        +-- harness_a.link(&harness_b)                   [LocalSimHarness.cpp:73]
        |     +-- m_peer = &harness_b
        |
        +-- create_test_data_envelope(send_env, 1,2,"Hello") [test:38]
        |     +-- envelope_init(env)
        |     +-- set all fields (message_id=12345, payload="Hello", len=5)
        |
        +-- harness_a.send_message(send_env)             [LocalSimHarness.cpp:104]
        |     +-- timestamp_now_us()                     [Timestamp.hpp]
        |     |
        |     +-- m_impairment.process_outbound(env, now_us) [ImpairmentEngine.cpp:62]
        |     |     +-- [enabled=false path]
        |     |     +-- find inactive slot i=0
        |     |     +-- envelope_copy(m_delay_buf[0].env, env)
        |     |     +-- m_delay_buf[0].release_us = now_us
        |     |     +-- m_delay_buf[0].active = true
        |     |     +-- m_delay_count = 1
        |     |     +-- return OK
        |     |
        |     +-- m_impairment.collect_deliverable(now_us, delayed_envelopes, N) [ImpairmentEngine.cpp:174]
        |     |     +-- i=0: active && release_us<=now_us --> true
        |     |     |     +-- envelope_copy(out_buf[0], m_delay_buf[0].env)
        |     |     |     +-- m_delay_buf[0].active = false
        |     |     |     +-- m_delay_count = 0
        |     |     +-- return 1
        |     |
        |     +-- [inject delayed loop, delayed_count=1]
        |     |     +-- harness_b.inject(delayed_envelopes[0]) [LocalSimHarness.cpp:86]
        |     |           +-- m_recv_queue.push(env)           [RingBuffer.hpp:127]
        |     |                 +-- slot 0: envelope_copy, m_tail=1, return OK
        |     |
        |     +-- harness_b.inject(send_env)                   [LocalSimHarness.cpp:86]
        |           +-- m_recv_queue.push(send_env)            [RingBuffer.hpp:127]
        |                 +-- slot 1: envelope_copy, m_tail=2, return OK
        |
        +-- harness_b.receive_message(recv_env, 100)     [LocalSimHarness.cpp:142]
        |     +-- m_recv_queue.pop(recv_env)             [RingBuffer.hpp:159]
        |     |     +-- h=0, t=2, cnt=2 (not empty)
        |     |     +-- envelope_copy(recv_env, m_buf[0])
        |     |     +-- m_head = 1
        |     |     +-- return OK
        |     +-- result_ok(OK) -> return OK immediately
        |
        +-- [assertion loop: verify recv_env == send_env]
        |
        +-- harness_a.close()                            [LocalSimHarness.cpp:187]
        +-- harness_b.close()
        +-- return true


## 5. Key Components Involved

Component                  Type               File                                 Role
-----------                --------           ----                                 ----
test_basic_send_receive    static fn (test)   tests/test_LocalSim.cpp:65           Test orchestrator.
LocalSimHarness (x2)       class              src/platform/LocalSimHarness.cpp     In-process transport.
ImpairmentEngine           class (member)     src/platform/ImpairmentEngine.cpp    Impairment simulation.
PrngEngine                 class (member)     src/platform/PrngEngine.hpp          xorshift64 PRNG (seeded, not consumed).
RingBuffer                 class (member)     src/core/RingBuffer.hpp              Lock-free SPSC FIFO.
MessageEnvelope            struct             src/core/MessageEnvelope.hpp         Message container.
ImpairmentConfig           struct             src/platform/ImpairmentConfig.hpp    Config (enabled=false).
TransportConfig            struct             src/core/ChannelConfig.hpp [inferred] Transport config.
Logger                     static util        src/core/Logger.hpp                  INFO/WARNING logging.
timestamp_now_us()         inline/platform fn src/core/Timestamp.hpp               Monotonic time source.
nanosleep()                POSIX              <time.h>                             Sleep in poll loop (not used in this run).


## 6. Branching Logic / Decision Points

Branch 1: m_cfg.enabled == false in process_outbound() [ImpairmentEngine.cpp:70]
  - TAKEN (enabled=false).
  - Bypasses ALL impairment logic (loss, jitter, partition, duplication).
  - Message goes directly into delay buffer with release_us=now_us.
  - This is the key branch that makes the test behave deterministically.

Branch 2: m_delay_count >= IMPAIR_DELAY_BUF_SIZE in disabled path [line 71]
  - NOT TAKEN (m_delay_count == 0 at start of send).
  - Buffer not full; slot 0 is available.

Branch 3: m_delay_buf[0].active in find-slot loop [line 78]
  - i=0: active == false (zeroed at init) -> slot found immediately.
  - Loop terminates after one iteration.

Branch 4: m_delay_buf[0].active && release_us <= now_us in collect_deliverable [line 187]
  - TAKEN: active=true AND release_us==now_us (== now_us, which satisfies <=).
  - Message is collected immediately.

Branch 5: result_ok(res) in inject() [LocalSimHarness.cpp:91]
  - result_ok(OK) == true -> no warning logged.

Branch 6: cnt >= MSG_RING_CAPACITY in RingBuffer::push() [RingBuffer.hpp:135]
  - First push: cnt=0, NOT TAKEN. Succeeds.
  - Second push: cnt=1, NOT TAKEN (assuming MSG_RING_CAPACITY >= 2). Succeeds.

Branch 7: cnt == 0 in RingBuffer::pop() [RingBuffer.hpp:167]
  - NOT TAKEN: cnt=2 at time of pop. Message available immediately.

Branch 8: result_ok(res) in receive_message() first pop [LocalSimHarness.cpp:148]
  - TAKEN (pop returned OK).
  - Immediate return. The polling loop with nanosleep() is NEVER entered.

Branch 9: timeout_ms == 0 in receive_message() [LocalSimHarness.cpp:153]
  - NOT EVALUATED because Branch 8 returned first.

Branch 10: iterations > 5000U cap [LocalSimHarness.cpp:160]
  - NOT EVALUATED (Branch 8 returned before reaching the polling loop).


## 7. Concurrency / Threading Behavior

This entire test runs in a single thread. There is NO concurrent access.

RingBuffer uses std::atomic<uint32_t> with acquire/release memory ordering for its
head and tail indices. In a single-threaded context:
  - The acquire/release semantics still apply but provide no meaningful ordering
    benefit (there is only one thread to synchronize with itself).
  - The atomic operations are correct and safe in single-threaded use.

ImpairmentEngine::process_outbound() is called from the same thread that calls
inject(), which is called from the same thread that calls receive_message(). There
is zero risk of data races in this test.

LocalSimHarness::send_message() calls m_peer->inject() synchronously in the same
call frame. There is no message-passing between OS threads. The "peer" relationship
is a direct object pointer, not an inter-thread channel.

The nanosleep-based polling loop in receive_message() (lines 164-177) is not entered
in this test because the queue is already populated when receive_message() is called.
If it were entered, it would still be single-threaded blocking, not concurrent.


## 8. Memory and Ownership Semantics (C/C++ Specific)

Stack allocations (test function scope):
  - LocalSimHarness harness_a: contains RingBuffer (with MessageEnvelope[MSG_RING_CAPACITY]),
    ImpairmentEngine (with DelayEntry[N] and MessageEnvelope[N] buffers).
    Total size may be several kilobytes depending on MSG_RING_CAPACITY and IMPAIR_DELAY_BUF_SIZE.
  - LocalSimHarness harness_b: same size as harness_a.
  - MessageEnvelope send_env: stack.
  - MessageEnvelope recv_env: stack.
  - TransportConfig cfg_a, cfg_b: stack.
  - MessageEnvelope delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]: stack, inside send_message().

No heap allocation occurs anywhere in this test or its call chain (Power of 10 rule 3).

Object ownership:
  - harness_a.m_peer points to harness_b (non-owning raw pointer).
  - harness_a does NOT manage harness_b's lifetime.
  - Both harness objects are stack-allocated and destroyed at function exit.

envelope_copy() semantics:
  - Used in every push/pop and at every impairment buffer boundary.
  - [ASSUMPTION: envelope_copy() is a field-by-field copy, equivalent to memcpy of the
    struct. It produces a fully independent copy with no shared mutable state.]

RingBuffer slot lifetime:
  - m_buf[0] and m_buf[1] hold envelope copies after the two push() calls.
  - After pop() of slot 0, m_head advances to 1 but m_buf[0] data is NOT erased.
    Stale data remains in slot 0. This is safe because the SPSC protocol ensures
    the producer will not overwrite slot 0 until the consumer has fully advanced past it.

struct timespec ts in receive_message():
  - Stack-allocated inside the polling loop body.
  - NOT allocated in this test run (Branch 8 bypasses the loop).

Memory ordering summary for this test (single-threaded; all acquires observe matching releases):
  push() slot 0: m_tail.store(1, release) -- consumer's acquire sees m_buf[0] fully written.
  push() slot 1: m_tail.store(2, release)
  pop()  slot 0: m_tail.load(acquire) sees 2; m_head.store(1, release).


## 9. Error Handling Flow

In the happy path (this test), no errors occur. The following describes what would
happen if each step failed:

Failure at harness_a.init():
  - init() returns a non-OK Result.
  - assert(init_a == Result::OK) fires. Test aborts.

Failure at RingBuffer::push() (ERR_FULL):
  - inject() detects it, logs WARNING_HI, still asserts res is OK or ERR_FULL.
  - send_message() returns ERR_FULL.
  - assert(send_r == Result::OK) fires. Test aborts.

Failure at process_outbound() (ERR_IO -- loss drop):
  - send_message() returns Result::OK (loss is treated as silent drop at line 115).
  - No message reaches harness_b's queue.
  - receive_message() returns ERR_TIMEOUT after 100ms polling.
  - assert(recv_r == Result::OK) fires. Test aborts.
  - This path CANNOT fire in this test because enabled=false.

Failure at RingBuffer::pop() (ERR_EMPTY):
  - receive_message() enters the polling loop for up to 100 iterations of 1ms each.
  - After 100ms with no message, returns ERR_TIMEOUT.
  - assert(recv_r == Result::OK) fires. Test aborts.
  - This path CANNOT fire in this test because two pushes occurred before pop.

Internal assertion failures:
  - Any assert() in RingBuffer, ImpairmentEngine, or LocalSimHarness that fires will
    immediately abort the test process (in debug builds with assert() enabled).


## 10. External Interactions

timestamp_now_us():
  - Called once inside send_message() at line 111.
  - Reads the OS monotonic clock (likely clock_gettime(CLOCK_MONOTONIC, ...)).
  - Returns microseconds. Value used as release_us for immediate delivery.
  - This is the only OS call in the send path.

nanosleep():
  - Called from receive_message() polling loop [LocalSimHarness.cpp:170].
  - NOT called in this test (Branch 8 bypasses the loop).
  - If called, sleeps 1 millisecond (1,000,000 ns) per iteration.

Logger::log():
  - Called at multiple points:
    - harness_a.init(): "Local simulation harness initialized (node 1)"
    - harness_b.init(): "Local simulation harness initialized (node 2)"
    - harness_a.link(): "Harness linked to peer"
    - harness_a.close(): "Transport closed"
    - harness_b.close(): "Transport closed"
  - Logger::log() is fire-and-forget; return values not checked.

No network sockets, no file I/O, no IPC. All data movement is by in-process pointer
dereference and struct copy.


## 11. State Changes / Side Effects

harness_a state transitions:
  Before init():
    m_peer=nullptr, m_open=false, m_recv_queue (uninitialized), m_impairment (uninitialized)
  After init():
    m_peer=nullptr, m_open=true,
    m_recv_queue: head=0, tail=0 (empty)
    m_impairment: initialized, enabled=false, m_delay_count=0
  After link(&harness_b):
    m_peer = &harness_b
  After send_message() completes:
    m_impairment: m_delay_buf[0] was activated then collected; m_delay_count=0 again.
    [No changes to m_recv_queue of harness_a -- harness_a's queue is for incoming messages
     and nothing was sent TO harness_a.]
  After close():
    m_peer=nullptr, m_open=false

harness_b state transitions:
  After init():
    m_peer=nullptr, m_open=true,
    m_recv_queue: head=0, tail=0 (empty)
  After two inject() calls from send_message():
    m_recv_queue: head=0, tail=2 (two messages buffered)
    m_buf[0] = copy of delayed_envelopes[0] (identical to send_env)
    m_buf[1] = copy of send_env
  After receive_message() returns:
    m_recv_queue: head=1, tail=2 (one message still buffered -- UNRETRIEVED)
  After close():
    m_peer=nullptr, m_open=false

NOTABLE SIDE EFFECT: At the end of test_basic_send_receive(), harness_b's RingBuffer
still contains one unretrieved copy of send_env in slot 1 (tail=2, head=1). This is
not a bug -- the RingBuffer is stack-allocated and will be destroyed when harness_b
goes out of scope. The destructor calls close(), which does NOT drain the queue.


## 12. Sequence Diagram (ASCII)

  main()   test_fn      harness_a           harness_b           ImpairmentEngine_A    RingBuffer_B
    |          |              |                   |                      |                   |
    +-- call-->|              |                   |                      |                   |
    |          |-- construct->|   (harness_a ctor: m_open=false)        |                   |
    |          |-- construct---------->|   (harness_b ctor: m_open=false)|                   |
    |          |              |                   |                      |                   |
    |          |-- init(1) -->|                   |                      |                   |
    |          |              |-- recv_queue.init--|-------------------|--|-- head=0,tail=0   |
    |          |              |-- impairment.init--|----> seed(42), zero bufs                |
    |          |              |-- m_open=true      |                      |                   |
    |          |              |                   |                      |                   |
    |          |-- init(2) ---------->|           |                      |                   |
    |          |              |       |-- recv_queue.init                |                   |
    |          |              |       |-- impairment.init                |                   |
    |          |              |       |-- m_open=true                    |                   |
    |          |              |                   |                      |                   |
    |          |-- link(&B)-->|                   |                      |                   |
    |          |              |-- m_peer=&B        |                      |                   |
    |          |              |                   |                      |                   |
    |          |-- build envelope (message_id=12345, "Hello")            |                   |
    |          |              |                   |                      |                   |
    |          |-- send_msg-->|                   |                      |                   |
    |          |              |-- now_us=clock()  |                      |                   |
    |          |              |-- process_outbound(env, now_us) -------> |                   |
    |          |              |                   |                      |-- enabled=false    |
    |          |              |                   |                      |-- slot[0].active=T |
    |          |              |                   |                      |-- release=now_us   |
    |          |              |                   |                      |-- delay_count=1    |
    |          |              |                   |                      |-- return OK        |
    |          |              |-- collect_deliverable(now_us) --------> |                   |
    |          |              |                   |                      |-- slot[0] ready    |
    |          |              |                   |                      |-- copy to out[0]   |
    |          |              |                   |                      |-- slot[0] freed    |
    |          |              |                   |                      |-- return count=1   |
    |          |              |                   |                      |                   |
    |          |              |-- inject(out[0])----------->|           |                   |
    |          |              |                   |         |-- push(env)------------>|     |
    |          |              |                   |                                   |-- m_buf[0]=env
    |          |              |                   |                                   |-- tail=1
    |          |              |                   |                                   |-- OK
    |          |              |-- inject(send_env)--------->|           |                   |
    |          |              |                   |         |-- push(env)------------>|     |
    |          |              |                   |                                   |-- m_buf[1]=env
    |          |              |                   |                                   |-- tail=2
    |          |              |                   |                                   |-- OK
    |          |              |-- return OK        |                                   |
    |          |              |                   |                      |                   |
    |          |-- recv_msg(100ms) --------->|    |                      |                   |
    |          |              |             |-- pop(recv_env) --------------------------------->|
    |          |              |             |                                                  |-- h=0,t=2
    |          |              |             |                                                  |-- copy m_buf[0]
    |          |              |             |                                                  |-- head=1
    |          |              |             |                                                  |-- OK
    |          |              |             |-- result_ok(OK)->return OK immediately           |
    |          |              |                   |                      |                   |
    |          |-- verify: message_id, source_id, dest_id, payload_length, payload bytes
    |          |              |                   |                      |                   |
    |          |-- close() -->|                   |                      |                   |
    |          |-- close() ---------->|           |                      |                   |
    |          |-- return true        |           |                      |                   |
    |<-- pass--+              |       |           |                      |                   |


## 13. Initialization vs Runtime Flow

Initialization (occurs inside test_basic_send_receive() before any data flows):
  1. Two LocalSimHarness constructors set m_open=false, m_peer=nullptr.
  2. harness_a.init(cfg_a): RingBuffer reset (head=tail=0), ImpairmentEngine seeded (seed=42).
  3. harness_b.init(cfg_b): same.
  4. harness_a.link(&harness_b): m_peer=&harness_b.
  5. create_test_data_envelope() populates send_env (pure data setup, no object state change).

Runtime (data flow):
  6. send_message() -> process_outbound() -> collect_deliverable() -> inject() x2 -> push() x2.
  7. receive_message() -> pop() x1 -> immediate return.
  8. Assertion loop over received envelope fields.

Cleanup:
  9. close() x2 -> m_open=false.
  10. Destructors call close() again (idempotent).

The initialization phase allocates no memory (all static/stack). The runtime phase
does no allocation either. The total live set is two LocalSimHarness objects on the
test function's stack frame.


## 14. Known Risks / Observations

Risk 1: Double injection of the same logical envelope.
  send_message() injects BOTH a copy from collect_deliverable (delayed_envelopes[0]) AND
  the original send_env (line 132). When impairments are disabled and release_us==now_us,
  collect_deliverable() moves the buffered copy to delayed_envelopes[], then the original
  is injected again. This results in TWO identical envelopes in harness_b's queue.
  The test only receives one, leaving one unretrieved.
  This behavior may be intentional (the disabled-impairments path is a pass-through with
  an artifact of the delay-buffer design) but it is subtle and could confuse future
  test authors.

Risk 2: test_basic_send_receive() does not verify which copy was received.
  Since both injected envelopes are byte-for-byte identical, the test passes whether the
  first or second copy is returned by pop(). If envelope_copy() introduced any difference
  (e.g., timestamp mutation), only one copy would pass the assertions.

Risk 3: Unretrieved message in harness_b's queue.
  At test end, harness_b has tail=2, head=1. One envelope remains. The RingBuffer is
  destroyed with this message still in it. This is safe (no leak) but could confuse
  diagnostics if the queue size is logged before close().

Risk 4: Stack size sensitivity.
  Two LocalSimHarness objects on the stack may be large. If MSG_RING_CAPACITY is, say,
  64 and MessageEnvelope is 512 bytes, one RingBuffer is 32 KB. Two harnesses with two
  buffers (delay + reorder) plus ring buffer could be 100+ KB of stack. This could
  overflow the default stack on some embedded targets.

Risk 5: timestamp_now_us() is called once during send_message() and that same value
  is used as both the reference time for release_us (via process_outbound) and the
  comparison time for collect_deliverable(). Because the same now_us is used for both,
  release_us == now_us is guaranteed, ensuring immediate collection. If clock granularity
  or process_outbound had a side effect that changed now_us between the two calls, the
  collect_deliverable check (release_us <= now_us) might fail on a racing clock edge.
  This is not a current defect but is a fragility.

Observation: The nanosleep polling loop in receive_message() is exercised only when
  the queue is empty on first pop. In this test it is never entered. The 5000-iteration
  cap (line 160) provides the required bounded-loop guarantee (Power of 10 rule 2).

Observation: harness_b.m_peer is never set (link() is unidirectional). If the test
  called harness_b.send_message(), it would hit the assert(m_peer != nullptr) at line 107
  and abort. The test is correctly structured to avoid this.


## 15. Unknowns / Assumptions

[ASSUMPTION] impairment_config_default() sets num_channels=0 (or equivalent), so the
  block at LocalSimHarness.cpp:55 (if config.num_channels > 0U) is NOT entered and
  imp_cfg.enabled remains false. The TransportConfig struct and transport_config_default()
  were not in the read files.

[ASSUMPTION] envelope_valid(envelope) returns true for the envelope constructed by
  create_test_data_envelope(). The definition of envelope_valid() was not in the read
  files. It is inferred to check that message_type is set and payload_length is sane.

[ASSUMPTION] envelope_copy() is a shallow field-by-field copy that does not modify any
  field values. If it, for example, updated timestamp_us, the delayed_envelopes[0] copy
  and the original send_env would have different timestamps, which might affect assertions.

[ASSUMPTION] result_ok(res) returns true if and only if res == Result::OK. Its definition
  was not in the read files.

[ASSUMPTION] transport_config_default() sets config.num_channels = 0 or some value
  that causes the enabled branch at line 55 not to fire. If num_channels > 0 and
  config.channels[0].impairments_enabled were true, imp_cfg.enabled would be set true
  and impairment logic would activate, potentially dropping or delaying the test message.

[UNKNOWN] The exact value of MSG_RING_CAPACITY. The test_queue_full test fills the queue
  to MSG_RING_CAPACITY (test line 191), confirming it is a finite constant. The ring buffer
  header confirms it must be a power of two.

[UNKNOWN] The exact value of IMPAIR_DELAY_BUF_SIZE. It must be >= 1 for the delay buffer
  to hold at least one message. The code at send_message() declares a stack array of
  this size, so it cannot be excessively large without risking stack overflow.

[ASSUMPTION] timestamp_now_us() returns a monotonically increasing value in microseconds
  sourced from CLOCK_MONOTONIC or equivalent. It is called exactly once during send_message()
  and the same value is used for both process_outbound() and collect_deliverable(), which
  is what ensures the immediate-delivery behavior in the disabled-impairments path.
```

---

Summary of what was produced:

Three complete Flow-of-Control documents were generated from direct reading of all 15 source files:

**UC_28_delivery_engine_init.md** traces `DeliveryEngine::init()` calling four sub-components in sequence: `AckTracker::init()` (memset + loop verification), `RetryManager::init()` (per-slot field init + `envelope_init()`), `DuplicateFilter::init()` (memset), and `MessageIdGen::init(seed)` (counter set), with exact line numbers, assertion positions, and the seed-coercion branch.

**UC_29_prng_deterministic_seed.md** traces the full seed path from `impairment_config_default()` setting `prng_seed=42`, through `ImpairmentEngine::init()` resolving the seed (`cfg.prng_seed != 0 ? seed : 42`), to `PrngEngine::seed()` setting `m_state`. Includes the complete xorshift64 algorithm (`S ^= S<<13`, `S ^= S>>7`, `S ^= S<<17`), its mathematical property (period 2^64-1, zero is a fixed point), and how all three PRNG consumer methods (`next()`, `next_double()`, `next_range()`) derive from it.

**UC_30_local_sim_round_trip.md** traces `test_basic_send_receive()` end-to-end: two harness constructions and inits, the unidirectional `link()`, envelope construction with hardcoded `message_id=12345`, the `send_message()` path through `process_outbound()` (disabled-impairments branch), `collect_deliverable()` (immediate release since `release_us==now_us`), two `inject()`/`push()` calls, and the immediate `pop()` return in `receive_message()`. A notable observation is flagged: the disabled-impairments path causes two identical copies of the envelope to be injected into the receiver's queue, leaving one unretrieved at test end.agentId: a71d11fc0fcec11ed (for resuming to continue this agent's work if needed)
<usage>total_tokens: 80298
tool_uses: 15
duration_ms: 467872</usage>