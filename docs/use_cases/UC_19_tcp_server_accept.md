Use Case: TcpBackend (server mode) accepts a new client connection on the listen socket.
Goal: Trace TcpBackend::init() in server mode creating listen socket, then accept_clients()
      using socket_accept() (backed by POSIX accept()), and storing the client fd in
      m_client_fds[].

## 1. Use Case Overview

Actor:        External TCP client initiating a connection to a bound server port.
Precondition: A TcpBackend object has been constructed and init() has not yet been called.
              config.is_server == true, config.kind == TransportKind::TCP.
Postcondition: m_listen_fd holds a valid listening socket fd; m_client_fds[0] (or the next
               available slot) holds the accepted client fd; m_client_count is incremented.
Trigger:      Caller invokes TcpBackend::init() followed by TcpBackend::receive_message()
              (or a direct call to accept_clients()) while a remote client is simultaneously
              issuing a TCP connect().
Success path: init() returns Result::OK; accept_clients() returns Result::OK; client fd is
              stored in m_client_fds[m_client_count - 1].
Failure paths: Any socket syscall fails → FATAL or WARNING_HI logged → Result::ERR_IO
               returned; m_listen_fd cleaned up; m_open remains false.

## 2. Entry Points

Primary entry point:  TcpBackend::init(const TransportConfig& config)
                      File: src/platform/TcpBackend.cpp, line 48
                      Precondition assertions (lines 50–51):
                          assert(config.kind == TransportKind::TCP)
                          assert(!m_open)

Secondary entry point: TcpBackend::accept_clients()
                       File: src/platform/TcpBackend.cpp, line 161
                       Called from receive_message() (line 337) or directly.
                       Precondition assertions (lines 163–164):
                           assert(m_is_server)
                           assert(m_listen_fd >= 0)

## 3. End-to-End Control Flow (Step-by-Step)

--- Phase 1: TcpBackend::init() ---

Step 1.  Caller constructs TcpBackend.
         TcpBackend::TcpBackend() (line 25):
           - m_listen_fd = -1, m_client_count = 0, m_open = false, m_is_server = false.
           - Loop (0..MAX_TCP_CONNECTIONS-1, i.e. 0..7): m_client_fds[i] = -1.

Step 2.  Caller invokes TcpBackend::init(config) with config.is_server == true.
         Line 50:  assert(config.kind == TransportKind::TCP)  — passes.
         Line 51:  assert(!m_open)                           — passes.
         Line 53:  m_cfg = config                            — full struct copy.
         Line 54:  m_is_server = true.

Step 3.  m_recv_queue.init() (line 57) → RingBuffer::init():
           - m_head.store(0, relaxed); m_tail.store(0, relaxed).
           - Assertions: MSG_RING_CAPACITY > 0, is power of two (64).

Step 4.  ImpairmentConfig initialized (lines 59–63):
           - impairment_config_default(imp_cfg) fills safe defaults.
           - If config.num_channels > 0:
               imp_cfg.enabled = config.channels[0].impairments_enabled.
         m_impairment.init(imp_cfg) (line 64) → ImpairmentEngine::init():
           - Copies cfg; seeds PRNG (seed 42 if cfg.prng_seed == 0).
           - memset(m_delay_buf, 0, sizeof(m_delay_buf)); m_delay_count = 0.
           - memset(m_reorder_buf, 0); m_reorder_count = 0.
           - m_initialized = true.

Step 5.  Branch: m_is_server == true → server path (line 66).

Step 6.  socket_create_tcp() (line 68) → SocketUtils.cpp line 26:
           - syscall: socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
           - Returns fd >= 0 or -1.
           - On failure: Logger::log(FATAL, ...) → return Result::ERR_IO.
         m_listen_fd = returned fd.

Step 7.  socket_set_reuseaddr(m_listen_fd) (line 75) → SocketUtils.cpp line 95:
           - syscall: setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &1, sizeof(int))
           - On failure: Logger::log(WARNING_HI), socket_close(fd), m_listen_fd = -1
             → return Result::ERR_IO.

Step 8.  socket_bind(m_listen_fd, config.bind_ip, config.bind_port) (line 83)
         → SocketUtils.cpp line 118:
           - inet_aton(ip, &addr.sin_addr) — parse IP string.
           - syscall: bind(fd, (sockaddr*)&addr, sizeof(addr))
           - On failure: Logger::log(FATAL), socket_close(fd), m_listen_fd = -1
             → return Result::ERR_IO.

Step 9.  socket_listen(m_listen_fd, MAX_TCP_CONNECTIONS) (line 91)
         → SocketUtils.cpp line 229:
           - syscall: listen(fd, 8)
           - On failure: Logger::log(FATAL), socket_close, m_listen_fd = -1
             → return Result::ERR_IO.

Step 10. m_open = true (line 98).
         Logger::log(INFO, "TcpBackend", "Server listening on %s:%u") (line 99).
         Line 110: assert(m_open)  — post-condition.
         Returns Result::OK.

--- Phase 2: TcpBackend::accept_clients() ---
(Called from receive_message() line 337 each time receive_message() is invoked
 in server mode; or callable directly.)

Step 11. Entry assertions (lines 163–164):
           assert(m_is_server)  — true.
           assert(m_listen_fd >= 0)  — true.

Step 12. Capacity guard (line 166):
           if (m_client_count >= MAX_TCP_CONNECTIONS) return Result::OK immediately.
           (MAX_TCP_CONNECTIONS == 8.)

Step 13. socket_accept(m_listen_fd) (line 171) → SocketUtils.cpp line 250:
           - syscall: accept(fd, NULL, NULL)
           - Blocks until a client connects OR returns -1 (EAGAIN if non-blocking,
             or real error).  [NOTE: listen socket is NOT set non-blocking by this
             code path — see Section 14 Known Risks.]
           - On failure: Logger::log(WARNING_LO), return -1.

Step 14. If socket_accept returns client_fd < 0 (no pending connection or error):
           accept_clients() returns Result::OK (line 174).
           No state change.

Step 15. If socket_accept returns client_fd >= 0:
           Line 178: assert(m_client_count < MAX_TCP_CONNECTIONS)
           Line 179: m_client_fds[m_client_count] = client_fd.
           Line 180: ++m_client_count.
           Line 182-183: Logger::log(INFO, "Accepted client %u, total clients: %u").
           Line 185: assert(m_client_count <= MAX_TCP_CONNECTIONS)
           Returns Result::OK.

## 4. Call Tree (Hierarchical)

TcpBackend::init(config)                                [TcpBackend.cpp:48]
  RingBuffer::init()                                    [RingBuffer.hpp:76]
    m_head.store(0, relaxed)
    m_tail.store(0, relaxed)
  impairment_config_default(imp_cfg)                    [inline, ChannelConfig.hpp approx]
  ImpairmentEngine::init(imp_cfg)                       [ImpairmentEngine.cpp:23]
    m_prng.seed(seed)
    memset(m_delay_buf, ...)
    memset(m_reorder_buf, ...)
  socket_create_tcp()                                   [SocketUtils.cpp:26]
    socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)           [POSIX syscall]
  socket_set_reuseaddr(m_listen_fd)                     [SocketUtils.cpp:95]
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, ...)       [POSIX syscall]
  socket_bind(m_listen_fd, ip, port)                    [SocketUtils.cpp:118]
    inet_aton(ip, &addr.sin_addr)                       [POSIX]
    bind(fd, &addr, sizeof(addr))                       [POSIX syscall]
  socket_listen(m_listen_fd, MAX_TCP_CONNECTIONS)       [SocketUtils.cpp:229]
    listen(fd, 8)                                       [POSIX syscall]

TcpBackend::accept_clients()                            [TcpBackend.cpp:161]
  socket_accept(m_listen_fd)                            [SocketUtils.cpp:250]
    accept(fd, NULL, NULL)                              [POSIX syscall]
  Logger::log(INFO, ...)                                [on success]

## 5. Key Components Involved

Component              File / Location                         Role
---------              ---------------                         ----
TcpBackend             src/platform/TcpBackend.cpp/.hpp        Orchestrates TCP lifecycle;
                                                               owns all state.
RingBuffer             src/core/RingBuffer.hpp                 Receive queue; initialized
                                                               during init().
ImpairmentEngine       src/platform/ImpairmentEngine.cpp       Initialized but not
                                                               exercised during accept.
SocketUtils            src/platform/SocketUtils.cpp/.hpp       All raw POSIX socket ops.
TransportConfig        src/core/ChannelConfig.hpp              Input configuration struct;
                                                               drives all branching.
Types.hpp              src/core/Types.hpp                      Constants (MAX_TCP_CONNECTIONS
                                                               = 8, SOCKET_RECV_BUF_BYTES
                                                               = 8192, etc.).
Logger                 src/core/Logger.hpp                     Event logging for all
                                                               transitions.

## 6. Branching Logic / Decision Points

Decision Point 1: config.is_server (TcpBackend.cpp line 66)
  true  → server path: socket_create_tcp → set_reuseaddr → bind → listen → m_open=true.
  false → client path: connect_to_server() (not covered in this use case).

Decision Point 2: socket_create_tcp() return value (line 69)
  fd >= 0 → continue.
  fd < 0  → Logger FATAL, return ERR_IO.

Decision Point 3: socket_set_reuseaddr() return value (line 75)
  true  → continue.
  false → Logger WARNING_HI, socket_close, m_listen_fd=-1, return ERR_IO.

Decision Point 4: socket_bind() return value (line 83)
  true  → continue.
  false → Logger FATAL, socket_close, m_listen_fd=-1, return ERR_IO.

Decision Point 5: socket_listen() return value (line 91)
  true  → m_open=true, log INFO, return OK.
  false → Logger FATAL, socket_close, m_listen_fd=-1, return ERR_IO.

Decision Point 6: m_client_count >= MAX_TCP_CONNECTIONS (accept_clients line 166)
  true  → return OK immediately (no accept attempted).
  false → proceed to socket_accept().

Decision Point 7: socket_accept() return value (accept_clients line 172)
  client_fd >= 0 → store in m_client_fds[m_client_count], increment m_client_count.
  client_fd < 0  → return OK silently (EAGAIN or real error, both treated the same).

Decision Point 8: config.num_channels > 0 (init line 61)
  true  → imp_cfg.enabled = config.channels[0].impairments_enabled.
  false → imp_cfg.enabled remains at default (false).

## 7. Concurrency / Threading Behavior

- TcpBackend has no internal threads; it is single-threaded by design.
- init() and accept_clients() are expected to be called from the same thread.
- RingBuffer uses std::atomic<uint32_t> (m_head, m_tail) to support an SPSC model;
  however, during init(), only relaxed stores are issued (no concurrent access yet).
- socket_accept(m_listen_fd) calls POSIX accept() which may block indefinitely
  if m_listen_fd is in blocking mode (see Section 14).
- m_client_fds[] is a plain int array — no atomic protection. [ASSUMPTION: access is
  serialized by single-threaded use of the TcpBackend object.]
- ImpairmentEngine::init() is called before any concurrent use; its PRNG state is
  not thread-safe. [ASSUMPTION: single-threaded use maintained.]

## 8. Memory & Ownership Semantics (C/C++ Specific)

- All storage is static / stack:
  - m_client_fds[MAX_TCP_CONNECTIONS] = m_client_fds[8]: plain int array, inline in
    TcpBackend object; no heap allocation.
  - m_wire_buf[SOCKET_RECV_BUF_BYTES] = m_wire_buf[8192]: inline uint8_t array.
  - m_recv_queue (RingBuffer): contains m_buf[64] of MessageEnvelope inline.
  - m_impairment (ImpairmentEngine): m_delay_buf[32], m_reorder_buf[32] inline.
  - m_cfg (TransportConfig): full struct copy on assignment at line 53 (includes
    channels[MAX_CHANNELS] = channels[8] inline).
- TransportConfig.bind_ip/peer_ip are char[48] fixed arrays; copied by value.
- Power of 10 rule 3: no malloc/new used anywhere in this path.
- socket fds (m_listen_fd, m_client_fds[i]) are OS resources; owned by TcpBackend;
  released in close() or on error paths.
- If init() fails mid-way, the partially created fd is explicitly closed before
  return (lines 78–79, 86, 94); no leak.
- Accepted client_fd ownership transfers to TcpBackend upon socket_accept() success.

## 9. Error Handling Flow

Error source              Detected at             Action
------------              -----------             ------
socket() fails            socket_create_tcp()     returns -1
                          init() line 69          Logger FATAL; return ERR_IO (no fd to close)
setsockopt() fails        socket_set_reuseaddr()  returns false
                          init() line 75          Logger WARNING_HI; socket_close(m_listen_fd);
                                                  m_listen_fd=-1; return ERR_IO
bind() fails              socket_bind()           returns false
                          init() line 83          Logger FATAL; socket_close(m_listen_fd);
                                                  m_listen_fd=-1; return ERR_IO
listen() fails            socket_listen()         returns false
                          init() line 91          Logger FATAL; socket_close(m_listen_fd);
                                                  m_listen_fd=-1; return ERR_IO
accept() fails/EAGAIN     socket_accept()         returns -1; Logger WARNING_LO
                          accept_clients() ln 172 return Result::OK (non-fatal; no client added)
At-capacity guard         accept_clients() ln 166 return Result::OK silently (no accept issued)
Assertion failure         Any assert()            abort() in debug; undefined in production
                                                  [ASSUMPTION: asserts enabled in debug builds]

Severity classification (F-Prime model):
  - FATAL:      socket(), bind(), listen() failures — unrecoverable for this init sequence.
  - WARNING_HI: setsockopt() failure.
  - WARNING_LO: accept() failure / EAGAIN.

## 10. External Interactions

Interaction               syscall / API                    When
-----------               -------------                    ----
Create TCP socket         socket(AF_INET,SOCK_STREAM,TCP)  init() step 6
Set SO_REUSEADDR          setsockopt(SO_REUSEADDR)         init() step 7
Bind to address/port      bind()                           init() step 8
Enable listening queue    listen(fd, 8)                    init() step 9
Accept client connection  accept(fd, NULL, NULL)           accept_clients() step 13
Log events                Logger::log()                    Multiple points

No network I/O (send/recv) occurs during init() or accept_clients().
No TLS/DTLS hooks are invoked in these paths (extension point exists but
is not implemented here).

## 11. State Changes / Side Effects

Before init():           After init() success:
  m_listen_fd = -1         m_listen_fd = valid fd (>= 0)
  m_client_count = 0       m_client_count = 0 (unchanged until accept)
  m_open = false           m_open = true
  m_is_server = false      m_is_server = true
  m_recv_queue: uninit     m_recv_queue: head=0, tail=0
  m_impairment: uninit     m_impairment: m_initialized=true, PRNG seeded

After accept_clients() success (one client):
  m_client_fds[0] = valid client fd
  m_client_count = 1
  OS kernel: connection accepted, client TCP state transitions to ESTABLISHED.
  Logger: INFO event written.

Side effects:
  - OS file descriptor table: m_listen_fd allocated; client fds allocated on accept.
  - Kernel TCP listening queue: established by listen(); reduced by accept().
  - Logger output (informational / warning depending on path).

## 12. Sequence Diagram (ASCII)

Caller          TcpBackend        SocketUtils           OS Kernel
------          ----------        -----------           ---------
  |                 |                 |                     |
  |--init(cfg)----->|                 |                     |
  |                 |--recv_queue.init()                    |
  |                 |--impairment.init()                    |
  |                 |--socket_create_tcp()-->|              |
  |                 |                 |--socket(STREAM)---->|
  |                 |                 |<--fd----------------| 
  |                 |<--fd------------|                     |
  |                 |--set_reuseaddr(fd)-->|                |
  |                 |                 |--setsockopt()------>|
  |                 |                 |<--0-----------------|
  |                 |<--true----------|                     |
  |                 |--socket_bind(fd,ip,port)-->|          |
  |                 |                 |--bind()------------>|
  |                 |                 |<--0-----------------|
  |                 |<--true----------|                     |
  |                 |--socket_listen(fd,8)-->|              |
  |                 |                 |--listen()---------->|
  |                 |                 |<--0-----------------|
  |                 |<--true----------|                     |
  |                 | m_open=true                           |
  |<--Result::OK----|                 |                     |
  |                 |                 |                     |
  |                 |                 |          [client connects]
  |                 |                 |                     |
  |--receive_message() or accept_clients() call:           |
  |                 |                 |                     |
  |                 |--accept_clients()|                    |
  |                 |--socket_accept(listen_fd)-->|         |
  |                 |                 |--accept()---------->|
  |                 |                 |<--client_fd---------|
  |                 |<--client_fd-----|                     |
  |                 | m_client_fds[0]=client_fd             |
  |                 | m_client_count=1                      |
  |                 |--Logger INFO                          |
  |<--Result::OK----|                 |                     |

## 13. Initialization vs Runtime Flow

Initialization phase (init()):
  - All memory zeroed / set to sentinel values in constructor (lines 26–32).
  - RingBuffer reset to empty.
  - ImpairmentEngine configured and PRNG seeded.
  - OS resources allocated: listen socket created, bound, put into listen state.
  - m_open = true marks transition from init to runtime.
  - No message I/O occurs.

Runtime phase (accept_clients()):
  - Called from receive_message() on every invocation in server mode (line 337),
    making it a lightweight, non-allocating polling operation.
  - Exactly one accept() per accept_clients() call (no loop); at most one new
    client per call.
  - Pure OS resource acquisition: no serialization, no impairment logic, no
    RingBuffer involvement.
  - m_client_fds[] and m_client_count updated as the only persistent state changes.

## 14. Known Risks / Observations

Risk 1: BLOCKING accept()
  socket_accept() calls accept(fd, NULL, NULL) without first setting m_listen_fd
  to non-blocking mode (O_NONBLOCK is NOT set on the listen socket in init()).
  If no client is pending, accept() will block indefinitely in the calling thread.
  This conflicts with the non-blocking design intent expressed in the comment at
  line 173 ("No pending connection (EAGAIN in non-blocking mode) is not an error").
  [OBSERVATION: The code assumes the caller understands this; accept_clients() is
  called from receive_message() which has its own timeout loop. Whether blocking
  is actually avoided depends on whether the OS happens to have a pending connection
  at that instant.]

Risk 2: One accept per receive_message() call
  accept_clients() accepts at most one client per call. A burst of simultaneous
  connects will be queued in the kernel backlog (depth = MAX_TCP_CONNECTIONS = 8)
  and accepted one-per-receive_message() poll cycle. This is bounded but slow.

Risk 3: accept() error vs EAGAIN indistinguishable
  socket_accept() returns -1 for both EAGAIN (no client ready) and genuine errors
  (e.g., EBADF, ENFILE). Both are logged at WARNING_LO and treated identically —
  accept_clients() returns Result::OK in both cases. Real errors are silently swallowed.

Risk 4: m_client_fds[] array shift on disconnect
  The array is compacted by a linear shift in recv_from_client() (lines 212–215).
  This is correct but O(N) and relies on the fact that indices are stable during a
  single receive_message() call. [OBSERVATION: Safe given single-threaded usage.]

Risk 5: Impairment engine initialized but unused during accept
  m_impairment is initialized here even though accept is a pure connection management
  operation. This is harmless overhead but is worth noting for code tracing.

## 15. Unknowns / Assumptions

[ASSUMPTION A]  The listen socket (m_listen_fd) is left in blocking mode because
                socket_set_nonblocking() is never called on it. Behavior of accept()
                with a blocking fd when no client is present will block the thread.

[ASSUMPTION B]  impairment_config_default() is declared as a free function (possibly
                inline in ImpairmentEngine.hpp); its exact signature was not read but
                its effect is inferred from ImpairmentEngine::init() usage.

[ASSUMPTION C]  Logger::log() is a thread-safe, non-blocking call. Its implementation
                was not read; it may or may not be re-entrant.

[ASSUMPTION D]  envelope_valid(), envelope_init(), envelope_copy() are defined in
                MessageEnvelope.hpp/.cpp; their exact implementations were not read.
                It is assumed they perform field validation and safe deep copy.

[ASSUMPTION E]  m_prng (used in ImpairmentEngine) is a custom PRNG type with seed()
                and next_double()/next_range() methods. Its exact type is not declared
                in the files read. [ASSUMPTION: deterministic, no dynamic allocation.]

[ASSUMPTION F]  Multiple calls to init() on the same TcpBackend object are prevented
                by the assert(!m_open) at line 51. If m_open is true, the assert
                fires in debug; behavior in production is undefined.

[ASSUMPTION G]  The caller always calls close() (or relies on the destructor) to
                release OS resources. No automatic recovery mechanism exists if init()
                is called without a matching close().