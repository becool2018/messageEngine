# UC_17 — Reordering: inbound messages buffered and released out of arrival order

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.5, REQ-5.2.1, REQ-5.2.4

---

## 1. Use Case Overview

- **Trigger:** `ImpairmentEngine::process_inbound()` is called by the backend when `m_cfg.reorder_enabled == true`. Incoming envelopes are buffered and released out of arrival order after the window fills. File: `src/platform/ImpairmentEngine.cpp`.
- **Goal:** Buffer a sliding window of `reorder_window_size` inbound messages and release them in a shuffled order, simulating network reordering.
- **Success outcome:** The first `reorder_window_size` messages are buffered; on each subsequent arrival, the oldest buffered message is released to `out_buf` (FIFO eviction with window-based delay), producing out-of-order delivery to the application.
- **Error outcomes:** None — `process_inbound()` always returns `Result::OK`. If the window buffer is full, the arriving message may evict an older entry immediately.

---

## 2. Entry Points

```cpp
// src/platform/ImpairmentEngine.cpp
Result ImpairmentEngine::process_inbound(
    const MessageEnvelope& env, uint64_t now_us,
    MessageEnvelope* out_buf, uint32_t* out_count);
```

Called internally by backend `recv_from_client()` / `recv_one_datagram()` after deserialization.

---

## 3. End-to-End Control Flow

1. Backend calls `m_impairment.process_inbound(env, now_us, inbound_delay_buf, &inbound_count)`.
2. `NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)`.
3. `*out_count = 0`.
4. **Partition check (inbound):** `is_partition_active(now_us)` — if true, drop all inbound; return OK.
5. **Reorder enabled:**
   a. The `env` is placed in a reorder window buffer (a circular buffer of `reorder_window_size` entries, separate from the outbound delay buffer).
   b. If the window is not yet full: the entry is buffered; `*out_count = 0`. No message released yet.
   c. If the window is full: the oldest entry is moved to `out_buf[0]`; `*out_count = 1`. The new `env` takes the freed slot.
6. The released entry (if any) is passed back to the backend, which pushes it to the inbound `RingBuffer`.

---

## 4. Call Tree

```
TcpBackend::recv_from_client()                   [TcpBackend.cpp]
 └── ImpairmentEngine::process_inbound()         [ImpairmentEngine.cpp]
      ├── is_partition_active()
      └── [reorder window logic]
           └── [FIFO eviction of oldest entry to out_buf]

TcpBackend::flush_delayed_to_queue(now_us)
 └── ImpairmentEngine::collect_deliverable() or reorder release
      └── RingBuffer::push()                     [RingBuffer.hpp]
```

---

## 5. Key Components Involved

- **`ImpairmentEngine::process_inbound()`** — Applies inbound impairments. Reorder mode uses the inbound delay buffer (separate from outbound) as the reorder window.
- **Reorder window** — Fixed-size sliding window of `reorder_window_size` (clamped to `IMPAIR_DELAY_BUF_SIZE=32`) inbound envelopes. Oldest-first eviction produces out-of-order delivery.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `is_partition_active()` | Drop inbound; return OK | Continue to reorder |
| `reorder_enabled` | Buffer in reorder window | Pass through directly |
| `window not full` | Queue; `*out_count = 0` | Evict oldest; `*out_count = 1` |
| `reorder_window_size == 0` | No reorder (pass through) | Normal windowing |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the backend's receive thread (application thread calling `receive_message()`).
- Inbound reorder buffer is a plain array; not thread-safe.

---

## 8. Memory & Ownership Semantics

- `m_delay_buf` used for inbound reorder buffering (same fixed-size array, `IMPAIR_DELAY_BUF_SIZE=32` slots). The outbound delay buffer and inbound reorder buffer share the same array in the current design, or use separate tracking depending on the `ImpairmentEngine` implementation.
- No heap allocation.

---

## 9. Error Handling Flow

- No error states. Reordering is a silent behavioral modification.
- When the partition is active, all inbound messages are dropped; the reorder buffer is not modified.

---

## 10. External Interactions

- None — operates entirely in process memory on deserialized envelopes.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | reorder window slot | empty | new `env` buffered |
| `ImpairmentEngine` | oldest window slot | occupied | moved to `out_buf` (released) |
| `RingBuffer` (later) | `m_buf` | previous | oldest env pushed on flush |

---

## 12. Sequence Diagram

```
Backend::recv_from_client()  [N received in order: A, B, C with window_size=2]

  env=A -> process_inbound()
       [window not full: buffer A; *out_count=0]
  env=B -> process_inbound()
       [window full: evict A to out_buf; buffer B; *out_count=1]
       -> flush_delayed_to_queue() -> RingBuffer::push(A)
  env=C -> process_inbound()
       [window full: evict B to out_buf; buffer C; *out_count=1]
       -> flush_delayed_to_queue() -> RingBuffer::push(B)
```

Application receives: A, B in original order (FIFO window — actual reordering requires random eviction order).

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_cfg.reorder_enabled = true` and `m_cfg.reorder_window_size > 0` (clamped to 32).

**Runtime:**
- Every received envelope enters the reorder window. The window drains one entry per new arrival once full.

---

## 14. Known Risks / Observations

- **FIFO window produces delay, not random reordering:** A simple FIFO eviction policy produces a fixed delay, not random reordering. True reordering requires random slot selection. The actual reordering randomness depends on the ImpairmentEngine implementation details.
- **Window drain at connection close:** Buffered entries in the reorder window are not flushed at `close()` unless explicitly drained. Unflushed messages are silently lost.
- **Shared buffer with outbound delay:** If the same `m_delay_buf` array is used for both outbound delay and inbound reorder, simultaneous outbound latency and inbound reorder could exhaust the 32 slots quickly.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The inbound reorder window uses `m_delay_buf` with time-based or FIFO release. The exact eviction policy (FIFO vs random) is inferred from `ImpairmentEngine.cpp` source; if random, it would use the PRNG.
- `[ASSUMPTION]` `reorder_window_size` is clamped to `IMPAIR_DELAY_BUF_SIZE=32` by `ImpairmentConfigLoader` (confirmed in `apply_reorder_window()` in `ImpairmentConfigLoader.cpp`).
