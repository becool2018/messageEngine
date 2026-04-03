# UC_07 — Duplicate detected and dropped by DuplicateFilter::check_and_record()

**HL Group:** HL-6 — Duplicate Message Suppression
**Actor:** System
**Requirement traceability:** REQ-3.2.6, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** `DeliveryEngine::receive()` dequeues a `RELIABLE_RETRY` DATA envelope and calls `m_dedup_filter.check_and_record(source_id, message_id)`. File: `src/core/DuplicateFilter.cpp`.
- **Goal:** Silently suppress delivery of any DATA envelope whose `(source_id, message_id)` pair has already been seen within the sliding dedup window.
- **Success outcome (duplicate detected):** `check_and_record()` returns `Result::ERR_DUPLICATE`. `receive()` returns `Result::ERR_DUPLICATE` to the caller. No application-level delivery occurs.
- **Success outcome (first occurrence):** `check_and_record()` returns `Result::OK`. The `(source_id, message_id)` is recorded in the circular buffer. `receive()` continues to deliver the envelope.
- **Error outcomes:** None — `check_and_record()` always returns `OK` or `ERR_DUPLICATE`.

---

## 2. Entry Points

```cpp
// src/core/DuplicateFilter.cpp (called from DeliveryEngine::receive())
Result DuplicateFilter::check_and_record(NodeId src, uint64_t msg_id);
```

Not called directly by the User; called internally by `DeliveryEngine::receive()`.

---

## 3. End-to-End Control Flow

1. `DeliveryEngine::receive()` (`DeliveryEngine.cpp`) calls `m_transport->receive_message(raw, timeout_ms)`, which pops a `MessageEnvelope` from the `RingBuffer`.
2. `receive()` checks `envelope_is_data(raw)` — true for `MessageType::DATA`.
3. Expiry check: `timestamp_expired(raw.expiry_time_us, now_us)` — false (not expired; that is UC_09).
4. **Branch:** `raw.reliability_class == RELIABLE_RETRY` — true; dedup check applies.
5. **`m_dedup_filter.check_and_record(raw.source_id, raw.message_id)`** is called.
6. Inside `DuplicateFilter::check_and_record()`:
   a. `NEVER_COMPILED_OUT_ASSERT(source_id != NODE_ID_INVALID)`.
   b. `NEVER_COMPILED_OUT_ASSERT(message_id != 0)`.
   c. **`is_duplicate(source_id, message_id)`** — linear scan of `m_buf[0..m_count-1]`:
      - For each entry: if `entry.source_id == source_id && entry.message_id == message_id` → return `true`.
      - If no match found: return `false`.
   d. If `is_duplicate()` returns true: return `Result::ERR_DUPLICATE` immediately (do NOT record again).
   e. If not duplicate: **`record(source_id, message_id)`**:
      - Write `m_buf[m_write_idx] = {source_id, message_id}`.
      - `m_write_idx = (m_write_idx + 1) % DEDUP_WINDOW_SIZE`.
      - If `m_count < DEDUP_WINDOW_SIZE`: `++m_count`.
      - (When `m_count == DEDUP_WINDOW_SIZE`: oldest entry at `m_write_idx` is overwritten on next call — sliding window eviction).
   f. Return `Result::OK`.
7. Back in `receive()`: if `check_and_record()` returned `ERR_DUPLICATE`:
   - `Logger::log(INFO, "DeliveryEngine", "Suppressed duplicate message_id=...")`.
   - Returns `Result::ERR_DUPLICATE` to the User.
8. If returned `Result::OK`: continue with delivery.

---

## 4. Call Tree

```
DeliveryEngine::receive()                       [DeliveryEngine.cpp]
 └── DuplicateFilter::check_and_record()        [DuplicateFilter.cpp]
      ├── DuplicateFilter::is_duplicate()       [DuplicateFilter.cpp]
      │    └── [linear scan m_buf[0..m_count-1]]
      └── DuplicateFilter::record()             [DuplicateFilter.cpp]
           └── [write to m_buf[m_write_idx]; advance index]
```

---

## 5. Key Components Involved

- **`DuplicateFilter`** — Circular buffer of `(source_id, message_id)` pairs, capacity `DEDUP_WINDOW_SIZE=128`. Provides O(N) duplicate check and O(1) record.
- **`DeliveryEngine::receive()`** — Calls `check_and_record()` only for `RELIABLE_RETRY` messages. BEST_EFFORT and RELIABLE_ACK messages bypass dedup.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `raw.reliability_class == RELIABLE_RETRY` | Call `check_and_record()` | Skip dedup (BEST_EFFORT / RELIABLE_ACK) |
| `is_duplicate()` returns true | Return `ERR_DUPLICATE`; skip `record()` | Call `record()`; return `OK` |
| `m_count == DEDUP_WINDOW_SIZE` | Oldest entry overwritten on next `record()` (UC_33) | `m_count` incremented |

---

## 7. Concurrency / Threading Behavior

- Executes synchronously in the caller's thread (the thread calling `receive()`).
- `DuplicateFilter::m_buf` and `m_write_idx` are plain arrays/integers; no atomic operations.
- Not safe to call from multiple threads simultaneously.

---

## 8. Memory & Ownership Semantics

- `DuplicateFilter::m_buf[DEDUP_WINDOW_SIZE]` — 128-entry array of `{NodeId, uint64_t}` pairs (~10 bytes each × 128 ≈ 1.3 KB); owned by `DuplicateFilter` value member inside `DeliveryEngine`. No heap allocation.
- Power of 10 Rule 3: no heap allocation.

---

## 9. Error Handling Flow

- `check_and_record()` returns `Result::OK` or `Result::ERR_DUPLICATE`; it has no failure states.
- If duplicate detected: `receive()` returns `ERR_DUPLICATE`. No state is modified (the duplicate entry is not recorded a second time).
- System state remains consistent in both branches.

---

## 10. External Interactions

- None — this flow operates entirely in process memory.
- `Logger::log()` writes to `stderr` when a duplicate is dropped (called from `DeliveryEngine::receive()`, not from `check_and_record()` itself).

---

## 11. State Changes / Side Effects

**On first occurrence (not duplicate):**

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DuplicateFilter` | `m_buf[m_write_idx]` | old entry | new `(source_id, message_id)` |
| `DuplicateFilter` | `m_write_idx` | W | `(W+1) % DEDUP_WINDOW_SIZE` |
| `DuplicateFilter` | `m_count` | C | `min(C+1, DEDUP_WINDOW_SIZE)` |

**On duplicate:** No state changes.

---

## 12. Sequence Diagram

```
DeliveryEngine::receive()
  -> DuplicateFilter::check_and_record(source_id, message_id)
       -> is_duplicate(source_id, message_id)
            [linear scan m_buf[0..m_count-1]]
            <- false  [first occurrence]
       -> record(source_id, message_id)
            [write m_buf[m_write_idx]; advance m_write_idx]
       <- Result::OK  [not duplicate; proceed with delivery]

  --- OR ---

  -> DuplicateFilter::check_and_record(source_id, message_id)
       -> is_duplicate(source_id, message_id)
            [scan finds match at entry k]
            <- true
       <- Result::ERR_DUPLICATE
  -> Logger::log(INFO, "Suppressed duplicate message_id=...")
  <- ERR_DUPLICATE
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DuplicateFilter::init()` called (all `m_count = 0`, `m_write_idx = 0`; buffer implicitly clean).

**Runtime:**
- Each `RELIABLE_RETRY` DATA envelope triggers one `check_and_record()` call. The window slides as `m_write_idx` advances; entries older than 128 positions are silently evicted (see UC_33).

---

## 14. Known Risks / Observations

- **O(N) linear scan:** `is_duplicate()` scans up to `DEDUP_WINDOW_SIZE=128` entries per check. For high-throughput RELIABLE_RETRY streams, this is 128 comparisons per received message. WCET is bounded and acceptable.
- **Window size is fixed:** If the sender retransmits more than 128 unique messages before a duplicate arrives, the older entries are evicted and the duplicate may not be detected.
- **Only RELIABLE_RETRY is deduped:** BEST_EFFORT and RELIABLE_ACK bypass dedup. Caller must not rely on dedup for these classes.
- **`source_id == 0` asserted as invalid:** Envelopes with `source_id == NODE_ID_INVALID (0)` trigger an assertion in `check_and_record()`.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` Only `RELIABLE_RETRY` DATA envelopes trigger `check_and_record()`. This is confirmed by reading `DeliveryEngine::receive()` in `DeliveryEngine.cpp` and the test `test_receive_duplicate` in `test_DeliveryEngine.cpp` which uses `RELIABLE_RETRY`.
