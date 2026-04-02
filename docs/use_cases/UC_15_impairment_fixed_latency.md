# UC_15 — Fixed latency: message held in delay buffer until release_us elapses

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.1, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_outbound()` is called when `m_cfg.fixed_latency_ms > 0`. The envelope is placed in the delay buffer with `deliver_time = now_us + fixed_latency_ms * 1000`. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Hold an outbound message in a fixed-size delay buffer and release it only after the configured fixed latency has elapsed.
- **Success outcome:** On the first `process_outbound()` call: envelope queued with `deliver_time` in the future; `*out_count = 0`. On a subsequent `collect_deliverable()` call (from the next `send_message()` or `flush_delayed_to_clients()`): envelope transferred to `out_buf`; `*out_count = 1`.
- **Error outcomes:** If the delay buffer is full, the envelope is silently discarded. No error is returned.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp
Result ImpairmentEngine::process_outbound(
    const MessageEnvelope& env, uint64_t now_us,
    MessageEnvelope* out_buf, uint32_t* out_count);

Result ImpairmentEngine::collect_deliverable(
    MessageEnvelope* out_buf, uint32_t* out_count, uint64_t now_us);
```

Both called from backend `send_message()` and `flush_delayed_to_clients()`.

---

## 3. End-to-End Control Flow

**First call (envelope queued):**
1. `process_outbound(env, now_us, out_buf, &out_count)`:
   a. Partition and loss checks pass.
   b. `deliver_time = now_us + (uint64_t)m_cfg.fixed_latency_ms * 1000ULL` (plus optional jitter, UC_16).
   c. **`queue_to_delay_buf(env, deliver_time)`**: if `m_delay_count < IMPAIR_DELAY_BUF_SIZE`, store `{env, deliver_time}` at `m_delay_buf[m_delay_count++]`.
   d. `collect_deliverable(...)`: scan buffer; no entries have `deliver_time <= now_us` yet (they are all in the future). `*out_count = 0`.
2. Backend's `send_to_all_clients()` loop runs zero iterations; no socket write.

**Second call (latency elapsed, message released):**
1. `flush_delayed_to_clients(now_us)` (or next `send_message()`) calls `collect_deliverable(delay_buf, delay_count, now_us, out_buf, &out_count)`:
   a. Scan `m_delay_buf[0..m_delay_count-1]`.
   b. For each entry where `entry.deliver_time <= now_us`: copy to `out_buf`; mark slot empty; `out_count++`.
   c. Compact the buffer (remove released entries).
2. Backend sends each `out_buf[i]` via `send_to_all_clients()`.

---

## 4. Call Tree

```
TcpBackend::send_message()                         [TcpBackend.cpp]
 └── ImpairmentEngine::process_outbound()          [ImpairmentEngine.cpp]
      ├── check_loss()
      ├── queue_to_delay_buf(env, deliver_time)     [stores; deliver_time in future]
      └── collect_deliverable(now_us, out_buf, ...) [returns 0 on first call]

TcpBackend::flush_delayed_to_clients(now_us)       [TcpBackend.cpp]
 └── ImpairmentEngine::collect_deliverable()
      [returns due entries; out_count >= 1]
```

---

## 5. Key Components Involved

- **`queue_to_delay_buf()`** — Appends `{env, deliver_time}` to the delay buffer. O(1) append; bounded by `IMPAIR_DELAY_BUF_SIZE=32`.
- **`collect_deliverable()`** — Scans the delay buffer; returns all entries whose `deliver_time <= now_us`. Removes released entries and compacts the array.
- **`flush_delayed_to_clients()`** — Called by the backend to drain any due entries from the delay buffer and send them to connected clients.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_delay_count >= IMPAIR_DELAY_BUF_SIZE` | Discard; queue full | Queue entry |
| `entry.deliver_time <= now_us` | Release to out_buf | Keep in buffer |

---

## 7. Concurrency / Threading Behavior

- All calls synchronous in the backend's calling thread.
- `m_delay_buf` and `m_delay_count` — plain array; not thread-safe.

---

## 8. Memory & Ownership Semantics

- `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` — 32-slot array of `{MessageEnvelope, uint64_t deliver_time}` inside `ImpairmentEngine`. Each slot holds a full envelope copy.
- No heap allocation.

---

## 9. Error Handling Flow

- **Delay buffer full:** Message silently discarded on `queue_to_delay_buf()`. `process_outbound()` returns `OK`. Caller has no indication.
- No other error states.

---

## 10. External Interactions

- None during the delay phase. `::send()` is called when `flush_delayed_to_clients()` releases the entry.

---

## 11. State Changes / Side Effects

**On queue:**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_delay_buf[m_delay_count]` | unused | `{env, deliver_time}` |
| `ImpairmentEngine` | `m_delay_count` | D | D+1 |

**On release (collect):**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_delay_count` | D | D-1 (compacted) |
| `out_count` | 0 | 1 |

---

## 12. Sequence Diagram

```
TcpBackend::send_message()  [first call; now_us = T]
  -> ImpairmentEngine::process_outbound(env, T, ...)
       -> queue_to_delay_buf(env, T + latency_us)    [deliver_time in future]
       -> collect_deliverable(T, ...)                [no entries due; *out_count=0]
  <- Result::OK  [no ::send() called]

TcpBackend::flush_delayed_to_clients()  [later; now_us = T + latency_us]
  -> ImpairmentEngine::collect_deliverable(T+lat, ...)
       [entry.deliver_time <= T+lat; copy to out_buf; *out_count=1]
  -> send_to_all_clients(out_buf[0])
       -> ::send()
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_cfg.fixed_latency_ms > 0` and `m_cfg.enabled == true`.
- `flush_delayed_to_clients()` / `flush_delayed_to_queue()` called regularly so buffered messages are not stranded.

**Runtime:**
- Each call to `send_message()` or `receive_message()` flushes the delay buffer. If calls are infrequent relative to `fixed_latency_ms`, messages wait until the next flush.

---

## 14. Known Risks / Observations

- **Delay buffer capacity:** With `IMPAIR_DELAY_BUF_SIZE=32` slots and a large `fixed_latency_ms`, 32 messages can queue before any are released. The 33rd message is silently dropped.
- **`flush_delayed_to_clients()` dependency:** Delayed messages are not automatically released; a periodic call to `send_message()` or a separate flush is required to drain the buffer.
- **Message ordering:** Fixed latency preserves arrival order (FIFO) only if all messages have the same latency. Jitter (UC_16) adds per-message variation that can cause reordering.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `deliver_time = now_us + fixed_latency_ms * 1000` (microseconds). Jitter is added on top if both `fixed_latency_ms` and `jitter_mean_ms` are configured.
- `[ASSUMPTION]` `flush_delayed_to_clients()` is called from `TcpBackend::send_message()` before the main send path; this ensures queued-but-due messages are released on every send call.
