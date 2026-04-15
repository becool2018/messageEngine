# UC_28 — DeliveryEngine::init(): DuplicateFilter, AckTracker, RetryManager all reset

**HL Group:** HL-16 — System Initialization
**Actor:** User
**Requirement traceability:** REQ-4.1.1, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6

---

## 1. Use Case Overview

- **Trigger:** User calls `DeliveryEngine::init(transport, channel_cfg, local_node_id)`. File: `src/core/DeliveryEngine.cpp`.
- **Goal:** Wire up the transport interface pointer, reset all internal subsystems to a clean initial state, and prepare the engine for sending and receiving.
- **Success outcome:** All subsystems reset; `m_transport` set; `m_cfg` stored; `m_local_node_id` set. Returns `Result::OK`. (Return type is `void` — see note in Section 9.)
- **Error outcomes:** None directly — assertions fire if preconditions are violated.

---

## 2. Entry Points

```cpp
// src/core/DeliveryEngine.cpp
void DeliveryEngine::init(TransportInterface* transport,
                          const ChannelConfig& channel_cfg,
                          NodeId local_node_id);
```

Called once at application startup.

---

## 3. End-to-End Control Flow

1. **`DeliveryEngine::init(transport, channel_cfg, local_node_id)`** — entry.
2. `NEVER_COMPILED_OUT_ASSERT(transport != nullptr)`.
3. `NEVER_COMPILED_OUT_ASSERT(local_node_id != NODE_ID_INVALID)` — enforces non-zero node ID.
4. `m_transport = transport`.
5. `m_cfg = channel_cfg` — stores channel config including `recv_timeout_ms`, `retry_backoff_ms`, `max_retries`.
6. `m_local_node_id = local_node_id`.
7. **`m_dedup_filter.init()`** (`DuplicateFilter.cpp`):
   - `m_count = 0`, `m_write_idx = 0`. Buffer contents implicitly stale (will be overwritten on first use).
8. **`m_ack_tracker.init()`** (`AckTracker.cpp`):
   - For each slot `i` in `0..ACK_TRACKER_CAPACITY-1`: `m_entries[i].state = FREE`.
9. **`m_retry_manager.init()`** (`RetryManager.cpp`):
   - For each slot `i` in `0..ACK_TRACKER_CAPACITY-1`: `m_entries[i].active = false`.
10. **`m_id_gen.init(local_node_id)`** (`MessageId.cpp`):
    - `m_counter = 0` (or seeded from `local_node_id`).
11. `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` — postcondition.

---

## 4. Call Tree

```
DeliveryEngine::init(transport, channel_cfg, local_node_id)  [DeliveryEngine.cpp]
 ├── DuplicateFilter::init()     [DuplicateFilter.cpp]
 ├── AckTracker::init()          [AckTracker.cpp]
 ├── RetryManager::init()        [RetryManager.cpp]
 └── MessageIdGen::init(node_id) [MessageId.cpp]
```

---

## 5. Key Components Involved

- **`DuplicateFilter::init()`** — Resets `m_count` and `m_write_idx`. Required so no stale (source_id, message_id) pairs remain from a prior session.
- **`AckTracker::init()`** — Sets all 32 slots to `FREE`. Required so `sweep_expired()` and `on_ack()` start from a clean state.
- **`RetryManager::init()`** — Sets all 32 entries to `inactive`. Required so `collect_due()` returns 0 until `schedule()` is called.
- **`MessageIdGen::init()`** — Resets the counter. Required so each message gets a unique ID starting from a known value.

---

## 6. Branching Logic / Decision Points

| Condition | True branch | False branch |
|-----------|-------------|--------------|
| `transport == nullptr` | NEVER_COMPILED_OUT_ASSERT fires | Proceed |
| `local_node_id == NODE_ID_INVALID` | Assert fires | Proceed |

---

## 7. Concurrency / Threading Behavior

- Called once during startup before any `send()` or `receive()` calls. Single-threaded.
- After `init()` completes, the caller is free to call `send()`, `receive()`, etc. from a single thread.

---

## 8. Memory & Ownership Semantics

- `m_transport` — non-owning pointer. The caller owns the transport object and must ensure it outlives the `DeliveryEngine`.
- `m_dedup_filter`, `m_ack_tracker`, `m_retry_manager`, `m_id_gen` — all value members of `DeliveryEngine`; no heap allocation. Owned and destroyed with the `DeliveryEngine` instance.
- No heap allocation on this path.

---

## 9. Error Handling Flow

- No error return (function is `void`). Precondition violations trigger `NEVER_COMPILED_OUT_ASSERT`.
- If the transport's `init()` has not been called separately, the transport may not be in a ready state. The `DeliveryEngine::init()` does not call `transport->init()` — that is the caller's responsibility.

---

## 10. External Interactions

- None during `init()`. Logger may be called by the subsystem `init()` functions internally for INFO-level confirmation, but no external I/O.

---

## 11. State Changes / Side Effects

| Object | Member | Before | After |
|--------|--------|--------|-------|
| `DeliveryEngine` | `m_transport` | undefined | valid transport pointer |
| `DeliveryEngine` | `m_cfg` | undefined | copy of `channel_cfg` |
| `DeliveryEngine` | `m_local_node_id` | undefined | `local_node_id` |
| `DuplicateFilter` | `m_count`, `m_write_idx` | undefined | 0 |
| `AckTracker` | `m_entries[*].state` | undefined | `FREE` |
| `RetryManager` | `m_entries[*].active` | undefined | `false` |
| `MessageIdGen` | `m_counter` | undefined | 0 (or seeded value) |

---

## 12. Sequence Diagram

```
User
  -> DeliveryEngine::init(transport, channel_cfg, local_node_id)
       -> DuplicateFilter::init()     [m_count=0; m_write_idx=0]
       -> AckTracker::init()          [all slots FREE]
       -> RetryManager::init()        [all entries inactive]
       -> MessageIdGen::init(node_id) [counter reset]
  [DeliveryEngine ready for send()/receive() calls]
```

---

## 13. Initialization vs Runtime Flow

**Preconditions:**
- `DeliveryEngine` default-constructed (all members zero-initialized by the compiler if value-initialized, but defaults should not be relied upon without explicit `init()` call).
- Caller has called `transport->init(config)` and verified it succeeded before passing the transport to `DeliveryEngine::init()`.

**Runtime:**
- `init()` must be called exactly once before any `send()` or `receive()`. Calling it a second time re-initializes all subsystems (dropping any pending ACK or retry state).

---

## 14. Known Risks / Observations

- **Re-initialization drops state:** Calling `init()` again flushes all pending ACK slots and retry entries without delivering or logging them. Any RELIABLE_RETRY messages that were in flight are silently abandoned.
- **`m_transport` lifetime:** `DeliveryEngine` holds a raw pointer to the transport. If the transport is destroyed before the `DeliveryEngine`, all subsequent calls will use a dangling pointer.
- **`init()` does not call `transport->init()`:** The transport must be independently initialized. This is a common source of misconfiguration in tests.

---

## 15. Unknowns / Assumptions

- `[ASSUMPTION]` `MessageIdGen::init(node_id)` resets the counter to 0 or a node-ID-seeded value. The exact seed formula is inferred from MessageId.cpp; for our purposes the counter starts from a known value.
- `[ASSUMPTION]` `DeliveryEngine::init()` returns `void`. If a future version adds a `Result` return, callers should check it.
