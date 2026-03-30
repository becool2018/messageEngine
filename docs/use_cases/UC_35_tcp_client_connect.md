# UC_35 — TCP client connect

**HL Group:** HL-8 — User starts a client endpoint
**Actor:** User
**Requirement traceability:** REQ-4.1.1, REQ-6.1.1, REQ-6.1.2, REQ-6.3.1, REQ-6.3.3, REQ-7.1.1

---

## 1. Use Case Overview

### Clear description of what triggers this flow

The User calls `TcpBackend::init()` with a `TransportConfig` where `config.is_server == false` and `config.kind == TransportKind::TCP`. This initiates outbound TCP connection setup: socket creation, `SO_REUSEADDR`, non-blocking mode, non-blocking `connect()` with `poll`-based timeout, and `getsockopt(SO_ERROR)` confirmation. On success the connected socket fd is stored in `m_client_fds[0]`.

### Expected outcome (single goal)

`m_client_fds[0]` holds a valid, connected, non-blocking TCP socket fd. `m_client_count == 1`. `m_open == true`. `Result::OK` is returned to the caller. The OS socket remains in `O_NONBLOCK` mode after the connect completes.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

**Primary entry point:**
`TcpBackend::init(const TransportConfig& config)` — `src/platform/TcpBackend.cpp`, line 88.
Precondition assertions (lines 90–91):
- `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP)`
- `NEVER_COMPILED_OUT_ASSERT(!m_open)`

**Client-mode sub-entry point:**
`TcpBackend::connect_to_server()` — `TcpBackend.cpp`, line 124.
Called from `init()` at line 110 when `m_is_server == false`.
Precondition assertions (lines 126–127):
- `NEVER_COMPILED_OUT_ASSERT(!m_is_server)`
- `NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] == -1)`

**Socket-level timeout implementation:**
`socket_connect_with_timeout(fd, ip, port, timeout_ms)` — `src/platform/SocketUtils.cpp`, line 157.
Called from `connect_to_server()` at TcpBackend.cpp:143.

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase 1: TcpBackend::init() — common setup

1. Caller constructs `TcpBackend`. Constructor (TcpBackend.cpp:28):
   - `m_listen_fd = -1`, `m_client_count = 0U`, `m_wire_buf{}`, `m_cfg{}`, `m_open = false`, `m_is_server = false`.
   - `NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U)` (line 33).
   - Loop `i = 0..7`: `m_client_fds[i] = -1` (lines 34–36).

2. Caller invokes `TcpBackend::init(config)` with `config.is_server == false`.
   - Line 90: `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP)` — passes.
   - Line 91: `NEVER_COMPILED_OUT_ASSERT(!m_open)` — passes.
   - Line 93: `m_cfg = config` — full struct copy (includes `peer_ip`, `peer_port`, `connect_timeout_ms`).
   - Line 94: `m_is_server = false`.

3. `m_recv_queue.init()` (line 97) → `RingBuffer::init()`:
   - `m_head.store(0U, relaxed)`; `m_tail.store(0U, relaxed)`.

4. ImpairmentConfig initialization (lines 99–104):
   - `impairment_config_default(imp_cfg)` fills safe defaults.
   - If `config.num_channels > 0U`: `imp_cfg.enabled = config.channels[0U].impairments_enabled`.
   - `m_impairment.init(imp_cfg)` → seed PRNG; zero delay/reorder buffers; `m_initialized = true`.

5. Branch: `m_is_server == false` → client path (line 110): `connect_to_server()`.

### Phase 2: connect_to_server() — outbound connection

6. **`connect_to_server()` entry (TcpBackend.cpp:124)**
   - `NEVER_COMPILED_OUT_ASSERT(!m_is_server)` (line 126).
   - `NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] == -1)` (line 127).

7. **`socket_create_tcp()` (line 129) → SocketUtils.cpp:28:**
   - syscall: `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`.
   - Returns `fd >= 0` or `-1`.
   - If `fd < 0`: `Logger::log(FATAL, "TcpBackend", "socket_create_tcp failed in client mode")`; return `ERR_IO`.

8. **`socket_set_reuseaddr(fd)` (line 136) → SocketUtils.cpp:97:**
   - syscall: `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(int))`.
   - If false: `Logger::log(WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed")`; `socket_close(fd)`; return `ERR_IO`.

9. **`socket_connect_with_timeout(fd, m_cfg.peer_ip, m_cfg.peer_port, m_cfg.connect_timeout_ms)` (line 143) → SocketUtils.cpp:157:**

   **9a. `socket_set_nonblocking(fd)` (SocketUtils.cpp:165)**
   - `NEVER_COMPILED_OUT_ASSERT(fd >= 0)`.
   - `fcntl(fd, F_GETFL, 0)` — get current flags.
   - If `flags < 0` → log `WARNING_LO`; return false.
   - `flags |= O_NONBLOCK`; `fcntl(fd, F_SETFL, flags)`.
   - If `result < 0` → log `WARNING_LO`; return false.
   - `NEVER_COMPILED_OUT_ASSERT(result == 0)`. Returns `true`.
   - If `socket_set_nonblocking` returns false → `socket_connect_with_timeout` returns false.

   **9b. Build `sockaddr_in`:**
   - `memset(&addr, 0, sizeof(addr))`.
   - `addr.sin_family = AF_INET`.
   - `addr.sin_port = htons(port)`.
   - `inet_aton(ip, &addr.sin_addr)` — if returns 0: log `WARNING_LO`; return false.

   **9c. Non-blocking `connect()` (SocketUtils.cpp:183):**
   - syscall: `connect(fd, (sockaddr*)&addr, sizeof(addr))`.
   - If `conn_result == 0`: immediate success (localhost or already connected); `NEVER_COMPILED_OUT_ASSERT(conn_result == 0)`; return true.
   - If `errno != EINPROGRESS` (unexpected error): log `WARNING_LO "connect(%s:%u) failed: %d"`; return false.
   - Expected case: `errno == EINPROGRESS` → connection handshake in progress.

   **9d. `poll()` waiting for writability (SocketUtils.cpp:199–209):**
   - `pfd.fd = fd`; `pfd.events = POLLOUT`; `pfd.revents = 0`.
   - syscall: `poll(&pfd, 1, (int)timeout_ms)`.
   - If `poll_result <= 0` (timeout or error): log `WARNING_LO "connect(%s:%u) timeout"`; return false.
   - If `poll_result > 0`: socket is writable — but this only means the connect attempt completed, not that it succeeded.

   **9e. `getsockopt(SO_ERROR)` — confirm success (SocketUtils.cpp:212–220):**
   - `getsockopt(fd, SOL_SOCKET, SO_ERROR, &opt_err, &opt_len)`.
   - If `getsock_result < 0` or `opt_err != 0`: log `WARNING_LO "connect(%s:%u) failed after poll: %d"`; return false.
   - `NEVER_COMPILED_OUT_ASSERT(getsock_result == 0 && opt_err == 0)`.
   - Returns `true` — connection is in ESTABLISHED state.

   **If `socket_connect_with_timeout` returns false:**
   - `connect_to_server()` (line 143): logs `WARNING_HI "Connection to %s:%u failed"`; `socket_close(fd)`; return `ERR_IO`.

10. **Store result (connect_to_server() lines 151–153):**
    - `m_client_fds[0U] = fd`.
    - `m_client_count = 1U`.
    - `m_open = true`.

11. **Log and postconditions (connect_to_server() lines 155–160):**
    - `Logger::log(INFO, "TcpBackend", "Connected to %s:%u", m_cfg.peer_ip, m_cfg.peer_port)`.
    - `NEVER_COMPILED_OUT_ASSERT(m_client_fds[0U] >= 0)` — postcondition.
    - `NEVER_COMPILED_OUT_ASSERT(m_client_count == 1U)` — postcondition.
    - Returns `Result::OK`.

12. **Back in `init()` (line 116):**
    - `NEVER_COMPILED_OUT_ASSERT(m_open)` — postcondition.
    - Returns `Result::OK`.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::init(config)                                     [TcpBackend.cpp:88]
├── NEVER_COMPILED_OUT_ASSERT(config.kind==TCP, !m_open)
├── m_cfg = config
├── m_recv_queue.init()                                      [RingBuffer.hpp]
│   ├── m_head.store(0U, relaxed)
│   └── m_tail.store(0U, relaxed)
├── impairment_config_default(imp_cfg)                       [ImpairmentConfig.hpp inline]
├── m_impairment.init(imp_cfg)                               [ImpairmentEngine.cpp:44]
│   ├── m_prng.seed(seed)
│   ├── memset(m_delay_buf, 0, ...)
│   └── memset(m_reorder_buf, 0, ...)
└── connect_to_server()                                      [TcpBackend.cpp:124]
    ├── socket_create_tcp()                                  [SocketUtils.cpp:28]
    │   └── socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)        [POSIX syscall]
    ├── socket_set_reuseaddr(fd)                             [SocketUtils.cpp:97]
    │   └── setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)    [POSIX syscall]
    └── socket_connect_with_timeout(fd, ip, port, timeout)  [SocketUtils.cpp:157]
        ├── socket_set_nonblocking(fd)                       [SocketUtils.cpp:66]
        │   ├── fcntl(fd, F_GETFL, 0)                        [POSIX syscall]
        │   └── fcntl(fd, F_SETFL, flags|O_NONBLOCK)         [POSIX syscall]
        ├── inet_aton(ip, &addr.sin_addr)                    [POSIX]
        ├── connect(fd, &addr, sizeof(addr))                 [POSIX syscall → EINPROGRESS]
        ├── poll(&pfd, 1, timeout_ms)                        [POSIX syscall]
        │   (waits for POLLOUT on fd)
        └── getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)        [POSIX syscall]
            (confirms connection success)
```

---

## 5. Key Components Involved

| Component | File / Location | Role in this flow |
|---|---|---|
| `TcpBackend` | `src/platform/TcpBackend.cpp/.hpp` | Orchestrates TCP client lifecycle; owns `m_client_fds[0]`, `m_client_count`, `m_open`. |
| `TcpBackend::connect_to_server()` | `TcpBackend.cpp:124` | Private helper: creates socket, calls `socket_connect_with_timeout()`, stores resulting fd. |
| `socket_connect_with_timeout()` | `src/platform/SocketUtils.cpp:157` | Sets `O_NONBLOCK`, issues `connect()`, polls for writability with timeout, confirms via `getsockopt(SO_ERROR)`. |
| `socket_set_nonblocking()` | `src/platform/SocketUtils.cpp:66` | Sets `O_NONBLOCK` on the fd via `fcntl(F_SETFL)`. |
| `RingBuffer` | `src/core/RingBuffer.hpp` | Receive queue; zero-initialized during `init()`. |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.cpp` | Initialized during `init()`; not exercised during connect. |
| `SocketUtils` | `src/platform/SocketUtils.cpp/.hpp` | All raw POSIX socket operations. |
| `TransportConfig` | `src/core/ChannelConfig.hpp` | Carries `peer_ip`, `peer_port`, `connect_timeout_ms`, `is_server`. |
| `Types.hpp` | `src/core/Types.hpp` | `MAX_TCP_CONNECTIONS = 8U`, `SOCKET_RECV_BUF_BYTES = 8192U`. |
| `Logger` | `src/core/Logger.hpp` | Emits INFO on success; WARNING_HI or FATAL on failures. |

---

## 6. Branching Logic / Decision Points

**Decision 1: config.is_server (TcpBackend.cpp:107)**
- `false` → client path: `connect_to_server()`.
- `true` → server path: `bind_and_listen()` (not covered in this use case).

**Decision 2: socket_create_tcp() return value (connect_to_server line 130)**
- `fd >= 0` → continue to `socket_set_reuseaddr`.
- `fd < 0` → log `FATAL`; return `ERR_IO` (no fd to close).

**Decision 3: socket_set_reuseaddr() return value (connect_to_server line 136)**
- `true` → continue to `socket_connect_with_timeout`.
- `false` → log `WARNING_HI`; `socket_close(fd)`; return `ERR_IO`.

**Decision 4: socket_set_nonblocking() result (SocketUtils.cpp:165)**
- `true` → proceed to build `sockaddr_in` and call `connect()`.
- `false` → `socket_connect_with_timeout` returns false → `WARNING_HI` in `connect_to_server`; `socket_close`; return `ERR_IO`.

**Decision 5: inet_aton() result (SocketUtils.cpp:175)**
- `aton_result != 0` → IP parsed successfully; proceed.
- `aton_result == 0` → log `WARNING_LO`; return false from `socket_connect_with_timeout`.

**Decision 6: connect() return value (SocketUtils.cpp:184)**
- `conn_result == 0` → immediate success (rare); return true.
- `errno == EINPROGRESS` → expected; proceed to `poll()`.
- `errno != EINPROGRESS` → log `WARNING_LO "connect failed: errno"`; return false.

**Decision 7: poll() result (SocketUtils.cpp:204)**
- `poll_result > 0` → socket writable; proceed to `getsockopt(SO_ERROR)`.
- `poll_result <= 0` (timeout or EINTR) → log `WARNING_LO "connect timeout"`; return false.

**Decision 8: getsockopt(SO_ERROR) result (SocketUtils.cpp:214)**
- `getsock_result >= 0 && opt_err == 0` → connection succeeded; `NEVER_COMPILED_OUT_ASSERT`; return true.
- `getsock_result < 0 || opt_err != 0` → log `WARNING_LO "connect failed after poll: %d"`; return false.

**Decision 9: socket_connect_with_timeout() final result (connect_to_server line 143)**
- `true` → store fd; `m_client_count = 1`; `m_open = true`; log INFO; return `OK`.
- `false` → log `WARNING_HI "Connection to %s:%u failed"`; `socket_close(fd)`; return `ERR_IO`.

**Decision 10: config.num_channels > 0U (init line 101)**
- `true` → `imp_cfg.enabled = config.channels[0U].impairments_enabled`.
- `false` → `imp_cfg.enabled` remains `false` (default).

---

## 7. Concurrency / Threading Behavior

### Threads created

None. `TcpBackend` is single-threaded by design. `init()` including the full connect sequence must be called from the caller's thread.

### Where context switches occur

`connect()` returns immediately with `EINPROGRESS`. The blocking wait is in `poll()` inside `socket_connect_with_timeout()`, which blocks for up to `connect_timeout_ms` milliseconds. This is the only blocking point during init.

### Synchronization primitives

- `RingBuffer` uses `std::atomic<uint32_t>` (`m_head`, `m_tail`) with acquire/release ordering. During `init()`, only relaxed stores are issued.
- `m_client_fds[0]` and `m_client_count` are plain `int` and `uint32_t` — no atomic protection. Access is serialized by single-threaded use.

### Producer/consumer relationships

Not applicable during the init/connect phase. No message I/O occurs.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory

All storage is statically allocated inline in the `TcpBackend` object — no heap:

- `m_client_fds[MAX_TCP_CONNECTIONS]` = `m_client_fds[8]`: plain `int` array. Only `m_client_fds[0]` is used in client mode.
- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` = `m_wire_buf[8192]`: inline `uint8_t` array; not used during init.
- `m_recv_queue` (`RingBuffer`): inline value member; reset by `init()`.
- `m_impairment` (`ImpairmentEngine`): inline value member; reset by `init()`.
- `m_cfg` (`TransportConfig`): full struct copy at line 93.

### Lifetime of key objects

The connected fd (`m_client_fds[0]`) is an OS resource owned by `TcpBackend` from the successful return of `socket_connect_with_timeout()` until `close()` or destructor.

### Stack vs heap usage

No heap. All objects are inline within the `TcpBackend` instance. The `sockaddr_in addr` and `pollfd pfd` are stack locals in `socket_connect_with_timeout()`.

### RAII usage

`TcpBackend::~TcpBackend()` calls `TcpBackend::close()` (qualified, line 47) to ensure fd release. If `connect_to_server()` fails mid-way, the partially created fd is explicitly closed before return (`socket_close(fd)` at TcpBackend.cpp:140 and 148). No fd leak on any failure path.

### Potential leaks or unsafe patterns

On failure in `socket_set_reuseaddr()`, `fd` is explicitly closed by `connect_to_server()` (line 140). On failure in `socket_connect_with_timeout()`, `fd` is explicitly closed by `connect_to_server()` (line 148). Both failure paths are clean.

**Important:** The fd remains in `O_NONBLOCK` mode after `socket_connect_with_timeout()` returns. Subsequent `recv()` calls on this fd in non-blocking mode will return `EAGAIN` immediately if no data is available, rather than blocking. The higher-level receive path (`tcp_recv_frame()` → `socket_recv_exact()` → `poll()` + `recv()`) uses `poll()` before `recv()`, so this is handled correctly — but only as long as the poll call precedes the recv. Direct `recv()` without prior poll would fail non-deterministically.

---

## 9. Error Handling Flow

| Error source | Detected at | Action |
|---|---|---|
| `socket()` fails | `socket_create_tcp()` (SocketUtils.cpp:31) | Log `WARNING_LO`; return `-1` |
| | `connect_to_server()` line 130 | Log `FATAL`; return `ERR_IO` |
| `setsockopt()` fails | `socket_set_reuseaddr()` (SocketUtils.cpp:103) | Log `WARNING_LO`; return false |
| | `connect_to_server()` line 136 | Log `WARNING_HI`; `socket_close(fd)`; return `ERR_IO` |
| `fcntl()` fails | `socket_set_nonblocking()` (SocketUtils.cpp:72) | Log `WARNING_LO`; return false |
| `inet_aton()` fails | `socket_connect_with_timeout()` (SocketUtils.cpp:175) | Log `WARNING_LO`; return false |
| `connect()` → non-EINPROGRESS | `socket_connect_with_timeout()` (SocketUtils.cpp:193) | Log `WARNING_LO`; return false |
| `poll()` timeout / error | `socket_connect_with_timeout()` (SocketUtils.cpp:205) | Log `WARNING_LO "connect timeout"`; return false |
| `getsockopt()` error / non-zero SO_ERROR | `socket_connect_with_timeout()` (SocketUtils.cpp:216) | Log `WARNING_LO "connect failed after poll"`; return false |
| `socket_connect_with_timeout()` false | `connect_to_server()` line 143 | Log `WARNING_HI "Connection to %s:%u failed"`; `socket_close(fd)`; return `ERR_IO` |
| Assertion failure | Any `NEVER_COMPILED_OUT_ASSERT()` | FATAL + component reset in production; `abort()` in debug |

**Severity classification (F-Prime model):**
- `FATAL` (logged by TcpBackend): `socket()` failure.
- `WARNING_HI` (logged by TcpBackend): `setsockopt()` failure, connect failure.
- `WARNING_LO` (logged by SocketUtils): all underlying syscall failures.

**Note on double logging:** `SocketUtils` functions log at `WARNING_LO` for syscall failures; `TcpBackend` logs the same event at a higher severity. Both fire for a single failure.

---

## 10. External Interactions

| Interaction | syscall / API | When |
|---|---|---|
| Create TCP socket | `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` | `connect_to_server()` step 7 |
| Set SO_REUSEADDR | `setsockopt(SO_REUSEADDR)` | `connect_to_server()` step 8 |
| Set O_NONBLOCK | `fcntl(F_GETFL)` + `fcntl(F_SETFL)` | Inside `socket_connect_with_timeout()` step 9a |
| Initiate TCP 3-way handshake | `connect()` | `socket_connect_with_timeout()` step 9c; returns EINPROGRESS |
| Wait for connection completion | `poll(POLLOUT, timeout_ms)` | `socket_connect_with_timeout()` step 9d |
| Confirm connection success | `getsockopt(SOL_SOCKET, SO_ERROR)` | `socket_connect_with_timeout()` step 9e |
| Log events | `Logger::log()` | Multiple points |

No data transfer (send/recv) occurs during init. No TLS hooks are invoked.

---

## 11. State Changes / Side Effects

| Object / Field | Before `init()` | After `init()` success |
|---|---|---|
| `m_client_fds[0]` | `-1` | valid connected fd (>= 0) |
| `m_client_count` | `0` | `1` |
| `m_open` | `false` | `true` |
| `m_is_server` | `false` | `false` (unchanged) |
| `m_listen_fd` | `-1` | `-1` (unchanged; client mode) |
| `m_recv_queue` | uninitialized | `head=0, tail=0` |
| `m_impairment` | uninitialized | `m_initialized=true`, PRNG seeded |
| OS fd (on fd): `O_NONBLOCK` | N/A | set via `fcntl` |
| OS kernel TCP state | N/A | ESTABLISHED (3-way handshake complete) |

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant SocketUtils
    participant OS_Kernel

    Caller->>TcpBackend: init(cfg [is_server=false])
    TcpBackend->>TcpBackend: m_recv_queue.init()
    TcpBackend->>TcpBackend: m_impairment.init(imp_cfg)
    TcpBackend->>TcpBackend: connect_to_server()
    TcpBackend->>SocketUtils: socket_create_tcp()
    SocketUtils->>OS_Kernel: socket(AF_INET, SOCK_STREAM, TCP)
    OS_Kernel-->>SocketUtils: fd
    SocketUtils-->>TcpBackend: fd
    TcpBackend->>SocketUtils: socket_set_reuseaddr(fd)
    SocketUtils->>OS_Kernel: setsockopt(SO_REUSEADDR)
    OS_Kernel-->>SocketUtils: 0
    SocketUtils-->>TcpBackend: true
    TcpBackend->>SocketUtils: socket_connect_with_timeout(fd, ip, port, timeout_ms)
    SocketUtils->>OS_Kernel: fcntl(F_GETFL) + fcntl(F_SETFL, O_NONBLOCK)
    OS_Kernel-->>SocketUtils: 0
    SocketUtils->>OS_Kernel: connect(fd, &addr, sizeof(addr))
    OS_Kernel-->>SocketUtils: -1 (errno=EINPROGRESS)
    SocketUtils->>OS_Kernel: poll(fd, POLLOUT, timeout_ms)
    Note over OS_Kernel: TCP 3-way handshake completes
    OS_Kernel-->>SocketUtils: 1 (fd writable)
    SocketUtils->>OS_Kernel: getsockopt(fd, SO_ERROR, &opt_err)
    OS_Kernel-->>SocketUtils: 0, opt_err=0
    SocketUtils-->>TcpBackend: true (connected)
    Note over TcpBackend: m_client_fds[0]=fd; m_client_count=1; m_open=true
    Note over TcpBackend: log INFO "Connected to %s:%u"
    TcpBackend-->>Caller: Result::OK
```

Failure path (connection refused or timeout):

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant SocketUtils
    participant OS_Kernel

    Caller->>TcpBackend: init(cfg [is_server=false])
    TcpBackend->>TcpBackend: connect_to_server()
    TcpBackend->>SocketUtils: socket_connect_with_timeout(fd, ip, port, 5000)
    SocketUtils->>OS_Kernel: connect() → EINPROGRESS
    SocketUtils->>OS_Kernel: poll(POLLOUT, 5000)
    OS_Kernel-->>SocketUtils: 1 (writable, but connection refused)
    SocketUtils->>OS_Kernel: getsockopt(SO_ERROR) → opt_err=ECONNREFUSED
    SocketUtils-->>TcpBackend: false
    Note over TcpBackend: log WARNING_HI; socket_close(fd)
    TcpBackend-->>Caller: Result::ERR_IO
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

- All member state zeroed in constructor (lines 29–36).
- `m_recv_queue` reset to empty.
- `ImpairmentEngine` configured and PRNG seeded.
- TCP socket created; `SO_REUSEADDR` set; `O_NONBLOCK` set; non-blocking `connect()` issued; `poll()` waits up to `connect_timeout_ms` for handshake completion; `getsockopt(SO_ERROR)` confirms success.
- `m_client_fds[0]` = connected fd; `m_client_count = 1`; `m_open = true`.
- No message I/O occurs.

### What happens during steady-state execution

After `init()` returns `OK`, the caller uses `send_message()` and `receive_message()` through the `TransportInterface`. The connected fd in `m_client_fds[0]` remains in `O_NONBLOCK` mode for the lifetime of the connection. The `receive_message()` loop uses `poll()` before each `recv()`, which is correct for non-blocking sockets.

---

## 14. Known Risks / Observations

**Risk 1: O_NONBLOCK left set after connect**
`socket_set_nonblocking()` is called inside `socket_connect_with_timeout()` to enable the non-blocking `connect()`. The fd is never set back to blocking mode after the handshake completes. All subsequent `recv()` calls on this fd run in non-blocking mode. The `socket_recv_exact()` receive path correctly uses `poll()` before `recv()`, handling this. However, any code that calls `recv()` directly on the fd without a prior `poll()` will get non-deterministic `EAGAIN` behavior.

**Risk 2: poll() EINTR not retried**
If a signal (other than SIGINT) arrives during the `poll()` wait inside `socket_connect_with_timeout()`, `poll()` returns -1 with `errno=EINTR`. The code treats `poll_result <= 0` as a timeout failure. The connect attempt is abandoned. No EINTR-retry loop is present.

**Risk 3: getsockopt check not exhaustive**
`getsockopt(SO_ERROR)` is called on the writable fd after `poll()` succeeds. This is the correct POSIX pattern to detect connection failure on a non-blocking socket. However, if `POLLHUP` or `POLLERR` is set in `pfd.revents` rather than `POLLOUT`, `poll` may still return `> 0` and the code proceeds to `getsockopt`. In that case `opt_err` will be non-zero and the function returns false correctly. The `pfd.revents` bits are not explicitly inspected before calling `getsockopt`.

**Risk 4: No reconnect on failure**
`connect_to_server()` makes exactly one attempt. If `socket_connect_with_timeout()` returns false, `ERR_IO` is returned to the caller. There is no automatic retry with backoff. The caller is responsible for retrying `init()` if desired (but note that `NEVER_COMPILED_OUT_ASSERT(!m_open)` in `init()` prevents re-initialization on the same object).

**Risk 5: Double logging on socket failures**
`SocketUtils` functions log at `WARNING_LO` for syscall failures; `TcpBackend` then logs the same event at `FATAL` or `WARNING_HI`. Both fire for a single failure, producing duplicate log entries.

**Risk 6: No IPv6 support**
`socket_connect_with_timeout()` hardcodes `AF_INET` and uses `inet_aton()`. IPv6 addresses are not supported.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `impairment_config_default()` is an inline function in `ImpairmentConfig.hpp`. Sets all numeric fields to 0, all booleans to `false`, `prng_seed` to `42ULL`. Confirmed.

- [ASSUMPTION] `connect_timeout_ms` defaults to 5000 ms (5 seconds) from `transport_config_default()`. The exact default is in `ChannelConfig.hpp`; the code uses `m_cfg.connect_timeout_ms` which is copied from the caller's config.

- [ASSUMPTION] `Logger::log()` is thread-safe or only called from a single thread context. No mutex is visible at call sites.

- [ASSUMPTION] The connected fd in `m_client_fds[0]` is used for both send and receive. In client mode, `m_client_fds[0]` is the only active fd. `m_client_fds[1..7]` remain `-1`.

- [ASSUMPTION] `poll()` on the non-blocking fd correctly returns `POLLOUT` when the TCP handshake completes on the target platform (macOS and Linux). On some platforms, both `POLLIN` and `POLLOUT` may be set on connect completion.

- [ASSUMPTION] Multiple calls to `init()` on the same `TcpBackend` object are prevented by `NEVER_COMPILED_OUT_ASSERT(!m_open)` at line 91. After a successful `init()`, `m_open == true`; a second `init()` triggers FATAL.

- [ASSUMPTION] The server-side `TcpBackend` (or equivalent server process) is already listening on `peer_ip:peer_port` before this `init()` is called. If the server is not yet listening, `connect()` returns `ECONNREFUSED` immediately (not `EINPROGRESS`), which is handled at SocketUtils.cpp:192–196.

- [ASSUMPTION] `PrngEngine::seed()` coerces seed=0 to seed=1 to ensure the xorshift64 state is never zero. Confirmed in `PrngEngine.hpp`.
