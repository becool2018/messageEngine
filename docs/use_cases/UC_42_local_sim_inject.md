# UC_42 — LocalSimHarness inject(): test code directly injects an envelope without going through send_message()

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User (test code)
**Requirement traceability:** REQ-4.1.2, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** Test code calls `LocalSimHarness::inject(envelope)` directly on a harness instance. File: `src/platform/LocalSimHarness.cpp`.

**Goal:** Bypass the impairment engine entirely and place an envelope directly into the harness's receive queue, allowing tests to inject specific scenarios (e.g., pre-built ACK envelopes, out-of-order sequences, or protocol control messages) without going through `send_message()`.

**Success outcome:** Returns `Result::OK`. `envelope` is now in `m_recv_queue` and will be returned by the next call to `receive_message()` on this harness.

**Error outcomes:**
- `Result::ERR_FULL` — `m_recv_queue` is at capacity (`MSG_RING_CAPACITY=64`). The envelope is dropped; a WARNING_HI is logged.

**Why it is factored out:** `inject()` is the primitive that `send_message()` uses to deliver messages into the peer harness. By exposing it as a public method, test code can push envelopes into a harness from a direction that `send_message()` cannot (e.g., injecting into the *local* receive queue, injecting under impairment conditions that would drop the message, or injecting a crafted envelope that was never serialised).

---

## 2. Entry Points

```cpp
Result LocalSimHarness::inject(const MessageEnvelope& envelope)
    // src/platform/LocalSimHarness.cpp:91

// Also called internally by:
Result LocalSimHarness::send_message(const MessageEnvelope& envelope)
    // src/platform/LocalSimHarness.cpp:109
```

In UC_42 the caller is **test code** that calls `inject()` directly. In UC_24 / UC_30 `inject()` is called by `send_message()` on the peer harness.

---

## 3. End-to-End Control Flow

1. Test code calls `harness.inject(envelope)`.
2. Assertion: `m_open` — the harness must be initialised.
3. `m_recv_queue.push(envelope)` — attempts to enqueue `envelope` in the SPSC RingBuffer.
   - **On success (`Result::OK`):** `m_recv_queue` now contains the envelope.
   - **On failure (`Result::ERR_FULL`):** Queue is at `MSG_RING_CAPACITY (64)`. Logs WARNING_HI "Receive queue full; dropping message".
4. Assertion: `res == Result::OK || res == Result::ERR_FULL` — post-condition: result must be one of the two valid codes.
5. Return `res`.

---

## 4. Call Tree

```
LocalSimHarness::inject()
 ├── NEVER_COMPILED_OUT_ASSERT(m_open)
 ├── m_recv_queue.push(envelope)      [RingBuffer]
 │    ├── [acquire-load m_head]
 │    ├── [compute next_tail]
 │    ├── [if full: return ERR_FULL]
 │    ├── [write envelope to slot]
 │    └── [release-store m_tail]
 ├── [if !OK] Logger::log(WARNING_HI "queue full")
 ├── NEVER_COMPILED_OUT_ASSERT(res == OK || ERR_FULL)
 └── return res
```

---

## 5. Key Components Involved

- **`LocalSimHarness::inject()`**: The sole write path into `m_recv_queue` from outside the impairment engine. Accepts any valid `MessageEnvelope` without serialisation, deserialisation, or impairment processing.
- **`RingBuffer` (`m_recv_queue`)** (`src/core/RingBuffer.hpp`): SPSC lock-free FIFO backed by `std::atomic<uint32_t>` head/tail pointers. `push()` uses a release store on `m_tail`; `pop()` uses an acquire load on `m_tail`. Capacity: `MSG_RING_CAPACITY=64` slots.
- **`MessageEnvelope`**: Copied by value into the ring buffer slot. No pointer aliasing; no ownership transfer.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|---|---|---|
| `m_open == false` | `NEVER_COMPILED_OUT_ASSERT` fires (abort or FATAL handler) | Continue |
| `m_recv_queue.push()` returns `ERR_FULL` | Log WARNING_HI, return `ERR_FULL` | Return `OK` |

No other branches exist in this function.

---

## 7. Concurrency / Threading Behavior

- **`RingBuffer` SPSC contract:** `push()` is safe for one producer thread; `pop()` is safe for one consumer thread. `inject()` is the producer; `receive_message()` (which calls `m_recv_queue.pop()`) is the consumer.
- If test code calls `inject()` from one thread while another thread calls `receive_message()` on the same harness, the SPSC contract is satisfied and the operation is lock-free.
- If two threads simultaneously call `inject()` on the same harness instance, the SPSC contract is violated — this is undefined behaviour and must be avoided.
- **Atomic operations:** `RingBuffer::push()` performs an **acquire load** on `m_head` (to check capacity) and a **release store** on `m_tail` (to publish the new element). These are the only atomics in this flow.

---

## 8. Memory & Ownership Semantics

- **No stack buffers of significance.** `inject()` only stores the local `res` variable.
- **`RingBuffer` internal storage:** Fixed-capacity array of `MessageEnvelope` values inside the RingBuffer member. `push()` copies `envelope` by value into `m_buf[next_tail % capacity]`.
- **Power of 10 Rule 3:** No heap allocation. The RingBuffer is a fixed-size member array; push/pop operate in-place.
- **Lifetime:** The copied `MessageEnvelope` in the ring buffer slot lives until `pop()` removes it. The caller's `envelope` argument may be freed after `inject()` returns.

---

## 9. Error Handling Flow

| Error condition | Result code | State | Caller action |
|---|---|---|---|
| Queue full | `ERR_FULL` | Envelope dropped; queue unchanged | Test should drain the queue before injecting; or reduce injection rate |

`ERR_FULL` does not indicate a system failure — it is a normal capacity overflow signal. The harness remains fully operational.

---

## 10. External Interactions

None — `inject()` operates entirely in process memory. No socket calls, no file I/O, no OS API calls.

---

## 11. State Changes / Side Effects

| Object / member | Before `inject()` | After successful `inject()` |
|---|---|---|
| `m_recv_queue` internal tail | position T | position T+1 |
| `m_recv_queue` slot at T | arbitrary | copy of `envelope` |
| Logger | — | No entry on success; WARNING_HI on `ERR_FULL` |

No other state is modified.

---

## 12. Sequence Diagram

```
Test code             LocalSimHarness        RingBuffer
 |                          |                    |
 | inject(envelope)         |                    |
 |------------------------->|                    |
 |                          | ASSERT(m_open)     |
 |                          | push(envelope)     |
 |                          |------------------> |
 |                          |                    | [acquire head]
 |                          |                    | [check capacity]
 |                          |                    | [copy envelope to slot]
 |                          |                    | [release store tail]
 |                          | <-- OK or ERR_FULL |
 |                          | ASSERT(res valid)  |
 | <-- Result::OK --------- |                    |
```

```
[Later, in application or test thread:]
harness.receive_message(env, timeout_ms)
 └── m_recv_queue.pop(env)    [acquire load tail → copy envelope out → release store head]
 └── return OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `m_open == true` (harness was successfully `init()`'d).
- No precondition on peer linkage: `inject()` writes to the local receive queue; the harness does not need to be linked to a peer for `inject()` to work.

**Runtime:** `inject()` is a fully reentrant-style single-step operation (acquire + copy + release). It can be called at any point after `init()` and before `close()`.

---

## 14. Known Risks / Observations

- **Bypasses impairment engine:** This is the intended behavior for test injection, but calling `inject()` from production code would silently circumvent all configured loss/latency/reorder impairments. There is no compile-time guard preventing production use.
- **SPSC constraint:** The RingBuffer's lock-free properties are only valid for a single producer and single consumer. Tests that inject from multiple threads simultaneously will corrupt the buffer without any detected error.
- **`MSG_RING_CAPACITY=64` limit:** If a test injects 64 messages without any `receive_message()` draining the queue, the 65th inject returns `ERR_FULL` silently (only a WARNING_HI log). Tests must consume or account for queue capacity.
- **No envelope validation:** `inject()` does not call `envelope_valid()`. If test code injects a malformed envelope, `DeliveryEngine::receive()` upstream may reject it or exhibit undefined filtering behavior.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `RingBuffer::push()` uses a **release store** on `m_tail` and `pop()` uses an **acquire load** on `m_tail`. This ensures the envelope write is visible to the consumer before the tail update, satisfying the SPSC memory ordering guarantee. This ordering was confirmed by reading the RingBuffer implementation comments but the exact `std::memory_order` values are inferred from the SPSC literature and the "acquire/release" annotations in the source comments.
- `[ASSUMPTION]` `MessageEnvelope` copy semantics are trivial (plain struct, no pointers to heap memory). A bitwise copy from `push()` into the ring buffer slot is sufficient for correct delivery. Confirmed by reading `src/core/MessageEnvelope.hpp`.
