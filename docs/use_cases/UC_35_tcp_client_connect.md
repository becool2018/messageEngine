# UC_35 — TCP client connect with configurable timeout

**HL Group:** HL-8 — Start a Client Endpoint
**Actor:** User
**Requirement traceability:** REQ-6.1.1, REQ-6.1.2, REQ-6.3.3, REQ-7.1.1

---

## 1. Use Case Overview

- **Trigger:** User calls `TcpBackend::init(config)` with `config.is_server == false` and `config.peer_ip` / `config.peer_port` set. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Establish a TCP connection to the configured peer within `connect_timeout_ms` milliseconds and make the transport ready for `send_message()` / `receive_message()`.
- **Success outcome:** `init()` returns `Result::OK`. `m_client_fds[0]` is the connected socket. `m_client_count == 1`. `m_open == true`.
- **Error outcomes:**
  - `Result::ERR_IO` — connection refused, timeout, or socket creation failed.
  - Logged at WARNING_LO on connect failure.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp
Result TcpBackend::init(const TransportConfig& config);
```

---

## 3. End-to-End Control Flow

1. **`TcpBackend::init(config)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
3. `NEVER_COMPILED_OUT_ASSERT(!config.is_server && config.peer_ip[0] != '\0')`.
4. Store `m_cfg = config`. `m_recv_queue.init()`.
5. `config.is_server == false` — client path.
6. **`connect_to_server()`** is called (`TcpBackend.cpp`):
   a. `m_sock_ops->create_tcp()` → `::socket(AF_INET, SOCK_STREAM, 0)`. Store as `m_client_fds[0]`.
   b. `m_sock_ops->set_nonblocking(m_client_fds[0])`.
   c. `m_sock_ops->connect_with_timeout(m_client_fds[0], peer_ip, peer_port, connect_timeout_ms)`:
      - `::connect()` in non-blocking mode → returns `EINPROGRESS`.
      - `::poll({fd, POLLOUT, 0}, 1, connect_timeout_ms)` — waits for writable (connected).
      - Check `SO_ERROR` via `getsockopt` — if 0: connected.
   d. If connect fails: close fd; return `ERR_IO`.
   e. `Logger::log(INFO, "TcpBackend", "Connected to %s:%u", peer_ip, peer_port)`.
7. `m_client_count = 1`.
8. `m_is_server = false`.
9. `m_open = true`.
10. Returns `Result::OK`.

---

## 4. Call Tree

```
TcpBackend::init(config)                       [TcpBackend.cpp]
 ├── RingBuffer::init()
 └── TcpBackend::connect_to_server()           [TcpBackend.cpp]
      ├── ISocketOps::create_tcp()             [::socket()]
      ├── ISocketOps::set_nonblocking()        [fcntl O_NONBLOCK]
      └── ISocketOps::connect_with_timeout()   [SocketUtils.cpp]
           ├── ::connect()                     [POSIX]
           ├── ::poll(fd, POLLOUT, timeout)     [POSIX]
           └── ::getsockopt(SO_ERROR)           [POSIX]
```

---

## 5. Key Components Involved

- **`connect_to_server()`** — Creates socket, sets non-blocking, connects with timeout using `::poll()`.
- **`ISocketOps::connect_with_timeout()`** — Implements the non-blocking connect + poll + `SO_ERROR` check pattern.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `config.is_server` | Server path (UC_19) | Client path (this UC) |
| `connect_with_timeout()` fails | Close fd; return ERR_IO | Set m_client_count=1; m_open=true |
| `::poll()` times out | Connection timeout; ERR_IO | Check SO_ERROR |
| `SO_ERROR != 0` | Connection refused; ERR_IO | Connected |

---

## 7. Concurrency / Threading Behavior

- `init()` is called once during setup; single-threaded.
- After `init()`, `send_message()` / `receive_message()` called from application thread.

---

## 8. Memory & Ownership Semantics

- `m_client_fds[0]` — connected socket fd; owned by TcpBackend; closed in `close()`.
- No heap allocation.

---

## 9. Error Handling Flow

- **`ERR_IO`:** Socket closed; `m_open` remains false. Caller must retry `init()` or report failure.
- **Timeout:** If the peer is not listening, `::connect()` returns `ETIMEDOUT` or `ECONNREFUSED` after the timeout.

---

## 10. External Interactions

- **POSIX `::socket()`, `::connect()`, `::poll()`, `::getsockopt()`:** Called during connection establishment.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_client_fds[0]` | -1 | connected socket fd |
| `TcpBackend` | `m_client_count` | 0 | 1 |
| `TcpBackend` | `m_is_server` | false | false |
| `TcpBackend` | `m_open` | false | true |

---

## 12. Sequence Diagram

```
User
  -> TcpBackend::init(config)   [config.is_server=false]
       -> RingBuffer::init()
       -> connect_to_server()
            -> ISocketOps::create_tcp()             [::socket()]
            -> ISocketOps::set_nonblocking()
            -> ISocketOps::connect_with_timeout()
                 -> ::connect()                     [EINPROGRESS]
                 -> ::poll(fd, POLLOUT, timeout)     [waits for writable]
                 -> ::getsockopt(SO_ERROR)           [verify 0]
            <- success
            -> Logger::log(INFO, "Connected to ...")
  [m_client_count=1; m_open=true]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Peer TCP server must be listening at `peer_ip:peer_port`.
- `config.connect_timeout_ms > 0`.

**Runtime:**
- After `init()`, `m_client_fds[0]` is the active connection used by `send_to_all_clients()` and `poll_clients_once()`.

---

## 14. Known Risks / Observations

- **Non-blocking connect race:** Between `::connect()` returning `EINPROGRESS` and `::poll()` returning writable, the connection attempt is in progress. If the peer closes immediately, `SO_ERROR` will be non-zero.
- **Server not yet listening:** If `connect_timeout_ms` is too short and the server is slow to start, the client will fail with timeout. Tests should sequence server `init()` before client `init()`.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `connect_with_timeout()` in `SocketUtils.cpp` uses non-blocking connect + `::poll(POLLOUT)` + `getsockopt(SO_ERROR)`. This is a standard POSIX pattern and confirmed from `ISocketOps.hpp` interface.
