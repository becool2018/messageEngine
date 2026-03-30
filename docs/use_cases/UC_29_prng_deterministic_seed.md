# UC_29 — PRNG deterministic seed

**HL Group:** HL-13 — User seeds the impairment engine for deterministic replay
**Actor:** System
**Requirement traceability:** REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

### What triggers this flow

`ImpairmentEngine::init(cfg)` is called with an `ImpairmentConfig` that carries a `prng_seed` value (zero or non-zero). The trigger is the init call itself, typically issued from `LocalSimHarness::init()` or directly from a test fixture.

### Expected outcome (single goal)

`PrngEngine::m_state` is set to a known, non-zero value deterministic from the provided seed. Every subsequent call to `m_prng.next()`, `next_double()`, or `next_range()` will produce the same sequence for the same seed across any number of process runs. The `ImpairmentEngine` is fully initialized and ready for `process_outbound()` calls.

---

## 2. Entry Points

**Primary entry point:**
- `ImpairmentEngine::init(const ImpairmentConfig& cfg)` — `src/platform/ImpairmentEngine.cpp`, line 44.

**Constructor (runs before init, seeds PRNG to 1ULL as a placeholder):**
- `ImpairmentEngine::ImpairmentEngine()` — `src/platform/ImpairmentEngine.cpp`, line 26.

**Seed delegation:**
- `PrngEngine::seed(uint64_t s)` — `src/platform/PrngEngine.hpp`, line 43 (inline).

**PRNG outputs consumed at runtime (after init):**
- `PrngEngine::next()` — `PrngEngine.hpp`, line 67.
- `PrngEngine::next_double()` — `PrngEngine.hpp`, line 91.
- `PrngEngine::next_range(lo, hi)` — `PrngEngine.hpp`, line 119.

**Configuration helper (called before init):**
- `impairment_config_default()` — `src/platform/ImpairmentConfig.hpp`, line 79.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase A — Configuration setup (caller, before init())**

1. Caller declares `ImpairmentConfig cfg`.
2. Caller calls `impairment_config_default(cfg)` (`ImpairmentConfig.hpp:79`). Sets all fields to safe defaults:
   - `enabled = false`, `fixed_latency_ms = 0U`, `jitter_mean_ms = 0U`, `jitter_variance_ms = 0U`
   - `loss_probability = 0.0`, `duplication_probability = 0.0`
   - `reorder_enabled = false`, `reorder_window_size = 0U`
   - `partition_enabled = false`, `partition_duration_ms = 0U`, `partition_gap_ms = 0U`
   - `prng_seed = 42ULL` — default deterministic seed.
3. Caller optionally overrides `cfg.prng_seed` with a test-specific value (e.g., `cfg.prng_seed = 0xDEADBEEFCAFEBABEULL`).

**Phase B — Constructor (runs automatically on object declaration)**

4. `ImpairmentEngine::ImpairmentEngine()` constructor (`ImpairmentEngine.cpp:26-38`):
   - Member initializer list: `m_cfg{}`, `m_delay_count(0U)`, `m_reorder_count(0U)`, `m_partition_active(false)`, `m_partition_start_us(0ULL)`, `m_next_partition_event_us(0ULL)`, `m_initialized(false)`.
   - `memset(m_delay_buf, 0, sizeof(m_delay_buf))`.
   - `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))`.
   - `m_prng.seed(1ULL)` — preliminary seed; will be overwritten by `init()`.
   - `NEVER_COMPILED_OUT_ASSERT(!m_initialized)`.
   - `NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U)`.

**Phase C — ImpairmentEngine::init()**

5. `ImpairmentEngine::init(cfg)` entered (`ImpairmentEngine.cpp:44`).

6. Two precondition assertions (`lines 47-48`):
   - `NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)` — 0 ≤ 32.
   - `NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0)`.

7. `m_cfg = cfg` — full struct copy of all `ImpairmentConfig` fields (line 51).

**Phase D — Seed resolution and PRNG seeding**

8. Seed resolution (`ImpairmentEngine.cpp:54-55`):
   ```
   uint64_t seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL;
   ```
   Decision table:
   - `cfg.prng_seed == 0` → `seed = 42ULL` (library default; coerces 0 to a defined value)
   - `cfg.prng_seed == 42` → `seed = 42ULL` (explicit match to default)
   - `cfg.prng_seed == <other>` → `seed = cfg.prng_seed`

9. `m_prng.seed(seed)` called (`PrngEngine.hpp:43`):
   - `if (s == 0ULL): m_state = 1ULL` — secondary safety net; should not fire given Step 8's guard.
   - `else: m_state = s` — the provided seed value is stored directly.
   - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)`.
   - Returns `void`.
   - After this call, `m_prng.m_state == seed` (for `seed != 0`).

**Phase E — Remainder of ImpairmentEngine::init()**

10. Zero-fill delay buffer: `memset(m_delay_buf, 0, sizeof(m_delay_buf))`, `m_delay_count = 0U`.
11. Zero-fill reorder buffer: `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))`, `m_reorder_count = 0U`.
12. Partition state initialization: `m_partition_active = false`, `m_partition_start_us = 0ULL`, `m_next_partition_event_us = 0ULL`. `is_partition_active()` initializes `m_next_partition_event_us` properly on its first call.
13. `m_initialized = true`.
14. Post-condition assertions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U)`.
15. `ImpairmentEngine::init()` returns `void`. After this point, the PRNG has not produced any output; its state is fully determined by the seed.

**Phase F — PRNG usage at runtime (illustrative)**

16. Caller later invokes `process_outbound(env, now_us)`. Inside `check_loss()` (`ImpairmentEngine.cpp:110`), if `loss_probability > 0.0`:
    - `double rand_val = m_prng.next_double()` (`PrngEngine.hpp:91`):
      - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)`.
      - `raw = next()` — executes the xorshift64 three-step transform on `m_state`.
      - `result = static_cast<double>(raw) / static_cast<double>(UINT64_MAX)` — scales to `[0.0, 1.0)`.
      - `NEVER_COMPILED_OUT_ASSERT(result >= 0.0 && result < 1.0)`.
      - Returns `result`.
    - `return rand_val < m_cfg.loss_probability`.
17. The sequence of values produced by `next()` is fully determined by `m_state` set in Step 9. Same seed → same sequence on every process run.

---

## 4. Call Tree (Hierarchical)

```
impairment_config_default(cfg)                          [ImpairmentConfig.hpp:79]
  └── cfg.prng_seed = 42ULL  (default)

[Optionally: cfg.prng_seed = <test seed>]

ImpairmentEngine::ImpairmentEngine()                    [ImpairmentEngine.cpp:26]
  ├── member initializers (m_delay_count=0, m_initialized=false, etc.)
  ├── memset(m_delay_buf, 0, ...)
  ├── memset(m_reorder_buf, 0, ...)
  ├── m_prng.seed(1ULL)                                 [PrngEngine.hpp:43]
  │   └── m_state = 1ULL
  ├── NEVER_COMPILED_OUT_ASSERT(!m_initialized)
  └── NEVER_COMPILED_OUT_ASSERT(IMPAIR_DELAY_BUF_SIZE > 0U)

ImpairmentEngine::init(cfg)                             [ImpairmentEngine.cpp:44]
  ├── [assertions] reorder_window_size, loss_probability
  ├── m_cfg = cfg  (struct copy)
  ├── seed = (cfg.prng_seed != 0) ? cfg.prng_seed : 42ULL
  ├── m_prng.seed(seed)                                 [PrngEngine.hpp:43]
  │   ├── if (s == 0ULL): m_state = 1ULL
  │   ├── else:           m_state = s
  │   └── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
  ├── memset(m_delay_buf, 0, sizeof(m_delay_buf))
  ├── m_delay_count = 0U
  ├── memset(m_reorder_buf, 0, sizeof(m_reorder_buf))
  ├── m_reorder_count = 0U
  ├── m_partition_active=false; m_partition_start_us=0; m_next_partition_event_us=0
  ├── m_initialized = true
  └── [assertions] m_initialized, m_delay_count == 0U

[Runtime — after init(), PRNG sequence starts here]
m_prng.next()                                           [PrngEngine.hpp:67]
  ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
  ├── m_state ^= m_state << 13U
  ├── m_state ^= m_state >> 7U
  ├── m_state ^= m_state << 17U
  ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
  └── return m_state

m_prng.next_double()                                    [PrngEngine.hpp:91]
  ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
  ├── raw = next()
  ├── result = (double)raw / (double)UINT64_MAX
  ├── NEVER_COMPILED_OUT_ASSERT(result in [0.0, 1.0))
  └── return result

m_prng.next_range(lo, hi)                               [PrngEngine.hpp:119]
  ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
  ├── NEVER_COMPILED_OUT_ASSERT(hi >= lo)
  ├── raw = next()
  ├── offset = (uint32_t)(raw % (uint64_t)(hi-lo+1))
  ├── result = lo + offset
  ├── NEVER_COMPILED_OUT_ASSERT(result in [lo, hi])
  └── return result
```

---

## 5. Key Components Involved

**`ImpairmentEngine` — `src/platform/ImpairmentEngine.cpp/.hpp`**
Top-level impairment coordinator. Owns `m_prng` as an inline value member. `init()` is the sole entry point for seed setup.

**`PrngEngine` — `src/platform/PrngEngine.hpp`**
Header-only inline class implementing the xorshift64 algorithm. State is a single `uint64_t m_state`. Methods: `seed(s)`, `next()`, `next_double()`, `next_range(lo, hi)`. All are safety-critical (SC), annotated HAZ-002, HAZ-007.

**`ImpairmentConfig` — `src/platform/ImpairmentConfig.hpp`**
POD struct containing all impairment parameters including `prng_seed`. `impairment_config_default()` sets `prng_seed = 42ULL`.

**`check_loss()` — `ImpairmentEngine.cpp:110`** (private helper)
Primary runtime consumer of `m_prng.next_double()`. Called by `process_outbound()` when `loss_probability > 0.0`.

**`apply_duplication()` — `ImpairmentEngine.cpp:127`** (private helper)
Secondary runtime consumer of `m_prng.next_double()`. Called by `process_outbound()` when `duplication_probability > 0.0`.

**`process_outbound()` — `ImpairmentEngine.cpp:151`** (public)
Also consumes `m_prng.next_range(0, jitter_variance_ms)` for jitter when `jitter_mean_ms > 0`.

---

## 6. Branching Logic / Decision Points

**Branch 1 — Seed coercion in `init()`** (`ImpairmentEngine.cpp:54`)
- Condition: `cfg.prng_seed == 0ULL`
- True: `seed = 42ULL` — library default; coerces 0 to a well-defined deterministic value.
- False: `seed = cfg.prng_seed` — test-provided value used directly.
- This is the primary branch controlling test determinism.

**Branch 2 — Zero-guard in `PrngEngine::seed()`** (`PrngEngine.hpp:46`)
- Condition: `s == 0ULL`
- True: `m_state = 1ULL` — secondary safety net; xorshift64 requires non-zero state.
- False: `m_state = s`.
- Under normal `ImpairmentEngine` usage, Branch 1 already prevents `s == 0` from reaching here.

**Branch 3 — Impairments enabled flag** (`ImpairmentEngine.cpp:159`)
- Condition: `m_cfg.enabled == false`
- True: PRNG is NOT consumed; message goes directly to delay buffer with `release_us = now_us`.
- False: impairments applied; PRNG IS consumed for loss, duplication, jitter.

**Branch 4 — Loss probability guard** (`ImpairmentEngine.cpp:115`)
- Condition: `m_cfg.loss_probability <= 0.0`
- True: `check_loss()` returns `false` immediately; PRNG NOT consumed.
- False: `m_prng.next_double()` called; one PRNG value consumed per message.

**Branch 5 — Jitter guard** (`ImpairmentEngine.cpp:185`)
- Condition: `m_cfg.jitter_mean_ms > 0U`
- True: `m_prng.next_range()` called; one PRNG value consumed per message.
- False: PRNG not consumed for jitter.

**Branch 6 — Duplication guard** (`ImpairmentEngine.cpp:203`)
- Condition: `m_cfg.duplication_probability > 0.0`
- True: `apply_duplication()` calls `m_prng.next_double()`; one PRNG value consumed.
- False: PRNG not consumed for duplication.

**Branch 7 — Partition guard** (`ImpairmentEngine.cpp:328`)
- Condition: `m_cfg.partition_enabled == false`
- True: `is_partition_active()` returns immediately; PRNG is NOT used for partition decisions.
- Partition timing is purely clock-driven (`m_cfg.partition_duration_ms`, `partition_gap_ms`).

---

## 7. Concurrency / Threading Behavior

`PrngEngine` is NOT thread-safe. `m_state` is a plain `uint64_t` with no atomic protection. The three-step XOR sequence in `next()` requires read-modify-write atomicity that a plain `uint64_t` does not provide.

`ImpairmentEngine::init()` is intended to complete before concurrent use begins. After `init()`, each call to `process_outbound()` mutates `m_state` via `next()`/`next_double()`/`next_range()`. Concurrent `process_outbound()` calls on the same instance produce a data race on `m_state`.

The design assumes single-threaded use of each `ImpairmentEngine` instance. This is consistent with the SPSC model used by `LocalSimHarness`'s `RingBuffer`.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

**`PrngEngine` state:** single `uint64_t m_state` (8 bytes). No dynamic allocation.
- Set to `1ULL` by the constructor's `m_prng.seed(1ULL)`.
- Overwritten to the config-derived seed value by `init()`'s `m_prng.seed(seed)`.

**`ImpairmentConfig` copy:** passed by `const` reference; copied into `m_cfg` at line 51 of `ImpairmentEngine.cpp`. After `init()` returns, `m_cfg` is independent of the caller's struct.

**Delay buffer:** `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` (32 entries) — inline in `ImpairmentEngine`. Each `DelayEntry` contains a `MessageEnvelope` (4096-byte payload array), `release_us` (`uint64_t`), `active` (`bool`). Per entry ~4129 bytes; total ~132 KB. Zero-filled by `memset` in both constructor and `init()`.

**Reorder buffer:** `m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]` (32 `MessageEnvelope` entries) — inline in `ImpairmentEngine`. ~132 KB. Zero-filled similarly.

**Total `ImpairmentEngine` stack footprint:** approximately 264 KB (dominated by two fixed arrays of 32 `MessageEnvelope` objects).

No heap allocation anywhere in the init or PRNG seeding path (Power of 10 Rule 3).

---

## 9. Error Handling Flow

`ImpairmentEngine::init()` is `void`; no `Result` is returned.

All error detection is via `NEVER_COMPILED_OUT_ASSERT()` — always active regardless of `NDEBUG`.

| Condition | Location | Effect |
|---|---|---|
| `cfg.reorder_window_size > IMPAIR_DELAY_BUF_SIZE` | `ImpairmentEngine.cpp:47` | Assert fires; program terminates. |
| `cfg.loss_probability` outside `[0.0, 1.0]` | `ImpairmentEngine.cpp:48` | Assert fires; program terminates. |
| `m_state == 0` after `seed()` | `PrngEngine.hpp:53` | Assert fires; but seed coercion prevents this in normal flow. |

At runtime, `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` in `next()`, `next_double()`, and `next_range()` guards against state corruption. The xorshift64 algorithm provably cannot reach 0 from a non-zero initial state (0 is a fixed point of the three XOR steps; the sequence has period 2^64 - 1 over all non-zero states).

**No `Logger::log()` is called during `ImpairmentEngine::init()`.** The effective seed is not observable at runtime without a debugger (see RISK-4).

---

## 10. External Interactions

During constructor and `init()`:
- No OS calls.
- No I/O.
- No network.
- No `Logger::log()` call inside `ImpairmentEngine::init()` itself.

During runtime PRNG consumption:
- `PrngEngine::next()` and its callers are pure computation with no external interactions.
- `Logger::log()` IS called inside `process_outbound()` on impairment events (partition drop, loss drop, duplication, delay buffer full), but those are runtime events, not init events.

---

## 11. State Changes / Side Effects

**Before constructor:**
All members undefined.

**After constructor (before `init()`):**

| Field | Value |
|---|---|
| `m_cfg` | `{}` (zero-initialized) |
| `m_prng.m_state` | `1ULL` |
| `m_delay_buf[0..31]` | all-zeros |
| `m_delay_count` | `0U` |
| `m_reorder_buf[0..31]` | all-zeros |
| `m_reorder_count` | `0U` |
| `m_partition_active` | `false` |
| `m_initialized` | `false` |

**After `init(cfg)` with, e.g., `cfg.prng_seed = 999`:**

| Field | Value |
|---|---|
| `m_cfg` | copy of `cfg` parameter |
| `m_prng.m_state` | `999ULL` |
| `m_delay_buf[0..31].active` | `false`; `release_us = 0`; `env` zeroed |
| `m_delay_count` | `0U` |
| `m_reorder_buf[0..31]` | all-zeros |
| `m_reorder_count` | `0U` |
| `m_partition_active` | `false` |
| `m_initialized` | `true` |

**After first call to `m_prng.next()` with seed=999:**
xorshift64 on 999:
- Step 1: `999 ^= 999 << 13` = `999 ^ 8183808` = `8182857`
- Step 2: `8182857 ^= 8182857 >> 7`
- Step 3: result `^= result << 17`
`m_state` transitions to the xorshift64 successor of 999. This value is deterministic and reproducible on every platform with conforming C++17.

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant Caller as Test/Caller
    participant IE as ImpairmentEngine
    participant PE as PrngEngine

    Caller->>IE: impairment_config_default(cfg)
    note over Caller: cfg.prng_seed = 42 (default)
    note over Caller: Optionally: cfg.prng_seed = SEED

    note over IE: [ImpairmentEngine eng;] -> constructor
    IE->>PE: seed(1ULL)
    PE-->>IE: m_state = 1ULL

    Caller->>IE: init(cfg)
    IE->>IE: assert bounds
    IE->>IE: m_cfg = cfg
    IE->>IE: seed = cfg.prng_seed != 0 ? prng_seed : 42
    IE->>PE: seed(seed)
    PE->>PE: if s==0: state=1; else: state=s
    PE->>PE: assert(state != 0)
    PE-->>IE: done
    IE->>IE: memset delay_buf; m_delay_count=0
    IE->>IE: memset reorder_buf; m_reorder_count=0
    IE->>IE: partition state = 0
    IE->>IE: m_initialized = true
    IE-->>Caller: (void)

    note over Caller: Later at runtime:
    Caller->>IE: process_outbound(env, now_us)
    IE->>PE: next_double() [via check_loss()]
    PE->>PE: state ^= state<<13; state ^= state>>7; state ^= state<<17
    PE-->>IE: return double in [0.0, 1.0)
    IE-->>Caller: Result::OK or ERR_IO
```

---

## 13. Initialization vs Runtime Flow

### Startup / initialization (this use case)

1. `ImpairmentEngine` constructor zeros state, seeds PRNG to `1ULL` (placeholder).
2. `impairment_config_default()` sets `prng_seed = 42` (or caller provides a custom seed).
3. `ImpairmentEngine::init(cfg)` copies config, resolves seed, calls `m_prng.seed(seed)`.
4. `m_prng.m_state` is set. All buffers zeroed. `m_initialized = true`.
5. After `init()` returns, the PRNG is seeded but has not yet produced output. The state is deterministic given the seed; the sequence is fully reproducible.

### Steady-state runtime (post-init)

PRNG consumers in order of call within `process_outbound()`:
1. `is_partition_active()` — does NOT consume PRNG (uses wall-clock only).
2. `check_loss()` — consumes one `next_double()` if `loss_probability > 0`.
3. `next_range(0, jitter_variance_ms)` — consumes one `next_range()` if `jitter_mean_ms > 0`.
4. `apply_duplication()` — consumes one `next_double()` if `duplication_probability > 0`.

With all three active, each `process_outbound()` call consumes up to 3 PRNG values. For identical seeds AND identical cfg AND identical message sequences, all impairment decisions reproduce exactly.

**xorshift64 algorithm (complete):**
Given state S (non-zero uint64\_t):
1. `S = S XOR (S << 13)`
2. `S = S XOR (S >> 7)`
3. `S = S XOR (S << 17)`
4. return S

Properties: period 2^64 - 1 (visits all non-zero 64-bit states exactly once); fixed point 0 (never reachable from non-zero initial state); no floating-point in the core step; shift constants (13, 7, 17) from Marsaglia's published maximal-period xorshift64 parameter table.

---

## 14. Known Risks / Observations

**RISK-1 — PRNG state not reset between tests if object is reused:**
If `init()` is called a second time with a different seed, `m_prng.m_state` is correctly re-seeded. But if `init()` is NOT called again and `process_outbound()` is called from a new test, the PRNG continues from where the previous test left off. This breaks determinism across tests on the same object instance.

**RISK-2 — Seed == 0 has two coercion layers:**
Both `ImpairmentEngine::init()` (to 42) and `PrngEngine::seed()` (to 1) coerce a zero seed to a different non-zero value. A test author who explicitly sets `prng_seed = 0` expecting "no seeding" will silently get seed = 42. The behavior is defined but non-obvious.

**RISK-3 — Reproducibility requires matching config:**
The PRNG is only consumed when the corresponding impairment is enabled (Branches 4–6). Different `cfg` values with the same seed produce different sequences of consumed random values. Tests must document both the seed AND the full `ImpairmentConfig` to guarantee reproducibility.

**RISK-4 — No `Logger::log()` on init:**
Unlike `LocalSimHarness`, `ImpairmentEngine::init()` emits no log message. The effective seed used (`42ULL` vs. user-provided) is not observable at runtime without a debugger.

**RISK-5 — `memset` on structs with non-trivially-constructible members:**
If `MessageEnvelope` or `DelayEntry` ever gains a non-trivially-constructible member (e.g., a `std::string` object), the `memset` will corrupt it. The current code is safe because all fields are POD.

**RISK-6 — Constructor pre-seeds PRNG to 1ULL:**
If `process_outbound()` were called before `init()` (a contract violation), the PRNG would produce a sequence from seed `1ULL`. The `NEVER_COMPILED_OUT_ASSERT(m_initialized)` in `process_outbound()` catches this at runtime in all builds.

---

## 15. Unknowns / Assumptions

`[CONFIRMED]` xorshift64 shift constants are (13, 7, 17) from `PrngEngine.hpp:73-75`. These match Marsaglia's maximal-period xorshift64 parameter table.

`[CONFIRMED]` `IMPAIR_DELAY_BUF_SIZE = 32` from `Types.hpp:28`.

`[CONFIRMED]` `UINT64_MAX` is available and correctly equals `2^64 - 1` on all C++17-conforming platforms (guaranteed by the standard).

`[CONFIRMED]` `impairment_config_default()` sets `prng_seed = 42ULL` (`ImpairmentConfig.hpp:92`). This is the fallback when the caller passes `cfg.prng_seed = 0`.

`[ASSUMPTION]` `MessageEnvelope` is a POD struct with a trivial zero-representation such that `memset` to 0 produces a valid "empty/uninitialized" envelope state.

`[UNKNOWN]` Whether `ImpairmentEngine::init()` is ever called more than once on the same object (e.g., to reset impairment state mid-test). The code supports it mechanically (all state is re-zeroed) but no test demonstrates this pattern.
