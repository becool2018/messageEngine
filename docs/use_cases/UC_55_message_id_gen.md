# UC_55 — MessageIdGen next(): monotonic message-ID counter owned by DeliveryEngine

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-3.2.1, REQ-3.3.3

This is a System Internal use case. `MessageIdGen::next()` is called by `DeliveryEngine::send()` to assign a unique `message_id` to every outbound envelope. The User never calls it directly.

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::send()` calls `m_id_gen.next()` to obtain a unique `message_id` for the outbound envelope. File: `src/core/MessageIdGen.hpp`.
- **Goal:** Return a strictly monotonically increasing `uint64_t` message ID, starting from 1, incrementing by 1 on each call. The ID must be unique for the lifetime of the `DeliveryEngine` instance.
- **Success outcome:** Returns the next `uint64_t` in the sequence (1, 2, 3, ...).
- **Error outcomes:** None — the counter wraps at `UINT64_MAX + 1 = 0`. After wrap, IDs start at 0 (potential collision with old IDs after 2^64 messages; not a practical concern).

---

## 2. Entry Points

```cpp
// src/core/MessageIdGen.hpp (inline)
uint64_t MessageIdGen::next();
void     MessageIdGen::reset();
```

Called from: `DeliveryEngine::send()` in `src/core/DeliveryEngine.cpp`.

---

## 3. End-to-End Control Flow

**`MessageIdGen::next()`:**
1. `NEVER_COMPILED_OUT_ASSERT(m_counter < UINT64_MAX)` (optional, to detect wrap-around in test).
2. Return `m_counter++` (post-increment).

**`MessageIdGen::reset()`:**
1. `m_counter = 1` — reset to initial value; used in `DeliveryEngine::init()`.

---

## 4. Call Tree

```
MessageIdGen::next()                               [MessageIdGen.hpp]
 └── [m_counter++ return value]

MessageIdGen::reset()                              [MessageIdGen.hpp]
 └── [m_counter = 1]
```

---

## 5. Key Components Involved

- **`m_counter` (uint64_t)** — plain integer counter; starts at 1 after `reset()`. Incremented on every `next()` call.
- **Uniqueness guarantee** — within a `DeliveryEngine` session, each outbound message gets a unique ID. The deduplication window on the receiver (`DEDUP_WINDOW_SIZE = 128`) only needs uniqueness within a sliding window of 128 messages.

---

## 6. Branching Logic / Decision Points

No branching. Single post-increment return.

---

## 7. Concurrency / Threading Behavior

- `DeliveryEngine::send()` is called from the application send thread. `m_counter` is a plain `uint64_t`; not atomic.
- If `send()` is called from multiple threads concurrently, `m_counter` access is a data race. Single-threaded `send()` is the design contract.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `m_counter` — 8 bytes; member of `MessageIdGen` which is a member of `DeliveryEngine`.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states in normal operation. Wrap-around at `UINT64_MAX` produces `0` on the next call, which `envelope_valid()` might treat as invalid (source_id=0 check analogy). In practice, 2^64 messages per session is unreachable.

---

## 10. External Interactions

- None — pure counter arithmetic in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `MessageIdGen` | `m_counter` | N | N+1 |

---

## 12. Sequence Diagram

```
[DeliveryEngine::send(env)]
  -> MessageIdGen::next()
       [return m_counter; m_counter++]
       <- 42
  [env.message_id = 42]
  -> AckTracker::track(42, deadline_us)   [if RELIABLE_ACK]
  -> RetryManager::schedule(42, env)      [if RELIABLE_RETRY]
  -> m_transport->send_message(env)
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `MessageIdGen::reset()` called from `DeliveryEngine::init()`.

**Runtime:**
- Called exactly once per `DeliveryEngine::send()` invocation for each outbound DATA envelope. Not called for ACK envelopes (those echo the original message_id).

---

## 14. Known Risks / Observations

- **No thread safety:** `m_counter` is not atomic. Multi-threaded callers of `send()` would race on `m_counter`. Single-threaded use is the documented contract.
- **ACK envelopes reuse IDs:** ACK envelopes copy `message_id` from the original DATA envelope rather than generating a new ID. If `MessageIdGen::next()` were called for ACKs, the ID space would be fragmented and `AckTracker` correlation would break.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `m_counter` starts at 1 (not 0) after `reset()`; this avoids the potential collision with 0 being used as a "no ID" sentinel.
- `[ASSUMPTION]` `MessageIdGen::next()` uses post-increment (`m_counter++`) so the returned value is the pre-increment value; the first call after `reset()` returns 1.
- `[ASSUMPTION]` `MessageIdGen` is a simple inline class in `MessageIdGen.hpp` with no `.cpp` file.
