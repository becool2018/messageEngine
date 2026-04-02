# UC_14 — Packet duplication: additional copy queued for delayed delivery

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.4, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` is called when `m_cfg.duplication_probability > 0.0` and the loss check did not drop the envelope. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Queue an extra copy of the outbound envelope alongside the original, simulating network packet duplication.
- **Success outcome:** `*out_count` is 2 (original + duplicate). Both appear in `out_buf` after `collect_deliverable()`. The backend sends both copies to the peer.
- **Error outcomes:** None — duplication is silent. If the delay buffer is full, the duplicate is silently discarded (but the original is still forwarded).

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp (called internally from process_outbound())
// Private helper: apply_duplication(env, now_us)
```

Not directly user-callable. Invoked inside `process_outbound()` after the loss check passes.

---

## 3. End-to-End Control Flow

1. `process_outbound()` passes the partition check and loss check (envelope not dropped).
2. **`apply_duplication(env, now_us)`** is called:
   a. `m_prng.next_double()` — draws random double `r` in `[0.0, 1.0)`.
   b. If `r < m_cfg.duplication_probability`:
      - **`queue_to_delay_buf(env, deliver_time)`** — places a duplicate `MessageEnvelope` copy into `m_delay_buf[m_delay_count]` with `deliver_time = now_us + latency_us`. If `m_delay_count >= IMPAIR_DELAY_BUF_SIZE`, the duplicate is silently discarded.
      - `m_delay_count++` if not full.
3. The original envelope is also placed in the delay buffer (or directly in `out_buf` if no latency configured).
4. `collect_deliverable(m_delay_buf, m_delay_count, now_us, out_buf, &out_count)` retrieves all entries whose `deliver_time <= now_us`. Both original and duplicate (with `deliver_time = now_us`) are immediately collectible.
5. Back in the backend: `out_count == 2`; loop sends both copies to peers.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound()              [ImpairmentEngine.cpp]
 ├── is_partition_active()
 ├── check_loss()
 ├── apply_duplication(env, now_us)
 │    ├── PrngEngine::next_double()               [PrngEngine.hpp]
 │    └── queue_to_delay_buf(env, deliver_time)   [duplicate copy]
 ├── queue_to_delay_buf(env, deliver_time)         [original copy]
 └── collect_deliverable(..., out_buf, &out_count)
```

---

## 5. Key Components Involved

- **`apply_duplication()`** — PRNG-driven decision; if triggered, queues a copy into the delay buffer.
- **`queue_to_delay_buf()`** — Appends an envelope (with `deliver_time`) to the circular delay buffer `m_delay_buf`. Silently discards if buffer full.
- **`collect_deliverable()`** — Transfers all due entries from the delay buffer into `out_buf`.
- **`PrngEngine`** — Provides the random draw for the duplication decision.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `r < duplication_probability` | Queue duplicate; `m_delay_count++` | No duplication |
| `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | Duplicate discarded silently | Duplicate queued |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the backend's calling thread.
- `PrngEngine::m_state` — plain `uint64_t`; not thread-safe.
- `m_delay_buf` — plain array; single-thread access assumed.

---

## 8. Memory & Ownership Semantics

- `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` — 32-slot array of `MessageEnvelope` inside `ImpairmentEngine`. Full copy of `env` stored per entry.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- No error states. If the delay buffer is full, only the duplicate is lost — the original is forwarded normally.
- `process_outbound()` always returns `Result::OK`.

---

## 10. External Interactions

- None — operates entirely in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S | xorshift64(S) |
| `ImpairmentEngine` | `m_delay_buf[m_delay_count]` | unused | duplicate envelope |
| `ImpairmentEngine` | `m_delay_count` | D | D+1 (if queued) |
| `*out_count` | 0 | 2 (both due immediately) |

---

## 12. Sequence Diagram

```
ImpairmentEngine::process_outbound(env, now_us, out_buf, &out_count)
  -> check_loss()                   <- false
  -> apply_duplication(env, now_us)
       -> PrngEngine::next_double() [r]
       [r < dup_prob]               <- true
       -> queue_to_delay_buf(env, now_us)   [duplicate; deliver_time = now_us]
  -> queue_to_delay_buf(env, now_us)         [original; deliver_time = now_us]
  -> collect_deliverable(now_us, out_buf, &out_count)
       [both entries deliver_time <= now_us; *out_count = 2]
  <- Result::OK
[Backend loop: send out_buf[0], out_buf[1] -> peer receives 2 copies]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_cfg.duplication_probability > 0.0` and `m_cfg.enabled == true`.
- PRNG seeded from `m_cfg.prng_seed`.

**Runtime:**
- Each `process_outbound()` call draws one additional random value for the duplication check, after the loss draw.

---

## 14. Known Risks / Observations

- **Receiver must handle duplicates:** The extra copy has the same `message_id`. For RELIABLE_RETRY receivers, `DuplicateFilter` suppresses the second delivery. For BEST_EFFORT, the application may receive the same message twice.
- **Delay buffer pressure:** Duplication at high rates can exhaust the 32-slot delay buffer. When full, further duplicates are silently discarded.
- **Both copies sent on same pump:** When no fixed latency is configured, `deliver_time = now_us` and both copies are collected immediately, resulting in back-to-back frames on the wire.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The duplicate and original share the same `deliver_time` (`now_us` plus any configured latency/jitter). This means they are delivered in the same `collect_deliverable()` call when no latency is configured.
