# UC_49 — timestamp_deadline_us(): compute an absolute expiry deadline from current time and a duration in milliseconds

**HL Group:** HL-22 — Read Monotonic Time and Compute Deadlines
**Actor:** User
**Requirement traceability:** REQ-3.2.2, REQ-3.2.5, REQ-3.3.4

---

## 1. Use Case Overview

- **Trigger:** A component calls `timestamp_deadline_us(duration_ms)` to compute an absolute deadline `timeout_ms` milliseconds from now. File: `src/core/Timestamp.hpp` or `src/core/Timestamp.cpp`.
- **Goal:** Return an absolute deadline in monotonic microseconds suitable for comparison with future `timestamp_now_us()` values. Used to set `expiry_time_us` on outbound envelopes and to compute polling deadlines in `receive()`.
- **Success outcome:** Returns `timestamp_now_us() + (uint64_t)duration_ms * 1000ULL`.
- **Error outcomes:** None — pure arithmetic after reading the clock. Inherits the clock-failure behaviour of `timestamp_now_us()`.

---

## 2. Entry Points

```cpp
// src/core/Timestamp.hpp
uint64_t timestamp_deadline_us(uint32_t duration_ms);
```

---

## 3. End-to-End Control Flow

1. **`timestamp_deadline_us(duration_ms)`** — entry.
2. `uint64_t now = timestamp_now_us()`.
3. Return `now + (uint64_t)duration_ms * 1000ULL`.

---

## 4. Call Tree

```
timestamp_deadline_us(duration_ms)                 [Timestamp.hpp]
 └── timestamp_now_us()                            [Timestamp.hpp — reads CLOCK_MONOTONIC]
```

---

## 5. Key Components Involved

- **`timestamp_now_us()`** — provides the monotonic base time (see UC_48).
- **Duration conversion** — `duration_ms * 1000` converts milliseconds to microseconds; result is the absolute deadline.

---

## 6. Branching Logic / Decision Points

No branching. Pure arithmetic.

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `duration_ms == 0` | Returns `timestamp_now_us()` (already expired) | Returns future deadline |

---

## 7. Concurrency / Threading Behavior

- Thread-safe (delegates to `timestamp_now_us()` which is thread-safe).
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

- `::clock_gettime(CLOCK_MONOTONIC)` — called indirectly via `timestamp_now_us()`.

---

## 11. State Changes / Side Effects

None. Pure time computation; no state is modified.

---

## 12. Sequence Diagram

```
[DeliveryEngine::send(env) — setting expiry]
  -> timestamp_deadline_us(cfg.expiry_ms)
       -> timestamp_now_us()
            -> ::clock_gettime(CLOCK_MONOTONIC, &ts)
            <- now_us
       <- now_us + duration_ms * 1000
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

- `[ASSUMPTION]` `timestamp_deadline_us()` is a two-line inline wrapper around `timestamp_now_us()` plus the multiplication; no caching of the "now" value.
- `[ASSUMPTION]` The conversion uses `(uint64_t)duration_ms * 1000ULL` to prevent 32-bit overflow before widening.
