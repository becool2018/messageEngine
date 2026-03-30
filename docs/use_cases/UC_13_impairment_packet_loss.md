## UC_13: ImpairmentEngine drops an outbound message based on configured loss probability

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()` to transmit a `MessageEnvelope` over TCP. Before any bytes are written to the socket, the `ImpairmentEngine` intercepts the message via `process_outbound()`. The PRNG generates a random double in [0.0, 1.0). If that value is less than `ImpairmentConfig::loss_probability`, the message is silently discarded and no bytes are transmitted. The caller receives `Result::OK` — the drop is transparent at the API boundary because loss is a simulated network event, not an application error.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp:249` | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp:62` | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |

Callers above `TcpBackend` invoke through the `TransportInterface` virtual dispatch table (declared in `src/core/TransportInterface.hpp`). `send_message` is declared `virtual` in `TcpBackend` and is resolved at runtime through the vtable.

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Caller invokes `TcpBackend::send_message(envelope)`** (TcpBackend.cpp:249).
   - Precondition assertions fire: `assert(m_open)` and `assert(envelope_valid(envelope))`.

2. **Serialize to wire buffer**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` (TcpBackend.cpp:256).
   - Writes serialized bytes into the `m_wire_buf[8192]` member array.
   - Returns `Result::OK` or an error code; the return value is checked immediately.

3. **Obtain current time**: `timestamp_now_us()` (Timestamp.hpp:28).
   - Calls `clock_gettime(CLOCK_MONOTONIC, &ts)`.
   - Returns `uint64_t` microseconds since the monotonic epoch.
   - Stored in local variable `now_us`.

4. **Call `m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:265).
   - `m_impairment` is an `ImpairmentEngine` instance embedded by value in `TcpBackend` (no pointer indirection).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:62):
   a. Preconditions asserted: `assert(m_initialized)` and `assert(envelope_valid(in_env))`.
   b. **Master switch check**: `if (!m_cfg.enabled)` (line 70) — if impairments are globally disabled, the message is buffered at `release_us = now_us` (zero delay) and returns `Result::OK`. The loss path is not taken.
   c. **Partition check**: `is_partition_active(now_us)` is called (line 93). If a partition is active, the message is dropped with `Result::ERR_IO`. This is a distinct drop path; the PRNG loss check is not reached.
   d. **Loss check** (lines 100–108):
      - Guard: `if (m_cfg.loss_probability > 0.0)` — if zero, the PRNG is not called; no loss is possible.
      - `loss_rand = m_prng.next_double()` — advances PRNG state via one xorshift64 iteration; returns a double in [0.0, 1.0).
      - `assert(loss_rand >= 0.0 && loss_rand < 1.0)` — postcondition verified at the call site.
      - `if (loss_rand < m_cfg.loss_probability)` — if true, message is dropped:
        - `Logger::log(Severity::WARNING_LO, "ImpairmentEngine", "message dropped (loss probability)")`.
        - `return Result::ERR_IO`.

6. **Back in `send_message()`** (TcpBackend.cpp:266–269):
   - `if (res == Result::ERR_IO)` evaluates true.
   - `return Result::OK` — the drop is converted to a success return. No bytes are written to any socket.

7. **Execution terminates**. The message was consumed and discarded. Serialized bytes in `m_wire_buf` are never transmitted.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)
├── assert(m_open)
├── assert(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)
├── timestamp_now_us()
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us)      [m_impairment value member]
│   ├── assert(m_initialized)
│   ├── assert(envelope_valid(in_env))
│   ├── [if !m_cfg.enabled] → buffer at now_us, return OK     [NOT taken in loss path]
│   ├── ImpairmentEngine::is_partition_active(now_us)         [skipped unless partition active]
│   ├── [if loss_probability > 0.0]
│   │   ├── PrngEngine::next_double()                         [m_prng value member]
│   │   │   ├── assert(m_state != 0ULL)
│   │   │   ├── PrngEngine::next()
│   │   │   │   ├── m_state ^= m_state << 13U
│   │   │   │   ├── m_state ^= m_state >> 7U
│   │   │   │   └── m_state ^= m_state << 17U
│   │   │   ├── result = static_cast<double>(raw) / static_cast<double>(UINT64_MAX)
│   │   │   └── assert(result >= 0.0 && result < 1.0)
│   │   └── [if loss_rand < loss_probability]
│   │       ├── Logger::log(WARNING_LO, "message dropped (loss probability)")
│   │       └── return Result::ERR_IO                         ← LOSS DROP POINT
│   └── [not reached: latency/dup/buffer logic]
└── [if res == ERR_IO] return Result::OK                      ← DROP CONVERTED TO OK; NO SOCKET WRITE
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Transport layer; owns `ImpairmentEngine` by value |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Applies loss decision; embedded as `m_impairment` |
| `ImpairmentConfig` | POD struct | `src/platform/ImpairmentConfig.hpp` | Holds `loss_probability` (double), `enabled` (bool) |
| `PrngEngine` | Class | `src/platform/PrngEngine.hpp` | xorshift64 PRNG; embedded in `ImpairmentEngine` as `m_prng` |
| `Serializer::serialize()` | Static function | `src/core/Serializer.hpp` | Converts envelope to wire bytes before impairment check |
| `timestamp_now_us()` | Inline function | `src/core/Timestamp.hpp:28` | CLOCK_MONOTONIC microsecond clock |
| `Logger::log()` | Static function | `src/core/Logger.hpp` | Records WARNING_LO event on drop |
| `envelope_valid()` | Inline function | `src/core/MessageEnvelope.hpp:55` | Validates message_type, payload_length, source_id |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Impairments master switch | ImpairmentEngine.cpp:70 | `!m_cfg.enabled` | If true: zero-delay buffer path, returns OK without loss check |
| Partition active check | ImpairmentEngine.cpp:93 | `is_partition_active(now_us)` | If true: drop with ERR_IO before loss check is reached |
| Loss probability guard | ImpairmentEngine.cpp:100 | `m_cfg.loss_probability > 0.0` | If false: PRNG not called; no loss possible |
| Loss decision | ImpairmentEngine.cpp:103 | `loss_rand < m_cfg.loss_probability` | If true: Logger call + return ERR_IO (message dropped) |
| ERR_IO interception | TcpBackend.cpp:266 | `res == Result::ERR_IO` | If true: return OK to caller (drop is transparent) |
| Serialization failure | TcpBackend.cpp:258 | `!result_ok(res)` | If true: returns error before impairment engine is reached |

**Ordering note**: Partition is checked before loss. If both are configured and a partition is active, the partition drop fires first and the PRNG is never consulted.

---

## 7. Concurrency / Threading Behavior

- `TcpBackend::send_message()` and `ImpairmentEngine::process_outbound()` contain no mutex, spinlock, or atomic operations anywhere in this call path.
- `PrngEngine::m_state` is a plain `uint64_t` (not `std::atomic`). It is not thread-safe.
- [ASSUMPTION] The design assumes `send_message()` is called from a single thread at a time per `TcpBackend` instance. No concurrent access protection is present.
- `Logger::log()` concurrency safety depends on the Logger implementation (not visible in these files).
- `timestamp_now_us()` calls `clock_gettime()`, which is POSIX thread-safe.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `TcpBackend` owns `ImpairmentEngine m_impairment` by value. Constructed with `TcpBackend`, destroyed with it. No heap allocation.
- `ImpairmentEngine` owns `PrngEngine m_prng` and `ImpairmentConfig m_cfg` by value. No heap allocation.
- `m_cfg` is copied by value assignment in `ImpairmentEngine::init()` (line 30: `m_cfg = cfg`).
- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes) is a member array of `TcpBackend`. Reused across calls; no allocation.
- The `MessageEnvelope` passed to `send_message()` and `process_outbound()` is accepted by `const&`. In the loss path no copy is ever made — the reference goes out of scope and the original is unmodified.
- `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` (32 `DelayEntry` slots, each containing a full `MessageEnvelope`) is a member array of `ImpairmentEngine`. In the loss path it is never written.
- Power of 10 rule 3 is fully observed: no `malloc`, `new`, or `delete` anywhere in this path.

---

## 9. Error Handling Flow

| Condition | Source Location | Propagation |
|---|---|---|
| `Serializer::serialize` returns non-OK | TcpBackend.cpp:258 | Immediately returned to caller; impairment engine not reached |
| `process_outbound` returns `ERR_IO` (loss drop) | ImpairmentEngine.cpp:105 | Intercepted at TcpBackend.cpp:266; converted to `Result::OK` |
| `process_outbound` returns `ERR_IO` (partition drop) | ImpairmentEngine.cpp:96 | Same interception path; also converted to `Result::OK` |
| `process_outbound` returns `ERR_FULL` | ImpairmentEngine.cpp:128 | Not intercepted by the ERR_IO branch; returned as-is to caller |
| `process_outbound` returns `OK` | ImpairmentEngine.cpp:167 | Execution continues to socket send path |
| `clock_gettime` fails | Timestamp.hpp:33 | `assert(result == 0)` fires; aborts in debug builds; undefined behavior in release |

The conversion of `ERR_IO` to `OK` at TcpBackend.cpp:266–269 means the caller cannot distinguish a packet-loss drop from a successful send. This is intentional by design.

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock read | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Event logging | `Logger::log()` | [ASSUMPTION] Writes to stderr or internal log buffer |
| Socket I/O | `tcp_send_frame()` | Not reached in the loss path |

In the loss path, no socket operations occur. The only external interactions are the monotonic clock read and the logger call.

---

## 11. State Changes / Side Effects

| State | Object | Change |
|---|---|---|
| `m_prng.m_state` | `PrngEngine` inside `ImpairmentEngine` | Advanced by one xorshift64 iteration (irreversible; deterministic given seed) |
| `m_delay_buf`, `m_delay_count` | `ImpairmentEngine` | Not modified in the loss path |
| `m_wire_buf` | `TcpBackend` | Written by serialization but never transmitted |
| Logger output | Process-level | WARNING_LO record emitted: "message dropped (loss probability)" |

Side-effect ordering:
1. PRNG state advances (one pseudorandom value consumed for the loss check).
2. Logger emits a WARNING_LO record.
3. No socket bytes are written.
4. No delay buffer entries are created.

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
  |                 |                 |               |    [loss_prob > 0.0]           |           |
  |                 |                 |               |                 |next_double() |           |
  |                 |                 |               |                 |------------->|           |
  |                 |                 |               |                 |<-- 0.031 ----|           |
  |                 |                 |               |   [0.031 < 0.05: DROP]         |           |
  |                 |                 |               |                 |log(WARN_LO)              |
  |                 |                 |               |                 |------------------------->|
  |                 |<----------------------------------------- ERR_IO-|              |           |
  |                 |[ERR_IO → return OK; no tcp_send_frame]           |              |           |
  |<---- OK --------|                 |               |                 |              |           |
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (called once at startup via `TcpBackend::init()`):

1. `m_recv_queue.init()` — zeros ring buffer state.
2. `impairment_config_default(imp_cfg)` — sets all fields to zero/false; `loss_probability = 0.0`.
3. `imp_cfg.enabled = config.channels[0U].impairments_enabled` — sets master switch only.
4. `m_impairment.init(imp_cfg)` (ImpairmentEngine.cpp:23):
   - Asserts `cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE` and `loss_probability` in [0.0, 1.0].
   - `m_cfg = cfg` — full config copied by value assignment.
   - `seed = (cfg.prng_seed != 0) ? cfg.prng_seed : 42ULL`.
   - `m_prng.seed(seed)` — sets `m_state`; 0 is coerced to 1 (xorshift64 requires non-zero state).
   - `memset(m_delay_buf, 0, sizeof(m_delay_buf))` — zeros all 32 `DelayEntry` slots.
   - `m_delay_count = 0`.
   - `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))` — zeros reorder buffer.
   - `m_reorder_count = 0`.
   - `m_partition_active = false`, `m_partition_start_us = 0`, `m_next_partition_event_us = 0`.
   - `m_initialized = true`.

**Note**: `loss_probability` defaults to `0.0`. `TcpBackend::init()` only sets `enabled` from `TransportConfig`; it does not expose `loss_probability`. [ASSUMPTION] A higher-level layer must configure `loss_probability` before passing the config to `init()`.

**Runtime phase**: Each call to `send_message()` reads `m_cfg.loss_probability` (immutable after `init()`). The only mutable impairment-engine state affected in the loss path is `m_prng.m_state`.

---

## 14. Known Risks / Observations

1. **Loss probability of exactly 0.0 bypasses the PRNG entirely**: The guard `if (m_cfg.loss_probability > 0.0)` prevents any PRNG call. This means the PRNG state is not advanced in the no-loss case, which affects the sequence for subsequent probabilistic checks (e.g., duplication) if they are enabled.

2. **`TcpBackend::init()` does not expose the full `ImpairmentConfig`**: Only `enabled` is pulled from `TransportConfig`. Fields like `loss_probability`, `jitter_*`, and `partition_*` remain at defaults (0.0/false) unless a higher-level layer sets them before calling `init()`.

3. **`ERR_IO` conflation**: `process_outbound` returns `ERR_IO` for both loss drops and partition drops. `TcpBackend::send_message()` converts both to `Result::OK`. The caller cannot distinguish the two cases, and there is no metric counter exposed at the `send_message` boundary.

4. **PRNG not thread-safe**: If `send_message()` is called concurrently from multiple threads on the same `TcpBackend` instance, `m_prng.m_state` is subject to a data race (undefined behavior under the C++17 memory model).

5. **Serialization occurs before the loss check**: CPU work and a full buffer write happen for every message, including those that will be dropped. Checking loss before serializing would save work, at the cost of leaving `m_wire_buf` stale on the loss path.

6. **Defense-in-depth assertion at ImpairmentEngine.cpp:102**: `assert(loss_rand >= 0.0 && loss_rand < 1.0)` is technically redundant given the identical postcondition inside `next_double()`, but provides a useful check at the point of use.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `TransportInterface` is a pure virtual base class; `TcpBackend::send_message` is dispatched via vtable from callers holding a `TransportInterface*`.
- [ASSUMPTION] `Serializer::serialize()` is deterministic and does not allocate heap memory.
- [ASSUMPTION] `Logger::log()` is safe to call from within `process_outbound()` and does not block or re-enter `ImpairmentEngine`.
- [ASSUMPTION] The build sets `NDEBUG` in production, disabling all `assert()` calls; in debug builds, assertion failures abort the process.
- [ASSUMPTION] `loss_probability` is configured to a non-zero value by a higher-level initialization layer not shown in these files; without it, the loss path is never entered at runtime.
- [UNKNOWN] The `Logger::log()` implementation is not present in the traced files. Its thread-safety and blocking characteristics are unknown.
- [UNKNOWN] Whether `Serializer::serialize` can fail for valid envelopes (e.g., oversized payload) is not traced here.
- [UNKNOWN] `IMPAIR_DELAY_BUF_SIZE` is 32 (Types.hpp:26). Whether this is sufficient for all deployment scenarios is a tuning question not addressed in the source.

---