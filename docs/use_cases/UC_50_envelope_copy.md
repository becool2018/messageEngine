# UC_50 — envelope_copy(): copies envelope bytes between internal buffers

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-3.1, REQ-4.1.2, REQ-4.1.3

This is a System Internal use case. It is invoked by `RingBuffer::push()`, `RingBuffer::pop()`, and `DeliveryEngine` to copy `MessageEnvelope` structs between stack frames and ring buffer slots. The User never calls it directly.

---

## 1. Use Case Overview

- **Trigger:** `RingBuffer::push(env)` or `RingBuffer::pop(out_env)` needs to transfer envelope contents between the caller's stack and the fixed-array buffer slot. File: `src/core/RingBuffer.hpp`.
- **Goal:** Perform a safe, complete copy of a `MessageEnvelope` struct (including the 4096-byte payload array) without any pointer aliasing.
- **Success outcome:** Destination `MessageEnvelope` is byte-identical to source. No partial copies.
- **Error outcomes:** None — `envelope_copy()` has no error states if called with valid non-null pointers.

---

## 2. Entry Points

```cpp
// src/core/Envelope.hpp (inline) or src/core/Envelope.cpp
void envelope_copy(MessageEnvelope* dst, const MessageEnvelope* src);
```

Called from:
- `RingBuffer::push()` — copies caller envelope into `m_buf[head]`.
- `RingBuffer::pop()` — copies `m_buf[tail]` into caller's output buffer.
- `DeliveryEngine::receive()` — copies raw_env to `*out_env`.

---

## 3. End-to-End Control Flow

1. **`envelope_copy(dst, src)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(dst != nullptr)`.
3. `NEVER_COMPILED_OUT_ASSERT(src != nullptr)`.
4. `NEVER_COMPILED_OUT_ASSERT(dst != src)` — no self-copy.
5. `memcpy(dst, src, sizeof(MessageEnvelope))` — full struct copy including payload array.
6. Returns (void).

---

## 4. Call Tree

```
envelope_copy(dst, src)                            [Envelope.hpp/cpp]
 ├── NEVER_COMPILED_OUT_ASSERT(dst != nullptr)
 ├── NEVER_COMPILED_OUT_ASSERT(src != nullptr)
 ├── NEVER_COMPILED_OUT_ASSERT(dst != src)
 └── memcpy(dst, src, sizeof(MessageEnvelope))
```

---

## 5. Key Components Involved

- **`memcpy()`** — copies the full `sizeof(MessageEnvelope) 4144 bytes` including the 4096-byte payload. Using `memcpy` instead of struct assignment avoids any potential compiler-generated overhead for large structs.
- **Non-aliasing assertion** — `dst != src` prevents an undefined-behavior self-copy through `memcpy`.

---

## 6. Branching Logic / Decision Points

No branching beyond the assertion checks.

---

## 7. Concurrency / Threading Behavior

- Called on the producer thread for `push()` and on the consumer thread for `pop()`.
- The SPSC ring buffer ensures `m_buf[head]` (write slot) and `m_buf[tail]` (read slot) are never the same slot simultaneously, so concurrent push and pop do not race on the same buffer slot.
- No `std::atomic` operations within `envelope_copy()` itself.

---

## 8. Memory & Ownership Semantics

- `dst` and `src` are pointers to `MessageEnvelope` objects owned by their respective callers.
- `sizeof(MessageEnvelope) 4144 bytes` — copying this on every push/pop is the dominant memory operation in the receive path; documented in WCET analysis.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states. Null pointer inputs trigger `NEVER_COMPILED_OUT_ASSERT`.

---

## 10. External Interactions

- None — operates entirely in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `*dst` | all fields | undefined or stale | byte-identical copy of `*src` |

`*src` is unchanged.

---

## 12. Sequence Diagram

```
[RingBuffer::push(env)]
  -> envelope_copy(&m_buf[head], &env)
       -> memcpy(dst, src, sizeof(MessageEnvelope))
  [m_head.store(next_head, release)]

[RingBuffer::pop(out_env)]
  -> envelope_copy(out_env, &m_buf[tail])
       -> memcpy(dst, src, sizeof(MessageEnvelope))
  [m_tail.store(next_tail, release)]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** `dst` and `src` are valid non-null pointers to distinct `MessageEnvelope` objects.

**Runtime:** Called on every ring buffer push and pop operation; the dominant copying operation in the message delivery pipeline.

---

## 14. Known Risks / Observations

- **Copy cost:** `memcpy` of 4144 bytes per message is the largest per-message memory operation. For very high message rates this can be a throughput bottleneck. The fixed-size struct design makes this cost constant and predictable.
- **No partial copy protection:** If the call is interrupted (signal) during `memcpy`, the destination may be partially written. In single-threaded or SPSC contexts this is not a concern since the slot is not visible to the consumer until after the full copy completes (guaranteed by the release store on `m_head`).

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `envelope_copy()` is implemented as a thin wrapper around `memcpy(sizeof(MessageEnvelope))`. It may be an inline function in `Envelope.hpp` rather than a separate `.cpp` function.
- `[ASSUMPTION]` In some call sites (e.g., `DeliveryEngine::receive()`), a plain struct assignment (`*out_env = raw_env`) may be used instead of an explicit `envelope_copy()` call; both are equivalent for a trivially-copyable struct.
