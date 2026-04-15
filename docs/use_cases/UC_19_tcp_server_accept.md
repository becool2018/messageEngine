# UC_19 — TCP server bind, listen, and non-blocking accept loop

**HL Group:** HL-7 — Start a Server Endpoint
**Actor:** User
**Requirement traceability:** REQ-6.1.1, REQ-6.1.2, REQ-6.3.1, REQ-7.1.1

---

## 1. Use Case Overview

- **Trigger:** User calls `TcpBackend::init(config)` with `config.is_server == true`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Bind a TCP listening socket to the configured IP/port, put it in the LISTEN state, and begin accepting incoming connections in a non-blocking accept loop embedded in `poll_clients_once()`.
- **Success outcome:** `init()` returns `Result::OK`. `m_listen_fd` is a bound, listening socket. `m_open == true`. Subsequent calls to `receive_message()` will accept new connections in the polling loop.
- **Error outcomes:**
  - `Result::ERR_IO` from `bind_and_listen()` — bind/listen failed; socket creation or option setting failed.
  - Logged at `WARNING_LO` or `FATAL` depending on the specific failure.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp
Result TcpBackend::init(const TransportConfig& config);
```

Called by the User during system initialization.

---

## 3. End-to-End Control Flow

**During `init()`:**
1. **`TcpBackend::init(config)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(!m_open)` — not already initialized.
3. `NEVER_COMPILED_OUT_ASSERT(config.is_server || config.peer_ip[0] != '\0')` — valid config.
4. Store `m_cfg = config`. Initialize `m_impairment` with `config.impairment` config. Call `m_recv_queue.init()`.
5. `config.is_server == true` — server path.
6. **`bind_and_listen(config.ip, config.port)`** is called (`TcpBackend.cpp`):
   a. `m_sock_ops->create_tcp()` — creates a TCP socket: `::socket(AF_INET, SOCK_STREAM, 0)`.
   b. `m_sock_ops->set_reuseaddr(m_listen_fd)` — sets `SO_REUSEADDR` to avoid `EADDRINUSE` on restart.
   c. `m_sock_ops->set_nonblocking(m_listen_fd)` — sets `O_NONBLOCK` flag.
   d. `m_sock_ops->do_bind(m_listen_fd, ip, port)` — `::bind()`.
   e. `m_sock_ops->do_listen(m_listen_fd, SOMAXCONN)` — `::listen()`.
   f. `LOG_INFO("TcpBackend", "Server listening on %s:%u", ip, port)`.
   g. Returns `Result::OK`.
7. If `bind_and_listen()` fails: log error; close fd; return `Result::ERR_IO`.
8. `m_is_server = true`, `m_open = true`.
9. Returns `Result::OK` to the User.

**During `poll_clients_once()` (ongoing accept loop):**
1. `receive_message()` → `poll_clients_once()` is called with each iteration.
2. `accept_and_handshake()` is called:
   a. `m_sock_ops->do_accept(m_listen_fd, &client_addr)` → `::accept()`. If no pending connection: returns `EAGAIN`/`EWOULDBLOCK`; returns `false`.
   b. If connection: new fd stored at `m_client_fds[m_client_count]`.
   c. `m_client_count++`.
   d. `LOG_INFO("TcpBackend", "Client connected: %s", addr_str)`.

---

## 4. Call Tree

```
TcpBackend::init(config)                      [TcpBackend.cpp]
 ├── RingBuffer::init()                        [RingBuffer.hpp]
 ├── ImpairmentEngine init (implicit)
 └── TcpBackend::bind_and_listen(ip, port)     [TcpBackend.cpp]
      ├── ISocketOps::create_tcp()             [SocketOpsImpl]
      ├── ISocketOps::set_reuseaddr()
      ├── ISocketOps::set_nonblocking()
      ├── ISocketOps::do_bind()                [::bind()]
      ├── ISocketOps::do_listen()              [::listen()]
      └── LOG_INFO(...)

TcpBackend::poll_clients_once() [called from receive_message()]
 └── TcpBackend::accept_and_handshake()
      └── ISocketOps::do_accept()              [::accept()]
```

---

## 5. Key Components Involved

- **`TcpBackend::bind_and_listen()`** — Sets up the server socket. Encapsulates all socket option + bind + listen steps.
- **`TcpBackend::accept_and_handshake()`** — Non-blocking accept on every `poll_clients_once()` call. In plaintext mode: just stores the accepted fd. In TLS mode (UC_36): performs handshake.
- **`ISocketOps / SocketOpsImpl`** — Injectable POSIX adapter for all socket syscalls.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `config.is_server` | Call `bind_and_listen()` | Call `connect_to_server()` (UC_35) |
| `bind_and_listen()` fails | Return `ERR_IO`; close listen fd | Set `m_open = true`; return OK |
| `do_accept()` returns EAGAIN | No new client; return (non-blocking) | Accept client; increment `m_client_count` |
| `m_client_count >= MAX_TCP_CONNECTIONS` | Reject new connection | Accept |

---

## 7. Concurrency / Threading Behavior

- `init()` is called once during setup; single-threaded.
- `poll_clients_once()` runs in the application thread calling `receive_message()`. The accept loop is non-blocking (O_NONBLOCK on `m_listen_fd`).
- No atomics; no multithreading within TcpBackend.

---

## 8. Memory & Ownership Semantics

- `m_listen_fd` — POSIX file descriptor; owned by `TcpBackend`. Closed in `close()`.
- `m_client_fds[MAX_TCP_CONNECTIONS]` — fixed array of 8 fd slots.
- No heap allocation. Power of 10 Rule 3 satisfied.

---

## 9. Error Handling Flow

- **`bind_and_listen()` failure:** Socket is closed before returning `ERR_IO`. `m_open` remains false. Transport must be re-initialized.
- **Accept failure (non-EAGAIN):** Logged at WARNING_LO; ignored (accept loop continues).
- **Max connections:** When `m_client_count == MAX_TCP_CONNECTIONS`, the listening socket continues to hold pending connections in the kernel backlog. They are accepted only after an existing client disconnects.

---

## 10. External Interactions

- **POSIX `::socket()`, `::setsockopt()`, `::bind()`, `::listen()`, `::accept()`:** Called during `init()` and `poll_clients_once()`.
- **`stderr`:** Logger INFO on bind success; WARNING_LO on failures.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_listen_fd` | -1 | valid listening fd |
| `TcpBackend` | `m_is_server` | false | true |
| `TcpBackend` | `m_open` | false | true |
| `TcpBackend` | `m_client_fds[m_client_count]` | -1 | accepted fd (on connect) |
| `TcpBackend` | `m_client_count` | 0 | 0..MAX_TCP_CONNECTIONS |

---

## 12. Sequence Diagram

```
User
  -> TcpBackend::init(config)   [config.is_server=true]
       -> RingBuffer::init()
       -> bind_and_listen(ip, port)
            -> ISocketOps::create_tcp()      [::socket()]
            -> ISocketOps::set_reuseaddr()   [SO_REUSEADDR]
            -> ISocketOps::set_nonblocking() [O_NONBLOCK]
            -> ISocketOps::do_bind()         [::bind()]
            -> ISocketOps::do_listen()       [::listen()]
            -> LOG_INFO("listening")
       <- Result::OK [m_open=true]
  <- Result::OK

[Later: client connects]
User -> TcpBackend::receive_message() -> poll_clients_once()
  -> accept_and_handshake()
       -> ISocketOps::do_accept()   [::accept()]  <- new fd
       [m_client_fds[m_client_count] = new_fd; m_client_count++]
       -> LOG_INFO("Client connected")
```

---

## 13. Initialization vs Runtime Flow

**Preconditions (before `init()`):**
- `TcpBackend` default-constructed; `m_open == false`; all fds == -1.
- Network interface available at the configured IP:port.

**Runtime (accept loop):**
- `accept_and_handshake()` runs on every `receive_message()` call. Accepts one connection per call (non-blocking; returns immediately if no pending connection). The loop is bounded by `poll_count` iterations (Power of 10 Rule 2 infrastructure deviation applies to the outer loop).

---

## 14. Known Risks / Observations

- **One accept per `receive_message()` call:** If the application calls `receive_message()` infrequently and many clients connect simultaneously, the kernel backlog absorbs connections but `m_client_count` may grow slowly.
- **`MAX_TCP_CONNECTIONS = 8`:** Hard limit on simultaneous clients. Excess connections are refused when the table is full.
- **`SO_REUSEADDR`:** Required to avoid `EADDRINUSE` when the server restarts quickly. Without it, the OS TIME_WAIT state blocks immediate rebind.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `accept_and_handshake()` performs a plaintext accept for `TcpBackend`; TLS handshake is only performed in `TlsTcpBackend::accept_and_handshake()` (UC_36).
- `[ASSUMPTION]` The listen backlog is `SOMAXCONN` (platform-defined, typically 128). Queued pending connections beyond the backlog are silently dropped by the kernel.
