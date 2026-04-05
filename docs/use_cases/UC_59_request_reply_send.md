# UC_59 — Request/reply: send a request and receive the matching response

**HL Group:** HL-28 — Bounded Request/Response via RequestReplyEngine
**Actor:** Application (requester side)
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** Application calls `RequestReplyEngine::send_request(destination, payload, len, timeout_us, now_us, correlation_id_out)`.
- **Goal:** Deliver a typed request to a remote node, track the correlation ID, and later retrieve the matching response via `receive_response()`.
- **Success outcome:** `Result::OK` from both calls; application payload round-trips correctly.
- **Error outcomes:**
  - `ERR_FULL` — pending request table (`MAX_PENDING_REQUESTS = 16`) is exhausted, or payload + header exceeds `MSG_MAX_PAYLOAD_BYTES`.
  - `ERR_INVALID` — engine not initialized.
  - `ERR_EMPTY` from `receive_response()` — response not yet arrived.
  - `ERR_INVALID` from `receive_response()` — slot was freed by `sweep_timeouts()` after timeout elapsed.

---

## 2. Entry Points

```cpp
// src/core/RequestReplyEngine.cpp
Result RequestReplyEngine::send_request(NodeId destination,
                                        const uint8_t* app_payload,
                                        uint32_t app_payload_len,
                                        uint64_t timeout_us,
                                        uint64_t now_us,
                                        uint64_t& correlation_id_out);

Result RequestReplyEngine::receive_response(uint64_t correlation_id,
                                            uint8_t* app_payload_buf,
                                            uint32_t buf_cap,
                                            uint32_t& app_payload_len_out,
                                            uint64_t now_us);
```

---

## 3. End-to-End Control Flow — send_request()

1. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — pre-condition check.
2. `find_free_pending()` — linear scan over `m_pending[MAX_PENDING_REQUESTS]`; if full, return `ERR_FULL`.
3. Assign `cid = m_cid_gen++` (local monotonic counter; wraps at UINT64_MAX→1).
4. `build_wire_payload(REQUEST, cid, app_payload, app_len, wire_buf, MSG_MAX_PAYLOAD_BYTES)` — writes `RRHeader(kind=0, correlation_id=cid, pad=0)` + app bytes into a stack buffer; returns 0 on overflow.
5. Construct `MessageEnvelope`: `message_type=DATA`, `reliability_class=RELIABLE_RETRY`, `expiry_time_us = now_us + timeout_us`, `payload_length = wire_len`.
6. `m_engine->send(env, now_us)` — assigns `env.source_id`, `env.message_id`; enqueues to transport; schedules retry if needed (REQ-3.3.3).
7. Record pending slot: `m_pending[slot] = {cid, expires, active=true}`.
8. Set `correlation_id_out = cid`; return `Result::OK`.

## 4. End-to-End Control Flow — receive_response()

1. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — pre-condition check.
2. `pump_inbound(now_us)` — drains up to `MAX_STASH_SIZE` envelopes from the underlying engine (see §5).
3. `find_pending(correlation_id)` — linear scan; return `ERR_INVALID` if not found (slot freed or wrong ID).
4. If `m_pending[idx].stash_ready == false`, return `ERR_EMPTY`.
5. `memcpy` stash payload to caller buffer; truncate to `buf_cap` if needed.
6. Free the pending slot: `m_pending[idx].active = false`; decrement `m_pending_count`.
7. Return `Result::OK`.

---

## 5. pump_inbound() Drain Loop

Called at the start of every `receive_response()` and `receive_request()` call.

```
for iter in [0, MAX_STASH_SIZE):
    engine.receive(env, timeout_ms=0, now_us)  // non-blocking
    if ERR_TIMEOUT / ERR_EMPTY → break
    if not envelope_is_data(env) → continue
    dispatch_inbound_envelope(env):
        parse_rr_header(env.payload, env.payload_length, hdr)
        if hdr.kind == RESPONSE → handle_inbound_response(hdr.correlation_id, …)
        else                    → handle_inbound_request(env.source_id, hdr.correlation_id, …)
```

`handle_inbound_response()`: finds matching `m_pending` slot; sets `stash_ready=true` and copies payload.  Unknown `correlation_id` → silent drop (no crash).

---

## 6. Call Tree

```
RequestReplyEngine::send_request()             [RequestReplyEngine.cpp]
 ├── find_free_pending()                        [RequestReplyEngine.cpp]
 ├── build_wire_payload()                       [RequestReplyEngine.cpp]
 └── DeliveryEngine::send()                     [DeliveryEngine.cpp]
      ├── MessageIdGen::next()                  [MessageId.cpp]
      ├── reserve_bookkeeping()                 [DeliveryEngine.cpp]
      │    ├── AckTracker::track()              [AckTracker.cpp]
      │    └── RetryManager::schedule()         [RetryManager.cpp]
      └── DeliveryEngine::send_via_transport()  [DeliveryEngine.cpp]
           └── LocalSimHarness::send_message()  [LocalSimHarness.cpp]

RequestReplyEngine::receive_response()         [RequestReplyEngine.cpp]
 ├── pump_inbound()                             [RequestReplyEngine.cpp]
 │    └── DeliveryEngine::receive()             [DeliveryEngine.cpp]  (×≤MAX_STASH_SIZE)
 │         └── dispatch_inbound_envelope()      [RequestReplyEngine.cpp]
 │              └── handle_inbound_response()   [RequestReplyEngine.cpp]
 └── (copy stash → caller buffer)
```

---

## 7. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `find_free_pending() >= MAX_PENDING_REQUESTS` | Return `ERR_FULL` | Continue with free slot |
| `build_wire_payload()` returns 0 | Return `ERR_FULL` | Wire payload ready |
| `engine.send()` returns non-OK | Log warning; propagate error | Record pending slot |
| `find_pending(cid) >= MAX_PENDING_REQUESTS` | Return `ERR_INVALID` | Check stash |
| `stash_ready == false` | Return `ERR_EMPTY` | Copy payload; free slot |
| `hdr.kind == RESPONSE` (in pump_inbound) | Route to `handle_inbound_response()` | Route to `handle_inbound_request()` |
| Response `correlation_id` unknown | Silent drop | Stash in matching pending slot |

---

## 8. Wire Format

Every request payload begins with a 12-byte `RRHeader` (defined in `RequestReplyHeader.hpp`):

```
Byte 0    : kind = 0 (REQUEST)
Bytes 1-8 : correlation_id (uint64_t, little-endian in struct; memcpy'd)
Bytes 9-11: _pad = 0
```

Application bytes follow immediately after byte 11. The receiver strips the header and delivers only the application bytes to `receive_request()`.

---

## 9. Concurrency / Threading Behavior

- `RequestReplyEngine` is single-threaded; no locks.
- All state (`m_pending`, `m_request_stash`, `m_cid_gen`) is modified only by `send_request()`, `receive_request()`, `receive_response()`, and `sweep_timeouts()` — never concurrently.

---

## 10. Memory & Ownership Semantics

- **No heap allocation** (Power of 10 Rule 3).
- `wire_buf[MSG_MAX_PAYLOAD_BYTES]` — stack buffer in `send_request()`; holds RRHeader + app payload.
- `m_pending[MAX_PENDING_REQUESTS]` — member array; each entry holds a stash payload of `APP_PAYLOAD_CAP = MSG_MAX_PAYLOAD_BYTES - 12` bytes.
- `m_engine` — non-owning pointer; owned by the caller.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RequestReplyEngine` | `m_cid_gen` | N | N+1 (or 1 on wrap) |
| `RequestReplyEngine` | `m_pending[slot]` | inactive | active; correlation_id set; expires_us set |
| `RequestReplyEngine` | `m_pending_count` | N | N+1 |
| `DeliveryEngine` | `m_id_gen`, `m_ack_tracker`, `m_retry_manager` | as before send | updated per RELIABLE_RETRY semantics |
| `RequestReplyEngine` (after receive_response) | `m_pending[slot]` | stash_ready=true | active=false; stash_ready=false |

---

## 12. Error Handling Flow

- **ERR_FULL (pending table):** Return immediately before any engine call; no side effects.
- **ERR_FULL (payload overflow):** Return immediately; `m_cid_gen` was already incremented — the correlation ID is "consumed" but never sent.
- **engine.send() failure:** Pending slot NOT recorded (rollback); return the engine error to caller.
- **ERR_EMPTY from receive_response:** Response not yet delivered; caller should retry on next tick.
- **timeout / sweep_timeouts:** Pending slot freed; subsequent receive_response returns ERR_INVALID.

---

## 13. Known Risks / Observations

- **Correlation ID leak on payload overflow:** `m_cid_gen` is incremented before `build_wire_payload()` fails. The skipped ID is harmless (never referenced in a pending slot) but represents a gap in the monotonic sequence.
- **MAX_PENDING_REQUESTS = 16:** An application that issues many concurrent requests without polling `receive_response()` will hit `ERR_FULL`. Call `sweep_timeouts()` periodically to reclaim expired slots.
- **Retry amplification:** Requests use `RELIABLE_RETRY`; if the remote node is slow to ACK, the retry manager may resend the request. The responder's duplicate filter (on a RELIABLE_RETRY receive path) suppresses duplicate DATA delivery. The correlation ID in the RRHeader is stable across retries.
