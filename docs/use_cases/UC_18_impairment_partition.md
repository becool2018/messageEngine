# UC_18 — Impairment: partition

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.6, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

When `ImpairmentConfig::partition_enabled` is true, `ImpairmentEngine::is_partition_active()` implements a two-state machine (inactive → active → inactive → ...) driven entirely by wall-clock time passed in as `now_us`. On the first call, it schedules the first partition event after `partition_gap_ms` milliseconds. When `now_us` reaches the scheduled event time, the state transitions to active and the method returns `true`. While active, `process_outbound()` drops every message immediately and returns `Result::ERR_IO`. After `partition_duration_ms` milliseconds, the state transitions back to inactive and traffic is allowed again until the next gap expires. This cycle repeats indefinitely without any additional external timer or thread.

---

## 2. Entry Points

| Entry Point | File | Line | Signature |
|---|---|---|---|
| `ImpairmentEngine::is_partition_active()` | `src/platform/ImpairmentEngine.cpp` | 322 | `bool ImpairmentEngine::is_partition_active(uint64_t now_us)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp` | 151 | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp` | 347 | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |

`is_partition_active()` is a method called only from `process_outbound()` at ImpairmentEngine.cpp:169. It is not called directly from `TcpBackend`.

---

## 3. End-to-End Control Flow (Step-by-Step)

**First call — partition timing initialization:**

1. `TcpBackend::send_message(envelope)` (TcpBackend.cpp:347):
   - `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` (lines 349–350).
   - `Serializer::serialize(...)` — wire bytes written to `m_wire_buf`.
   - `now_us = timestamp_now_us()` — `clock_gettime(CLOCK_MONOTONIC)` (TcpBackend.cpp:362).
   - `m_impairment.process_outbound(envelope, now_us)` called (line 363).

2. **Inside `process_outbound()`** (ImpairmentEngine.cpp:151):
   - `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))` (lines 155–156).
   - `if (!m_cfg.enabled)` — false; continues.
   - **`is_partition_active(now_us)` called** (line 169).

3. **Inside `is_partition_active()`** (ImpairmentEngine.cpp:322):
   - `NEVER_COMPILED_OUT_ASSERT(m_initialized)` (line 325).
   - `if (!m_cfg.partition_enabled)` (line 328) — false; continues.
   - `if (m_next_partition_event_us == 0ULL)` (line 333) — **true on first call** (initialized to 0 in `init()` at line 69):
     - `m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_gap_ms) * 1000ULL` (line 335).
       - Example: `partition_gap_ms = 100` → event set 100 ms in the future.
     - `m_partition_active = false` (line 336).
     - `NEVER_COMPILED_OUT_ASSERT(!m_partition_active)` (line 337).
     - Returns `false`. Traffic passes.

4. **Back in `process_outbound()`** — `is_partition_active` returned false; continues to loss/jitter/buffer path. Message is processed normally.

---

**Subsequent calls during the gap (inactive state, gap not yet elapsed):**

5. `is_partition_active(now_us)` called on each `process_outbound()`:
   - `m_next_partition_event_us != 0ULL` — first-call branch skipped.
   - `if (!m_partition_active && now_us >= m_next_partition_event_us)` (line 342) — false (gap not elapsed).
   - `if (m_partition_active && now_us >= m_next_partition_event_us)` (line 354) — false (not active).
   - `NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL)` (line 365).
   - Returns `m_partition_active` = `false`. Traffic passes.

---

**Transition: inactive → active (gap expires):**

6. `is_partition_active(now_us)` called when `now_us >= m_next_partition_event_us`:
   - `if (!m_partition_active && now_us >= m_next_partition_event_us)` (line 342) — **true**:
     - `m_partition_active = true` (line 344).
     - `m_partition_start_us = now_us` (line 345).
     - `m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_duration_ms) * 1000ULL` (line 346).
       - Example: `partition_duration_ms = 50` → next event 50 ms in the future.
     - `Logger::log(Severity::WARNING_LO, "ImpairmentEngine", "partition started (duration: %u ms)", m_cfg.partition_duration_ms)` (lines 347–349).
     - `NEVER_COMPILED_OUT_ASSERT(m_partition_active)` (line 350).
     - Returns `true`.

7. **Back in `process_outbound()`** (line 169):
   - `if (is_partition_active(now_us))` — true.
   - `Logger::log(Severity::WARNING_LO, "ImpairmentEngine", "message dropped (partition active)")` (lines 170–171).
   - `return Result::ERR_IO` (line 172). Loss/jitter/buffer checks are not reached.

8. **Back in `TcpBackend::send_message()`** (line 364):
   - `if (res == Result::ERR_IO)` — true.
   - `return Result::OK` (line 365). Message dropped; no bytes written to socket.

---

**During active partition — all messages dropped:**

9. Every subsequent `process_outbound()` call while `now_us < m_next_partition_event_us`:
   - `is_partition_active()`: neither transition branch fires.
   - Falls through to `NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL)` (line 365) and `return m_partition_active` (line 366) = `true`.
   - `process_outbound()` drops message; returns `ERR_IO`.
   - `send_message()` returns `OK`.

---

**Transition: active → inactive (partition duration expires):**

10. `is_partition_active(now_us)` called when `now_us >= m_next_partition_event_us` (duration elapsed):
    - First branch `(!m_partition_active && ...)` (line 342) — false (currently active).
    - `if (m_partition_active && now_us >= m_next_partition_event_us)` (line 354) — **true**:
      - `m_partition_active = false` (line 356).
      - `m_next_partition_event_us = now_us + static_cast<uint64_t>(m_cfg.partition_gap_ms) * 1000ULL` (line 357).
        - Next gap begins.
      - `Logger::log(Severity::WARNING_LO, "ImpairmentEngine", "partition ended")` (lines 358–359).
      - `NEVER_COMPILED_OUT_ASSERT(!m_partition_active)` (line 360).
      - Returns `false`.

11. Traffic resumes. Cycle repeats from step 5 with the new gap deadline.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)                      [TcpBackend.cpp:347]
├── NEVER_COMPILED_OUT_ASSERT(m_open)
├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, ...)
├── timestamp_now_us()
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us)  [ImpairmentEngine.cpp:151]
│   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
│   ├── [if !m_cfg.enabled → passthrough; NOT taken]
│   ├── ImpairmentEngine::is_partition_active(now_us)    ← STATE MACHINE [line 322]
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   │   ├── [if !partition_enabled] → return false       ← FEATURE DISABLED
│   │   ├── [if m_next_partition_event_us == 0ULL]       ← FIRST CALL ONLY
│   │   │   ├── m_next_partition_event_us = now_us + gap_ms * 1000
│   │   │   ├── m_partition_active = false
│   │   │   ├── NEVER_COMPILED_OUT_ASSERT(!m_partition_active)
│   │   │   └── return false
│   │   ├── [if !active && now_us >= event_us]           ← INACTIVE → ACTIVE
│   │   │   ├── m_partition_active = true
│   │   │   ├── m_partition_start_us = now_us
│   │   │   ├── m_next_partition_event_us = now_us + duration_ms * 1000
│   │   │   ├── Logger::log(WARNING_LO, "partition started (duration: N ms)")
│   │   │   ├── NEVER_COMPILED_OUT_ASSERT(m_partition_active)
│   │   │   └── return true
│   │   ├── [if active && now_us >= event_us]            ← ACTIVE → INACTIVE
│   │   │   ├── m_partition_active = false
│   │   │   ├── m_next_partition_event_us = now_us + gap_ms * 1000
│   │   │   ├── Logger::log(WARNING_LO, "partition ended")
│   │   │   ├── NEVER_COMPILED_OUT_ASSERT(!m_partition_active)
│   │   │   └── return false
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_next_partition_event_us > 0ULL)  ← STEADY STATE
│   │   └── return m_partition_active
│   │
│   ├── [if is_partition_active returned true]           ← DROP BRANCH
│   │   ├── Logger::log(WARNING_LO, "message dropped (partition active)")
│   │   └── return Result::ERR_IO
│   │
│   └── [if false: continues to check_loss(), latency, queue_to_delay_buf...]
│
└── [if res == ERR_IO] return Result::OK                 ← DROP TRANSPARENT TO CALLER
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `ImpairmentEngine::is_partition_active()` | Method | `ImpairmentEngine.cpp:322` | Two-state machine; transitions on wall-clock `now_us` |
| `ImpairmentConfig::partition_enabled` | `bool` | `ImpairmentConfig.hpp:61` | Master partition feature switch |
| `ImpairmentConfig::partition_duration_ms` | `uint32_t` | `ImpairmentConfig.hpp:64` | Duration of each drop-all partition period (ms) |
| `ImpairmentConfig::partition_gap_ms` | `uint32_t` | `ImpairmentConfig.hpp:67` | Duration of each traffic-allowed gap between partitions (ms) |
| `ImpairmentEngine::m_partition_active` | `bool` | `ImpairmentEngine.hpp:170` | Current state: `true` = dropping all traffic |
| `ImpairmentEngine::m_partition_start_us` | `uint64_t` | `ImpairmentEngine.hpp:171` | Wall-clock µs when current partition began; written on activation but never read internally |
| `ImpairmentEngine::m_next_partition_event_us` | `uint64_t` | `ImpairmentEngine.hpp:172` | Wall-clock µs of next state transition; `0ULL` = not yet initialized |
| `timestamp_now_us()` | Inline function | `src/core/Timestamp.hpp:31` | Provides `now_us` to `process_outbound()`; `is_partition_active()` does not call it independently |
| `Logger::log()` | Static function | (Logger.hpp) | Records partition start, partition end, and per-message drop events |
| `TcpBackend::send_message()` | Method | `src/platform/TcpBackend.cpp:347` | Converts ERR_IO from `process_outbound()` to OK; transparent drop to caller |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Partition feature disabled | ImpairmentEngine.cpp:328 | `!m_cfg.partition_enabled` | Return false immediately; no state changes; no drops |
| First-call initialization | ImpairmentEngine.cpp:333 | `m_next_partition_event_us == 0ULL` | Set first event to `now_us + gap_ms * 1000`; return false |
| Inactive → active | ImpairmentEngine.cpp:342 | `!m_partition_active && now_us >= m_next_partition_event_us` | Flip to active; set duration deadline; log; return true |
| Active → inactive | ImpairmentEngine.cpp:354 | `m_partition_active && now_us >= m_next_partition_event_us` | Flip to inactive; set gap deadline; log; return false |
| Steady state (no transition) | ImpairmentEngine.cpp:365 | Neither transition condition met | Return `m_partition_active` unchanged; no state mutation |
| Drop in `process_outbound` | ImpairmentEngine.cpp:169 | `is_partition_active(now_us) == true` | Logger call; return ERR_IO; `check_loss()` not called; PRNG not advanced |
| ERR_IO in `send_message` | TcpBackend.cpp:364 | `res == Result::ERR_IO` | Return OK to caller; transparent drop |

**Transition is edge-triggered by `now_us`**: The state machine transitions on the first call to `is_partition_active()` where `now_us >= m_next_partition_event_us`. If `process_outbound()` is called infrequently, the partition may start or end later than the configured millisecond boundary by up to one call interval.

**Both transitions cannot fire in the same call**: The inactive→active branch executes `return true` immediately; the active→inactive branch is never reached in the same invocation. A single call advances the state machine by at most one transition.

**Partition check fires before loss check**: Inside `process_outbound()`, `is_partition_active()` is called at line 169, before `check_loss()` at line 176. During a partition, the PRNG is never consulted. The PRNG sequence for loss and duplication is not advanced during partition-active drops.

---

## 7. Concurrency / Threading Behavior

- `is_partition_active()` reads and writes `m_partition_active` (bool), `m_partition_start_us` (uint64_t), and `m_next_partition_event_us` (uint64_t). All are plain (non-atomic) member variables. Not thread-safe.
- No mutex, spinlock, or atomic operation protects any of these fields.
- [ASSUMPTION] All calls to `process_outbound()` (and thus `is_partition_active()`) occur from the same thread per `ImpairmentEngine` instance.
- `Logger::log()` is called on every state transition and on every dropped message during an active partition. If Logger is synchronous, a high message rate during a partition produces a burst of synchronous log writes.
- `timestamp_now_us()` calls `clock_gettime(CLOCK_MONOTONIC)`, which is POSIX thread-safe. However, `is_partition_active()` does not call `clock_gettime` directly — it uses the `now_us` value computed by `send_message()` before entering `process_outbound()`.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `is_partition_active()` modifies only three scalar member variables (`m_partition_active`, `m_partition_start_us`, `m_next_partition_event_us`). No buffer writes, no `envelope_copy`, no delay buffer access.
- In the partition-active drop path, `process_outbound()` returns `ERR_IO` before any `queue_to_delay_buf()` or `m_delay_buf` write. The delay buffer is entirely untouched.
- `m_partition_start_us` is written on each inactive→active transition (ImpairmentEngine.cpp:345) but is never read elsewhere in the implementation. It is stored but unused by the state machine logic.
- The `in_env` reference passed to `process_outbound()` is never written to or copied in the partition drop path. It is accessed only by `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))` before `is_partition_active()` is called.
- `m_wire_buf` in `TcpBackend` is written by `Serializer::serialize()` before `process_outbound()` is called, but the bytes are never transmitted in the partition drop path.
- No heap allocation anywhere in this path. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| `partition_duration_ms == 0` | ImpairmentEngine.cpp:346 | `m_next_partition_event_us = now_us + 0`. On the very next call, `now_us' >= now_us` is immediately true → instant active→inactive transition → partition has zero duration; traffic never actually blocked for more than one call interval |
| `partition_gap_ms == 0` | ImpairmentEngine.cpp:335 or 357 | `m_next_partition_event_us = now_us + 0`. On next call, gap expires immediately → continuous partition → all traffic dropped forever |
| Both durations zero | Both above combined | Zero-duration partitions with zero gaps; effectively all traffic dropped continuously |
| `m_next_partition_event_us` overflow | ImpairmentEngine.cpp:335/346/357 | `now_us + (ms * 1000)` where `ms` is `uint32_t`; max additional value ≈ 4.29×10¹² µs (~49 days); comfortably within `uint64_t` range |
| `is_partition_active` called before `init()` | ImpairmentEngine.cpp:325 | `NEVER_COMPILED_OUT_ASSERT(m_initialized)` fires; process aborts in debug builds |
| `clock_gettime` failure in `timestamp_now_us` | Timestamp.hpp:36 | `NEVER_COMPILED_OUT_ASSERT(result == 0)` fires in debug; in release builds `now_us` receives garbage; partition state machine receives an invalid time and may transition incorrectly |
| Very long real-time gap between calls | All transition checks | State machine catches up by at most one transition per call; missed intermediate transitions are not processed; the machine picks up from its last known state |

---

## 10. External Interactions

| Interaction | Function | API | When |
|---|---|---|---|
| Monotonic clock | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX | In `send_message()` before `process_outbound()`; the resulting `now_us` drives the state machine |
| Logger: partition start | `Logger::log()` | WARNING_LO | Once per inactive→active transition |
| Logger: partition end | `Logger::log()` | WARNING_LO | Once per active→inactive transition |
| Logger: message dropped | `Logger::log()` | WARNING_LO | Once per dropped message during active partition |
| Socket I/O | `tcp_send_frame()` | POSIX write on TCP fd | Not reached; partition drop prevents execution reaching `send_to_all_clients()` |

**Logger call volume during active partition**: One `Logger::log()` call fires per `send_message()` invocation while the partition is active ("message dropped (partition active)"). At high send rates this can produce a very large volume of log output. There is no rate-limiting on these log calls.

---

## 11. State Changes / Side Effects

| State | Object | Phase | Change |
|---|---|---|---|
| `m_next_partition_event_us` | `ImpairmentEngine` | First call | Set to `now_us + gap_ms * 1000` (from 0ULL) |
| `m_partition_active` | `ImpairmentEngine` | inactive→active | Set from `false` to `true` |
| `m_partition_start_us` | `ImpairmentEngine` | inactive→active | Set to `now_us` at transition moment |
| `m_next_partition_event_us` | `ImpairmentEngine` | inactive→active | Set to `now_us + duration_ms * 1000` |
| `m_partition_active` | `ImpairmentEngine` | active→inactive | Set from `true` to `false` |
| `m_next_partition_event_us` | `ImpairmentEngine` | active→inactive | Set to `now_us + gap_ms * 1000` |
| Logger output | Process-level | inactive→active | WARNING_LO: "partition started (duration: N ms)" |
| Logger output | Process-level | active→inactive | WARNING_LO: "partition ended" |
| Logger output | Process-level | Per dropped message | WARNING_LO: "message dropped (partition active)" |
| `m_delay_buf`, `m_delay_count` | `ImpairmentEngine` | All partition phases | Not modified |
| `m_prng.m_state` | `PrngEngine` | All partition phases | Not modified (PRNG not called in partition drop path) |
| `m_wire_buf` | `TcpBackend` | All partition phases | Written by serialization but never transmitted |

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant ImpairmentEngine
    participant Logger

    Note over Caller,Logger: gap_ms=100ms, duration_ms=50ms. Timeline in ms from T=0.

    Caller->>TcpBackend: send_message(A) at T=0
    TcpBackend->>ImpairmentEngine: process_outbound(A, 0µs)
    ImpairmentEngine->>ImpairmentEngine: is_partition_active(0)
    Note over ImpairmentEngine: first call: event_us = 0+100000<br/>m_partition_active = false; return false
    Note over ImpairmentEngine: A processed normally
    TcpBackend-->>Caller: OK

    Caller->>TcpBackend: send_message(B) at T=50ms
    TcpBackend->>ImpairmentEngine: process_outbound(B, 50000)
    ImpairmentEngine->>ImpairmentEngine: is_partition_active(50000)
    Note over ImpairmentEngine: !active, 50000 < 100000: no transition; return false
    Note over ImpairmentEngine: B processed normally
    TcpBackend-->>Caller: OK

    Caller->>TcpBackend: send_message(C) at T=100ms
    TcpBackend->>ImpairmentEngine: process_outbound(C, 100000)
    ImpairmentEngine->>ImpairmentEngine: is_partition_active(100000)
    Note over ImpairmentEngine: !active, 100000>=100000: ACTIVATE<br/>m_partition_active=true; m_partition_start_us=100000<br/>event_us=100000+50000=150000
    ImpairmentEngine->>Logger: WARNING_LO "partition started 50ms"
    Note over ImpairmentEngine: return true
    ImpairmentEngine->>Logger: WARNING_LO "message dropped (partition active)"
    ImpairmentEngine-->>TcpBackend: ERR_IO
    TcpBackend-->>Caller: OK (drop transparent)

    Caller->>TcpBackend: send_message(D) at T=120ms
    TcpBackend->>ImpairmentEngine: process_outbound(D, 120000)
    ImpairmentEngine->>ImpairmentEngine: is_partition_active(120000)
    Note over ImpairmentEngine: active, 120000 < 150000: no transition; return true
    ImpairmentEngine->>Logger: WARNING_LO "message dropped (partition active)"
    ImpairmentEngine-->>TcpBackend: ERR_IO
    TcpBackend-->>Caller: OK (drop transparent)

    Caller->>TcpBackend: send_message(E) at T=150ms
    TcpBackend->>ImpairmentEngine: process_outbound(E, 150000)
    ImpairmentEngine->>ImpairmentEngine: is_partition_active(150000)
    Note over ImpairmentEngine: active, 150000>=150000: DEACTIVATE<br/>m_partition_active=false<br/>event_us=150000+100000=250000
    ImpairmentEngine->>Logger: WARNING_LO "partition ended"
    Note over ImpairmentEngine: return false
    Note over ImpairmentEngine: E processed normally
    TcpBackend-->>Caller: OK
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (`ImpairmentEngine::init()`):
- `m_partition_active = false` (line 66).
- `m_partition_start_us = 0ULL` (line 67).
- `m_next_partition_event_us = 0ULL` (line 69) — sentinel value; the value `0` is used as the "not yet initialized" indicator that triggers first-call initialization inside `is_partition_active()`.
- `m_cfg.partition_enabled`, `m_cfg.partition_duration_ms`, `m_cfg.partition_gap_ms` stored by value copy from the provided `ImpairmentConfig`.
- All partition fields default to `false` / `0` in `impairment_config_default()` (ImpairmentConfig.hpp:89–91); a higher-level layer must set them before calling `init()`.
- `TcpBackend::init()` (TcpBackend.cpp:99–104) calls `impairment_config_default(imp_cfg)` and only overrides `imp_cfg.enabled` from channel config. `partition_enabled`, `partition_duration_ms`, and `partition_gap_ms` all remain at their defaults (false/0) unless a higher-level layer explicitly populates the full `ImpairmentConfig` and passes it directly to `m_impairment.init()`.

**First runtime call**: `is_partition_active()` detects `m_next_partition_event_us == 0ULL` and sets the first gap deadline relative to the actual `now_us` at that moment. This defers the wall-clock anchor to the first `send_message()` invocation rather than `init()` time.

**Runtime state machine**:
```
State:    INACTIVE ─────(gap_ms)────> ACTIVE ──(duration_ms)──> INACTIVE ─────(gap_ms)────> ...
Trigger:  first call sets anchor   now_us >= event_us          now_us >= event_us
```

**Drift behavior**: Each transition sets `m_next_partition_event_us = now_us + interval_ms * 1000` at the moment of detection, not relative to the originally scheduled time. If calls to `process_outbound()` are delayed (caller is slow or bursty), each transition fires slightly later than configured. The cumulative effect is that the partition cycle drifts forward in wall-clock time.

---

## 14. Known Risks / Observations

1. **Partition timing drifts with call frequency**: Each `m_next_partition_event_us` is set relative to the detection `now_us`, not the originally scheduled event time. Infrequent `send_message()` calls cause partitions to start and end later than configured.

2. **`m_partition_start_us` is written but never read**: The field is set on each inactive→active transition (ImpairmentEngine.cpp:345) and is never referenced elsewhere in the implementation. It cannot be used for observability or metrics without additional accessor code.

3. **`partition_gap_ms == 0` causes permanent partition**: If `partition_gap_ms` is 0, the gap deadline is set to `now_us + 0 = now_us`. On the very next call, `now_us' >= now_us` is immediately true and the inactive→active transition fires instantly. Traffic is never allowed. This condition is not asserted against or logged.

4. **`partition_duration_ms == 0` causes zero-duration partition**: The partition activates (logs "partition started"), then on the very next call the active→inactive transition fires. Traffic is blocked for at most one poll interval, which may be imperceptible.

5. **Logger called once per dropped message**: During an active partition at high send rates, a WARNING_LO log entry is emitted for every single `process_outbound()` call. With no rate-limiting on the log call, this can flood the log system.

6. **PRNG not advanced during partition drops**: The partition check fires before `check_loss()` in `process_outbound()`. During a partition, no PRNG values are consumed. This means the loss/duplication/jitter PRNG sequence is not disturbed by partition drops.

7. **State machine catches up by at most one transition per call**: If a very long real-time gap elapses between calls (e.g., process suspended), the machine does not retroactively process all missed transitions.

8. **`process_inbound()` does not check partition state**: The `process_inbound()` implementation does not call `is_partition_active()`. Inbound messages are not dropped by the partition mechanism. Partitions currently affect only the outbound path.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `partition_duration_ms > 0` and `partition_gap_ms > 0` are enforced by the caller before passing the config to `init()`. No assertions or guards inside `ImpairmentEngine` enforce these minimum values at runtime.
- [ASSUMPTION] `send_message()` is called frequently enough relative to `partition_gap_ms` and `partition_duration_ms` that the one-transition-per-call detection mechanism provides acceptable timing accuracy for simulation purposes.
- [ASSUMPTION] Single-threaded access per `ImpairmentEngine` instance.
- [ASSUMPTION] `m_partition_start_us` is reserved for future metric reporting (e.g., computing actual vs configured partition duration for observability).
- [UNKNOWN] Whether the Logger implementation can handle the burst of WARNING_LO messages generated during an active partition at high message rates without blocking, dropping entries, or causing performance degradation.
- [UNKNOWN] Whether `process_inbound()` is intended to also check partition state in a future implementation. Currently, inbound messages are unaffected by the partition mechanism.
- [UNKNOWN] Whether `TcpBackend::init()` is ever given a fully populated `ImpairmentConfig` with non-default partition fields. The traced `init()` code (TcpBackend.cpp:99–104) only sets `enabled` from channel config; `partition_enabled`, `partition_duration_ms`, and `partition_gap_ms` all remain at their defaults (false/0) unless a higher-level layer explicitly sets them.
