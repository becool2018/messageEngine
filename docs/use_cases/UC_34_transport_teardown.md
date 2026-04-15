# UC_34 — Transport teardown: flush impairment delay buffer, close all fds, reset state

**HL Group:** HL-9 — Close a Transport
**Actor:** User
**Requirement traceability:** REQ-4.1.4, REQ-6.1.3, REQ-7.1.1

---

## 1. Use Case Overview

- **Trigger:** User calls `TcpBackend::close()` (or `UdpBackend::close()`, `LocalSimHarness::close()`). File: `src/platform/TcpBackend.cpp`.
- **Goal:** Gracefully flush any pending impairment-delayed messages, close all socket file descriptors (listen + all client fds), and reset the transport to an uninitialized state.
- **Success outcome:** All fds closed. `m_open == false`. `m_client_count == 0`. The object may be re-initialized via `init()`.
- **Error outcomes:** None returned (function is void). Socket close failures are logged at WARNING_LO.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp
void TcpBackend::close() override;
```

Called by the User or at destruction (destructor calls `close()` if `m_open`).

---

## 3. End-to-End Control Flow

1. **`TcpBackend::close()`** — entry.
2. `if (!m_open) { return; }` — idempotent: no-op if already closed.
3. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
4. **Flush pending delayed messages:**
   - `uint64_t now_us = timestamp_now_us()`.
   - `flush_delayed_to_clients(now_us)` — drains any impairment-delayed outbound envelopes that are now due.
5. **Close client sockets:**
   - Bounded loop over `i` in `0..m_client_count-1`:
     - `m_sock_ops->do_close(m_client_fds[i])` → `::close(fd)`. If error: log WARNING_LO.
     - `m_client_fds[i] = -1`.
   - `m_client_count = 0`.
6. **Close listen socket (server only):**
   - If `m_is_server && m_listen_fd >= 0`: `m_sock_ops->do_close(m_listen_fd)`. `m_listen_fd = -1`.
7. `m_open = false`.
8. `LOG_INFO("TcpBackend", "Closed")`.

---

## 4. Call Tree

```
TcpBackend::close()                          [TcpBackend.cpp]
 ├── flush_delayed_to_clients(now_us)        [TcpBackend.cpp]
 │    └── [send any due delayed envelopes -> ::send()]
 ├── [loop: close all client fds]
 │    └── ISocketOps::do_close() -> ::close() [POSIX]
 └── [close listen fd if server]
      └── ISocketOps::do_close() -> ::close()
```

---

## 5. Key Components Involved

- **`flush_delayed_to_clients()`** — Ensures any buffered impairment-delayed messages are sent before the socket is closed. Prevents silent loss of already-queued messages.
- **`ISocketOps::do_close()`** — Wraps `::close(fd)`. Logs WARNING_LO on failure (e.g., fd already closed).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `!m_open` | Return immediately | Proceed with teardown |
| `m_is_server && m_listen_fd >= 0` | Close listen fd | Skip |
| `do_close()` returns error | Log WARNING_LO | Continue |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread.
- After `close()` returns, `m_open == false`. Concurrent calls to `send_message()` or `receive_message()` will assert-fail (their `NEVER_COMPILED_OUT_ASSERT(m_open)` will fire).

---

## 8. Memory & Ownership Semantics

- All fds are OS resources; released by `::close()`.
- `m_wire_buf`, `m_client_fds`, `m_recv_queue` — fixed members; remain allocated but `m_open=false` prevents use.
- No heap allocation to free.

---

## 9. Error Handling Flow

- Socket close errors (e.g., invalid fd): logged at WARNING_LO; teardown continues. All fds are processed regardless.
- After `close()`, the object is safe to `init()` again for re-use.

---

## 10. External Interactions

- **POSIX `::close()`:** Called on each client fd and optionally the listen fd.
- **`stderr`:** Logger on INFO close + WARNING_LO on close error.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `TcpBackend` | `m_open` | true | false |
| `TcpBackend` | `m_client_fds[i]` | valid fds | -1 |
| `TcpBackend` | `m_client_count` | N | 0 |
| `TcpBackend` | `m_listen_fd` (server) | valid fd | -1 |

---

## 12. Sequence Diagram

```
User
  -> TcpBackend::close()
       -> flush_delayed_to_clients(now_us)   [send pending delayed messages]
       [loop: close all client fds]
            -> ISocketOps::do_close(fd)      [::close()]
       -> ISocketOps::do_close(m_listen_fd)  [server only]
       -> LOG_INFO("Closed")
       [m_open = false]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TcpBackend::init()` has been called; `m_open == true`.

**Runtime:**
- `close()` is idempotent. Calling it multiple times is safe; the second call returns immediately.

---

## 14. Known Risks / Observations

- **Undelivered delayed messages:** `flush_delayed_to_clients()` sends messages that are due at the time of `close()`. Messages with `deliver_time > now_us` are still in the delay buffer and are silently lost at teardown.
- **Destructor calls `close()`:** If `TcpBackend` goes out of scope without an explicit `close()`, the destructor handles cleanup. However, the destructor's `close()` call happens in the destructor context and should not be relied upon for deterministic teardown in production code.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `TcpBackend::~TcpBackend()` calls `close()` if `m_open`. This is a standard RAII pattern but is inferred; the exact destructor behavior is confirmed by reading TcpBackend.cpp.
