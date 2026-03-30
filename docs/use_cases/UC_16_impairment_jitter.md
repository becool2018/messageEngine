# UC_16 — Impairment: jitter

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.2, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

When `ImpairmentConfig::jitter_mean_ms` is non-zero, `ImpairmentEngine::process_outbound()` calls `m_prng.next_range(0U, m_cfg.jitter_variance_ms)` to generate a random jitter offset in milliseconds. This offset is added to the fixed base latency (`fixed_latency_ms * 1000` µs) to produce a per-message `release_us` which is stored in the delay buffer via `queue_to_delay_buf()`. As a result, successive messages released at the same wall-clock time receive different `release_us` values, causing them to be collected and transmitted at varying future times — simulating network jitter.

**Design note**: Despite the field name `jitter_mean_ms`, the implementation does not produce a Gaussian distribution centered on the mean. Instead, it uses `next_range(0, jitter_variance_ms)` to produce a uniform random offset in `[0, jitter_variance_ms]` milliseconds. The `jitter_mean_ms` field serves only as an enable guard (`> 0` means jitter is on). The actual per-message additional delay is drawn from `Uniform(0, jitter_variance_ms)`, not `Normal(jitter_mean_ms, jitter_variance_ms)`. The mean of the actual distribution is `jitter_variance_ms / 2`, regardless of `jitter_mean_ms`.

---

## 2. Entry Points

| Entry Point | File | Line | Signature |
|---|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp` | 347 | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp` | 151 | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `PrngEngine::next_range()` | `src/platform/PrngEngine.hpp` | 119 | `uint32_t PrngEngine::next_range(uint32_t lo, uint32_t hi)` |
| `PrngEngine::next()` | `src/platform/PrngEngine.hpp` | 67 | `uint64_t PrngEngine::next()` |

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **`TcpBackend::send_message(envelope)`** (TcpBackend.cpp:347).
   - Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` (lines 349–350).

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` (line 354). Returns OK.

3. **Get time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:362).
   - `clock_gettime(CLOCK_MONOTONIC, &ts)`.
   - `now_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000` (Timestamp.hpp:41–44).

4. **`m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:363).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:151):
   a. Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))` (lines 155–156).
   b. **Master switch**: `m_cfg.enabled == true`; continues.
   c. **Partition check**: `is_partition_active(now_us)` (line 169) — returns false; continues.
   d. **Loss check**: `check_loss()` (line 176). If `m_cfg.loss_probability > 0.0`, `check_loss()` (ImpairmentEngine.cpp:110) calls `m_prng.next_double()` — PRNG call #1 (optional). If message survives, continues.
   e. **Latency/jitter calculation** (lines 183–189):
      - `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL` (line 183).
        - Example: `fixed_latency_ms = 20` → `base_delay_us = 20000 µs`.
      - `jitter_us = 0ULL` (line 184).
      - **Jitter guard** (line 185): `if (m_cfg.jitter_mean_ms > 0U)` — if true, enter jitter path:
        - `jitter_offset_ms = m_prng.next_range(0U, m_cfg.jitter_variance_ms)` (line 186).
        - **Inside `next_range(lo=0, hi=jitter_variance_ms)`** (PrngEngine.hpp:119):
          - Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)`, `NEVER_COMPILED_OUT_ASSERT(hi >= lo)` (lines 122–123).
          - `raw = next()` (line 125):
            - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` (line 70).
            - `m_state ^= m_state << 13U` (line 73).
            - `m_state ^= m_state >> 7U` (line 74).
            - `m_state ^= m_state << 17U` (line 75).
            - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` (line 78).
            - Returns new `m_state`.
          - `range = hi - lo + 1U = jitter_variance_ms + 1U` (line 126).
          - `offset = static_cast<uint32_t>(raw % static_cast<uint64_t>(range))` (line 129).
          - `result = lo + offset = 0 + offset`. Result in `[0, jitter_variance_ms]`.
          - `NEVER_COMPILED_OUT_ASSERT(result >= lo && result <= hi)` (line 133).
          - Returns `result`.
        - `jitter_offset_ms` is a value in `[0, jitter_variance_ms]`.
          - Example: `jitter_variance_ms = 10`, PRNG returns 7 → `jitter_offset_ms = 7`.
        - `jitter_us = static_cast<uint64_t>(jitter_offset_ms) * 1000ULL` (line 187).
          - Example: `7 * 1000 = 7000 µs`.
      - `release_us = now_us + base_delay_us + jitter_us` (line 189).
        - Example: `now_us + 20000 + 7000 = now_us + 27000`.
   f. **Buffer capacity check** (line 192): `m_delay_count < IMPAIR_DELAY_BUF_SIZE`; passes.
   g. **`queue_to_delay_buf(in_env, release_us)`** (line 197): find first inactive `m_delay_buf[i]`; `envelope_copy(m_delay_buf[i].env, in_env)`; set `release_us`; set `active = true`; `++m_delay_count`; assert; return OK.
   h. **Duplication check** (line 203): if `m_cfg.duplication_probability > 0.0`, `apply_duplication()` is called — PRNG advanced again.
   i. Postcondition assertion (line 208); return `Result::OK`.

6. **`flush_delayed_to_clients(now_us)`** called from `send_message()` after `send_to_all_clients()`. The newly buffered entry has `release_us > now_us`; it is not yet collected. Returns without transmitting the buffered entry.

7. **On a future call** when `now_us' >= release_us`, the entry is collected by `collect_deliverable()` and transmitted by `flush_delayed_to_clients()` — same mechanics as UC_15, but `release_us` differs per message due to jitter.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)                      [TcpBackend.cpp:347]
├── Serializer::serialize(envelope, m_wire_buf, ...)    [Serializer.cpp:117]
├── timestamp_now_us()                                  [Timestamp.hpp:31]
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us) [ImpairmentEngine.cpp:151]
│   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
│   ├── [m_cfg.enabled == true → impairment path]
│   ├── is_partition_active(now_us) → false
│   ├── check_loss()                                    [ImpairmentEngine.cpp:110]
│   │   ├── [if loss_prob > 0.0]
│   │   └── m_prng.next_double()     ← PRNG call #1 (optional; loss check)
│   │       └── PrngEngine::next()   [xorshift64]
│   ├── base_delay_us = fixed_latency_ms * 1000
│   ├── jitter_us = 0
│   ├── [if jitter_mean_ms > 0U]     ← JITTER ENABLE GUARD (line 185)
│   │   └── m_prng.next_range(0U, jitter_variance_ms)  ← PRNG call #N (jitter)
│   │       ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)
│   │       ├── NEVER_COMPILED_OUT_ASSERT(hi >= lo)
│   │       ├── PrngEngine::next()
│   │       │   ├── m_state ^= m_state << 13U
│   │       │   ├── m_state ^= m_state >> 7U
│   │       │   └── m_state ^= m_state << 17U
│   │       ├── offset = raw % (jitter_variance_ms + 1)
│   │       ├── result = 0 + offset  ∈ [0, jitter_variance_ms]
│   │       └── NEVER_COMPILED_OUT_ASSERT(result >= 0 && result <= jitter_variance_ms)
│   │   jitter_us = result * 1000ULL
│   ├── total_delay_us = base_delay_us + jitter_us
│   ├── release_us = now_us + total_delay_us
│   ├── queue_to_delay_buf(in_env, release_us)          [ImpairmentEngine.cpp:83]
│   │   └── [find inactive slot; envelope_copy; mark active; ++m_delay_count]
│   ├── [if duplication_prob > 0.0]
│   │   └── apply_duplication()                        ← PRNG call #N+1 (optional)
│   │       └── m_prng.next_double()
│   └── return OK
├── send_to_all_clients(m_wire_buf, wire_len)            [TcpBackend.cpp:258]
└── flush_delayed_to_clients(now_us)                     [TcpBackend.cpp:280]
    └── collect_deliverable(now_us, delayed, 32)
        └── [newly buffered entry: release_us > now_us → 0 collected]
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `ImpairmentConfig::jitter_mean_ms` | `uint32_t` | `ImpairmentConfig.hpp:42` | Enable guard only: jitter active iff `> 0`; not used as a statistical parameter |
| `ImpairmentConfig::jitter_variance_ms` | `uint32_t` | `ImpairmentConfig.hpp:43` | Upper bound of uniform jitter distribution (milliseconds) |
| `ImpairmentConfig::fixed_latency_ms` | `uint32_t` | `ImpairmentConfig.hpp:38` | Base delay in ms; jitter is added on top of this |
| `PrngEngine::next_range(lo, hi)` | Inline method | `PrngEngine.hpp:119` | Generates uniform integer in `[lo, hi]` inclusive; called for jitter offset |
| `PrngEngine::next()` | Inline method | `PrngEngine.hpp:67` | xorshift64 core; called internally by `next_range` |
| `PrngEngine::m_state` | `uint64_t` | `PrngEngine.hpp:140` | PRNG state; advanced once per `next()` call |
| `ImpairmentEngine::m_delay_buf` | `DelayEntry[32]` member | `ImpairmentEngine.hpp:166` | Holds buffered messages with their per-message `release_us` |
| `ImpairmentEngine::queue_to_delay_buf()` | Private method | `ImpairmentEngine.cpp:83` | Writes the jitter-adjusted entry into the first free slot |
| `ImpairmentEngine::collect_deliverable()` | Method | `ImpairmentEngine.cpp:216` | Releases messages whose `release_us <= now_us` |
| `envelope_copy()` | Inline function | `MessageEnvelope.hpp:56` | `memcpy` of full envelope into delay buffer slot |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Jitter enable guard | ImpairmentEngine.cpp:185 | `m_cfg.jitter_mean_ms > 0U` | If false: `jitter_us = 0`; PRNG not called; pure fixed latency |
| Loss check fires first | ImpairmentEngine.cpp:176 (via `check_loss()`) | `loss_rand < loss_probability` | If true: message dropped before jitter is computed |
| `jitter_variance_ms == 0` with `jitter_mean_ms > 0` | PrngEngine.hpp:119 | `hi = 0, lo = 0` | `next_range(0, 0)` always returns 0; `jitter_us = 0` despite jitter being "enabled"; PRNG state still advances one step |
| `fixed_latency_ms == 0` | ImpairmentEngine.cpp:183 | `m_cfg.fixed_latency_ms == 0` | `base_delay_us = 0`; total delay equals jitter component alone |
| Delay buffer full | ImpairmentEngine.cpp:192 | `m_delay_count >= 32` | ERR_FULL returned; jitter was computed but message is dropped (or sent immediately — see UC_15 Risk 1) |
| Release time reached | ImpairmentEngine.cpp:229 | `release_us <= now_us` | Entry collected and transmitted; jitter delay has elapsed |

**`jitter_mean_ms` naming discrepancy**: The field is used exclusively as a boolean enable guard (`> 0`). The actual jitter distribution is `Uniform(0, jitter_variance_ms)`, not centered on `jitter_mean_ms`. If `jitter_variance_ms = 10`, the mean actual additional delay is 5 ms regardless of what `jitter_mean_ms` is set to. This is a naming discrepancy in the implementation.

**PRNG call ordering within `process_outbound()`**:
1. Loss check via `check_loss()`: `m_prng.next_double()` — only if `loss_probability > 0.0`.
2. Jitter offset: `m_prng.next_range(0, jitter_variance_ms)` — only if `jitter_mean_ms > 0`.
3. Duplication check via `apply_duplication()`: `m_prng.next_double()` — only if `duplication_probability > 0.0`.

Enabling or disabling any one of these shifts the PRNG subsequence seen by all later checks within the same `process_outbound()` call and all future calls.

---

## 7. Concurrency / Threading Behavior

- `PrngEngine::next_range()` and `PrngEngine::next()` modify `m_state` in place. `m_state` is a plain `uint64_t` (PrngEngine.hpp:140), not `std::atomic`. Not thread-safe.
- `m_delay_buf` and `m_delay_count` are modified by `queue_to_delay_buf()` (called from `process_outbound()`) and read/modified by `collect_deliverable()`. No locking or atomic protection.
- [ASSUMPTION] Single-threaded access per `ImpairmentEngine` instance.
- `timestamp_now_us()` → `clock_gettime(CLOCK_MONOTONIC)` is POSIX thread-safe.
- PRNG call count in `process_outbound()` when jitter is enabled: 0 or 1 (loss) + 1 (jitter) + 0 or 1 (duplication). The sequence is strictly deterministic given the same seed and configuration.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `next_range()` and `next()` are pure inline computations on `m_state` (a single `uint64_t`). Zero heap allocation. No pointers.
- `jitter_offset_ms` is a local `uint32_t` declared inside the `if (m_cfg.jitter_mean_ms > 0U)` block (ImpairmentEngine.cpp:186). Per Power of 10 rule 6, its scope is the smallest block where it is used.
- `jitter_us` is a `uint64_t` declared and initialized to `0ULL` at line 184, then conditionally updated at line 187. Both are stack variables; automatically reclaimed when `process_outbound()` returns.
- `m_delay_buf[i].release_us` stores the per-message jitter-adjusted release time as the only persistent outcome of the jitter computation.
- `base_delay_us`, `release_us` are also local `uint64_t` stack variables, reclaimed on return.
- No heap allocation anywhere. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| `jitter_variance_ms == 0` with `jitter_mean_ms > 0` | PrngEngine.hpp:119 | `next_range(0, 0)`: `range = 1`, `offset = raw % 1 = 0`, returns 0. No error; `jitter_us = 0`. PRNG state still advances by one iteration. |
| `hi < lo` in `next_range` | PrngEngine.hpp:123 | `NEVER_COMPILED_OUT_ASSERT(hi >= lo)` fires. With `lo = 0` and `hi = jitter_variance_ms` (a `uint32_t`), this cannot underflow. |
| `m_prng.m_state == 0` before `next_range` | PrngEngine.hpp:122 | `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)` fires. Cannot happen if `seed()` was called correctly during `init()` (coerces 0 to 1). |
| Delay buffer full after jitter computed | ImpairmentEngine.cpp:192 | ERR_FULL returned; jitter computation was done but its result is discarded with the dropped/bypassed message |
| Potential overflow of `jitter_us` | ImpairmentEngine.cpp:187 | `jitter_offset_ms` is `uint32_t` (max ~4.29B); cast to `uint64_t` before multiply by 1000: max `jitter_us` ≈ 4.29×10¹² µs. Safe within `uint64_t`. |
| Potential overflow of `release_us` | ImpairmentEngine.cpp:189 | `now_us + base_delay_us + jitter_us`: with both `fixed_latency_ms` and `jitter_variance_ms` as `uint32_t`, max total extra delay ≈ 8.59×10¹² µs (~99 days). A realistic `now_us` plus this sum fits safely in `uint64_t`. |

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Socket I/O | `tcp_send_frame()` | Not called at jitter-buffering time; called when `release_us` matures in a later `flush_delayed_to_clients()` |
| Logger | `Logger::log()` | Not called on the jitter path itself; called on buffer-full (WARNING_HI), loss drop (WARNING_LO), duplication (WARNING_LO), or partition drop (WARNING_LO) |

No external interaction occurs during the jitter offset computation itself. `PrngEngine` is entirely self-contained.

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

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant ImpairmentEngine
    participant PrngEngine

    Note over Caller,PrngEngine: Messages A and B both sent at T=0;<br/>jitter_variance_ms=10; fixed_latency_ms=20

    Caller->>TcpBackend: send_message(A)
    TcpBackend->>ImpairmentEngine: process_outbound(A, T0)
    ImpairmentEngine->>PrngEngine: next_range(0, 10)
    PrngEngine-->>ImpairmentEngine: 7 (example)
    Note over ImpairmentEngine: jitter_us=7000; release_us=T0+27000<br/>queue_to_delay_buf(A, T0+27000)
    ImpairmentEngine-->>TcpBackend: OK
    TcpBackend-->>Caller: OK

    Caller->>TcpBackend: send_message(B)
    TcpBackend->>ImpairmentEngine: process_outbound(B, T0)
    ImpairmentEngine->>PrngEngine: next_range(0, 10)
    PrngEngine-->>ImpairmentEngine: 3 (example; different PRNG state)
    Note over ImpairmentEngine: jitter_us=3000; release_us=T0+23000<br/>queue_to_delay_buf(B, T0+23000)
    ImpairmentEngine-->>TcpBackend: OK
    TcpBackend-->>Caller: OK

    Note over Caller,PrngEngine: At T0+23001µs: collect_deliverable<br/>slot[A]: T0+27000 > T0+23001 → skip<br/>slot[B]: T0+23000 ≤ T0+23001 → COLLECT B → transmitted first

    Note over Caller,PrngEngine: At T0+27001µs: collect_deliverable<br/>slot[A]: T0+27000 ≤ T0+27001 → COLLECT A → transmitted second<br/>Arrival order: B then A (JITTER-INDUCED REORDERING)
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (`ImpairmentEngine::init()` called from `TcpBackend::init()`):
- `jitter_mean_ms` defaults to `0` in `impairment_config_default()` (ImpairmentConfig.hpp:83). Jitter is off by default.
- `jitter_variance_ms` defaults to `0` (ImpairmentConfig.hpp:84).
- Both must be set to non-zero values in the `ImpairmentConfig` before calling `ImpairmentEngine::init()` to enable jitter.
- PRNG is seeded in `init()` via `m_prng.seed(seed)` (ImpairmentEngine.cpp:55). The entire sequence of jitter values for a run is fully determined by the seed value and the order of `process_outbound()` calls.
- `m_delay_buf` zeroed via `memset` (line 58). `m_delay_count = 0` (line 59). `m_initialized = true` (line 72).

**Runtime phase**:
- Each `process_outbound()` call with jitter enabled advances `m_prng.m_state` once for the jitter `next_range` call (plus once for loss and once for duplication if those are also enabled).
- The PRNG sequence is shared across all impairment types. Enabling or disabling loss (`loss_probability > 0`) adds or removes a `next_double()` call before the jitter call, shifting the jitter subsequence for all subsequent messages even with the same seed.
- `m_cfg` is immutable after `init()`. `fixed_latency_ms` and `jitter_variance_ms` cannot change at runtime.

---

## 14. Known Risks / Observations

1. **`jitter_mean_ms` is not the statistical mean of the jitter distribution**: The mean jitter applied per message is `jitter_variance_ms / 2`, not `jitter_mean_ms`. The field name is misleading. The implementation at ImpairmentEngine.cpp:185–187 uses a uniform draw from `[0, jitter_variance_ms]`, not a Gaussian centered on `jitter_mean_ms`. This is a naming discrepancy — `jitter_mean_ms` functions as an enable flag, not a distribution parameter.

2. **Jitter can cause unintentional message reordering**: As shown in the sequence diagram, two messages sent at the same `now_us` can arrive at the receiver in reverse order if their jitter values differ. This is an expected side-effect of jitter simulation but may interact unexpectedly with ordered-channel logic at higher layers.

3. **PRNG call ordering is configuration-dependent**: Enabling loss (`loss_probability > 0`) inserts a `next_double()` call via `check_loss()` before the jitter `next_range()` call. With the same seed, changing `loss_probability` from 0 to any positive value will change all jitter values for all subsequent messages.

4. **`next_range` uses modulo reduction**: `raw % range` introduces a slight modulo bias for range values that do not divide `UINT64_MAX + 1` evenly. For jitter simulation this bias is negligible (sub-percent level for small `jitter_variance_ms`), but it is not a perfectly uniform distribution.

5. **Upper bound is inclusive**: `next_range(0, jitter_variance_ms)` can return exactly `jitter_variance_ms`. The maximum additional delay per message is `jitter_variance_ms` ms, not `jitter_variance_ms - 1`.

6. **Jitter adds to, never subtracts from, the base latency**: `jitter_offset_ms` is drawn from `[0, jitter_variance_ms]` (always non-negative). Symmetric jitter is not implemented.

7. **`jitter_variance_ms == 0` with `jitter_mean_ms > 0` wastes a PRNG call**: The guard `jitter_mean_ms > 0` passes, `next_range(0, 0)` is called, always returns 0, and `jitter_us = 0`. One PRNG step is consumed for no effect on timing.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `jitter_mean_ms` and `jitter_variance_ms` are set to meaningful non-zero values by a higher-level layer before `ImpairmentEngine::init()` is called. `TcpBackend::init()` only populates `enabled` from channel config; jitter fields remain at default (0) unless explicitly set.
- [ASSUMPTION] The caller accepts that jitter values are drawn from a uniform distribution over `[0, jitter_variance_ms]`, not a Gaussian distribution.
- [ASSUMPTION] Single-threaded access per `ImpairmentEngine` instance.
- [UNKNOWN] Whether `jitter_mean_ms` is intended to be used as a true statistical mean in a future implementation (e.g., as the center of a Gaussian). The field name suggests this intent but the current implementation is uniform.
- [UNKNOWN] The exact PRNG output sequence for a given seed and message sequence cannot be determined statically; the xorshift64 sequence must be computed.
- [UNKNOWN] Whether `flush_delayed_to_clients()` polling frequency is high enough relative to `jitter_variance_ms` to faithfully represent the configured jitter distribution at the receiver.
