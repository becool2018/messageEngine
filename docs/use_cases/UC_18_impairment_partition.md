# UC_18 — Partition: all traffic blocked for partition_duration_ms, then released for partition_gap_ms

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.6, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** `ImpairmentEngine::process_outbound()` is called with an inbound message while the `ImpairmentConfig` has `partition_enabled = true`, `partition_duration_ms > 0`, and `partition_gap_ms > 0`.

**Goal:** Simulate a periodic, intermittent link outage. The partition state machine alternates between two states: *gap* (traffic passes, waiting for the next partition to begin) and *active* (all traffic dropped, link is simulated as severed). The transition times are determined by wall-clock microseconds provided by the caller.

**Success outcome (gap state):** `is_partition_active()` returns `false`; `process_outbound()` proceeds to the loss/jitter/delay path and may accept the message.

**Success outcome (active state):** `is_partition_active()` returns `true`; `process_outbound()` returns `Result::ERR_IO` immediately. `TcpBackend::send_message()` treats `ERR_IO` as a silent drop and returns `Result::OK` to the caller.

**Error outcomes:** None beyond the intended ERR_IO drop. The state machine does not produce hard errors.

---

## 2. Entry Points

```
// src/platform/ImpairmentEngine.hpp / ImpairmentEngine.cpp
bool ImpairmentEngine::is_partition_active(uint64_t now_us);
```

Called from within:

```
Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env,
                                          uint64_t now_us);
```

which is called from:

```
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);
```

`is_partition_active()` may also be called directly from tests.

---

## 3. End-to-End Control Flow (Step-by-Step)

**State machine initialization (first call after `init()`)**

1. `process_outbound(in_env, now_us)` is called.
2. `is_partition_active(now_us)` is called.
3. Precondition: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
4. `!m_cfg.partition_enabled` — `false` (partition is enabled). Continue.
5. `m_next_partition_event_us == 0ULL` — `true` on first call after `init()`.
6. Initialize the first event: `m_next_partition_event_us = now_us + partition_gap_ms * 1000ULL`.
7. `m_partition_active = false`.
8. `NEVER_COMPILED_OUT_ASSERT(!m_partition_active)`.
9. Returns `false`. Traffic passes on this first call.

**Gap state (waiting for partition to begin)**

10. Subsequent `is_partition_active(now_us)` calls while `now_us < m_next_partition_event_us`:
    - `!partition_enabled` — `false`.
    - `m_next_partition_event_us != 0` — skip initialization branch.
    - `!m_partition_active && now_us >= m_next_partition_event_us` — `false` (event not yet reached).
    - `m_partition_active && now_us >= m_next_partition_event_us` — `false` (not active).
    - `NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL)`.
    - Returns `m_partition_active` == `false`. Traffic passes.

**Transition: gap → active (now_us crosses m_next_partition_event_us)**

11. `is_partition_active(now_us)` with `now_us >= m_next_partition_event_us` and `!m_partition_active`:
    - Branch `!m_partition_active && now_us >= m_next_partition_event_us` is `true`.
    - `m_partition_active = true`.
    - `m_partition_start_us = now_us`.
    - `m_next_partition_event_us = now_us + partition_duration_ms * 1000ULL`.
    - Logger::log(WARNING_LO, "ImpairmentEngine", "partition started (duration: %u ms)", ...)`.
    - `NEVER_COMPILED_OUT_ASSERT(m_partition_active)`.
    - Returns `true`. Traffic is dropped.
12. Back in `process_outbound()`: `is_partition_active()` returned `true`.
13. Logger::log(WARNING_LO, "ImpairmentEngine", "message dropped (partition active)")`.
14. Returns `Result::ERR_IO`.
15. Back in `TcpBackend::send_message()`: `res == ERR_IO` → treated as silent drop → returns `Result::OK`.

**Active state (partition in progress)**

16. All `is_partition_active(now_us)` calls while `now_us < m_next_partition_event_us` and `m_partition_active`:
    - Both transition branches are `false`.
    - Returns `m_partition_active` == `true`. All messages dropped.

**Transition: active → gap (now_us crosses m_next_partition_event_us again)**

17. `is_partition_active(now_us)` with `now_us >= m_next_partition_event_us` and `m_partition_active`:
    - Branch `m_partition_active && now_us >= m_next_partition_event_us` is `true`.
    - `m_partition_active = false`.
    - `m_next_partition_event_us = now_us + partition_gap_ms * 1000ULL`.
    - `Logger::log(WARNING_LO, "ImpairmentEngine", "partition ended")`.
    - `NEVER_COMPILED_OUT_ASSERT(!m_partition_active)`.
    - Returns `false`. Traffic passes again.
18. The gap → active → gap cycle repeats indefinitely as long as `partition_enabled` is `true`.

---

## 4. Call Tree

```
TcpBackend::send_message()
 ├── Serializer::serialize()
 ├── timestamp_now_us()
 └── ImpairmentEngine::process_outbound()
      └── ImpairmentEngine::is_partition_active()
           └── [one of four branches]:
                A. First call: initialize m_next_partition_event_us → return false
                B. Gap, event not reached: return false
                C. Gap→Active transition: set m_partition_active=true; log; return true
                D. Active→Gap transition: set m_partition_active=false; log; return false
           └── [if true]
                └── Logger::log(WARNING_LO, "message dropped (partition active)")
                    return ERR_IO → send_message() returns OK (silent drop)
```

---

## 5. Key Components Involved

- **`ImpairmentEngine::is_partition_active()`** (`src/platform/ImpairmentEngine.cpp`): Core of this UC. Maintains a 2-state (gap, active) machine using three member variables: `m_partition_active` (bool), `m_partition_start_us` (uint64_t, set but not used beyond logging), and `m_next_partition_event_us` (uint64_t, the deadline for the next state transition).

- **`ImpairmentEngine::process_outbound()`**: Calls `is_partition_active()` unconditionally as the first impairment check (after the disabled-impairments guard). A `true` return causes immediate ERR_IO return, skipping loss, jitter, and delay-buffer steps.

- **`ImpairmentConfig`** (`src/core/ImpairmentConfig.hpp`): Fields used: `partition_enabled` (bool gate), `partition_duration_ms` (active state duration), `partition_gap_ms` (gap state duration).

- **`Logger`**: Emits `WARNING_LO` events at both state transitions and on each dropped message.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!m_cfg.partition_enabled` | Return `false` immediately; no state change | Continue state machine |
| `m_next_partition_event_us == 0ULL` (first call) | Initialize gap: `next_event = now + gap_ms*1000`; `active = false`; return `false` | Skip initialization |
| `!m_partition_active && now_us >= m_next_partition_event_us` | Transition to active: `active = true`; `next_event = now + dur_ms*1000`; log; return `true` | Not yet time to activate |
| `m_partition_active && now_us >= m_next_partition_event_us` | Transition to gap: `active = false`; `next_event = now + gap_ms*1000`; log; return `false` | Still in active state |
| Default (neither transition branch fires) | `NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL)`; return `m_partition_active` | (unreachable; all cases covered above) |
| `is_partition_active()` returns `true` (in `process_outbound`) | Log "message dropped"; return `ERR_IO` | Proceed to `check_loss()` |

---

## 7. Concurrency / Threading Behavior

`is_partition_active()` reads and writes `m_partition_active`, `m_partition_start_us`, and `m_next_partition_event_us`. None of these are atomic. There is no mutex. Concurrent calls from multiple threads produce data races.

[ASSUMPTION: single-threaded use, as with all other `ImpairmentEngine` methods.]

The state machine is not driven by a timer interrupt or background thread. It only advances when `is_partition_active(now_us)` is called. If `send_message()` is not called for a period longer than `partition_duration_ms`, the next call may "skip" directly through the active state into the next gap without any messages being dropped. [ASSUMPTION: the application drives sends frequently enough that the partition duration is observable.]

---

## 8. Memory & Ownership Semantics

**Stack allocations in this flow:** None within `is_partition_active()`.

**Fixed member variables involved:**

| Member | Type | Role |
|--------|------|------|
| `m_partition_active` | `bool` | Current state |
| `m_partition_start_us` | `uint64_t` | Timestamp when active state began (set but not used in decisions) |
| `m_next_partition_event_us` | `uint64_t` | Deadline for next transition |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. All state is fixed member variables. No buffers are allocated.

---

## 9. Error Handling Flow

| Condition | Behavior | Downstream effect |
|-----------|----------|-------------------|
| `process_outbound()` returns `ERR_IO` (partition active) | `send_message()` treats `ERR_IO` as a silent intentional drop; returns `OK` to caller | Message is discarded; no ACK tracking, no retry scheduling |
| `partition_duration_ms = 0` | Partition immediately transitions back to gap on the same call that activates it (since `now_us >= next_event` after duration-0 addition). Effectively no active state. | [ASSUMPTION: edge case; behavior is safe but unintended] |
| `partition_gap_ms = 0` | Immediately transitions from gap to active (next_event = now_us + 0). First call after init returns `false`, second call returns `true`. | Effectively always-active partition. |
| Both `partition_duration_ms` and `partition_gap_ms` are 0 | Alternates `false/true/false/true...` on every call. | Unpredictable behavior; [ASSUMPTION: caller should validate config before calling `init()`] |

---

## 10. External Interactions

None. `is_partition_active()` is purely in-memory state machine logic. No POSIX calls are made.

The `now_us` parameter is provided by the caller (obtained via `timestamp_now_us()` in `TcpBackend::send_message()`). The state machine trusts this value monotonically increases between calls. [ASSUMPTION: `now_us` is always monotonically non-decreasing. Clock wrap-around or backwards jumps would confuse the state machine.]

---

## 11. State Changes / Side Effects

**First call:**

| Member | Before | After |
|--------|--------|-------|
| `m_next_partition_event_us` | `0ULL` | `now_us + partition_gap_ms * 1000` |
| `m_partition_active` | `false` | `false` |

**Gap → Active transition:**

| Member | Before | After |
|--------|--------|-------|
| `m_partition_active` | `false` | `true` |
| `m_partition_start_us` | any | `now_us` |
| `m_next_partition_event_us` | prior gap deadline | `now_us + partition_duration_ms * 1000` |

**Active → Gap transition:**

| Member | Before | After |
|--------|--------|-------|
| `m_partition_active` | `true` | `false` |
| `m_next_partition_event_us` | prior active deadline | `now_us + partition_gap_ms * 1000` |

**Log side effects:**
- Gap → Active: `Logger::log(WARNING_LO, "ImpairmentEngine", "partition started (duration: %u ms)", duration_ms)`.
- Active → Gap: `Logger::log(WARNING_LO, "ImpairmentEngine", "partition ended")`.
- Each dropped message: `Logger::log(WARNING_LO, "ImpairmentEngine", "message dropped (partition active)")`.

---

## 12. Sequence Diagram

```
TcpBackend           ImpairmentEngine          Logger
  |                       |                       |
  |--send_message()       |                       |
  |  (timestamp_now_us)   |                       |
  |--process_outbound()-->|                       |
  |                       |--is_partition_active()|
  |                       |                       |
  |  [First call]:        |                       |
  |                       |  next_event = now +   |
  |                       |  gap_ms*1000          |
  |                       |  active = false       |
  |                       |--return false         |
  |                       |  (proceed to loss...) |
  |                                               |
  |  [Later: now >= next_event, !active]:         |
  |                       |--is_partition_active()|
  |                       |  active = true        |
  |                       |  next_event = now +   |
  |                       |  dur_ms*1000          |
  |                       |------------------------>"partition started"
  |                       |--return true          |
  |--ERR_IO (drop)        |                       |
  |  (send_message        |                       |
  |   returns OK)         |                       |
  |                                               |
  |  [During active state, per message]:          |
  |                       |--is_partition_active()|
  |                       |  (both transitions    |
  |                       |  false; return true)  |
  |                       |--"message dropped"--->|
  |--ERR_IO (drop)        |                       |
  |                                               |
  |  [Later: now >= next_event, active]:          |
  |                       |--is_partition_active()|
  |                       |  active = false       |
  |                       |  next_event = now +   |
  |                       |  gap_ms*1000          |
  |                       |------------------------>"partition ended"
  |                       |--return false         |
  |                       |  (proceed to loss...) |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**

- `ImpairmentEngine::init(cfg)`: stores config; resets `m_partition_active = false`, `m_partition_start_us = 0`, `m_next_partition_event_us = 0`. The first call to `is_partition_active()` after `init()` performs lazy initialization of the first gap deadline.
- The `m_next_partition_event_us = 0` sentinel is the trigger for lazy initialization. After the first call it will always be `> 0`.

**Steady-state runtime:**

- The state machine transitions lazily: it only advances when `is_partition_active(now_us)` is called. No background timer thread exists.
- If `partition_gap_ms = 5000` and `partition_duration_ms = 1000`, the expected behavior is: 5 seconds of traffic passing, 1 second of all-drop, 5 seconds pass, 1 second drop, repeat.
- Actual transition precision depends on call frequency. If `send_message()` is called every 200 ms, transition detection is accurate to within ~200 ms.

---

## 14. Known Risks / Observations

- **No timer thread.** The partition boundary is only detected when `process_outbound()` (and thus `is_partition_active()`) is called. If sends are infrequent, the partition may be "missed" entirely: the `now_us` in the first call after the partition would have already exceeded both the activation time and the deactivation time, causing both transition branches to fire in rapid succession across consecutive calls. The second call may skip the active state if `partition_duration_ms` is short relative to the inter-send interval.

- **`m_partition_start_us` is set but not used in decisions.** It is available for external query or logging but has no effect on the state machine. This is a minor dead field.

- **`now_us` monotonicity requirement.** The state machine assumes `now_us` is non-decreasing. A non-monotonic clock (e.g., NTP adjustment backwards) would cause both transition conditions to evaluate as `false`, leaving the machine stuck in its current state.

- **Tight coupling between partition and loss/jitter.** `is_partition_active()` is evaluated first in `process_outbound()`. A message dropped by the partition does not consume a PRNG call for loss or jitter. This means partition activity affects the PRNG sequence for subsequent messages only through its effect on call ordering — an important consideration for reproducibility tests.

- **No backpressure from partition.** Messages dropped during a partition are silently lost. There is no buffer-and-deliver-later semantic. If the caller needs to retransmit after a partition ends, it must handle that at a higher layer (e.g., `RetryManager`).

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `timestamp_now_us()` returns a monotonically non-decreasing value. Backwards clock jumps would confuse the state machine.
- [ASSUMPTION] `m_partition_start_us` is stored for potential future use (e.g., logging partition duration), but the current implementation does not use it for any decision.
- [ASSUMPTION] The partition state machine has no "draining" behavior: messages in `m_delay_buf` that were queued before a partition starts are still delivered when their `release_us` elapses (via `collect_deliverable()`). The partition only blocks new messages from being enqueued via `process_outbound()`.
- [ASSUMPTION] If `partition_duration_ms = 0`, the transition from gap to active and back to gap occurs within a single call or two consecutive calls. This produces a momentary drop for exactly one message before returning to gap state.
- [ASSUMPTION] The state machine is designed for a simple periodic partition pattern. Aperiodic or stochastic partition patterns are not supported by this design.
- [ASSUMPTION] `partition_gap_ms` and `partition_duration_ms` are validated by the caller before calling `init()`. The engine does not clamp or validate these values beyond the assertion on `loss_probability` range. A value of `0` for either is technically valid but produces degenerate behavior.
