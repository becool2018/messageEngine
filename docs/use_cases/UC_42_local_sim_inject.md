# UC_42 — LocalSimHarness inject(): test code directly injects an envelope without going through send_message()

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User (test code)
**Requirement traceability:** REQ-4.1.3, REQ-5.3.2

---

## 1. Use Case Overview

**Trigger:** Test code (or a linked peer harness) calls `LocalSimHarness::inject(envelope)` directly on a harness instance that has been successfully initialised.

**Goal:** Push an `MessageEnvelope` directly into the harness's inbound receive queue (`m_recv_queue`), bypassing the impairment engine entirely. The envelope is immediately available for the next call to `receive_message()` on the same harness instance. This allows test code to inject arbitrary envelopes — including envelopes that would be dropped by the impairment engine under `send_message()` — for precise control of test scenarios.

**Success outcome:** Returns `Result::OK`. The envelope is at the back of `m_recv_queue`. `receive_message()` on this harness will return it (in FIFO order) on its next call.

**Error outcomes:**
- `m_open == false` at entry → `NEVER_COMPILED_OUT_ASSERT` fires (precondition violation; not a recoverable error).
- `m_recv_queue` is full → returns `Result::ERR_FULL`; envelope is dropped; WARNING_HI is logged.

**System Internal note:** `inject()` is also called by the linked peer harness inside `send_message()` to deliver envelopes that survived the impairment engine. In that context the caller is the linked `LocalSimHarness`'s own `send_message()`, not external test code. This UC focuses on the direct test-code invocation path.

**Contrast with `send_message()`:**
- `send_message()` → applies impairment engine (loss, latency, duplication, reordering, partition) → calls `m_peer->inject()` for surviving messages.
- `inject()` → directly calls `m_recv_queue.push(envelope)` with no impairment logic.

---

## 2. Entry Points

```cpp
// src/platform/LocalSimHarness.cpp, line 91
Result LocalSimHarness::inject(const MessageEnvelope& envelope)
```

Declared in:
```
// src/platform/LocalSimHarness.hpp, line 76
Result inject(const MessageEnvelope& envelope);
```

---

## 3. End-to-End Control Flow (Step-by-Step)

1. Test code holds a pointer or reference to the target `LocalSimHarness` instance (`harness_B`).
2. Test code constructs or obtains a `MessageEnvelope` object with the desired fields populated.
3. Test code calls `harness_B.inject(envelope)`.
4. `LocalSimHarness::inject()` enters. Assertion: `m_open` — the harness must have been initialised with a successful `init()` call; otherwise `NEVER_COMPILED_OUT_ASSERT` triggers.
5. `Result res = m_recv_queue.push(envelope)` — attempts to push the envelope into the fixed-capacity ring buffer `m_recv_queue` (capacity `MSG_RING_CAPACITY` = 64 entries).
6. Branch: `if (!result_ok(res))`:
   - True: `m_recv_queue` was full. `Logger::log(Severity::WARNING_HI, "LocalSimHarness", "Receive queue full; dropping message")` is called. `res` retains `Result::ERR_FULL`.
   - False: push succeeded; `res == Result::OK`.
7. Postcondition assertion: `res == Result::OK || res == Result::ERR_FULL` — confirms the only two valid outcomes.
8. Return `res`.
9. The envelope is now at the back of `m_recv_queue` (on success). The next call to `harness_B.receive_message()` will pop it in FIFO order.

---

## 4. Call Tree (Primary Success Path)

```
[Test code or LocalSimHarness::send_message()]
 └── LocalSimHarness::inject(envelope)
      └── m_recv_queue.push(envelope)     [RingBuffer::push()]
```

This is the shortest call tree in the codebase — two levels deep, no branches on the success path.

---

## 5. Key Components Involved

- **`LocalSimHarness::inject()`** — the entry point; performs one assertion, one push, one conditional log, one postcondition assertion.
- **`RingBuffer`** (`m_recv_queue`) — fixed-capacity ring buffer (capacity `MSG_RING_CAPACITY` = 64 `MessageEnvelope` slots); embedded in the `LocalSimHarness` object. `push()` returns `OK` on success, `ERR_FULL` when all 64 slots are occupied.
- **`MessageEnvelope`** — the payload; passed by const reference; copied into the ring buffer by `push()`. The caller retains ownership of the original.
- **`Logger::log()`** — called only on the error path (queue full).
- **`NEVER_COMPILED_OUT_ASSERT`** — guards the `m_open` precondition; never disabled; fires abort (debug) or registered reset handler (production) on violation.

**What `inject()` does NOT touch:**
- `m_impairment` (impairment engine) — not consulted.
- `m_peer` (linked peer harness) — not consulted.
- Any socket or file descriptor — not applicable (LocalSimHarness is in-process).

---

## 6. Branching Logic / Decision Points

| Condition | True path | False path |
|-----------|-----------|------------|
| `m_open == false` at entry | `NEVER_COMPILED_OUT_ASSERT` fires; execution does not continue | Proceed to `push()` |
| `m_recv_queue.push()` returns `ERR_FULL` | Log WARNING_HI; return `ERR_FULL` | Return `Result::OK` |

These are the only two decision points in the function. There is no retry, no backoff, and no partial success.

---

## 7. Concurrency / Threading Behavior

`LocalSimHarness` has no internal synchronisation. `m_recv_queue` is a plain `RingBuffer` with no mutex. If test code calls `inject()` concurrently from one thread while `receive_message()` is draining the queue from another thread, the behaviour is a data race and is undefined.

In the intended usage pattern for deterministic simulation testing:
- A single test thread calls `inject()` to preload envelopes.
- The same or a different thread calls `receive_message()` after the inject calls complete.
- No concurrent access to the same `LocalSimHarness` instance is expected.

No `std::atomic` is used. No mutex is held.

---

## 8. Memory and Ownership Semantics

**Stack allocations in `inject()`:**
- `Result res` (1 byte) — return value from `push()`.

**Heap allocations:**
- None. Power of 10 Rule 3 is fully satisfied.

**`m_recv_queue` (RingBuffer):**
- Embedded directly in the `LocalSimHarness` object (no dynamic allocation).
- Capacity: `MSG_RING_CAPACITY` = 64 `MessageEnvelope` slots.
- Each slot is a copy of the envelope; `push()` copies the caller's envelope into the ring. The caller's original is not modified or held by reference.

**Ownership:**
- The envelope passed to `inject()` is passed as `const MessageEnvelope&`. The `RingBuffer::push()` makes a value copy into the ring's fixed storage. After `inject()` returns, the caller's envelope object is no longer referenced by the harness.
- `m_recv_queue` is owned by the `LocalSimHarness` object. Its lifetime is the lifetime of the harness instance.

---

## 9. Error Handling Flow

| Error event | State after | Return value |
|-------------|-------------|--------------|
| `m_open == false` (precondition violation) | NEVER_COMPILED_OUT_ASSERT fires; no return | N/A |
| `m_recv_queue.push()` returns `ERR_FULL` | Envelope not queued; `m_recv_queue` unchanged | `ERR_FULL` |
| `m_recv_queue.push()` returns `OK` | Envelope appended to queue | `OK` |

The `ERR_FULL` error is a WARNING_HI event, not a FATAL event. Test code should check the return value and handle a full queue by either reading messages first or increasing `MSG_RING_CAPACITY` via recompilation if needed.

---

## 10. External Interactions

`inject()` has no external interactions:
- No socket I/O.
- No file I/O.
- No system calls.
- No network activity.

All work is in-process memory operations (copying `MessageEnvelope` into the ring buffer).

---

## 11. State Changes / Side Effects

| What changes | When | Description |
|---|---|---|
| `m_recv_queue` write position | `push()` succeeds | Tail pointer advances; one slot is filled with a copy of `envelope` |
| No other state changes | — | `m_open`, `m_peer`, `m_impairment` are not modified |

Side effects:
- WARNING_HI log entry if the queue is full.
- No other observable side effects (no network traffic, no timers, no metrics counters).

---

## 12. Sequence Diagram

**Direct injection by test code:**

```
Test code            harness_B              m_recv_queue (RingBuffer)
    |                     |                          |
    |-- inject(env) ------>|                          |
    |                     | (assert m_open)          |
    |                     |-- push(env) ------------->|
    |                     |<-- OK --------------------|
    |                     | (assert res == OK or ERR_FULL)
    |<-- Result::OK -------|                          |
    |                     |                          |
    |-- receive_message() ->|                         |
    |                     |-- pop(envelope) ---------->|
    |                     |<-- OK (returns env) -------|
    |<-- Result::OK -------|                          |
```

**Injection by linked peer during send_message() (internal path):**

```
harness_A::send_message(env)
    |
    |-- m_impairment.process_outbound()   [applies loss/latency/etc.]
    |   [message survives impairment]
    |-- m_peer->inject(env) -----------> harness_B::inject(env)
    |                                         |-- m_recv_queue.push(env)
    |                                         |<-- OK
    |<-- Result::OK                           |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (`init()`, called once before `inject()` is usable):**
- `LocalSimHarness::init()` calls `m_recv_queue.init()` to reset the ring buffer and `m_impairment.init(imp_cfg)` to configure the impairment engine.
- `m_open` is set to `true` only after `init()` succeeds.
- `inject()` asserts `m_open`; calling `inject()` before `init()` is a precondition violation.

**Runtime (after `init()` returns OK):**
- Test code or the linked peer calls `inject()` zero or more times to preload envelopes.
- `receive_message()` drains the queue in FIFO order.
- There is no limit on how many times `inject()` may be called per `receive_message()` cycle, as long as the queue does not overflow (64 entries maximum).

---

## 14. Known Risks / Observations

- **No envelope validation in `inject()`:** The function asserts `m_open` but does not call `envelope_valid(envelope)`. Contrast with `send_message()` which asserts `envelope_valid()`. A malformed envelope (e.g., `message_type == INVALID`) can be injected and will later be returned by `receive_message()` to the caller. For test code this is often intentional; for production code it would be a bug.
- **Queue depth shared between `inject()` and `recv_from_client()`-style paths:** In production usage `LocalSimHarness` is used without `inject()`. In test usage `inject()` is the primary way to populate the queue. If both paths are active simultaneously (e.g., linked peer delivers via `send_message()` while test code simultaneously calls `inject()`), the 64-entry queue could overflow faster than expected.
- **No notification to waiting `receive_message()`:** `inject()` pushes to the queue but does not signal any condition variable or wake-up mechanism. A `receive_message()` call blocked in the `nanosleep` polling loop will pick up the injected message on its next poll iteration (≤1 ms later), not immediately. This is acceptable for deterministic testing but could add up to 1 ms of latency per inject-then-receive cycle.
- **`ERR_FULL` is silent from the harness perspective:** If `inject()` returns `ERR_FULL`, the test code's envelope is silently dropped. If the test does not check the return value, the test may observe incorrect behaviour (e.g., `receive_message()` returning the wrong envelope or timing out) without a clear error.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `RingBuffer::push()` makes a full value copy of the `MessageEnvelope` into the ring's fixed storage. There are no internal pointers within `MessageEnvelope` that would dangle after the caller's envelope goes out of scope.
- [ASSUMPTION] `MSG_RING_CAPACITY` = 64 is sufficient for the intended test scenarios. Tests that need to inject more than 64 messages between consecutive `receive_message()` drains must drain the queue to make room.
- [ASSUMPTION] The harness instance on which `inject()` is called is the harness that the test expects to read from via `receive_message()`. Calling `inject()` on `harness_A` when the test reads from `harness_B` is a test logic error that this function cannot detect.
- [ASSUMPTION] `envelope_valid()` is not checked in `inject()` deliberately, to allow test code to inject intentionally malformed envelopes for negative-path testing (e.g., testing how `receive_message()` handles an envelope with `message_type == INVALID`).
- [ASSUMPTION] The test infrastructure calls `init()` before `inject()`. The `m_open` assertion will fire (and abort in debug builds) if this is violated, which is the intended failure mode.
