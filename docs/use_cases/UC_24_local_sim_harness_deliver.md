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
5. **`m_impairment.process_outbound(envelope, now_us)`** — applies outbound impairments. ERR_IO = loss-dropped; ERR_FULL = delay buffer saturated.
6. **`m_impairment.collect_deliverable(now_us, out_buf, buf_cap)`** — retrieves due envelopes (immediately when no latency configured).
7. **`flush_outbound_batch(envelope, out_buf, out_count, now_us)`** — for each due envelope, calls `m_peer->deliver_from_peer(env, now_us)`.
8. Inside **`deliver_from_peer(env, now_us)`** on B:
   a. `m_impairment.is_partition_active(now_us)` — if partition active, drops and returns `ERR_IO`.
   b. `m_impairment.process_inbound(env, now_us, &out, 1U, &count)` — applies reorder; count=0 means buffered (`ERR_TIMEOUT`), count=1 means pass through.
   c. `m_recv_queue.push(inbound_out)` — queue; `ERR_FULL` if ring full.
9. `flush_outbound_batch()` propagates `ERR_IO` or `ERR_FULL` for the current envelope to the caller.
10. Returns `Result::OK` on success.

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
LocalSimHarness::send_message(envelope)                [LocalSimHarness.cpp]
 ├── ImpairmentEngine::process_outbound()              [ImpairmentEngine.cpp]
 ├── ImpairmentEngine::collect_deliverable()
 └── LocalSimHarness::flush_outbound_batch(...)        [private helper]
      └── LocalSimHarness_B::deliver_from_peer(env, now_us) [peer's private helper]
           ├── ImpairmentEngine_B::is_partition_active()    [inbound partition check]
           ├── ImpairmentEngine_B::process_inbound()        [inbound reorder]
           └── RingBuffer_B::push(inbound_out)              [peer's m_recv_queue]

LocalSimHarness::receive_message(out, timeout_ms) [LocalSimHarness.cpp]
 ├── RingBuffer::pop(out)
 └── [nanosleep loop:]
      └── RingBuffer::pop(out)
```

---

## 5. Key Components Involved

- **`LocalSimHarness`** — In-process transport. `send_message()` calls `m_peer->deliver_from_peer()` to route through the peer's inbound impairment before enqueuing. No sockets involved.
- **`ImpairmentEngine`** (sender A) — Applied outbound: loss/delay/duplication affect whether and when the envelope reaches `flush_outbound_batch()`.
- **`ImpairmentEngine`** (receiver B) — Applied inbound inside `deliver_from_peer()`: partition check via `is_partition_active()` drops the message; reorder simulation via `process_inbound()` may buffer it.
- **`LocalSimHarness::inject()`** — Raw test hook that bypasses all impairment; pushes directly to `m_recv_queue`. NOT called during linked-peer delivery.
- **`RingBuffer`** — SPSC lock-free queue. Producer: `deliver_from_peer()` on harness A's thread. Consumer: `pop()` on harness B's `receive_message()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_peer == nullptr` | Assert fires | Proceed |
| `process_outbound()` drops (loss/partition outbound) | Return OK (silent drop) | Proceed to collect_deliverable |
| `deliver_from_peer()` inbound partition active | Return ERR_IO to flush_outbound_batch | Push to recv queue |
| `deliver_from_peer()` reorder-buffered (count==0) | Return ERR_TIMEOUT; not queued now | Continue loop |
| `deliver_from_peer()` returns ERR_FULL (ring full) | Current: propagate ERR_FULL; non-current: log | OK |
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
       -> ImpairmentEngine_A::process_outbound()      [outbound impairments; pass-through if none]
       -> ImpairmentEngine_A::collect_deliverable()   [out_count=1 when no latency]
       -> LocalSimHarness_A::flush_outbound_batch()
            -> LocalSimHarness_B::deliver_from_peer(envelope, now_us)
                 -> ImpairmentEngine_B::is_partition_active() [inbound partition check]
                 -> ImpairmentEngine_B::process_inbound()     [inbound reorder pass-through]
                 -> RingBuffer_B::push(envelope)              [atomic m_head release]
                 <- Result::OK
            <- Result::OK
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
