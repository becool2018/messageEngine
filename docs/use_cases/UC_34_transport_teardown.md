# UC_34 — Transport teardown

**HL Group:** HL-9 — User closes a transport
**Actor:** User
**Requirement traceability:** REQ-4.1.4, REQ-6.1.3, REQ-6.3.2, REQ-7.1.1, REQ-7.1.2

---

## 1. Use Case Overview

### Clear description of what triggers this flow

Transport teardown is triggered by one of two events: (1) the User sends SIGINT (e.g., Ctrl+C) while a Server or Client process is running — the signal handler sets a `volatile sig_atomic_t` stop flag that the main loop detects on its next iteration; or (2) the application reaches the natural end of its operational sequence (Client sends all N messages; Server exhausts its bounded iteration count). In both cases, the final act is an explicit call to `TcpBackend::close()`, `UdpBackend::close()`, or `LocalSimHarness::close()` depending on which transport is in use. The `DeliveryEngine` has no `close()` method; it holds pointers and value-type sub-objects but performs no explicit teardown. The transport backends' destructors call `close()` idempotently when the stack-allocated objects go out of scope at process exit.

### Expected outcome (single goal)

All file descriptors held by the transport backend are closed via POSIX `close(2)`. `m_open` is set to `false`. `Logger` emits an `INFO`-level event. In-flight messages in the `RingBuffer`, `RetryManager`, and `AckTracker` are abandoned in place — no flush-to-wire occurs.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

**SIGINT path (Server):**
The kernel delivers SIGINT to the process. The OS invokes `signal_handler()` (Server.cpp:55–59), which writes `g_stop_flag = 1` (a `volatile sig_atomic_t`). This is an asynchronous signal delivery, not a function call from the main thread. The main loop reads `g_stop_flag` at the top of each iteration and `break`s when non-zero.

**Natural exit path (Client):**
After the bounded `for` loop at Client.cpp:240–260 completes all `NUM_MESSAGES` iterations, execution falls through to the final pump at Client.cpp:265–276, then reaches `transport.close()` at Client.cpp:281 directly.

**Natural exit path (Server):**
If `g_stop_flag` is never set, the loop exits when `iter` reaches `MAX_LOOP_ITERS`. Execution falls through to `transport.close()` at Server.cpp:258.

**Destructor path:**
When either process exits via `return` from `main()`, the stack-allocated backend object goes out of scope. `TcpBackend::~TcpBackend()` (TcpBackend.cpp:43–48) calls `TcpBackend::close()` with a qualified call (line 47). Since `close()` guards on `fd >= 0`, the double-close is safe (idempotent for `fd = -1`).

---

## 3. End-to-End Control Flow (Step-by-Step)

### SIGINT → Server shutdown path (primary trace)

1. User presses Ctrl+C. Kernel sends SIGINT to the server process.

2. Kernel interrupts the server's current blocking call — most likely inside `TcpBackend::receive_message()` → `poll_clients_once()` → `recv_from_client()` → `tcp_recv_frame()` → `socket_recv_exact()` → `poll(2)` (SocketUtils.cpp:355). `poll()` returns -1 (`EINTR`) or 0 (timeout depending on platform). Either way `socket_recv_exact()` returns false, propagating up to `recv_from_client()` returning `ERR_IO`, then `receive_message()` returning `ERR_TIMEOUT` after the bounded loop exhausts (TcpBackend.cpp:420).

3. Before or after `poll` returns, the OS invokes `signal_handler()` in the signal context. `g_stop_flag = 1` is written atomically as `sig_atomic_t`.

4. `engine.receive()` (DeliveryEngine.cpp) receives `ERR_TIMEOUT` from the transport and returns it to the main loop.

5. The main loop checks `g_stop_flag != 0` at the top of the next iteration. The condition is true. Logger emits "Stop flag set; exiting loop". `break` exits the for-loop.

6. Execution reaches `transport.close()` called on the stack-allocated `TcpBackend` object.

7. **`TcpBackend::close()` (TcpBackend.cpp:427–445) executes:**
   - a. Checks `m_listen_fd >= 0` (line 429). If true (server mode), calls `socket_close(m_listen_fd)` (SocketUtils.cpp:274–286) → POSIX `close(m_listen_fd)`. Sets `m_listen_fd = -1` (line 431).
   - b. Fixed loop over `i = 0..MAX_TCP_CONNECTIONS-1` (line 435). For each slot where `m_client_fds[i] >= 0` (line 436): calls `socket_close(m_client_fds[i])` → POSIX `close()`; sets `m_client_fds[i] = -1` (line 438).
   - c. `m_client_count = 0U` (line 442).
   - d. `m_open = false` (line 443).
   - e. `Logger::log(INFO, "TcpBackend", "Transport closed")` (line 444).

8. **`socket_close()` (SocketUtils.cpp:274–286)** for each fd: `NEVER_COMPILED_OUT_ASSERT(fd >= 0)` (line 277); calls `close(fd)` (line 280). If `close()` returns < 0 (line 281), logs `WARNING_LO` (line 282) and continues — fd is treated as closed regardless.

9. Control returns to the application. Logger emits final statistics message.

10. `signal(SIGINT, old_handler)` restores the original handler.

11. `main()` returns 0. Stack unwinds. `TcpBackend::~TcpBackend()` (TcpBackend.cpp:43–48) is called. It calls `TcpBackend::close()` (line 47, qualified call). `m_listen_fd == -1` → if-branch skipped. All `m_client_fds[i] == -1` → loop body never entered. `m_open` already false. Logger emits "Transport closed" again (harmless duplicate).

12. `DeliveryEngine` goes out of scope. Compiler-generated destructor runs. Sub-objects `m_ack_tracker`, `m_retry_manager`, `m_dedup`, `m_id_gen` are stack-allocated value types with trivial or compiler-generated destructors. Their in-memory state is discarded. No network I/O occurs. All pending entries in `m_ack_tracker.m_slots[]` and `m_retry_manager.m_slots[]` are abandoned without notification.

13. `RingBuffer m_recv_queue` (embedded in the transport backend) goes out of scope. Its two `std::atomic<uint32_t>` members are destroyed. Any `MessageEnvelope` objects still in `m_buf[]` are abandoned.

### Client natural shutdown path (secondary trace)

14. Client calls `engine.pump_retries(final_now_us)` → `m_retry_manager.collect_due()` scans active slots, collects due-for-retry envelopes. For each: `send_via_transport()` calls `m_transport->send_message()`. Since `m_open == true`, the send proceeds normally through `TcpBackend::send_message()`.

15. Client calls `engine.sweep_ack_timeouts(final_now_us)` → `m_ack_tracker.sweep_expired()` scans all slots, transitions expired `PENDING` entries to `FREE`. No network I/O.

16. Client calls `transport.close()`. `TcpBackend::close()` executes identically to steps 7–8, except `m_listen_fd == -1` (client mode has no listen socket), so only `m_client_fds[0]` (the single connection) is closed.

17. `main()` returns. Destructor path proceeds as in steps 11–13.

### UdpBackend::close() path

18. `UdpBackend::close()` (UdpBackend.cpp:262–271):
    - Checks `m_fd >= 0` (line 264). If true, calls `socket_close(m_fd)` → `close(m_fd)`. Sets `m_fd = -1` (line 266).
    - `m_open = false` (line 269).
    - `Logger::log(INFO, "UdpBackend", "Transport closed")` (line 270).
    - No loop over client fds (UDP has one bound socket; no connection state).

### LocalSimHarness::close() path

19. `LocalSimHarness::close()`: sets `m_peer = nullptr`. Sets `m_open = false`. Calls `Logger::log(INFO, "LocalSimHarness", "Transport closed")`. No socket fds to close. After `m_peer = nullptr`, any subsequent `send_message()` call on this harness fires `NEVER_COMPILED_OUT_ASSERT(m_peer != nullptr)`.

---

## 4. Call Tree (Hierarchical)

```
Server::main()
├── signal(SIGINT, signal_handler)              [Server.cpp:196]
├── for loop (iter=0..MAX_LOOP_ITERS)           [Server.cpp:207]
│   ├── g_stop_flag check                       [Server.cpp:209]
│   │   (signal_handler sets g_stop_flag=1 asynchronously)
│   ├── engine.receive()                        [DeliveryEngine]
│   │   └── m_transport->receive_message()      [TcpBackend.cpp]
│   │       ├── m_recv_queue.pop()              [RingBuffer.hpp]
│   │       ├── accept_clients()                [TcpBackend.cpp]
│   │       │   └── socket_accept()             [SocketUtils.cpp]
│   │       └── recv_from_client()              [TcpBackend.cpp]
│   │           ├── tcp_recv_frame()            [SocketUtils.cpp]
│   │           │   └── socket_recv_exact()     [SocketUtils.cpp]
│   │           │       ├── poll(2)             [OS syscall]
│   │           │       └── recv(2)             [OS syscall]
│   │           ├── Serializer::deserialize()
│   │           └── m_recv_queue.push()         [RingBuffer.hpp]
│   ├── engine.pump_retries()                   [DeliveryEngine]
│   │   ├── m_retry_manager.collect_due()
│   │   └── send_via_transport() [loop]
│   │       └── m_transport->send_message()     [TcpBackend.cpp]
│   └── engine.sweep_ack_timeouts()             [DeliveryEngine]
│       └── m_ack_tracker.sweep_expired()
│
├── transport.close()                           [Server.cpp:258]
│   └── TcpBackend::close()                    [TcpBackend.cpp:427]
│       ├── [m_listen_fd>=0] socket_close(m_listen_fd) [SocketUtils.cpp:274]
│       │   ├── NEVER_COMPILED_OUT_ASSERT(fd>=0)
│       │   └── close(fd)                      [OS syscall]
│       └── [loop i=0..MAX_TCP_CONNECTIONS-1]
│           └── socket_close(m_client_fds[i])  [SocketUtils.cpp:274]
│               └── close(fd)                  [OS syscall]
│
├── signal(SIGINT, old_handler)
└── return 0
    └── ~TcpBackend()                          [TcpBackend.cpp:43]
        └── TcpBackend::close()                [TcpBackend.cpp:47, qualified]
            (all fds already -1; no-ops)

--- UdpBackend path ---
UdpBackend::close()                            [UdpBackend.cpp:262]
├── socket_close(m_fd)                         [SocketUtils.cpp:274]
│   └── close(fd)                              [OS syscall]
└── (m_open=false, log INFO)

--- LocalSimHarness path ---
LocalSimHarness::close()
├── m_peer = nullptr
└── (m_open=false, log INFO)
    (no OS calls)
```

---

## 5. Key Components Involved

| Component | File / Location | Role in this flow |
|---|---|---|
| `TcpBackend` | `src/platform/TcpBackend.cpp/.hpp` | Primary resource holder for TCP fds. `close()` releases `m_listen_fd` and all `m_client_fds[]`. Destructor calls `close()` as safety net. |
| `UdpBackend` | `src/platform/UdpBackend.cpp/.hpp` | Holds a single bound UDP socket (`m_fd`). `close()` releases it. Simpler than TCP (no connection loop). |
| `LocalSimHarness` | `src/platform/LocalSimHarness.cpp/.hpp` | In-memory transport. `close()` nulls the peer pointer and clears `m_open`. No OS resources. |
| `socket_close()` | `src/platform/SocketUtils.cpp:274` | Thin POSIX `close(2)` wrapper. `NEVER_COMPILED_OUT_ASSERT(fd >= 0)`. Logs `WARNING_LO` on failure but always returns. |
| `DeliveryEngine` | `src/core/DeliveryEngine.cpp/.hpp` | Holds transport pointer and value-type sub-objects. Has no `close()` method; all in-flight state is abandoned at process exit. |
| `RetryManager` | `src/core/RetryManager.cpp/.hpp` | Fixed slot table (32 entries). Active slots at teardown are abandoned without network notification. |
| `AckTracker` | `src/core/AckTracker.cpp/.hpp` | Tracks PENDING/ACKED/FREE entries. Remaining PENDING entries at teardown are not notified to the application. |
| `RingBuffer` | `src/core/RingBuffer.hpp` | Lock-free SPSC queue embedded in each transport. Not drained during teardown; unread envelopes are abandoned. |
| `g_stop_flag` / `signal_handler` | `Server.cpp:51–59` | `volatile sig_atomic_t` global. Written by signal handler; read by main loop. Only mechanism for SIGINT-triggered shutdown. |

---

## 6. Branching Logic / Decision Points

**Decision 1: Is the stop flag set? (Server.cpp:209)**
- `g_stop_flag != 0` → `break` exits the main for-loop; proceeds to `transport.close()`.
- `g_stop_flag == 0` → continue with receive/pump iteration.

**Decision 2: Has the loop bound been reached?**
- `iter == MAX_LOOP_ITERS` (Server) or `msg_idx > NUM_MESSAGES` (Client) → loop exits naturally; same teardown path.

**Decision 3: Is m_listen_fd >= 0? (TcpBackend::close() line 429)**
- `true` → `socket_close(m_listen_fd)` called (server mode); `m_listen_fd = -1`.
- `false` → skip (client mode, or already closed).

**Decision 4: Is m_client_fds[i] >= 0? (loop over MAX_TCP_CONNECTIONS = 8)**
- `true` → `socket_close(m_client_fds[i])`; fd set to -1.
- `false` → slot already empty; skip.

**Decision 5: Is m_fd >= 0? (UdpBackend::close() line 264)**
- `true` → `socket_close(m_fd)`; `m_fd = -1`.
- `false` → already closed; skip.

**Decision 6: Did close(fd) succeed? (socket_close() line 281)**
- `result >= 0` → silent, normal return.
- `result < 0` → log `WARNING_LO`; function returns anyway (fd treated as closed).

**Decision 7: Is the retry entry expired or exhausted? (RetryManager::collect_due)**
- At deadline or max retries → slot deactivated; not added to output buffer.
- Otherwise → if `next_retry_us <= now_us`, copy to output buffer for final resend.

**Decision 8: Has the AckTracker entry expired? (sweep_expired)**
- `now_us >= deadline_us` on a PENDING slot → slot transitions to FREE; logged at `WARNING_HI`.
- Otherwise → slot remains PENDING (abandoned at exit).

---

## 7. Concurrency / Threading Behavior

### Threads created

None. The entire system is single-threaded. No worker threads are created by any backend, `DeliveryEngine`, `RetryManager`, or `AckTracker`. All receive polling uses `poll(2)` with bounded timeout in the main thread.

### Where context switches occur

The main thread blocks inside `poll(2)` (called from `socket_recv_exact()` or `socket_recv_from()`) for up to `timeout_ms` milliseconds per iteration. SIGINT delivery interrupts this `poll`.

### Synchronization primitives

- **`volatile sig_atomic_t g_stop_flag`:** Written by signal handler; read by main loop. `sig_atomic_t` guarantees atomicity of the write; `volatile` prevents compiler caching. No mutex needed (signal handler and main loop share the same thread).
- **`std::atomic<uint32_t> m_head` and `m_tail`** (`RingBuffer`): SPSC acquire/release ordering. During teardown, these atomics are abandoned with no special synchronization required.
- No mutexes, condition variables, or semaphores are present anywhere in the codebase.

### Producer/consumer relationships

`RingBuffer` is conceptually SPSC. Both producer (`push`) and consumer (`pop`) execute on the same main thread. During teardown, neither push nor pop is called after `m_open = false`.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory

All significant objects are stack-allocated in `main()`:
- `TcpBackend transport` (stack object; lifetime = enclosing `main()` scope).
- `DeliveryEngine engine` (stack object).
- `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`: value members of `DeliveryEngine`.
- `RingBuffer m_recv_queue`, `ImpairmentEngine m_impairment`: value members of each backend.
- `MessageEnvelope m_buf[MSG_RING_CAPACITY = 64]` (`RingBuffer`): inline array; 64 × (4096 + overhead) bytes.
- `RetryEntry m_slots[ACK_TRACKER_CAPACITY = 32]` (`RetryManager`): 32 full `MessageEnvelope` copies.
- `AckTracker::Entry m_slots[ACK_TRACKER_CAPACITY = 32]` (`AckTracker`): 32 slots.
- `uint8_t m_wire_buf[SOCKET_RECV_BUF_BYTES = 8192]` (backend): inline serialization scratch buffer.

No `malloc`/`new` is used on critical paths (Power of 10 rule 3).

### Lifetime of key objects

All objects are alive from initialization through `return` from `main()`. No object is created or destroyed during the steady-state loop. File descriptors (stored as `int`) are the only external OS resources requiring explicit release; `TcpBackend::close()` releases them.

### Stack vs heap usage

Entirely stack-based. No heap allocation after init.

### RAII usage

- `TcpBackend::~TcpBackend()` calls `TcpBackend::close()` with a qualified call (TcpBackend.cpp:47). This is the RAII cleanup point. Qualified call prevents virtual dispatch in the destructor.
- `UdpBackend::~UdpBackend()` calls `close()` (UdpBackend.cpp:39–43).
- `LocalSimHarness::~LocalSimHarness()` calls `close()` (LocalSimHarness.cpp:35–38).
- `RingBuffer` compiler-generated destructor destroys two `std::atomic<uint32_t>` members. No special cleanup needed.
- `DeliveryEngine` has no explicit destructor. Sub-objects use compiler-generated destructors.

### Potential leaks or unsafe patterns

- **Double-close safety:** `TcpBackend::close()` is called explicitly at teardown and again implicitly by `~TcpBackend()`. The second call is safe because all fds are set to `-1` after the first close.
- **Abandoned in-flight messages:** `MessageEnvelope` objects in `RingBuffer`, `RetryManager`, and `AckTracker` at exit are simply discarded. RELIABLE_RETRY messages mid-flight at SIGINT time are not retried and not notified to the application layer.
- **Signal handler safety:** The handler writes only to a `sig_atomic_t` global. It does not call Logger, malloc, or any async-signal-unsafe functions.

---

## 9. Error Handling Flow

### How errors propagate

- `socket_close(fd)` logs `WARNING_LO` if `close(2)` fails but does not return an error to the caller (`TcpBackend::close()` has `void` return type). At teardown, there is no sensible recovery from a `close()` failure.
- `TcpBackend::close()` and `UdpBackend::close()` both have `void` return type. No errors from close are propagated.
- `LocalSimHarness::close()` makes no OS calls; no errors possible.

### Retry logic

During the final `pump_retries()` call (before teardown), `RetryManager::collect_due()` identifies messages due for retry. `DeliveryEngine::pump_retries()` calls `send_via_transport()` for each. If `send_via_transport()` returns non-OK (e.g., connection already disrupted), `WARNING_LO` is logged; the send failure is not fatal. No further retry occurs because teardown follows immediately.

### Fallback paths

- `TcpBackend::close()` calls raw `close(2)` on each fd — no `shutdown(2)` first. The kernel sends a FIN but any data in the TCP send buffer may be lost depending on `SO_LINGER` state (not set; default applies).
- If `close(2)` fails with `EBADF`, the fd is still set to `-1`. No re-try of the close.
- No graceful drain of the ring buffer before closing.

---

## 10. External Interactions

### Network calls

- `close(2)` on each TCP fd: sends FIN to remote peer via kernel TCP stack. Up to `MAX_TCP_CONNECTIONS + 1 = 9` calls for server mode; 1 call for client mode; 1 call for `UdpBackend`.
- `poll(2)` and `send(2)` / `recv(2)` during final `pump_retries()`: bounded by number of due retry slots (0 to 32).
- No `shutdown(2)` syscall. No explicit `SO_LINGER` configuration.

### File I/O

- `Logger::log()` is called at multiple teardown points. [ASSUMPTION: Logger writes to stdout/stderr or a file.]

### Hardware interaction

None directly. Socket operations go through the kernel network stack.

### IPC

None. Server and Client communicate via TCP sockets, closed during teardown as described. No shared memory, pipes, or message queues.

---

## 11. State Changes / Side Effects

| Object / Field | Before teardown | After teardown |
|---|---|---|
| `m_listen_fd` | valid fd | `-1` |
| `m_client_fds[i]` | valid fd or `-1` | all `-1` |
| `m_client_count` | 0..8 | `0U` |
| `m_open` | `true` | `false` |
| `m_fd` (UdpBackend) | valid fd | `-1` |
| `m_peer` (LocalSimHarness) | valid pointer | `nullptr` |
| `g_stop_flag` | `1` (if SIGINT) | remains `1` (not reset) |
| `AckTracker::m_slots[]` | any PENDING/ACKED entries | abandoned in memory |
| `RetryManager::m_slots[]` | any active entries | abandoned in memory |
| `RingBuffer::m_buf[]` | any queued envelopes | abandoned in memory |
| OS kernel TCP connection | ESTABLISHED | TIME_WAIT / CLOSE_WAIT |
| OS kernel UDP socket | bound | released |

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant OS_Signal as OS / Signal
    participant Server as Server main
    participant DE as DeliveryEngine
    participant TB as TcpBackend
    participant SU as SocketUtils
    participant OS as OS Kernel

    OS_Signal->>Server: SIGINT → signal_handler sets g_stop_flag=1
    Note over Server: poll(2) returns EINTR
    Server->>DE: engine.receive() → ERR_TIMEOUT
    Note over Server: top of loop: g_stop_flag != 0 → break

    Server->>DE: engine.pump_retries(now_us)
    DE->>TB: send_message() [final retry sends]
    TB->>SU: tcp_send_frame()
    SU->>OS: send(2)

    Server->>DE: engine.sweep_ack_timeouts(now_us)

    Server->>TB: transport.close()
    TB->>SU: socket_close(m_listen_fd)
    SU->>OS: close(listen_fd)
    OS-->>SU: 0
    loop i=0..MAX_TCP_CONNECTIONS-1
        TB->>SU: socket_close(m_client_fds[i])
        SU->>OS: close(client_fd)
        OS-->>SU: 0
    end
    Note over TB: m_client_count=0; m_open=false; log INFO "Transport closed"
    TB-->>Server: void

    Note over Server: return 0 → stack unwind
    Note over TB: ~TcpBackend() → close() [no-op, all fds=-1]
    Note over DE: ~DeliveryEngine() [compiler-generated; no I/O]
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup

- `main()` fills `TransportConfig` and `ChannelConfig` structs (stack-allocated).
- `TcpBackend::init()` creates sockets, sets `m_open = true`, initializes `m_recv_queue.init()`, initializes `m_impairment.init()`.
- `DeliveryEngine::init()` stores the transport pointer; calls `m_ack_tracker.init()`, `m_retry_manager.init()`, `m_dedup.init()`, `m_id_gen.init(seed)`; sets `m_initialized = true`.
- `signal(SIGINT, signal_handler)` registers the handler.
- All allocation happens here. No allocation occurs during the main loop.

### What happens during steady-state execution

- The main loop runs up to `MAX_LOOP_ITERS` iterations. Each iteration: checks stop flag; calls `engine.receive()`; if data: echoes or processes; calls `engine.pump_retries()`; calls `engine.sweep_ack_timeouts()`.
- No allocation. All `MessageEnvelope` objects passed by value or written into pre-allocated static arrays.

---

## 14. Known Risks / Observations

**Risk 1: No graceful TCP drain before close**
`TcpBackend::close()` calls raw `close(2)` on each fd without a prior `shutdown(SHUT_WR)`. The peer may receive an abrupt RST rather than a clean FIN+ACK if there is unread data in the receive buffer. No explicit half-close sequence.

**Risk 2: SIGINT during tcp_send_frame() — partial frame**
If SIGINT arrives while `send(2)` or `poll(2)` is executing inside `socket_send_all()`, these syscalls return -1/EINTR. `socket_send_all()` does not handle `EINTR` retry; it returns false immediately. This may deliver a partial frame header or payload to the peer, causing the peer's `tcp_recv_frame()` to fail on the next receive.

**Risk 3: No SIGPIPE handling**
If the peer closes its socket before the server finishes sending an echo reply, `send(2)` may generate SIGPIPE. The default SIGPIPE action is process termination. There is no `signal(SIGPIPE, SIG_IGN)` call anywhere in the code. This could cause abrupt termination rather than clean shutdown.

**Risk 4: Abandoned in-flight RELIABLE_RETRY messages**
Any RELIABLE_RETRY messages still in `RetryManager` slots at teardown are never retried and never notified to the application layer. The design has no flush-before-close protocol.

**Risk 5: DeliveryEngine holds a dangling raw pointer after backend goes out of scope**
`DeliveryEngine` holds a raw `TransportInterface*` to the backend. If the backend object goes out of scope before `DeliveryEngine`, the pointer becomes dangling. In the current code this cannot happen (both are on the same stack), but the API does not enforce this ordering.

**Risk 6: Duplicate "Transport closed" log**
`TcpBackend::close()` is called explicitly at Server.cpp:258 and again implicitly by `~TcpBackend()`. This emits a duplicate `Logger::log(INFO, "TcpBackend", "Transport closed")` message. Harmless but misleading in log analysis.

**Risk 7: ImpairmentEngine delay buffer abandoned**
Any messages held in `m_impairment.m_delay_buf[]` at shutdown are abandoned without delivery. No drain occurs during `TcpBackend::close()`.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `Logger::log()` implementation is not provided. Assumed to write to stdout/stderr or a file. Assumed to be async-signal-unsafe (correctly avoided in the signal handler).

- [ASSUMPTION] Default `SO_LINGER` state is in effect on all TCP sockets. The kernel will attempt to flush pending send data on `close(2)`, but the application has no way to wait for or confirm this.

- [CONFIRMED] `ImpairmentEngine::process_outbound()` and `collect_deliverable()` operate on a fixed internal delay buffer of `IMPAIR_DELAY_BUF_SIZE = 32` slots. Any messages held there at shutdown are abandoned; no explicit drain occurs during `TcpBackend::close()`.

- [ASSUMPTION] `DuplicateFilter::check_and_record()` and `MessageIdGen::next()` implementations are not provided. Their teardown behavior (no explicit cleanup) is assumed safe.

- [ASSUMPTION] `envelope_init()`, `envelope_copy()`, `envelope_valid()` are inline utility functions in `MessageEnvelope.hpp` operating on the struct by value. Confirmed.

- [ASSUMPTION] `timestamp_now_us()`, `timestamp_deadline_us()`, `timestamp_expired()` are POSIX clock-based, not async-signal-safe, and not called from the signal handler.

- [UNKNOWN] Whether `Logger::log()` can be called safely inside `~TcpBackend()` after `main()` returns (i.e., during static destruction order). If Logger uses any global state destroyed before TcpBackend, the destructor's log call could access destroyed state.

- [UNKNOWN] The exact behavior of `poll(2)` returning after SIGINT on the target platform. On Linux, `poll()` returns -1 with `EINTR`; on macOS it may return 0 (timeout). `socket_recv_exact()` treats `poll <= 0` as timeout/error in both cases.

- [UNKNOWN] Whether any OS-level TCP send buffer data is lost when `close(2)` is called without a prior `shutdown(SHUT_WR)`. Platform- and state-dependent.

- [ASSUMPTION] `old_handler` at `signal(SIGINT, signal_handler)` correctly captures `SIG_DFL`. The assertion `old_handler != SIG_ERR` (if present) would fire on a severe OS-level error.
