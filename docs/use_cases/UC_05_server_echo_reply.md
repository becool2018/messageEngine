# UC_05 — Server echo reply — calls receive_message() then send_message() in sequence

**HL Group:** Application Workflow (above system boundary) — combines HL-5 (receive) then
HL-1/2/3 (send); the two-step pattern is above the HL boundary
**Actor:** User/Application (Server process)
**Requirement traceability:** REQ-3.1, REQ-3.2.1, REQ-3.2.2, REQ-3.2.4, REQ-3.2.5,
REQ-3.2.6, REQ-3.3.2, REQ-3.3.3, REQ-4.1.2, REQ-4.1.3, REQ-7.1.1, REQ-7.1.2

---

## 1. Use Case Overview

**Trigger:** The server's main event loop (`run_server_iteration()` in `Server.cpp`) fires
once per iteration. It calls `DeliveryEngine::receive()` to wait for an inbound data message
and, if one arrives, calls `DeliveryEngine::send()` with a reply envelope whose payload is
an exact copy of the received payload (echo semantics).

**Goal:** Receive one data message from the transport and immediately transmit an echo reply
to the sender, inverting source and destination IDs and preserving payload and reliability
class. This is an application-layer pattern — it is above the HL system boundary and
documents a two-step combination of system calls rather than a single system primitive.

**Success outcome:** `receive()` returns `OK` with a DATA envelope, `send_echo_reply()`
returns `OK`, and the echo frame appears on the TCP socket pointing back to the original
sender.

**Error outcomes:**
- `receive()` returns `ERR_TIMEOUT` — no message arrived within `RECV_TIMEOUT_MS` (100 ms).
  The echo reply is not sent; the loop continues.
- `receive()` returns `ERR_EXPIRED` — expired message received; not a DATA message from the
  application's perspective (the message is dropped). No echo sent.
- `receive()` returns `ERR_DUPLICATE` — duplicate RELIABLE_RETRY message; dropped. No echo.
- `send_echo_reply()` returns non-OK — transport error or engine failure; WARNING_LO logged;
  the receive counter is incremented but the send counter is not.
- `envelope_valid(received)` check fails in `send_echo_reply()` — defensive assert fires;
  this should not occur if the transport and dedup logic are correct.

---

## 2. Entry Points

```
// src/app/Server.cpp
static void run_server_iteration(DeliveryEngine& engine,
                                 uint32_t& messages_received,
                                 uint32_t& messages_sent);

// Called inside run_server_iteration():
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::receive(MessageEnvelope& env,
                               uint32_t timeout_ms, uint64_t now_us);

// src/app/Server.cpp
static Result send_echo_reply(DeliveryEngine& engine,
                              const MessageEnvelope& received,
                              uint64_t now_us);

// Called inside send_echo_reply():
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us);
```

This UC is invoked from the server's main loop: `for (int iter = 0; iter < MAX_LOOP_ITERS
(100000); ++iter) { run_server_iteration(...); }`. Each iteration calls
`run_server_iteration()` once.

---

## 3. End-to-End Control Flow (Step-by-Step)

### Phase 1 — Receive

1. `run_server_iteration()` fires its two pre-condition asserts: `RECV_TIMEOUT_MS > 0`,
   `messages_sent <= messages_received + 1`.
2. Calls `timestamp_now_us()` → `now_us` (monotonic clock via `CLOCK_MONOTONIC`).
3. Declares `MessageEnvelope received` on the stack; calls `envelope_init(received)` —
   zero-initializes via `memset`, sets `message_type = INVALID`.
4. Calls `engine.receive(received, RECV_TIMEOUT_MS (100), now_us)`.
5. Inside `DeliveryEngine::receive()`:
   a. Pre-condition asserts: `m_initialized`, `now_us > 0`.
   b. Guard: `if (!m_initialized) return ERR_INVALID`.
   c. Calls `m_transport->receive_message(received, timeout_ms)` via virtual dispatch →
      `TcpBackend::receive_message()`.
   d. `TcpBackend::receive_message()` first tries `m_recv_queue.pop(received)`. If the queue
      has a message, returns `OK` immediately. Otherwise enters the poll loop (up to
      `poll_count = min(50, ceil(timeout_ms / 100))` iterations), calls
      `poll_clients_once(100)` which may call `recv_from_client()` and push to the queue,
      then retries `pop()`. On timeout: returns `ERR_TIMEOUT`.
   e. If transport returns non-OK: `receive()` returns that code immediately.
   f. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(received))` — asserts the envelope is valid.
   g. `timestamp_expired(received.expiry_time_us, now_us)` — if expired, logs WARNING_LO,
      returns `ERR_EXPIRED`.
   h. `envelope_is_control(received)` — if ACK/NAK/HEARTBEAT: if ACK, calls
      `m_ack_tracker.on_ack()` and `m_retry_manager.on_ack()`; returns `OK` (but
      `envelope_is_data()` will be false, so the outer check in `run_server_iteration()`
      skips the echo).
   i. `NEVER_COMPILED_OUT_ASSERT(envelope_is_data(received))` — for the normal DATA case.
   j. If `reliability_class == RELIABLE_RETRY`: calls `m_dedup.check_and_record(src, id)`.
      On `ERR_DUPLICATE`: returns `ERR_DUPLICATE`.
   k. Logs INFO, asserts post-condition, returns `OK`.
6. Back in `run_server_iteration()`: checks `result_ok(res) && envelope_is_data(received)`.
   - If not a DATA message or res != OK: no echo sent; continues to retry/sweep calls.

### Phase 2 — Echo reply

7. If Phase 1 succeeded with a DATA message: increments `messages_received`.
8. Logs INFO: `"Received msg#... from node ..., len ...: "` and prints payload via
   `print_payload_as_string()`.
9. Calls `send_echo_reply(engine, received, now_us)`.
10. Inside `send_echo_reply()`:
    a. Pre-condition asserts: `envelope_valid(received)`, `now_us > 0`.
    b. Declares `MessageEnvelope reply` on the stack; calls `envelope_init(reply)`.
    c. Builds reply envelope:
       - `reply.message_type = MessageType::DATA`.
       - `reply.source_id = received.destination_id` — the server's node ID that the
         original sender targeted.
       - `reply.destination_id = received.source_id` — the original sender.
       - `reply.priority = received.priority`.
       - `reply.reliability_class = received.reliability_class` — echo preserves the
         sender's reliability class.
       - `reply.timestamp_us = now_us`.
       - `reply.expiry_time_us = timestamp_deadline_us(now_us, 10000U)` — 10 second expiry.
       - `reply.payload_length = received.payload_length`.
    d. Bounds check: `if (reply.payload_length > MSG_MAX_PAYLOAD_BYTES) return ERR_INVALID`.
    e. `memcpy(reply.payload, received.payload, reply.payload_length)` — copies payload.
    f. Calls `engine.send(reply, now_us)`.
11. Inside `DeliveryEngine::send(reply, now_us)`:
    - Stamps `reply.source_id = m_local_id` (overwrites the value set above — see §14).
    - Stamps `reply.message_id = m_id_gen.next()`.
    - Stamps `reply.timestamp_us = now_us`.
    - Calls `send_via_transport(reply)` → `TcpBackend::send_message()` → serialize +
      impairment + flush + `send_frame()` (as in UC_01).
    - If `reliability_class == RELIABLE_RETRY` (or `RELIABLE_ACK`): calls
      `AckTracker::track()`.
    - If `reliability_class == RELIABLE_RETRY`: calls `RetryManager::schedule()`.
    - Returns `OK` or an error code.
12. Back in `send_echo_reply()`: checks `result_ok(res)`, logs WARNING_LO on failure, returns
    `res`.
13. Back in `run_server_iteration()`: if `result_ok(echo_res)`, increments `messages_sent`.

### Phase 3 — Retry pump and ACK sweep (each iteration)

14. `engine.pump_retries(now_us)` — collects any due retries from `RetryManager` and
    re-sends them. Logs count if > 0.
15. `engine.sweep_ack_timeouts(now_us)` — collects any expired PENDING ACK slots from
    `AckTracker`. Logs count as WARNING_HI if > 0.

---

## 4. Call Tree

```
run_server_iteration(engine, messages_received, messages_sent)
 ├── timestamp_now_us()
 ├── envelope_init(received)
 ├── DeliveryEngine::receive(received, 100, now_us)
 │    ├── TcpBackend::receive_message(received, 100)    [virtual dispatch]
 │    │    ├── RingBuffer::pop(received)                 [try queue first]
 │    │    └── [poll loop: poll_clients_once → recv_from_client → push]
 │    ├── timestamp_expired(expiry_us, now_us)
 │    ├── envelope_is_control(received)                  [ACK/NAK/HEARTBEAT branch]
 │    │    ├── AckTracker::on_ack(src, id)               [if ACK]
 │    │    └── RetryManager::on_ack(src, id)             [if ACK]
 │    └── DuplicateFilter::check_and_record(src, id)    [if RELIABLE_RETRY]
 ├── [if DATA] print_payload_as_string(payload, len)
 ├── [if DATA] send_echo_reply(engine, received, now_us)
 │    ├── envelope_init(reply)
 │    ├── [build reply fields + memcpy payload]
 │    └── DeliveryEngine::send(reply, now_us)
 │         ├── MessageIdGen::next()
 │         ├── DeliveryEngine::send_via_transport(reply)
 │         │    └── TcpBackend::send_message(reply)      [virtual dispatch]
 │         │         └── [serialize + impairment + flush + send_frame]
 │         ├── AckTracker::track(reply, deadline)        [if RELIABLE_ACK or RETRY]
 │         └── RetryManager::schedule(reply, ...)        [if RELIABLE_RETRY]
 ├── DeliveryEngine::pump_retries(now_us)
 └── DeliveryEngine::sweep_ack_timeouts(now_us)
```

---

## 5. Key Components Involved

- **`run_server_iteration()`** — Application-level orchestrator in `Server.cpp`. Combines
  receive and echo-send into one iteration. Not a system API; it is the application's event
  loop body.

- **`DeliveryEngine::receive()`** — System API. Delegates to `TcpBackend::receive_message()`,
  then applies expiry check, control-message dispatch (ACK processing), and dedup for
  RELIABLE_RETRY. Returns the DATA envelope to the application.

- **`TcpBackend::receive_message()`** — Polls the receive ring buffer, runs
  `poll_clients_once()` if the queue is empty, handles `flush_delayed_to_queue()` for
  impairment-delayed messages.

- **`send_echo_reply()`** — Application helper in `Server.cpp`. Constructs the reply
  envelope by inverting source/destination and copying the payload. Calls
  `DeliveryEngine::send()` which is the same SC function as UC_01–03.

- **`DeliveryEngine::send()`** — System API. Stamps `source_id`, `message_id`,
  `timestamp_us`, dispatches to transport, and optionally tracks/schedules.

- **`DeliveryEngine::pump_retries()` and `sweep_ack_timeouts()`** — Called each iteration
  for maintenance. Even though they are part of the loop, they are separate use cases
  (UC_10 and UC_11).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `receive()` returns non-OK | No echo; continue to pump/sweep | Continue to DATA check | Phase 3 |
| `envelope_is_data(received)` is false (ACK/control received) | No echo | — | Phase 3 |
| `timestamp_expired()` in `receive()` returns true | Return `ERR_EXPIRED` to `run_server_iteration()` | Continue | Data/control check |
| `envelope_is_control()` true (ACK received) | Call `on_ack()`, return `OK`; outer code skips echo (not DATA) | — | dedup check |
| `check_and_record()` returns `ERR_DUPLICATE` | Return `ERR_DUPLICATE`; no echo | Continue | Return envelope |
| `reply.payload_length > MSG_MAX_PAYLOAD_BYTES` | Return `ERR_INVALID` | memcpy + send | Return from `send_echo_reply()` |
| `engine.send(reply)` returns non-OK | Log WARNING_LO; return non-OK | Increment `messages_sent` | Return to `run_server_iteration()` |

---

## 7. Concurrency / Threading Behavior

- **Single-threaded:** `Server.cpp` uses one main thread. All operations in
  `run_server_iteration()` — receive, send, pump, sweep — are called sequentially on the
  same thread. No locks, no condition variables.

- **Signal handler interaction:** `g_stop_flag` is a `volatile sig_atomic_t` written by
  `signal_handler()` (SIGINT) and read by the main loop. This is async-signal-safe. No other
  shared state is accessed from the signal handler.

- **`RingBuffer` SPSC contract:** The `TcpBackend`'s `m_recv_queue` is a SPSC ring buffer.
  In the server, the poll thread and the application thread are the same thread, so the
  producer path (`recv_from_client()` → `push()`) and the consumer path (`receive_message()`
  → `pop()`) are both on the main thread. The SPSC contract is trivially satisfied.

- **`std::atomic` in `RingBuffer`:** `m_head` and `m_tail` use acquire/release ordering.
  When both producer and consumer are the same thread, the acquire/release pairs still
  provide the necessary happens-before relationship but impose no actual synchronization cost.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `received` envelope | `run_server_iteration()` stack | ≈ 4140 bytes | Populated by `receive()`; read-only after receipt |
| `reply` envelope | `send_echo_reply()` stack | ≈ 4140 bytes | Built from `received`; payload memcpy'd |
| `payload_buf[PAYLOAD_PRINT_MAX]` | `print_payload_as_string()` stack | 256 bytes | Used for console output only; does not affect network state |
| `m_wire_buf` (TcpBackend) | `TcpBackend` member | 8192 bytes | Reused by `send_echo_reply()` → `send_message()` |
| `delayed[]` in `flush_delayed_to_clients` | Stack of `flush_delayed_to_clients` | 32 × ~4140 ≈ 132 KB | See UC_01 §8 |
| `timeout_buf[]` in `sweep_ack_timeouts` | Stack of `sweep_ack_timeouts` | `ACK_TRACKER_CAPACITY * sizeof(MessageEnvelope)` = 32 × ~4140 ≈ 132 KB | Allocated only if `sweep_ack_timeouts()` is called with active expired slots |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. The server loop uses only stack
and statically-allocated members.

**Object lifetimes:** `TcpBackend transport` and `DeliveryEngine engine` are declared as
locals in `main()`, lifetime = process lifetime. `run_server_iteration()` receives
references to both; no ownership transfer.

---

## 9. Error Handling Flow

| Condition | System state after | What `run_server_iteration()` does |
|---|---|---|
| `receive()` → `ERR_TIMEOUT` | Queue empty; no socket data arrived in 100 ms | Normal; continue loop iteration |
| `receive()` → `ERR_EXPIRED` | Expired message consumed from queue; WARNING_LO logged | Skip echo; continue |
| `receive()` → `ERR_DUPLICATE` | Duplicate dropped; no state change beyond dedup window | Skip echo; continue |
| `send_echo_reply()` → non-OK | Reply not sent; WARNING_LO logged | `messages_sent` not incremented; continue |
| `pump_retries()` sends some retries | RetryManager `retry_count` incremented for each due slot | Logged at INFO if count > 0 |
| `sweep_ack_timeouts()` finds expired slots | AckTracker slots freed; expired envelopes logged at WARNING_HI | Count logged at WARNING_HI |

---

## 10. External Interactions

| API | fd / clock type | Notes |
|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC, &ts)` | POSIX monotonic clock | `timestamp_now_us()` called once per iteration at the start of `run_server_iteration()` |
| `poll(pfds, nfds, 100)` | Listen fd + client fds | Called inside `poll_clients_once()` from `TcpBackend::receive_message()`'s poll loop. Blocks up to 100 ms. |
| `ISocketOps::recv_frame()` | TCP client fd | Called inside `recv_from_client()` to read a length-prefixed frame |
| `ISocketOps::send_frame()` | TCP client fd | Called inside `send_to_all_clients()` to write the echo reply frame |
| `signal(SIGINT, signal_handler)` | Process signal | Installs signal handler before the main loop; `g_stop_flag` is checked each iteration |
| `printf` (via `print_payload_as_string`) | stdout | Prints received payload as a string; non-critical path |

---

## 11. State Changes / Side Effects

**Receive phase:**
| Object | Member | Before | After |
|---|---|---|---|
| `RingBuffer m_recv_queue` | `m_head` | H | H+1 (envelope consumed) |
| `DuplicateFilter m_dedup` | `m_window[m_next]` | Stale entry | `{src, msg_id, valid=true}` (if RELIABLE_RETRY) |
| `DuplicateFilter m_dedup` | `m_next` | N | (N+1) % DEDUP_WINDOW_SIZE |
| `AckTracker m_slots[i]` | `state` | `PENDING` | `ACKED` (if inbound ACK processed) |

**Echo send phase:**
| Object | Member | Before | After |
|---|---|---|---|
| `MessageEnvelope reply` | `source_id` | `received.destination_id` | `m_local_id` (overwritten by `send()`) |
| `MessageEnvelope reply` | `message_id` | 0 | New non-zero monotonic value |
| `MessageIdGen m_id_gen` | `m_next` | N | N+1 |
| `TcpBackend m_wire_buf` | bytes | Stale | Echo reply frame |
| Kernel TCP send buffer | bytes | Previous | Appended with echo reply frame |
| `AckTracker m_slots[j]` | `state` | `FREE` | `PENDING` (if RELIABLE_ACK or RELIABLE_RETRY) |
| `RetryManager m_slots[k]` | `active` | `false` | `true` (if RELIABLE_RETRY) |

**Retry/sweep phase:**
- `pump_retries()`: RetryManager slot `retry_count` incremented, `backoff_ms` doubled, `next_retry_us` updated.
- `sweep_ack_timeouts()`: Expired PENDING AckTracker slots freed (state → FREE), `m_count` decremented.

---

## 12. Sequence Diagram

```
Server main loop   DeliveryEngine   TcpBackend         DuplicateFilter  AckTracker
      |                  |               |                    |               |
      |--timestamp_now_us()              |                    |               |
      |--envelope_init(received)         |                    |               |
      |--receive(received,100,now)------>|                    |               |
      |                  |--receive_message(received,100)---->|               |
      |                  |               |  [pop or poll+recv]|               |
      |                  |               |<--OK, envelope-----|               |
      |                  |--timestamp_expired()               |               |
      |                  |--envelope_is_control()?             |               |
      |                  |--check_and_record(src,id)--------->|               |
      |                  |<--OK (new message)-----------------|               |
      |<--OK (DATA env)--|               |                    |               |
      |                  |               |                    |               |
      |--print_payload() |               |                    |               |
      |--send_echo_reply(engine,received,now)                 |               |
      |    envelope_init(reply)          |                    |               |
      |    [build reply fields]          |                    |               |
      |    memcpy(reply.payload,...)     |                    |               |
      |    engine.send(reply,now)------->|                    |               |
      |                  |--MessageIdGen::next()              |               |
      |                  |--send_via_transport(reply)-------->|               |
      |                  |               |  [serialize+impairment+send_frame] |
      |                  |<--OK----------|               |    |               |
      |                  |--track(reply, deadline)---------->  |               |
      |                  |<--OK (PENDING slot)---------------|               |
      |<--OK-------------|               |                    |               |
      |                  |               |                    |               |
      |--pump_retries(now)               |                    |               |
      |--sweep_ack_timeouts(now)         |                    |               |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `main()` in `Server.cpp` calls `transport_config_default(cfg)`, overrides fields for server
  mode (bind port, `is_server = true`, `reliability = RELIABLE_RETRY`), then calls
  `transport.init(cfg)` and `engine.init(&transport, cfg.channels[0],
  LOCAL_SERVER_NODE_ID)`.
- `signal(SIGINT, signal_handler)` is installed before the loop.

**Steady-state:**
- The loop runs up to `MAX_LOOP_ITERS = 100000` iterations. Each iteration is one call to
  `run_server_iteration()`.
- The server blocks inside `poll_clients_once()` for up to 100 ms waiting for client data.
  If no data, `receive()` returns `ERR_TIMEOUT` and the iteration ends quickly.
- `g_stop_flag` is checked at the top of each iteration; SIGINT causes a clean exit.

---

## 14. Known Risks / Observations

- **`send()` overwrites `reply.source_id` with `m_local_id`.** `send_echo_reply()` sets
  `reply.source_id = received.destination_id` (the server's own ID as known by the client).
  Then `DeliveryEngine::send()` immediately overwrites it with `m_local_id`. If
  `received.destination_id == m_local_id`, these are the same value and there is no
  discrepancy. If they differ (e.g., multicast destination), the override may produce an
  unexpected `source_id` in the reply.

- **Echo preserves the sender's `reliability_class`.** If the client sent a RELIABLE_RETRY
  message, the server also sends the echo as RELIABLE_RETRY. This consumes an AckTracker
  and RetryManager slot on the server for each echo. The server and client can accumulate
  in-flight tracked messages simultaneously if the client sends faster than the server
  receives ACKs.

- **`now_us` is captured once at the start of `run_server_iteration()` and reused for
  receive, echo-send, pump, and sweep.** The actual wall-clock time may drift by up to a
  few milliseconds across these four calls within a single iteration.

- **`MAX_LOOP_ITERS = 100000`.** The server loop is bounded by Power of 10 Rule 2 compliance.
  A production server that must run indefinitely would need a different architecture.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The server configures channel 0 with `reliability = RELIABLE_RETRY` and
  `max_retries = 3`. Therefore the echo reply will be sent as RELIABLE_RETRY, causing both
  AckTracker and RetryManager slots to be consumed on the server side.

- [ASSUMPTION] The client and server are both using `LOCAL_SERVER_NODE_ID = 1` for the
  server and `LOCAL_CLIENT_NODE_ID = 2` for the client. These are file-local constants in
  their respective .cpp files and are not enforced by the system.

- [ASSUMPTION] `print_payload_as_string()` treats the payload as a null-terminated ASCII
  string. Binary payloads may produce garbage output but do not affect correctness.

- [ASSUMPTION] The echo loop terminates due to either `g_stop_flag` (SIGINT) or exhausting
  `MAX_LOOP_ITERS`. In the demo, `MAX_LOOP_ITERS = 100000` iterations × 100 ms poll
  timeout per iteration = up to ~10,000 seconds (2.7 hours) of wall time if no messages are
  received.
