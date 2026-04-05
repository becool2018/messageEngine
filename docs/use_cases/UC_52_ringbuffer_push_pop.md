# UC_52 — RingBuffer push/pop: SPSC lock-free queue used internally by all backends

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-4.1.2, REQ-4.1.3, REQ-6.3.5

This is a System Internal use case. `RingBuffer` is used by `TcpBackend`, `UdpBackend`, `LocalSimHarness`, `TlsTcpBackend`, and `DtlsUdpBackend` to stage inbound envelopes between the network receive path and `receive_message()`. The User never calls `push()` or `pop()` directly.

---

## 1. Use Case Overview

- **Trigger (push):** A backend's receive path (`recv_from_client()`, `poll_clients_once()`, or `recvfrom()`) successfully deserializes an envelope and calls `RingBuffer::push(&env)`.
- **Trigger (pop):** `receive_message()` or `DeliveryEngine::receive()` calls `RingBuffer::pop(&out_env)`.
- **Goal:** Transfer `MessageEnvelope` structs between a single producer context and a single consumer context without locks, using atomic operations for head/tail coordination.
- **Success outcome (push):** Returns `true`; envelope is in `m_buf[m_head]`; `m_head` advanced.
- **Success outcome (pop):** Returns `true`; `*out_env` contains the dequeued envelope; `m_tail` advanced.
- **Error outcomes:**
  - `push()` returns `false` — queue full (`MSG_RING_CAPACITY = 64`); envelope is dropped.
  - `pop()` returns `false` — queue empty; `*out_env` is unmodified.

---

## 2. Entry Points

```cpp
// src/core/RingBuffer.hpp
bool RingBuffer::push(const MessageEnvelope& env);
bool RingBuffer::pop(MessageEnvelope* out_env);
void RingBuffer::init();
```

---

## 3. End-to-End Control Flow

**`RingBuffer::push(env)`:**
1. `uint32_t head = m_head.load(memory_order_relaxed)`.
2. `uint32_t next_head = (head + 1) % MSG_RING_CAPACITY`.
3. `uint32_t tail = m_tail.load(memory_order_acquire)` — acquire to observe consumer's most recent pop.
4. If `next_head == tail`: queue full; return false.
5. `memcpy(&m_buf[head], &env, sizeof(MessageEnvelope))` (or `envelope_copy()`).
6. `m_head.store(next_head, memory_order_release)` — release: makes the new element visible to consumer.
7. Return `true`.

**`RingBuffer::pop(out_env)`:**
1. `uint32_t tail = m_tail.load(memory_order_relaxed)`.
2. `uint32_t head = m_head.load(memory_order_acquire)` — acquire to observe producer's most recent push.
3. If `tail == head`: queue empty; return false.
4. `memcpy(out_env, &m_buf[tail], sizeof(MessageEnvelope))` (or `envelope_copy()`).
5. `m_tail.store((tail + 1) % MSG_RING_CAPACITY, memory_order_release)`.
6. Return `true`.

**`RingBuffer::init()`:**
1. `m_head.store(0, memory_order_relaxed)`.
2. `m_tail.store(0, memory_order_relaxed)`.

---

## 4. Call Tree

```
RingBuffer::push(env)                              [RingBuffer.hpp]
 ├── m_head.load(relaxed)
 ├── m_tail.load(acquire)
 ├── memcpy / envelope_copy()
 └── m_head.store(next_head, release)

RingBuffer::pop(out_env)                           [RingBuffer.hpp]
 ├── m_tail.load(relaxed)
 ├── m_head.load(acquire)
 ├── memcpy / envelope_copy()
 └── m_tail.store(next_tail, release)
```

---

## 5. Key Components Involved

- **`m_head` (std::atomic<uint32_t>)** — producer index; written only by the producer thread; read by both.
- **`m_tail` (std::atomic<uint32_t>)** — consumer index; written only by the consumer thread; read by both.
- **Acquire/release ordering** — the release store on `m_head` (push) synchronizes with the acquire load on `m_head` (pop), ensuring the consumer sees the complete `memcpy` before advancing `m_tail`. Symmetric for pop→push.
- **`m_buf[MSG_RING_CAPACITY]`** — fixed array of 64 `MessageEnvelope` slots; 64 × 4144 bytes ≈ 259 KB static allocation.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `next_head == m_tail` (push) | Queue full; return false | Copy + advance head; return true |
| `tail == head` (pop) | Queue empty; return false | Copy + advance tail; return true |

---

## 7. Concurrency / Threading Behavior

- **SPSC (Single Producer, Single Consumer):** `push()` must be called from one thread only; `pop()` from one (different) thread only. This contract is enforced by convention, not by the ring buffer itself.
- `m_head` — producer writes (release); consumer reads (acquire).
- `m_tail` — consumer writes (release); producer reads (acquire).
- **Memory ordering:** The acquire/release pair provides the necessary happens-before: the producer's `memcpy` is visible to the consumer after the consumer's acquire load of `m_head` sees the updated value.
- **No mutex or spinlock.** Power of 10 Rule 3 compliant (no heap allocation; atomics are permitted).

---

## 8. Memory & Ownership Semantics

- `m_buf[MSG_RING_CAPACITY]` — static array member of `RingBuffer`; `MSG_RING_CAPACITY = 64`; each slot `sizeof(MessageEnvelope) = 4144 bytes`; total ≈ 265 KB.
- No heap allocation. Power of 10 Rule 3 compliant.
- `m_head`, `m_tail` — `std::atomic<uint32_t>` members; permitted carve-out from no-STL rule.

---

## 9. Error Handling Flow

- **Full (push returns false):** The calling backend logs `WARNING_LO` and drops the envelope. The application will not see this message. `m_head` and `m_tail` are unchanged.
- **Empty (pop returns false):** `receive_message()` returns `ERR_TIMEOUT`. No state change.

---

## 10. External Interactions

- None — operates entirely in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before (push) | After (push) |
|--------|--------|---------------|--------------|
| `RingBuffer` | `m_buf[m_head]` | stale or old | copy of pushed `env` |
| `RingBuffer` | `m_head` | H | (H+1) % 64 |

| Object | Member | Before (pop) | After (pop) |
|--------|--------|--------------|-------------|
| `*out_env` | all fields | undefined | copy of `m_buf[m_tail]` |
| `RingBuffer` | `m_tail` | T | (T+1) % 64 |

---

## 12. Sequence Diagram

```
[Backend receive thread]
  -> RingBuffer::push(env)
       [m_head.load(relaxed)]
       [m_tail.load(acquire)]
       [m_buf[head] = env]
       [m_head.store(next_head, release)]
       <- true

[Application receive thread]
  -> RingBuffer::pop(&out_env)
       [m_tail.load(relaxed)]
       [m_head.load(acquire)]  [sees release from push]
       [out_env = m_buf[tail]]
       [m_tail.store(next_tail, release)]
       <- true
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `RingBuffer::init()` must be called before `push()` or `pop()`. Called from `TcpBackend::init()`, `UdpBackend::init()`, etc.

**Runtime:**
- `push()` called on every successful inbound deserialization.
- `pop()` called on every `receive_message()` or `DeliveryEngine::receive()` invocation.

---

## 14. Known Risks / Observations

- **SPSC violation:** If two threads simultaneously call `push()`, the relaxed load of `m_head` in both threads may read the same head value; both would write to the same slot and the second write would corrupt the first. The SPSC invariant must be maintained by callers.
- **Capacity saturation:** At `MSG_RING_CAPACITY = 64`, a slow consumer will cause the producer to start dropping envelopes silently (push returns false, WARNING_LO logged). This is the documented behavior.
- **Large slot size:** Each slot is 4144 bytes. The full ring buffer occupies ~259 KB. For embedded targets with tight SRAM this is the dominant static allocation.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `m_head` and `m_tail` use `memory_order_acquire` on cross-thread reads and `memory_order_release` on the store that makes new data visible, consistent with standard SPSC ring buffer patterns.
- `[ASSUMPTION]` `RingBuffer::init()` uses relaxed stores (no need for release on initialization since the caller ensures no concurrent access during init).
- `[ASSUMPTION]` The ring buffer is always used in a single-producer, single-consumer context; no multi-producer or multi-consumer usage exists in this codebase.
