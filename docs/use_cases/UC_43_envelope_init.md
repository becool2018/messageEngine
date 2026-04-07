# UC_43 — envelope_init(): zero-initialise a MessageEnvelope to a safe, INVALID state before field assignment

**HL Group:** HL-21 — Build and Classify a Message Envelope
**Actor:** User
**Requirement traceability:** REQ-3.1, REQ-3.2.1

---

## 1. Use Case Overview

- **Trigger:** User calls `envelope_init(envelope)` before populating a `MessageEnvelope` for sending. File: `src/core/Envelope.cpp` (or `src/core/Envelope.hpp` as an inline function).
- **Goal:** Zero-initialize all fields of a `MessageEnvelope` to safe, deterministic values so that no field is left in an undefined state before field assignment. `message_type` is set to `MessageType::INVALID` to ensure an unfinished envelope cannot be mistaken for a valid DATA or ACK message.
- **Success outcome:** All fields of `env` are zeroed. `env.message_type == MessageType::INVALID`. `env.payload_length == 0`. `env.payload` buffer is zeroed.
- **Error outcomes:** None — `envelope_init()` has no error states. The parameter is a reference; null is not possible.

---

## 2. Entry Points

```cpp
// src/core/MessageEnvelope.hpp (inline)
void envelope_init(MessageEnvelope& env);
```

---

## 3. End-to-End Control Flow

1. **`envelope_init(env)`** — entry (reference parameter; no null check needed).
2. `memset(&env, 0, sizeof(MessageEnvelope))` — zero all bytes of the struct.
3. `env.message_type = MessageType::INVALID` — explicitly mark envelope as not-yet-valid (value 255 / 0xFF, distinct from DATA=0, ACK=1, NAK=2, HEARTBEAT=3).
5. Returns (void).

---

## 4. Call Tree

```
envelope_init(env)                                 [MessageEnvelope.hpp inline]
 ├── memset(&env, 0, sizeof(MessageEnvelope))
 └── env.message_type = MessageType::INVALID
```

---

## 5. Key Components Involved

- **`memset()`** — zeroes all struct fields including `payload[MSG_MAX_PAYLOAD_BYTES]`. Ensures no stale data from a previous use of the same stack location is present.
- **`MessageType::INVALID = 255`** — sentinel value that `envelope_valid()` and `Serializer::deserialize()` both check; an un-initialized envelope cannot be serialized or delivered.

---

## 6. Branching Logic / Decision Points

No branching logic. The function performs a fixed sequence of operations on any non-null pointer.

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| (no branching) | memset + set INVALID | N/A |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. The envelope is stack-allocated or caller-owned; no shared state.
- No `std::atomic` operations.

---

## 8. Memory & Ownership Semantics

- `*env` — `MessageEnvelope` struct; `sizeof(MessageEnvelope) = sizeof header fields + MSG_MAX_PAYLOAD_BYTES (4096) 4144 bytes`.
- `memset` touches the entire struct including the 4096-byte payload array.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- No error states. If `env == nullptr`, `NEVER_COMPILED_OUT_ASSERT` triggers.

---

## 10. External Interactions

- None — this flow operates entirely in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `env` | all fields | undefined | zeroed |
| `env` | `message_type` | undefined | `MessageType::INVALID` |
| `env` | `payload_length` | undefined | 0 |
| `env` | `payload[*]` | undefined | all bytes 0 |

---

## 12. Sequence Diagram

```
User -> envelope_init(env)
  -> memset(&env, 0, sizeof(MessageEnvelope))
  -> env.message_type = MessageType::INVALID
  <- (void)

[User populates env.message_type, env.source_id, env.payload, etc.]
[User -> DeliveryEngine::send(env)]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:** `env` must point to a writable `MessageEnvelope` (stack or caller-owned).

**Runtime:** Called before every outbound message construction. Ensures the envelope cannot carry stale fields from a previous send cycle when stack memory is reused.

---

## 14. Known Risks / Observations

- **memset of large struct:** `sizeof(MessageEnvelope) 4144 bytes` makes `memset` the dominant cost of this function. For high-frequency senders this may be a minor but measurable overhead; callers that reuse the same envelope for multiple sends can skip `envelope_init()` after the first call if they overwrite all relevant fields.
- **INVALID sentinel:** `MessageType::INVALID = 255` (0xFF) is the zero-initialized value's complement; `memset(0)` sets it to 0 (DATA), not INVALID. The explicit `env->message_type = MessageType::INVALID` step after `memset` is required.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `envelope_init()` is implemented as a free function (not a constructor), consistent with the C-style API pattern used throughout the codebase for struct initializers.
- `[ASSUMPTION]` `MessageType::INVALID = 255` (0xFF); the `memset(0)` followed by explicit assignment pattern is required because INVALID is non-zero.
