# UC_14 — Packet duplication: additional copy queued for delayed delivery

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.4, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

**Trigger:** `ImpairmentEngine::process_outbound(in_env, now_us)` is called; the message survives the loss and partition checks; `m_cfg.duplication_probability > 0.0`; and the PRNG roll for duplication falls below `duplication_probability`.

**Goal:** Queue an additional copy of the message in the delay buffer with a release time 100 µs after the original. Both the original and the duplicate will be returned to the caller on subsequent calls to `collect_deliverable()`.

**Success outcome:** The original message is already queued in `m_delay_buf` by `queue_to_delay_buf()` (the primary path). The duplicate is also queued via `apply_duplication()` at `release_us + 100 µs`. `m_delay_count` is incremented twice (once for the original, once for the duplicate). `process_outbound()` returns `Result::OK`.

**Error outcomes:**
- If the PRNG roll for duplication is `>= duplication_probability`: no duplicate is queued; `process_outbound()` returns `OK` with only the original in the buffer.
- If `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` at the time of the duplication attempt: the duplicate is silently dropped (no error is returned from `apply_duplication()`; the log entry is only emitted if `queue_to_delay_buf()` succeeds). The original is already queued.
- If `duplication_probability <= 0.0`: `apply_duplication()` is never called.
- If the primary `queue_to_delay_buf()` for the original fails (delay buffer full): `process_outbound()` returns `ERR_FULL` before `apply_duplication()` is reached; no duplication occurs.

---

## 2. Entry Points

**Primary entry:** Transport backend or test harness calls:
```
// src/platform/ImpairmentEngine.cpp, line 151
Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us)
```

The caller supplies:
- `in_env` — the message envelope to be transmitted.
- `now_us` — current monotonic time in microseconds.

`apply_duplication()` and `queue_to_delay_buf()` are private helpers; not externally callable.

---

## 3. End-to-End Control Flow

1. Transport backend calls `ImpairmentEngine::process_outbound(in_env, now_us)`.
2. Pre-condition asserts fire: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
3. Branch `!m_cfg.enabled` — impairments are enabled; continues.
4. `is_partition_active(now_us)` — returns `false`; no partition active.
5. `check_loss()` — returns `false`; message survives loss roll.
6. **Latency/jitter calculation:**
   - `base_delay_us = (uint64_t)m_cfg.fixed_latency_ms * 1000ULL`.
   - If `m_cfg.jitter_mean_ms > 0`: calls `m_prng.next_range(0U, m_cfg.jitter_variance_ms)` → `jitter_offset_ms`; `jitter_us = jitter_offset_ms * 1000ULL`. Otherwise `jitter_us = 0`.
   - `release_us = now_us + base_delay_us + jitter_us`.
7. **Buffer full check for original:**
   - If `m_delay_count >= IMPAIR_DELAY_BUF_SIZE`: logs `WARNING_HI` "delay buffer full"; returns `ERR_FULL`. Duplication not reached.
8. **Queue original:** calls `queue_to_delay_buf(in_env, release_us)`:
   - Pre-condition asserts: `m_initialized`, `m_delay_count < IMPAIR_DELAY_BUF_SIZE`.
   - Linear scan `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]` for first inactive slot.
   - On finding inactive slot: `envelope_copy(m_delay_buf[i].env, in_env)`, sets `release_us`, sets `active = true`, increments `m_delay_count`.
   - Post-condition assert: `m_delay_count <= IMPAIR_DELAY_BUF_SIZE`.
   - Returns `Result::OK`.
9. `result_ok(res)` is `true`; proceeds to duplication check.
10. Branch `m_cfg.duplication_probability > 0.0` — true; calls `apply_duplication(in_env, release_us)`:
    a. Pre-condition asserts: `NEVER_COMPILED_OUT_ASSERT(m_initialized)`, `NEVER_COMPILED_OUT_ASSERT(m_cfg.duplication_probability > 0.0)`.
    b. Calls `m_prng.next_double()`:
       - `PrngEngine::next_double()` fires `NEVER_COMPILED_OUT_ASSERT(m_state != 0)`.
       - Applies xorshift64 (`state ^= state << 13; state ^= state >> 7; state ^= state << 17`).
       - Converts raw `uint64_t` to `double` in `[0.0, 1.0)`.
       - Fires post-condition assert: `dup_rand >= 0.0 && dup_rand < 1.0`.
    c. Fires assert: `NEVER_COMPILED_OUT_ASSERT(dup_rand >= 0.0 && dup_rand < 1.0)`.
    d. Branch `dup_rand < m_cfg.duplication_probability` — **true** (duplication fires):
       - Branch `m_delay_count < IMPAIR_DELAY_BUF_SIZE` — space available:
         - Calls `queue_to_delay_buf(env, release_us + 100ULL)` — queues duplicate with `release_us` offset by +100 µs.
         - If `result_ok(res)`: logs `WARNING_LO` "message duplicated".
       - If `m_delay_count >= IMPAIR_DELAY_BUF_SIZE`: duplicate is silently dropped; no log.
    e. `apply_duplication()` returns `void`.
11. Post-condition assert: `NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)`.
12. `process_outbound()` returns `Result::OK`.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound(in_env, now_us)
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
 ├── !m_cfg.enabled => false
 ├── is_partition_active(now_us) => false
 ├── check_loss() => false                              [PRNG step #1 if loss_prob > 0]
 ├── [latency/jitter: release_us = now_us + base + jitter]
 │    └── m_prng.next_range() if jitter enabled         [PRNG step #2 if jitter enabled]
 ├── m_delay_count < IMPAIR_DELAY_BUF_SIZE
 ├── queue_to_delay_buf(in_env, release_us)             [original]
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_delay_count < IMPAIR_DELAY_BUF_SIZE)
 │    ├── [loop i=0..31] find inactive slot
 │    ├── envelope_copy(); release_us; active=true; ++m_delay_count
 │    └── return Result::OK
 ├── duplication_probability > 0.0 => true
 ├── apply_duplication(in_env, release_us)
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(dup_prob > 0.0)
 │    ├── m_prng.next_double()                          [PRNG step — duplication roll]
 │    │    ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0)
 │    │    ├── PrngEngine::next()                       [xorshift64]
 │    │    └── return raw / UINT64_MAX
 │    ├── NEVER_COMPILED_OUT_ASSERT(dup_rand in [0.0,1.0))
 │    ├── dup_rand < dup_prob => true
 │    ├── m_delay_count < IMPAIR_DELAY_BUF_SIZE
 │    └── queue_to_delay_buf(in_env, release_us + 100)  [duplicate, +100 µs]
 │         ├── envelope_copy(); active=true; ++m_delay_count
 │         └── return Result::OK
 │    └── Logger::log(WARNING_LO, "message duplicated")
 ├── NEVER_COMPILED_OUT_ASSERT(m_delay_count <= IMPAIR_DELAY_BUF_SIZE)
 └── return Result::OK
```

---

## 5. Key Components Involved

- **`ImpairmentEngine::process_outbound()`** — orchestrates the full outbound impairment pipeline; calls `apply_duplication()` only after the original is successfully queued.
- **`ImpairmentEngine::apply_duplication()`** — private helper; performs the PRNG roll, decides whether to duplicate, and calls `queue_to_delay_buf()` for the copy.
- **`ImpairmentEngine::queue_to_delay_buf()`** — private helper; finds the first inactive `DelayEntry` slot, copies the envelope, sets `release_us`, marks `active = true`, increments `m_delay_count`. Called twice in this UC: once for the original and once for the duplicate.
- **`PrngEngine::next_double()`** — advances `m_state` via xorshift64 and returns a `double` in `[0.0, 1.0)`. Shared with the loss roll.
- **`ImpairmentConfig::duplication_probability`** — the configurable threshold for duplication.
- **`DelayEntry::release_us`** — the wall-clock µs at which the entry becomes deliverable. For the duplicate, this is the original `release_us + 100 µs`.
- **`Logger::log()`** — emits `WARNING_LO` "message duplicated" when the duplicate is successfully queued.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `!m_cfg.enabled` | Zero-latency queue; return OK | Continue impairment checks | Partition check |
| `is_partition_active()` | Return `ERR_IO` (partition) | Continue to loss check | Loss check |
| `check_loss()` | Return `ERR_IO` (loss drop) | Continue to latency calc | Latency/jitter |
| `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` (before original) | Return `ERR_FULL` | Queue original | `apply_duplication()` |
| `queue_to_delay_buf()` for original returns non-OK | Return non-OK; no duplication | Proceed to duplication check | Duplication probability check |
| `duplication_probability > 0.0` | Call `apply_duplication()` | Skip duplication | Post-condition assert |
| `dup_rand < duplication_probability` | Attempt to queue duplicate | No duplicate | Return from `apply_duplication()` |
| `m_delay_count < IMPAIR_DELAY_BUF_SIZE` (before duplicate) | Call `queue_to_delay_buf()` for duplicate | Silently drop duplicate | Log if queued |
| `result_ok(queue_to_delay_buf())` for duplicate | Log `WARNING_LO` "message duplicated" | No log (failure not logged) | Return from `apply_duplication()` |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The transport backend or application thread calling `process_outbound()`. All work is in-line.
- **`m_prng.m_state`** — single `uint64_t`; advanced once per `next_double()` call. Not atomic.
- **`m_delay_count`** and **`m_delay_buf[i].active`** — non-atomic; modified twice in this UC (once for original, once for duplicate).
- **[ASSUMPTION]** Single-threaded access model. Concurrent calls from two threads would race on `m_prng.m_state`, `m_delay_count`, and `m_delay_buf`.
- **Determinism:** The duplication roll always consumes exactly one PRNG step from `next_double()`. In a session with the same seed and same call sequence, the same messages are duplicated.

---

## 8. Memory & Ownership Semantics

- **`ImpairmentEngine::m_delay_buf[IMPAIR_DELAY_BUF_SIZE]`** — fixed array of 32 `DelayEntry` structs, each containing a `MessageEnvelope` copy, `uint64_t release_us`, and `bool active`. Embedded in `ImpairmentEngine`. No heap allocation.
- **`envelope_copy()`** — `memcpy` of sizeof(MessageEnvelope) bytes. Called twice in this UC:
  - For the original: `m_delay_buf[i].env <- in_env`.
  - For the duplicate: `m_delay_buf[j].env <- in_env` (same source, different slot, different `release_us`).
- **`in_env`** — `const` reference; never modified on either the original or duplicate path.
- **Power of 10 Rule 3 confirmation:** No dynamic allocation. Both copies go into pre-allocated slots in `m_delay_buf`.
- **Slot ownership:** After `queue_to_delay_buf()`, the `DelayEntry` slot owns its copy of the envelope until `collect_deliverable()` frees it (sets `active = false`, decrements `m_delay_count`).

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `Result::OK` | Both original and duplicate queued (this UC success) | `m_delay_count` incremented by 2; two `DelayEntry` slots active | Normal; caller polls `collect_deliverable()` for both |
| `Result::OK` (no duplicate) | Duplicate PRNG roll failed | `m_delay_count` incremented by 1; one slot active | Normal |
| `Result::ERR_FULL` (buffer check before original) | `m_delay_count >= 32` before original | No slots allocated | Log `WARNING_HI`; caller drops message |
| Duplicate silently dropped (buffer full) | `m_delay_count >= 32` before duplicate | Original queued; `m_delay_count` incremented by 1 | `OK` returned; caller unaware duplicate was lost |
| `NEVER_COMPILED_OUT_ASSERT(false)` (logic error) | `queue_to_delay_buf()` finds no free slot despite count check | Abort (debug) / `on_fatal_assert()` (production) | N/A |

---

## 10. External Interactions

No OS calls, socket calls, or file I/O occur inside `process_outbound()`, `apply_duplication()`, or `queue_to_delay_buf()`. All state is in-process.

- `Logger::log()` — writes to the configured log sink ([ASSUMPTION] synchronous write).
- `m_prng.next_double()` — pure arithmetic on `m_state`.

The transport backend calls `process_outbound()` before emitting any socket I/O. Whether the I/O actually occurs depends on the return value of `process_outbound()` and the caller's use of `collect_deliverable()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `m_delay_buf[i].active` | `false` | `true` (original slot) |
| `m_delay_buf[i].env` | stale/zeroed | copy of `in_env` |
| `m_delay_buf[i].release_us` | 0 | `now_us + base_delay_us + jitter_us` |
| `m_delay_buf[j].active` | `false` | `true` (duplicate slot) |
| `m_delay_buf[j].env` | stale/zeroed | copy of `in_env` (identical to original) |
| `m_delay_buf[j].release_us` | 0 | `now_us + base_delay_us + jitter_us + 100` |
| `m_delay_count` | D | D+2 (if both queued) or D+1 (if duplicate dropped) |
| `PrngEngine::m_state` | S | xorshift64(S) — one step (for the duplication roll) |
| `Logger` | log output | — | `WARNING_LO` "message duplicated" (if duplicate queued) |

If a jitter roll also occurred earlier in the same `process_outbound()` call, `m_state` was already advanced by `next_range()` before the duplication roll.

---

## 12. Sequence Diagram

```
TransportBackend    ImpairmentEngine    queue_to_delay_buf()    apply_duplication()    PrngEngine    Logger
     |                    |                     |                       |                  |            |
     |--process_outbound(env,now)->              |                       |                  |            |
     |                    |--[partition,loss,latency checks pass]        |                  |            |
     |                    |--queue_to_delay_buf(env, release_us)-->      |                  |            |
     |                    |                     | envelope_copy()        |                  |            |
     |                    |                     | active=true            |                  |            |
     |                    |                     | ++m_delay_count        |                  |            |
     |                    |<--- OK -------------|                        |                  |            |
     |                    |                     |                        |                  |            |
     |                    |--apply_duplication(env, release_us)--------->|                  |            |
     |                    |                     |                        |--next_double()-->|            |
     |                    |                     |                        |<-- dup_rand -----|            |
     |                    |                     |                        | dup_rand < prob  |            |
     |                    |                     |                        |  => true         |            |
     |                    |                     |<--queue_to_delay_buf(env, release_us+100)-|            |
     |                    |                     | envelope_copy()        |                  |            |
     |                    |                     | active=true            |                  |            |
     |                    |                     | ++m_delay_count        |                  |            |
     |                    |                     |---> OK --------------->|                  |            |
     |                    |                     |                        |--log(WARNING_LO, "duplicated")->
     |<--- Result::OK -----|                     |                        |                  |            |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `ImpairmentEngine::init(cfg)` stores `cfg.duplication_probability` in `m_cfg`, zeroes `m_delay_buf` via `memset`, sets `m_delay_count = 0`, calls `m_prng.seed(cfg.prng_seed)`.
- The constructor calls `m_prng.seed(1ULL)` as initial safe state; `init()` always reseeds.

**Runtime (this UC):**
- `apply_duplication()` is called only when `duplication_probability > 0.0` and the primary `queue_to_delay_buf()` succeeded.
- The duplication PRNG roll always consumes exactly one step from `next_double()`, regardless of whether the roll results in a duplicate.
- PRNG stream ordering in a single `process_outbound()` call (if all impairments enabled): (1) loss roll via `next_double()`, (2) jitter roll via `next_range()`, (3) duplication roll via `next_double()`. The order is deterministic and fixed.

---

## 14. Known Risks / Observations

- **`IMPAIR_DELAY_BUF_SIZE = 32`:** With both original and duplicate, each message consumes two delay slots. At `duplication_probability = 1.0`, every message fills two slots; the effective buffer capacity is halved to 16 effective messages before `ERR_FULL` is returned.
- **Duplicate silently dropped when buffer is full:** If `m_delay_count == 31` (one slot remaining), the original fills the last slot; `m_delay_count` becomes 32; `apply_duplication()` then finds `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` and drops the duplicate silently. No error is returned and no log is emitted in this case (only logged if `queue_to_delay_buf()` succeeds). This makes full-buffer duplicate loss invisible at the `process_outbound()` return value level.
- **Duplicate release offset is fixed at 100 µs:** The offset `release_us + 100ULL` is hardcoded, not configurable. This is a minor design limitation that could cause all duplicates to arrive in a burst 100 µs after originals under zero-latency configuration.
- **PRNG stream coupling:** Loss and duplication share the same `PrngEngine` instance. If `loss_probability == 0.0` (check_loss fast-path returns without calling PRNG), the duplication roll consumes PRNG step N rather than step N+1. A test that sets `loss_probability` to a non-zero value and observes different duplication decisions is implicitly testing PRNG stream coupling.
- **`WARNING_LO` per duplicate under high probability:** At `duplication_probability = 0.9`, nearly every message logs `WARNING_LO`. This can fill the log sink under load.
- **Duplicate identity:** The duplicate envelope is byte-identical to the original (same `message_id`, `source_id`, `timestamp_us`). The receiver's `DuplicateFilter` will suppress it for `RELIABLE_RETRY` messages; for `BEST_EFFORT` or `RELIABLE_ACK` messages the duplicate passes through to the application.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `process_outbound()` is called from a single thread. Concurrent access races on `m_prng.m_state`, `m_delay_count`, and `m_delay_buf`.
- [ASSUMPTION] The 100 µs duplicate offset is intentional to simulate a duplicate arriving slightly after the original (e.g., from a route echo). It is not configurable.
- [ASSUMPTION] `duplication_probability` is in `[0.0, 1.0]` at all times. The `init()` function asserts `loss_probability` but the source does not show an explicit init-time assert for `duplication_probability`. [ASSUMPTION] The caller is expected to supply a valid value.
- [ASSUMPTION] The caller of `process_outbound()` polls `collect_deliverable()` to retrieve both the original and the duplicate. If `collect_deliverable()` is not called before the delay buffer fills up, future messages will be dropped with `ERR_FULL`.
- [ASSUMPTION] The PRNG stream ordering (loss → jitter → duplication) is stable across all call paths in `process_outbound()`. No code path reorders these rolls.
