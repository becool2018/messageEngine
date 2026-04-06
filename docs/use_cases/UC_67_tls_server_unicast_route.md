# UC_67 — TLS server routes unicast frame to matched TLS session

**HL Group:** HL-31 — TCP/TLS Server Unicast Message Routing / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.9

---

## 1. Use Case Overview

- **Trigger:** `TlsTcpBackend::send_message(env)` is called on a server endpoint where `env.destination_id > 0`. File: `src/platform/TlsTcpBackend.cpp`.
- **Goal:** Deliver the serialized frame through exactly one TLS session whose `m_client_node_ids[slot]` equals `env.destination_id`, rather than broadcasting to all TLS sessions.
- **Success outcome:** Frame encrypted and written through the single matching TLS session only. Other TLS sessions do not receive the frame. Returns `Result::OK`.
- **Error outcomes:**
  - `destination_id > 0` but no slot registered with that NodeId → `ERR_INVALID` returned; WARNING_HI logged (see UC_68).
  - `tls_send_frame()` fails for the matched slot → WARNING_HI logged; TLS session and client slot removed; returns `ERR_IO`.

**Broadcast fallback:** When `env.destination_id == 0`, `send_to_all_clients()` fans out to all connected TLS sessions unchanged.

---

## 2. Entry Points

```cpp
// src/platform/TlsTcpBackend.cpp (implements TransportInterface)
Result TlsTcpBackend::send_message(const MessageEnvelope& env);
```

Called by `DeliveryEngine::send()`. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. **`TlsTcpBackend::send_message(env)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. `NEVER_COMPILED_OUT_ASSERT(m_is_server)`.
4. **`Serializer::serialize(env, m_wire_buf, sizeof(m_wire_buf), &wire_len)`** — encode envelope.
5. If serialize fails: log WARNING_HI; return `ERR_IO`.
6. **`ImpairmentEngine::process_outbound(env, m_wire_buf, wire_len)`** — apply impairments.
7. **`ImpairmentEngine::collect_deliverable(callback)`** — retrieve deliverable frames.
8. For each deliverable frame — **`flush_delayed_to_clients(frame_buf, frame_len, frame_destination_id)`**:
   a. `NEVER_COMPILED_OUT_ASSERT(frame_len > 0)`.
   b. **Routing decision on `frame_destination_id`:**
      - **`frame_destination_id == 0`** (broadcast): call `send_to_all_clients(frame_buf, frame_len)` — calls `tls_send_frame(idx, ...)` for each slot.
      - **`frame_destination_id > 0`** (unicast): call **`find_client_slot(frame_destination_id)`**:
        - Loop `idx` in `0..m_client_count-1` (bounded by `MAX_TCP_CONNECTIONS`).
        - If `m_client_node_ids[idx] == frame_destination_id`: return `idx`.
        - If not found: return `MAX_TCP_CONNECTIONS`.
      - If `find_client_slot()` returns `MAX_TCP_CONNECTIONS`: log `WARNING_HI "unicast to unregistered NodeId N; drop"`; set `final_result = ERR_INVALID`; continue.
      - If slot found: call **`tls_send_frame(slot, frame_buf, frame_len, m_cfg.send_timeout_ms)`**:
        - `mbedtls_ssl_write(&m_ssl_contexts[slot], frame_buf, frame_len)` — encrypt and transmit through TLS record layer. Retry loop on `MBEDTLS_ERR_SSL_WANT_WRITE`.
        - Returns true on full write; false on TLS error or timeout.
      - If `tls_send_frame()` returns false: log `WARNING_HI`; `remove_client(slot)` (frees SSL context + compacts array); set `final_result = ERR_IO`.
9. Return `final_result`.

---

## 4. Call Tree

```
TlsTcpBackend::send_message(env)                      [TlsTcpBackend.cpp]
 ├── Serializer::serialize(env, m_wire_buf, ...)
 ├── ImpairmentEngine::process_outbound(...)
 ├── ImpairmentEngine::collect_deliverable(cb)
 └── [for each deliverable frame:]
      TlsTcpBackend::flush_delayed_to_clients(buf, len, dst_id)
       ├── [dst_id == 0:] send_to_all_clients(buf, len)
       │    └── [for each slot:] tls_send_frame(idx, buf, len, timeout)
       │         └── mbedtls_ssl_write()
       └── [dst_id > 0:]
            TlsTcpBackend::find_client_slot(dst_id)
             └── [scan m_client_node_ids[]]
            TlsTcpBackend::tls_send_frame(slot, buf, len, timeout)
             └── mbedtls_ssl_write(&m_ssl_contexts[slot], ...)
                  └── ::send() / kernel TLS  [POSIX via mbedTLS]
```

---

## 5. Key Components Involved

- **`flush_delayed_to_clients()`** — Per-frame routing dispatcher: inspects `destination_id`; calls broadcast or unicast path.
- **`find_client_slot(destination_id)`** — Linear scan of `m_client_node_ids[]`; returns slot index or `MAX_TCP_CONNECTIONS`.
- **`tls_send_frame(slot, buf, len, timeout)`** — Single-session TLS send; calls `mbedtls_ssl_write()` for exactly one TLS context at `m_ssl_contexts[slot]`.
- **`send_to_all_clients()`** — Broadcast path; calls `tls_send_frame()` for each slot in turn.
- **`ImpairmentEngine`** — Routing decision is made at delivery time, after impairment delays have elapsed.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `serialize()` fails | Log WARNING_HI; return ERR_IO | Continue to impairment |
| `frame_destination_id == 0` | `send_to_all_clients()` (broadcast) | `find_client_slot()` (unicast) |
| `find_client_slot()` returns MAX_TCP_CONNECTIONS | Log WARNING_HI; ERR_INVALID | `tls_send_frame()` |
| `tls_send_frame()` returns false | Log WARNING_HI; `remove_client()`; ERR_IO | OK |
| `mbedtls_ssl_write()` returns WANT_WRITE | Retry loop | Write complete |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application thread.
- `m_ssl_contexts[]` is read here (send) and written during handshake in `accept_and_handshake()`. Both are on the application thread; no races during steady-state operation.
- `m_client_node_ids[]` written on HELLO receive path; read here on send path; same thread.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` — static member; not heap allocated.
- `m_ssl_contexts[MAX_TCP_CONNECTIONS]` — array of `mbedtls_ssl_context`; initialized during handshake; freed in `remove_client()`.
- No heap allocation on critical path.

---

## 9. Error Handling Flow

- **`serialize()` failure:** Return `ERR_IO` immediately.
- **`find_client_slot()` miss:** `ERR_INVALID`; frame dropped. Other deliverable frames still processed.
- **`tls_send_frame()` failure:** `remove_client(slot)` frees `m_ssl_contexts[slot]` via `mbedtls_ssl_free()` and compacts `m_client_fds[]` and `m_client_node_ids[]`. `ERR_IO` accumulated.

---

## 10. External Interactions

- **`mbedtls_ssl_write()`:** Called for exactly one TLS context (`m_ssl_contexts[slot]`). Direction: outbound to one client through TLS record layer.
- **POSIX `::send()`:** Called internally by mbedTLS.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| TLS session (slot) | record layer output | previous | + unicast encrypted frame |
| `TlsTcpBackend` | `m_ssl_contexts[slot]` (on send fail) | active TLS context | freed; slot compacted |
| `TlsTcpBackend` | `m_client_count` (on send fail) | N | N-1 |

---

## 12. Sequence Diagram

```
DeliveryEngine::send(env)         [env.destination_id = N > 0]
  -> TlsTcpBackend::send_message(env)
       -> Serializer::serialize(env, m_wire_buf, ...)
       -> ImpairmentEngine::process_outbound(...)
       -> ImpairmentEngine::collect_deliverable(cb)
            -> flush_delayed_to_clients(buf, len, dst_id=N)
                 -> find_client_slot(N)
                      [scan m_client_node_ids[]; slot S found]
                 <- S
                 -> tls_send_frame(S, buf, len, timeout)
                      -> mbedtls_ssl_write(&m_ssl_contexts[S], buf, len)
                           [TLS encrypt; ::send() to client socket]
                      <- bytes_written == len
                 <- true
                 [only TLS session at slot S receives the frame]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TlsTcpBackend::init()` called with `m_is_server == true`.
- At least one TLS client connected, handshake complete, and HELLO processed (UC_65).

**Runtime:**
- Routing decision made per deliverable frame inside `flush_delayed_to_clients()`.
- `m_ssl_contexts[slot]` must remain valid (client connected) at the time of the send.

---

## 14. Known Risks / Observations

- **`remove_client()` and SSL context:** When `tls_send_frame()` fails, `remove_client(slot)` must call `mbedtls_ssl_free(&m_ssl_contexts[slot])` before compacting the array. If the compacted slot carries an uninitialized context, subsequent sends to the moved slot may corrupt mbedTLS state.
- **`tls_enabled == false`:** In plaintext mode, `tls_send_frame(slot, ...)` falls through to `tcp_send_frame(m_client_fds[slot], ...)`. The unicast routing logic in `flush_delayed_to_clients()` is identical.
- **Routing at delivery time (same note as UC_66):** Delayed frames are routed at `collect_deliverable()` time. A client HELLO registered after the frame was enqueued in the impairment buffer will still be found correctly.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `tls_send_frame(slot, buf, len, timeout)` takes a slot index (not a file descriptor), consistent with the TLS backend's use of slot indices throughout.
- `[ASSUMPTION]` `remove_client(slot)` in `TlsTcpBackend` calls `mbedtls_ssl_free()` before compacting arrays, as required by mbedTLS cleanup semantics.
