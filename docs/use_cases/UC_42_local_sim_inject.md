# UC_42 — LocalSimHarness inject(): test code directly injects an envelope without going through send_message()

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User
**Requirement traceability:** REQ-5.3.2, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** User (typically test code) calls `LocalSimHarness::inject(envelope)` directly. File: `src/platform/LocalSimHarness.cpp`.
- **Goal:** Bypass `send_message()` and its impairment pipeline to place an envelope directly into the harness's own receive ring buffer, simulating a message that has already been delivered to this endpoint.
- **Success outcome:** The envelope is pushed into `m_recv_queue`. A subsequent call to `receive_message()` on the same harness instance returns this envelope. Returns `Result::OK`.
- **Error outcomes:**
  - `Result::ERR_FULL` — `m_recv_queue` is at capacity (`MSG_RING_CAPACITY = 64`); envelope is dropped.

---

## 2. Entry Points

```cpp
// src/platform/LocalSimHarness.cpp
Result LocalSimHarness::inject(const MessageEnvelope& envelope);
```

---

## 3. End-to-End Control Flow

1. **`LocalSimHarness::inject(envelope)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. **`m_recv_queue.push(envelope)`** — attempt to push the envelope into the SPSC ring buffer.
   - `RingBuffer::push()`: loads `m_head` (relaxed), computes next_head, checks if `next_head == m_tail` (acquire). If equal: queue full; returns false.
   - If full: return `Result::ERR_FULL`.
   - Writes envelope into `m_buf[m_head]`; stores `m_head = next_head` (release).
4. Returns `Result::OK`.

---

## 4. Call Tree

```
LocalSimHarness::inject(envelope)                  [LocalSimHarness.cpp]
 └── RingBuffer::push(envelope)                    [RingBuffer.hpp]
      ├── m_head.load(relaxed)
      ├── m_tail.load(acquire)                     [full check]
      └── m_head.store(next_head, release)         [commit]
```

---

## 5. Key Components Involved

- **`inject()`** — test-only entry point that bypasses the impairment pipeline. Enables test code to deliver specific envelopes directly for boundary-condition testing (e.g., injecting duplicates, expired messages, or specific message IDs).
- **`RingBuffer::push()`** — SPSC lock-free push; places the envelope at the producer head slot. Atomic release store on `m_head` makes the envelope visible to the consumer.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_recv_queue` full (next_head == m_tail) | Return ERR_FULL | Push envelope; return OK |

---

## 7. Concurrency / Threading Behavior

- `inject()` acts as the **producer** on `m_recv_queue` (same role as the internal receive path in `send_message()` → peer harness).
- `receive_message()` is the **consumer** on `m_recv_queue`.
- SPSC contract: `inject()` and `receive_message()` must be called from different threads (or sequentially) — not simultaneously from multiple producers.
- `RingBuffer::push()` uses `m_head.load(relaxed)` and `m_head.store(next_head, release)`.
- `RingBuffer::pop()` uses `m_tail.load(relaxed)` and `m_tail.store(next_tail, release)`.
- The release store on `m_head` synchronizes with the acquire load on `m_head` in the consumer.

---

## 8. Memory & Ownership Semantics

- `m_recv_queue` — `RingBuffer` with `MSG_RING_CAPACITY = 64` slots; each slot holds one `MessageEnvelope` (fixed-size struct, ~4140 bytes each).
- `m_recv_queue.m_buf[MSG_RING_CAPACITY]` — fixed array; no heap allocation. Power of 10 Rule 3 compliant.
- The envelope is copied by value into the ring buffer slot; no pointer aliasing.

---

## 9. Error Handling Flow

- **`ERR_FULL`:** Ring buffer at capacity. The injected envelope is dropped. Caller must drain the queue (via `receive_message()`) before injecting more. This is a test-time concern; production code does not call `inject()`.
- No other error states. `m_open` is asserted; calling `inject()` on a closed harness triggers `NEVER_COMPILED_OUT_ASSERT`.

---

## 10. External Interactions

- None — this flow operates entirely in process memory. No sockets, file I/O, or OS calls.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `LocalSimHarness` | `m_recv_queue.m_buf[m_head]` | undefined | copy of `envelope` |
| `LocalSimHarness` | `m_recv_queue.m_head` | H | H+1 (mod MSG_RING_CAPACITY) |

---

## 12. Sequence Diagram

```
User -> LocalSimHarness::inject(envelope)
  -> RingBuffer::push(envelope)
       [m_head.load(relaxed)]
       [m_tail.load(acquire) — full check]
       [m_buf[m_head] = envelope]
       [m_head.store(next_head, release)]
       <- true (success)
  <- Result::OK

[User -> LocalSimHarness::receive_message()]
  -> RingBuffer::pop(out_env)       [consumes the injected envelope]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `LocalSimHarness::init()` must have been called; `m_open == true`.
- `m_recv_queue.init()` must have been called (done inside `init()`).

**Runtime:**
- `inject()` is typically called in test setup to pre-populate the receive queue before exercising `receive_message()` behavior. It can also be called mid-test to simulate message arrival.

---

## 14. Known Risks / Observations

- **Bypasses impairment:** `inject()` skips the `ImpairmentEngine` entirely. Injected envelopes are not subject to loss, delay, or duplication. This is intentional for testing specific receive-path behavior.
- **SPSC violation risk:** If multiple threads call `inject()` concurrently on the same harness, the SPSC contract is violated and the ring buffer can corrupt. Test code must ensure single-producer discipline.
- **Capacity limit:** `MSG_RING_CAPACITY = 64`. Tests that inject more than 64 messages without consuming any will silently drop envelopes.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `inject()` pushes directly to `m_recv_queue` (the same ring buffer populated by the receive side of `send_message()`). This is inferred from the purpose of the function and the harness architecture.
- `[ASSUMPTION]` `inject()` is declared as a public member of `LocalSimHarness` (not a free function) and is not part of the `TransportInterface`; it is a test-only extension.
