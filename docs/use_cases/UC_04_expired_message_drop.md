# UC_04 — Expired message detected and dropped at send time

**HL Group:** HL-4 — Send with Expiry Deadline
**Actor:** User
**Requirement traceability:** REQ-3.3.4, REQ-3.2.2, REQ-3.2.7, REQ-4.1.2

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send(envelope, now_us)` where `envelope.expiry_time_us != 0` and `envelope.expiry_time_us <= now_us`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Detect that the message's delivery deadline has already passed and drop it before any network I/O or tracker allocation occurs.
- **Success outcome:** `Result::ERR_EXPIRED` is returned. No bytes are written to any socket; no ACK slot or retry slot is allocated.
- **Error outcomes:** `Result::ERR_EXPIRED` is the only outcome for this UC (the drop is the success condition).

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(const MessageEnvelope& envelope, uint64_t now_us);
```

Synchronous in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `m_id_gen.next()` is called; `message_id` assigned to `work` (stack copy). The ID is consumed even for an expired message (counter advances).
4. **`send_via_transport(work, now_us)`** is called.
5. Inside `send_via_transport()`:
   a. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)`.
   b. **`timestamp_expired(work.expiry_time_us, now_us)`** is evaluated (`Timestamp.hpp`). Implementation: `return (expiry_time_us != 0U) && (now_us >= expiry_time_us)`.
   c. **Branch: condition true** — `Logger::log(WARNING_LO, "DeliveryEngine", "Message expired before send; dropping. id=%llu", ...)` is called.
   d. Returns `Result::ERR_EXPIRED` immediately.
6. `send_via_transport()` returns `ERR_EXPIRED` to `send()`.
7. `send()` receives `ERR_EXPIRED` from `send_via_transport()`. The reliability_class branch that would call `track()` or `schedule()` is evaluated AFTER `send_via_transport()` in UC_02 and UC_03 — but here `send_via_transport()` already returned an error, so neither `track()` nor `schedule()` is called.
8. Returns `Result::ERR_EXPIRED` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                         [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                      [MessageId.cpp]
 └── DeliveryEngine::send_via_transport()      [DeliveryEngine.cpp]
      ├── timestamp_expired()                  [Timestamp.hpp]  <- returns true; expiry detected
      └── Logger::log(WARNING_LO, ...)         [Logger.hpp]
          [returns ERR_EXPIRED immediately; TcpBackend::send_message() NOT called]
```

---

## 5. Key Components Involved

- **`DeliveryEngine::send_via_transport()`** — The expiry gate. It is the single choke point for all reliability classes; every send passes through it before touching the transport.
- **`timestamp_expired()`** — Inline function in `Timestamp.hpp`. Returns `true` if `expiry_time_us != 0 && now_us >= expiry_time_us`. Requires only two integer comparisons.
- **`Logger`** — Emits a `WARNING_LO` log entry identifying the dropped message by ID.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `expiry_time_us != 0 && now_us >= expiry_time_us` | Log + return `ERR_EXPIRED` | Proceed to `m_transport->send_message()` |
| `expiry_time_us == 0` | Never expires (special case in `timestamp_expired`) | Normal expiry check |

---

## 7. Concurrency / Threading Behavior

- Fully synchronous in the caller's thread.
- `timestamp_expired()` reads only its two arguments; no shared state accessed.
- No `std::atomic` operations; no threading concerns specific to this path.

---

## 8. Memory & Ownership Semantics

- `MessageEnvelope work` — ~4152 bytes on stack; created but immediately discarded when `ERR_EXPIRED` is returned.
- Logger uses a 512-byte stack buffer in `Logger::log()` for `snprintf`.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **`ERR_EXPIRED`:** The only result. State is consistent: no tracker slots opened, no socket write attempted, counter in `MessageIdGen` advanced by 1 (this is a minor side effect).
- **Caller action:** Caller should treat `ERR_EXPIRED` as a terminal condition for this envelope; retrying with the same envelope and an older `now_us` is incorrect.

---

## 10. External Interactions

- **`Logger::log()`** writes to `stderr` via `fprintf`. This is the only external interaction; no socket I/O occurs.
- No POSIX socket calls, file I/O, or hardware interaction.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `MessageIdGen` | `m_counter` | N | N+1 (consumed even for expired message) |
| `stderr` | output | — | one WARNING_LO log line |

No `AckTracker`, `RetryManager`, `DuplicateFilter`, or socket state is modified.

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::send(envelope, now_us)  [expiry_time_us <= now_us]
       -> MessageIdGen::next()               [counter consumed]
       -> DeliveryEngine::send_via_transport()
            -> timestamp_expired()           [returns true]
            -> Logger::log(WARNING_LO, ...)  [logs drop]
            <- ERR_EXPIRED
  <- ERR_EXPIRED
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called with valid transport pointer.
- The User has set `envelope.expiry_time_us` to a value in the past relative to `now_us`.

**Runtime:**
- Expiry check occurs on every `send()` call before any transport interaction. There is no special initialization required; `timestamp_expired()` is stateless.

---

## 14. Known Risks / Observations

- **`MessageIdGen` counter consumed for expired messages:** If the application sends many expired envelopes in rapid succession, IDs are consumed (counter advances) even though nothing is transmitted. This has no functional impact (IDs are 64-bit) but is worth noting for audit trails.
- **Expiry granularity:** `now_us` is microsecond-precision from `CLOCK_MONOTONIC`. If the User provides a stale `now_us`, messages might be incorrectly dropped or incorrectly forwarded.
- **`expiry_time_us == 0` means never-expires:** The zero sentinel is checked inside `timestamp_expired()`; messages with `expiry_time_us == 0` are never dropped.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The `MessageIdGen` counter advances even for expired messages. This is inferred from the code path where `m_id_gen.next()` is called unconditionally before `send_via_transport()`.
- `[ASSUMPTION]` `timestamp_expired()` returns `false` for `expiry_time_us == 0`, treating zero as "never expires." This is read directly from `Timestamp.hpp`.
