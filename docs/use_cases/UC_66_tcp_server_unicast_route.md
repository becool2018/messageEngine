# UC_66 — TCP server routes unicast frame to matched client slot

**HL Group:** HL-31 — TCP/TLS Server Unicast Message Routing / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.9

---

## 1. Use Case Overview

- **Trigger:** `TcpBackend::send_message(env)` is called on a server endpoint where `env.destination_id > 0`. File: `src/platform/TcpBackend.cpp`.
- **Goal:** Deliver the serialized frame to exactly one connected client whose `m_client_node_ids[slot]` equals `env.destination_id`, rather than broadcasting to all clients.
- **Success outcome:** Frame written to the single matching slot only. Other clients do not receive the frame. Returns `Result::OK`.
- **Error outcomes:**
  - `destination_id > 0` but no slot is registered with that NodeId → `ERR_INVALID` returned; WARNING_HI logged; no bytes sent (see UC_68).
  - `send_frame()` fails for the matched slot → WARNING_HI logged; client removed; returns `ERR_IO`.

**Broadcast fallback:** When `env.destination_id == 0` (NODE_ID_INVALID), the broadcast path is taken unchanged: `send_to_all_clients()` fans out to all connected slots.

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (implements TransportInterface)
Result TcpBackend::send_message(const MessageEnvelope& env);
```

Called by `DeliveryEngine::send()`. Not called directly by the User (User calls `DeliveryEngine::send()`).

---

## 3. End-to-End Control Flow

1. **`TcpBackend::send_message(env)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(m_open)`.
3. `NEVER_COMPILED_OUT_ASSERT(m_is_server)` (unicast routing is only meaningful on the server; clients always have a single peer).
4. **`Serializer::serialize(env, m_wire_buf, sizeof(m_wire_buf), &wire_len)`** — encode envelope.
5. If serialize fails: log WARNING_HI; return `ERR_IO`.
6. **`ImpairmentEngine::process_outbound(env, m_wire_buf, wire_len)`** — apply loss/delay/duplicate/reorder impairments. May push to `m_impair_delay_buf` for later delivery.
7. **`ImpairmentEngine::collect_deliverable(callback)`** — retrieve all frames whose delay has elapsed.
8. For each deliverable frame — **`flush_delayed_to_clients(frame_buf, frame_len, frame_destination_id)`**:
   a. `NEVER_COMPILED_OUT_ASSERT(frame_len > 0)`.
   b. **Routing decision on `frame_destination_id`:**
      - **`frame_destination_id == 0`** (broadcast sentinel): call `send_to_all_clients(frame_buf, frame_len)` — fans out to all slots (existing broadcast path, unchanged).
      - **`frame_destination_id > 0`** (unicast): call **`find_client_slot(frame_destination_id)`**:
        - Loop `idx` in `0..m_client_count-1` (bounded by `MAX_TCP_CONNECTIONS`).
        - If `m_client_node_ids[idx] == frame_destination_id`: return `idx`.
        - If not found: return `MAX_TCP_CONNECTIONS` (sentinel for "not found").
      - If `find_client_slot()` returns `MAX_TCP_CONNECTIONS`: log `WARNING_HI "unicast to unregistered NodeId N; drop"`; set `final_result = ERR_INVALID`; continue to next deliverable frame.
      - If slot found: call **`send_to_slot(slot, frame_buf, frame_len)`**:
        - `m_sock_ops->send_frame(m_client_fds[slot], frame_buf, frame_len, m_cfg.send_timeout_ms)`.
        - If `send_frame()` returns false: log `WARNING_HI`; `remove_client(slot)`; set `final_result = ERR_IO`.
9. Return `final_result` (OK unless an error occurred in step 8).

---

## 4. Call Tree

```
TcpBackend::send_message(env)                        [TcpBackend.cpp]
 ├── Serializer::serialize(env, m_wire_buf, ...)
 ├── ImpairmentEngine::process_outbound(...)
 ├── ImpairmentEngine::collect_deliverable(cb)
 └── [for each deliverable frame:]
      TcpBackend::flush_delayed_to_clients(buf, len, dst_id)
       ├── [dst_id == 0:] send_to_all_clients(buf, len)      [broadcast]
       │    └── [for each slot:] ISocketOps::send_frame(fd, buf, len, ...)
       └── [dst_id > 0:]
            TcpBackend::find_client_slot(dst_id)             [O(MAX_TCP_CONNECTIONS)]
             └── [scan m_client_node_ids[]]
            TcpBackend::send_to_slot(slot, buf, len)         [unicast]
             └── ISocketOps::send_frame(m_client_fds[slot], buf, len, ...)
                  └── tcp_send_frame()                       [SocketUtils.cpp]
```

---

## 5. Key Components Involved

- **`flush_delayed_to_clients()`** — Per-frame routing dispatcher: inspects `destination_id` and calls broadcast or unicast path accordingly.
- **`find_client_slot(destination_id)`** — Linear scan of `m_client_node_ids[]`; returns slot index or `MAX_TCP_CONNECTIONS` sentinel.
- **`send_to_slot(slot, buf, len)`** — Single-client send; calls `ISocketOps::send_frame()` for exactly one fd.
- **`send_to_all_clients()`** — Existing broadcast fan-out; unchanged by this fix.
- **`ImpairmentEngine`** — Sits between `send_message()` and the actual wire write; delayed/reordered frames are routed at delivery time, not at queue time, so late arrivals of HELLO frames before the delayed message is released are handled correctly.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `serialize()` fails | Log WARNING_HI; return ERR_IO | Continue to impairment |
| `frame_destination_id == 0` | `send_to_all_clients()` (broadcast) | `find_client_slot()` (unicast) |
| `find_client_slot()` returns MAX_TCP_CONNECTIONS | Log WARNING_HI; ERR_INVALID | `send_to_slot()` |
| `send_frame()` returns false | Log WARNING_HI; `remove_client()`; ERR_IO | Continue; OK |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application thread.
- `m_client_node_ids[]` is written on the receive path (HELLO interception) and read here on the send path. Both run on the same thread; no synchronization required.

---

## 8. Memory & Ownership Semantics

- `m_wire_buf[SOCKET_RECV_BUF_BYTES]` — static member; holds serialized frame. Not heap allocated.
- Impairment delay buffer (`m_impair_delay_buf`) — fixed-size static array; frames may be held here between `process_outbound()` and `collect_deliverable()`.
- No heap allocation on critical path.

---

## 9. Error Handling Flow

- **`serialize()` failure:** Return `ERR_IO` immediately. No bytes sent.
- **`find_client_slot()` miss:** `ERR_INVALID`; frame dropped; other deliverable frames are still processed.
- **`send_frame()` failure:** Client removed; `ERR_IO` accumulated in `final_result`; other deliverable frames still processed.

---

## 10. External Interactions

- **POSIX `::send()`:** Called via `tcp_send_frame()` on a single client fd (`m_client_fds[slot]`). Direction: outbound to one client.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| TCP socket (slot) | kernel send buffer | previous | + unicast frame |
| `TcpBackend` | `m_client_fds[slot]` (on send fail) | valid fd | closed; slot compacted |
| `TcpBackend` | `m_client_count` (on send fail) | N | N-1 |

---

## 12. Sequence Diagram

```
DeliveryEngine::send(env)         [env.destination_id = N > 0]
  -> TcpBackend::send_message(env)
       -> Serializer::serialize(env, m_wire_buf, ...)
       -> ImpairmentEngine::process_outbound(...)
       -> ImpairmentEngine::collect_deliverable(cb)
            -> flush_delayed_to_clients(buf, len, dst_id=N)
                 -> find_client_slot(N)
                      [scan m_client_node_ids[]; slot S found]
                 <- S
                 -> send_to_slot(S, buf, len)
                      -> ISocketOps::send_frame(m_client_fds[S], buf, len, timeout)
                           -> tcp_send_frame()
                                -> socket_send_all(fd, hdr, 4)
                                -> socket_send_all(fd, buf, len)
                           <- true
                 [only client at slot S receives frame]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `TcpBackend::init()` called with `m_is_server == true`.
- At least one client connected and `handle_hello_frame()` has been called to register its NodeId.

**Runtime:**
- `flush_delayed_to_clients()` is called for every deliverable frame. The routing decision (broadcast vs unicast) is made fresh per frame based on the current `destination_id`.

---

## 14. Known Risks / Observations

- **Late HELLO registration:** If a unicast send occurs before the client's HELLO is processed, `find_client_slot()` will miss and return `ERR_INVALID`. Application should allow time for HELLO exchange after client connection before sending unicast messages.
- **Routing at delivery time:** Because the impairment engine may delay frames, a frame queued before a client HELLO is registered may be delivered after registration is complete. This is the correct behavior: the routing decision is made at `collect_deliverable()` time, not at `process_outbound()` time.
- **`send_to_slot()` vs `send_to_all_clients()`:** These are separate code paths. `send_to_slot()` sends to exactly one fd; it does not call `remove_client()` with `idx--` loop logic. If the send fails, `remove_client(slot)` is called directly.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `flush_delayed_to_clients()` receives the `destination_id` of the original envelope alongside the serialized buffer. This requires either storing `destination_id` in the delay buffer slot or re-deserializing the frame, or passing it as a separate argument from `collect_deliverable()`.
- `[ASSUMPTION]` `find_client_slot()` is a private helper on `TcpBackend`; it returns `MAX_TCP_CONNECTIONS` (an out-of-range sentinel) when no match is found.
