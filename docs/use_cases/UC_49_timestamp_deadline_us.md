# UC_49 — timestamp_deadline_us(): compute an absolute expiry deadline from current time and a duration in milliseconds

**HL Group:** HL-22 — Read Monotonic Time and Compute Deadlines
**Actor:** User
**Requirement traceability:** REQ-3.2.2, REQ-3.2.5, REQ-3.3.4

---

## 1. Use Case Overview

- **Trigger:** A component calls `timestamp_deadline_us(now_us, duration_ms)` to compute an absolute deadline `duration_ms` milliseconds from the provided `now_us` base time. File: `src/core/Timestamp.hpp`.
- **Goal:** Return an absolute deadline in monotonic microseconds suitable for comparison with future `timestamp_now_us()` values. Used to set `expiry_time_us` on outbound envelopes and to compute polling deadlines in `receive()`.
- **Success outcome:** Returns `timestamp_now_us() + (uint64_t)duration_ms * 1000ULL`.
- **Error outcomes:** None — pure arithmetic after reading the clock. Inherits the clock-failure behaviour of `timestamp_now_us()`.

---

## 2. Entry Points

```cpp
// src/core/Timestamp.hpp (inline)
uint64_t timestamp_deadline_us(uint64_t now_us, uint32_t duration_ms);
```

---

## 3. End-to-End Control Flow

1. **`timestamp_deadline_us(now_us, duration_ms)`** — entry. `now_us` is the current monotonic time supplied by the caller.
2. Return `now_us + (uint64_t)duration_ms * 1000ULL`. No internal clock read; the caller is responsible for providing a fresh `now_us` from `timestamp_now_us()`.

---

## 4. Call Tree

```
timestamp_deadline_us(now_us, duration_ms)          [Timestamp.hpp inline]
 └── (pure arithmetic: now_us + duration_ms * 1000ULL)
```

---

## 5. Key Components Involved

- **`now_us` parameter** — caller-supplied monotonic base time (from `timestamp_now_us()`); no internal clock read.
- **Duration conversion** — `duration_ms * 1000` converts milliseconds to microseconds; result is the absolute deadline.

---

## 6. Branching Logic / Decision Points

No branching. Pure arithmetic.

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `duration_ms == 0` | Returns `timestamp_now_us()` (already expired) | Returns future deadline |

---

## 7. Concurrency / Threading Behavior

- Thread-safe (pure arithmetic on immutable inputs; no shared state).
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- No heap allocation. Power of 10 Rule 3 compliant.
- Returns `uint64_t` by value.

---

## 9. Error Handling Flow

- Inherits clock failure handling from `timestamp_now_us()`.
- Arithmetic overflow: `uint64_t` result for reasonable `duration_ms` values (up to ~584 billion ms) cannot overflow in practice.

---

## 10. External Interactions

- None — pure arithmetic; no OS calls. The caller is responsible for reading the clock via `timestamp_now_us()`.

---

## 11. State Changes / Side Effects

None. Pure time computation; no state is modified.

---

## 12. Sequence Diagram

```
[DeliveryEngine::send(env) — setting expiry]
  -> timestamp_deadline_us(now_us, cfg.expiry_ms)
       <- now_us + cfg.expiry_ms * 1000ULL
  [env.expiry_time_us = deadline]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** `CLOCK_MONOTONIC` available (see UC_48).

**Runtime:** Called during `send()` to set `expiry_time_us` on outbound envelopes, and in `receive()` to compute the polling loop deadline.

---

## 14. Known Risks / Observations

- **Consistency with `timestamp_expired()`:** `timestamp_expired(expiry_time_us)` returns true when `timestamp_now_us() >= expiry_time_us`. A deadline set with `timestamp_deadline_us(0)` is immediately expired. Zero-duration deadlines should be avoided for live messages.
- **duration_ms = 0 edge case:** Returns the current time; the message is already expired by the time it is set. Callers must use non-zero durations for live messages.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `timestamp_deadline_us(now_us, duration_ms)` is a one-line inline: `return now_us + (uint64_t)duration_ms * 1000ULL`. The caller supplies `now_us` to avoid a redundant clock read and to keep the computation deterministic in tests.
- `[ASSUMPTION]` The conversion uses `(uint64_t)duration_ms * 1000ULL` to prevent 32-bit overflow before widening.
