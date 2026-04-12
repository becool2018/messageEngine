# UC_72 — Peer Reconnect Ordering State Reset: drain_hello_reconnects() clears stale per-peer sequence state

**HL Group:** HL-34: Peer Reconnect and Ordering State Reset
**Actor:** System (internal — called automatically at the start of `DeliveryEngine::receive()`)
**Requirement traceability:** REQ-3.3.6

---

## 1. Use Case Overview

- **Trigger:** A previously-connected peer disconnects and reconnects. The transport backend
  detects the new HELLO frame on an already-registered channel and enqueues the peer's `NodeId`
  into the transport's HELLO reconnect queue. On the next call to `DeliveryEngine::receive()`,
  `drain_hello_reconnects()` drains this queue and resets the stale ordering state for each
  reconnecting peer.
- **Goal:** Prevent the `OrderingBuffer` from stalling delivery for a reconnecting peer because
  `next_expected_seq` still holds a value from the previous connection session. After reset,
  the reconnecting peer's messages (starting at `seq=1`) are delivered correctly.
- **Success outcome:** Per-peer `next_expected_seq` reset to 1; all held messages for the peer
  freed; staged `m_held_pending` discarded if it belongs to the reconnecting peer.
- **No-op outcome:** Peer has no active ordering state (first connection, or UNORDERED-only
  traffic) — `reset_peer()` is idempotent and returns immediately.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp — called at the top of receive()
void DeliveryEngine::drain_hello_reconnects();

// Private helpers called from drain_hello_reconnects():
void DeliveryEngine::reset_peer_ordering(NodeId src);   // SC: HAZ-001, HAZ-016
void OrderingBuffer::reset_peer(NodeId src);            // SC: HAZ-001, HAZ-016

// Transport interface (backend-dependent):
NodeId TransportInterface::pop_hello_peer();
```

---

## 3. End-to-End Control Flow

1. TCP/TLS backend receives a HELLO frame (`MessageType::HELLO`) on an already-registered
   channel (i.e., the `NodeId` is already in the server's routing table). The backend pushes
   the `NodeId` onto its internal HELLO reconnect queue (bounded by `ORDERING_PEER_COUNT`).
2. `DeliveryEngine::receive()` is called by the application.
3. At entry, `receive()` calls `drain_hello_reconnects()`.
4. **`drain_hello_reconnects()`** (bounded loop ≤ `ORDERING_PEER_COUNT` iterations):
   a. Calls `m_transport->pop_hello_peer()`.
   b. If returns `NODE_ID_INVALID` (queue empty) → break.
   c. Calls `reset_peer_ordering(hello_src)`.
5. **`reset_peer_ordering(hello_src)`:**
   a. If `m_held_pending_valid` and `m_held_pending.source_id == hello_src`:
      - Discards staged held message; sets `m_held_pending_valid = false`.
      - Logs INFO.
   b. Calls `m_ordering.reset_peer(hello_src)`.
6. **`OrderingBuffer::reset_peer(hello_src)`:**
   a. `find_peer(hello_src)` — if not found → no-op (return).
   b. Set `m_peers[peer_idx].next_expected_seq = 1U` (reset to initial value).
   c. Free all hold slots where `env.source_id == hello_src` (`m_hold[i].active = false`).
   d. Log INFO: peer reset.
7. `drain_hello_reconnects()` continues polling until `pop_hello_peer()` returns `NODE_ID_INVALID`.
8. `receive()` continues normally: polls transport, runs reassembly, dedup, ordering gate.
9. Reconnecting peer's messages (seq=1, 2, …) now flow through a clean ordering gate.

---

## 4. Call Tree

```
DeliveryEngine::receive()
 └── drain_hello_reconnects()                     [NSC; bounded ≤ ORDERING_PEER_COUNT]
      └── m_transport->pop_hello_peer()           [per iteration; returns NodeId or INVALID]
      └── reset_peer_ordering(hello_src)          [SC: HAZ-001, HAZ-016]
           ├── [discard m_held_pending if src matches]
           └── OrderingBuffer::reset_peer(src)    [SC: HAZ-001, HAZ-016]
                ├── find_peer(src)
                ├── m_peers[idx].next_expected_seq = 1U
                └── [free hold slots for src]
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| Transport backend (HELLO queue) | Detects HELLO on known channel; enqueues reconnecting `NodeId` |
| `pop_hello_peer()` | Drains one `NodeId` from the HELLO reconnect queue per call |
| `drain_hello_reconnects()` | Polls the queue and dispatches per-peer resets |
| `reset_peer_ordering()` | Clears `m_held_pending` if owned by reconnecting peer |
| `OrderingBuffer::reset_peer()` | Resets `next_expected_seq` and frees hold slots |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `pop_hello_peer()` returns `NODE_ID_INVALID` | Queue empty; break loop |
| Peer not in OrderingBuffer peer table | No-op (idempotent) |
| `m_held_pending_valid && source == hello_src` | Staged pending message discarded |
| Hold slots found for reconnecting src | All freed (`active = false`) |

---

## 7. Concurrency / Threading Behavior

`drain_hello_reconnects()` runs on the application's receive thread. The transport
backend may push to the HELLO queue from a background recv thread; `pop_hello_peer()`
must be thread-safe. TCP/TLS backends protect the HELLO queue with `m_hello_mutex` or
use a bounded `NodeId` ring.

`OrderingBuffer::reset_peer()` is not called concurrently — it is private to
`DeliveryEngine` and only invoked from `drain_hello_reconnects()`.

---

## 8. Memory & Ownership Semantics

- No heap allocation. The HELLO queue in the transport backend is a fixed `NodeId` array.
- `m_held_pending` is a `MessageEnvelope` member of `DeliveryEngine`; discard is a flag
  clear (`m_held_pending_valid = false`) — no free needed.

---

## 9. Error Handling Flow

`reset_peer_ordering()` and `OrderingBuffer::reset_peer()` are `void` — errors are not
propagated. If `find_peer()` fails (peer not tracked), the function is a no-op. This is
safe: an untracked peer has no stale sequence state to clear.

---

## 10. External Interactions

- Transport backend HELLO queue — populated by `TcpBackend::handle_hello_frame()` or
  `TlsTcpBackend::handle_hello_frame()` when a HELLO is received on an already-registered
  channel (indicating reconnect rather than first connect).

---

## 11. State Changes / Side Effects

- `PeerState.next_expected_seq` reset to 1 for reconnecting peer.
- All `HoldSlot.active` flags cleared for reconnecting peer.
- `DeliveryEngine::m_held_pending_valid` cleared if staged message belongs to reconnecting peer.

---

## 12. Sequence Diagram

```
TCP backend (recv thread)
  <- receives HELLO from peer P (already registered)
  -> pushes NodeId P onto HELLO queue

Application thread
  -> DeliveryEngine::receive()
       -> drain_hello_reconnects()
            -> pop_hello_peer()  -> NodeId P
            -> reset_peer_ordering(P)
                 -> discard m_held_pending (if P's)
                 -> OrderingBuffer::reset_peer(P)
                      -> next_expected_seq[P] = 1
                      -> free hold slots for P
            -> pop_hello_peer()  -> NODE_ID_INVALID  [queue empty]
       -> continue: poll transport, reassembly, dedup, ordering...
  <- delivers first message from new session (seq=1)
```

---

## 13. Initialization vs Runtime Flow

- **Initialization:** Not applicable — `drain_hello_reconnects()` is a runtime-only function.
  At startup no reconnects have occurred.
- **Runtime:** Called on every `receive()` invocation, even when no reconnects have occurred.
  The `pop_hello_peer()` returns `NODE_ID_INVALID` immediately when the queue is empty, making
  the common-case cost a single virtual call that returns immediately.

---

## 14. Known Risks / Observations

- **Ordering reset is irreversible:** If a HELLO is spuriously generated (e.g., duplicate
  HELLO on first connect), the reset resets `next_expected_seq` for a peer that was
  mid-stream. The transport backend guards against this by enqueuing reconnect HELLO only
  when the `NodeId` is already in its routing table (first connect is silently registered,
  not enqueued).
- **Race between reset and delivery:** If the transport's recv thread pushes a data message
  from the reconnecting peer to the inbound ring before the HELLO queue is drained, that
  data message may be processed with stale ordering state and discarded as a duplicate.
  This is acceptable: the sender will retransmit on the new session if RELIABLE_RETRY is used.

---

## 15. Unknowns / Assumptions

- `ORDERING_PEER_COUNT` is the bound on both the `drain_hello_reconnects()` loop and the
  HELLO queue capacity. Defined in `src/core/Types.hpp`.
- UDP and DTLS backends use `TransportInterface`'s default no-op `pop_hello_peer()` (returns
  `NODE_ID_INVALID`). HELLO enforcement on those transports is via `process_hello_or_validate()`.
