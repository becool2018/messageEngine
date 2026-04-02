# UC_44 — envelope_is_data() / envelope_is_control() / envelope_valid(): classify or validate an envelope type and contents

**HL Group:** HL-21 — Build and Classify a Message Envelope
**Actor:** User
**Requirement traceability:** REQ-3.1, REQ-3.2.1, REQ-3.3.4

---

## 1. Use Case Overview

- **Trigger:** User (or internal system code) calls `envelope_is_data()`, `envelope_is_control()`, or `envelope_valid()` on a received or constructed `MessageEnvelope`. File: `src/core/Envelope.hpp` or `src/core/Envelope.cpp`.
- **Goal:** Classify an envelope as application DATA or control traffic (ACK/NAK/HEARTBEAT), or validate that an envelope's fields are internally consistent and safe to process.
- **Success outcome:** Each predicate returns a boolean indicating whether the envelope meets the stated classification or validity criteria.
- **Error outcomes:** None — these are pure predicate functions. `NEVER_COMPILED_OUT_ASSERT` fires if the envelope pointer is null.

---

## 2. Entry Points

```cpp
// src/core/Envelope.hpp / src/core/Envelope.cpp
bool envelope_is_data(const MessageEnvelope* env);
bool envelope_is_control(const MessageEnvelope* env);
bool envelope_valid(const MessageEnvelope* env);
```

---

## 3. End-to-End Control Flow

**`envelope_is_data(env)`:**
1. `NEVER_COMPILED_OUT_ASSERT(env != nullptr)`.
2. Returns `env->message_type == MessageType::DATA`.

**`envelope_is_control(env)`:**
1. `NEVER_COMPILED_OUT_ASSERT(env != nullptr)`.
2. Returns `env->message_type == MessageType::ACK || env->message_type == MessageType::NAK || env->message_type == MessageType::HEARTBEAT`.

**`envelope_valid(env)`:**
1. `NEVER_COMPILED_OUT_ASSERT(env != nullptr)`.
2. Checks:
   - `env->message_type != MessageType::INVALID`.
   - `env->payload_length <= MSG_MAX_PAYLOAD_BYTES`.
   - `env->source_id != 0` (or other non-zero sentinel).
3. Returns true only if all checks pass.

---

## 4. Call Tree

```
envelope_is_data(env)                              [Envelope.hpp/cpp]
 └── env->message_type == DATA

envelope_is_control(env)                           [Envelope.hpp/cpp]
 └── env->message_type ∈ {ACK, NAK, HEARTBEAT}

envelope_valid(env)                                [Envelope.hpp/cpp]
 ├── message_type != INVALID
 ├── payload_length <= MSG_MAX_PAYLOAD_BYTES
 └── source_id != 0
```

---

## 5. Key Components Involved

- **`envelope_is_data()`** — called by `DeliveryEngine::receive()` to decide whether to deliver the envelope to the application or route it to ACK/retry handling.
- **`envelope_is_control()`** — used to distinguish ACK/NAK/HEARTBEAT envelopes that must be handled by the reliability subsystem, not delivered to application code.
- **`envelope_valid()`** — called by `Serializer::deserialize()` and `DeliveryEngine::receive()` as a sanity check on inbound envelopes before processing.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `message_type == DATA` | `is_data()` returns true | `is_data()` returns false |
| `message_type ∈ {ACK, NAK, HEARTBEAT}` | `is_control()` returns true | `is_control()` returns false |
| `message_type == INVALID` | `valid()` returns false | Continue checks |
| `payload_length > MSG_MAX_PAYLOAD_BYTES` | `valid()` returns false | Continue checks |
| `source_id == 0` | `valid()` returns false | `valid()` returns true |

---

## 7. Concurrency / Threading Behavior

- Pure read-only predicates; no state modification.
- Safe to call from any thread as long as the envelope is not concurrently modified.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- Read-only access to `*env`; no writes.
- No heap allocation. Power of 10 Rule 3 compliant.
- Pointer must remain valid for the duration of the call (no aliasing concerns for single-call predicates).

---

## 9. Error Handling Flow

- No error states. If `env == nullptr`, `NEVER_COMPILED_OUT_ASSERT` fires.
- `envelope_valid()` returning false is not an error itself — it is information for the caller to act on (discard, log, or assert as appropriate).

---

## 10. External Interactions

- None — these functions operate entirely in process memory.

---

## 11. State Changes / Side Effects

None. These are pure predicates; no state is modified.

---

## 12. Sequence Diagram

```
[DeliveryEngine::receive() after deserialization]
  -> envelope_valid(&env)       <- true (or false -> discard)
  -> envelope_is_data(&env)
       <- true
  [deliver to application ring buffer]

[or]
  -> envelope_is_control(&env)
       <- true
  [route to AckTracker::on_ack() / RetryManager::on_ack()]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** `env` must point to a fully populated `MessageEnvelope` (either from `Serializer::deserialize()` or from user construction after `envelope_init()`).

**Runtime:** Called on every received envelope in `DeliveryEngine::receive()` to determine dispatch path.

---

## 14. Known Risks / Observations

- **`envelope_valid()` is not exhaustive:** It checks structural validity (type, length, source_id) but does not verify the timestamp, expiry_time_us, or message_id for correctness. Those checks are performed separately.
- **DATA vs control exclusivity:** An envelope cannot simultaneously be DATA and control (they test disjoint `message_type` values). `is_data()` and `is_control()` are mutually exclusive predicates.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `envelope_valid()` checks `source_id != 0` as part of validity; exact set of validity conditions is inferred from usage in `DeliveryEngine` and `Serializer`.
- `[ASSUMPTION]` These functions are implemented as free functions (not methods) consistent with the C-style struct API pattern in this codebase.
- `[ASSUMPTION]` `envelope_is_control()` covers ACK, NAK, and HEARTBEAT; HEARTBEAT routing behavior is inferred from the MessageType enum values present in `Types.hpp`.
