## UC_14: ImpairmentEngine duplicates an outbound message based on configured duplication probability

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()`. The message survives the loss check inside `ImpairmentEngine::process_outbound()` and is placed into the delay buffer with a computed `release_us`. Immediately after, the duplication check fires: `m_prng.next_double()` is called a second time. If that value is less than `ImpairmentConfig::duplication_probability`, a second copy of the same envelope is inserted into the delay buffer with `release_us + 100` microseconds. Both copies are later released by `collect_deliverable()` and transmitted, causing the receiver to see the same message twice.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp:249` | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp:62` | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `ImpairmentEngine::collect_deliverable()` | `src/platform/ImpairmentEngine.cpp:174` | `uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us, MessageEnvelope* out_buf, uint32_t buf_cap)` |

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Caller invokes `TcpBackend::send_message(envelope)`** (TcpBackend.cpp:249).
   - Preconditions: `assert(m_open)`, `assert(envelope_valid(envelope))`.

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` (TcpBackend.cpp:256). Returns OK; wire bytes written to `m_wire_buf`.

3. **Obtain time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:264). Calls `clock_gettime(CLOCK_MONOTONIC)`.

4. **Call `m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:265).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:62):
   a. Preconditions: `assert(m_initialized)`, `assert(envelope_valid(in_env))`.
   b. **Master switch**: `m_cfg.enabled` is true; continues.
   c. **Partition check**: `is_partition_active(now_us)` returns false; continues.
   d. **Loss check** (lines 100–108): `m_prng.next_double()` called; result >= `loss_probability`; message is NOT dropped.
   e. **Latency calculation** (lines 111–122):
      - `base_delay_us = m_cfg.fixed_latency_ms * 1000ULL`.
      - If `m_cfg.jitter_mean_ms > 0`: `jitter_offset_ms = m_prng.next_range(0, m_cfg.jitter_variance_ms)`; `jitter_us = jitter_offset_ms * 1000ULL`. Otherwise `jitter_us = 0`.
      - `total_delay_us = base_delay_us + jitter_us`.
      - `release_us = now_us + total_delay_us`.
   f. **Buffer capacity check** (line 125): `m_delay_count < IMPAIR_DELAY_BUF_SIZE`; continues.
   g. **Primary copy buffered** (lines 132–141): Loop over `m_delay_buf[0..31]`, find first slot where `active == false`:
      - `envelope_copy(m_delay_buf[i].env, in_env)` — `memcpy` of full `MessageEnvelope`.
      - `m_delay_buf[i].release_us = release_us`.
      - `m_delay_buf[i].active = true`.
      - `++m_delay_count`.
      - `assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
      - Break.
   h. **Duplication check** (lines 144–163):
      - Guard: `if (m_cfg.duplication_probability > 0.0)`.
      - `dup_rand = m_prng.next_double()` — advances PRNG state again; returns double in [0.0, 1.0).
      - `assert(dup_rand >= 0.0 && dup_rand < 1.0)`.
      - `if (dup_rand < m_cfg.duplication_probability)` — if true:
        - Inner capacity check: `if (m_delay_count < IMPAIR_DELAY_BUF_SIZE)`.
        - Inner loop over `m_delay_buf[0..31]`, find first inactive slot:
          - `envelope_copy(m_delay_buf[i].env, in_env)` — second `memcpy` of the same envelope.
          - `m_delay_buf[i].release_us = release_us + 100ULL` — duplicate released 100 µs after original.
          - `m_delay_buf[i].active = true`.
          - `++m_delay_count`.
          - `Logger::log(Severity::WARNING_LO, "ImpairmentEngine", "message duplicated")`.
          - Break.
   i. Postcondition: `assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
   j. Returns `Result::OK`.

6. **Back in `send_message()`** (TcpBackend.cpp:266): `res == OK`; not the ERR_IO branch; continues.

7. **`collect_deliverable()` called** (TcpBackend.cpp:273–275): `m_impairment.collect_deliverable(now_us, delayed_envelopes, IMPAIR_DELAY_BUF_SIZE)`.
   - Scans `m_delay_buf[0..31]`.
   - For each slot where `active == true` AND `release_us <= now_us`: copies to `out_buf`, marks slot inactive, decrements `m_delay_count`.
   - Returns count of ready messages.
   - **At the moment of the initial `send_message()` call**: if `total_delay_us > 0`, neither the original nor the duplicate has matured yet (`release_us > now_us`). `delayed_count = 0`. Both remain in the buffer.
   - **On a subsequent `send_message()` call** (or the receive-side poll loop): when `now_us >= release_us`, the original is collected. When `now_us >= release_us + 100`, the duplicate is collected.

8. **Transmission**: For each collected envelope (up to two for the duplicate case), `Serializer::serialize()` is called again and `tcp_send_frame()` transmits the bytes to all connected clients.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)
├── assert(m_open)
├── assert(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, ...)
├── timestamp_now_us()
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us)
│   ├── assert(m_initialized)
│   ├── assert(envelope_valid(in_env))
│   ├── [!m_cfg.enabled → zero-delay buffer; NOT taken]
│   ├── is_partition_active(now_us) → false
│   ├── [loss_probability > 0.0]
│   │   └── PrngEngine::next_double()                     ← PRNG call #1 (loss check)
│   │       └── PrngEngine::next() [xorshift64]
│   ├── [jitter_mean_ms > 0: optional]
│   │   └── PrngEngine::next_range(0, jitter_variance_ms) ← PRNG call #2 (jitter; may be absent)
│   │       └── PrngEngine::next() [xorshift64]
│   ├── [find first inactive slot in m_delay_buf]
│   │   └── envelope_copy(m_delay_buf[i].env, in_env)     ← PRIMARY COPY (memcpy)
│   │       m_delay_buf[i].release_us = release_us
│   │       m_delay_buf[i].active = true
│   │       ++m_delay_count
│   ├── [duplication_probability > 0.0]
│   │   ├── PrngEngine::next_double()                     ← PRNG call #N (dup check)
│   │   │   └── PrngEngine::next() [xorshift64]
│   │   └── [dup_rand < duplication_probability]
│   │       ├── [find first inactive slot in m_delay_buf]
│   │       │   └── envelope_copy(m_delay_buf[j].env, in_env) ← DUPLICATE COPY (memcpy)
│   │       │       m_delay_buf[j].release_us = release_us + 100ULL
│   │       │       m_delay_buf[j].active = true
│   │       │       ++m_delay_count
│   │       └── Logger::log(WARNING_LO, "message duplicated")
│   └── return Result::OK
├── ImpairmentEngine::collect_deliverable(now_us, delayed_envelopes, 32)
│   ├── assert(m_initialized), assert(out_buf != NULL), assert(buf_cap > 0)
│   ├── [scan m_delay_buf[0..31]]
│   │   └── [if active && release_us <= now_us] → envelope_copy to out_buf; mark inactive
│   └── return delayed_count
├── [if m_client_count == 0] → discard; return OK
├── [send original wire bytes to all clients via tcp_send_frame]
└── [for each delayed envelope]
    ├── Serializer::serialize(delayed_envelopes[i], m_wire_buf, ...)
    └── [send to all clients via tcp_send_frame]
        ← DUPLICATE TRANSMITTED here when its release_us matures
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Owns `ImpairmentEngine`; orchestrates send and delayed delivery |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Applies duplication; manages `m_delay_buf` |
| `ImpairmentConfig` | POD struct | `src/platform/ImpairmentConfig.hpp` | Holds `duplication_probability` (double) |
| `PrngEngine` | Class | `src/platform/PrngEngine.hpp` | xorshift64; `next_double()` called separately for loss and duplication |
| `DelayEntry` | Nested struct | `ImpairmentEngine.hpp:125` | `{MessageEnvelope env, uint64_t release_us, bool active}` — one slot per buffered message |
| `envelope_copy()` | Inline function | `src/core/MessageEnvelope.hpp:49` | `memcpy(&dst, &src, sizeof(MessageEnvelope))` — used for both original and duplicate |
| `Logger::log()` | Static function | `src/core/Logger.hpp` | Records WARNING_LO "message duplicated" |
| `collect_deliverable()` | Method | `ImpairmentEngine.cpp:174` | Releases matured entries from `m_delay_buf` to caller |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Master switch | ImpairmentEngine.cpp:70 | `!m_cfg.enabled` | If true: zero-delay buffer, no duplication check |
| Partition check | ImpairmentEngine.cpp:93 | `is_partition_active(now_us)` | If true: ERR_IO drop before duplication check |
| Loss survives | ImpairmentEngine.cpp:103 | `loss_rand >= loss_probability` | Message must survive loss to reach duplication check |
| Jitter enabled | ImpairmentEngine.cpp:114 | `m_cfg.jitter_mean_ms > 0` | If true: extra PRNG call for jitter offset before duplication |
| Delay buffer full (primary) | ImpairmentEngine.cpp:125 | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | If true: ERR_FULL returned; duplication check not reached |
| Duplication probability guard | ImpairmentEngine.cpp:144 | `m_cfg.duplication_probability > 0.0` | If false: PRNG not called for duplication |
| Duplication decision | ImpairmentEngine.cpp:147 | `dup_rand < m_cfg.duplication_probability` | If true: second copy placed in delay buffer |
| Delay buffer full (duplicate) | ImpairmentEngine.cpp:149 | `m_delay_count < IMPAIR_DELAY_BUF_SIZE` | If false: duplicate silently not added (buffer full) |
| Release time check | ImpairmentEngine.cpp:187 | `release_us <= now_us` | Controls when original and duplicate are handed to transmission |

**Critical ordering**: The original is buffered first (primary slot), then the duplication check runs. If the buffer is full after the primary is inserted (exactly at capacity), the duplicate's inner capacity check `if (m_delay_count < IMPAIR_DELAY_BUF_SIZE)` fails silently — no duplicate is added and no error is returned.

---

## 7. Concurrency / Threading Behavior

- No locks or atomics in `process_outbound()` or `collect_deliverable()`.
- `m_delay_buf` and `m_delay_count` are plain member variables. Both are modified in `process_outbound()` (writes) and `collect_deliverable()` (reads and writes). Concurrent calls from multiple threads produce data races.
- [ASSUMPTION] Single-threaded access to each `TcpBackend` instance is required for correctness.
- `m_prng.m_state` is advanced twice in this path (loss check + duplication check), plus optionally a third time if jitter is enabled.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- Both the original and the duplicate are stored as full `MessageEnvelope` copies inside `DelayEntry::env` in `m_delay_buf`. Each copy is a full `memcpy` of `sizeof(MessageEnvelope)` bytes, which includes the fixed-size `payload[4096]` array regardless of `payload_length`. This is a potentially large copy (approximately 4119 bytes per envelope).
- `m_delay_buf` is a statically-sized member array of 32 `DelayEntry` slots. With both original and duplicate buffered, two slots are consumed until `collect_deliverable()` reclaims them.
- `m_delay_count` tracks the number of active (occupied) slots. It is incremented twice in the duplication path (once per `envelope_copy`). Decremented by `collect_deliverable()` when each matures.
- `delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]` at TcpBackend.cpp:272 is a local stack array of 32 `MessageEnvelope` objects. Stack frame size: 32 * ~4119 bytes = approximately 131 KB. [ASSUMPTION] The stack is large enough; no guard against stack overflow is present.
- `in_env` (the original caller's envelope reference) is never modified. Both copies are independent of the caller's object.
- Power of 10 rule 3: no `malloc` or `new` anywhere in this path.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| Serialization fails | TcpBackend.cpp:258 | Return error to caller; impairment engine not entered |
| Partition active | ImpairmentEngine.cpp:96 | Return ERR_IO; message and duplicate both dropped |
| Loss fires | ImpairmentEngine.cpp:105 | Return ERR_IO; duplication check not reached |
| Primary delay buffer full | ImpairmentEngine.cpp:128 | Return ERR_FULL; duplication check not reached |
| Duplicate buffer full (inner check) | ImpairmentEngine.cpp:149 | Duplicate silently not added; function returns OK anyway |
| `collect_deliverable` out_buf full | ImpairmentEngine.cpp:186 | Loop exits early; remaining matured entries left in buffer until next call |

The silent suppression of the duplicate when the buffer is full (ImpairmentEngine.cpp:149) means the actual duplication rate may be lower than configured at high traffic levels.

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Event logging | `Logger::log()` | WARNING_LO "message duplicated" |
| Socket write (original) | `tcp_send_frame()` | POSIX write/send on TCP fd — when original matures |
| Socket write (duplicate) | `tcp_send_frame()` | POSIX write/send on TCP fd — 100 µs later when duplicate matures |

---

## 11. State Changes / Side Effects

| State | Object | Change |
|---|---|---|
| `m_prng.m_state` | `PrngEngine` | Advanced at least twice (loss + dup checks); three times if jitter enabled |
| `m_delay_buf[i]` | `ImpairmentEngine` | Slot i: original copy written; `active = true`; `release_us` set |
| `m_delay_buf[j]` | `ImpairmentEngine` | Slot j: duplicate copy written; `active = true`; `release_us = original + 100` |
| `m_delay_count` | `ImpairmentEngine` | Incremented by 2 (one for original, one for duplicate) |
| Logger output | Process-level | WARNING_LO "message duplicated" emitted |
| `m_wire_buf` | `TcpBackend` | Overwritten on each `collect_deliverable` transmission iteration |

After `collect_deliverable()` runs twice (once for each matured entry), `m_delay_count` decrements by 2 and both slots become `active = false`.

---

## 12. Sequence Diagram (ASCII)

```
Caller        TcpBackend         ImpairmentEngine    PrngEngine    Logger   tcp_send_frame
  |                |                    |                 |           |            |
  |send_message(e) |                    |                 |           |            |
  |--------------->|                    |                 |           |            |
  |                | process_outbound() |                 |           |            |
  |                |------------------->|                 |           |            |
  |                |                    | next_double()   |           |            |
  |                |                    |[loss check]---->|           |            |
  |                |                    |<-- 0.80 --------|           |            |
  |                |                    |[0.80 >= 0.05: survives]     |            |
  |                |                    |                 |           |            |
  |                |                    | [compute release_us]        |            |
  |                |                    | [buffer primary copy]       |            |
  |                |                    |   m_delay_buf[0].active=true|            |
  |                |                    |   m_delay_count=1           |            |
  |                |                    |                 |           |            |
  |                |                    | next_double()   |           |            |
  |                |                    |[dup check]----->|           |            |
  |                |                    |<-- 0.03 --------|           |            |
  |                |                    |[0.03 < 0.10: DUPLICATE]     |            |
  |                |                    |   m_delay_buf[1].active=true|            |
  |                |                    |   release_us+100            |            |
  |                |                    |   m_delay_count=2           |            |
  |                |                    | log(WARNING_LO)             |            |
  |                |                    |------------------------>    |            |
  |                |<---- OK -----------|                 |           |            |
  |                |                    |                 |           |            |
  |                | collect_deliverable(now_us)          |           |            |
  |                |------------------->|                 |           |            |
  |                |  [release_us > now_us: 0 ready]      |           |            |
  |                |<-- 0 -------------|                  |           |            |
  |<--- OK --------|                    |                 |           |            |
  :                :                    :                 :           :            :
  [... time passes: now_us >= release_us ...]
  :                :                    :                 :           :            :
  |send_message(e2)|                    |                 |           |            |
  |--------------->|                    |                 |           |            |
  |                | collect_deliverable(now_us')         |           |            |
  |                |------------------->|                 |           |            |
  |                |  [slot 0 ready: original]            |           |            |
  |                |<-- delayed_count=1-|                 |           |            |
  |                |                    |                 |           | send(orig) |
  |                |-------------------------------------------------->|           |
  :                :                    :                 :           :            :
  [... 100µs later ...]
  |send_message(e3)|                    |                 |           |            |
  |--------------->|                    |                 |           |            |
  |                | collect_deliverable(now_us'')        |           |            |
  |                |------------------->|                 |           |            |
  |                |  [slot 1 ready: duplicate]           |           |            |
  |                |<-- delayed_count=1-|                 |           |            |
  |                |                    |                 |           | send(dup)  |
  |                |-------------------------------------------------->|           |
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase**: Same as UC_13. Key additional field: `m_cfg.duplication_probability` defaults to `0.0` via `impairment_config_default()`. Must be set by a higher-level layer before `init()` to be non-zero.

**Runtime phase**:
- First `send_message()` call: PRNG advanced for loss check, optionally for jitter, then for duplication check. Two `DelayEntry` slots occupied.
- Subsequent `send_message()` calls or the receive poll loop: `collect_deliverable()` scans for matured entries. Original and duplicate are released at different times (`release_us` and `release_us + 100`), so they may be collected on the same or different calls depending on polling frequency relative to 100 µs.
- If the polling interval is coarser than 100 µs (which is typical — the `receive_message` poll uses 100 ms increments), both original and duplicate are likely collected in the same `collect_deliverable()` call.

---

## 14. Known Risks / Observations

1. **Silent duplicate suppression when buffer full**: If `m_delay_count == IMPAIR_DELAY_BUF_SIZE - 1` after the primary is inserted (i.e., buffer now full), the inner duplicate capacity check fails silently. No log, no error; the configured duplication rate is not achieved.

2. **100 µs fixed offset is hardcoded**: The duplicate's `release_us + 100ULL` offset (ImpairmentEngine.cpp:153) is a magic constant, not part of `ImpairmentConfig`. It cannot be configured. At typical polling granularities (milliseconds), both copies arrive at virtually the same time.

3. **Large stack frame in `send_message()`**: `delayed_envelopes[IMPAIR_DELAY_BUF_SIZE]` (TcpBackend.cpp:272) is 32 full `MessageEnvelope` objects on the stack. Each envelope is approximately 4119 bytes (4096 payload + fields), so approximately 131 KB of stack is consumed per `send_message()` call.

4. **PRNG call count depends on configuration**: The number of PRNG calls in `process_outbound()` is 1 (loss only) + 1 if jitter enabled + 1 (duplication check) = 2 or 3. Enabling or disabling jitter shifts the PRNG sequence seen by the duplication check, altering which messages are duplicated in a given test run even with the same seed.

5. **Duplicate carries identical `message_id`**: The duplicated `MessageEnvelope` is an exact `memcpy` of the original, including `message_id`. Receivers with a deduplication window (e.g., `DedupFilter`) will detect and suppress the second copy, which is the intended behavior for testing deduplication logic.

6. **`assert(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)` at ImpairmentEngine.cpp:166** is the only postcondition check. The inner duplicate insertion loop does not have its own postcondition assertion on `m_delay_count` after incrementing.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The receive-side deduplication mechanism (e.g., `DedupFilter` using a sliding window keyed on `(source_id, message_id)`) exists elsewhere in the codebase and is what the duplication impairment is intended to test.
- [ASSUMPTION] `tcp_send_frame()` is called with the re-serialized duplicate bytes, not the original `m_wire_buf` (which may have been overwritten). This is correct per the code at TcpBackend.cpp:302–305.
- [ASSUMPTION] The stack is large enough to hold `delayed_envelopes[32]` (~131 KB). No stack guard or overflow detection is visible.
- [ASSUMPTION] Single-threaded access per `TcpBackend` instance.
- [UNKNOWN] Whether 100 µs is a meaningful delay at the TCP transmission layer given OS scheduling jitter.
- [UNKNOWN] Whether `duplication_probability = 1.0` (all messages duplicated) has been tested with the buffer full conditions that would arise.
- [UNKNOWN] The `Logger::log()` implementation's thread-safety and performance characteristics.

---