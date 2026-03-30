## UC_15: ImpairmentEngine buffers an outbound message and releases it after a fixed latency delay

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()`. The `ImpairmentEngine::process_outbound()` computes a `release_us` timestamp equal to `now_us + (fixed_latency_ms * 1000)` and places the message into the internal `m_delay_buf` array rather than transmitting it immediately. The actual TCP transmission is deferred: on subsequent calls to `send_message()` (or the receive-side poll loop), `collect_deliverable()` is called with the current time, finds entries whose `release_us <= now_us`, copies them to an output buffer, and the caller transmits them. This produces an artificial but deterministic end-to-end latency equal to `fixed_latency_ms`.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp:249` | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp:62` | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `ImpairmentEngine::collect_deliverable()` | `src/platform/ImpairmentEngine.cpp:174` | `uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us, MessageEnvelope* out_buf, uint32_t buf_cap)` |
| `timestamp_now_us()` | `src/core/Timestamp.hpp:28` | `uint64_t timestamp_now_us()` |

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase 1 — Buffering (at send time):**

1. **`TcpBackend::send_message(envelope)`** (TcpBackend.cpp:249).
   - Preconditions: `assert(m_open)`, `assert(envelope_valid(envelope))`.

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)`. Returns OK.

3. **Get time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:264).
   - `clock_gettime(CLOCK_MONOTONIC, &ts)` — returns seconds and nanoseconds.
   - `now_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000`.

4. **`m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:265).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:62):
   a. Preconditions asserted.
   b. Master switch: `m_cfg.enabled == true`; continues.
   c. Partition check: `is_partition_active(now_us)` returns false; continues.
   d. Loss check: `m_cfg.loss_probability == 0.0` — guard fails; PRNG not called. Message survives.
   e. **Latency calculation** (lines 111–122):
      - `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL`.
        - Example: `fixed_latency_ms = 50` → `base_delay_us = 50000`.
      - `jitter_us = 0ULL` (because `m_cfg.jitter_mean_ms == 0` in the pure fixed-latency case).
      - `total_delay_us = base_delay_us + jitter_us = 50000`.
      - `release_us = now_us + 50000`.
   f. **Buffer capacity check** (line 125): `m_delay_count < IMPAIR_DELAY_BUF_SIZE`; passes.
   g. **Buffer insertion loop** (lines 132–141): iterate `i = 0..31`:
      - Find first slot where `m_delay_buf[i].active == false`.
      - `envelope_copy(m_delay_buf[i].env, in_env)` — `memcpy` of full `MessageEnvelope`.
      - `m_delay_buf[i].release_us = release_us` (= `now_us + 50000`).
      - `m_delay_buf[i].active = true`.
      - `++m_delay_count`.
      - `assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
      - Break.
   h. Duplication guard: `m_cfg.duplication_probability == 0.0`; skipped.
   i. Postcondition: `assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
   j. Returns `Result::OK`.

6. **Back in `send_message()`** (TcpBackend.cpp:266): `res == OK`; continues.

7. **First `collect_deliverable()` call** (TcpBackend.cpp:273–275):
   - `m_impairment.collect_deliverable(now_us, delayed_envelopes, IMPAIR_DELAY_BUF_SIZE)`.
   - Scans `m_delay_buf[0..31]`.
   - For slot just inserted: `m_delay_buf[i].release_us = now_us + 50000 > now_us` — condition `release_us <= now_us` is false. Entry not collected.
   - Returns `delayed_count = 0`.

8. **No delayed messages to send yet**; `send_message()` returns `Result::OK` without transmitting anything for the deferred envelope.

**Phase 2 — Release (at or after `now_us + fixed_latency_ms * 1000`):**

9. A subsequent call to `send_message()` (or the receive-side poll loop in `receive_message()`) obtains a fresh `now_us' = timestamp_now_us()` where `now_us' >= now_us + 50000`.

10. **`collect_deliverable(now_us', delayed_envelopes, 32)`** (ImpairmentEngine.cpp:174):
    - Preconditions: `assert(m_initialized)`, `assert(out_buf != NULL)`, `assert(buf_cap > 0)`.
    - Loop `i = 0..31` (fixed bound, Power of 10 rule 2):
      - `if (m_delay_buf[i].active && m_delay_buf[i].release_us <= now_us')` — true for the buffered entry.
      - `envelope_copy(out_buf[out_count], m_delay_buf[i].env)` — copies the stored envelope to the output buffer.
      - `++out_count`.
      - `m_delay_buf[i].active = false` — slot reclaimed.
      - `assert(m_delay_count > 0)`.
      - `--m_delay_count`.
    - Postconditions: `assert(out_count <= buf_cap)`, `assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
    - Returns `out_count` (>= 1).

11. **Delayed message transmission loop** (TcpBackend.cpp:298–315):
    - For each `i < delayed_count`:
      - `Serializer::serialize(delayed_envelopes[i], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)`.
      - For each active client fd: `tcp_send_frame(m_client_fds[j], m_wire_buf, delayed_len, send_timeout_ms)`.
    - The deferred message is now transmitted to all connected TCP clients.

---

## 4. Call Tree (Hierarchical)

```
[Phase 1: send_message at T=0]
TcpBackend::send_message(envelope)
├── Serializer::serialize(envelope, m_wire_buf, ...)
├── timestamp_now_us()                                [T=0: e.g. 1000000000 µs]
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us=T0)
│   ├── [partition=false, loss_prob=0.0]
│   ├── base_delay_us = fixed_latency_ms * 1000       [e.g. 50 * 1000 = 50000]
│   ├── jitter_us = 0                                 [jitter_mean_ms == 0]
│   ├── release_us = T0 + 50000
│   ├── [m_delay_count < 32: find inactive slot i]
│   │   └── envelope_copy(m_delay_buf[i].env, in_env) [memcpy ~4119 bytes]
│   │       m_delay_buf[i].release_us = T0 + 50000
│   │       m_delay_buf[i].active = true
│   │       ++m_delay_count
│   └── return OK
├── ImpairmentEngine::collect_deliverable(T0, delayed_envelopes, 32)
│   ├── [scan m_delay_buf[0..31]]
│   │   └── slot i: active=true, release_us=T0+50000 > T0 → NOT collected
│   └── return 0
└── [delayed_count == 0: no deferred transmission; return OK]

[Phase 2: collect_deliverable at T=T0+50000+ε]
TcpBackend::send_message(envelope2)   [or receive_message poll]
├── ...
├── ImpairmentEngine::collect_deliverable(T1, delayed_envelopes, 32)
│   ├── assert(m_initialized), assert(out_buf != NULL), assert(buf_cap > 0)
│   ├── [scan m_delay_buf[0..31]]
│   │   └── slot i: active=true, release_us=T0+50000 <= T1 → COLLECT
│   │       envelope_copy(out_buf[0], m_delay_buf[i].env)  [memcpy ~4119 bytes]
│   │       m_delay_buf[i].active = false
│   │       --m_delay_count
│   └── return 1
└── [delayed_count == 1]
    ├── Serializer::serialize(delayed_envelopes[0], m_wire_buf, ...)
    └── [for each client j]
        └── tcp_send_frame(m_client_fds[j], m_wire_buf, delayed_len, timeout)
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Owns engine; calls `process_outbound` and `collect_deliverable` each send cycle |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Computes `release_us`; manages `m_delay_buf` |
| `ImpairmentConfig::fixed_latency_ms` | `uint32_t` field | `src/platform/ImpairmentConfig.hpp:36` | Configures the fixed delay in milliseconds |
| `DelayEntry` | Nested struct | `ImpairmentEngine.hpp:125` | `{env, release_us, active}` — holds one buffered message |
| `m_delay_buf[32]` | Member array | `ImpairmentEngine.hpp:136` | Fixed-size array of delay entries; 32 slots |
| `m_delay_count` | `uint32_t` | `ImpairmentEngine.hpp:137` | Count of currently active (occupied) slots |
| `timestamp_now_us()` | Inline function | `src/core/Timestamp.hpp:28` | Called at send time and at collection time |
| `envelope_copy()` | Inline function | `src/core/MessageEnvelope.hpp:49` | Full `memcpy` of envelope into delay buffer slot |
| `collect_deliverable()` | Method | `ImpairmentEngine.cpp:174` | Scans for matured entries; copies to caller's buffer |
| `tcp_send_frame()` | Function | `src/platform/SocketUtils.hpp` | Performs the actual deferred socket write |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Master switch disabled | ImpairmentEngine.cpp:70 | `!m_cfg.enabled` | `release_us = now_us` (immediate delivery, zero delay) |
| Partition active | ImpairmentEngine.cpp:93 | `is_partition_active(now_us)` | If true: ERR_IO; message never buffered |
| Fixed latency zero | ImpairmentEngine.cpp:111 | `m_cfg.fixed_latency_ms == 0` | `base_delay_us = 0`; message released immediately if jitter also 0 |
| Jitter enabled | ImpairmentEngine.cpp:114 | `m_cfg.jitter_mean_ms > 0` | If true: additional random delay added to `release_us` |
| Delay buffer full | ImpairmentEngine.cpp:125 | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | ERR_FULL; message dropped; no delay entry created |
| Entry matured | ImpairmentEngine.cpp:187 | `m_delay_buf[i].active && release_us <= now_us` | If true: entry collected and slot reclaimed |
| Output buffer full | ImpairmentEngine.cpp:186 | `out_count >= buf_cap` | Loop exits; remaining matured entries stay until next call |

**Zero-latency path**: When `m_cfg.enabled` is false (or both `fixed_latency_ms` and `jitter_mean_ms` are 0), `release_us = now_us`, and `collect_deliverable()` immediately returns the message. This is the pass-through path.

---

## 7. Concurrency / Threading Behavior

- No locking protects `m_delay_buf` or `m_delay_count`.
- `process_outbound()` writes to `m_delay_buf` and increments `m_delay_count`.
- `collect_deliverable()` reads and writes `m_delay_buf` and decrements `m_delay_count`.
- Both are called from `send_message()` in sequence (same thread, same call), and from `receive_message()`'s poll loop (TcpBackend.cpp:363–369). If `send_message()` and `receive_message()` are called from different threads on the same `TcpBackend` instance, concurrent access to `m_delay_buf` is a data race.
- [ASSUMPTION] Single-threaded use per `TcpBackend` instance.
- `timestamp_now_us()` → `clock_gettime()` is POSIX thread-safe.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- Each `DelayEntry` in `m_delay_buf` contains a full `MessageEnvelope` copy (approximately 4119 bytes including `payload[4096]`). The 32-slot `m_delay_buf` array therefore occupies approximately 131 KB of `ImpairmentEngine`'s in-object storage, and thus of `TcpBackend`'s storage.
- **Ownership transfer**: When `process_outbound()` calls `envelope_copy()`, the envelope content is copied into the delay buffer. The caller's original envelope reference is not retained; the copy in `m_delay_buf[i].env` is the live object until `collect_deliverable()` copies it out.
- **Second copy**: `collect_deliverable()` calls `envelope_copy(out_buf[out_count], m_delay_buf[i].env)`, copying from the delay buffer slot to the caller-provided `delayed_envelopes[]` stack array. The delay buffer slot is then immediately cleared (`active = false`).
- **Total copies in path**: original envelope → `m_delay_buf[i].env` (in `process_outbound`) → `delayed_envelopes[n]` (in `collect_deliverable`) → `m_wire_buf` via `Serializer::serialize` (in transmission loop). Three distinct copies of the envelope data are made before transmission.
- No heap allocation anywhere. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| Serialization error (initial) | TcpBackend.cpp:258 | Return error; engine not called |
| Partition active | ImpairmentEngine.cpp:96 | ERR_IO; no delay buffer entry created |
| Loss fires | ImpairmentEngine.cpp:105 | ERR_IO; no delay buffer entry created |
| Delay buffer full | ImpairmentEngine.cpp:128 | WARNING_HI logged; ERR_FULL returned to `send_message()`; [ASSUMPTION] propagated to caller |
| Serialization error (delayed) | TcpBackend.cpp:304 | `continue` — delayed message silently skipped; no retry; permanently lost |
| `tcp_send_frame()` fails | TcpBackend.cpp:311 | Return value ignored via `(void)` cast; failure is silent at this level |

**Delayed message serialization failure at TcpBackend.cpp:304**: The `continue` statement skips transmission of that delayed entry but does not restore it to the delay buffer. The message is permanently lost without any error propagated upward.

---

## 10. External Interactions

| Interaction | Function | API | Timing |
|---|---|---|---|
| Clock read (send time) | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC)` | At `send_message()` entry |
| Clock read (release time) | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC)` | At subsequent `send_message()` or `receive_message()` poll |
| Socket write | `tcp_send_frame()` | POSIX write on TCP fd | At release time, not send time |
| Logger | `Logger::log()` | Internal | Only if delay buffer full (WARNING_HI) |

The wall-clock gap between the two `timestamp_now_us()` calls determines the actual observed delay. OS scheduling, system load, and `clock_gettime` resolution all affect whether the configured `fixed_latency_ms` is precisely honored.

---

## 11. State Changes / Side Effects

| State | Object | Phase | Change |
|---|---|---|---|
| `m_delay_buf[i].env` | `ImpairmentEngine` | Send | Filled with `memcpy` of `in_env` |
| `m_delay_buf[i].release_us` | `ImpairmentEngine` | Send | Set to `now_us + fixed_latency_ms * 1000` |
| `m_delay_buf[i].active` | `ImpairmentEngine` | Send | Set to `true` |
| `m_delay_count` | `ImpairmentEngine` | Send | Incremented by 1 |
| `m_delay_buf[i].active` | `ImpairmentEngine` | Release | Set to `false` |
| `m_delay_count` | `ImpairmentEngine` | Release | Decremented by 1 |
| `out_buf[n]` (= `delayed_envelopes`) | `TcpBackend` local | Release | Filled with `memcpy` of buffered envelope |
| `m_wire_buf` | `TcpBackend` | Transmission | Overwritten by `Serializer::serialize` of delayed envelope |

---

## 12. Sequence Diagram (ASCII)

```
Caller       TcpBackend        ImpairmentEngine   clock_gettime   tcp_send_frame
  |               |                   |                  |               |
  |send_msg(env)  |                   |                  |               |
  |-------------->|                   |                  |               |
  |               | timestamp_now_us()|                  |               |
  |               |---------------------------------->   |               |
  |               |<---- T0 (e.g. 1000050000 µs) -------|               |
  |               |                   |                  |               |
  |               |process_outbound(env, T0)             |               |
  |               |------------------>|                  |               |
  |               |                   | base_delay = fixed_latency_ms*1000
  |               |                   | release_us = T0 + 50000         |
  |               |                   | m_delay_buf[0] = {env,T0+50000,true}
  |               |                   | m_delay_count = 1               |
  |               |<------ OK --------|                  |               |
  |               |                   |                  |               |
  |               |collect_deliverable(T0, buf, 32)      |               |
  |               |------------------>|                  |               |
  |               |  slot0: T0+50000 > T0 → not ready    |               |
  |               |<-- delayed_count=0|                  |               |
  |<--- OK -------|                   |                  |               |
  :               :                   :                  :               :
  [50 ms elapses]
  :               :                   :                  :               :
  |send_msg(env2) |                   |                  |               |
  |-------------->|                   |                  |               |
  |               | timestamp_now_us()|                  |               |
  |               |---------------------------------->   |               |
  |               |<---- T1 = T0+50001µs (or later) ----|               |
  |               |                   |                  |               |
  |               |process_outbound(env2, T1)            |               |
  |               |------------------>|                  |               |
  |               |  [buffers env2 at T1+50000]          |               |
  |               |<------ OK --------|                  |               |
  |               |                   |                  |               |
  |               |collect_deliverable(T1, buf, 32)      |               |
  |               |------------------>|                  |               |
  |               |  slot0: T0+50000 <= T1 → COLLECT    |               |
  |               |  envelope_copy(buf[0], slot0.env)    |               |
  |               |  slot0.active = false                |               |
  |               |  m_delay_count--                     |               |
  |               |<-- delayed_count=1|                  |               |
  |               |                   |                  |               |
  |               | serialize(buf[0]) + tcp_send_frame() |               |
  |               |---------------------------------------------->       |
  |<--- OK -------|                   |                  |               |
```

---

## 13. Initialization vs Runtime Flow

**Initialization**: `fixed_latency_ms` defaults to `0` in `impairment_config_default()`. When `0`, `base_delay_us = 0`, and unless jitter is added, `release_us = now_us`. The first `collect_deliverable()` call immediately returns the message (zero-delay passthrough).

For a meaningful fixed latency, `fixed_latency_ms` must be set to a non-zero value before `ImpairmentEngine::init()` is called. `TcpBackend::init()` currently only configures `enabled`; the full `ImpairmentConfig` must be populated at a higher layer.

**Runtime**: The delay buffer acts as a time-ordered queue, though it is not sorted — `collect_deliverable()` performs a linear scan of all 32 slots each call. With 32 slots and high message rates, multiple messages may be buffered simultaneously. All messages with `release_us <= now_us` are collected in a single scan. The buffer provides no ordering guarantee: messages inserted at different times may be scanned and collected in array-index order, not release-time order.

---

## 14. Known Risks / Observations

1. **Delay buffer is not sorted by `release_us`**: Messages with earlier `release_us` values are not guaranteed to be collected before those with later values unless they occupy lower array indices. If two messages have the same `fixed_latency_ms`, they will typically be collected in insertion order (lower index first) because the scan is linear from index 0.

2. **Buffer capacity limits maximum in-flight delayed messages**: With 32 slots and `fixed_latency_ms = 50 ms`, the maximum sustainable throughput before buffer saturation is 32 messages / 50 ms = 640 msg/s. At higher rates, `ERR_FULL` is returned.

3. **`collect_deliverable()` is called with the same `now_us` as `process_outbound()`**: At TcpBackend.cpp:264 and 273, both calls use the same `now_us` value. This means a message buffered in the current `process_outbound()` call is guaranteed not to be immediately collected — `release_us = now_us + delay > now_us`. This is correct.

4. **Delayed message serialization failure is silent**: If `Serializer::serialize` fails for a delayed envelope (TcpBackend.cpp:304), the `continue` skips that entry. The message is gone with no error propagated to the caller.

5. **Large stack allocation in `send_message()`**: `delayed_envelopes[32]` on the stack consumes approximately 131 KB per call frame.

6. **Polling frequency determines effective precision**: The delay is only enforced at poll points (each `send_message()` call or `receive_message()` iteration). If `send_message()` is called infrequently, messages may be held longer than `fixed_latency_ms`.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `fixed_latency_ms` is configured non-zero at a higher layer before `init()`.
- [ASSUMPTION] `collect_deliverable()` is called frequently enough (relative to `fixed_latency_ms`) to achieve the intended delay granularity.
- [ASSUMPTION] CLOCK_MONOTONIC on the target platform has sub-millisecond resolution. On Linux this is typically 1 µs; on some embedded platforms it may be coarser.
- [ASSUMPTION] The 32-slot delay buffer is sufficient for the message rate and configured latency of the deployment.
- [ASSUMPTION] Single-threaded access per `TcpBackend` instance.
- [UNKNOWN] Whether `tcp_send_frame()` can block (non-blocking vs blocking socket mode) and for how long, which affects the timing of deferred transmissions.
- [UNKNOWN] Whether the receive-side poll loop in `receive_message()` (TcpBackend.cpp:361–369) provides sufficient collection frequency for the configured `fixed_latency_ms` values.agentId: a83d00e31f3c8a545 (for resuming to continue this agent's work if needed)
<usage>total_tokens: 110404
tool_uses: 0
duration_ms: 316007</usage>