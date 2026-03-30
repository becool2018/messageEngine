## UC_17: ImpairmentEngine reorders buffered messages and delivers them out of sequence

---

## 1. Use Case Overview

When `ImpairmentConfig::reorder_enabled` is true and `reorder_window_size > 0`, `ImpairmentEngine::process_inbound()` maintains a fixed-size reorder window (`m_reorder_buf`). Incoming messages are buffered until the window fills. When a new message arrives and the window is already at capacity, a randomly selected buffered message — chosen via `m_prng.next_range()` — is released to the caller while the new message takes its place in the window. This produces out-of-order delivery: the message released is not necessarily the one that arrived first. Reordering operates on the inbound path; it is distinct from the outbound delay buffer used by latency and jitter.

---

## 2. Entry Points

| Entry Point | File | Signature |
|---|---|---|
| `ImpairmentEngine::process_inbound()` | `src/platform/ImpairmentEngine.cpp:209` | `Result ImpairmentEngine::process_inbound(const MessageEnvelope& in_env, uint64_t now_us, MessageEnvelope* out_buf, uint32_t buf_cap, uint32_t& out_count)` |

`process_inbound()` is not called from within `TcpBackend.cpp` in the traced source files. [ASSUMPTION] It is called by a receive-side path — for example, after `recv_from_client()` deserializes a frame and before the result is pushed to `m_recv_queue` — or by a different transport or simulation layer component not traced here.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Sub-case A — Window not yet full (buffering phase):**

1. **Caller invokes `process_inbound(in_env, now_us, out_buf, buf_cap, out_count)`** (ImpairmentEngine.cpp:209).
   - Preconditions asserted: `assert(m_initialized)`, `assert(envelope_valid(in_env))`, `assert(out_buf != NULL)`, `assert(buf_cap > 0)`.
   - `(void)now_us` — the `now_us` parameter is explicitly unused; cast to void immediately at line 220.
   - `out_count = 0U`.

2. **Reorder disabled check** (line 226):
   - `if (!m_cfg.reorder_enabled || m_cfg.reorder_window_size == 0U)`:
     - `envelope_copy(out_buf[0], in_env)` — passthrough; message copied directly to caller's output buffer.
     - `out_count = 1U`.
     - `assert(out_count <= buf_cap)`.
     - Return `Result::OK`. No reordering occurs.

3. **Reorder enabled; window not full** (line 234):
   - `if (m_reorder_count < m_cfg.reorder_window_size)`:
     - `envelope_copy(m_reorder_buf[m_reorder_count], in_env)` — message appended to end of window.
     - `++m_reorder_count`.
     - `out_count = 0U` — **no output on this call; message is held in the window**.
     - `assert(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)`.
     - Return `Result::OK`.
   - Caller receives `out_count = 0`: nothing to deliver yet.

**Sub-case B — Window full (release phase):**

4. **`m_reorder_count == m_cfg.reorder_window_size`** — both guards above are false; execution falls to line 246.

5. **Random release index selected** (line 246):
   - `release_idx = m_prng.next_range(0U, m_reorder_count - 1U)`.
   - **Inside `next_range(lo=0, hi=m_reorder_count - 1)`** (PrngEngine.hpp:113):
     - `assert(m_state != 0ULL)`.
     - `assert(hi >= lo)` — true when `m_reorder_count >= 1`, which is guaranteed since the window is full and `reorder_window_size >= 1`.
     - `raw = next()`:
       - `m_state ^= m_state << 13U`.
       - `m_state ^= m_state >> 7U`.
       - `m_state ^= m_state << 17U`.
       - Returns new `m_state`.
     - `range = m_reorder_count - 1 - 0 + 1 = m_reorder_count`.
     - `offset = static_cast<uint32_t>(raw % static_cast<uint64_t>(m_reorder_count))`.
     - `result = 0 + offset` — in `[0, m_reorder_count - 1]`.
     - `assert(result >= 0 && result <= m_reorder_count - 1)`.
     - Returns `release_idx`.
   - `assert(release_idx < m_reorder_count)` — verified at ImpairmentEngine.cpp:247.

6. **Release the randomly selected message** (lines 249–251):
   - `if (buf_cap >= 1U)`:
     - `envelope_copy(out_buf[0], m_reorder_buf[release_idx])` — copy the selected buffered message to the caller's output buffer.
     - `out_count = 1U`.

7. **Shift window to close the gap** (lines 256–258):
   - `for (uint32_t i = release_idx; i < m_reorder_count - 1U; ++i)`:
     - `envelope_copy(m_reorder_buf[i], m_reorder_buf[i + 1U])` — each entry shifts one position left.
   - Loop bound is fixed at `m_reorder_count - 1` (maximum `IMPAIR_DELAY_BUF_SIZE - 1 = 31` iterations). Power of 10 rule 2 satisfied.
   - After loop: `--m_reorder_count`. Window now has one fewer entry.

8. **Insert new message at end of window** (lines 262–265):
   - `if (m_reorder_count < m_cfg.reorder_window_size)` — always true immediately after the decrement, since window was at capacity before the shift.
     - `envelope_copy(m_reorder_buf[m_reorder_count], in_env)` — new message appended.
     - `++m_reorder_count`.
   - Window is restored to exactly `reorder_window_size` entries.

9. **Postconditions** (lines 269–271):
   - `assert(out_count <= buf_cap)`.
   - `assert(m_reorder_count <= m_cfg.reorder_window_size)`.
   - `assert(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)`.
   - Return `Result::OK`.

10. **Caller receives `out_count = 1`** with a message that was already in the window — not necessarily the message just passed in. The message just passed in (`in_env`) is now inside the window and will be held until a future call selects it via PRNG.

---

## 4. Call Tree (Hierarchical)

```
[caller — receive-side path; ASSUMPTION]
ImpairmentEngine::process_inbound(in_env, now_us, out_buf, buf_cap, out_count)
├── assert(m_initialized)
├── assert(envelope_valid(in_env))
├── assert(out_buf != NULL)
├── assert(buf_cap > 0)
├── (void)now_us                                         [explicitly unused]
├── out_count = 0
│
├── [if !reorder_enabled || reorder_window_size == 0]    ← PASSTHROUGH PATH
│   ├── envelope_copy(out_buf[0], in_env)               [memcpy; immediate delivery]
│   ├── out_count = 1
│   ├── assert(out_count <= buf_cap)
│   └── return OK
│
├── [if m_reorder_count < reorder_window_size]           ← SUB-CASE A: BUFFERING
│   ├── envelope_copy(m_reorder_buf[m_reorder_count], in_env)  [memcpy into window]
│   ├── ++m_reorder_count
│   ├── out_count = 0                                    [no output]
│   ├── assert(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)
│   └── return OK
│
└── [window full: m_reorder_count == reorder_window_size] ← SUB-CASE B: RELEASE
    ├── PrngEngine::next_range(0U, m_reorder_count - 1U)  ← PRNG CALL
    │   ├── assert(m_state != 0ULL)
    │   ├── assert(hi >= lo)
    │   ├── PrngEngine::next()
    │   │   ├── m_state ^= m_state << 13U
    │   │   ├── m_state ^= m_state >> 7U
    │   │   └── m_state ^= m_state << 17U
    │   ├── offset = raw % m_reorder_count
    │   ├── result = 0 + offset                          ∈ [0, m_reorder_count - 1]
    │   └── assert(result in [0, m_reorder_count - 1])
    ├── assert(release_idx < m_reorder_count)
    ├── [if buf_cap >= 1U]
    │   ├── envelope_copy(out_buf[0], m_reorder_buf[release_idx])  [memcpy; release]
    │   └── out_count = 1
    ├── [shift loop: i = release_idx .. m_reorder_count - 2]
    │   └── envelope_copy(m_reorder_buf[i], m_reorder_buf[i + 1U])  [compact window]
    ├── --m_reorder_count
    ├── [if m_reorder_count < reorder_window_size]        [always true here]
    │   ├── envelope_copy(m_reorder_buf[m_reorder_count], in_env)  [insert new msg]
    │   └── ++m_reorder_count
    ├── assert(out_count <= buf_cap)
    ├── assert(m_reorder_count <= reorder_window_size)
    ├── assert(m_reorder_count <= IMPAIR_DELAY_BUF_SIZE)
    └── return OK
```

---

## 5. Key Components Involved

| Component | Type | Location | Role |
|---|---|---|---|
| `ImpairmentEngine::process_inbound()` | Method | `ImpairmentEngine.cpp:209` | Main reordering logic; only method that uses `m_reorder_buf` |
| `ImpairmentConfig::reorder_enabled` | `bool` | `ImpairmentConfig.hpp:52` | Master reorder switch; must be true for reordering to engage |
| `ImpairmentConfig::reorder_window_size` | `uint32_t` | `ImpairmentConfig.hpp:56` | Depth of reorder window; must be <= `IMPAIR_DELAY_BUF_SIZE` (32); enforced at `init()` |
| `ImpairmentEngine::m_reorder_buf` | `MessageEnvelope[32]` | `ImpairmentEngine.hpp:138` | Fixed-size reordering window; holds full envelope copies |
| `ImpairmentEngine::m_reorder_count` | `uint32_t` | `ImpairmentEngine.hpp:139` | Current number of messages buffered in the window |
| `PrngEngine::next_range(lo, hi)` | Inline method | `PrngEngine.hpp:113` | Selects which buffered message to release; called once per release-phase invocation |
| `PrngEngine::next()` | Inline method | `PrngEngine.hpp:63` | xorshift64 core; called internally by `next_range` |
| `envelope_copy()` | Inline function | `MessageEnvelope.hpp:49` | `memcpy` of full `MessageEnvelope`; called multiple times per release-phase call |
| `now_us` | `uint64_t` parameter | ImpairmentEngine.cpp:211 | Accepted but explicitly discarded via `(void)now_us` at line 220; unused |

---

## 6. Branching Logic / Decision Points

| Decision | Location | Condition | Outcome |
|---|---|---|---|
| Reorder disabled | ImpairmentEngine.cpp:226 | `!reorder_enabled OR reorder_window_size == 0` | Passthrough; `out_count = 1`; PRNG not called |
| Window not full | ImpairmentEngine.cpp:234 | `m_reorder_count < reorder_window_size` | Message buffered; `out_count = 0`; PRNG not called |
| Window full | implicit at line 246 | `m_reorder_count == reorder_window_size` | PRNG called; random message released; new message inserted |
| `buf_cap < 1` at release | ImpairmentEngine.cpp:249 | `buf_cap < 1` | Release copy skipped; `out_count` stays 0; message evicted from window without delivery |
| Insert after shift | ImpairmentEngine.cpp:262 | `m_reorder_count < reorder_window_size` | Always true after decrement; new message always inserted |

**First `reorder_window_size` calls**: Every call returns `out_count = 0`. No PRNG consumption. The window fills one message at a time.

**Steady state**: Every call after the window is full returns `out_count = 1`, releases one randomly selected buffered message, and inserts the new one. One PRNG call per invocation.

**`reorder_window_size == 1`**: The release call becomes `next_range(0, 0)`, which always returns 0. The single buffered message is always released immediately when a new one arrives. PRNG state still advances on every release call even though the result is always 0. No effective reordering occurs with a window of 1.

---

## 7. Concurrency / Threading Behavior

- `process_inbound()` modifies `m_reorder_buf` and `m_reorder_count`. No locking or atomic operations are present.
- `m_prng.m_state` is advanced once per release-phase call to `process_inbound()`. `m_prng` is the same `PrngEngine` instance used by `process_outbound()` for loss, jitter, and duplication decisions.
- [ASSUMPTION] `process_inbound()` is called from a single receive thread. If `process_inbound()` and `process_outbound()` are called concurrently from separate send and receive threads on the same `ImpairmentEngine` instance, `m_prng.m_state`, `m_reorder_buf`, and `m_reorder_count` are all subject to data races (undefined behavior under C++17).
- The shift loop (`for i = release_idx .. m_reorder_count - 2`) performs at most `reorder_window_size - 1` `envelope_copy` calls. Maximum iterations: 31 (bounded by `IMPAIR_DELAY_BUF_SIZE - 1`). Power of 10 rule 2 satisfied.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

- `m_reorder_buf[IMPAIR_DELAY_BUF_SIZE]` is a member array of 32 full `MessageEnvelope` objects inside `ImpairmentEngine`. Each envelope is approximately 4119 bytes (4096-byte payload array + header fields); total storage approximately 131 KB within `ImpairmentEngine`.
- Each `envelope_copy` into the reorder window is a full `memcpy` of the original received envelope. The `in_env` `const&` reference is not retained after `process_inbound()` returns; the window owns independent copies.
- The shift loop performs up to 31 `memcpy` calls, each copying approximately 4119 bytes. Worst-case data movement per release-phase call: 31 * 4119 bytes ≈ 127 KB.
- `out_buf` is a caller-provided pointer. `process_inbound()` writes at most one `MessageEnvelope` to `out_buf[0]`. The caller must ensure `buf_cap >= 1` (asserted) and the buffer is writable and large enough.
- `m_reorder_count` never exceeds `m_cfg.reorder_window_size` (postcondition asserted) nor `IMPAIR_DELAY_BUF_SIZE` (asserted). This is enforced by `init()` asserting `cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE`.
- No heap allocation. Power of 10 rule 3 satisfied.

---

## 9. Error Handling Flow

| Condition | Location | Outcome |
|---|---|---|
| `reorder_window_size > IMPAIR_DELAY_BUF_SIZE` | `ImpairmentEngine::init()` line 26 | `assert(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)` fires; caught at initialization before runtime |
| `buf_cap == 0` | ImpairmentEngine.cpp:219 | `assert(buf_cap > 0)` fires in debug builds; undefined behavior if reached in release |
| `buf_cap >= 1` — normal case | ImpairmentEngine.cpp:249 | Release copy proceeds; `out_count = 1` |
| `buf_cap < 1` at release phase | ImpairmentEngine.cpp:249 | Copy skipped; `out_count` remains 0; selected message is evicted from window via the shift and permanently lost — not delivered to caller |
| `m_reorder_count == 0` entering release branch | Never reachable | The `< reorder_window_size` buffering guard ensures the release branch is entered only when the window is full; `reorder_window_size == 0` is caught by the disabled check at line 226 |
| PRNG state zero | PrngEngine.hpp:116 | `assert(m_state != 0ULL)` fires; prevented by `seed()` coercing 0 to 1 at `init()` |

**Silent message loss when `buf_cap < 1` in release phase**: The `assert(buf_cap > 0)` at line 219 prevents this in debug builds. In a production (`NDEBUG`) build, if `buf_cap == 0` is passed, the assertion does not fire, the copy is skipped, the window is compacted, and the new message is inserted — but the selected message is lost without any error code or log entry.

---

## 10. External Interactions

`process_inbound()` has no external interactions in the normal path:

| Interaction | Function | API |
|---|---|---|
| None | — | No socket I/O, no clock reads, no Logger calls in the normal reorder path |

`now_us` is accepted as a formal parameter but is immediately cast to `(void)now_us` at line 220. No `clock_gettime` or `Logger::log` calls occur in the reordering logic. Error paths (e.g., buffer misuse) do not have Logger calls either; they rely entirely on assertions.

---

## 11. State Changes / Side Effects

| State | Object | Phase | Change |
|---|---|---|---|
| `m_reorder_buf[m_reorder_count]` | `ImpairmentEngine` | Buffering (sub-case A) | New message written via `envelope_copy` |
| `m_reorder_count` | `ImpairmentEngine` | Buffering | Incremented by 1 |
| `m_prng.m_state` | `PrngEngine` | Release (sub-case B) | Advanced by one xorshift64 iteration |
| `out_buf[0]` | Caller-provided buffer | Release | Written with the randomly selected buffered message |
| `m_reorder_buf[release_idx..n-1]` | `ImpairmentEngine` | Release | Shifted left by one position (shift loop) |
| `m_reorder_count` | `ImpairmentEngine` | Release | Decremented by 1 (after release), then incremented by 1 (after insert); net 0 |
| `m_reorder_buf[m_reorder_count]` | `ImpairmentEngine` | Release | New incoming message inserted at end |
| `out_count` | Output parameter | Both phases | Set to 0 (buffering) or 1 (release or passthrough) |

---

## 12. Sequence Diagram (ASCII)

```
Window size = 3. Messages A, B, C, D, E arrive in order.

Caller              ImpairmentEngine                  PrngEngine
  |                       |                               |
  | process_inbound(A)    |                               |
  |---------------------->|                               |
  |  [count=0 < 3: buffer]|                               |
  |  reorder_buf = [A]    |                               |
  |  reorder_count = 1    |                               |
  |<-- out_count=0 -------|                               |
  |                       |                               |
  | process_inbound(B)    |                               |
  |---------------------->|                               |
  |  [count=1 < 3: buffer]|                               |
  |  reorder_buf = [A, B] |                               |
  |  reorder_count = 2    |                               |
  |<-- out_count=0 -------|                               |
  |                       |                               |
  | process_inbound(C)    |                               |
  |---------------------->|                               |
  |  [count=2 < 3: buffer]|                               |
  |  reorder_buf = [A,B,C]|                               |
  |  reorder_count = 3    |                               |
  |<-- out_count=0 -------|                               |
  |                       |                               |
  | process_inbound(D)    |                               |
  |---------------------->|                               |
  |  [count=3 == 3: RELEASE PHASE]                        |
  |                       | next_range(0, 2)              |
  |                       |------------------------------>|
  |                       |<---- release_idx = 1 ---------|  (selects B)
  |                       |                               |
  |  out_buf[0] = B (index 1 selected)                    |
  |  out_count = 1                                        |
  |  shift: [A,B,C] → [A,C]  (B removed; C shifts left)  |
  |  reorder_count = 2                                    |
  |  insert D: [A,C,D]                                    |
  |  reorder_count = 3                                    |
  |<-- out_count=1, delivers B ---|                       |
  |                       |                               |
  | process_inbound(E)    |                               |
  |---------------------->|                               |
  |  [count=3 == 3: RELEASE PHASE]                        |
  |                       | next_range(0, 2)              |
  |                       |------------------------------>|
  |                       |<---- release_idx = 2 ---------|  (selects D)
  |                       |                               |
  |  out_buf[0] = D (index 2 selected)                    |
  |  shift: [A,C,D] → [A,C]  (D removed)                 |
  |  reorder_count = 2                                    |
  |  insert E: [A,C,E]                                    |
  |  reorder_count = 3                                    |
  |<-- out_count=1, delivers D ---|                       |

Send order:     A  B  C  D  E  ...
Deliver order:  (none×3)  B  D  ...
Still held:     A, C, E (in window; not yet released)
```

---

## 13. Initialization vs Runtime Flow

**Initialization phase** (`ImpairmentEngine::init()`):
- `assert(cfg.reorder_window_size <= IMPAIR_DELAY_BUF_SIZE)` — enforced before any runtime use; prevents out-of-bounds access to `m_reorder_buf`.
- `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))` — all 32 slots zeroed; `MessageEnvelope::message_type` fields set to 0 (= `MessageType::DATA`) by the zero-fill, not `MessageType::INVALID`. [ASSUMPTION] Zeroed slots are never accessed as valid envelopes before being filled by a `process_inbound()` call.
- `m_reorder_count = 0`.
- `m_cfg.reorder_enabled` and `m_cfg.reorder_window_size` stored by value copy from the provided `ImpairmentConfig`.
- PRNG seeded via `m_prng.seed(seed)`.
- `m_initialized = true`.

**Runtime filling phase** (first `reorder_window_size` calls): `process_inbound()` returns `out_count = 0` every call. No PRNG consumption. No output. The caller must tolerate a startup period of `reorder_window_size` silent calls before any messages are delivered.

**Runtime steady state** (after window fills): every `process_inbound()` call consumes one PRNG value, releases one message to the caller, and maintains the window at exactly `reorder_window_size` entries. Shift loop executes up to `reorder_window_size - 1` times per call.

**Drain/teardown**: There is no `flush_reorder_buffer()` method. Messages held in `m_reorder_buf` at process shutdown are permanently lost. There is no mechanism to drain the window by injecting dummy messages or by calling a dedicated API.

---

## 14. Known Risks / Observations

1. **No drain/flush API**: Messages held in `m_reorder_buf` at teardown or transport close are permanently discarded without notification. There is no `flush_reorder_buffer()` or equivalent.

2. **Shift loop is O(n) per release call**: For a window of 32, the shift loop performs up to 31 `memcpy` calls (~127 KB of data movement) every `process_inbound()` call in steady state. This is bounded and Power-of-10 compliant but is potentially expensive at high inbound message rates.

3. **`now_us` is accepted but entirely ignored**: The `now_us` parameter is immediately discarded via `(void)now_us`. Reordering is purely count-driven (window fills trigger random release) with no time-based timeout. If inbound traffic slows or stops, messages can be held in the window indefinitely with no forced release mechanism.

4. **PRNG is shared with outbound path**: `m_prng` is the same `PrngEngine` used by `process_outbound()` for loss, jitter, and duplication. If both `process_inbound()` and `process_outbound()` are called on the same `ImpairmentEngine` instance, the PRNG sequence for reordering and for outbound impairments is interleaved. The combined sequence is deterministic only if the exact interleaving of inbound and outbound calls is also deterministic and reproducible.

5. **`reorder_window_size == 1` does not reorder**: `next_range(0, 0)` always returns 0; the only buffered message is always released immediately when a new one arrives. PRNG state still advances on every call. This is not a bug per se but a configuration edge case that produces no reordering effect.

6. **Zeroed `m_reorder_buf` slots after `memset`**: `memset` zeros all fields, setting `message_type` to 0 (`MessageType::DATA`) rather than `MessageType::INVALID`. Zeroed slots should never be accessed as valid messages before being filled, but if they were (e.g., via an `out_count` bug), `envelope_valid()` would return true for a zeroed envelope because `source_id == 0 == NODE_ID_INVALID` would cause it to return false — providing a safety check.

7. **`buf_cap < 1` in release phase causes silent message loss in NDEBUG builds**: The `assert(buf_cap > 0)` at line 219 catches this only in debug builds. In release builds, if `buf_cap == 0` reaches the release branch, the selected message is evicted from the window without delivery.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `process_inbound()` is called from a receive-side path not shown in `TcpBackend.cpp`. The traced source does not show where or how `process_inbound()` is invoked in the TCP receive pipeline.
- [ASSUMPTION] The caller always passes `buf_cap >= 1` in production use, consistent with the assertion at line 219.
- [ASSUMPTION] Single-threaded access; `process_inbound()` and `process_outbound()` are not called concurrently on the same `ImpairmentEngine` instance.
- [ASSUMPTION] Message loss at teardown (un-drained reorder buffer) is acceptable to the system. No flush mechanism is required by the current design.
- [ASSUMPTION] The zeroed-out `m_reorder_buf` slots (after `memset` in `init()`) are never accessed before being overwritten by a valid `envelope_copy()`. No sentinel value (e.g., `MessageType::INVALID`) is explicitly set in the zero-filled slots.
- [UNKNOWN] Whether the reorder buffer is ever explicitly drained at transport shutdown. No flush method is visible in the codebase.
- [UNKNOWN] How the caller integrates `out_count = 0` responses (filling phase) into the broader receive loop — whether it retries immediately or waits for the next inbound message.
- [UNKNOWN] The statistical distribution of per-message hold time in terms of number of subsequent messages: with a window of N and uniform random selection, the expected number of subsequent messages before a given held message is selected follows a geometric distribution. The maximum hold time is unbounded in theory (a message could be unlucky repeatedly), though it is probabilistically unlikely for reasonable window sizes.

---