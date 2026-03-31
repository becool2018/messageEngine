# UC_07 — Duplicate detected and dropped by DuplicateFilter::check_and_record()

**HL Group:** HL-6 — Duplicate Message Suppression
**Actor:** System
**Requirement traceability:** REQ-3.2.6, REQ-3.3.3

---

## 1. Use Case Overview

**Trigger:** `DeliveryEngine::receive()` has just received a structurally valid, non-expired
DATA envelope whose `reliability_class` is `RELIABLE_RETRY`. It calls
`m_dedup.check_and_record(env.source_id, env.message_id)` and that call returns
`Result::ERR_DUPLICATE`.

**Goal:** Silently discard a message whose `(source_id, message_id)` pair already appears in
the dedup sliding window, so that retransmitted copies of RELIABLE_RETRY messages are not
delivered to the application a second time.

**Success outcome (from the system's perspective):** `check_and_record()` returns
`ERR_DUPLICATE`. `DeliveryEngine::receive()` logs INFO and returns `ERR_DUPLICATE` to the
application. The application-level caller discards the result; no duplicate is delivered.

**Error outcomes:**
- There are no error outcomes in `check_and_record()` beyond `OK` (new message, recorded)
  and `ERR_DUPLICATE` (seen before, not recorded again).
- `check_and_record()` never fails with a resource error; window-full eviction is handled
  silently by the round-robin `record()` logic (see UC_33 for eviction details).

---

## 2. Entry Points

```
// src/core/DuplicateFilter.cpp — called from DeliveryEngine::receive()
Result DuplicateFilter::check_and_record(NodeId src, uint64_t msg_id);

// Indirect entry via:
// src/core/DeliveryEngine.cpp
Result DeliveryEngine::receive(MessageEnvelope& env,
                               uint32_t timeout_ms,
                               uint64_t now_us);
```

`check_and_record()` is never called directly from application code. The User interacts with
`DeliveryEngine::receive()` which calls it internally. The full inbound chain is:

```
[Application] → DeliveryEngine::receive()
                  → TcpBackend::receive_message()         (get envelope)
                  → timestamp_expired()                   (expiry check)
                  → envelope_is_control()                 (ACK/control check)
                  → DuplicateFilter::check_and_record()   (dedup — this UC)
```

---

## 3. End-to-End Control Flow (Step-by-Step)

### Step 1 — Context: how control arrives at check_and_record()

1. Application calls `DeliveryEngine::receive(env, timeout_ms, now_us)`.
2. `receive()` calls `m_transport->receive_message(env, timeout_ms)` → returns `OK` with a
   DATA envelope whose `reliability_class == RELIABLE_RETRY`.
3. `receive()` fires `NEVER_COMPILED_OUT_ASSERT(envelope_valid(env))`.
4. `timestamp_expired(env.expiry_time_us, now_us)` returns `false` — message not expired.
5. `envelope_is_control(env)` returns `false` — not an ACK/NAK/HEARTBEAT.
6. `NEVER_COMPILED_OUT_ASSERT(envelope_is_data(env))` fires (passes).
7. `receive()` checks `env.reliability_class == RELIABLE_RETRY`: TRUE.
8. Calls `m_dedup.check_and_record(env.source_id, env.message_id)`.

### Step 2 — Inside DuplicateFilter::check_and_record()

9. Pre-condition assert: `m_count <= DEDUP_WINDOW_SIZE`.
10. Calls `is_duplicate(src, msg_id)`.
11. Inside `DuplicateFilter::is_duplicate(src, msg_id) const`:
    a. Iterates over all `DEDUP_WINDOW_SIZE (128)` slots in `m_window[]` (bounded loop).
    b. For each slot at index `i`:
       - `NEVER_COMPILED_OUT_ASSERT(i < DEDUP_WINDOW_SIZE)` fires.
       - Checks `m_window[i].valid && m_window[i].src == src && m_window[i].msg_id == msg_id`.
       - On match: returns `true` immediately.
    c. If no match found: asserts `m_count <= DEDUP_WINDOW_SIZE`, returns `false`.

### Step 3 — Duplicate found path

12. `is_duplicate()` returns `true` — this `(src, msg_id)` pair was previously recorded.
13. `check_and_record()` returns `Result::ERR_DUPLICATE` immediately.
    - **`record()` is NOT called** — the window is not updated.
    - Post-condition assert does NOT fire (no assert after the ERR_DUPLICATE return).

### Step 4 — Back in DeliveryEngine::receive()

14. `dedup_res == ERR_DUPLICATE`.
15. `receive()` enters the `if (dedup_res == Result::ERR_DUPLICATE)` branch:
    - Logs INFO: `"Suppressed duplicate message_id=... from src=..."`.
    - Returns `Result::ERR_DUPLICATE`.
16. Application receives `ERR_DUPLICATE`. The envelope's content is accessible in the output
    parameter `env` (it was filled by `receive_message()`) but should be discarded.

### Alternative path — New message (not a duplicate)

12b. `is_duplicate()` returns `false` — never seen this `(src, msg_id)` before.
13b. `check_and_record()` calls `record(src, msg_id)`.
14b. Inside `DuplicateFilter::record(src, msg_id)`:
     a. Pre-condition asserts: `m_next < DEDUP_WINDOW_SIZE`, `m_count <= DEDUP_WINDOW_SIZE`.
     b. Writes `{src, msg_id, valid=true}` to `m_window[m_next]`.
     c. Advances `m_next = (m_next + 1) % DEDUP_WINDOW_SIZE` (round-robin).
     d. If `m_count < DEDUP_WINDOW_SIZE`: increments `m_count`.
        (If window is full, `m_count` stays at 128 — the write at step 14b silently evicted
        the oldest entry by overwriting its slot without decrementing the count first.)
     e. Post-condition asserts: `m_next < DEDUP_WINDOW_SIZE`,
        `m_count <= DEDUP_WINDOW_SIZE`.
15b. Post-condition assert in `check_and_record()`: `m_count <= DEDUP_WINDOW_SIZE`.
16b. Returns `Result::OK`.
17b. `receive()` continues to the INFO log and returns `OK` with the valid new envelope.

---

## 4. Call Tree

```
DeliveryEngine::receive(env, timeout_ms, now_us)
 ├── TcpBackend::receive_message(env, timeout_ms)    [get envelope]
 ├── timestamp_expired(expiry_us, now_us)             [expiry check]
 ├── envelope_is_control(env)                         [ACK/control check]
 ├── envelope_is_data(env)                            [assert]
 └── DuplicateFilter::check_and_record(src, msg_id)  [this UC]
      └── DuplicateFilter::is_duplicate(src, msg_id)
           └── [linear scan over m_window[0..127]]
      └── [if not duplicate] DuplicateFilter::record(src, msg_id)
           └── [write to m_window[m_next], advance m_next]
```

---

## 5. Key Components Involved

- **`DeliveryEngine::receive()`** — Gate. Only calls `check_and_record()` for DATA envelopes
  with `reliability_class == RELIABLE_RETRY`. BEST_EFFORT and RELIABLE_ACK messages bypass
  the dedup filter entirely.

- **`DuplicateFilter::check_and_record()`** — Atomic check-and-record. Prevents a time-of-
  check / time-of-use gap that would exist if `is_duplicate()` and `record()` were called
  separately by the caller.

- **`DuplicateFilter::is_duplicate()`** — Linear scan of the `m_window[]` array (up to 128
  slots). O(DEDUP_WINDOW_SIZE) per call. Returns `true` on first match.

- **`DuplicateFilter::record()`** — Round-robin write. Advances `m_next` modulo
  `DEDUP_WINDOW_SIZE`. When the window is full, the slot at the current `m_next` position is
  overwritten, silently evicting the oldest recorded entry. This is the sliding-window
  eviction mechanism.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch | Next control |
|---|---|---|---|
| `reliability_class == RELIABLE_RETRY` in `receive()` | Call `check_and_record()` | Skip dedup entirely | Return envelope |
| `is_duplicate()` returns `true` | Return `ERR_DUPLICATE` from `check_and_record()`; `receive()` logs and returns `ERR_DUPLICATE` | Call `record()`; return `OK` | Return to application |
| `m_window[i].valid && src_match && id_match` in scan | Return `true` (duplicate found) | Continue scan | Next slot or return false |
| `m_count < DEDUP_WINDOW_SIZE` in `record()` | Increment `m_count` | Cap `m_count` at window size | Post-condition asserts |

---

## 7. Concurrency / Threading Behavior

- **`DuplicateFilter` has no synchronization.** It is owned by `DeliveryEngine` and must be
  accessed from a single thread — the same thread that calls `receive()`.

- **No `std::atomic` operations.** All reads and writes to `m_window[]`, `m_next`, and
  `m_count` are plain C++ member accesses. If two threads called `receive()` concurrently on
  the same `DeliveryEngine`, there would be a data race on these fields.

- **Linear scan is O(128).** The bounded loop in `is_duplicate()` always iterates all 128
  slots regardless of `m_count` (it does not short-circuit at `m_count` entries). The inner
  check `m_window[i].valid` guards against false matches on uninitialized or evicted slots.

---

## 8. Memory & Ownership Semantics

| Name | Location | Size | Notes |
|---|---|---|---|
| `m_window[]` | `DuplicateFilter` member | `DEDUP_WINDOW_SIZE * sizeof(Entry)` = 128 × (4 + 8 + 1 + padding) ≈ 128 × 16 = 2048 bytes | Fixed array; no heap. Stores `NodeId src`, `uint64_t msg_id`, `bool valid`. |
| `m_next` | `DuplicateFilter` member | `uint32_t` (4 bytes) | Write pointer; wraps at `DEDUP_WINDOW_SIZE`. |
| `m_count` | `DuplicateFilter` member | `uint32_t` (4 bytes) | Number of valid entries; capped at `DEDUP_WINDOW_SIZE`. |

**Power of 10 Rule 3 confirmation:** No dynamic allocation. `DuplicateFilter` uses only its
fixed `m_window[]` array.

**Object lifetime:** `DuplicateFilter m_dedup` is a member of `DeliveryEngine`. It is
initialized by `m_dedup.init()` inside `DeliveryEngine::init()`. Its lifetime equals the
lifetime of the `DeliveryEngine` instance.

---

## 9. Error Handling Flow

| Condition | System state after | What `receive()` does |
|---|---|---|
| `check_and_record()` → `ERR_DUPLICATE` | `m_window[]` unchanged (no new entry added); INFO log; `receive()` returns `ERR_DUPLICATE` | Application should discard the envelope; call `receive()` again to get the next message |
| `check_and_record()` → `OK` (new message) | One entry added/evicted in `m_window[]`; `m_next` advanced; `m_count` potentially at cap | `receive()` continues to return the valid envelope to the application |

`check_and_record()` itself has no failure mode other than these two outcomes. It cannot
return `ERR_INVALID`, `ERR_FULL`, or `ERR_IO`.

---

## 10. External Interactions

No external interactions occur within `DuplicateFilter::check_and_record()` or its callees.
No system calls, no I/O, no POSIX APIs, no atomics. This UC is entirely in-memory.

`DeliveryEngine::receive()` (the caller) uses `TcpBackend::receive_message()` to obtain the
envelope, but those interactions are described in UC_06 and UC_21, not here.

---

## 11. State Changes / Side Effects

### Duplicate path (ERR_DUPLICATE):
| Object | Member | Before | After |
|---|---|---|---|
| `DuplicateFilter m_dedup` | `m_window[]` | Contains `{src, msg_id, valid=true}` at some index `k` | Unchanged — no write |
| `DuplicateFilter m_dedup` | `m_next` | N | Unchanged |
| `DuplicateFilter m_dedup` | `m_count` | C | Unchanged |
| Logger | output | — | INFO entry: `"Suppressed duplicate message_id=... from src=..."` |

### New message path (OK):
| Object | Member | Before | After |
|---|---|---|---|
| `DuplicateFilter m_dedup` | `m_window[m_next_before]` | Any (valid or evicted) | `{src, msg_id, valid=true}` |
| `DuplicateFilter m_dedup` | `m_next` | N | `(N+1) % DEDUP_WINDOW_SIZE` |
| `DuplicateFilter m_dedup` | `m_count` | C (C < 128) | C+1 |
| `DuplicateFilter m_dedup` | `m_count` (window full) | 128 | 128 (unchanged; eviction is silent) |

---

## 12. Sequence Diagram

```
DeliveryEngine::receive()    DuplicateFilter         Logger
       |                           |                    |
       | [envelope received, DATA, RELIABLE_RETRY]      |
       |--check_and_record(src, id)-->                  |
       |                           |                    |
       |                           |--is_duplicate(src, id)
       |                           |   scan m_window[0..127]
       |                           |   [duplicate path] match found at index k
       |                           |<--true              |
       |                           |                    |
       |<--ERR_DUPLICATE-----------|                    |
       |--log INFO "Suppressed duplicate"--------------->|
       |--return ERR_DUPLICATE to application            |

       === Alternative: new message ===

       |--check_and_record(src, id)-->                  |
       |                           |--is_duplicate()    |
       |                           |   [no match]<--false
       |                           |--record(src, id)   |
       |                           |   write m_window[m_next]
       |                           |   advance m_next   |
       |                           |   update m_count   |
       |<--OK----------------------|                    |
       | [continue to return envelope to application]   |
```

---

## 13. Initialization vs Runtime Flow

**Initialization:**
- `DuplicateFilter::init()` is called inside `DeliveryEngine::init()`:
  - `memset(m_window, 0, sizeof(m_window))` — zeroes all 128 entries; `valid = false`.
  - `m_next = 0`.
  - `m_count = 0`.
  - Post-condition asserts: `m_next == 0`, `m_count == 0`.

**Steady-state:**
- `check_and_record()` is called once per received RELIABLE_RETRY DATA message.
- The window accumulates entries until `m_count == DEDUP_WINDOW_SIZE (128)`. Thereafter,
  new entries evict the oldest (the one at `m_next`) silently.
- The window is not cleared between messages; it persists for the lifetime of the
  `DeliveryEngine` instance. Entries are only evicted by the round-robin overwrite.
- There is no TTL on entries; a (src, msg_id) pair once recorded remains detectable as a
  duplicate until evicted by a subsequent `record()` call that wraps around to its slot.

---

## 14. Known Risks / Observations

- **O(DEDUP_WINDOW_SIZE) linear scan.** Every call to `is_duplicate()` iterates all 128
  slots. There is no early exit when `m_count` entries have been checked (it always scans
  the full 128 even if only 5 entries are valid). For WCET analysis this is a fixed-cost
  O(128) operation. See `docs/WCET_ANALYSIS.md`.

- **Silent eviction when window is full.** When `m_count == 128`, the slot at `m_next` is
  overwritten with the new entry. The evicted `(src, msg_id)` pair is no longer detectable.
  A retransmitted copy of the evicted message arriving after eviction would be delivered as a
  new message (false negative dedup). This is inherent to a fixed sliding-window design.

- **No per-sender tracking.** The window is shared across all source nodes. A single active
  sender with a high message rate can fill the window and evict entries from other senders.

- **Dedup only active for RELIABLE_RETRY.** BEST_EFFORT and RELIABLE_ACK messages bypass
  `check_and_record()` entirely. A BEST_EFFORT sender that retransmits manually will produce
  duplicates at the application level.

- **`check_and_record()` is not idempotent.** If the same `(src, msg_id)` pair is recorded
  twice by accident (e.g., through a `check_and_record()` call on the new-message path
  followed by an error that causes the receive to fail and retry the same frame), the second
  call would correctly return `ERR_DUPLICATE` — so the guarantee holds.

---

## 15. Unknowns / Assumptions

- [ASSUMPTION] `m_window[i].valid` is the correct way to identify live entries vs. slots
  that have never been written (after init, all slots are zeroed and `valid == false`). After
  eviction by round-robin write, the overwritten slot has `valid = true` with new content.
  There is no way to distinguish an evicted slot from a live slot.

- [ASSUMPTION] The `(src, msg_id)` pair is globally unique per sender session. MessageIdGen
  generates monotonically increasing non-zero IDs per sender, so a given `(src, msg_id)` is
  only reused after a full 64-bit wraparound (effectively never).

- [ASSUMPTION] The dedup window size (`DEDUP_WINDOW_SIZE = 128`) is sufficient for the
  expected number of in-flight RELIABLE_RETRY messages plus their retransmissions. If
  `max_retries = 3` and `ACK_TRACKER_CAPACITY = 32`, at most 32 messages can be in flight,
  and each can produce up to 3 duplicates, so the window needs to hold at most 32 × 4 = 128
  entries at steady state — exactly matching the window size. This capacity relationship
  appears intentional.

- [ASSUMPTION] Both sender and receiver use the same `(source_id, message_id)` for duplicate
  detection. The sender's `source_id` is `m_local_id` (stamped by `DeliveryEngine::send()`)
  and the `message_id` is assigned by `MessageIdGen::next()`. The dedup filter uses
  `env.source_id` and `env.message_id` as received from the wire.
