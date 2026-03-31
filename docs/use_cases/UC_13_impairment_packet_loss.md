# UC_13 — Packet loss: outbound message dropped with configured probability

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.3, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1

---

## 1. Use Case Overview

**Trigger:** A transport backend (or `LocalSimHarness`) calls `ImpairmentEngine::process_outbound(in_env, now_us)` and the engine's PRNG roll falls below the configured `loss_probability`.

**Goal:** Drop the outbound message entirely — do not queue it in the delay buffer, do not deliver it. Return `Result::ERR_IO` to signal the caller that the message was lost.

**Success outcome (for the impairment):** The message is silently discarded. `Logger::log(WARNING_LO, "message dropped (loss probability)")` is emitted. `m_delay_count` is unchanged. The caller receives `Result::ERR_IO`.

**Error outcomes:**
- If `loss_probability <= 0.0`: `check_loss()` returns `false` immediately; no PRNG roll occurs and no drop happens.
- If `loss_probability >= 1.0`: `check_loss()` always calls the PRNG but the returned value is always `< 1.0` (by PRNG design), so every message is dropped.
- If `m_cfg.enabled == false`: the loss check is never reached; the message is queued with zero latency.
- If a partition is active: the partition drop fires first (`Result::ERR_IO` for partition) before the loss check is reached.

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

`check_loss()` is a private helper; it is not callable externally.

---

## 3. End-to-End Control Flow

1. Transport backend calls `ImpairmentEngine::process_outbound(in_env, now_us)`.
2. `process_outbound()` fires pre-condition asserts: `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
3. Branch: `!m_cfg.enabled` — if impairments are disabled, skip all checks and queue the message with zero latency (not this UC).
4. Branch: `is_partition_active(now_us)` — evaluates the partition state machine. If `partition_enabled == false` or partition is not active, returns `false`; execution continues.
5. **Loss check:** calls `check_loss()`:
   a. `check_loss()` fires `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(m_cfg.loss_probability <= 1.0)`.
   b. If `m_cfg.loss_probability <= 0.0`: returns `false` immediately. Loss check passed; not this UC.
   c. Calls `m_prng.next_double()`:
      - `PrngEngine::next_double()` fires `NEVER_COMPILED_OUT_ASSERT(m_state != 0)`.
      - Calls `next()`: applies xorshift64 (`state ^= state << 13; state ^= state >> 7; state ^= state << 17`). Updates `m_state` in-place.
      - Divides raw `uint64_t` by `UINT64_MAX` to obtain `double` in `[0.0, 1.0)`.
      - Fires post-condition assert: `result >= 0.0 && result < 1.0`.
      - Returns `rand_val`.
   d. `check_loss()` fires `NEVER_COMPILED_OUT_ASSERT(rand_val >= 0.0 && rand_val < 1.0)`.
   e. Evaluates `rand_val < m_cfg.loss_probability`.
      - If `true`: **message is dropped**; returns `true`.
      - If `false`: message survives; returns `false` (not this UC).
6. `process_outbound()` receives `true` from `check_loss()`.
7. Logs `WARNING_LO` "message dropped (loss probability)".
8. Returns `Result::ERR_IO` to the caller.
9. **`m_delay_count` is not modified.** The message is gone.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound(in_env, now_us)
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))
 ├── !m_cfg.enabled => false (impairments enabled)
 ├── is_partition_active(now_us) => false (no partition)
 ├── check_loss()
 │    ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 │    ├── NEVER_COMPILED_OUT_ASSERT(loss_probability <= 1.0)
 │    ├── loss_probability <= 0.0 => false
 │    ├── m_prng.next_double()
 │    │    ├── NEVER_COMPILED_OUT_ASSERT(m_state != 0)
 │    │    ├── PrngEngine::next()          [xorshift64: updates m_state]
 │    │    ├── result = raw / UINT64_MAX
 │    │    └── NEVER_COMPILED_OUT_ASSERT(result in [0.0, 1.0))
 │    ├── NEVER_COMPILED_OUT_ASSERT(rand_val in [0.0, 1.0))
 │    └── return (rand_val < loss_probability) => true
 ├── Logger::log(WARNING_LO, "message dropped (loss probability)")
 └── return Result::ERR_IO
```

---

## 5. Key Components Involved

- **`ImpairmentEngine::process_outbound()`** — orchestrates all outbound impairments; the loss check is one of its guarded branches.
- **`ImpairmentEngine::check_loss()`** — private helper; encapsulates the PRNG roll and probability comparison. Ensures `loss_probability == 0` short-circuits without touching the PRNG state.
- **`PrngEngine::next_double()`** — xorshift64-based PRNG advancing `m_state` by one step; produces a double in `[0.0, 1.0)`.
- **`PrngEngine::next()`** — the raw xorshift64 update; called by `next_double()`.
- **`ImpairmentConfig::loss_probability`** — the configurable threshold; values in `[0.0, 1.0]`. Set at `init()` time.
- **`Logger::log()`** — emits `WARNING_LO` to signal the drop event.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `!m_cfg.enabled` | Queue with `release_us = now_us` (no impairments) | Continue to partition check | Partition check |
| `is_partition_active(now_us)` | Return `ERR_IO` (partition drop) | Continue to `check_loss()` | `check_loss()` |
| `loss_probability <= 0.0` (inside `check_loss()`) | Return `false` (no loss) | Call PRNG | PRNG roll |
| `rand_val < loss_probability` | Return `true` (drop) | Return `false` (survive) | Caller checks return |
| `check_loss()` returns `true` (in `process_outbound()`) | Log; return `ERR_IO` | Proceed to latency/jitter/queuing path | — |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The transport backend thread (or application thread if using `LocalSimHarness`) calling `process_outbound()`. All work is in-line.
- **`m_prng.m_state`** — a single `uint64_t` inside `PrngEngine`; modified by every `next()` call. Not protected by any mutex or atomic.
- **[ASSUMPTION]** Single-threaded access. Concurrent calls to `process_outbound()` from two threads would create a data race on `m_state` and `m_delay_count`.
- **Determinism:** Given the same PRNG seed and the same call sequence, `next_double()` always returns the same value. This is the mechanism for reproducible impairment decisions (REQ-5.3.1).

---

## 8. Memory & Ownership Semantics

- **`in_env`** — passed by `const` reference; `check_loss()` and `process_outbound()` do not modify it. On a drop, `in_env` is simply not copied anywhere; no buffer slot is consumed.
- **`ImpairmentEngine::m_delay_buf[IMPAIR_DELAY_BUF_SIZE]`** — not touched on the loss drop path.
- **`PrngEngine::m_state`** — single `uint64_t` member; modified in-place by `next()`. Owned by `ImpairmentEngine::m_prng`.
- **Power of 10 Rule 3 confirmation:** No dynamic allocation on this path. `check_loss()` and `next_double()` operate entirely on pre-allocated state.

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `Result::ERR_IO` (loss drop) | `rand_val < loss_probability` | `m_prng.m_state` advanced by one step; all other state unchanged | Caller treats message as undeliverable; may log; `RELIABLE_RETRY` senders will retry via `pump_retries()` |
| `Result::ERR_IO` (partition drop) | `is_partition_active()` returns `true` | `m_prng.m_state` unchanged (partition check precedes loss check); partition state may advance | Same as loss |
| `Result::ERR_FULL` | `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` (reached only if loss check passes) | Not applicable to this UC | Log `WARNING_HI`; caller drops message |
| `Result::OK` | Loss check passes; message queued | `m_delay_count` incremented; delay slot allocated | Not this UC |

---

## 10. External Interactions

No OS calls, socket calls, or file I/O occur on the loss-drop path. The entire path is in-process computation:
- `m_prng.next_double()` — pure arithmetic on `m_state`.
- `Logger::log()` — writes to configured log sink ([ASSUMPTION] synchronous write to stderr or a buffer).

The caller of `process_outbound()` is typically a transport backend such as `UdpBackend`, which itself calls `sendto(2)` — but only after `process_outbound()` returns `OK` (i.e., not on the drop path).

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine::m_state` | S | S | xorshift64(S) — advanced by one step |
| `ImpairmentEngine::m_delay_buf` | — | unchanged | unchanged (no slot allocated) |
| `ImpairmentEngine::m_delay_count` | D | D | D (unchanged) |
| `Logger` | log output | — | `WARNING_LO` "message dropped (loss probability)" |

The dropped message is gone. Nothing in `AckTracker` or `RetryManager` is touched by `ImpairmentEngine`; those components live one layer above in `DeliveryEngine`.

---

## 12. Sequence Diagram

```
TransportBackend    ImpairmentEngine    check_loss()     PrngEngine::next_double()    Logger
     |                    |                 |                       |                    |
     |--process_outbound(env,now)->         |                       |                    |
     |                    |--!enabled?-->false                      |                    |
     |                    |--is_partition_active()-->false          |                    |
     |                    |--check_loss()-->|                       |                    |
     |                    |                |--loss_prob <= 0? false |                    |
     |                    |                |--m_prng.next_double()-->                   |
     |                    |                |                        | xorshift64(state)  |
     |                    |                |                        | raw/UINT64_MAX     |
     |                    |                |<--- rand_val ----------|                    |
     |                    |                | rand_val < prob => true|                    |
     |                    |<--- true -------|                       |                    |
     |                    |--Logger::log(WARNING_LO, "dropped")------>                  |
     |<-- ERR_IO ---------|                 |                       |                    |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `ImpairmentEngine::init(cfg)` stores `cfg.loss_probability` in `m_cfg.loss_probability`, asserts it is in `[0.0, 1.0]`, and calls `m_prng.seed(cfg.prng_seed)` (or default seed 42).
- The constructor also calls `m_prng.seed(1ULL)` as a safe initial state; `init()` always reseeds.

**Runtime (this UC):**
- On each call to `process_outbound()`, the PRNG advances exactly once via `next_double()` (or zero times if `loss_probability <= 0.0`).
- The loss decision is consumed from the PRNG stream before the duplication roll. Order within the stream is: loss → (latency/jitter, then) duplication.
- The PRNG state is cumulative across all messages processed in a session. Replay requires the same seed and the same sequence of `process_outbound()` calls.

---

## 14. Known Risks / Observations

- **PRNG consumed on every non-zero loss check:** Even when the loss check passes (message survives), `next_double()` advances `m_state`. The PRNG stream is shared with duplication (also calls `next_double()`). If both are enabled, each message consumes two PRNG steps.
- **`loss_probability == 1.0` edge case:** The PRNG returns values in `[0.0, 1.0)` — strictly less than 1.0. So `rand_val < 1.0` is always true, and 100% loss is achieved for any `loss_probability >= 1.0`. Setting `loss_probability = 1.0` behaves as total loss.
- **Impairment order:** Partition check fires before loss check. A partition in effect will never consume a PRNG step for loss; the PRNG is only advanced when partition is inactive.
- **`IMPAIR_DELAY_BUF_SIZE = 32`:** Irrelevant to the loss-drop path (no slot is allocated), but worth noting for the survival path: if the delay buffer is full when a message survives the loss check, it is also dropped with `ERR_FULL`.
- **`WARNING_LO` log volume:** With high `loss_probability`, every dropped message emits a `WARNING_LO` log line. Under load this can flood the log sink.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `process_outbound()` is called from a single thread. Concurrent access would race on `m_prng.m_state`.
- [ASSUMPTION] The caller checks the `Result` return value of `process_outbound()` and does not attempt further delivery when `ERR_IO` is returned.
- [ASSUMPTION] `loss_probability` is in `[0.0, 1.0]` after `init()`. The init-time assert enforces this, but runtime mutation of `m_cfg` (if any) is not guarded.
- [ASSUMPTION] The PRNG seed is set once at `init()` time and is not changed during a session. Reseeding mid-session would break deterministic replay.
- [ASSUMPTION] The xorshift64 algorithm's output distribution is uniform enough for packet-loss simulation purposes. No statistical bias correction is applied.
