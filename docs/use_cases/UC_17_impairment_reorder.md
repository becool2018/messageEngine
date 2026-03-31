# UC_17 — Reordering: inbound messages buffered and released out of arrival order

**HL Group:** HL-12 — Configure Network Impairments
**Actor:** System
**Requirement traceability:** REQ-5.1.5, REQ-5.2.1, REQ-5.2.2, REQ-5.2.4, REQ-5.3.1, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** An inbound message envelope is passed to `ImpairmentEngine::process_inbound()`. The `ImpairmentConfig` has `reorder_enabled = true` and `reorder_window_size > 0`.

**Goal:** Simulate out-of-order delivery. Arriving messages are buffered in an internal reorder window (`m_reorder_buf`). When the window is full, a randomly chosen buffered message is released and the newly arriving message is added to the window. The caller receives a message that is likely not the most recently arrived one.

**Success outcome:** `process_inbound()` returns `Result::OK`. When `m_reorder_count < reorder_window_size`, `out_count = 0` (message buffered, nothing released). When `m_reorder_count == reorder_window_size`, `out_count = 1` (one buffered message released at a random index; new message appended to the window).

**Error outcomes:**
- `ERR_FULL` — [ASSUMPTION: this path does not occur under correct sizing because `reorder_window_size <= IMPAIR_DELAY_BUF_SIZE` is asserted at `init()`. The return value `ERR_FULL` is only reachable if `buf_cap < 1`.]
- `Result::OK` with `out_count = 0` is not an error; it means the window is filling up.

---

## 2. Entry Points

```
// src/platform/ImpairmentEngine.hpp / ImpairmentEngine.cpp
Result ImpairmentEngine::process_inbound(const MessageEnvelope& in_env,
                                         uint64_t now_us,
                                         MessageEnvelope* out_buf,
                                         uint32_t buf_cap,
                                         uint32_t& out_count);
```

Called from the inbound path of a backend (e.g., after `recv_from_client()` → `Serializer::deserialize()`). [ASSUMPTION: `process_inbound()` is called by the backend's receive path before pushing messages to the application queue. In the current `TcpBackend` implementation, `process_inbound()` is not called in the production code path; it is used directly in tests. The description here traces the intended architectural path.]

PRNG method invoked:

```
// src/platform/PrngEngine.hpp
uint32_t PrngEngine::next_range(uint32_t lo, uint32_t hi);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Sub-case A — Window filling (m_reorder_count < reorder_window_size)**

1. Caller invokes `process_inbound(in_env, now_us, out_buf, buf_cap, out_count)`.
2. Preconditions checked:
   - `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
   - `NEVER_COMPILED_OUT_ASSERT(envelope_valid(in_env))`.
   - `NEVER_COMPILED_OUT_ASSERT(out_buf != nullptr)`.
   - `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)`.
3. `out_count = 0U` initialized.
4. `(void)now_us` — timestamp parameter is explicitly unused. Reordering is event-driven by window fullness, not wall-clock time.
5. Reordering gate: `!m_cfg.reorder_enabled || m_cfg.reorder_window_size == 0U` — both conditions are `false` (reorder is enabled and window is non-zero). Gate is not taken.
6. `m_reorder_count < m_cfg.reorder_window_size` — `true` (window not yet full).
7. `envelope_copy(m_reorder_buf[m_reorder_count], in_env)` — message is copied into the next free window slot.
8. `++m_reorder_count`.
9. `out_count = 0U` — no message is delivered to the caller.
10. `NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)`.
11. Returns `Result::OK` with `out_count = 0`.

**Sub-case B — Window full (m_reorder_count == reorder_window_size)**

1–4. Same as Sub-case A.
5. Reordering gate: not taken (same as above).
6. `m_reorder_count < m_cfg.reorder_window_size` — `false` (window is full).
7. Fall through to release-and-replace logic.
8. `release_idx = m_prng.next_range(0U, m_reorder_count - 1U)`.
   - Inside `next_range(0, m_reorder_count - 1)`:
     - `NEVER_COMPILED_OUT_ASSERT(m_state != 0ULL)`.
     - `NEVER_COMPILED_OUT_ASSERT(hi >= lo)` — `m_reorder_count >= 1` guaranteed by window-full condition.
     - `raw = next()` — one xorshift64 step.
     - `offset = raw % m_reorder_count`.
     - Returns `offset` (in `[0, m_reorder_count - 1]`).
9. `NEVER_COMPILED_OUT_ASSERT(release_idx < m_reorder_count)`.
10. `buf_cap >= 1U` — `true` (precondition asserted).
11. `envelope_copy(out_buf[0], m_reorder_buf[release_idx])` — the randomly selected message is copied to the output buffer. `out_count = 1`.
12. **Compact the reorder window**: shift entries left from `release_idx` to `m_reorder_count - 2`:
    - `for (uint32_t i = release_idx; i < m_reorder_count - 1U; ++i)`
    - `envelope_copy(m_reorder_buf[i], m_reorder_buf[i + 1U])`
    - Loop bound: at most `m_reorder_count - 1` iterations (fixed by `m_reorder_count`).
13. `--m_reorder_count` — window now has one free slot at the back.
14. `m_reorder_count < m_cfg.reorder_window_size` — `true` (we just decremented).
15. `envelope_copy(m_reorder_buf[m_reorder_count], in_env)` — new message appended to back of window.
16. `++m_reorder_count` — window returns to `reorder_window_size`.
17. Postcondition assertions:
    - `NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap)`.
    - `NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= m_cfg.reorder_window_size)`.
    - `NEVER_COMPILED_OUT_ASSERT(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)`.
18. Returns `Result::OK` with `out_count = 1`.

**Sub-case C — Reordering disabled**

1–4. Same precondition checks.
5. Gate: `!m_cfg.reorder_enabled || m_cfg.reorder_window_size == 0U` — `true`.
6. `envelope_copy(out_buf[0], in_env)` — pass-through copy.
7. `out_count = 1U`.
8. `NEVER_COMPILED_OUT_ASSERT(out_count <= buf_cap)`.
9. Returns `Result::OK` with `out_count = 1`. No buffering occurs.

---

## 4. Call Tree

```
ImpairmentEngine::process_inbound()
 ├── [Sub-case C: reorder disabled]
 │    └── envelope_copy()                  [pass-through; out_count=1]
 ├── [Sub-case A: window filling]
 │    └── envelope_copy()                  [into m_reorder_buf[count]]
 │         (out_count = 0; caller gets nothing)
 └── [Sub-case B: window full]
      ├── PrngEngine::next_range(0, count-1)
      │    └── PrngEngine::next()          [xorshift64 step]
      ├── envelope_copy()                  [release_idx → out_buf[0]]
      ├── for-loop: envelope_copy()×N      [compact window; fixed bound]
      └── envelope_copy()                  [in_env → back of window]
```

---

## 5. Key Components Involved

- **`ImpairmentEngine`** (`src/platform/ImpairmentEngine.hpp/.cpp`): Owns `m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]` and `m_reorder_count`. The reorder window is a simple array treated as a queue with random pop. Separate from the delay buffer (`m_delay_buf`).

- **`PrngEngine::next_range()`** (`src/platform/PrngEngine.hpp`): Selects the random release index from `[0, m_reorder_count - 1]`. Deterministic given the seed. Same xorshift64 state as used for loss and jitter decisions.

- **`envelope_copy()`** (`src/core/MessageEnvelope.hpp`): Used three times in Sub-case B: once to extract the released message, once per shift step to compact the array, and once to insert the new message at the back. Each call is a `memcpy` of `sizeof(MessageEnvelope)` bytes.

- **`ImpairmentConfig::reorder_window_size`** (`src/core/ImpairmentConfig.hpp`): Controls how many messages are held before any are released. Asserted `<= IMPAIR_DELAY_BUF_SIZE` at `init()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!reorder_enabled \|\| reorder_window_size == 0` | Pass-through: `out_count = 1`; `out_buf[0] = in_env`; return OK | Proceed to buffering logic |
| `m_reorder_count < reorder_window_size` (window filling) | Buffer message: `m_reorder_buf[count] = in_env`; `++count`; `out_count = 0`; return OK | Fall through to release logic (window full) |
| `buf_cap >= 1U` (in release path) | Execute release, compact, and insert | Skip release and insert (out_count stays 0) — [ASSUMPTION: unreachable given precondition assertion] |
| `m_reorder_count < reorder_window_size` after decrement (in release path) | Insert `in_env` at back | Do not insert (window unexpectedly at capacity — [ASSUMPTION: unreachable]) |
| `next_range()` returns `release_idx` | Specific message at that index is released | (all outcomes are valid; index is random) |

---

## 7. Concurrency / Threading Behavior

`process_inbound()` operates on the same `ImpairmentEngine` instance as `process_outbound()`. Both modify `m_prng.m_state`. There is no internal synchronization. If a single `ImpairmentEngine` is used from multiple threads (e.g., a send thread calling `process_outbound()` and a receive thread calling `process_inbound()`), concurrent access to `m_prng` and the respective buffers constitutes a data race.

[ASSUMPTION: the `ImpairmentEngine` is used from a single thread, or the caller serializes access.]

Note: `m_reorder_buf` is entirely separate from `m_delay_buf`. Sub-case B's compaction loop (`envelope_copy` shifts) is fully sequential within the function; there are no re-entrant calls.

---

## 8. Memory & Ownership Semantics

**Stack allocations in this flow:**

| Variable | Declared in | Size |
|----------|-------------|------|
| `release_idx` | `process_inbound()` | `uint32_t`, 4 bytes |

**Fixed member arrays:**

| Member | Owner | Capacity |
|--------|-------|----------|
| `m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]` | `ImpairmentEngine` | 32 × sizeof(MessageEnvelope) ≈ 132 KB total |
| `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]` | `ImpairmentEngine` | 32 × sizeof(DelayEntry) (separate; not used in this UC) |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. `out_buf` is caller-provided; `ImpairmentEngine` does not own it. `m_reorder_buf` is a fixed member array.

**Compaction overhead:** In Sub-case B, the inner `envelope_copy` loop shifts up to `m_reorder_count - 1` entries, each a full `sizeof(MessageEnvelope)` `memcpy`. For a window of 32 and 4120-byte envelopes, the worst-case shift is 31 × 4120 ≈ 128 KB of `memcpy` per released message. [ASSUMPTION: sizeof(MessageEnvelope) ≈ 4120 bytes.]

---

## 9. Error Handling Flow

| Error | Trigger | State after | Caller action |
|-------|---------|-------------|---------------|
| `out_count = 0` with `Result::OK` | Window is still filling | `m_reorder_buf` has one more entry; `m_reorder_count` incremented | Caller receives no envelope; must handle this gracefully (not a failure) |
| `out_count = 1` with `Result::OK` | Window was full | One message released; new message appended; `m_reorder_count` unchanged | Caller processes `out_buf[0]`; note it is not the most recently arrived message |
| `buf_cap < 1` | Caller error | Assertion fires at precondition check | `NEVER_COMPILED_OUT_ASSERT(buf_cap > 0U)` — abort or reset handler |

---

## 10. External Interactions

None. `process_inbound()` is purely in-memory. No POSIX calls are made during buffering or release. The `now_us` parameter is explicitly ignored (`(void)now_us`).

---

## 11. State Changes / Side Effects

**Sub-case A (window filling):**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `ImpairmentEngine` | `m_reorder_buf[count]` | zero or stale | Copy of `in_env` |
| `ImpairmentEngine` | `m_reorder_count` | N | N + 1 |

**Sub-case B (window full):**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `PrngEngine` | `m_state` | S_n | S_{n+1} (one xorshift64 step) |
| `out_buf[0]` | (caller's buffer) | uninitialized | Copy of `m_reorder_buf[release_idx]` |
| `ImpairmentEngine` | `m_reorder_buf[release_idx .. count-2]` | original values | Shifted left by one position |
| `ImpairmentEngine` | `m_reorder_buf[count-1]` (after decrement) | previous entry | Copy of `in_env` |
| `ImpairmentEngine` | `m_reorder_count` | N | N (decremented then incremented; net zero change) |

---

## 12. Sequence Diagram

```
Caller (backend)    ImpairmentEngine     PrngEngine
  |                      |                   |
  |--process_inbound()-->|                   |
  |  (in_env, now_us,    |                   |
  |   out_buf, cap,      |                   |
  |   &out_count)        |                   |
  |                      |--reorder_enabled? |
  |                      |  (yes; window>0)  |
  |                      |                   |
  |  [Sub-case A: count < window_size]       |
  |                      |--envelope_copy(   |
  |                      |  buf[count],      |
  |                      |  in_env)          |
  |                      |  count++          |
  |<--OK (out_count=0)---|                   |
  |                      |                   |
  |  [Sub-case B: count == window_size]      |
  |                      |--next_range(0,    |
  |                      |  count-1)-------->|
  |                      |                   |--next() [xorshift64]
  |                      |<--release_idx-----|
  |                      |--envelope_copy(   |
  |                      |  out_buf[0],      |
  |                      |  buf[rel_idx])    |
  |                      |--compact loop     |
  |                      |  (shift left)     |
  |                      |--count--          |
  |                      |--envelope_copy(   |
  |                      |  buf[count],      |
  |                      |  in_env)          |
  |                      |  count++          |
  |<--OK (out_count=1)---|                   |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**

- `ImpairmentEngine::init(cfg)` asserts `cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE`, zeros `m_reorder_buf` via `memset`, sets `m_reorder_count = 0`, seeds PRNG.

**Steady-state runtime:**

- First `reorder_window_size` calls to `process_inbound()` fill the window without releasing any messages (`out_count = 0` each time). The caller must handle a stream of zero deliveries at startup.
- Once the window is full, every call releases exactly one message (chosen at random) and inserts the new one. From this point, `out_count = 1` on every call.
- If reordering is disabled after `init()` by re-initializing with `reorder_enabled = false`, the `m_reorder_buf` still holds buffered messages that will never be released through `process_inbound()`. Those messages are effectively lost unless the application drains them manually. [ASSUMPTION: there is no drain path in the current API.]

---

## 14. Known Risks / Observations

- **`reorder_window_size` messages are silently withheld on startup.** The first `reorder_window_size` calls to `process_inbound()` return `out_count = 0`. The caller's receive loop must tolerate a stream of empty responses during this fill phase.

- **Messages may never be released if sends stop.** Once the window is full, messages are only released when new messages arrive. If no new message arrives, the oldest window entries remain buffered indefinitely. There is no timeout-based flush in `process_inbound()`.

- **Compaction cost grows with window size.** For `reorder_window_size = 32` and early release_idx values, the compaction loop performs up to 31 `memcpy` calls of ~4120 bytes each. This is bounded but potentially expensive in latency-sensitive paths.

- **PRNG shared with outbound path.** `m_prng.next_range()` is called from both `process_outbound()` (for loss and jitter) and `process_inbound()` (for reorder index). The ordering of these calls determines the reproducible sequence for a given seed. Interleaving outbound and inbound processing changes the PRNG sequence.

- **`IMPAIR_DELAY_BUF_SIZE = 32` applies to reorder buffer too.** `m_reorder_buf` has the same compile-time capacity as `m_delay_buf`. `reorder_window_size` is asserted `<= IMPAIR_DELAY_BUF_SIZE` at init.

- **Dropped envelopes during window flush.** If the reorder window contains N messages at shutdown or disable time and no drain mechanism exists, those N messages are lost.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `process_inbound()` is intended to be called by the backend's receive path between `Serializer::deserialize()` and the application queue push. In the current `TcpBackend::recv_from_client()` implementation, `process_inbound()` is not called; messages are pushed directly to `m_recv_queue`. This UC documents the designed behavior.
- [ASSUMPTION] `now_us` is passed but intentionally ignored (`(void)now_us` in the source). A future extension could release buffered messages based on age rather than window fullness.
- [ASSUMPTION] There is no drain or flush API for `m_reorder_buf`. Buffered messages stranded in the window when the system shuts down are lost.
- [ASSUMPTION] `sizeof(MessageEnvelope)` ≈ 4120 bytes. The compaction loop's cost depends on this.
- [ASSUMPTION] The caller's `out_buf` has sufficient capacity for at most 1 envelope (`out_count` never exceeds 1). A `buf_cap >= 1` is required and asserted.
- [ASSUMPTION] If `reorder_window_size` is set to 1, Sub-case B fires on every call after the first, producing a behavior where the current message is always held and the previous one is released — equivalent to a 1-message delay.
