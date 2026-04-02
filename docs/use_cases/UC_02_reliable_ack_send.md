# UC_02 — RELIABLE_ACK send: single transmission, ACK slot allocated in AckTracker

**HL Group:** HL-2 — Send with Acknowledgement
**Actor:** User
**Requirement traceability:** REQ-3.3.2, REQ-3.2.4, REQ-3.2.1, REQ-4.1.2, REQ-7.1.1

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send(envelope, now_us)` with `envelope.reliability_class == ReliabilityClass::RELIABLE_ACK`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Transmit the envelope exactly once, register the message in `AckTracker` so its acknowledgement can be detected (or timed out), and return `Result::OK`.
- **Success outcome:** `Result::OK` returned. The serialized envelope is on the wire; an ACK slot in `AckTracker` is in state `PENDING` with a deadline of `now_us + recv_timeout_ms * 1000`.
- **Error outcomes:**
  - `Result::ERR_EXPIRED` — expiry check fails in `send_via_transport()` before the socket write.
  - `Result::ERR_FULL` — transport's `send_message()` returns `ERR_FULL`.
  - `Result::ERR_FULL` from `AckTracker::track()` (tracker full) — logged at `WARNING_LO` but send still returns `OK` (track failure is non-fatal).
  - Any non-OK from transport propagated upward.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(const MessageEnvelope& envelope, uint64_t now_us);
```

Synchronous call in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `m_id_gen.next()` assigns a new monotonic `message_id`; written into `MessageEnvelope work` (stack copy).
4. `work.reliability_class == RELIABLE_ACK` — true.
5. **`send_via_transport(work, now_us)`** is called first to put the message on the wire.
6. Inside `send_via_transport()`: expiry check; then `m_transport->send_message(work)` → `TcpBackend::send_message()`. Serialization, impairment pass-through, and `::send()` happen as in UC_01. Returns `Result::OK` on success.
7. Back in `send()`: `send_res` is checked.
   - If `send_res != Result::OK`: return `send_res` immediately; ACK slot is NOT allocated.
8. On `send_res == Result::OK`:
9. **`m_ack_tracker.track(work.message_id, work.source_id, deadline_us)`** is called (`AckTracker.cpp`). `deadline_us = now_us + (uint64_t)m_cfg.recv_timeout_ms * 1000ULL`.
   - Inside `track()`: linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]` for first `FREE` slot.
   - If found: sets `entry.state = PENDING`, `entry.message_id = message_id`, `entry.source_id = source_id`, `entry.deadline_us = deadline_us`. Returns `Result::OK`.
   - If no free slot: returns `Result::ERR_FULL`.
10. `track_res != Result::OK` — logged at `WARNING_LO` ("AckTracker full — send proceeds without tracking"). Send still returns `Result::OK`.
11. `NEVER_COMPILED_OUT_ASSERT` on the postcondition in `send()`.
12. Returns `Result::OK` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                         [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                      [MessageId.cpp]
 ├── DeliveryEngine::send_via_transport()      [DeliveryEngine.cpp]
 │    ├── timestamp_expired()                  [Timestamp.hpp]
 │    └── TcpBackend::send_message()           [TcpBackend.cpp]
 │         ├── ImpairmentEngine::process_outbound()
 │         ├── ImpairmentEngine::collect_deliverable()
 │         ├── Serializer::serialize()
 │         └── send_to_all_clients() -> ::send()
 └── AckTracker::track()                       [AckTracker.cpp]
      └── [linear scan for FREE slot]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Coordinates send + ACK slot registration. The two steps (wire send then track) are sequenced deliberately: if the wire send fails, no slot is wasted.
- **`MessageIdGen`** — Assigns the unique ID that AckTracker will look for when an ACK arrives.
- **`AckTracker`** — Fixed-capacity table (32 slots). Stores `(message_id, source_id, deadline_us)` in `PENDING` state. Required so `sweep_ack_timeouts()` and `on_ack()` can resolve outstanding sends.
- **`TcpBackend`** — Performs the actual socket write. Identical to UC_01 path.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `reliability_class == RELIABLE_ACK` | Proceed to track in AckTracker after send | Not this UC |
| `send_res != OK` | Return send_res; skip AckTracker::track() | Call track() |
| AckTracker slot found | Entry set to PENDING; track() returns OK | track() returns ERR_FULL |
| `track_res != OK` | Log warning; return OK anyway | Return OK |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread; no new threads.
- `AckTracker::m_entries` is a plain fixed array; no atomic operations. Not safe to call `send()` and `sweep_ack_timeouts()` from different threads without external locking.
- `MessageIdGen::m_counter` is a plain `uint32_t`; same single-thread contract as UC_01.

---

## 8. Memory & Ownership Semantics

- `MessageEnvelope work` — stack copy (~4152 bytes) in `DeliveryEngine::send()`.
- `AckTracker::m_entries[ACK_TRACKER_CAPACITY]` — fixed array of 32 `AckEntry` structs; owned by `AckTracker` value member inside `DeliveryEngine`. No heap allocation.
- `m_wire_buf[8192]` in `TcpBackend` — as in UC_01.
- Power of 10 Rule 3: no heap allocation on this path.

---

## 9. Error Handling Flow

- **`ERR_EXPIRED`:** Expiry check in `send_via_transport()` fires before any socket or tracker interaction. No slot consumed. Caller should discard.
- **Transport failure:** `send_res != OK` returned before `track()` is called — tracker is clean.
- **`AckTracker` full:** `track_res != OK` logged and suppressed; send returns `OK`. The message is on the wire but cannot be sweep-detected for timeout. Caller has no direct notification of this condition.

---

## 10. External Interactions

- **POSIX `::send()`:** TCP socket `m_client_fds[idx]` in `TcpBackend`, outbound direction.
- No file I/O, signals, or hardware.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `MessageIdGen` | `m_counter` | N | N+1 |
| `AckTracker` | `m_entries[slot].state` | `FREE` | `PENDING` |
| `AckTracker` | `m_entries[slot].message_id` | undefined | assigned message_id |
| `AckTracker` | `m_entries[slot].deadline_us` | undefined | `now_us + timeout_us` |
| TCP socket | kernel send buffer | previous | + serialized frame |

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::send(envelope, now_us)
       -> MessageIdGen::next()
       -> DeliveryEngine::send_via_transport()
            -> TcpBackend::send_message()
                 -> Serializer::serialize()
                 -> send_to_all_clients() -> ::send()
            <- Result::OK
       -> AckTracker::track(message_id, source_id, deadline_us)
            [scan FREE slot; set PENDING]
            <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called; `AckTracker::init()` called (all slots set to FREE).
- `TcpBackend::init()` called; connection established.

**Runtime:**
- Each RELIABLE_ACK send allocates one `AckTracker` slot. Maximum 32 outstanding RELIABLE_ACK messages can be tracked simultaneously.

---

## 14. Known Risks / Observations

- **AckTracker slot silently lost:** When the tracker is full, `track()` failure is logged but the send returns `OK`. The caller has no indication that timeout detection is disabled for this message.
- **No automatic retry:** Unlike RELIABLE_RETRY, RELIABLE_ACK sends only once. If the ACK never arrives, `sweep_ack_timeouts()` will report the timeout but there is no automatic resend.
- **Linear scan O(N):** `AckTracker::track()` uses a linear scan over 32 entries — bounded and fast, but worth noting for WCET analysis.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `deadline_us = now_us + recv_timeout_ms * 1000` — this calculation is inferred from how `m_cfg.recv_timeout_ms` is used in `send()` in `DeliveryEngine.cpp`; confirmed by test `test_sweep_detects_timeout` which sets `recv_timeout_ms = 1000` and sweeps with `now_us + 10000000`.
