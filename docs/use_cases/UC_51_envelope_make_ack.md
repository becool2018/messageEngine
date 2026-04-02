# UC_51 — envelope_make_ack(): constructs an ACK reply envelope

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

This is a System Internal use case. It is invoked by `DeliveryEngine::receive()` automatically for every received `RELIABLE_ACK` or `RELIABLE_RETRY` DATA envelope. The User never calls it directly.

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` has accepted a DATA envelope with `reliability_class == RELIABLE_ACK` or `RELIABLE_RETRY`. File: `src/core/Envelope.cpp` or `src/core/Envelope.hpp`.
- **Goal:** Construct a minimal ACK envelope suitable for sending back to the originator. The ACK carries the original `message_id` and `source_id` so the sender can match it to its pending `AckTracker` / `RetryManager` entry.
- **Success outcome:** `*ack_env` is populated with `message_type = ACK`, `message_id = original.message_id`, `source_id = my_node_id`, `destination_id = original.source_id`, `payload_length = 0`, and a current `timestamp_us`.
- **Error outcomes:** None — `envelope_make_ack()` has no error states (both pointers are asserted non-null).

---

## 2. Entry Points

```cpp
// src/core/Envelope.hpp (inline) or src/core/Envelope.cpp
void envelope_make_ack(const MessageEnvelope* original,
                       MessageEnvelope*       ack_env,
                       NodeId                 my_node_id);
```

Called from: `DeliveryEngine::receive()` in `src/core/DeliveryEngine.cpp`.

---

## 3. End-to-End Control Flow

1. **`envelope_make_ack(original, ack_env, my_node_id)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(original != nullptr)`.
3. `NEVER_COMPILED_OUT_ASSERT(ack_env != nullptr)`.
4. `envelope_init(ack_env)` — zero-initialize the ACK envelope.
5. `ack_env->message_type = MessageType::ACK`.
6. `ack_env->reliability_class = ReliabilityClass::BEST_EFFORT` — ACKs are not re-ACKed.
7. `ack_env->message_id = original->message_id` — echo original ID for correlation.
8. `ack_env->source_id = my_node_id` — identify the sender of this ACK.
9. `ack_env->destination_id = original->source_id` — send ACK back to the originator.
10. `ack_env->timestamp_us = timestamp_now_us()` — current time.
11. `ack_env->payload_length = 0` — no payload.
12. Returns (void).

---

## 4. Call Tree

```
envelope_make_ack(original, ack_env, my_node_id)  [Envelope.hpp/cpp]
 ├── envelope_init(ack_env)                        [zero-init]
 └── timestamp_now_us()                            [Timestamp.hpp — for ack timestamp]
```

---

## 5. Key Components Involved

- **`envelope_init()`** — ensures ACK envelope starts from a clean zero state (see UC_43).
- **`message_id` echo** — the key field that allows the sender's `AckTracker::on_ack(message_id)` and `RetryManager::on_ack(message_id)` to locate and resolve the pending entry.
- **`BEST_EFFORT` reliability** — ACKs must not trigger further ACK generation; assigning BEST_EFFORT breaks the ACK-of-ACK chain.

---

## 6. Branching Logic / Decision Points

No branching. Fixed field assignments regardless of `original` content.

---

## 7. Concurrency / Threading Behavior

- Called from the application receive thread inside `DeliveryEngine::receive()`.
- `ack_env` is a stack-local variable in `receive()`; no shared state.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `ack_env` — stack-allocated `MessageEnvelope` in `DeliveryEngine::receive()`; passed by pointer into this function; not heap-allocated.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states. Both pointer arguments are asserted non-null.

---

## 10. External Interactions

- **`clock_gettime(CLOCK_MONOTONIC)`** — called indirectly via `timestamp_now_us()` to populate `ack_env->timestamp_us`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `*ack_env` | all fields | undefined | ACK envelope with message_id, source/dest IDs, zero payload |

---

## 12. Sequence Diagram

```
[DeliveryEngine::receive() — after RELIABLE_ACK/RETRY data accepted]
  -> envelope_make_ack(&raw_env, &ack_env, m_node_id)
       -> envelope_init(&ack_env)
       -> timestamp_now_us()                        <- now_us
       [set ACK fields: type, message_id, source, dest, timestamp]
  <- (void)
  -> m_transport->send_message(ack_env)             [ACK sent to originator]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `original` points to a valid DATA envelope with a non-zero `message_id` and `source_id`.
- `my_node_id` is the local node's ID (non-zero).

**Runtime:** Called on every received RELIABLE_ACK or RELIABLE_RETRY DATA message. The generated ACK is sent immediately via the transport.

---

## 14. Known Risks / Observations

- **ACK storm prevention:** ACK envelopes use `BEST_EFFORT` so they are never re-ACKed. Without this, receiving an ACK would trigger another ACK, causing an infinite loop.
- **expiry_time_us = 0:** With `envelope_init()` zeroing all fields, `ack_env->expiry_time_us = 0`, which means the ACK is immediately "expired" by `timestamp_expired()`. The send path must not apply expiry filtering to ACK envelopes, or the ACK will be dropped before transmission.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `envelope_make_ack()` accepts `my_node_id` as a parameter (passed from `m_node_id` in `DeliveryEngine`); the exact signature may differ if `my_node_id` is accessed via a different mechanism.
- `[ASSUMPTION]` `expiry_time_us` is left as 0 from `envelope_init()`; the send path does not filter ACKs by expiry.
- `[ASSUMPTION]` The function is called only for DATA envelopes with RELIABLE_ACK or RELIABLE_RETRY class; it is not called for BEST_EFFORT DATA or for control message types.
