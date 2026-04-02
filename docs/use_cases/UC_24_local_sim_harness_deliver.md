# UC_24 — LocalSimHarness: send on one harness, receive on linked peer

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User
**Requirement traceability:** REQ-5.3.2, REQ-4.1.2, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** User calls `LocalSimHarness::send_message(envelope)` on harness A, which is linked to harness B via `link()`. File: `src/platform/LocalSimHarness.cpp`.
- **Goal:** Deliver the envelope in-process from harness A to harness B's receive queue without any real sockets, respecting configured impairments.
- **Success outcome:** `send_message()` on A returns `Result::OK`. The envelope is pushed into harness B's `m_recv_queue`. A subsequent `receive_message()` on B returns `Result::OK` with the envelope.
- **Error outcomes:**
  - `Result::ERR_FULL` — harness B's ring buffer is at capacity.
  - Impairment loss/partition silently drops without error.
  - `Result::ERR_NOT_INIT` — `m_peer` is null (harnesses not linked).

---

## 2. Entry Points

```cpp
// src/platform/LocalSimHarness.cpp
Result LocalSimHarness::send_message(const MessageEnvelope& envelope) override;
Result LocalSimHarness::receive_message(MessageEnvelope& out, uint32_t timeout_ms) override;
void   LocalSimHarness::link(LocalSimHarness* peer);
```

---

## 3. End-to-End Control Flow

**Send on harness A:**
1. `LocalSimHarness::send_message(envelope)` — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. `NEVER_COMPILED_OUT_ASSERT(m_peer != nullptr)` — linked.
4. `uint64_t now_us = timestamp_now_us()`.
5. **`m_impairment.process_outbound(envelope, now_us, delay_buf, &delay_count)`** — applies impairments. Default: `delay_buf[0] = envelope`, `delay_count = 1`.
6. **`m_impairment.collect_deliverable(delay_buf, delay_count, now_us, out_buf, &out_count)`** — retrieves due envelopes (immediately, no latency configured).
7. For each `out_buf[i]`: **`m_peer->inject(out_buf[i])`** — pushes the envelope directly into harness B's `m_recv_queue` via `RingBuffer::push()`.
8. If `inject()` returns `ERR_FULL`: return `ERR_FULL`.
9. Returns `Result::OK`.

**Receive on harness B:**
1. `LocalSimHarness::receive_message(out, timeout_ms)`.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. Try `m_recv_queue.pop(out)` — if data available: return `OK`.
4. **Bounded nanosleep loop** (Power of 10 Rule 2: `min(timeout_ms, 5000)` iterations of 1ms `nanosleep`):
   a. `nanosleep(&ts, nullptr)` — sleep 1ms.
   b. `m_recv_queue.pop(out)` — if data: return `OK`.
5. After loop: return `ERR_TIMEOUT`.

---

## 4. Call Tree

```
LocalSimHarness::send_message(envelope)          [LocalSimHarness.cpp]
 ├── ImpairmentEngine::process_outbound()        [ImpairmentEngine.cpp]
 ├── ImpairmentEngine::collect_deliverable()
 └── LocalSimHarness::inject(out_buf[i])         [peer's method]
      └── RingBuffer::push(envelope)             [peer's m_recv_queue; RingBuffer.hpp]

LocalSimHarness::receive_message(out, timeout_ms) [LocalSimHarness.cpp]
 ├── RingBuffer::pop(out)
 └── [nanosleep loop:]
      └── RingBuffer::pop(out)
```

---

## 5. Key Components Involved

- **`LocalSimHarness`** — In-process transport. `send_message()` calls `m_peer->inject()` to directly enqueue to the peer's ring buffer. No sockets involved.
- **`ImpairmentEngine`** — Applied on the sender side (harness A). Loss/delay/duplication affect what reaches the peer.
- **`LocalSimHarness::inject()`** — Bypasses the sender's `send_message()` path; directly pushes to `m_recv_queue`.
- **`RingBuffer`** — SPSC lock-free queue. Producer: `inject()` on harness A's thread (same thread in this single-threaded pattern). Consumer: `pop()` on harness B's `receive_message()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_peer == nullptr` | Assert fires | Proceed |
| `process_outbound()` drops | `out_count=0`; no inject | inject to peer |
| `m_peer->inject()` returns ERR_FULL | Return ERR_FULL | Return OK |
| `m_recv_queue.pop()` succeeds immediately | Return OK | Enter nanosleep loop |
| Loop exhausted | Return ERR_TIMEOUT | Continue |

---

## 7. Concurrency / Threading Behavior

- In the typical single-thread test pattern: both A and B are driven from the same application thread.
- `RingBuffer::push()` — atomic `m_head` release store. `pop()` — atomic `m_tail` acquire load.
- Thread-safe for SPSC use: one producer (inject on harness A's thread) and one consumer (pop on harness B's thread). In a single-thread test these are the same thread.

---

## 8. Memory & Ownership Semantics

- `RingBuffer::m_buf[MSG_RING_CAPACITY]` — 64-slot array inside harness B's `m_recv_queue`. Envelopes stored by value.
- `delay_buf[IMPAIR_DELAY_BUF_SIZE]` — 32-slot stack array in `send_message()`.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **`ERR_FULL`:** Peer ring buffer at capacity. Caller should drain B before sending more.
- **`ERR_TIMEOUT`:** B's receive queue empty for `timeout_ms`. Normal in tests with sufficient delay.

---

## 10. External Interactions

- None — operates entirely in process memory. No sockets, no OS I/O (except `nanosleep` in `receive_message()`).

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RingBuffer` (B) | `m_buf[m_head]` | stale | new `MessageEnvelope` |
| `RingBuffer` (B) | `m_head` (atomic) | H | H+1 (release) |

---

## 12. Sequence Diagram

```
User
  -> LocalSimHarness_A::send_message(envelope)
       -> ImpairmentEngine::process_outbound()   [pass-through; no impairments]
       -> ImpairmentEngine::collect_deliverable() [out_count=1]
       -> LocalSimHarness_B::inject(envelope)
            -> RingBuffer_B::push(envelope)       [atomic m_head release]
       <- Result::OK

User
  -> LocalSimHarness_B::receive_message(out, 100)
       -> RingBuffer_B::pop(out)                  [atomic m_tail acquire]
       <- Result::OK; out populated
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Both harnesses initialized via `init()` with `TransportKind::LOCAL_SIM`.
- `harness_a.link(&harness_b)` called — sets `m_peer = &harness_b`.
- For bidirectional: `harness_b.link(&harness_a)` also called.

**Runtime:**
- `send_message()` on A directly pushes to B's ring buffer in the same call stack. There is no background thread or deferred delivery unless impairment latency is configured.

---

## 14. Known Risks / Observations

- **`nanosleep` loop in `receive_message()`:** Blocks the calling thread for up to `min(timeout_ms, 5000)` ms in 1ms increments. Suitable for test code; not for production.
- **No actual network:** LocalSimHarness provides functional equivalence only. Real network latency, jitter, and OS scheduling are not modeled.
- **Bidirectional link required for full exchange:** `harness_a.link(&harness_b)` only sets A's peer. B's replies must go through `harness_b.link(&harness_a)`.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `inject()` is a public method of `LocalSimHarness` that calls `m_recv_queue.push()` directly, bypassing `send_message()` and the impairment engine. This is confirmed by the test `test_receive_data_best_effort` which calls `harness_a.inject(inject_env)`.
