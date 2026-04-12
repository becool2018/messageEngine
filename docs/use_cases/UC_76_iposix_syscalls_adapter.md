# UC_76 — IPosixSyscalls / PosixSyscallsImpl: injectable POSIX syscall adapter for SocketUtils fault injection

**HL Group:** System Internals (sub-functions, not user-facing goals)
**Actor:** System (internal — production code uses singleton `PosixSyscallsImpl`; tests inject `MockPosixSyscalls`)
**Requirement traceability:** REQ-6.3.1, REQ-6.3.2, REQ-6.3.3

---

## 1. Use Case Overview

- **Trigger:** Any call through `SocketUtils.cpp` (socket creation, connect, poll, send,
  recv, etc.) dispatches through `IPosixSyscalls` virtual methods rather than calling the
  POSIX kernel API directly.
- **Goal:** Make `SocketUtils.cpp` testable under fault conditions by allowing a mock
  `IPosixSyscalls` to force error returns (e.g., `socket()` returning -1, `connect()` setting
  `ECONNREFUSED`) that cannot be reproduced on a loopback test environment without kernel
  fault injection.
- **Production path:** `PosixSyscallsImpl` singleton forwards each call to the real POSIX API
  with no additional logic (`socket()`, `fcntl()`, `connect()`, `poll()`, `send()`,
  `sendto()`, `recvfrom()`, `inet_ntop()`).
- **Test path:** A `MockPosixSyscalls` instance is injected via `SocketUtils::set_syscalls()`
  before the test. Mock methods return preset error codes to exercise otherwise-unreachable
  error branches in `SocketUtils.cpp`.

---

## 2. Entry Points

```cpp
// src/platform/IPosixSyscalls.hpp — pure-virtual interface
// src/platform/PosixSyscallsImpl.hpp/.cpp — production singleton
// Production: SocketUtils uses the singleton by default
// Test: SocketUtils::set_syscalls(&mock) to inject mock
```

---

## 3. End-to-End Control Flow

**Production path:**

1. `SocketUtils::create_tcp_socket()` (or similar) calls
   `g_syscalls->socket(AF_INET, SOCK_STREAM, 0)`.
2. `g_syscalls` points to the `PosixSyscallsImpl` singleton (set at startup or by default).
3. `PosixSyscallsImpl::socket()` calls `::socket(AF_INET, SOCK_STREAM, 0)` and returns the fd.
4. `SocketUtils` checks the return value; on -1 logs at WARNING_HI and returns `ERR_IO`.

**Test path (fault injection):**

1. Test calls `SocketUtils::set_syscalls(&mock_syscalls)`.
2. Test configures `mock_syscalls.socket_return = -1` and `mock_syscalls.socket_errno = EMFILE`.
3. Test calls the production code under test (`SocketUtils::create_tcp_socket()`).
4. `g_syscalls->socket()` dispatches to `MockPosixSyscalls::socket()`, returns -1, sets errno.
5. `SocketUtils` error-handling branch (previously unreachable on loopback) is now exercised.
6. Test verifies WARNING_HI was logged and `ERR_IO` was returned.

---

## 4. Call Tree

```
SocketUtils::create_tcp_socket()   [or connect, poll, send, etc.]
 └── g_syscalls->socket(...)        [IPosixSyscalls virtual dispatch]
      ├── PosixSyscallsImpl::socket() -> ::socket()    [production]
      └── MockPosixSyscalls::socket() -> preset error  [test]
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| `IPosixSyscalls` | Pure-virtual interface; 8 methods: `socket`, `fcntl`, `connect`, `poll`, `send`, `sendto`, `recvfrom`, `inet_ntop` |
| `PosixSyscallsImpl` | Production singleton; thin forwarding wrappers to POSIX API; no logic |
| `MockPosixSyscalls` (tests only) | Configurable stubs that return preset error codes |
| `SocketUtils::set_syscalls()` | Test hook to swap `g_syscalls`; must be reset after each test |

---

## 6. Branching Logic / Decision Points

All branching logic lives in `SocketUtils.cpp` callers, not in `IPosixSyscalls`. The interface
is a pure forwarding abstraction with no policy.

---

## 7. Concurrency / Threading Behavior

`g_syscalls` is a global pointer in `SocketUtils.cpp`. It is set once at startup (production)
or in the test harness before the code under test runs. Concurrent `set_syscalls()` and
socket operations are not supported — tests must be single-threaded or use a fixed injection
before spawning threads.

---

## 8. Memory & Ownership Semantics

`PosixSyscallsImpl` is a singleton (static storage); `MockPosixSyscalls` is typically
stack-allocated in the test fixture and injected via raw pointer. `SocketUtils` does not
own the `IPosixSyscalls` object and must not delete it.

---

## 9. Error Handling Flow

All error handling is in `SocketUtils.cpp` callers. `IPosixSyscalls` methods return the
raw syscall return value and set `errno`; they do not log or map errors.

---

## 10. External Interactions

- **Production:** POSIX kernel API (`socket()`, `fcntl()`, `connect()`, `poll()`, `send()`,
  `sendto()`, `recvfrom()`, `inet_ntop()`).
- **Test:** `MockPosixSyscalls` — no kernel interaction; preset return values only.

---

## 11. State Changes / Side Effects

`PosixSyscallsImpl` is stateless. `MockPosixSyscalls` tracks call counts and preset return
values for test verification.

---

## 12. Sequence Diagram

```
[Production]
SocketUtils::connect_with_timeout()
  -> g_syscalls->connect(fd, &addr, len)
       -> PosixSyscallsImpl::connect()
            -> ::connect(fd, &addr, len)   [POSIX kernel]
  <- fd or -1

[Test]
test_socket_create_fail():
  SocketUtils::set_syscalls(&mock);         [inject mock]
  mock.socket_return = -1; mock.errno = EMFILE;
  SocketUtils::create_tcp_socket()
    -> g_syscalls->socket(...)
         -> MockPosixSyscalls::socket()  -> returns -1
    -> logs WARNING_HI; returns ERR_IO
  VERIFY(result == ERR_IO);
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** `PosixSyscallsImpl::instance()` is available from program start; no
  explicit init required. `SocketUtils` uses it by default.
- **Runtime:** Every socket syscall in `SocketUtils.cpp` goes through `g_syscalls`. In
  production this is a single virtual dispatch (one vtable lookup) per syscall — negligible overhead.

---

## 14. Known Risks / Observations

- **Pattern identical to `ISocketOps` and `IMbedtlsOps`:** All three injectable adapters
  follow the same design (UC_57, UC_58, UC_76). `IPosixSyscalls` covers the 8 POSIX syscalls
  used inside `SocketUtils.cpp` that are architecturally unreachable on loopback without injection.
- **Test isolation:** Failure to reset `g_syscalls` after a test that injects a mock will cause
  subsequent tests (or production code initialised in the same process) to use the mock.
  Test fixtures must restore `g_syscalls` in teardown.

---

## 15. Unknowns / Assumptions

- The 8 POSIX methods covered by `IPosixSyscalls` are the complete set used in `SocketUtils.cpp`
  at the time of writing. Any new POSIX call added to `SocketUtils.cpp` that is unreachable
  on loopback must be added to the interface.
- `PosixSyscallsImpl` is a singleton; `&PosixSyscallsImpl::instance()` is the default value
  of `g_syscalls`.
