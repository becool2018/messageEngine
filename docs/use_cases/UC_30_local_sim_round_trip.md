# UC_30 — LocalSimHarness round-trip: message sent, impaired, and received in one process

**HL Group:** HL-14 — In-Process Simulation
**Actor:** User
**Requirement traceability:** REQ-5.3.2, REQ-4.1.2, REQ-4.1.3, REQ-5.1.3

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::send()` on an engine backed by `LocalSimHarness` with impairments configured; then calls `DeliveryEngine::receive()` on the linked peer engine. File: `src/platform/LocalSimHarness.cpp`, `src/core/DeliveryEngine.cpp`.
- **Goal:** Demonstrate end-to-end message delivery through the full `DeliveryEngine` → `LocalSimHarness` impairment pipeline and back to a second engine, all within a single process, no sockets required.
- **Success outcome:** `send()` returns `OK`; `receive()` on the peer returns `OK` with the envelope. The impairment engine's configured loss/latency/duplication decisions are applied in transit.
- **Error outcomes:** `ERR_TIMEOUT` from `receive()` if the message was dropped by the impairment; `ERR_DUPLICATE` if a duplicate is received on a RELIABLE_RETRY path.

---

## 2. Entry Points

```cpp
// Two linked engines + harnesses
engine_a.send(envelope, now_us);       // backed by harness_a
engine_b.receive(out, timeout_ms, now_us); // backed by harness_b
```

---

## 3. End-to-End Control Flow

1. User sets up: `harness_a.init(cfg_a)`, `harness_b.init(cfg_b)`, `harness_a.link(&harness_b)`, `harness_b.link(&harness_a)`, `engine_a.init(&harness_a, ...)`, `engine_b.init(&harness_b, ...)`.
2. **`engine_a.send(envelope, now_us)`** — assigns message_id, calls `send_via_transport(work, now_us)` → `harness_a.send_message(work)`.
3. `harness_a.send_message(work)`:
   a. `m_impairment.process_outbound(work, now_us)` — applies loss/dup/latency. ERR_IO = silent drop; OK = queued.
   b. `collect_deliverable(now_us, ...)` — retrieves due entries.
   c. `flush_outbound_batch(...)` → for each due envelope: `harness_b.deliver_from_peer(env, now_us)`.
   d. Inside `deliver_from_peer()` on B: inbound partition check (`is_partition_active()`), then inbound reorder (`process_inbound()`), then `RingBuffer_b.push(env)`.
4. **`engine_b.receive(out, timeout_ms, now_us)`** → `harness_b.receive_message(out, timeout_ms)`:
   a. `m_recv_queue.pop(out)` — returns the injected envelope.
5. `engine_b.receive()` applies expiry check, dedup (if RELIABLE_RETRY), auto-ACK (if applicable).
6. Returns `OK` with `out` populated.

---

## 4. Call Tree

```
engine_a.send(envelope, now_us)
 └── DeliveryEngine::send_via_transport()
      └── LocalSimHarness_a::send_message()
           ├── ImpairmentEngine_a::process_outbound()
           ├── ImpairmentEngine_a::collect_deliverable()
           └── LocalSimHarness_a::flush_outbound_batch()
                └── LocalSimHarness_b::deliver_from_peer()     [inbound impairment on B]
                     ├── ImpairmentEngine_b::is_partition_active()
                     ├── ImpairmentEngine_b::process_inbound()
                     └── RingBuffer_b::push()

engine_b.receive(out, timeout_ms, now_us)
 └── LocalSimHarness_b::receive_message()
      └── RingBuffer_b::pop()
```

---

## 5. Key Components Involved

- **`DeliveryEngine`** — Both engines (A and B) apply full message lifecycle logic (ID assignment, ACK tracking, dedup).
- **`LocalSimHarness`** — In-process transport; avoids all socket overhead.
- **`ImpairmentEngine`** — Applied on the sender's harness side; simulates all configured impairments.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| Outbound loss impairment fires | Silent drop; B gets nothing | deliver_from_peer() called on B |
| Inbound partition active on B | `deliver_from_peer()` returns ERR_IO; B gets nothing | Push to B's ring |
| Inbound reorder-buffered on B | `deliver_from_peer()` returns ERR_TIMEOUT; B gets nothing yet | Push to B's ring |
| `receive()` timeout | Return `ERR_TIMEOUT` | Return `OK` with envelope |
| RELIABLE_RETRY duplicate | `ERR_DUPLICATE` | Normal delivery |

---

## 7. Concurrency / Threading Behavior

- In-process, single-threaded in typical test usage. Both A and B driven from the same thread.
- `RingBuffer` SPSC atomics ensure memory visibility.

---

## 8. Memory & Ownership Semantics

- Both harnesses hold their `RingBuffer` as value members. No heap allocation.
- `LocalSimHarness_b`'s `RingBuffer::m_buf` holds the injected envelope by value.

---

## 9. Error Handling Flow

- **Loss**: `send()` returns `OK`; `receive()` returns `ERR_TIMEOUT`. Test must handle this.
- **Normal delivery**: Both return `OK`. Round-trip complete.

---

## 10. External Interactions

- `nanosleep()` — used in `LocalSimHarness::receive_message()` polling loop (1ms sleeps).
- No sockets.

---

## 11. State Changes / Side Effects

Same as UC_24 (sender side) and UC_28 (engine init). After round-trip:

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `RingBuffer_b` | `m_buf[slot]` | stale | envelope (on success) |
| `DuplicateFilter` (engine_b) | `m_buf[m_write_idx]` | old | new entry (RELIABLE_RETRY) |

---

## 12. Sequence Diagram

```
User
  -> engine_a.send(envelope, now_us)
       -> harness_a.send_message()
            -> ImpairmentEngine_a::process_outbound()    [outbound impairments]
            -> ImpairmentEngine_a::collect_deliverable()
            -> flush_outbound_batch()
                 -> harness_b.deliver_from_peer(env, now_us)  [inbound impairment on B]
                      -> ImpairmentEngine_b::is_partition_active()
                      -> ImpairmentEngine_b::process_inbound()
                      -> RingBuffer_b.push(env)
  -> engine_b.receive(out, timeout_ms, now_us)
       -> harness_b.receive_message()
            -> RingBuffer_b.pop(out)
       [expiry check; dedup; auto-ACK]
  <- Result::OK
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- Both harnesses linked bidirectionally.
- Both engines initialized with their respective harnesses.
- Impairment config set on harness_a before `init()`.

**Runtime:**
- Single-shot or looping round-trip pattern. Each call is independent.

---

## 14. Known Risks / Observations

- **Impairment loss causes timeout:** Test must account for the probability that a message is dropped and adjust expectations accordingly (or use `loss_probability=0` for deterministic tests).
- **Fixed seed for deterministic tests:** Use `prng_seed` to ensure identical loss sequences across test runs (UC_29, UC_31).

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Engine B automatically sends ACKs for RELIABLE_ACK/RELIABLE_RETRY messages when `receive()` is called. This is confirmed by `DeliveryEngine::receive()` which auto-sends ACKs for those reliability classes.
