# UC_24 — LocalSimHarness: send on one harness, receive on linked peer

**HL Group:** HL-14 — In-Process Simulation
**Actor:** System (called by test fixtures or simulation orchestrators)
**Requirement traceability:** REQ-4.1.2, REQ-4.1.3, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** A test fixture or simulation orchestrator has constructed two `LocalSimHarness` instances (A and B), called `init()` on each, and called `A.link(&B)`. The trigger is a call to `A.send_message(envelope)` followed by `B.receive_message(out, timeout_ms)`.

**Goal:** The `MessageEnvelope` passed to `A.send_message()` is delivered into `B`'s receive queue through an in-memory path — no real sockets, no OS network stack. `B.receive_message()` returns `Result::OK` with the envelope populated.

**Success outcome:** `B.receive_message()` returns `OK` with `out` populated from the transmitted envelope. No sockets, no serialization, no OS network stack involved.

**Error outcomes:**
- Silent drop (`Result::OK` returned by `A.send_message()`) — loss or partition impairment fires.
- `ERR_FULL` returned by `A.send_message()` — `B.m_recv_queue` is at capacity when the primary envelope is injected.
- `ERR_TIMEOUT` returned by `B.receive_message()` — queue was empty for the full timeout duration.
- Assertion abort — `m_open == false`, `m_peer == nullptr`, or `!envelope_valid()` at entry of `send_message()`.

---

## 2. Entry Points

**Setup:**
```cpp
// src/platform/LocalSimHarness.cpp, line 78
void LocalSimHarness::link(LocalSimHarness* peer);
// Called once before the first send_message(). Sets A.m_peer = &B.
```

**Send:**
```cpp
// src/platform/LocalSimHarness.cpp, line 109
Result LocalSimHarness::send_message(const MessageEnvelope& envelope);
// Called by test fixture on instance A.
```

**Inject (internal, called from send_message):**
```cpp
// src/platform/LocalSimHarness.cpp, line 91
Result LocalSimHarness::inject(const MessageEnvelope& envelope);
// Called by A on instance B to push envelopes into B's receive queue.
```

**Receive:**
```cpp
// src/platform/LocalSimHarness.cpp, line 147
Result LocalSimHarness::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms);
// Called by test fixture on instance B.
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase 1 — Setup: `A.link(&B)`**

1. `A.link(&B)` entered.
2. `NEVER_COMPILED_OUT_ASSERT(peer != nullptr)` — forbids null.
3. `NEVER_COMPILED_OUT_ASSERT(peer != this)` — forbids self-link.
4. `A.m_peer = &B` — raw non-owning pointer stored.
5. `Logger::log(INFO, "LocalSimHarness", "Harness linked to peer")`.
6. Returns. Note: `B.m_peer` is unaffected; `B.link(&A)` must be called separately for bidirectional delivery.

**Phase 2 — `A.send_message(envelope)`**

7. Entry assertions: `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(m_peer != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.

8. `now_us = timestamp_now_us()` — inline; `clock_gettime(CLOCK_MONOTONIC, &ts)`; returns microseconds.

9. `res = A.m_impairment.process_outbound(envelope, now_us)`:
   - **Impairments disabled** (`m_cfg.enabled == false`): Calls `queue_to_delay_buf(envelope, now_us)` with `release_us = now_us` (immediate). If delay buffer full (32 slots), returns `ERR_FULL`. Otherwise returns `OK`.
   - **Impairments enabled**:
     - `is_partition_active(now_us)`: if active, returns `ERR_IO`.
     - `check_loss()`: draws `m_prng.next_double()`; if less than `loss_probability`, returns `ERR_IO`.
     - Computes `release_us = now_us + latency_us + jitter_us`; calls `queue_to_delay_buf(envelope, release_us)`.
     - `apply_duplication()`: if `duplication_probability > 0.0`, draws PRNG; if fires, calls `queue_to_delay_buf(envelope, release_us + 100)`.
     - Returns `OK`.

10. **Drop check.** If `res == ERR_IO` (loss or partition): return `Result::OK` silently. B never sees the message.

11. **Collect delayed messages.** `delayed_count = A.m_impairment.collect_deliverable(now_us, delayed_envelopes[32], 32)`. Scans `m_delay_buf[0..31]`; copies entries where `active && release_us <= now_us` to `delayed_envelopes[]`, clears those slots, decrements `m_delay_count`. With impairments disabled and `release_us = now_us`, `delayed_count = 1` on the first call.

12. **Inject delayed messages** (bounded loop, `i = 0..delayed_count-1`):
    - `NEVER_COMPILED_OUT_ASSERT(i < IMPAIR_DELAY_BUF_SIZE)`.
    - `(void)m_peer->inject(delayed_envelopes[i])` — return value discarded. [Risk 4]
    - `B.inject(env)`: asserts `m_open`; calls `B.m_recv_queue.push(env)`.
    - `RingBuffer::push()`: loads `t = m_tail.load(relaxed)`, `h = m_head.load(acquire)`. If `t - h >= 64`: returns `ERR_FULL`; `inject()` logs `WARNING_HI`. Otherwise: `envelope_copy(m_buf[t & RING_MASK], env)`; stores `m_tail.store(t+1, release)`.

13. **Inject primary envelope.** `res = m_peer->inject(envelope)` — same path as step 12 but return value is checked. Post-condition assert: `res == OK || res == ERR_FULL`. Returns `res`.

**Phase 3 — `B.receive_message(envelope, timeout_ms)`**

14. Entry assertion: `NEVER_COMPILED_OUT_ASSERT(m_open)`.

15. **Fast-path pop.** `res = B.m_recv_queue.pop(envelope)`:
    - Loads `h = m_head.load(relaxed)`, `t = m_tail.load(acquire)`.
    - If `t - h == 0`: returns `ERR_EMPTY`.
    - Else: `envelope_copy(envelope, m_buf[h & RING_MASK])`; stores `m_head.store(h+1, release)`; returns `OK`.
    - If `OK`: return `OK` immediately (typical path when queue was populated in Phase 2).

16. **Zero-timeout check.** If `timeout_ms == 0U`: return `ERR_TIMEOUT` immediately (non-blocking).

17. **Bounded poll loop.** `iterations = min(timeout_ms, 5000U)`. For `i = 0..iterations-1`:
    - `nanosleep({0, 1000000})` — sleeps ~1 ms. Return value `(void)` cast.
    - `res = B.m_recv_queue.pop(envelope)`. If `OK`: return `OK`.
18. Return `ERR_TIMEOUT`.

---

## 4. Call Tree

```
A.link(&B)
 ├── NEVER_COMPILED_OUT_ASSERT(peer != nullptr, peer != this)
 ├── A.m_peer = &B
 └── Logger::log(INFO, ...)

A.send_message(envelope)
 ├── NEVER_COMPILED_OUT_ASSERT(m_open, m_peer != nullptr, envelope_valid)
 ├── timestamp_now_us()
 │    └── clock_gettime(CLOCK_MONOTONIC, &ts)
 ├── A.m_impairment.process_outbound(envelope, now_us)
 │    ├── is_partition_active(now_us)
 │    ├── check_loss()              [m_prng.next_double()]
 │    ├── queue_to_delay_buf(envelope, release_us)
 │    │    └── envelope_copy(m_delay_buf[slot].env, envelope)
 │    └── apply_duplication(...)    [maybe queue_to_delay_buf(+100µs)]
 ├── [if ERR_IO] → return OK        [silent drop]
 ├── A.m_impairment.collect_deliverable(now_us, delayed[32], 32)
 │    └── [loop 0..31] copy + deactivate if release_us <= now_us
 ├── [loop i=0..delayed_count-1]
 │    └── (void)B.inject(delayed[i])
 │         ├── NEVER_COMPILED_OUT_ASSERT(m_open)
 │         └── B.m_recv_queue.push(delayed[i])
 │              ├── m_tail.load(relaxed); m_head.load(acquire)
 │              ├── envelope_copy(m_buf[t & RING_MASK], delayed[i])
 │              └── m_tail.store(t+1, release)
 └── B.inject(envelope)             [primary envelope]
      └── B.m_recv_queue.push(envelope)

B.receive_message(envelope, timeout_ms)
 ├── NEVER_COMPILED_OUT_ASSERT(m_open)
 ├── B.m_recv_queue.pop(envelope)   [fast path]
 │    ├── m_head.load(relaxed); m_tail.load(acquire)
 │    ├── envelope_copy(envelope, m_buf[h & RING_MASK])
 │    └── m_head.store(h+1, release)
 └── [if ERR_EMPTY] poll loop
      ├── nanosleep({0, 1000000})   [POSIX syscall, ~1ms]
      └── B.m_recv_queue.pop(envelope)
```

---

## 5. Key Components Involved

- **`LocalSimHarness` (A — sender)** (`src/platform/LocalSimHarness.cpp/.hpp`): Owns `m_impairment` (inline `ImpairmentEngine`) and `m_peer` raw pointer to B. Applies all impairment decisions; calls `B.inject()`.
- **`LocalSimHarness` (B — receiver)**: Owns `m_recv_queue` (inline `RingBuffer`). `inject()` is the producer; `receive_message()` is the consumer.
- **`ImpairmentEngine`** (`src/platform/ImpairmentEngine.hpp`): Embedded in A. Decides drop (loss/partition), delay, duplicate. Stores delayed envelopes in `m_delay_buf[IMPAIR_DELAY_BUF_SIZE]`. SC: `process_outbound()` (HAZ-002, HAZ-007), `collect_deliverable()` (HAZ-004, HAZ-006).
- **`RingBuffer` (`B.m_recv_queue`)** (`src/core/RingBuffer.hpp`): SPSC lock-free FIFO. Capacity `MSG_RING_CAPACITY = 64`. `std::atomic<uint32_t>` head/tail. `inject()` is producer; `receive_message()` is consumer.
- **`PrngEngine`** (embedded in `ImpairmentEngine`): Seedable PRNG for all probability decisions. Consumed only when impairments enabled.
- **`timestamp_now_us()`** (`src/core/Timestamp.hpp`): Inline; `clock_gettime(CLOCK_MONOTONIC)`. The only syscall on the send path (excluding `nanosleep` on receive).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `link()`: `peer == nullptr` or `peer == this` | `NEVER_COMPILED_OUT_ASSERT` fires; program terminates | `m_peer = peer` |
| `process_outbound()` returns `ERR_IO` | Return `OK` (silent drop) | Proceed to `collect_deliverable()` |
| `process_outbound()` returns `ERR_FULL` | Falls through; primary envelope still injected without impairment tracking [Risk 3] | (same path) |
| Impairments disabled | Queue with `release_us = now_us` (immediate) | Full impairment chain (partition → loss → latency → queue) |
| `release_us <= now_us` in collect | Copy to output buffer; deactivate slot | Entry remains for next call |
| `B.inject()` (delayed) returns `ERR_FULL` | `WARNING_HI` logged inside `inject()`; `(void)` at call site [Risk 4] | Envelope queued |
| `B.inject()` (primary) returns `ERR_FULL` | `WARNING_HI` logged; `ERR_FULL` returned to caller | Return `OK` |
| `B.receive_message()` fast-path `pop()` returns `OK` | Return `OK` immediately | Check timeout_ms |
| `timeout_ms == 0` | Return `ERR_TIMEOUT` immediately | Enter `nanosleep` poll loop |

---

## 7. Concurrency / Threading Behavior

**RingBuffer SPSC contract:**
- Producer: thread calling `A.send_message()` via `B.inject()` → `B.m_recv_queue.push()`.
- Consumer: thread calling `B.receive_message()` via `B.m_recv_queue.pop()`.
- With exactly one producer and one consumer, acquire/release ordering on `m_tail` (producer stores release; consumer loads acquire) and `m_head` (consumer stores release; producer loads acquire) ensures correct visibility without a mutex.

**`A.m_impairment` state (`m_delay_buf`, `m_delay_count`, `m_prng`):** Exclusively owned by A. No lock. Multiple concurrent callers to `A.send_message()` would race on impairment state.

**`A.m_peer` pointer:** Set once by `link()` before concurrent use. All subsequent accesses are reads. Safe without atomics provided `link()` happens-before `send_message()` and `close()` is not called concurrently.

**`close()` race:** `close()` sets `m_peer = nullptr` and `m_open = false` without synchronization. A thread past the entry assertion at `send_message()` line 112 but not yet at line 137 will crash if `close()` nullifies `m_peer` concurrently. [Risk 7]

**`nanosleep` in `receive_message()`:** The calling thread sleeps ~1 ms per iteration. A concurrent `inject()` from a producer thread completes without waking the sleeper; the next `pop()` call acquires the new `m_tail` via the acquire load.

---

## 8. Memory & Ownership Semantics

- **`A.m_peer` (`LocalSimHarness*`)**: Raw non-owning pointer to B. A does not manage B's lifetime. B must remain alive for the duration of any `A.send_message()`. No RAII, no weak pointer.
- **`A.m_impairment.m_delay_buf[32]`** (inline `DelayEntry[32]` in `ImpairmentEngine`): Each `DelayEntry` holds a full `MessageEnvelope` copy (~4149 bytes). Total ≈ 133 KB embedded in A.
- **`delayed_envelopes[32]`**: Stack-local in `A.send_message()`. 32 × ~4140 bytes ≈ 132 KB stack usage per call. Populated by `collect_deliverable()` via `envelope_copy()`.
- **`B.m_recv_queue.m_buf[64]`**: Inline in `RingBuffer`, inline in B. 64 × ~4140 bytes ≈ 265 KB embedded in B.
- **`envelope_copy()`**: `memcpy(&dst, &src, sizeof(MessageEnvelope))`. Full deep copy including inline `payload[4096]`.
- **Constructor:** `m_peer(nullptr)`, `m_open(false)`. Assert `!m_open` in body.
- **Destructor:** `LocalSimHarness::close()` called as qualified non-virtual. Sets `m_peer = nullptr`, `m_open = false`.
- **No heap allocation** anywhere in this use case.

---

## 9. Error Handling Flow

| Error | Source | Handling |
|-------|--------|----------|
| Loss or partition drop | `process_outbound()` → `ERR_IO` | Intercepted; converted to `OK` (silent drop) |
| Delay buffer full | `process_outbound()` → `ERR_FULL` | Not intercepted; primary envelope still injected [Risk 3] |
| `inject()` (primary) returns `ERR_FULL` | `B.m_recv_queue.push()` | `WARNING_HI` logged; `ERR_FULL` returned to caller |
| `inject()` (delayed) returns `ERR_FULL` | `B.m_recv_queue.push()` | `WARNING_HI` logged; `(void)` cast at call site; caller unaware |
| `link()` precondition fails | `NEVER_COMPILED_OUT_ASSERT` | Program terminates unconditionally |
| `nanosleep` interrupted | `EINTR` | Return value `(void)`-cast; next `pop()` attempted immediately |
| `receive_message()` timeout | Loop exhausted | Return `ERR_TIMEOUT` |

---

## 10. External Interactions

- **`clock_gettime(2)`**: Inside `timestamp_now_us()` during `send_message()`. Non-blocking; `CLOCK_MONOTONIC`.
- **`nanosleep(2)`**: In `receive_message()` when queue initially empty. Called with `{tv_sec=0, tv_nsec=1000000}`. Suspends calling thread ~1 ms. Return value `(void)`-cast.
- **No socket calls** (`socket`, `bind`, `sendto`, `recvfrom`, `poll`, `close`) at any point. This is the key property enabling deterministic in-process testing.
- **Logger**: Called at `INFO` on link/init/close; `WARNING_HI` on queue full; `WARNING_LO` on partition/loss drop.

---

## 11. State Changes / Side Effects

| Object | Field | Change |
|--------|-------|--------|
| A | `m_peer` | Set to `&B` by `link()` |
| A.m_impairment | `m_delay_buf[slot].env` | Overwritten by `queue_to_delay_buf()` |
| A.m_impairment | `m_delay_buf[slot].release_us` | Set to `now_us` or `now_us + delay` |
| A.m_impairment | `m_delay_buf[slot].active` | Set true by queue; false by collect |
| A.m_impairment | `m_delay_count` | Incremented by queue; decremented by collect |
| A.m_impairment | `m_prng` (PrngEngine) | Advanced by loss/jitter/duplication draws |
| A.m_impairment | `m_partition_active`, `m_next_partition_event_us` | May change inside `is_partition_active()` |
| B.m_recv_queue | `m_buf[t & RING_MASK]` | Written by `envelope_copy` in `push()` |
| B.m_recv_queue | `m_tail` (atomic) | Incremented on each `push()` (release store) |
| B.m_recv_queue | `m_head` (atomic) | Incremented on each `pop()` (release store) |
| `envelope` (out) | All fields + payload | Written by `receive_message()` pop on success |
| Not changed | `B.m_impairment`, `A.m_recv_queue` | Dormant; not used in this flow |

---

## 12. Sequence Diagram

```
TestFixture       LocalSimHarness A     ImpairmentEngine A     LocalSimHarness B     RingBuffer B
     |                   |                      |                      |                   |
     |--link(&B)-------->|                      |                      |                   |
     |                   | m_peer = &B          |                      |                   |
     |--send_message(env)->                     |                      |                   |
     |                   |--timestamp_now_us()  |                      |                   |
     |                   |--process_outbound(env, now_us)------------->|                   |
     |                   | [disabled: queue release_us=now_us]         |                   |
     |                   |<--Result::OK---------|                      |                   |
     |                   |--collect_deliverable(now_us, 32)----------->|                   |
     |                   | [delayed_count=1, release_us<=now_us]       |                   |
     |                   |<--delayed[0]=env-----|                      |                   |
     |                   |--inject(delayed[0])------------------------->|                   |
     |                   |                      |                      |--push(delayed[0])->|
     |                   |                      |                      |<--OK--------------|
     |                   |--inject(envelope)----------------------------->                  |
     |                   |                      |                      |--push(envelope)--->|
     |                   |                      |                      |<--OK--------------|
     |<--Result::OK------|                      |                      |                   |
     |--receive_message(out, timeout_ms)--------|--------------------->|                   |
     |                   |                      |                      |--pop(out)-------->|
     |                   |                      |                      |<--OK, out=env-----|
     |<--Result::OK [out populated]-------------|--------------------->|                   |
```

**Drop path:** `process_outbound()` → `ERR_IO` → `send_message()` returns `OK` with no `inject()` call.

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `LocalSimHarness()` constructor: `m_peer(nullptr)`, `m_open(false)`.
- `LocalSimHarness::init(config)`: Asserts `config.kind == TransportKind::LOCAL_SIM` and `!m_open`. Calls `m_recv_queue.init()` (atomics to 0). Calls `impairment_config_default(imp_cfg)` (all impairments disabled, `prng_seed=42`); overrides from `config.channels[0].impairment` if `num_channels > 0`. Calls `m_impairment.init(imp_cfg)` (seeds PRNG, zeroes delay/reorder buffers). Sets `m_open = true`. Logs `INFO`.
- `link(peer)`: Must be called after both instances have `init()`'d and before any `send_message()`.
- No heap allocation at any point.

**Steady-state runtime:** No allocation, no socket I/O. Operations are in-memory `envelope_copy()` calls and atomic ring buffer index updates. Single external interaction: `nanosleep(1ms)` in `receive_message()` when queue is initially empty.

---

## 14. Known Risks

- **Risk 1: Double injection when latency impairment is active.** When `fixed_latency_ms > 0` and `release_us > now_us`, `collect_deliverable()` returns 0 delayed entries. But `send_message()` still calls `B.inject(envelope)` at the primary injection step. The message is delivered to B immediately AND a copy sits in `m_delay_buf` for injection on the next `send_message()` call's `collect_deliverable()` scan. This produces an unintended duplicate not attributable to the duplication impairment. A correctness bug.
- **Risk 2: B.m_impairment never used on receive path.** `B.m_impairment` is initialized but never called by `receive_message()` or `inject()`. All impairment is applied at the sender (A) side only.
- **Risk 3: ERR_FULL from process_outbound not handled.** If `A.m_delay_buf` is full, `process_outbound()` returns `ERR_FULL`. Only `ERR_IO` is intercepted; `ERR_FULL` falls through. The primary envelope is still injected without impairment tracking.
- **Risk 4: inject() result for delayed envelopes discarded.** `(void)m_peer->inject(delayed[i])`. Caller cannot detect that a delayed message was dropped due to queue full.
- **Risk 5: Iteration cap of 5000 truncates long timeouts.** A caller requesting `timeout_ms = 10000` receives `ERR_TIMEOUT` after ~5000 ms. The truncation is silent.
- **Risk 6: m_peer lifetime is caller responsibility.** If B is destroyed before `A.send_message()` completes, the dereference at `inject()` is a use-after-free.
- **Risk 7: close() races with concurrent send/receive.** `close()` sets `m_peer = nullptr` and `m_open = false` without synchronization. A thread past the entry assertion but not yet at the `inject()` call will crash.
- **Risk 8: Unidirectional linking.** `A.link(&B)` enables only A→B delivery. `B.link(&A)` must also be called for B→A.

---

## 15. Unknowns / Assumptions

- [CONFIRMED] `timestamp_now_us()` calls `clock_gettime(CLOCK_MONOTONIC, &ts)` and returns `ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL`. Monotonically increasing.
- [CONFIRMED] `envelope_copy()` is `memcpy(&dst, &src, sizeof(MessageEnvelope))`. Full deep copy including inline `payload[4096]`.
- [CONFIRMED] `impairment_config_default()` sets `enabled=false`, all probabilities and delays to 0, `prng_seed=42ULL`. A freshly initialized harness is a transparent pass-through unless impairments are configured.
- [CONFIRMED] `MSG_RING_CAPACITY = 64U`, `IMPAIR_DELAY_BUF_SIZE = 32U` from `Types.hpp`.
- [CONFIRMED] `NEVER_COMPILED_OUT_ASSERT` fires unconditionally regardless of `NDEBUG`. Defined in `src/core/Assert.hpp`.
- [ASSUMPTION] `Logger::log()` is safe to call from the calling thread context.
- [ASSUMPTION] `A.link(&B)` is called after both `A.init()` and `B.init()` return `OK`, and before any concurrent send/receive. The code does not enforce this ordering.
