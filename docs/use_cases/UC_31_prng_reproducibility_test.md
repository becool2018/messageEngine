# UC_31 — PRNG reproducibility test

**HL Group:** HL-13 — User seeds the impairment engine for deterministic replay
**Actor:** Test function (`test_prng_deterministic()` in `tests/test_ImpairmentEngine.cpp`)
**Requirement traceability:** REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

**Trigger:** `main()` in `tests/test_ImpairmentEngine.cpp` calls `test_prng_deterministic()` (line 496) as part of the impairment engine test suite.

**Precondition:** No threads are running. Two `ImpairmentEngine` objects are stack-allocated inside the test function. Both are uninitialized (constructor has run; `init()` has not yet been called). No messages have been sent.

**Postcondition:** Both `ImpairmentEngine` objects have been initialized with `prng_seed=12345ULL`. The test confirms that identical seeds result in identical `ImpairmentConfig` state (seed value and `enabled` flag). Both `PrngEngine` instances hold `m_state=12345ULL`, guaranteeing that any subsequent sequence of `next()` calls will produce identical output on both engines. The test function returns `true`.

**Scope:** In-process, single-threaded. Tests the seed propagation path only: `ImpairmentConfig` construction → `impairment_config_default()` → seed override → `ImpairmentEngine::init()` → `PrngEngine::seed()`. No messages are processed, no impairment decisions are made, and the PRNG is not consumed beyond seeding.

**Important note on test coverage:** The test does NOT call `next()` or `next_double()` on either engine and does not compare raw output sequences. It validates the determinism invariant indirectly: it confirms that equal seeds produce equal `m_cfg.prng_seed` values, relying on code inspection of the xorshift64 algorithm (a pure function of state) to complete the proof. The full determinism argument is: equal `prng_seed` in config → `init()` passes seed to `PrngEngine::seed()` → `seed()` sets `m_state = seed` → equal `m_state` → equal `next()` sequences. Only the first two steps are exercised by the test; the rest follow from the algorithm's definition.

---

## 2. Entry Points

| Symbol | File | Line |
|---|---|---|
| `main()` | `tests/test_ImpairmentEngine.cpp` | 464 |
| `test_prng_deterministic()` | `tests/test_ImpairmentEngine.cpp` | 186 |
| `ImpairmentEngine::ImpairmentEngine()` (constructor) | `src/platform/ImpairmentEngine.cpp` | 26 |
| `impairment_config_default(cfg)` | `src/platform/ImpairmentConfig.hpp` | 79 |
| `ImpairmentEngine::init(cfg)` | `src/platform/ImpairmentEngine.cpp` | 44 |
| `PrngEngine::seed(s)` | `src/platform/PrngEngine.hpp` | 43 |
| `ImpairmentEngine::config()` | `src/platform/ImpairmentEngine.hpp` | 130 |

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase 1 — Entry

**Step 1.1** `main()` [line 464]: The process starts. `int failed = 0` is set. Several earlier tests (`test_passthrough_disabled`, `test_loss_deterministic`, `test_no_loss`, `test_fixed_latency`) execute first. This document traces only `test_prng_deterministic`, invoked at line 496.

**Step 1.2** `main()` calls `test_prng_deterministic()` [line 496].

### Phase 2 — engine1 Construction and Initialization

**Step 2.1** `ImpairmentEngine engine1;` constructed on the stack [line 188].
Inside `ImpairmentEngine::ImpairmentEngine()` [line 26]:
- `m_cfg{}` — value-initializes all `ImpairmentConfig` fields to zero/false
- `m_delay_count = 0U`
- `m_reorder_count = 0U`
- `m_partition_active = false`
- `m_partition_start_us = 0ULL`
- `m_next_partition_event_us = 0ULL`
- `m_initialized = false`
- `memset(m_delay_buf, 0, sizeof(m_delay_buf))` — zeroes all 32 `DelayEntry` slots
- `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))` — zeroes all 32 `MessageEnvelope` reorder slots
- `m_prng.seed(1ULL)` — **preliminary seed** (will be overwritten by `init()`)
  - Inside `PrngEngine::seed(s=1ULL)` [line 43]: `s != 0ULL` → `m_state = 1ULL`; `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` passes
- `NEVER_COMPILED_OUT_ASSERT(!m_initialized)` — passes (`m_initialized=false`)
- `NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U)` — passes (32 > 0)

**Step 2.2** `ImpairmentConfig cfg1;` declared on the stack [line 189]. Fields are indeterminate until `impairment_config_default()` is called.

**Step 2.3** `impairment_config_default(cfg1)` called [line 190].
Inside `impairment_config_default(cfg)` [`ImpairmentConfig.hpp:79`]:
- `cfg1.enabled = false`
- `cfg1.fixed_latency_ms = 0U`
- `cfg1.jitter_mean_ms = 0U`
- `cfg1.jitter_variance_ms = 0U`
- `cfg1.loss_probability = 0.0`
- `cfg1.duplication_probability = 0.0`
- `cfg1.reorder_enabled = false`
- `cfg1.reorder_window_size = 0U`
- `cfg1.partition_enabled = false`
- `cfg1.partition_duration_ms = 0U`
- `cfg1.partition_gap_ms = 0U`
- `cfg1.prng_seed = 42ULL` ← default; will be overridden next

**Step 2.4** `cfg1.prng_seed = 12345ULL` [line 191] — overwrites the default `42ULL` with the test-specified seed.

**Step 2.5** `engine1.init(cfg1)` called [line 192].
Inside `ImpairmentEngine::init(cfg)` [line 44]:

- **Step 2.5a** Precondition assertions [lines 47–48]:
  - `NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)` → `0 <= 32` — passes
  - `NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0)` → `0.0 ∈ [0.0,1.0]` — passes

- **Step 2.5b** `m_cfg = cfg` [line 51] — copies all `ImpairmentConfig` fields into `engine1.m_cfg`.
  `engine1.m_cfg.prng_seed == 12345ULL` after this assignment.

- **Step 2.5c** Seed resolution [lines 54–55]:
  `seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL`
  `= 12345ULL` (non-zero → used directly, not coerced)
  `m_prng.seed(12345ULL)` called.
  Inside `PrngEngine::seed(s=12345ULL)` [line 43]:
  - `s != 0ULL` → `m_state = 12345ULL` (**overwrites constructor's 1ULL**)
  - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` — passes
  - Returns (inline void)

- **Step 2.5d** `memset(m_delay_buf, 0, sizeof(m_delay_buf))` [line 58] — re-zeroes all 32 `DelayEntry` slots (already zeroed by constructor; `init()` repeats for safety on reuse).

- **Step 2.5e** `m_delay_count = 0` [line 59]

- **Step 2.5f** `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))` [line 62] — re-zeroes all 32 `MessageEnvelope` reorder slots.

- **Step 2.5g** `m_reorder_count = 0` [line 63]

- **Step 2.5h** `m_partition_active = false` [line 66]; `m_partition_start_us = 0` [line 67]; `m_next_partition_event_us = 0` [line 69]

- **Step 2.5i** `m_initialized = true` [line 72]

- **Step 2.5j** Postcondition assertions [lines 75–76]:
  - `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — passes
  - `NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U)` — passes

- Returns `Result::OK`. `engine1.m_prng.m_state = 12345ULL`.

### Phase 3 — engine2 Construction and Initialization

**Step 3.1** `ImpairmentEngine engine2;` constructed on the stack [line 194] — identical execution to Step 2.1. `engine2.m_prng.m_state = 1ULL` after constructor.

**Step 3.2** `ImpairmentConfig cfg2;` declared [line 195].

**Step 3.3** `impairment_config_default(cfg2)` [line 196] — identical outcome to Step 2.3. `cfg2.prng_seed = 42ULL`.

**Step 3.4** `cfg2.prng_seed = 12345ULL` [line 197] — same override as Step 2.4.

**Step 3.5** `engine2.init(cfg2)` [line 198] — identical execution path to Step 2.5, applied to `engine2`.
`engine2.m_prng.m_state = 12345ULL` after completion.

### Phase 4 — Assertions

**Step 4.1** `assert(engine1.config().prng_seed == engine2.config().prng_seed)` [line 201]
- `ImpairmentEngine::config()` [line 130] returns `const ImpairmentConfig&` — inline accessor returning `m_cfg`.
- `engine1.m_cfg.prng_seed = 12345ULL`
- `engine2.m_cfg.prng_seed = 12345ULL`
- `12345ULL == 12345ULL` — **assertion passes**

**Step 4.2** `assert(engine1.config().enabled == false)` [line 207]
- `engine1.m_cfg.enabled = false` (set by `impairment_config_default()`; never overridden)
- **assertion passes**

**Step 4.3** `assert(engine2.config().enabled == false)` [line 208]
- `engine2.m_cfg.enabled = false`
- **assertion passes**

### Phase 5 — Return and Reporting

**Step 5.1** `return true` [line 210]. Control returns to `main()`.

**Step 5.2** `main()` evaluates `!test_prng_deterministic()` [line 496]: `!true == false` → else branch → `printf("PASS: test_prng_deterministic\n")` [line 500].

**Step 5.3** `main()` continues remaining tests and eventually evaluates `return (failed > 0) ? 1 : 0` [line 552].

### Phase 6 — Stack Unwind

**Step 6.1** `engine2` goes out of scope. Compiler-generated destructor: no explicit `~ImpairmentEngine()`; trivial destruction. Stack memory reclaimed.

**Step 6.2** `cfg2` goes out of scope. POD struct; trivial destruction.

**Step 6.3** `engine1` and `cfg1` go out of scope; same as above.

No logging, no OS calls, no side effects occur during destruction.

---

## 4. Call Tree (Hierarchical)

```
main()  [test_ImpairmentEngine.cpp:464]
  test_prng_deterministic()  [line 186]
    ImpairmentEngine::ImpairmentEngine()  [ImpairmentEngine.cpp:26]  — engine1 ctor
      m_prng.seed(1ULL)  [PrngEngine.hpp:43]                        — preliminary seed
      memset(m_delay_buf, 0, ...)                                    — CRT
      memset(m_reorder_buf, 0, ...)                                  — CRT
      NEVER_COMPILED_OUT_ASSERT(!m_initialized)
      NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U)
    impairment_config_default(cfg1)  [ImpairmentConfig.hpp:79]       — inline
    [cfg1.prng_seed = 12345ULL]
    engine1.init(cfg1)  [ImpairmentEngine.cpp:44]
      NEVER_COMPILED_OUT_ASSERT(reorder_window_size <= 32)
      NEVER_COMPILED_OUT_ASSERT(loss_probability in [0,1])
      m_cfg = cfg1
      m_prng.seed(12345ULL)  [PrngEngine.hpp:43]                    — inline, m_state=12345
      memset(m_delay_buf, 0, ...)                                    — CRT
      m_delay_count = 0
      memset(m_reorder_buf, 0, ...)                                  — CRT
      m_reorder_count = 0
      m_initialized = true
      NEVER_COMPILED_OUT_ASSERT(m_initialized)
      NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U)
    ImpairmentEngine::ImpairmentEngine()  [ImpairmentEngine.cpp:26]  — engine2 ctor
      m_prng.seed(1ULL)  [PrngEngine.hpp:43]
      memset(...)  ×2
    impairment_config_default(cfg2)  [ImpairmentConfig.hpp:79]       — inline
    [cfg2.prng_seed = 12345ULL]
    engine2.init(cfg2)  [ImpairmentEngine.cpp:44]
      m_prng.seed(12345ULL)  [PrngEngine.hpp:43]                    — inline, m_state=12345
      [identical structure to engine1.init]
    engine1.config()  [ImpairmentEngine.hpp:130]                    — inline const ref
    engine2.config()  [ImpairmentEngine.hpp:130]                    — inline const ref
    assert(seeds equal)  [line 201]                                  — standard C assert()
    assert(engine1 disabled)  [line 207]
    assert(engine2 disabled)  [line 208]
    return true
```

---

## 5. Key Components Involved

| Component | File | Role in this flow |
|---|---|---|
| `test_prng_deterministic()` | `tests/test_ImpairmentEngine.cpp:186` | Test driver. Stack-allocates two `ImpairmentEngine` objects with identical seeds and asserts that config state is equal after initialization. |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.cpp/.hpp` | Composite object holding `PrngEngine m_prng`, `ImpairmentConfig m_cfg`, delay buffer, reorder buffer, and partition state. `init()` seeds the PRNG from config. Constructor pre-seeds to `1ULL`. |
| `PrngEngine` | `src/platform/PrngEngine.hpp` | Header-only inline xorshift64 PRNG. Single state variable `uint64_t m_state`. `seed(s)` sets `m_state = s` (coerces `0 → 1`). `next()` applies `x^=x<<13; x^=x>>7; x^=x<<17` and returns `m_state`. Period 2^64−1. |
| `ImpairmentConfig` | `src/platform/ImpairmentConfig.hpp` | POD struct with 11 fields. `impairment_config_default()` initializes all fields including `prng_seed=42ULL`. The test overrides `prng_seed` to `12345ULL`. |
| `impairment_config_default()` | `src/platform/ImpairmentConfig.hpp:79` | Inline initializer that sets all `ImpairmentConfig` fields to safe defaults (`enabled=false`, all impairments zeroed, `prng_seed=42ULL`). |
| `Types.hpp` | `src/core/Types.hpp` | Provides `IMPAIR_DELAY_BUF_SIZE=32`, `MSG_MAX_PAYLOAD_BYTES=4096`, and the `Result` enum used throughout. |

---

## 6. Branching Logic / Decision Points

| # | Location | Condition | Path Taken | Effect |
|---|---|---|---|---|
| 1 | `PrngEngine::seed()` line 46–47 (in constructor) | `s == 0ULL` | `s=1ULL` → **false** → else branch | `m_state = 1ULL` (preliminary; overwritten by `init()`) |
| 2 | `ImpairmentEngine::init()` line 54 | `cfg.prng_seed != 0ULL` | `12345ULL != 0` → **true** | `seed = 12345ULL` (no coercion to 42; seed used directly) |
| 3 | `PrngEngine::seed()` line 46–47 (in `init()`) | `s == 0ULL` | `s=12345ULL` → **false** → else branch | `m_state = 12345ULL` (**final state**) |
| 4 | `main()` line 496 | `!test_prng_deterministic()` | Returns `true` → `!true` → **false** | PASS branch; `failed` unchanged |

No branches are taken differently between `engine1` and `engine2` initializations. That symmetry is the key invariant: equal inputs → equal execution → equal final state.

**Paths NOT taken in this test (but important for understanding the design):**
- Seed coercion in `init()`: `cfg.prng_seed == 0ULL` → coerced to `42ULL`. Not triggered here.
- Seed coercion in `PrngEngine::seed()`: `s == 0ULL` → `m_state = 1ULL`. Not triggered here (neither constructor's `1ULL` nor `init()`'s `12345ULL` is zero).

---

## 7. Concurrency / Threading Behavior

The test is entirely single-threaded. Both `ImpairmentEngine` objects live on the stack of `test_prng_deterministic()` and are accessed sequentially.

`PrngEngine` holds only `m_state` (`uint64_t`), written during `seed()` and read/written during `next()`. The test never calls `next()`, so `m_state` is not mutated after `seed()` completes.

No `std::atomic` variables are accessed in this test. No mutexes, condition variables, or thread creation occur anywhere on this execution path.

---

## 8. Memory and Ownership Semantics (C/C++ Specific)

All objects are automatic (stack) variables within `test_prng_deterministic()`. No heap allocation (`malloc`/`new`) occurs anywhere on this path.

Approximate stack layout inside `test_prng_deterministic()`:

| Object | Dominant contributor | Approximate size |
|---|---|---|
| `ImpairmentEngine engine1` — `m_delay_buf[32]` | 32 × `DelayEntry` (~4129 B each) | ~132 KB |
| `ImpairmentEngine engine1` — `m_reorder_buf[32]` | 32 × `MessageEnvelope` (~4120 B each) | ~132 KB |
| `ImpairmentConfig cfg1` | 11 POD fields | ~64 B |
| `ImpairmentEngine engine2` | Same as engine1 | ~264 KB |
| `ImpairmentConfig cfg2` | Same as cfg1 | ~64 B |
| **Total** | | **~528 KB** |

`DelayEntry` size: `MessageEnvelope` (4096 payload + ~24 overhead) + `uint64_t release_us` (8 B) + `bool active` (1 B) + padding. Approximately 4129 B per entry.

**Ownership:**
- No pointers cross function boundaries. `config()` returns `const ImpairmentConfig&` pointing into `engine1.m_cfg`; the reference is valid for the lifetime of `engine1`, which is the scope of the test function.
- `memset` in `init()` correctly zero-initializes `m_delay_buf` and `m_reorder_buf` because both are raw C arrays of POD-compatible structs.

**RAII and destruction:**
- `ImpairmentEngine` has no explicit destructor; the compiler-generated destructor is trivial (all members are POD or have trivial destructors). Stack memory is reclaimed when the test function returns.
- The constructor's `memset` plus `init()`'s `memset` means the buffers are zeroed twice on a fresh object. This redundancy ensures correctness if `init()` is ever called on a reused engine without a preceding constructor.

---

## 9. Error Handling Flow

`test_prng_deterministic()` returns `bool`; no `Result` codes are produced.

**Failure mode:** If any `assert()` fires (standard C `assert()` used in test code), the process calls `abort()` and prints a diagnostic to `stderr`. The test does not use exceptions (`-fno-exceptions` compile flag; F-Prime style).

The three `assert()` calls in the test body are the only possible external failure points:
- Line 201: seed values must match → would fire only if `cfg` copy was corrupted during assignment or if `config()` returned a wrong object.
- Line 207: `enabled` must be `false` → would fire only if `impairment_config_default()` was wrong or if an earlier code path in `init()` set `enabled=true`.
- Line 208: same as 207 for `engine2`.

Internal `NEVER_COMPILED_OUT_ASSERT` calls inside `init()` and `seed()` (at least 6 across both engines) would also trigger abort-or-reset if preconditions or postconditions were violated.

No `Logger::log()` calls occur on this path. `ImpairmentEngine::init()` emits no log messages; the impairment processing path that does log is never entered.

---

## 10. External Interactions

No external interactions occur on this execution path.

| Interaction | Status |
|---|---|
| Sockets / network | None |
| File I/O | None |
| OS timers / `clock_gettime` | None (`timestamp_now_us()` is not called) |
| `Logger::log()` | None (init emits no log; no impairment decisions made) |
| `memset()` | Called 4 times (×2 per engine: once in constructor, once in `init()`). CRT function, not an OS interaction. |
| `assert()` (standard C) | Called 3 times in test body. Invokes `abort()` on failure (CRT). |
| `printf()` | Called by `main()` (not by `test_prng_deterministic()`) to print `"PASS: ..."`. |

---

## 11. State Changes / Side Effects

| Object | State after constructor (before `init()`) | State after `init()` |
|---|---|---|
| `engine1.m_initialized` | `false` | `true` |
| `engine1.m_prng.m_state` | `1ULL` (constructor's `seed(1ULL)`) | `12345ULL` (init's `seed(12345ULL)`) |
| `engine1.m_cfg` | Value-initialized (all zeros/false) | Copy of `cfg1`: `prng_seed=12345ULL`, `enabled=false`, all impairments off |
| `engine1.m_delay_buf[0..31]` | Zeroed by constructor `memset` | Zeroed again by `init()` `memset` |
| `engine1.m_delay_count` | `0` | `0` |
| `engine1.m_reorder_buf[0..31]` | Zeroed by constructor `memset` | Zeroed again by `init()` `memset` |
| `engine2.*` | Same as engine1 before init | Identical final state to engine1 |

**Side effects visible outside the test:**
- stdout: `"PASS: test_prng_deterministic\n"` printed by `main()` if the test returns `true`.
- Both engine objects are destroyed when `test_prng_deterministic()` returns; no persistent state remains.
- No log output is produced by `test_prng_deterministic()` itself.

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant main
    participant test as test_prng_deterministic()
    participant e1 as ImpairmentEngine engine1
    participant p1 as PrngEngine (in engine1)
    participant e2 as ImpairmentEngine engine2
    participant p2 as PrngEngine (in engine2)

    main->>test: call test_prng_deterministic()

    note over test,p1: engine1 construction
    test->>e1: ImpairmentEngine() constructor
    e1->>p1: seed(1ULL)
    note over p1: m_state = 1ULL (preliminary)
    e1-->>test: constructed

    note over test: impairment_config_default(cfg1) → prng_seed=42ULL<br/>cfg1.prng_seed = 12345ULL (override)

    test->>e1: init(cfg1)
    note over e1: m_cfg = cfg1; seed=12345ULL (non-zero, no coercion)
    e1->>p1: seed(12345ULL)
    note over p1: m_state = 12345ULL (overwrites 1ULL)
    p1-->>e1: done
    note over e1: memset bufs; m_initialized=true
    e1-->>test: Result::OK

    note over test,p2: engine2 construction
    test->>e2: ImpairmentEngine() constructor
    e2->>p2: seed(1ULL)
    note over p2: m_state = 1ULL (preliminary)
    e2-->>test: constructed

    note over test: impairment_config_default(cfg2) → prng_seed=42ULL<br/>cfg2.prng_seed = 12345ULL (override)

    test->>e2: init(cfg2)
    note over e2: m_cfg = cfg2; seed=12345ULL
    e2->>p2: seed(12345ULL)
    note over p2: m_state = 12345ULL
    p2-->>e2: done
    note over e2: memset bufs; m_initialized=true
    e2-->>test: Result::OK

    note over test: assert(engine1.config().prng_seed == engine2.config().prng_seed)<br/>12345ULL == 12345ULL → passes
    note over test: assert(engine1.config().enabled == false) → passes
    note over test: assert(engine2.config().enabled == false) → passes

    test-->>main: true
    main->>main: printf("PASS: test_prng_deterministic")
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase (what this test exercises entirely):**
- `ImpairmentEngine` constructor: `m_prng.seed(1ULL)`, `memset` both buffers, `m_initialized=false`.
- `impairment_config_default()`: sets all fields to safe defaults.
- Caller overrides `prng_seed` to `12345ULL`.
- `ImpairmentEngine::init()`: copies config, resolves seed (`12345ULL` → non-zero → used directly), calls `m_prng.seed(12345ULL)`, re-zeros buffers, sets `m_initialized=true`.

Everything in this test is initialization-phase activity. No messages are sent, no impairment decisions are made, and no PRNG values are consumed beyond seeding.

**Runtime phase (NOT exercised by this test):**
- `process_outbound()` → `check_loss()` → `m_prng.next_double()` (consumes PRNG for loss decision)
- `process_outbound()` → `apply_duplication()` → `m_prng.next_double()` (duplication decision)
- `process_outbound()` → jitter calculation → `m_prng.next_range(0, jitter_variance_ms)` (jitter)
- `collect_deliverable()` → scans delay buffer for entries with `release_us <= now_us`

**Correctness argument for determinism (by inspection):**
Given equal `prng_seed` values passed to `PrngEngine::seed()`, both engines have equal `m_state`. The xorshift64 algorithm (`x ^= x<<13; x ^= x>>7; x ^= x<<17`) is a pure bijection on `uint64_t` — its output is a deterministic function of `m_state` alone, with no global state or external input. Therefore equal `m_state` → equal `next()` sequences → equal impairment decisions under equal message inputs.

---

## 14. Known Risks / Observations

**Risk 1 — Indirect validation only (primary risk).**
The test does not call `next()` on either engine and does not compare raw output sequences. It asserts config-level seed equality (line 201) and `enabled=false` (lines 207–208). A regression that correctly stores the seed in `m_cfg` but passes a wrong value to `PrngEngine::seed()` — for example, an off-by-one or a hardcoded default — would not be caught by this test. The full determinism proof requires `m_prng.m_state` to be verified directly, which is impossible from test code because `m_prng` is a private member. **Recommendation:** Add a complementary test that initializes two engines with the same seed, processes identical messages through `process_outbound()` with `loss_probability > 0`, and compares the sequence of `Result::ERR_IO` / `Result::OK` outcomes.

**Risk 2 — PrngEngine is an opaque private member.**
`m_prng` is `private` inside `ImpairmentEngine`. White-box verification of `m_state` equality is impossible without exposing a test accessor or using `friend` declarations. The test is limited to observing externally visible config fields.

**Risk 3 — Naming confusion: `test_prng_deterministic` vs `test_loss_deterministic`.**
`test_loss_deterministic()` (lines 81–112 in the same file) actually demonstrates deterministic *behavior* by verifying that `loss_probability=1.0` drops all messages. It uses a single engine seeded at 42 and does not compare two independent engines. `test_prng_deterministic()` tests only config-level seed propagation. The names are misleading: the "loss deterministic" test provides stronger behavioral evidence; the "PRNG deterministic" test provides weaker structural evidence.

**Risk 4 — Large stack allocation.**
Two `ImpairmentEngine` objects on the test stack. Each contains `m_delay_buf[32]` and `m_reorder_buf[32]`, where each element holds a full `MessageEnvelope` with a `MSG_MAX_PAYLOAD_BYTES=4096`-byte payload array. Total stack usage is approximately 528 KB. On POSIX platforms with the default 8 MB thread stack this is safe. On embedded targets with stacks ≤ 64 KB it would overflow.

**Risk 5 — Constructor pre-seeding creates an intermediate state.**
The constructor seeds `m_prng` to `1ULL` before `init()` re-seeds it to `12345ULL`. If a method that consumes the PRNG (e.g., `process_outbound()`) were called between construction and `init()`, the sequence would start from a different state. The `NEVER_COMPILED_OUT_ASSERT(m_initialized)` guard at the top of `process_outbound()` prevents this at runtime; the test cannot accidentally trigger this path.

**Risk 6 — Test assertions use standard `assert()` (NDEBUG-disableable).**
The three assertions in the test body (lines 201, 207, 208) use the standard C `assert()` macro, which is disabled when compiled with `-DNDEBUG`. In a release-mode test build the assertions would be no-ops and the test would pass regardless of engine state. Internal engine assertions use `NEVER_COMPILED_OUT_ASSERT()` which is always active. Test assertions should ideally also use an always-active check.

**Risk 7 — Seed coercion path not covered.**
Neither `ImpairmentEngine::init()`'s coercion (`prng_seed == 0 → 42`) nor `PrngEngine::seed()`'s coercion (`s == 0 → 1`) is exercised by this test. A separate test with `prng_seed=0ULL` is needed to cover those branches. The coercion test case is documented in UC_29.

---

## 15. Unknowns / Assumptions

`ImpairmentEngine` has a user-provided constructor [`ImpairmentEngine.cpp:26`] that initializes all fields to known safe values and pre-seeds `m_prng` to `1ULL`. This has been confirmed by reading the source.

`MessageEnvelope` size is dominated by its `4096`-byte payload array (`MSG_MAX_PAYLOAD_BYTES`, `Types.hpp`). Each `ImpairmentEngine` occupies approximately 264 KB on the stack. Two engines require approximately 528 KB. Confirmed by reading struct definitions; exact alignment padding is compiler-dependent.

The xorshift64 algorithm in `PrngEngine::next()` uses the constants `(<<13, >>7, <<17)`, confirmed by reading `PrngEngine.hpp:73–75`. These are the Marsaglia canonical xorshift64 constants for maximum period (2^64 − 1). The algorithm is a pure bijection on non-zero 64-bit state; determinism follows directly.

`IMPAIR_DELAY_BUF_SIZE = 32` confirmed by `Types.hpp:28`.

The test description ("call `next()` N times and compare sequences") describes design intent for the use case goal, not the literal test code. The actual test code (lines 186–211) does not call `next()`. This distinction is documented explicitly throughout this UC.

`[ASSUMPTION]` The compiler-generated destructor for `ImpairmentEngine` is trivial (no explicit destructor defined). Destruction of the two engine objects produces no logging and no side effects. Confirmed by the absence of `~ImpairmentEngine()` in the read source files.

`[ASSUMPTION]` `assert()` in the test body maps to the standard C `assert()` macro (from `<cassert>`), which is disabled by `NDEBUG`. The test suite is expected to be compiled without `NDEBUG` in CI. If compiled with `NDEBUG`, the three test assertions would be silently skipped.
