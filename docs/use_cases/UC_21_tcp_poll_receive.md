# UC_21 — TCP poll and receive — poll_clients_once() called internally by TcpBackend::receive_message()

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System
**Requirement traceability:** REQ-6.1.6, REQ-6.3.3, REQ-4.1.3

---

## 1. Use Case Overview

- **Trigger:** `TcpBackend::poll_clients_once(timeout_ms)` is called internally from `TcpBackend::receive_message()`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Accept any pending new connections (server mode), poll all active client fds for readable data, and drain one frame from each fd that is ready.
- **Success outcome:** Zero or more `MessageEnvelope`s are pushed into `m_recv_queue`. Returns void.
- **Error outcomes:** Individual client errors are handled by `remove_client()` inside `recv_from_client()`. Function itself has no return value.

**Invoking UC:** Called from `TcpBackend::receive_message()` in a bounded loop (`poll_count = min(50, (timeout_ms+99)/100)` iterations of 100ms each).

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (private)
void TcpBackend::poll_clients_once(uint32_t timeout_ms);
```

Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `TcpBackend::receive_message(out, timeout_ms)` calls `poll_clients_once(100)` in a bounded loop.
2. Inside `poll_clients_once(timeout_ms)`:
3. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
4. If `m_is_server`: **`accept_and_handshake()`** is called first to accept any pending connection (non-blocking; returns immediately if none).
5. **Build `pollfd` array:** `fds[m_client_count]` — for each client, set `fd = m_client_fds[idx]`, `events = POLLIN`.
6. **`::poll(fds, m_client_count, timeout_ms)`** — blocks up to `timeout_ms` milliseconds waiting for any client fd to become readable.
7. If `::poll()` returns 0 (timeout): return (no data).
8. If `::poll()` returns < 0 (error): log `WARNING_LO`; return.
9. **Loop** over `i` in `0..m_client_count-1` (bounded by `MAX_TCP_CONNECTIONS`):
   - If `fds[i].revents & POLLIN`: call **`recv_from_client(i, timeout_ms)`** (UC_06). Note: `m_client_count` may decrease inside `recv_from_client()` if a client is removed; loop bound is re-evaluated.
10. Returns (no return value).

---

## 4. Call Tree

```
TcpBackend::receive_message()              [TcpBackend.cpp]
 └── [bounded loop: poll_count iterations]
      TcpBackend::poll_clients_once(100)   [TcpBackend.cpp]
       ├── TcpBackend::accept_and_handshake()   [server only]
       │    └── ISocketOps::do_accept()         [::accept()]
       ├── ::poll(fds, n, timeout_ms)            [POSIX]
       └── [for each POLLIN client:]
            TcpBackend::recv_from_client(idx, timeout_ms)  (UC_06)
```

---

## 5. Key Components Involved

- **`poll_clients_once()`** — Fan-in: accepts new clients (server) and reads one frame from each readable client.
- **`::poll()`** — POSIX multiplexing call. Blocks until at least one fd is readable or timeout expires.
- **`accept_and_handshake()`** — Accepts one pending TCP connection per call (non-blocking). For TlsTcpBackend, performs TLS handshake.
- **`recv_from_client()`** (UC_06) — Reads, deserializes, and enqueues one frame per client.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_is_server` | Call `accept_and_handshake()` | Skip accept |
| `::poll()` returns 0 | Timeout; return | Process ready fds |
| `::poll()` returns < 0 | Log error; return | Process ready fds |
| `fds[i].revents & POLLIN` | Call `recv_from_client(i, ...)` | Skip client |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application's thread.
- `::poll()` blocks the calling thread for up to `timeout_ms` milliseconds.
- Not thread-safe.

---

## 8. Memory & Ownership Semantics

- `pollfd fds[MAX_TCP_CONNECTIONS]` — stack array of 8 `pollfd` structs. Each is ~8 bytes.
- No heap allocation.

---

## 9. Error Handling Flow

- **`::poll()` error:** Logged; `poll_clients_once()` returns without reading. Called again on next iteration.
- **`recv_from_client()` error:** Client removed inside `recv_from_client()`; loop continues with remaining clients.

---

## 10. External Interactions

- **POSIX `::poll()`:** On all active client fds. Blocks up to `timeout_ms`.
- **POSIX `::accept()`:** On `m_listen_fd` (server mode only). Non-blocking.

---

## 11. State Changes / Side Effects

- `m_recv_queue` — populated with zero or more new `MessageEnvelope`s from `recv_from_client()`.
- `m_client_fds[]` / `m_client_count` — updated on new accepts or client removals.

---

## 12. Sequence Diagram

```
TcpBackend::receive_message(out, timeout_ms)
  [bounded loop: poll_count iterations]
  -> poll_clients_once(100)
       -> accept_and_handshake()              [if server; non-blocking]
       -> ::poll(fds, n, 100ms)               [wait for readable fds]
            <- revents = POLLIN for client[0]
       -> recv_from_client(0, 100)            [UC_06]
            -> ISocketOps::recv_frame() -> Serializer::deserialize()
            -> RingBuffer::push()
       <- [returns; no new messages for other fds]
  [RingBuffer::pop() succeeds; envelope returned to caller]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TcpBackend::init()` called; `m_open == true`.
- For server: `m_listen_fd` valid. For client: at least one entry in `m_client_fds[]`.

**Runtime:**
- `poll_clients_once()` is called repeatedly from `receive_message()`. Each call is bounded by one `::poll()` timeout and one pass through the client array.

---

## 14. Known Risks / Observations

- **One frame per client per call:** Each `recv_from_client()` reads only one frame. If a client has multiple frames queued in the kernel buffer, they are drained one per `receive_message()` call iteration.
- **Thundering herd:** If all 8 clients have data ready, all 8 are serviced in sequence in a single `poll_clients_once()` call. Total latency for the last client = sum of first 7 clients' frame read times.
- **Poll overhead:** `::poll()` has O(n) kernel overhead per call. For 8 clients this is negligible.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `poll_count = min(50, (timeout_ms + 99) / 100)` — the number of `poll_clients_once(100)` iterations in `receive_message()`. This is inferred from TcpBackend.cpp; the calculation bounds the outer loop to at most 50 iterations of 100ms each (= 5 seconds maximum).
