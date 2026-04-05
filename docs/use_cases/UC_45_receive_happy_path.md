# UC_45 — Receive happy path: DATA envelope delivered successfully (dedup passes, not expired)

**HL Group:** HL-5 — Receive a Message
**Actor:** User
**Requirement traceability:** REQ-3.3.1, REQ-3.3.3, REQ-3.3.4, REQ-3.2.6, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::receive(env, timeout_ms, now_us)` after a DATA envelope has arrived. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Return the next available DATA envelope to the caller. The envelope must pass deduplication (not a repeated `(source_id, message_id)`) and expiry checks (not past `expiry_time_us`) before delivery.
- **Success outcome:** `receive()` returns `Result::OK`. `env` contains the delivered envelope. If the envelope's `reliability_class == RELIABLE_ACK` or `RELIABLE_RETRY`, an ACK envelope is automatically sent back to the originator.
- **Error outcomes:**
  - `Result::ERR_TIMEOUT` — no message available within `timeout_ms` milliseconds.
  - `Result::ERR_DROPPED` — message received but dropped (duplicate or expired); covered by UC_07 and UC_09.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us);
```

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::receive(env, timeout_ms, now_us)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
3. `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)`.
4. Compute deadline: `uint64_t deadline_us = timestamp_now_us() + (uint64_t)timeout_ms * 1000ULL`.
5. **Poll loop** (bounded by `timeout_ms` deadline — Rule 2 infrastructure deviation):
   a. `m_transport->receive_message(recv_timeout_ms, &raw_env)` — block up to `recv_timeout_ms` ms waiting for a frame from the transport.
   b. If `Result::ERR_TIMEOUT`: check `timestamp_now_us() >= deadline_us` → if expired, return `ERR_TIMEOUT`. Else retry.
   c. On `Result::OK`: proceed to filtering.
6. **`envelope_valid(&raw_env)`** — structural validity check. If false: discard; continue poll.
7. **`envelope_is_control(&raw_env)`** — if true:
   - If `message_type == ACK`: call `AckTracker::on_ack(raw_env.message_id)` + `RetryManager::on_ack(raw_env.message_id)`. Continue poll.
   - Other control types (NAK, HEARTBEAT): log at INFO; continue poll.
8. **`envelope_is_data(&raw_env)`** — if true (the happy path):
9. **Expiry check:** `timestamp_expired(raw_env.expiry_time_us)` → if expired: log WARNING_LO; continue poll (UC_09 path).
10. **Dedup check (RELIABLE_RETRY only):** if `raw_env.reliability_class == RELIABLE_RETRY`: call `DuplicateFilter::check_and_record(raw_env.source_id, raw_env.message_id)`. If `Result::ERR_DUPLICATE`: log INFO; return `ERR_DUPLICATE` (UC_07 path).
11. **ACK generation (RELIABLE_ACK or RELIABLE_RETRY):**
    - `envelope_make_ack(ack_env, raw_env, m_local_id, now_us)` — construct ACK with matching `message_id` and `destination_id = raw_env.source_id`.
    - `m_transport->send_message(ack_env)` — send ACK back. Return value checked; log WARNING_LO on failure.
12. `env` already holds the delivered envelope (written in place by `m_transport->receive_message()`).
13. Returns `Result::OK`.

---

## 4. Call Tree

```
DeliveryEngine::receive(env, timeout_ms, now_us)   [DeliveryEngine.cpp]
 ├── TransportInterface::receive_message(env, timeout_ms)  [TcpBackend / UdpBackend / LocalSimHarness]
 ├── check_routing(env, m_local_id, now_us)        [DeliveryEngine.cpp — expiry + destination check]
 ├── envelope_is_control()                         [MessageEnvelope.hpp]
 │    └── process_ack(m_ack_tracker, m_retry_manager, env)  [DeliveryEngine.cpp]
 │         ├── AckTracker::on_ack(src, msg_id)     [AckTracker.cpp]
 │         └── RetryManager::on_ack(src, msg_id)   [RetryManager.cpp]
 ├── envelope_is_data()                            [MessageEnvelope.hpp]
 ├── DuplicateFilter::check_and_record(src, id)    [DuplicateFilter.cpp — RELIABLE_RETRY only]
 ├── send_data_ack(transport, env, local_id, now_us) [DeliveryEngine.cpp — RELIABLE_ACK/RETRY only]
 │    └── envelope_make_ack(ack, env, local_id, now_us)  [MessageEnvelope.hpp]
 └── TransportInterface::send_message(ack_env)     [sends ACK back]
```

---

## 5. Key Components Involved

- **`TransportInterface::receive_message()`** — blocks waiting for a raw frame; returns the deserialized envelope.
- **`envelope_valid()`** — structural gate; prevents processing of malformed frames.
- **`timestamp_expired()`** — receive-side expiry filter (REQ-3.3.4).
- **`DuplicateFilter::check_and_record()`** — dedup gate for RELIABLE_RETRY; linear scan over 128 recent entries (REQ-3.2.6).
- **`envelope_make_ack()`** — constructs the ACK response envelope (REQ-3.2.4).
- **`AckTracker::on_ack()` / `RetryManager::on_ack()`** — resolve outstanding PENDING entries when the ACK side receives an ACK from the remote peer (UC_08, UC_12).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `receive_message()` times out | Check deadline; retry or return ERR_TIMEOUT | Proceed with envelope |
| `envelope_valid()` false | Discard; continue poll | Proceed to data/control check |
| `envelope_is_control()` | Route to ACK/NAK handler; continue poll | Check if data |
| `timestamp_expired()` true | Log; discard; continue poll (UC_09) | Proceed |
| `reliability_class == RELIABLE_RETRY` | `check_and_record()` duplicate check | Skip dedup |
| `check_and_record()` returns `ERR_DUPLICATE` | Return `ERR_DUPLICATE` (UC_07) | Deliver |
| `reliability_class ∈ {RELIABLE_ACK, RELIABLE_RETRY}` | `envelope_make_ack()` + send ACK | Skip ACK generation |

---

## 7. Concurrency / Threading Behavior

- `receive()` is called from the application receive thread; single-threaded per call.
- `DuplicateFilter` members (`m_buf`, `m_count`, `m_write_idx`) are not atomic; must not be accessed from multiple threads simultaneously.
- `AckTracker` and `RetryManager` member access also not thread-safe.
- No `std::atomic` operations in this path within `DeliveryEngine`.

---

## 8. Memory & Ownership Semantics

- `raw_env` — stack-allocated `MessageEnvelope` (4144 bytes) in `receive()` stack frame.
- `ack_env` — stack-allocated `MessageEnvelope` for the ACK response.
- `env` — caller-provided reference; written in place by `m_transport->receive_message()` and the routing/dedup checks operate on the same object.
- No heap allocation. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- **ERR_TIMEOUT:** Deadline elapsed with no deliverable message. `*out_env` is unmodified. Caller retries.
- **Dropped (duplicate or expired):** Poll continues; these are not returned as errors to the caller. The caller only sees `OK` (delivered) or `ERR_TIMEOUT` (none available). `ERR_DROPPED` may be returned as a distinct code depending on implementation; see UC_07 and UC_09 for detail.
- **ACK send failure:** Logged at WARNING_LO; the incoming message is still delivered to the caller. ACK failure is non-fatal for the receive path.

---

## 10. External Interactions

- **Network I/O:** `m_transport->receive_message()` calls into `TcpBackend` (poll + recv) or `UdpBackend` (recvfrom) which touch POSIX socket APIs.
- **`clock_gettime(CLOCK_MONOTONIC)`:** Called via `timestamp_now_us()` for deadline computation and `timestamp_expired()`.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DuplicateFilter` | `m_buf[m_write_idx]` | prior entry | new `(source_id, message_id)` (RELIABLE_RETRY) |
| `AckTracker` | `m_table[slot].state` | PENDING | ACKED/FREE (if ACK processed) |
| `RetryManager` | `m_table[slot].active` | true | false (if ACK processed) |
| `*out_env` | all fields | undefined | copy of delivered envelope |

---

## 12. Sequence Diagram

```
User -> DeliveryEngine::receive(env, timeout_ms, now_us)
  -> TransportInterface::receive_message(env, timeout_ms)
       <- Result::OK, env filled
  -> check_routing(env, m_local_id, now_us)     <- Result::OK (not expired, addressed to us)
  -> envelope_is_data(env)                      <- true
  [if RELIABLE_RETRY]
  -> DuplicateFilter::check_and_record(src, id) <- Result::OK (not duplicate)
  [if RELIABLE_ACK or RELIABLE_RETRY]
  -> send_data_ack(transport, env, local_id, now_us)
       -> envelope_make_ack(ack_env, env, local_id, now_us)
       -> TransportInterface::send_message(ack_env)  [ACK sent back]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` has been called.
- `m_transport` has been initialized and `m_open == true`.
- A DATA envelope has arrived at the transport layer.

**Runtime:**
- Each call to `receive()` processes at most one envelope (successful path). If the received envelope is dropped (duplicate/expired), the poll loop continues until a deliverable message or timeout.

---

## 14. Known Risks / Observations

- **ACK send on receive path:** The ACK is sent inline during `receive()`; if the transport is slow or the send buffer is full, this adds latency to the receive call.
- **Stack frame size:** `raw_env` and `ack_env` together consume ~8280 bytes of stack. This is the largest single-frame allocation in the system; documented in `docs/STACK_ANALYSIS.md`.
- **Dedup only for RELIABLE_RETRY:** BEST_EFFORT and RELIABLE_ACK messages are not deduplicated; duplicate DATA frames of those classes will be delivered twice.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The inner `recv_timeout_ms` passed to `m_transport->receive_message()` is computed from the remaining deadline, not a fixed value; this prevents the poll loop from blocking past the user-supplied `timeout_ms`.
- `[ASSUMPTION]` `envelope_make_ack()` is called for both RELIABLE_ACK and RELIABLE_RETRY; both classes generate an ACK response.
- `[ASSUMPTION]` Control messages (ACK, NAK, HEARTBEAT) received on the receive path are handled inline in `receive()`, not buffered separately.
