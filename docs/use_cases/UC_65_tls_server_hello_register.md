# UC_65 — TLS server receives HELLO and registers client NodeId

**HL Group:** HL-30 — TCP/TLS Client NodeId Registration on Connect / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.8, REQ-6.1.9

---

## 1. Use Case Overview

- **Trigger:** An inbound HELLO frame arrives on an accepted TLS client session during `TlsTcpBackend::drain_readable_clients()`. File: `src/platform/TlsTcpBackend.cpp`.
- **Goal:** Record the client's `NodeId` in the server's per-slot routing table (`m_client_node_ids[]`) so that subsequent unicast sends can route to the correct TLS session.
- **Success outcome:** `m_client_node_ids[slot] = env.source_id`. The HELLO frame is consumed and never surfaced to `DeliveryEngine` or the User. `ERR_AGAIN` returned from `tls_recv_client_frame()` to signal "frame consumed, no user data".
- **Error outcomes:**
  - `mbedtls_ssl_read()` failure → client removed; `ERR_IO` returned.
  - `deserialize()` failure → frame dropped; `ERR_IO` returned.
  - Slot not found → WARNING_HI logged; `ERR_AGAIN` returned (routing table not updated).

**Post-condition:** `m_client_node_ids[slot]` holds the client's NodeId. Unicast sends with `destination_id == env.source_id` now route to `m_ssl_contexts[slot]`.

---

## 2. Entry Points

```cpp
// src/platform/TlsTcpBackend.cpp (private)
Result TlsTcpBackend::tls_recv_client_frame(uint32_t idx, uint32_t timeout_ms);
```

Called by `drain_readable_clients()` for each slot that has `POLLIN` set. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `TlsTcpBackend::poll_clients_once()` detects `POLLIN` on the underlying TCP fd for slot `idx`.
2. Calls **`tls_recv_client_frame(idx, timeout_ms)`**.
3. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
4. `NEVER_COMPILED_OUT_ASSERT(idx < m_client_count)`.
5. **`mbedtls_ssl_read(&m_ssl_contexts[idx], m_recv_buf, sizeof(m_recv_buf))`** — decrypts one TLS record from the client. Retried on `MBEDTLS_ERR_SSL_WANT_READ`. Returns decrypted byte count or negative error.
6. If `mbedtls_ssl_read()` returns <= 0 (error or EOF): `remove_client(idx)`; return `ERR_IO`.
7. **`Serializer::deserialize(m_recv_buf, bytes_read, &env)`** — decodes the wire frame.
8. If `deserialize()` fails: `LOG_WARN_HI(...)`; return `ERR_IO`.
9. **HELLO interception check:** `if (env.message_type == MessageType::HELLO)`:
   a. Call **`handle_hello_frame(idx, env.source_id)`**:
      - `NEVER_COMPILED_OUT_ASSERT(env.source_id != NODE_ID_INVALID)`.
      - Set `m_client_node_ids[idx] = env.source_id`.
      - `LOG_INFO("TlsTcpBackend", "Registered client NodeId %u at slot %u", env.source_id, idx)`.
   b. Return **`ERR_AGAIN`** — frame consumed; not pushed to ring buffer.
10. Non-HELLO path: impairment engine + ring buffer push (TlsTcpBackend normal receive path).

---

## 4. Call Tree

```
TlsTcpBackend::poll_clients_once()                    [TlsTcpBackend.cpp]
 └── TlsTcpBackend::tls_recv_client_frame(idx, ...)   [TlsTcpBackend.cpp]
      ├── mbedtls_ssl_read(&m_ssl_contexts[idx], ...)  [mbedTLS]
      ├── Serializer::deserialize(buf, len, &env)
      └── [if HELLO:]
           TlsTcpBackend::handle_hello_frame(idx, id) [TlsTcpBackend.cpp]
            └── m_client_node_ids[idx] = id
```

---

## 5. Key Components Involved

- **`tls_recv_client_frame(idx, timeout)`** — Decrypts one TLS record; gates the HELLO interception check before impairment/ring-buffer path.
- **`handle_hello_frame(idx, id)`** — Directly indexes `m_client_node_ids[idx]` using the slot index already known from the poll loop; no fd scan required (unlike TCP variant).
- **`Serializer::deserialize()`** — Decodes the wire frame; validates PROTO_VERSION and PROTO_MAGIC.
- **`m_ssl_contexts[MAX_TCP_CONNECTIONS]`** — Array of `mbedtls_ssl_context`; one per accepted TLS client session.
- **`m_client_node_ids[MAX_TCP_CONNECTIONS]`** — Parallel array; maps slot index to registered NodeId.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `mbedtls_ssl_read()` <= 0 | `remove_client(idx)`; return ERR_IO | Proceed to deserialize |
| `deserialize()` fails | Log WARNING_HI; return ERR_IO | Check message_type |
| `env.message_type == HELLO` | `handle_hello_frame()`; return ERR_AGAIN | Normal receive path |
| `env.source_id == NODE_ID_INVALID` | ASSERT fires | Register NodeId; log INFO |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application thread (same thread as `receive_message()`).
- `m_client_node_ids[]` is written on the HELLO receive path and read on the send path (`find_client_slot()`). Both paths share the application thread; no synchronization required.

---

## 8. Memory & Ownership Semantics

- `m_recv_buf[SOCKET_RECV_BUF_BYTES]` — static member of `TlsTcpBackend`; filled by `mbedtls_ssl_read()`. Not heap allocated.
- `MessageEnvelope env` — stack allocated inside `tls_recv_client_frame()`; discarded on HELLO path.
- `m_client_node_ids[MAX_TCP_CONNECTIONS]` — static array; zeroed (NODE_ID_INVALID) at init.
- `m_ssl_contexts[MAX_TCP_CONNECTIONS]` — initialized during TLS handshake in `accept_and_handshake()`; freed in `remove_client()`.

---

## 9. Error Handling Flow

- **`mbedtls_ssl_read()` failure:** TLS session broken; `remove_client(idx)` frees the SSL context and compacts the client array; `ERR_IO` returned.
- **`deserialize()` failure:** Frame dropped; `ERR_IO` returned. TLS session and slot remain open.
- **`handle_hello_frame()` called with valid idx:** Direct array write; no failure path (idx is already bounds-checked by the ASSERT in step 4).

---

## 10. External Interactions

- **`mbedtls_ssl_read()`:** Decrypts data received from the client's TLS session at slot `idx`. Direction: inbound. May call POSIX `::recv()` internally.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TlsTcpBackend` | `m_client_node_ids[idx]` | NODE_ID_INVALID (0) | `env.source_id` |

No change to `m_recv_queue` — HELLO frames are consumed before the ring buffer push.

---

## 12. Sequence Diagram

```
TlsTcpBackend::poll_clients_once()
  -> tls_recv_client_frame(idx, timeout)
       -> mbedtls_ssl_read(&m_ssl_contexts[idx], m_recv_buf, ...)
            [TLS decrypt; underlying ::recv() inside mbedTLS]
       <- bytes_read
       -> Serializer::deserialize(m_recv_buf, bytes_read, &env)
            <- OK; env.message_type = HELLO; env.source_id = N
       [env.message_type == HELLO]
       -> handle_hello_frame(idx, N)
            -> m_client_node_ids[idx] = N
            -> LOG_INFO("Registered client NodeId N at slot S")
       <- ERR_AGAIN  [frame consumed]
  [drain_readable_clients() discards ERR_AGAIN; loop continues]
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `m_client_node_ids[]` zeroed (NODE_ID_INVALID) at `TlsTcpBackend::init()`.
- `m_ssl_contexts[]` initialized during TLS handshake in `accept_and_handshake()`.

**Runtime:**
- HELLO arrives once per client connection, immediately after the TLS handshake completes.
- After registration, `m_client_node_ids[idx]` is stable for the lifetime of the TLS session.

---

## 14. Known Risks / Observations

- **Slot index vs fd:** Unlike the TCP variant (`handle_hello_frame(fd, id)` which scans by fd), the TLS variant passes the slot index directly because `poll_clients_once()` already knows the slot. This is simpler and avoids the linear scan.
- **`remove_client()` must reset NodeId:** When a client disconnects, `m_client_node_ids[idx]` must be zeroed as part of `remove_client()` slot compaction; otherwise a reused slot could inherit a stale NodeId.
- **HELLO replay:** Same concern as UC_64 — multiple HELLOs overwrite; last HELLO wins.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `tls_recv_client_frame()` is the TLS-backend analogue of `recv_from_client()` in TcpBackend; it performs the same HELLO interception immediately after deserialize.
- `[ASSUMPTION]` `handle_hello_frame(idx, id)` in `TlsTcpBackend` takes a slot index directly, not a file descriptor, because the TLS poll loop already knows the slot.
