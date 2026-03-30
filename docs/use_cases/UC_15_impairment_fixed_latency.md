# UC_15 — Impairment: fixed latency

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.1, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()`. Internally, `ImpairmentEngine::process_outbound()` computes a `release_us` timestamp equal to `now_us + (fixed_latency_ms * 1000)` and places the message into the internal `m_delay_buf` array (via the private helper `queue_to_delay_buf()`) rather than transmitting it immediately. On the same call, `flush_delayed_to_clients()` is called, which calls `collect_deliverable()` with the same `now_us`. Because `release_us = now_us + delay > now_us`, the newly buffered entry is not yet collected. On a subsequent `send_message()` call (or `receive_message()` poll loop), when `now_us' >= release_us`, `collect_deliverable()` finds the matured entry, copies it out, and the caller serializes and transmits it. This produces an artificial but deterministic end-to-end latency equal to `fixed_latency_ms` milliseconds.

---

## 2. Entry Points

| Entry Point | File | Line | Signature |
|---|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp` | 347 | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp` | 151 | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `ImpairmentEngine::queue_to_delay_buf()` | `src/platform/ImpairmentEngine.cpp` | 83 | `Result ImpairmentEngine::queue_to_delay_buf(const MessageEnvelope& env, uint64_t release_us)` |
| `ImpairmentEngine::collect_deliverable()` | `src/platform/ImpairmentEngine.cpp` | 216 | `uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us, MessageEnvelope* out_buf, uint32_t buf_cap)` |
| `TcpBackend::flush_delayed_to_clients()` | `src/platform/TcpBackend.cpp` | 280 | `void TcpBackend::flush_delayed_to_clients(uint64_t now_us)` |
| `timestamp_now_us()` | `src/core/Timestamp.hpp` | 31 | `inline uint64_t timestamp_now_us()` |

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase 1 — Buffering (at send time):**

1. **`TcpBackend::send_message(envelope)`** (TcpBackend.cpp:347).
   - Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` (lines 349–350).

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` (line 354). Returns OK or returns the error early.

3. **Get time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:362).
   - `clock_gettime(CLOCK_MONOTONIC, &ts)` — returns seconds and nanoseconds.
   - `now_us = ts.tv_sec * 1000000 + ts.tv_nsec / 1000` (Timestamp.hpp:41–44).

4. **`m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:363).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:151):
   a. Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))` (lines 155–156).
   b. **`!m_cfg.enabled`** check (line 159): if impairments are disabled, calls `queue_to_delay_buf(in_env, now_us)` immediately with `release_us = now_us` (zero delay, immediate delivery).
   c. (For this UC, `m_cfg.enabled == true`.) **Partition check**: `is_partition_active(now_us)` (line 169) — returns false; continues.
   d. **Loss check**: `check_loss()` (line 176) — returns false (`loss_probability == 0.0`); continues.
   e. **Latency calculation** (lines 183–188):
      - `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL`.
        - Example: `fixed_latency_ms = 50` → `base_delay_us = 50000`.
      - `jitter_us = 0ULL` (because `m_cfg.jitter_mean_ms == 0`).
      - `release_us = now_us + 50000 + 0 = now_us + 50000` (line 189).
   f. **Buffer capacity check** (line 192): `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` — false; continues.
   g. **`queue_to_delay_buf(in_env, release_us)`** called (line 197).

6. **Inside `queue_to_delay_buf()`** (ImpairmentEngine.cpp:83):
   - Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE)` (lines 86–87).
   - Loop `i = 0..IMPAIR_DELAY_BUF_SIZE-1` (line 90):
     - Find first slot where `!m_delay_buf[i].active`.
     - `envelope_copy(m_delay_buf[i].env, env)` — `memcpy` of full `MessageEnvelope` (MessageEnvelope.hpp:58).
     - `m_delay_buf[i].release_us = release_us` (= `now_us + 50000`).
     - `m_delay_buf[i].active = true`.
     - `++m_delay_count`.
     - `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)` (line 96).
     - Returns `Result::OK` (line 97).
   - If no free slot found (logic error given the caller's check): `NEVER_COMPILED_OUT_ASSERT(false)` fires (line 102); returns `ERR_FULL`.

7. **Back in `process_outbound()`** (line 197): `res == OK`; continues.

8. **Duplication check** (line 203): `m_cfg.duplication_probability > 0.0` — false; `apply_duplication()` not called.

9. **Postcondition** (line 208): `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`. Returns `Result::OK`.

10. **Back in `send_message()`** (line 364): `res == ERR_IO` is false; continues.

11. **No-clients check** (line 369): `m_client_count > 0`; continues.

12. **`send_to_all_clients(m_wire_buf, wire_len)`** (TcpBackend.cpp:375): transmits the serialized main envelope immediately to all connected clients.

13. **`flush_delayed_to_clients(now_us)`** (TcpBackend.cpp:376).

14. **Inside `flush_delayed_to_clients()`** (ImpairmentEngine.cpp:280):
    - Preconditions: `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`, `NEVER_COMPILED_OUT_ASSERT(m_open)` (lines 282–283).
    - **`m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`** (line 286).
    - **Inside `collect_deliverable()`** (ImpairmentEngine.cpp:216):
      - Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)` (lines 221–223).
      - Loop `i = 0..IMPAIR_DELAY_BUF_SIZE-1` (line 228):
        - Slot just inserted: `m_delay_buf[i].active == true` and `release_us = now_us + 50000 > now_us` — condition `release_us <= now_us` is false. Entry NOT collected.
      - Returns `out_count = 0`.
    - `count = 0`; the loop body (lines 290–299) never executes.
    - No delayed transmissions on this call.

15. **`send_message()` returns `Result::OK`** (line 379) after postcondition check `NEVER_COMPILED_OUT_ASSERT(wire_len > 0U)`.

**Phase 2 — Release (at or after `now_us + fixed_latency_ms * 1000`):**

16. A subsequent call to `send_message()` or `receive_message()` obtains a fresh `now_us' = timestamp_now_us()` where `now_us' >= now_us + 50000`.

17. **`flush_delayed_to_clients(now_us')`** or equivalently **`flush_delayed_to_queue(now_us')`** is called.

18. **`collect_deliverable(now_us', delayed, 32)`** (ImpairmentEngine.cpp:216):
    - Scans `m_delay_buf[0..31]`.
    - For the buffered entry: `active == true` and `release_us = now_us + 50000 <= now_us'` — condition true.
    - `envelope_copy(out_buf[0], m_delay_buf[i].env)` — copies the stored envelope to the output buffer.
    - `++out_count`.
    - `m_delay_buf[i].active = false` — slot reclaimed.
    - `NEVER_COMPILED_OUT_ASSERT(m_delay_count > 0U)` (line 236).
    - `--m_delay_count`.
    - Postconditions: `NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap)`, `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)` (lines 242–243).
    - Returns `out_count = 1`.

19. **In `flush_delayed_to_clients()`** (TcpBackend.cpp:290–299): for the one delayed entry:
    - `Serializer::serialize(delayed[0], m_wire_buf, SOCKET_RECV_BUF_BYTES, delayed_len)`.
    - `send_to_all_clients(m_wire_buf, delayed_len)` — calls `tcp_send_frame()` for each connected client.
    - The deferred message is now transmitted.

---

## 4. Call Tree (Hierarchical)

```
[Phase 1: send_message at T=0]
TcpBackend::send_message(envelope)             [TcpBackend.cpp:347]
├── NEVER_COMPILED_OUT_ASSERT(m_open)
├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, 8192, wire_len)  [Serializer.cpp:117]
├── timestamp_now_us()                         [T=0; Timestamp.hpp:31]
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)    [POSIX]
├── ImpairmentEngine::process_outbound(envelope, T0)  [ImpairmentEngine.cpp:151]
│   ├── [m_cfg.enabled == true → full impairment path]
│   ├── is_partition_active(T0) → false        [ImpairmentEngine.cpp:322]
│   ├── check_loss() → false                   [ImpairmentEngine.cpp:110]
│   ├── base_delay_us = 50 * 1000 = 50000      [line 183]
│   ├── jitter_us = 0                          [line 184; jitter_mean_ms == 0]
│   ├── release_us = T0 + 50000                [line 189]
│   ├── m_delay_count < 32 → proceed           [line 192]
│   ├── queue_to_delay_buf(in_env, T0+50000)   [ImpairmentEngine.cpp:83]
│   │   ├── find first inactive m_delay_buf[i]
│   │   ├── envelope_copy(m_delay_buf[i].env, env)  [memcpy sizeof(MessageEnvelope)]
│   │   ├── m_delay_buf[i].release_us = T0+50000
│   │   ├── m_delay_buf[i].active = true
│   │   ├── ++m_delay_count
│   │   └── return OK
│   ├── [duplication_probability == 0.0 → skip]
│   └── return OK
├── [res != ERR_IO → continue]
├── [m_client_count > 0 → continue]
├── send_to_all_clients(m_wire_buf, wire_len)  [TcpBackend.cpp:258]
│   └── [for each client i: tcp_send_frame(m_client_fds[i], ...)]
└── flush_delayed_to_clients(T0)               [TcpBackend.cpp:280]
    └── collect_deliverable(T0, delayed, 32)   [ImpairmentEngine.cpp:216]
        └── slot i: release_us=T0+50000 > T0 → NOT collected; returns 0

[Phase 2: flush_delayed_to_clients at T=T0+50000+ε]
TcpBackend::send_message(envelope2)  [or receive_message poll]
├── ...
└── flush_delayed_to_clients(T1)               [TcpBackend.cpp:280]
    ├── collect_deliverable(T1, delayed, 32)   [ImpairmentEngine.cpp:216]
    │   ├── slot i: active=true, release_us=T0+50000 <= T1 → COLLECT
    │   │   envelope_copy(delayed[0], m_delay_buf[i].env)
    │   │   m_delay_buf[i].active = false
    │   │   --m_delay_count
    │   └── return 1
    ├── Serializer::serialize(delayed[0], m_wire_buf, ...)
    └── send_to_all_clients(m_wire_buf, delayed_len)
        └── [for each client j: tcp_send_frame(m_client_fds[j], ...)]
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Owns impairment engine; calls `process_outbound` and `flush_delayed_to_clients` each send cycle |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Computes `release_us`; manages `m_delay_buf` via `queue_to_delay_buf()` |
| `ImpairmentEngine::queue_to_delay_buf()` | Private method | `ImpairmentEngine.cpp:83` | Finds first inactive `m_delay_buf` slot and writes the entry; called from `process_outbound()` |
| `ImpairmentConfig::fixed_latency_ms` | `uint32_t` field | `ImpairmentConfig.hpp:38` | Configures the fixed delay in milliseconds |
| `DelayEntry` | Nested struct | `ImpairmentEngine.hpp:155` | `{env, release_us, active}` — holds one buffered message |
| `m_delay_buf[32]` | Member array | `ImpairmentEngine.hpp:166` | Fixed-size array of delay entries; 32 slots (`IMPAIR_DELAY_BUF_SIZE`) |
| `m_delay_count` | `uint32_t` | `ImpairmentEngine.hpp:167` | Count of currently active (occupied) slots |
| `timestamp_now_us()` | Inline function | `src/core/Timestamp.hpp:31` | Called in `send_message()` before impairment processing; result reused for `flush_delayed_to_clients()` |
| `envelope_copy()` | Inline function | `src/core/MessageEnvelope.hpp:56` | Full `memcpy` of `sizeof(MessageEnvelope)` into delay buffer slot |
| `collect_deliverable()` | Method | `ImpairmentEngine.cpp:216` | Scans for matured entries; copies to caller's buffer |
| `flush_delayed_to_clients()` | Private method | `TcpBackend.cpp:280` | Calls `collect_deliverable()` and re-serializes + sends each deliverable message |
| `send_to_all_clients()` | Private method | `TcpBackend.cpp:258` | Iterates `m_client_fds[0..7]` and calls `tcp_send_frame()` for each active fd |
| `tcp_send_frame()` | Function | `src/platform/SocketUtils.cpp:393` | Performs the actual deferred socket write (4-byte length prefix + payload) |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Impairments disabled | ImpairmentEngine.cpp:159 | `!m_cfg.enabled` | `release_us = now_us` passed to `queue_to_delay_buf`; message released immediately on same `flush_delayed_to_clients()` call |
| Partition active | ImpairmentEngine.cpp:169 | `is_partition_active(now_us)` | If true: logs WARNING_LO, returns ERR_IO; message never buffered |
| Loss fires | ImpairmentEngine.cpp:176 | `check_loss()` returns true | Logs WARNING_LO, returns ERR_IO; message never buffered |
| Fixed latency zero | ImpairmentEngine.cpp:183 | `m_cfg.fixed_latency_ms == 0` | `base_delay_us = 0`; message released immediately if jitter also 0 |
| Jitter enabled | ImpairmentEngine.cpp:185 | `m_cfg.jitter_mean_ms > 0` | If true: additional random delay added to `release_us` |
| Delay buffer full | ImpairmentEngine.cpp:192 | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | Logs WARNING_HI; returns ERR_FULL (not ERR_IO); `send_message()` does not special-case ERR_FULL — see Section 14, Risk 1 |
| No clients | TcpBackend.cpp:369 | `m_client_count == 0U` | Logs WARNING_LO; returns OK; message was already buffered in delay buf; will be released to empty socket loop |
| Entry matured | ImpairmentEngine.cpp:229 | `m_delay_buf[i].active && release_us <= now_us` | If true: entry collected and slot reclaimed |
| Output buffer full during collect | ImpairmentEngine.cpp:228 | `out_count >= buf_cap` | Loop exits (`out_count < buf_cap` is a loop guard); remaining matured entries stay until next call |

**Zero-latency path**: When `!m_cfg.enabled`, `release_us = now_us` is passed to `queue_to_delay_buf()`. The subsequent `collect_deliverable(now_us, ...)` immediately finds and returns the entry (same `now_us`). This is the immediate-passthrough path without delay.

---

## 7. Concurrency / Threading Behavior

- No locking protects `m_delay_buf` or `m_delay_count`.
- `queue_to_delay_buf()` (called from `process_outbound()`) writes to `m_delay_buf` and increments `m_delay_count`.
- `collect_deliverable()` reads and writes `m_delay_buf` and decrements `m_delay_count`.
- Both are called from `send_message()` in sequence (same thread, same call stack via `process_outbound()` then `flush_delayed_to_clients()`). Likewise in `receive_message()` via `flush_delayed_to_queue()`.
- If `send_message()` and `receive_message()` are called from different threads on the same `TcpBackend` instance, concurrent access to `m_delay_buf` and `m_delay_count` would be a data race.
- [ASSUMPTION] Single-threaded use per `TcpBackend` instance.
- `timestamp_now_us()` → `clock_gettime(CLOCK_MONOTONIC, &ts)` is POSIX thread-safe.
- `PrngEngine::m_state` is a plain `uint64_t`; not thread-safe.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- Each `DelayEntry` in `m_delay_buf` contains a full `MessageEnvelope`. `MessageEnvelope` contains `uint8_t payload[MSG_MAX_PAYLOAD_BYTES]` = `payload[4096]` (Types.hpp:21) as an inline array, plus header fields. Total per-entry size = 3×`uint64_t` (24) + 2×`NodeId` (8) + `uint32_t` (4) + `MessageType` (1) + `uint8_t` (1) + `ReliabilityClass` (1) + padding + `payload[4096]` ≈ 4119+ bytes. The 32-slot `m_delay_buf` array therefore occupies approximately 131+ KB of `ImpairmentEngine`'s inline storage, and thus of `TcpBackend`'s inline storage.
- **Ownership transfer at buffer insertion**: `queue_to_delay_buf()` calls `envelope_copy()` which performs `memcpy(&dst, &src, sizeof(MessageEnvelope))` (MessageEnvelope.hpp:58). The caller's original `in_env` const reference is not retained; the copy in `m_delay_buf[i].env` is the live object until `collect_deliverable()` copies it out.
- **Second copy at collection**: `collect_deliverable()` calls `envelope_copy(out_buf[out_count], m_delay_buf[i].env)`, copying from the delay buffer slot into the stack-allocated `delayed[]` array in `flush_delayed_to_clients()`. The delay buffer slot is immediately cleared (`active = false`).
- **Third copy in `flush_delayed_to_clients()`**: `Serializer::serialize(delayed[i], m_wire_buf, ...)` reads `delayed[i]` and writes serialized bytes into `m_wire_buf` (TcpBackend.cpp:293). This is not an envelope copy but a serialization into the wire buffer.
- **Total envelope copies in path**: original `in_env` → `m_delay_buf[i].env` (in `queue_to_delay_buf`) → `delayed[i]` (in `collect_deliverable`) → serialized into `m_wire_buf` (in `flush_delayed_to_clients`). Two full `MessageEnvelope` copies plus one serialization pass.
- **`delayed[IMPAIR_DELAY_BUF_SIZE]`** in `flush_delayed_to_clients()` (TcpBackend.cpp:285): stack array of 32 `MessageEnvelope` objects. If `sizeof(MessageEnvelope) ≈ 4119` bytes, this is approximately 131 KB of stack allocation per call to `flush_delayed_to_clients()`.
- No heap allocation anywhere. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| Serialization error (initial) | TcpBackend.cpp:356 | `!result_ok(res)`: logs WARNING_LO, returns `res` early; engine not called |
| `clock_gettime()` failure | Timestamp.hpp:36 | `NEVER_COMPILED_OUT_ASSERT(result == 0)` fires in debug; in release, `now_us` receives garbage; impairment state machine may misbehave |
| Partition active | ImpairmentEngine.cpp:169 | Logs WARNING_LO; returns ERR_IO; no delay buffer entry created |
| Loss fires | ImpairmentEngine.cpp:176 | Logs WARNING_LO; returns ERR_IO; no delay buffer entry created |
| Delay buffer full | ImpairmentEngine.cpp:192 | Logs WARNING_HI; returns ERR_FULL; `send_message()` does NOT special-case ERR_FULL (only ERR_IO is checked at line 364); execution continues to `send_to_all_clients()` which sends the already-serialized `m_wire_buf` directly, bypassing the delay. |
| No free slot in `queue_to_delay_buf` | ImpairmentEngine.cpp:102 | `NEVER_COMPILED_OUT_ASSERT(false)` fires (logic error); unreachable in correct operation |
| Serialization error (delayed) | TcpBackend.cpp:295 | `continue` — delayed message skipped; permanently lost without logging |
| `tcp_send_frame()` fails | `send_to_all_clients()` TcpBackend.cpp:270 | Logs WARNING_LO per failing client; loop continues to next client |
| `m_client_count == 0` | TcpBackend.cpp:369 | Logs WARNING_LO; returns OK; message was already placed in delay buffer; will remain there until next collect passes |

---

## 10. External Interactions

| Interaction | Function | API | Timing |
|---|---|---|---|
| Clock read | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC)` POSIX | Once in `send_message()` before impairment; same `now_us` used for `flush_delayed_to_clients()` |
| Clock read (receive poll) | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC)` POSIX | Once per outer loop iteration in `receive_message()` (TcpBackend.cpp:411) |
| Socket write | `tcp_send_frame()` via `socket_send_all()` | POSIX `poll()` + `send()` on TCP fd | At release time via `send_to_all_clients()` |
| Logger | `Logger::log()` | Internal | On partition drop, loss drop, buffer full, client send failure |

The wall-clock gap between the `timestamp_now_us()` call in `send_message()` and the next call where `flush_delayed_to_clients()` fires determines the actual observed delay. OS scheduling, system load, and `clock_gettime` resolution all affect whether `fixed_latency_ms` is precisely honored.

---

## 11. State Changes / Side Effects

| State | Object | Phase | Change |
|---|---|---|---|
| `m_delay_buf[i].env` | `ImpairmentEngine` | Send | Filled with `memcpy` of `in_env` via `queue_to_delay_buf()` |
| `m_delay_buf[i].release_us` | `ImpairmentEngine` | Send | Set to `now_us + fixed_latency_ms * 1000` |
| `m_delay_buf[i].active` | `ImpairmentEngine` | Send | Set to `true` |
| `m_delay_count` | `ImpairmentEngine` | Send | Incremented by 1 |
| `m_delay_buf[i].active` | `ImpairmentEngine` | Release | Set to `false` |
| `m_delay_count` | `ImpairmentEngine` | Release | Decremented by 1 |
| `delayed[]` (stack, in `flush_delayed_to_clients`) | `TcpBackend` local | Release | Filled with `memcpy` of buffered envelope; freed on return |
| `m_wire_buf` | `TcpBackend` | Transmission | Overwritten by `Serializer::serialize` of delayed envelope |

---

## 12. Sequence Diagram

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant ImpairmentEngine
    participant clock_gettime
    participant tcp_send_frame

    Caller->>TcpBackend: send_message(env)
    TcpBackend->>TcpBackend: Serializer::serialize(env, m_wire_buf)
    TcpBackend->>clock_gettime: timestamp_now_us()
    clock_gettime-->>TcpBackend: T0
    TcpBackend->>ImpairmentEngine: process_outbound(env, T0)
    Note over ImpairmentEngine: base_delay=fixed_latency_ms*1000<br/>release_us=T0+50000<br/>queue_to_delay_buf(env, T0+50000)<br/>m_delay_buf[0]={env,T0+50000,true}
    ImpairmentEngine-->>TcpBackend: OK
    TcpBackend->>TcpBackend: send_to_all_clients(m_wire_buf, wire_len)
    TcpBackend->>TcpBackend: flush_delayed_to_clients(T0)
    TcpBackend->>ImpairmentEngine: collect_deliverable(T0, delayed, 32)
    Note over ImpairmentEngine: slot0: T0+50000 > T0 → not ready
    ImpairmentEngine-->>TcpBackend: 0
    TcpBackend-->>Caller: OK

    Note over Caller,tcp_send_frame: 50 ms elapses

    Caller->>TcpBackend: send_message(env2)
    TcpBackend->>clock_gettime: timestamp_now_us()
    clock_gettime-->>TcpBackend: T1 ≥ T0+50000
    TcpBackend->>ImpairmentEngine: process_outbound(env2, T1)
    Note over ImpairmentEngine: buffers env2 at release_us=T1+50000
    ImpairmentEngine-->>TcpBackend: OK
    TcpBackend->>TcpBackend: send_to_all_clients(m_wire_buf, wire_len)
    TcpBackend->>TcpBackend: flush_delayed_to_clients(T1)
    TcpBackend->>ImpairmentEngine: collect_deliverable(T1, delayed, 32)
    Note over ImpairmentEngine: slot0: T0+50000 ≤ T1 → COLLECT<br/>envelope_copy(delayed[0], slot0.env)<br/>slot0.active=false; m_delay_count--
    ImpairmentEngine-->>TcpBackend: 1
    TcpBackend->>TcpBackend: serialize(delayed[0]) → m_wire_buf
    TcpBackend->>tcp_send_frame: send_to_all_clients(m_wire_buf, delayed_len)
    tcp_send_frame-->>TcpBackend: true
    TcpBackend-->>Caller: OK
```

---

## 13. Initialization vs Runtime Flow

**Initialization** (`ImpairmentEngine::init()` called from `TcpBackend::init()` at TcpBackend.cpp:104):
- `fixed_latency_ms` defaults to `0` in `impairment_config_default()` (ImpairmentConfig.hpp:82). When `0`, `base_delay_us = 0`, and if jitter is also 0, `release_us = now_us`. The first `collect_deliverable()` call immediately returns the message (zero-delay passthrough).
- For a meaningful fixed latency, `fixed_latency_ms` must be set to a non-zero value in the `ImpairmentConfig` before calling `ImpairmentEngine::init()`.
- `TcpBackend::init()` (TcpBackend.cpp:88) calls `impairment_config_default(imp_cfg)` and then only overrides `imp_cfg.enabled` from `config.channels[0].impairments_enabled` (lines 100–103). All other impairment fields (`fixed_latency_ms`, `jitter_mean_ms`, etc.) remain at their defaults (0) unless the caller populates the full `ImpairmentConfig` and passes it directly to `m_impairment.init()`.
- `m_delay_buf` is zeroed via `memset` in `ImpairmentEngine::init()` (line 58). `m_delay_count = 0` (line 59). `m_initialized = true` (line 72).
- PRNG is seeded: `seed = cfg.prng_seed != 0 ? cfg.prng_seed : 42ULL` (ImpairmentEngine.cpp:54).

**Runtime**: The delay buffer acts as a time-ordered queue, though it is not sorted — `collect_deliverable()` performs a linear scan of all 32 slots each call. With 32 slots and high message rates, multiple messages may be buffered simultaneously. All messages with `release_us <= now_us` are collected in a single scan. The buffer provides no ordering guarantee: messages with different `release_us` values may be collected in array-index order, not release-time order.

---

## 14. Known Risks / Observations

1. **`ERR_FULL` from `process_outbound()` is not special-cased in `send_message()`**: `send_message()` at line 364 checks `if (res == Result::ERR_IO)`. `ERR_FULL` (value `2`) is not `ERR_IO` (value `5`) and falls through. Execution then proceeds to `send_to_all_clients()`, which transmits the already-serialized `m_wire_buf` immediately — bypassing the intended delay. Messages that overflow the delay buffer are silently sent without the configured latency.

2. **Delay buffer is not sorted by `release_us`**: Messages with earlier `release_us` values are not guaranteed to be collected before those with later values unless they occupy lower array indices. `collect_deliverable()` scans linearly from index 0.

3. **Buffer capacity limits maximum in-flight delayed messages**: With 32 slots and `fixed_latency_ms = 50 ms`, the maximum sustainable throughput before buffer saturation is 32 messages / 50 ms = 640 msg/s. At higher rates, `ERR_FULL` is returned and the message bypasses delay (see Risk 1).

4. **`flush_delayed_to_clients()` is called with the same `now_us` as `process_outbound()`**: At TcpBackend.cpp:362–376, both calls use the same `now_us` computed at line 362. A message buffered in the current `process_outbound()` call has `release_us = now_us + delay > now_us` and is guaranteed not to be collected immediately.

5. **Delayed message serialization failure is silent**: If `Serializer::serialize` fails for a delayed envelope (TcpBackend.cpp:295), `continue` skips that entry with no logging. The message is permanently lost.

6. **Large stack allocation in `flush_delayed_to_clients()`**: `delayed[IMPAIR_DELAY_BUF_SIZE]` at TcpBackend.cpp:285 allocates 32 `MessageEnvelope` objects on the stack, approximately 131+ KB per call frame.

7. **Polling frequency determines effective precision**: Delay is only enforced at `flush_delayed_to_clients()` call points (each `send_message()` call or each `receive_message()` iteration). If calls are infrequent, messages may be held longer than `fixed_latency_ms`.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `fixed_latency_ms` is configured non-zero at a higher layer before `ImpairmentEngine::init()` is called. `TcpBackend::init()` only populates `enabled` from channel config; all other impairment parameters remain at default (0/false) unless the caller explicitly sets them.
- [ASSUMPTION] `flush_delayed_to_clients()` (or `flush_delayed_to_queue()` on the receive path) is called frequently enough relative to `fixed_latency_ms` to achieve the intended delay granularity.
- [ASSUMPTION] `CLOCK_MONOTONIC` on the target platform has sub-millisecond resolution. On Linux this is typically 1 µs; on some platforms it may be coarser.
- [ASSUMPTION] The 32-slot delay buffer (`IMPAIR_DELAY_BUF_SIZE = 32U`, Types.hpp:28) is sufficient for the message rate and configured latency of the deployment.
- [ASSUMPTION] Single-threaded access per `TcpBackend` instance.
- [UNKNOWN] Whether `tcp_send_frame()` can block (socket is in blocking mode unless `socket_set_nonblocking()` is explicitly called) and for how long.
- [UNKNOWN] Whether the receive-side `flush_delayed_to_queue()` in `receive_message()` (TcpBackend.cpp:412) provides sufficient collection frequency for the configured `fixed_latency_ms` values in the absence of outbound traffic.
