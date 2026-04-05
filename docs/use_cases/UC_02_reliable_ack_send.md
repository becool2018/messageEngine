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
  - `Result::ERR_FULL` from `reserve_bookkeeping()` (AckTracker full) — logged at `WARNING_HI`; send returns `ERR_FULL` immediately with no wire I/O and no slot wasted.
  - Any non-OK from transport propagated upward.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& envelope, uint64_t now_us);
```

Synchronous call in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))`.
3. `m_id_gen.next()` assigns a new monotonic `message_id`; written into `MessageEnvelope work` (stack copy).
4. `work.reliability_class == RELIABLE_ACK` — true.
5. **`reserve_bookkeeping(m_ack_tracker, m_retry_manager, work, m_cfg, now_us)`** is called (static helper, `DeliveryEngine.cpp`). Inside: `ack_deadline = now_us + recv_timeout_ms * 1000`. Calls `m_ack_tracker.track(work, ack_deadline)`.
   - Inside `track()`: linear scan of `m_entries[0..ACK_TRACKER_CAPACITY-1]` for first `FREE` slot.
   - If found: sets `entry.state = PENDING`, `entry.message_id`, `entry.source_id`, `entry.deadline_us`. Returns `Result::OK`.
   - If no free slot: logs `WARNING_HI`; returns `Result::ERR_FULL`. `reserve_bookkeeping()` returns `ERR_FULL` to `send()`.
   - For `RELIABLE_ACK`: after successful `track()`, `reserve_bookkeeping()` returns `OK` (no `schedule()` needed).
6. `book_res != OK` — `record_send_failure()` called; `send()` returns `ERR_FULL` immediately. No wire I/O occurs.
7. On `book_res == OK` (slot reserved): **`send_fragments(work, now_us)`** is called, which calls `send_via_transport()` per frame. Inside: expiry check, then `m_transport->send_message(work)` → `TcpBackend::send_message()`. Serialization, impairment pass-through, and `::send()` happen as in UC_01.
   - If `send_fragments()` returns `ERR_IO` (first frame failed — nothing on wire): **`rollback_on_transport_failure()`** is called via `handle_send_fragments_failure()`, which calls `m_ack_tracker.cancel(work.source_id, work.message_id)` to free the slot (PENDING→FREE, no stat bump). `send()` returns `ERR_IO`.
   - If `send_fragments()` returns `ERR_IO_PARTIAL` (≥1 frame already transmitted): bookkeeping slot is **preserved** (AckTracker deadline remains active). `send()` returns `ERR_IO`. The ACK timeout sweep will eventually fire if no ACK arrives.
8. On `send_res == OK`: `++m_stats.msgs_sent`.
9. `NEVER_COMPILED_OUT_ASSERT(env.source_id == m_local_id)` and `NEVER_COMPILED_OUT_ASSERT(env.message_id != 0ULL)`.
10. Returns `Result::OK` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                         [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                      [MessageId.cpp]
 ├── reserve_bookkeeping()                     [DeliveryEngine.cpp]
 │    └── AckTracker::track()                  [AckTracker.cpp]
 │         └── [linear scan for FREE slot; set PENDING]
 ├── send_fragments()                          [DeliveryEngine.cpp]
 │    └── DeliveryEngine::send_via_transport() [DeliveryEngine.cpp]  (×frames)
 │         ├── timestamp_expired()             [Timestamp.hpp]
 │         └── TcpBackend::send_message()      [TcpBackend.cpp]
 │              ├── ImpairmentEngine::process_outbound()
 │              ├── ImpairmentEngine::collect_deliverable()
 │              ├── Serializer::serialize()
 │              └── send_to_all_clients() -> ::send()
 └── [on transport failure] rollback_on_transport_failure()
      └── AckTracker::cancel()                 [AckTracker.cpp]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Coordinates send + ACK slot registration. The two steps (bookkeeping then wire send) are sequenced deliberately: `reserve_bookkeeping()` runs first so that if the tracker is full, the caller gets `ERR_FULL` before any byte is sent to the wire. If `send_fragments()` fails and nothing reached the wire (`ERR_IO`), `rollback_on_transport_failure()` frees the `AckTracker` slot via `cancel()` so no spurious timeout fires. If `send_fragments()` fails after at least one frame was transmitted (`ERR_IO_PARTIAL`), the slot is intentionally **preserved** — rolling it back would orphan partial reassembly state at the receiver and suppress the retry that would recover the message.
- **`MessageIdGen`** — Assigns the unique ID that AckTracker will look for when an ACK arrives.
- **`AckTracker`** — Fixed-capacity table (32 slots). Stores `(message_id, source_id, deadline_us)` in `PENDING` state. Required so `sweep_ack_timeouts()` and `on_ack()` can resolve outstanding sends.
- **`TcpBackend`** — Performs the actual socket write. Identical to UC_01 path.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `reliability_class == RELIABLE_ACK` | Enter `reserve_bookkeeping()` path | Not this UC |
| AckTracker slot available (inside `reserve_bookkeeping`) | `track()` OK; proceed | `track()` returns `ERR_FULL`; `send()` returns `ERR_FULL` |
| `book_res != OK` | Return `ERR_FULL` without I/O | Proceed to `send_via_transport()` |
| `send_fragments()` returns `ERR_IO` (nothing sent) | `rollback_on_transport_failure()` via `handle_send_fragments_failure()`; return `ERR_IO` | Proceed; `msgs_sent++` |
| `send_fragments()` returns `ERR_IO_PARTIAL` (≥1 frame sent) | Preserve AckTracker slot; return `ERR_IO` | — |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread; no new threads.
- `AckTracker::m_entries` is a plain fixed array; no atomic operations. Not safe to call `send()` and `sweep_ack_timeouts()` from different threads without external locking.
- `MessageIdGen::m_counter` is a plain `uint32_t`; same single-thread contract as UC_01.

---

## 8. Memory & Ownership Semantics

- `MessageEnvelope work` — stack copy (4144 bytes) in `DeliveryEngine::send()`.
- `AckTracker::m_entries[ACK_TRACKER_CAPACITY]` — fixed array of 32 `AckEntry` structs; owned by `AckTracker` value member inside `DeliveryEngine`. No heap allocation.
- `m_wire_buf[8192]` in `TcpBackend` — as in UC_01.
- Power of 10 Rule 3: no heap allocation on this path.

---

## 9. Error Handling Flow

- **`ERR_EXPIRED`:** Expiry check in `send_via_transport()` fires before any socket interaction. No slot consumed. Caller should discard.
- **`AckTracker` full:** `reserve_bookkeeping()` returns `ERR_FULL`; `send()` returns `ERR_FULL` immediately. No bytes are sent to the wire. Caller must handle the `ERR_FULL`.
- **Transport failure after bookkeeping — nothing sent:** `rollback_on_transport_failure()` via `handle_send_fragments_failure()` calls `cancel()` to free the `AckTracker` slot (PENDING→FREE), preventing a spurious timeout sweep. `send()` returns `ERR_IO`.
- **Transport failure after bookkeeping — partial send:** If ≥1 fragment was already transmitted (`ERR_IO_PARTIAL`), the `AckTracker` slot is **preserved**. The ACK timeout sweep will fire if no ACK arrives, allowing the application to detect the failure. `send()` returns `ERR_IO` (normalised).

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
       -> reserve_bookkeeping()
            -> AckTracker::track(work, deadline_us)
                 [scan FREE slot; set PENDING]
                 <- Result::OK
            <- Result::OK
       -> send_fragments()
            -> DeliveryEngine::send_via_transport()
                 -> TcpBackend::send_message()
                      -> Serializer::serialize()
                      -> send_to_all_clients() -> ::send()
                 <- Result::OK
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

- **AckTracker full returns `ERR_FULL`:** When the tracker is full, `send()` returns `ERR_FULL` before any wire I/O. The caller must handle `ERR_FULL` — unlike the previous behavior where tracking failure was silent.
- **No automatic retry:** Unlike RELIABLE_RETRY, RELIABLE_ACK sends only once. If the ACK never arrives, `sweep_ack_timeouts()` will report the timeout but there is no automatic resend.
- **Linear scan O(N):** `AckTracker::track()` uses a linear scan over 32 entries — bounded and fast, but worth noting for WCET analysis.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `deadline_us = now_us + recv_timeout_ms * 1000` — this calculation is inferred from how `m_cfg.recv_timeout_ms` is used in `send()` in `DeliveryEngine.cpp`; confirmed by test `test_sweep_detects_timeout` which sets `recv_timeout_ms = 1000` and sweeps with `now_us + 10000000`.
