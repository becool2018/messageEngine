# UC_08 — ACK Resolution

**HL Group:** HL-2 — User sends a message requiring confirmation
**Actor:** System
**Requirement traceability:** REQ-3.2.4, REQ-3.3.2, REQ-3.3.3, REQ-3.2.5

---

## 1. Use Case Overview

### Clear description of what triggers this flow

The sender's `DeliveryEngine::receive()` is called and the transport returns a
`MessageEnvelope` with `message_type = ACK`. This is an in-band control message
sent by the remote peer (typically constructed via `envelope_make_ack()`) to
confirm receipt of a previously sent `DATA` message. The ACK's `message_id`
field carries the `message_id` of the original `DATA` message being acknowledged,
and the ACK's `source_id` field carries the remote peer's local node ID.

This use case is the completion event for UC_02 (Reliable-with-ACK send) and
the cancellation event for UC_03 (Reliable-with-retry send). It documents the
path from the moment `DeliveryEngine::receive()` detects `message_type == ACK`
through the state transitions in `AckTracker` and `RetryManager`.

### Expected outcome (single goal)

The corresponding `AckTracker` slot transitions from `PENDING` to `ACKED`. The
corresponding `RetryManager` slot (if present) is deactivated (`active = false`,
`m_count` decremented). Both transitions are attempted unconditionally and
independently of each other; `ERR_INVALID` from either is discarded.
`DeliveryEngine::receive()` returns `Result::OK` to the caller with the ACK
envelope populated in `env`. A subsequent call to `sweep_ack_timeouts()` will
reclaim the `ACKED` slot.

---

## 2. Entry Points

### Exact functions, threads, or events where execution begins

- **Primary entry point:** `DeliveryEngine::receive(MessageEnvelope& env,
  uint32_t timeout_ms, uint64_t now_us)` — `DeliveryEngine.cpp` line 149.
- **ACK detection branch:** line 180: `if (env.message_type == MessageType::ACK)`.
- **ACK resolution sub-entries (called sequentially):**
  - `AckTracker::on_ack(NodeId src, uint64_t msg_id)` — `AckTracker.cpp:80`.
  - `RetryManager::on_ack(NodeId src, uint64_t msg_id)` — `RetryManager.cpp:123`.
- **Pre-condition:** A `PENDING` entry exists in `AckTracker` (placed there by
  `AckTracker::track()` during the originating `send()` call). An active retry
  slot may or may not exist in `RetryManager`, depending on whether the original
  message used `RELIABLE_ACK` (no retry) or `RELIABLE_RETRY` (with retry).

### Example: main(), ISR, callback, RPC handler, etc.

The application calls `engine.receive(env, timeout_ms, now_us)` in its main
loop. When the transport returns an ACK envelope, `receive()` processes it
internally. No ISR or separate thread is involved; the call is synchronous.

---

## 3. End-to-End Control Flow (Step-by-Step)

**Step 1 — Application invokes `DeliveryEngine::receive()`**

Application calls `engine.receive(env, timeout_ms, now_us)`. Typically, `now_us`
was captured just before this call via `timestamp_now_us()`.

Precondition assertions at `DeliveryEngine.cpp:153-154`:
- `NEVER_COMPILED_OUT_ASSERT(m_initialized)` — engine is initialized.
- `NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)` — valid timestamp.

`m_initialized` guard at line 156: if false, returns `ERR_INVALID`. For this
use case, `m_initialized == true` and execution continues.

**Step 2 — Transport receive (DeliveryEngine.cpp:161)**

```
Result res = m_transport->receive_message(env, timeout_ms);
```

Virtual dispatch to `TcpBackend::receive_message()`. The `TcpBackend` pops a
`MessageEnvelope` from `m_recv_queue` (SPSC ring buffer). If the queue is empty,
it enters a polling loop: `poll_clients_once(100)` (calls `recv_from_client()`
per connected fd, which calls `tcp_recv_frame()` → `Serializer::deserialize()`
→ `m_recv_queue.push()`), then `flush_delayed_to_queue()` for impairment-delayed
messages. This repeats up to `poll_count` iterations (capped at 50, covering up
to 5 seconds). Returns `OK` on success or `ERR_TIMEOUT` if no message arrived.

For this use case: transport returns `OK` with `env` populated as:
- `env.message_type = MessageType::ACK`
- `env.message_id = original_data_message_id` (set by `envelope_make_ack()`
  at `MessageEnvelope.hpp:95`)
- `env.source_id = remote_peer_node_id` (the ACK sender's local ID, set at
  `MessageEnvelope.hpp:97`)
- `env.expiry_time_us = 0U` (set at `MessageEnvelope.hpp:101`)
- `env.payload_length = 0U`

If `res != OK` (e.g., `ERR_TIMEOUT`): `receive()` returns `res` immediately;
no ACK processing occurs.

**Step 3 — Envelope validation (DeliveryEngine.cpp:168)**

```
NEVER_COMPILED_OUT_ASSERT(envelope_valid(env));
```

`envelope_valid()` (`MessageEnvelope.hpp:63`): checks `message_type != INVALID`
(ACK=1, passes), `payload_length <= MSG_MAX_PAYLOAD_BYTES` (0 <= 4096, passes),
`source_id != NODE_ID_INVALID` (peer_id != 0, passes). Assertion passes.

**Step 4 — Expiry check (DeliveryEngine.cpp:171-175)**

```
if (timestamp_expired(env.expiry_time_us, now_us))
```

`timestamp_expired()` (`Timestamp.hpp:51`): `env.expiry_time_us = 0U`.
`(0 != 0ULL) = false` → returns `false`. ACK envelopes never expire.
Branch not taken; execution continues.

**Step 5 — Control message detection (DeliveryEngine.cpp:179)**

```
if (envelope_is_control(env))
```

`envelope_is_control()` (`MessageEnvelope.hpp:79`): returns true for ACK,
NAK, or HEARTBEAT. For `message_type == ACK`: returns `true`. Branch taken.

**Step 6 — ACK type check (DeliveryEngine.cpp:180)**

```
if (env.message_type == MessageType::ACK)
```

`MessageType::ACK = 1`. Condition is true. ACK resolution block entered.

**Step 7 — AckTracker::on_ack() (DeliveryEngine.cpp:183-184)**

```
Result ack_res = m_ack_tracker.on_ack(env.source_id, env.message_id);
(void)ack_res;
```

**Inside `AckTracker::on_ack()`** (`AckTracker.cpp:80-106`):

- Precondition: `NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)`.
- Linear scan of `m_slots[0..ACK_TRACKER_CAPACITY-1]` (fixed bound, 32 slots):
  ```
  for (i = 0..31):
    if (m_slots[i].state == EntryState::PENDING
        && m_slots[i].env.source_id == src
        && m_slots[i].env.message_id == msg_id)
  ```
  - `m_slots[i].env.source_id`: the local node's ID (set during `send()` at
    `DeliveryEngine.cpp:88` as `env.source_id = m_local_id`).
  - `src` parameter: `env.source_id` from the incoming ACK = remote peer's ID.
  - In a two-node deployment, `local_id != peer_id` → comparison fails for all
    slots → `on_ack()` returns `ERR_INVALID`. See section 14, Risk 1.
  - In loopback/self-test (local_id == peer_id), the match succeeds:
    - `m_slots[i].state = EntryState::ACKED`.
    - Postcondition: `NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == ACKED)`.
    - Returns `Result::OK`.
  - If no match: `NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)`.
    Returns `Result::ERR_INVALID`.

- `(void)ack_res`: result discarded. `ERR_INVALID` does not fail `receive()`.
- Note: `m_count` is **not** decremented by `on_ack()`. It is decremented by
  `sweep_one_slot()` (called from `sweep_expired()`) when the `ACKED` slot is
  transitioned back to `FREE`.

**Step 8 — RetryManager::on_ack() (DeliveryEngine.cpp:186-187)**

```
Result retry_res = m_retry_manager.on_ack(env.source_id, env.message_id);
(void)retry_res;
```

**Inside `RetryManager::on_ack()`** (`RetryManager.cpp:123-154`):

- Preconditions:
  - `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
  - `NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID)`.
- Linear scan of `m_slots[0..ACK_TRACKER_CAPACITY-1]` (fixed bound, 32 slots):
  ```
  for (i = 0..31):
    if (m_slots[i].active
        && m_slots[i].env.source_id == src
        && m_slots[i].env.message_id == msg_id)
  ```
  - Same source_id matching issue as `AckTracker::on_ack()`. In a two-node
    deployment, `m_slots[i].env.source_id = local_id` vs `src = peer_id` → no
    match → returns `ERR_INVALID`.
  - On match (loopback):
    - `m_slots[i].active = false`.
    - `--m_count`.
    - Postcondition: `NEVER_COMPILED_OUT_ASSERT(m_count <= ACK_TRACKER_CAPACITY)`.
    - `Logger::log(INFO, "RetryManager", "Cancelled retry for message_id=%llu
      from node=%u", msg_id, src)`.
    - Returns `Result::OK`.
  - On no match:
    - `Logger::log(WARNING_LO, "RetryManager", "No retry entry found for
      message_id=%llu from node=%u", msg_id, src)`.
    - Returns `Result::ERR_INVALID`.

- `(void)retry_res`: result discarded.
- Note: A `RELIABLE_ACK` message has no retry slot (only `AckTracker` tracks
  it). `RetryManager::on_ack()` will return `ERR_INVALID` for all `RELIABLE_ACK`
  messages — this is the expected behavior and the `WARNING_LO` log is normal.

**Step 9 — ACK receipt log (DeliveryEngine.cpp:189-191)**

```
Logger::log(INFO, "DeliveryEngine", "Received ACK for message_id=%llu from src=%u",
            (unsigned long long)env.message_id, env.source_id);
```

**Step 10 — Return (DeliveryEngine.cpp:194)**

```
return Result::OK;
```

The ACK envelope is fully processed. `env` still contains the ACK message fields
(the caller can inspect `env.message_type == ACK` and `env.message_id` to
identify which message was acknowledged). `receive()` returns `OK`.

**--- ACKED slot cleanup (separate periodic call, not part of receive()) ---**

After `on_ack()` sets `m_slots[i].state = ACKED`, the slot is not freed until
the application calls `sweep_ack_timeouts()`:

```
DeliveryEngine::sweep_ack_timeouts(now_us)
  → AckTracker::sweep_expired(now_us, timeout_buf, ACK_TRACKER_CAPACITY)
  → [loop i=0..31] sweep_one_slot(i, now_us, expired_buf, buf_cap, expired_count)
    → if (m_slots[i].state == ACKED): m_slots[i].state = FREE; --m_count.
```

`sweep_one_slot()` (`AckTracker.cpp:112`) handles two cases:
- `ACKED` slots: transitions to `FREE`, decrements `m_count`. Not added to
  `expired_buf`.
- Expired `PENDING` slots (`now_us >= deadline_us`): copies to `expired_buf`
  (if space), transitions to `FREE`, decrements `m_count`. These are reported
  to the caller as timed-out messages.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::receive(env, timeout_ms, now_us)          [DeliveryEngine.cpp:149]
|
+-- NEVER_COMPILED_OUT_ASSERT(m_initialized)
+-- NEVER_COMPILED_OUT_ASSERT(now_us > 0ULL)
+-- [m_initialized guard: passes]
+-- m_transport->receive_message(env, timeout_ms)         [virtual dispatch, line 161]
|   \-- TcpBackend::receive_message()                     [TcpBackend.cpp:386]
|       +-- m_recv_queue.pop(envelope)                    [RingBuffer, fast path]
|       +-- [poll loop if empty]:
|       |   +-- poll_clients_once(100)                    [TcpBackend.cpp:326]
|       |   |   +-- accept_clients() [server mode only]
|       |   |   \-- recv_from_client(fd, 100)             [TcpBackend.cpp:224]
|       |   |       +-- tcp_recv_frame()                  [SocketUtils.cpp]
|       |   |       +-- Serializer::deserialize()         [Serializer.cpp:175]
|       |   |       \-- m_recv_queue.push(env)
|       |   +-- m_recv_queue.pop(envelope)
|       |   +-- flush_delayed_to_queue(now_us)
|       |   \-- m_recv_queue.pop(envelope)
|       \-- return OK (ACK envelope)
|
+-- NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))        [line 168, inline]
+-- timestamp_expired(env.expiry_time_us=0, now_us)       [Timestamp.hpp:51, inline]
|   \-- returns false (expiry_us == 0)
+-- envelope_is_control(env)                              [MessageEnvelope.hpp:79, inline]
|   \-- returns true (message_type == ACK)
+-- [message_type == ACK: true]
|
+-- m_ack_tracker.on_ack(env.source_id, env.message_id)   [AckTracker.cpp:80]
|   +-- NEVER_COMPILED_OUT_ASSERT(m_count <= 32)
|   +-- [loop i=0..31]:
|   |   [if PENDING && source_id == src && message_id == msg_id]:
|   |       m_slots[i].state = ACKED
|   |       NEVER_COMPILED_OUT_ASSERT(state == ACKED)
|   |       return OK
|   \-- [no match]: NEVER_COMPILED_OUT_ASSERT(m_count <= 32); return ERR_INVALID
+-- (void)ack_res
|
+-- m_retry_manager.on_ack(env.source_id, env.message_id) [RetryManager.cpp:123]
|   +-- NEVER_COMPILED_OUT_ASSERT(m_initialized)
|   +-- NEVER_COMPILED_OUT_ASSERT(src != NODE_ID_INVALID)
|   +-- [loop i=0..31]:
|   |   [if active && source_id == src && message_id == msg_id]:
|   |       m_slots[i].active = false
|   |       --m_count
|   |       NEVER_COMPILED_OUT_ASSERT(m_count <= 32)
|   |       Logger::log(INFO "Cancelled retry...")
|   |       return OK
|   \-- [no match]: Logger::log(WARNING_LO "No retry entry..."); return ERR_INVALID
+-- (void)retry_res
|
+-- Logger::log(INFO "Received ACK for message_id=...")
\-- return OK

--- ACKED slot cleanup (separate call) ---

DeliveryEngine::sweep_ack_timeouts(now_us)              [DeliveryEngine.cpp:267]
    AckTracker::sweep_expired(now_us, timeout_buf, 32)  [AckTracker.cpp:147]
        [loop i=0..31]:
            sweep_one_slot(i, now_us, expired_buf, cap, count)  [AckTracker.cpp:112]
                [if ACKED]: state = FREE; --m_count; return 0
                [if PENDING && now_us >= deadline_us]: copy to buf; FREE; --m_count; return 1
```

---

## 5. Key Components Involved

### DeliveryEngine
- **Responsibility:** Receives from transport, validates envelope, routes to
  expiry check, control message detection, and ACK resolution. Calls both
  `on_ack()` methods sequentially and discards both results.
- **Why in this flow:** It is the sole receive API. ACK routing at lines 179-194
  is the gating logic.

### AckTracker
- **Responsibility:** Tracks up to 32 outstanding messages awaiting ACK. Each
  `Entry` struct holds a full `MessageEnvelope` copy, a `deadline_us`, and a
  `state` (`FREE/PENDING/ACKED`). `on_ack()` performs `PENDING → ACKED`.
  `sweep_one_slot()` (called from `sweep_expired()`) performs `ACKED → FREE`.
- **State machine:** `FREE` → (track) → `PENDING` → (on_ack) → `ACKED` →
  (sweep) → `FREE`. Also: `PENDING` → (deadline expired) → `FREE` via sweep.
- **Why in this flow:** Holds the canonical "did we get an ACK for this message"
  state. `ACKED` state persists until `sweep_expired()` reclaims the slot.

### RetryManager
- **Responsibility:** Tracks up to 32 active retry schedules. `on_ack()`
  cancels the retry by setting `active = false` and decrementing `m_count`.
  After cancellation, `collect_due()` skips the inactive slot.
- **Why in this flow:** For `RELIABLE_RETRY` messages, ACK receipt must cancel
  further retransmission. `on_ack()` is the cancellation mechanism.

### MessageEnvelope / envelope_make_ack()
- **Responsibility:** `envelope_make_ack()` (`MessageEnvelope.hpp:88-103`)
  constructs the ACK envelope on the sender (remote peer) side: sets
  `message_type = ACK`, `message_id = original.message_id`,
  `source_id = my_id` (the ACK sender's local node ID),
  `destination_id = original.source_id`, `expiry_time_us = 0U`.
- **Why in this flow:** The ACK's `message_id` is the lookup key used by both
  `on_ack()` calls. The ACK's `source_id` (remote peer's ID) is the problematic
  field in the source_id matching issue.

### timestamp_expired() (Timestamp.hpp:51)
- **Responsibility:** Returns false for `expiry_time_us = 0`. ACK envelopes are
  always set to `expiry_time_us = 0` by `envelope_make_ack()`.
- **Why in this flow:** Ensures ACKs are never dropped by the expiry gate.

### sweep_one_slot() (AckTracker.cpp:112)
- **Responsibility:** Private helper. Handles one slot during
  `sweep_expired()`. Releases `ACKED` slots to `FREE`. Reports expired `PENDING`
  slots to the caller via `expired_buf`.
- **Why in this flow:** `ACKED` slots are not freed by `on_ack()` itself; they
  accumulate until `sweep_expired()` runs and `sweep_one_slot()` clears them.

---

## 6. Branching Logic / Decision Points

**Branch 1: transport returns OK? (line 162-164)**
- No: return transport error; ACK processing never reached.

**Branch 2: envelope_valid() assertion**
- ACK envelopes: `message_type = ACK (1) != INVALID (255)` ✓,
  `payload_length = 0 <= 4096` ✓, `source_id != 0` ✓. Passes.
- Failure would abort (NEVER_COMPILED_OUT_ASSERT).

**Branch 3: timestamp_expired? (line 171)**
- `expiry_time_us = 0` → always false for ACKs. Never taken.

**Branch 4: envelope_is_control? (line 179)**
- ACK: true → enter control handler.
- DATA: false → proceed to dedup path.

**Branch 5: message_type == ACK? (line 180)**
- ACK: true → call both `on_ack()` methods.
- NAK or HEARTBEAT: false → skip ACK resolution, return `OK` at line 194.

**Branch 6: AckTracker slot scan match (AckTracker.cpp:89-99)**
- Match found (PENDING && source_id == src && message_id == msg_id):
  `state = ACKED`, return `OK`.
- No match: return `ERR_INVALID`. Discarded by DeliveryEngine.
- Critical: in a two-node deployment, `source_id` mismatch means no match ever.
  See Risk 1.

**Branch 7: RetryManager slot scan match (RetryManager.cpp:130-147)**
- Match found (active && source_id == src && message_id == msg_id):
  `active = false`, `--m_count`, log `INFO`, return `OK`.
- No match: log `WARNING_LO`, return `ERR_INVALID`. Discarded.
- Same source_id mismatch issue.

**Branch 8: AckTracker result vs RetryManager result — independence**
- Both are called unconditionally. A match in `AckTracker` but not `RetryManager`
  (e.g., `RELIABLE_ACK` message) is normal; both results are discarded.
- A match in `RetryManager` but not `AckTracker` is theoretically possible if
  the `AckTracker` slot was already swept.

**Branch 9: sweep_one_slot() state dispatch**
- `PENDING && now_us >= deadline_us`: copy to expired_buf, `FREE`, `--m_count`.
- `ACKED`: directly to `FREE`, `--m_count`. (Not added to expired_buf.)
- `FREE` or non-expired `PENDING`: no action, return 0.

---

## 7. Concurrency / Threading Behavior

### Threads created
None. `receive()`, `on_ack()`, and `sweep_ack_timeouts()` all execute
synchronously on the caller's thread.

### Where context switches occur
Inside `TcpBackend::receive_message()`: `poll()` (waits for socket readability)
and `recv()` (reads bytes) are POSIX blocking calls. The rest of the flow
executes without blocking.

### Synchronization primitives
`AckTracker::m_slots[]` and `m_count`: no atomics, no mutex. Not thread-safe.
`RetryManager::m_slots[]` and `m_count`: no atomics, no mutex. Not thread-safe.

`RingBuffer` uses `std::atomic<uint32_t>` for head/tail (acquire/release). This
protects the queue between `recv_from_client()` (producer) and `receive_message()`
(consumer), but only if they are called from separate threads. In the single-
threaded model, both are called from the same thread.

If `receive()` and `pump_retries()` / `sweep_ack_timeouts()` are called
concurrently from different threads, there are data races on both `m_ack_tracker`
and `m_retry_manager`. No mitigation exists in the current code.

### Producer/consumer relationships
`AckTracker`: producer is `track()` (called from `send()`); consumer is
`on_ack()` and `sweep_one_slot()`. Single-threaded model assumes no concurrent
access.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

### AckTracker slot memory
`AckTracker::m_slots[32]`: array of 32 `Entry` structs, each containing
`MessageEnvelope env` (~4136 bytes), `uint64_t deadline_us` (8 bytes),
`EntryState state` (1 byte + padding). Total: ~132 KB. Embedded by value in
`AckTracker`, which is embedded by value in `DeliveryEngine`.

`on_ack()` only writes `m_slots[i].state = ACKED`. No `memcpy`. The envelope
data in the slot is retained (not cleared) until `sweep_one_slot()` sets
`state = FREE`. The slot is logically available for reuse once `FREE`.

### RetryManager slot memory
`RetryManager::m_slots[32]`: array of 32 `RetryEntry` structs, each containing
`MessageEnvelope env` (~4136 bytes) plus scalar fields (~32 bytes). Total: ~133 KB.

`on_ack()` only sets `m_slots[i].active = false` and decrements `m_count`. The
envelope data persists in the slot until overwritten by the next `schedule()`
call. The slot is logically free immediately (`!m_slots[i].active` is the
"free" condition checked by `schedule()` and `collect_due()`).

### Incoming ACK envelope
`env` (the `DeliveryEngine::receive()` parameter) is written by
`TcpBackend::receive_message()` via `Serializer::deserialize()`. Its
`source_id` and `message_id` fields are used as lookup keys in both `on_ack()`
calls. No data is copied from `env` into `AckTracker` or `RetryManager` during
ACK processing.

### No heap allocation
All storage is statically allocated in object members. ACK resolution is O(1)
memory.

---

## 9. Error Handling Flow

### ERR_INVALID from AckTracker::on_ack()
Possible causes:
- No PENDING slot found for `(src, msg_id)` — either not tracked (BEST_EFFORT),
  already ACKED (duplicate ACK), swept as expired, or source_id mismatch (Risk 1).
- Action: `AckTracker::on_ack()` emits no log on `ERR_INVALID` (silent miss).
  `DeliveryEngine` discards with `(void)`. `receive()` still returns `OK`.

### ERR_INVALID from RetryManager::on_ack()
Possible causes:
- No active retry slot for `(src, msg_id)` — message was `RELIABLE_ACK` (no
  retry slot), slot already inactive (exhausted or expired), or source_id
  mismatch.
- Action: `RetryManager::on_ack()` logs `WARNING_LO "No retry entry found..."`.
  `DeliveryEngine` discards with `(void)`.
- Note: `WARNING_LO` for `RELIABLE_ACK` messages is expected and normal.

### NEVER_COMPILED_OUT_ASSERT failures
All assertions are active in all builds. A failing assertion in `on_ack()` (e.g.,
`m_count <= ACK_TRACKER_CAPACITY`) will trigger the `NEVER_COMPILED_OUT_ASSERT`
behavior: log FATAL + component reset in production, abort in debug.

### Slot not freed by on_ack()
The `ACKED` slot in `AckTracker` is not immediately freed. If `sweep_ack_timeouts()`
is not called frequently enough and many messages are ACKed without sweep, the
tracker fills with `ACKED` slots. `track()` will then return `ERR_FULL` for new
messages.

---

## 10. External Interactions

### Network calls
`TcpBackend::receive_message()` calls:
- `poll(pfd, 1, timeout_ms)` — waits for socket readability.
- `recv(fd, buf, len, flags)` (inside `tcp_recv_frame()`) — reads frame bytes.
These are the only kernel entries during `receive()`.

### Logger
- `RetryManager::on_ack()`: logs `INFO "Cancelled retry for message_id=..."` on
  success, or `WARNING_LO "No retry entry found..."` on miss.
- `DeliveryEngine::receive()`: logs `INFO "Received ACK for message_id=..."` unconditionally
  (line 189-191), whether or not the `on_ack()` calls succeeded.
- `AckTracker::on_ack()`: no log on `ERR_INVALID`.

### No direct socket, file, hardware, or IPC interaction
beyond the transport layer described above.

---

## 11. State Changes / Side Effects

### AckTracker
- `m_slots[i].state`: `PENDING` → `ACKED` (on match). Unchanged (remains
  `PENDING`) on no match — slot will be swept by `sweep_expired()` on deadline.
- `m_count`: **unchanged** by `on_ack()`. Decremented only in `sweep_one_slot()`
  when the `ACKED` slot is later collected.

### RetryManager
- `m_slots[j].active`: `true` → `false` (on match). Unchanged on no match —
  slot continues retrying until exhaustion or expiry.
- `m_count`: decremented by 1 on match. Unchanged on no match.
- All other slot fields (`retry_count`, `backoff_ms`, etc.) are retained but
  irrelevant once `active = false`; `collect_due()` skips inactive slots.

### Logger
- One `INFO` log from `DeliveryEngine` ("Received ACK for message_id=N").
- One `INFO` log from `RetryManager` on match ("Cancelled retry for message_id=N").
  Or one `WARNING_LO` from `RetryManager` on miss ("No retry entry found...").

### No changes to DuplicateFilter, RingBuffer (beyond the pop in transport),
or any transport-layer state.

---

## 12. Sequence Diagram using mermaid

```text
Caller       DeliveryEngine    TcpBackend    AckTracker      RetryManager     Logger
  |               |                |               |               |              |
  |--receive()-->|                 |               |               |              |
  |               |--recv_msg()-->|                |               |              |
  |               |  [poll+recv: reads ACK frame]  |               |              |
  |               |<--OK, ACK env--|               |               |              |
  |               |                                |               |              |
  |               |--ASSERT(envelope_valid) [pass] |               |              |
  |               |--timestamp_expired(0) [false]  |               |              |
  |               |--envelope_is_control() [true]  |               |              |
  |               |  [message_type == ACK: true]   |               |              |
  |               |                                |               |              |
  |               |--on_ack(src, msg_id)---------->|               |              |
  |               |               [loop 0..31]     |               |              |
  |               |               [PENDING match]  |               |              |
  |               |               state = ACKED    |               |              |
  |               |<--OK---------------------------|               |              |
  |               | (void) ack_res                 |               |              |
  |               |                                |               |              |
  |               |--on_ack(src, msg_id)-------------------------->|              |
  |               |                                [loop 0..31]    |              |
  |               |                                [active match]  |              |
  |               |                                active = false  |              |
  |               |                                m_count--       |              |
  |               |                                |---log("Cancelled retry")---->|
  |               |<--OK-------------------------------------------|              |
  |               | (void) retry_res               |               |              |
  |               |                                               |               |
  |               |--log("Received ACK for message_id=...")--------------------->|
  |               |                                                               |
  |<--OK----------|                                |               |              |

--- ACKED slot cleanup (separate call, later) ---

  Caller         DeliveryEngine    AckTracker
    |                  |               |
    |--sweep_ack_---->|               |
    |  timeouts(now)   |               |
    |                  |--sweep_expired(now, buf, 32)->|
    |                  |               [loop 0..31]    |
    |                  |               sweep_one_slot()|
    |                  |               [ACKED slot]    |
    |                  |               state = FREE    |
    |                  |               m_count--       |
    |                  |<--expired_count               |
    |<--0 or N        |               |
```

---

## 13. Initialization vs Runtime Flow

### What happens during startup (init phase)

`AckTracker::init()` (`AckTracker.cpp:23-38`): `memset(m_slots, 0, sizeof(m_slots))`,
`m_count = 0`, loop asserting all slots are `FREE`. Called from
`DeliveryEngine::init()`.

`RetryManager::init()` (`RetryManager.cpp:45-67`): `m_count = 0`, `m_initialized = true`,
loop zeroing all `RetryEntry` fields, calling `envelope_init()` on each. Called
from `DeliveryEngine::init()`.

### What happens during steady-state execution

**ACK resolution lifecycle:**
1. `send()` → `AckTracker::track()` → slot: `FREE → PENDING`.
2. `send()` → `RetryManager::schedule()` → slot: `inactive → active` (if
   `RELIABLE_RETRY`).
3. `receive()` → ACK arrives → `AckTracker::on_ack()` → `PENDING → ACKED`.
4. `receive()` → ACK arrives → `RetryManager::on_ack()` → `active → false`.
5. `sweep_ack_timeouts()` → `sweep_expired()` → `sweep_one_slot()` →
   `ACKED → FREE`.

The application must call `sweep_ack_timeouts()` periodically for step 5 to
occur. Without it, `ACKED` slots accumulate indefinitely and the `AckTracker`
fills, blocking new `track()` calls.

---

## 14. Known Risks / Observations

### Risk 1 — Source ID matching semantics in on_ack()

`AckTracker::track()` stores the envelope's `source_id = m_local_id` (the
sender's own node ID). `AckTracker::on_ack()` looks up with `src = env.source_id`
from the incoming ACK, which is the remote peer's node ID (set by
`envelope_make_ack()` at `MessageEnvelope.hpp:97`). In a two-node deployment,
`local_id != peer_id`. The match condition
`m_slots[i].env.source_id == src` compares `local_id` against `peer_id` — these
never match. `on_ack()` returns `ERR_INVALID` for every legitimate ACK.
`RetryManager::on_ack()` has the identical mismatch at `RetryManager.cpp:131`.

**Practical consequence:** The `PENDING` slot in `AckTracker` is never
transitioned to `ACKED` by an incoming ACK. It remains `PENDING` until
`sweep_ack_timeouts()` fires its `deadline_us` timeout, generating a spurious
`WARNING_HI "ACK timeout"` even for messages that were actually acknowledged.
Retry slots are never cancelled by ACK; retries continue until exhaustion or
expiry. [RISK — appears to be a latent bug]

### Risk 2 — Both on_ack() results discarded

If both `on_ack()` calls fail (as they do in the two-node scenario), the
application receives `OK` from `receive()` and a `WARNING_LO` from
`RetryManager`. There is no direct signal to the application that ACK resolution
failed. The `Logger` output is the only observable indication. [OBSERVATION]

### Risk 3 — ACKED slots not immediately reclaimed

After `on_ack()` sets `state = ACKED` (in the loopback case), the slot holds
a full `MessageEnvelope` copy until `sweep_ack_timeouts()` runs. If ACKs arrive
rapidly and `sweep_ack_timeouts()` is not called between sends, the `AckTracker`
can fill with `ACKED` slots, causing `track()` to return `ERR_FULL` for new
messages. [RISK]

### Risk 4 — RetryManager and AckTracker share ACK_TRACKER_CAPACITY = 32

Both have a fixed 32-slot table. Under `RELIABLE_RETRY` with 32 outstanding
messages, both tables are simultaneously at capacity. New messages cannot be
tracked or scheduled for retry, silently downgrading to `BEST_EFFORT` behavior.
[RISK]

### Risk 5 — AckTracker::on_ack() silent ERR_INVALID

When `on_ack()` returns `ERR_INVALID` (no match), it logs nothing. The only
observable effect is the `DeliveryEngine` INFO log "Received ACK for message_id=N"
and the `RetryManager` `WARNING_LO`. Without the `WARNING_LO`, a silent ACK
resolution failure would be completely invisible. [OBSERVATION]

---

## 15. Unknowns / Assumptions

- [ASSUMPTION 1] The ACK envelope was created by `envelope_make_ack()`
  (`MessageEnvelope.hpp:88-103`) at the remote peer. This correctly carries the
  original data message's `message_id` in the ACK's `message_id` field
  (`ack.message_id = original.message_id` at line 95).

- [ASSUMPTION 2] `DeliveryEngine::receive()`, `pump_retries()`, and
  `sweep_ack_timeouts()` are all called from the same thread. No
  synchronization protects `AckTracker` or `RetryManager`; concurrent access
  would produce data races.

- [ASSUMPTION 3] For `RELIABLE_ACK` messages, no retry slot exists in
  `RetryManager`. `RetryManager::on_ack()` always returns `ERR_INVALID` for
  such messages, and the resulting `WARNING_LO` is expected and harmless.

- [UNKNOWN 1] Whether `sweep_ack_timeouts()` is called frequently enough to
  prevent `AckTracker` from filling with stale `ACKED` slots. If not called,
  the tracker fills within 32 ACK'd messages.

- [UNKNOWN 2] Whether `envelope_make_ack()` is the only mechanism for
  generating ACK messages in the system. If other ACK sources exist (synthetic
  ACKs from tests or impairment engines), their `source_id` assignments may
  differ and would produce different matching behavior.

- [UNKNOWN 3] Whether the application inspects `env` after `receive()` returns
  `OK` with `message_type == ACK`. The design allows this (env is populated),
  but no application-layer code in the reviewed source does so.
