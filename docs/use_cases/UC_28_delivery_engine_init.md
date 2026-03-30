# UC_28 — DeliveryEngine initialization

**HL Group:** HL-16 — User initializes the system
**Actor:** System
**Requirement traceability:** REQ-3.2.1, REQ-3.2.4, REQ-3.2.5, REQ-3.2.6, REQ-3.3.1, REQ-3.3.2, REQ-3.3.3

---

## 1. Use Case Overview

### What triggers this flow

The system initialization routine (caller owns the `DeliveryEngine` object) calls `DeliveryEngine::init()` after constructing a `TransportInterface`-backed transport and populating a `ChannelConfig`. This is always an initialization-phase call, before any `send()` or `receive()` calls.

### Expected outcome (single goal)

All four owned sub-components (`AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`) are fully initialized to a known-clean state, `m_initialized == true`, and the engine is ready for `send()`/`receive()` calls. No messages have been sent or received. No dynamic allocation occurs after this function returns.

---

## 2. Entry Points

**Primary entry point:**
- `DeliveryEngine::init(TransportInterface* transport, const ChannelConfig& cfg, NodeId local_id)` — `src/core/DeliveryEngine.cpp`, lines 17–51.

**Transitively entered during init():**
- `AckTracker::init()` — `src/core/AckTracker.cpp`, line 23.
- `RetryManager::init()` — `src/core/RetryManager.cpp`, line 46.
- `DuplicateFilter::init()` — `src/core/DuplicateFilter.cpp`, line 23.
- `MessageIdGen::init(seed)` — `src/core/MessageId.hpp`, line 36 (inline).

---

## 3. End-to-End Control Flow (Step-by-Step)

1. Caller invokes `DeliveryEngine::init(transport, cfg, local_id)` (`DeliveryEngine.cpp:17`).

2. Two precondition assertions fire:
   - `NEVER_COMPILED_OUT_ASSERT(transport != nullptr)` (line 21)
   - `NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID)` (line 22)
   Both are unconditional (not gated on `NDEBUG`); failure terminates the program.

3. Member fields assigned from parameters:
   - `m_transport = transport` (line 24)
   - `m_cfg = cfg` (full struct copy, line 25)
   - `m_local_id = local_id` (line 26)
   - `m_initialized = false` (line 27)
   `m_initialized` is explicitly set to `false` before sub-component init begins, ensuring any re-entrant or interrupted call cannot see a partially ready state.

4. `m_ack_tracker.init()` called (`AckTracker.cpp:23`):
   - `memset(m_slots, 0, sizeof(m_slots))` — zeroes the entire `ACK_TRACKER_CAPACITY` (32) slot array.
   - `m_count = 0U`.
   - Post-condition assertion: `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`.
   - Verification loop (bounded by `ACK_TRACKER_CAPACITY`): asserts `m_slots[i].state == EntryState::FREE` for all `i`. Each slot was zeroed by `memset`; this relies on `EntryState::FREE == 0`.
   - Returns `void`.

5. `m_retry_manager.init()` called (`RetryManager.cpp:46`):
   - `m_count = 0U`.
   - `m_initialized = true`.
   - Initialization loop (bounded by `ACK_TRACKER_CAPACITY`): for each slot sets `active=false`, `retry_count=0U`, `backoff_ms=0U`, `next_retry_us=0ULL`, `expiry_us=0ULL`, `max_retries=0U`, calls `envelope_init(m_slots[i].env)`. Unlike `AckTracker`, no `memset` — each field is explicitly zeroed.
   - Post-condition assertions: `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`, `NEVER_COMPILED_OUT_ASSERT(m_initialized)`.
   - Returns `void`.

6. `m_dedup.init()` called (`DuplicateFilter.cpp:23`):
   - `memset(m_window, 0, sizeof(m_window))` — zeroes the `DEDUP_WINDOW_SIZE` (128) entry sliding window.
   - `m_next = 0U`; `m_count = 0U`.
   - Post-condition assertions: `NEVER_COMPILED_OUT_ASSERT(m_next == 0U)`, `NEVER_COMPILED_OUT_ASSERT(m_count == 0U)`.
   - Returns `void`.

7. Seed computation (`DeliveryEngine.cpp:36-39`):
   - `uint64_t id_seed = static_cast<uint64_t>(local_id)`
   - `if (id_seed == 0ULL): id_seed = 1ULL` — ensures non-zero seed for `MessageIdGen`.
   - Guarantees a deterministic, node-specific starting ID sequence.

8. `m_id_gen.init(id_seed)` called (`MessageId.hpp:36`):
   - `NEVER_COMPILED_OUT_ASSERT(seed != 0ULL)` (line 39).
   - `m_next = seed` (line 41).
   - `NEVER_COMPILED_OUT_ASSERT(m_next == seed)` (line 44).
   - Returns `void`.

9. `m_initialized = true` (line 42).

10. Two post-condition assertions fire:
    - `NEVER_COMPILED_OUT_ASSERT(m_initialized)` (line 45)
    - `NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)` (line 46)

11. `Logger::log(Severity::INFO, "DeliveryEngine", "Initialized channel=%u, local_id=%u", cfg.channel_id, local_id)` (lines 48-50).

12. `DeliveryEngine::init()` returns `void`. Control returns to caller. The engine is ready.

---

## 4. Call Tree (Hierarchical)

```
DeliveryEngine::init(transport, cfg, local_id)          [DeliveryEngine.cpp:17]
  ├── NEVER_COMPILED_OUT_ASSERT(transport != nullptr)
  ├── NEVER_COMPILED_OUT_ASSERT(local_id != NODE_ID_INVALID)
  ├── m_transport=transport; m_cfg=cfg; m_local_id=local_id; m_initialized=false
  │
  ├── m_ack_tracker.init()                               [AckTracker.cpp:23]
  │   ├── memset(m_slots, 0, sizeof(m_slots))
  │   ├── m_count = 0U
  │   ├── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
  │   └── [loop 0..ACK_TRACKER_CAPACITY)
  │       └── NEVER_COMPILED_OUT_ASSERT(m_slots[i].state == FREE)
  │
  ├── m_retry_manager.init()                             [RetryManager.cpp:46]
  │   ├── m_count = 0U
  │   ├── m_initialized = true
  │   ├── [loop 0..ACK_TRACKER_CAPACITY)
  │   │   ├── m_slots[i].active = false
  │   │   ├── m_slots[i].retry_count = 0U
  │   │   ├── m_slots[i].backoff_ms = 0U
  │   │   ├── m_slots[i].next_retry_us = 0ULL
  │   │   ├── m_slots[i].expiry_us = 0ULL
  │   │   ├── m_slots[i].max_retries = 0U
  │   │   └── envelope_init(m_slots[i].env)
  │   ├── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
  │   └── NEVER_COMPILED_OUT_ASSERT(m_initialized)
  │
  ├── m_dedup.init()                                     [DuplicateFilter.cpp:23]
  │   ├── memset(m_window, 0, sizeof(m_window))
  │   ├── m_next = 0U; m_count = 0U
  │   ├── NEVER_COMPILED_OUT_ASSERT(m_next == 0U)
  │   └── NEVER_COMPILED_OUT_ASSERT(m_count == 0U)
  │
  ├── [seed = static_cast<uint64_t>(local_id); if 0 -> 1]
  │
  ├── m_id_gen.init(id_seed)                             [MessageId.hpp:36]
  │   ├── NEVER_COMPILED_OUT_ASSERT(seed != 0ULL)
  │   ├── m_next = seed
  │   └── NEVER_COMPILED_OUT_ASSERT(m_next == seed)
  │
  ├── m_initialized = true
  ├── NEVER_COMPILED_OUT_ASSERT(m_initialized)
  ├── NEVER_COMPILED_OUT_ASSERT(m_transport != nullptr)
  └── Logger::log(INFO, "DeliveryEngine", "Initialized channel=%u, local_id=%u", ...)
```

---

## 5. Key Components Involved

**`DeliveryEngine` — `src/core/DeliveryEngine.cpp/.hpp`**
Coordinator. Owns all four sub-components as inline value members. `init()` is the sole entry point for bringing the engine into a ready state.

**`AckTracker` — `src/core/AckTracker.cpp/.hpp`**
Tracks outstanding messages awaiting ACK in a fixed array of `ACK_TRACKER_CAPACITY` (32) slots. `init()` zeroes all slots via `memset`.

**`RetryManager` — `src/core/RetryManager.cpp/.hpp`**
Schedules messages for retry with exponential backoff in a fixed array of `ACK_TRACKER_CAPACITY` (32) slots. `init()` explicitly zeroes each field and calls `envelope_init()` per slot. Has its own `m_initialized` flag checked by every runtime method.

**`DuplicateFilter` — `src/core/DuplicateFilter.cpp/.hpp`**
Sliding-window dedup suppression over `DEDUP_WINDOW_SIZE` (128) entries. `init()` zeroes the window via `memset`.

**`MessageIdGen` — `src/core/MessageId.hpp`**
Inline class. Simple incrementing counter seeded with `local_id`. Generates unique non-zero message IDs per sender. `init(seed)` sets `m_next = seed`.

**`TransportInterface*` — `src/core/TransportInterface.hpp`**
Non-owning raw pointer stored as `m_transport`. Not called during `init()`. `DeliveryEngine` does not manage its lifetime.

**`ChannelConfig` — `src/core/ChannelConfig.hpp`**
Passed by `const` reference and copied into `m_cfg`. The caller's copy is independent after `init()` returns.

**`Logger` — `src/core/Logger.hpp`**
Emits one `INFO` message at the end of successful `init()`. The only external call during init.

---

## 6. Branching Logic / Decision Points

**Branch 1 — `transport == nullptr`** (`DeliveryEngine.cpp:21`)
- Condition: `transport == nullptr`
- True: `NEVER_COMPILED_OUT_ASSERT` fires; program terminates unconditionally.
- False: `m_transport = transport`; execution continues.

**Branch 2 — `local_id == NODE_ID_INVALID`** (`DeliveryEngine.cpp:22`)
- Condition: `local_id == NODE_ID_INVALID` (= 0U per `Types.hpp`)
- True: `NEVER_COMPILED_OUT_ASSERT` fires; program terminates.
- False: execution continues.

**Branch 3 — Seed coercion** (`DeliveryEngine.cpp:37-39`)
- Condition: `id_seed == 0ULL` (which implies `local_id == 0`, contradicting Branch 2; this is a belt-and-suspenders guard)
- True: `id_seed = 1ULL` — guarantees non-zero seed for `MessageIdGen`.
- False: `id_seed = static_cast<uint64_t>(local_id)`.

**Branch 4 — `AckTracker` per-slot state assertion** (`AckTracker.cpp:36`)
- Condition: `m_slots[i].state != EntryState::FREE`
- True: assertion fires — would indicate `EntryState::FREE != 0`, invalidating the `memset` optimization.
- False: all slots confirmed FREE; loop completes.

**Branch 5 — `RetryManager` slot loop** (`RetryManager.cpp:54-62`)
- Unlike `AckTracker`, `RetryManager` does NOT use `memset`; it initializes each field individually and calls `envelope_init()` per slot. Safer against padding-related `memset` traps but O(`ACK_TRACKER_CAPACITY`).

---

## 7. Concurrency / Threading Behavior

`DeliveryEngine::init()` is intended to be called once during system initialization, before any concurrent access begins. The code contains no locks, mutexes, or atomics in the init path.

`AckTracker`, `RetryManager`, and `DuplicateFilter` all use plain (non-atomic) fields. None are thread-safe for concurrent calls.

The `RingBuffer` (used by `LocalSimHarness`, not directly by `DeliveryEngine`) uses `std::atomic<uint32_t>` for SPSC thread safety, but `RingBuffer` is not touched during `DeliveryEngine::init()`.

`init()` must complete (happens-before) any thread calling `send()` or `receive()`. No synchronization is provided or needed within `init()` itself.

If `init()` is called a second time (re-init scenario), `m_initialized` is set to `false` at line 27, making the engine temporarily non-functional during re-init. No re-init guard exists.

---

## 8. Memory & Ownership Semantics (C/C++ Specific)

**Allocation model:** all storage is statically allocated as inline value members inside the `DeliveryEngine` object. No dynamic (heap) allocation occurs anywhere in the init path (Power of 10 Rule 3).

| Member | Type | Storage |
|---|---|---|
| `m_ack_tracker` | `AckTracker` (value) | `m_slots[32]` inline |
| `m_retry_manager` | `RetryManager` (value) | `m_slots[32]` inline |
| `m_dedup` | `DuplicateFilter` (value) | `m_window[128]` inline |
| `m_id_gen` | `MessageIdGen` (value) | single `uint64_t m_next` |
| `m_transport` | `TransportInterface*` | 8-byte pointer (non-owning) |
| `m_cfg` | `ChannelConfig` (value) | copied from caller |

**`m_transport`:** non-owning raw pointer. `DeliveryEngine` does not `delete` it. Lifetime management is the caller's responsibility.

**`ChannelConfig` copy:** `cfg` is passed by `const` reference and copied into `m_cfg` at line 25 (`m_cfg = cfg`). After `init()` returns, `m_cfg` is independent of the caller's struct. No pointers to the caller's `cfg` are retained.

**`memset` usage:**
- `AckTracker::init()` uses `memset` on `m_slots`. Correct as long as `EntryState::FREE == 0` and all slot fields have trivial zero representations.
- `DuplicateFilter::init()` uses `memset` on `m_window` (relies on `valid=false` when `0x00`).
- `RetryManager::init()` avoids `memset` and initializes each field explicitly, which is safer but carries O(N) CPU cost.

**`envelope_init()`:** called per slot in `RetryManager`. Zero-initializes all `MessageEnvelope` fields without dynamic allocation.

---

## 9. Error Handling Flow

`DeliveryEngine::init()` is declared `void`; it returns no `Result`.

All error detection is via `NEVER_COMPILED_OUT_ASSERT()` (unconditional, not gated on `NDEBUG`). Assertion failures terminate the program regardless of build configuration.

| Condition | Location | Effect |
|---|---|---|
| `transport == nullptr` | `DeliveryEngine.cpp:21` | Assert fires; program terminates. |
| `local_id == NODE_ID_INVALID` | `DeliveryEngine.cpp:22` | Assert fires; program terminates. |
| `AckTracker` post-loop slot != FREE | `AckTracker.cpp:36` | Assert fires; indicates `EntryState::FREE != 0`. |
| `MessageIdGen` seed == 0 | `MessageId.hpp:39` | Assert fires; but coercion at line 37 prevents this in normal flow. |

Sub-component inits are all `void` with no return-value error paths. No error recovery is possible during init; the system must be restarted. This is consistent with the F-Prime `FATAL` error model for init-phase failures.

`Logger::log()` at the end is informational only; a failure there would not roll back init.

---

## 10. External Interactions

**`TransportInterface*`:**
The pointer is stored but the transport is NOT called during `init()`. No network I/O occurs.

**`Logger::log()`:**
Called once at the end of `DeliveryEngine::init()` with `Severity::INFO`. Outputs `channel_id` and `local_id` values. This is the only external call during init.

No file I/O, no OS calls, no socket interactions during `init()`.

---

## 11. State Changes / Side Effects

**Before `init()` (object declared but not yet initialized):**
All members are indeterminate (no default constructor defined for `DeliveryEngine`).

**After `init()` completes:**

| Member | State |
|---|---|
| `m_transport` | `= transport` (caller-provided pointer) |
| `m_cfg` | `= copy of cfg` parameter |
| `m_local_id` | `= local_id` |
| `m_initialized` | `= true` |
| `m_ack_tracker.m_slots[0..31].state` | `= FREE` (zeroed) |
| `m_ack_tracker.m_count` | `= 0` |
| `m_retry_manager.m_slots[0..31].active` | `= false` |
| `m_retry_manager.m_slots[0..31]` (all scalar fields) | `= 0` |
| `m_retry_manager.m_count` | `= 0` |
| `m_retry_manager.m_initialized` | `= true` |
| `m_dedup.m_window[0..127].valid` | `= false` (zeroed) |
| `m_dedup.m_next` | `= 0` |
| `m_dedup.m_count` | `= 0` |
| `m_id_gen.m_next` | `= id_seed` (= `(uint64_t)local_id`, minimum 1) |

Side effect: `Logger::log()` emits one `INFO` message with channel and local node ID.

---

## 12. Sequence Diagram using mermaid

```mermaid
sequenceDiagram
    participant Caller
    participant DE as DeliveryEngine
    participant AT as AckTracker
    participant RM as RetryManager
    participant DF as DuplicateFilter
    participant MIG as MessageIdGen

    Caller->>DE: init(transport, cfg, local_id)
    DE->>DE: assert transport != nullptr
    DE->>DE: assert local_id != NODE_ID_INVALID
    DE->>DE: m_transport=transport; m_cfg=cfg; m_local_id=local_id; m_initialized=false

    DE->>AT: init()
    AT->>AT: memset(m_slots, 0); m_count=0
    AT->>AT: [assert loop: all FREE]
    AT-->>DE: done

    DE->>RM: init()
    RM->>RM: m_count=0; m_initialized=true
    RM->>RM: [slot loop: zero all fields, envelope_init each]
    RM-->>DE: done

    DE->>DF: init()
    DF->>DF: memset(m_window, 0); m_next=0; m_count=0
    DF-->>DE: done

    DE->>DE: id_seed = local_id; if 0 -> 1
    DE->>MIG: init(id_seed)
    MIG->>MIG: m_next = id_seed
    MIG-->>DE: done

    DE->>DE: m_initialized = true
    DE->>DE: assert(m_initialized); assert(m_transport != nullptr)
    DE->>DE: Logger::log(INFO, "Initialized channel=%u, local_id=%u")
    DE-->>Caller: (void)
```

---

## 13. Initialization vs Runtime Flow

### Startup / initialization (this use case)

`DeliveryEngine::init()` is the sole init-phase entry point. All four sub-components are initialized in fixed order: `AckTracker`, `RetryManager`, `DuplicateFilter`, `MessageIdGen`. No messages are sent or received. All storage is pre-allocated as inline value members. After `init()` returns, `m_initialized == true`.

### Steady-state runtime (post-init, not covered here)

- `send()` uses `m_id_gen.next()` to assign message IDs; `m_ack_tracker.track()` and `m_retry_manager.schedule()` for reliable classes.
- `receive()` uses `m_dedup.check_and_record()` for `RELIABLE_RETRY` dedup.
- `pump_retries()` drives `m_retry_manager.collect_due()` + `send_via_transport()`.
- `sweep_ack_timeouts()` drives `m_ack_tracker.sweep_expired()`.

Any runtime call that reaches a sub-component without a prior `init()` will fail the `NEVER_COMPILED_OUT_ASSERT(m_initialized)` guard in `send()`/`receive()` (lines 79 and 153 of `DeliveryEngine.cpp`). `RetryManager` carries its own `NEVER_COMPILED_OUT_ASSERT(m_initialized)` in every method.

---

## 14. Known Risks / Observations

**RISK-1 — No return value from `init()`:**
The function is declared `void`. If an assertion fails in a hypothetical release build with `NEVER_COMPILED_OUT_ASSERT` disabled (which should not occur by project policy), partial initialization could result. `m_initialized` would be set to `true` even if sub-components were not fully initialized.

**RISK-2 — `memset` reliance on zero-equals-FREE assumption:**
`AckTracker::init()` and `DuplicateFilter::init()` use `memset` and then assert the resulting state matches `FREE`/`false`. If `EntryState::FREE != 0` or the `valid` bool field has a non-zero `false` representation on a given platform, this is undefined behavior. The verification loop in `AckTracker` guards against this in all builds (since `NEVER_COMPILED_OUT_ASSERT` is unconditional).

**RISK-3 — Ordering of sub-component inits is implicit:**
The code initializes `AckTracker`, `RetryManager`, `DuplicateFilter`, then `MessageIdGen` in that order. There is no formal dependency between them; any order would be correct. A future developer adding a cross-dependency could introduce a subtle ordering bug.

**RISK-4 — `m_initialized` set to `false` at entry enables reentrancy hazard:**
If `init()` is called a second time on an already-initialized engine, `m_initialized` is set to `false` at line 27, making the engine temporarily non-functional. If any other thread observes `m_initialized` in this window, it will see `false`. No re-init guard exists.

**RISK-5 — `ACK_TRACKER_CAPACITY` used for both `AckTracker` and `RetryManager`:**
Both sub-components share the same capacity constant (32). This is intentional but means the retry table is identically sized to the ACK tracker. If retry load significantly differs from ACK tracking load, one table may be over- or under-sized.

**OBSERVATION — `envelope_init()` O(N) cost:**
`envelope_init()` is called `ACK_TRACKER_CAPACITY` (32) times in `RetryManager::init()`. For large capacity values this adds measurable latency to the system startup path. At the current capacity of 32 this is negligible.

---

## 15. Unknowns / Assumptions

`[CONFIRMED]` `NODE_ID_INVALID = 0U` from `Types.hpp:31`. The assertion at `DeliveryEngine.cpp:22` guards against `local_id == 0`.

`[CONFIRMED]` `ACK_TRACKER_CAPACITY = 32U`, `DEDUP_WINDOW_SIZE = 128U` from `Types.hpp`.

`[CONFIRMED]` `EntryState::FREE` is defined as `0U` in `AckTracker.hpp:72`. The `memset` zeroing in `AckTracker::init()` is therefore correct, and the verification loop will always find `FREE` after `memset`.

`[CONFIRMED]` `DuplicateFilter::Entry.valid` is a `bool`. Standard C++ guarantees `false` is represented as `0x00`, so `memset` to 0 correctly initializes `valid = false`.

`[ASSUMPTION]` `envelope_init()` zero-initializes all `MessageEnvelope` fields without dynamic allocation. Referenced in `RetryManager.cpp` but definition not read directly.

`[ASSUMPTION]` `Logger::log()` is non-blocking, does not allocate on the critical path, and always returns successfully. No return value is checked.

`[UNKNOWN]` Whether re-initialization (calling `init()` more than once on the same object) is an intended use pattern. The code supports it mechanically (all state is re-zeroed), but no documentation or test exercises this pattern.
