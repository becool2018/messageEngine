## 1. Use Case Overview

Use Case: Receiver deserializes an inbound TCP-framed wire message into a MessageEnvelope.

A remote sender has serialized a MessageEnvelope into a big-endian wire format, prefixed it with a 4-byte big-endian length header, and pushed it over an established TCP connection. On the receiving side, TcpBackend::receive_message() is called by the application (or by DeliveryEngine). The goal is to trace the complete path from raw bytes arriving on the OS socket through tcp_recv_frame(), Serializer::deserialize(), RingBuffer::push(), and back out through TcpBackend::receive_message() returning a fully-populated MessageEnvelope to the caller.

Scope: platform layer (TcpBackend, SocketUtils) and core layer (Serializer, RingBuffer). The DeliveryEngine is the typical caller but is not the focus here; it is noted where it interacts.

## 2. Entry Points

Primary external entry point:
    TcpBackend::receive_message(MessageEnvelope& envelope, uint32_t timeout_ms)
    File: src/platform/TcpBackend.cpp, lines 325-380

This is the sole public receive API exposed by TcpBackend (implementing TransportInterface). It is called by DeliveryEngine::receive() at line 159 of DeliveryEngine.cpp, which itself is called by application code.

Secondary internal entry points (called from receive_message):
    TcpBackend::accept_clients()                  — TcpBackend.cpp lines 161-187
    TcpBackend::recv_from_client(int, uint32_t)   — TcpBackend.cpp lines 193-243
    tcp_recv_frame(int, uint8_t*, uint32_t, uint32_t, uint32_t*)  — SocketUtils.cpp lines 429-476
    Serializer::deserialize(const uint8_t*, uint32_t, MessageEnvelope&) — Serializer.cpp lines 173-257
    RingBuffer::push(const MessageEnvelope&)       — RingBuffer.hpp lines 127-146
    RingBuffer::pop(MessageEnvelope&)              — RingBuffer.hpp lines 159-178

## 3. End-to-End Control Flow (Step-by-Step)

Step 1 — Caller invokes receive_message
    DeliveryEngine::receive() (DeliveryEngine.cpp:159) calls:
        m_transport->receive_message(env, timeout_ms)
    This dispatches to TcpBackend::receive_message(envelope, timeout_ms).

Step 2 — Precondition check (TcpBackend.cpp:327-328)
    assert(m_open) fires first. If m_open is false, execution aborts in debug builds.

Step 3 — Fast-path: check RingBuffer for already-queued messages (TcpBackend.cpp:330-333)
    Result res = m_recv_queue.pop(envelope);
    RingBuffer::pop() (RingBuffer.hpp:159-178):
        Loads m_head (relaxed), m_tail (acquire).
        Computes cnt = t - h.
        If cnt == 0: returns ERR_EMPTY immediately. Control continues to Step 4.
        If cnt > 0: copies m_buf[h & RING_MASK] into envelope via envelope_copy() (memcpy of sizeof(MessageEnvelope)), advances m_head (release store), returns OK.
    If OK: receive_message returns OK immediately. Skip to Step 15.

Step 4 — Server mode: attempt to accept new clients (TcpBackend.cpp:336-338)
    If m_is_server == true:
        accept_clients() is called (result ignored with (void)).
        accept_clients() (TcpBackend.cpp:161-187):
            Calls socket_accept(m_listen_fd) — wraps POSIX accept(fd, NULL, NULL).
            If a new fd is returned and m_client_count < MAX_TCP_CONNECTIONS:
                m_client_fds[m_client_count] = client_fd; ++m_client_count.

Step 5 — Compute polling iteration count (TcpBackend.cpp:341-344)
    poll_count = (timeout_ms + 99U) / 100U; capped at 50.
    Each iteration represents a 100 ms polling window.

Step 6 — Outer polling loop begins (TcpBackend.cpp:346)
    for (uint32_t attempt = 0U; attempt < poll_count; ++attempt)

Step 7 — Inner loop: receive from all connected clients (TcpBackend.cpp:347-352)
    for (uint32_t i = 0U; i < MAX_TCP_CONNECTIONS; ++i)
        Skip if m_client_fds[i] < 0 (slot unused).
        Call: recv_from_client(m_client_fds[i], 100U)
            [result is discarded with (void)]

Step 8 — Inside recv_from_client (TcpBackend.cpp:193-243)
    Preconditions: assert(client_fd >= 0), assert(m_open).
    Calls: tcp_recv_frame(client_fd, m_wire_buf, SOCKET_RECV_BUF_BYTES, timeout_ms, &out_len)
        SOCKET_RECV_BUF_BYTES = 8192, timeout_ms = 100.

Step 9 — Inside tcp_recv_frame (SocketUtils.cpp:429-476)
    Preconditions: assert fd>=0, buf!=NULL, buf_cap>0, out_len!=NULL.
    9a. Read 4-byte length header:
        Calls socket_recv_exact(fd, header, 4U, timeout_ms)
        socket_recv_exact (SocketUtils.cpp:337-385):
            Loop while received < 4:
                poll(fd, POLLIN, timeout_ms) — blocks up to 100 ms.
                recv(fd, &buf[received], remaining, 0).
                If recv returns 0: socket closed, return false.
                If recv < 0: error, return false.
                received += recv_result.
        If socket_recv_exact returns false: tcp_recv_frame logs WARNING_HI, returns false.
    9b. Parse frame_len from header[0..3] (big-endian shifts).
    9c. Validate frame_len:
        max_frame_size = Serializer::WIRE_HEADER_SIZE + MSG_MAX_PAYLOAD_BYTES = 44 + 4096 = 4140.
        If frame_len > 4140 or frame_len > buf_cap(8192): log WARNING_HI, return false.
    9d. Read payload bytes:
        Calls socket_recv_exact(fd, buf, frame_len, timeout_ms).
        Same poll-recv loop, now for frame_len bytes.
    9e. *out_len = frame_len.
    9f. Postconditions: assert *out_len <= buf_cap, *out_len <= max_frame_size.
    Returns true.

Step 10 — Back in recv_from_client: check tcp_recv_frame result (TcpBackend.cpp:200-222)
    If tcp_recv_frame returned false:
        Logs WARNING_LO "recv_frame failed; closing connection".
        Finds this fd in m_client_fds[], calls socket_close(), shifts remaining entries,
        decrements m_client_count.
        Returns ERR_IO.
    If true: proceeds to deserialization.

Step 11 — Deserialization (TcpBackend.cpp:226-232, Serializer.cpp:173-257)
    MessageEnvelope env declared on stack (local to recv_from_client).
    Calls: Serializer::deserialize(m_wire_buf, out_len, env)
    Inside Serializer::deserialize:
        Preconditions: assert(buf != nullptr), assert(buf_len <= 0xFFFFFFFFUL).
        If buf_len < WIRE_HEADER_SIZE(44): return ERR_INVALID.
        Calls envelope_init(env) — memset env to 0, sets message_type = INVALID.
        offset = 0.
        Reads fields in strict big-endian order using read_u8 / read_u32 / read_u64:
            env.message_type    = read_u8(buf, 0)  → offset 1
            env.reliability_class = read_u8(buf, 1) → offset 2
            env.priority        = read_u8(buf, 2)  → offset 3
            padding1            = read_u8(buf, 3)  → offset 4; validated == 0
            env.message_id      = read_u64(buf, 4) → offset 12
            env.timestamp_us    = read_u64(buf,12) → offset 20
            env.source_id       = read_u32(buf,20) → offset 24
            env.destination_id  = read_u32(buf,24) → offset 28
            env.expiry_time_us  = read_u64(buf,28) → offset 36
            env.payload_length  = read_u32(buf,36) → offset 40
            padding2            = read_u32(buf,40) → offset 44; validated == 0
        assert(offset == 44) — WIRE_HEADER_SIZE.
        Validates env.payload_length <= MSG_MAX_PAYLOAD_BYTES(4096).
        Validates buf_len >= 44 + env.payload_length.
        If env.payload_length > 0: memcpy(env.payload, &buf[44], env.payload_length).
        Postcondition: assert(envelope_valid(env)).
        Returns OK.
    If deserialize returns non-OK (TcpBackend.cpp:228-232):
        Logs WARNING_LO "Deserialize failed".
        Returns the error result from recv_from_client.

Step 12 — Push into RingBuffer (TcpBackend.cpp:235-239)
    res = m_recv_queue.push(env)
    Inside RingBuffer::push (RingBuffer.hpp:127-146):
        Loads m_tail (relaxed), m_head (acquire).
        cnt = t - h.
        assert(cnt <= MSG_RING_CAPACITY).
        If cnt >= MSG_RING_CAPACITY: returns ERR_FULL.
        envelope_copy(m_buf[t & RING_MASK], env) — memcpy of full MessageEnvelope.
        m_tail.store(t + 1U, release).
        Returns OK.
    If ERR_FULL: TcpBackend logs WARNING_HI "Recv queue full; dropping message".
    Postcondition: assert(out_len <= SOCKET_RECV_BUF_BYTES).
    recv_from_client returns OK (regardless of push result, because the frame was valid).

Step 13 — Back in outer polling loop (TcpBackend.cpp:355-358)
    res = m_recv_queue.pop(envelope);
    If OK (message now in queue): receive_message returns OK. Proceed to Step 15.

Step 14 — Check impairment-delayed messages (TcpBackend.cpp:361-375)
    uint64_t now_us = timestamp_now_us() — clock_gettime(CLOCK_MONOTONIC).
    m_impairment.collect_deliverable(now_us, delayed, IMPAIR_DELAY_BUF_SIZE).
    Any deliverable delayed envelopes are pushed into m_recv_queue.
    Final m_recv_queue.pop(envelope) attempted.
    If OK: receive_message returns OK.
    If still empty: loop continues or falls through to Step 15.

Step 15 — Return path
    If timeout exhausted with no message: receive_message returns ERR_TIMEOUT.
    If a message was successfully popped: returns OK with envelope populated.
    Caller (DeliveryEngine::receive) checks the return value at line 160.

## 4. Call Tree (Hierarchical)

TcpBackend::receive_message(envelope, timeout_ms)
    RingBuffer::pop(envelope)                           [fast path check]
        m_head.load(relaxed), m_tail.load(acquire)
        envelope_copy() [memcpy]
        m_head.store(h+1, release)
    TcpBackend::accept_clients()                        [server mode only]
        socket_accept(m_listen_fd)
            accept(fd, NULL, NULL)                      [POSIX]
    [polling loop, up to 50 iterations]
        TcpBackend::recv_from_client(client_fd, 100ms)
            tcp_recv_frame(fd, m_wire_buf, 8192, 100, &out_len)
                socket_recv_exact(fd, header, 4, 100)
                    poll(fd, POLLIN, 100)               [POSIX]
                    recv(fd, buf, remaining, 0)         [POSIX]
                [parse frame_len from 4-byte BE header]
                [validate frame_len vs 4140 and 8192]
                socket_recv_exact(fd, buf, frame_len, 100)
                    poll(fd, POLLIN, 100)               [POSIX]
                    recv(fd, buf+received, remaining, 0)[POSIX]
            Serializer::deserialize(m_wire_buf, out_len, env)
                envelope_init(env)                      [memset]
                read_u8(buf, offset) x4
                read_u64(buf, offset) x3
                read_u32(buf, offset) x3
                read_u8(buf, offset) [padding validate]
                read_u32(buf, offset) [padding validate]
                memcpy(env.payload, &buf[44], payload_length)
                envelope_valid(env)                     [inline predicate]
            RingBuffer::push(env)
                m_tail.load(relaxed), m_head.load(acquire)
                envelope_copy(m_buf[slot], env)         [memcpy]
                m_tail.store(t+1, release)
        RingBuffer::pop(envelope)                       [check after each client poll]
        timestamp_now_us()                              [clock_gettime]
        ImpairmentEngine::collect_deliverable(...)
        RingBuffer::push(delayed[i])                    [0..n delayed envelopes]
        RingBuffer::pop(envelope)                       [final check per iteration]
    return ERR_TIMEOUT                                  [if loop exhausted]

## 5. Key Components Involved

TcpBackend (src/platform/TcpBackend.cpp/.hpp)
    Owns m_recv_queue (RingBuffer), m_impairment (ImpairmentEngine), m_wire_buf (uint8_t[8192]),
    m_client_fds[MAX_TCP_CONNECTIONS], m_client_count. Orchestrates all receive logic.

SocketUtils (src/platform/SocketUtils.cpp/.hpp)
    tcp_recv_frame: framing protocol (4-byte BE length prefix + payload).
    socket_recv_exact: guaranteed full-read loop with poll-based timeout.
    Both are stateless free functions — no object state.

Serializer (src/core/Serializer.cpp/.hpp)
    Stateless class (static methods only). Performs big-endian deserialization of the 44-byte
    wire header plus opaque payload bytes into MessageEnvelope fields.

RingBuffer (src/core/RingBuffer.hpp)
    SPSC lock-free queue using std::atomic<uint32_t>. Stores up to 64 MessageEnvelope objects.
    push() is the producer path; pop() is the consumer path.

MessageEnvelope (src/core/MessageEnvelope.hpp)
    Plain struct: 8 fields + payload[4096]. Total sizeof ~4128+ bytes.
    Copied by value throughout (envelope_copy = memcpy).

Timestamp (src/core/Timestamp.hpp)
    timestamp_now_us(): inline, calls clock_gettime(CLOCK_MONOTONIC).

Types.hpp
    Defines all constants: SOCKET_RECV_BUF_BYTES=8192, MSG_RING_CAPACITY=64,
    Serializer::WIRE_HEADER_SIZE=44, MSG_MAX_PAYLOAD_BYTES=4096.

## 6. Branching Logic / Decision Points

Decision 1 (TcpBackend.cpp:330-333): Is there already a message in m_recv_queue?
    YES → pop and return OK immediately (fast path, no socket I/O).
    NO  → continue to accept/poll loop.

Decision 2 (TcpBackend.cpp:336-338): Is this a server?
    YES → call accept_clients() to accept any new pending connections.
    NO  → skip.

Decision 3 (TcpBackend.cpp:341-344): Compute poll_count.
    timeout_ms=0 → poll_count=0, loop never executes, return ERR_TIMEOUT.
    timeout_ms=100 → poll_count=1.
    timeout_ms > 5000 → poll_count capped at 50.

Decision 4 (TcpBackend.cpp:348-349): Is m_client_fds[i] valid?
    fd < 0 → skip that slot.
    fd >= 0 → call recv_from_client.

Decision 5 (SocketUtils.cpp:440, 463): Did socket_recv_exact succeed?
    Poll returned <= 0 (timeout/error) → return false from tcp_recv_frame.
    recv() returned 0 (graceful close) → return false.
    recv() returned < 0 (error) → return false.
    All bytes received → continue.

Decision 6 (SocketUtils.cpp:453-458): Is frame_len within bounds?
    frame_len > 4140 or > 8192 → reject, return false.
    Otherwise: proceed to read payload.

Decision 7 (TcpBackend.cpp:200-222): Did tcp_recv_frame fail?
    YES → close and remove the fd from m_client_fds[], return ERR_IO.
    NO  → proceed to deserialization.

Decision 8 (Serializer.cpp:205-207): Is padding1 == 0?
    NO → return ERR_INVALID (malformed wire frame).

Decision 9 (Serializer.cpp:228-232): Is padding2 == 0?
    NO → return ERR_INVALID.

Decision 10 (Serializer.cpp:238-240): Is payload_length <= MSG_MAX_PAYLOAD_BYTES?
    NO → return ERR_INVALID.

Decision 11 (Serializer.cpp:243-246): Does buf_len >= WIRE_HEADER_SIZE + payload_length?
    NO → return ERR_INVALID (truncated buffer).

Decision 12 (TcpBackend.cpp:228-232): Did Serializer::deserialize fail?
    YES → log and return error from recv_from_client.

Decision 13 (RingBuffer.hpp:135-137): Is the ring buffer full?
    YES → push returns ERR_FULL; TcpBackend logs WARNING_HI; message is dropped.
    NO  → message enqueued.

Decision 14 (TcpBackend.cpp:355-358): Did pop succeed after recv_from_client?
    YES → return OK from receive_message.
    NO  → check impairment engine, try once more, continue loop.

## 7. Concurrency / Threading Behavior

RingBuffer contract: Exactly one producer thread (calling push) and one consumer thread (calling pop). In TcpBackend's design, recv_from_client is the producer (calls push) and receive_message is the consumer (calls pop). Both are called from the same thread in TcpBackend (receive_message calls recv_from_client which calls push, then receive_message calls pop). This is single-threaded at the TcpBackend level, satisfying the SPSC contract.

Memory ordering: push uses m_head.load(acquire) to observe freed slots, then m_tail.store(release) to publish data. pop uses m_tail.load(acquire) to observe new data, then m_head.store(release) to free slots. The acquire/release pair forms a synchronizes-with relationship.

socket_recv_exact uses poll() as a blocking wait before each recv(). The poll timeout of 100 ms per attempt caps the maximum blocking time per client per outer loop iteration.

No mutexes or condition variables are used. No locking around m_client_fds manipulation. [ASSUMPTION: TcpBackend is single-threaded; concurrent calls to receive_message would cause data races on m_client_fds and m_wire_buf.]

timestamp_now_us uses CLOCK_MONOTONIC, which is per-thread safe on POSIX.

## 8. Memory & Ownership Semantics (C/C++ Specific)

m_wire_buf (TcpBackend member, uint8_t[8192]):
    Stack-like semantics; member variable allocated with TcpBackend object.
    Written by tcp_recv_frame into TcpBackend::recv_from_client.
    Passed by pointer to Serializer::deserialize. NOT thread-safe if two threads call
    recv_from_client simultaneously (single shared buffer for all clients).

MessageEnvelope env (local to recv_from_client, TcpBackend.cpp:226):
    Declared on the stack of recv_from_client. Populated by Serializer::deserialize.
    Passed by const reference to RingBuffer::push, which internally copies it via envelope_copy
    (memcpy of sizeof(MessageEnvelope) bytes) into m_buf[slot] in the ring buffer.

m_buf[MSG_RING_CAPACITY] (RingBuffer member, inline static allocation):
    64 MessageEnvelope objects embedded in the RingBuffer object.
    Written by push (producer), read by pop (consumer) with acquire/release ordering.

envelope_copy: always a full memcpy of sizeof(MessageEnvelope). For a 4096-byte payload array
plus header fields, this is approximately 4128+ bytes per copy. Two copies occur:
    1. Serializer writes env on recv_from_client's stack.
    2. RingBuffer::push copies that env into m_buf[slot].
    3. RingBuffer::pop copies m_buf[slot] into the caller's envelope (a third copy).

No dynamic allocation anywhere on this path. All buffers are statically sized.

envelope_init uses memset (SocketUtils calls indirectly through Serializer).
Padding bytes are validated on read, enforcing wire format correctness.

## 9. Error Handling Flow

ERR_TIMEOUT (Result::ERR_TIMEOUT = 1):
    Returned by receive_message when the outer polling loop exhausts all poll_count attempts
    without a message arriving. Propagates to DeliveryEngine::receive which returns it to
    the application.

ERR_IO (Result::ERR_IO = 5):
    Returned by recv_from_client when tcp_recv_frame fails (socket closed, poll timeout, recv error).
    TcpBackend closes and removes the offending fd from m_client_fds[].
    The outer receive_message loop continues to other clients.

ERR_INVALID (Result::ERR_INVALID = 4):
    Returned by Serializer::deserialize if:
        - Buffer too short (< 44 bytes).
        - Padding bytes non-zero.
        - payload_length > 4096.
        - Total buffer insufficient for header + payload.
    Logged at WARNING_LO, returned from recv_from_client, result discarded in receive_message
    polling loop (with (void) cast at line 351). The bad frame is silently dropped.

ERR_FULL (Result::ERR_FULL = 2):
    Returned by RingBuffer::push if the 64-slot queue is exhausted.
    Logged at WARNING_HI. The deserialized envelope is discarded. recv_from_client still
    returns OK (the frame was received; overflow is a policy decision, not an I/O error).

Assertion failures (debug builds only):
    assert(m_open) in receive_message: fires if called before init or after close.
    assert(fd >= 0) in tcp_recv_frame and socket_recv_exact.
    assert(offset == WIRE_HEADER_SIZE) in Serializer::deserialize: fires if read logic drifts.
    assert(envelope_valid(env)) at end of deserialize: fires if deserialization produced
    an invalid envelope despite passing all earlier checks.

## 10. External Interactions

POSIX kernel (socket I/O):
    poll(fd, POLLIN, timeout_ms) — called once per partial-read iteration.
    recv(fd, buf, len, 0) — called one or more times per logical read (partial read handling).
    Both in socket_recv_exact (SocketUtils.cpp:337-385).
    accept(m_listen_fd, NULL, NULL) — called in socket_accept for server mode.

CLOCK_MONOTONIC:
    clock_gettime(CLOCK_MONOTONIC, &ts) — called in timestamp_now_us() once per outer loop
    iteration (TcpBackend.cpp:361), and also whenever delayed messages are collected.

Logger:
    Logger::log() called at WARNING_LO, WARNING_HI, and INFO severity at various points.
    Logging is the only externally visible side effect on non-error paths.
    [ASSUMPTION: Logger is thread-safe or single-threaded; implementation not shown.]

ImpairmentEngine:
    collect_deliverable() is called once per outer loop iteration to drain latency-buffered
    messages. Its internal state is not traced here (out of scope for this use case).

## 11. State Changes / Side Effects

TcpBackend state changes:
    m_client_fds[i] set to -1 and m_client_count decremented if tcp_recv_frame fails
    (connection loss cleanup in recv_from_client, TcpBackend.cpp:206-218).
    m_client_fds[m_client_count] and ++m_client_count if accept_clients accepts a new connection.

RingBuffer state changes:
    m_tail incremented (release store) by push when a message is enqueued.
    m_head incremented (release store) by pop when a message is consumed.

Serializer: no state (static methods only). No side effects beyond writing the output envelope.

SocketUtils: no object state. Side effects are kernel-level (bytes consumed from TCP socket
receive buffer; fd state potentially changed to closed).

MessageEnvelope env (local to recv_from_client): created on stack, destroyed on return.
    Its content is immortalized inside m_buf[slot] before destruction.

## 12. Sequence Diagram (ASCII)

Caller          DeliveryEngine    TcpBackend          SocketUtils           Serializer    RingBuffer
  |                   |               |                     |                    |              |
  |---receive()------>|               |                     |                    |              |
  |                   |---recv_msg()->|                     |                    |              |
  |                   |               |---pop(env)--------->|                    |              |
  |                   |               |   [empty]           |                    |              |
  |                   |               |<--ERR_EMPTY---------|                    |              |
  |                   |               |                     |                    |              |
  |                   |               |---accept_clients()  |                    |              |
  |                   |               |  (server mode)      |                    |              |
  |                   |               |                     |                    |              |
  |                   |               |== polling loop ==   |                    |              |
  |                   |               |                     |                    |              |
  |                   |               |---recv_from_client(fd, 100ms)           |              |
  |                   |               |     |---tcp_recv_frame(fd,...,&out_len)->|              |
  |                   |               |     |       |---socket_recv_exact(4)-->[kernel:poll]   |
  |                   |               |     |       |                          [kernel:recv]   |
  |                   |               |     |       |   [parse 4-byte BE len = frame_len]      |
  |                   |               |     |       |---socket_recv_exact(frame_len)->[kernel] |
  |                   |               |     |<------true, *out_len=frame_len                   |
  |                   |               |     |---deserialize(m_wire_buf, out_len, env)---------->|
  |                   |               |     |       envelope_init(env) [memset]                |
  |                   |               |     |       read_u8/u32/u64 fields...                  |
  |                   |               |     |       memcpy(payload)                            |
  |                   |               |     |<------OK, env populated ----------------------->|
  |                   |               |     |---push(env)--------------------------------->    |
  |                   |               |     |                                           [m_tail++]
  |                   |               |     |<--OK---------------------------------------      |
  |                   |               |                                                         |
  |                   |               |---pop(envelope)------------------------------------>    |
  |                   |               |<--OK, envelope filled------------------------------    |
  |                   |<--OK----------|                     |                    |              |
  |<--OK--------------|               |                     |                    |              |

## 13. Initialization vs Runtime Flow

Initialization phase (TcpBackend::init, TcpBackend.cpp:48-112):
    m_recv_queue.init() — zeroes m_head and m_tail (relaxed stores); safe before concurrent access.
    m_impairment.init(imp_cfg) — sets up impairment engine state.
    For server mode: socket_create_tcp, socket_set_reuseaddr, socket_bind, socket_listen.
    For client mode: socket_create_tcp, socket_set_reuseaddr, socket_connect_with_timeout.
    m_open set to true. All heap-equivalent state is in-object (no malloc/new).
    m_wire_buf is uninitialized at construction; first written by recv_from_client.

Runtime phase (receive_message called repeatedly):
    Every call is self-contained: pop first, then accept, then recv loop, then return.
    No persistent state is mutated between calls except:
        m_recv_queue (producer=recv_from_client, consumer=receive_message).
        m_client_fds / m_client_count (updated by accept_clients, recv_from_client).
        m_impairment internal state (not traced here).

Teardown (TcpBackend::close):
    All fds closed, m_client_count = 0, m_open = false.
    Destructor calls close() if m_open is still true.

## 14. Known Risks / Observations

Risk 1 — Single shared m_wire_buf:
    TcpBackend has one m_wire_buf[8192] shared across all client connections. The inner loop
    processes clients sequentially but each call to recv_from_client overwrites the same buffer.
    This is safe as-is (single thread), but is a latent hazard if concurrency is ever introduced.

Risk 2 — Message silently dropped on RingBuffer full:
    If the consumer is slow, push() returns ERR_FULL and the deserialized frame is discarded
    permanently. WARNING_HI is logged but there is no flow-control feedback to the sender.
    Under sustained load this could cause silent data loss.

Risk 3 — recv_from_client result discarded:
    At TcpBackend.cpp:351, recv_from_client is called with (void), so ERR_IO (connection closed)
    is silently ignored in the outer loop. The fd cleanup still happens inside recv_from_client,
    but the caller never observes the loss of a connection except indirectly.

Risk 4 — Three full MessageEnvelope copies per message:
    Each message is copied: once from m_wire_buf into recv_from_client's local env; once from env
    into m_buf[slot] by push; once from m_buf[slot] to the caller's envelope by pop. Each copy
    is sizeof(MessageEnvelope) which is 4096 + ~32 bytes ≈ 4128 bytes.

Risk 5 — No integrity check on wire payload:
    There is no CRC, hash, or sequence number checked after deserialization. Bit errors in
    transit (e.g., under impairment simulation or non-TLS paths) produce silently corrupt
    envelopes if they do not violate the padding or length constraints.

Risk 6 — poll_count = 0 if timeout_ms = 0:
    A timeout_ms of 0 causes poll_count = 0. The inner recv loop never executes. Only
    the initial fast-path pop is attempted. This may be intentional for non-blocking checks,
    but should be documented in the API contract.

## 15. Unknowns / Assumptions

[ASSUMPTION 1] DeliveryEngine is the sole caller of receive_message in production.
    No other component calls TcpBackend::receive_message directly based on code read.

[ASSUMPTION 2] TcpBackend is operated single-threaded.
    No mutex protects m_wire_buf, m_client_fds, or m_client_count. The design requires
    that only one thread calls receive_message/send_message at a time.

[ASSUMPTION 3] Logger::log() is thread-safe or called from a single thread.
    The Logger implementation was not read; assumed safe based on usage pattern.

[ASSUMPTION 4] ImpairmentEngine::collect_deliverable() is side-effect free with respect to
    the TCP socket and does not call any socket APIs.

[ASSUMPTION 5] envelope_valid() postcondition after deserialize:
    The assertion assert(envelope_valid(env)) at Serializer.cpp:254 will not fire for any
    buffer that passes all prior checks, because message_type, payload_length, and source_id
    are already validated. However, if message_type maps to MessageType::INVALID (0xFF) via
    static_cast, or source_id == NODE_ID_INVALID (0), the assertion would fire.
    [The wire format allows any byte value for message_type; only INVALID = 255 is rejected.]

[ASSUMPTION 6] MSG_RING_CAPACITY (64) is a power of two.
    Asserted in RingBuffer::init(). RING_MASK = 63. The push/pop slot calculation
    (t & RING_MASK, h & RING_MASK) depends on this invariant.

[UNKNOWN 1] Whether m_recv_queue is the same object accessed by send_message's impairment
    delayed-message path. Review of send_message shows it uses m_recv_queue.push() for
    inbound delayed envelopes in receive_message's polling loop; it appears both paths
    share the same queue, which could cause contention if threading is introduced.

[UNKNOWN 2] Whether Serializer::deserialize validates message_type against the known enum values.
    The code does static_cast<MessageType>(read_u8(...)) without checking if the byte value
    is a valid MessageType enumerator. An unrecognized byte would produce an out-of-range enum,
    which is undefined behavior under C++17 for a scoped enum. envelope_valid() would catch
    MessageType::INVALID (255) but not all invalid values.