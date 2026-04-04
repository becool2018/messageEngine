# UC_61 — Delivery Observability Hooks: polling and draining the event ring

**HL Group:** Observability / Diagnostics
**Actor:** Application (caller of DeliveryEngine)
**Requirement traceability:** REQ-7.2.5

This use case describes how an application polls or bulk-drains the
`DeliveryEngine` observability event ring to observe send, ACK, retry, and
drop events without modifying delivery state.

---

## 1. Use Case Overview

- **Trigger:** The application calls `DeliveryEngine::poll_event()` or
  `DeliveryEngine::drain_events()` after performing one or more delivery
  operations (send, receive, pump_retries, sweep_ack_timeouts).
- **Goal:** Retrieve DeliveryEvent records that document what happened during
  the last delivery operations (SEND_OK, SEND_FAIL, ACK_RECEIVED,
  RETRY_FIRED, ACK_TIMEOUT, DUPLICATE_DROP, EXPIRY_DROP, MISROUTE_DROP).
- **Success outcome:** Event(s) returned in FIFO order; ring size decreases
  by the number of events consumed.
- **Error outcomes:**
  - `poll_event()` returns `ERR_EMPTY` when no events are pending.
  - `drain_events()` returns 0 when no events are pending.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.hpp

/// Pop the oldest pending observability event.
/// Returns OK + fills out; returns ERR_EMPTY when ring is empty.
/// NSC: no effect on delivery state.
Result poll_event(DeliveryEvent& out);

/// Return the number of unread events currently in the ring.
/// NSC: read-only.
uint32_t pending_event_count() const;

/// Drain up to buf_cap events into out_buf; returns count drained.
/// NSC: no effect on delivery state.
uint32_t drain_events(DeliveryEvent* out_buf, uint32_t buf_cap);
```

Called from application code after any `DeliveryEngine` operation that may
emit events.

---

## 3. End-to-End Control Flow

### 3.1 poll_event() — single-event poll

1. Assert `m_initialized` and ring size bounded.
2. Call `m_events.pop(out)`.
3. If ring is empty, `pop()` returns `ERR_EMPTY`; `poll_event()` propagates
   that result unchanged.
4. Otherwise copies the oldest `DeliveryEvent` into `out` and returns `OK`.

### 3.2 drain_events() — bulk drain

1. Assert `m_initialized` and `out_buf != nullptr || buf_cap == 0`.
2. Loop up to `buf_cap` times, calling `m_events.pop()` on each iteration.
3. Breaks early when `ERR_EMPTY` is returned (ring exhausted).
4. Returns count of events written into `out_buf`.

### 3.3 Ring overflow behavior

The ring is bounded by `DELIVERY_EVENT_RING_CAPACITY` (defined in
`src/core/Types.hpp`). When the ring is full and a new event is emitted:

- The **oldest** event is silently overwritten (ring/circular-buffer semantics).
- `push()` never blocks, never fails, and never allocates.
- `pending_event_count()` remains capped at `DELIVERY_EVENT_RING_CAPACITY`.

This means a slow consumer may miss the oldest events if the producer emits
faster than the consumer drains. Use `drain_events()` with a buffer sized to
`DELIVERY_EVENT_RING_CAPACITY` to minimize loss.

---

## 4. Event Kinds Emitted

| `DeliveryEventKind` | Emitted by | Condition |
|---------------------|-----------|-----------|
| `SEND_OK`           | `send()` | Transport accepted the message |
| `SEND_FAIL`         | `send()` / `record_send_failure()` | Transport rejected (non-expiry) |
| `EXPIRY_DROP`       | `send()` / `receive()` | Message had expired before or during send/receive |
| `ACK_RECEIVED`      | `receive()` | An ACK control message was processed |
| `RETRY_FIRED`       | `pump_retries()` | A retry transmission succeeded |
| `ACK_TIMEOUT`       | `sweep_ack_timeouts()` | ACK deadline elapsed without receipt |
| `DUPLICATE_DROP`    | `receive()` | RELIABLE_RETRY message seen more than once |
| `MISROUTE_DROP`     | `receive()` / `emit_routing_drop_event()` | Message destination_id ≠ local_id |

Each `DeliveryEvent` carries:
- `kind` — the event classification above.
- `message_id` — the affected message's ID.
- `node_id` — the relevant peer (source on receive-side events, destination
  on send-side events).
- `timestamp_us` — when the event was recorded (µs, from `now_us` argument).
- `result` — associated `Result` code (`OK`, `ERR_EXPIRED`, `ERR_DUPLICATE`,
  `ERR_TIMEOUT`, `ERR_INVALID`, `ERR_IO`, etc.).

---

## 5. Call Tree

```
Application
  -> DeliveryEngine::poll_event(out)             [DeliveryEngine.cpp]
       -> DeliveryEventRing::pop(out)             [DeliveryEventRing.hpp]
            [copies oldest event; decrements count]
            <- Result::OK or ERR_EMPTY

  -> DeliveryEngine::pending_event_count()        [DeliveryEngine.cpp]
       -> DeliveryEventRing::size()               [DeliveryEventRing.hpp]
            <- uint32_t count

  -> DeliveryEngine::drain_events(buf, cap)       [DeliveryEngine.cpp]
       -> DeliveryEventRing::pop(buf[i])          [DeliveryEventRing.hpp]  (loop, bounded by cap)
            <- uint32_t drained
```

Internal emission path (all NSC):
```
DeliveryEngine::send() / receive() / pump_retries() / sweep_ack_timeouts()
  -> DeliveryEngine::emit_event(kind, msg_id, node, ts, result)
       -> DeliveryEventRing::push(ev)             [DeliveryEventRing.hpp]
            [overwrites oldest if full; never blocks]
```

---

## 6. Branching Logic / Decision Points

**`poll_event()`:**
- Ring empty → return `ERR_EMPTY` (no copy performed).
- Ring non-empty → copy oldest, decrement count, return `OK`.

**`drain_events()`:**
- `buf_cap == 0` → return 0 immediately (loop does not execute).
- Ring exhausted before `buf_cap` reached → break early, return partial count.
- `buf_cap` reached before ring exhausted → return `buf_cap`.

**Ring push (overflow):**
- `count < DELIVERY_EVENT_RING_CAPACITY` → normal push; increment count.
- `count == DELIVERY_EVENT_RING_CAPACITY` → overwrite oldest; advance tail.

---

## 7. Concurrency / Threading Behavior

- `DeliveryEngine` is single-threaded by design contract.
- `DeliveryEventRing` uses no locks. Concurrent access from multiple threads
  would be a data race.
- All event emission and consumption must occur on the same thread as
  `DeliveryEngine::send()`, `receive()`, `pump_retries()`, and
  `sweep_ack_timeouts()`.

---

## 8. Memory and Ownership Semantics

- `DeliveryEventRing m_events` — fixed array of `DELIVERY_EVENT_RING_CAPACITY`
  `DeliveryEvent` structs; member of `DeliveryEngine`; zero-initialized in
  `DeliveryEngine::init()`.
- `DeliveryEvent` — plain struct (POD); no heap allocation.
- Power of 10 Rule 3 compliant: no dynamic allocation anywhere in this path.
- `drain_events()` writes into a **caller-supplied** array; the caller owns
  that buffer and must ensure it has at least `buf_cap` elements.

---

## 9. Error Handling Flow

| Condition | Behavior |
|-----------|----------|
| `poll_event()` on empty ring | Returns `ERR_EMPTY`; `out` not modified. |
| `drain_events()` on empty ring | Returns 0; `out_buf` not written. |
| `drain_events()` with `buf_cap == 0` | Returns 0 immediately; loop does not run. |
| Ring overflows (too many events) | Oldest event silently overwritten; no error returned. |

No `FATAL` paths exist in this observability-only code path.

---

## 10. External Interactions

- None beyond the `DeliveryEngine` instance itself. Observability events
  carry no pointers; they are self-contained value structs safe to copy
  and log.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DeliveryEventRing` | `m_count` | N | N − 1 (poll) or N − drained (drain) |
| `DeliveryEventRing` | `m_tail` | T | (T + drained) % CAP |
| `out` / `out_buf[i]` | (caller memory) | undefined | filled with event |

---

## 12. Sequence Diagram

### 12.1 poll_event() — single event

```
Application
  -> engine.poll_event(ev)
       -> m_events.pop(ev)
            [ring non-empty: copy m_buf[m_tail] into ev; m_tail++; m_count--]
            <- Result::OK
       <- Result::OK  ev.kind == SEND_OK (example)

Application
  -> engine.poll_event(ev)
       -> m_events.pop(ev)
            [ring empty: return ERR_EMPTY without modifying ev]
            <- Result::ERR_EMPTY
       <- Result::ERR_EMPTY
```

### 12.2 drain_events() — bulk drain

```
Application
  -> engine.drain_events(buf, 16)
       i=0: m_events.pop(buf[0]) -> OK
       i=1: m_events.pop(buf[1]) -> OK
       ...
       i=N: m_events.pop(buf[N]) -> ERR_EMPTY  [ring exhausted]
       break; return N
  <- N (events written into buf[0..N-1])
```

### 12.3 Overflow — oldest event overwritten

```
[ring at DELIVERY_EVENT_RING_CAPACITY]
engine.send(env, now_us)
  -> emit_event(SEND_OK, ...)
       -> m_events.push(ev)
            [full: write new event at m_head; advance m_tail (oldest discarded)]
Application never sees the discarded event.
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine::init()` called, which calls `m_events.init()`.
- After `init()`, `pending_event_count() == 0`.

**Runtime:**
- Events accumulate as delivery operations execute.
- Application drains at its own pace (polling loop, periodic drain, etc.).
- No events are emitted during `init()` itself.

---

## 14. Known Risks / Observations

- **Slow consumer loss:** If the application does not drain the ring between
  delivery bursts, the oldest events are silently overwritten. Size the drain
  buffer to `DELIVERY_EVENT_RING_CAPACITY` to capture all events in one call.
- **No timestamps from wall clock:** `timestamp_us` is taken from the
  `now_us` argument passed by the caller to `send()`, `receive()`, etc. If
  the caller passes a stale `now_us`, event timestamps will be inaccurate.
- **No callback / push interface:** The ring is pull-only. There is no
  mechanism to notify the application that events are available. The
  application must poll.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `DELIVERY_EVENT_RING_CAPACITY` is defined in
  `src/core/Types.hpp` and is a compile-time constant ≤ 256.
- `[ASSUMPTION]` Single-threaded access only. No locking is needed or
  provided.
- `[ASSUMPTION]` `drain_events()` with `out_buf == nullptr` and
  `buf_cap == 0` is a valid no-op call; the assertion permits this.
