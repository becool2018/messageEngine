# UC_05 — Server echo reply — calls receive_message() then send_message() in sequence

**HL Group:** Application Workflow (above system boundary)
**Actor:** User/Application (Server process)
**Requirement traceability:** REQ-3.3.1, REQ-3.3.2, REQ-4.1.2, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** The server application calls `DeliveryEngine::receive()` and receives a DATA envelope; it then calls `DeliveryEngine::send()` with a response, forming an echo loop. Files: `src/app/Server.cpp`, `src/core/DeliveryEngine.cpp`.
- **Goal:** Receive one inbound DATA message and immediately send a reply back to the originator. This is an application-layer pattern that combines two system-boundary calls.
- **Success outcome:** `receive()` returns `Result::OK` with the inbound envelope; `send()` returns `Result::OK` with the echoed reply on the wire.
- **Error outcomes:**
  - `Result::ERR_TIMEOUT` from `receive()` — no message arrived within `timeout_ms`; send is not called.
  - `Result::ERR_EXPIRED` from `receive()` — received message was stale; send is not called.
  - `Result::ERR_DUPLICATE` from `receive()` — duplicate suppressed; send is not called.
  - Any error from `send()` on the reply path.

**Note:** This UC documents the application-layer pattern. The two system calls (`receive` and `send`) are described individually in UC_45 and UC_01/UC_02/UC_03 respectively; this document describes their composition in `Server.cpp`.

---

## 2. Entry Points

```cpp
// src/app/Server.cpp — main event loop
// Calls:
Result DeliveryEngine::receive(MessageEnvelope& out, uint32_t timeout_ms, uint64_t now_us);
Result DeliveryEngine::send(MessageEnvelope& envelope, uint64_t now_us);
```

Both called sequentially in the server's main loop thread.

---

## 3. End-to-End Control Flow

1. **Server application main loop** (`Server.cpp`) calls `engine.receive(recv_env, timeout_ms, now_us)`.
2. `DeliveryEngine::receive()` calls `m_transport->receive_message(recv_env, timeout_ms)` — blocks up to `timeout_ms` milliseconds.
   - If no message: returns `ERR_TIMEOUT`; loop continues.
3. If a DATA envelope is received: expiry check, dedup check (RELIABLE_RETRY only), ACK auto-send (RELIABLE_ACK/RELIABLE_RETRY). Returns `Result::OK` with `recv_env` populated.
4. **Server application** inspects `recv_env.message_type == DATA`; constructs a reply `MessageEnvelope reply` by copying relevant fields (source/destination swapped, payload set).
5. **`engine.send(reply, now_us)`** is called. For BEST_EFFORT echo: no ACK slot; message goes directly to wire via TCP as in UC_01.
6. `send()` returns `Result::OK`. Echo reply is on the wire.
7. Loop iterates; server calls `receive()` again.

---

## 4. Call Tree

```
Server main loop                               [Server.cpp]
 ├── DeliveryEngine::receive()                 [DeliveryEngine.cpp]
 │    ├── TcpBackend::receive_message()        [TcpBackend.cpp]
 │    │    └── poll_clients_once() -> recv_from_client() -> Serializer::deserialize()
 │    ├── timestamp_expired()                  [Timestamp.hpp]
 │    ├── DuplicateFilter::check_and_record()  [DuplicateFilter.cpp]  (RELIABLE_RETRY only)
 │    └── [auto-ACK send for RELIABLE_ACK/RETRY]
 └── DeliveryEngine::send(reply, now_us)       [DeliveryEngine.cpp]
      ├── MessageIdGen::next()
      ├── reserve_bookkeeping()                [DeliveryEngine.cpp] (no-op for BEST_EFFORT)
      └── send_via_transport()
           └── TcpBackend::send_message()
                └── Serializer::serialize() -> ::send()
```

---

## 5. Key Components Involved

- **`Server.cpp`** — Application orchestration: constructs the reply envelope and sequences receive then send.
- **`DeliveryEngine`** — Provides the two system-boundary calls; manages ID assignment, ACK, retry, and dedup internally.
- **`TcpBackend`** — Handles the network I/O for both receive and send.
- **`DuplicateFilter`** — Ensures the same DATA message is not echoed twice if retransmitted by the sender.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `receive()` returns `ERR_TIMEOUT` | Loop continues; no send | Envelope received; proceed to send |
| `receive()` returns `ERR_DUPLICATE` | Loop continues; no send | First occurrence; proceed to send |
| `recv_env.message_type != DATA` | Server may skip echo; depends on application | Echo the DATA message |
| `send()` returns error | Server logs error; loop continues | Echo delivered |

---

## 7. Concurrency / Threading Behavior

- Single-threaded server event loop (`Server.cpp`). Both `receive()` and `send()` are called from the same thread.
- No synchronization primitives are required within this single-thread pattern.
- The `TcpBackend` `poll_clients_once()` is bounded per iteration (`poll_count` iterations of `poll(100ms)`).

---

## 8. Memory & Ownership Semantics

- `recv_env` and `reply` are stack-allocated `MessageEnvelope` structs (~4152 bytes each) in `Server.cpp`'s event loop stack frame.
- No heap allocation in this flow.
- Ownership: all envelopes are stack-local; no transfer of ownership.

---

## 9. Error Handling Flow

- `ERR_TIMEOUT`: Expected during quiet periods; server loops back.
- `ERR_EXPIRED` / `ERR_DUPLICATE`: Message dropped; server logs and continues.
- Send error: server logs; no retry at the application layer for BEST_EFFORT echo.

---

## 10. External Interactions

- **POSIX `::poll()` and `::recv()`:** Called inside `TcpBackend::poll_clients_once()` to receive inbound data.
- **POSIX `::send()`:** Called via `send_to_all_clients()` to send the echo reply.
- **`stderr`:** Logger writes to stderr on error conditions.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DuplicateFilter` | `m_buf[m_write_idx]` | old entry | new (source_id, message_id) on RELIABLE_RETRY |
| `MessageIdGen` | `m_counter` | N | N+1 (reply ID assigned) |
| TCP socket | kernel send/recv buffers | previous | inbound consumed; reply appended |

---

## 12. Sequence Diagram

```
Server (main loop)
  -> DeliveryEngine::receive(recv_env, timeout_ms, now_us)
       -> TcpBackend::receive_message()
            -> poll_clients_once() -> ::poll() -> ::recv()
            -> Serializer::deserialize()
            -> RingBuffer::push()
       -> RingBuffer::pop()
       -> timestamp_expired()       [check; false for fresh message]
       -> DuplicateFilter (if RELIABLE_RETRY)
       <- Result::OK [recv_env populated]
  [Server builds reply: swap src/dst, set payload]
  -> DeliveryEngine::send(reply, now_us)
       -> MessageIdGen::next()
       -> reserve_bookkeeping()    [no-op for BEST_EFFORT; allocates slot for RELIABLE_ACK/RETRY]
       -> send_via_transport() -> TcpBackend::send_message() -> ::send()
       <- Result::OK
  [Server loops back to receive()]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called with a live `TcpBackend`.
- TCP connection established (client connected to server).

**Runtime:**
- The server loop runs indefinitely (Power of 10 Rule 2 deviation: infrastructure event loop). Each iteration blocks on `receive()` for up to `timeout_ms` milliseconds.

---

## 14. Known Risks / Observations

- **Tight coupling of receive/send:** If `send()` fails (e.g., transport full), the original DATA message has already been ACKed (if RELIABLE_ACK). The sender will not retransmit, but the echo was not sent.
- **Single-client assumption:** `send_to_all_clients()` echoes to all connected clients, not just the original sender. In a multi-client scenario, this broadcasts the echo.
- **No backpressure:** If the sender floods faster than the server can echo, the server's `receive()` queue fills; older messages may be dropped.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The reply envelope in `Server.cpp` swaps `source_id` and `destination_id` from `recv_env`. This is a common echo pattern but specific behavior depends on `Server.cpp` application logic.
- `[ASSUMPTION]` The echo reply uses `ReliabilityClass::BEST_EFFORT` unless the server explicitly copies the original `reliability_class`. Application code determines this.
