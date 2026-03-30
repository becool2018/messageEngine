# UC_35 — TCP Client Connect

Use Case: TcpBackend (client mode) creates a socket, connects to the configured peer
          address and port with a non-blocking connect + poll-based timeout, and stores
          the resulting file descriptor ready for send/receive.
Goal: Trace TcpBackend::init() in client mode creating the socket, calling
      connect_to_server(), invoking socket_connect_with_timeout() (backed by POSIX
      socket/connect/poll/getsockopt), and storing the fd in m_client_fds[0] with
      m_open = true.

---

## 1. Use Case Overview

Actor:        Application code (or a test harness) that needs an outbound TCP connection.
Precondition: A TcpBackend object has been constructed and init() has not yet been called.
              config.is_server == false, config.kind == TransportKind::TCP.
              config.peer_ip and config.peer_port identify the reachable server.
              config.connect_timeout_ms specifies the maximum time to wait for the
              TCP handshake to complete (default 5000 ms from transport_config_default()).
Postcondition: m_client_fds[0] holds a valid, connected TCP socket fd; m_client_count == 1;
               m_open == true; the OS socket is in ESTABLISHED state and left in
               non-blocking mode (O_NONBLOCK is set and never unset).
Trigger:      Caller invokes TcpBackend::init(config) with config.is_server == false.
Success path: init() → connect_to_server() → socket_connect_with_timeout() returns true
              → m_client_fds[0] = fd, m_client_count = 1, m_open = true
              → init() returns Result::OK.
Failure paths:
  - socket() syscall fails → socket_create_tcp() returns -1 → Logger FATAL → ERR_IO.
  - setsockopt(SO_REUSEADDR) fails → socket_set_reuseaddr() returns false
    → Logger WARNING_HI → socket_close(fd) → ERR_IO.
  - fcntl(F_SETFL, O_NONBLOCK) fails → socket_set_nonblocking() returns false
    → socket_connect_with_timeout() returns false → Logger WARNING_HI → ERR_IO.
  - inet_aton() fails (bad peer_ip string) → socket_connect_with_timeout() returns false
    → Logger WARNING_HI → socket_close(fd) → ERR_IO.
  - connect() returns neither 0 nor EINPROGRESS → immediate hard failure
    → Logger WARNING_LO → socket_connect_with_timeout() returns false → ERR_IO.
  - poll() returns <= 0 (timeout or error) → Logger WARNING_LO
    → socket_connect_with_timeout() returns false → ERR_IO.
  - getsockopt(SO_ERROR) shows connection error → Logger WARNING_LO
    → socket_connect_with_timeout() returns false → ERR_IO.

---

## 2. Entry Points

Primary entry point:  TcpBackend::init(const TransportConfig& config)
                      File: src/platform/TcpBackend.cpp, line 48
                      Precondition assertions (lines 50-51):
                          assert(config.kind == TransportKind::TCP)   [line 50]
                          assert(!m_open)                              [line 51]

Secondary entry point (called from init): TcpBackend::connect_to_server()
                      File: src/platform/TcpBackend.cpp, line 118
                      Precondition assertions (lines 120-121):
                          assert(!m_is_server)                         [line 120]
                          assert(m_client_fds[0U] == -1)              [line 121]

The flow is always initiated by the caller of init(). There is no background thread,
timer, or event that triggers this use case autonomously.

---

## 3. End-to-End Control Flow (Step-by-Step)

--- Phase 1: TcpBackend construction (precedes init()) ---

Step 1.  Caller constructs TcpBackend via TcpBackend::TcpBackend() (line 25).
         Initializations performed:
           m_listen_fd   = -1
           m_client_count = 0U
           m_open        = false
           m_is_server   = false
         Loop (i = 0..MAX_TCP_CONNECTIONS-1, i.e., 0..7):
           m_client_fds[i] = -1
         assert(MAX_TCP_CONNECTIONS > 0U) fires as invariant check.  [line 29]

--- Phase 2: TcpBackend::init() — shared setup ---

Step 2.  Caller invokes TcpBackend::init(config) with config.is_server == false.
         Line 50: assert(config.kind == TransportKind::TCP) — passes.
         Line 51: assert(!m_open)                           — passes (m_open is false).
         Line 53: m_cfg = config — full struct copy of TransportConfig (includes
                  bind_ip[48], peer_ip[48], bind_port, peer_port, connect_timeout_ms,
                  num_channels, channels[MAX_CHANNELS], local_node_id, is_server).
         Line 54: m_is_server = false.

Step 3.  m_recv_queue.init() (line 57).
         RingBuffer::init() resets internal atomic head and tail counters:
           m_head.store(0, memory_order_relaxed)
           m_tail.store(0, memory_order_relaxed)
         [ASSUMPTION: RingBuffer capacity is MSG_RING_CAPACITY == 64, enforced by
          internal assertion within RingBuffer::init().]

Step 4.  ImpairmentConfig initialization (lines 59-63):
           impairment_config_default(imp_cfg) fills imp_cfg with safe defaults:
             enabled = false, all probabilities = 0.0, all delays = 0.
           Branch: if (config.num_channels > 0U):
             imp_cfg.enabled = config.channels[0U].impairments_enabled
             (typically false unless caller has explicitly enabled impairments).
         m_impairment.init(imp_cfg) (line 64):
           Copies imp_cfg into ImpairmentEngine internal state.
           Seeds PRNG (seed = imp_cfg.prng_seed; if 0, defaults to 42).
           memset(m_delay_buf, 0, sizeof(m_delay_buf)); m_delay_count = 0.
           memset(m_reorder_buf, 0); m_reorder_count = 0.
           m_initialized = true.
         [ASSUMPTION: ImpairmentEngine::init() is defined in
          src/platform/ImpairmentEngine.cpp.]

Step 5.  Branch at line 66: m_is_server == false → client path.
         Execution falls to line 104: Result res = connect_to_server().

--- Phase 3: TcpBackend::connect_to_server() ---

Step 6.  Entry into connect_to_server() (line 118).
         Line 120: assert(!m_is_server)         — passes.
         Line 121: assert(m_client_fds[0U] == -1) — passes (set to -1 in constructor).

Step 7.  socket_create_tcp() called (line 123).
         Executes in src/platform/SocketUtils.cpp, line 26:
           int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
         On failure (fd < 0):
           Logger::log(WARNING_LO, "SocketUtils", "socket(AF_INET, SOCK_STREAM) failed: %d", errno)
           [Note: SocketUtils logs WARNING_LO; TcpBackend then logs FATAL at line 125-127
            and returns Result::ERR_IO.]
         On success: assert(fd >= 0) fires in socket_create_tcp(); fd returned.
         Local variable fd in connect_to_server() now holds a valid socket descriptor.

Step 8.  socket_set_reuseaddr(fd) called (line 130).
         Executes in SocketUtils.cpp, line 95:
           assert(fd >= 0)
           int optval = 1
           int result = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))
         On failure (result < 0):
           Logger::log(WARNING_LO, "SocketUtils", "setsockopt(SO_REUSEADDR) failed: %d", errno)
           returns false.
         TcpBackend connect_to_server() lines 130-135:
           Logger::log(WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed")
           socket_close(fd)
           returns Result::ERR_IO.
         On success: assert(result == 0) in SocketUtils; returns true.

Step 9.  socket_connect_with_timeout(fd, m_cfg.peer_ip, m_cfg.peer_port,
                                      m_cfg.connect_timeout_ms) called (line 137).
         Executes in SocketUtils.cpp, line 155.

         Sub-step 9a: Precondition assertions (lines 159-160):
           assert(fd >= 0)
           assert(ip != NULL)

         Sub-step 9b: socket_set_nonblocking(fd) called (line 163).
           Executes in SocketUtils.cpp, line 64:
             assert(fd >= 0)
             int flags = fcntl(fd, F_GETFL, 0)
             On failure: Logger::log(WARNING_LO, ...), return false.
             flags |= O_NONBLOCK
             int result = fcntl(fd, F_SETFL, flags)
             On failure: Logger::log(WARNING_LO, ...), return false.
             assert(result == 0); return true.
           If socket_set_nonblocking() returns false:
             socket_connect_with_timeout() returns false immediately.
             (fd is not closed here; TcpBackend lines 138-143 call socket_close(fd).)

         Sub-step 9c: Build sockaddr_in (lines 168-178):
           memset(&addr, 0, sizeof(addr))
           addr.sin_family = AF_INET
           addr.sin_port = htons(m_cfg.peer_port)
           inet_aton(m_cfg.peer_ip, &addr.sin_addr)
           On inet_aton failure (returns 0):
             Logger::log(WARNING_LO, "SocketUtils", "inet_aton('%s') failed", ip)
             return false.

         Sub-step 9d: Non-blocking connect attempt (lines 181-186):
           int conn_result = connect(fd, (struct sockaddr*)&addr, sizeof(addr))
           Case A — conn_result == 0:
             Connection established immediately (server on same host, loopback).
             assert(conn_result == 0); return true.
           Case B — conn_result < 0 and errno == EINPROGRESS:
             Non-blocking connect in progress; continue to poll.  [Normal path]
           Case C — conn_result < 0 and errno != EINPROGRESS:
             Hard failure (ECONNREFUSED, ENETUNREACH, etc.)
             Logger::log(WARNING_LO, "SocketUtils", "connect(%s:%u) failed: %d", ip, port, errno)
             return false.

         Sub-step 9e: Poll for writability (lines 197-207) [Case B only]:
           struct pollfd pfd; pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
           int poll_result = poll(&pfd, 1, (int)timeout_ms)
           On poll_result <= 0 (timeout or poll error):
             Logger::log(WARNING_LO, "SocketUtils", "connect(%s:%u) timeout", ip, port)
             return false.

         Sub-step 9f: Verify connection success via SO_ERROR (lines 210-218):
           int opt_err = 0; socklen_t opt_len = sizeof(opt_err);
           int getsock_result = getsockopt(fd, SOL_SOCKET, SO_ERROR, &opt_err, &opt_len)
           On getsock_result < 0 OR opt_err != 0:
             Logger::log(WARNING_LO, "SocketUtils",
                        "connect(%s:%u) failed after poll: %d", ip, port, opt_err)
             return false.
           assert(getsock_result == 0 && opt_err == 0); return true.
           [At this point the socket is ESTABLISHED and still in O_NONBLOCK mode.]

Step 10. Back in TcpBackend::connect_to_server() (line 137):
         If socket_connect_with_timeout() returned false (lines 138-143):
           Logger::log(WARNING_HI, "TcpBackend",
                      "Connection to %s:%u failed", m_cfg.peer_ip, m_cfg.peer_port)
           socket_close(fd)
           return Result::ERR_IO.

Step 11. Successful connection — update TcpBackend state (lines 145-150):
           m_client_fds[0U] = fd          [line 145]
           m_client_count   = 1U          [line 146]
           m_open           = true        [line 147]
           Logger::log(INFO, "TcpBackend",
                      "Connected to %s:%u", m_cfg.peer_ip, m_cfg.peer_port) [lines 149-150]

Step 12. Post-condition assertions in connect_to_server() (lines 152-153):
           assert(m_client_fds[0U] >= 0)  — passes.
           assert(m_client_count == 1U)    — passes.
         Returns Result::OK.

--- Phase 4: Return from init() ---

Step 13. Back in TcpBackend::init() (line 104-107):
           Result res = connect_to_server()  — res == Result::OK.
           result_ok(res) == true → no early return.
         Line 110: assert(m_open) — passes (m_open == true).
         Returns Result::OK.

---

## 4. Call Tree (Hierarchical)

```
TcpBackend::init(config)                                [TcpBackend.cpp:48]
├── assert(config.kind == TransportKind::TCP)           [line 50]
├── assert(!m_open)                                     [line 51]
├── m_cfg = config                                      [line 53]  struct copy
├── m_recv_queue.init()                                 [line 57, RingBuffer.hpp]
│   ├── m_head.store(0, relaxed)
│   └── m_tail.store(0, relaxed)
├── impairment_config_default(imp_cfg)                  [line 60, ChannelConfig.hpp:51]
├── ImpairmentEngine::init(imp_cfg)                     [line 64, ImpairmentEngine.cpp]
│   ├── m_prng.seed(seed)
│   ├── memset(m_delay_buf, ...)
│   └── memset(m_reorder_buf, ...)
└── connect_to_server()                                 [line 104, TcpBackend.cpp:118]
    ├── assert(!m_is_server)                            [line 120]
    ├── assert(m_client_fds[0U] == -1)                  [line 121]
    ├── socket_create_tcp()                             [line 123, SocketUtils.cpp:26]
    │   └── socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)  [POSIX syscall]
    ├── socket_set_reuseaddr(fd)                        [line 130, SocketUtils.cpp:95]
    │   └── setsockopt(fd, SOL_SOCKET, SO_REUSEADDR)   [POSIX syscall]
    ├── socket_connect_with_timeout(fd, ip, port, ms)  [line 137, SocketUtils.cpp:155]
    │   ├── socket_set_nonblocking(fd)                 [SocketUtils.cpp:64]
    │   │   ├── fcntl(fd, F_GETFL, 0)                  [POSIX syscall]
    │   │   └── fcntl(fd, F_SETFL, flags|O_NONBLOCK)   [POSIX syscall]
    │   ├── inet_aton(peer_ip, &addr.sin_addr)          [POSIX]
    │   ├── connect(fd, &addr, sizeof(addr))            [POSIX syscall — non-blocking]
    │   ├── poll(&pfd, 1, timeout_ms)                  [POSIX syscall — wait POLLOUT]
    │   │   [blocks up to connect_timeout_ms ms]
    │   └── getsockopt(fd, SOL_SOCKET, SO_ERROR, ...)  [POSIX syscall — verify success]
    ├── [on failure] socket_close(fd)                  [line 141, SocketUtils.cpp:272]
    │   └── close(fd)                                  [POSIX syscall]
    ├── m_client_fds[0U] = fd                          [line 145]
    ├── m_client_count   = 1U                          [line 146]
    ├── m_open           = true                        [line 147]
    ├── Logger::log(INFO, ...)                         [lines 149-150]
    ├── assert(m_client_fds[0U] >= 0)                  [line 152]
    └── assert(m_client_count == 1U)                   [line 153]
```

---

## 5. Key Components Involved

Component                 File / Location                        Role in this flow
---------                 ---------------                        -----------------
TcpBackend                src/platform/TcpBackend.cpp/.hpp       Top-level orchestrator;
                                                                  owns all state; drives
                                                                  the client connect
                                                                  sequence.
SocketUtils               src/platform/SocketUtils.cpp/.hpp      Provides all raw POSIX
                                                                  socket primitives:
                                                                  socket_create_tcp(),
                                                                  socket_set_reuseaddr(),
                                                                  socket_connect_with_timeout(),
                                                                  socket_set_nonblocking(),
                                                                  socket_close().
TransportConfig           src/core/ChannelConfig.hpp:37          Input struct; supplies
                                                                  peer_ip, peer_port,
                                                                  connect_timeout_ms,
                                                                  is_server, channels[].
ChannelConfig             src/core/ChannelConfig.hpp:21          Per-channel settings
                                                                  (impairments_enabled
                                                                  read at line 62 of
                                                                  TcpBackend.cpp).
ImpairmentEngine          src/platform/ImpairmentEngine.cpp      Initialized during init()
                                                                  but not exercised in the
                                                                  connect path itself.
RingBuffer                src/core/RingBuffer.hpp                Receive queue; initialized
                                                                  to empty during init();
                                                                  not used during connect.
Types.hpp                 src/core/Types.hpp                     Defines constants
                                                                  (MAX_TCP_CONNECTIONS=8,
                                                                  SOCKET_RECV_BUF_BYTES=8192)
                                                                  and Result/Severity enums.
Logger                    src/core/Logger.hpp                    F-Prime style event log;
                                                                  called at all key
                                                                  transitions and errors.

---

## 6. Branching Logic / Decision Points

Decision Point 1: config.is_server (TcpBackend.cpp line 66)
  false → client path (this use case): call connect_to_server() at line 104.
  true  → server path: socket_create_tcp → bind → listen (covered in UC_19).

Decision Point 2: socket_create_tcp() return value (connect_to_server() line 123-128)
  fd >= 0 → continue.
  fd < 0  → Logger::log(FATAL, "TcpBackend", "socket_create_tcp failed in client mode")
             return Result::ERR_IO.

Decision Point 3: socket_set_reuseaddr() return value (line 130-135)
  true  → continue.
  false → Logger::log(WARNING_HI, "TcpBackend", "socket_set_reuseaddr failed")
          socket_close(fd); return Result::ERR_IO.

Decision Point 4: socket_set_nonblocking() inside socket_connect_with_timeout() (line 163)
  true  → continue to address build + connect.
  false → socket_connect_with_timeout() returns false immediately.
          TcpBackend lines 138-143: Logger WARNING_HI; socket_close(fd); ERR_IO.

Decision Point 5: inet_aton(peer_ip) result (SocketUtils.cpp line 173-178)
  aton_result != 0 → address parsed; continue.
  aton_result == 0 → Logger WARNING_LO in SocketUtils; return false from
                     socket_connect_with_timeout(); TcpBackend closes fd, returns ERR_IO.

Decision Point 6: connect() return value (SocketUtils.cpp lines 181-194)
  conn_result == 0         → immediate success; return true (loopback/fast path).
  errno == EINPROGRESS     → expected for non-blocking; proceed to poll.
  errno != EINPROGRESS     → hard error; Logger WARNING_LO; return false.

Decision Point 7: poll() result (SocketUtils.cpp lines 202-207)
  poll_result > 0  → socket became writable (or error bit set); proceed to SO_ERROR check.
  poll_result == 0 → timeout expired; Logger WARNING_LO "timeout"; return false.
  poll_result < 0  → poll() error; Logger WARNING_LO "timeout"; return false.
  [Both timeout and poll error produce the same log message "timeout" and path.]

Decision Point 8: getsockopt(SO_ERROR) result (SocketUtils.cpp lines 212-218)
  getsock_result == 0 AND opt_err == 0 → connection successful; assert; return true.
  getsock_result < 0 OR opt_err != 0  → Logger WARNING_LO "failed after poll"; return false.
  [opt_err != 0 catches cases where poll returned POLLOUT but the connection was rejected
   (e.g., ECONNREFUSED delivered asynchronously).]

Decision Point 9: socket_connect_with_timeout() overall return in connect_to_server()
                  (lines 137-143)
  true  → update state (m_client_fds[0], m_client_count, m_open); return OK.
  false → Logger WARNING_HI; socket_close(fd); return ERR_IO.

Decision Point 10: config.num_channels > 0U (init() line 61)
  true  → imp_cfg.enabled = config.channels[0].impairments_enabled (may be true or false).
  false → imp_cfg.enabled remains at default (false from impairment_config_default).

---

## 7. Concurrency / Threading Behavior

- TcpBackend has no internal threads. init() and connect_to_server() execute entirely
  in the caller's thread.
- poll() at sub-step 9e blocks the calling thread for up to connect_timeout_ms
  milliseconds (default 5000 ms). This is the only blocking point in this flow.
  All other operations are non-blocking or instantaneous.
- The socket fd is in non-blocking mode (O_NONBLOCK set by socket_set_nonblocking()
  at sub-step 9b) at the point connect() is issued and remains non-blocking afterward.
  No blocking send/recv will occur by accident on this fd.
- m_client_fds[0] and m_client_count are plain (non-atomic) fields. They are written
  once in connect_to_server() and thereafter read by send_message() and receive_message().
  [ASSUMPTION: The caller serializes access; no concurrent send/receive is started
   before init() returns.]
- m_open is a plain bool; written true at line 147 before connect_to_server() returns.
  [ASSUMPTION: Single-threaded use; no concurrent reader of m_open during init().]
- RingBuffer uses std::atomic<uint32_t> m_head and m_tail (SPSC design). During init(),
  only relaxed stores are issued; no concurrent access exists yet.
- ImpairmentEngine PRNG state is not thread-safe. [ASSUMPTION: all calls to
  ImpairmentEngine are serialized by the single-threaded caller.]

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

All storage for TcpBackend is static / inline (Power of 10 rule 3: no heap after init):

  m_client_fds[MAX_TCP_CONNECTIONS]  = m_client_fds[8]
    int array, inline member of TcpBackend object; no heap allocation.
    Lifetime: TcpBackend object lifetime. Initialized to -1 in constructor.

  m_wire_buf[SOCKET_RECV_BUF_BYTES]  = m_wire_buf[8192]
    uint8_t array, inline member; used for serialization in send/receive (not in connect).

  m_recv_queue (RingBuffer)
    Contains m_buf[MSG_RING_CAPACITY] = m_buf[64] of MessageEnvelope inline.

  m_impairment (ImpairmentEngine)
    m_delay_buf[IMPAIR_DELAY_BUF_SIZE] and m_reorder_buf[] inline.

  m_cfg (TransportConfig)
    Full struct copy (includes bind_ip[48], peer_ip[48], channels[MAX_CHANNELS]=channels[8])
    assigned from the caller's config at line 53 of init(). Caller's config can be
    freed or go out of scope after init() returns.

  Local variable fd in connect_to_server():
    Stack-allocated int. On failure, fd is explicitly closed by socket_close(fd) before
    the function returns, preventing fd leak. On success, fd is stored into
    m_client_fds[0U] and becomes TcpBackend's responsibility.

  sockaddr_in addr in socket_connect_with_timeout():
    Stack-allocated struct; memset to zero; lifetime is within socket_connect_with_timeout().

  struct pollfd pfd in socket_connect_with_timeout():
    Stack-allocated; lifetime is within socket_connect_with_timeout().

  opt_err (int) in socket_connect_with_timeout():
    Stack-allocated; passed by pointer to getsockopt(); no ownership transfer.

OS resource ownership:
  - The connected socket fd created by socket() is owned by TcpBackend from the moment
    it is stored in m_client_fds[0U]. TcpBackend::close() (called by the destructor
    at line 41) releases it via socket_close().
  - On any failure path in connect_to_server(), fd is closed before returning (lines
    133-134 or 141-142). No fd leak exists on any modeled failure path.
  - No malloc(), new, or equivalent is used anywhere in this flow.

---

## 9. Error Handling Flow

Error source                         Detected at / line              Action
------------                         ------------------              ------
socket() syscall fails               socket_create_tcp() / line 30   Returns -1 to
                                     connect_to_server() / line 123  connect_to_server().
                                                                      Logger FATAL (line 125);
                                                                      return ERR_IO.
                                                                      (No fd to close.)

setsockopt(SO_REUSEADDR) fails       socket_set_reuseaddr() / ln 103 Returns false.
                                     connect_to_server() / line 130  Logger WARNING_HI (ln 131);
                                                                      socket_close(fd) (ln 133);
                                                                      return ERR_IO.

fcntl(F_GETFL) fails                 socket_set_nonblocking() ln 71  Returns false.
                                     socket_connect_with_timeout()   Returns false.
                                     connect_to_server() / line 137  Logger WARNING_HI (ln 139);
                                                                      socket_close(fd) (ln 141);
                                                                      return ERR_IO.

fcntl(F_SETFL, O_NONBLOCK) fails     socket_set_nonblocking() ln 81  Same propagation path
                                                                      as above.

inet_aton() fails (bad peer_ip)      SocketUtils.cpp / line 174      Logger WARNING_LO in
                                     socket_connect_with_timeout()   SocketUtils; returns false.
                                     connect_to_server() / line 137  Logger WARNING_HI (ln 139);
                                                                      socket_close(fd) (ln 141);
                                                                      return ERR_IO.

connect() fails, errno !=            SocketUtils.cpp / line 190-193  Logger WARNING_LO in
EINPROGRESS (e.g., ECONNREFUSED)     socket_connect_with_timeout()   SocketUtils; returns false.
                                     connect_to_server() / line 137  Logger WARNING_HI; close;
                                                                      return ERR_IO.

poll() timeout or error              SocketUtils.cpp / lines 202-207 Logger WARNING_LO in
                                     socket_connect_with_timeout()   SocketUtils; returns false.
                                     connect_to_server() / line 137  Logger WARNING_HI; close;
                                                                      return ERR_IO.

getsockopt(SO_ERROR) shows error     SocketUtils.cpp / lines 212-218 Logger WARNING_LO in
or getsockopt itself fails                                            SocketUtils; returns false.
                                     connect_to_server() / line 137  Logger WARNING_HI; close;
                                                                      return ERR_IO.

connect_to_server() returns ERR_IO   init() / lines 104-107          result_ok(res) == false;
                                                                      init() returns ERR_IO.

Assertion failure (debug build)      Any assert()                    abort() in debug.
                                                                      [ASSUMPTION: Production
                                                                       builds may differ.]

Severity classification (F-Prime model):
  - FATAL:      socket() failure in TcpBackend (component cannot be initialized).
  - WARNING_HI: setsockopt() failure; any connection failure logged by TcpBackend.
  - WARNING_LO: Low-level SocketUtils failures (syscall details logged close to source).

Retry logic: None. connect_to_server() makes a single attempt. Retry at a higher level
             is the caller's responsibility. [ASSUMPTION: The application layer or a
             retry manager calls init() again after teardown if reconnect is needed.]

---

## 10. External Interactions

Interaction                    syscall / API                       When
-----------                    -------------                       ----
Create TCP socket              socket(AF_INET, SOCK_STREAM,        connect_to_server()
                               IPPROTO_TCP)                        step 7
Set SO_REUSEADDR               setsockopt(SOL_SOCKET,              connect_to_server()
                               SO_REUSEADDR, 1)                    step 8
Get socket flags               fcntl(fd, F_GETFL, 0)              socket_connect_with
                                                                   _timeout() step 9b
Set O_NONBLOCK                 fcntl(fd, F_SETFL,                  socket_connect_with
                               flags|O_NONBLOCK)                   _timeout() step 9b
Initiate TCP 3-way handshake   connect(fd, &addr, sizeof(addr))    socket_connect_with
                                                                   _timeout() step 9d
Wait for connection ready       poll(&pfd, 1, timeout_ms)          socket_connect_with
                                                                   _timeout() step 9e
Verify connection success       getsockopt(fd, SOL_SOCKET,         socket_connect_with
                               SO_ERROR, &opt_err, &opt_len)       _timeout() step 9f
Close fd on failure             close(fd)                          socket_close() on any
                                                                   failure path
Log events                     Logger::log()                       Multiple points

Network: The TCP three-way handshake (SYN, SYN-ACK, ACK) is initiated by connect()
and completed when poll() returns with POLLOUT and getsockopt(SO_ERROR) confirms 0.
No application-level data is sent or received during this use case.

TLS/DTLS: No TLS hooks are called during connect. The extension point for TLS
is not implemented in this path. [ASSUMPTION: secure transport would be layered
atop this after the fd is established.]

---

## 11. State Changes / Side Effects

Before init():               After init() success (client mode):
  m_listen_fd   = -1           m_listen_fd   = -1  (unchanged; not used in client mode)
  m_client_fds[0] = -1         m_client_fds[0] = valid connected fd (>= 0)
  m_client_fds[1..7] = -1      m_client_fds[1..7] = -1 (unchanged)
  m_client_count = 0           m_client_count = 1
  m_open        = false         m_open        = true
  m_is_server   = false         m_is_server   = false (unchanged)
  m_cfg: zeroed/default         m_cfg: copy of caller's TransportConfig
  m_recv_queue: uninit          m_recv_queue: head=0, tail=0 (empty)
  m_impairment: uninit          m_impairment: m_initialized=true, PRNG seeded

OS-level side effects:
  - One TCP socket fd allocated in the OS fd table.
  - Socket set to O_NONBLOCK; this persists for the lifetime of the connection.
    Subsequent send and receive calls on m_client_fds[0] will use poll-based
    looping (socket_send_all, socket_recv_exact) to handle partial I/O.
  - TCP connection in ESTABLISHED state to peer_ip:peer_port.
  - Kernel TCP send/receive buffers allocated for the connection.

Logger side effects:
  - On success: one INFO event ("Connected to <ip>:<port>").
  - On failure: one WARNING_HI event from TcpBackend plus one WARNING_LO event
    from SocketUtils (at the point of the failing syscall).

---

## 12. Sequence Diagram using ASCII

```text
Caller          TcpBackend          SocketUtils             OS Kernel
------          ----------          -----------             ---------
  |                  |                   |                      |
  |--init(cfg)------>|                   |                      |
  |                  | [assert kind==TCP, !m_open]              |
  |                  | m_cfg = cfg                              |
  |                  |--recv_queue.init()                       |
  |                  |   [m_head=0, m_tail=0]                   |
  |                  |--impairment.init()                       |
  |                  |   [prng seeded, bufs zeroed]             |
  |                  | m_is_server = false                      |
  |                  |                   |                      |
  |                  |--connect_to_server()                     |
  |                  |  [assert !is_server, fds[0]==-1]         |
  |                  |                   |                      |
  |                  |--socket_create_tcp()-->|                 |
  |                  |                   |--socket(STREAM)----->|
  |                  |                   |<--fd----------------|
  |                  |<--fd (>= 0)-------|                      |
  |                  |                   |                      |
  |                  |--socket_set_reuseaddr(fd)-->|            |
  |                  |                   |--setsockopt()------->|
  |                  |                   |  SO_REUSEADDR=1      |
  |                  |                   |<--0 (ok)------------|
  |                  |<--true------------|                      |
  |                  |                   |                      |
  |                  |--socket_connect_with_timeout(fd,ip,port,ms)-->|
  |                  |                   |                      |
  |                  |                   |--socket_set_nonblocking(fd)
  |                  |                   |--fcntl(F_GETFL)----->|
  |                  |                   |<--flags-------------|
  |                  |                   |--fcntl(F_SETFL,     |
  |                  |                   |   flags|O_NONBLOCK)->|
  |                  |                   |<--0 (ok)------------|
  |                  |                   |                      |
  |                  |                   |--inet_aton(peer_ip)  |
  |                  |                   |  [parse to in_addr]  |
  |                  |                   |                      |
  |                  |                   |--connect(fd,&addr)-->|
  |                  |                   |  [non-blocking]      |
  |                  |                   |<--(-1)/EINPROGRESS--|
  |                  |                   |  [TCP SYN sent]      |
  |                  |                   |                      |<---[SYN-ACK from server]
  |                  |                   |--poll(fd,POLLOUT,ms)->|
  |                  |                   |  [blocks up to ms]   |
  |                  |                   |<--1 (writable)------|
  |                  |                   |                      |
  |                  |                   |--getsockopt(SO_ERROR)->|
  |                  |                   |<--opt_err=0---------|
  |                  |                   |  [ESTABLISHED]       |
  |                  |<--true------------|                      |
  |                  |                   |                      |
  |                  | m_client_fds[0]=fd                       |
  |                  | m_client_count=1                         |
  |                  | m_open=true                              |
  |                  |--Logger INFO "Connected to ip:port"      |
  |                  | [assert fds[0]>=0, count==1]             |
  |                  | return OK                                |
  |                  |                   |                      |
  |  [assert m_open] |                   |                      |
  |<--Result::OK-----|                   |                      |
  |                  |                   |                      |

Failure variant (connection refused):

Caller          TcpBackend          SocketUtils             OS Kernel
------          ----------          -----------             ---------
  |                  |                   |                      |
  |                  |--socket_connect_with_timeout(fd,ip,port,ms)-->|
  |                  |                   |--connect(fd,&addr)-->|
  |                  |                   |<--(-1)/ECONNREFUSED-|
  |                  |                   | Logger WARNING_LO    |
  |                  |                   | "connect failed: %d" |
  |                  |<--false-----------|                      |
  |                  | Logger WARNING_HI "Connection to ip:port failed"
  |                  |--socket_close(fd)->|                     |
  |                  |                   |--close(fd)---------->|
  |                  |                   |<--0-----------------|
  |                  | return ERR_IO                            |
  |<--Result::ERR_IO-|                   |                      |

Failure variant (timeout):

Caller          TcpBackend          SocketUtils             OS Kernel
------          ----------          -----------             ---------
  |                  |                   |                      |
  |                  |--socket_connect_with_timeout(fd,ip,port,ms)-->|
  |                  |                   |--connect(fd,&addr)-->|
  |                  |                   |<--(-1)/EINPROGRESS--|
  |                  |                   |--poll(fd,POLLOUT,ms)->|
  |                  |                   |  [timeout expires]   |
  |                  |                   |<--0 (timeout)-------|
  |                  |                   | Logger WARNING_LO    |
  |                  |                   | "connect timeout"    |
  |                  |<--false-----------|                      |
  |                  | Logger WARNING_HI "Connection to ip:port failed"
  |                  |--socket_close(fd)->|                     |
  |                  |<--Result::ERR_IO--|                      |
  |<--Result::ERR_IO-|                   |                      |
```

---

## 13. Initialization vs Runtime Flow

Initialization phase (everything in this use case is initialization):

  Constructor (before init()):
    - All member scalars set to sentinel values (-1, 0, false).
    - m_client_fds[0..7] = -1. Fixed-size, compile-time allocation only.

  init() — shared setup:
    - m_cfg copied from caller; caller's TransportConfig can be discarded afterward.
    - RingBuffer reset to empty (head=0, tail=0).
    - ImpairmentEngine configured and PRNG seeded with deterministic seed.
    - No OS resources acquired yet at this point.

  init() — client connect path (connect_to_server()):
    - First OS resource acquired: socket fd via socket().
    - SO_REUSEADDR set (advisory; allows fast port reuse).
    - O_NONBLOCK set permanently on the fd via fcntl().
    - TCP handshake driven through connect() + poll() + getsockopt(SO_ERROR).
    - On success: fd stored; m_open = true marks transition to runtime-ready state.

Runtime phase (post-init, not part of this use case):
    - send_message() uses m_client_fds[0] via tcp_send_frame() → socket_send_all()
      → poll(POLLOUT) + send() loop.
    - receive_message() uses m_client_fds[0] via recv_from_client() → tcp_recv_frame()
      → socket_recv_exact() → poll(POLLIN) + recv() loop.
    - In client mode, accept_clients() is never called (m_is_server == false).
    - m_impairment.process_outbound() and m_impairment.collect_deliverable() are called
      by send_message() and receive_message() if impairments are enabled.
    - No further OS resource allocation occurs during steady-state operation
      (Power of 10 rule 3).

---

## 14. Known Risks / Observations

Risk 1: Socket left in non-blocking mode permanently
  socket_set_nonblocking() sets O_NONBLOCK on the fd inside
  socket_connect_with_timeout() and never clears it. This is intentional — subsequent
  send_message() and receive_message() calls use poll-based loops (socket_send_all,
  socket_recv_exact) that are designed to work with non-blocking fds. However, any
  code path that naively calls send() or recv() directly on m_client_fds[0] without
  polling first will receive EAGAIN instead of blocking.
  [OBSERVATION: The design is self-consistent but requires all users of the fd to
   go through the SocketUtils framing functions, which they do.]

Risk 2: poll() is the only blocking point; duration is bounded by connect_timeout_ms
  The calling thread blocks inside poll() for at most connect_timeout_ms milliseconds
  (default 5000 ms). If connect_timeout_ms is 0, poll() returns immediately, and a
  connection that requires any round-trip time will always time out.
  [OBSERVATION: transport_config_default() sets connect_timeout_ms = 5000U (line 72
   of ChannelConfig.hpp), which is a safe default. Callers setting it to 0 will
   always fail the connect in the non-loopback case.]

Risk 3: POLLOUT can be set even when SO_ERROR != 0 (async error delivery)
  On some kernels, poll() returns POLLOUT when an async connection error (e.g.,
  ECONNREFUSED delivered via ICMP after a short delay) is pending. The getsockopt
  (SO_ERROR) check at sub-step 9f is the correct mitigation and is present.
  [OBSERVATION: The implementation correctly distinguishes poll wakeup from
   actual connection success.]

Risk 4: init() is not re-entrant and cannot be called twice without close()
  assert(!m_open) at line 51 fires in debug if init() is called on an already-open
  backend. In production (without asserts), m_cfg would be overwritten while
  m_client_fds[0] still holds a live fd, leaking the previous connection.
  [OBSERVATION: Callers must call close() or rely on the destructor before
   re-initializing. No guard beyond the assert exists.]

Risk 5: No impairment applied to the connect handshake itself
  ImpairmentEngine is initialized but process_outbound() is never called during
  connect. The impairment engine operates at the message (send_message) layer, not
  at the connection establishment layer. Simulating connection failures or latency
  at the TCP handshake level is not supported by the current design.

Risk 6: Logging severity inconsistency between layers
  socket_create_tcp() in SocketUtils logs at WARNING_LO when socket() fails (line 31),
  but TcpBackend immediately re-logs the same event at FATAL (line 125-127). This
  produces two log entries for a single syscall failure. The WARNING_LO from SocketUtils
  may mislead a reader into thinking the event is recoverable when TcpBackend treats it
  as FATAL.

Risk 7: Single connection slot (m_client_fds[0]) for client mode
  In client mode, only m_client_fds[0] is used. The slot is reserved by the constructor
  (all slots -1) and never revisited unless a reconnect pattern is implemented by the
  caller (close() + init()). There is no mechanism in the current design for the client
  to maintain multiple outbound connections simultaneously.

---

## 15. Unknowns / Assumptions

[ASSUMPTION A]  The connected socket fd is permanently left in O_NONBLOCK mode after
                socket_connect_with_timeout() returns. send_message() and
                receive_message() are assumed to use only poll-wrapped I/O functions
                (socket_send_all, socket_recv_exact) that handle non-blocking fds
                correctly. No code path re-enables blocking mode.

[ASSUMPTION B]  impairment_config_default() is declared as a free function defined
                inline in ImpairmentEngine.hpp or a closely related header. Its exact
                signature was not read but its behavior is inferred from usage at
                TcpBackend.cpp lines 59-63.

[ASSUMPTION C]  ImpairmentEngine::init() is defined in src/platform/ImpairmentEngine.cpp.
                Its full implementation was not read. The behavior described (PRNG seed,
                memset of delay/reorder buffers, m_initialized flag) is inferred from
                the UC_19 reference document and from TcpBackend.cpp calling patterns.

[ASSUMPTION D]  Logger::log() is a thread-safe, non-blocking, non-allocating call. Its
                implementation was not read. It is assumed it will not block the calling
                thread for a meaningful duration.

[ASSUMPTION E]  The PRNG used by ImpairmentEngine is a deterministic, custom type (not
                from STL, consistent with MISRA/F-Prime rules). It is assumed to have
                seed() and value-generation methods. No dynamic allocation is assumed.

[ASSUMPTION F]  transport_config_default() (ChannelConfig.hpp line 66) represents the
                baseline configuration a caller would use in tests. Production callers
                may supply different peer_ip, peer_port, and connect_timeout_ms values.

[ASSUMPTION G]  The caller does not start concurrent send_message() or receive_message()
                calls before init() returns Result::OK. The implementation provides no
                synchronization guard for this scenario beyond the m_open flag.

[ASSUMPTION H]  The server at peer_ip:peer_port is listening when init() is called. If
                the server is not yet ready, connect() will return ECONNREFUSED (not
                EINPROGRESS), causing an immediate failure (Decision Point 6, Case C).
                There is no built-in retry loop in connect_to_server(); the caller must
                retry init() after close() if a connection-refused failure is acceptable.

[ASSUMPTION I]  assert() in production builds is not disabled (i.e., NDEBUG is not
                defined in production compilation). If NDEBUG is defined, the precondition
                checks (e.g., assert(!m_open) at line 51) become no-ops and misuse of
                the API will silently corrupt state.

[ASSUMPTION J]  The RingBuffer internal implementation uses std::atomic<uint32_t> for
                m_head and m_tail (inferred from UC_19 reference). The carve-out in
                CLAUDE.md section 3 explicitly permits std::atomic for integral types;
                this is consistent with the MISRA C++:2023 endorsement.
