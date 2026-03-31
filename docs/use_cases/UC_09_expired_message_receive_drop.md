# UC_09 — Incoming message dropped at receive because expiry_time_us has passed

**HL Group:** HL-5 — Receive a Message
**Actor:** System
**Requirement traceability:** REQ-3.2.2, REQ-3.2.7, REQ-3.3.4

---

## 1. Use Case Overview

**Trigger:** `DeliveryEngine::receive()` is called; the transport delivers a `MessageEnvelope` whose `expiry_time_us` field is non-zero and `now_us >= expiry_time_us`.

**Goal:** Silently drop the stale envelope before it is returned to the application, and return `Result::ERR_EXPIRED` to the caller so it can decide how to proceed (retry, log, or discard).

**Success outcome:** The expired envelope is discarded. A `WARNING_LO` log entry is emitted. No state in `AckTracker`, `RetryManager`, or `DuplicateFilter` is modified. The caller receives `Result::ERR_EXPIRED`.

**Error outcomes:**
- If the transport itself returns non-OK (e.g., `ERR_TIMEOUT`), `receive()` returns early; the expiry check is not reached.
- If `expiry_time_us == 0` (never-expires), `timestamp_expired()` returns `false`; the message is not dropped by this path.

---

## 2. Entry Points

**Primary entry:** Application thread calls:
```
// src/core/DeliveryEngine.cpp, line 149
Result DeliveryEngine::receive(MessageEnvelope& env, uint32_t timeout_ms, uint64_t now_us)
```

The application supplies:
- `env` — caller-owned `MessageEnvelope` on the stack (output buffer).
- `timeout_ms` — transport receive timeout in milliseconds.
- `now_us` — current monotonic time in microseconds, typically from `timestamp_now_us()`.

The expiry check is an embedded guard inside `receive()` — it is not a separately callable function.

---

## 3. End-to-End Control Flow

1. Application calls `DeliveryEngine::receive(env, timeout_ms, now_us)`.
2. `receive()` fires `NEVER_COMPILED_OUT_ASSERT(m_initialized)` and `NEVER_COMPILED_OUT_ASSERT(now_us > 0)`.
3. Guard `!m_initialized` — engine is initialized; execution continues.
4. `receive()` calls `m_transport->receive_message(env, timeout_ms)` via virtual dispatch.
5. Transport fills `env` with the next available message and returns `Result::OK`. (If non-OK, `receive()` returns `res` immediately — expiry check not reached.)
6. `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))` fires — checks `message_type != INVALID`, `payload_length <= MSG_MAX_PAYLOAD_BYTES`, `source_id != NODE_ID_INVALID`.
7. `receive()` calls `timestamp_expired(env.expiry_time_us, now_us)`:
    - In `Timestamp.hpp`: fires two asserts confirming both values fit in `uint64_t`.
    - Evaluates `(expiry_time_us != 0ULL) && (now_us >= expiry_time_us)`.
    - Since `expiry_time_us` is non-zero and `now_us >= expiry_time_us`, returns `true`.
8. Branch taken: `timestamp_expired()` returned `true`.
9. `receive()` logs `WARNING_LO`: "Dropping expired message_id=%llu from src=%u" with the envelope's `message_id` and `source_id`.
10. `receive()` returns `Result::ERR_EXPIRED` immediately. The output `env` is defined but its content represents the now-dropped message; the caller should treat it as invalid.
11. `AckTracker`, `RetryManager`, and `DuplicateFilter` are not touched.

---

## 4. Call Tree

```
DeliveryEngine::receive()
 ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
 ├── NEVER_COMPILED_OUT_ASSERT(now_us > 0)
 ├── m_transport->receive_message(env, timeout_ms)     [virtual dispatch]
 ├── NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))
 ├── timestamp_expired(env.expiry_time_us, now_us)     [inline]
 │    ├── NEVER_COMPILED_OUT_ASSERT(expiry_time_us fits uint64_t)
 │    ├── NEVER_COMPILED_OUT_ASSERT(now_us fits uint64_t)
 │    └── return (expiry_time_us != 0) && (now_us >= expiry_time_us) => true
 ├── Logger::log(WARNING_LO, "Dropping expired message_id=...")
 └── return Result::ERR_EXPIRED
```

---

## 5. Key Components Involved

- **`DeliveryEngine::receive()`** — top-level receive orchestrator; applies the expiry guard immediately after envelope validation.
- **`TransportInterface::receive_message()`** — virtual method; delivers the expired envelope without knowing its expiry state. The transport has no expiry awareness.
- **`timestamp_expired()`** (`src/core/Timestamp.hpp`) — inline predicate; the sole mechanism for expiry evaluation. Treats `expiry_time_us == 0` as never-expires.
- **`Logger::log()`** — emits a `WARNING_LO` log entry with enough context (message ID, source) to diagnose latency or clock-skew issues.

---

## 6. Branching Logic / Decision Points

| Condition | True Branch | False Branch | Next Control |
|-----------|-------------|--------------|--------------|
| `m_transport->receive_message()` returns `OK` | Continue to `envelope_valid` check | Return `res` to caller immediately | `envelope_valid` assert |
| `envelope_valid(env)` fails | `NEVER_COMPILED_OUT_ASSERT` fires (abort in debug, `on_fatal_assert` in prod) | Continue to expiry check | `timestamp_expired()` |
| `timestamp_expired(env.expiry_time_us, now_us)` | Log `WARNING_LO`; return `ERR_EXPIRED` | Continue to `envelope_is_control()` (not this UC) | — |
| `expiry_time_us == 0` inside `timestamp_expired()` | Return `false` (never-expires; not this UC) | Evaluate `now_us >= expiry_time_us` | Return bool |

---

## 7. Concurrency / Threading Behavior

- **Thread context:** The single application thread calling `receive()`. All processing is in-line; no context switch.
- **No mutex or atomic operations** on this path. `timestamp_expired()` is a pure inline function operating only on its arguments.
- **Clock:** `now_us` is supplied by the caller. It is derived from `clock_gettime(CLOCK_MONOTONIC, ...)` before the call. If the caller passes a stale `now_us`, messages may be spuriously retained or dropped. [ASSUMPTION] The caller refreshes `now_us` on each call to `receive()`.
- **[ASSUMPTION]** Single-threaded access. Concurrent calls to `receive()` from different threads are not protected.

---

## 8. Memory & Ownership Semantics

- **`MessageEnvelope& env`** — caller-allocated (typically stack). Filled by the transport in step 4 with the expired message. After step 10 the caller should treat `env` as containing a stale/invalid message; its contents are implementation-defined relative to the drop.
- **No heap allocation** anywhere on this path. All state is stack-local or in fixed arrays.
- **Power of 10 Rule 3 confirmation:** `timestamp_expired()` is a pure computation on two `uint64_t` values; no allocation, no side effects on memory.
- **`AckTracker` / `RetryManager` / `DuplicateFilter`:** All untouched. An expired message that was tracked for retry (RELIABLE_RETRY) will still occupy a `RetryManager` slot until it either ACKs, its expiry fires in `collect_due()`, or its retry budget is exhausted.

---

## 9. Error Handling Flow

| Result code | Trigger condition | System state after | Caller action |
|-------------|------------------|--------------------|---------------|
| `Result::ERR_TIMEOUT` | Transport `receive_message()` returns `ERR_TIMEOUT` | No state modified | Caller polls again later |
| `Result::ERR_EXPIRED` | `timestamp_expired()` returns `true` (this UC) | `env` contains stale data; all trackers untouched | Caller discards `env`; may log or count expiry events |
| Assert abort (debug) | `envelope_valid(env)` fails after transport receive | Process aborted | N/A — indicates transport bug |

No retry logic is triggered by `ERR_EXPIRED` at the `DeliveryEngine::receive()` level. The retry decision is entirely the application's responsibility.

---

## 10. External Interactions

- `m_transport->receive_message(env, timeout_ms)` — virtual dispatch. For `TcpBackend`: `poll(2)` on socket fds then framed-byte read. For `UdpBackend`: `recvfrom(2)`. For `LocalSimHarness`: in-process ring buffer pop.
- The caller invokes `clock_gettime(CLOCK_MONOTONIC, ...)` via `timestamp_now_us()` before calling `receive()` to obtain `now_us`.
- No additional OS calls occur inside the expiry-drop path itself.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `env` (caller's stack) | all fields | arbitrary | filled by transport (stale content) |
| `Logger` | log output | — | `WARNING_LO` "Dropping expired message_id=... from src=..." |
| `AckTracker` | any | unchanged | unchanged |
| `RetryManager` | any | unchanged | unchanged |
| `DuplicateFilter` | any | unchanged | unchanged |

No persistent or shared state is modified. The expired message is silently consumed from the transport queue; it will not appear again on a subsequent `receive()` call.

---

## 12. Sequence Diagram

```
Application       DeliveryEngine       TransportInterface    timestamp_expired()
    |                   |                     |                      |
    |--receive(env,t,n)->|                     |                      |
    |                   |--receive_message(env,t)->                   |
    |                   |<--- OK + env filled (expiry in past) -------|
    |                   |                     |                      |
    |                   |--ASSERT envelope_valid(env)                 |
    |                   |                     |                      |
    |                   |--timestamp_expired(expiry_us, now_us)------>|
    |                   |                     |  (expiry_us != 0)    |
    |                   |                     |  && (now >= expiry)  |
    |                   |<---------------------------------------- true
    |                   |                     |                      |
    |                   |--Logger::log(WARNING_LO, "Dropping expired...")|
    |<-- ERR_EXPIRED ----|                     |                      |
```

---

## 13. Initialization vs Runtime Flow

**Initialization (before this UC):**
- `DeliveryEngine::init()` sets `m_initialized = true` and wires in the transport. No expiry-related initialization is needed.
- `timestamp_expired()` is a stateless inline function; it has no init-time dependency.

**Runtime (this UC):**
- This UC fires in any steady-state receive loop where the sender set a non-zero `expiry_time_us` and the message was delayed (e.g., by network latency, impairment engine buffering, or a slow application loop).
- The check is O(1): two comparisons on `uint64_t` values.
- The log emission is the only side effect.

---

## 14. Known Risks / Observations

- **Silent discard:** `ERR_EXPIRED` is returned but the caller is not obligated to log or count it. A sender that sets short expiry times on a high-latency path will experience silent message loss.
- **Clock skew between nodes:** `expiry_time_us` is set by the sender using the sender's `CLOCK_MONOTONIC`. The receiver evaluates it using the receiver's `CLOCK_MONOTONIC`. If the two clocks diverge (different machines), messages may be incorrectly dropped or kept. [ASSUMPTION] Clock skew is acceptable for the use cases currently targeted (in-process simulation and single-host testing).
- **No retry triggered:** An expired `RELIABLE_RETRY` message that is dropped here still occupies a `RetryManager` slot on the sender until `collect_due()` observes `expiry_us` has passed. There is a window where the receiver drops the message but the sender has not yet cleaned up the slot.
- **`envelope_valid` assert is load-bearing:** If the transport backend produces an envelope with `source_id == NODE_ID_INVALID` or `message_type == INVALID`, the assert fires before the expiry check. This protects against corrupted transports but will abort the process in debug builds.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] The application calls `timestamp_now_us()` immediately before `receive()` on each polling cycle so that `now_us` is fresh.
- [ASSUMPTION] `expiry_time_us` in the envelope is expressed in the sender's `CLOCK_MONOTONIC` epoch. Cross-machine expiry is not supported without clock synchronization.
- [ASSUMPTION] The caller checks the return code of `receive()` and does not use the contents of `env` when `ERR_EXPIRED` is returned.
- [ASSUMPTION] There is no per-channel or per-reliability-class override of the expiry policy. All envelopes with non-zero `expiry_time_us` are subject to this drop.
- [ASSUMPTION] `expiry_time_us == 0` is reserved to mean "never expires" and is not a valid past-epoch timestamp. The code uses this sentinel as-is; no collision check is performed.
