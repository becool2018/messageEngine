# UC_13 — Packet loss: outbound message dropped with configured probability

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.3, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` is called by any backend's `send_message()` when `m_cfg.enabled == true` and `m_cfg.loss_probability > 0.0`. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Drop the outbound message with the configured probability, preventing it from reaching the transport write layer.
- **Success outcome (dropped):** `process_outbound()` returns `Result::ERR_IO`; the backend skips the subsequent `collect_deliverable()` call (or it returns 0). `send_to_all_clients()` / `sendto()` is never called.
- **Success outcome (not dropped):** `process_outbound()` returns `Result::OK`; the envelope is queued in the internal delay buffer. `collect_deliverable()` extracts due envelopes; the backend sends each one.
- **Error outcomes:** None — `process_outbound()` always returns `Result::OK`; loss is a silent drop.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp (called from backend send_message())
Result ImpairmentEngine::process_outbound(const MessageEnvelope& in_env, uint64_t now_us);

// Called after process_outbound() to retrieve due envelopes from the delay buffer:
uint32_t ImpairmentEngine::collect_deliverable(uint64_t now_us,
    MessageEnvelope* out_buf, uint32_t buf_cap);
```

Called internally by `TcpBackend::send_message()`, `UdpBackend::send_message()`, `LocalSimHarness::send_message()`, `TlsTcpBackend::send_message()`, `DtlsUdpBackend::send_message()`.

---

## 3. End-to-End Control Flow

1. Backend `send_message()` calls `m_impairment.process_outbound(envelope, now_us)`.
2. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
3. **Partition check:** `is_partition_active(now_us)` — if true, `++m_stats.partition_drops`, log `WARNING_LO`, return `ERR_IO`. For this UC: partition is inactive.
4. **Loss check:** `check_loss()` is called:
   a. `m_prng.next_double()` — xorshift64 produces a uniform double in `[0.0, 1.0)`.
   b. If `random_value < m_cfg.loss_probability`: return `true` (drop).
   c. Otherwise: return `false` (forward).
5. If `check_loss()` returns true:
   - `++m_stats.loss_drops`.
   - `Logger::log(WARNING_LO, "ImpairmentEngine", "message dropped (loss probability)")`.
   - Returns `Result::ERR_IO`. The envelope is NOT placed in the delay buffer.
6. If not lost: compute `release_us` (latency + jitter), call `queue_to_delay_buf(in_env, release_us)`. Apply duplication (UC_14) if configured. Returns `Result::OK`.
7. Back in backend `send_message()`: `process_outbound()` returned `ERR_IO`, so the backend skips `collect_deliverable()` (or calls it and gets 0 due entries). No socket write occurs for the dropped message.

---

## 4. Call Tree

```
TcpBackend::send_message()                            [TcpBackend.cpp]
 ├── ImpairmentEngine::process_outbound(env, now_us)  [ImpairmentEngine.cpp]
 │    ├── is_partition_active(now_us)
 │    ├── check_loss()
 │    │    └── PrngEngine::next_double()               [PrngEngine.hpp]
 │    └── [if not lost: queue_to_delay_buf; apply_duplication]
 └── ImpairmentEngine::collect_deliverable(now_us, out_buf, cap)  [if process_outbound OK]
```

---

## 5. Key Components Involved

- **`ImpairmentEngine::process_outbound()`** — Applies all configured outbound impairments: partition, loss, duplication, latency/jitter. For this UC, only loss is active.
- **`PrngEngine::next_double()`** — xorshift64 produces a pseudo-random double in `[0.0, 1.0)`. The PRNG state is `m_prng` inside `ImpairmentEngine`, seeded from `m_cfg.prng_seed`.
- **`check_loss()`** — Compares the random double against `m_cfg.loss_probability`. Returns true when the random draw falls below the threshold.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!m_cfg.enabled` | Pass through to delay buf immediately | Apply impairments |
| `is_partition_active(now_us)` | Return `ERR_IO` (partition drop); UC_18 | Check loss |
| `random < loss_probability` | Return `ERR_IO` (loss drop) | Queue to delay buf |
| `loss_probability == 0.0` | Never drops (check not entered) | Normal |
| `loss_probability == 1.0` | Always drops (UC_32) | Normal |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the backend's calling thread (the thread calling `send_message()`).
- `PrngEngine::m_state` — plain `uint64_t`; modified by xorshift64 each call. Not thread-safe.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `out_buf` — caller-provided stack array (in `TcpBackend::send_message()`: `delay_buf[IMPAIR_DELAY_BUF_SIZE]`, 32 slots). Not allocated here.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- No error states. Loss is a silent drop; the backend returns `Result::OK` from `send_message()` regardless.
- The caller (`DeliveryEngine::send()`) receives `Result::OK` even when the message was dropped. This is by design for impairment simulation.

---

## 10. External Interactions

- None — this flow operates entirely in process memory.
- `Logger::log()` writes to `stderr` when loss occurs.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S | xorshift64(S) (new state) |
| `out_count` | value | 0 | 0 (dropped) or 1 (forwarded) |

---

## 12. Sequence Diagram

```
TcpBackend::send_message()
  -> ImpairmentEngine::process_outbound(env, now_us)
       -> is_partition_active()              <- false
       -> check_loss()
            -> PrngEngine::next_double()     [xorshift64; returns r in [0,1)]
            [r < loss_probability]           <- true (drop)
       <- Result::ERR_IO (dropped; not queued)
  [ERR_IO: collect_deliverable not called; no ::send() occurs]
  <- Result::OK  [backend returns OK; loss is silent to the caller]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `ImpairmentEngine` configured with `m_cfg.enabled = true`, `m_cfg.loss_probability > 0.0`.
- `PrngEngine::seed(m_cfg.prng_seed)` called during backend `init()`.

**Runtime:**
- `check_loss()` is called on every outbound message when loss is configured. Each call advances the PRNG state by one step.

---

## 14. Known Risks / Observations

- **Silent drop:** `send_message()` returns `OK` even for dropped messages. `DeliveryEngine::send()` has no indication of the drop.
- **PRNG determinism:** With the same seed and identical message sequence, every drop/forward decision is reproducible (UC_29, UC_31).
- **`loss_probability == 1.0` drops all:** See UC_32. All outbound traffic is dropped; no socket writes occur.
- **PRNG not thread-safe:** If send_message() is called from multiple threads, concurrent `next_double()` calls race on `m_state`.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `check_loss()` is a private helper inside `ImpairmentEngine` that calls `m_prng.next_double()` and compares against `m_cfg.loss_probability`. The exact function name is inferred from `ImpairmentEngine.cpp`; confirmed by the test `test_ImpairmentEngine.cpp`.
