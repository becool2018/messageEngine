# UC_16 — Jitter: per-message delay drawn from uniform distribution around mean

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.2, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** A caller invokes `TcpBackend::send_message()` with a valid `MessageEnvelope`. The `ImpairmentConfig` has `enabled = true`, `jitter_mean_ms > 0`, and `jitter_variance_ms >= 0`. `loss_probability = 0.0` and no partition is active.

**Goal:** Each outbound message receives an additional random delay in microseconds, drawn uniformly from `[0, jitter_variance_ms]` milliseconds. The random offset is produced by `PrngEngine::next_range(0, jitter_variance_ms)`, which advances the deterministic xorshift64 PRNG state. The total release time is `now_us + base_delay_us + jitter_us`, where `base_delay_us` may be zero if `fixed_latency_ms == 0`. The envelope is then buffered in `m_delay_buf` as in UC_15 and released via `collect_deliverable()`.

**Success outcome:** `process_outbound()` returns `Result::OK`; one slot in `m_delay_buf` is occupied with `release_us = now_us + fixed_latency_ms*1000 + jitter_offset_ms*1000`; the PRNG state has advanced by one call to `next_range()`.

**Error outcomes:**
- `ERR_FULL` — `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` before queuing.
- `ERR_IO` — partition active or loss roll fired (not applicable in this UC).

---

## 2. Entry Points

```
// src/platform/ImpairmentEngine.hpp / ImpairmentEngine.cpp
Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env,
                                          uint64_t now_us);
```

Called from:

```
// src/platform/TcpBackend.cpp
Result TcpBackend::send_message(const MessageEnvelope& envelope);
```

Release path (same as UC_15):

```
void TcpBackend::flush_delayed_to_clients(uint64_t now_us);
uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us,
                                                MessageEnvelope* out_buf,
                                                uint32_t buf_cap);
```

PRNG method invoked:

```
// src/platform/PrngEngine.hpp
uint32_t PrngEngine::next_range(uint32_t lo, uint32_t hi);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. `TcpBackend::send_message(envelope)` is called.
2. Preconditions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `Serializer::serialize(envelope, m_wire_buf, SOCKET_RECV_BUF_BYTES, wire_len)` produces wire bytes. Returns `OK` or the function returns the error.
4. `timestamp_now_us()` → `now_us`.
5. `m_impairment.process_outbound(envelope, now_us)` is called.
6. Inside `process_outbound()`:
   a. Assertions: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
   b. `!m_cfg.enabled` is `false` — impairments are active.
   c. `is_partition_active(now_us)` — returns `false`.
   d. `check_loss()` — `m_cfg.loss_probability <= 0.0` → returns `false` immediately; PRNG not consumed.
   e. `base_delay_us = static_cast<uint64_t>(m_cfg.fixed_latency_ms) * 1000ULL`. (May be 0 if `fixed_latency_ms == 0`.)
   f. Jitter branch: `m_cfg.jitter_mean_ms > 0` is `true`.
      - `jitter_offset_ms = m_prng.next_range(0U, m_cfg.jitter_variance_ms)`.
      - Inside `next_range(lo=0, hi=jitter_variance_ms)`:
        i. `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)`.
        ii. `NEVER_COMPILED_OUT_ASSERT(hi >= lo)`.
        iii. `raw = next()` — advances xorshift64 state: `state ^= state << 13; state ^= state >> 7; state ^= state << 17`.
        iv. `range = jitter_variance_ms - 0 + 1`.
        v. `offset = static_cast<uint32_t>(raw % static_cast<uint64_t>(range))`.
        vi. `result = 0 + offset` — in `[0, jitter_variance_ms]`.
        vii. `NEVER_COMPILED_OUT_ASSERT(result >= lo && result <= hi)`.
        viii. Returns `result` (uint32_t milliseconds).
      - `jitter_us = static_cast<uint64_t>(jitter_offset_ms) * 1000ULL`.
   g. `release_us = now_us + base_delay_us + jitter_us`.
   h. Capacity guard: `m_delay_count < IMPAIR_DELAY_BUF_SIZE`.
   i. `queue_to_delay_buf(in_env, release_us)` — same logic as UC_15 Step 7.
7. Duplication check: `m_cfg.duplication_probability == 0.0` — skip.
8. `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`. Returns `Result::OK`.
9. Back in `TcpBackend::send_message()`: not `ERR_IO`; proceed.
10. `m_client_count > 0` check — assume connected.
11. `flush_delayed_to_clients(now_us)` — `collect_deliverable(now_us, ...)` scans delay buffer.
    - `release_us > now_us` (jitter added delay, not yet elapsed) → `count = 0`.
12. `send_message()` returns `Result::OK`.

**Release phase** (identical to UC_15 Phase C): on a subsequent `flush_delayed_to_clients()` call where `now_us >= release_us`, the slot is collected, re-serialized, and sent to all clients via `send_to_all_clients()`.

---

## 4. Call Tree

```
TcpBackend::send_message()
 ├── Serializer::serialize()
 ├── timestamp_now_us()
 ├── ImpairmentEngine::process_outbound()
 │    ├── ImpairmentEngine::is_partition_active()   [returns false]
 │    ├── ImpairmentEngine::check_loss()            [returns false; prob=0]
 │    ├── PrngEngine::next_range(0, jitter_var_ms)  [consumes PRNG state]
 │    │    └── PrngEngine::next()                   [xorshift64 step]
 │    └── ImpairmentEngine::queue_to_delay_buf()
 │         └── envelope_copy()                      [memcpy into delay slot]
 └── TcpBackend::flush_delayed_to_clients()
      └── ImpairmentEngine::collect_deliverable()   [0 on first call; N later]
           └── (count > 0)
                ├── Serializer::serialize()
                └── TcpBackend::send_to_all_clients()
                     └── ISocketOps::send_frame()
                          └── socket_send_all() → poll() + send()
```

---

## 5. Key Components Involved

- **`ImpairmentEngine`** (`src/platform/ImpairmentEngine.hpp/.cpp`): Same role as UC_15, with the addition that `m_prng.next_range()` is called in the jitter branch to compute the per-message random offset.

- **`PrngEngine`** (`src/platform/PrngEngine.hpp`): xorshift64 implementation. `next_range(lo, hi)` calls `next()` internally, advancing `m_state`. The state after this call is deterministic given the seed and call sequence. Key method for this UC.

- **`ImpairmentEngine::queue_to_delay_buf()`** (private): Stores envelope in first free `m_delay_buf` slot with the jitter-adjusted `release_us`.

- **`ImpairmentEngine::collect_deliverable()`**: Scans `m_delay_buf` for slots where `release_us <= now_us`. Because jitter adds a variable delay, different messages in the buffer may have different `release_us` values and may be released out of order relative to insertion order.

- **`TcpBackend`**: Owns `m_impairment` and drives the two-phase (enqueue then dequeue) logic.

- **`Serializer`**, **`ISocketOps`**: Same roles as UC_15.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!m_cfg.enabled` | Pass-through immediately | Proceed to all checks |
| `is_partition_active(now_us)` | Drop; return `ERR_IO` | Continue |
| `check_loss()` | Drop; return `ERR_IO` | Compute delays |
| `m_cfg.jitter_mean_ms > 0` | Call `prng.next_range(0, jitter_variance_ms)`; compute `jitter_us` | `jitter_us = 0` |
| `m_cfg.fixed_latency_ms > 0` | `base_delay_us = fixed_latency_ms * 1000` | `base_delay_us = 0` |
| `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | Log WARNING_HI; return `ERR_FULL` | Call `queue_to_delay_buf()` |
| `!m_delay_buf[i].active` in scan | Use slot | Skip |
| No free slot after full scan | `NEVER_COMPILED_OUT_ASSERT(false)` | (unreachable) |
| `m_cfg.duplication_probability > 0.0` | `apply_duplication()` — consumes another PRNG call | Skip |
| `release_us <= now_us` in `collect_deliverable` | Copy; free slot | Skip |
| Messages with different `release_us` in buffer | Each released independently when its own timestamp elapses | Held until its own deadline |

---

## 7. Concurrency / Threading Behavior

Identical to UC_15. No synchronization primitives inside `ImpairmentEngine` or `PrngEngine`. The xorshift64 `m_state` in `PrngEngine` is a plain `uint64_t` — not atomic. Sequential calls from a single thread produce a reproducible sequence. Concurrent calls from multiple threads without external locking produce a data race on `m_state` (undefined behavior).

[ASSUMPTION: single-threaded use per `TcpBackend` instance, or external locking applied by the caller.]

---

## 8. Memory & Ownership Semantics

Identical to UC_15 with one addition:

- `PrngEngine::m_state` — single `uint64_t` member of `ImpairmentEngine::m_prng`. Modified in place by `next_range()` → `next()`. No allocation; no ownership transfer.
- `jitter_offset_ms` — `uint32_t` local in `process_outbound()`. Lives on the stack, discarded at function return.
- `jitter_us` — `uint64_t` local in `process_outbound()`. Lives on the stack.

All other memory semantics are as in UC_15. Power of 10 Rule 3 confirmed: no dynamic allocation.

---

## 9. Error Handling Flow

Identical to UC_15, with one additional consideration:

| Error | Trigger | State after | Caller action |
|-------|---------|-------------|---------------|
| `ERR_FULL` | Delay buffer full | `m_prng.m_state` has already advanced (PRNG was consumed before the capacity check fires the buffer-full path) | `send_message()` propagates `ERR_FULL`; PRNG state is advanced even though the message was not queued |
| All others | Same as UC_15 | Same as UC_15 | Same as UC_15 |

Note: the PRNG call in step 6f (jitter computation) occurs before the capacity guard check at step 6h. If the capacity guard fires, the PRNG has already advanced. This means a retry after `ERR_FULL` will produce a different jitter value than the failed attempt.

---

## 10. External Interactions

Identical to UC_15. No POSIX calls occur during the buffering phase. During the release phase, `poll()` and `send()` are invoked per client fd as in UC_15 Section 10.

---

## 11. State Changes / Side Effects

In addition to the UC_15 state changes:

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` (inside `m_prng`) | seed-derived value S_n | S_{n+1} after one xorshift64 step (from `next_range()` → `next()`) |
| `ImpairmentEngine` | `m_delay_buf[i].release_us` | `0` or stale | `now_us + fixed_latency_ms*1000 + jitter_offset_ms*1000` (jitter is non-deterministic per call unless seed is fixed) |

All other state changes are as in UC_15.

---

## 12. Sequence Diagram

```
Caller            TcpBackend           ImpairmentEngine    PrngEngine
  |                    |                      |                 |
  |--send_message()--->|                      |                 |
  |                    |--serialize()-------->|                 |
  |                    |--timestamp_now_us()  |                 |
  |                    |--process_outbound()->|                 |
  |                    |                      |--is_partition?  |
  |                    |                      |  (false)        |
  |                    |                      |--check_loss()   |
  |                    |                      |  (false)        |
  |                    |                      |--jitter_mean>0  |
  |                    |                      |--next_range(0,  |
  |                    |                      |  var_ms)------->|
  |                    |                      |                 |--next()
  |                    |                      |                 |  xorshift64
  |                    |                      |<--jitter_ms-----|
  |                    |                      |--jitter_us =    |
  |                    |                      |  jitter_ms*1000 |
  |                    |                      |--release_us =   |
  |                    |                      |  now+base+jit   |
  |                    |                      |--queue_to_      |
  |                    |                      |  delay_buf()    |
  |                    |<-----OK--------------|                 |
  |                    |--flush_delayed_to_clients(now_us)      |
  |                    |  collect_deliverable → 0 (too early)  |
  |<---OK--------------|                      |                 |
  |                                           |                 |
  | ...time passes (base + jitter elapses)... |                 |
  |                                           |                 |
  |--send_message()--->| (or periodic flush)  |                 |
  |                    |--flush_delayed_to_clients(now_us2)     |
  |                    |  collect_deliverable → 1               |
  |                    |--serialize(delayed[0])                 |
  |                    |--send_to_all_clients()                 |
  |<---OK--------------|                      |                 |
```

---

## 13. Initialization vs Runtime Flow

**Initialization** (same as UC_15):

- `ImpairmentEngine::init(cfg)` seeds `m_prng` with `cfg.prng_seed` (or `42` if zero). The jitter path will produce the identical sequence of `jitter_offset_ms` values given the same seed.

**Steady-state runtime:**

- Every `send_message()` call consumes one PRNG step to generate the jitter offset. The PRNG state advances monotonically; there is no rewind or reset between calls.
- Messages queued with different jitter values may have different `release_us` timestamps and are therefore released by `collect_deliverable()` in deadline order, not insertion order. This naturally produces reordering even without the explicit reorder window.

---

## 14. Known Risks / Observations

- **PRNG advance before capacity guard.** `next_range()` is called in step 6f before the `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` guard is checked. If the buffer is full, the PRNG has already advanced but the message was not queued. Retrying will use a different jitter value, breaking reproducibility for that slot.

- **`IMPAIR_DELAY_BUF_SIZE = 32` limit applies.** High jitter variance combined with a fast send rate can fill the 32-slot delay buffer before any messages are released, causing `ERR_FULL`.

- **Out-of-order release.** Messages queued with large jitter values may be held while later messages with small jitter values are released first. This produces ordering effects that are independent of the explicit reorder window (`m_reorder_buf`). If the caller does not expect out-of-order delivery, this is a source of confusion.

- **Modulo bias in `next_range()`.** `raw % range` introduces a small statistical bias when `range` is not a power of 2. For impairment simulation this is acceptable, but the distribution is not perfectly uniform.

- **Jitter of 0 ms.** If `jitter_variance_ms == 0` but `jitter_mean_ms > 0`, `next_range(0, 0)` returns `0` deterministically, consuming one PRNG step but adding no delay. This is safe but wastes a PRNG call.

- **Large stack frame** in `flush_delayed_to_clients()`: same 132 KB concern as UC_15.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `jitter_mean_ms` is used only as a gate condition (`> 0` enables jitter). The actual jitter is drawn from `[0, jitter_variance_ms]` uniformly. The `mean` name suggests a Gaussian distribution, but the implementation uses a uniform distribution via `next_range(0, jitter_variance_ms)`. The mean of this distribution is `jitter_variance_ms / 2`, which may or may not equal `jitter_mean_ms`.
- [ASSUMPTION] The PRNG is consumed in the order: loss check (if enabled), jitter computation (if enabled), duplication check (if enabled). Changing the evaluation order would change the reproducible sequence for a given seed.
- [ASSUMPTION] Enabling both fixed latency and jitter results in `release_us = now_us + fixed_latency_ms*1000 + jitter_us`. The two delays are additive, not averaged.
- [ASSUMPTION] If both loss and jitter are enabled, the loss roll fires first. A message that passes the loss check will then have jitter applied. A dropped message does not consume a PRNG step for jitter.
- [ASSUMPTION] `timestamp_now_us()` wraps a monotonic POSIX clock. See UC_15 §15 for the same assumption.
- [ASSUMPTION] Only `config.channels[0U].impairment` is used by `TcpBackend`, as in UC_15.
