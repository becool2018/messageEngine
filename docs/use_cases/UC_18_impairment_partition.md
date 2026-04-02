# UC_18 — Partition: all traffic blocked for partition_duration_ms, then released for partition_gap_ms

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.6, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` or `process_inbound()` is called when `m_cfg.partition_enabled == true`. `is_partition_active(now_us)` determines whether the current time falls within a partition window. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Drop all outbound (and inbound) traffic for `partition_duration_ms` milliseconds, then allow traffic for `partition_gap_ms` milliseconds, cycling repeatedly to simulate intermittent network outages.
- **Success outcome (partition active):** All messages are dropped; `*out_count = 0`. Returns `Result::OK`.
- **Success outcome (partition gap):** Messages are processed normally through the loss/duplication/latency pipeline.
- **Error outcomes:** None — always returns `Result::OK`.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp
bool ImpairmentEngine::is_partition_active(uint64_t now_us);
```

Called from `process_outbound()` and `process_inbound()` at the top of each function.

---

## 3. End-to-End Control Flow

1. `process_outbound(env, now_us, out_buf, &out_count)` is called.
2. **`is_partition_active(now_us)`** is called:
   a. If `!m_cfg.partition_enabled`: return `false`.
   b. Compute `cycle_us = (partition_duration_ms + partition_gap_ms) * 1000ULL`.
   c. `phase_us = now_us % cycle_us` (position within current cycle).
   d. If `phase_us < partition_duration_ms * 1000ULL`: return `true` (in partition).
   e. Otherwise: return `false` (in gap).
3. If `is_partition_active()` returns `true`:
   - `Logger::log(WARNING_LO, "ImpairmentEngine", "Partition active: dropping outbound message")`.
   - Returns `Result::OK` with `*out_count = 0`. No further processing.
4. If `false`: continue to loss check and subsequent impairments.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound()            [ImpairmentEngine.cpp]
 └── is_partition_active(now_us)                [ImpairmentEngine.cpp]
      [returns true during partition window]
      [returns false during gap window]
```

---

## 5. Key Components Involved

- **`is_partition_active()`** — Modulo-arithmetic check against a repeating cycle of `(duration + gap)`. No state machine required; the partition state is derived from `now_us` alone.
- **`process_outbound()` / `process_inbound()`** — Both check partition at the top; a partition drops all traffic in both directions.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!partition_enabled` | is_partition_active returns false | Check phase |
| `phase_us < duration_ms * 1000` | Partition active; drop all | Gap; normal processing |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the calling thread.
- `is_partition_active()` reads `m_cfg` fields and `now_us` only; no shared mutable state.
- No atomics required.

---

## 8. Memory & Ownership Semantics

- No allocations. Purely computational (modulo arithmetic).
- `m_cfg` fields `partition_duration_ms`, `partition_gap_ms` — read-only during operation.

---

## 9. Error Handling Flow

- No error states. The partition drops silently.
- The caller (`DeliveryEngine::send()`) receives `Result::OK` even when all messages are partitioned. This is by design.

---

## 10. External Interactions

- **`stderr`:** Logger on each dropped message during partition.
- No network calls during partition.

---

## 11. State Changes / Side Effects

- No state modified by `is_partition_active()`. The partition cycle is derived purely from `now_us` modulo the cycle period.

---

## 12. Sequence Diagram

```
[now_us falls in partition window: phase_us < duration_ms * 1000]

process_outbound(env, now_us, out_buf, &out_count)
  -> is_partition_active(now_us)    <- true
  -> Logger::log("Partition active: dropping...")
  [*out_count = 0; return OK]
  <- Result::OK  [no ::send() called]

[now_us advances to gap window: phase_us >= duration_ms * 1000]

process_outbound(env, now_us, out_buf, &out_count)
  -> is_partition_active(now_us)    <- false
  -> check_loss() -> ...  [normal processing resumes]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_cfg.partition_enabled = true`, `partition_duration_ms > 0`, `partition_gap_ms > 0`.

**Runtime:**
- The partition cycle repeats indefinitely as long as the impairment engine is active. The cycle resets whenever `now_us % cycle_us` wraps back to 0.

---

## 14. Known Risks / Observations

- **Stateless partition:** The partition state is derived from `now_us` with no persistent state. This means the partition will always start at the same phase relative to `now_us = 0` (CLOCK_MONOTONIC epoch). If `now_us` is always > 0 at system start, the initial phase depends on startup time.
- **All traffic dropped:** Both outbound and inbound are affected. RELIABLE_RETRY senders will eventually exhaust their retry budget if the partition outlasts `max_retries * backoff`.
- **`send()` returns OK during partition:** As with loss, the application has no indication that the message was dropped by the partition impairment.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `is_partition_active()` uses `now_us % cycle_us` for the cycle check. The cycle starts at `now_us = 0` (CLOCK_MONOTONIC boot). In practice, `now_us` is large (seconds since boot), so the initial partition phase is `now_us % cycle_us`.
- `[ASSUMPTION]` Both `process_outbound()` and `process_inbound()` call `is_partition_active()` at their top — confirmed by reading `ImpairmentEngine.cpp`.
