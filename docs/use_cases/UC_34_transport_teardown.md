## 1. Use Case Overview

### Clear description of what triggers this flow

Transport teardown is triggered by one of two events: (1) the user presses Ctrl+C (SIGINT) while the Server or Client process is running, which sets a volatile stop flag that the main loop detects on the next iteration; or (2) the application reaches the natural end of its operational sequence (Client sends all N messages, Client.cpp:281 calls `transport.close()` explicitly; Server exits the bounded loop at MAX_LOOP_ITERS and calls `transport.close()` at Server.cpp:258). In both cases the final act is an explicit call to `TcpBackend::close()`, `UdpBackend::close()`, or `LocalSimHarness::close()` depending on which transport was in use. The `DeliveryEngine` has no `close()` method of its own; it holds pointers and value-type sub-objects but performs no teardown step. The transport backends' destructors also call `close()` idempotently when the stack-allocated objects go out of scope at process exit.

### Expected outcome

All file descriptors held by the transport backend are closed via POSIX `close(2)`. In-flight messages sitting in the RingBuffer (inbound receive queue), the RetryManager slot table, and the AckTracker slot table are abandoned in place — no flush-to-wire occurs. The `m_open` flag on the backend is set to false. Logger emits an INFO-level teardown event. The process exits with a code reflecting whether the operation succeeded (0) or encountered errors (non-zero, Client only).

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **SIGINT path (Server):** Kernel delivers SIGINT to the process. The OS invokes `signal_handler()` (Server.cpp:55-59), which writes `g_stop_flag = 1` (a `volatile sig_atomic_t`). This is not a function call from the main thread; it is an asynchronous signal delivery. The main loop at Server.cpp:207-253 reads `g_stop_flag` at the top of each iteration (Server.cpp:209) and breaks when it is non-zero.

- **Natural exit path (Client):** After the bounded for-loop at Client.cpp:240-260 completes all `NUM_MESSAGES` (= 5) iterations, execution falls through to the final pump at Client.cpp:265-276, then reaches `transport.close()` at Client.cpp:281 directly.

- **Natural exit path (Server):** If `g_stop_flag` is never set, the loop exits when `iter` reaches `MAX_LOOP_ITERS` (= 100000). Execution falls through to `transport.close()` at Server.cpp:258.

- **Destructor path:** If either process exits via `return` from `main()`, the stack-allocated `TcpBackend transport` object (Server.cpp:176, Client.cpp:216) goes out of scope. `TcpBackend::~TcpBackend()` (TcpBackend.cpp:39-42) calls `close()` again. Since `close()` checks `m_fd >= 0` and `m_listen_fd >= 0` before acting, the double-close is safe (idempotent for fd = -1).

---

## 3. End-to-End Control Flow (Step-by-Step)

**SIGINT -> Server shutdown path (primary trace):**

1. User presses Ctrl+C. The kernel sends SIGINT to the server process.

2. The kernel interrupts the server's current blocking call (most likely inside `TcpBackend::receive_message()` -> `recv_from_client()` -> `tcp_recv_frame()` -> `socket_recv_exact()` -> `poll(2)` at SocketUtils.cpp:355). `poll()` returns -1 with errno=EINTR or returns 0 (timeout), depending on platform behavior. Either way `socket_recv_exact()` returns false, propagating up to `recv_from_client()` returning `Result::ERR_IO`, propagating to `receive_message()` returning `Result::ERR_TIMEOUT` after exhausting poll iterations (TcpBackend.cpp:379).

3. Before or after poll returns, the OS invokes `signal_handler()` (Server.cpp:55-59) in the signal context. `g_stop_flag = 1` is written atomically as `sig_atomic_t`.

4. `engine.receive()` (DeliveryEngine.cpp:159) receives `Result::ERR_TIMEOUT` from the transport and returns it to the main loop at Server.cpp:222.

5. The main loop at Server.cpp:207 checks `g_stop_flag != 0` at Server.cpp:209 at the top of the next iteration. The condition is true. Logger emits "Stop flag set; exiting loop" (Server.cpp:210). `break` exits the for-loop.

6. Execution reaches Server.cpp:258: `transport.close()` is called directly on the stack-allocated `TcpBackend` object.

7. **TcpBackend::close()** (TcpBackend.cpp:386-404) executes:
   - a. Checks `m_listen_fd >= 0`. If true (server mode), calls `socket_close(m_listen_fd)` (SocketUtils.cpp:272-284) which calls POSIX `close(m_listen_fd)`. Sets `m_listen_fd = -1`.
   - b. Enters a fixed loop over `MAX_TCP_CONNECTIONS` (= 8) slots. For each slot where `m_client_fds[i] >= 0`, calls `socket_close(m_client_fds[i])` (POSIX `close()`), then sets `m_client_fds[i] = -1`.
   - c. Sets `m_client_count = 0U`.
   - d. Sets `m_open = false`.
   - e. Calls `Logger::log(Severity::INFO, "TcpBackend", "Transport closed")`.

8. **socket_close()** (SocketUtils.cpp:272-284) for each fd: asserts `fd >= 0`, calls `close(fd)`. If `close()` returns < 0, logs `WARNING_LO` but continues — the fd is treated as closed regardless.

9. Control returns to Server.cpp:259-261. Logger emits the final statistics message.

10. `signal(SIGINT, old_handler)` restores the original handler (Server.cpp:264).

11. `main()` returns 0. Stack unwinds. `TcpBackend::~TcpBackend()` (TcpBackend.cpp:39-42) is called. It calls `close()` again. `m_listen_fd == -1` so the `if (m_listen_fd >= 0)` branch is skipped. All `m_client_fds[i] == -1` so the loop body is never entered. `m_open` is already false. Logger emits "Transport closed" again (harmless duplicate).

12. `DeliveryEngine engine` goes out of scope. Its destructor is compiler-generated (no explicit destructor defined in DeliveryEngine.hpp). Sub-objects `m_ack_tracker`, `m_retry_manager`, `m_dedup`, `m_id_gen` are all stack-allocated value types with trivial or compiler-generated destructors. Their in-memory state is simply discarded. No network I/O occurs. All pending entries in `m_ack_tracker.m_slots[]` and `m_retry_manager.m_slots[]` are abandoned without notification.

13. `RingBuffer m_recv_queue` (embedded in `TcpBackend`) goes out of scope. Its destructor releases the two `std::atomic<uint32_t>` members. Any `MessageEnvelope` objects still in `m_buf[]` are abandoned.

**Client natural shutdown path (secondary trace):**

14. Client.cpp:265: `engine.pump_retries(final_now_us)` is called. This calls `m_retry_manager.collect_due()` (RetryManager.cpp:136), which scans all active slots, checks expiry and retry counts, and copies due-for-retry envelopes into a stack buffer. For each collected envelope, `send_via_transport()` (DeliveryEngine.cpp:54-69) calls `m_transport->send_message()`. Since the client still has `m_open = true` at this point, the send proceeds through `TcpBackend::send_message()` normally.

15. Client.cpp:267: `engine.sweep_ack_timeouts(final_now_us)` is called. This calls `m_ack_tracker.sweep_expired()` (AckTracker.cpp:142-194), which scans all slots, transitions expired PENDING entries to FREE (decrementing `m_count`), transitions ACKED entries to FREE. No network I/O is performed.

16. Client.cpp:281: `transport.close()` is called. TcpBackend::close() executes identically to steps 7-8 above, except `m_listen_fd == -1` (client mode has no listen socket), so only the `m_client_fds[0]` fd (the single connection) is closed.

17. Client.cpp:287: exit code computed, `main()` returns. Destructor path proceeds as in step 11-13.

**UdpBackend::close() path (if UDP transport in use):**

18. `UdpBackend::close()` (UdpBackend.cpp:248-257): checks `m_fd >= 0`. Calls `socket_close(m_fd)`. Sets `m_fd = -1`. Sets `m_open = false`. Calls `Logger::log(INFO, "UdpBackend", "Transport closed")`. No loop over client fds (UDP is connectionless; there is only one bound socket).

**LocalSimHarness::close() path (if sim harness in use):**

19. `LocalSimHarness::close()` (LocalSimHarness.cpp:187-192): sets `m_peer = nullptr`. Sets `m_open = false`. Calls `Logger::log(INFO, "LocalSimHarness", "Transport closed")`. No socket fds to close. Any messages remaining in the peer's `m_recv_queue` are silently abandoned. After `m_peer = nullptr`, any subsequent call to `send_message()` on this harness would assert-fail on the `assert(m_peer != nullptr)` pre-condition (LocalSimHarness.cpp:107).

---

## 4. Call Tree (Hierarchical)

```
Server::main()
|
+-- signal(SIGINT, signal_handler)          [Server.cpp:196]
|
+-- for loop (iter=0..MAX_LOOP_ITERS)       [Server.cpp:207]
|   |
|   +-- g_stop_flag check                  [Server.cpp:209]
|   |     (signal_handler sets g_stop_flag=1 asynchronously)
|   |
|   +-- engine.receive()                   [DeliveryEngine.cpp:147]
|   |   +-- m_transport->receive_message() [TcpBackend.cpp:325]
|   |   |   +-- m_recv_queue.pop()         [RingBuffer.hpp:159]
|   |   |   +-- accept_clients()           [TcpBackend.cpp:161]
|   |   |   |   +-- socket_accept()        [SocketUtils.cpp:250]
|   |   |   +-- recv_from_client()         [TcpBackend.cpp:193]
|   |   |   |   +-- tcp_recv_frame()       [SocketUtils.cpp:429]
|   |   |   |   |   +-- socket_recv_exact()[SocketUtils.cpp:337]
|   |   |   |   |       +-- poll(2)        [OS syscall]
|   |   |   |   |       +-- recv(2)        [OS syscall]
|   |   |   |   +-- Serializer::deserialize()
|   |   |   |   +-- m_recv_queue.push()    [RingBuffer.hpp:127]
|   |   |   +-- m_recv_queue.pop()         [RingBuffer.hpp:159]
|   |   +-- timestamp_expired()
|   |   +-- m_ack_tracker.on_ack()         [AckTracker.cpp:81]
|   |   +-- m_retry_manager.on_ack()       [RetryManager.cpp:97]
|   |   +-- m_dedup.check_and_record()
|   |
|   +-- engine.pump_retries()              [DeliveryEngine.cpp:225]
|   |   +-- m_retry_manager.collect_due()  [RetryManager.cpp:136]
|   |   +-- send_via_transport() [loop]    [DeliveryEngine.cpp:54]
|   |       +-- m_transport->send_message()[TcpBackend.cpp:249]
|   |           +-- Serializer::serialize()
|   |           +-- m_impairment.process_outbound()
|   |           +-- tcp_send_frame() [loop][SocketUtils.cpp:391]
|   |               +-- socket_send_all()  [SocketUtils.cpp:290]
|   |                   +-- poll(2)        [OS syscall]
|   |                   +-- send(2)        [OS syscall]
|   |
|   +-- engine.sweep_ack_timeouts()        [DeliveryEngine.cpp:267]
|       +-- m_ack_tracker.sweep_expired()  [AckTracker.cpp:142]
|
+-- transport.close()                      [Server.cpp:258]
|   \-- TcpBackend::close()                [TcpBackend.cpp:386]
|       +-- socket_close(m_listen_fd)      [SocketUtils.cpp:272]
|       |   +-- close(fd)                  [OS syscall]
|       +-- [loop i=0..MAX_TCP_CONNECTIONS]
|           +-- socket_close(m_client_fds[i]) [SocketUtils.cpp:272]
|               +-- close(fd)              [OS syscall]
|
+-- signal(SIGINT, old_handler)            [Server.cpp:264]
+-- return 0
    \-- ~TcpBackend()                      [TcpBackend.cpp:39]
        \-- TcpBackend::close()            [TcpBackend.cpp:386]
            (all fds already -1; no-ops)

--- UdpBackend::close() [if UDP transport] ---
UdpBackend::close()                        [UdpBackend.cpp:248]
+-- socket_close(m_fd)                     [SocketUtils.cpp:272]
|   +-- close(fd)                          [OS syscall]
\-- (m_open = false, log)

--- LocalSimHarness::close() [if sim transport] ---
LocalSimHarness::close()                   [LocalSimHarness.cpp:187]
+-- m_peer = nullptr
\-- (m_open = false, log)
    (no sockets, no OS calls)
```

---

## 5. Key Components Involved

### TcpBackend (TcpBackend.cpp / TcpBackend.hpp)

**Responsibility:** Owns all TCP file descriptors — one listening socket (`m_listen_fd`) and up to `MAX_TCP_CONNECTIONS` (8) client connection fds (`m_client_fds[]`). Responsible for closing all of them in `close()`. Also owns `m_recv_queue` (a RingBuffer) and `m_impairment` (ImpairmentEngine), both value-type members.

**Why it exists in this flow:** It is the primary resource holder for TCP sockets. `close()` is the single site where all TCP fds are surrendered back to the OS. The destructor delegates to `close()` to ensure cleanup even if the caller forgets.

### UdpBackend (UdpBackend.cpp / UdpBackend.hpp)

**Responsibility:** Owns a single bound UDP socket (`m_fd`). `close()` releases it. Does not track individual connections.

**Why it exists in this flow:** The UDP equivalent of TcpBackend. Its close path is simpler (one fd, no loop).

### LocalSimHarness (LocalSimHarness.cpp / LocalSimHarness.hpp)

**Responsibility:** In-memory transport with no real sockets. Holds a raw pointer to a peer harness (`m_peer`). `close()` nulls the peer pointer and clears `m_open`.

**Why it exists in this flow:** The simulation transport must be torn down cleanly to prevent the peer from calling `inject()` into a closed harness's RingBuffer. Nulling `m_peer` stops new sends from the calling side. [ASSUMPTION: the peer harness's `close()` is called separately by the test harness; there is no automatic bilateral teardown.]

### socket_close() (SocketUtils.cpp:272-284)

**Responsibility:** Thin wrapper around POSIX `close(2)`. Asserts `fd >= 0`. Logs `WARNING_LO` if `close()` fails but does not propagate the error (the fd is considered released regardless).

**Why it exists in this flow:** All fd release flows through this function, providing a consistent logging and assertion point.

### DeliveryEngine (DeliveryEngine.cpp / DeliveryEngine.hpp)

**Responsibility:** Coordinates ACK tracking, retry scheduling, and duplicate suppression. Does not own any sockets and has no `close()` method.

**Why it exists in this flow:** The DeliveryEngine's in-flight state (RetryManager slots, AckTracker slots, RingBuffer) is entirely abandoned at shutdown with no notification, flush, or drain. This is relevant to understanding what happens to pending messages.

### RetryManager (RetryManager.cpp / RetryManager.hpp)

**Responsibility:** Manages a fixed table of `ACK_TRACKER_CAPACITY` (32) retry slots. At shutdown, any active slots (messages not yet ACKed, not yet expired) remain active in memory but will never be acted upon again.

**Why it exists in this flow:** At the final `pump_retries()` call (Client.cpp:266, Server.cpp:243 on each iteration), due-for-retry messages are sent one last time. Any remaining active slots after the loop breaks are silently dropped at process exit.

### AckTracker (AckTracker.cpp / AckTracker.hpp)

**Responsibility:** Tracks outstanding messages in a PENDING/ACKED/FREE state machine. `sweep_expired()` transitions timed-out PENDING entries to FREE and returns their envelopes. At shutdown, any remaining PENDING entries are abandoned.

**Why it exists in this flow:** The final `sweep_ack_timeouts()` call clears any entries whose ACK deadline has passed. Any entries not yet at their deadline when the process exits are never notified to the application layer.

### RingBuffer (RingBuffer.hpp)

**Responsibility:** Lock-free SPSC ring buffer holding inbound `MessageEnvelope` objects. Embedded by value in each transport backend. At backend `close()`, the buffer is not drained — any messages queued but not yet consumed by the application are lost.

**Why it exists in this flow:** The ring buffer is not explicitly flushed or drained during teardown. Its `m_head` and `m_tail` atomics are simply abandoned when the owning transport object goes out of scope.

### g_stop_flag / signal_handler (Server.cpp:51-59)

**Responsibility:** `g_stop_flag` is a `volatile sig_atomic_t` global. The signal handler writes `1` to it. The main loop checks it at the top of each iteration. This is the sole mechanism by which SIGINT causes the server to exit.

**Why it exists in this flow:** POSIX signal handlers cannot call most library functions safely. The write to `sig_atomic_t` is the approved minimal pattern. [ASSUMPTION: No mutex protects `g_stop_flag`; signal delivery and the main loop's read are on the same thread, so no data race exists.]

---

## 6. Branching Logic / Decision Points

### Decision 1: Is the stop flag set?
- **Condition:** `g_stop_flag != 0` (Server.cpp:209)
- **Outcome if true:** `break` exits the main for-loop; proceeds to `transport.close()`
- **Outcome if false:** Continue with receive/echo/pump iteration
- **Where control goes:** Server.cpp:258 (`transport.close()`)

### Decision 2: Has the loop bound been reached?
- **Condition:** `iter == MAX_LOOP_ITERS` (Server) or `msg_idx > NUM_MESSAGES` (Client)
- **Outcome:** Loop exits naturally; same teardown path as stop-flag path
- **Where control goes:** Server.cpp:258 or Client.cpp:265

### Decision 3: Is m_listen_fd >= 0?
- **Condition:** TcpBackend::close() line 388
- **Outcome if true:** `socket_close(m_listen_fd)` is called (server mode has a listen socket)
- **Outcome if false:** Skip (client mode, or already closed)
- **Where control goes:** The client-fd loop at TcpBackend.cpp:394

### Decision 4: Is m_client_fds[i] >= 0? (loop over MAX_TCP_CONNECTIONS)
- **Condition:** TcpBackend::close() line 395 for each i in [0, MAX_TCP_CONNECTIONS)
- **Outcome if true:** `socket_close(m_client_fds[i])` called; fd set to -1
- **Outcome if false:** Slot was already empty; skip
- **Where control goes:** Next slot, or after loop: m_client_count = 0, m_open = false

### Decision 5: Is m_fd >= 0? (UdpBackend)
- **Condition:** UdpBackend::close() line 250
- **Outcome if true:** `socket_close(m_fd)` called; m_fd set to -1
- **Outcome if false:** Already closed; skip
- **Where control goes:** m_open = false, log

### Decision 6: Did close(fd) succeed?
- **Condition:** `socket_close()` line 278: `result = close(fd)` returns < 0
- **Outcome if true (success):** Silent, normal return
- **Outcome if false (failure):** `WARNING_LO` logged; function returns anyway (fd considered closed)
- **Where control goes:** Caller continues regardless

### Decision 7: Is the retry entry expired or exhausted? (RetryManager::collect_due)
- **Condition:** RetryManager.cpp:156 (`now_us >= expiry_us`) or line 166 (`retry_count >= max_retries`)
- **Outcome if true:** Slot deactivated, m_count decremented; entry not added to output buffer
- **Outcome if false:** If `next_retry_us <= now_us`, copy to output buffer for resend; otherwise skip this iteration

### Decision 8: Has the AckTracker entry expired? (sweep_expired)
- **Condition:** AckTracker.cpp:163 (`now_us >= deadline_us`) on a PENDING slot
- **Outcome if true:** Slot transitioned to FREE, envelope copied to caller's buffer, logged as WARNING_HI
- **Outcome if false:** Slot remains PENDING

### Decision 9: Is m_open true at the start of close()?
- **Condition:** Neither TcpBackend::close() nor UdpBackend::close() asserts `m_open` at entry. The fd guards (`>= 0`) serve as the effective idempotency check.
- **Outcome:** No assertion fires on a double-close; the fd checks silently skip already-closed resources.

---

## 7. Concurrency / Threading Behavior

### Threads created

No worker threads are created by TcpBackend, UdpBackend, LocalSimHarness, DeliveryEngine, RetryManager, or AckTracker. The entire system runs single-threaded in the main loop. All receive polling is done by blocking/polling within the main thread (poll(2) with bounded timeout).

### Where context switches occur

The main thread blocks inside `poll(2)` (called from `socket_recv_exact()` or `socket_recv_from()`) for up to `timeout_ms` milliseconds per iteration. This is the only blocking point. SIGINT delivery interrupts this poll.

### Synchronization primitives

- **`volatile sig_atomic_t g_stop_flag`** (Server.cpp:51): The only cross-context synchronization. Written by the signal handler; read by the main loop. `sig_atomic_t` guarantees atomicity of the write. `volatile` prevents the compiler from caching the read. No mutex is needed because signal handler and main loop share the same thread.

- **`std::atomic<uint32_t> m_head` and `m_tail`** (RingBuffer.hpp:209-210): Used for SPSC lock-free access. However, since the entire system is single-threaded (no separate producer/consumer threads), these atomics are technically used from a single thread. The acquire/release ordering still provides the correct memory visibility semantics even in a single-threaded context, at negligible cost. During teardown, these atomics are simply abandoned with no special synchronization needed.

- **No mutexes, no condition variables, no semaphores** are present anywhere in the codebase.

### Producer/consumer relationships

The RingBuffer in each transport backend is conceptually SPSC:
- **Producer role:** The `recv_from_client()` or `socket_recv_from()` path calls `m_recv_queue.push()`.
- **Consumer role:** `receive_message()` calls `m_recv_queue.pop()`.
In practice both roles execute on the same main thread. During teardown, neither push nor pop is called after `close()` sets `m_open = false`.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### Who owns allocated memory

All significant objects are stack-allocated in `main()`:
- `TcpBackend transport` (Server.cpp:176, Client.cpp:216): stack object; lifetime is the enclosing `main()` scope.
- `DeliveryEngine engine` (Server.cpp:188, Client.cpp:228): stack object.
- `AckTracker m_ack_tracker`, `RetryManager m_retry_manager`, `DuplicateFilter m_dedup`, `MessageIdGen m_id_gen`: all value members of `DeliveryEngine`; lifetime tied to `engine`.
- `RingBuffer m_recv_queue`, `ImpairmentEngine m_impairment`: value members of each backend; lifetime tied to the backend.
- `MessageEnvelope m_buf[MSG_RING_CAPACITY]` (RingBuffer.hpp:204): inline array within RingBuffer; 64 MessageEnvelope objects, each containing a 4096-byte payload array. Total RingBuffer data: 64 * (4096 + overhead) bytes on the stack.
- `RetryEntry m_slots[ACK_TRACKER_CAPACITY]` (RetryManager.hpp:99): 32 slots, each containing a full MessageEnvelope copy.
- `AckTracker::Entry m_slots[ACK_TRACKER_CAPACITY]` (AckTracker.hpp:78): 32 slots.
- `uint8_t m_wire_buf[SOCKET_RECV_BUF_BYTES]` (TcpBackend.hpp:55): 8192-byte serialization scratch buffer, stack-allocated as a member of TcpBackend.

No dynamic allocation (malloc/new) is used anywhere on critical paths; this is explicitly enforced by Power of 10 rule 3.

### Lifetime of key objects

- All objects listed above are alive from initialization through `return` from `main()`. No object is created or destroyed during the steady-state loop.
- File descriptors (integers) stored in `m_listen_fd` and `m_client_fds[]` are OS resources. They are the only external resources that require explicit release. `TcpBackend::close()` releases them. The destructor provides a safety net.

### Stack vs heap usage

- Entirely stack-based. No heap allocation after init. The MessageEnvelope copies in RetryManager and AckTracker are value-type copies stored inline in their respective slot arrays.

### RAII usage (constructors/destructors)

- `TcpBackend::~TcpBackend()` calls `close()` (TcpBackend.cpp:39-42). This is the primary RAII cleanup point.
- `UdpBackend::~UdpBackend()` calls `close()` (UdpBackend.cpp:36-39).
- `LocalSimHarness::~LocalSimHarness()` calls `close()` (LocalSimHarness.cpp:35-38).
- `RingBuffer` has a compiler-generated destructor that destroys the two `std::atomic<uint32_t>` members. No special cleanup needed.
- `DeliveryEngine` has no explicit destructor. Its sub-objects use compiler-generated destructors.

### Potential leaks or unsafe patterns

- **Double-close safety:** `TcpBackend::close()` is called explicitly at Server.cpp:258 and again implicitly by `~TcpBackend()`. The second call is safe because all fds are set to -1 after the first close. However, `is_open()` (TcpBackend.cpp:410-414) asserts `m_open == (m_listen_fd >= 0 || m_client_count > 0U) || !m_open`. After the explicit close, `m_open = false`, so the `|| !m_open` guard passes. No issue.

- **Abandoned in-flight messages:** Any `MessageEnvelope` objects still in RingBuffer, RetryManager, or AckTracker at the time of process exit are simply discarded. This is by design (no flush protocol), but means that RELIABLE_RETRY messages that were mid-flight at SIGINT time are not retried and not notified to the application layer.

- **signal_handler safety:** The handler writes only to a `sig_atomic_t` global. It does not call Logger, malloc, or any async-signal-unsafe functions. [ASSUMPTION: `Logger::log()` would be unsafe in a signal context; the design correctly avoids it here.]

---

## 9. Error Handling Flow

### How errors propagate

- `socket_close(fd)` logs `WARNING_LO` if `close(2)` fails but does not return an error to the caller (TcpBackend::close() has `void` return type). This is an intentional design choice: at teardown, there is no sensible recovery from a `close()` failure.
- `TcpBackend::close()` itself has `void` return type. No errors from close are propagated.
- `UdpBackend::close()` same pattern.
- `LocalSimHarness::close()` has no OS calls; cannot produce errors.

### Retry logic

- During the final `pump_retries()` call (before teardown): `RetryManager::collect_due()` identifies messages due for retry. `DeliveryEngine::pump_retries()` calls `send_via_transport()` for each. If `send_via_transport()` returns non-OK (e.g., the connection was already disrupted), `WARNING_LO` is logged (DeliveryEngine.cpp:248). The send failure is not fatal; the message is simply not retried again because teardown follows immediately.
- After `transport.close()`, the `m_open = false` flag means any future `send_message()` call would fire `assert(m_open)` (TcpBackend.cpp:251). No such call is made post-close in the current code.

### Fallback paths

- If `socket_close()` fails and errno indicates the fd was already closed (EBADF), the code still sets the fd to -1 and continues. There is no re-try of the close.
- There is no graceful TCP FIN/ACK flush: `TcpBackend::close()` calls raw `close(2)` on each fd. The OS kernel will send a FIN, but any data in the kernel's TCP send buffer that has not yet been transmitted may be lost (RST or FIN depending on SO_LINGER state, which is not set). [ASSUMPTION: default SO_LINGER behavior applies; the kernel attempts to flush pending TCP data in the background, but the application has no visibility into whether this succeeds.]
- No `shutdown(2)` (with `SHUT_WR`) is called before `close(2)`. This means there is no explicit half-close sequence. [OBSERVATION: This is a potential issue if in-flight TCP data in the kernel's send buffer needs to be delivered to the peer before the connection closes.]

---

## 10. External Interactions

### Network calls

- `close(2)` on each TCP fd: sends FIN to remote peer (via kernel TCP stack). Number of syscalls: 1 (listen fd, server mode) + up to MAX_TCP_CONNECTIONS (8) client fds = up to 9 close() calls for TcpBackend in server mode; 1 close() call for client mode; 1 close() call for UdpBackend.
- `poll(2)` and `send(2)` / `recv(2)` during the final `pump_retries()` iteration (before `transport.close()`): bounded by the number of due retry slots (0 to 32).
- No `shutdown(2)` syscall. No `getsockopt(SO_ERROR)` during close. No explicit TCP linger configuration.

### File I/O

- `Logger::log()` is called at multiple teardown points. [ASSUMPTION: Logger writes to stdout/stderr or a log file; the implementation is not provided in the files listed. No file descriptor leak from Logger is visible in this scope.]

### Hardware interaction

- None directly. Socket operations go through the kernel network stack.

### IPC

- None. The two processes (Server and Client) communicate via TCP sockets, which are closed during teardown as described. No shared memory, pipes, or message queues are used.

---

## 11. State Changes / Side Effects

### What system state is modified

- **`m_listen_fd`:** -1 after close (was a valid fd).
- **`m_client_fds[i]`:** All set to -1 after close (were valid fds or already -1).
- **`m_client_count`:** Set to 0U.
- **`m_open`:** Set to false.
- **`m_fd` (UdpBackend):** Set to -1.
- **`m_peer` (LocalSimHarness):** Set to nullptr.
- **`g_stop_flag`:** Remains 1 after teardown (not reset; the process is exiting).
- **`AckTracker::m_slots[]`:** Any remaining PENDING entries are not transitioned by close; they are simply abandoned in memory. The last `sweep_expired()` call before the loop breaks may have cleared some.
- **`RetryManager::m_slots[]`:** Any remaining active entries are abandoned in memory. The last `pump_retries()` call may have sent due entries.
- **`RingBuffer m_recv_queue`:** Not explicitly cleared. `m_head` and `m_tail` atomics retain their last values. Any unread `MessageEnvelope` objects in `m_buf[]` are abandoned.

### Persistent changes (DB, disk, device state)

- No database or persistent storage is used.
- The OS kernel's TCP connection table is updated when `close(2)` is called — the connection transitions to TIME_WAIT or CLOSE_WAIT on the remote end, eventually expiring.
- `SO_REUSEADDR` was set during `init()`, so the port can be immediately reused after the process exits and TIME_WAIT expires quickly.

---

## 12. Sequence Diagram using ASCII

```text
## Participants:
  OS/Signal  | Server main | DeliveryEngine | TcpBackend | RetryMgr | AckTracker | SocketUtils

## Flow:

OS/Signal     Server main   DeliveryEngine  TcpBackend  RetryMgr  AckTracker  SocketUtils
    |               |               |            |           |           |           |
  [Ctrl+C]          |               |            |           |           |           |
    |               |               |            |           |           |           |
    |--SIGINT----->[signal_handler] |            |           |           |           |
    |          g_stop_flag=1        |            |           |           |           |
    |               |               |            |           |           |           |
    |      [current poll(2) in]     |            |           |           |           |
    |      [socket_recv_exact()]    |            |           |           |           |
    |<---EINTR/timeout-----------poll(2)---------|           |           |           |
    |               |               |            |           |           |           |
    |        receive() returns      |            |           |           |           |
    |        ERR_TIMEOUT            |            |           |           |           |
    |               |               |            |           |           |           |
    |         [top of loop]         |            |           |           |           |
    |         check g_stop_flag     |            |           |           |           |
    |         g_stop_flag != 0      |            |           |           |           |
    |         break                 |            |           |           |           |
    |               |               |            |           |           |           |
    |         pump_retries(now_us)->|            |           |           |           |
    |               |        collect_due(now_us)------------>|           |           |
    |               |               |<--[retry envelopes]----|           |           |
    |               |        [loop: send_via_transport()]     |           |           |
    |               |               |--send_message()------->|           |           |
    |               |               |            |--------tcp_send_frame()---------->|
    |               |               |            |           |      send(2)/poll(2)  |
    |               |               |<--OK-------|           |           |           |
    |               |<--count-------|            |           |           |           |
    |               |               |            |           |           |           |
    |      sweep_ack_timeouts(now)-->|            |           |           |           |
    |               |               |--sweep_expired(now_us)------------>|           |
    |               |               |<--[expired envelopes]--------------|           |
    |               |               |  [log WARNING_HI per timeout]      |           |
    |               |<--count-------|            |           |           |           |
    |               |               |            |           |           |           |
    |         transport.close()---->|            |           |           |           |
    |               |               |            |           |           |           |
    |               |        TcpBackend::close() |           |           |           |
    |               |               |  [m_listen_fd >= 0?]  |           |           |
    |               |               |--socket_close(listen_fd)--------->|           |
    |               |               |            |           |     close(listen_fd)  |
    |               |               |            |           |           |  [FIN->OS]|
    |               |               |  [loop i=0..7]         |           |           |
    |               |               |  [m_client_fds[i]>=0?] |           |           |
    |               |               |--socket_close(client_fd[i])------->|           |
    |               |               |            |           |    close(client_fd)   |
    |               |               |            |           |           |  [FIN->OS]|
    |               |               |  m_client_count = 0    |           |           |
    |               |               |  m_open = false        |           |           |
    |               |               |  Logger: "Transport closed"        |           |
    |               |<--void--------|            |           |           |           |
    |               |               |            |           |           |           |
    |         Logger: stats          |            |           |           |           |
    |         signal(SIGINT,old)     |            |           |           |           |
    |         return 0               |            |           |           |           |
    |               |               |            |           |           |           |
    |         [stack unwind]         |            |           |           |           |
    |         ~TcpBackend()-------->|            |           |           |           |
    |               |        TcpBackend::close() |           |           |           |
    |               |        [all fds==-1; no-op]|           |           |           |
    |               |        m_open=false (already)|         |           |           |
    |               |        Logger: "Transport closed" (dup)|           |           |
    |               |               |            |           |           |           |
    |         [~DeliveryEngine: compiler-gen; no I/O]        |           |           |
    |         [~RetryManager: m_slots[] abandoned]           |           |           |
    |         [~AckTracker: m_slots[] abandoned]             |           |           |
    |         [~RingBuffer: m_buf[] abandoned]               |           |           |
    |               |               |            |           |           |           |
                 [process exits]
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup

- `main()` parses arguments and fills `TransportConfig` and `ChannelConfig` structs (stack-allocated).
- `TcpBackend::init()` creates sockets (TCP: `socket_create_tcp()`, `socket_set_reuseaddr()`, `socket_bind()`, `socket_listen()` for server; `socket_connect_with_timeout()` for client), sets `m_open = true`, initializes `m_recv_queue.init()` (resets atomic head/tail to 0), initializes `m_impairment.init()`.
- `DeliveryEngine::init()` stores the transport pointer, calls `m_ack_tracker.init()` (memset + loop verifying FREE), `m_retry_manager.init()` (loop clearing slots), `m_dedup.init()`, `m_id_gen.init(seed)`, sets `m_initialized = true`.
- `signal(SIGINT, signal_handler)` registers the handler.
- All allocation happens here. No allocation occurs during the main loop.

### What happens during steady-state execution

- The main loop runs up to `MAX_LOOP_ITERS` (100000) iterations. Each iteration:
  1. Checks stop flag.
  2. Calls `engine.receive()` (polls transport for up to `RECV_TIMEOUT_MS` = 100ms).
  3. If data received: calls `send_echo_reply()` / `engine.send()`.
  4. Calls `engine.pump_retries()` to resend due RELIABLE_RETRY messages.
  5. Calls `engine.sweep_ack_timeouts()` to detect and log ACK deadlines missed.
- No allocation. All MessageEnvelope objects passed by value or written into pre-allocated static arrays.

---

## 14. Known Risks / Observations

### Race conditions

- **`g_stop_flag` and signal delivery:** Technically safe — `sig_atomic_t` + `volatile` is the POSIX-approved pattern for this. However, if a future change introduces a non-atomic read-modify-write of `g_stop_flag` in the signal handler, a race would be introduced.
- **SIGINT during `tcp_send_frame()`:** If SIGINT arrives while `send(2)` or `poll(2)` is executing inside `socket_send_all()`, these syscalls will return -1/EINTR. `socket_send_all()` does not handle EINTR retry; it returns false immediately. This causes the `tcp_send_frame()` to fail mid-frame, potentially delivering a partial frame header or payload to the peer. The peer's `tcp_recv_frame()` would then fail on the next receive attempt. This is a potential data corruption window at the transport framing layer during signal delivery. [RISK: low probability at shutdown, but notable.]
- **No SIGPIPE handling:** If the peer closes its socket before the server finishes sending an echo reply, `send(2)` may generate SIGPIPE. SIGPIPE's default action is to terminate the process. There is no `signal(SIGPIPE, SIG_IGN)` call anywhere in the code. [RISK: could cause abrupt process termination rather than clean shutdown.]

### Tight coupling

- `DeliveryEngine` holds a raw `TransportInterface*` pointer. If the `TcpBackend` object goes out of scope before `DeliveryEngine` does, the pointer becomes dangling. In the current code this cannot happen (both are on the same stack in `main()`), but the API does not enforce this ordering.
- `LocalSimHarness::send_message()` asserts `m_peer != nullptr`. If `close()` is called on one peer before the other stops sending, the other peer's `send_message()` will assert-fail at runtime. There is no bilateral shutdown coordination.

### Hidden side effects

- `TcpBackend::~TcpBackend()` calls `close()` a second time after the explicit `transport.close()` at Server.cpp:258. This emits a duplicate `Logger::log(INFO, "TcpBackend", "Transport closed")` message. While harmless, it could be misleading in log analysis.
- `socket_close()` does not call `shutdown(2)` before `close(2)`. This means the kernel may send RST rather than FIN+ACK in some edge cases (particularly if there is unread data in the receive buffer). The peer application will see an abrupt connection reset, not a clean close.

### Unclear control paths

- There is no explicit mechanism to drain the RingBuffer (`m_recv_queue`) before closing. Messages injected by a peer just before the local harness calls `close()` are lost silently, with no log or error returned to the sender.
- `DeliveryEngine` has no `shutdown()` or `flush()` method. The application is responsible for ensuring all RELIABLE_RETRY messages are acknowledged or expired before calling `transport.close()`. The current Client/Server code does not wait for all ACKs before closing.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `Logger::log()` implementation is not provided. Assumed to write to stdout/stderr or a file. Assumed to be thread-safe enough for single-threaded use but async-signal-unsafe (correctly avoided in the signal handler).

- [ASSUMPTION] `Serializer::serialize()` and `Serializer::deserialize()` implementations are not provided. Assumed to be deterministic, endian-safe, and bounded. Their behavior during teardown (if called by a final retry send) is assumed correct.

- [ASSUMPTION] `ImpairmentEngine::process_outbound()` and `collect_deliverable()` implementations are not provided. Assumed to operate on a fixed internal buffer (IMPAIR_DELAY_BUF_SIZE = 32). Any messages held in the impairment delay buffer at shutdown are abandoned without delivery.

- [ASSUMPTION] `DuplicateFilter::check_and_record()` and `MessageIdGen::next()` implementations are not provided. Their teardown behavior (no explicit cleanup) is assumed safe.

- [ASSUMPTION] `envelope_init()`, `envelope_copy()`, `envelope_valid()`, `envelope_is_control()`, `envelope_is_data()` are assumed to be simple, inline, or trivially implemented utility functions operating on the MessageEnvelope struct by value.

- [ASSUMPTION] `timestamp_now_us()`, `timestamp_deadline_us()`, `timestamp_expired()` are assumed to be POSIX clock-based, not async-signal-safe, and not called from the signal handler (confirmed by inspection).

- [ASSUMPTION] Default SO_LINGER state is in effect on all TCP sockets. The kernel will attempt to flush pending send data on `close(2)`, but the application has no way to wait for or confirm this.

- [UNKNOWN] Whether `Logger::log()` can be called safely inside `~TcpBackend()` after `main()` returns (i.e., during static destruction order). If Logger uses any global state destroyed before TcpBackend, the destructor's log call could access destroyed state.

- [UNKNOWN] The exact behavior of `poll(2)` returning after SIGINT on the target platform. On Linux, `poll()` returns -1 with EINTR; on macOS it may also return 0 (timeout). `socket_recv_exact()` treats poll <= 0 as timeout/error and returns false, which is the correct behavior in both cases.

- [UNKNOWN] Whether any OS-level TCP send buffer data is lost when `close(2)` is called without a prior `shutdown(SHUT_WR)`. This is platform- and state-dependent.

- [ASSUMPTION] The `old_handler` variable at Server.cpp:196 (`void (*old_handler)(int) = signal(SIGINT, signal_handler)`) correctly captures `SIG_DFL` as the previous handler. The assert `old_handler != SIG_ERR` at line 197 will fire if `signal()` fails, which would indicate a severe OS-level error.

- [UNKNOWN] Whether `close(2)` on a TCP socket that is in the middle of a blocking `recv(2)` in the same thread can occur. In the current single-threaded design, `close(2)` is only called after the blocking poll/recv completes (naturally or via EINTR), so no concurrent close-while-blocking situation exists.