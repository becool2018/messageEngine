# UC_64 — TCP server receives HELLO and registers client NodeId

**HL Group:** HL-30 — TCP/TLS Client NodeId Registration on Connect / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.8, REQ-6.1.9

---

## 1. Use Case Overview

- **Trigger:** An inbound HELLO frame arrives on an accepted client socket during `TcpBackend::drain_readable_clients()`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Record the client's `NodeId` in the server's per-slot routing table (`m_client_node_ids[]`) so that subsequent unicast sends from the server can route to the correct socket.
- **Success outcome:** `m_client_node_ids[slot] = env.source_id`. The HELLO frame is consumed and never surfaced to `DeliveryEngine` or the User. `ERR_AGAIN` returned from `recv_from_client()` to signal "frame consumed, no user data".
- **Error outcomes:**
  - Slot not found for `client_fd` → WARNING_HI logged; `ERR_AGAIN` returned (HELLO still consumed; routing table not updated).

**Post-condition:** `m_client_node_ids[slot]` holds the client's NodeId. Unicast sends with `destination_id == env.source_id` now route to `m_client_fds[slot]`.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (private)
Result TcpBackend::recv_from_client(int client_fd, uint32_t timeout_ms);
```

Called by `drain_readable_clients()` for each fd that has `POLLIN` set. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `TcpBackend::poll_clients_once()` detects `POLLIN` on `client_fd`.
2. Calls **`recv_from_client(client_fd, timeout_ms)`** (UC_06 path).
3. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
4. **`m_sock_ops->recv_frame(client_fd, m_recv_buf, sizeof(m_recv_buf), timeout_ms)`** — reads the 4-byte length prefix + frame payload. Returns byte count or error.
5. If `recv_frame()` fails (EOF or error): `remove_client(client_fd)`; return `ERR_IO`.
6. **`Serializer::deserialize(m_recv_buf, bytes_read, &env)`** — decodes the wire frame into `MessageEnvelope env`. Validates PROTO_VERSION and PROTO_MAGIC.
7. If `deserialize()` fails: `Logger::log(WARNING_HI, ...)`; return `ERR_IO`.
8. **HELLO interception check:** `if (env.message_type == MessageType::HELLO)`:
   a. Call **`handle_hello_frame(client_fd, env.source_id)`**:
      - `NEVER_COMPILED_OUT_ASSERT(env.source_id != NODE_ID_INVALID)`.
      - Loop over `idx` in `0..m_client_count-1` (bounded by `MAX_TCP_CONNECTIONS`):
        - If `m_client_fds[idx] == client_fd`: found slot.
        - Set `m_client_node_ids[idx] = env.source_id`.
        - `Logger::log(INFO, "TcpBackend", "Registered client NodeId %u at slot %u", env.source_id, idx)`.
        - Return.
      - If no slot found: `Logger::log(WARNING_HI, "TcpBackend", "HELLO: no slot for fd %d")`.
   b. Return **`ERR_AGAIN`** — frame consumed; caller discards this return and continues the poll loop.
9. Non-HELLO path: impairment engine + ring buffer push (UC_06 / UC_21 normal path).

---

## 4. Call Tree

```
TcpBackend::poll_clients_once()                    [TcpBackend.cpp]
 └── TcpBackend::recv_from_client(fd, timeout)     [TcpBackend.cpp]
      ├── ISocketOps::recv_frame(fd, buf, ...)      [tcp_recv_frame()]
      ├── Serializer::deserialize(buf, len, &env)
      └── [if HELLO:]
           TcpBackend::handle_hello_frame(fd, id)  [TcpBackend.cpp]
            └── [scan m_client_fds[]; set m_client_node_ids[slot] = id]
```

---

## 5. Key Components Involved

- **`recv_from_client()`** — Reads and deserializes one frame; gates the HELLO interception check before impairment/ring-buffer path.
- **`handle_hello_frame(fd, id)`** — Scans `m_client_fds[]` by fd value to find the slot index; writes `id` into `m_client_node_ids[slot]`.
- **`Serializer::deserialize()`** — Decodes the wire frame; rejects mismatched version or magic bytes before `message_type` is inspected.
- **`m_client_node_ids[]`** — Fixed-size array (`MAX_TCP_CONNECTIONS`) parallel to `m_client_fds[]`; maps slot index to registered NodeId.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `recv_frame()` fails | `remove_client()`; return ERR_IO | Proceed to deserialize |
| `deserialize()` fails | Log WARNING_HI; return ERR_IO | Check message_type |
| `env.message_type == HELLO` | `handle_hello_frame()`; return ERR_AGAIN | Normal UC_06 path |
| Slot found in `handle_hello_frame()` | Register NodeId; log INFO | Log WARNING_HI (no update) |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application thread (same thread as `receive_message()`).
- `m_client_node_ids[]` is written only on the HELLO receive path and read only on the send path (`find_client_slot()`). Both paths are on the same thread, so no synchronization primitive is required.

---

## 8. Memory & Ownership Semantics

- `m_recv_buf[SOCKET_RECV_BUF_BYTES]` — static member of `TcpBackend`; filled by `recv_frame()`. Not heap allocated.
- `MessageEnvelope env` — stack allocated inside `recv_from_client()`; discarded on HELLO path.
- `m_client_node_ids[MAX_TCP_CONNECTIONS]` — static array owned by `TcpBackend`; zeroed (NODE_ID_INVALID) at init.

---

## 9. Error Handling Flow

- **`recv_frame()` failure:** Client removed; `ERR_IO` returned; poll loop continues with remaining clients.
- **`deserialize()` failure:** Frame dropped; `ERR_IO` returned; client is not removed (framing remains valid if version mismatch is the only issue).
- **No slot found in `handle_hello_frame()`:** WARNING_HI logged; `ERR_AGAIN` returned; client remains connected. Routing table entry is absent; unicast to this client's NodeId will return `ERR_INVALID` until a subsequent HELLO succeeds.

---

## 10. External Interactions

- **POSIX `::recv()`:** Called via `recv_frame()` → `tcp_recv_frame()` on `client_fd`. Direction: inbound from client.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_client_node_ids[slot]` | NODE_ID_INVALID (0) | `env.source_id` |

No change to `m_recv_queue` — HELLO frames are consumed before the ring buffer push.

---

## 12. Sequence Diagram

```
TcpBackend::poll_clients_once()
  -> recv_from_client(client_fd, timeout)
       -> ISocketOps::recv_frame(client_fd, m_recv_buf, ...)
            -> tcp_recv_frame()
                 -> socket_recv_all(fd, hdr, 4)   [4-byte length]
                 -> socket_recv_all(fd, buf, len)  [payload]
            <- bytes_read
       -> Serializer::deserialize(m_recv_buf, bytes_read, &env)
            <- OK; env.message_type = HELLO
       [env.message_type == HELLO]
       -> handle_hello_frame(client_fd, env.source_id)
            [scan m_client_fds[]; idx found]
            -> m_client_node_ids[idx] = env.source_id
            -> Logger::log(INFO, "Registered client NodeId N at slot S")
       <- ERR_AGAIN  [frame consumed]
  [drain_readable_clients() discards ERR_AGAIN; loop continues]
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `m_client_node_ids[]` is zeroed (NODE_ID_INVALID) when `TcpBackend::init()` is called.

**Runtime:**
- The HELLO frame arrives once per client connection, immediately after the TCP handshake.
- `handle_hello_frame()` runs as part of the first `recv_from_client()` call for a new client.
- After registration, `m_client_node_ids[slot]` is stable for the lifetime of the connection.

---

## 14. Known Risks / Observations

- **HELLO replay:** A malicious or buggy client could send multiple HELLO frames. Each would overwrite `m_client_node_ids[slot]` with possibly a different NodeId. The server logs each event at INFO; the last HELLO wins.
- **HELLO never arrives:** If a client connects but never sends a HELLO (e.g., a legacy client), `m_client_node_ids[slot]` remains NODE_ID_INVALID. The server can still broadcast to all clients; only unicast to that slot's NodeId is affected.
- **`remove_client()` resets NodeId:** When `remove_client(idx)` compacts the array, `m_client_node_ids[idx]` must be reset to NODE_ID_INVALID and the moved entry's NodeId must be carried over correctly.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `handle_hello_frame()` searches `m_client_fds[]` by fd value (linear scan, O(MAX_TCP_CONNECTIONS)). This is the natural approach given the small table size (MAX_TCP_CONNECTIONS = 8).
- `[ASSUMPTION]` `ERR_AGAIN` is the return code for "frame consumed but no user envelope produced"; this is discarded by `drain_readable_clients()` without logging.
