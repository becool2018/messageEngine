# UC_19 — TCP server accept

**HL Group:** HL-7 — User starts a server endpoint
**Actor:** User
**Requirement traceability:** REQ-4.1.1, REQ-6.1.1, REQ-6.1.2, REQ-6.1.3, REQ-6.3.1, REQ-7.1.1

---

## 1. Use Case Overview

### Clear description of what triggers this flow

The User calls `TcpBackend::init()` with a `TransportConfig` where `config.is_server == true` and `config.kind == TransportKind::TCP`. This initiates server endpoint setup: socket creation, SO_REUSEADDR, bind, listen. A client connection is then accepted when `TcpBackend::accept_clients()` is called — either directly or via the `poll_clients_once()` path inside `receive_message()`.

### Expected outcome (single goal)

`m_listen_fd` holds a valid, bound, listening TCP socket. When a remote client connects, `accept_clients()` stores the accepted fd in `m_client_fds[m_client_count - 1]` and increments `m_client_count`. `m_open` is true and `Result::OK` is returned to the caller.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

**Primary entry point — socket setup:**
`TcpBackend::init(const TransportConfig& config)` — `src/platform/TcpBackend.cpp`, line 88.
Precondition assertions (lines 90–91):
- `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP)`
- `NEVER_COMPILED_OUT_ASSERT(!m_open)`

**Secondary entry point — client accept:**
`TcpBackend::accept_clients()` — `src/platform/TcpBackend.cpp`, line 167.
Called from `poll_clients_once()` (line 332), which is called from `receive_message()` (line 404) in server mode on every poll iteration.
Precondition assertions (lines 169–170):
- `NEVER_COMPILED_OUT_ASSERT(m_is_server)`
- `NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0)`

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase 1: TcpBackend::init() — socket setup

1. Caller constructs `TcpBackend`. Constructor (line 28):
   - `m_listen_fd = -1`, `m_client_count = 0U`, `m_wire_buf{}`, `m_cfg{}`, `m_open = false`, `m_is_server = false`.
   - `NEVER_COMPILED_OUT_ASSERT(MAX_TCP_CONNECTIONS > 0U)` (line 33).
   - Loop `i = 0..MAX_TCP_CONNECTIONS-1` (i.e., 0..7): `m_client_fds[i] = -1` (lines 34–36).

2. Caller invokes `TcpBackend::init(config)` with `config.is_server == true`.
   - Line 90: `NEVER_COMPILED_OUT_ASSERT(config.kind == TransportKind::TCP)` — passes.
   - Line 91: `NEVER_COMPILED_OUT_ASSERT(!m_open)` — passes.
   - Line 93: `m_cfg = config` — full struct copy.
   - Line 94: `m_is_server = config.is_server = true`.

3. `m_recv_queue.init()` (line 97) → `RingBuffer::init()`:
   - `NEVER_COMPILED_OUT_ASSERT(MSG_RING_CAPACITY > 0U)`.
   - `NEVER_COMPILED_OUT_ASSERT((MSG_RING_CAPACITY & (MSG_RING_CAPACITY - 1U)) == 0U)`.
   - `m_head.store(0U, relaxed)`; `m_tail.store(0U, relaxed)`.

4. ImpairmentConfig initialization (lines 99–104):
   - `impairment_config_default(imp_cfg)` fills safe defaults (all impairments disabled, all numeric fields 0, `prng_seed=42`).
   - If `config.num_channels > 0U`: `imp_cfg.enabled = config.channels[0U].impairments_enabled`.
   - `m_impairment.init(imp_cfg)` → `ImpairmentEngine::init()`:
     - Copies cfg; seeds PRNG (seed = `cfg.prng_seed != 0 ? cfg.prng_seed : 42`).
     - `memset(m_delay_buf, 0, sizeof(m_delay_buf))`; `m_delay_count = 0`.
     - `memset(m_reorder_buf, 0, sizeof(m_reorder_buf))`; `m_reorder_count = 0`.
     - `m_partition_active = false`; `m_partition_start_us = 0`; `m_next_partition_event_us = 0`.
     - `m_initialized = true`.

5. Branch: `m_is_server == true` → server path (line 107).

6. `bind_and_listen(config.bind_ip, config.bind_port)` (line 108) → `TcpBackend.cpp:54`:
   - `NEVER_COMPILED_OUT_ASSERT(ip != nullptr)` (line 56).
   - `NEVER_COMPILED_OUT_ASSERT(m_is_server)` (line 57).

7. `socket_create_tcp()` (line 59) → `SocketUtils.cpp:28`:
   - syscall: `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`.
   - Returns `fd >= 0` or `-1`.
   - On failure: `Logger::log(WARNING_LO, ...)` → returns `-1`.
   - `m_listen_fd = returned fd`.
   - If `m_listen_fd < 0`: `Logger::log(FATAL, "TcpBackend", "socket_create_tcp failed in server mode")`; return `ERR_IO`.

8. `socket_set_reuseaddr(m_listen_fd)` (line 64) → `SocketUtils.cpp:97`:
   - syscall: `setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(int))`.
   - On failure: `Logger::log(WARNING_LO, ...)`; return false.
   - If false: `Logger::log(WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed")`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

9. `socket_bind(m_listen_fd, ip, port)` (line 70) → `SocketUtils.cpp:120`:
   - `inet_aton(ip, &addr.sin_addr)` — parse IP string.
   - syscall: `bind(fd, (sockaddr*)&addr, sizeof(addr))`.
   - On failure: `Logger::log(WARNING_LO, ...)`; return false.
   - If false: `Logger::log(FATAL, "TcpBackend", "socket_bind failed on port %u", port)`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

10. `socket_listen(m_listen_fd, MAX_TCP_CONNECTIONS)` (line 76) → `SocketUtils.cpp:231`:
    - syscall: `listen(fd, 8)`.
    - On failure: `Logger::log(WARNING_LO, ...)`; return false.
    - If false: `Logger::log(FATAL, "TcpBackend", "socket_listen failed")`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

11. `m_open = true` (line 82). `Logger::log(INFO, "TcpBackend", "Server listening on %s:%u", ip, port)` (line 83). `NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0)` (line 84) — postcondition. `bind_and_listen` returns `Result::OK`.

12. Back in `init()` (line 116): `NEVER_COMPILED_OUT_ASSERT(m_open)` — postcondition. Returns `Result::OK`.

### Phase 2: TcpBackend::accept_clients() — connection accept

Called from `poll_clients_once()` (TcpBackend.cpp:332), which is called from `receive_message()` (TcpBackend.cpp:404) on every poll iteration in server mode.

13. Entry assertions (lines 169–170):
    - `NEVER_COMPILED_OUT_ASSERT(m_is_server)` — true.
    - `NEVER_COMPILED_OUT_ASSERT(m_listen_fd >= 0)` — true.

14. Capacity guard (line 172): if `m_client_count >= MAX_TCP_CONNECTIONS` return `Result::OK` immediately. (`MAX_TCP_CONNECTIONS == 8U`.)

15. `socket_accept(m_listen_fd)` (line 177) → `SocketUtils.cpp:252`:
    - syscall: `accept(fd, nullptr, nullptr)`.
    - Blocks if `m_listen_fd` is in blocking mode (listen socket is NOT set non-blocking — see Section 14, Risk 1).
    - On failure or no pending connection: `Logger::log(WARNING_LO, ...)`; return `-1`.

16. If `socket_accept` returns `client_fd < 0` (no pending connection or error):
    - `accept_clients()` returns `Result::OK` (line 180). No state change.

17. If `socket_accept` returns `client_fd >= 0`:
    - Line 184: `NEVER_COMPILED_OUT_ASSERT(m_client_count < MAX_TCP_CONNECTIONS)`.
    - Line 185: `m_client_fds[m_client_count] = client_fd`.
    - Line 186: `++m_client_count`.
    - Lines 188–189: `Logger::log(INFO, "TcpBackend", "Accepted client %u, total clients: %u", m_client_count - 1U, m_client_count)`.
    - Line 191: `NEVER_COMPILED_OUT_ASSERT(m_client_count <= MAX_TCP_CONNECTIONS)`.
    - Returns `Result::OK`.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::init(config)                                    [TcpBackend.cpp:88]
  m_recv_queue.init()                                       [RingBuffer.hpp]
    m_head.store(0U, relaxed)
    m_tail.store(0U, relaxed)
  impairment_config_default(imp_cfg)                        [ImpairmentConfig.hpp inline]
  m_impairment.init(imp_cfg)                                [ImpairmentEngine.cpp:44]
    m_prng.seed(seed)
    memset(m_delay_buf, ...)
    memset(m_reorder_buf, ...)
  bind_and_listen(config.bind_ip, config.bind_port)         [TcpBackend.cpp:54]
    socket_create_tcp()                                     [SocketUtils.cpp:28]
      socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)             [POSIX syscall]
    socket_set_reuseaddr(m_listen_fd)                       [SocketUtils.cpp:97]
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)         [POSIX syscall]
    socket_bind(m_listen_fd, ip, port)                      [SocketUtils.cpp:120]
      inet_aton(ip, &addr.sin_addr)                         [POSIX]
      bind(fd, &addr, sizeof(addr))                         [POSIX syscall]
    socket_listen(m_listen_fd, MAX_TCP_CONNECTIONS)         [SocketUtils.cpp:231]
      listen(fd, 8)                                         [POSIX syscall]

TcpBackend::accept_clients()                                [TcpBackend.cpp:167]
  socket_accept(m_listen_fd)                                [SocketUtils.cpp:252]
    accept(fd, nullptr, nullptr)                            [POSIX syscall]
  Logger::log(INFO, ...)                                    [on success]
```

---

## 5. Key Components Involved

| Component | File / Location | Role in this flow |
|---|---|---|
| `TcpBackend` | `src/platform/TcpBackend.cpp/.hpp` | Orchestrates TCP server lifecycle; owns all state including `m_listen_fd`, `m_client_fds[]`, `m_client_count`. |
| `TcpBackend::bind_and_listen()` | `TcpBackend.cpp:54` | Private helper: creates, configures, binds, and places the listen socket into listening state. |
| `TcpBackend::accept_clients()` | `TcpBackend.cpp:167` | Private helper: accepts one pending connection per call; appends fd to `m_client_fds[]`. |
| `RingBuffer` | `src/core/RingBuffer.hpp` | Receive queue; zero-initialized during `init()`. |
| `ImpairmentEngine` | `src/platform/ImpairmentEngine.cpp` | Initialized during `init()`; not exercised during accept itself. |
| `SocketUtils` | `src/platform/SocketUtils.cpp/.hpp` | All raw POSIX socket operations: `socket()`, `setsockopt()`, `bind()`, `listen()`, `accept()`. |
| `TransportConfig` | `src/core/ChannelConfig.hpp` | Input configuration driving all branching (`is_server`, `bind_ip`, `bind_port`, `num_channels`). |
| `Types.hpp` | `src/core/Types.hpp` | Constants: `MAX_TCP_CONNECTIONS = 8U`, `SOCKET_RECV_BUF_BYTES = 8192U`, `MSG_RING_CAPACITY = 64U`. |
| `Logger` | `src/core/Logger.hpp` | Emits INFO on success; WARNING_HI or FATAL on failures. |
| `impairment_config_default()` | `ImpairmentConfig.hpp` | Inline helper; sets all impairment fields to safe defaults with `prng_seed = 42`. |

---

## 6. Branching Logic / Decision Points

**Decision 1: config.is_server (TcpBackend.cpp:107)**
- `true` → server path: `bind_and_listen()`.
- `false` → client path: `connect_to_server()` (not covered in this use case).

**Decision 2: socket_create_tcp() return value (bind_and_listen line 60)**
- `fd >= 0` → continue to `socket_set_reuseaddr`.
- `fd < 0` → `Logger FATAL`; return `ERR_IO` (no fd to close).

**Decision 3: socket_set_reuseaddr() return value (bind_and_listen line 64)**
- `true` → continue to `socket_bind`.
- `false` → `Logger WARNING_HI`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

**Decision 4: socket_bind() return value (bind_and_listen line 70)**
- `true` → continue to `socket_listen`.
- `false` → `Logger FATAL`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

**Decision 5: socket_listen() return value (bind_and_listen line 76)**
- `true` → `m_open = true`; log INFO; return `OK`.
- `false` → `Logger FATAL`; `socket_close(m_listen_fd)`; `m_listen_fd = -1`; return `ERR_IO`.

**Decision 6: m_client_count >= MAX_TCP_CONNECTIONS (accept_clients line 172)**
- `true` → return `OK` immediately; no `accept()` issued.
- `false` → proceed to `socket_accept()`.

**Decision 7: socket_accept() return value (accept_clients line 178)**
- `client_fd >= 0` → store in `m_client_fds[m_client_count]`; increment `m_client_count`; log INFO; return `OK`.
- `client_fd < 0` → return `OK` silently (both EAGAIN and real errors treated identically).

**Decision 8: config.num_channels > 0U (init line 101)**
- `true` → `imp_cfg.enabled = config.channels[0U].impairments_enabled`.
- `false` → `imp_cfg.enabled` remains `false` (default).

---

## 7. Concurrency / Threading Behavior

### Threads created

None. `TcpBackend` is single-threaded by design. `init()` and `accept_clients()` must be called from the same thread.

### Where context switches occur

`socket_accept()` calls POSIX `accept()`, which may block indefinitely in the calling thread if `m_listen_fd` is in blocking mode (the listen socket is NOT set non-blocking in this code path — see Section 14, Risk 1).

### Synchronization primitives

- `RingBuffer` uses `std::atomic<uint32_t>` (`m_head`, `m_tail`) for SPSC safety. During `init()`, only relaxed stores are issued (no concurrent access yet).
- `m_client_fds[]` is a plain `int` array with no atomic protection.
- `ImpairmentEngine` PRNG state is not thread-safe.

### Producer/consumer relationships

Not applicable during the init and accept phases. No message I/O occurs.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory

All storage is statically allocated inline in the `TcpBackend` object — no heap use:

- `m_client_fds[MAX_TCP_CONNECTIONS]` = `m_client_fds[8]`: plain `int` array.
- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` = `m_wire_buf[8192]`: inline `uint8_t` array.
- `m_recv_queue` (`RingBuffer`): contains `m_buf[64]` of `MessageEnvelope` plus two `std::atomic<uint32_t>`.
- `m_impairment` (`ImpairmentEngine`): `m_delay_buf[32]` `DelayEntry` (each containing a full `MessageEnvelope`), `m_reorder_buf[32]` `MessageEnvelope`, and scalar fields — all inline.
- `m_cfg` (`TransportConfig`): full struct copy at line 93; includes `channels[MAX_CHANNELS]` inline.

### Lifetime of key objects

OS file descriptors (`m_listen_fd`, `m_client_fds[i]`) are the only external resources. They are owned by `TcpBackend` from allocation until `close()` or destructor.

### Stack vs heap usage

No heap allocation. All objects are on the stack within the `TcpBackend` instance.

### RAII usage

`TcpBackend::~TcpBackend()` calls `TcpBackend::close()` (qualified call, line 47) to ensure fd release even if the caller forgets to call `close()`.

### Potential leaks or unsafe patterns

If `bind_and_listen()` fails mid-way, the partially created fd is explicitly closed before return (lines 66, 72–73, 78–79). No fd leak on any failure path. The accepted `client_fd` ownership transfers to `TcpBackend` on success of `socket_accept()`.

---

## 9. Error Handling Flow

| Error source | Detected at | Action |
|---|---|---|
| `socket()` fails | `socket_create_tcp()` (SocketUtils.cpp:31) | Log `WARNING_LO`; return `-1` |
| | `bind_and_listen()` line 60 | Log `FATAL`; return `ERR_IO` (no fd to close) |
| `setsockopt()` fails | `socket_set_reuseaddr()` (SocketUtils.cpp:103) | Log `WARNING_LO`; return `false` |
| | `bind_and_listen()` line 64 | Log `WARNING_HI`; `socket_close(m_listen_fd)`; `m_listen_fd=-1`; return `ERR_IO` |
| `bind()` fails | `socket_bind()` (SocketUtils.cpp:142) | Log `WARNING_LO`; return `false` |
| | `bind_and_listen()` line 70 | Log `FATAL`; `socket_close(m_listen_fd)`; `m_listen_fd=-1`; return `ERR_IO` |
| `listen()` fails | `socket_listen()` (SocketUtils.cpp:237) | Log `WARNING_LO`; return `false` |
| | `bind_and_listen()` line 76 | Log `FATAL`; `socket_close(m_listen_fd)`; `m_listen_fd=-1`; return `ERR_IO` |
| `accept()` fails / EAGAIN | `socket_accept()` (SocketUtils.cpp:259) | Log `WARNING_LO`; return `-1` |
| | `accept_clients()` line 178 | Return `Result::OK` silently (non-fatal) |
| At-capacity guard | `accept_clients()` line 172 | Return `Result::OK` silently; no accept issued |
| Assertion failure | Any `NEVER_COMPILED_OUT_ASSERT()` | `abort()` in debug; FATAL + component reset in production |

**Note on double logging:** `SocketUtils` functions log at `WARNING_LO` for syscall failures; `TcpBackend` then logs the same event at `FATAL` or `WARNING_HI`. Both fire for a single failure, producing duplicate log entries at different severity levels.

---

## 10. External Interactions

| Interaction | syscall / API | When |
|---|---|---|
| Create TCP socket | `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` | `bind_and_listen()` step 7 |
| Set SO_REUSEADDR | `setsockopt(SO_REUSEADDR)` | `bind_and_listen()` step 8 |
| Bind to address/port | `bind()` | `bind_and_listen()` step 9 |
| Enable listening queue | `listen(fd, 8)` | `bind_and_listen()` step 10 |
| Accept client connection | `accept(fd, nullptr, nullptr)` | `accept_clients()` step 15 |
| Log events | `Logger::log()` | Multiple points throughout |

No TCP data transfer (send/recv) occurs during init or accept. No TLS hooks are invoked.

---

## 11. State Changes / Side Effects

| Object / Field | Before `init()` | After `init()` success | After `accept_clients()` success |
|---|---|---|---|
| `m_listen_fd` | `-1` | valid fd (>= 0) | unchanged |
| `m_client_count` | `0` | `0` | `1` (first client) |
| `m_open` | `false` | `true` | unchanged |
| `m_is_server` | `false` | `true` | unchanged |
| `m_recv_queue` | uninitialized | `head=0, tail=0` | unchanged |
| `m_impairment` | uninitialized | `m_initialized=true`, PRNG seeded | unchanged |
| `m_client_fds[0]` | `-1` | `-1` | valid client fd |

**OS side effects:**
- OS fd table: `m_listen_fd` allocated; client fds allocated on accept.
- Kernel TCP listening queue: established by `listen()`; each `accept()` removes one entry.
- Logger output at INFO or WARNING level depending on path.

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant Caller
    participant TcpBackend
    participant SocketUtils
    participant OS_Kernel

    Caller->>TcpBackend: init(cfg [is_server=true])
    TcpBackend->>TcpBackend: m_recv_queue.init()
    TcpBackend->>TcpBackend: m_impairment.init(imp_cfg)
    TcpBackend->>TcpBackend: bind_and_listen(ip, port)
    TcpBackend->>SocketUtils: socket_create_tcp()
    SocketUtils->>OS_Kernel: socket(AF_INET, SOCK_STREAM, TCP)
    OS_Kernel-->>SocketUtils: listen_fd
    SocketUtils-->>TcpBackend: listen_fd
    TcpBackend->>SocketUtils: socket_set_reuseaddr(listen_fd)
    SocketUtils->>OS_Kernel: setsockopt(SO_REUSEADDR)
    OS_Kernel-->>SocketUtils: 0
    SocketUtils-->>TcpBackend: true
    TcpBackend->>SocketUtils: socket_bind(listen_fd, ip, port)
    SocketUtils->>OS_Kernel: bind()
    OS_Kernel-->>SocketUtils: 0
    SocketUtils-->>TcpBackend: true
    TcpBackend->>SocketUtils: socket_listen(listen_fd, 8)
    SocketUtils->>OS_Kernel: listen()
    OS_Kernel-->>SocketUtils: 0
    SocketUtils-->>TcpBackend: true
    Note over TcpBackend: m_open=true; log INFO "Server listening"
    TcpBackend-->>Caller: Result::OK

    Note over OS_Kernel: External client issues connect()

    Caller->>TcpBackend: receive_message() [triggers accept]
    TcpBackend->>TcpBackend: poll_clients_once() → accept_clients()
    TcpBackend->>SocketUtils: socket_accept(listen_fd)
    SocketUtils->>OS_Kernel: accept(listen_fd, nullptr, nullptr)
    OS_Kernel-->>SocketUtils: client_fd
    SocketUtils-->>TcpBackend: client_fd
    Note over TcpBackend: m_client_fds[0]=client_fd; m_client_count=1; log INFO
    TcpBackend-->>Caller: Result::OK
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

- All member state zeroed or set to sentinel values in constructor (lines 29–36).
- `m_recv_queue` reset to empty (atomic head/tail to 0).
- `ImpairmentEngine` configured and PRNG seeded.
- OS resources allocated: listen socket created, SO_REUSEADDR set, bound to `bind_ip:bind_port`, placed in listen state with backlog `MAX_TCP_CONNECTIONS = 8`.
- `m_open = true` marks transition from init to runtime.
- No message I/O occurs.

### What happens during steady-state execution (accept phase)

- `accept_clients()` is called from `poll_clients_once()` on every invocation of `receive_message()` in server mode. It is a lightweight, non-allocating polling operation.
- Exactly one `accept()` per `accept_clients()` call (no loop); at most one new client accepted per call.
- Pure OS resource acquisition: no serialization, no impairment logic, no `RingBuffer` involvement.
- `m_client_fds[]` and `m_client_count` are the only persistent state changes.

---

## 14. Known Risks / Observations

**Risk 1: BLOCKING accept()**
`socket_accept()` calls `accept(fd, nullptr, nullptr)` without first setting `m_listen_fd` to non-blocking mode. `O_NONBLOCK` is NOT set on the listen socket in `bind_and_listen()`. If no client is pending, `accept()` will block indefinitely in the calling thread. The comment at TcpBackend.cpp:179 ("No pending connection (EAGAIN in non-blocking mode) is not an error") implies non-blocking intent that the code does not implement.

**Risk 2: One accept per receive_message() call**
`accept_clients()` accepts at most one client per call. A burst of simultaneous connects is queued in the kernel backlog (depth = `MAX_TCP_CONNECTIONS = 8`) and accepted one-per-`receive_message()` poll cycle.

**Risk 3: accept() error vs EAGAIN indistinguishable**
`socket_accept()` returns `-1` for both EAGAIN (no client ready) and genuine errors (e.g., EBADF, ENFILE). Both are logged at `WARNING_LO` and treated identically. Real errors are silently swallowed.

**Risk 4: m_client_fds[] array compaction on disconnect**
`remove_client_fd()` (TcpBackend.cpp:204–217) compacts the array with a linear shift. This is O(N) and relies on indices being stable during a single `receive_message()` call. Safe given single-threaded usage.

**Risk 5: Double logging on socket failures**
`SocketUtils` functions log at `WARNING_LO` for syscall failures; `TcpBackend` then logs the same event at `FATAL` or `WARNING_HI`. Both fire on a single failure.

**Risk 6: accepted client fd not set non-blocking**
The code calls `socket_set_nonblocking()` on the accepted client fd. However, `socket_accept()` (SocketUtils.cpp:258) calls `accept()` and returns immediately without calling `socket_set_nonblocking()` on the result. [ASSUMPTION: if non-blocking client I/O is needed, the caller of accept must set it explicitly.]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The listen socket (`m_listen_fd`) is left in blocking mode because `socket_set_nonblocking()` is never called on it in `bind_and_listen()`. Behavior of `accept()` with a blocking fd when no client is present will block the thread indefinitely.

- [ASSUMPTION] `impairment_config_default()` is an inline function in `ImpairmentConfig.hpp`. It sets all numeric fields to 0, all booleans to `false`, and `prng_seed` to `42ULL`.

- [ASSUMPTION] `Logger::log()` is a thread-safe, non-blocking call. Its implementation was not read; it may or may not be re-entrant.

- [ASSUMPTION] `envelope_valid()` checks: `message_type != INVALID`, `payload_length <= MSG_MAX_PAYLOAD_BYTES`, `source_id != NODE_ID_INVALID`. `envelope_copy()` is `memcpy(&dst, &src, sizeof(MessageEnvelope))`. Both confirmed in `MessageEnvelope.hpp`.

- [ASSUMPTION] `m_prng` (PrngEngine member of ImpairmentEngine) is seeded via `m_prng.seed(seed)` during `init()`. PrngEngine coerces seed=0 to seed=1 to ensure the xorshift64 state is never zero.

- [ASSUMPTION] Multiple calls to `init()` on the same `TcpBackend` object are prevented by `NEVER_COMPILED_OUT_ASSERT(!m_open)` at line 91. In production builds this triggers FATAL + component reset rather than `abort()`.

- [ASSUMPTION] The caller always calls `close()` or relies on the destructor (`TcpBackend::close()` at TcpBackend.cpp:47) to release OS resources.
