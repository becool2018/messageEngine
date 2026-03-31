# UC_19 — TCP server bind, listen, and non-blocking accept loop

**HL Group:** HL-7 — Start a Server Endpoint
**Actor:** System
**Requirement traceability:** REQ-4.1.1, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.3.1, REQ-6.3.2, REQ-6.3.5, REQ-7.1.1

---

## 1. Use Case Overview

**Trigger:** The application calls `TcpBackend::init(config)` with `config.is_server = true` and `config.kind == TransportKind::TCP`.

**Goal:** The server backend creates a TCP listening socket, binds it to the configured IP/port, calls `listen()`, and sets the socket to non-blocking mode. From that point on, `accept_clients()` is called from within `poll_clients_once()` on every `receive_message()` iteration to accept new client connections without blocking. Each accepted client fd is stored in `m_client_fds[]`.

**Success outcome:** `bind_and_listen()` returns `Result::OK`, `m_listen_fd >= 0`, `m_open = true`. Subsequent calls to `accept_clients()` inside the `receive_message()` loop add new client file descriptors to `m_client_fds[]` when connections are pending.

**Error outcomes:**
- `ERR_IO` from `bind_and_listen()` if any of: `create_tcp()`, `set_reuseaddr()`, `do_bind()`, `do_listen()`, or `set_nonblocking()` fails.
- `accept_clients()` silently returns `OK` if no connection is pending (EAGAIN in non-blocking mode) or the client array is full.

---

## 2. Entry Points

**Initialization path:**

```
// src/platform/TcpBackend.cpp
Result TcpBackend::init(const TransportConfig& config);
  └── Result TcpBackend::bind_and_listen(const char* ip, uint16_t port);
```

**Steady-state accept path (called per poll iteration):**

```
// src/platform/TcpBackend.cpp
void TcpBackend::poll_clients_once(uint32_t timeout_ms);
  └── Result TcpBackend::accept_clients();
```

Underlying socket operations via virtual interface:

```
// src/platform/ISocketOps.hpp (implemented by SocketOpsImpl)
int  ISocketOps::create_tcp()
bool ISocketOps::set_reuseaddr(int fd)
bool ISocketOps::do_bind(int fd, const char* ip, uint16_t port)
bool ISocketOps::do_listen(int fd, int backlog)
bool ISocketOps::set_nonblocking(int fd)
int  ISocketOps::do_accept(int fd)
```

---

## 3. End-to-End Control Flow (Step-by-Step)

**Phase A — Initialization: bind and listen**

1. Application calls `TcpBackend::init(config)`.
2. Preconditions: `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP)`, `NEVER_COMPILED_OUT_ASSERT(!m_open)`.
3. `m_cfg = config`, `m_is_server = config.is_server` (true).
4. `m_recv_queue.init()` — initializes the inbound ring buffer.
5. `impairment_config_default(imp_cfg)` — creates a default config; if `config.num_channels > 0`, overrides with `config.channels[0].impairment`.
6. `m_impairment.init(imp_cfg)` — seeds PRNG, zeros delay buffer.
7. `m_is_server` is `true` → `bind_and_listen(config.bind_ip, config.bind_port)` is called.
8. Inside `bind_and_listen(ip, port)`:
   a. `NEVER_COMPILED_OUT_ASSERT(ip != nullptr)`, `NEVER_COMPILED_OUT_ASSERT(m_is_server)`.
   b. `m_listen_fd = m_sock_ops->create_tcp()` — calls `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` via vtable. Returns fd >= 0 or -1.
   c. If `m_listen_fd < 0`: log FATAL "socket_create_tcp failed"; return `ERR_IO`.
   d. `m_sock_ops->set_reuseaddr(m_listen_fd)` — calls `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(int))` via vtable.
   e. If `set_reuseaddr` fails: log WARNING_HI; `do_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.
   f. `m_sock_ops->do_bind(m_listen_fd, ip, port)` — calls `socket_bind()`:
      - `inet_aton(ip, &addr.sin_addr)` — parse IP string.
      - `bind(fd, &addr, sizeof(addr))` — POSIX bind.
   g. If `do_bind` fails: log FATAL; `do_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.
   h. `m_sock_ops->do_listen(m_listen_fd, static_cast<int>(MAX_TCP_CONNECTIONS))` — calls `listen(fd, backlog=8)`.
   i. If `do_listen` fails: log FATAL; `do_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.
   j. `m_sock_ops->set_nonblocking(m_listen_fd)` — calls `fcntl(fd, F_GETFL, 0)` then `fcntl(fd, F_SETFL, flags | O_NONBLOCK)`.
   k. If `set_nonblocking` fails: log WARNING_HI; `do_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.
   l. `m_open = true`.
   m. Log INFO "Server listening on %s:%u".
   n. `NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0)`. Returns `Result::OK`.
9. Back in `init()`: `result_ok(res)` is `true`. `NEVER_COMPILED_OUT_ASSERT(m_open)`. Returns `Result::OK`.

**Phase B — Steady state: poll and accept**

10. Application calls `TcpBackend::receive_message(envelope, timeout_ms)`.
11. `m_recv_queue.pop(envelope)` — try the fast path; returns `ERR_EMPTY` if nothing queued.
12. `poll_count = min((timeout_ms + 99) / 100, 50)` — number of 100 ms poll iterations.
13. Loop for `poll_count` iterations (Power of 10 fixed bound):
    a. `poll_clients_once(100U)` is called.
14. Inside `poll_clients_once(100U)`:
    a. `NEVER_COMPILED_OUT_ASSERT(m_open)`, `NEVER_COMPILED_OUT_ASSERT(timeout_ms <= 60000U)`.
    b. Build `pollfd pfds[MAX_POLL_FDS]` (stack array; `MAX_POLL_FDS = MAX_TCP_CONNECTIONS + 1 = 9`).
    c. `has_listen = m_is_server && (m_listen_fd >= 0) && (m_client_count < MAX_TCP_CONNECTIONS)`.
    d. If `has_listen`: `pfds[0] = {m_listen_fd, POLLIN, 0}`; `nfds = 1`.
    e. Loop `i = 0..MAX_TCP_CONNECTIONS-1`: for each valid client fd, append `{m_client_fds[i], POLLIN, 0}` to `pfds[]`; `++nfds`.
    f. `NEVER_COMPILED_OUT_ASSERT(nfds <= MAX_POLL_FDS)`.
    g. If `nfds == 0`: return immediately (nothing to watch).
    h. `poll(pfds, nfds, 100)` — POSIX poll. Blocks up to 100 ms.
    i. `poll_rc <= 0` (timeout or error): return.
    j. `has_listen && (pfds[0].revents & POLLIN) != 0` — listen fd is readable.
    k. `accept_clients()` is called.
15. Inside `accept_clients()`:
    a. `NEVER_COMPILED_OUT_ASSERT(m_is_server)`, `NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0)`.
    b. `m_client_count >= MAX_TCP_CONNECTIONS` — if at capacity, return `OK` without calling `do_accept`.
    c. `client_fd = m_sock_ops->do_accept(m_listen_fd)` — calls `accept(m_listen_fd, nullptr, nullptr)`.
       - In non-blocking mode: if no pending connection, `accept()` returns -1 with `errno = EAGAIN`. `socket_accept()` logs WARNING_LO and returns -1.
       - If a client is pending: returns the new client fd.
    d. If `client_fd < 0`: return `OK` (no pending connection; not an error).
    e. `NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS)`.
    f. `m_client_fds[m_client_count] = client_fd`.
    g. `++m_client_count`.
    h. Log INFO "Accepted client %u, total clients: %u".
    i. `NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS)`. Returns `OK`.
16. Back in `poll_clients_once()`: proceed to receive loop over client fds.
17. Back in `receive_message()` outer loop: try `m_recv_queue.pop()` again; continue.

---

## 4. Call Tree

```
TcpBackend::init()
 ├── m_recv_queue.init()
 ├── ImpairmentEngine::init()
 └── TcpBackend::bind_and_listen()
      ├── ISocketOps::create_tcp()          → socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
      ├── ISocketOps::set_reuseaddr()       → setsockopt(SO_REUSEADDR)
      ├── ISocketOps::do_bind()             → inet_aton() + bind()
      ├── ISocketOps::do_listen()           → listen(fd, MAX_TCP_CONNECTIONS)
      └── ISocketOps::set_nonblocking()     → fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)

TcpBackend::receive_message()
 └── TcpBackend::poll_clients_once()
      ├── [build pollfd array]
      ├── poll(pfds, nfds, 100ms)           [POSIX]
      ├── TcpBackend::accept_clients()       [if listen fd readable]
      │    └── ISocketOps::do_accept()      → accept(listen_fd, nullptr, nullptr)
      └── TcpBackend::recv_from_client()    [for each readable client fd]
```

---

## 5. Key Components Involved

- **`TcpBackend::bind_and_listen()`** (`src/platform/TcpBackend.cpp`): Creates and configures the listening socket. Sequence: create → SO_REUSEADDR → bind → listen → set_nonblocking. Each step closes and returns on failure.

- **`TcpBackend::accept_clients()`** (`src/platform/TcpBackend.cpp`): Non-blocking accept wrapper. Called once per `poll_clients_once()` iteration when the listen fd has a pending connection. Appends accepted fd to `m_client_fds[]`.

- **`TcpBackend::poll_clients_once()`** (`src/platform/TcpBackend.cpp`): Builds the `pollfd` set (listen fd + all client fds), calls `poll()`, then dispatches accept or receive based on which fds are readable.

- **`ISocketOps`** (`src/platform/ISocketOps.hpp`): Virtual interface. In production, `SocketOpsImpl::instance()` delegates to the `socket_*` free functions in `SocketUtils.cpp`. In tests, a mock can inject failures.

- **`SocketUtils`** (`src/platform/SocketUtils.cpp`): Free functions wrapping POSIX: `socket()`, `setsockopt()`, `inet_aton()`, `bind()`, `listen()`, `fcntl()`, `accept()`. All check return values and log failures.

- **`m_client_fds[MAX_TCP_CONNECTIONS]`**: Fixed-size array of 8 integer file descriptors. Tracks all connected clients. Index 0 used for client mode (single connection). Server mode fills slots 0..7 as clients arrive.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_is_server` in `init()` | `bind_and_listen()` | `connect_to_server()` |
| `create_tcp()` returns -1 | Log FATAL; return `ERR_IO` | Proceed |
| `set_reuseaddr()` returns false | Log WARNING_HI; close fd; return `ERR_IO` | Proceed |
| `do_bind()` returns false | Log FATAL; close fd; return `ERR_IO` | Proceed |
| `do_listen()` returns false | Log FATAL; close fd; return `ERR_IO` | Proceed |
| `set_nonblocking()` returns false | Log WARNING_HI; close fd; return `ERR_IO` | Set `m_open = true`; return OK |
| `m_client_count >= MAX_TCP_CONNECTIONS` in `accept_clients()` | Return OK without accepting | Call `do_accept()` |
| `do_accept()` returns -1 | Return OK (EAGAIN — no pending connection) | Store fd; `++m_client_count`; log INFO |
| `has_listen && pfds[0].revents & POLLIN` in `poll_clients_once()` | Call `accept_clients()` | Skip accept |
| `poll_rc <= 0` in `poll_clients_once()` | Return immediately (timeout or error) | Process readable fds |

---

## 7. Concurrency / Threading Behavior

- `bind_and_listen()` runs once during `init()` on the calling thread.
- `accept_clients()` runs inside `poll_clients_once()`, which is called from `receive_message()` on the calling thread.
- `m_client_fds[]` and `m_client_count` are plain non-atomic members. They are modified by `accept_clients()` and read in `poll_clients_once()`, `send_to_all_clients()`, and `recv_from_client()`. No locking exists.
- If `send_message()` and `receive_message()` are called concurrently from different threads, `m_client_fds[]` and `m_client_count` are subject to data races.

[ASSUMPTION: single-threaded use, or the application serializes all send/receive calls.]

- `poll()` is a blocking POSIX call with a 100 ms timeout. It releases no locks during the wait.
- The listen fd is set non-blocking so `accept()` returns immediately with EAGAIN if no client is ready. `poll()` still provides the wait; the non-blocking flag prevents an unexpected indefinite block on accept itself.

---

## 8. Memory & Ownership Semantics

**Stack allocations:**

| Variable | Declared in | Size |
|----------|-------------|------|
| `pfds[MAX_POLL_FDS]` | `poll_clients_once()` | `(MAX_TCP_CONNECTIONS + 1) * sizeof(struct pollfd)` = 9 × 8 = 72 bytes |
| `imp_cfg` | `init()` | `sizeof(ImpairmentConfig)` ≈ 64 bytes [ASSUMPTION] |

**Fixed member arrays:**

| Member | Capacity constant | Initial value |
|--------|-------------------|---------------|
| `m_client_fds[MAX_TCP_CONNECTIONS]` | `MAX_TCP_CONNECTIONS = 8` | All -1 (set in constructor) |

**Ownership:** `TcpBackend` owns `m_listen_fd` and all `m_client_fds[]`. All file descriptors are closed by `TcpBackend::close()` or `~TcpBackend()`. The destructor calls `TcpBackend::close()` with an explicit qualified call to avoid virtual dispatch in destructors (per code comment).

**Power of 10 Rule 3 confirmation:** No dynamic allocation. All socket fds are stored in fixed member arrays. `pollfd pfds[]` is stack-local.

---

## 9. Error Handling Flow

| Error | Trigger | State after | Action |
|-------|---------|-------------|--------|
| `create_tcp()` fails | `socket()` returns -1 | `m_listen_fd = -1`; `m_open = false` | Log FATAL; return ERR_IO from `bind_and_listen()` and `init()` |
| `set_reuseaddr()` fails | `setsockopt()` returns -1 | fd closed; `m_listen_fd = -1` | Log WARNING_HI; return ERR_IO |
| `do_bind()` fails | `bind()` returns -1 | fd closed; `m_listen_fd = -1` | Log FATAL; return ERR_IO |
| `do_listen()` fails | `listen()` returns -1 | fd closed; `m_listen_fd = -1` | Log FATAL; return ERR_IO |
| `set_nonblocking()` fails | `fcntl()` returns -1 | fd closed; `m_listen_fd = -1` | Log WARNING_HI; return ERR_IO |
| `do_accept()` returns -1 | No pending connection (EAGAIN) | `m_client_count` unchanged | Return OK — not an error |
| `m_client_count >= MAX_TCP_CONNECTIONS` | Array full | No action | Return OK; new connections are refused until a client disconnects |
| `poll()` returns 0 | 100 ms timeout | No state change | Return from `poll_clients_once()`; outer loop retries |
| `poll()` returns -1 | POSIX error | No state change | Return from `poll_clients_once()` (treated same as timeout) |

---

## 10. External Interactions

**During `bind_and_listen()` (initialization):**

- `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` — allocates a kernel socket structure; returns fd.
- `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(int))` — sets socket option.
- `inet_aton(ip, &addr.sin_addr)` — parses IP string to binary. No network I/O.
- `bind(fd, &addr, sizeof(addr))` — registers the local address with the OS.
- `listen(fd, MAX_TCP_CONNECTIONS)` — marks the socket as passive; creates backlog queue.
- `fcntl(fd, F_GETFL, 0)` + `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` — sets non-blocking mode.

**During `accept_clients()` (steady-state):**

- `poll(pfds, nfds, 100)` — waits up to 100 ms for any fd to become readable.
- `accept(listen_fd, nullptr, nullptr)` — accepts a pending connection; returns new client fd. In non-blocking mode, returns immediately with errno=EAGAIN if no connection is pending.

All fd values produced by these calls are stored in `m_client_fds[]` or `m_listen_fd`. All are closed by `TcpBackend::close()`.

---

## 11. State Changes / Side Effects

**After `bind_and_listen()` succeeds:**

| Member | Before | After |
|--------|--------|-------|
| `m_listen_fd` | `-1` | OS-assigned fd (>= 0) |
| `m_open` | `false` | `true` |
| `m_is_server` | `false` | `true` |

**After each successful `accept_clients()` call:**

| Member | Before | After |
|--------|--------|-------|
| `m_client_fds[m_client_count]` | `-1` | New client fd (>= 0) |
| `m_client_count` | N | N + 1 (max 8) |

**OS-level side effects:**

- `bind()` reserves the port in the OS for the duration of `m_listen_fd` lifetime.
- `listen()` creates a kernel backlog queue for the port.
- `SO_REUSEADDR` allows re-binding the same port immediately after a previous process holding it enters TIME_WAIT.
- Each `accept()` creates a new connected socket fd in the kernel and removes the corresponding SYN from the backlog queue.

---

## 12. Sequence Diagram

```
Application       TcpBackend           ISocketOps(impl)      OS Kernel
  |                    |                      |                   |
  |--init(config)----->|                      |                   |
  |  (is_server=true)  |--create_tcp()------->|                   |
  |                    |                      |--socket()-------->|
  |                    |                      |<--fd=5------------|
  |                    |<--5------------------|                   |
  |                    |--set_reuseaddr(5)--->|                   |
  |                    |                      |--setsockopt()---->|
  |                    |                      |<--0---------------|
  |                    |--do_bind(5,ip,port)->|                   |
  |                    |                      |--inet_aton()      |
  |                    |                      |--bind()---------->|
  |                    |                      |<--0---------------|
  |                    |--do_listen(5, 8)---->|                   |
  |                    |                      |--listen()-------->|
  |                    |                      |<--0---------------|
  |                    |--set_nonblocking(5)->|                   |
  |                    |                      |--fcntl(GETFL)---->|
  |                    |                      |--fcntl(SETFL)---->|
  |                    |  m_open=true         |                   |
  |<--OK---------------|  log "listening"     |                   |
  |                                           |                   |
  | [Client connects externally]              |                   |
  |                                           |   TCP SYN         |
  |                                           |<------------------| (kernel queues)
  |                                           |                   |
  |--receive_message()->|                     |                   |
  |                    |--poll_clients_once()->|                   |
  |                    |  [build pfds:         |                   |
  |                    |   pfds[0]=listen_fd]  |                   |
  |                    |--poll(pfds,1,100ms)-->|                   |
  |                    |  (POLLIN on fd 5)     |                   |
  |                    |--accept_clients()     |                   |
  |                    |--do_accept(5)------->|                   |
  |                    |                      |--accept()-------->|
  |                    |                      |<--client_fd=6-----|
  |                    |  m_client_fds[0]=6   |                   |
  |                    |  m_client_count=1     |                   |
  |                    |  log "Accepted"       |                   |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (one-time):**

- `bind_and_listen()` runs exactly once per `TcpBackend` lifetime, during `init()`.
- The listening socket is created, configured, bound, and placed in listen state. Non-blocking mode is set.
- `m_open` transitions from `false` to `true`.

**Steady-state runtime:**

- `accept_clients()` is called on every `poll_clients_once()` invocation as long as: (a) `m_is_server` is true, (b) `m_listen_fd >= 0`, and (c) `m_client_count < MAX_TCP_CONNECTIONS`.
- When the client array is full (`m_client_count == 8`), `has_listen` is `false` and the listen fd is excluded from the `poll()` call. New connections pile up in the OS backlog until a slot frees.
- Clients are removed from `m_client_fds[]` when their connection breaks (detected in `recv_from_client()` → `remove_client_fd()`), freeing space for new `accept_clients()` calls.

---

## 14. Known Risks / Observations

- **`MAX_TCP_CONNECTIONS = 8` hard ceiling.** The server accepts at most 8 simultaneous clients. New connections after this limit are not accepted until a slot is freed. The OS backlog (also `MAX_TCP_CONNECTIONS`) determines how many can queue. Connections beyond the backlog are refused with RST.

- **No client address stored.** `accept(listen_fd, nullptr, nullptr)` does not capture the client's IP or port. There is no per-client tracking beyond the fd number. [ASSUMPTION: the application does not need to know which client sent which message.]

- **Single-threaded accept.** `accept_clients()` accepts at most one new client per `poll_clients_once()` invocation. If multiple clients connect simultaneously, they are accepted one per `receive_message()` call, potentially over several iterations.

- **Non-blocking accept can log spurious WARNING_LO.** `socket_accept()` in `SocketUtils.cpp` calls `Logger::log(WARNING_LO, ...)` when `accept()` returns -1, even when the reason is EAGAIN (no pending connection). This produces noisy logs during normal poll-with-no-clients operation. [ASSUMPTION: this is a minor observability issue, not a correctness problem.]

- **`m_listen_fd` not added to `m_client_fds[]`.** The listen fd is tracked separately. Only connected client fds go into `m_client_fds[]`. This is correct TCP server design but means `close()` must handle `m_listen_fd` separately, which it does.

- **`set_nonblocking()` only applied to `m_listen_fd`.** Accepted client fds are returned from `accept()` in blocking mode. Client I/O uses `poll()` + `recv_frame()` to avoid blocking indefinitely. [ASSUMPTION: client fds are left in blocking mode intentionally; `recv_frame()` uses `poll()` with timeout before each `recv()`.]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The OS backlog for `listen()` is set to `MAX_TCP_CONNECTIONS` (8). The actual kernel backlog may be higher due to system limits (SOMAXCONN), but this is the requested value.
- [ASSUMPTION] `socket_accept()` in SocketUtils logs WARNING_LO on EAGAIN. This is technically a misclassification of a normal non-blocking condition, but no code change is traced here.
- [ASSUMPTION] Accepted client fds are in blocking mode. The `set_nonblocking()` call is applied only to `m_listen_fd`, not to fds returned by `do_accept()`.
- [ASSUMPTION] `inet_aton()` is used for IP parsing. This function does not support IPv6. REQ-6.3.1 mentions optional IPv6 support; the current implementation is IPv4 only.
- [ASSUMPTION] `config.bind_ip` is a valid null-terminated C string. No bounds check is performed on the string length before `inet_aton()`.
- [ASSUMPTION] The `poll_clients_once()` `pfds` array includes at most `MAX_TCP_CONNECTIONS + 1 = 9` entries. The `static const uint32_t MAX_POLL_FDS = MAX_TCP_CONNECTIONS + 1U` is a local constant, not a global compile-time constant. Changing `MAX_TCP_CONNECTIONS` automatically adjusts `MAX_POLL_FDS`.
