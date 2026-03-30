# UC_13 — Impairment: packet loss

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.3, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()` to transmit a `MessageEnvelope` over TCP. The
message is serialized to `m_wire_buf`. `ImpairmentEngine::process_outbound()` is then called.
Inside `process_outbound()`, the private helper `check_loss()` is called: the PRNG generates a
random double in [0.0, 1.0). If that value is less than `ImpairmentConfig::loss_probability`,
the message is silently discarded and no bytes are transmitted. `process_outbound()` returns
`Result::ERR_IO`, which `TcpBackend::send_message()` converts to `Result::OK` — the drop is
transparent at the API boundary because loss is a simulated network event, not an application
error.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp:347` | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp:151` | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `ImpairmentEngine::check_loss()` | `src/platform/ImpairmentEngine.cpp:110` | `bool ImpairmentEngine::check_loss()` (private helper) |

Callers above `TcpBackend` invoke through the `TransportInterface` virtual dispatch table
(declared in `src/core/TransportInterface.hpp`). `send_message` is declared `virtual` in
`TcpBackend` and is resolved at runtime through the vtable.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Caller invokes `TcpBackend::send_message(envelope)`** (TcpBackend.cpp:347).
   - Precondition assertions:
     `NEVER_COMPILED_OUT_ASSERT(m_open)` [line 349]
     `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` [line 350]

2. **Serialize to wire buffer**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` (TcpBackend.cpp:354-358).
   - Writes serialized bytes into the `m_wire_buf[8192]` member array.
   - Returns `Result::OK` or an error code; checked immediately.
   - If non-OK: log WARNING_LO "Serialize failed", return error.

3. **Obtain current time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:362).
   - Calls `clock_gettime(CLOCK_MONOTONIC, &ts)` (Timestamp.hpp:35).
   - Returns `uint64_t` microseconds since the monotonic epoch.

4. **Call `m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:363).
   - `m_impairment` is an `ImpairmentEngine` instance embedded by value in `TcpBackend`
     (no pointer indirection).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:151):
   a. Preconditions asserted:
      `NEVER_COMPILED_OUT_ASSERT(m_initialized)` [line 155]
      `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))` [line 156]
   b. **Master switch check** [line 159]: `if (!m_cfg.enabled)` — if impairments are
      globally disabled, the message is queued in the delay buffer at `release_us = now_us`
      (zero delay) via `queue_to_delay_buf()`. The loss path is not taken.
   c. **Partition check** [line 169]: `if (is_partition_active(now_us))` — if a partition
      is active, log WARNING_LO "message dropped (partition active)" and return `Result::ERR_IO`.
      This is a distinct drop path; `check_loss()` is not reached.
   d. **Loss check** [line 176]: `if (check_loss())`.
      - Inside `check_loss()` [ImpairmentEngine.cpp:110]:
        `NEVER_COMPILED_OUT_ASSERT(m_initialized)` [line 112]
        `NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0)` [line 113]
        Guard: `if (m_cfg.loss_probability <= 0.0) return false;` [line 115]
        `rand_val = m_prng.next_double()` — advances PRNG state via one xorshift64 iteration
        (PrngEngine.hpp:91).
        `NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0)` [line 119]
        Returns `rand_val < m_cfg.loss_probability`.
      - If `check_loss()` returns true (loss fires):
        Log WARNING_LO "message dropped (loss probability)" [lines 177-179]
        Return `Result::ERR_IO` [line 179]

6. **Back in `send_message()`** (TcpBackend.cpp:364-366):
   - `if (res == Result::ERR_IO)` evaluates true.
   - `return Result::OK` — the drop is converted to a success return.
   - No bytes are written to any socket.

7. **Execution terminates**. The message was consumed and discarded. Serialized bytes in
   `m_wire_buf` are never transmitted.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)                              [TcpBackend.cpp:347]
├── NEVER_COMPILED_OUT_ASSERT(m_open)
├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, 8192, wire_len) [line 354]
├── timestamp_now_us()                                          [line 362]
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)                     [Timestamp.hpp:35]
├── ImpairmentEngine::process_outbound(envelope, now_us)        [line 363]
│   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
│   ├── [if !m_cfg.enabled] → queue_to_delay_buf(now_us) [NOT taken in loss path]
│   ├── is_partition_active(now_us)                             [line 169, skipped]
│   ├── check_loss()                                            [line 176]
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   │   ├── NEVER_COMPILED_OUT_ASSERT(loss_probability <= 1.0)
│   │   ├── [if loss_probability <= 0.0: return false]
│   │   ├── m_prng.next_double()                               [PrngEngine.hpp:91]
│   │   │   ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0)
│   │   │   ├── PrngEngine::next()                             [PrngEngine.hpp:67]
│   │   │   │   ├── m_state ^= m_state << 13U
│   │   │   │   ├── m_state ^= m_state >> 7U
│   │   │   │   └── m_state ^= m_state << 17U
│   │   │   ├── result = (double)raw / (double)UINT64_MAX
│   │   │   └── NEVER_COMPILED_OUT_ASSERT(result >= 0.0 && result < 1.0)
│   │   └── return rand_val < loss_probability                  ← LOSS DECISION
│   ├── [if check_loss() true]
│   │   ├── Logger::log(WARNING_LO, "message dropped (loss probability)")
│   │   └── return Result::ERR_IO                              ← LOSS DROP POINT
│   └── [not reached: latency/dup/buffer logic]
└── [if res == ERR_IO] return Result::OK                        ← DROP CONVERTED TO OK
    [NO socket write; m_client_count check and send_to_all_clients not reached]
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Transport layer; owns `ImpairmentEngine` by value |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Applies loss decision via `check_loss()` private helper |
| `ImpairmentEngine::check_loss()` | Private method | `ImpairmentEngine.cpp:110` | Isolated loss logic; PRNG call + probability comparison |
| `ImpairmentConfig` | POD struct | `src/platform/ImpairmentConfig.hpp` | Holds `loss_probability` (double), `enabled` (bool) |
| `PrngEngine` | Class | `src/platform/PrngEngine.hpp` | xorshift64 PRNG; all methods inline; embedded in `ImpairmentEngine` as `m_prng` |
| `Serializer::serialize()` | Static function | `src/core/Serializer.hpp` | Converts envelope to wire bytes before impairment check |
| `timestamp_now_us()` | Inline function | `src/core/Timestamp.hpp:31` | CLOCK_MONOTONIC microsecond clock |
| `Logger::log()` | Static function | `src/core/Logger.hpp` | Records WARNING_LO event on drop |
| `envelope_valid()` | Inline function | `src/core/MessageEnvelope.hpp:63` | Validates message_type, payload_length, source_id |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Impairments master switch | ImpairmentEngine.cpp:159 | `!m_cfg.enabled` | If true: zero-delay buffer path via `queue_to_delay_buf()`, returns OK without loss check |
| Partition active check | ImpairmentEngine.cpp:169 | `is_partition_active(now_us)` | If true: drop with ERR_IO before loss check is reached |
| Loss probability guard | ImpairmentEngine.cpp:115 (in `check_loss`) | `m_cfg.loss_probability <= 0.0` | If true: return false immediately; PRNG not called |
| Loss decision | ImpairmentEngine.cpp:120 (in `check_loss`) | `rand_val < m_cfg.loss_probability` | If true: `check_loss()` returns true → log + return ERR_IO |
| ERR_IO interception | TcpBackend.cpp:364 | `res == Result::ERR_IO` | If true: return OK to caller (drop is transparent) |
| Serialization failure | TcpBackend.cpp:356 | `!result_ok(res)` | If true: returns error before impairment engine is reached |

**Ordering note**: Partition is checked before loss. If both are configured and a partition is
active, the partition drop fires first and `check_loss()` is never called.

---

## 7. Concurrency / Threading Behavior

- `TcpBackend::send_message()` and `ImpairmentEngine::process_outbound()` contain no mutex,
  spinlock, or atomic operations anywhere in this call path.
- `PrngEngine::m_state` is a plain `uint64_t` (not `std::atomic`). It is not thread-safe.
  [ASSUMPTION] The design assumes `send_message()` is called from a single thread at a time
  per `TcpBackend` instance.
- `Logger::log()` uses a stack-local buffer per call; no shared mutable state. `fprintf(stderr)`
  is POSIX thread-safe but may interleave lines.
- `timestamp_now_us()` calls `clock_gettime()`, which is POSIX thread-safe.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `TcpBackend` owns `ImpairmentEngine m_impairment` by value. Constructed with `TcpBackend`,
  destroyed with it. No heap allocation.
- `ImpairmentEngine` owns `PrngEngine m_prng` and `ImpairmentConfig m_cfg` by value.
  No heap allocation.
- `m_cfg` is copied by value assignment in `ImpairmentEngine::init()` at line 51: `m_cfg = cfg`.
- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) is a member array of `TcpBackend`. Reused
  across calls; no allocation.
- The `MessageEnvelope` passed to `send_message()` and `process_outbound()` is accepted by
  `const&`. In the loss path no copy is ever made — the reference goes out of scope and the
  original is unmodified.
- `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` (32 `DelayEntry` slots, each containing a full
  `MessageEnvelope`) is a member array of `ImpairmentEngine`. In the loss path it is never
  written.
- Power of 10 rule 3 is fully observed: no `malloc`, `new`, or `delete` anywhere in this path.

---

## 9. Error Handling Flow

| Condition | Source Location | Propagation |
|---|---|---|
| `Serializer::serialize` returns non-OK | TcpBackend.cpp:356 | Logged WARNING_LO "Serialize failed"; returned to caller; impairment engine not reached |
| `process_outbound` returns `ERR_IO` (loss drop) | ImpairmentEngine.cpp:179 | Intercepted at TcpBackend.cpp:364; converted to `Result::OK` |
| `process_outbound` returns `ERR_IO` (partition drop) | ImpairmentEngine.cpp:172 | Same interception path; also converted to `Result::OK` |
| `process_outbound` returns `ERR_FULL` (delay buf full) | ImpairmentEngine.cpp:163 or 195 | Not intercepted by the ERR_IO branch; returned as-is to caller |
| `process_outbound` returns `OK` | (normal path) | Execution continues to `m_client_count` check, `send_to_all_clients()`, `flush_delayed_to_clients()` |
| `clock_gettime` fails | Timestamp.hpp:36 | `NEVER_COMPILED_OUT_ASSERT(result == 0)` fires; aborts process (always active in release) |

The conversion of `ERR_IO` to `OK` at TcpBackend.cpp:364-366 means the caller cannot
distinguish a packet-loss drop from a successful send. This is intentional by design.

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock read | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Event logging | `Logger::log()` | Writes formatted line to stderr via fprintf |
| Socket I/O | `send_to_all_clients()` / `tcp_send_frame()` | Not reached in the loss path |

In the loss path, no socket operations occur. The only external interactions are the monotonic
clock read and the Logger call.

---

## 11. State Changes / Side Effects

| State | Object | Change |
|---|---|---|
| `m_prng.m_state` | `PrngEngine` inside `ImpairmentEngine` | Advanced by one xorshift64 iteration (irreversible; deterministic given seed) |
| `m_delay_buf`, `m_delay_count` | `ImpairmentEngine` | Not modified in the loss path |
| `m_wire_buf` | `TcpBackend` | Written by serialization but never transmitted |
| Logger output | Process-level | WARNING_LO record emitted: "message dropped (loss probability)" |

Side-effect ordering:
1. Serialization writes m_wire_buf (wasted work).
2. PRNG state advances (one pseudorandom value consumed for the loss check in `check_loss()`).
3. Logger emits a WARNING_LO record.
4. No socket bytes are written.
5. No delay buffer entries are created.

---

## 12. Sequence Diagram (ASCII)

```
Caller         TcpBackend        Serializer   timestamp_now_us  ImpairmentEngine  PrngEngine  Logger
  |                 |                 |               |                 |              |           |
  |send_message(env)|                 |               |                 |              |           |
  |---------------->|                 |               |                 |              |           |
  |                 |serialize(env,..) |               |                 |              |           |
  |                 |---------------->|               |                 |              |           |
  |                 |<--OK, wire_len--|               |                 |              |           |
  |                 |                 |               |                 |              |           |
  |                 |   timestamp_now_us()             |                 |              |           |
  |                 |------------------------------->  |                 |              |           |
  |                 |<---------- now_us --------------|                 |              |           |
  |                 |                 |               |                 |              |           |
  |                 |  process_outbound(env, now_us)  |                 |              |           |
  |                 |-------------------------------------------------->|              |           |
  |                 |                 |               |    [enabled=true]              |           |
  |                 |                 |               |    [no partition]              |           |
  |                 |                 |               |    check_loss()                |           |
  |                 |                 |               |    [loss_prob > 0.0]           |           |
  |                 |                 |               |                 |next_double() |           |
  |                 |                 |               |                 |------------->|           |
  |                 |                 |               |                 |<-- 0.031 ----|           |
  |                 |                 |               |   [0.031 < 0.05: DROP]         |           |
  |                 |                 |               |  log(WARNING_LO "dropped")     |           |
  |                 |                 |               |                 |------------------------->|
  |                 |<----------------------------------------- ERR_IO-|              |           |
  |                 |[ERR_IO → return OK; no send_to_all_clients]      |              |           |
  |<---- OK --------|                 |               |                 |              |           |
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (called once at startup via `TcpBackend::init()` at
TcpBackend.cpp:88):

1. `m_recv_queue.init()` — zeros ring buffer state.
2. `impairment_config_default(imp_cfg)` (ImpairmentConfig.hpp:79) — sets:
   `enabled = false`, `loss_probability = 0.0`, `duplication_probability = 0.0`,
   `prng_seed = 42ULL`, and all other fields to 0/false.
3. `imp_cfg.enabled = config.channels[0U].impairments_enabled` — sets master switch only
   (TcpBackend.cpp:102). Other fields (loss_probability, etc.) remain at defaults.
4. `m_impairment.init(imp_cfg)` (ImpairmentEngine.cpp:44):
   - `NEVER_COMPILED_OUT_ASSERT(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)` [line 47]
   - `NEVER_COMPILED_OUT_ASSERT(cfg.loss_probability >= 0.0 && cfg.loss_probability <= 1.0)` [line 48]
   - `m_cfg = cfg` — full config copied by value.
   - `seed = (cfg.prng_seed != 0ULL) ? cfg.prng_seed : 42ULL` — default is 42.
   - `m_prng.seed(seed)` — sets `m_state`; 0 is coerced to 1 (xorshift64 requires non-zero state).
   - `memset(m_delay_buf, 0, sizeof(m_delay_buf))`.
   - `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))`.
   - `m_delay_count = 0`, `m_reorder_count = 0`.
   - `m_partition_active = false`, `m_partition_start_us = 0`, `m_next_partition_event_us = 0`.
   - `m_initialized = true`.

**Note**: `loss_probability` defaults to `0.0`. `TcpBackend::init()` only sets `enabled`
from `TransportConfig`; it does not expose `loss_probability`. [ASSUMPTION] A higher-level
layer must configure `loss_probability` before passing the config to `init()`.

**Runtime phase**: Each call to `send_message()` reads `m_cfg.loss_probability` (immutable
after `init()`). The only mutable impairment-engine state affected in the loss path is
`m_prng.m_state` (advanced once in `check_loss()`).

---

## 14. Known Risks / Observations

1. **`check_loss()` uses `loss_probability <= 0.0` as the no-loss guard**: The guard
   `if (m_cfg.loss_probability <= 0.0)` prevents any PRNG call when loss is disabled. This
   means the PRNG state is not advanced in the no-loss case, which affects the sequence for
   subsequent probabilistic checks (e.g., duplication) if they are enabled. PRNG call count
   varies by configuration.

2. **`TcpBackend::init()` does not expose the full `ImpairmentConfig`**: Only `enabled`
   is pulled from `TransportConfig`. Fields like `loss_probability`, `jitter_*`, and
   `partition_*` remain at defaults (0.0/false/42) unless a higher-level layer sets them
   before calling `init()`.

3. **`ERR_IO` conflation**: `process_outbound()` returns `ERR_IO` for both loss drops and
   partition drops. `TcpBackend::send_message()` converts both to `Result::OK`. The caller
   cannot distinguish the two cases, and there is no metric counter exposed at the
   `send_message` boundary.

4. **PRNG not thread-safe**: If `send_message()` is called concurrently from multiple threads
   on the same `TcpBackend` instance, `m_prng.m_state` is subject to a data race (undefined
   behavior under the C++17 memory model). No atomics protect it.

5. **Serialization occurs before the loss check**: CPU work and a full buffer write happen for
   every message, including those that will be dropped. Checking loss before serializing would
   save work, at the cost of leaving `m_wire_buf` stale on the loss path.

6. **`NEVER_COMPILED_OUT_ASSERT` in `check_loss()` is always active**: The assertion
   `NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0)` at line 119 fires in both
   debug and production builds. A PRNG implementation defect that produces an out-of-range
   value would abort the process in production.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `TransportInterface` is a pure virtual base class; `TcpBackend::send_message`
  is dispatched via vtable from callers holding a `TransportInterface*`.
- [ASSUMPTION] `Serializer::serialize()` is deterministic and does not allocate heap memory.
- [ASSUMPTION] `Logger::log()` is safe to call from within `process_outbound()` and does not
  block or re-enter `ImpairmentEngine`.
- [ASSUMPTION] `loss_probability` is configured to a non-zero value by a higher-level
  initialization layer not shown in these files; without it, the loss path is never entered
  at runtime.
- [ASSUMPTION] `impairment_config_default()` is always called before `ImpairmentEngine::init()`
  to establish safe baseline values. The default prng_seed is 42 (ImpairmentConfig.hpp:92).
- [UNKNOWN] The `Logger::log()` implementation's thread-safety and blocking characteristics
  (Logger.hpp not fully traced here).
- [UNKNOWN] Whether `Serializer::serialize` can fail for valid envelopes (e.g., oversized
  payload). Confirmed that it checks the payload_length constraint before writing.
- [UNKNOWN] `IMPAIR_DELAY_BUF_SIZE` is 32 (Types.hpp:28). Whether this is sufficient for all
  deployment scenarios under high message rates is a tuning question.

---
