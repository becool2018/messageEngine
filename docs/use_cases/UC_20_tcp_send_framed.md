# UC_20 — TCP send framed message — send_frame() / send_to_all_clients() called internally by TcpBackend::send_message()

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System
**Requirement traceability:** REQ-6.1.5, REQ-6.1.6, REQ-6.3.2

---

## 1. Use Case Overview

- **Trigger:** `TcpBackend::send_to_all_clients(buf, len)` is called by `TcpBackend::send_message()` after serialization and impairment processing. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Send a length-prefixed framed message to every currently-connected TCP client, handling partial writes.
- **Success outcome:** All bytes of the frame are written to all client sockets. Returns `bool` — `true` if all sends succeeded, `false` if at least one client send failed.
- **Error outcomes:** Socket write failure for a client is logged and the client is removed from the table. Other clients still receive the message.

**Invoking UC:** UC_01, UC_02, UC_03, UC_10 (any send path) invoke this function.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (private)
bool TcpBackend::send_to_all_clients(const uint8_t* buf, uint32_t len);
```

Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `TcpBackend::send_message()` calls `send_to_all_clients(m_wire_buf, wire_len)` for each deliverable envelope from `collect_deliverable()`.
2. `NEVER_COMPILED_OUT_ASSERT(buf != nullptr)` and `NEVER_COMPILED_OUT_ASSERT(len > 0)`.
3. **Loop** over `idx` in `0..m_client_count-1` (Power of 10 Rule 2: bounded by `MAX_TCP_CONNECTIONS`):
   a. **`m_sock_ops->send_frame(m_client_fds[idx], buf, len, m_cfg.send_timeout_ms)`** is called. Dispatches to `tcp_send_frame()` (`SocketUtils.cpp`):
      - Prepend 4-byte big-endian length: `hdr = htonl(len)`.
      - `socket_send_all(fd, (uint8_t*)&hdr, 4, timeout_ms)` — `::send()` retry loop until all 4 bytes sent or error.
      - `socket_send_all(fd, buf, len, timeout_ms)` — `::send()` retry loop until all `len` bytes sent or error.
      - Returns `true` on full success; `false` on partial or error.
   b. If `send_frame` returns `false`: `Logger::log(WARNING_LO, "TcpBackend", "send failed to client %u")`, `remove_client(idx)`, `idx--` (re-check same index after compaction), `m_client_count--`.
4. Loop completes.

---

## 4. Call Tree

```
TcpBackend::send_to_all_clients(buf, len)      [TcpBackend.cpp]
 └── [for each client idx:]
      ISocketOps::send_frame(fd, buf, len, timeout) [SocketOpsImpl]
       └── tcp_send_frame()                    [SocketUtils.cpp]
            ├── socket_send_all(fd, hdr, 4)    [::send() retry loop]
            └── socket_send_all(fd, buf, len)  [::send() retry loop]
```

---

## 5. Key Components Involved

- **`send_to_all_clients()`** — Fan-out loop; broadcasts one frame to all connected clients.
- **`tcp_send_frame()`** — 4-byte length prefix + payload write with retry loop on `EINTR`/short writes.
- **`socket_send_all()`** — `::send()` with retry loop; ensures full delivery or error return.
- **`remove_client(idx)`** — On failure, compacts the client array (last slot moved to `idx`, `m_client_count` decremented).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_client_count == 0` | Loop body never executes | Normal loop |
| `send_frame()` returns false | `remove_client(idx)`; `idx--` (re-check) | Continue to next client |
| `socket_send_all()` partial write | Retry with remaining bytes | Full write complete |

---

## 6a. Unicast Routing (REQ-6.1.9) — added with HELLO registration fix

`TcpBackend::send_message()` routes outbound frames based on `destination_id`:

| destination_id | Routing action |
|---|---|
| 0 (NODE_ID_INVALID) | Broadcast: `flush_delayed_to_clients()` calls `send_to_all_clients()` for all connected slots |
| > 0 | Unicast: `flush_delayed_to_clients()` calls `find_client_slot(destination_id)` then `send_to_slot(slot, buf, len)` for the single matched slot |
| > 0, no slot found | `ERR_INVALID` returned; WARNING_HI logged; no bytes sent |

The routing decision is made inside `flush_delayed_to_clients()` per-message, so
delayed/reordered messages are routed correctly even if they were queued before
the slot registration was observed.

---

## 7. Concurrency / Threading Behavior

- Synchronous in the backend's calling thread.
- `socket_send_all()` uses `::send()` with `MSG_NOSIGNAL` (or `SIGPIPE` suppressed); no signal interference.
- Not thread-safe.

---

## 8. Memory & Ownership Semantics

- `buf` — caller-provided pointer to `m_wire_buf[SOCKET_RECV_BUF_BYTES]` (8192 bytes); not owned by `send_to_all_clients()`.
- 4-byte header buffer in `tcp_send_frame()` — stack allocated.
- No heap allocation.

---

## 9. Error Handling Flow

- **`send_frame()` failure (socket closed, EPIPE, timeout):** Client removed; loop continues to remaining clients. The send is best-effort across clients.
- **`send_timeout_ms` exceeded:** `socket_send_all()` gives up after `timeout_ms`; returns false. Same client removal applies.

---

## 10. External Interactions

- **POSIX `::send()`:** Called on each client fd in `m_client_fds[idx]`. Direction: outbound. May be called multiple times per frame (retry loop on short writes).

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| TCP socket | kernel send buffer | previous | + length-prefixed frame |
| `TcpBackend` | `m_client_fds[idx]` (on failure) | valid fd | closed; slot compacted |
| `TcpBackend` | `m_client_count` (on failure) | N | N-1 |

---

## 12. Sequence Diagram

```
TcpBackend::send_message()
  -> send_to_all_clients(wire_buf, wire_len)
       [for each client idx in 0..m_client_count-1:]
            -> ISocketOps::send_frame(fd, buf, len, timeout)
                 -> tcp_send_frame()
                      -> socket_send_all(fd, hdr, 4)   [::send(); 4-byte length]
                      -> socket_send_all(fd, buf, len) [::send(); payload]
                 <- true (success)
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TcpBackend::init()` called; at least one client in `m_client_fds[]`.
- `Serializer::serialize()` has populated `m_wire_buf` with the wire frame.

**Runtime:**
- Called on every send path. The loop runs 0..8 times depending on `m_client_count`.

---

## 14. Known Risks / Observations

- **Broadcast semantics:** All connected clients receive the same message. There is no per-client routing based on `destination_id`.
- **Remove during loop:** `remove_client(idx)` compacts the array; `idx--` is used to re-check the same slot after compaction. If this decrement is missed, one client might be skipped.
- **`send_timeout_ms`:** A blocking send on a slow client can hold up all other clients for `send_timeout_ms` milliseconds per client.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `idx--` after `remove_client(idx)` is used to re-check the compacted slot. This pattern is inferred from the TcpBackend send loop implementation to avoid skipping the moved client.
