## UC_16: ImpairmentEngine applies jitter by randomizing per-message delay around a configured mean

---

## 1. Use Case Overview

When `ImpairmentConfig::jitter_mean_ms` is non-zero, `ImpairmentEngine::process_outbound()` calls `m_prng.next_range(0, m_cfg.jitter_variance_ms)` to generate a random jitter offset in milliseconds. This offset is added to the fixed base latency (`fixed_latency_ms * 1000` µs) to produce a per-message `release_us`. As a result, successive messages released at the same wall-clock time receive different `release_us` values, causing them to be collected and transmitted at varying future times — simulating network jitter.

**Design note**: Despite the field name `jitter_mean_ms`, the implementation does not produce a Gaussian distribution centered on the mean. Instead, it uses `next_range(0, jitter_variance_ms)` to produce a uniform random offset in `[0, jitter_variance_ms]` milliseconds. The `jitter_mean_ms` field serves only as an enable guard (`> 0` means jitter is on). The actual per-message additional delay is drawn from `Uniform(0, jitter_variance_ms)`, not `Normal(jitter_mean_ms, jitter_variance_ms)`. The mean of the actual distribution is `jitter_variance_ms / 2`, regardless of `jitter_mean_ms`.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp:249` | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp:62` | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `PrngEngine::next_range()` | `src/platform/PrngEngine.hpp:113` | `uint32_t PrngEngine::next_range(uint32_t lo, uint32_t hi)` |

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **`TcpBackend::send_message(envelope)`** (TcpBackend.cpp:249).
   - Preconditions: `assert(m_open)`, `assert(envelope_valid(envelope))`.

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)`. Returns OK.

3. **Get time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:264).
   - `clock_gettime(CLOCK_MONOTONIC, &ts)`.
   - `now_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000`.

4. **`m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:265).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:62):
   a. Preconditions asserted: `assert(m_initialized)`, `assert(envelope_valid(in_env))`.
   b. **Master switch**: `m_cfg.enabled == true`; continues.
   c. **Partition check**: `is_partition_active(now_us)` returns false; continues.
   d. **Loss check** (lines 100–108): if `m_cfg.loss_probability > 0.0`, PRNG call #1 (`next_double()`) is made. If message survives (or `loss_probability == 0.0` so the PRNG is not called at all), execution continues.
   e. **Latency/jitter calculation** (lines 111–122):
      - `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL`.
        - Example: `fixed_latency_ms = 20` → `base_delay_us = 20000 µs`.
      - `jitter_us = 0ULL`.
      - **Jitter guard** (line 114): `if (m_cfg.jitter_mean_ms > 0U)` — if true, enter jitter path:
        - `jitter_offset_ms = m_prng.next_range(0U, m_cfg.jitter_variance_ms)` (line 117).
        - **Inside `next_range(lo=0, hi=jitter_variance_ms)`** (PrngEngine.hpp:113):
          - Preconditions: `assert(m_state != 0ULL)`, `assert(hi >= lo)`.
          - `raw = next()`:
            - `assert(m_state != 0ULL)`.
            - `m_state ^= m_state << 13U`.
            - `m_state ^= m_state >> 7U`.
            - `m_state ^= m_state << 17U`.
            - `assert(m_state != 0ULL)`.
            - Returns new `m_state` as `raw`.
          - `range = hi - lo + 1 = jitter_variance_ms + 1`.
          - `offset = static_cast<uint32_t>(raw % static_cast<uint64_t>(range))`.
          - `result = lo + offset = 0 + offset`. Result in `[0, jitter_variance_ms]`.
          - `assert(result >= lo && result <= hi)`.
          - Returns `result`.
        - `jitter_offset_ms` is now a value in `[0, jitter_variance_ms]`.
          - Example: `jitter_variance_ms = 10`, PRNG returns 7 → `jitter_offset_ms = 7`.
        - `jitter_us = static_cast<uint64_t>(jitter_offset_ms) * 1000ULL`.
          - Example: `7 * 1000 = 7000 µs`.
      - `total_delay_us = base_delay_us + jitter_us`.
        - Example: `20000 + 7000 = 27000 µs`.
      - `release_us = now_us + total_delay_us`.
        - Example: if `now_us = 1000000000` → `release_us = 1000027000`.
   f. **Buffer capacity check** (line 125): `m_delay_count < IMPAIR_DELAY_BUF_SIZE`; passes.
   g. **Buffer insertion** (lines 132–141): find first inactive `m_delay_buf[i]`; `envelope_copy(m_delay_buf[i].env, in_env)`; set `release_us`; set `active = true`; `++m_delay_count`; assert; break.
   h. **Duplication check** (line 144): if `m_cfg.duplication_probability > 0.0`, PRNG advanced again.
   i. Postcondition assertions; return `Result::OK`.

6. **`collect_deliverable(now_us, ...)` called** from `send_message()` immediately after (TcpBackend.cpp:273). The newly buffered entry has `release_us > now_us`; it is not yet collected. Returns 0.

7. **On a future call** when `now_us' >= release_us`, the entry is collected and transmitted (same mechanics as UC_15, but `release_us` differs per message due to jitter).

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)
├── Serializer::serialize(envelope, m_wire_buf, ...)
├── timestamp_now_us()
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us)
│   ├── assert(m_initialized)
│   ├── assert(envelope_valid(in_env))
│   ├── [partition check → false]
│   ├── [if loss_prob > 0.0]
│   │   └── PrngEngine::next_double()               ← PRNG call #1 (optional; loss check)
│   │       └── PrngEngine::next() [xorshift64]
│   ├── base_delay_us = fixed_latency_ms * 1000
│   ├── jitter_us = 0
│   ├── [if jitter_mean_ms > 0U]                    ← JITTER ENABLE GUARD
│   │   └── PrngEngine::next_range(0, jitter_variance_ms)  ← PRNG call #N (jitter)
│   │       ├── assert(m_state != 0ULL)
│   │       ├── assert(hi >= lo)
│   │       ├── PrngEngine::next()
│   │       │   ├── m_state ^= m_state << 13U
│   │       │   ├── m_state ^= m_state >> 7U
│   │       │   └── m_state ^= m_state << 17U
│   │       ├── offset = raw % (jitter_variance_ms + 1)
│   │       ├── result = 0 + offset                 ∈ [0, jitter_variance_ms]
│   │       └── assert(result >= 0 && result <= jitter_variance_ms)
│   │   jitter_us = result * 1000ULL
│   ├── total_delay_us = base_delay_us + jitter_us
│   ├── release_us = now_us + total_delay_us
│   ├── [find inactive slot; envelope_copy; mark active; ++m_delay_count]
│   ├── [if duplication_prob > 0.0]
│   │   └── PrngEngine::next_double()               ← PRNG call #N+1 (optional; dup check)
│   └── return OK
├── ImpairmentEngine::collect_deliverable(now_us, buf, 32)
│   └── [newly buffered entry: release_us > now_us → 0 collected]
└── return OK
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `ImpairmentConfig::jitter_mean_ms` | `uint32_t` | `ImpairmentConfig.hpp:40` | Enable guard only: jitter active iff `> 0`; not used as a statistical parameter |
| `ImpairmentConfig::jitter_variance_ms` | `uint32_t` | `ImpairmentConfig.hpp:41` | Upper bound of uniform jitter distribution (milliseconds) |
| `ImpairmentConfig::fixed_latency_ms` | `uint32_t` | `ImpairmentConfig.hpp:36` | Base delay in ms; jitter is added on top of this |
| `PrngEngine::next_range(lo, hi)` | Inline method | `PrngEngine.hpp:113` | Generates uniform integer in `[lo, hi]` inclusive; called for jitter offset |
| `PrngEngine::next()` | Inline method | `PrngEngine.hpp:63` | xorshift64 core; called internally by `next_range` |
| `PrngEngine::m_state` | `uint64_t` | `PrngEngine.hpp:134` | PRNG state; advanced once per `next()` call |
| `ImpairmentEngine::m_delay_buf` | Member array `DelayEntry[32]` | `ImpairmentEngine.hpp:136` | Holds buffered messages with their per-message `release_us` |
| `ImpairmentEngine::collect_deliverable()` | Method | `ImpairmentEngine.cpp:174` | Releases messages whose `release_us <= now_us` |
| `envelope_copy()` | Inline function | `MessageEnvelope.hpp:49` | `memcpy` of full envelope into delay buffer slot |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Jitter enable guard | ImpairmentEngine.cpp:114 | `m_cfg.jitter_mean_ms > 0U` | If false: `jitter_us = 0`; PRNG not called; pure fixed latency |
| Loss check fires first | ImpairmentEngine.cpp:100–108 | `loss_rand < loss_probability` | If true: message dropped before jitter is computed |
| `jitter_variance_ms == 0` with `jitter_mean_ms > 0` | PrngEngine.hpp:113 | `hi = 0, lo = 0` | `next_range(0, 0)` always returns 0; `jitter_us = 0` despite jitter being "enabled"; PRNG state still advances |
| `fixed_latency_ms == 0` | ImpairmentEngine.cpp:111 | `m_cfg.fixed_latency_ms == 0` | `base_delay_us = 0`; total delay equals jitter component alone |
| Delay buffer full | ImpairmentEngine.cpp:125 | `m_delay_count >= 32` | ERR_FULL returned; jitter was computed but message is dropped |
| Release time reached | ImpairmentEngine.cpp:187 | `release_us <= now_us` | Entry collected and transmitted; jitter delay has elapsed |

**`jitter_mean_ms` naming mismatch**: The field is used exclusively as a boolean enable guard (`> 0`). The actual jitter distribution is `Uniform(0, jitter_variance_ms)`, not centered on `jitter_mean_ms`. If `jitter_variance_ms = 10`, the mean actual additional delay is 5 ms regardless of what `jitter_mean_ms` is set to.

**PRNG call ordering within `process_outbound()`**:
1. Loss check: `next_double()` — only if `loss_probability > 0.0`.
2. Jitter offset: `next_range(0, jitter_variance_ms)` — only if `jitter_mean_ms > 0`.
3. Duplication check: `next_double()` — only if `duplication_probability > 0.0`.

Enabling or disabling any one of these shifts the PRNG subsequence seen by all later checks.

---

## 7. Concurrency / Threading Behavior

- `PrngEngine::next_range()` and `PrngEngine::next()` modify `m_state` in place. `m_state` is a plain `uint64_t`, not `std::atomic`. Not thread-safe.
- `m_delay_buf` and `m_delay_count` are modified by `process_outbound()` and read/modified by `collect_deliverable()`. No locking or atomic protection is present.
- [ASSUMPTION] Single-threaded access per `TcpBackend`/`ImpairmentEngine` instance.
- `timestamp_now_us()` → `clock_gettime(CLOCK_MONOTONIC)` is POSIX thread-safe.
- PRNG call count in `process_outbound()` when jitter is enabled: 0 or 1 (loss, if configured) + 1 (jitter) + 0 or 1 (duplication, if configured). The sequence is strictly deterministic given the same seed and the same configuration.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `next_range()` and `next()` are pure inline computations on `m_state` (a single `uint64_t`). Zero heap allocation. No pointers.
- `jitter_offset_ms` is a local `uint32_t` declared inside the `if (m_cfg.jitter_mean_ms > 0U)` block. Per Power of 10 rule 6, its scope is the smallest block where it is used.
- `jitter_us` is a local `uint64_t` declared and initialized to `0ULL` before the jitter block, then conditionally updated. Both are stack variables; automatically reclaimed when `process_outbound()` returns.
- `m_delay_buf[i].release_us` stores the per-message jitter-adjusted release time as the only persistent outcome of the jitter computation.
- `total_delay_us` and `release_us` are also local `uint64_t` stack variables, reclaimed on return.
- No heap allocation anywhere. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| `jitter_variance_ms == 0` with `jitter_mean_ms > 0` | PrngEngine.hpp:113 | `next_range(0, 0)`: `range = 1`, `offset = raw % 1 = 0`, returns 0. No error; `jitter_us = 0`. PRNG state still advances by one iteration. |
| `hi < lo` in `next_range` | PrngEngine.hpp:117 | `assert(hi >= lo)` fires. With `lo = 0` and `hi = jitter_variance_ms` (a `uint32_t`), this cannot underflow. |
| `m_prng.m_state == 0` before `next_range` | PrngEngine.hpp:116 | `assert(m_state != 0ULL)` fires. Cannot happen if `seed()` was called correctly during `init()` (coerces 0 to 1). |
| Delay buffer full after jitter computed | ImpairmentEngine.cpp:125 | ERR_FULL returned; jitter computation was done but its result is discarded with the message |
| Potential overflow of `jitter_us` | ImpairmentEngine.cpp:118 | `jitter_offset_ms` is `uint32_t` (max ~4.29B); cast to `uint64_t` before multiply by 1000: max `jitter_us` ≈ 4.29e12 µs. Safe within `uint64_t`. |
| Potential overflow of `release_us` | ImpairmentEngine.cpp:122 | `now_us + total_delay_us`: with `fixed_latency_ms` and `jitter_variance_ms` both `uint32_t`, max `total_delay_us` ≈ 8.59e12 µs (~99 days). A realistic `now_us` plus this sum fits safely in `uint64_t`. |

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Socket I/O | `tcp_send_frame()` | Not called at jitter-buffering time; called when `release_us` matures in a later `collect_deliverable()` |
| Logger | `Logger::log()` | Not called on the jitter path itself; called only on buffer-full (WARNING_HI) or duplication events |

No external interaction occurs during the jitter offset computation. `PrngEngine` is entirely self-contained. The only observable external effect of jitter is delayed socket transmission at a future poll point.

---

## 11. State Changes / Side Effects

| State | Object | Change |
|---|---|---|
| `m_prng.m_state` | `PrngEngine` | Advanced by one xorshift64 iteration for the jitter `next_range` call |
| `m_delay_buf[i].release_us` | `ImpairmentEngine` | Set to `now_us + base_delay_us + jitter_us` — varies per message due to jitter |
| `m_delay_buf[i].env` | `ImpairmentEngine` | Filled with `memcpy` of `in_env` |
| `m_delay_buf[i].active` | `ImpairmentEngine` | Set to `true` |
| `m_delay_count` | `ImpairmentEngine` | Incremented by 1 |

The only observable jitter effect is the variation in `release_us` across successive messages sent at the same `now_us`. Messages sent simultaneously with different PRNG-generated jitter offsets will be collected and transmitted at different future times, producing irregular inter-arrival spacing at the receiver.

---

## 12. Sequence Diagram (ASCII)

```
[Message A and B both sent at T=0; jitter_variance_ms=10; fixed_latency_ms=20]

send_message(A)                           send_message(B)
  process_outbound(A, T0)                   process_outbound(B, T0)
  |                                         |
  | base_delay_us = 20000                   | base_delay_us = 20000
  |                                         |
  | next_range(0, 10)                        | next_range(0, 10)
  |   next() → rawA                         |   next() → rawB   [different state]
  |   offset = rawA % 11 = 7               |   offset = rawB % 11 = 3
  |   return 7                              |   return 3
  |                                         |
  | jitter_us = 7000                        | jitter_us = 3000
  | release_us = T0 + 27000                 | release_us = T0 + 23000
  | m_delay_buf[0] = {A, T0+27000, true}    | m_delay_buf[1] = {B, T0+23000, true}

collect_deliverable at T0+23001:
  slot0: T0+27000 > T0+23001 → skip A
  slot1: T0+23000 <= T0+23001 → COLLECT B → transmitted first

collect_deliverable at T0+27001:
  slot0: T0+27000 <= T0+27001 → COLLECT A → transmitted second
  slot1: inactive

Arrival order at receiver: B then A
Send    order:             A then B
Result: JITTER-INDUCED REORDERING
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (`ImpairmentEngine::init()` called from `TcpBackend::init()`):
- `jitter_mean_ms` defaults to `0` in `impairment_config_default()`. Jitter is off by default.
- `jitter_variance_ms` defaults to `0`.
- Both must be set to non-zero values in the `ImpairmentConfig` before calling `ImpairmentEngine::init()` to enable jitter.
- PRNG is seeded in `init()` via `m_prng.seed(seed)`. The entire sequence of jitter values for a run is fully determined by the seed value and the order of `process_outbound()` calls.
- `m_delay_buf` zeroed via `memset`. `m_delay_count = 0`. `m_initialized = true`.

**Runtime phase**:
- Each `process_outbound()` call with jitter enabled advances `m_prng.m_state` once for the jitter `next_range` call (plus once for loss and once for duplication if those are also enabled).
- The PRNG sequence is shared across all impairment types. Enabling or disabling loss (`loss_probability > 0`) adds or removes a `next_double()` call before the jitter call, shifting the jitter subsequence for all subsequent messages even with the same seed.
- `m_cfg` is immutable after `init()`. `fixed_latency_ms` and `jitter_variance_ms` cannot change at runtime.

---

## 14. Known Risks / Observations

1. **`jitter_mean_ms` is not the statistical mean of the jitter distribution**: The mean jitter applied per message is `jitter_variance_ms / 2`, not `jitter_mean_ms`. The field name is misleading. The implementation comment at ImpairmentEngine.cpp:116 reads "Gaussian-like jitter centered on mean with variance" but the implementation is a single uniform draw — not Gaussian, and not centered on `jitter_mean_ms`.

2. **Jitter can cause unintentional message reordering**: As shown in the sequence diagram, two messages sent at the same `now_us` can arrive at the receiver in reverse order if their jitter values differ. This is an expected side-effect of jitter simulation, but it may interact unexpectedly with ordered-channel logic at higher layers.

3. **PRNG call ordering is configuration-dependent**: Enabling loss (`loss_probability > 0`) inserts a `next_double()` call before the jitter `next_range()` call. With the same seed, changing `loss_probability` from 0 to any positive value will change all jitter values for all subsequent messages, because the loss PRNG call consumes one state step before jitter.

4. **`next_range` uses modulo reduction**: `raw % range` introduces a slight modulo bias for range values that do not divide `UINT64_MAX + 1` evenly. For jitter simulation this bias is negligible (sub-percent level for small `jitter_variance_ms`), but it is not a perfectly uniform distribution.

5. **Upper bound is inclusive**: `next_range(0, jitter_variance_ms)` can return exactly `jitter_variance_ms`. The maximum additional delay per message is `jitter_variance_ms` ms, not `jitter_variance_ms - 1`.

6. **Jitter adds to, never subtracts from, the base latency**: `jitter_offset_ms` is drawn from `[0, jitter_variance_ms]` (always non-negative). Jitter can only increase delay beyond `fixed_latency_ms`. Symmetric jitter — where some messages arrive faster than the mean and some slower — is not implemented.

7. **`jitter_variance_ms == 0` with `jitter_mean_ms > 0` wastes a PRNG call**: The guard `jitter_mean_ms > 0` passes, `next_range(0, 0)` is called, always returns 0, and `jitter_us = 0`. One PRNG step is consumed for no effect on timing, but this shifts the subsequent duplication PRNG call.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `jitter_mean_ms` and `jitter_variance_ms` are set to meaningful non-zero values by a higher-level layer before `ImpairmentEngine::init()` is called.
- [ASSUMPTION] The caller accepts that jitter values are drawn from a uniform distribution over `[0, jitter_variance_ms]`, not a Gaussian distribution.
- [ASSUMPTION] Single-threaded access per `ImpairmentEngine` instance.
- [UNKNOWN] Whether `jitter_mean_ms` is intended to be used as a true statistical mean in a future implementation (e.g., as the center of a Gaussian). The comment at ImpairmentEngine.cpp:116 suggests this intent but the implementation is uniform.
- [UNKNOWN] The exact PRNG output sequence for a given seed and message sequence cannot be determined statically; the xorshift64 sequence must be computed to inspect specific values.
- [UNKNOWN] Whether `collect_deliverable()` polling frequency is high enough relative to `jitter_variance_ms` to faithfully represent the configured jitter distribution at the receiver. If polling is coarser than `jitter_variance_ms`, all messages within a polling window appear to arrive simultaneously, eliminating the observable jitter effect.
- [UNKNOWN] Whether the `TcpBackend::init()` path ever sets `jitter_mean_ms` or `jitter_variance_ms` from a higher-level config structure; the traced `init()` code only sets `enabled`.

---