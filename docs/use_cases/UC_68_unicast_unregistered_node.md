# UC_68 — Unicast send to unregistered NodeId returns ERR_INVALID

**HL Group:** HL-31 — TCP/TLS Server Unicast Message Routing / System Internals
**Actor:** System
**Requirement traceability:** REQ-6.1.9

---

## 1. Use Case Overview

- **Trigger:** `TcpBackend::send_message(env)` (or `TlsTcpBackend::send_message(env)`) is called on a server endpoint where `env.destination_id > 0` but no entry in `m_client_node_ids[]` matches that NodeId. File: `src/platform/TcpBackend.cpp` / `TlsTcpBackend.cpp`.
- **Goal:** Detect the unregistered destination before any bytes are sent and return `ERR_INVALID` to the caller so the application can take corrective action.
- **Success outcome (from system perspective):** No bytes are sent to any client. WARNING_HI is logged with the unresolvable NodeId. `ERR_INVALID` is returned to `DeliveryEngine::send()`.
- **Error outcomes:** None beyond the `ERR_INVALID` return itself.

**Caller handling:** `DeliveryEngine::send()` receives `ERR_INVALID` from the transport; it logs the failure and records a `MISROUTE_DROP` event in the `DeliveryEventRing`. The application can observe this via `poll_event()` (UC_61).

---

## 2. Entry Points

```cpp
// src/platform/TcpBackend.cpp (private)
Result TcpBackend::flush_delayed_to_clients(const uint8_t* buf,
                                            uint32_t len,
                                            NodeId destination_id);
```

(Mirror path in `TlsTcpBackend::flush_delayed_to_clients()`.)

Called internally by `send_message()`. Not called directly by the User.

---

## 3. End-to-End Control Flow

1. `TcpBackend::send_message(env)` serializes `env`, applies impairment, collects deliverable frames.
2. For a deliverable frame with `destination_id N > 0`: calls **`flush_delayed_to_clients(buf, len, N)`**.
3. `NEVER_COMPILED_OUT_ASSERT(len > 0)`.
4. `NEVER_COMPILED_OUT_ASSERT(N != NODE_ID_INVALID)` — destination_id > 0 is a unicast path.
5. **`find_client_slot(N)`**:
   - Loop `idx` in `0..m_client_count-1` (bounded by `MAX_TCP_CONNECTIONS`).
   - Compare `m_client_node_ids[idx]` with `N`.
   - None match: return `MAX_TCP_CONNECTIONS`.
6. `find_client_slot()` returns `MAX_TCP_CONNECTIONS` (not found).
7. **`LOG_WARN_HI("TcpBackend", "unicast to unregistered NodeId %u; drop", N)`**.
8. Set `final_result = ERR_INVALID`.
9. Continue processing remaining deliverable frames (if any).
10. `flush_delayed_to_clients()` / `send_message()` returns `ERR_INVALID` to `DeliveryEngine::send()`.
11. `DeliveryEngine::send()` records `MISROUTE_DROP` event in `DeliveryEventRing`; returns `ERR_INVALID` to the application.

---

## 4. Call Tree

```
TcpBackend::send_message(env)                        [TcpBackend.cpp]
 ├── Serializer::serialize(...)
 ├── ImpairmentEngine::process_outbound(...)
 ├── ImpairmentEngine::collect_deliverable(cb)
 └── flush_delayed_to_clients(buf, len, dst_id=N)
      └── find_client_slot(N)                        [scan m_client_node_ids[]]
           <- MAX_TCP_CONNECTIONS  [not found]
      -> LOG_WARN_HI("unicast to unregistered NodeId N; drop")
      -> final_result = ERR_INVALID
  <- ERR_INVALID

DeliveryEngine::send()
 <- ERR_INVALID
 -> DeliveryEventRing::push(MISROUTE_DROP, env.message_id)
```

---

## 5. Key Components Involved

- **`find_client_slot(destination_id)`** — Linear scan of `m_client_node_ids[]`; returns `MAX_TCP_CONNECTIONS` when no match found.
- **`LOG_WARN_HI(...)`** — WARNING_HI used because an unregistered unicast destination is a system-wide routing anomaly, not a recoverable per-connection error.
- **`DeliveryEventRing`** — Records `MISROUTE_DROP` for observability; application polls via `poll_event()` (UC_61).

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `find_client_slot()` returns MAX_TCP_CONNECTIONS | Log WARNING_HI; ERR_INVALID | `send_to_slot()` / `tls_send_frame()` |
| Additional deliverable frames remain | Process them (may succeed or fail independently) | Return `final_result` |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the application thread.
- `m_client_node_ids[]` is read in `find_client_slot()` and written in `handle_hello_frame()` on the receive path. Both are on the same thread; no races.

---

## 8. Memory & Ownership Semantics

- No allocation. `find_client_slot()` scans a static fixed-size array.
- No bytes written to any socket fd.

---

## 9. Error Handling Flow

- **`ERR_INVALID` returned:** The frame is silently dropped. No retransmission is attempted.
- **Caller (`DeliveryEngine::send()`) behaviour:** Records `MISROUTE_DROP` event. If the envelope had `RELIABLE_RETRY` semantics, the RetryManager will attempt to retransmit — each retry will also result in `ERR_INVALID` until the target client connects and sends a HELLO.
- **Application response:** Application should call `poll_event()` / `drain_events()` to observe `MISROUTE_DROP` events and decide whether to wait for the client to reconnect or abort the send.

---

## 10. External Interactions

- **None.** No socket calls are made. No bytes leave the process.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DeliveryEventRing` | event ring | N events | N+1 events (MISROUTE_DROP) |

No change to `m_client_fds[]`, `m_client_node_ids[]`, or any socket state.

---

## 12. Sequence Diagram

```
DeliveryEngine::send(env)         [env.destination_id = N; no client registered for N]
  -> TcpBackend::send_message(env)
       -> Serializer::serialize(env, m_wire_buf, ...)
       -> ImpairmentEngine::process_outbound(...)
       -> ImpairmentEngine::collect_deliverable(cb)
            -> flush_delayed_to_clients(buf, len, dst_id=N)
                 -> find_client_slot(N)
                      [scan m_client_node_ids[0..m_client_count-1]; no match]
                 <- MAX_TCP_CONNECTIONS
                 -> LOG_WARN_HI("unicast to unregistered NodeId N; drop")
                 -> final_result = ERR_INVALID
       <- ERR_INVALID
  <- ERR_INVALID
  -> DeliveryEventRing::push(MISROUTE_DROP, env.message_id)
  <- ERR_INVALID  [propagated to application]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions that lead to this path:**
- Server is running (`m_is_server == true`; `m_open == true`).
- A unicast send (`destination_id > 0`) is attempted before the target client has connected and sent a HELLO, or after the target client has disconnected and its slot has been compacted.

**Runtime:**
- This path can occur at any time during steady-state operation if the application sends to a NodeId whose client is not currently connected.

---

## 14. Known Risks / Observations

- **Silent drop on `RELIABLE_RETRY`:** If the reliability class is `RELIABLE_RETRY`, the RetryManager will keep re-queuing the message. Each retry hits this path again, generating a `MISROUTE_DROP` event and a WARNING_HI log per retry. The retry will not succeed until the client connects and registers. Applications should implement a maximum wait before cancelling the retry.
- **Race between HELLO and first unicast:** The application layer must ensure the server has received and processed the client's HELLO before attempting unicast to that client's NodeId. There is no built-in synchronisation between the HELLO receive path and the application's send path.
- **WARNING_HI severity:** Each `MISROUTE_DROP` is logged at WARNING_HI. If `RELIABLE_RETRY` sends are common before client registration, log volume may be high. Applications should delay sending unicast until HELLO receipt is confirmed via an application-level acknowledgement.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `find_client_slot()` returns `MAX_TCP_CONNECTIONS` as the "not found" sentinel. This value is out-of-range for all valid slot indices (0..MAX_TCP_CONNECTIONS-1), making it an unambiguous sentinel.
- `[ASSUMPTION]` `DeliveryEngine::send()` maps transport `ERR_INVALID` to a `MISROUTE_DROP` observability event. The MISROUTE_DROP event type is defined in REQ-7.2.5.
