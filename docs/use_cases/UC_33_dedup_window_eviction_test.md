# UC_33 — Sliding-window eviction: oldest entry evicted when DEDUP_WINDOW_SIZE entries are full

**HL Group:** HL-6 — Duplicate Message Suppression
**Actor:** System
**Requirement traceability:** REQ-3.2.6, REQ-3.3.3

---

## 1. Use Case Overview

- **Trigger:** `DuplicateFilter::record()` is called when `m_count == DEDUP_WINDOW_SIZE` (128). The new `(source_id, message_id)` entry overwrites the oldest entry in the circular buffer. File: `src/core/DuplicateFilter.cpp`.
- **Goal:** Maintain a bounded sliding window of recently-seen message IDs. When full, evict the oldest entry to make room for the newest, preserving the invariant that only the last `DEDUP_WINDOW_SIZE` unique messages are tracked.
- **Success outcome:** The new entry is recorded at `m_buf[m_write_idx]` (the oldest slot). `m_write_idx` advances. `m_count` remains at `DEDUP_WINDOW_SIZE`. The evicted entry is no longer detectable as a duplicate.
- **Error outcomes:** None — `record()` has no error states.

---

## 2. Entry Points

```cpp
// src/core/DuplicateFilter.cpp (called from check_and_record())
void DuplicateFilter::record(NodeId source_id, uint64_t message_id);
```

Called from `check_and_record()` when `is_duplicate()` returns false.

---

## 3. End-to-End Control Flow

1. `check_and_record(source_id, message_id)` is called.
2. `is_duplicate()` scans `m_buf[0..m_count-1]` — returns false (new unique message).
3. **`record(source_id, message_id)`** is called:
   a. `m_buf[m_write_idx] = {source_id, message_id}`.
   b. `m_write_idx = (m_write_idx + 1) % DEDUP_WINDOW_SIZE` — advance circular write pointer.
   c. `if (m_count < DEDUP_WINDOW_SIZE) { m_count++; }` — cap at window size.
4. After this call, the entry at the old `m_write_idx` (now overwritten) is no longer in the window. A future call to `is_duplicate()` for that evicted `(source_id, message_id)` will return `false`.
5. Returns (void).

---

## 4. Call Tree

```
DuplicateFilter::check_and_record()            [DuplicateFilter.cpp]
 ├── DuplicateFilter::is_duplicate()           [linear scan; returns false]
 └── DuplicateFilter::record()                 [DuplicateFilter.cpp]
      [m_buf[m_write_idx] = {source_id, message_id}]
      [m_write_idx = (m_write_idx + 1) % 128]
      [m_count stays 128]
```

---

## 5. Key Components Involved

- **`DuplicateFilter::record()`** — Circular write into `m_buf`. When `m_count == DEDUP_WINDOW_SIZE`, `m_count` is not incremented; the modulo write wraps around and overwrites the oldest slot.
- **`DuplicateFilter::is_duplicate()`** — Linear scan O(N=128). The evicted entry is no longer in the buffer; future duplicates of it will be delivered.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `m_count < DEDUP_WINDOW_SIZE` | Increment `m_count` | Keep `m_count` at 128 (full) |
| `m_write_idx + 1 == DEDUP_WINDOW_SIZE` | Wrap to 0 (modulo) | Increment normally |

---

## 7. Concurrency / Threading Behavior

- Synchronous in the caller's thread. `m_buf`, `m_count`, `m_write_idx` are plain members; not thread-safe.

---

## 8. Memory & Ownership Semantics

- `m_buf[DEDUP_WINDOW_SIZE]` — 128-entry fixed array. Overwritten in-place; no heap allocation.
- Evicted entry remains in memory at the overwritten slot until overwritten again; logically evicted.

---

## 9. Error Handling Flow

- No error states. Eviction is a normal, expected operation once the window is full.

---

## 10. External Interactions

- None — operates entirely in process memory.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DuplicateFilter` | `m_buf[m_write_idx]` | oldest entry | new `(source_id, message_id)` |
| `DuplicateFilter` | `m_write_idx` | W | `(W+1) % 128` |
| `DuplicateFilter` | `m_count` | 128 | 128 (unchanged) |

---

## 12. Sequence Diagram

```
[m_count == 128; m_write_idx = W]
DuplicateFilter::check_and_record(new_src, new_id)
  -> is_duplicate(new_src, new_id)  <- false
  -> record(new_src, new_id)
       [m_buf[W] = {new_src, new_id}]       [oldest entry EVICTED]
       [m_write_idx = (W+1) % 128]
       [m_count stays 128]

[Later: is_duplicate(old_src, old_id) for the evicted entry -> false (no longer in window)]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DuplicateFilter::init()` called.
- `DEDUP_WINDOW_SIZE = 128` RELIABLE_RETRY messages have been received (window full).

**Runtime:**
- Window fills after the first 128 unique RELIABLE_RETRY messages. After that, every new receive evicts one entry. The window tracks only the 128 most-recent (source_id, message_id) pairs.

---

## 14. Known Risks / Observations

- **Late-arriving duplicates may slip through:** If a retransmitted copy of a message arrives after the original entry has been evicted (more than 128 unique messages in between), it will be delivered as a new message rather than suppressed.
- **O(N) linear scan:** `is_duplicate()` scans all 128 entries. This is bounded (Power of 10 Rule 2) but is the slowest operation in the receive critical path for RELIABLE_RETRY messages.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `record()` does not sort or reorganize `m_buf`; it writes to `m_write_idx` and advances the pointer. The circular buffer is written in arrival order, not message-id order. `is_duplicate()` scans linearly, so order does not affect correctness.
