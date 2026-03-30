# UC_32 — 100% loss configuration test

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.3, REQ-5.2.4, REQ-5.3.1, REQ-5.3.2

---

## 1. Use Case Overview

Use Case ID : UC_32
Title       : 100% Loss Configuration Test
Source File : tests/test_ImpairmentEngine.cpp, function test_loss_deterministic() (lines 81-112)
Scope       : Verifies that configuring loss_probability = 1.0 causes every call to
              process_outbound() to return ERR_IO (message dropped), and that a subsequent
              call to collect_deliverable() returns 0 messages (nothing was buffered).

Description:
  An ImpairmentEngine is initialised with enabled=true and loss_probability=1.0.
  Five MessageEnvelope objects are submitted via process_outbound(). Because the
  PRNG always produces a value in [0.0, 1.0), and the loss check is
  (rand_val < loss_probability), and 1.0 > any value in [0.0,1.0), every message is
  dropped and ERR_IO is returned. collect_deliverable() then scans the delay buffer
  (which is empty because no messages survived loss) and returns 0.

  The loss check is performed inside the private helper check_loss()
  [ImpairmentEngine.cpp:110], not inline in process_outbound(). process_outbound()
  calls check_loss() which internally calls m_prng.next_double(). The PRNG advances
  exactly once per send call (one next_double() call per check_loss() invocation).

---

## 2. Entry Points

Primary entry point (process-wide):
  main()  [test_ImpairmentEngine.cpp:464]
    -> calls test_loss_deterministic()  [line 475]

Secondary entry point (the test function itself):
  test_loss_deterministic()  [test_ImpairmentEngine.cpp:81]

Objects constructed on the stack inside test_loss_deterministic():
  - ImpairmentEngine engine   [line 83]
  - ImpairmentConfig cfg      [line 84]
  - MessageEnvelope env       [line 95]  -- inside the bounded loop
  - MessageEnvelope out_buf[10] [line 106] -- for collect_deliverable

---

## 3. End-to-End Control Flow (Step-by-Step)

--- SETUP PHASE ---

Step 1  main() calls test_loss_deterministic() [line 475]

Step 2  Stack allocation: ImpairmentEngine engine [line 83]
  ImpairmentEngine::ImpairmentEngine() constructor runs [ImpairmentEngine.cpp:26]:
    m_delay_count=0, m_reorder_count=0, m_partition_active=false, m_initialized=false
    memset(m_delay_buf, 0, ...) and memset(m_reorder_buf, 0, ...)
    m_prng.seed(1ULL)    -- preliminary seed

Step 3  Stack allocation: ImpairmentConfig cfg [line 84]

Step 4  impairment_config_default(cfg) [line 85]
  Inline function [ImpairmentConfig.hpp:79].
  All fields set to safe defaults; prng_seed = 42ULL.

Step 5  Override config fields [lines 86-88]:
  cfg.enabled          = true
  cfg.loss_probability = 1.0
  cfg.prng_seed        = 42ULL  (already 42 from default; explicit re-assignment)

Step 6  engine.init(cfg) [line 90]
  -- Enters ImpairmentEngine::init() [ImpairmentEngine.cpp:44]

  Step 6a  Precondition assertions [lines 47-48]:
    NEVER_COMPILED_OUT_ASSERT(0 <= IMPAIR_DELAY_BUF_SIZE=32)  -- passes
    NEVER_COMPILED_OUT_ASSERT(1.0 >= 0.0 && 1.0 <= 1.0)      -- passes

  Step 6b  m_cfg = cfg [line 51]
    Copies: enabled=true, loss_probability=1.0, prng_seed=42.

  Step 6c  Seed computation [lines 54-55]:
    seed = (42ULL != 0ULL) ? 42ULL : 42ULL  = 42ULL
    m_prng.seed(42ULL)
      -- PrngEngine::seed() [PrngEngine.hpp:43]:
         42ULL != 0  ->  m_state = 42ULL
         NEVER_COMPILED_OUT_ASSERT(m_state != 0)  -- passes

  Step 6d  memset(m_delay_buf, 0, ...)  -- all 32 DelayEntry slots zeroed, active=false
  Step 6e  m_delay_count = 0
  Step 6f  memset(m_reorder_buf, 0, ...)
  Step 6g  m_reorder_count = 0
  Step 6h  m_partition_active = false
           m_partition_start_us = 0
           m_next_partition_event_us = 0
  Step 6i  m_initialized = true
  Step 6j  Postcondition assertions pass.

--- MESSAGE LOOP PHASE (5 iterations, i = 0..4) ---

The loop [lines 94-103]:
  for (uint32_t i = 0U; i < 5U; ++i)

Each iteration is identical in structure.  Traced once for i=0, then summarised.

Step 7  Iteration i=0:

  Step 7a  create_test_envelope(env, 1U, 2U, 100ULL) [line 96]
    [Helper at line 29]
    envelope_init(env)             -- zeroes all fields
    env.message_type               = MessageType::DATA
    env.message_id                 = 100ULL
    env.timestamp_us               = 0ULL
    env.source_id                  = 1U
    env.destination_id             = 2U
    env.priority                   = 0U
    env.reliability_class          = ReliabilityClass::BEST_EFFORT
    env.expiry_time_us             = 0ULL
    env.payload_length             = 0U

  Step 7b  now_us = 1000000ULL + static_cast<uint64_t>(0) = 1000000ULL [line 98]

  Step 7c  engine.process_outbound(env, 1000000ULL) [line 99]
    -- Enters ImpairmentEngine::process_outbound() [ImpairmentEngine.cpp:151]

    Step 7c-i  Precondition assertions [lines 155-156]:
      NEVER_COMPILED_OUT_ASSERT(m_initialized)         -- true, passes
      NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env)) -- passes

    Step 7c-ii  Check enabled flag [line 159]:
      !m_cfg.enabled == !true == false  ->  disabled path NOT taken.
      Execution continues past the disabled block.

    Step 7c-iii  Check partition [line 169]:
      is_partition_active(1000000ULL)
      -- Enters ImpairmentEngine::is_partition_active() [ImpairmentEngine.cpp:322]

        NEVER_COMPILED_OUT_ASSERT(m_initialized) -- passes
        if (!m_cfg.partition_enabled):
          m_cfg.partition_enabled = false (default was false, never overridden)
          -> return false immediately [line 329]

      is_partition_active returns false -> partition branch NOT taken [line 172-173].

    Step 7c-iv  Loss check [lines 176-180]:
      if (check_loss()):
      -- Enters ImpairmentEngine::check_loss() [ImpairmentEngine.cpp:110]

        NEVER_COMPILED_OUT_ASSERT(m_initialized)                      -- passes [line 112]
        NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0)      -- passes [line 113]

        if (m_cfg.loss_probability <= 0.0):  [line 115]
          1.0 <= 0.0  ->  false  ->  not taken.

        double rand_val = m_prng.next_double()  [line 118]
        -- Enters PrngEngine::next_double() [PrngEngine.hpp:91]

          NEVER_COMPILED_OUT_ASSERT(m_state != 0)  m_state=42  -- passes
          raw = next()
          -- Enters PrngEngine::next() [PrngEngine.hpp:67]
            NEVER_COMPILED_OUT_ASSERT(m_state != 0)  -- passes
            m_state ^= m_state << 13U
              42 = 0x000000000000002A
              42 << 13 = 0x0000000000054000
              42 ^ 0x54000 = 0x000000000005402A
            m_state ^= m_state >> 7U
            m_state ^= m_state << 17U
              ... (result is some large non-zero uint64_t; exact value
                   requires multi-step binary arithmetic)
            NEVER_COMPILED_OUT_ASSERT(m_state != 0)  -- passes (xorshift64 invariant)
            return m_state  (some large non-zero uint64_t)

          result = static_cast<double>(raw) / static_cast<double>(UINT64_MAX)
                 = value V in [0.0, 1.0)
          NEVER_COMPILED_OUT_ASSERT(result >= 0.0 && result < 1.0)  -- passes
          return result  (V < 1.0 always)

        NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0) [line 119] -- passes

        return rand_val < m_cfg.loss_probability:
          V < 1.0  ->  true  (because V is always < 1.0 and loss_probability == 1.0)

      -- check_loss() returns true.

      Logger::log(Severity::WARNING_LO, "ImpairmentEngine",
                 "message dropped (loss probability)")  [line 178]
      return Result::ERR_IO  [line 179]

    -- Returns ERR_IO from process_outbound()

  Step 7d  assert(r == Result::ERR_IO) [line 102] -- passes.

Step 8  Iterations i=1,2,3,4:
  Identical to step 7, with:
    msg_id  = 101, 102, 103, 104 respectively (100ULL + static_cast<uint64_t>(i))
    now_us  = 1000001, 1000002, 1000003, 1000004

  Each calls m_prng.next_double() (via check_loss()) once, advancing m_state by one
  xorshift64 step.
  Each produces a value V in [0.0, 1.0), which is < 1.0, so each returns ERR_IO.
  Five Logger::log() calls are made total.

  No messages are ever written to m_delay_buf (the return happens before the buffer
  insertion code at lines 192-200).
  m_delay_count remains 0 throughout.

--- COLLECTION PHASE ---

Step 9   MessageEnvelope out_buf[10] [line 106] -- stack-allocated.

Step 10  now_us = 2000000ULL [line 107]

Step 11  engine.collect_deliverable(2000000ULL, out_buf, 10U) [line 108]
  -- Enters ImpairmentEngine::collect_deliverable() [ImpairmentEngine.cpp:216]

  Step 11a  Precondition assertions [lines 221-223]:
    NEVER_COMPILED_OUT_ASSERT(m_initialized)   -- true, passes
    NEVER_COMPILED_OUT_ASSERT(out_buf != NULL) -- stack array decays to non-null pointer, passes
    NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)    -- 10 > 0, passes

  Step 11b  out_count = 0 [line 225]

  Step 11c  Loop [lines 228-238]:
    for (i=0; i < 32 && out_count < 10; ++i)
      m_delay_buf[i].active == false for ALL i (nothing was ever buffered).
      Condition (active && release_us <= now_us) is false for all 32 slots.
      Loop body never executes.
    Loop exits after 32 iterations with out_count = 0.

  Step 11d  Postcondition assertions [lines 242-243]:
    NEVER_COMPILED_OUT_ASSERT(0 <= 10)   -- passes
    NEVER_COMPILED_OUT_ASSERT(0 <= 32)   -- passes

  Step 11e  return 0U

Step 12  assert(count == 0U) [line 109] -- passes.

Step 13  return true [line 111]

Step 14  main() prints "PASS: test_loss_deterministic\n" [line 479]

---

## 4. Call Tree (Hierarchical)

```
main()  [test_ImpairmentEngine.cpp:464]
  test_loss_deterministic()  [line 81]
    ImpairmentEngine::ImpairmentEngine()  [ImpairmentEngine.cpp:26]   -- ctor
      m_prng.seed(1ULL)  [PrngEngine.hpp:43]
      memset(m_delay_buf, ...) and memset(m_reorder_buf, ...)
    impairment_config_default(cfg)  [ImpairmentConfig.hpp:79]         -- inline
    engine.init(cfg)  [ImpairmentEngine.cpp:44]
      m_prng.seed(42ULL)  [PrngEngine.hpp:43]                         -- inline
      memset(m_delay_buf, ...) and memset(m_reorder_buf, ...)
    create_test_envelope(env, ...)  [test_ImpairmentEngine.cpp:29]    -- x5 (loop)
      envelope_init(env)
    engine.process_outbound(env, now_us)  [ImpairmentEngine.cpp:151]  -- x5 (loop)
      is_partition_active(now_us)  [ImpairmentEngine.cpp:322]
        (returns false immediately: partition_enabled=false)
      check_loss()  [ImpairmentEngine.cpp:110]                        -- x5 total
        m_prng.next_double()  [PrngEngine.hpp:91]                     -- x5 total
          m_prng.next()  [PrngEngine.hpp:67]                          -- x5 total
      Logger::log(WARNING_LO, ..., "message dropped (loss probability)") -- x5 total
      return ERR_IO                                                    -- x5 total
    engine.collect_deliverable(now_us, out_buf, 10U)  [ImpairmentEngine.cpp:216]
      (loop over 32 slots; all inactive; returns 0)
```

---

## 5. Key Components Involved

| Component | File | Role in this UC |
|---|---|---|
| `test_loss_deterministic` | `tests/test_ImpairmentEngine.cpp:81` | Drives the scenario; asserts all results |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.cpp/.hpp` | Orchestrates loss check; manages delay buffer; delegates to helpers |
| `check_loss()` | `ImpairmentEngine.cpp:110` | Private helper; queries PRNG and compares result to loss_probability |
| `PrngEngine` | `src/platform/PrngEngine.hpp` | Generates next_double() for loss check; xorshift64 algorithm |
| `ImpairmentConfig` | `src/platform/ImpairmentConfig.hpp` | Carries loss_probability=1.0 and other fields |
| `Types.hpp` | `src/core/Types.hpp` | Result::ERR_IO value 5U, IMPAIR_DELAY_BUF_SIZE=32 |
| `Logger` | `src/core/Logger.hpp` | Records WARNING_LO drop events (5 total) |
| `MessageEnvelope` | `src/core/MessageEnvelope.hpp` | Carries message data; validated by envelope_valid() |

---

## 6. Branching Logic / Decision Points

Decision 1 -- process_outbound() line 159: if (!m_cfg.enabled)
  m_cfg.enabled = true  ->  !true = false  ->  passthrough branch SKIPPED.

Decision 2 -- is_partition_active() line 328: if (!m_cfg.partition_enabled)
  partition_enabled = false  ->  !false = true  ->  return false immediately.
  Partition branch in process_outbound() line 169-173 NOT taken.

Decision 3 -- check_loss() line 115: if (m_cfg.loss_probability <= 0.0)
  1.0 <= 0.0 = false  ->  early-return false NOT taken.
  Execution proceeds to next_double().

Decision 4 -- check_loss() line 120: return rand_val < m_cfg.loss_probability
  rand_val in [0.0, 1.0)  and  loss_probability = 1.0
  rand_val < 1.0  ->  always true  ->  check_loss() returns true.
  This is the central correctness invariant of the 100% loss test.

Decision 5 -- process_outbound() line 176: if (check_loss())
  check_loss() returns true  ->  branch taken  ->  log + return ERR_IO.
  The buffer insertion code [lines 192-209] is NEVER reached in this test.

Decision 6 -- collect_deliverable() line 229: if (active && release_us <= now_us)
  active = false for all 32 slots  ->  condition always false  ->  count stays 0.

Decision 7 -- main() line 475: if (!test_loss_deterministic())
  Returns true  ->  else branch  ->  PASS printed.

---

## 7. Concurrency / Threading Behavior

Single-threaded. No locks, no atomics, no threads.

PrngEngine::next() mutates m_state on each call. In this test, next_double() is called
exactly 5 times (once per loop iteration via check_loss()). Because there is only one
thread, there is no race on m_state. The sequence of m_state values is deterministic
given seed=42:
  seed: m_state = 42
  after call 1: m_state = xorshift64(42)
  after call 2: m_state = xorshift64(xorshift64(42))
  ... etc.
This determinism is what makes the test reproducible and satisfies REQ-5.3.1.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

All allocations are automatic (stack):
  - ImpairmentEngine engine: large struct; contains m_delay_buf[32] and m_reorder_buf[32],
    each with 32 * ~4120 bytes = ~132 KB per array.
  - ImpairmentConfig cfg: ~64 bytes.
  - MessageEnvelope env: created/destroyed inside the loop [line 95].
  - MessageEnvelope out_buf[10]: stack array at line 106.

No dynamic allocation on this path. Power of 10 rule 3 is satisfied.

Copy semantics:
  - envelope_init() + field assignments create env from scratch each iteration.
  - process_outbound() takes in_env by const reference; it never copies env into any
    buffer because the function returns ERR_IO (via check_loss() returning true) before
    reaching the buffer insertion code at lines 192-200 (queue_to_delay_buf call).

m_delay_buf state after all 5 iterations: all 32 slots remain active=false, release_us=0,
  env=zeroed.  No changes since memset in init().

Logger::log() takes const char* and format arguments; no heap allocation expected
  if the implementation uses a fixed internal buffer.

---

## 9. Error Handling Flow

The error flow IS the success scenario for this test:

  process_outbound() returns ERR_IO (Result::ERR_IO = value 5U) for each of the 5
  messages.  Each ERR_IO is immediately asserted by the test at line 102.

  If assert() fires (i.e., r != ERR_IO), abort() is called and the process terminates.

  Logger::log(WARNING_LO, "ImpairmentEngine", "message dropped (loss probability)")
  is called 5 times (one per dropped message).
  These are WARNING_LO events (localized, recoverable per F-Prime model), which is
  correct: a single dropped message is localized and does not require system restart.

  collect_deliverable() returns 0 (uint32_t), which is not an error code.
  The test asserts count == 0U.

  No FATAL events are generated.  No ERR_FULL events occur (delay buffer is never
  accessed on the loss path).

---

## 10. External Interactions

| Interaction | Function | Detail |
|---|---|---|
| Logger::log() | Called 5 times | Severity WARNING_LO; tag "ImpairmentEngine"; message "message dropped (loss probability)" |
| printf() | Called in main() | Prints "PASS: test_loss_deterministic\n" after the test returns true |

All other interactions (memset, assert/NEVER_COMPILED_OUT_ASSERT) are CRT intrinsics.
No file I/O, no sockets, no network calls.

---

## 11. State Changes / Side Effects

State of engine after init():
  m_prng.m_state = 42ULL
  m_delay_buf: all inactive
  m_delay_count = 0
  m_initialized = true
  m_cfg.enabled = true, m_cfg.loss_probability = 1.0

State of engine after 5 process_outbound() calls (all ERR_IO):
  m_prng.m_state = xorshift64^5(42)  (advanced 5 steps by 5 next_double() calls via check_loss())
  m_delay_buf: unchanged (all inactive, count=0)
  m_delay_count: still 0
  is_partition_active was called 5 times but partition_enabled=false so no state change.
  check_loss() was called 5 times (each consuming one PRNG call).

State of engine after collect_deliverable():
  Unchanged (loop found nothing to deliver).

Side effects:
  Logger::log() called 5 times (WARNING_LO).
  stdout: "PASS: test_loss_deterministic\n" (from main()).

---

## 12. Sequence Diagram (ASCII)

```
  main()   test_loss_deterministic()   ImpairmentEngine   check_loss()  PrngEngine   Logger
    |               |                        |                |               |           |
    |--test_loss()-->                         |                |               |           |
    |               |                        |                |               |           |
    |         impairment_config_default(cfg)  |                |               |           |
    |               |                        |                |               |           |
    |         engine.init(cfg) ------------->|                |               |           |
    |               |                        |---seed(42) ---------------------->          |
    |               |                        |   m_state=42                  |           |
    |               |<-----------------------|                |               |           |
    |               |                        |                |               |           |
    |    [loop i=0..4]                        |                |               |           |
    |               |                        |                |               |           |
    |         create_test_envelope(env)       |                |               |           |
    |               |                        |                |               |           |
    |         process_outbound(env,now)------>|                |               |           |
    |               |                        |--is_partition--|                |           |
    |               |                        |  (ret false)   |               |           |
    |               |                        |--check_loss()-->               |           |
    |               |                        |  (loss>0.0)    |--next_double()->           |
    |               |                        |                |   next() ---->|           |
    |               |                        |                |  m_state=xs64 |           |
    |               |                        |                |<---raw--------|           |
    |               |                        |                |  /UINT64_MAX  |           |
    |               |                        |                |<-- V<1.0 -----|           |
    |               |                        |                | V < 1.0=true  |           |
    |               |                        |<--returns true--|               |           |
    |               |                        | log(WARN_LO)------------------------------>|
    |               |                        |  "dropped"     |               |           |
    |               |<----ERR_IO-------------|                |               |           |
    |         assert(r==ERR_IO)              |                |               |           |
    |    [end loop]                           |                |               |           |
    |               |                        |                |               |           |
    |         collect_deliverable(2M,buf,10)->|                |               |           |
    |               |                        | scan 32 slots  |               |           |
    |               |                        | all inactive   |               |           |
    |               |<----return 0-----------|                |               |           |
    |         assert(count==0)               |                |               |           |
    |               |                        |                |               |           |
    |<--return true--|                        |                |               |           |
    |  printf(PASS)  |                        |                |               |           |
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (steps 1-6 above):
  ImpairmentEngine constructor -> m_prng.seed(1ULL) (preliminary).
  ImpairmentEngine::init() -- seeds PRNG with 42ULL, zeroes buffers, stores config.
  This is the only allocation-equivalent phase; no further buffers are allocated.

**Runtime phase** (steps 7-12 above):
  5 calls to process_outbound() -- each: partition check (no-op), check_loss() (PRNG consumed
    once), log, return ERR_IO. The buffer insertion code is never reached.
  1 call to collect_deliverable() -- reads delay buffer (empty), returns 0.

**The loss decision path in process_outbound()**:
  is_partition_active() -> false (returns immediately, partition_enabled=false)
  check_loss() -> next_double() -> V < 1.0 -> true -> ERR_IO returned.

The delay buffer insertion code [lines 192-209] is never reached in this test.
No jitter (jitter_mean_ms == 0), no duplication (duplication_probability == 0.0).

---

## 14. Known Risks / Observations

1. **Mathematical guarantee of loss_rand < 1.0**: `next_double()` divides by UINT64_MAX.
   If raw == UINT64_MAX exactly, the result would round to 1.0 in IEEE 754 double
   arithmetic. However, raw == UINT64_MAX is the only value that could produce 1.0 after
   division. The `NEVER_COMPILED_OUT_ASSERT(result < 1.0)` in next_double()
   [PrngEngine.hpp:103] would fire and abort() would be called. xorshift64 starting from
   non-zero state can visit all 2^64 - 1 non-zero states, which includes UINT64_MAX = 2^64 - 1.
   Starting from seed=42, the exact step at which the state would equal UINT64_MAX is
   unknown without full computation, but it is extremely unlikely within 5 steps.

2. **Logger::log() blocking**: If Logger::log() blocks (e.g., on a mutex or synchronous
   write), the 5 WARNING_LO calls could introduce non-deterministic timing. Not relevant in
   unit test context, but relevant in integration testing.

3. **Envelope not validated for expiry_time_us = 0**: `envelope_valid()` is called in
   `process_outbound()` as an assertion. The test sets `expiry_time_us = 0ULL`.
   `envelope_valid()` accepts this (expiry=0 means "no expiry" in the MessageEnvelope
   convention), confirmed by the test passing.

4. **Five iterations**: The loop runs exactly 5 times. This is a correct bounded loop per
   Power of 10. However, 5 samples with seed 42 does not provide statistical confidence
   about edge cases (e.g., what if the 6th PRNG value were UINT64_MAX?). The mathematical
   argument in Decision 4 above provides the correctness argument, not the sample count.

5. **check_loss() private helper**: The loss check is delegated to the private helper
   `check_loss()` [ImpairmentEngine.cpp:110], not inline in `process_outbound()`. Any
   modification to `check_loss()` affects this test's behavior without changing the call
   site in `process_outbound()`.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `envelope_valid()` is defined in MessageEnvelope.hpp and accepts an envelope
  with expiry_time_us=0, payload_length=0, and MessageType::DATA. Confirmed indirectly by the
  test passing.
- [ASSUMPTION] `envelope_init()` zeroes all fields in the MessageEnvelope struct. Confirmed by
  name convention and test usage pattern.
- [ASSUMPTION] `Logger::log()` is a no-alloc call that does not block in unit test context.
- [ASSUMPTION] xorshift64 starting from state=42 does not produce UINT64_MAX within 5 steps.
  (Extremely unlikely; no exact computation performed here.)
- [ASSUMPTION] UINT64_MAX is defined in <cstdint> as 18446744073709551615ULL and is available
  in PrngEngine.hpp scope.
- [ASSUMPTION] The test binary is compiled with NDEBUG undefined so assert() calls in test code
  are active. NEVER_COMPILED_OUT_ASSERT() in engine code is always active regardless.
- [ASSUMPTION] check_loss() is a private helper [ImpairmentEngine.cpp:110] that encapsulates
  the PRNG query and comparison. The PRNG is advanced exactly once per process_outbound() call
  in this test configuration (only check_loss() calls next_double(); no jitter, no duplication).
