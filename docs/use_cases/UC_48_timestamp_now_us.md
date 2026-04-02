# UC_48 — timestamp_now_us(): read CLOCK_MONOTONIC in microseconds for use in send, receive, pump, and sweep calls

**HL Group:** HL-22 — Read Monotonic Time and Compute Deadlines
**Actor:** User
**Requirement traceability:** REQ-3.2.2, REQ-3.2.5, REQ-3.3.4

---

## 1. Use Case Overview

- **Trigger:** Any component calls `timestamp_now_us()` to read the current monotonic clock. File: `src/core/Timestamp.hpp` (inline) or `src/core/Timestamp.cpp`.
- **Goal:** Return the current time in microseconds from `CLOCK_MONOTONIC` for use as a consistent, non-decreasing time base in message expiry, ACK timeout tracking, retry backoff, and impairment delay scheduling.
- **Success outcome:** Returns `uint64_t` microseconds since an arbitrary monotonic epoch. Consistent across all call sites within one process.
- **Error outcomes:** If `clock_gettime()` fails (extremely rare), behavior is implementation-defined. The function asserts or logs `FATAL` since a clock failure is unrecoverable.

---

## 2. Entry Points

```cpp
// src/core/Timestamp.hpp
uint64_t timestamp_now_us();
```

---

## 3. End-to-End Control Flow

1. **`timestamp_now_us()`** — entry.
2. `struct timespec ts`.
3. `::clock_gettime(CLOCK_MONOTONIC, &ts)` — POSIX call to read monotonic clock.
4. Check return value: if non-zero (failure): `NEVER_COMPILED_OUT_ASSERT(false)` (or log FATAL + return 0).
5. Return `(uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL)`.

---

## 4. Call Tree

```
timestamp_now_us()                                 [Timestamp.hpp / Timestamp.cpp]
 └── ::clock_gettime(CLOCK_MONOTONIC, &ts)         [POSIX]
```

---

## 5. Key Components Involved

- **`clock_gettime(CLOCK_MONOTONIC)`** — POSIX monotonic clock; never set backwards by the OS; unaffected by wall-clock adjustments (NTP, daylight saving). Required property for correct timeout and expiry tracking.
- **Microsecond conversion** — `tv_sec * 1_000_000 + tv_nsec / 1000`: standard ns→µs truncation.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `clock_gettime()` returns non-zero | FATAL / assert | Return computed microseconds |

---

## 7. Concurrency / Threading Behavior

- `clock_gettime(CLOCK_MONOTONIC, ...)` is thread-safe on all POSIX platforms.
- Safe to call from multiple threads simultaneously.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `struct timespec ts` — 16-byte stack allocation.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- `clock_gettime()` failure is treated as unrecoverable (`NEVER_COMPILED_OUT_ASSERT` or FATAL log + abort). The time source is a fundamental system dependency; continuing with a stale or zero timestamp would silently corrupt all timeout and expiry logic.

---

## 10. External Interactions

- **`clock_gettime(CLOCK_MONOTONIC)`** — single POSIX system call.

---

## 11. State Changes / Side Effects

None. Pure time-reading function; no state is modified.

---

## 12. Sequence Diagram

```
[DeliveryEngine::receive() / send() / pump_retries() / sweep_ack_timeouts()]
  -> timestamp_now_us()
       -> ::clock_gettime(CLOCK_MONOTONIC, &ts)
       <- ts.tv_sec, ts.tv_nsec
  <- uint64_t microseconds
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** POSIX system with `CLOCK_MONOTONIC` support (standard on Linux and macOS).

**Runtime:** Called on every `send()`, `receive()`, `pump_retries()`, and `sweep_ack_timeouts()` call to establish the current time basis. Also called by `ImpairmentEngine` for delay scheduling.

---

## 14. Known Risks / Observations

- **Wrap-around:** `uint64_t` microseconds wraps after ~584,542 years; not a practical concern.
- **Resolution:** POSIX does not guarantee nanosecond resolution; actual resolution is platform-dependent (typically 1 µs on Linux with `CLOCK_MONOTONIC`).
- **CLOCK_MONOTONIC vs CLOCK_REALTIME:** `CLOCK_MONOTONIC` is used (not `CLOCK_REALTIME`) to prevent time jumps from NTP or system clock changes from corrupting retry and expiry logic.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `timestamp_now_us()` is implemented as a short inline function or a thin wrapper; no buffering or caching of the clock value.
- `[ASSUMPTION]` Failure of `clock_gettime()` triggers `NEVER_COMPILED_OUT_ASSERT(false)` rather than returning 0 silently, consistent with the assertion policy in `CLAUDE.md §10`.
