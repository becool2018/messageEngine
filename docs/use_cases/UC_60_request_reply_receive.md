# UC_60 — Request/reply: receive a request and send the matching response

**HL Group:** HL-3 — Reliable Request/Response Exchange
**Actor:** Application (responder side)
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** Remote node has sent a request (UC_59) addressed to this node. Application calls `RequestReplyEngine::receive_request()` to poll for it.
- **Goal:** Retrieve the inbound request payload and correlation ID, process it, then call `send_response()` to return a reply to the originator.
- **Success outcome:** `Result::OK` from `receive_request()`; application payload delivered; `Result::OK` from `send_response()`.
- **Error outcomes:**
  - `ERR_EMPTY` from `receive_request()` — no request has arrived yet.
  - `ERR_INVALID` from `receive_request()` — engine not initialized.
  - `ERR_FULL` from `send_response()` — response payload + header exceeds `MSG_MAX_PAYLOAD_BYTES`.
  - `ERR_INVALID` from `send_response()` — engine not initialized.

---

## 2. Entry Points

```cpp
// src/core/RequestReplyEngine.cpp
Result RequestReplyEngine::receive_request(uint8_t*  app_payload_buf,
                                           uint32_t  buf_cap,
                                           uint32_t& app_payload_len_out,
                                           NodeId&   src_out,
                                           uint64_t& correlation_id_out,
                                           uint64_t  now_us);

Result RequestReplyEngine::send_response(NodeId         destination,
                                         uint64_t       correlation_id,
                                         const uint8_t* app_payload,
                                         uint32_t       app_payload_len,
                                         uint64_t       now_us);
```

---

## 3. End-to-End Control Flow — receive_request()

1. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — pre-condition check.
2. `pump_inbound(now_us)` — drain up to `MAX_STASH_SIZE` envelopes from the engine (see §5 of UC_59 and §6 below).
3. Linear scan over `m_request_stash[MAX_STASH_SIZE]`; return the first active slot.
4. `memcpy` stash payload to `app_payload_buf`; truncate to `buf_cap` if needed.
5. Set `app_payload_len_out`, `src_out = stash.src`, `correlation_id_out = stash.correlation_id`.
6. Free the stash slot: `m_request_stash[i].active = false`; decrement `m_stash_count`.
7. Return `Result::OK`.
8. If no active slot found after full scan, return `Result::ERR_EMPTY`.

## 4. End-to-End Control Flow — send_response()

1. `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — pre-condition check.
2. `build_wire_payload(RESPONSE, correlation_id, app_payload, app_len, wire_buf, MSG_MAX_PAYLOAD_BYTES)` — writes `RRHeader(kind=1, correlation_id, pad=0)` + app bytes into a stack buffer; returns 0 on overflow.
3. If `wire_len == 0`, return `ERR_FULL`.
4. Construct `MessageEnvelope`: `message_type=DATA`, `reliability_class=BEST_EFFORT` (REQ-3.3.2 — fire-and-forget; requester times out if lost), `expiry_time_us = now_us + 5 000 000 µs`.
5. `m_engine->send(env, now_us)` — BEST_EFFORT: no ACK slot, no retry slot reserved.
6. Return `Result::OK` on success; propagate engine error otherwise.

---

## 5. Call Tree

```
RequestReplyEngine::receive_request()          [RequestReplyEngine.cpp]
 ├── pump_inbound()                             [RequestReplyEngine.cpp]
 │    └── DeliveryEngine::receive()             [DeliveryEngine.cpp]  (×≤MAX_STASH_SIZE)
 │         └── dispatch_inbound_envelope()      [RequestReplyEngine.cpp]
 │              └── handle_inbound_request()    [RequestReplyEngine.cpp]
 └── (copy stash → caller buffer)

RequestReplyEngine::send_response()            [RequestReplyEngine.cpp]
 ├── build_wire_payload()                       [RequestReplyEngine.cpp]
 └── DeliveryEngine::send()                     [DeliveryEngine.cpp]
      ├── MessageIdGen::next()                  [MessageId.cpp]
      └── DeliveryEngine::send_via_transport()  [DeliveryEngine.cpp]
           └── LocalSimHarness::send_message()  [LocalSimHarness.cpp]
```

---

## 6. pump_inbound() and handle_inbound_request()

`pump_inbound()` drains the engine in a bounded loop (`≤ MAX_STASH_SIZE` iterations) by calling `engine.receive()` with `timeout_ms=0` (non-blocking).  Each DATA envelope is passed to `dispatch_inbound_envelope()`:

```
dispatch_inbound_envelope(env):
    parse_rr_header(env.payload, env.payload_length, hdr) → ok/fail
    if not ok  → return false (not an RR message; skip)
    app_start = env.payload + RR_HEADER_SIZE
    app_len   = env.payload_length - RR_HEADER_SIZE
    if hdr.kind == RESPONSE → handle_inbound_response(...)
    else                    → handle_inbound_request(env.source_id, hdr.correlation_id, ...)
```

`handle_inbound_request()`:
1. `find_free_stash()` — scan `m_request_stash[MAX_STASH_SIZE]`; if full, log `WARNING_LO` and drop (graceful — no crash).
2. Copy app payload into free slot.
3. Set `stash.src`, `stash.correlation_id`, `stash.active = true`; increment `m_stash_count`.

---

## 7. Wire Format

Every response payload begins with a 12-byte `RRHeader` (defined in `RequestReplyHeader.hpp`):

```
Byte 0    : kind = 1 (RESPONSE)
Bytes 1-8 : correlation_id (uint64_t; echoed from the matching request's RRHeader)
Bytes 9-11: _pad = 0
```

Application bytes follow immediately after byte 11. The requester's `handle_inbound_response()` strips the header before delivering to `receive_response()`.

---

## 8. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `pump_inbound` — `engine.receive()` returns non-OK | Break out of drain loop | Parse envelope |
| `envelope_is_data(env)` | Parse RRHeader | Skip (continue) |
| `parse_rr_header()` returns false | Return false from `dispatch_inbound_envelope` (non-RR) | Route to request or response handler |
| `hdr.kind == RESPONSE` | Route to `handle_inbound_response` | Route to `handle_inbound_request` |
| `find_free_stash() >= MAX_STASH_SIZE` | Log WARNING_LO; drop request | Stash into free slot |
| Active stash slot found in scan | Copy to caller; free slot; return OK | Continue scan |
| No active slot after full scan | Return ERR_EMPTY | — |
| `build_wire_payload()` returns 0 | Return ERR_FULL | Continue send |
| `engine.send()` returns non-OK | Log warning; propagate | Return OK |

---

## 9. Concurrency / Threading Behavior

- `RequestReplyEngine` is single-threaded; no locks.
- `receive_request()` and `send_response()` must not be called concurrently from multiple threads.

---

## 10. Memory & Ownership Semantics

- **No heap allocation** (Power of 10 Rule 3).
- `m_request_stash[MAX_STASH_SIZE]` — member array; each slot holds a payload of `APP_PAYLOAD_CAP = MSG_MAX_PAYLOAD_BYTES - 12` bytes (≤ 4084 bytes).
- `wire_buf[MSG_MAX_PAYLOAD_BYTES]` — stack buffer in `send_response()`; holds RRHeader + response app payload.
- `m_engine` — non-owning pointer; owned by the caller.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After (receive_request) |
|--------|--------|--------|--------------------------|
| `RequestReplyEngine` | `m_request_stash[i]` | active=true; payload set | active=false |
| `RequestReplyEngine` | `m_stash_count` | N | N−1 |

| Object | Member | Before | After (send_response) |
|--------|--------|--------|------------------------|
| `DeliveryEngine` | `m_id_gen` | N | N+1 |
| Transport | inbound queue of requester | no response | response envelope present |

---

## 12. Error Handling Flow

- **ERR_EMPTY from receive_request():** No request arrived yet; application should retry on next poll tick.
- **Stash overflow (during pump_inbound):** Excess inbound requests are dropped with `WARNING_LO`; the stash holds at most `MAX_STASH_SIZE = 16` requests. Application should drain the stash frequently.
- **ERR_FULL from send_response():** Response payload too large; application must reduce payload size.
- **engine.send() failure in send_response():** Transport error propagated; requester will time out and the pending slot will be freed by `sweep_timeouts()` on the requester side.

---

## 13. Known Risks / Observations

- **Dropped requests on stash overflow:** If the responder does not call `receive_request()` frequently enough and more than `MAX_STASH_SIZE` requests arrive between polls, excess requests are silently dropped.  The requester will retry (RELIABLE_RETRY) and the duplicate filter on the responder's DeliveryEngine will suppress retransmissions of already-stashed messages; new requests still arrive in order.
- **No response dedup on responder:** If `send_response()` is called twice for the same `correlation_id`, both envelopes reach the requester; the first is accepted and the second is discarded by `handle_inbound_response()` (duplicate stash guard).
- **BEST_EFFORT response delivery:** If the response envelope is lost in transit (e.g., due to impairment), the requester will time out.  The application must decide whether to retry the entire request cycle.
