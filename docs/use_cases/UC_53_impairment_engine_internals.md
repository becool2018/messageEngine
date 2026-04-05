# UC_53 — ImpairmentEngine process_outbound/collect_deliverable/process_inbound: called internally by each backend on every send/receive

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-5.1.1, REQ-5.1.2, REQ-5.1.3, REQ-5.1.4, REQ-5.1.5, REQ-5.1.6, REQ-5.3.1

This is a System Internal use case. These three methods are called by backends on every send/receive operation to apply the configured impairments. The User never calls them directly; they are activated via `ImpairmentEngine::init(config)`.

---

## 1. Use Case Overview

- **Trigger:** A backend calls `m_impairment.process_outbound(env, now_us)` on outbound send, or `m_impairment.process_inbound(env)` on inbound receive, or `m_impairment.collect_deliverable(now_us, out_env)` to release delayed messages.
- **Goal:** Apply the full configured impairment pipeline to each message: loss, duplication, latency/jitter delay queueing, and partition/reorder decisions.
- **Success outcome (process_outbound):** Returns `ImpairmentResult` indicating whether the message should be forwarded (`SEND`), dropped (`DROP`), or delayed (`DELAYED`). For `DELAYED`, the message is stored in the delay buffer for later release.
- **Success outcome (collect_deliverable):** Returns all delay-buffer entries whose `deliver_time_us <= now_us`; zero or more envelopes released.
- **Success outcome (process_inbound):** Returns `ImpairmentResult` for the inbound path (reorder, partition).
- **Error outcomes:** None returned. Buffer-full conditions logged at WARNING_LO.

---

## 2. Entry Points

```cpp
// src/core/ImpairmentEngine.cpp
ImpairmentResult ImpairmentEngine::process_outbound(const MessageEnvelope& env,
                                                    uint64_t now_us);
bool ImpairmentEngine::collect_deliverable(uint64_t now_us,
                                           MessageEnvelope* out_env);
ImpairmentResult ImpairmentEngine::process_inbound(const MessageEnvelope& env);
```

---

## 3. End-to-End Control Flow

**`process_outbound(env, now_us)`:**
1. **Partition check:** `is_partition_active(now_us)` — if true, return `DROP` (UC_18).
2. **Loss check:** `check_loss()` — `m_prng.next_double() < m_cfg.loss_probability` → if true, return `DROP` (UC_13).
3. **Duplication check:** `apply_duplication()` — if `m_prng.next_double() < m_cfg.duplication_probability`: `queue_to_delay_buf(env, now_us, 0)` — a copy queued with zero extra delay (UC_14).
4. **Delay computation:**
   - If `latency_ms > 0`: `deliver_us = now_us + latency_ms * 1000`.
   - If `jitter_mean_ms > 0`: `deliver_us += m_prng.next_range(-jitter_range, +jitter_range)` (UC_16).
   - If no delay: return `SEND`.
5. **`queue_to_delay_buf(env, now_us, deliver_us)`** (UC_15): stores in `m_delay_buf[IMPAIR_DELAY_BUF_SIZE=32]`.
   - If buffer full: log WARNING_LO; return `DROP`.
6. Return `DELAYED`.

**`collect_deliverable(now_us, out_env)`:**
1. Scan `m_delay_buf[0..IMPAIR_DELAY_BUF_SIZE-1]` (bounded loop).
2. For each occupied slot: if `deliver_time_us <= now_us`: copy to `*out_env`; mark slot free; return `true`.
3. If no due entry: return `false`.

**`process_inbound(env)`:**
1. **Partition check:** `is_partition_active(timestamp_now_us())` — if true, return `DROP`.
2. **Reorder check:** if reorder enabled, buffer message and release according to reorder policy (UC_17).
3. Return `DELIVER` or `BUFFERED`.

---

## 4. Call Tree

```
ImpairmentEngine::process_outbound(env, now_us)   [ImpairmentEngine.cpp]
 ├── is_partition_active(now_us)                  [partition check]
 ├── PrngEngine::next_double()                    [loss check]
 ├── PrngEngine::next_double()                    [duplication check]
 ├── queue_to_delay_buf()                         [duplication copy]
 ├── PrngEngine::next_range()                     [jitter draw]
 └── queue_to_delay_buf(env, deliver_us)          [latency/jitter queue]

ImpairmentEngine::collect_deliverable(now_us)     [ImpairmentEngine.cpp]
 └── [scan m_delay_buf; release due entries]

ImpairmentEngine::process_inbound(env)            [ImpairmentEngine.cpp]
 ├── is_partition_active()
 └── [reorder buffer logic]
```

---

## 5. Key Components Involved

- **`is_partition_active()`** — modulo-cycle partition check; drops all traffic during partition windows (UC_18).
- **`PrngEngine::next_double()`** — draws a value in `[0.0, 1.0)` for loss and duplication probability comparisons (UC_54).
- **`PrngEngine::next_range()`** — draws a uniform integer for jitter delay (UC_54).
- **`m_delay_buf[IMPAIR_DELAY_BUF_SIZE=32]`** — fixed array of delay slots; bounded by Power of 10 Rule 3.
- **`queue_to_delay_buf()`** — stores an envelope with an absolute `deliver_time_us` in the first free delay slot.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `is_partition_active()` | Drop | Continue |
| `next_double() < loss_probability` | Drop | Continue |
| `next_double() < duplication_probability` | Queue duplicate copy | Skip duplication |
| `latency_ms > 0 or jitter > 0` | Queue to delay buf | Return SEND immediately |
| Delay buf full | Log WARNING_LO; Drop | Queue successfully; return DELAYED |
| `deliver_time_us <= now_us` | Release from delay buf | Skip (not yet due) |

---

## 7. Concurrency / Threading Behavior

- Called from the backend's send/receive thread; single-threaded per call.
- `m_delay_buf`, `m_prng` are plain members; not thread-safe.
- No `std::atomic` operations in ImpairmentEngine.

---

## 8. Memory & Ownership Semantics

- `m_delay_buf[IMPAIR_DELAY_BUF_SIZE=32]` — fixed array; each slot holds one `MessageEnvelope` (4144 bytes) plus a `deliver_time_us` field; total ≈ 32 × 4148 bytes ≈ 133 KB static.
- No heap allocation. Power of 10 Rule 3 compliant.
- `m_prng` — `PrngEngine` member; no dynamic state.

---

## 9. Error Handling Flow

- **Delay buf full (SEND path):** Message is dropped (returns DROP); WARNING_LO logged. This is a capacity constraint; the caller sees SEND → drops message; the application does not receive it.
- **No other error states.** Loss, drop, and delay are normal impairment outcomes.

---

## 10. External Interactions

- None — operates entirely in process memory. No socket or file I/O.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_delay_buf[slot]` | free | occupied with `(env, deliver_time_us)` |
| `ImpairmentEngine` | `m_prng` | state S | state S' (xorshift64 advanced) |
| `ImpairmentEngine` | `m_partition_start_us` | — | updated when partition cycle changes |

---

## 12. Sequence Diagram

```
[TcpBackend::send_message(env)]
  -> ImpairmentEngine::process_outbound(env, now_us)
       -> is_partition_active(now_us)    <- false
       -> PrngEngine::next_double()      <- 0.3  [< 0.5 loss_prob -> DROP]
       <- ImpairmentResult::DROP
  [message not sent]

[or with no impairment]
  -> ImpairmentEngine::process_outbound(env, now_us)
       <- ImpairmentResult::SEND
  -> ::send(fd, wire_buf, len)           [message transmitted]

[TcpBackend::receive_message() poll loop]
  -> ImpairmentEngine::collect_deliverable(now_us, &env)
       [scan delay buf; deliver_time_us <= now_us]
       <- true
  -> m_recv_queue.push(env)
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `ImpairmentEngine::init(config)` called with the configured `ImpairmentConfig`.
- `PrngEngine::seed()` called (for deterministic mode).

**Runtime:**
- `process_outbound()` called on every outbound message.
- `collect_deliverable()` called periodically from the receive loop to release delayed messages.
- `process_inbound()` called on every received message for partition/reorder checks.

---

## 14. Known Risks / Observations

- **Delay buffer saturation:** At `IMPAIR_DELAY_BUF_SIZE = 32`, high-rate senders with significant latency configured will fill the buffer and start dropping. This is a documented capacity limit.
- **Determinism dependency:** All randomized decisions use `m_prng` (xorshift64). Calling functions in a different order or from different threads changes the PRNG sequence and breaks reproducibility.
- **Partition modulo logic:** `is_partition_active()` uses integer division of `(now_us - start_us)` by `(partition_duration_us + partition_gap_us)`; see UC_18 for details.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `process_outbound()` applies impairments in the order: partition → loss → duplication → delay. Different ordering would change behavior for combined-impairment configurations.
- `[ASSUMPTION]` `collect_deliverable()` scans the delay buffer linearly (O(IMPAIR_DELAY_BUF_SIZE=32)) on every call; this is bounded and acceptable.
- `[ASSUMPTION]` `process_inbound()` applies partition and reorder checks; the exact reorder buffer mechanism is inferred from UC_17.
