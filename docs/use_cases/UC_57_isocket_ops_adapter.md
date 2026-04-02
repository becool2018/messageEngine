# UC_57 — ISocketOps / SocketOpsImpl: injectable POSIX socket adapter for TcpBackend, UdpBackend, TlsTcpBackend, DtlsUdpBackend

**HL Group:** System Internals
**Actor:** System
**Requirement traceability:** REQ-6.3.1, REQ-6.3.2, REQ-6.3.3, REQ-4.1.1

This is a System Internal use case. `ISocketOps` is the injectable socket interface that all backends use in place of direct POSIX calls. `SocketOpsImpl` is the production implementation. Test code injects a mock to simulate socket failures. The User never calls these directly.

---

## 1. Use Case Overview

- **Trigger:** A backend (`TcpBackend`, `UdpBackend`, etc.) is constructed with an `ISocketOps*` parameter. At runtime, every POSIX socket operation is dispatched through this interface.
- **Goal:** Decouple all backends from direct POSIX socket calls, enabling full socket-failure fault injection in unit tests without real network hardware.
- **Success outcome (production):** `SocketOpsImpl` is injected; all methods delegate directly to the corresponding POSIX calls (`::socket()`, `::bind()`, `::connect()`, `::send()`, `::recv()`, `::close()`, etc.).
- **Error outcomes:** Each method returns the POSIX return value (file descriptor on success, -1 on error). Backends check all return values per Power of 10 Rule 7.

---

## 2. Entry Points

```cpp
// src/platform/ISocketOps.hpp
class ISocketOps {
    virtual int  create_tcp() = 0;
    virtual int  create_udp() = 0;
    virtual int  bind_socket(int fd, const char* ip, uint16_t port) = 0;
    virtual int  set_nonblocking(int fd) = 0;
    virtual int  set_reuseaddr(int fd) = 0;
    virtual int  do_listen(int fd, int backlog) = 0;
    virtual int  do_accept(int fd) = 0;
    virtual int  connect_with_timeout(int fd, const char* ip, uint16_t port, uint32_t timeout_ms) = 0;
    virtual ssize_t do_send(int fd, const void* buf, size_t len) = 0;
    virtual ssize_t do_recv(int fd, void* buf, size_t len) = 0;
    virtual int  do_close(int fd) = 0;
    virtual int  do_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) = 0;
};

// src/platform/SocketOpsImpl.hpp
class SocketOpsImpl : public ISocketOps { /* delegates to POSIX */ };
```

---

## 3. End-to-End Control Flow

**Production path (SocketOpsImpl):**
Each method is a one-line delegation:
- `create_tcp()`: `return ::socket(AF_INET, SOCK_STREAM, 0)`.
- `bind_socket(fd, ip, port)`: `return ::bind(fd, ...)`.
- `connect_with_timeout(fd, ip, port, timeout_ms)`: implements non-blocking connect + `::poll(POLLOUT, timeout_ms)` + `getsockopt(SO_ERROR)` — see UC_35.
- `do_send(fd, buf, len)`: `return ::send(fd, buf, len, MSG_NOSIGNAL)`.
- `do_recv(fd, buf, len)`: `return ::recv(fd, buf, len, 0)`.
- `do_close(fd)`: `return ::close(fd)`.
- `do_poll(fds, nfds, timeout_ms)`: `return ::poll(fds, nfds, timeout_ms)`.

**Test path (MockSocketOps / StubSocketOps):**
- Any method can return -1 with a configured `errno` to simulate `ECONNREFUSED`, `ETIMEDOUT`, `EADDRINUSE`, etc.
- Enables branch coverage of all error paths in backends without a real network.

---

## 4. Call Tree

```
TcpBackend::connect_to_server()                    [TcpBackend.cpp]
 └── m_sock_ops->create_tcp()                      [ISocketOps vtable]
      └── SocketOpsImpl::create_tcp()              [production]
           └── ::socket(AF_INET, SOCK_STREAM, 0)   [POSIX]

TcpBackend::bind_and_listen()                      [TcpBackend.cpp]
 ├── m_sock_ops->create_tcp()
 ├── m_sock_ops->set_reuseaddr()
 ├── m_sock_ops->bind_socket()
 ├── m_sock_ops->set_nonblocking()
 └── m_sock_ops->do_listen()
```

---

## 5. Key Components Involved

- **`ISocketOps`** — pure-virtual interface; the only socket-calling point in backend code. No backend contains a direct POSIX call.
- **`SocketOpsImpl`** — production singleton; delegates each virtual method to the corresponding POSIX call with minimal wrapping.
- **`connect_with_timeout()`** — the most complex method; implements the non-blocking connect + poll + SO_ERROR pattern (see UC_35).
- **Virtual dispatch** — Rule 9 exception; vtable-backed virtual functions are the approved mechanism for this polymorphism.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `ISocketOps*` is `SocketOpsImpl` | POSIX calls | Mock returns configured result |
| POSIX call returns -1 | Caller logs + returns ERR_IO | Caller continues |

---

## 7. Concurrency / Threading Behavior

- POSIX socket calls are thread-safe per POSIX standard.
- `SocketOpsImpl` has no mutable state; it is a stateless wrapper.
- Mock implementations used in tests are single-threaded.
- No `std::atomic` operations in this interface.

---

## 8. Memory & Ownership Semantics

- `SocketOpsImpl` — stateless; may be a singleton or a stack-local object.
- The `ISocketOps*` pointer is injected at construction time; the backend does not own the `ISocketOps` object.
- No heap allocation in `SocketOpsImpl` methods. Power of 10 Rule 3 compliant.

---

## 9. Error Handling Flow

- Each `ISocketOps` method returns the POSIX return value (typically -1 on error with `errno` set).
- All callers check return values (Power of 10 Rule 7); on -1: log WARNING_LO or WARNING_HI and return `Result::ERR_IO`.
- No silent discard of errors.

---

## 10. External Interactions

- **POSIX:** `::socket()`, `::bind()`, `::listen()`, `::accept()`, `::connect()`, `::send()`, `::recv()`, `::close()`, `::poll()`, `::getsockopt()` — all called through `SocketOpsImpl`.

---

## 11. State Changes / Side Effects

`SocketOpsImpl` is stateless; all side effects are in the POSIX kernel (socket table, file descriptor table). The calling backend tracks the file descriptors returned by `create_tcp()` etc.

---

## 12. Sequence Diagram

```
[TcpBackend constructor]
  TcpBackend(ISocketOps* ops) : m_sock_ops(ops) {}

[TcpBackend::init(config) — production]
  -> m_sock_ops->create_tcp()                      [ISocketOps vtable]
       -> SocketOpsImpl::create_tcp()
            -> ::socket(AF_INET, SOCK_STREAM, 0)
            <- fd=5
  [m_client_fds[0] = 5]

[TcpBackend::init(config) — test with mock]
  -> m_sock_ops->create_tcp()                      [ISocketOps vtable]
       -> MockSocketOps::create_tcp()              [returns -1, errno=ENOBUFS]
  [TcpBackend returns ERR_IO — branch covered]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- The `ISocketOps*` must be injected at backend construction time.
- For production: `SocketOpsImpl::instance()` or a stack-allocated `SocketOpsImpl`.
- For tests: a mock object configured with the desired failure sequence.

**Runtime:**
- `ISocketOps` methods are called on every connection event, send, receive, poll, and close operation.

---

## 14. Known Risks / Observations

- **Vtable overhead:** Each socket call incurs one virtual dispatch (one indirect function call). For high-frequency send/receive, this is negligible compared to the syscall latency.
- **Mock state management in tests:** Test mocks must be carefully sequenced to return the right error at the right call number. Misconfigured mocks can produce misleading test results.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `SocketOpsImpl` is implemented as a stateless class with all methods as simple one-line wrappers; no caching or state beyond the POSIX file descriptors.
- `[ASSUMPTION]` `connect_with_timeout()` is the only non-trivial `ISocketOps` method; all others are direct POSIX delegations.
- `[ASSUMPTION]` The backends store `ISocketOps*` as a member named `m_sock_ops`; exact name inferred from TcpBackend.cpp / UdpBackend.cpp patterns.
