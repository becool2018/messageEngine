# UC_01 — Best-effort send over TCP (no ACK, no retry, no dedup)

**HL Group:** HL-1 — Fire-and-Forget Message Send
**Actor:** User
**Requirement traceability:** REQ-3.3.1, REQ-4.1.2, REQ-6.1.5, REQ-6.1.6, REQ-7.1.4

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send(envelope, now_us)` with `envelope.reliability_class == ReliabilityClass::BEST_EFFORT`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Transmit the envelope to the connected peer(s) once, with no acknowledgement tracking, no retry scheduling, and no deduplication records written.
- **Success outcome:** `Result::OK` is returned. The serialized envelope has been handed to the transport layer and forwarded to all connected TCP clients.
- **Error outcomes:**
  - `Result::ERR_EXPIRED` — `envelope.expiry_time_us != 0 && envelope.expiry_time_us <= now_us` at the top of `send_via_transport()`.
  - `Result::ERR_FULL` — `m_transport->send_message()` returns `ERR_FULL` (transport ring buffer full).
  - Any non-OK result propagated from `TcpBackend::send_message()` passes through unchanged.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::send(MessageEnvelope& env, uint64_t now_us);
```

Called directly by application code. No thread is created; the call is synchronous in the caller's thread.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::send(envelope, now_us)`** (`DeliveryEngine.cpp`) is called by the User.
2. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` — verifies transport is wired up.
3. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(envelope))` — verifies envelope is not INVALID type.
4. `m_id_gen.next()` is called (`MessageId.cpp`) to assign a unique monotonic `message_id`; the result is written into a local copy of the envelope (stack-allocated `MessageEnvelope work`).
5. **Branch:** `envelope.reliability_class == BEST_EFFORT` — true, so neither `m_ack_tracker.track()` nor `m_retry_mgr.schedule()` is called. Both ACK and retry branches are entirely skipped.
6. **`send_fragments(work, now_us)`** is called (`DeliveryEngine.cpp`), which calls `send_via_transport()` per frame. For BEST_EFFORT with a payload ≤ `FRAG_MAX_PAYLOAD_BYTES`, this is always a single frame.
7. Inside `send_via_transport`: `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)`.
8. **Branch:** `work.expiry_time_us != 0 && timestamp_expired(work.expiry_time_us, now_us)` — if true, log `WARNING_LO` and return `Result::ERR_EXPIRED`. For a freshly built envelope this is false; continue.
9. **`m_transport->send_message(work)`** is called — virtual dispatch to `TcpBackend::send_message()` (`TcpBackend.cpp`).
10. Inside `TcpBackend::send_message()`:
    a. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
    b. `m_impairment.process_outbound(env, now_us, delay_buf, &delay_count)` is called (`ImpairmentEngine.cpp`). With all impairments disabled (default config), no loss/delay/duplication is applied; the envelope is passed through immediately.
    c. `m_impairment.collect_deliverable(delay_buf, delay_count, now_us, out_buf, &out_count)` retrieves any immediately-due messages (the one just queued).
    d. For each deliverable envelope (the one envelope): `Serializer::serialize(env, m_wire_buf, SOCKET_RECV_BUF_BYTES, &wire_len)` serializes it to a 52-byte header + payload wire frame in `m_wire_buf`.
    e. `send_to_all_clients(m_wire_buf, wire_len)` iterates over `m_client_fds[0..m_client_count-1]` and calls `m_sock_ops->send_frame(fd, buf, len, timeout_ms)` for each, which calls `tcp_send_frame()` → `socket_send_all()` → `::send()`.
11. `TcpBackend::send_message()` returns `Result::OK`.
12. `send_via_transport()` checks `send_res != Result::OK` — false; returns `Result::OK`.
13. `DeliveryEngine::send()` receives `Result::OK` from `send_via_transport()`.
14. Returns `Result::OK` to the User.

---

## 4. Call Tree

```
DeliveryEngine::send()                       [DeliveryEngine.cpp]
 ├── MessageIdGen::next()                    [MessageId.cpp]
 └── DeliveryEngine::send_via_transport()    [DeliveryEngine.cpp]
      ├── timestamp_expired()                [Timestamp.hpp]
      └── TcpBackend::send_message()         [TcpBackend.cpp]
           ├── ImpairmentEngine::process_outbound()    [ImpairmentEngine.cpp]
           ├── ImpairmentEngine::collect_deliverable() [ImpairmentEngine.cpp]
           ├── Serializer::serialize()                 [Serializer.cpp]
           └── TcpBackend::send_to_all_clients()       [TcpBackend.cpp]
                └── ISocketOps::send_frame()           [SocketOpsImpl / ISocketOps.hpp]
                     └── tcp_send_frame()              [SocketUtils.cpp]
                          └── socket_send_all()        [SocketUtils.cpp]
                               └── ::send()            [POSIX]
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Orchestrates the send path. Assigns message ID and dispatches to `send_via_transport()`. For BEST_EFFORT, intentionally skips ACK and retry registration.
- **`MessageIdGen`** — Provides monotonically increasing unique message IDs. Required so each envelope is distinct on the wire even if the User reuses an envelope struct.
- **`TcpBackend`** — Concrete transport implementation. Serializes, applies impairments, and writes to all connected client sockets.
- **`ImpairmentEngine`** — Sits between the transport logic and socket write. Applies configured impairments. With default config (all disabled), it is a pass-through.
- **`Serializer`** — Converts `MessageEnvelope` to big-endian wire bytes. Required for interoperability; the envelope struct layout is not the wire format.
- **`ISocketOps / SocketOpsImpl`** — Injectable POSIX adapter. Allows fault injection in tests; in production calls `::send()` directly.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `envelope.reliability_class == BEST_EFFORT` | Skip ACK and retry tracking entirely | Register ACK slot or retry slot (not this UC) |
| `work.expiry_time_us != 0 && timestamp_expired(...)` | Return `ERR_EXPIRED`; never reaches transport | Proceed to `m_transport->send_message()` |
| `impairment enabled && loss check passes` | Drop envelope (impairments disabled in default) | Forward envelope |
| `send_res != Result::OK` from transport | Log warning; propagate error up to caller | Return `Result::OK` |
| `m_client_count == 0` (no TCP clients) | `send_to_all_clients` is a no-op; `Result::OK` returned | Normal send loop |

---

## 7. Concurrency / Threading Behavior

- The entire call executes synchronously in the caller's thread.
- No threads are created by this flow.
- `MessageIdGen::next()` modifies `m_counter` (a plain `uint32_t`). Calling `send()` from multiple threads simultaneously is unsafe; the component is designed for single-threaded use by the application.
- No `std::atomic` operations occur on the critical send path.
- `RingBuffer` SPSC contracts are not involved on the outbound send path (the ring buffer is used for inbound receive queuing).

---

## 8. Memory & Ownership Semantics

- **Stack buffers used:**
  - `MessageEnvelope work` — copy of the input envelope with updated `message_id`; 4144 bytes on the stack in `DeliveryEngine::send()`.
  - `m_wire_buf[SOCKET_RECV_BUF_BYTES]` — 8192-byte member of `TcpBackend`; used to hold the serialized frame.
  - `delay_buf[IMPAIR_DELAY_BUF_SIZE]` — 32-slot array of `MessageEnvelope` inside `ImpairmentEngine::process_outbound()`.
- **No heap allocation** on this path (Power of 10 Rule 3 satisfied).
- `DeliveryEngine` owns `m_id_gen`, `m_ack_tracker`, `m_retry_mgr` as value members; no ownership transfer occurs.
- `m_transport` is a non-owning pointer set by `DeliveryEngine::init()`; the transport object is owned by the caller.

---

## 9. Error Handling Flow

- **`ERR_EXPIRED`:** Produced in `send_via_transport()` before any socket interaction. State remains unchanged; no ACK or retry slot was allocated. Caller should log and discard the envelope.
- **`ERR_FULL`:** Transport's ring is full (e.g., impairment delay buffer). No retry slot was allocated (BEST_EFFORT). Caller may retry the send or discard.
- **Transport failure (other):** Any non-OK result from `TcpBackend::send_message()` is propagated unchanged. No cleanup is needed because no ACK or retry slot was opened.

---

## 10. External Interactions

- **POSIX `::send()`:** Called via `socket_send_all()` on the TCP socket file descriptor for each connected client. Direction: outbound. FD is `m_client_fds[idx]` inside `TcpBackend`.
- No file I/O, no signal handling, no hardware interaction on the BEST_EFFORT send path.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `MessageIdGen` | `m_counter` | N | N+1 (or 1 if wrap from UINT32_MAX) |
| `TcpBackend` | `m_wire_buf` | stale data | overwritten with serialized frame |
| TCP socket | kernel send buffer | previous bytes | + serialized frame appended |

No `AckTracker` state, `RetryManager` state, or `DuplicateFilter` state is modified.

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::send(envelope, now_us)
       -> MessageIdGen::next()                 [assigns message_id]
       -> DeliveryEngine::send_via_transport()
            -> timestamp_expired()             [check expiry; false]
            -> TcpBackend::send_message()
                 -> ImpairmentEngine::process_outbound()    [no-op: disabled]
                 -> ImpairmentEngine::collect_deliverable() [returns 1 envelope]
                 -> Serializer::serialize()                 [-> wire bytes]
                 -> send_to_all_clients()
                      -> ISocketOps::send_frame()
                           -> ::send()         [POSIX; TCP socket]
            <- Result::OK
       <- Result::OK
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions (must be true before this flow runs):**
- `DeliveryEngine::init(&transport, channel_cfg, local_node_id)` has been called, setting `m_transport` and resetting `m_id_gen`, `m_ack_tracker`, `m_retry_mgr`, `m_dedup_filter`.
- `TcpBackend::init(config)` has been called; `m_open == true`; at least one client is connected (`m_client_count >= 1`) for the send to reach the wire (otherwise `send_to_all_clients` is a no-op).

**Runtime behavior:**
- Each call to `send()` independently serializes one envelope and writes it to all currently-connected clients in the caller's thread.

---

## 14. Known Risks / Observations

- **No delivery confirmation:** BEST_EFFORT provides no guarantee. If the TCP socket buffers are full or the connection drops after `::send()` returns, the message is silently lost.
- **Silent success with zero clients:** If no clients are connected, `send_to_all_clients()` does nothing but `Result::OK` is still returned. The caller cannot distinguish "sent to zero clients" from "sent successfully."
- **`MessageIdGen` not thread-safe:** Calling `send()` from multiple threads concurrently creates a data race on `m_counter`.
- **Stack depth:** Each `send()` allocates 4144 bytes (`MessageEnvelope work`) on the stack; callers on constrained stacks should be aware of this.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` The impairment engine is in its default (all-disabled) state for this UC description. If impairments are configured, loss or delay can suppress the message even though `send()` returns `Result::OK`.
- `[ASSUMPTION]` `send_to_all_clients()` broadcasts to every currently-connected client without per-client filtering based on `destination_id`. This is inferred from the implementation loop in `TcpBackend.cpp`.
