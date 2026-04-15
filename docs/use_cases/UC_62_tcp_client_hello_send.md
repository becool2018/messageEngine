# UC_62 — TCP client sends HELLO frame on connect

**HL Group:** HL-30 — TCP/TLS Client NodeId Registration on Connect / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.8, REQ-6.1.10

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::init()` calls `transport->register_local_id(local_id)` immediately after `TcpBackend::init()` returns `Result::OK`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Transmit a HELLO frame carrying the client's local `NodeId` to the server so the server can populate its per-client routing table.
- **Success outcome:** HELLO frame written to the wire. Server will call `handle_hello_frame()` on receipt and register the client's NodeId in its `m_client_node_ids[]` table.
- **Error outcomes:**
  - `Serializer::serialize()` failure → WARNING_HI logged; `ERR_IO` returned from `register_local_id()`.
  - `send_frame()` failure → WARNING_HI logged; `ERR_IO` returned from `register_local_id()`.

**Note — server mode:** When `TcpBackend` is in server mode (`m_is_server == true`), `register_local_id()` only stores `m_local_node_id = id`; no HELLO frame is sent on the wire. The server's NodeId is used for logging only.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (implements TransportInterface)
Result TcpBackend::register_local_id(NodeId id);
```

Called by `DeliveryEngine::init()` after a successful `transport->init()`. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::init()` calls `m_transport->register_local_id(local_id)`.
2. Dispatches via virtual call to **`TcpBackend::register_local_id(id)`**.
3. `NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID)`.
4. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
5. Store `m_local_node_id = id`.
6. If `m_is_server == true`: return `Result::OK` immediately (no HELLO sent by server).
7. Client path — **`send_hello_frame()`** is called:
   a. Build `MessageEnvelope hello`: `message_type = MessageType::HELLO`, `source_id = id`, `destination_id = 0`, `payload_length = 0`, `message_id = 0`, `timestamp_us = 0`, `reliability_class = BEST_EFFORT`.
   b. **`Serializer::serialize(hello, m_wire_buf, sizeof(m_wire_buf), &hello_len)`** — encodes the envelope into `m_wire_buf`. Includes PROTO_VERSION and PROTO_MAGIC per REQ-3.2.8.
   c. If `serialize()` fails: `LOG_WARN_HI("TcpBackend", "HELLO serialize failed")`. Return `ERR_IO`.
   d. **`m_sock_ops->send_frame(m_client_fds[0], m_wire_buf, hello_len, m_cfg.send_timeout_ms)`** — writes the length-prefixed HELLO frame to the server socket.
   e. If `send_frame()` returns false: `LOG_WARN_HI("TcpBackend", "HELLO send failed")`. Return `ERR_IO`.
   f. `LOG_INFO("TcpBackend", "Sent HELLO NodeId %u to server")`. Return `Result::OK`.
8. `register_local_id()` returns `Result::OK` on success, `ERR_IO` on error.

---

## 4. Call Tree

```
DeliveryEngine::init()                              [DeliveryEngine.cpp]
 └── TransportInterface::register_local_id(id)      [virtual dispatch]
      └── TcpBackend::register_local_id(id)         [TcpBackend.cpp]
           └── TcpBackend::send_hello_frame()        [TcpBackend.cpp]
                ├── Serializer::serialize(hello, ...)
                └── ISocketOps::send_frame(fd, buf, len, timeout)
                     └── tcp_send_frame()            [SocketUtils.cpp]
                          ├── socket_send_all(fd, hdr, 4)
                          └── socket_send_all(fd, buf, len)
```

---

## 5. Key Components Involved

- **`TcpBackend::register_local_id()`** — Stores local NodeId; dispatches to `send_hello_frame()` in client mode.
- **`TcpBackend::send_hello_frame()`** — Constructs the HELLO envelope and serializes + sends it.
- **`Serializer::serialize()`** — Encodes the HELLO envelope into the wire buffer with protocol version and magic bytes.
- **`ISocketOps::send_frame()`** — Writes the length-prefixed frame to the connected socket; abstracts POSIX `::send()`.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_is_server == true` | Return OK; no HELLO sent | Proceed to `send_hello_frame()` |
| `Serializer::serialize()` fails | Log WARNING_HI; return ERR_IO | Call `send_frame()` |
| `send_frame()` returns false | Log WARNING_HI; return ERR_IO | Log INFO; return OK |

---

## 7. Concurrency / Threading Behavior

- Called once during `DeliveryEngine::init()`; single-threaded at init time.
- No concurrent access to `m_wire_buf` or `m_client_fds[0]` is possible during init.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` — static member of `TcpBackend`; scratch buffer reused for every send. Not heap allocated.
- `MessageEnvelope hello` — stack allocated inside `send_hello_frame()`; zero payload.
- No heap allocation.

---

## 9. Error Handling Flow

- **Serialize failure:** Returns `ERR_IO` immediately. The transport is open but the server has not registered this client. Unicast sends from the server to this client will fail with `ERR_INVALID` until a successful HELLO is received.
- **`send_frame()` failure:** Same outcome. Socket may be broken; caller should treat `ERR_IO` from `register_local_id()` as an initialization failure.

---

## 10. External Interactions

- **POSIX `::send()`:** Called via `tcp_send_frame()` → `socket_send_all()` on `m_client_fds[0]`. Direction: outbound to server.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_local_node_id` | NODE_ID_INVALID (0) | `id` |
| TCP socket | kernel send buffer | previous | + HELLO frame |

---

## 12. Sequence Diagram

```
DeliveryEngine::init()
  -> TcpBackend::register_local_id(id)
       [m_is_server == false]
       -> send_hello_frame()
            -> Serializer::serialize(hello, m_wire_buf, ...)
            <- serialize_len
            -> ISocketOps::send_frame(m_client_fds[0], m_wire_buf, len, timeout)
                 -> tcp_send_frame()
                      -> socket_send_all(fd, hdr, 4)   [4-byte length prefix]
                      -> socket_send_all(fd, buf, len) [HELLO payload]
                 <- true
            -> LOG_INFO("Sent HELLO NodeId N to server")
       <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Initialization only:**
- `register_local_id()` is called exactly once per `DeliveryEngine::init()` cycle.
- HELLO frames are only ever sent during init; they do not appear on the steady-state send path.

**Runtime:**
- After init, `m_local_node_id` is set and remains constant. No further HELLO frames are sent unless `close()` + `init()` are called again.

---

## 14. Known Risks / Observations

- **HELLO lost in transit:** TCP guarantees delivery, so a HELLO lost means the connection was broken. The server will remove the client slot on the next receive error.
- **Server mode guard:** If `register_local_id()` is called in server mode, the function returns `OK` immediately. This is correct: servers do not announce themselves via HELLO; they only receive HELLOs from clients.
- **`m_wire_buf` reuse:** The HELLO serialize step overwrites `m_wire_buf`. Since `register_local_id()` is called during init (before any concurrent send), this is safe.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `NODE_ID_INVALID == 0` — the broadcast sentinel used for `destination_id` in the HELLO envelope; inferred from `Types.hpp` convention.
- `[ASSUMPTION]` `send_hello_frame()` is a private helper on `TcpBackend`; it is not part of `TransportInterface`.
