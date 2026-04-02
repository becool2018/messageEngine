# UC_09 — Incoming message dropped at receive because expiry_time_us has passed

**HL Group:** HL-5 — Receive a Message
**Actor:** System
**Requirement traceability:** REQ-3.3.4, REQ-3.2.7, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` dequeues a DATA envelope from the transport ring buffer and detects that `envelope.expiry_time_us != 0` and `envelope.expiry_time_us <= now_us`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Drop a stale DATA message before delivering it to the application, preventing the caller from acting on an envelope whose deadline has passed.
- **Success outcome:** `Result::ERR_EXPIRED` is returned. No application-level delivery occurs; the envelope is discarded.
- **Error outcomes:** `Result::ERR_EXPIRED` is the only result for this UC (the drop is the success condition from a correctness standpoint).

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::receive(MessageEnvelope& out, uint32_t timeout_ms, uint64_t now_us);
```

Called by the User. Expiry detection is an internal branch within `receive()`.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::receive(out, timeout_ms, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)`.
3. `m_transport->receive_message(raw, timeout_ms)` — pops one envelope from the `RingBuffer` (which was previously pushed by `recv_from_client()`). Returns `Result::OK` with `raw` populated.
4. `envelope_is_data(raw)` — true for `MessageType::DATA`.
5. **`timestamp_expired(raw.expiry_time_us, now_us)`** is called (`Timestamp.hpp`). Returns `true` because `raw.expiry_time_us != 0 && now_us >= raw.expiry_time_us`.
6. `Logger::log(WARNING_LO, "DeliveryEngine", "Received expired DATA message; dropping. id=%llu ...")`.
7. Returns `Result::ERR_EXPIRED` immediately. No dedup check, no ACK auto-send, no copy to `out`.

---

## 4. Call Tree

```
DeliveryEngine::receive(out, timeout_ms, now_us)   [DeliveryEngine.cpp]
 ├── TcpBackend::receive_message(raw, timeout_ms)  [TcpBackend.cpp]
 │    └── RingBuffer::pop(raw)                     [RingBuffer.hpp]
 ├── envelope_is_data(raw)                         [MessageEnvelope.hpp]
 ├── timestamp_expired(raw.expiry_time_us, now_us) [Timestamp.hpp]  <- true
 └── Logger::log(WARNING_LO, ...)                  [Logger.hpp]
     [returns ERR_EXPIRED; dedup/ACK paths NOT entered]
```

---

## 5. Key Components Involved

- **`DeliveryEngine::receive()`** — The expiry gate on the receive path. Checks expiry before any dedup or ACK processing.
- **`timestamp_expired()`** — Stateless inline check: `return (expiry_time_us != 0U) && (now_us >= expiry_time_us)`.
- **`TcpBackend::receive_message()`** — Provides the popped raw envelope; no expiry knowledge.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `envelope_is_data(raw)` | Check expiry | Handle as control message |
| `timestamp_expired(expiry_time_us, now_us)` | Log and return `ERR_EXPIRED` | Continue to dedup / delivery |
| `expiry_time_us == 0` | `timestamp_expired` returns false (never expires) | Normal expiry check |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread.
- `timestamp_expired()` reads only its arguments; no shared state.
- No atomics or threading concerns specific to this path.

---

## 8. Memory & Ownership Semantics

- `raw` — stack-allocated `MessageEnvelope` (~4152 bytes) inside `receive()`. Discarded (not copied to `out`) when expired.
- `out` — caller-provided; untouched on `ERR_EXPIRED`.
- No heap allocation.

---

## 9. Error Handling Flow

- **`ERR_EXPIRED`:** The only result. The envelope has been removed from the ring buffer (consumed by `pop()`), so it will not be re-delivered. The caller should log and loop back to call `receive()` again.
- Dedup state is NOT updated for expired messages (check happens before `check_and_record()`).
- No ACK is sent for expired RELIABLE_ACK or RELIABLE_RETRY messages.

---

## 10. External Interactions

- **`stderr`:** `Logger::log()` writes one WARNING_LO line.
- No network calls on the expired-drop path (the envelope was already consumed from the ring buffer by the transport receive call).

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RingBuffer` | `m_tail` (atomic) | T | T+1 (acquire load; envelope consumed) |
| `stderr` | output | — | one WARNING_LO log line |

No `DuplicateFilter`, `AckTracker`, or `RetryManager` state is modified.

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::receive(out, timeout_ms, now_us)
       -> TcpBackend::receive_message(raw, timeout_ms)
            -> RingBuffer::pop(raw)             [m_tail acquire load; T+1]
            <- Result::OK; raw populated
       -> envelope_is_data(raw)                 <- true
       -> timestamp_expired(expiry_time_us, now_us)  <- true (expired)
       -> Logger::log(WARNING_LO, "expired...")
       <- ERR_EXPIRED
  <- ERR_EXPIRED
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` and transport `init()` called.
- An envelope with `expiry_time_us` in the past was queued (possibly sent by a peer before the deadline, then delayed in transit or in the ring buffer).

**Runtime:**
- Expiry check is evaluated on every `receive()` call that returns a DATA envelope, regardless of reliability class.

---

## 14. Known Risks / Observations

- **Consumed but not delivered:** The expired envelope is removed from the ring buffer and cannot be retrieved. If the sender does not retry, the data is permanently lost.
- **No auto-NAK:** No NAK is automatically sent to the peer to indicate expiry. The peer's AckTracker or RetryManager will eventually time out independently.
- **`now_us` staleness:** If the User passes an old `now_us`, messages may be incorrectly dropped or incorrectly kept. The system relies on the caller providing a current monotonic time.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Expiry check happens before dedup check. The order (expiry first, then dedup) is read directly from `DeliveryEngine::receive()` in `DeliveryEngine.cpp`.
- `[ASSUMPTION]` Expired RELIABLE_ACK messages do not trigger an auto-ACK send. The auto-ACK send in `receive()` is only reached after the expiry branch is passed (for non-expired messages).
