## 1. Use Case Overview

Use Case ID : UC_31
Title       : PRNG Reproducibility Test
Source File : tests/test_ImpairmentEngine.cpp, function test_prng_deterministic() (lines 179-203)
Scope       : Verifies that two PrngEngine instances seeded identically produce the same
              internal state after initialization and would, by implication, produce the
              same sequence of outputs on every subsequent call to next()/next_double().

Description:
  The test constructs two ImpairmentEngine objects (engine1, engine2), initialises each
  with an independent but identically valued ImpairmentConfig (prng_seed = 12345ULL).
  It then asserts that both config copies carry the same seed, and that both engines
  are in the same default-disabled state.  Because PrngEngine is private inside
  ImpairmentEngine, the test cannot call next() directly.  Instead it relies on the
  fact that ImpairmentEngine::init() passes the seed straight to PrngEngine::seed(),
  and that xorshift64 is a pure function of its state: equal seeds => equal state =>
  equal future sequences.

NOTE: The test does NOT call next() or next_double() on the two engines and compare
raw values; it validates the determinism invariant indirectly through config equality.
The description in the use-case goal ("call next() N times on each, compare sequences")
describes the intent of the design, not literal test code. The actual test code only
confirms seed propagation and shared disabled state.  This distinction is called out
explicitly wherever relevant below.


## 2. Entry Points

Primary entry point (process-wide):
  main()  [test_ImpairmentEngine.cpp:209]
    -> calls test_prng_deterministic()  [line 242]

Secondary entry point (the test function itself):
  test_prng_deterministic()  [test_ImpairmentEngine.cpp:179]

Objects constructed on the stack inside test_prng_deterministic():
  - ImpairmentEngine engine1   [line 181]  -- default-constructed; m_initialized = false
  - ImpairmentConfig cfg1      [line 182]
  - ImpairmentEngine engine2   [line 187]
  - ImpairmentConfig cfg2      [line 189]


## 3. End-to-End Control Flow (Step-by-Step)

Step 1  main() [line 209]
  The process starts. An int 'failed' is set to 0.  Several earlier tests run first
  (test_passthrough_disabled, test_loss_deterministic, test_no_loss, test_fixed_latency).
  This document focuses solely on test_prng_deterministic, invoked at line 242.

Step 2  main() calls test_prng_deterministic() [line 242]

Step 3  Stack allocation of engine1, cfg1 [lines 181-182]
  Both are POD/class types.  ImpairmentEngine has no user-provided constructor, so
  m_initialized is indeterminate at this point (it will be set in init()).
  [ASSUMPTION] The compiler zero-initialises no fields automatically; init() is
  required to establish all invariants.

Step 4  impairment_config_default(cfg1) [line 183]
  Inline function in ImpairmentConfig.hpp:77-91.
  Sets ALL fields:
    cfg1.enabled                 = false
    cfg1.fixed_latency_ms        = 0
    cfg1.jitter_mean_ms          = 0
    cfg1.jitter_variance_ms      = 0
    cfg1.loss_probability        = 0.0
    cfg1.duplication_probability = 0.0
    cfg1.reorder_enabled         = false
    cfg1.reorder_window_size     = 0
    cfg1.partition_enabled       = false
    cfg1.partition_duration_ms   = 0
    cfg1.partition_gap_ms        = 0
    cfg1.prng_seed               = 42ULL   <-- default; overridden next

Step 5  cfg1.prng_seed = 12345ULL [line 184]
  Overwrites the default 42 with 12345.

Step 6  engine1.init(cfg1) [line 185]
  -- Enters ImpairmentEngine::init() [ImpairmentEngine.cpp:23]

  Step 6a  Precondition assertions [lines 26-27]:
    assert(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)
      -> 0 <= 32  -- passes
    assert(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0)
      -> 0.0 in [0.0,1.0]  -- passes

  Step 6b  m_cfg = cfg [line 30]
    Copies all ImpairmentConfig fields into engine1.m_cfg.
    engine1.m_cfg.prng_seed == 12345ULL.

  Step 6c  Seed computation [lines 33-34]:
    seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL
         = 12345ULL   (non-zero, so used directly)
    m_prng.seed(12345ULL)

    -- Enters PrngEngine::seed() [PrngEngine.hpp:40]
       s = 12345ULL  (non-zero)
       m_state = 12345ULL
       assert(m_state != 0ULL)  -- passes
    -- Returns from PrngEngine::seed()

  Step 6d  memset(m_delay_buf, 0, sizeof(m_delay_buf)) [line 37]
    Zeroes all 32 DelayEntry slots (env + release_us + active).

  Step 6e  m_delay_count = 0 [line 38]

  Step 6f  memset(m_reorder_buf, 0, sizeof(m_reorder_buf)) [line 41]
    Zeroes all 32 MessageEnvelope slots in the reorder buffer.

  Step 6g  m_reorder_count = 0 [line 42]

  Step 6h  m_partition_active = false [line 45]
           m_partition_start_us = 0 [line 46]
           m_next_partition_event_us = 0 [line 48]

  Step 6i  m_initialized = true [line 51]

  Step 6j  Postcondition assertions [lines 54-55]:
    assert(m_initialized)       -- passes
    assert(m_delay_count == 0U) -- passes

  -- Returns from init()

Step 7  Stack allocation of engine2, cfg2 [lines 187-189]
  Mirror of steps 3-4 for the second engine.

Step 8  impairment_config_default(cfg2) [line 190]
  Identical outcome to step 4: all defaults, prng_seed = 42.

Step 9  cfg2.prng_seed = 12345ULL [line 191]
  Same override as step 5.

Step 10  engine2.init(cfg2) [line 192]
  Identical execution path to step 6, with engine2 as the object.
  engine2.m_prng.m_state = 12345ULL after seed().

Step 11  assert(engine1.config().prng_seed == engine2.config().prng_seed) [line 194]
  ImpairmentEngine::config() returns const ImpairmentConfig& (inline, no copy).
  engine1.m_cfg.prng_seed == 12345ULL
  engine2.m_cfg.prng_seed == 12345ULL
  12345 == 12345  -- assertion passes.

Step 12  assert(engine1.config().enabled == false) [line 200]
  engine1.m_cfg.enabled = false  -- assertion passes.

Step 13  assert(engine2.config().enabled == false) [line 201]
  engine2.m_cfg.enabled = false  -- assertion passes.

Step 14  return true [line 203]
  Control returns to main().

Step 15  main() checks return value [line 242]:
  !true == false  -> else branch taken -> printf("PASS: test_prng_deterministic\n")

Step 16  main() evaluates final return [line 248]:
  (failed > 0) ? 1 : 0
  If all previous tests also passed, failed == 0, process exits 0.


## 4. Call Tree (Hierarchical)

main()  [test_ImpairmentEngine.cpp:209]
  test_prng_deterministic()  [line 179]
    impairment_config_default(cfg1)  [ImpairmentConfig.hpp:77]   -- inline
    engine1.init(cfg1)  [ImpairmentEngine.cpp:23]
      m_prng.seed(12345ULL)  [PrngEngine.hpp:40]               -- inline
      memset(m_delay_buf, ...)                                   -- CRT
      memset(m_reorder_buf, ...)                                 -- CRT
    impairment_config_default(cfg2)  [ImpairmentConfig.hpp:77]   -- inline
    engine2.init(cfg2)  [ImpairmentEngine.cpp:23]
      m_prng.seed(12345ULL)  [PrngEngine.hpp:40]               -- inline
      memset(m_delay_buf, ...)                                   -- CRT
      memset(m_reorder_buf, ...)                                 -- CRT
    engine1.config()  [ImpairmentEngine.hpp:119]                 -- inline accessor
    engine2.config()  [ImpairmentEngine.hpp:119]                 -- inline accessor
    assert(...)  x3                                              -- CRT macro


## 5. Key Components Involved

Component              | File                               | Role
-----------------------|------------------------------------|-------------------------------
test_prng_deterministic| test_ImpairmentEngine.cpp:179      | Test driver; stack-allocates two
                       |                                    | engines and asserts invariants
ImpairmentEngine       | src/platform/ImpairmentEngine.hpp  | Composite object holding PRNG,
                       | src/platform/ImpairmentEngine.cpp  | delay buf, reorder buf, config
PrngEngine             | src/platform/PrngEngine.hpp        | xorshift64 PRNG, inline-only
ImpairmentConfig       | src/platform/ImpairmentConfig.hpp  | POD configuration struct +
                       |                                    | impairment_config_default()
Types.hpp              | src/core/Types.hpp                 | IMPAIR_DELAY_BUF_SIZE=32,
                       |                                    | Result enum


## 6. Branching Logic / Decision Points

Decision Point 1 -- PrngEngine::seed(), line 43-47:
  if (s == 0ULL) { m_state = 1ULL; } else { m_state = s; }
  Path taken: s = 12345ULL != 0  ->  else branch  ->  m_state = 12345ULL.
  Both engine1 and engine2 follow the same path.

Decision Point 2 -- ImpairmentEngine::init(), line 33:
  seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL
  Path taken: prng_seed = 12345ULL != 0  ->  seed = 12345ULL.
  Same for both engines.

Decision Point 3 -- main(), lines 242-246 (if !test_prng_deterministic()):
  test returns true  ->  else branch  ->  PASS printed, failed unchanged.

No branches are taken differently between the two engine initializations.
That is the key invariant: equal inputs => equal state.


## 7. Concurrency / Threading Behavior

None. The test is entirely single-threaded. Both ImpairmentEngine objects live on the
stack of test_prng_deterministic() and are accessed sequentially.  There is no shared
state between engine1 and engine2.

PrngEngine holds only m_state (uint64_t), which is written only during seed() and
next()/next_double().  The test never calls next() so m_state is never mutated after
seed() completes.

No mutexes, atomics, or thread creation occur anywhere in this execution path.


## 8. Memory & Ownership Semantics (C/C++ Specific)

All objects are automatic (stack) variables within test_prng_deterministic().
No heap allocation (malloc/new) occurs anywhere on this path.

Stack layout inside test_prng_deterministic() [ASSUMPTION: no padding/reordering by
compiler changes the ordering, but sizes are fixed]:

  ImpairmentEngine engine1:
    ImpairmentConfig m_cfg       -- ~88 bytes (all fields packed)
    PrngEngine m_prng            -- 8 bytes (uint64_t m_state)
    DelayEntry m_delay_buf[32]   -- 32 * (MessageEnvelope + uint64_t + bool + padding)
    uint32_t m_delay_count       -- 4 bytes
    MessageEnvelope m_reorder_buf[32] -- large fixed buffer
    uint32_t m_reorder_count     -- 4 bytes
    bool m_partition_active      -- 1 byte
    uint64_t m_partition_start_us -- 8 bytes
    uint64_t m_next_partition_event_us -- 8 bytes
    bool m_initialized           -- 1 byte
  ImpairmentConfig cfg1          -- same size as m_cfg above
  ImpairmentEngine engine2       -- same size as engine1
  ImpairmentConfig cfg2          -- same size as cfg1

Ownership: no pointers cross function boundaries in this test; everything is
value-copied or const-referenced.  config() returns const ImpairmentConfig& pointing
into the engine's own m_cfg -- the reference is valid for the lifetime of the engine,
which is the scope of the test function.

memset in init() correctly zero-initialises the buffers because:
  - m_delay_buf is a raw C array of structs; memset is valid.
  - m_reorder_buf is a raw C array of MessageEnvelope; memset is valid.
  - Power of 10 rule 3 comment confirms this intent.


## 9. Error Handling Flow

No error return codes are produced by test_prng_deterministic(); it returns bool.

Failure mode: if any assert() fires, the process calls abort() (standard C behavior
for assert() failure), printing a diagnostic to stderr.  The test does not use
exceptions (compile flag -fno-exceptions; F-Prime style).

The three assertions in the test body (lines 194, 200, 201) are the only possible
failure points in this path:
  - Line 194: seeds must match -> would fire only if cfg copy was corrupted.
  - Line 200: enabled must be false -> would fire only if default init was wrong.
  - Line 201: same as 200 for engine2.

Additional assertions inside init() and seed() (6 total across both engines) would
abort() if preconditions or postconditions were violated.

No Logger::log() calls occur on this path (init() logs nothing; no impairments are
applied).


## 10. External Interactions

None on this execution path.

  - No sockets, files, or OS timers are touched.
  - No Logger::log() calls are emitted (the impairment path that logs is not entered).
  - memset() and assert() are the only CRT interactions.
  - printf() is called by main() (not test_prng_deterministic()) to print PASS/FAIL.


## 11. State Changes / Side Effects

engine1 after init():
  m_initialized              = true
  m_cfg                      = copy of cfg1 (prng_seed=12345, all else default)
  m_prng.m_state             = 12345ULL
  m_delay_buf[0..31].active  = false, release_us = 0, env = zeroed
  m_delay_count              = 0
  m_reorder_buf[0..31]       = zeroed
  m_reorder_count            = 0
  m_partition_active         = false
  m_partition_start_us       = 0
  m_next_partition_event_us  = 0

engine2 after init(): identical state to engine1 (same seed, same config).

Side effects visible outside the test:
  - stdout: "PASS: test_prng_deterministic\n" printed by main() if test returns true.
  - Both engine objects are destroyed (stack unwinds) when test_prng_deterministic()
    returns; no persistent state remains.


## 12. Sequence Diagram (ASCII)

  main()                test_prng_deterministic()   ImpairmentEngine   PrngEngine
    |                           |                         |                |
    |---test_prng_deterministic()-->                       |                |
    |                           |                         |                |
    |                   impairment_config_default(cfg1)   |                |
    |                           |                         |                |
    |                   engine1.init(cfg1) -------------->|                |
    |                           |                         |--seed(12345)-->|
    |                           |                         |  m_state=12345 |
    |                           |                         |<---------------|
    |                           |                         | memset bufs    |
    |                           |                         | m_initialized=T|
    |                           |<------------------------|                |
    |                           |                         |                |
    |                   impairment_config_default(cfg2)   |                |
    |                           |                         |                |
    |                   engine2.init(cfg2) -------------->|  (engine2)     |
    |                           |                         |--seed(12345)-->|
    |                           |                         |  m_state=12345 |
    |                           |                         |<---------------|
    |                           |<------------------------|                |
    |                           |                         |                |
    |                   assert(cfg seeds equal)           |                |
    |                   assert(engine1 disabled)          |                |
    |                   assert(engine2 disabled)          |                |
    |                           |                         |                |
    |<---return true------------|                         |                |
    |                           |                         |                |
    | printf("PASS: ...")        |                         |                |


## 13. Initialization vs Runtime Flow

Initialization phase (what this test exercises entirely):
  impairment_config_default() -> engine.init() -> PrngEngine::seed()
  Everything in this test is initialization-phase activity.  No messages are sent,
  no impairment decisions are made, and no PRNG values are consumed beyond seeding.

Runtime phase (NOT exercised by this test):
  process_outbound() -> is_partition_active() -> m_prng.next_double() -> loss check
  process_outbound() -> m_prng.next_double() -> duplication check
  collect_deliverable() -> scan delay buffer

The test's correctness argument is:
  IF seed() deterministically sets m_state to the given value (verified by inspection
  of xorshift64 algorithm which is a pure function of m_state), THEN equal m_state =>
  equal next() sequences.  The test confirms the "equal m_state" precondition by
  asserting equal config seeds, relying on the code path proven above.


## 14. Known Risks / Observations

Risk 1 -- Indirect validation only:
  The test does not call next() on either engine.  It only confirms that the seed is
  stored in the config.  A regression that correctly stores the seed in m_cfg but
  passes a wrong value to PrngEngine::seed() would not be caught.
  Recommendation: add a direct sequence-comparison sub-test calling process_outbound()
  with known loss configurations and comparing ERR_IO / OK results across both engines.

Risk 2 -- PrngEngine is private:
  Because m_prng is private to ImpairmentEngine, white-box verification of m_state
  equality is impossible from the test.  The test must treat PrngEngine as a black box.

Risk 3 -- test_prng_deterministic name vs. test_loss_deterministic:
  test_loss_deterministic() [lines 77-108] actually demonstrates deterministic behavior
  by verifying that loss_probability=1.0 drops all messages, but it uses a single engine
  with seed 42 and does not compare two engines.  UC_31 maps to test_prng_deterministic()
  which tests only config-level determinism, not sequence-level.

Risk 4 -- Stack size:
  Two large ImpairmentEngine objects on the stack inside the test function.  Each
  contains two fixed arrays of size 32 (delay buf + reorder buf).  Depending on
  MessageEnvelope size, this could be several KB.  On constrained platforms this may
  cause a stack overflow.  [ASSUMPTION: host test environment has sufficient stack.]

Risk 5 -- assert() in release builds:
  If NDEBUG is defined, assert() is a no-op.  All correctness guarantees from Power
  of 10 assertions vanish.  The test still returns true, but the intermediate state
  checks provide no assurance.


## 15. Unknowns / Assumptions

[ASSUMPTION 1] ImpairmentEngine has no user-provided constructor.  Default construction
  leaves m_initialized indeterminate.  This is safe because init() sets it before any
  other method is called.

[ASSUMPTION 2] MessageEnvelope size is reasonable (< 1 KB each) so the stack usage
  of two ImpairmentEngine objects does not exceed typical test-environment limits.

[ASSUMPTION 3] The test binary is compiled without NDEBUG so assert() is active.

[ASSUMPTION 4] memset on structs containing only POD types (bool, uint32_t, uint64_t,
  fixed char arrays) produces correct zero-initialisation.  This is guaranteed by the
  C++ standard for POD types; it would be undefined for types with vtables.

[ASSUMPTION 5] The xorshift64 algorithm in PrngEngine::next() is correctly implemented
  with the standard triple (<<13, >>7, <<17).  Verification by inspection confirms this
  matches the canonical xorshift64 constants.

[ASSUMPTION 6] Logger.hpp / Logger::log() is available and links cleanly, even though
  it is not called on this test path.  If it requires global initialization, that must
  occur before main() is entered.

[ASSUMPTION 7] The use-case goal description ("call next() N times") describes design
  intent; actual test code does not perform this.  Documents labelled as such above.
```

---