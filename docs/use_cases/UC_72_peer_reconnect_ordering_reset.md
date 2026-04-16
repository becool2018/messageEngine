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
- **Goal:** Prevent ordered delivery from stalling for a reconnecting peer due to stale
  per-peer state on either end. Two mechanisms are reset:
  - **Inbound:** `OrderingBuffer` may retain `next_expected_seq=N` from the prior session.
    The new connection sends from `seq=1`, which is discarded as a duplicate. Reset clears
    this so the new session's messages flow through correctly.
  - **Outbound:** `DeliveryEngine::m_seq_state` may retain `next_seq=N` for the peer as
    an outbound destination. The reconnecting peer's fresh receive-side `OrderingBuffer`
    starts at `expected=1`, so messages arriving with `seq=N+1` are held out-of-order
    indefinitely. Reset restarts the outbound sequence at 1 to match.
- **Success outcome:** Per-peer `next_expected_seq` reset to 1 (inbound); outbound
  `m_seq_state.next_seq` reset to 1 for `dst == reconnecting peer`; all held messages
  for the peer freed; staged `m_held_pending` discarded if it belongs to the reconnecting
  peer; all stale PENDING `AckTracker` slots and active `RetryManager` slots for the
  reconnecting peer cancelled, with one `RECONNECT_CANCEL` observability event emitted
  per cancelled entry.
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

// Called from reset_peer_ordering() — peer reconnect cleanup:
uint32_t AckTracker::cancel_peer(NodeId dst);           // SC: HAZ-001, HAZ-016
uint32_t RetryManager::cancel_peer(NodeId dst);         // SC: HAZ-016

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
      - Logs WARNING_LO.
   b. Calls `m_ordering.reset_peer(hello_src)` — resets inbound ordering state.
   c. Iterates `m_seq_state[]` (bounded, ≤ `ACK_TRACKER_CAPACITY`): finds slot where
      `active == true && dst == hello_src`, sets `next_seq = 1U`, logs INFO, and breaks.
      If no slot exists (peer never received an outbound message), no-op.
   d. Calls `m_ack_tracker.cancel_peer(hello_src)`: cancels all PENDING `AckTracker`
      slots where `destination_id == hello_src`; returns count cancelled; emits one
      `RECONNECT_CANCEL` observability event per cancelled entry; logs WARNING_HI if
      count > 0.
   e. Calls `m_retry_manager.cancel_peer(hello_src)`: cancels all active `RetryManager`
      slots where `destination_id == hello_src`; returns count cancelled; emits one
      `RECONNECT_CANCEL` observability event per cancelled entry; logs WARNING_HI if
      count > 0.
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
 └── drain_hello_reconnects()                          [NSC; bounded ≤ ORDERING_PEER_COUNT]
      └── m_transport->pop_hello_peer()                [per iteration; returns NodeId or INVALID]
      └── reset_peer_ordering(hello_src)               [SC: HAZ-001, HAZ-016]
           ├── [discard m_held_pending if src matches]
           ├── OrderingBuffer::reset_peer(src)         [SC: HAZ-001, HAZ-016]  ← inbound reset
           │    ├── find_peer(src)
           │    ├── m_peers[idx].next_expected_seq = 1U
           │    └── [free hold slots for src]
           ├── [iterate m_seq_state[]; reset next_seq=1 for dst==src]         ← outbound reset
           ├── AckTracker::cancel_peer(src)            [SC: HAZ-001, HAZ-016]  ← cancel stale PENDING slots
           │    └── [iterate slots; free PENDING where destination_id==src; return count]
           └── RetryManager::cancel_peer(src)          [SC: HAZ-016]           ← cancel stale retry slots
                └── [iterate slots; deactivate active where destination_id==src; return count]
```

---

## 5. Key Components

| Component | Responsibility |
|---|---|
| Transport backend (HELLO queue) | Detects HELLO on known channel; enqueues reconnecting `NodeId` |
| `pop_hello_peer()` | Drains one `NodeId` from the HELLO reconnect queue per call |
| `drain_hello_reconnects()` | Polls the queue and dispatches per-peer resets |
| `reset_peer_ordering()` | Clears `m_held_pending` if owned by reconnecting peer; resets inbound and outbound sequence state; cancels stale AckTracker and RetryManager entries |
| `OrderingBuffer::reset_peer()` | Resets `next_expected_seq` to 1 and frees hold slots (inbound) |
| `m_seq_state[]` loop in `reset_peer_ordering()` | Finds slot for `dst == src` and resets `next_seq` to 1 (outbound) |
| `AckTracker::cancel_peer(src)` | Cancels all stale PENDING AckTracker slots for the reconnecting peer's destination; prevents stale envelopes from being treated as pending ACK recipients on the new connection |
| `RetryManager::cancel_peer(src)` | Cancels all stale active RetryManager slots for the reconnecting peer's destination; prevents retry pump from re-sending pre-reset sequence-numbered envelopes on the new connection |

---

## 6. Branching Logic / Decision Points

| Condition | Outcome |
|---|---|
| `pop_hello_peer()` returns `NODE_ID_INVALID` | Queue empty; break loop |
| Peer not in OrderingBuffer peer table | No-op (idempotent) for inbound reset |
| `m_held_pending_valid && source == hello_src` | Staged pending message discarded |
| Hold slots found for reconnecting src | All freed (`active = false`) |
| `m_seq_state[i].active && dst == src` found | `next_seq` reset to 1; outbound sequence restarted |
| No `m_seq_state` slot for dst == src | No-op — peer never sent an outbound message from this node |
| `AckTracker::cancel_peer(src)` returns count > 0 | WARNING_HI logged; one `RECONNECT_CANCEL` event emitted per cancelled slot |
| `AckTracker::cancel_peer(src)` returns 0 | No in-flight ACK slots for this peer; no-op (idempotent) |
| `RetryManager::cancel_peer(src)` returns count > 0 | WARNING_HI logged; one `RECONNECT_CANCEL` event emitted per cancelled slot |
| `RetryManager::cancel_peer(src)` returns 0 | No active retry slots for this peer; no-op (idempotent) |

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

- `PeerState.next_expected_seq` reset to 1 for reconnecting peer (inbound).
- All `HoldSlot.active` flags cleared for reconnecting peer.
- `DeliveryEngine::m_held_pending_valid` cleared if staged message belongs to reconnecting peer.
- `m_seq_state[dst == src].next_seq` reset to 1 for reconnecting peer (outbound), if a slot exists.
- All `AckTracker` slots in PENDING state with `destination_id == src` freed; `m_count` decremented for each; one `RECONNECT_CANCEL` observability event emitted per cancelled entry.
- All `RetryManager` slots in WAITING/active state with `destination_id == src` deactivated; `m_count` decremented for each; one `RECONNECT_CANCEL` observability event emitted per cancelled entry.

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
                 -> OrderingBuffer::reset_peer(P)         [inbound]
                      -> next_expected_seq[P] = 1
                      -> free hold slots for P
                 -> m_seq_state[] scan for dst==P         [outbound]
                      -> m_seq_state[i].next_seq = 1
                 -> AckTracker::cancel_peer(P)            [cancel stale PENDING slots]
                      -> free PENDING slots where dst==P
                      -> emit RECONNECT_CANCEL event per cancelled slot
                 -> RetryManager::cancel_peer(P)          [cancel stale retry slots]
                      -> deactivate active slots where dst==P
                      -> emit RECONNECT_CANCEL event per cancelled slot
            -> pop_hello_peer()  -> NODE_ID_INVALID  [queue empty]
       -> continue: poll transport, reassembly, dedup, ordering...
  <- delivers first message from new session (seq=1)
  <- sends reply to P with seq=1 (not seq=N+1 from prior session)
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
- **Outbound seq not reset (historical, now fixed):** Prior to 2026-04-16, `reset_peer_ordering()`
  reset only the inbound `OrderingBuffer`. The outbound `m_seq_state` slot for `dst == src`
  was not reset, causing the server to send seq=N+1 on the first message of the new session
  while the reconnecting client expected seq=1. All echo replies were held out-of-order
  indefinitely (observed: 9 s timeout, 0/5 echoes received). Fixed by HAZ-016 outbound
  mitigation in `reset_peer_ordering()` (INSP-033).

---

## 15. Unknowns / Assumptions

- `ORDERING_PEER_COUNT` is the bound on both the `drain_hello_reconnects()` loop and the
  HELLO queue capacity. Defined in `src/core/Types.hpp`.
- UDP and DTLS backends use `TransportInterface`'s default no-op `pop_hello_peer()` (returns
  `NODE_ID_INVALID`). HELLO enforcement on those transports is via `process_hello_or_validate()`.
