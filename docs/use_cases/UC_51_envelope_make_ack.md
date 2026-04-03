# UC_51 — envelope_make_ack(): constructs an ACK reply envelope

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

This is a System Internal use case. It is invoked by `DeliveryEngine::receive()` automatically for every received `RELIABLE_ACK` or `RELIABLE_RETRY` DATA envelope. The User never calls it directly.

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` has accepted a DATA envelope with `reliability_class == RELIABLE_ACK` or `RELIABLE_RETRY`. File: `src/core/Envelope.cpp` or `src/core/Envelope.hpp`.
- **Goal:** Construct a minimal ACK envelope suitable for sending back to the originator. The ACK carries the original `message_id` and `source_id` so the sender can match it to its pending `AckTracker` / `RetryManager` entry.
- **Success outcome:** `ack` is populated with `message_type = ACK`, `message_id = original.message_id`, `source_id = my_id`, `destination_id = original.source_id`, `payload_length = 0`, and `timestamp_us = now_us` (passed by caller).
- **Error outcomes:** None — `envelope_make_ack()` has no error states.

---

## 2. Entry Points

```cpp
// src/core/MessageEnvelope.hpp (inline)
void envelope_make_ack(MessageEnvelope&       ack,
                       const MessageEnvelope& original,
                       NodeId                 my_id,
                       uint64_t               now_us);
```

Called from: `DeliveryEngine::receive()` in `src/core/DeliveryEngine.cpp`.

---

## 3. End-to-End Control Flow

1. **`envelope_make_ack(ack, original, my_id, now_us)`** — entry.
2. `envelope_init(ack)` — zero-initialize the ACK envelope.
3. `ack.message_type = MessageType::ACK`.
4. `ack.reliability_class = ReliabilityClass::BEST_EFFORT` — ACKs are not re-ACKed.
5. `ack.message_id = original.message_id` — echo original ID for correlation.
6. `ack.source_id = my_id` — identify the sender of this ACK.
7. `ack.destination_id = original.source_id` — send ACK back to the originator.
8. `ack.priority = original.priority` — preserve priority from the original message.
9. `ack.timestamp_us = now_us` — caller-supplied timestamp; no internal clock call.
10. `ack.expiry_time_us = 0U` — ACKs do not expire.
11. `ack.payload_length = 0U` — no payload.
12. Returns (void).

---

## 4. Call Tree

```
envelope_make_ack(ack, original, my_id, now_us)  [MessageEnvelope.hpp inline]
 └── envelope_init(ack)                           [zero-init]
```

---

## 5. Key Components Involved

- **`envelope_init()`** — ensures ACK envelope starts from a clean zero state (see UC_43).
- **`message_id` echo** — the key field that allows the sender's `AckTracker::on_ack(src, msg_id)` and `RetryManager::on_ack(src, msg_id)` to locate and resolve the pending entry.
- **`BEST_EFFORT` reliability** — ACKs must not trigger further ACK generation; assigning BEST_EFFORT breaks the ACK-of-ACK chain.

---

## 6. Branching Logic / Decision Points

No branching. Fixed field assignments regardless of `original` content.

---

## 7. Concurrency / Threading Behavior

- Called from the application receive thread inside `DeliveryEngine::receive()`.
- `ack` is a stack-local variable in `receive()`; no shared state.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `ack` — stack-allocated `MessageEnvelope` in `send_data_ack()` called from `DeliveryEngine::receive()`; passed by reference into this function; not heap-allocated.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states.

---

## 10. External Interactions

- None — `now_us` is supplied by the caller; no clock call is made inside `envelope_make_ack()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `*ack` | all fields | undefined | ACK envelope with message_id, source/dest IDs, zero payload |

---

## 12. Sequence Diagram

```
[send_data_ack() — called from DeliveryEngine::receive() after RELIABLE_ACK/RETRY data accepted]
  -> envelope_make_ack(ack, raw_env, m_local_id, now_us)
       -> envelope_init(ack)
       [set ACK fields: type, message_id, source, dest, priority, timestamp=now_us, expiry=0, length=0]
  <- (void)
  -> m_transport->send_message(ack)             [ACK sent to originator]
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
- **expiry_time_us = 0:** With `envelope_init()` zeroing all fields, `ack->expiry_time_us = 0`, which means the ACK is immediately "expired" by `timestamp_expired()`. The send path must not apply expiry filtering to ACK envelopes, or the ACK will be dropped before transmission.

---

## 15. Unknowns / Assumptions

- `[CONFIRMED]` `envelope_make_ack()` accepts `my_id` and `now_us` as caller-supplied parameters (passed from `m_local_id` and the current `now_us` in `send_data_ack()`). No internal clock call is made.
- `[ASSUMPTION]` `expiry_time_us` is left as 0 from `envelope_init()`; the send path does not filter ACKs by expiry.
- `[ASSUMPTION]` The function is called only for DATA envelopes with RELIABLE_ACK or RELIABLE_RETRY class; it is not called for BEST_EFFORT DATA or for control message types.
