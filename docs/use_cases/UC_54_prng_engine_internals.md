# UC_54 — PrngEngine next/next_double/next_range: xorshift64 PRNG called internally by ImpairmentEngine

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-5.2.4, REQ-5.3.1

This is a System Internal use case. `PrngEngine` is owned by `ImpairmentEngine` and called on every loss/duplication/jitter decision. The User never calls these methods directly; they interact with the PRNG only via `ImpairmentEngine::init(config)` and `PrngEngine::seed()` (covered in UC_29).

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` or `process_inbound()` calls `m_prng.next()`, `m_prng.next_double()`, or `m_prng.next_range(min, max)` to produce a random value for an impairment decision.
- **Goal:** Advance the xorshift64 PRNG state and return the next value in the deterministic sequence. For the same seed and call sequence, the output is always identical (REQ-5.3.1).
- **Success outcome:** Returns a `uint64_t` (from `next()`), a `double` in `[0.0, 1.0)` (from `next_double()`), or an `int32_t` in `[min, max]` (from `next_range()`).
- **Error outcomes:** None.

---

## 2. Entry Points

```cpp
// src/core/PrngEngine.hpp (inline)
uint64_t  PrngEngine::next();
double    PrngEngine::next_double();
int32_t   PrngEngine::next_range(int32_t min, int32_t max);
```

---

## 3. End-to-End Control Flow

**`PrngEngine::next()`:**
1. `NEVER_COMPILED_OUT_ASSERT(m_state != 0)`.
2. `m_state ^= m_state << 13`.
3. `m_state ^= m_state >> 7`.
4. `m_state ^= m_state << 17`.
5. Return `m_state`.

**`PrngEngine::next_double()`:**
1. `uint64_t r = next()`.
2. Return `(double)(r >> 11) * (1.0 / (double)(1ULL << 53))`.
   - Right-shift discards low 11 bits; divides by 2^53 to produce a value in `[0.0, 1.0)`.

**`PrngEngine::next_range(min, max)`:**
1. `NEVER_COMPILED_OUT_ASSERT(min <= max)`.
2. `uint64_t r = next()`.
3. `int32_t range = max - min + 1`.
4. Return `min + (int32_t)(r % (uint64_t)range)`.

---

## 4. Call Tree

```
PrngEngine::next_double()                          [PrngEngine.hpp]
 └── PrngEngine::next()                            [xorshift64 state advance]

PrngEngine::next_range(min, max)                   [PrngEngine.hpp]
 └── PrngEngine::next()                            [xorshift64 state advance]
```

---

## 5. Key Components Involved

- **`m_state` (uint64_t)** — xorshift64 PRNG state; never zero (invariant maintained by `seed()`).
- **xorshift64 shifts** — `<< 13`, `>> 7`, `<< 17` — standard xorshift64 parameters; full period of 2^64 - 1.
- **`next_double()` normalization** — `>> 11` then `/ 2^53` maps the 53 most-significant bits of the 64-bit value to `[0.0, 1.0)` using IEEE 754 double mantissa precision.
- **`next_range()` modulo** — `% range` reduces to `[0, range-1]`; slight modulo bias for large ranges, acceptable for impairment simulation purposes.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_state == 0` | NEVER_COMPILED_OUT_ASSERT fires | xorshift64 proceeds |
| `min > max` | NEVER_COMPILED_OUT_ASSERT fires | Compute range |

---

## 7. Concurrency / Threading Behavior

- `m_state` is a plain `uint64_t` member; not atomic.
- `ImpairmentEngine` owns one `PrngEngine`; both must be accessed from a single thread.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `m_state` — 8 bytes; member of `PrngEngine` which is a member of `ImpairmentEngine`.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states. Assertion on zero state and on invalid range.

---

## 10. External Interactions

- None — pure arithmetic in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S (non-zero) | S' (non-zero, xorshift64 advanced) |

---

## 12. Sequence Diagram

```
[ImpairmentEngine::process_outbound()]
  -> PrngEngine::next_double()
       -> PrngEngine::next()
            [m_state ^= m_state << 13]
            [m_state ^= m_state >>  7]
            [m_state ^= m_state << 17]
            <- new m_state
       [normalize to [0.0, 1.0)]
       <- 0.37
  [0.37 < loss_probability (0.5) -> DROP]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `PrngEngine::seed(value)` must have been called with a non-zero seed before any `next()` call (see UC_29).

**Runtime:**
- Called on every impairment decision during `process_outbound()` and `process_inbound()`. Typically 1–3 calls per message depending on which impairments are enabled.

---

## 14. Known Risks / Observations

- **Zero state invariant:** xorshift64 with state 0 produces 0 forever. `seed()` coerces 0→1. If `m_state` were ever cleared to 0 outside of `seed()` (e.g., by a bad memset), all subsequent `next()` calls would return 0, producing predictably wrong impairment decisions (e.g., 0.0 < any positive probability → always drop).
- **Modulo bias in `next_range()`:** For very large ranges (approaching 2^32), the bias from `% range` is non-negligible. For the jitter use case (ranges of tens of milliseconds), the bias is negligible.
- **Determinism:** The same state produces the same sequence regardless of platform, provided `uint64_t` arithmetic is the same. This is guaranteed for two's-complement 64-bit unsigned arithmetic on all conforming C++ implementations.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` xorshift64 shift parameters are `13, 7, 17`; this is one of the standard parameter sets with full period.
- `[ASSUMPTION]` `next_double()` uses the `>> 11` + `/ 2^53` normalization to use 53 mantissa bits; this is the standard double-precision PRNG normalization technique.
- `[ASSUMPTION]` `PrngEngine::next()` is an inline function in `PrngEngine.hpp`; the xorshift64 body is always inlined into the call sites.
