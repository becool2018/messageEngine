# UC_29 — PRNG seeded with known value before test run

**HL Group:** HL-13 — Deterministic Impairment Replay
**Actor:** User
**Requirement traceability:** REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

- **Trigger:** User sets `ImpairmentConfig::prng_seed` to a known non-zero value before calling `TcpBackend::init()` (or any backend `init()`). File: `src/platform/ImpairmentEngine.cpp`, `src/platform/PrngEngine.hpp`.
- **Goal:** Ensure that the `ImpairmentEngine`'s PRNG starts from a known state, so every loss, jitter, and duplication decision is reproducible for a given input stream.
- **Success outcome:** After `init()`, the `PrngEngine::m_state` is set to `prng_seed` (coerced to 1 if 0). All subsequent `next()` / `next_double()` / `next_range()` calls produce a deterministic sequence.
- **Error outcomes:** None — `seed()` always succeeds.

---

## 2. Entry Points

```cpp
// src/platform/PrngEngine.hpp (inline)
void PrngEngine::seed(uint64_t s);
```

Called from `ImpairmentEngine` initialization when `prng_seed` is set in `ImpairmentConfig`.

---

## 3. End-to-End Control Flow

1. User sets `cfg.impairment.prng_seed = 12345ULL` (or any known value).
2. User calls `backend.init(cfg)`.
3. Inside `TcpBackend::init()`: `m_impairment.configure(cfg.impairment)` is called.
4. Inside `ImpairmentEngine::configure()`: **`m_prng.seed(cfg.prng_seed)`** is called.
5. Inside `PrngEngine::seed(s)`:
   a. `m_state = (s == 0ULL) ? 1ULL : s` — coerce 0 to 1 (xorshift64 requires non-zero state).
   b. `NEVER_COMPILED_OUT_ASSERT(m_state != 0)`.
6. `PrngEngine` is now in a fully deterministic state.
7. Every subsequent call to `next()` applies: `m_state ^= m_state << 13; m_state ^= m_state >> 7; m_state ^= m_state << 17; return m_state;`
8. `next_double()` = `(double)(next() >> 11) / 9007199254740992.0` — 53-bit mantissa; uniform in `[0, 1)`.
9. `next_range(min, max)` = `min + (next() % (max - min))`.

---

## 4. Call Tree

```
TcpBackend::init(cfg)                        [TcpBackend.cpp]
 └── ImpairmentEngine::configure(cfg.impairment)  [ImpairmentEngine.cpp]
      └── PrngEngine::seed(cfg.prng_seed)    [PrngEngine.hpp]
           [m_state = prng_seed; coerce 0->1]
```

---

## 5. Key Components Involved

- **`PrngEngine`** — xorshift64 PRNG. Non-zero initial state is mandatory; `seed()` enforces this.
- **`ImpairmentEngine`** — Owns `m_prng`. Seeds it from `ImpairmentConfig::prng_seed` during configuration.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `s == 0` | `m_state = 1` (xorshift64 cannot use 0) | `m_state = s` |

---

## 7. Concurrency / Threading Behavior

- `seed()` is called once at init time; single-threaded.
- `m_state` is a plain `uint64_t`; not thread-safe during operation.

---

## 8. Memory & Ownership Semantics

- `PrngEngine::m_state` — 8-byte member of `PrngEngine` which is a value member of `ImpairmentEngine`. No heap allocation.

---

## 9. Error Handling Flow

- No error states. `seed()` always succeeds.

---

## 10. External Interactions

- None — pure computation; no OS calls.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | undefined | `prng_seed` (or 1 if seed was 0) |

---

## 12. Sequence Diagram

```
User
  [cfg.impairment.prng_seed = 12345ULL]
  -> TcpBackend::init(cfg)
       -> ImpairmentEngine::configure(cfg.impairment)
            -> PrngEngine::seed(12345ULL)
                 [m_state = 12345ULL]
  [PRNG deterministic from this point]
  -> ImpairmentEngine::process_outbound(env, ...)
       -> PrngEngine::next_double()    [deterministic value based on seed]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `ImpairmentConfig.prng_seed` set before calling backend `init()`.

**Runtime:**
- After seeding, `m_state` advances by one xorshift64 step per `next()` call. The full deterministic sequence is `seed -> xorshift64(seed) -> xorshift64^2(seed) -> ...`.

---

## 14. Known Risks / Observations

- **Seed = 0 silently coerced to 1:** User setting `prng_seed = 0` does not produce a random seed; it produces the fixed seed 1. The behavior is documented in `PrngEngine::seed()`.
- **Not cryptographically secure:** xorshift64 is a statistical PRNG suitable for simulation, not for security.
- **Reproducibility requires identical call sequence:** The PRNG produces the same sequence only if `process_outbound()` is called the same number of times with the same impairment configuration. Adding or removing impairment-check calls changes the sequence even with the same seed.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `ImpairmentEngine::configure()` calls `m_prng.seed(cfg.prng_seed)` as part of loading the `ImpairmentConfig`. This is confirmed by reading `ImpairmentEngine.cpp`.
