# UC_63 — TLS client sends HELLO frame on connect (via TLS session)

**HL Group:** HL-30 — TCP/TLS Client NodeId Registration on Connect / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.8, REQ-6.1.10

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::init()` calls `transport->register_local_id(local_id)` immediately after `TlsTcpBackend::init()` returns `Result::OK` and the TLS handshake has completed. File: `src/platform/TlsTcpBackend.cpp`.
- **Goal:** Transmit a HELLO frame carrying the client's local `NodeId` through the established TLS session to the server, enabling the server to populate its per-client routing table.
- **Success outcome:** HELLO frame written through the TLS record layer. Server will call `handle_hello_frame()` on receipt and register the client's NodeId.
- **Error outcomes:**
  - TLS session not yet established → NEVER_COMPILED_OUT_ASSERT fires (precondition violation).
  - `Serializer::serialize()` failure → WARNING_HI logged; `ERR_IO` returned.
  - `tls_send_frame()` failure → WARNING_HI logged; `ERR_IO` returned.

**Note — server mode:** When `TlsTcpBackend` is in server mode (`m_is_server == true`), `register_local_id()` only stores `m_local_node_id = id`; no HELLO frame is sent on the wire.

---

## 2. Entry Points

```cpp
// src/platform/TlsTcpBackend.cpp (implements TransportInterface)
Result TlsTcpBackend::register_local_id(NodeId id);
```

Called by `DeliveryEngine::init()` after a successful `transport->init()`. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::init()` calls `m_transport->register_local_id(local_id)`.
2. Dispatches via virtual call to **`TlsTcpBackend::register_local_id(id)`**.
3. `NEVER_COMPILED_OUT_ASSERT(id != NODE_ID_INVALID)`.
4. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
5. Store `m_local_node_id = id`.
6. If `m_is_server == true`: return `Result::OK` immediately (server does not send HELLO).
7. Client path — **`send_hello_frame()`** is called:
   a. Build `MessageEnvelope hello`: `message_type = MessageType::HELLO`, `source_id = id`, `destination_id = 0`, `payload_length = 0`, `message_id = 0`, `timestamp_us = 0`, `reliability_class = BEST_EFFORT`.
   b. **`Serializer::serialize(hello, m_wire_buf, sizeof(m_wire_buf), &hello_len)`** — encodes the envelope into `m_wire_buf`. Includes PROTO_VERSION and PROTO_MAGIC per REQ-3.2.8.
   c. If `serialize()` fails: `Logger::log(WARNING_HI, "TlsTcpBackend", "HELLO serialize failed")`. Return `ERR_IO`.
   d. **`tls_send_frame(0, m_wire_buf, hello_len, m_cfg.send_timeout_ms)`** — sends the frame through the TLS session at slot 0 (the server connection).
      - `mbedtls_ssl_write()` is called in a retry loop until all bytes are written or timeout.
      - Returns true on full success; false on TLS error or timeout.
   e. If `tls_send_frame()` returns false: `Logger::log(WARNING_HI, "TlsTcpBackend", "HELLO TLS send failed")`. Return `ERR_IO`.
   f. `Logger::log(INFO, "TlsTcpBackend", "Sent HELLO NodeId %u to server via TLS")`. Return `Result::OK`.
8. `register_local_id()` returns `Result::OK` on success, `ERR_IO` on error.

---

## 4. Call Tree

```
DeliveryEngine::init()                               [DeliveryEngine.cpp]
 └── TransportInterface::register_local_id(id)       [virtual dispatch]
      └── TlsTcpBackend::register_local_id(id)       [TlsTcpBackend.cpp]
           └── TlsTcpBackend::send_hello_frame()     [TlsTcpBackend.cpp]
                ├── Serializer::serialize(hello, ...)
                └── TlsTcpBackend::tls_send_frame(0, buf, len, timeout)
                     └── mbedtls_ssl_write()          [mbedTLS record layer]
                          └── ::send() / kernel TLS   [POSIX]
```

---

## 5. Key Components Involved

- **`TlsTcpBackend::register_local_id()`** — Stores local NodeId; dispatches to `send_hello_frame()` in client mode.
- **`TlsTcpBackend::send_hello_frame()`** — Constructs the HELLO envelope and serializes + sends it through TLS.
- **`Serializer::serialize()`** — Encodes the HELLO envelope into the wire buffer with protocol version and magic bytes.
- **`tls_send_frame(slot, buf, len, timeout)`** — Writes the frame via `mbedtls_ssl_write()` with retry logic; slot 0 is the server connection for a TLS client.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_is_server == true` | Return OK; no HELLO sent | Proceed to `send_hello_frame()` |
| `Serializer::serialize()` fails | Log WARNING_HI; return ERR_IO | Call `tls_send_frame()` |
| `tls_send_frame()` returns false | Log WARNING_HI; return ERR_IO | Log INFO; return OK |
| TLS session not established | ASSERT fires (precondition) | Normal send |

---

## 7. Concurrency / Threading Behavior

- Called once during `DeliveryEngine::init()`; single-threaded at init time.
- TLS handshake is completed inside `TlsTcpBackend::init()` before `register_local_id()` is called; there is no race on the TLS session.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` — static member of `TlsTcpBackend`; scratch buffer. Not heap allocated.
- `MessageEnvelope hello` — stack allocated inside `send_hello_frame()`; zero payload.
- No heap allocation.

---

## 9. Error Handling Flow

- **Serialize failure:** Returns `ERR_IO` immediately. Server routing table for this client is unpopulated.
- **`tls_send_frame()` failure:** Returns `ERR_IO`. The TLS session may be broken; caller should treat this as an initialization failure.
- In both cases the transport remains open (`m_open == true`) but the server cannot route unicast messages to this client until a successful HELLO is registered.

---

## 10. External Interactions

- **`mbedtls_ssl_write()`:** Called via `tls_send_frame()` on the TLS context for slot 0. Direction: outbound to server through the TLS record layer.
- **POSIX `::send()`:** Underlying socket call made by mbedTLS.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TlsTcpBackend` | `m_local_node_id` | NODE_ID_INVALID (0) | `id` |
| TLS session (slot 0) | record layer output | previous | + HELLO record |

---

## 12. Sequence Diagram

```
DeliveryEngine::init()
  -> TlsTcpBackend::register_local_id(id)
       [m_is_server == false; TLS session established]
       -> send_hello_frame()
            -> Serializer::serialize(hello, m_wire_buf, ...)
            <- hello_len
            -> tls_send_frame(0, m_wire_buf, hello_len, timeout)
                 -> mbedtls_ssl_write()     [TLS record layer encrypt + send]
                 <- bytes_written == hello_len
            <- true
            -> Logger::log(INFO, "Sent HELLO NodeId N to server via TLS")
       <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Initialization only:**
- `register_local_id()` is called exactly once per `DeliveryEngine::init()` cycle, after TLS handshake completion.
- HELLO frames only appear on the init path.

**Precondition:**
- TLS handshake must be complete before `send_hello_frame()` is called. `TlsTcpBackend::init()` guarantees this ordering.

---

## 14. Known Risks / Observations

- **TLS session torn down between handshake and HELLO send:** Extremely unlikely in init sequence; `NEVER_COMPILED_OUT_ASSERT(m_open)` would catch a broken state before `send_hello_frame()` is reached.
- **`tls_enabled == false`:** When TLS is disabled, `TlsTcpBackend` falls back to plaintext socket sends. `tls_send_frame()` in plaintext mode calls `tcp_send_frame()` directly. The HELLO flow is identical from `send_hello_frame()`'s perspective.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Slot 0 is always the server connection in TLS client mode; this matches the TcpBackend client convention where `m_client_fds[0]` is the single server socket.
- `[ASSUMPTION]` `tls_send_frame()` is a private helper on `TlsTcpBackend` analogous to `tcp_send_frame()` in TcpBackend.
