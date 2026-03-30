# UC_14 — Impairment: duplication

**HL Group:** HL-12 — User configures network impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.4, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

A caller invokes `TcpBackend::send_message()`. The message survives the loss check inside
`ImpairmentEngine::process_outbound()` and is placed into the delay buffer via the private helper
`queue_to_delay_buf()` with a computed `release_us`. Immediately after, `process_outbound()` calls
the private helper `apply_duplication()`. Inside `apply_duplication()`, `m_prng.next_double()` is
called. If that value is less than `ImpairmentConfig::duplication_probability`, a second call to
`queue_to_delay_buf()` inserts a duplicate copy into the delay buffer with `release_us + 100`
microseconds. Both entries are later released by `collect_deliverable()` — called from
`flush_delayed_to_clients()` — and transmitted via `send_to_all_clients()`, causing the receiver to
see the same message twice.

---

## 2. Entry Points

| Entry Point | File | Line | Signature |
|---|---|---|---|
| `TcpBackend::send_message()` | `src/platform/TcpBackend.cpp` | 347 | `Result TcpBackend::send_message(const MessageEnvelope& envelope)` |
| `ImpairmentEngine::process_outbound()` | `src/platform/ImpairmentEngine.cpp` | 151 | `Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)` |
| `ImpairmentEngine::apply_duplication()` | `src/platform/ImpairmentEngine.cpp` | 127 | `void ImpairmentEngine::apply_duplication(const MessageEnvelope& env, uint64_t release_us)` — private helper |
| `ImpairmentEngine::collect_deliverable()` | `src/platform/ImpairmentEngine.cpp` | 216 | `uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us, MessageEnvelope* out_buf, uint32_t buf_cap)` |

---

## 3. End-to-End Control Flow (Step-by-Step)

1. **Caller invokes `TcpBackend::send_message(envelope)`** (TcpBackend.cpp:347).
   - `NEVER_COMPILED_OUT_ASSERT(m_open)` — always active; not stripped in release builds.
   - `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.

2. **Serialize**: `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)`
   (TcpBackend.cpp:354). Returns OK; wire bytes written to `m_wire_buf`.
   - Return value checked: if not OK, log WARNING_LO and return to caller immediately.

3. **Obtain time**: `now_us = timestamp_now_us()` (TcpBackend.cpp:362). Calls
   `clock_gettime(CLOCK_MONOTONIC)`.

4. **Call `m_impairment.process_outbound(envelope, now_us)`** (TcpBackend.cpp:363).

5. **Inside `process_outbound()`** (ImpairmentEngine.cpp:151):
   a. `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
   b. **Master switch** (line 159): `m_cfg.enabled` is true; continues.
   c. **Partition check** (line 169): `is_partition_active(now_us)` returns false; continues.
   d. **Loss check** (line 176): calls private `check_loss()` (ImpairmentEngine.cpp:110).
      - `check_loss()` calls `m_prng.next_double()`; result >= `loss_probability`; returns false.
      - Message is NOT dropped; continues.
   e. **Latency calculation** (lines 183–189):
      - `base_delay_us = m_cfg.fixed_latency_ms * 1000ULL`.
      - If `m_cfg.jitter_mean_ms > 0U`: `jitter_offset_ms = m_prng.next_range(0U, m_cfg.jitter_variance_ms)`;
        `jitter_us = jitter_offset_ms * 1000ULL`. Otherwise `jitter_us = 0ULL`.
      - `release_us = now_us + base_delay_us + jitter_us`.
   f. **Delay buffer capacity check** (line 192): `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` is false;
      continues.
   g. **Primary copy buffered via `queue_to_delay_buf(in_env, release_us)`** (line 197):
      - Enters `queue_to_delay_buf()` (ImpairmentEngine.cpp:83).
      - `NEVER_COMPILED_OUT_ASSERT(m_initialized)`,
        `NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE)`.
      - Fixed loop (bound: `IMPAIR_DELAY_BUF_SIZE`): finds first slot where `active == false`.
      - `envelope_copy(m_delay_buf[i].env, env)` — `memcpy` of full `MessageEnvelope` (~4119 bytes).
      - `m_delay_buf[i].release_us = release_us`.
      - `m_delay_buf[i].active = true`.
      - `++m_delay_count`.
      - `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
      - Returns `Result::OK`.
   h. Return value of `queue_to_delay_buf` checked (line 198): `result_ok(res)` is true; continues.
   i. **Duplication probability guard** (line 203): `m_cfg.duplication_probability > 0.0` is true;
      calls `apply_duplication(in_env, release_us)` (line 204).

6. **Inside `apply_duplication(env, release_us)`** (ImpairmentEngine.cpp:127):
   a. `NEVER_COMPILED_OUT_ASSERT(m_initialized)`,
      `NEVER_COMPILED_OUT_ASSERT(m_cfg.duplication_probability > 0.0)`.
   b. `dup_rand = m_prng.next_double()` — advances PRNG state; returns double in [0.0, 1.0).
   c. `NEVER_COMPILED_OUT_ASSERT(dup_rand >= 0.0 && dup_rand < 1.0)`.
   d. `if (dup_rand < m_cfg.duplication_probability)` — if true (duplication fires):
      - Inner capacity check: `if (m_delay_count < IMPAIR_DELAY_BUF_SIZE)`.
      - If capacity available: `queue_to_delay_buf(env, release_us + 100ULL)`.
        - Finds next inactive slot; inserts duplicate with `release_us` offset by **100 µs**
          (a fixed hardcoded constant, not 100 ms).
        - `++m_delay_count` (now 2 total).
        - Returns `Result::OK`.
      - `result_ok(res)` checked (line 139): on success, logs WARNING_LO "message duplicated".
   e. Returns (void).

7. **Back in `process_outbound()`** (line 208):
   - `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)` — postcondition.
   - Returns `Result::OK`.

8. **Back in `send_message()`** (TcpBackend.cpp:364):
   - `res == Result::OK` (not `ERR_IO`); the `ERR_IO` branch is not taken.
   - `m_client_count > 0U`; continues.

9. **`send_to_all_clients(m_wire_buf, wire_len)`** (TcpBackend.cpp:375):
   - Loops over `m_client_fds[0..MAX_TCP_CONNECTIONS-1]`; calls `tcp_send_frame()` for each active fd.
   - Transmits the original message (pre-delay) to all connected clients immediately.

10. **`flush_delayed_to_clients(now_us)`** (TcpBackend.cpp:376):
    - Enters `flush_delayed_to_clients()` (TcpBackend.cpp:280).
    - Stack-allocates `MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]` — 32 slots on the stack.
    - Calls `m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE)`.
    - **Inside `collect_deliverable()`** (ImpairmentEngine.cpp:216):
      - `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)`,
        `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)`.
      - Loops over `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]`.
      - For each slot: if `active == true` AND `release_us <= now_us`: copies to `out_buf`, marks
        inactive, decrements `m_delay_count`.
      - **At the moment of the initial `send_message()` call**: if `total_delay_us > 0`, neither the
        original nor the duplicate has matured (`release_us > now_us`). Returns `count = 0`.
    - For each collected envelope: `Serializer::serialize()` then `send_to_all_clients()`.
    - If nothing matured, the loop body does not execute.

11. **Both entries remain buffered** until a future call (from `send_message()` or the receive-side
    poll loop's `flush_delayed_to_queue()`) supplies a `now_us >= release_us` (original) and
    `now_us >= release_us + 100` (duplicate). The 100 µs offset is smaller than typical OS polling
    granularity (≥1 ms), so both are almost always collected in the same `collect_deliverable()` call.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::send_message(envelope)                       TcpBackend.cpp:347
├── NEVER_COMPILED_OUT_ASSERT(m_open)
├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))
├── Serializer::serialize(envelope, m_wire_buf, ...)
├── timestamp_now_us()
│   └── clock_gettime(CLOCK_MONOTONIC, &ts)
├── ImpairmentEngine::process_outbound(envelope, now_us) ImpairmentEngine.cpp:151
│   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
│   ├── [!m_cfg.enabled → queue_to_delay_buf; NOT taken]
│   ├── is_partition_active(now_us) → false
│   ├── check_loss()                                     ImpairmentEngine.cpp:110
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0)
│   │   └── PrngEngine::next_double()                   ← PRNG call #1 (loss check)
│   │       └── PrngEngine::next() [xorshift64]
│   │   → returns false (message survives)
│   ├── [jitter_mean_ms > 0U: optional]
│   │   └── PrngEngine::next_range(0U, jitter_variance_ms) ← PRNG call #2 (jitter; conditional)
│   │       └── PrngEngine::next() [xorshift64]
│   ├── [m_delay_count < IMPAIR_DELAY_BUF_SIZE]
│   ├── queue_to_delay_buf(in_env, release_us)           ImpairmentEngine.cpp:83
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE)
│   │   ├── [loop: find first inactive slot in m_delay_buf]
│   │   │   └── envelope_copy(m_delay_buf[i].env, in_env) ← PRIMARY COPY (memcpy)
│   │   │       m_delay_buf[i].release_us = release_us
│   │   │       m_delay_buf[i].active = true
│   │   │       ++m_delay_count
│   │   │       NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)
│   │   └── return Result::OK
│   ├── [m_cfg.duplication_probability > 0.0 → call apply_duplication]
│   ├── apply_duplication(in_env, release_us)            ImpairmentEngine.cpp:127
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
│   │   ├── NEVER_COMPILED_OUT_ASSERT(m_cfg.duplication_probability > 0.0)
│   │   ├── PrngEngine::next_double()                    ← PRNG call #N (dup check)
│   │   │   └── PrngEngine::next() [xorshift64]
│   │   ├── NEVER_COMPILED_OUT_ASSERT(dup_rand >= 0.0 && dup_rand < 1.0)
│   │   └── [dup_rand < duplication_probability]
│   │       ├── [m_delay_count < IMPAIR_DELAY_BUF_SIZE]
│   │       ├── queue_to_delay_buf(env, release_us + 100ULL) ImpairmentEngine.cpp:83
│   │       │   └── envelope_copy(m_delay_buf[j].env, env) ← DUPLICATE COPY (memcpy)
│   │       │       m_delay_buf[j].release_us = release_us + 100ULL  ← +100 MICROSECONDS
│   │       │       m_delay_buf[j].active = true
│   │       │       ++m_delay_count
│   │       └── [result_ok(res)] → Logger::log(WARNING_LO, "message duplicated")
│   ├── NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)
│   └── return Result::OK
├── [res != ERR_IO: not dropped]
├── [m_client_count > 0U]
├── send_to_all_clients(m_wire_buf, wire_len)            TcpBackend.cpp:258
│   └── [loop over m_client_fds] → tcp_send_frame(fd, buf, len, timeout_ms)
└── flush_delayed_to_clients(now_us)                     TcpBackend.cpp:280
    ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
    ├── NEVER_COMPILED_OUT_ASSERT(m_open)
    ├── [stack: MessageEnvelope delayed[IMPAIR_DELAY_BUF_SIZE]]  ← ~131 KB on stack
    ├── ImpairmentEngine::collect_deliverable(now_us, delayed, 32) ImpairmentEngine.cpp:216
    │   ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
    │   ├── NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)
    │   ├── NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)
    │   ├── [loop: scan m_delay_buf[0..31]]
    │   │   └── [active && release_us <= now_us] → envelope_copy to out_buf; active=false; --m_delay_count
    │   ├── NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap)
    │   ├── NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)
    │   └── return out_count  (0 if neither entry has matured yet)
    └── [for each matured envelope in delayed[]]
        ├── Serializer::serialize(delayed[i], m_wire_buf, ...)
        └── send_to_all_clients(m_wire_buf, delayed_len)
            └── tcp_send_frame(fd, buf, len, timeout_ms)
                ← ORIGINAL and/or DUPLICATE transmitted here when release_us matures
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `TcpBackend` | Class | `src/platform/TcpBackend.hpp/.cpp` | Owns `ImpairmentEngine`; orchestrates send and delayed delivery |
| `ImpairmentEngine` | Class | `src/platform/ImpairmentEngine.hpp/.cpp` | Applies duplication via `apply_duplication()`; manages `m_delay_buf` |
| `apply_duplication()` | Private method | `ImpairmentEngine.cpp:127` | Rolls PRNG; calls `queue_to_delay_buf()` with +100 µs offset on success |
| `queue_to_delay_buf()` | Private method | `ImpairmentEngine.cpp:83` | Finds free slot; copies envelope; sets `release_us`; increments `m_delay_count` |
| `collect_deliverable()` | Method | `ImpairmentEngine.cpp:216` | Releases matured entries from `m_delay_buf` to caller-supplied buffer |
| `flush_delayed_to_clients()` | Private method | `TcpBackend.cpp:280` | Calls `collect_deliverable()`; re-serializes; calls `send_to_all_clients()` |
| `send_to_all_clients()` | Private method | `TcpBackend.cpp:258` | Iterates `m_client_fds`; calls `tcp_send_frame()` for each active fd |
| `ImpairmentConfig` | POD struct | `src/platform/ImpairmentConfig.hpp` | Holds `duplication_probability` (double); default 0.0 |
| `PrngEngine` | Class | `src/platform/PrngEngine.hpp` | xorshift64; all methods inline; `next_double()` returns [0.0, 1.0) |
| `DelayEntry` | Nested struct | `ImpairmentEngine.hpp:155` | `{MessageEnvelope env, uint64_t release_us, bool active}` — one slot per buffered message |
| `envelope_copy()` | Inline function | `src/core/MessageEnvelope.hpp:56` | `memcpy(&dst, &src, sizeof(MessageEnvelope))` — used for both original and duplicate |
| `Logger::log()` | Static function | `src/core/Logger.hpp` | Records WARNING_LO "message duplicated" after successful `queue_to_delay_buf()` |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Master switch | ImpairmentEngine.cpp:159 | `!m_cfg.enabled` | If true: zero-delay buffer via `queue_to_delay_buf(in_env, now_us)`; no duplication check |
| Partition check | ImpairmentEngine.cpp:169 | `is_partition_active(now_us)` | If true: log WARNING_LO; return ERR_IO before duplication check |
| Loss survives | ImpairmentEngine.cpp:176 | `check_loss()` returns false | Message must survive loss to reach duplication check |
| Jitter enabled | ImpairmentEngine.cpp:185 | `m_cfg.jitter_mean_ms > 0U` | If true: extra PRNG call for jitter offset before duplication |
| Delay buffer full (primary) | ImpairmentEngine.cpp:192 | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | If true: log WARNING_HI; return ERR_FULL; duplication check not reached |
| `queue_to_delay_buf` result | ImpairmentEngine.cpp:198 | `result_ok(res)` | If false: return error; duplication check not reached |
| Duplication probability guard | ImpairmentEngine.cpp:203 | `m_cfg.duplication_probability > 0.0` | If false: `apply_duplication()` not called; PRNG not advanced for dup check |
| Duplication decision | `apply_duplication()` line 136 | `dup_rand < m_cfg.duplication_probability` | If true: second copy placed in delay buffer via `queue_to_delay_buf()` |
| Delay buffer full (duplicate) | `apply_duplication()` line 137 | `m_delay_count < IMPAIR_DELAY_BUF_SIZE` | If false: duplicate silently not added; no error returned |
| Logger fires | `apply_duplication()` line 139 | `result_ok(res)` after inner `queue_to_delay_buf` | Logger only fires when duplicate is actually buffered; not on failed inner capacity check |
| Release time check | ImpairmentEngine.cpp:229 | `active && release_us <= now_us` | Controls when original and duplicate are handed to transmission |
| Duplicate suppression (receiver) | DeliveryEngine.cpp (DedupFilter) | `(source_id, message_id)` match | Receiver's dedup filter detects and drops the second copy |

**Critical ordering**: The original is buffered first via `queue_to_delay_buf()`, then
`apply_duplication()` is called. If the buffer reaches capacity after inserting the primary,
the inner capacity check in `apply_duplication()` fails silently — no duplicate is added,
no error is returned, and the WARNING_LO log is not emitted.

---

## 7. Concurrency / Threading Behavior

- No locks or atomics in `process_outbound()`, `apply_duplication()`, `queue_to_delay_buf()`,
  or `collect_deliverable()`.
- `m_delay_buf` and `m_delay_count` are plain member variables. Both are modified in
  `queue_to_delay_buf()` (writes) and `collect_deliverable()` (reads and writes). Concurrent calls
  from multiple threads produce data races with undefined behavior.
- [ASSUMPTION] Single-threaded access to each `TcpBackend` / `ImpairmentEngine` instance is
  required for correctness.
- `m_prng.m_state` is advanced at least twice in this path (loss check + duplication check, inside
  `check_loss()` and `apply_duplication()` respectively), plus optionally a third time if jitter is
  enabled.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- Both the original and the duplicate are stored as full `MessageEnvelope` copies inside
  `DelayEntry::env` in `m_delay_buf`. Each copy is a full `memcpy` of `sizeof(MessageEnvelope)`
  bytes via `envelope_copy()`, which includes the fixed-size `payload[4096]` array regardless of
  `payload_length`. This is approximately 4119 bytes per slot copy.
- `m_delay_buf` is a statically-sized member array of `IMPAIR_DELAY_BUF_SIZE` (32) `DelayEntry`
  slots. With both original and duplicate buffered, two slots are consumed until
  `collect_deliverable()` reclaims them.
- `m_delay_count` tracks the number of active (occupied) slots. It is incremented twice in the
  duplication path (once per `queue_to_delay_buf()` call). Decremented by `collect_deliverable()`
  when each entry matures.
- `delayed[IMPAIR_DELAY_BUF_SIZE]` at `flush_delayed_to_clients()` (TcpBackend.cpp:285) is a
  local stack array of 32 `MessageEnvelope` objects. Stack frame size: 32 × ~4119 bytes
  ≈ 131 KB. Power of 10 rule 3 is satisfied (no heap allocation); however, the large stack
  frame is a known risk.
- `in_env` (the original caller's envelope reference) is never modified. Both copies are
  independent of the caller's object.
- Power of 10 rule 3: no `malloc` or `new` anywhere in this path.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| Serialization fails | TcpBackend.cpp:356 | Log WARNING_LO; return error to caller; impairment engine not entered |
| Partition active | ImpairmentEngine.cpp:169 | Log WARNING_LO; return ERR_IO; message and duplicate both dropped |
| Loss fires | ImpairmentEngine.cpp:176 | Log WARNING_LO; return ERR_IO; duplication check not reached |
| Primary delay buffer full | ImpairmentEngine.cpp:192 | Log WARNING_HI; return ERR_FULL; `apply_duplication()` not called |
| `queue_to_delay_buf` fails for primary | ImpairmentEngine.cpp:198 | Return error to `process_outbound()` caller; `apply_duplication()` not called |
| `apply_duplication()` inner capacity check fails | `apply_duplication()` line 137 | Duplicate silently not added; `apply_duplication()` returns void; `process_outbound()` still returns OK |
| `queue_to_delay_buf` fails for duplicate | `apply_duplication()` line 138 | `result_ok(res)` is false; WARNING_LO log is NOT emitted; silent suppression |
| `collect_deliverable` out_buf full | ImpairmentEngine.cpp:228 | Loop condition `out_count < buf_cap` exits early; remaining matured entries left in buffer until next call |

The silent suppression of the duplicate when the buffer is full means the actual duplication
rate may be lower than configured at high traffic levels, without any observable signal to the
caller.

---

## 10. External Interactions

| Interaction | Function | API |
|---|---|---|
| Monotonic clock | `timestamp_now_us()` | `clock_gettime(CLOCK_MONOTONIC, ...)` — POSIX |
| Event logging (duplicate fired) | `Logger::log()` | WARNING_LO "message duplicated" (only when `queue_to_delay_buf()` succeeds for duplicate) |
| Event logging (partition drop) | `Logger::log()` | WARNING_LO "message dropped (partition active)" |
| Event logging (loss drop) | `Logger::log()` | WARNING_LO "message dropped (loss probability)" |
| Event logging (buffer full) | `Logger::log()` | WARNING_HI "delay buffer full; dropping message" |
| Socket write (original) | `tcp_send_frame()` | POSIX `write`/`send` on TCP fd — when original `release_us` matures |
| Socket write (duplicate) | `tcp_send_frame()` | POSIX `write`/`send` on TCP fd — when `release_us + 100` matures |

---

## 11. State Changes / Side Effects

| State | Object | Change |
|---|---|---|
| `m_prng.m_state` | `PrngEngine` (inside `ImpairmentEngine`) | Advanced at least twice (inside `check_loss()` and `apply_duplication()`); three times if jitter enabled |
| `m_delay_buf[i]` | `ImpairmentEngine` | Slot i: original copy written via `queue_to_delay_buf()`; `active = true`; `release_us` set |
| `m_delay_buf[j]` | `ImpairmentEngine` | Slot j: duplicate copy written via `queue_to_delay_buf()`; `active = true`; `release_us = original + 100ULL` (microseconds) |
| `m_delay_count` | `ImpairmentEngine` | Incremented by 2 (once per `queue_to_delay_buf()` call) |
| Logger output | Process-level | WARNING_LO "message duplicated" emitted (only on successful duplicate insertion) |
| `m_wire_buf` | `TcpBackend` | Overwritten on each `flush_delayed_to_clients()` serialization iteration |

After `collect_deliverable()` processes both matured entries, `m_delay_count` decrements by 2
and both slots revert to `active = false`.

---

## 12. Sequence Diagram (ASCII)

```
Caller        TcpBackend            ImpairmentEngine   PrngEngine    Logger   send_to_all_clients
  |                |                       |                |            |            |
  |send_message(e) |                       |                |            |            |
  |--------------->|                       |                |            |            |
  |                | process_outbound()    |                |            |            |
  |                |---------------------->|                |            |            |
  |                |                       | check_loss()   |            |            |
  |                |                       | next_double()  |            |            |
  |                |                       |[loss check]--->|            |            |
  |                |                       |<-- 0.80 -------|            |            |
  |                |                       |[0.80 >= 0.05: survives]     |            |
  |                |                       |                |            |            |
  |                |                       | [compute release_us]        |            |
  |                |                       | queue_to_delay_buf(orig)    |            |
  |                |                       |   m_delay_buf[0].active=true|            |
  |                |                       |   m_delay_count=1           |            |
  |                |                       |                |            |            |
  |                |                       | apply_duplication()         |            |
  |                |                       | next_double()  |            |            |
  |                |                       |[dup check]---->|            |            |
  |                |                       |<-- 0.03 -------|            |            |
  |                |                       |[0.03 < 0.10: DUPLICATE]     |            |
  |                |                       | queue_to_delay_buf(dup+100µs)|           |
  |                |                       |   m_delay_buf[1].active=true|            |
  |                |                       |   release_us+100            |            |
  |                |                       |   m_delay_count=2           |            |
  |                |                       | log(WARNING_LO)             |            |
  |                |                       |------------------------------>            |
  |                |<--- Result::OK -------|                |            |            |
  |                |                       |                |            |            |
  |                | send_to_all_clients(wire_buf)          |            |            |
  |                |-------------------------------------------------------------->  |
  |                |   [original wire bytes transmitted immediately]                 |
  |                |                       |                |            |            |
  |                | flush_delayed_to_clients(now_us)       |            |            |
  |                | collect_deliverable(now_us)            |            |            |
  |                |---------------------->|                |            |            |
  |                |  [release_us > now_us: 0 ready]        |            |            |
  |                |<--- count=0 ----------|                |            |            |
  |<--- Result::OK-|                       |                |            |            |
  :                :                       :                :            :            :
  [... time passes: now_us >= release_us ...]
  :                :                       :                :            :            :
  |send_message(e2)|                       |                |            |            |
  |--------------->|                       |                |            |            |
  |                | flush_delayed_to_clients(now_us')      |            |            |
  |                | collect_deliverable(now_us')           |            |            |
  |                |---------------------->|                |            |            |
  |                |  [slot 0: active && release_us <= now_us']          |            |
  |                |<--- count=1 ----------|                |            |            |
  |                | Serializer::serialize(delayed[0])      |            |            |
  |                | send_to_all_clients(m_wire_buf)        |            |            |
  |                |-------------------------------------------------------------->  |
  |                |   [original delayed copy transmitted]                           |
  :                :                       :                :            :            :
  [... 100µs later: now_us >= release_us + 100 ...]
  :                :                       :                :            :            :
  |send_message(e3)|                       |                |            |            |
  |--------------->|                       |                |            |            |
  |                | flush_delayed_to_clients(now_us'')     |            |            |
  |                | collect_deliverable(now_us'')          |            |            |
  |                |---------------------->|                |            |            |
  |                |  [slot 1: active && release_us+100 <= now_us'']     |            |
  |                |<--- count=1 ----------|                |            |            |
  |                | Serializer::serialize(delayed[0])      |            |            |
  |                | send_to_all_clients(m_wire_buf)        |            |            |
  |                |-------------------------------------------------------------->  |
  |                |   [DUPLICATE transmitted]                                       |
```

Note: The sequence above assumes `fixed_latency_ms > 0`, so neither entry matures at call time.
If `fixed_latency_ms == 0` and `jitter_mean_ms == 0`, both entries have `release_us == now_us`
and both are collected by `collect_deliverable()` on the same `flush_delayed_to_clients()` call.

---

## 13. Initialization vs Runtime Flow

**Initialization phase**:

- `ImpairmentEngine::init(cfg)` (ImpairmentEngine.cpp:44) stores `m_cfg = cfg` and reseeds PRNG.
- `m_cfg.duplication_probability` defaults to `0.0` via `impairment_config_default()`
  (ImpairmentConfig.hpp). Must be set to a value `> 0.0` before `init()` for duplication to fire.
- If `cfg.prng_seed == 0ULL`, `init()` substitutes seed 42 (ImpairmentEngine.cpp:54).
- `m_delay_buf` and `m_delay_count` are zeroed; `m_initialized` is set to true.
- `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(m_delay_count == 0U)`
  confirm postconditions.

**Runtime phase**:

- First `send_message()` call: PRNG advanced inside `check_loss()` for loss check; optionally
  inside jitter calculation; then inside `apply_duplication()` for the duplication roll. Two
  `DelayEntry` slots occupied.
- Subsequent `send_message()` calls or the receive poll loop: `collect_deliverable()` scans for
  matured entries. Original and duplicate are released at different times (`release_us` and
  `release_us + 100`), so they may be collected on the same or different calls depending on
  polling frequency relative to 100 µs.
- If the polling interval is coarser than 100 µs (the `receive_message` poll uses 100 ms
  increments), both original and duplicate are virtually always collected in the same
  `collect_deliverable()` call.

---

## 14. Known Risks / Observations

1. **Silent duplicate suppression when buffer full**: If `m_delay_count == IMPAIR_DELAY_BUF_SIZE`
   after the primary is inserted (buffer now full at 32), the inner capacity check in
   `apply_duplication()` fails silently. No log, no error; the configured duplication rate is not
   achieved. The WARNING_LO "message duplicated" is also NOT emitted in this case (it is gated on
   `result_ok(res)` from the inner `queue_to_delay_buf()` call).

2. **100 µs fixed offset is a hardcoded constant**: The duplicate's `release_us + 100ULL` offset
   (`apply_duplication()` line 138) is **100 microseconds** — not 100 milliseconds. This is not
   part of `ImpairmentConfig` and cannot be configured. At polling granularities of ≥1 ms
   (typical), both copies arrive at virtually the same time from the receiver's perspective.

3. **Large stack frame in `flush_delayed_to_clients()`**: `delayed[IMPAIR_DELAY_BUF_SIZE]`
   (TcpBackend.cpp:285) is 32 full `MessageEnvelope` objects on the stack. Each envelope is
   approximately 4119 bytes (4096 payload + metadata fields), so approximately 131 KB of stack
   is consumed per call. No guard against stack overflow is present.

4. **PRNG call count depends on configuration**: The number of PRNG calls in `process_outbound()`
   is 1 (loss check, inside `check_loss()`) + 1 if jitter enabled + 1 (duplication check, inside
   `apply_duplication()`). Enabling or disabling jitter shifts the PRNG sequence seen by
   `apply_duplication()`, altering which messages are duplicated in a given test run even with
   the same seed.

5. **Duplicate carries identical `message_id`**: The duplicated `MessageEnvelope` is an exact
   `memcpy` of the original, including `message_id`. Receivers with a deduplication window
   (e.g., `DedupFilter` keyed on `(source_id, message_id)`) will detect and suppress the second
   copy. This is the intended behavior for testing deduplication logic.

6. **`apply_duplication()` has no postcondition assertion on `m_delay_count`**: The postcondition
   `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)` after
   `apply_duplication()` is in `process_outbound()` (line 208), not inside `apply_duplication()`
   itself. This means the count invariant is only verified at the `process_outbound()` level.

7. **`NEVER_COMPILED_OUT_ASSERT` is never stripped**: Unlike the standard C `assert()`, this
   macro is active in both debug and release builds. Violations terminate the program regardless
   of build configuration.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The receive-side deduplication mechanism (`DedupFilter` using a sliding window
  keyed on `(source_id, message_id)`) exists in `DeliveryEngine` and is what the duplication
  impairment is intended to exercise.
- [ASSUMPTION] Single-threaded access per `TcpBackend` / `ImpairmentEngine` instance. No
  thread-safety mechanism is present in these classes.
- [ASSUMPTION] The stack is large enough to hold `delayed[IMPAIR_DELAY_BUF_SIZE]` (~131 KB) in
  `flush_delayed_to_clients()`. No stack guard or overflow detection is visible.
- [UNKNOWN] Whether 100 µs is a meaningful or observable delay at the TCP transmission layer
  given OS scheduling jitter (typically ≥1 ms on non-RTOS platforms).
- [UNKNOWN] Whether `duplication_probability = 1.0` (all messages duplicated) has been tested
  for buffer-full conditions that would arise under sustained traffic.
- [UNKNOWN] The `Logger::log()` implementation's thread-safety and performance characteristics
  (e.g., whether it blocks, allocates, or is safe to call from signal handlers).

---
