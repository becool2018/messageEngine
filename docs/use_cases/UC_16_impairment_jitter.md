# UC_16 — Jitter: per-message delay drawn from uniform distribution around mean

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.2, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` is called when `m_cfg.jitter_mean_ms > 0`. A per-message random delay is added to the base `fixed_latency_ms`. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Vary the per-message delivery delay randomly within a range `[jitter_mean_ms - jitter_variance_ms, jitter_mean_ms + jitter_variance_ms]` to simulate realistic network jitter.
- **Success outcome:** Each message receives a different `deliver_time` drawn from a uniform distribution. Messages with shorter drawn delays may be released before messages with longer delays, creating natural reordering.
- **Error outcomes:** None — `process_outbound()` always returns `Result::OK`.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp (called internally)
// deliver_time = now_us + fixed_latency_us + jitter_us
// jitter_us = (jitter_mean_ms +/- jitter_variance_ms) * 1000
```

Not directly user-callable. Invoked inside `process_outbound()` when computing `deliver_time`.

---

## 3. End-to-End Control Flow

1. `process_outbound()` passes partition and loss checks.
2. **Jitter calculation:**
   a. If `m_cfg.jitter_variance_ms == 0`: `jitter_us = (uint64_t)m_cfg.jitter_mean_ms * 1000ULL`.
   b. If `m_cfg.jitter_variance_ms > 0`: `m_prng.next_range(0, 2 * m_cfg.jitter_variance_ms * 1000)` — draws a uniform integer in `[0, 2*variance*1000)`. Then `jitter_us = (jitter_mean_ms * 1000) - (variance * 1000) + drawn_value`.
3. `deliver_time = now_us + (uint64_t)m_cfg.fixed_latency_ms * 1000ULL + jitter_us`.
4. `queue_to_delay_buf(env, deliver_time)`.
5. `collect_deliverable(now_us, ...)` — entries due immediately (if any) are returned; the jittered entry is not due yet.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound()         [ImpairmentEngine.cpp]
 ├── check_loss()
 ├── [compute deliver_time with jitter]
 │    └── PrngEngine::next_range(0, 2*variance*1000)  [PrngEngine.hpp]
 └── queue_to_delay_buf(env, deliver_time)
```

---

## 5. Key Components Involved

- **`PrngEngine::next_range(min, max)`** — Returns a uniform integer in `[min, max)` using xorshift64 modulo. Provides per-message jitter variation.
- **`queue_to_delay_buf()`** — Stores the envelope with its jittered `deliver_time`.
- **`collect_deliverable()`** — Time-sorted retrieval; earlier `deliver_time` values are released first, which can reorder messages that were queued in order but have different delays.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `jitter_variance_ms > 0` | Draw random offset from PRNG | `jitter_us = mean_ms * 1000` |
| `jitter_mean_ms == 0` | No jitter applied | Add jitter to delay |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the backend's calling thread.
- `PrngEngine::m_state` — plain `uint64_t`; not thread-safe.

---

## 8. Memory & Ownership Semantics

- Same as UC_15: `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` holds all delayed envelopes.
- No heap allocation.

---

## 9. Error Handling Flow

- No error states. Jitter is a silent timing modification.
- If delay buffer is full, the envelope is discarded (as in UC_15).

---

## 10. External Interactions

- None during the delay phase.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S | xorshift64(S) (one draw for jitter) |
| `ImpairmentEngine` | `m_delay_buf[m_delay_count]` | unused | `{env, deliver_time}` with jitter |
| `ImpairmentEngine` | `m_delay_count` | D | D+1 |

---

## 12. Sequence Diagram

```
ImpairmentEngine::process_outbound(env, now_us, ...)
  -> check_loss()                         <- false
  -> PrngEngine::next_range(0, 2*var*1000) [draw d]
  [jitter_us = mean*1000 - var*1000 + d]
  [deliver_time = now_us + lat_us + jitter_us]
  -> queue_to_delay_buf(env, deliver_time)
  -> collect_deliverable(now_us, ...)     [*out_count = 0; not due yet]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_cfg.jitter_mean_ms > 0` and `m_cfg.enabled == true`.
- PRNG seeded from `m_cfg.prng_seed`.

**Runtime:**
- Each message gets an independent jitter draw. No state accumulates between calls.

---

## 14. Known Risks / Observations

- **Implicit reordering:** Messages with shorter jitter draws arrive before later-queued messages with shorter delays. This is intentional but means jitter implicitly enables reordering even without `reorder_enabled = true`.
- **Variance boundary:** `jitter_mean_ms - jitter_variance_ms` can be negative if `variance > mean`. The PRNG draw ensures the minimum delay is `(mean - variance) * 1000` microseconds, which could be zero or negative. The implementation should clamp to 0.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The jitter range is `[mean - variance, mean + variance]` milliseconds, converted to microseconds. Negative jitter values are clamped to 0 to prevent `deliver_time` from being in the past. This clamping behavior is inferred from the ImpairmentEngine source.
