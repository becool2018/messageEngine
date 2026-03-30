## 1. Use Case Overview

Use Case ID : UC_32
Title       : 100% Loss Configuration Test
Source File : tests/test_ImpairmentEngine.cpp, function test_loss_deterministic() (lines 77-108)
Scope       : Verifies that configuring loss_probability = 1.0 causes every call to
              process_outbound() to return ERR_IO (message dropped), and that a subsequent
              call to collect_deliverable() returns 0 messages (nothing was buffered).

Description:
  An ImpairmentEngine is initialised with enabled=true and loss_probability=1.0.
  Five MessageEnvelope objects are submitted via process_outbound().  Because the
  PRNG always produces a value in [0.0, 1.0), and the loss check is
  (prng_value < loss_probability), and 1.0 > any value in [0.0,1.0), every message is
  dropped and ERR_IO is returned.  collect_deliverable() then scans the delay buffer
  (which is empty because no messages survived loss) and returns 0.


## 2. Entry Points

Primary entry point (process-wide):
  main()  [test_ImpairmentEngine.cpp:209]
    -> calls test_loss_deterministic()  [line 220]

Secondary entry point (the test function itself):
  test_loss_deterministic()  [test_ImpairmentEngine.cpp:77]

Objects constructed on the stack inside test_loss_deterministic():
  - ImpairmentEngine engine   [line 79]
  - ImpairmentConfig cfg      [line 80]
  - MessageEnvelope env       [line 91]  -- inside the bounded loop


## 3. End-to-End Control Flow (Step-by-Step)

--- SETUP PHASE ---

Step 1  main() calls test_loss_deterministic() [line 220]

Step 2  Stack allocation: ImpairmentEngine engine, ImpairmentConfig cfg [lines 79-80]

Step 3  impairment_config_default(cfg) [line 81]
  Inline function [ImpairmentConfig.hpp:77].
  All fields set to safe defaults; prng_seed = 42ULL.

Step 4  Override config fields [lines 82-84]:
  cfg.enabled          = true
  cfg.loss_probability = 1.0
  cfg.prng_seed        = 42ULL  (already 42 from default; explicit re-assignment)

Step 5  engine.init(cfg) [line 86]
  -- Enters ImpairmentEngine::init() [ImpairmentEngine.cpp:23]

  Step 5a  Precondition assertions [lines 26-27]:
    assert(0 <= IMPAIR_DELAY_BUF_SIZE=32)  -- passes
    assert(1.0 >= 0.0 && 1.0 <= 1.0)      -- passes

  Step 5b  m_cfg = cfg [line 30]
    Copies: enabled=true, loss_probability=1.0, prng_seed=42.

  Step 5c  Seed computation [lines 33-34]:
    seed = (42ULL != 0ULL) ? 42ULL : 42ULL  = 42ULL
    m_prng.seed(42ULL)
      -- PrngEngine::seed() [PrngEngine.hpp:40]:
         42ULL != 0  ->  m_state = 42ULL
         assert(m_state != 0)  -- passes

  Step 5d  memset(m_delay_buf, 0, ...)  -- all 32 DelayEntry slots zeroed, active=false
  Step 5e  m_delay_count = 0
  Step 5f  memset(m_reorder_buf, 0, ...)
  Step 5g  m_reorder_count = 0
  Step 5h  m_partition_active = false
           m_partition_start_us = 0
           m_next_partition_event_us = 0
  Step 5i  m_initialized = true
  Step 5j  Postcondition assertions pass.

--- MESSAGE LOOP PHASE (5 iterations, i = 0..4) ---

The loop [lines 90-98]:
  for (uint32_t i = 0U; i < 5U; ++i)

Each iteration is identical in structure.  Traced once for i=0, then summarised.

Step 6  Iteration i=0:

  Step 6a  create_test_envelope(env, 1U, 2U, 100ULL) [line 92]
    [Helper at line 27]
    envelope_init(env)             -- zeroes all fields [ASSUMPTION: defined in MessageEnvelope.hpp]
    env.message_type               = MessageType::DATA
    env.message_id                 = 100ULL
    env.timestamp_us               = 0
    env.source_id                  = 1U
    env.destination_id             = 2U
    env.priority                   = 0
    env.reliability_class          = ReliabilityClass::BEST_EFFORT
    env.expiry_time_us             = 0
    env.payload_length             = 0

  Step 6b  now_us = 1000000ULL + 0 = 1000000ULL [line 94]

  Step 6c  engine.process_outbound(env, 1000000ULL) [line 95]
    -- Enters ImpairmentEngine::process_outbound() [ImpairmentEngine.cpp:62]

    Step 6c-i  Precondition assertions [lines 66-67]:
      assert(m_initialized)         -- true, passes
      assert(envelope_valid(in_env)) -- [ASSUMPTION: checks message_type != INVALID etc.]

    Step 6c-ii  Check enabled flag [line 70]:
      !m_cfg.enabled == !true == false  ->  skip passthrough branch.
      NOTE: The passthrough branch (lines 71-90) is NOT entered.

    Step 6c-iii  Check partition [line 93]:
      is_partition_active(1000000ULL)
      -- Enters ImpairmentEngine::is_partition_active() [ImpairmentEngine.cpp:279]

        assert(m_initialized) -- passes
        if (!m_cfg.partition_enabled) -> !false... wait:
          m_cfg.partition_enabled = false (default was false, never overridden)
          -> return false immediately [line 286]

      is_partition_active returns false -> partition branch NOT taken.

    Step 6c-iv  Loss check [lines 100-108]:
      if (m_cfg.loss_probability > 0.0):
        1.0 > 0.0  ->  true  ->  enter branch.

      loss_rand = m_prng.next_double()
      -- Enters PrngEngine::next_double() [PrngEngine.hpp:86]

        assert(m_state != 0)  m_state=42  -- passes
        raw = next()
        -- Enters PrngEngine::next() [PrngEngine.hpp:63]
          assert(m_state != 0)  -- passes
          m_state ^= m_state << 13U
            42 = 0x000000000000002A
            42 << 13 = 0x0000000000054000
            42 ^ 0x54000 = 0x000000000005402A
          m_state ^= m_state >> 7U
            0x5402A >> 7 = 0x0000000000000A80
            0x5402A ^ 0xA80 = 0x000000000005EAA  [ASSUMPTION: exact bits; key point is
            the result is non-zero and < UINT64_MAX, producing next_double() < 1.0]
          m_state ^= m_state << 17U
            ... (result is non-zero by xorshift64 property)
          assert(m_state != 0)  -- passes
          return m_state  (some large non-zero uint64_t)

        result = static_cast<double>(raw) / static_cast<double>(UINT64_MAX)
               = value in [0.0, 1.0)
               NOTE: UINT64_MAX = 18446744073709551615.
               result is strictly < 1.0 because raw <= UINT64_MAX - 1 after xorshift
               [ASSUMPTION: xorshift64 never produces UINT64_MAX; it never produces 0
               because starting from non-zero the algorithm keeps state non-zero].
        assert(result >= 0.0 && result < 1.0)  -- passes
        return result  (call this value V; 0.0 <= V < 1.0)

      assert(loss_rand >= 0.0 && loss_rand < 1.0) [line 102] -- passes

      if (loss_rand < m_cfg.loss_probability):
        V < 1.0  ->  true  (because V is always < 1.0 and loss_probability == 1.0)

      Logger::log(WARNING_LO, "ImpairmentEngine", "message dropped (loss probability)")
      return Result::ERR_IO [line 106]

    -- Returns ERR_IO from process_outbound()

  Step 6d  assert(r == Result::ERR_IO) [line 98] -- passes.

Step 7  Iterations i=1,2,3,4:
  Identical to step 6, with:
    msg_id  = 101, 102, 103, 104 respectively
    now_us  = 1000001, 1000002, 1000003, 1000004

  Each calls m_prng.next_double() once, advancing m_state by one xorshift64 step.
  Each produces a value in [0.0, 1.0), which is < 1.0, so each returns ERR_IO.
  Five Logger::log() calls are made total.

  No messages are ever written to m_delay_buf (the return happens before the buffer
  insertion code at lines 125-141).
  m_delay_count remains 0 throughout.

--- COLLECTION PHASE ---

Step 8  MessageEnvelope out_buf[10] [line 102] -- stack-allocated, uninitialised.

Step 9  now_us = 2000000ULL [line 103]

Step 10  engine.collect_deliverable(2000000ULL, out_buf, 10U) [line 104]
  -- Enters ImpairmentEngine::collect_deliverable() [ImpairmentEngine.cpp:174]

  Step 10a  Precondition assertions [lines 179-181]:
    assert(m_initialized)   -- true, passes
    assert(out_buf != NULL) -- stack array decays to non-null pointer, passes
    assert(buf_cap > 0U)    -- 10 > 0, passes

  Step 10b  out_count = 0 [line 183]

  Step 10c  Loop [lines 186-197]:
    for (i=0; i < 32 && out_count < 10; ++i)
      m_delay_buf[i].active == false for ALL i (nothing was ever buffered).
      Condition (active && release_us <= now_us) is false for all 32 slots.
      Loop body never executes.
    Loop exits after 32 iterations with out_count = 0.

  Step 10d  Postcondition assertions [lines 200-201]:
    assert(0 <= 10)   -- passes
    assert(0 <= 32)   -- passes

  Step 10e  return 0U

Step 11  assert(count == 0U) [line 105] -- passes.

Step 12  return true [line 107]

Step 13  main() prints "PASS: test_loss_deterministic\n"


## 4. Call Tree (Hierarchical)

main()  [test_ImpairmentEngine.cpp:209]
  test_loss_deterministic()  [line 77]
    impairment_config_default(cfg)  [ImpairmentConfig.hpp:77]       -- inline
    engine.init(cfg)  [ImpairmentEngine.cpp:23]
      m_prng.seed(42ULL)  [PrngEngine.hpp:40]                       -- inline
      memset(m_delay_buf, ...)                                       -- CRT x2
    create_test_envelope(env, ...)  [test_ImpairmentEngine.cpp:27]  -- x5 (loop)
      envelope_init(env)                                             -- (MessageEnvelope.hpp)
    engine.process_outbound(env, now_us)  [ImpairmentEngine.cpp:62] -- x5 (loop)
      envelope_valid(in_env)                                         -- assert
      is_partition_active(now_us)  [ImpairmentEngine.cpp:279]
        (returns false immediately: partition_enabled=false)
      m_prng.next_double()  [PrngEngine.hpp:86]                     -- x5 total
        m_prng.next()  [PrngEngine.hpp:63]                          -- x5 total
      Logger::log(WARNING_LO, ...)                                   -- x5 total
      return ERR_IO                                                  -- x5 total
    engine.collect_deliverable(now_us, out_buf, 10U)  [ImpairmentEngine.cpp:174]
      (loop over 32 slots; all inactive; returns 0)


## 5. Key Components Involved

Component              | File                               | Role in this UC
-----------------------|------------------------------------|-------------------------------
test_loss_deterministic| test_ImpairmentEngine.cpp:77       | Drives the scenario; asserts
ImpairmentEngine       | src/platform/ImpairmentEngine.cpp  | Applies loss; manages delay buf
PrngEngine             | src/platform/PrngEngine.hpp        | Generates next_double() for loss
ImpairmentConfig       | src/platform/ImpairmentConfig.hpp  | Carries loss_probability=1.0
Types.hpp              | src/core/Types.hpp                 | Result::ERR_IO value 5,
                       |                                    | IMPAIR_DELAY_BUF_SIZE=32
Logger                 | src/core/Logger.hpp [ASSUMPTION]   | Records WARNING_LO drop events
MessageEnvelope        | src/core/MessageEnvelope.hpp       | Carries message data


## 6. Branching Logic / Decision Points

Decision 1 -- process_outbound() line 70: if (!m_cfg.enabled)
  m_cfg.enabled = true  ->  !true = false  ->  passthrough branch SKIPPED.

Decision 2 -- is_partition_active() line 285: if (!m_cfg.partition_enabled)
  partition_enabled = false  ->  !false = true  ->  return false immediately.
  Partition branch in process_outbound() line 93 NOT taken.

Decision 3 -- process_outbound() line 100: if (m_cfg.loss_probability > 0.0)
  1.0 > 0.0 = true  ->  enter loss block.

Decision 4 -- process_outbound() line 103: if (loss_rand < m_cfg.loss_probability)
  loss_rand in [0.0, 1.0)  and  loss_probability = 1.0
  loss_rand < 1.0  ->  always true  ->  drop path ALWAYS taken.
  This is the central correctness invariant.

Decision 5 -- collect_deliverable() line 187: if (active && release_us <= now_us)
  active = false for all 32 slots  ->  condition always false  ->  count stays 0.

Decision 6 -- main() line 220: if (!test_loss_deterministic())
  Returns true  ->  else branch  ->  PASS printed.


## 7. Concurrency / Threading Behavior

Single-threaded.  No locks, no atomics, no threads.

PrngEngine::next() mutates m_state on each call.  In this test, next_double() is called
exactly 5 times (once per loop iteration).  Because there is only one thread, there is
no race on m_state.  The sequence of m_state values is deterministic given seed=42:
  seed: m_state = 42
  after call 1: m_state = xorshift64(42)
  after call 2: m_state = xorshift64(xorshift64(42))
  ... etc.
This determinism is what makes the test reproducible.


## 8. Memory & Ownership Semantics (C/C++ Specific)

All allocations are automatic (stack):
  - ImpairmentEngine engine: large struct; contains m_delay_buf[32] and m_reorder_buf[32].
  - ImpairmentConfig cfg: ~88 bytes.
  - MessageEnvelope env: created/destroyed inside the loop [line 91].
  - MessageEnvelope out_buf[10]: stack array at line 102.

No dynamic allocation on this path.  Power of 10 rule 3 is satisfied.

Copy semantics:
  - envelope_init() + field assignments create env from scratch each iteration.
  - process_outbound() takes in_env by const reference; it never copies env into any
    buffer because the function returns ERR_IO before reaching the buffer insertion code
    at line 134 (envelope_copy into m_delay_buf).

m_delay_buf state after all 5 iterations: all 32 slots remain active=false, release_us=0,
  env=zeroed.  No changes since memset in init().

Logger::log() [ASSUMPTION] takes const char* arguments; no heap allocation expected
  if the implementation uses a fixed internal buffer.


## 9. Error Handling Flow

The error flow IS the success scenario for this test:

  process_outbound() returns ERR_IO (Result::ERR_IO = value 5U) for each of the 5
  messages.  Each ERR_IO is immediately asserted by the test at line 98.

  If assert() fires (i.e., r != ERR_IO), abort() is called and the process terminates.

  Logger::log(WARNING_LO, ...) is called 5 times with the message:
    "message dropped (loss probability)"
  These are WARNING_LO events (localized, recoverable per F-Prime model), which is
  correct: a single dropped message is localized and does not require system restart.

  collect_deliverable() returns 0 (uint32_t), which is not an error code.
  The test asserts count == 0U.

  No FATAL events are generated.  No ERR_FULL events occur (delay buffer is never
  accessed on the loss path).


## 10. External Interactions

Logger::log() -- called 5 times.
  Severity: WARNING_LO
  Tag: "ImpairmentEngine"
  Message: "message dropped (loss probability)"
  [ASSUMPTION] Logger writes to a static internal buffer or stdout/stderr.  No dynamic
  allocation.  No file I/O or sockets.

printf() -- called in main() after the test returns, not inside the test function.

All other interactions (memset, assert) are CRT intrinsics, not external I/O.


## 11. State Changes / Side Effects

State of engine after init():
  m_prng.m_state = 42ULL
  m_delay_buf: all inactive
  m_delay_count = 0
  m_initialized = true

State of engine after 5 process_outbound() calls (all ERR_IO):
  m_prng.m_state = xorshift64^5(42)  (advanced 5 steps by 5 next_double() calls)
  m_delay_buf: unchanged (all inactive, count=0)
  m_delay_count: still 0
  is_partition_active was called 5 times but partition_enabled=false so no state change

State of engine after collect_deliverable():
  Unchanged (loop found nothing to deliver).

Side effects:
  Logger::log() called 5 times (WARNING_LO).
  stdout: "PASS: test_loss_deterministic\n" (from main()).


## 12. Sequence Diagram (ASCII)

  main()   test_loss_deterministic()   ImpairmentEngine   PrngEngine   Logger
    |               |                        |                |           |
    |--test_loss()-->                         |                |           |
    |               |                        |                |           |
    |         impairment_config_default(cfg)  |                |           |
    |               |                        |                |           |
    |         engine.init(cfg) ------------->|                |           |
    |               |                        |---seed(42) --->|           |
    |               |                        |   m_state=42   |           |
    |               |                        |<---------------|           |
    |               |<-----------------------|                |           |
    |               |                        |                |           |
    |    [loop i=0..4]                        |                |           |
    |               |                        |                |           |
    |         create_test_envelope(env)       |                |           |
    |               |                        |                |           |
    |         process_outbound(env,now)------>|                |           |
    |               |                        |--is_partition--|           |
    |               |                        |  (ret false)   |           |
    |               |                        |--next_double()->|           |
    |               |                        |     next() --->|           |
    |               |                        |  m_state=xs64  |           |
    |               |                        |<---raw---------|           |
    |               |                        |  /UINT64_MAX   |           |
    |               |                        |<-- V<1.0 ------|           |
    |               |                        |  V < 1.0=true  |           |
    |               |                        |--log(WARN_LO)------------>|
    |               |                        |  "dropped"     |           |
    |               |<----ERR_IO-------------|                |           |
    |         assert(r==ERR_IO)              |                |           |
    |    [end loop]                           |                |           |
    |               |                        |                |           |
    |         collect_deliverable(2M,buf,10)->|                |           |
    |               |                        | scan 32 slots  |           |
    |               |                        | all inactive   |           |
    |               |<----return 0-----------|                |           |
    |         assert(count==0)               |                |           |
    |               |                        |                |           |
    |<--return true--|                        |                |           |
    |  printf(PASS)  |                        |                |           |


## 13. Initialization vs Runtime Flow

Initialization phase (steps 1-5 above):
  ImpairmentEngine::init() -- seeds PRNG, zeroes buffers, stores config.
  This is the only allocation-equivalent phase; no further buffers are allocated.

Runtime phase (steps 6-11 above):
  5 calls to process_outbound() -- PRNG consumed once per call.
  1 call to collect_deliverable() -- reads delay buffer (empty), returns 0.

The loss decision in process_outbound() is the core runtime behavior:
  next_double() -> compare against loss_probability -> ERR_IO.
  No latency, jitter, duplication, or reordering logic is reached because
  ERR_IO is returned before any of those code sections.

The delay buffer insertion code [lines 125-163] is never reached in this test.


## 14. Known Risks / Observations

Risk 1 -- Mathematical guarantee of loss_rand < 1.0:
  next_double() divides by UINT64_MAX.  If raw == UINT64_MAX exactly, result would
  round to 1.0 in IEEE 754 double arithmetic.  However, xorshift64 with a non-zero
  seed never produces 0 (by the algorithm's invariant), and UINT64_MAX = 2^64 - 1.
  The algorithm can produce UINT64_MAX.  If raw == UINT64_MAX:
    static_cast<double>(UINT64_MAX) / static_cast<double>(UINT64_MAX) == 1.0
  This would make next_double() return exactly 1.0, violating the postcondition
  assert(result < 1.0) and causing abort().
  [OBSERVATION] The implementation has this edge-case vulnerability.  In practice,
  xorshift64(42) over 5 steps is unlikely to hit UINT64_MAX, so the test passes, but
  this is a latent defect if a seed or sequence step reaches that state.

Risk 2 -- Logger::log() blocking:
  If Logger::log() blocks (e.g., on a mutex or synchronous write), the 5 WARNING_LO
  calls could introduce non-deterministic timing.  Not relevant in unit test context,
  but relevant in integration.

Risk 3 -- Envelope not validated:
  envelope_valid() is called in process_outbound() as an assertion.  If it uses
  strict validation (e.g., expiry_time_us > 0), an envelope with expiry=0 might
  fail.  The test sets expiry_time_us = 0 [line 41].  [ASSUMPTION] envelope_valid()
  permits expiry=0 to mean "no expiry" or the assertion is lenient enough to pass.

Risk 4 -- Five iterations:
  The loop runs exactly 5 times.  This is a correct bounded loop per Power of 10.
  However, 5 samples with seed 42 does not provide statistical confidence that 1.0
  loss is correct; it only exercises the code path.  The mathematical argument in
  Decision 4 above provides the correctness argument, not the sample count.


## 15. Unknowns / Assumptions

[ASSUMPTION 1] envelope_valid() is defined in MessageEnvelope.hpp and accepts an
  envelope with expiry_time_us=0, payload_length=0, and MESSAGE_TYPE::DATA.

[ASSUMPTION 2] envelope_init() zeroes all fields in the MessageEnvelope struct.

[ASSUMPTION 3] Logger::log() is a no-alloc, thread-safe call that does not block.

[ASSUMPTION 4] xorshift64 starting from state=42 does not produce UINT64_MAX within
  5 steps.  (Verified to be extremely unlikely; no exact computation performed here.)

[ASSUMPTION 5] UINT64_MAX is defined in <cstdint> as 18446744073709551615ULL and is
  available in PrngEngine.hpp scope.

[ASSUMPTION 6] The test binary is compiled with NDEBUG undefined so all assert()
  calls are active.

[ASSUMPTION 7] Logger::log() accepts a format-string-style variadic interface similar
  to printf.  [Observed from call at ImpairmentEngine.cpp:305 with a %u format arg;
  the loss-drop call has no format args -- consistent with a simple string overload.]
```

---