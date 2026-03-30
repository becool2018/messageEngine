Use Case 29: ImpairmentEngine Seeds Its PRNG for Deterministic Test Replay
===========================================================================

## 1. Use Case Overview

Actor:        ImpairmentEngine (initialized by LocalSimHarness or directly by a test).
Trigger:      ImpairmentEngine::init(cfg) is called with a populated ImpairmentConfig
              that carries a prng_seed value (zero or non-zero).
Precondition: An ImpairmentConfig struct has been populated (e.g., via
              impairment_config_default() then overridden). The prng_seed field
              carries either 0 (use default 42) or a specific test-chosen seed.
Postcondition: PrngEngine internal state m_state is set to a known, non-zero value.
               Every subsequent call to m_prng.next(), next_double(), or next_range()
               will produce the same sequence for the same seed, in any process run.
               ImpairmentEngine is fully initialized and ready for process_outbound().
Scope:        Initialization path only. The xorshift64 algorithm is described in full.

This use case is the primary mechanism by which test engineers achieve deterministic
replay of complex impairment scenarios (loss, jitter, duplication) without a live network.


## 2. Entry Points

Primary entry point:
  ImpairmentEngine::init(const ImpairmentConfig& cfg)
  File: src/platform/ImpairmentEngine.cpp, line 23.

Seed delegation:
  PrngEngine::seed(uint64_t s)
  File: src/platform/PrngEngine.hpp, line 40 (inline).

PRNG consumption (runtime, after init):
  PrngEngine::next()           -- PrngEngine.hpp, line 63.
  PrngEngine::next_double()    -- PrngEngine.hpp, line 86.
  PrngEngine::next_range()     -- PrngEngine.hpp, line 113.

Configuration helper (called before init, by LocalSimHarness or tests):
  impairment_config_default()  -- ImpairmentConfig.hpp, line 77.


## 3. End-to-End Control Flow (Step-by-Step)

-- Phase A: Configuration Setup (caller, before init()) --

Step A1  -- Caller declares ImpairmentConfig cfg.
Step A2  -- Caller calls impairment_config_default(cfg). [ImpairmentConfig.hpp:77]
            This sets ALL fields to safe defaults:
              enabled                 = false
              fixed_latency_ms        = 0U
              jitter_mean_ms          = 0U
              jitter_variance_ms      = 0U
              loss_probability        = 0.0
              duplication_probability = 0.0
              reorder_enabled         = false
              reorder_window_size     = 0U
              partition_enabled       = false
              partition_duration_ms   = 0U
              partition_gap_ms        = 0U
              prng_seed               = 42ULL   <-- DEFAULT DETERMINISTIC SEED
Step A3  -- Caller optionally overrides prng_seed with a test-specific value, e.g.:
              cfg.prng_seed = 0xDEADBEEFCAFEBABEULL

-- Phase B: ImpairmentEngine::init() --

Step B1  -- ImpairmentEngine::init(cfg) is entered. [ImpairmentEngine.cpp:23]

Step B2  -- Two precondition assertions fire:
              assert(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)  [line 26]
              assert(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0) [line 27]

Step B3  -- m_cfg = cfg (full struct copy). [line 30]

-- Phase C: Seed Resolution and PRNG Seeding --

Step C1  -- Seed resolution logic: [lines 33-34]
              uint64_t seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL;
            Decision table:
              cfg.prng_seed == 0       -> seed = 42ULL   (default)
              cfg.prng_seed == 42      -> seed = 42ULL   (explicit match to default)
              cfg.prng_seed == <other> -> seed = cfg.prng_seed

Step C2  -- m_prng.seed(seed) is called. [line 34]
            Inside PrngEngine::seed(s) [PrngEngine.hpp:40]:
              a. Check: if (s == 0ULL): m_state = 1ULL  [line 43-44]
                        else:           m_state = s      [line 46]
                 NOTE: Because the caller already coerced 0 to 42 in Step C1,
                 the s==0 branch in seed() is a secondary safety net that should
                 never fire in normal use. It would only fire if seed() were
                 called directly with 0.
              b. Post-condition assertion: assert(m_state != 0ULL) [line 50]
            PrngEngine::seed() returns (void).

-- Phase D: Remainder of ImpairmentEngine::init() --

Step D1  -- Zero-fill delay buffer: [lines 37-38]
              memset(m_delay_buf, 0, sizeof(m_delay_buf))
              m_delay_count = 0U
            m_delay_buf is an array of IMPAIR_DELAY_BUF_SIZE DelayEntry structs.
            Each DelayEntry contains: MessageEnvelope env, uint64_t release_us, bool active.

Step D2  -- Zero-fill reorder buffer: [lines 41-42]
              memset(m_reorder_buf, 0, sizeof(m_reorder_buf))
              m_reorder_count = 0U
            m_reorder_buf is an array of IMPAIR_DELAY_BUF_SIZE MessageEnvelope structs.

Step D3  -- Partition state initialization: [lines 45-48]
              m_partition_active         = false
              m_partition_start_us       = 0ULL
              m_next_partition_event_us  = 0ULL
            NOTE: The comment at line 47 states that is_partition_active() will
            initialize m_next_partition_event_us on its first call.

Step D4  -- m_initialized = true. [line 51]

Step D5  -- Post-condition assertions: [lines 54-55]
              assert(m_initialized)
              assert(m_delay_count == 0U)

Step D6  -- ImpairmentEngine::init() returns (void).

-- Phase E: PRNG Usage at Runtime (illustrative, not init) --

Step E1  -- Caller eventually invokes process_outbound(env, now_us).
Step E2  -- Inside process_outbound(), if loss_probability > 0.0:
              double loss_rand = m_prng.next_double()   [ImpairmentEngine.cpp:101]
            Inside PrngEngine::next_double() [PrngEngine.hpp:86]:
              a. assert(m_state != 0ULL)
              b. raw = next()          -- calls xorshift64 (see Section on algorithm)
              c. result = (double)raw / (double)UINT64_MAX
              d. assert(result >= 0.0 && result < 1.0)
              e. returns result

Step E3  -- The sequence of values produced by next() is fully determined by the
            initial m_state set in Step C2. Same seed -> same sequence, every run.


## 4. Call Tree (Hierarchical)

[Pre-init call by test/harness]
impairment_config_default(cfg)                         [ImpairmentConfig.hpp:77]
  +-- cfg.prng_seed = 42ULL  (default)

[Optionally, test overrides: cfg.prng_seed = 0xDEADBEEF...]

ImpairmentEngine::init(cfg)                            [ImpairmentEngine.cpp:23]
  |
  +-- [assertions] reorder_window_size, loss_probability
  |
  +-- m_cfg = cfg  (struct copy)
  |
  +-- seed = (cfg.prng_seed != 0) ? cfg.prng_seed : 42ULL
  |
  +-- m_prng.seed(seed)                                [PrngEngine.hpp:40]
  |     +-- if (s == 0ULL): m_state = 1ULL
  |     |   else:           m_state = s
  |     +-- assert(m_state != 0ULL)
  |
  +-- memset(m_delay_buf,   0, sizeof(m_delay_buf))
  +-- m_delay_count = 0U
  +-- memset(m_reorder_buf, 0, sizeof(m_reorder_buf))
  +-- m_reorder_count = 0U
  +-- m_partition_active = false
  +-- m_partition_start_us = 0ULL
  +-- m_next_partition_event_us = 0ULL
  +-- m_initialized = true
  +-- [assertions] m_initialized, m_delay_count == 0U

[Runtime calls -- produced in reproducible order after seeding]
m_prng.next()                                          [PrngEngine.hpp:63]
  +-- assert(m_state != 0ULL)
  +-- m_state ^= m_state << 13U    // xorshift step 1
  +-- m_state ^= m_state >> 7U     // xorshift step 2
  +-- m_state ^= m_state << 17U    // xorshift step 3
  +-- assert(m_state != 0ULL)
  +-- return m_state

m_prng.next_double()                                   [PrngEngine.hpp:86]
  +-- assert(m_state != 0ULL)
  +-- raw = next()
  +-- result = (double)raw / (double)UINT64_MAX
  +-- assert(result in [0.0, 1.0))
  +-- return result

m_prng.next_range(lo, hi)                             [PrngEngine.hpp:113]
  +-- assert(m_state != 0ULL)
  +-- assert(hi >= lo)
  +-- raw = next()
  +-- range = hi - lo + 1U
  +-- offset = (uint32_t)(raw % (uint64_t)range)
  +-- result = lo + offset
  +-- assert(result in [lo, hi])
  +-- return result


## 5. Key Components Involved

Component             Type          File                              Role
-----------           --------      ----                              ----
ImpairmentEngine      class         src/platform/ImpairmentEngine.cpp Top-level impairment coordinator.
PrngEngine            class (member) src/platform/PrngEngine.hpp      xorshift64 PRNG (header-only inline).
ImpairmentConfig      struct        src/platform/ImpairmentConfig.hpp Config including prng_seed.
impairment_config_default  inline fn ImpairmentConfig.hpp            Sets safe defaults, default seed=42.
DelayEntry (inner)    struct        ImpairmentEngine.hpp              Buffered message with release time.


## 6. Branching Logic / Decision Points

Branch 1: cfg.prng_seed == 0 vs non-zero [ImpairmentEngine.cpp:33]
  - == 0:     seed = 42ULL  (library-defined default for reproducibility)
  - != 0:     seed = cfg.prng_seed  (test-provided value for custom replay)
  This is the primary branch that controls test determinism.

Branch 2: s == 0 in PrngEngine::seed() [PrngEngine.hpp:43]
  - == 0:     m_state = 1ULL  (coercion; xorshift64 requires non-zero state)
  - != 0:     m_state = s
  This branch is a secondary safety net. Under normal ImpairmentEngine usage it should
  never fire because Step C1 already guards against 0.

Branch 3: m_cfg.enabled check in process_outbound() [ImpairmentEngine.cpp:70]
  - false: messages pass through to delay buffer immediately with release_us=now_us.
           PRNG is NOT consumed in this path.
  - true:  impairments applied; PRNG IS consumed for loss, duplication, jitter checks.
  Critical for test design: if enabled==false the PRNG state does not advance, so
  the output sequence is still deterministic but trivially so.

Branch 4: loss_probability > 0.0 [ImpairmentEngine.cpp:100]
  - The PRNG next_double() call only occurs when this condition is true.
  - If loss_probability == 0.0, the PRNG state is NOT consumed for the loss check.
  - This means the sequence of random numbers consumed depends on configuration.
  - For identical seeds AND identical configurations, output is reproducible.

Branch 5: jitter_mean_ms > 0U [ImpairmentEngine.cpp:114]
  - PRNG next_range() is only called when jitter is enabled.
  - Same implication as Branch 4 for reproducibility.

Branch 6: duplication_probability > 0.0 [ImpairmentEngine.cpp:144]
  - PRNG next_double() is only called when this condition is true.

Branch 7: partition_enabled in is_partition_active() [ImpairmentEngine.cpp:285]
  - false: returns immediately without consuming PRNG.
  - true:  state machine runs. PRNG is NOT used for partition timing (durations are
           deterministic from the config values).


## 7. Concurrency / Threading Behavior

PrngEngine is NOT thread-safe. m_state is a plain uint64_t with no atomic protection.

ImpairmentEngine::init() is intended to complete before concurrent use begins.
After init(), PrngEngine::next() modifies m_state on every call. If process_outbound()
and collect_deliverable() were called from different threads, m_state would be subject
to a data race. The design assumes single-threaded use of each ImpairmentEngine instance
(consistent with the SPSC model used by LocalSimHarness's RingBuffer).

The xorshift64 algorithm itself is not safe for concurrent calls because the three-step
XOR sequence requires read-modify-write atomicity that plain uint64_t does not provide.

Conclusion: Use from a single thread only. This is consistent with the F-Prime model
where each channel/harness is driven by a single thread or event loop.


## 8. Memory and Ownership Semantics (C/C++ Specific)

PrngEngine state: single uint64_t m_state (8 bytes). No dynamic allocation.
  - Value-initialized in the containing ImpairmentEngine object.
  - Seeded by value copy from the caller's cfg.prng_seed.

ImpairmentConfig is passed by const reference and copied into m_cfg (line 30).
  - After init() returns, m_cfg is independent of the caller's struct.

DelayEntry array: m_delay_buf[IMPAIR_DELAY_BUF_SIZE] -- inline in ImpairmentEngine.
  - Contains MessageEnvelope structs (potentially large objects).
  - Zero-filled by memset at init. [ASSUMPTION: MessageEnvelope has trivial zero repr.]

Reorder buffer: m_reorder_buf[IMPAIR_DELAY_BUF_SIZE] -- inline in ImpairmentEngine.
  - Zero-filled by memset at init.

No heap allocation occurs anywhere in the init or PRNG seeding path (Power of 10 rule 3).

sizeof considerations:
  - PrngEngine: 8 bytes.
  - ImpairmentConfig: approximately 64 bytes (estimated from field types).
  - ImpairmentEngine total: dominated by two arrays of IMPAIR_DELAY_BUF_SIZE
    DelayEntry/MessageEnvelope structs. [ASSUMPTION: IMPAIR_DELAY_BUF_SIZE is O(16-64).]


## 9. Error Handling Flow

ImpairmentEngine::init() is void; no Result is returned.
All error detection is via assert().

Assertion failures during init:
  - cfg.reorder_window_size > IMPAIR_DELAY_BUF_SIZE -> assert at line 26.
  - cfg.loss_probability outside [0.0, 1.0]         -> assert at line 27.
  - m_state == 0 after seed()                        -> assert at PrngEngine.hpp:50.
                                                        (Cannot fire if seed>0 is passed.)

At runtime (PRNG consumption):
  - assert(m_state != 0ULL) fires in next()/next_double()/next_range() if somehow
    m_state becomes 0. The xorshift64 algorithm provably cannot reach 0 if the initial
    state is non-zero (the three-step XOR operation preserves this invariant).
    [MATHEMATICAL PROPERTY: xorshift64 generates a maximal-length sequence over all
    non-zero 64-bit values; 0 is a fixed point and is never reachable from non-zero state.]

Assertion in next_double():
  - assert(result >= 0.0 && result < 1.0). This should always pass since raw is a
    positive uint64_t divided by UINT64_MAX. Only a floating-point implementation
    defect could violate this.

Assertion in next_range():
  - assert(hi >= lo) fires if caller provides inverted range.
  - assert(result >= lo && result <= hi) validates the modulo arithmetic.


## 10. External Interactions

During init():
  - No OS calls.
  - No I/O.
  - No network.
  - No Logger::log() call inside ImpairmentEngine::init() itself.
    [OBSERVATION: Unlike DeliveryEngine::init(), there is no INFO log at the end of
    ImpairmentEngine::init(). This is an observability gap.]

During runtime PRNG consumption:
  - PrngEngine::next() and its callers are pure computation with no external interactions.
  - Logger::log() IS called inside process_outbound() on impairment events (loss drop,
    duplication), but those are runtime events, not init events.


## 11. State Changes / Side Effects

Before init():
  m_cfg                    undefined
  m_prng.m_state           undefined (uninitialized uint64_t)
  m_delay_buf              undefined
  m_delay_count            undefined
  m_reorder_buf            undefined
  m_reorder_count          undefined
  m_partition_active       undefined
  m_partition_start_us     undefined
  m_next_partition_event_us undefined
  m_initialized            undefined (false if zero-initialized by constructor)

After init(cfg) with, e.g., cfg.prng_seed = 999:
  m_cfg                    = copy of cfg parameter
  m_prng.m_state           = 999ULL
  m_delay_buf[0..N-1]      = all-zeros (active=false, release_us=0, env=zeroed)
  m_delay_count            = 0U
  m_reorder_buf[0..N-1]    = all-zeros
  m_reorder_count          = 0U
  m_partition_active       = false
  m_partition_start_us     = 0ULL
  m_next_partition_event_us = 0ULL
  m_initialized            = true

After first call to m_prng.next() (e.g., seed=999):
  xorshift64 steps on 999:
    step 1: 999 ^= 999 << 13  = 999 ^ 8183808   = 8182857
    step 2: 8182857 ^= 8182857 >> 7  (illustrative; exact value requires binary arithmetic)
    step 3: result ^= result << 17
  m_state transitions to the xorshift64 successor of 999.
  This value is deterministic and reproducible on every platform with conforming C++17.


## 12. Sequence Diagram (ASCII)

  Test/Caller             ImpairmentEngine        PrngEngine           Logger
      |                         |                     |                    |
      |-- config_default() -->  [cfg.prng_seed=42]    |                    |
      |-- cfg.prng_seed=SEED -> [cfg.prng_seed=SEED]  |                    |
      |                         |                     |                    |
      |-- init(cfg) ----------> |                     |                    |
      |                         |-- assert bounds     |                    |
      |                         |-- m_cfg = cfg       |                    |
      |                         |                     |                    |
      |                         |-- seed=(cfg.prng_seed!=0)?cfg.prng_seed:42
      |                         |                     |                    |
      |                         |-- seed(seed) -----> |                    |
      |                         |                     |-- if s==0: state=1 |
      |                         |                     |-- else: state=s    |
      |                         |                     |-- assert(state!=0) |
      |                         |                     |                    |
      |                         |-- memset delay_buf  |                    |
      |                         |-- m_delay_count=0   |                    |
      |                         |-- memset reorder_buf|                    |
      |                         |-- m_reorder_count=0 |                    |
      |                         |-- partition state=0 |                    |
      |                         |-- m_initialized=true|                    |
      |                         |-- assert(init)      |                    |
      |                         |-- assert(delay==0)  |                    |
      | <-- (void) ------------ |                     |                    |
      |                         |                     |                    |
      | (later at runtime)      |                     |                    |
      |-- process_outbound() -> |                     |                    |
      |                         |-- next_double() --> |                    |
      |                         |                     |-- next()           |
      |                         |                     |   state^=state<<13 |
      |                         |                     |   state^=state>>7  |
      |                         |                     |   state^=state<<17 |
      |                         |                     |-- returns state    |
      |                         |                     |-- /UINT64_MAX      |
      |                         |                     |-- return double    |
      |                         |                     |                    |
      |                         |  [loss check pass]  |                    |
      |                         |-- [buffer message]  |                    |
      |                         |-- (returns OK)      |                    |
      | <-- Result::OK -------- |                     |                    |


## 13. Initialization vs Runtime Flow

Initialization (this use case):
  1. impairment_config_default() sets prng_seed = 42 (or caller provides custom seed).
  2. ImpairmentEngine::init(cfg) copies config, resolves seed, calls m_prng.seed(seed).
  3. m_prng.m_state is set. All buffers zeroed. m_initialized = true.
  4. After init() returns, the PRNG is seeded but has not yet produced output.
     The state is deterministic given the seed; the sequence is thus fully reproducible.

Runtime (post-init):
  - Each call to m_prng.next() advances m_state via three XOR-shift steps.
  - m_prng.next_double() calls next() then divides.
  - m_prng.next_range(lo, hi) calls next() then applies modulo.
  - The ORDER in which these methods are called depends on enabled impairments
    and message arrival patterns. For identical seeds AND identical cfg AND identical
    message sequences, all impairment decisions reproduce exactly.

xorshift64 Algorithm (Complete):
  Given state S (non-zero uint64_t):
    S = S XOR (S left-shift 13)
    S = S XOR (S right-shift 7)
    S = S XOR (S left-shift 17)
    return S

  Properties:
    - Period: 2^64 - 1 (visits all non-zero 64-bit states exactly once).
    - Fixed point: 0 (never reachable from non-zero initial state).
    - No floating-point operations in the core step (fast, deterministic).
    - No seed-dependent branching in the hot path.
    - All three shift constants (13, 7, 17) are from Marsaglia's published
      maximal-period xorshift64 parameter table.


## 14. Known Risks / Observations

Risk 1: PRNG state is not reset between tests if the same ImpairmentEngine object is reused.
  If init() is called a second time with a different seed, m_prng.m_state is correctly
  re-seeded. But if init() is NOT called again and process_outbound() is called from
  a new test, the PRNG continues from where the previous test left off. This can break
  determinism across tests.

Risk 2: seed==0 handling has two layers (ImpairmentEngine and PrngEngine).
  Both coerce 0 to a non-zero value (42 and 1 respectively). If the behavior of the
  double-coercion is not understood, a test author who explicitly sets prng_seed=0
  expecting "no seeding" will get seed=42, which is non-obvious.

Risk 3: Impairment decision reproducibility requires matching config.
  As noted in Branch 4-6, the PRNG is only consumed when the corresponding impairment
  is enabled. Different cfg values (even with the same seed) produce different sequences
  of consumed random values. Tests must document both seed AND full ImpairmentConfig.

Risk 4: No Logger::log() on init.
  Unlike DeliveryEngine, ImpairmentEngine::init() emits no log message. The effective
  seed used is not observable at runtime without a debugger.

Risk 5: memset on structs containing non-trivial members.
  If MessageEnvelope or DelayEntry ever gains a non-trivially-constructible member
  (e.g., a string object), the memset will corrupt it. The current code is safe because
  all fields are POD, but this is a latent maintenance risk.

Observation: The xorshift64 algorithm has known weaknesses for cryptographic use but is
  entirely appropriate for simulation impairment decisions. It is fast, reproducible, and
  has a period of 2^64-1 which is far more than sufficient for test replay.


## 15. Unknowns / Assumptions

[ASSUMPTION] IMPAIR_DELAY_BUF_SIZE is a compile-time constant of sufficient size to hold
  both the delay buffer and the reorder buffer. Its exact value was not in the read files.

[ASSUMPTION] MessageEnvelope is a POD struct with a trivial zero-representation such that
  memset to 0 produces a valid "empty/uninitialized" envelope state.

[ASSUMPTION] The xorshift64 shift constants (13, 7, 17) are those from the original
  Marsaglia paper for maximal-period 64-bit output. The code comment confirms "xorshift64"
  but does not cite the specific parameter set.

[ASSUMPTION] UINT64_MAX is available and correctly equals 2^64 - 1 on the target platform.
  This is guaranteed by the C++17 standard on all conforming platforms.

[UNKNOWN] Whether ImpairmentEngine::init() is ever called more than once (e.g., to reset
  impairment state mid-test). The code supports it but no test in test_LocalSim.cpp
  demonstrates this pattern.

[UNKNOWN] The exact value of IMPAIR_DELAY_BUF_SIZE and whether it matches MSG_RING_CAPACITY.
  These constants determine the worst-case memory footprint of ImpairmentEngine.

[ASSUMPTION] The partition state machine does NOT consume the PRNG. Partition timing is
  fully deterministic from the config values (duration_ms, gap_ms) and the clock. This
  is confirmed by reading is_partition_active() which uses only integer comparisons.